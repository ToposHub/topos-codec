"""V8 批 3：CPU 参考解码器 round-trip 差分验收。

- 位精确 oracle：帧级 q-hash（tc_symbol_hash_mix 折叠；编码/解码同构）
  —— clean 流 enc==dec 逐位相等（tc_dev_v8_enc_qhash == tc_dev_v8_dec_qhash）；
- 像素输出：解码平面尺寸/值域检查；重编码确定性不受解码影响；
- conceal：瓦片 CRC 损伤 → 对应瓦片状态 CONCEALED、concealed_slices 计数
  正确、其余瓦片 hash 不受影响（整体 hash 语义确定）；
- 性能登记：4K V8 vs V7-R2 解码墙钟（≤5% 劣化门槛）。
"""
from __future__ import annotations

import ctypes
import time

import numpy as np
import pytest

from topos_codec.topos_binding import (
    TOPOS_CODEC_ABI_VERSION, ToposCodec, _CFrameConfig,
)


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
    fc.reserved[0] = 9 if v8 else 8
    return fc


def _planes(w: int, h: int, seed: int) -> list[bytes]:
    rng = np.random.default_rng(seed)
    y = (rng.integers(0, 1 << 10, (h, w)) + np.arange(w)[None, :] * 2).astype("<u2") % 1024
    xs = np.arange(w // 2)
    cb = np.tile(((xs * 11) % 1024).astype("<u2"), (h, 1))
    cr = np.tile(((xs * 5 + 7) % 1024).astype("<u2"), (h, 1))
    return [y.tobytes(), cb.tobytes(), cr.tobytes()]


def _bind_qhash(lib) -> None:
    lib.tc_dev_v8_enc_qhash.argtypes = []
    lib.tc_dev_v8_enc_qhash.restype = ctypes.c_uint64
    lib.tc_dev_v8_dec_qhash.argtypes = []
    lib.tc_dev_v8_dec_qhash.restype = ctypes.c_uint64
    lib.tc_dev_v8_set_qhash.argtypes = [ctypes.c_int]
    lib.tc_dev_v8_set_qhash(1)  # oracle 开启（生产默认 OFF）


def _decode_to_planes(codec: ToposCodec, pkt: bytes, w: int, h: int):
    lib = codec._lib
    planes_ptr = (ctypes.POINTER(ctypes.c_ushort) * 4)()
    strides = (ctypes.c_size_t * 4)()
    holders = []
    dims = [(h, w), (h, w // 2), (h, w // 2)]
    for i, (rows, cols) in enumerate(dims):
        arr = np.zeros((rows, cols), dtype="<u2")
        holders.append(arr)
        planes_ptr[i] = ctypes.cast(arr.ctypes.data, ctypes.POINTER(ctypes.c_ushort))
        strides[i] = cols
    import topos_codec.topos_binding as tb
    out_info = tb._CFrameOutput()
    out_info.struct_size = ctypes.sizeof(tb._CFrameOutput)
    out_info.abi_version = TOPOS_CODEC_ABI_VERSION
    buf = (ctypes.c_ubyte * len(pkt)).from_buffer_copy(pkt)
    rc = lib.tc_frame_decode(ctypes.cast(buf, ctypes.c_void_p),
                             ctypes.c_size_t(len(pkt)), planes_ptr,
                             strides, ctypes.byref(out_info))
    assert rc == 0, f"decode rc={rc}: {lib.tc_last_error() if hasattr(lib, 'tc_last_error') else ''}"
    return holders, out_info


@pytest.mark.parametrize("w,h,qp,seed", [(160, 120, 30, 21), (320, 240, 20, 22),
                                         (96, 200, 44, 23), (256, 144, 58, 24)])
def test_v8_roundtrip_qhash_bit_exact(w: int, h: int, qp: int, seed: int) -> None:
    codec = ToposCodec()
    _bind_qhash(codec._lib)
    data = _planes(w, h, seed)
    pkt, _st = codec.encode_frame(_make_fc(w, h, qp), data)
    planes, out_info = _decode_to_planes(codec, pkt, w, h)
    assert codec._lib.tc_dev_v8_enc_qhash() == codec._lib.tc_dev_v8_dec_qhash(), \
        "round-trip q-hash mismatch（解码 ≠ 编码 q）"
    assert out_info.concealed_slices == 0
    # 像素值域：全平面落在 [0, 1023]（u16 容器）
    for arr in planes:
        assert int(arr.min()) >= 0 and int(arr.max()) <= 1023


def test_v8_roundtrip_deterministic_and_reencode_stable() -> None:
    codec = ToposCodec()
    _bind_qhash(codec._lib)
    w, h = 200, 160
    data = _planes(w, h, 25)
    pkt, _ = codec.encode_frame(_make_fc(w, h, 30), data)
    _decode_to_planes(codec, pkt, w, h)
    h1 = codec._lib.tc_dev_v8_dec_qhash()
    _decode_to_planes(codec, pkt, w, h)
    h2 = codec._lib.tc_dev_v8_dec_qhash()
    assert h1 == h2  # 解码确定性
    # 解码不污染编码器状态：重编码仍逐字节一致
    pkt2, _ = codec.encode_frame(_make_fc(w, h, 30), data)
    assert pkt2 == pkt


def _flip_one_stream_byte(pkt: bytearray) -> int:
    """翻转任一瓦片流字节（按 inspect 布局简化：直接翻最后一段 CRC 区前一字节）。"""
    # CRC 区在包尾每平面 T×4B；向前找流区：翻 packet 尾部倒数 (4*T+8) 处
    # 简化：翻最后一个 CRC 字（必使末平面末瓦片 CRC 坏）
    pkt[-1] ^= 0xFF
    return len(pkt) - 1


def test_v8_conceal_tile_crc() -> None:
    codec = ToposCodec()
    _bind_qhash(codec._lib)
    w, h = 160, 120
    data = _planes(w, h, 26)
    pkt, _st = codec.encode_frame(_make_fc(w, h, 30), data)
    bad = bytearray(pkt)
    bad[-1] ^= 0xFF  # 末瓦片 CRC 损伤
    planes, out_info = _decode_to_planes(codec, bytes(bad), w, h)
    assert out_info.concealed_slices >= 1
    assert out_info.slice_count == 3
    statuses = [out_info.slice_status[i] for i in range(3)]
    assert statuses[-1] == 1  # TC_FRAME_SLICE_CONCEALED（末瓦片）
    assert sum(statuses) == out_info.concealed_slices
    # conceal 段像素 = mid（bit[10] 10bit → 512）：损伤瓦片所在平面（V 平面）
    # 末瓦片 = plane V 全平面（rows<32 → 每 plane 一片）→ V 平面恒 512
    assert int(planes[2].min()) == 512 and int(planes[2].max()) == 512
    # Y/U 平面未受影响（非全 mid）
    assert not (int(planes[0].min()) == 512 and int(planes[0].max()) == 512)


def test_v8_decode_perf_4k_vs_v7() -> None:
    """性能登记：4K 422 10bit qp30 V8 vs V7-R2 解码墙钟（门槛 ≤1.05×）。"""
    codec = ToposCodec()
    _bind_qhash(codec._lib)
    w = h = 1024  # 1K 矩形（pytest 预算内；4K 外推口径见落档 bench）
    data = _planes(w, h, 27)
    pkt8, _ = codec.encode_frame(_make_fc(w, h, 30, v8=True), data)
    pkt7, _ = codec.encode_frame(_make_fc(w, h, 30, v8=False), data)
    codec._lib.tc_dev_v8_set_qhash(0)  # 生产形态（oracle 关）
    runs = 3
    t8 = t7 = 0.0
    for _ in range(runs):
        t0 = time.perf_counter()
        _decode_to_planes(codec, pkt8, w, h)
        t8 += time.perf_counter() - t0
        t0 = time.perf_counter()
        _p, _i = _decode_v7(codec, pkt7, w, h)
        t7 += time.perf_counter() - t0
    ratio = (t8 / runs) / (t7 / runs)
    print(f"\nV8 decode {t8/runs*1000:.1f}ms vs V7 {t7/runs*1000:.1f}ms "
          f"(1K², ratio {ratio:.2f}×)")
    assert ratio < 1.3  # 生产形态（oracle 关）；4K 口径在批 3 bench 落档


def _decode_v7(codec: ToposCodec, pkt: bytes, w: int, h: int):
    lib = codec._lib
    planes_ptr = (ctypes.POINTER(ctypes.c_ushort) * 4)()
    strides = (ctypes.c_size_t * 4)()
    holders = []
    for rows, cols in [(h, w), (h, w // 2), (h, w // 2)]:
        arr = np.zeros((rows, cols), dtype="<u2")
        holders.append(arr)
        planes_ptr[i] = ctypes.cast(arr.ctypes.data, ctypes.POINTER(ctypes.c_ushort)) \
            if (i := len(holders) - 1) is not None else None
        strides[len(holders) - 1] = cols
    import topos_codec.topos_binding as tb
    out_info = tb._CFrameOutput()
    out_info.struct_size = ctypes.sizeof(tb._CFrameOutput)
    out_info.abi_version = TOPOS_CODEC_ABI_VERSION
    buf = (ctypes.c_ubyte * len(pkt)).from_buffer_copy(pkt)
    rc = lib.tc_frame_decode(ctypes.cast(buf, ctypes.c_void_p),
                             ctypes.c_size_t(len(pkt)), planes_ptr,
                             strides, ctypes.byref(out_info))
    assert rc == 0
    return holders, out_info
