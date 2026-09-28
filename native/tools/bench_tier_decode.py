#!/usr/bin/env python3
"""Topos 产品档位解码速率矩阵（2K/4K × 六档，2026-09-11 口径）。

与编码矩阵（bench_tier_speed.py）同源同格：真实素材、产品码控路径出包
（首帧 sized + 反馈单遍、rans2、flat、444 含 ADR-C042 定标），然后逐帧
`tc_frame_decode` 计时（真实输出平面；p50/p95，每格 5 轮 × 60 帧）：

  - 1t 与满配（默认 min(ncpu,16)）两档（`tc_dev_set_thread_count`）；
  - ffmpeg prores_ks 同档 mov 解码参照（-f null，默认/单线程两档）。

方法论沿用 docs/codec/topos_tier_decode_matrix_2026-09-10.md（逐帧
中位口径），素材换真实解码 raw（旧表为合成素材 + V2 熵 + P4 定标）。
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

from bench_tier_speed import (  # noqa: E402
    FPS, PRORES_ANCHOR, plane_count_bytes, make_fc,
)
from src.shared.codec.topos_binding import ToposCodec  # noqa: E402
from src.shared.export.topos_rate_control import ToposRateFeedback  # noqa: E402
from src.shared.codec.topos_profiles import TOPOS_PROFILE_TIERS  # noqa: E402

QP_MAX = 95
ROUNDS = 5


def encode_packets(codec, src: np.memmap, w: int, h: int, n: int,
                   tier_id: str, bit_depth: int):
    """产品码控路径出包（不计时）。返回 (packets, bpp)。"""
    t = TOPOS_PROFILE_TIERS[tier_id]
    pf = 0 if tier_id in ("proxy", "lt", "standard", "hq") else 1
    dims = plane_count_bytes(pf, w, h)
    per_frame_px = sum(hh * ww for hh, ww in dims)
    target = max(1, round(t.target_bpp * w * h / 8.0))
    fc = make_fc(w, h, t.native_profile, pf, bit_depth,
                 t.qmatrix_id, t.chroma_qp_offset)
    rc = ToposRateFeedback(target, qp_max=QP_MAX)
    packets = []
    total = 0
    for i in range(n):
        off = i * per_frame_px
        fr, c = [], 0
        for hh, ww in dims:
            px = hh * ww
            fr.append(np.ascontiguousarray(
                src[off + c:off + c + px], dtype="<u2").tobytes())
            c += px
        if i == 0:
            pkt, st, qp = codec.encode_sized(fc, fr, target, 0, QP_MAX)
            rc.seed(qp, len(pkt))
        else:
            fc.qp_base = rc.qp
            pkt, st = codec.encode_frame(fc, fr)
            rc.note(len(pkt))
        packets.append(bytes(pkt))
        total += len(pkt)
    return packets, total * 8.0 / (w * h * n)


def decode_stats(codec, packets, warmup: bool = True) -> dict:
    """逐帧解码计时 → p50/p95 fps。"""
    if warmup:
        for pkt in packets[:8]:
            codec.decode(pkt)
    dts = []
    for _ in range(ROUNDS):
        for pkt in packets:
            t0 = time.perf_counter()
            codec.decode(pkt)
            dts.append(time.perf_counter() - t0)
    a = np.array(dts)
    return dict(samples=len(a), p50_fps=float(1.0 / np.median(a)),
                p95_fps=float(1.0 / np.percentile(a, 95)),
                p50_realtime=float((1.0 / np.median(a)) / FPS))


def prores_decode_ref(src_path: Path, pix_fmt: str, w: int, h: int, n: int,
                      profile: str, tmp: Path, threads=None) -> dict:
    mov = tmp / f"dec_{profile}_{pix_fmt}_{threads}.mov"
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-f", "rawvideo",
         "-pixel_format", pix_fmt, "-video_size", f"{w}x{h}",
         "-framerate", str(int(FPS)), "-i", str(src_path),
         "-frames:v", str(n), "-c:v", "prores_ks", "-profile:v", profile,
         str(mov)], check=True)
    cmd = ["ffmpeg", "-v", "error"]
    if threads is not None:
        cmd += ["-threads", str(threads)]
    cmd += ["-i", str(mov), "-f", "null", "-"]
    best = None
    for _ in range(2):
        t0 = time.perf_counter()
        subprocess.run(cmd, check=True)
        wall = time.perf_counter() - t0
        best = wall if best is None else min(best, wall)
    return dict(fps=n / best, realtime=(n / best) / FPS,
                mbps=mov.stat().st_size * 8.0 / (n / FPS) / 1e6)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--family", required=True, choices=("422", "444"))
    ap.add_argument("--src-fmt", required=True)
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--label", required=True)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()

    W, H, N = args.width, args.height, args.frames
    bd = 12 if args.src_fmt.endswith("12le") else 10
    src = np.memmap(args.source, dtype="<u2", mode="r")
    codec = ToposCodec()
    mt_default = codec.slice_threads()
    tmp = Path(tempfile.mkdtemp(prefix="tierdec_"))
    tiers = ("proxy", "lt", "standard", "hq") if args.family == "422" else (
        ("pro444",) if bd == 10 else ("pro444", "extreme"))
    print(f"[dec] {args.label}/{args.family} {args.src_fmt} {W}x{H} × {N} 帧 "
          f"× {ROUNDS} 轮；满配 {mt_default}t；档位 {'/'.join(tiers)}")

    result = dict(label=args.label, family=args.family, src_fmt=args.src_fmt,
                  width=W, height=H, frames=N, bit_depth=bd,
                  threads_default=mt_default, rounds=ROUNDS, rows=[])
    for tier in tiers:
        packets, bpp = encode_packets(codec, src, W, H, N, tier, bd)
        codec.set_slice_threads(mt_default)
        mt = decode_stats(codec, packets)
        codec.set_slice_threads(1)
        st1 = decode_stats(codec, packets)
        codec.set_slice_threads(mt_default)
        ref = prores_decode_ref(Path(args.source), args.src_fmt, W, H, N,
                                PRORES_ANCHOR[tier], tmp)
        ref1 = prores_decode_ref(Path(args.source), args.src_fmt, W, H, N,
                                 PRORES_ANCHOR[tier], tmp, threads=1)
        row = dict(tier=tier, bpp=bpp, packets=len(packets),
                   decode_mt=mt, decode_1t=st1,
                   prores_dec=ref, prores_dec_1t=ref1,
                   ratio_mt=mt["p50_fps"] / ref["fps"],
                   ratio_1t=st1["p50_fps"] / ref1["fps"])
        print(f"[dec] {args.label}/{tier}: {bpp:.3f}bpp | "
              f"满配 p50 {mt['p50_fps']:.0f}fps ({mt['p50_realtime']:.1f}×实时, "
              f"p95 {mt['p95_fps']:.0f}) | 1t p50 {st1['p50_fps']:.0f}fps "
              f"({st1['p50_realtime']:.2f}×) | prores {ref['fps']:.0f}/"
              f"{ref1['fps']:.0f}fps → {row['ratio_mt']:.1f}×/"
              f"{row['ratio_1t']:.1f}×")
        result["rows"].append(row)

    args.output.write_text(json.dumps(result, indent=1))
    print(f"[dec] written {args.output}")


if __name__ == "__main__":
    main()
