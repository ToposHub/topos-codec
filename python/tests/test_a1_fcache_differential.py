"""A1（速度计划 v2）差分机：F-cache 数值等价参考对拍。

GPU 化一期（DCT+量化 → Metal compute）的验收前置（计划 v2 A1 行：
"数值等价差分机（u32 F 平面逐值）"）。本文件在 CPU/numpy 侧实现
规范参考（两趟整数矩阵乘，kTransformM 取自库内冻结表），与
m7_prepare 产出的 F-cache（dev 捕获 API 导出）逐值对拍：

- 位宽论证（ADR-C002）：|x'| ≤ 2^11、|M| ≤ 13 → |A| < 2^18、
  |F| ≤ 2^25 —— int64 累加后 cast int32 精确；
- pad 语义：边缘复制（tc_plane_pad_u16，plane.c:34-42）；
- 布局：自然序 [u*8+v]，块绝对索引 (by*cols+bx)*64，三 plane 由
  tc_dev_fcache_layout 的块偏移分隔；
- 仅 sized 编码走 m7/F-cache 路径（plain 不触发捕获）。

后续 Metal kernel（kernels_dct）以同一参考对拍 → 三方一致钉死。
"""
from __future__ import annotations

import ctypes

import numpy as np
import pytest

from topos_codec.topos_binding import TOPOS_CODEC_ABI_VERSION, ToposCodec, _CFrameConfig


def _bind_dev_fcache(lib) -> None:
    lib.tc_dev_set_fcache_capture.argtypes = [ctypes.c_int]
    lib.tc_dev_set_fcache_capture.restype = None
    lib.tc_dev_fcache_elems.argtypes = []
    lib.tc_dev_fcache_elems.restype = ctypes.c_int64
    lib.tc_dev_fcache_copy.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.tc_dev_fcache_copy.restype = ctypes.c_int64
    lib.tc_dev_fcache_layout.argtypes = [
        ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32)]
    lib.tc_dev_fcache_layout.restype = None
    lib.tc_transform_matrix.argtypes = []
    lib.tc_transform_matrix.restype = ctypes.POINTER(ctypes.c_int16)


def _pad_edge(plane: np.ndarray, dst_h: int, dst_w: int) -> np.ndarray:
    h, w = plane.shape
    out = np.empty((dst_h, dst_w), dtype=plane.dtype)
    out[:h, :w] = plane
    out[:h, w:] = plane[:, w - 1: w]          # 右缘复制末列
    out[h:, :] = out[h - 1: h, :]             # 底缘复制末行
    return out


def _reference_f(plane_padded: np.ndarray, mid: int, m: np.ndarray) -> np.ndarray:
    """两趟整数矩阵乘（与 transform.c:18-41 scalar 同式）。

    块批量化：X[nb,8,8] → A[y,v]=Σ_x X[y,x]·M[v,x] → F[u,v]=Σ_y M[u,y]·A[y,v]。
    整数域累加次序无关（|F| < 2^25），int64 累加后 cast int32 精确。
    """
    h, w = plane_padded.shape
    rows, cols = h // 8, w // 8
    x = plane_padded.astype(np.int64)
    x = x.reshape(rows, 8, cols, 8).transpose(0, 2, 1, 3).reshape(rows * cols, 8, 8)
    x = x - mid
    mi = m.astype(np.int64)
    a = np.einsum("nyc,vc->nyv", x, mi)
    f = np.einsum("uy,nyv->nuv", mi, a)
    return f.astype(np.int32).reshape(rows * cols, 64)


@pytest.fixture(scope="module")
def codec() -> ToposCodec:
    return ToposCodec()


@pytest.mark.parametrize("w,h,bd,seed", [
    (160, 112, 10, 0xA1D1),
    (64, 48, 10, 0xA1D2),
    (63, 45, 10, 0xA1D3),   # 非 8 对齐：pad 边缘复制路径
    (128, 96, 12, 0xA1D4),  # 12-bit level shift
])
def test_fcache_matches_reference(codec: ToposCodec, w, h, bd, seed) -> None:
    lib = codec._lib
    _bind_dev_fcache(lib)

    rng = np.random.default_rng(seed)
    planes = [rng.integers(0, 1 << bd, (h, w)).astype("<u2") for _ in range(3)]
    fc = _CFrameConfig()
    fc.struct_size = ctypes.sizeof(_CFrameConfig)
    fc.abi_version = TOPOS_CODEC_ABI_VERSION
    fc.visible_width = w
    fc.visible_height = h
    fc.profile = 5
    fc.pixel_format = 1  # 444：三平面同几何
    fc.bit_depth = bd
    fc.qp_base = 30
    fc.qmatrix_id = 0
    fc.slice_rows = 8
    fc.sar_num = fc.sar_den = 1
    fc.reserved[0] = 8  # rans2（产品默认，m7/F-cache 路径）

    cap = codec.packet_bound(fc)
    target = max(1, cap // 8)
    m = np.fromiter(
        (int(v) for v in lib.tc_transform_matrix()[:64]), dtype=np.int64
    ).reshape(8, 8)

    lib.tc_dev_set_fcache_capture(1)
    try:
        codec.encode_sized(fc, planes, target, 0, 63)
        elems = int(lib.tc_dev_fcache_elems())
        assert elems > 0, "F-cache 未捕获（m7 路径未触发？）"

        buf = np.empty(elems, dtype=np.int32)
        got = int(lib.tc_dev_fcache_copy(buf.ctypes.data, buf.size))
        assert got == elems
        off = (ctypes.c_uint32 * 3)()
        planes_n = ctypes.c_uint32(0)
        lib.tc_dev_fcache_layout(off, ctypes.byref(planes_n))
        assert planes_n.value == 3
    finally:
        lib.tc_dev_set_fcache_capture(0)

    coded_w = (w + 7) // 8 * 8
    coded_h = (h + 7) // 8 * 8
    rows, cols = coded_h // 8, coded_w // 8
    mid = 1 << (bd - 1)
    for p in range(3):
        begin = int(off[p]) * 64
        end = int(off[p + 1]) * 64 if p < 2 else elems
        assert (end - begin) == rows * cols * 64, f"plane {p} 块数不符"
        lib_f = buf[begin:end].reshape(rows * cols, 64)
        padded = _pad_edge(planes[p], coded_h, coded_w)
        ref = _reference_f(padded, mid, m)
        mismatch = int((lib_f != ref).sum())
        assert mismatch == 0, (
            f"plane {p}: {mismatch}/{lib_f.size} 系数与参考不符"
            f"（最大偏差 {int(np.abs(lib_f - ref).max())}）"
        )
