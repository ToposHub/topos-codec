"""V8 批 5：损伤流 conceal 矩阵（conformance）。

系统性覆盖 损伤位置 × 粒度档 的瓦片域定界语义：
- 结构性损伤（扩展头保留位/段目录裂缝/段长/截断/拖尾）→ 扫描期明确错误码；
- 载荷损伤（瓦片表字节/段流数据/段流终态/存储 CRC）→ 对应瓦片 conceal：
  坏瓦片像素 == mid（10bit → 512），好瓦片与其余平面 == 参考解码位精确；
- concealed_slices 计数与 slice_status 逐位一致。

布局解析为纯 Python 算术（53B 头 + 8B 扩展头 + 逐平面
[dir S×12B][表 T×350B][段流 Σlen][CRC T×4B]），尾断言 == 包长自校验。
"""
from __future__ import annotations

import ctypes

import numpy as np
import pytest

from topos_codec.topos_binding import (
    TOPOS_CODEC_ABI_VERSION, ToposCodec,
)
from topos_codec import topos_binding as tb
from test_v8_roundtrip import _planes

_W, _H, _QP = 160, 280, 30  # 35 块行：tile=32→2 瓦片/面、tile=16→3、tile=64→1
_MID_10 = 512

# (sb_log2, tile_log2) → 每平面瓦片数 T（35 块行）
_MATRIX_CONFIGS = [(4, 5), (3, 4), (5, 6)]


def _make_fc(w: int, h: int, qp: int, sb_log2: int | None = None,
             tr_log2: int | None = None):
    from topos_codec.topos_binding import _CFrameConfig

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
    fc.reserved[0] = 9
    if sb_log2 is not None:
        fc.reserved[3] = sb_log2
    if tr_log2 is not None:
        fc.reserved[4] = tr_log2
    return fc


def _decode(codec: ToposCodec, pkt: bytes, w: int, h: int):
    planes_ptr = (ctypes.POINTER(ctypes.c_ushort) * 4)()
    strides = (ctypes.c_size_t * 4)()
    holders = []
    dims = [(h, w), (h, w // 2), (h, w // 2)]
    for i, (rows, cols) in enumerate(dims):
        arr = np.zeros((rows, cols), dtype="<u2")
        holders.append(arr)
        planes_ptr[i] = ctypes.cast(arr.ctypes.data,
                                    ctypes.POINTER(ctypes.c_ushort))
        strides[i] = cols
    out_info = tb._CFrameOutput()
    out_info.struct_size = ctypes.sizeof(tb._CFrameOutput)
    out_info.abi_version = TOPOS_CODEC_ABI_VERSION
    buf = (ctypes.c_ubyte * len(pkt)).from_buffer_copy(pkt)
    rc = codec._lib.tc_frame_decode(ctypes.cast(buf, ctypes.c_void_p),
                                    ctypes.c_size_t(len(pkt)), planes_ptr,
                                    strides, ctypes.byref(out_info))
    return rc, holders, out_info


def _v8_layout(pkt: bytes, w: int, h: int, sb_log2: int, tr_log2: int):
    """纯 Python 布局解析（与 C scan 同构）；尾断言 == 包长。"""
    sb, tr = 1 << sb_log2, 1 << tr_log2
    off = 53 + 8
    planes = []
    for p in range(3):
        cols = (w + 7) // 8 if p == 0 else (w + 15) // 16
        rows = (h + 7) // 8
        s_cnt = (cols * rows + sb - 1) // sb
        t_cnt = (rows + tr - 1) // tr
        dir_off = off
        off += s_cnt * 12
        tab_off = off
        off += t_cnt * 350
        lens = [int.from_bytes(pkt[dir_off + i * 12 + 4:dir_off + i * 12 + 8],
                               "big") for i in range(s_cnt)]
        str_off = off
        off += sum(lens)
        crc_off = off
        off += t_cnt * 4
        planes.append(dict(cols=cols, rows=rows, s=s_cnt, t=t_cnt,
                           dir=dir_off, tab=tab_off, stream=str_off,
                           lens=lens, crc=crc_off))
    assert off == len(pkt), f"布局自校验失败 {off} != {len(pkt)}"
    return planes


def _tile_abs_index(layout, plane: int, tile: int) -> int:
    return sum(layout[p]["t"] for p in range(plane)) + tile


def _tile_pixel_rows(layout, plane: int, tile: int, tr: int) -> tuple[int, int]:
    """瓦片 t 的可见像素行区间（块行 ×8 截到可见高）。"""
    rows = layout[plane]["rows"]
    y0 = min(tile * tr, rows) * 8
    y1 = min((tile + 1) * tr, rows) * 8
    return y0, y1


@pytest.fixture(scope="module")
def _module_codec():
    return ToposCodec()


@pytest.mark.parametrize("sb_log2,tr_log2", _MATRIX_CONFIGS)
def test_v8_control_clean_bit_exact(_module_codec, sb_log2: int, tr_log2: int) -> None:
    """对照：无损编码 → 解码位精确（q-hash 之外再钉一层像素级对照）。"""
    codec = _module_codec
    data = _planes(_W, _H, 0x51)
    pkt, _ = codec.encode_frame(_make_fc(_W, _H, _QP, sb_log2, tr_log2), data)
    rc, planes, info = _decode(codec, pkt, _W, _H)
    assert rc == 0 and info.concealed_slices == 0
    for p in range(3):
        got = np.frombuffer(data[p], "<u2").reshape(_H, -1)
        assert np.array_equal(planes[p], got), f"plane {p} 不一致"


@pytest.mark.parametrize("sb_log2,tr_log2", _MATRIX_CONFIGS)
@pytest.mark.parametrize("damage", ["table_byte", "stream_data",
                                    "stream_terminal", "crc_byte"])
def test_v8_damage_tile_scoped_conceal(_module_codec, sb_log2: int, tr_log2: int,
                                       damage: str) -> None:
    codec = _module_codec
    tr = 1 << tr_log2
    data = _planes(_W, _H, 0x51)
    pkt, _ = codec.encode_frame(_make_fc(_W, _H, _QP, sb_log2, tr_log2), data)
    layout = _v8_layout(pkt, _W, _H, sb_log2, tr_log2)

    plane, tile = 1, (layout[1]["t"]) // 2  # 中间瓦片：两侧好瓦片可断言
    lp = layout[plane]
    bad = bytearray(pkt)
    if damage == "table_byte":
        bad[lp["tab"] + tile * 350 + 17] ^= 0xFF
    elif damage == "stream_data":
        first = (tile * tr * lp["cols"]) // (1 << sb_log2)  # seg_first(t)
        off_in = lp["dir"] + first * 12
        seg_len = int.from_bytes(pkt[off_in + 4:off_in + 8], "big")
        # 翻该段流数据区中段字节（终态 4B 之后）
        bad[lp["stream"] + int.from_bytes(pkt[off_in:off_in + 4], "big")
            + min(4, seg_len - 1)] ^= 0xFF
    elif damage == "stream_terminal":
        first = (tile * tr * lp["cols"]) // (1 << sb_log2)
        off_in = lp["dir"] + first * 12
        bad[lp["stream"] + int.from_bytes(pkt[off_in:off_in + 4], "big")] ^= 0xFF
    elif damage == "crc_byte":
        bad[lp["crc"] + tile * 4] ^= 0xFF

    rc, planes, info = _decode(codec, bytes(bad), _W, _H)
    assert rc == 0, f"decode rc={rc}"
    assert info.concealed_slices == 1
    statuses = [info.slice_status[i] for i in range(info.slice_count)]
    victim = _tile_abs_index(layout, plane, tile)
    assert statuses[victim] == 1
    assert sum(statuses) == 1

    # 参考解码（clean）
    ref_rc, ref, _ = _decode(codec, pkt, _W, _H)
    assert ref_rc == 0

    y0, y1 = _tile_pixel_rows(layout, plane, tile, tr)
    for p in range(3):
        if p == plane:
            mask = np.zeros(_H, dtype=bool)
            mask[y0:y1] = True
            assert np.all(planes[p][mask] == _MID_10), \
                f"损伤瓦片区非 mid（{damage} plane {p}）"
            assert np.array_equal(planes[p][~mask], ref[p][~mask]), \
                f"好瓦片区被污染（{damage} plane {p}）"
        else:
            assert np.array_equal(planes[p], ref[p]), \
                f"未损伤平面被污染（{damage} plane {p}）"


@pytest.mark.parametrize("damage", ["ext_rsv", "dir_len", "dir_crack",
                                    "truncate", "trailing"])
def test_v8_damage_scan_rejects(_module_codec, damage: str) -> None:
    """结构性损伤 → 扫描期明确失败（解码 rc != 0，不产出帧）。"""
    codec = _module_codec
    sb_log2, tr_log2 = 4, 5
    data = _planes(_W, _H, 0x51)
    pkt, _ = codec.encode_frame(_make_fc(_W, _H, _QP, sb_log2, tr_log2), data)
    layout = _v8_layout(pkt, _W, _H, sb_log2, tr_log2)
    bad = bytearray(pkt)
    if damage == "ext_rsv":
        bad[53 + 3] = 1
    elif damage == "dir_len":
        d0 = layout[0]["dir"]
        bad[d0 + 4:d0 + 8] = (3).to_bytes(4, "big")
    elif damage == "dir_crack":
        d0 = layout[0]["dir"]
        o1 = int.from_bytes(pkt[d0 + 12:d0 + 16], "big")
        bad[d0 + 12:d0 + 16] = (o1 + 8).to_bytes(4, "big")
    elif damage == "truncate":
        bad = bad[:-1]
    elif damage == "trailing":
        bad = bad + b"\x00"
    rc, _planes_out, _info = _decode(codec, bytes(bad), _W, _H)
    assert rc != 0, f"{damage}: 损伤流不应解码成功"
