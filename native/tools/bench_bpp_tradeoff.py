#!/usr/bin/env python3
"""码率-速度权衡探针（2026-09-11）：tier bpp 下调换什么。

对每格（源 × 档位 × 显式目标 bpp 点）：产品码控路径编码 60 帧
（编码纯时间）→ 同包解码 p50/p95（3 轮）。用于量化"画质领先兑现
成更小文件"能买到多少编码/解码速度——解码侧逐块 IDCT/写回为固定
成本，符号量随 bpp 的弹性有限（解码矩阵已示 4K 444 5.5→8.0bpp
仅差 5%），本探针给出每格的实测弹性。

与 bench_tier_speed/bench_tier_decode 的差别：target 显式传入
（不读 tier 表），一次跑多点。
"""
from __future__ import annotations

import argparse
import ctypes
import json
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]
TOOLS = Path(__file__).resolve().parent
for p in (str(REPO), str(TOOLS)):
    if p not in sys.path:
        sys.path.insert(0, p)

from bench_tier_speed import FPS, make_fc, plane_count_bytes  # noqa: E402
from src.shared.codec.topos_binding import ToposCodec  # noqa: E402
from src.shared.export.topos_rate_control import ToposRateFeedback  # noqa: E402
from src.shared.codec.topos_profiles import TOPOS_PROFILE_TIERS  # noqa: E402

QP_MAX = 95
ROUNDS = 3


def run_point(codec, src, w, h, n, tier_id, bd, target_bpp):
    t = TOPOS_PROFILE_TIERS[tier_id]
    pf = 0 if tier_id in ("proxy", "lt", "standard", "hq") else 1
    dims = plane_count_bytes(pf, w, h)
    per_frame_px = sum(hh * ww for hh, ww in dims)
    target = max(1, round(target_bpp * w * h / 8.0))
    fc = make_fc(w, h, t.native_profile, pf, bd, t.qmatrix_id,
                 t.chroma_qp_offset)

    def frame_bytes(i):
        off = i * per_frame_px
        out, c = [], 0
        for hh, ww in dims:
            px = hh * ww
            out.append(np.ascontiguousarray(
                src[off + c:off + c + px], dtype="<u2").tobytes())
            c += px
        return out

    fc.qp_base = 40
    codec.encode_frame(fc, frame_bytes(0))          # 预热不计时

    rc = ToposRateFeedback(target, qp_max=QP_MAX)
    packets, total, enc_secs, qps = [], 0, 0.0, []
    for i in range(n):
        fr = frame_bytes(i)
        t0 = time.perf_counter()
        if i == 0:
            pkt, st, qp = codec.encode_sized(fc, fr, target, 0, QP_MAX)
            rc.seed(qp, len(pkt))
        else:
            fc.qp_base = rc.qp
            pkt, st = codec.encode_frame(fc, fr)
            qp = int(fc.qp_base)
            rc.note(len(pkt))
        enc_secs += time.perf_counter() - t0
        qps.append(qp)
        total += len(pkt)
        packets.append(bytes(pkt))

    for pkt in packets[:8]:
        codec.decode(pkt)                            # 解码预热
    dts = []
    for _ in range(ROUNDS):
        for pkt in packets:
            t0 = time.perf_counter()
            codec.decode(pkt)
            dts.append(time.perf_counter() - t0)
    a = np.array(dts)
    bpp = total * 8.0 / (w * h * n)
    return dict(target_bpp=target_bpp, bpp=bpp, ratio=bpp / target_bpp,
                mbps=total * 8.0 / (n / FPS) / 1e6,
                encode_fps=n / enc_secs, decode_p50=1.0 / float(np.median(a)),
                decode_p95=1.0 / float(np.percentile(a, 95)),
                qp0=qps[0], qp_mean=float(np.mean(qps)))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--family", required=True, choices=("422", "444"))
    ap.add_argument("--src-fmt", required=True)
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--cells", required=True,
                    help="tier:bpp,bpp[;tier:...] 显式目标点")
    ap.add_argument("--label", required=True)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()

    W, H, N = args.width, args.height, args.frames
    bd = 12 if args.src_fmt.endswith("12le") else 10
    src = np.memmap(args.source, dtype="<u2", mode="r")
    codec = ToposCodec()
    rows = []
    for cell in args.cells.split(";"):
        tier, bps = cell.split(":")
        for bp in bps.split(","):
            r = run_point(codec, src, W, H, N, tier, bd, float(bp))
            rows.append(dict(tier=tier, **r))
            print(f"[trade] {args.label}/{tier} @ {bp}bpp: 落 {r['bpp']:.3f}bpp "
                  f"({r['ratio']:.3f}×) qp{r['qp0']}→均{r['qp_mean']:.1f} | "
                  f"编码 {r['encode_fps']:.1f}fps | 解码 p50 "
                  f"{r['decode_p50']:.1f}fps (p95 {r['decode_p95']:.1f})")
    out = dict(label=args.label, family=args.family, src_fmt=args.src_fmt,
               width=W, height=H, frames=N, bit_depth=bd, rows=rows)
    args.output.write_text(json.dumps(out, indent=1))
    print(f"[trade] written {args.output}")


if __name__ == "__main__":
    main()
