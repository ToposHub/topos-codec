#!/usr/bin/env python3
"""Topos vs DNxHR 编码/解码速度 + 码率 + 画质对比（2026-09-11）。

DNxHR 梯子（Avid，固定码率按档位×分辨率定死）：LB/SQ/HQ（8-bit
4:2:2，ffmpeg 编码器仅收 yuv422p，10-bit 源经 format 滤镜降位——
规格限制计入其画质）、HQX（10-bit 4:2:2）、444（10-bit 4:4:4；
12-bit 输入被编码器内部转 10-bit）。

每档：timed encode ×2 取优（60 帧）、timed decode（-f null ×2 取优）、
解码回源域算合成 PSNR（422 权重 0.5/0.25/0.25，444 均值；峰 1023）。
Topos 同场同法：产品码控路径（C043 定标）编码 fps + PSNR + 解码
p50（3 轮），与 C043 矩阵互验。
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]
TOOLS = Path(__file__).resolve().parent
for p in (str(REPO), str(TOOLS)):
    if p not in sys.path:
        sys.path.insert(0, p)

import bench_tier_speed as bts  # noqa: E402
from bench_tier_decode import decode_stats  # noqa: E402
from src.shared.codec.topos_binding import ToposCodec  # noqa: E402
from src.shared.export.topos_rate_control import ToposRateFeedback  # noqa: E402
from src.shared.codec.topos_profiles import TOPOS_PROFILE_TIERS  # noqa: E402

FPS = 25.0
N_FRAMES = 60

# (名称, ffmpeg profile, 输入格式滤镜（None=原生）)
DNX_422 = (("lb", "dnxhr_lb", "yuv422p"), ("sq", "dnxhr_sq", "yuv422p"),
           ("hq", "dnxhr_hq", "yuv422p"), ("hqx", "dnxhr_hqx", None))
DNX_444 = (("444", "dnxhr_444", None),)


def timed(fn) -> float:
    best = None
    for _ in range(2):
        t0 = time.perf_counter()
        fn()
        w = time.perf_counter() - t0
        best = w if best is None else min(best, w)
    return best


def dnx_row(name: str, profile: str, vf_fmt, src: Path, src_fmt: str,
            w: int, h: int, tmp: Path) -> dict:
    mov = tmp / f"dnx_{name}_{w}.mov"
    dec = tmp / f"dnx_{name}_{w}.dec.raw"

    def enc():
        cmd = ["ffmpeg", "-y", "-v", "error", "-f", "rawvideo",
               "-pixel_format", src_fmt, "-video_size", f"{w}x{h}",
               "-framerate", "25", "-i", str(src), "-frames:v", str(N_FRAMES)]
        if vf_fmt:
            cmd += ["-vf", f"format={vf_fmt}"]
        cmd += ["-c:v", "dnxhd", "-profile:v", profile, str(mov)]
        subprocess.run(cmd, check=True)

    enc_wall = timed(enc)
    size = mov.stat().st_size

    def decnull():
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(mov),
                        "-f", "null", "-"], check=True)

    dec_wall = timed(decnull)

    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", str(mov),
                    "-f", "rawvideo", "-pix_fmt", src_fmt, str(dec)],
                   check=True)
    pf = 0 if "422" in src_fmt else 1
    dims = bts.plane_count_bytes(pf, w, h)
    srcm = np.memmap(src, dtype="<u2", mode="r")
    per_px = sum(a * b for a, b in dims)
    px_plane = [a * b for a, b in dims]
    mse = [0.0, 0.0, 0.0]
    with open(dec, "rb") as f:
        for i in range(N_FRAMES):
            b = f.read(per_px * 2)
            c = 0
            off = i * per_px
            for k, sz in enumerate(px_plane):
                a = np.frombuffer(b[c * 2:(c + sz) * 2], dtype="<u2").astype(np.float64)
                s = srcm[off + c:off + c + sz].astype(np.float64)
                mse[k] += float(np.mean((a - s) ** 2))
                c += sz

    def p(x):
        return 999.0 if x <= 0 else 10 * np.log10(1023.0 ** 2 / x)

    wts = (0.5, 0.25, 0.25) if pf == 0 else (1 / 3, 1 / 3, 1 / 3)
    comb = p(sum(w * m / N_FRAMES for w, m in zip(wts, mse)))
    bpp = size * 8.0 / (w * h * N_FRAMES)
    return dict(name=f"dnxhr_{name}", vendor="dnxhr", profile=profile,
                mbps=size * 8.0 / (N_FRAMES / FPS) / 1e6, bpp=bpp,
                enc_fps=N_FRAMES / enc_wall, dec_fps=N_FRAMES / dec_wall,
                psnr=comb)


def topos_row(codec: ToposCodec, tier_id: str, src: Path, src_fmt: str,
              w: int, h: int, tmp: Path) -> dict:
    bd = 12 if src_fmt.endswith("12le") else 10
    t = TOPOS_PROFILE_TIERS[tier_id]
    pf = 0 if tier_id in ("proxy", "lt", "standard", "hq") else 1
    dims = bts.plane_count_bytes(pf, w, h)
    px_plane = [a * b for a, b in dims]
    per_px = sum(px_plane)
    target = max(1, round(t.target_bpp * w * h / 8.0))
    fc = bts.make_fc(w, h, t.native_profile, pf, bd, t.qmatrix_id,
                     t.chroma_qp_offset)
    srcm = np.memmap(src, dtype="<u2", mode="r")

    def frame_bytes(i):
        off = i * per_px
        out, c = [], 0
        for sz in px_plane:
            out.append(np.ascontiguousarray(
                srcm[off + c:off + c + sz], dtype="<u2").tobytes())
            c += sz
        return out

    fc.qp_base = 40
    codec.encode_frame(fc, frame_bytes(0))            # 预热
    rc = ToposRateFeedback(target, qp_max=95)
    packets, total, enc_secs = [], 0, 0.0
    # 第一遍：纯编码计时（不夹解码/PSNR 工作，与 C043 矩阵同口径）
    for i in range(N_FRAMES):
        fr = frame_bytes(i)
        t0 = time.perf_counter()
        if i == 0:
            pkt, st, qp = codec.encode_sized(fc, fr, target, 0, 95)
            rc.seed(qp, len(pkt))
        else:
            fc.qp_base = rc.qp
            pkt, st = codec.encode_frame(fc, fr)
            rc.note(len(pkt))
        enc_secs += time.perf_counter() - t0
        total += len(pkt)
        packets.append(bytes(pkt))
    # 第二遍：PSNR（不计入编码时间）
    mse = [0.0, 0.0, 0.0]
    for i in range(N_FRAMES):
        rec = codec.decode(packets[i])
        off = i * per_px
        c = 0
        for k, sz in enumerate(px_plane):
            a = np.frombuffer(rec.planes[k], dtype="<u2", count=sz).astype(np.float64)
            s = srcm[off + c:off + c + sz].astype(np.float64)
            mse[k] += float(np.mean((a - s) ** 2))
            c += sz

    def p(x):
        return 999.0 if x <= 0 else 10 * np.log10(1023.0 ** 2 / x)

    wts = (0.5, 0.25, 0.25) if pf == 0 else (1 / 3, 1 / 3, 1 / 3)
    comb = p(sum(w * m / N_FRAMES for w, m in zip(wts, mse)))
    ds = decode_stats(codec, packets)
    bpp = total * 8.0 / (w * h * N_FRAMES)
    return dict(name=f"topos_{tier_id}", vendor="topos", tier=tier_id,
                mbps=total * 8.0 / (N_FRAMES / FPS) / 1e6, bpp=bpp,
                enc_fps=N_FRAMES / enc_secs,
                dec_fps=ds["p50_fps"], psnr=comb)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--family", required=True, choices=("422", "444"))
    ap.add_argument("--src-fmt", required=True)
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--tiers", required=True, help="逗号分隔 topos tier")
    ap.add_argument("--label", required=True)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()

    W, H = args.width, args.height
    src = Path(args.source)
    tmp = Path(tempfile.mkdtemp(prefix="dnx_"))
    codec = ToposCodec()
    dnx_set = DNX_422 if args.family == "422" else DNX_444
    rows = []
    print(f"[dnx] {args.label}/{args.family} {args.src_fmt} {W}x{H} × {N_FRAMES} 帧")
    for name, prof, vf in dnx_set:
        r = dnx_row(name, prof, vf, src, args.src_fmt, W, H, tmp)
        rows.append(r)
        print(f"[dnx] {r['name']}: {r['mbps']:.1f}Mbps ({r['bpp']:.3f}bpp) "
              f"{r['psnr']:.2f}dB | 编码 {r['enc_fps']:.1f}fps | "
              f"解码 {r['dec_fps']:.1f}fps")
    for tier in args.tiers.split(","):
        r = topos_row(codec, tier, src, args.src_fmt, W, H, tmp)
        rows.append(r)
        print(f"[dnx] {r['name']}: {r['mbps']:.1f}Mbps ({r['bpp']:.3f}bpp) "
              f"{r['psnr']:.2f}dB | 编码 {r['enc_fps']:.1f}fps | "
              f"解码 p50 {r['dec_fps']:.1f}fps")
    args.output.write_text(json.dumps(
        dict(label=args.label, family=args.family, src_fmt=args.src_fmt,
             width=W, height=H, frames=N_FRAMES, rows=rows), indent=1))
    print(f"[dnx] written {args.output}")


if __name__ == "__main__":
    main()
