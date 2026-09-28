"""阶段 1：ctypes 绑定层与 C ABI 一致性测试（R-18 守卫）。

库未构建时整体 skip（构建方式见 native/README.md）。
"""
from __future__ import annotations

import ctypes

import pytest

from topos_codec import topos_binding
from topos_codec.topos_binding import (
    TC_ERR_INVALID_ARGUMENT,
    TC_ERR_MALFORMED,
    TC_OK,
    TOPOS_CODEC_ABI_VERSION,
    ToposCodec,
    ToposCodecError,
    _CCpuFeatures,
    _CCodecCapabilities,
    _CVersionInfo,
)


@pytest.fixture(scope="module")
def codec() -> ToposCodec:
    import os

    explicit = os.environ.get("TOPOS_CODEC_LIB")
    if explicit is None and not any(
        p.is_file() for p in topos_binding.candidate_library_paths()
    ):
        pytest.skip("libtopos_codec 未构建；先运行 bash native/run_tests.sh")
    return ToposCodec()


def test_abi_version(codec: ToposCodec) -> None:
    # v2（R3/ADR-C014 决策 C-116）：追加 alpha 预算 API；三方一致（lib=绑定=钉死值）
    assert codec.abi_version == TOPOS_CODEC_ABI_VERSION == 2


def test_encode_stage_stats_api(codec: ToposCodec) -> None:
    """E1：阶段画像 API 镜像 native 结构并可清零/读取。"""
    codec.encode_stage_stats_reset()
    stats = codec.encode_stage_stats_get()
    assert stats.pad_ns == 0
    assert stats.blocks == 0
    assert stats.check_wall_ns == 0


def test_version_fields(codec: ToposCodec) -> None:
    v = codec.version()
    assert (v.major, v.minor, v.patch) == (0, 1, 0)
    assert v.abi_version == TOPOS_CODEC_ABI_VERSION
    assert v.git_commit
    assert v.build_target
    # R-18 守卫：C 侧 sizeof 与 ctypes 镜像一致
    assert v.struct_size == ctypes.sizeof(_CVersionInfo)


def test_status_messages(codec: ToposCodec) -> None:
    known = [
        TC_OK,
        topos_binding.TC_WARN_CONCEALED,
        topos_binding.TC_ERR_OUT_OF_MEMORY,
        topos_binding.TC_ERR_LIMIT_EXCEEDED,
        topos_binding.TC_ERR_MALFORMED,
        topos_binding.TC_ERR_NOT_IMPLEMENTED,
    ]
    for code in known:
        assert codec.status_message(code)
    assert codec.status_message(9999) == "unknown status"
    assert codec.status_message(-9999) == "unknown status"
    assert codec.status_message(TC_OK) == "OK"


def test_cpu_features(codec: ToposCodec) -> None:
    cf = codec.cpu_features()
    assert cf.struct_size == ctypes.sizeof(_CCpuFeatures)
    assert cf.abi_version == TOPOS_CODEC_ABI_VERSION
    known_mask = (
        topos_binding.TOPOS_CPU_X86_AVX2
        | topos_binding.TOPOS_CPU_X86_AVX512F
        | topos_binding.TOPOS_CPU_ARM_NEON
        | topos_binding.TOPOS_CPU_X86_FMA
    )
    assert cf.flags & ~known_mask == 0


def test_codec_capabilities_are_truthful(codec: ToposCodec) -> None:
    """RD7-01: expose the public V7-A reader, but keep V7-B gated."""
    caps = codec.capabilities()
    assert caps.struct_size == ctypes.sizeof(_CCodecCapabilities)
    assert caps.abi_version == TOPOS_CODEC_ABI_VERSION
    assert caps.max_width == 16384
    assert caps.max_height == 16384
    assert caps.auto2k_max_dim == 2048
    assert caps.full_decode
    assert caps.fixed_reduced_decode
    assert caps.auto2k_decode
    assert caps.cpu_surface_output
    assert caps.host_visible_gpu_surface
    assert not caps.v7a_band_decode  # V 代际收纳 2026-09-13：V7-A 归档，位不再广播
    assert not caps.v7b_scalable_base


def test_decode_request_drop_alpha_flag_is_encoded(codec: ToposCodec) -> None:
    """DROP_ALPHA is a request-level optional flag, not an ABI layout change."""
    request = codec.make_decode_request(
        topos_binding.TC_DECODE_MODE_FULL,
        flags=topos_binding.TC_DECODE_FLAG_DROP_ALPHA,
    )
    assert request.flags == topos_binding.TC_DECODE_FLAG_DROP_ALPHA
    with pytest.raises(ToposCodecError):
        codec.make_decode_request(topos_binding.TC_DECODE_MODE_FULL, flags=2)


def test_null_argument_sets_last_error(codec: ToposCodec) -> None:
    status = int(codec._lib.tc_version(None))  # noqa: SLF001 — 绕过包装直测 NULL 路径
    assert status == TC_ERR_INVALID_ARGUMENT
    assert codec.last_error() != ""


def test_explicit_missing_path_raises() -> None:
    with pytest.raises(ToposCodecError):
        topos_binding.load_library("/nonexistent/libtopos_codec.dylib")


def test_windows_named_build_directory_is_discovered(
        tmp_path, monkeypatch: pytest.MonkeyPatch) -> None:
    build_dir = tmp_path / "native" / "build" / "win-release-zig"
    build_dir.mkdir(parents=True)
    monkeypatch.setattr(topos_binding, "_repo_root", lambda: tmp_path)
    monkeypatch.setattr(topos_binding.sys, "platform", "win32")

    candidates = topos_binding.candidate_library_paths()

    assert build_dir / "topos_codec.dll" in candidates


def test_default_load_skips_unloadable_candidate(
        tmp_path, monkeypatch: pytest.MonkeyPatch) -> None:
    bad = tmp_path / "bad.dll"
    good = tmp_path / "good.dll"
    bad.write_bytes(b"!<arch>\\n")
    good.write_bytes(b"MZ")
    monkeypatch.setattr(topos_binding, "candidate_library_paths", lambda: [bad, good])
    marker = object()

    def fake_cdll(path: str):
        if path == str(bad):
            raise OSError("not a Windows DLL")
        return marker

    monkeypatch.setattr(topos_binding.ctypes, "CDLL", fake_cdll)

    assert topos_binding.load_library() is marker


# —— 阶段 4：elementary frame 编解码 ——


def _make_config(w: int, h: int, **kw) -> topos_binding._CFrameConfig:
    c = topos_binding._CFrameConfig()
    c.struct_size = ctypes.sizeof(topos_binding._CFrameConfig)
    c.abi_version = TOPOS_CODEC_ABI_VERSION
    c.visible_width = w
    c.visible_height = h
    for k, v in kw.items():
        setattr(c, k, v)
    return c


def _synth_planes(w: int, h: int, with_alpha: bool) -> "tuple[list, list]":
    """简单梯度 + 棋盘内容（struct 打包为 uint16 LE bytes）。"""
    import struct

    cw = (w + 1) // 2
    y = struct.pack(
        f"<{w*h}H", *[((x * 7 + r * 13) % 400 + 300) for r in range(h) for x in range(w)]
    )
    u = struct.pack(f"<{cw*h}H", *[(480 + (x % 5) * 8) for r in range(h) for x in range(cw)])
    v = struct.pack(f"<{cw*h}H", *[(520 + (r % 4) * 6) for r in range(h) for x in range(cw)])
    planes = [y, u, v]
    raw = [list(struct.unpack(f"<{w*h}H", y)), None, None, None]
    if with_alpha:
        a = struct.pack(
            f"<{w*h}H", *[(65535 if x < w // 2 else ((x + r) % 4096)) for r in range(h) for x in range(w)]
        )
        planes.append(a)
        raw[3] = list(struct.unpack(f"<{w*h}H", a))
    return planes, raw


def test_config_validate_and_bound(codec: ToposCodec) -> None:
    c = _make_config(64, 48, qp_base=20, qmatrix_id=1)
    assert codec.config_validate(c) == TC_OK
    assert codec.packet_bound(c) > 53
    # v1.5 qp 域 0..95（ADR-C031）：64/95 合法，96 越界
    assert codec.config_validate(_make_config(64, 48, qp_base=64)) == TC_OK
    assert codec.config_validate(_make_config(64, 48, qp_base=95)) == TC_OK
    bad = _make_config(64, 48, qp_base=96)
    assert codec.config_validate(bad) == TC_ERR_INVALID_ARGUMENT


def test_profile_pixel_format_cross_validation(codec: ToposCodec) -> None:
    """R4.4 交叉规则钉死（ADR-C018 C-137；编码入口经帧头校验单一事实源）。

    Standard(3) 不限格式——422/444/GBR × 10/12 全合法（v1.2~v1.4 枚举
    扩展的交付载体，3+444 非矛盾组合）；Pro444(5) 须 pf≠0；Extreme(6)
    须 pf≠0 且 12-bit；违反 → TC_ERR_MALFORMED（声明自相矛盾）。
    """
    for pf, bd in [(0, 10), (0, 12), (1, 10), (1, 12), (2, 10), (2, 12)]:
        c = _make_config(64, 48, qp_base=20, profile=3, pixel_format=pf, bit_depth=bd)
        if pf == 2:
            c.color_matrix = 0  # GBR 契约（v1.4）
        assert codec.config_validate(c) == TC_OK
    assert codec.config_validate(_make_config(
        64, 48, qp_base=20, profile=5, pixel_format=0, bit_depth=10)) == TC_ERR_MALFORMED
    assert codec.config_validate(_make_config(
        64, 48, qp_base=20, profile=6, pixel_format=1, bit_depth=10)) == TC_ERR_MALFORMED
    assert codec.config_validate(_make_config(
        64, 48, qp_base=20, profile=5, pixel_format=1, bit_depth=12)) == TC_OK
    ok_gbr = _make_config(64, 48, qp_base=20, profile=6, pixel_format=2, bit_depth=12)
    ok_gbr.color_matrix = 0
    assert codec.config_validate(ok_gbr) == TC_OK


def test_symbol_hist_plane_breakdown(codec: ToposCodec) -> None:
    """C0 统计：按平面直方图之和必须回到全局 token 直方图。"""
    w, h = 64, 48
    planes, _ = _synth_planes(w, h, with_alpha=False)
    cfg = _make_config(w, h, qp_base=24, qmatrix_id=1)
    codec.symbol_hist_reset()
    codec.symbol_hist_enable(True)
    try:
        codec.encode_frame(cfg, planes)
    finally:
        codec.symbol_hist_enable(False)
    dc, run, lvl = codec.symbol_hist_get()
    per_plane = [codec.symbol_hist_plane_get(p) for p in range(3)]
    assert [sum(p[0][i] for p in per_plane) for i in range(29)] == dc
    assert [sum(p[1][i] for p in per_plane) for i in range(64)] == run
    assert [sum(p[2][i] for p in per_plane) for i in range(28)] == lvl
    assert codec.symbol_hist_plane_get(3) == ([0] * 29, [0] * 64, [0] * 28)


def test_frame_roundtrip_lossless_alpha(codec: ToposCodec) -> None:
    import struct

    w, h = 40, 24
    planes, raw = _synth_planes(w, h, with_alpha=True)
    cfg = _make_config(w, h, qp_base=24, qmatrix_id=1, alpha_mode=1, alpha_bit_depth=16)
    pkt, stats = codec.encode_frame(cfg, planes)
    assert stats.packet_size == len(pkt)
    assert (
        stats.color_payload_bytes + stats.alpha_payload_bytes
        + stats.color_header_bytes + stats.alpha_header_bytes
    ) == stats.packet_size
    assert stats.alpha_max_abs_error == 0

    # 确定性
    pkt2, _ = codec.encode_frame(cfg, planes)
    assert pkt2 == pkt

    frame = codec.decode(pkt)
    assert frame.info.plane_count == 4
    assert frame.info.concealed_slices == 0
    assert list(struct.unpack(f"<{w*h}H", frame.planes[3])) == raw[3]  # Alpha bit-exact


@pytest.mark.parametrize("retired_em", [2, 3, 4, 5, 6, 7])
def test_retired_entropy_write_rejected(codec: ToposCodec, retired_em: int) -> None:
    """V 代际收纳（2026-09-13）：退役 em 写路径拒绝钉死。

    em∈{2..7}（V2-Rice/V3/V4/V5/V6/V7-R）写端退役——绑定层 encode_frame
    显式 ToposCodecError（INVALID_ARGUMENT，信息含 ADR 指引），禁止静默
    回落；em 编号永久封存。原 C1(em=4)/C2(em=5) roundtrip 宿主随代际退役，
    本测试承接其配置域覆盖（语义反转为拒绝）。
    """
    w, h = 64, 48
    planes, _ = _synth_planes(w, h, with_alpha=False)
    cfg = _make_config(w, h, qp_base=24, qmatrix_id=1,
                       color_range=1, color_primaries=1,
                       color_transfer=1, color_matrix=1,
                       chroma_siting=0, sar_num=1, sar_den=1)
    cfg.reserved[0] = retired_em
    with pytest.raises(ToposCodecError, match="retired"):
        codec.encode_frame(cfg, planes)


def test_retired_generation_read_rejected(codec: ToposCodec) -> None:
    """V 代际收纳（2026-09-13）：退役代际文件读 = UNSUPPORTED_VERSION。

    用保留代际合法包 patch 代际字节（+帧头 CRC 重算）构造退役包 fixture
    （写端已拒绝，无法再编码产包）：patch 后的读必须干净拒绝且错误信息
    含 retired 指引，绝不 crash/误解码；还原字节后读恢复 OK（fixture
    方法自检）。生产构建零回放面；考古回放 = TOPOS_DEV_REPLAY 构建 +
    TOPOS_DEV=1（native frame_header.c 双重门）。
    """
    import zlib

    w, h = 32, 24
    planes, _ = _synth_planes(w, h, with_alpha=False)
    for sel, off, retired in ((0, 6, 3), (1, 45, 0), (8, 45, 6)):
        cfg = _make_config(w, h, qp_base=24, qmatrix_id=0, bit_depth=10)
        cfg.reserved[0] = sel
        pkt, _stats = codec.encode_frame(cfg, planes)
        original = pkt[off]
        patched = bytearray(pkt)
        patched[off] = retired
        # 帧头 CRC：覆盖前 49B，存于 49..52（大端）——zlib.crc32 同算法
        patched[49:53] = zlib.crc32(bytes(patched[:49])).to_bytes(4, "big")
        with pytest.raises(ToposCodecError) as exc:
            codec.decode(bytes(patched))
        assert "retired" in str(exc.value)
        assert exc.value.args and "ADR-C0xx" in str(exc.value)
        # 还原自检（fixture 方法不破坏合法读取）
        restored = bytearray(patched)
        restored[off] = original
        restored[49:53] = zlib.crc32(bytes(restored[:49])).to_bytes(4, "big")
        frame = codec.decode(bytes(restored))
        assert frame.info.concealed_slices == 0


def test_frame_concealment(codec: ToposCodec) -> None:
    w, h = 32, 16
    planes, _ = _synth_planes(w, h, with_alpha=False)
    cfg = _make_config(w, h, qp_base=30, qmatrix_id=1, slice_rows=1)
    pkt, _ = codec.encode_frame(cfg, planes)
    # 找到第一个 slice payload（53 + 17 之后）并破坏一个字节 → CRC 坏 → conceal
    bad = bytearray(pkt)
    assert len(bad) > 53 + 17 + 4
    bad[53 + 17 + 2] ^= 0x40
    frame = codec.decode(bytes(bad))
    assert frame.info.concealed_slices == 1
    assert topos_binding.TC_FRAME_SLICE_CONCEALED in frame.info.slice_status
    # header 破坏 → 整帧拒绝
    bad2 = bytearray(pkt)
    bad2[30] ^= 0x01
    with pytest.raises(ToposCodecError):
        codec.decode(bytes(bad2))


def test_frame_encode_sized(codec: ToposCodec) -> None:
    w, h = 64, 48
    planes, _ = _synth_planes(w, h, with_alpha=False)
    cfg = _make_config(w, h, qp_base=20, qmatrix_id=1)
    pkt0, stats0 = codec.encode_frame(cfg, planes)
    target = stats0.packet_size // 2
    pkt, stats, qp_used = codec.encode_sized(cfg, planes, target, 0, 63)
    assert qp_used >= 20
    assert stats.packet_size <= target
    frame = codec.decode(pkt)
    assert frame.info.plane_count == 3


def test_explicit_scalable_encode(codec: ToposCodec) -> None:
    """RD4/RD7：V7-B writer 可显式调用，但不改变默认 encode 路径。"""
    if not getattr(codec, "_has_scalable_encoder", False):
        pytest.skip("dylib 缺少 tc_frame_encode_scalable")
    w, h = 48, 24
    planes, _ = _synth_planes(w, h, with_alpha=False)
    cfg = _make_config(
        w, h, qp_base=24, qmatrix_id=1, color_range=1,
        color_primaries=1, color_transfer=1, color_matrix=1,
        chroma_siting=0, sar_num=1, sar_den=1,
    )
    pkt = codec.encode_scalable(cfg, planes, base_max_dim=16)
    assert pkt[6] == 7
    assert len(pkt) > 53
    preview = codec.decode_reduced(pkt, topos_binding.TC_DECODE_SCALE_THIRD)
    assert (preview.info.visible_width, preview.info.visible_height) == (16, 8)
    with pytest.raises(ToposCodecError):
        codec.encode_scalable(cfg, planes, base_max_dim=2049)


def test_movie_packet_base_reads_only_v7b_base(codec: ToposCodec, tmp_path) -> None:
    """RD4-04：MOV 预览读取返回 embedded base，而不是完整 V7-B sample。"""
    if not getattr(codec, "_has_scalable_encoder", False) or not getattr(
        codec, "_has_movie_packet_base", False
    ):
        pytest.skip("dylib 缺少 V7-B writer 或 tc_movie_packet_base")
    w, h = 48, 24
    planes, _ = _synth_planes(w, h, with_alpha=False)
    cfg = _make_config(
        w, h, qp_base=24, qmatrix_id=1, color_range=1,
        color_primaries=1, color_transfer=1, color_matrix=1,
        chroma_siting=0, sar_num=1, sar_den=1,
    )
    packet = codec.encode_scalable(cfg, planes, base_max_dim=16)
    path = str(tmp_path / "v7b_base.mov")
    with topos_binding.ToposMuxFile(
        codec, path, codec.movie_config(w, h, qp=24, qmatrix=1)
    ) as mux:
        mux.add_packet(packet, 0, 1)
        mux.finish()

    with topos_binding.ToposMovieFile(codec, path) as movie:
        base, base_size, is_base = movie.packet_base_into(0)
        assert is_base is True
        assert base_size < len(packet)
        base_frame = codec.decode(ctypes.string_at(base, base_size))
        assert (base_frame.info.visible_width, base_frame.info.visible_height) == (16, 8)
        arena, offsets, sizes, flags = movie.packets_base_into_batch(0, 1)
        assert int(offsets[0]) == 0
        assert int(sizes[0]) == base_size
        assert int(flags[0]) == 1
        assert ctypes.string_at(ctypes.addressof(arena), base_size) == \
            ctypes.string_at(base, base_size)


def test_decode_request_modes_and_context(codec: ToposCodec) -> None:
    """RD1：request API 的 stateless/context/batch 几何与输出路径一致。"""
    if not getattr(codec, "_has_decode_request", False):
        pytest.skip("dylib 缺少 tc_frame_decode_request")
    import numpy as np

    w, h = 64, 48
    planes, _ = _synth_planes(w, h, with_alpha=False)
    cfg = _make_config(w, h, qp_base=24, qmatrix_id=1)
    pkt, _ = codec.encode_frame(cfg, planes)

    auto_request = codec.make_decode_request(topos_binding.TC_DECODE_MODE_AUTO_2K)
    auto_frame = codec.decode_request(pkt, auto_request)
    assert (auto_frame.info.visible_width, auto_frame.info.visible_height) == (64, 48)

    request = codec.make_decode_request(
        topos_binding.TC_DECODE_MODE_REDUCED,
        scale=topos_binding.TC_DECODE_SCALE_HALF,
    )
    frame = codec.decode_request(pkt, request)
    assert (frame.info.visible_width, frame.info.visible_height) == (32, 24)
    assert len(frame.planes[0]) == 32 * 24 * 2

    scaled = codec.make_decode_request(
        topos_binding.TC_DECODE_MODE_SCALED,
        target_size=(40, 30),
    )
    scaled_frame = codec.decode_request(pkt, scaled)
    assert (scaled_frame.info.visible_width, scaled_frame.info.visible_height) == (40, 30)

    if not getattr(codec, "_has_decoder_request", False):
        pytest.skip("dylib 缺少 request-aware decoder context")
    with topos_binding.ToposDecoder(codec) as dec:
        info = dec.prepare_request(pkt, request)
        arrays = [
            np.zeros((codec.plane_geometry(info, p)[1],
                      codec.plane_geometry(info, p)[0]), dtype="<u2")
            for p in range(info.plane_count)
        ]
        got = dec.decode_views_request(pkt, dec.make_views(arrays), request)
        assert (got.visible_width, got.visible_height) == (32, 24)
        assert [a.tobytes() for a in arrays] == frame.planes

        batch_arrays = []
        batch_views = []
        for _ in range(2):
            current = [
                np.zeros((codec.plane_geometry(info, p)[1],
                          codec.plane_geometry(info, p)[0]), dtype="<u2")
                for p in range(info.plane_count)
            ]
            batch_arrays.append(current)
            batch_views.append(dec.make_views(current))
        if not getattr(codec, "_has_decoder_batch_request", False):
            pytest.skip("dylib 缺少 tc_decoder_decode_batch_request")
        batch_info = dec.decode_views_batch_request(
            [pkt, pkt], batch_views, request)
        assert [x.visible_width for x in batch_info] == [32, 32]
        assert [a.tobytes() for a in batch_arrays[0]] == frame.planes
        assert [a.tobytes() for a in batch_arrays[1]] == frame.planes


# ---------------- 阶段 5：MOV 容器（ToposMuxFile / ToposMovieFile / faststart） ----------------


def test_movie_roundtrip_file(codec: ToposCodec, tmp_path) -> None:
    import os

    w, h = 64, 48
    planes, _ = _synth_planes(w, h, with_alpha=False)
    fc = _make_config(w, h, qp_base=24, qmatrix_id=1,
                      color_range=1, color_primaries=1, color_transfer=1, color_matrix=1,
                      sar_num=1, sar_den=1)
    pkts = []
    pts_expect = []
    with topos_binding.ToposMuxFile(
        codec, str(tmp_path / "m.mov"), codec.movie_config(w, h, qp=24, qmatrix=1)
    ) as mux:
        for i in range(4):
            pkt, _ = codec.encode_frame(fc, planes)
            pkts.append(pkt)
            mux.add_packet(pkt, i, 1)
            pts_expect.append((i, 1))
        mux.finish()

    with topos_binding.ToposMovieFile(codec, str(tmp_path / "m.mov")) as mv:
        info = mv.info()
        assert (info.width, info.height) == (w, h)
        assert info.sample_count == 4
        assert info.faststart is False
        assert info.timescale == 24000
        assert info.index_bytes > 0
        for i, expect in enumerate(pkts):
            assert mv.packet(i) == expect
            assert mv.pts(i) == pts_expect[i]
            assert mv.is_sync(i) is True
        with pytest.raises(ToposCodecError):
            mv.packet(99)


def test_movie_faststart_file(codec: ToposCodec, tmp_path) -> None:
    w, h = 64, 48
    planes, _ = _synth_planes(w, h, with_alpha=False)
    fc = _make_config(w, h, qp_base=24, qmatrix_id=1,
                      color_range=1, color_primaries=1, color_transfer=1, color_matrix=1,
                      sar_num=1, sar_den=1)
    std = tmp_path / "std.mov"
    fs = tmp_path / "fs.mov"
    with topos_binding.ToposMuxFile(
        codec, str(std), codec.movie_config(w, h, qp=24, qmatrix=1)
    ) as mux:
        for i in range(3):
            pkt, _ = codec.encode_frame(fc, planes)
            mux.add_packet(pkt, i, 1)
        mux.finish()

    codec.faststart_file(str(std), str(fs))
    with topos_binding.ToposMovieFile(codec, str(fs)) as mv:
        info = mv.info()
        assert info.faststart is True
        assert info.sample_count == 3
        with topos_binding.ToposMovieFile(codec, str(std)) as mv0:
            assert mv0.info().faststart is False
            for i in range(3):
                assert mv.packet(i) == mv0.packet(i)


def test_movie_corrupt_rejected(codec: ToposCodec, tmp_path) -> None:
    import os

    w, h = 64, 48
    planes, _ = _synth_planes(w, h, with_alpha=False)
    fc = _make_config(w, h, qp_base=24, qmatrix_id=1,
                      color_range=1, color_primaries=1, color_transfer=1, color_matrix=1,
                      sar_num=1, sar_den=1)
    path = tmp_path / "m.mov"
    with topos_binding.ToposMuxFile(
        codec, str(path), codec.movie_config(w, h, qp=24, qmatrix=1)
    ) as mux:
        for i in range(3):
            pkt, _ = codec.encode_frame(fc, planes)
            mux.add_packet(pkt, i, 1)
        mux.finish()

    # 截半 → 结构性拒绝
    data = path.read_bytes()
    (tmp_path / "trunc.mov").write_bytes(data[: len(data) // 2])
    with pytest.raises(ToposCodecError):
        with topos_binding.ToposMovieFile(codec, str(tmp_path / "trunc.mov")):
            pass


def test_mux_alpha_budget_roundtrip(codec: ToposCodec, tmp_path) -> None:
    """R3：tpcB 预算元数据——set_alpha_budget 写入 → alpha_budget() 读回全字段。"""
    w, h = 40, 24
    planes, _ = _synth_planes(w, h, with_alpha=True)
    fc = _make_config(w, h, qp_base=24, qmatrix_id=1,
                      alpha_mode=2, alpha_bit_depth=12,
                      color_range=1, color_primaries=1, color_transfer=1,
                      color_matrix=1, sar_num=1, sar_den=1)
    path = str(tmp_path / "budget.mov")
    budget = {
        'target_ratio_bp': 2500,
        'actual_ratio_bp': 2417,
        'max_abs_error': 9,
        'flags': {'overrun': False, 'authorized': False, 'adapted': True},
        'frame_count': 2,
    }
    with topos_binding.ToposMuxFile(
        codec, path, codec.movie_config(w, h, qp=24, qmatrix=1,
                                        alpha_mode=2, alpha_bit_depth=12)
    ) as mux:
        for i in range(2):
            pkt, _ = codec.encode_frame(fc, planes)
            mux.add_packet(pkt, i, 1)
        mux.set_alpha_budget(budget)
        mux.finish()

    with topos_binding.ToposMovieFile(codec, path) as mv:
        bud = mv.alpha_budget()
        assert bud is not None
        assert bud['alpha_mode'] == 2
        assert bud['alpha_bit_depth'] == 12
        assert bud['target_ratio_bp'] == 2500
        assert bud['actual_ratio_bp'] == 2417
        assert bud['max_abs_error'] == 9
        assert bud['flags'] == {'overrun': False, 'authorized': False,
                                'adapted': True, 'ratio_saturated': False}
        assert bud['frame_count'] == 2

    # 未 set_alpha_budget 的文件 → None（旧文件兼容：无 tpcB 即无预算记录）
    plain = str(tmp_path / "plain.mov")
    with topos_binding.ToposMuxFile(
        codec, plain, codec.movie_config(w, h, qp=24, qmatrix=1)
    ) as mux:
        fc3 = _make_config(w, h, qp_base=24, qmatrix_id=1,
                           color_range=1, color_primaries=1, color_transfer=1,
                           color_matrix=1, sar_num=1, sar_den=1)
        pkt, _ = codec.encode_frame(fc3, _synth_planes(w, h, False)[0])
        mux.add_packet(pkt, 0, 1)
        mux.finish()
    with topos_binding.ToposMovieFile(codec, plain) as mv:
        assert mv.alpha_budget() is None


# —— 阶段4：跨帧批量解码（单批跨帧切片队列）——


def _batch_make_packets(codec: ToposCodec):
    """三帧不同几何/内容（含 alpha）编码 → [(pkt, planes, w, h, alpha), ...]。"""
    out = []
    for w, h, alpha, qp in ((64, 48, False, 22), (96, 64, True, 30), (32, 32, False, 10)):
        fc = _make_config(w, h, qp_base=qp, qmatrix_id=1)
        if alpha:
            fc.alpha_mode = 1
            fc.alpha_bit_depth = 16
        planes, _ = _synth_planes(w, h, alpha)
        pkt, _ = codec.encode_frame(fc, planes)
        out.append((pkt, planes, w, h, alpha))
    return out


def test_frame_decode_batch_roundtrip(codec: ToposCodec) -> None:
    if not getattr(codec, "_has_decode_batch", False):
        pytest.skip("dylib 缺少 tc_frame_decode_batch")
    items = _batch_make_packets(codec)
    pkts = [it[0] for it in items]

    # 逐帧参考（现有公共路径）；批量输出位不变契约
    singles = [codec.decode(p) for p in pkts]
    frames = codec.decode_batch(pkts)
    assert len(frames) == 3
    for i, f in enumerate(frames):
        ref = singles[i]
        assert f.info.visible_width == ref.info.visible_width
        assert f.info.plane_count == ref.info.plane_count
        assert f.info.concealed_slices == ref.info.concealed_slices == 0
        assert f.info.slice_count == ref.info.slice_count
        assert f.info.slice_status == ref.info.slice_status
        assert f.planes == ref.planes

    # count == 0 无操作；count == 1 等价单帧
    assert codec.decode_batch([]) == []
    one = codec.decode_batch(pkts[:1])
    assert one[0].planes == singles[0].planes


def test_frame_decode_batch_conceal_isolation(codec: ToposCodec) -> None:
    if not getattr(codec, "_has_decode_batch", False):
        pytest.skip("dylib 缺少 tc_frame_decode_batch")
    items = _batch_make_packets(codec)
    pkts = [it[0] for it in items]
    singles = [codec.decode(p) for p in pkts]

    # 中帧末尾 payload 翻一个字节 → CRC 坏 → 仅该帧 conceal，邻帧位不变
    bad = bytearray(pkts[1])
    bad[-10] ^= 0x40
    frames = codec.decode_batch([pkts[0], bytes(bad), pkts[2]])
    assert frames[0].info.concealed_slices == 0
    assert frames[1].info.concealed_slices >= 1
    assert frames[2].info.concealed_slices == 0
    assert frames[0].planes == singles[0].planes
    assert frames[2].planes == singles[2].planes

    # 结构解析失败（截断包）→ 整批拒绝
    with pytest.raises(ToposCodecError):
        codec.decode_batch([pkts[0], pkts[1][: len(pkts[1]) // 2]])


def test_decoder_decode_views_batch(codec: ToposCodec) -> None:
    if not getattr(codec, "_has_decoder_batch", False):
        pytest.skip("dylib 缺少 tc_decoder_decode_batch")
    import numpy as np

    items = _batch_make_packets(codec)
    pkts = [it[0] for it in items]
    singles = [codec.decode(p) for p in pkts]

    with topos_binding.ToposDecoder(codec) as dec:
        views_list = []
        arrays_list = []
        for pkt, *_ in items:
            info = dec.prepare(pkt)
            geoms = [codec.plane_geometry(info, p) for p in range(info.plane_count)]
            arrays = [np.zeros((gh, gw), dtype="<u2") for gw, gh in geoms]
            views_list.append(dec.make_views(arrays))
            arrays_list.append(arrays)
        infos = dec.decode_views_batch(pkts, views_list)
        for i, (info, arrays) in enumerate(zip(infos, arrays_list)):
            ref = singles[i]
            assert info.visible_width == ref.info.visible_width
            assert info.concealed_slices == ref.info.concealed_slices
            # 位一致：native 直写的 '<u2' 平面 vs decode() 输出 bytes
            for p in range(ref.info.plane_count):
                assert arrays[p].tobytes() == ref.planes[p]


def test_decoder_worker_budget_setter(codec: ToposCodec) -> None:
    """D3：context worker 预算可在帧之间原子更新且不改像素语义。"""
    if not getattr(codec, "_has_decoder_ctx", False):
        pytest.skip("dylib 缺少 decoder context")
    item = _batch_make_packets(codec)[0]
    with topos_binding.ToposDecoder(codec, max_slice_workers=1) as dec:
        assert dec.set_max_slice_workers(2) == 2
        assert dec.set_max_slice_workers(0) == 0
        info = dec.prepare(item[0])
        import numpy as np

        arrays = [
            np.zeros((codec.plane_geometry(info, p)[1],
                      codec.plane_geometry(info, p)[0]), dtype="<u2")
            for p in range(info.plane_count)
        ]
        got = dec.decode_views(item[0], dec.make_views(arrays), len(arrays))
        assert got.visible_width == item[2]
        assert [a.tobytes() for a in arrays] == [
            bytes(p) for p in codec.decode(item[0]).planes
        ]


def test_movie_packets_into_batch(codec: ToposCodec, tmp_path) -> None:
    """阶段4 兑现：批量聚读 arena——字节与逐包 packet() 逐位一致 + 紧凑布局。

    arena 槽位 (地址, 长度) 直接喂 decode_views_batch 的指针形态（零 bytes
    物化），输出与逐帧参考位一致。
    """
    import ctypes as ct

    if not getattr(codec, "_has_movie_packet_batch", False):
        pytest.skip("dylib 缺少 tc_movie_packet_batch")
    import numpy as np

    w, h = 64, 48
    planes, _ = _synth_planes(w, h, with_alpha=False)
    fc = _make_config(w, h, qp_base=24, qmatrix_id=1,
                      color_range=1, color_primaries=1, color_transfer=1, color_matrix=1,
                      sar_num=1, sar_den=1)
    path = str(tmp_path / "batch.mov")
    pkts = []
    with topos_binding.ToposMuxFile(
        codec, path, codec.movie_config(w, h, qp=24, qmatrix=1)
    ) as mux:
        for i in range(5):
            pkt, _ = codec.encode_frame(fc, planes)
            pkts.append(pkt)
            mux.add_packet(pkt, i, 1)
        mux.finish()

    with topos_binding.ToposMovieFile(codec, path) as mv:
        arena, offs, sizes = mv.packets_into_batch(0, 5)
        base = ct.addressof(arena)
        prefix = 0
        for i, p in enumerate(pkts):
            assert sizes[i] == len(p)
            assert offs[i] == prefix  # 紧凑布局（无空洞）
            assert ct.string_at(base + offs[i], sizes[i]) == p
            prefix += len(p)

        # 子区间 + arena 复用（grow-only：缩量批在旧缓冲上仍正确）
        arena2, offs2, sizes2 = mv.packets_into_batch(2, 3)
        b2 = ct.addressof(arena2)
        for k, i in enumerate((2, 3, 4)):
            assert ct.string_at(b2 + offs2[k], sizes2[k]) == pkts[i]

        # count==0 空批
        a0, o0, s0 = mv.packets_into_batch(0, 0)
        assert (a0, len(o0), len(s0)) == (None, 0, 0)
        # 区间越界 → 拒绝
        with pytest.raises(ToposCodecError):
            mv.packets_into_batch(4, 2)

        # 指针形态批量解码：arena 槽位直读 == 逐帧参考（零物化路径）
        refs = [codec.decode(p) for p in pkts]
        with topos_binding.ToposDecoder(codec) as dec:
            arena, offs, sizes = mv.packets_into_batch(0, 5)
            base = ct.addressof(arena)
            specs = [(base + int(offs[k]), int(sizes[k])) for k in range(5)]
            views_list, arrays_list = [], []
            for addr, sz in specs:
                info = dec.prepare(addr, sz)
                geoms = [codec.plane_geometry(info, p) for p in range(info.plane_count)]
                arrays = [np.zeros((gh, gw), dtype="<u2") for gw, gh in geoms]
                views_list.append(dec.make_views(arrays))
                arrays_list.append(arrays)
            infos = dec.decode_views_batch(specs, views_list)
            for i, (info, arrays) in enumerate(zip(infos, arrays_list)):
                assert info.concealed_slices == refs[i].info.concealed_slices
                for p in range(refs[i].info.plane_count):
                    assert arrays[p].tobytes() == refs[i].planes[p]


# —— 旧库/异库防御：符号漂移必须可诊断而非裸 dlsym 崩溃 ——


class _MissingSymbolLib:
    """模拟缺核心符号的旧 dylib：任何属性访问抛 dlsym 风格 AttributeError。"""

    def __getattr__(self, name: str):
        raise AttributeError(f"dlsym(0x0, {name}): symbol not found")


def test_stale_library_actionable_error() -> None:
    """核心符号缺失 → 绑定期转可操作 ToposCodecError（含重建指引）。"""
    with pytest.raises(ToposCodecError, match="重新构建"):
        ToposCodec(library=_MissingSymbolLib())  # type: ignore[arg-type]


def test_optional_symbol_silent_degradation(
    codec: ToposCodec, monkeypatch
) -> None:
    """可选符号缺失 → 直方图 API 静默降级（零值），不抛错。"""
    monkeypatch.setattr(codec, "_has_symbol_hist_plane", False)
    monkeypatch.setattr(codec, "_has_symbol_hist", False)
    assert codec.symbol_hist_plane_get(0) == ([0] * 29, [0] * 64, [0] * 28)
    assert codec.symbol_pair_hist_get(1) == [0] * (64 * 28)
    assert codec.symbol_hist_get() == ([0] * 29, [0] * 64, [0] * 28)
    codec.symbol_hist_enable(True)  # 无操作，不得抛错
    codec.symbol_hist_reset()


def test_degraded_features_warning(
    codec: ToposCodec, monkeypatch, caplog
) -> None:
    """可选符号缺失 → 加载期 warning 点名缺失符号（可见性守卫）。"""
    import logging

    monkeypatch.setattr(codec, "_has_cvt", False)
    with caplog.at_level(logging.WARNING, logger="topos_codec.topos_binding"):
        codec._warn_degraded_features()
    assert "tc_convert_packed_rgb" in caplog.text
    assert "降级" in caplog.text
