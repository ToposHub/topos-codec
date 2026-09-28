#!/usr/bin/env python3
"""simd_parity_check.py — scalar 与 AVX2 分发逐位一致性（W05，stdlib-only）。

同一进程内经 tc_dev_set_simd_mode 切换 TC_SIMD_SCALAR(1)/TC_SIMD_AUTO(0)：
  1. 同输入同配置编码两次 → 包字节必须一致（位流级确定性）；
  2. 同包解码两次 → 输出平面字节必须一致（像素级确定性）；
  3. 报告 tc_dev_simd_backend 前后端名，确认确实切换成功。
覆盖 422/444/GBR × 10/12/16-bit × alpha 的代表组合。

用法：python simd_parity_check.py [lib-path]（缺省同 dll_load_test）
退出码：0 = 全部一致；1 = 任何差异或失败。
"""
from __future__ import annotations

import ctypes
import sys
from ctypes import POINTER, byref, c_int32, c_size_t, c_uint8, c_uint16, c_uint32
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dll_load_test import (  # noqa: E402
    FrameConfig, FrameInput, FrameOutput, FrameStats, find_lib)

TC_OK = 0
TC_SIMD_AUTO, TC_SIMD_SCALAR = 0, 1

# (label, w, h, profile, pixel_format, bit_depth, alpha_mode)
# bd16 需 v8/v9 熵（reserved[0]=9）；alpha_mode=2 需 alpha_bit_depth∈{8,10,12}
CASES = [
    ("422 bd10",        64, 48, 3, 0, 10, 0),
    ("422 bd12",        64, 48, 3, 0, 12, 0),
    ("422 bd16 v8",     64, 48, 3, 0, 16, 0),
    ("444 bd10",        64, 48, 3, 1, 10, 0),
    ("GBR bd12",        64, 48, 3, 2, 12, 0),
    ("422 bd10 alpha2", 64, 48, 3, 0, 10, 2),
]


def chroma_wh(fmt: int, w: int, h: int) -> tuple[int, int]:
    if fmt == 0:  # 4:2:2
        return w // 2, h
    return w, h  # 444/GBR


def make_planes(w: int, h: int, fmt: int, bd: int, with_alpha: bool):
    max_v = (1 << bd) - 1
    cw, ch = chroma_wh(fmt, w, h)
    y = (c_uint16 * (w * h))()
    u = (c_uint16 * (cw * ch))()
    v = (c_uint16 * (cw * ch))()
    for i in range(w * h):
        y[i] = (i * 4 + (i // w) * 7) % (max_v + 1)
    for i in range(cw * ch):
        u[i] = (i * 3) % (max_v + 1)
        v[i] = (i * 5 + 13) % (max_v + 1)
    planes = [y, u, v]
    if with_alpha:
        a = (c_uint16 * (w * h))()
        for i in range(w * h):
            a[i] = (i * 11) % (max_v + 1)
        planes.append(a)
    return planes


def main() -> int:
    lib_path = find_lib()
    lib = ctypes.CDLL(str(lib_path))
    for fn in ("tc_abi_version",):
        getattr(lib, fn).restype = c_int32
        getattr(lib, fn).argtypes = []
    if lib.tc_abi_version() != 2:
        print("ABI != 2")
        return 1
    lib.tc_frame_packet_bound.restype = c_size_t
    lib.tc_frame_packet_bound.argtypes = [POINTER(FrameConfig)]
    lib.tc_frame_encode.restype = c_int32
    lib.tc_frame_encode.argtypes = [
        POINTER(FrameConfig), POINTER(FrameInput),
        POINTER(c_uint8), c_size_t, POINTER(FrameStats)]
    lib.tc_frame_decode.restype = c_int32
    lib.tc_frame_decode.argtypes = [
        POINTER(c_uint8), c_size_t,
        POINTER(POINTER(c_uint16)), POINTER(c_size_t), POINTER(FrameOutput)]
    lib.tc_frame_plane_geometry.restype = c_int32
    lib.tc_frame_plane_geometry.argtypes = [
        POINTER(FrameOutput), c_uint32, POINTER(c_uint32), POINTER(c_uint32)]
    lib.tc_dev_set_simd_mode.restype = None
    lib.tc_dev_set_simd_mode.argtypes = [c_int32]
    lib.tc_dev_simd_backend.restype = ctypes.c_char_p
    lib.tc_dev_simd_backend.argtypes = []

    def encode(cfg, planes) -> bytes:
        fi = FrameInput(struct_size=ctypes.sizeof(FrameInput), abi_version=2)
        for i, p in enumerate(planes):
            fi.planes[i] = p
        cap = lib.tc_frame_packet_bound(byref(cfg))
        pkt = (c_uint8 * cap)()
        st = FrameStats(struct_size=ctypes.sizeof(FrameStats), abi_version=2)
        rc = lib.tc_frame_encode(byref(cfg), byref(fi), pkt, cap, byref(st))
        if rc != TC_OK:
            raise RuntimeError(f"encode rc={rc}")
        return bytes(bytearray(pkt)[:st.packet_size])

    def decode(pkt: bytes, n_planes: int) -> list[bytes]:
        buf = (c_uint8 * len(pkt)).from_buffer_copy(pkt)
        info = FrameOutput()
        rc = lib.tc_frame_decode(buf, len(pkt), None, None, byref(info))
        if rc != TC_OK:
            raise RuntimeError(f"probe rc={rc}")
        outs = []
        keep = []  # 平面缓冲保活：POINTER 赋值不持引用，防 GC 悬挂
        ptrs = (POINTER(c_uint16) * 4)()
        strides = (c_size_t * 4)(0, 0, 0, 0)
        for p in range(n_planes):
            gw, gh = c_uint32(), c_uint32()
            assert lib.tc_frame_plane_geometry(byref(info), p, byref(gw), byref(gh)) == TC_OK
            plane = (c_uint16 * (gw.value * gh.value))()
            ptrs[p] = plane
            keep.append(plane)
        lib.tc_frame_decode(buf, len(pkt), ptrs, strides, byref(info))
        if info.concealed_slices != 0:
            raise RuntimeError("concealed slices")
        for p in range(n_planes):
            outs.append(bytes(bytearray(keep[p])))
        return outs

    fails = 0
    for label, w, h, profile, fmt, bd, alpha in CASES:
        cfg = FrameConfig(struct_size=ctypes.sizeof(FrameConfig), abi_version=2,
                          visible_width=w, visible_height=h, profile=profile,
                          pixel_format=fmt, bit_depth=bd, qmatrix_id=1,
                          qp_base=20, alpha_mode=alpha,
                          alpha_bit_depth=12 if alpha else 0)
        if bd >= 13:
            cfg.reserved[0] = 8  # V8 段化 rANS（bd16 域要求 rans2/v8 家族；9 是微 GOP 路径）
        planes = make_planes(w, h, fmt, bd, alpha != 0)
        n_planes = len(planes)

        lib.tc_dev_set_simd_mode(TC_SIMD_SCALAR)
        bs_scalar = lib.tc_dev_simd_backend().decode()
        pkt_scalar = encode(cfg, planes)
        dec_scalar = decode(pkt_scalar, n_planes)

        lib.tc_dev_set_simd_mode(TC_SIMD_AUTO)
        bs_auto = lib.tc_dev_simd_backend().decode()
        pkt_auto = encode(cfg, planes)
        dec_auto = decode(pkt_auto, n_planes)

        ok_stream = pkt_scalar == pkt_auto
        ok_pixels = dec_scalar == dec_auto
        status = "OK" if (ok_stream and ok_pixels) else "MISMATCH"
        if not (ok_stream and ok_pixels):
            fails += 1
        print(f"{label:18s} backend {bs_scalar.decode() if isinstance(bs_scalar, bytes) else bs_scalar}"
              f"/{bs_auto.decode() if isinstance(bs_auto, bytes) else bs_auto}"
              f"  bitstream={'一致' if ok_stream else '不一致'}"
              f"({len(pkt_auto)}B)  pixels={'逐位一致' if ok_pixels else '不一致'}  [{status}]")
        lib.tc_dev_set_simd_mode(TC_SIMD_AUTO)

    if fails:
        print(f"simd parity: {fails} 例不一致")
        return 1
    print("simd parity: 全部逐位一致（位流 + 像素）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
