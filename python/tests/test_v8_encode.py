"""V8 批 2：段化多状态 rANS 编码器验收（Python 侧）。

- 结构：V8 包过 topos_inspect（scan_v8 同源）——tiles/CRC/段数一致；
- 确定性：同输入两次编码逐字节一致；
- qp 单调：码率随 qp 上升不增；
- 入口契约：查询模式（planes_out=NULL）TC_OK、实解码 NOT_IMPLEMENTED
  （批 3）、sized 编码 NOT_IMPLEMENTED（批 5）。
"""
from __future__ import annotations

import ctypes
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

from topos_codec.topos_binding import (
    TOPOS_CODEC_ABI_VERSION, ToposCodec, _CFrameConfig,
)

REPO = Path(__file__).resolve().parents[2]
# Windows：exe 带 .exe 后缀，且 VS 多配置生成器把产物放在配置子目录
# （build/release/Release/）；Unix 单配置惯例为 build/release/ 直达。
_EXE = ".exe" if sys.platform == "win32" else ""
INSPECT_CANDIDATES = [
    REPO / ("native/build/release/topos_inspect" + _EXE),
    REPO / "native/build/release/Release/topos_inspect.exe",
    # VS 多配置生成器的工具链命名目录（win-msvc/win-release-zig 等）
    *sorted((REPO / "native/build").glob("*/Release/topos_inspect.exe")),
]
INSPECT = next((p for p in INSPECT_CANDIDATES if p.exists()),
               INSPECT_CANDIDATES[0])


def _make_fc(w: int, h: int, qp: int, v8: bool = True) -> _CFrameConfig:
    fc = _CFrameConfig()
    fc.struct_size = ctypes.sizeof(_CFrameConfig)
    fc.abi_version = TOPOS_CODEC_ABI_VERSION
    fc.visible_width = w
    fc.visible_height = h
    fc.profile = 3
    fc.pixel_format = 0
    fc.bit_depth = 10
    fc.qp_base = qp
    fc.qmatrix_id = 1
    fc.reserved[0] = 9 if v8 else 8  # 9 = V8；8 = V7-R2
    return fc


def _planes(w: int, h: int, seed: int) -> list[bytes]:
    rng = np.random.default_rng(seed)
    y = rng.integers(0, 1 << 10, (h, w)).astype("<u2")
    xs = np.arange(w // 2)
    cb = np.tile(((xs * 11) % 1024).astype("<u2"), (h, 1))
    cr = np.tile(((xs * 5 + 7) % 1024).astype("<u2"), (h, 1))
    return [y.tobytes(), cb.tobytes(), cr.tobytes()]


def _inspect(pkt: bytes) -> str:
    assert INSPECT.exists(), "topos_inspect 未构建"
    p = REPO / ".v8_pytest_tmp.tpkt"
    p.write_bytes(pkt)
    try:
        out = subprocess.run([str(INSPECT), str(p)], capture_output=True,
                             text=True, timeout=60)
        assert out.returncode == 0, out.stderr
        return out.stdout
    finally:
        p.unlink(missing_ok=True)


@pytest.mark.parametrize("w,h", [(160, 120), (96, 200)])
def test_v8_encode_structure_and_determinism(w: int, h: int) -> None:
    codec = ToposCodec()
    fc = _make_fc(w, h, 30)
    data = _planes(w, h, 11)
    pkt1, st1 = codec.encode_frame(fc, data)
    pkt2, st2 = codec.encode_frame(fc, data)
    assert len(pkt1) == len(pkt2) == st1.packet_size
    assert pkt1 == pkt2  # 确定性（位精确重编码）

    report = _inspect(pkt1)
    assert "version 8.0" in report
    assert "crc_bad 0" in report
    assert "segment_blocks 16" in report   # 批 0 默认档
    assert "tile_rows 32" in report
    # 段数 = ceil(blocks/16)：luma (w/8)*(h/8)，chroma 减半
    blocks_l = (w // 8) * (h // 8)
    segs_expect = (blocks_l + 15) // 16
    assert f"segments {segs_expect}/" in report
    # 瓦片数：15 行 < 32 → 每 plane 1 片 → slice_count=3
    assert st1.slice_count == 3


def test_v8_qp_monotonic() -> None:
    codec = ToposCodec()
    w, h = 160, 120
    data = _planes(w, h, 12)
    sizes = []
    for qp in (20, 30, 44):
        pkt, _st = codec.encode_frame(_make_fc(w, h, qp), data)
        sizes.append(len(pkt))
    assert sizes[0] >= sizes[1] >= sizes[2]


@pytest.mark.parametrize("sb_log2,tile_log2", [(3, 4), (4, 5), (5, 6)])
def test_v8_granularity_signaling(sb_log2: int, tile_log2: int) -> None:
    """reserved[3]/[4] 覆盖段/瓦片粒度（批 1 布局枚举）→ 编码 + scan 自检。

    注：编码端 reserved[4]=0 语义为"用默认档 32 块行"（tile_log2=0 的
    整平面信号化仅解码侧定义，由 test_v8_format.c 扫描用例覆盖）。"""
    codec = ToposCodec()
    w, h = 160, 120
    fc = _make_fc(w, h, 30)
    fc.reserved[3] = sb_log2
    fc.reserved[4] = tile_log2
    pkt, st = codec.encode_frame(fc, _planes(w, h, 18))
    assert st.packet_size == len(pkt)
    report = _inspect(pkt)
    assert "crc_bad 0" in report
    assert f"segment_blocks {1 << sb_log2}" in report
    assert f"tile_rows {1 << tile_log2}" in report


def test_v8_granularity_invalid_rejected() -> None:
    codec = ToposCodec()
    fc = _make_fc(160, 120, 30)
    fc.reserved[3] = 6  # sb_log2 ∉ {3,4,5}
    with pytest.raises(Exception):
        codec.encode_frame(fc, _planes(160, 120, 19))
    fc = _make_fc(160, 120, 30)
    fc.reserved[4] = 3  # tile_log2 ∉ {0,4,5,6}
    with pytest.raises(Exception):
        codec.encode_frame(fc, _planes(160, 120, 19))


# 批 2 验收：golden V8 基准流冻结（SHA256；跨批回归硬门禁——
# 任何触碰共享原语/组装路径的改动若移动这些哈希即为位流破坏）。
#: 2026-09-21 重钉：dev 合并带来 H1（sRGB 默认 transfer=13，帧头 B34
#: 1→13）——C 侧 conformance 金样已随 dev 更新，本 dict 漏更导致漂移。
#: 重钉值由当前构建逐字节复现（qmatrix_id=1 路径与边缘均衡战役无关）。
V8_GOLDEN_SHA = {
    (160, 120, 30, 11): "b89038a25c403bc022842c95d0b6a44cb3efe1dc7bfe61921eadb76ae5a6bb77",
    (320, 240, 20, 15): "a6019c39c58a82a9ae69816dd053cf97585eec92d5b54a987a1bca4606dc52f4",
    (96, 200, 44, 17): "f45150ef4d40969126ca1a9587030020fc40326c1d0aa4e96f418165fb1f799d",
}


@pytest.mark.parametrize("w,h,qp,seed", sorted(V8_GOLDEN_SHA))
def test_v8_golden_streams(w: int, h: int, qp: int, seed: int) -> None:
    import hashlib
    codec = ToposCodec()
    pkt, _st = codec.encode_frame(_make_fc(w, h, qp), _planes(w, h, seed))
    assert hashlib.sha256(pkt).hexdigest() == V8_GOLDEN_SHA[(w, h, qp, seed)]


def test_v8_vs_v7_size_same_frame() -> None:
    """同帧同 qp：V8 vs V7-R2 码率比（登记口径；小帧簿记占比放大）。"""
    codec = ToposCodec()
    w, h = 320, 240
    data = _planes(w, h, 13)
    _p7, st7 = codec.encode_frame(_make_fc(w, h, 30, v8=False), data)
    _p8, st8 = codec.encode_frame(_make_fc(w, h, 30, v8=True), data)
    assert st7.packet_size > 0 and st8.packet_size > 0
    ratio = st8.packet_size / st7.packet_size
    # 小帧（720 块）簿记（3×350B 表 + 39×16B 段簿记）占比放大；放宽带
    assert 0.9 < ratio < 1.6, f"V8/V7 size ratio {ratio:.3f} out of band"


def test_v8_decode_entry_contract() -> None:
    codec = ToposCodec()
    lib = codec._lib
    w, h = 160, 120
    pkt, _st = codec.encode_frame(_make_fc(w, h, 30), _planes(w, h, 14))
    buf = (ctypes.c_ubyte * len(pkt)).from_buffer_copy(pkt)
    info = codec._frame_output() if hasattr(codec, "_frame_output") else None
    # 查询模式：planes_out=NULL → TC_OK
    import topos_codec.topos_binding as tb
    out_info = tb._CFrameOutput()
    out_info.struct_size = ctypes.sizeof(tb._CFrameOutput)
    out_info.abi_version = TOPOS_CODEC_ABI_VERSION
    rc = lib.tc_frame_decode(ctypes.cast(buf, ctypes.c_void_p),
                             ctypes.c_size_t(len(pkt)), None, None,
                             ctypes.byref(out_info))
    assert rc == 0
    assert out_info.visible_width == w
    # 实解码（批 3 落地）：TC_OK + 像素值域
    planes_arr = (ctypes.POINTER(ctypes.c_ushort) * 4)()
    strides_arr = (ctypes.c_size_t * 4)()
    holders = []
    for rows, cols in [(h, w), (h, w // 2), (h, w // 2)]:
        arr = np.zeros((rows, cols), dtype="<u2")
        holders.append(arr)
        planes_arr[len(holders) - 1] = ctypes.cast(
            arr.ctypes.data, ctypes.POINTER(ctypes.c_ushort))
        strides_arr[len(holders) - 1] = cols
    rc = lib.tc_frame_decode(ctypes.cast(buf, ctypes.c_void_p),
                             ctypes.c_size_t(len(pkt)), planes_arr,
                             strides_arr, ctypes.byref(out_info))
    assert rc == 0
    assert int(holders[0].min()) >= 0 and int(holders[0].max()) <= 1023
