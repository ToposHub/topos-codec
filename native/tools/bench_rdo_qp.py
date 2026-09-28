#!/usr/bin/env python3
"""RDO 按 qp 域门控评估（战役遗留项 6，2026-09-11）。

层：
  L1 固定 qp R-D 对拍（V2 熵，RDO 仅 V2 可用）：qp 网格 × rdo{0,1} →
     bytes/PSNR 曲线 → 匹配 PSNR 的 Δbits 与匹配 bytes 的 ΔPSNR，
     按 qp 区间定位正负分界 qp*；
  L2 产品语境同码率：tier 目标（proxy/standard/hq）×
     {默认 rans2 无RDO, V2+RDO, V2 无RDO}——回答"门控启用能否打过当前
     产品默认"（V2 相对 rans2 有 ~9% payload 劣势）；
  L3 编码开销：固定 qp RDO on/off 墙钟比。

口径：2K prores422HQ 真实素材（1920×1080 yuv422p10le），flat 矩阵，
slice_rows=16。RDO = reserved[2]（须 reserved[0]=1 V2）。
"""
from __future__ import annotations

import argparse
import ctypes
import json
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]
import sys  # noqa: E402

if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from src.shared.codec.topos_binding import (  # noqa: E402,F401
    TOPOS_CODEC_ABI_VERSION, ToposCodec, _CFrameConfig,
)

TIERS = {"proxy": 0.32310, "standard": 1.64528, "hq": 2.66245}
QP_MAX = 95


def load_raw(path: Path, w: int, h: int, n: int):
    fsz = (w * h + 2 * (w // 2) * h) * 2
    frames = []
    with open(path, "rb") as f:
        while len(frames) < n:
            b = f.read(fsz)
            if len(b) < fsz:
                break
            yh = w * h
            ch = (w // 2) * h
            y = np.frombuffer(b[:yh * 2], dtype="<u2").reshape(h, w)
            u = np.frombuffer(b[yh * 2:(yh + ch) * 2], dtype="<u2").reshape(h, w // 2)
            v = np.frombuffer(b[(yh + ch) * 2:], dtype="<u2").reshape(h, w // 2)
            frames.append([y, u, v])
    return frames


def make_fc(w: int, h: int, entropy: int, rdo: int, qp: int = 20) -> _CFrameConfig:
    fc = _CFrameConfig()
    fc.struct_size = ctypes.sizeof(_CFrameConfig)
    fc.abi_version = TOPOS_CODEC_ABI_VERSION
    fc.visible_width = w
    fc.visible_height = h
    fc.profile = 3
    fc.pixel_format = 0
    fc.bit_depth = 10
    fc.qmatrix_id = 0
    fc.qp_base = qp
    fc.qp_delta_chroma = 0
    fc.slice_rows = 16
    fc.sar_num = fc.sar_den = 1
    fc.reserved[0] = entropy
    fc.reserved[2] = rdo
    return fc


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    d = a.astype(np.float64) - b.astype(np.float64)
    mse = float(np.mean(d * d))
    if mse <= 0.0:
        return 999.0
    return 10.0 * np.log10(1023.0 * 1023.0 / mse)


def frame_psnr(codec: ToposCodec, pkt: bytes, src, w: int, h: int) -> list:
    fr = codec.decode(pkt)
    dims = [(h, w), (h, w // 2), (h, w // 2)]
    out = []
    for p, s, (hh, ww) in zip(fr.planes, src, dims):
        rec = np.frombuffer(p, dtype="<u2", count=hh * ww)
        out.append(psnr(rec.reshape(hh, ww), s))
    return out


def enc(planes) -> list:
    return [p.tobytes() for p in planes]


def interp_at(xs, ys, x):
    """单调曲线（x 增）上对 x 的线性插值；域外 None。"""
    if x < xs[0] or x > xs[-1]:
        return None
    for i in range(len(xs) - 1):
        if xs[i] <= x <= xs[i + 1]:
            if xs[i + 1] == xs[i]:
                return ys[i]
            t = (x - xs[i]) / (xs[i + 1] - xs[i])
            return ys[i] + t * (ys[i + 1] - ys[i])
    return ys[-1]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixture", default="/tmp/aqrepro/2k.raw")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--qps", type=int, nargs="+",
                    default=[24, 32, 40, 48, 52, 56, 58, 60, 62, 64, 66, 68, 70, 72, 76, 80, 84])
    ap.add_argument("--timing-qp", type=int, nargs="+", default=[48, 62])
    ap.add_argument("--output", type=Path,
                    default=REPO / "native" / "topos_codec" / "tools"
                    / "rdo_qp_eval_2026-09-11.json")
    args = ap.parse_args()

    W, H = args.width, args.height
    frames = load_raw(Path(args.fixture), W, H, max(args.frames, 8))
    if not frames:
        raise SystemExit(f"fixture 不可用: {args.fixture}")
    frames = frames[:args.frames] if args.frames > 3 else frames[:3]
    print(f"[rdo] fixture={args.fixture} {W}x{H} × {len(frames)} 帧")
    codec = ToposCodec()
    result = dict(fixture=str(args.fixture), width=W, height=H,
                  frames=len(frames), layers={})

    # ---- L1：固定 qp R-D 对拍（V2，rdo 0/1）----
    l1 = {}
    for rdo in (0, 1):
        rows = []
        for qp in args.qps:
            fc = make_fc(W, H, 1, rdo, qp)
            tot_b = 0
            ps = [0.0, 0.0, 0.0]
            for fr in frames:
                pkt, st = codec.encode_frame(fc, enc(fr))
                tot_b += len(pkt)
                p = frame_psnr(codec, pkt, fr, W, H)
                for k in range(3):
                    ps[k] += p[k] / len(frames)
            rows.append(dict(qp=qp, bytes=tot_b / len(frames),
                             y=ps[0], u=ps[1], v=ps[2]))
            print(f"[rdo] L1 rdo={rdo} qp={qp}: {int(tot_b/len(frames))}B "
                  f"Y={ps[0]:.2f} U={ps[1]:.2f} V={ps[2]:.2f}")
        l1[rdo] = rows
    # 匹配 bytes 的 ΔPSNR —— 域分离（ADR-C037/C040 教训：跨无损平台插值
    # 会产生 ±19 dB 级伪影）。无损点（PSNR>=999 哨兵）只做同 qp 直比
    # Δbytes；有损域内才做匹配 bytes 插值。
    r0, r1 = l1[0], l1[1]
    LOSSLESS = 999.0

    def curve_lossy(rows):
        pts = [r for r in rows if r["y"] < LOSSLESS - 1.0]
        pts.sort(key=lambda r: r["bytes"])
        return pts

    p1 = curve_lossy(r1)
    if p1:
        b1s = [r["bytes"] for r in p1]
        y1s = [r["y"] for r in p1]
        u1s = [r["u"] for r in p1]
        v1s = [r["v"] for r in p1]
    else:
        b1s = y1s = u1s = v1s = []
    match = []
    for i, r in enumerate(r0):
        same_qp = next((q for q in r1 if q["qp"] == r["qp"]), None)
        if r["y"] >= LOSSLESS - 1.0:
            # 平台段：两侧都无损（或 rdo1 跳出平台）→ 同 qp 字节直比
            if same_qp is not None:
                db = (same_qp["bytes"] - r["bytes"]) / r["bytes"] * 100.0
                tag = "lossless" if same_qp["y"] >= LOSSLESS - 1.0 else "rdo1-lossy"
                match.append(dict(qp=r["qp"], mode=tag, dbytes_pct=db))
                print(f"[rdo] L1 平台 qp={r['qp']} ({tag}): Δbytes={db:+.2f}%")
            continue
        if not b1s:
            continue
        py = interp_at(b1s, y1s, r["bytes"])
        pu = interp_at(b1s, u1s, r["bytes"])
        pv = interp_at(b1s, v1s, r["bytes"])
        if py is None:
            continue
        match.append(dict(qp=r["qp"], mode="matched", dy=py - r["y"],
                          du=pu - r["u"], dv=pv - r["v"]))
        print(f"[rdo] L1 同码率 qp={r['qp']}: ΔY={py - r['y']:+.2f} "
              f"ΔU={pu - r['u']:+.2f} ΔV={pv - r['v']:+.2f} dB")
    result["layers"]["L1"] = dict(curves={str(k): v for k, v in l1.items()},
                                  matched=match)

    # ---- L2：产品语境（tier 目标，首帧 sized + 8 帧反馈）----
    from src.shared.export.topos_rate_control import ToposRateFeedback  # noqa: E402

    frames_full = frames if len(frames) >= 8 else load_raw(
        Path(args.fixture), W, H, 8)[:8]
    l2 = []
    for tier, bpp in TIERS.items():
        target = max(1, round(bpp * W * H / 8.0))
        for name, ent, rdo in (("default_rans2", 8, 0), ("v2_rdo1", 1, 1),
                               ("v2_rdo0", 1, 0)):
            # 逐帧重跑拿 PSNR（sized/反馈确定性 → 轨迹可复现，逐帧解码）
            fc = make_fc(W, H, ent, rdo)
            pkt, st, qp_used = codec.encode_sized(fc, enc(frames_full[0]), target, 0, QP_MAX)
            rc = ToposRateFeedback(target, qp_max=QP_MAX)
            rc.seed(qp_used, len(pkt))
            ps = frame_psnr(codec, pkt, frames_full[0], W, H)
            tot = len(pkt)
            for i in range(1, len(frames_full)):
                fc.qp_base = rc.qp
                pkt, st = codec.encode_frame(fc, enc(frames_full[i]))
                rc.note(len(pkt))
                p = frame_psnr(codec, pkt, frames_full[i], W, H)
                ps = [a + b for a, b in zip(ps, p)]
                tot += len(pkt)
            n = len(frames_full)
            avg_ps = [x / n for x in ps]
            l2.append(dict(tier=tier, cfg=name, target=target,
                           qp0=qp_used, avg_ratio=tot / (target * n),
                           y=avg_ps[0], u=avg_ps[1], v=avg_ps[2]))
            print(f"[rdo] L2 {tier}/{name}: qp0={qp_used} avg={tot/(target*n):.3f}×T "
                  f"Y={avg_ps[0]:.2f} U={avg_ps[1]:.2f} V={avg_ps[2]:.2f}")
    result["layers"]["L2"] = l2

    # ---- L3：编码开销（固定 qp，墙钟，两轮取最小）----
    l3 = {}
    for qp in args.timing_qp:
        sec = {}
        for rdo in (0, 1):
            best = None
            for _ in range(2):
                t0 = time.perf_counter()
                fc = make_fc(W, H, 1, rdo, qp)
                for fr in frames:
                    codec.encode_frame(fc, enc(fr))
                dt = time.perf_counter() - t0
                best = dt if best is None or dt < best else best
            sec[rdo] = best
        ovh = (sec[1] / sec[0] - 1.0) * 100.0 if sec[0] else None
        l3[qp] = dict(sec_rdo0=sec[0], sec_rdo1=sec[1], overhead_pct=ovh)
        print(f"[rdo] L3 qp={qp}: rdo0={sec[0]:.3f}s rdo1={sec[1]:.3f}s "
              f"overhead={ovh:+.1f}%")
    result["layers"]["L3"] = l3

    args.output.write_text(json.dumps(result, indent=1))
    print(f"[rdo] written {args.output}")


if __name__ == "__main__":
    main()
