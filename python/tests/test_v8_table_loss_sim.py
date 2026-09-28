"""V8 批 0：瓦片聚合表统计损失模拟器差分验收。

``tc_dev_rans2_tile_sim``（codec.c dev 实验入口，不进生产路径）对 tok
捕获会话重放 V7-R2 编码：

- A 锚：重导 hist/joints + 生产同构模型选择/发射，逐带 payload 必须
  与生产相等——任一失配模拟器报 MALFORMED，本测试断言失败；
- B：瓦片聚合计数模型（表每瓦片一份 + 每带瓦片模型流）。

本文件钉：A 锚通过（混合内容合成帧）、B 确定性（两次调用全等）、
段簿记解析值（seg_blocks=16 手算几何）、b_table 每瓦片 ≤ 前缀上界
350B、非法参数拒绝、无捕获会话拒绝。
"""
from __future__ import annotations

import ctypes

import numpy as np
import pytest

from topos_codec.topos_binding import (
    TOPOS_CODEC_ABI_VERSION, ToposCodec, _CFrameConfig,
)


class _CTileSimOut(ctypes.Structure):
    _fields_ = [
        ("a_total", ctypes.c_uint64),
        ("a_prefix", ctypes.c_uint64),
        ("a_stream", ctypes.c_uint64),
        ("b_total", ctypes.c_uint64),
        ("b_table", ctypes.c_uint64),
        ("b_stream", ctypes.c_uint64),
        ("n_bands", ctypes.c_uint32),
        ("n_tiles", ctypes.c_uint32),
        ("tile_rows", ctypes.c_uint32),
        ("seg_blocks", ctypes.c_uint32),
        ("seg_count", ctypes.c_uint64),
        ("seg_dir_bytes", ctypes.c_uint64),
        ("seg_state_extra", ctypes.c_uint64),
        ("flags_lvl", ctypes.c_uint32 * 3),
        ("flags_dc", ctypes.c_uint32 * 2),
    ]


def _bind_sim(lib) -> None:
    lib.tc_dev_rans2_tile_sim.argtypes = [
        ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(_CTileSimOut),
    ]
    lib.tc_dev_rans2_tile_sim.restype = ctypes.c_int32


def _make_fc(w: int, h: int, qp: int, slice_rows: int) -> _CFrameConfig:
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
    fc.slice_rows = slice_rows
    fc.sar_num = fc.sar_den = 1
    fc.reserved[0] = 8  # V7-R2
    return fc


def _synthetic_422(w: int, h: int, seed: int) -> list[bytes]:
    """左半噪声 + 右半梯度（混合统计，双族模型选择都有覆盖）。"""
    rng = np.random.default_rng(seed)
    y = np.zeros((h, w), dtype="<u2")
    y[:, : w // 2] = rng.integers(0, 1 << 10, (h, w // 2))
    xs = np.arange(w - w // 2, dtype=np.float64)
    y[:, w // 2:] = ((xs[None, :] / max(1, w - w // 2 - 1)) * 1023.0
                     + 0.5).astype("<u2")
    cb = np.tile(((np.arange(w // 2) * 7) % 1024).astype("<u2"), (h, 1))
    cr = np.tile(((np.arange(w // 2) * 13 + 3) % 1024).astype("<u2"), (h, 1))
    return [y.tobytes(), cb.tobytes(), cr.tobytes()]


def _sim(lib, tile_rows: int, seg_blocks: int) -> _CTileSimOut:
    o = _CTileSimOut()
    rc = lib.tc_dev_rans2_tile_sim(tile_rows, seg_blocks, ctypes.byref(o))
    assert rc == 0, f"tile sim rc={rc}（A 锚差分失配或参数/会话错误）"
    return o


@pytest.mark.parametrize("seed", [7, 8])
def test_tile_sim_anchor_and_semantics(seed: int) -> None:
    codec = ToposCodec()
    lib = codec._lib
    _bind_sim(lib)
    w, h, qp = 160, 120, 30
    data = _synthetic_422(w, h, seed)
    lib.tc_dev_set_tok_capture(1)
    try:
        _pkt, _st = codec.encode_frame(_make_fc(w, h, qp, slice_rows=4), data)
        o0 = _sim(lib, 0, 16)          # 整平面一片
        o16 = _sim(lib, 16, 16)        # 16 块行一片
    finally:
        lib.tc_dev_set_tok_capture(0)

    # A 锚已由 rc==0 背书；账目自洽
    assert o0.a_prefix + o0.a_stream == o0.a_total
    assert o0.a_total > 0 and o0.b_stream > 0
    assert o0.b_total == o0.b_table + o0.b_stream

    # 段簿记解析值：blocks = 300/150/150，S = 19+10+10 = 39
    assert o0.seg_count == 39
    assert o0.seg_dir_bytes == 12 * 39
    # n_bands = ceil(15/4)×3 = 12 → 额外终态 4×(39−12)
    assert o0.n_bands == 12
    assert o0.seg_state_extra == 4 * (39 - 12)
    # b_table：每瓦片 ≤ TC_RANS2_PREFIX_MAX(350)
    assert o0.n_tiles == 3
    assert o0.b_table <= 3 * 350
    # tile_rows=16：15 块行 < 16 → 每 plane 仍 1 片（n_tiles=3）

    # 确定性：同捕获会话内重复调用全等（重新捕获同帧）
    lib.tc_dev_set_tok_capture(1)
    try:
        _pkt2, _st2 = codec.encode_frame(_make_fc(w, h, qp, slice_rows=4), data)
        o0b = _sim(lib, 0, 16)
    finally:
        lib.tc_dev_set_tok_capture(0)
    assert o0b.a_total == o0.a_total
    assert o0b.b_total == o0.b_total
    assert o0b.seg_count == o0.seg_count
    assert list(o0b.flags_lvl) == list(o0.flags_lvl)


def test_tile_sim_rejects_bad_args() -> None:
    codec = ToposCodec()
    lib = codec._lib
    _bind_sim(lib)
    o = _CTileSimOut()
    # 无捕获会话（本进程内 capture 已关）→ 拒绝
    assert lib.tc_dev_rans2_tile_sim(0, 16, ctypes.byref(o)) != 0
    # 非法 seg_blocks
    lib.tc_dev_set_tok_capture(1)
    try:
        data = _synthetic_422(160, 120, 9)
        _pkt, _st = codec.encode_frame(_make_fc(160, 120, 30, slice_rows=4),
                                       data)
        assert lib.tc_dev_rans2_tile_sim(0, 0, ctypes.byref(o)) != 0
        assert lib.tc_dev_rans2_tile_sim(0, 257, ctypes.byref(o)) != 0
        # 合法调用后再次关会话 → 回到拒绝态
        assert lib.tc_dev_rans2_tile_sim(8, 16, ctypes.byref(o)) == 0
    finally:
        lib.tc_dev_set_tok_capture(0)
    assert lib.tc_dev_rans2_tile_sim(0, 16, ctypes.byref(o)) != 0
