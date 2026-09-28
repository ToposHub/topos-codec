"""V16：产品 MOV 路径必须能承载 native 已声明的 16-bit 视频。"""

from __future__ import annotations

from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

from topos_codec.topos_binding import (
    ToposCodec, ToposDecoder, ToposMovieFile, candidate_library_paths,
)
from topos_codec.topos_encoder import ToposVideoEncoder


@dataclass(frozen=True)
class _Frame:
    planes: tuple[np.ndarray, np.ndarray, np.ndarray]
    pixel_format: str
    width: int
    height: int
    frame_index: int = 0
    is_planar: bool = True


@pytest.mark.parametrize(
    ("tier", "pixel_format", "pixel_enum"),
    [("hq", "yuv422p16le", 0),
     ("hq", "yuv444p16le", 1),
     ("4444", "gbrp16le", 2)],
)
def test_sixteen_bit_movie_roundtrip(
    tmp_path: Path, tier: str, pixel_format: str, pixel_enum: int,
) -> None:
    if not any(path.is_file() for path in candidate_library_paths()):
        pytest.skip("TOPOS native 库未构建")
    width, height = 96, 64
    x = np.arange(width, dtype=np.uint16)[None, :]
    y = np.arange(height, dtype=np.uint16)[:, None]
    chroma_width = width // 2 if pixel_enum == 0 else width
    planes = (
        np.broadcast_to((32000 + x).astype("<u2"), (height, width)).copy(),
        np.broadcast_to((33000 + y).astype("<u2"),
                        (height, chroma_width)).copy(),
        np.full((height, chroma_width), 32768, dtype="<u2"),
    )
    output = tmp_path / "sixteen.mov"
    config = SimpleNamespace(
        width=width, height=height, fps=Fraction(25, 1),
        pix_fmt=pixel_format, profile=tier, crf=0, has_alpha=False,
        entropy_mode="rans2", aq_mode="off", rdo_mode="off", gop="intra",
    )
    encoder = ToposVideoEncoder(str(output), config, slice_threads=2)
    encoder.open()
    encoder.encode_frame(_Frame(planes, pixel_format, width, height))
    encoder.close()

    codec = ToposCodec()
    with ToposMovieFile(codec, str(output)) as movie, ToposDecoder(codec) as decoder:
        assert movie.info().sample_count == 1
        packet, size = movie.packet_into(0)
        info = decoder.prepare(packet, size)
        assert info.bit_depth == 16
        assert info.pixel_format == pixel_enum
        decoded = [np.empty_like(plane) for plane in planes]
        decoder.decode_views(packet, decoder.make_views(decoded), 3, size)
    for source, actual in zip(planes, decoded):
        assert np.max(np.abs(actual.astype(np.int32) - source.astype(np.int32))) <= 1


@pytest.mark.parametrize(
    ("tier", "pixel_format", "chroma_width"),
    [("hq", "yuv422p16le", 48),
     ("hq", "yuv444p16le", 96),
     ("4444", "gbrp16le", 96)],
)
def test_sixteen_bit_full_scale_rate_control(
    tmp_path: Path, tier: str, pixel_format: str, chroma_width: int,
) -> None:
    """产品码控在 0/65535 边界仍须可写可读且保持 16-bit 域。"""
    width, height = 96, 64
    first = np.tile(np.linspace(0, 65535, width, dtype="<u2"),
                    (height, 1)).copy()
    first[0, :4] = [0, 1, 65534, 65535]
    planes = (
        first,
        np.tile(np.linspace(0, 65535, chroma_width, dtype="<u2"),
                (height, 1)).copy(),
        np.full((height, chroma_width), 65535, dtype="<u2"),
    )
    config = SimpleNamespace(
        width=width, height=height, fps=Fraction(25, 1),
        pix_fmt=pixel_format, profile=tier, crf=None, has_alpha=False,
        entropy_mode="rans2", aq_mode="off", rdo_mode="off", gop="intra",
    )
    path = tmp_path / "fullscale.mov"
    encoder = ToposVideoEncoder(str(path), config, slice_threads=2)
    encoder.open()
    encoder.encode_frame(_Frame(planes, pixel_format, width, height))
    encoder.close()

    codec = ToposCodec()
    with ToposMovieFile(codec, str(path)) as movie, ToposDecoder(codec) as decoder:
        packet, size = movie.packet_into(0)
        info = decoder.prepare(packet, size)
        assert info.bit_depth == 16
        decoded = [np.empty_like(plane) for plane in planes]
        decoder.decode_views(packet, decoder.make_views(decoded), 3, size)
    assert int(decoded[0].min()) <= 64
    assert int(decoded[0].max()) >= 65470
    assert int(decoded[2].min()) >= 65470


def test_sixteen_bit_full_scale_low_qp_fails_closed(tmp_path: Path) -> None:
    width, height = 96, 64
    planes = (
        np.tile(np.linspace(0, 65535, width, dtype="<u2"),
                (height, 1)).copy(),
        np.full((height, width), 32768, dtype="<u2"),
        np.full((height, width), 65535, dtype="<u2"),
    )
    config = SimpleNamespace(
        width=width, height=height, fps=Fraction(25, 1),
        pix_fmt="gbrp16le", profile="4444", crf=0, has_alpha=False,
        entropy_mode="rans2", aq_mode="off", rdo_mode="off", gop="intra",
    )
    path = tmp_path / "out-of-token-domain.mov"
    encoder = ToposVideoEncoder(str(path), config, slice_threads=2)
    encoder.open()
    with pytest.raises(RuntimeError, match="token domain violated"):
        encoder.encode_frame(_Frame(planes, "gbrp16le", width, height))
    encoder.abort()
    assert not path.exists()
