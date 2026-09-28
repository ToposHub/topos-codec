#!/usr/bin/env python3
"""等画质码率对比（Topos vs ProRes 同档，2026-09-11）。

回答"同画质下码率差多少"：Topos 以固定 qp 网格（档位落点 qp 为中心，
±4..±12，钳位 0..95）在共享源上建 R-D 点列（16 帧，产品编码几何：
rans2、flat、444 含 C042 配置），对 ProRes 同档锚点的合成 PSNR 做
log2(bpp) 线性插值 → 等画质 bpp → 码率比。同时在锚点码率处插值
Topos PSNR → 同码率 ΔdB（与 gap/align444 批互验）。

锚点（bpp + 合成 PSNR）从既有证据 JSON 读取：
  --anchor-family gap   → gap_decomp json profiles[tier].prores_bpp/.prores
  --anchor-family align → align444 json anchors[name].bpp/.psnr
"""
from __future__ import annotations

import argparse
import ctypes
import json
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]
TOOLS = Path(__file__).resolve().parent
for p in (str(REPO), str(TOOLS)):
    if p not in sys.path:
        sys.path.insert(0, p)

from bench_tier_speed import make_fc, plane_count_bytes  # noqa: E402
from src.shared.codec.topos_binding import ToposCodec  # noqa: E402
from src.shared.codec.topos_profiles import TOPOS_PROFILE_TIERS  # noqa: E402


def load_anchor(path: Path, family: str, key: str) -> tuple:
    d = json.loads(path.read_text())
    if family == "gap":
        for r in d["profiles"]:
            if r["tier"] == key:
                return r["prores_bpp"], r["prores"]["combined"]
    else:
        for r in d["anchors"]:
            if r["anchor"] == key:
                return r["bpp"], r["psnr"]["combined"]
    raise SystemExit(f"锚点 {key} 不在 {path}")


def run_qp(codec, src, w, h, n, tier_id, bd, qp, dims, per_frame_px):
    t = TOPOS_PROFILE_TIERS[tier_id]
    pf = 0 if tier_id in ("proxy", "lt", "standard", "hq") else 1
    fc = make_fc(w, h, t.native_profile, pf, bd, t.qmatrix_id,
                 t.chroma_qp_offset, qp=qp)
    total = 0
    mse = [0.0, 0.0, 0.0]
    for i in range(n):
        off = i * per_frame_px
        fr, c = [], 0
        for hh, ww in dims:
            px = hh * ww
            fr.append(np.ascontiguousarray(
                src[off + c:off + c + px], dtype="<u2").tobytes())
            c += px
        pkt, st = codec.encode_frame(fc, fr)
        total += len(pkt)
        rec = codec.decode(pkt)
        c = 0
        for k, (hh, ww) in enumerate(dims):
            px = hh * ww
            a = np.frombuffer(rec.planes[k], dtype="<u2", count=px).astype(np.float64)
            s = src[off + c:off + c + px].astype(np.float64)
            mse[k] += float(np.mean((a - s) ** 2))
            c += px
    m = [v / n for v in mse]
    peak = float((1 << bd) - 1)

    def p(x):
        return 999.0 if x <= 0.0 else 10.0 * np.log10(peak * peak / x)

    weights = (0.5, 0.25, 0.25) if pf == 0 else (1 / 3, 1 / 3, 1 / 3)
    combined = p(sum(wi * mi for wi, mi in zip(weights, m)))
    return total * 8.0 / (w * h * n), float(combined), m


def interp_logbpp(points, target_psnr):
    """points: [(bpp, psnr)] → 在 target_psnr 处插值 bpp（log2 域线性）。"""
    pts = sorted(points, key=lambda x: x[1])
    for (b0, p0), (b1, p1) in zip(pts, pts[1:]):
        if p0 <= target_psnr <= p1:
            f = 0.0 if p1 == p0 else (target_psnr - p0) / (p1 - p0)
            return 2.0 ** (np.log2(b0) + f * (np.log2(b1) - np.log2(b0))), False
    # 区间外：用最近两端外推（标记）
    (b0, p0), (b1, p1) = pts[0], pts[-1]
    if p1 == p0:
        return b0, True
    f = (target_psnr - p0) / (p1 - p0)
    return 2.0 ** (np.log2(b0) + f * (np.log2(b1) - np.log2(b0))), True


def interp_psnr(points, target_bpp):
    pts = sorted(points, key=lambda x: x[0])
    for (b0, p0), (b1, p1) in zip(pts, pts[1:]):
        if b0 <= target_bpp <= b1:
            f = 0.0 if b1 == b0 else (np.log2(target_bpp) - np.log2(b0)) / (np.log2(b1) - np.log2(b0))
            return p0 + f * (p1 - p0), False
    (b0, p0), (b1, p1) = pts[0], pts[-1]
    f = (np.log2(target_bpp) - np.log2(b0)) / (np.log2(b1) - np.log2(b0))
    return p0 + f * (p1 - p0), True


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--tier", required=True)
    ap.add_argument("--family", required=True, choices=("422", "444"))
    ap.add_argument("--src-fmt", required=True)
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--frames", type=int, default=16)
    ap.add_argument("--qp-center", type=int, required=True)
    ap.add_argument("--anchor-json", required=True)
    ap.add_argument("--anchor-family", required=True, choices=("gap", "align"))
    ap.add_argument("--anchor-key", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()

    W, H, N = args.width, args.height, args.frames
    bd = 12 if args.src_fmt.endswith("12le") else 10
    anchor_bpp, anchor_psnr = load_anchor(
        Path(args.anchor_json), args.anchor_family, args.anchor_key)
    pf = 0 if args.family == "422" else 1
    dims = plane_count_bytes(pf, W, H)
    per_frame_px = sum(hh * ww for hh, ww in dims)
    src = np.memmap(args.source, dtype="<u2", mode="r")
    codec = ToposCodec()

    grid = sorted({min(95, max(0, q)) for q in
                   (args.qp_center + d for d in (-8, -4, 0, 4, 8, 12))})
    points = []
    for qp in grid:
        bpp, psnr, _ = run_qp(codec, src, W, H, N, args.tier, bd, qp,
                              dims, per_frame_px)
        points.append(dict(qp=qp, bpp=bpp, psnr=psnr))
        print(f"[iso] {args.label}/{args.tier} qp{qp}: {bpp:.4f}bpp "
              f"{psnr:.2f}dB")
    curve = [(p["bpp"], p["psnr"]) for p in points]
    iso_bpp, iso_extrap = interp_logbpp(curve, anchor_psnr)
    matched_psnr, m_extrap = interp_psnr(curve, anchor_bpp)
    row = dict(label=args.label, tier=args.tier, anchor=dict(
        key=args.anchor_key, bpp=anchor_bpp, psnr=anchor_psnr),
        points=points,
        iso=dict(bpp=iso_bpp, ratio=iso_bpp / anchor_bpp,
                 extrap=iso_extrap, saving=1.0 - iso_bpp / anchor_bpp),
        matched=dict(psnr=matched_psnr, delta=matched_psnr - anchor_psnr,
                     extrap=m_extrap))
    print(f"[iso] {args.label}/{args.tier} vs {args.anchor_key}: "
          f"锚点 {anchor_bpp:.3f}bpp/{anchor_psnr:.2f}dB | 同码率 "
          f"{matched_psnr:.2f}dB (Δ{matched_psnr - anchor_psnr:+.2f}) | "
          f"同画质 {iso_bpp:.3f}bpp ({iso_bpp / anchor_bpp:.3f}×, "
          f"省 {1 - iso_bpp / anchor_bpp:+.1%})"
          f"{' [外推]' if iso_extrap or m_extrap else ''}")
    d = json.loads(args.output.read_text()) if args.output.exists() else []
    d.append(row)
    args.output.write_text(json.dumps(d, indent=1))


if __name__ == "__main__":
    main()
