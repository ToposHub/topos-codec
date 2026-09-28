#!/usr/bin/env python3
"""M-B2：生成 fuzz_mov 的音频种子（确定性，一次生成入库）。

产出（tests/fuzz/seed_corpus/）：
  seed_audio_lpcm16_48k.mov      lpcm 16bit 立体声（标准布局）
  seed_audio_lpcm24_44k.mov      lpcm 24bit 立体声 44.1k
  seed_audio_mp4a_48k.mov        mp4a AAC 立体声
  seed_audio_mp4a_48k_fs.mov     mp4a faststart 形态
  seed_audio_mp4a_primed.mov     mp4a + elst（priming=1024）
  seed_audio_multitrack.mov      v1.6 三轨混合（lpcm 主混音 + s16 mono
                                 "Dialogue" + f32 stereo "Music"，含轨名）
  seed_audio_tmcd.mov            v1.5 tmcd 时间码轨（30DF，与音轨共存）

用法：python gen_audio_seeds.py [--lib <dylib>]
"""
from __future__ import annotations

import argparse
import ctypes
import struct
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
sys.path.insert(0, str(ROOT))

W, H = 64, 48


def main() -> int:
    ap = argparse.ArgumentParser()
    here = HERE
    name = "topos_codec.dylib" if sys.platform == "darwin" else "topos_codec.so"
    default_lib = ""
    for bld in ("release", "debug"):
        cand = here.parents[1] / "build" / bld / name
        if cand.exists():
            default_lib = str(cand)
            break
    ap.add_argument("--lib", default=default_lib)
    args = ap.parse_args()

    import os
    if args.lib:
        os.environ["TOPOS_CODEC_LIB"] = args.lib
    from src.shared.codec import topos_binding
    from src.shared.codec.topos_binding import (
        TC_AUDIO_CODEC_LPCM,
        TC_AUDIO_CODEC_MP4A,
        ToposCodec,
        TOPOS_CODEC_ABI_VERSION,
    )

    codec = ToposCodec()
    fc = topos_binding._CFrameConfig()
    fc.struct_size = ctypes.sizeof(topos_binding._CFrameConfig)
    fc.abi_version = TOPOS_CODEC_ABI_VERSION
    fc.visible_width, fc.visible_height = W, H
    fc.profile, fc.pixel_format, fc.bit_depth = 3, 0, 10
    fc.qmatrix_id, fc.qp_base = 1, 24
    fc.color_range = fc.color_primaries = fc.color_transfer = fc.color_matrix = 1
    fc.sar_num = fc.sar_den = 1
    cw = (W + 1) // 2
    y = struct.pack(f"<{W * H}H", *[((x * 7 + r * 13) % 400 + 300)
                                    for r in range(H) for x in range(W)])
    u = struct.pack(f"<{cw * H}H", *[(480 + (x % 5) * 8)
                                     for r in range(H) for x in range(cw)])
    v = struct.pack(f"<{cw * H}H", *[(520 + (r % 4) * 6)
                                     for r in range(H) for x in range(cw)])
    vp = [codec.encode_frame(fc, [y, u, v])[0] for _ in range(8)]

    out_dir = HERE / "seed_corpus"
    out_dir.mkdir(exist_ok=True)

    def mux(path, codec_id, rate, channels, layout, bits, chunks, spc, asc,
            priming=0):
        cfg = codec.movie_config(W, H, timescale=24000, qp=24, qmatrix=1,
                                 audio_codec=codec_id, audio_sample_rate=rate,
                                 audio_channels=channels, audio_layout=layout,
                                 audio_bits_per_sample=bits)
        with topos_binding.ToposMuxFile(codec, str(path), cfg) as m:
            if asc:
                m.set_audio_asc(asc)
            for i in range(8):
                m.add_packet(vp[i], i * 1000, 1000)
                if i < len(chunks):
                    m.add_audio(chunks[i], spc)
            if priming:
                m.set_audio_priming(priming)
            m.finish()

    # lpcm 16bit 48k 立体声
    rate, ch = 48000, 2
    t = np.arange(1920 * 8) / rate
    # rint×满刻度（astype 直接截断会把 0.5·sin 全部清零——种子音频
    # 载荷曾因此全零，语料多样性受损）
    tone = np.rint(0.5 * 32767 * np.sin(2 * np.pi * 440 * t)).astype(np.int64)
    packed = np.repeat(tone, 2).astype(">i2").tobytes()  # 双声道同相
    frame_bytes = ch * 2
    chunks = [packed[i * 1920 * frame_bytes:(i + 1) * 1920 * frame_bytes]
              for i in range(8)]
    mux(out_dir / "seed_audio_lpcm16_48k.mov", TC_AUDIO_CODEC_LPCM, rate, ch,
        0, 16, chunks, 1920, b"")

    # lpcm 24bit 44.1k
    t2 = np.arange(1764 * 8) / 44100
    tone2 = np.rint(0.5 * 8388607 * np.sin(2 * np.pi * 440 * t2)).astype(np.int64)
    packed2 = np.repeat(tone2, 2).astype(">i4").view(np.uint8).reshape(-1, 4)[:, 1:]
    packed2 = np.ascontiguousarray(packed2).tobytes()
    chunks2 = [packed2[i * 1764 * 6:(i + 1) * 1764 * 6] for i in range(8)]
    mux(out_dir / "seed_audio_lpcm24_44k.mov", TC_AUDIO_CODEC_LPCM, 44100, ch,
        0, 24, chunks2, 1764, b"")

    # mp4a（ASC 立体声 48k：0x12 0x10）
    chunks3 = [bytes(((k * 13 + i) & 0xFF) for k in range(96)) for i in range(8)]
    mux(out_dir / "seed_audio_mp4a_48k.mov", TC_AUDIO_CODEC_MP4A, rate, ch,
        0, 0, chunks3, 1024, b"\x12\x10")
    mux(out_dir / "seed_audio_mp4a_primed.mov", TC_AUDIO_CODEC_MP4A, rate, ch,
        0, 0, chunks3, 1024, b"\x12\x10", priming=1024)

    # v1.6 多轨（三轨混合格式 + 轨名；M-B8 后 fuzz 种子补齐）
    from src.shared.codec.topos_binding import (
        TC_AUDIO_FMT_FLOAT32,
        TC_AUDIO_FMT_INT,
        TC_AUDIO_LAYOUT_MONO,
        TC_AUDIO_LAYOUT_STEREO,
        ToposAudioTrackConfig,
    )
    cfg_multi = codec.movie_config(W, H, timescale=24000, qp=24, qmatrix=1,
                                   audio_codec=TC_AUDIO_CODEC_LPCM,
                                   audio_sample_rate=rate,
                                   audio_channels=2,
                                   audio_layout=TC_AUDIO_LAYOUT_STEREO,
                                   audio_bits_per_sample=16)
    with topos_binding.ToposMuxFile(codec, str(out_dir / "seed_audio_multitrack.mov"),
                                    cfg_multi) as m:
        idx_dlg = m.add_audio_track(ToposAudioTrackConfig(
            codec=TC_AUDIO_CODEC_LPCM, sample_rate=rate, channel_count=1,
            channel_layout=TC_AUDIO_LAYOUT_MONO, bits_per_sample=16,
            sample_format=TC_AUDIO_FMT_INT, name="Dialogue"))
        idx_mus = m.add_audio_track(ToposAudioTrackConfig(
            codec=TC_AUDIO_CODEC_LPCM, sample_rate=rate, channel_count=2,
            channel_layout=TC_AUDIO_LAYOUT_STEREO, bits_per_sample=32,
            sample_format=TC_AUDIO_FMT_FLOAT32, name="Music"))
        assert (idx_dlg, idx_mus) == (1, 2), (idx_dlg, idx_mus)
        tone_mono = np.rint(0.4 * 32767
                            * np.sin(2 * np.pi * 660 * t)).astype(np.int64)
        mono_packed = tone_mono.astype(">i2").tobytes()
        mus_f32 = (0.3 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)
        mus_packed = np.repeat(mus_f32, 2).astype(">f4").tobytes()
        for i in range(8):
            m.add_packet(vp[i], i * 1000, 1000)
            if i < len(chunks):
                m.add_audio(chunks[i], 1920)
                m.add_audio_to(idx_dlg, mono_packed[i * 3840:(i + 1) * 3840], 1920)
                m.add_audio_to(idx_mus, mus_packed[i * 15360:(i + 1) * 15360], 1920)
        m.finish()

    # v1.5 tmcd（30DF 起始时码；与音轨共存形态）
    cfg_tc = codec.movie_config(W, H, timescale=24000, qp=24, qmatrix=1,
                                audio_codec=TC_AUDIO_CODEC_LPCM,
                                audio_sample_rate=rate,
                                audio_channels=2,
                                audio_layout=TC_AUDIO_LAYOUT_STEREO,
                                audio_bits_per_sample=16)
    with topos_binding.ToposMuxFile(codec, str(out_dir / "seed_audio_tmcd.mov"),
                                    cfg_tc) as m:
        for i in range(8):
            m.add_packet(vp[i], i * 1000, 1000)
            if i < len(chunks):
                m.add_audio(chunks[i], 1920)
        m.set_timecode(1, 2, 3, 12, 30, 1)
        m.finish()

    # faststart 变换
    codec.faststart_file(str(out_dir / "seed_audio_mp4a_48k.mov"),
                         str(out_dir / "seed_audio_mp4a_48k_fs.mov"))

    for f in sorted(out_dir.glob("seed_audio_*")):
        print(f"{f.name}: {f.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
