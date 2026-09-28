"""Topos Codec Python 绑定示例（阶段 10）：encode → mux → 重新打开 → decode。

等价于 examples/encode_decode.c 的 C 示例，面向以 Topos Color 集成方式
（python/topos_codec/topos_binding.py）接入的开发方。

运行（仓库内，需已构建 native 库）：
    TOPOS_CODEC_LIB=native/build/debug/libtopos_codec.dylib \
        python3 native/examples/topos_roundtrip.py
"""
from __future__ import annotations

import ctypes
import os
import struct
import sys
import tempfile

sys.path.insert(
    0,
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "src"),
)

from topos_codec import topos_binding  # noqa: E402
from topos_codec.topos_binding import (  # noqa: E402
    ToposCodec,
    ToposMuxFile,
    ToposMovieFile,
)

W, H, FRAMES = 320, 180, 12
CW = (W + 1) // 2


def plane_u16(values: list[int]) -> bytes:
    """uint16 LE 紧凑平面字节。"""
    return struct.pack(f"<{len(values)}H", *values)


def make_planes(frame_i: int) -> list[bytes]:
    y = plane_u16([(r * 4 + c + frame_i * 7) % 1024 for r in range(H) for c in range(W)])
    u = plane_u16([c * 1023 // (CW - 1) for _ in range(H) for c in range(CW)])
    v = plane_u16([r * 1023 // (H - 1) for r in range(H) for _ in range(CW)])
    a = plane_u16([c * 65535 // (W - 1) for _ in range(H) for c in range(W)])
    return [y, u, v, a]


def main() -> int:
    codec = ToposCodec()

    # 能力协商（阶段 10 公共 API）
    if codec.query_support(3, 0, 10, 2) != 0:
        print("query_support: 不支持 profile3/YUV422/10bit/alpha2")
        return 1

    # 帧配置（与 movie_config 字段保持一致：tpcC 规则）
    fc = topos_binding._CFrameConfig()
    fc.struct_size = ctypes.sizeof(topos_binding._CFrameConfig)
    fc.abi_version = codec.abi_version
    fc.visible_width, fc.visible_height = W, H
    fc.qp_base = 28
    fc.qmatrix_id = 1
    fc.alpha_mode = 2
    fc.alpha_bit_depth = 12
    fc.color_range = 1
    fc.color_primaries = 1
    fc.color_transfer = 1
    fc.color_matrix = 1
    fc.sar_num, fc.sar_den = 1, 1

    path = os.path.join(tempfile.mkdtemp(), "demo.mov")
    movie_cfg = codec.movie_config(width=W, height=H, qp=28, qmatrix=1,
                                   alpha_mode=2, alpha_bit_depth=12)
    mux = ToposMuxFile(codec, path, movie_cfg)
    for i in range(FRAMES):
        packet, stats = codec.encode_frame(fc, make_planes(i))
        mux.add_packet(packet, int(i * 1000), 1000)
        assert stats.packet_size == len(packet)
        assert stats.alpha_max_abs_error <= 16, stats.alpha_max_abs_error
    mux.finish()
    mux.close()

    movie = ToposMovieFile(codec, path)
    summary = movie.info()
    assert summary.sample_count == FRAMES, summary.sample_count
    frame = codec.decode(movie.packet(0))
    assert frame.info.concealed_slices == 0
    assert frame.info.visible_width == W and frame.info.visible_height == H
    assert frame.info.alpha_mode == 2
    movie.close()

    print(f"topos_roundtrip.py: PASS（{FRAMES} 帧，{os.path.getsize(path)} 字节）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
