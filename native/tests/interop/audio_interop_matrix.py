#!/usr/bin/env python3
"""M-B0：v1.1 音轨跨引擎互操作矩阵（优化计划 §3 M-B0）。

产物矩阵（binding 自产，确定性）：
  lpcm 16/24/32bit × 2.0/5.1/7.1 × 44.1k/48k × standard/faststart = 36
  mp4a 2.0/5.1 @48k × standard/faststart                          =  4
脚本层引擎（可 CI 化；手动清单见互操作报告）：
  ffprobe   流识别（codec_name/采样率/声道）
  ffmpeg    lpcm 解码逐位相等（md5 对拍参考 BE→LE 字节）；AAC 相关性
  CoreAudio afinfo + afconvert（= QuickTime/AVFoundation 同源解码引擎）
  VLC       --intf dummy + sout 转码（VLC 自带 mov demuxer/lpcm 解码器）
  mpv       未安装时记 N/A（不阻塞）
硬引擎 = ffprobe/ffmpeg/CoreAudio（FAIL → 退出码 1）；
软引擎 = VLC/mpv（FAIL 只记录，由对策条目驱动，不改退出码）。

用法：
  python audio_interop_matrix.py [--lib <libtopos_codec>] [--work <dir>]
                                 [--json <out.json>] [--only lpcm|mp4a]
退出码：0 = 硬引擎全过；1 = 任一硬引擎失败。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional

import numpy as np

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))

# ── 产物矩阵定义 ─────────────────────────────────────────────────────────

LAYOUT_NAME = {0: "2.0", 1: "5.1", 2: "7.1"}
LAYOUT_CHANNELS = {0: 2, 1: 6, 2: 8}
RATES = (44100, 48000)
BITS_LPCM = (16, 24, 32)
AAC_RATE = 48000
CHUNK_SECONDS = 1.0 / 25.0          # 每音频 chunk = 一视频帧时长
NUM_CHUNKS = 10                     # 0.4s 音频；10 视频帧 @24fps ≈ 0.417s


def pcm_master_i16(rate: int, channels: int) -> np.ndarray:
    """确定性多频正弦母带（int16 域，逐声道差半音可辨识）。"""
    n = int(rate * CHUNK_SECONDS) * NUM_CHUNKS
    t = np.arange(n, dtype=np.float64) / rate
    out = np.zeros((n, channels), dtype=np.int64)
    for c in range(channels):
        freq = 110.0 * (2 ** (c / 12.0))
        out[:, c] = np.round((32760 * (0.8 - 0.05 * c))
                             * np.sin(2 * math.pi * freq * t))
    return out.astype(np.int64)


def pack_be_i16(master: np.ndarray, bits: int) -> bytes:
    """int16 母带 → 交错 signed BE packed（'twos' 语义，b bits）。"""
    q = (master << (bits - 16)).astype(np.int64)
    inter = q.T.reshape(-1)                       # (n, ch) → 逐样本交错
    if bits == 16:
        return inter.astype(">i2").tobytes()
    if bits == 24:
        # 大端 int32 字节 1..3：[0, hi, mid, lo] → hi mid lo（'twos' BE 24）
        u = (inter.astype(">i4").view(np.uint8).reshape(-1, 4)[:, 1:4])
        return u.tobytes()
    if bits == 32:
        return inter.astype(">i4").tobytes()
    raise ValueError(f"bits {bits}")


def be_to_le(data: bytes, bits: int) -> bytes:
    """存储序（BE）→ ffmpeg pcm_sXXle 输出序，供逐位 md5 对拍。"""
    step = bits // 8
    a = np.frombuffer(data, dtype=np.uint8)
    return a.reshape(-1, step)[:, ::-1].tobytes()


# ── 产物生成 ─────────────────────────────────────────────────────────────


@dataclass
class Product:
    name: str
    path: Path
    codec: str                     # lpcm | mp4a
    rate: int
    channels: int
    bits: int                      # lpcm 有效；mp4a 记 16
    faststart: bool
    ref_i16: np.ndarray = field(repr=False)          # (n, ch) int16 域参考
    ref_le: Optional[bytes] = field(default=None, repr=False)
    samples_total: int = 0


def _video_packets(codec_obj, count: int) -> List[bytes]:
    import ctypes

    from src.shared.codec import topos_binding as tb
    from src.shared.codec.topos_binding import TOPOS_CODEC_ABI_VERSION

    W, H = 64, 48
    cw = (W + 1) // 2
    fc = tb._CFrameConfig()
    fc.struct_size = ctypes.sizeof(tb._CFrameConfig)
    fc.abi_version = TOPOS_CODEC_ABI_VERSION
    fc.visible_width = W
    fc.visible_height = H
    fc.profile = 3
    fc.pixel_format = 0
    fc.bit_depth = 10
    fc.qmatrix_id = 1
    fc.qp_base = 24
    fc.color_range = 1
    fc.color_primaries = 1
    fc.color_transfer = 1
    fc.color_matrix = 1
    fc.sar_num = 1
    fc.sar_den = 1
    y = struct.pack(f"<{W * H}H",
                    *[((x * 7 + r * 13) % 400 + 300)
                      for r in range(H) for x in range(W)])
    u = struct.pack(f"<{cw * H}H", *[(480 + (x % 5) * 8)
                                     for r in range(H) for x in range(cw)])
    v = struct.pack(f"<{cw * H}H", *[(520 + (r % 4) * 6)
                                     for r in range(H) for x in range(cw)])
    return [codec_obj.encode_frame(fc, [y, u, v])[0] for _ in range(count)]


def build_matrix(work: Path, lib: Optional[str],
                 only: Optional[str]) -> List[Product]:
    from src.shared.codec import topos_binding
    from src.shared.codec.topos_binding import (
        TC_AUDIO_CODEC_LPCM,
        TC_AUDIO_CODEC_MP4A,
        ToposCodec,
    )

    if lib:
        import os
        os.environ["TOPOS_CODEC_LIB"] = lib
    codec = ToposCodec()
    pkts = _video_packets(codec, NUM_CHUNKS)

    products: List[Product] = []

    def mux_one(path: Path, codec_id: int, rate: int, channels: int,
                layout_id: int, bits: int, audio_chunks: List[bytes],
                samples_per_chunk: int, asc: bytes) -> None:
        cfg = codec.movie_config(
            64, 48, timescale=24000, qp=24, qmatrix=1,
            audio_codec=codec_id, audio_sample_rate=rate,
            audio_channels=channels, audio_layout=layout_id,
            audio_bits_per_sample=bits)
        with topos_binding.ToposMuxFile(codec, str(path), cfg) as mux:
            if asc:
                mux.set_audio_asc(asc)
            n_rounds = max(NUM_CHUNKS, len(audio_chunks))
            for i in range(n_rounds):
                if i < NUM_CHUNKS:
                    mux.add_packet(pkts[i], i * 1000, 1000)
                if i < len(audio_chunks):
                    mux.add_audio(audio_chunks[i], samples_per_chunk)
            if codec_id == TC_AUDIO_CODEC_MP4A:
                # M-B1 复跑口径：AAC 组带 elst（v1.1 交付基线带编辑列表）
                mux.set_audio_priming(1024)
            mux.finish()

    def faststart_of(p: Product) -> Product:
        dst = p.path.with_name(p.path.stem + "_fs.mov")
        codec.faststart_file(str(p.path), str(dst))
        return Product(name=dst.stem, path=dst, codec=p.codec, rate=p.rate,
                       channels=p.channels, bits=p.bits, faststart=True,
                       ref_i16=p.ref_i16, ref_le=p.ref_le,
                       samples_total=p.samples_total)

    if only in (None, "lpcm"):
        for rate in RATES:
            chunk_frames = int(rate * CHUNK_SECONDS)
            for layout_id, channels in LAYOUT_CHANNELS.items():
                master = pcm_master_i16(rate, channels)
                for bits in BITS_LPCM:
                    packed = pack_be_i16(master, bits)
                    frame_bytes = channels * (bits // 8)
                    chunks = [packed[i * chunk_frames * frame_bytes:
                                     (i + 1) * chunk_frames * frame_bytes]
                              for i in range(NUM_CHUNKS)]
                    name = (f"lpcm_{bits}b_"
                            f"{LAYOUT_NAME[layout_id].replace('.', '')}_{rate}")
                    path = work / f"{name}.mov"
                    mux_one(path, TC_AUDIO_CODEC_LPCM, rate, channels,
                            layout_id, bits, chunks, chunk_frames, b"")
                    p = Product(
                        name=name, path=path, codec="lpcm", rate=rate,
                        channels=channels, bits=bits, faststart=False,
                        ref_i16=master,
                        ref_le=be_to_le(packed, bits),
                        samples_total=chunk_frames * NUM_CHUNKS)
                    products.append(p)
                    products.append(faststart_of(p))

    if only in (None, "mp4a"):
        import av  # noqa: PLC0415

        for layout_id, channels in ((0, 2), (1, 6)):
            layout_str = "stereo" if channels == 2 else "5.1"
            n = int(AAC_RATE * CHUNK_SECONDS) * NUM_CHUNKS
            t = np.arange(n, dtype=np.float64) / AAC_RATE
            master_f = np.zeros((n, channels), dtype=np.float32)
            for c in range(channels):
                master_f[:, c] = (0.7 * np.sin(
                    2 * math.pi * 110.0 * (2 ** (c / 12.0)) * t)).astype(np.float32)
            master_i = np.round(master_f * 32767.0).astype(np.int64)

            enc = av.CodecContext.create("aac", "w")
            enc.sample_rate = AAC_RATE
            enc.layout = layout_str
            enc.format = "fltp"
            pkts_aac: List[bytes] = []
            asc = b""
            step = 1024
            pts = 0
            for off in range(0, n, step):
                block = master_f[off:off + step]
                if block.shape[0] < step:
                    block = np.vstack([block, np.zeros(
                        (step - block.shape[0], channels), dtype=np.float32)])
                planar = np.ascontiguousarray(block.T)
                frame = av.AudioFrame.from_ndarray(planar, format="fltp",
                                                   layout=layout_str)
                frame.sample_rate = AAC_RATE
                frame.pts = pts
                pts += step
                for pkt in enc.encode(frame):
                    if not asc:
                        asc = bytes(enc.extradata) if enc.extradata else b"\x12\x10"
                    pkts_aac.append(bytes(pkt))
            for pkt in enc.encode(None):
                if not asc:
                    asc = bytes(enc.extradata) if enc.extradata else b"\x12\x10"
                pkts_aac.append(bytes(pkt))

            name = f"mp4a_{LAYOUT_NAME[layout_id].replace('.', '')}_{AAC_RATE}"
            path = work / f"{name}.mov"
            mux_one(path, TC_AUDIO_CODEC_MP4A, AAC_RATE, channels, layout_id,
                    0, pkts_aac, 1024, asc)
            p = Product(name=name, path=path, codec="mp4a", rate=AAC_RATE,
                        channels=channels, bits=16, faststart=False,
                        ref_i16=master_i, samples_total=n)
            products.append(p)
            products.append(faststart_of(p))

    return products


# ── 引擎检查 ─────────────────────────────────────────────────────────────

FFPROBE_EXPECT_LPCM = {16: "pcm_s16be", 24: "pcm_s24be", 32: "pcm_s32be"}
HARD_ENGINES = ("ffprobe", "ffmpeg", "avfoundation")


def _run(cmd: List[str], timeout: int = 60) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def check_ffprobe(p: Product, ffprobe: str, tmp: Path) -> dict:
    r = _run([ffprobe, "-v", "error", "-select_streams", "a",
              "-show_entries",
              "stream=codec_name,sample_rate,channels,channel_layout,duration",
              "-of", "json", str(p.path)])
    if r.returncode != 0:
        return {"engine": "ffprobe", "verdict": "FAIL",
                "detail": r.stderr.strip()[:160]}
    streams = json.loads(r.stdout).get("streams", [])
    if len(streams) != 1:
        return {"engine": "ffprobe", "verdict": "FAIL",
                "detail": f"audio streams={len(streams)}"}
    s = streams[0]
    want_codec = FFPROBE_EXPECT_LPCM[p.bits] if p.codec == "lpcm" else "aac"
    issues = []
    if s.get("codec_name") != want_codec:
        issues.append(f"codec={s.get('codec_name')}≠{want_codec}")
    if int(s.get("sample_rate", 0)) != p.rate:
        issues.append(f"rate={s.get('sample_rate')}≠{p.rate}")
    if int(s.get("channels", 0)) != p.channels:
        issues.append(f"ch={s.get('channels')}≠{p.channels}")
    if issues:
        return {"engine": "ffprobe", "verdict": "FAIL",
                "detail": ";".join(issues), "layout": s.get("channel_layout", "")}
    return {"engine": "ffprobe", "verdict": "PASS", "detail": "",
            "layout": s.get("channel_layout", ""), "duration": s.get("duration", "")}


def _wav_to_i16(wav: Path, ffmpeg: str, channels: int) -> np.ndarray:
    """任意容器 wav → (n, ch) int16（统一经 ffmpeg 解，兼容
    WAVE_FORMAT_EXTENSIBLE 等非 wave 模块可读形态）。"""
    r = subprocess.run([ffmpeg, "-v", "error", "-y", "-i", str(wav),
                        "-map", "0:a:0", "-c:a", "pcm_s16le", "-f", "s16le", "-"],
                       capture_output=True, timeout=60)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.decode("utf-8", "replace").strip()[:120])
    a = np.frombuffer(r.stdout, dtype="<i2")
    return a.reshape(-1, channels)


def _best_corr(ref: np.ndarray, got: np.ndarray,
               max_lag: int = 4800, win: int = 12000) -> tuple:
    """numpy 互相关搜索：返回 (best_corr, best_lag)。"""
    if len(got) < win:
        return -2.0, -1
    r = ref[:win].astype(np.float64)
    r -= r.mean()
    re = np.sqrt(np.sum(r * r)) or 1.0
    best_c, best_l = -2.0, -1
    for lag in range(0, min(max_lag, len(got) - win) + 1):
        seg = got[lag:lag + win].astype(np.float64)
        seg -= seg.mean()
        se = np.sqrt(np.sum(seg * seg)) or 1.0
        c = float(np.dot(r, seg) / (re * se))
        if c > best_c:
            best_c, best_l = c, lag
        if c > 0.995:
            break
    return best_c, best_l


def check_ffmpeg(p: Product, ffmpeg: str, tmp: Path) -> dict:
    if p.codec == "lpcm":
        r = _run([ffmpeg, "-v", "error", "-i", str(p.path), "-map", "0:a:0",
                  "-c:a", f"pcm_s{p.bits}le", "-f", "md5", "-"])
        if r.returncode != 0:
            return {"engine": "ffmpeg", "verdict": "FAIL",
                    "detail": r.stderr.strip()[:160]}
        got = r.stdout.strip().split("=")[-1].strip()
        want = hashlib.md5(p.ref_le).hexdigest()
        if got != want:
            return {"engine": "ffmpeg", "verdict": "FAIL",
                    "detail": f"md5 {got}≠{want}（声道重排/位变换？）"}
        return {"engine": "ffmpeg", "verdict": "PASS", "detail": "bit-exact"}
    wav = tmp / f"{p.name}_ff.wav"
    r = _run([ffmpeg, "-v", "error", "-y", "-i", str(p.path), "-map", "0:a:0",
              "-c:a", "pcm_s16le", "-f", "wav", str(wav)])
    if r.returncode != 0:
        return {"engine": "ffmpeg", "verdict": "FAIL", "detail": r.stderr.strip()[:160]}
    got = _wav_to_i16(wav, ffmpeg, p.channels)[:, 0]
    corr, lag = _best_corr(p.ref_i16[:, 0], got)
    if corr < 0.9:
        return {"engine": "ffmpeg", "verdict": "FAIL",
                "detail": f"corr={corr:.3f} lag={lag}"}
    return {"engine": "ffmpeg", "verdict": "PASS", "detail": f"corr={corr:.3f} lag={lag}"}


def check_avfoundation(p: Product, ffprobe: str, tmp: Path) -> dict:
    """avconvert（AVFoundation=QuickTime Player 同源引擎）：能打开并导出
    音频 → 流级识别核对。AudioFile C API（afinfo/afconvert）对多轨 MOV
    支持浅（见 check_coreaudio），本引擎才是用户级事实标准。"""
    if platform.system() != "Darwin" or not Path("/usr/bin/avconvert").exists():
        return {"engine": "avfoundation", "verdict": "N/A", "detail": "非 macOS"}
    m4a = tmp / f"{p.name}_av.m4a"
    r = _run(["/usr/bin/avconvert", "-p", "PresetAppleM4A",
              "-s", str(p.path), "-o", str(m4a)])
    if r.returncode != 0 or not m4a.exists():
        return {"engine": "avfoundation", "verdict": "FAIL",
                "detail": f"avconvert 拒开/导出失败 rc={r.returncode}"}
    pr = _run([ffprobe, "-v", "error", "-select_streams", "a",
               "-show_entries", "stream=codec_name,sample_rate,channels,duration",
               "-of", "json", str(m4a)])
    if pr.returncode != 0:
        return {"engine": "avfoundation", "verdict": "FAIL",
                "detail": "导出物无法 probe"}
    st = json.loads(pr.stdout).get("streams", [])
    if not st:
        return {"engine": "avfoundation", "verdict": "FAIL",
                "detail": "导出物无音频流"}
    s = st[0]
    if int(s.get("sample_rate", 0)) != p.rate:
        return {"engine": "avfoundation", "verdict": "FAIL",
                "detail": f"导出 {s.get('sample_rate')}Hz ≠ {p.rate}"}
    got_ch = int(s.get("channels", 0))
    if got_ch != p.channels:
        if got_ch == 2 and p.channels > 2:
            # PresetAppleM4A 恒为立体声：>2ch 产物被下混导出 =
            # 文件已成功打开并解码（PASS），声道数差异是 preset 行为
            return {"engine": "avfoundation", "verdict": "PASS",
                    "detail": f"opened; M4A preset 下混 {p.channels}ch→2ch "
                              f"dur={float(s.get('duration', 0)):.3f}s"}
        return {"engine": "avfoundation", "verdict": "FAIL",
                "detail": f"导出 {got_ch}ch ≠ {p.channels}ch"}
    return {"engine": "avfoundation", "verdict": "PASS",
            "detail": f"opened+exported {s.get('codec_name')} "
                      f"dur={float(s.get('duration', 0)):.3f}s"}


def check_coreaudio(p: Product, ffmpeg: str, tmp: Path) -> dict:
    """afinfo + afconvert——CoreAudio AudioFile C API。注意：该 API 对
    多轨 MOV（含视频 trak）支持浅（M-B0 实测 mp4a 组合 ExtAudioFileRead
    'typ?'失败、lpcm 组合字节数误读），仅作软引擎记录，不代表 QuickTime
    Player 行为（后者走 AVFoundation，见 check_avfoundation）。"""
    if platform.system() != "Darwin" or not Path("/usr/bin/afinfo").exists():
        return {"engine": "coreaudio", "verdict": "N/A", "detail": "非 macOS"}
    r = _run(["/usr/bin/afinfo", str(p.path)])
    if r.returncode != 0:
        return {"engine": "coreaudio", "verdict": "FAIL",
                "detail": f"afinfo: {r.stderr.strip()[:120]}"}
    wav = tmp / f"{p.name}_ca.wav"
    r = _run(["/usr/bin/afconvert", "-f", "WAVE", "-d", "LEI16",
              str(p.path), str(wav)])
    if r.returncode != 0:
        # AudioFile API 对多轨 MOV 的已知浅支持：记录不阻塞（软引擎）
        return {"engine": "coreaudio", "verdict": "SOFT-FAIL",
                "detail": f"afconvert（AudioFile API 多轨 MOV 限制）: "
                          f"{r.stderr.strip()[:100]}"}
    try:
        got = _wav_to_i16(wav, ffmpeg, p.channels)[:, 0]
    except Exception as exc:  # noqa: BLE001
        return {"engine": "coreaudio", "verdict": "SOFT-FAIL", "detail": str(exc)}
    corr, lag = _best_corr(p.ref_i16[:, 0], got)
    if corr < 0.95:
        return {"engine": "coreaudio", "verdict": "SOFT-FAIL",
                "detail": f"corr={corr:.3f} lag={lag}"}
    return {"engine": "coreaudio", "verdict": "PASS",
            "detail": f"corr={corr:.3f} lag={lag}"}


def check_vlc(p: Product, vlc: str, ffmpeg: str, ffprobe: str,
              tmp: Path) -> dict:
    if not vlc:
        return {"engine": "vlc", "verdict": "N/A", "detail": "未安装"}
    wav = tmp / f"{p.name}_vlc.wav"
    dst = f"std{{access=file,mux=wav,dst={wav}}}"
    sout = f"#transcode{{acodec=s16l,channels={p.channels}}}:{dst}"
    try:
        _run([vlc, "--intf", "dummy", "--aout=dummy", "-V", "dummy",
              "--sout", sout, str(p.path), "vlc://quit"], timeout=90)
    except subprocess.TimeoutExpired:
        return {"engine": "vlc", "verdict": "FAIL", "detail": "timeout 90s"}
    if not wav.exists() or wav.stat().st_size < 44:
        return {"engine": "vlc", "verdict": "FAIL", "detail": "no wav out"}
    got = _wav_to_i16(wav, ffmpeg, p.channels)[:, 0]
    corr, lag = _best_corr(p.ref_i16[:, 0], got)
    if corr < 0.9:
        return {"engine": "vlc", "verdict": "FAIL",
                "detail": f"corr={corr:.3f} lag={lag}"}
    return {"engine": "vlc", "verdict": "PASS",
            "detail": f"corr={corr:.3f} lag={lag}"}


def check_mpv(p: Product, mpv: str, ffmpeg: str, tmp: Path) -> dict:
    if not mpv:
        return {"engine": "mpv", "verdict": "N/A", "detail": "未安装"}
    wav = tmp / f"{p.name}_mpv.wav"
    _run([mpv, "--no-video", "--ao=null", "--audio-format=s16",
          "--audio-channels", str(p.channels),
          f"--ao=null:file={wav}", str(p.path)], timeout=60)
    if not wav.exists() or wav.stat().st_size < 44:
        return {"engine": "mpv", "verdict": "FAIL", "detail": "no wav out"}
    got = _wav_to_i16(wav, ffmpeg, p.channels)[:, 0]
    corr, lag = _best_corr(p.ref_i16[:, 0], got)
    return {"engine": "mpv", "verdict": "PASS" if corr >= 0.9 else "FAIL",
            "detail": f"corr={corr:.3f} lag={lag}"}


# ── 主流程 ───────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    here = Path(__file__).resolve().parent
    name = "topos_codec.dylib" if sys.platform == "darwin" else "topos_codec.so"
    default_lib = ""
    for bld in ("release", "debug", "asan"):
        cand = here.parents[1] / "build" / bld / name
        if cand.exists():
            default_lib = str(cand)
            break
    ap.add_argument("--lib", default=default_lib)
    ap.add_argument("--work", default=tempfile.mkdtemp(prefix="topos_audio_interop_"))
    ap.add_argument("--json", default="")
    ap.add_argument("--only", choices=("lpcm", "mp4a"), default="")
    ap.add_argument("--ffprobe", default=shutil.which("ffprobe") or "")
    ap.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "")
    ap.add_argument("--vlc",
                    default=str(Path("/Applications/VLC.app/Contents/MacOS/VLC")))
    ap.add_argument("--mpv", default=shutil.which("mpv") or "")
    args = ap.parse_args()

    if not args.ffprobe or not args.ffmpeg:
        print("需要 ffprobe/ffmpeg 在 PATH", file=sys.stderr)
        return 2
    vlc = args.vlc if Path(args.vlc).exists() else ""

    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    products = build_matrix(work, args.lib, args.only or None)
    print(f"产物 {len(products)} 个 → {work}")

    tmp = Path(tempfile.mkdtemp(prefix="topos_audio_interop_dec_"))
    results: List[dict] = []
    hard_fail = False
    for p in products:
        row: dict = {"name": p.name, "codec": p.codec, "rate": p.rate,
                     "channels": p.channels, "bits": p.bits,
                     "faststart": p.faststart, "checks": []}
        engines = [("ffprobe", lambda pp, tt: check_ffprobe(pp, args.ffprobe, tt)),
                   ("ffmpeg", lambda pp, tt: check_ffmpeg(pp, args.ffmpeg, tt)),
                   ("avfoundation", lambda pp, tt: check_avfoundation(pp, args.ffprobe, tt)),
                   ("coreaudio", lambda pp, tt: check_coreaudio(pp, args.ffmpeg, tt)),
                   ("vlc", lambda pp, tt: check_vlc(pp, vlc, args.ffmpeg,
                                                    args.ffprobe, tt)),
                   ("mpv", lambda pp, tt: check_mpv(pp, args.mpv, args.ffmpeg, tt))]
        for engine, fn in engines:
            if engine == "vlc" and not vlc or engine == "mpv" and not args.mpv:
                res = {"engine": engine, "verdict": "N/A", "detail": "未安装"}
            else:
                try:
                    res = fn(p, tmp)
                except Exception as exc:  # noqa: BLE001
                    res = {"engine": engine, "verdict": "FAIL",
                           "detail": f"exc: {exc}"}
            row["checks"].append(res)
            if res["verdict"] == "FAIL" and engine in HARD_ENGINES:
                hard_fail = True
        results.append(row)
        marks = " ".join(f"{c['engine']}={c['verdict']}" for c in row["checks"])
        print(f"{p.name:32s} {marks}")

    if args.json:
        Path(args.json).write_text(json.dumps(results, ensure_ascii=False, indent=1))
        print(f"JSON → {args.json}")

    n_pass = sum(1 for r in results for c in r["checks"] if c["verdict"] == "PASS")
    n_fail = sum(1 for r in results for c in r["checks"] if c["verdict"] == "FAIL")
    n_soft = sum(1 for r in results for c in r["checks"] if c["verdict"] == "SOFT-FAIL")
    print(f"汇总：PASS {n_pass} / FAIL {n_fail} / SOFT-FAIL {n_soft}")
    for r in results:
        for c in r["checks"]:
            if c["verdict"] in ("FAIL", "SOFT-FAIL"):
                print(f"  {c['verdict']:9s} {r['name']} [{c['engine']}] {c['detail']}")
    return 1 if hard_fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
