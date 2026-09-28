#!/usr/bin/env python3
"""AQ 码率模型 qp≥64 失配复现与分层定位（战役遗留项 2，2026-09-11）。

复现产品路径（topos_encoder._encode_packet 同源语义）：
  首帧 sized 搜索（qp_min=0/qp_max=95）→ ToposRateFeedback.seed →
  单遍固定 qp + note() 反馈 + qp95 AQ 守卫。

分层：
  L1 码流曲线  固定 qp 的 bytes(qp)（AQ on/off）——C 搜索可见的曲线形状；
  L2 帧内搜索  首帧 sized 落点（qp_used、bytes/target）；
  L3 帧间反馈  8 帧 qp 轨迹 / 平均 bytes-target 偏差 / PSNR。

口径：2K prores422HQ 真实素材（1920×1080 yuv422p10le），proxy tier
target_bpp=0.32310（ADR-C032），flat 矩阵，slice_rows=16，qp_base 起点 20。
"""
from __future__ import annotations

import argparse
import ctypes
import json
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from src.shared.codec.topos_binding import (  # noqa: E402
    TOPOS_CODEC_ABI_VERSION, ToposCodec, _CFrameConfig,
)
from src.shared.export.topos_rate_control import ToposRateFeedback  # noqa: E402

PROXY_BPP = 0.32310
QP_MAX = 95
SLICE_ROWS = 16


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


def make_fc(entropy: int, aq: int, w: int, h: int, qp: int = 20) -> _CFrameConfig:
    fc = _CFrameConfig()
    fc.struct_size = ctypes.sizeof(_CFrameConfig)
    fc.abi_version = TOPOS_CODEC_ABI_VERSION
    fc.visible_width = w
    fc.visible_height = h
    fc.profile = 3
    fc.pixel_format = 0
    fc.bit_depth = 10
    fc.qmatrix_id = 0        # flat（ADR-C033）
    fc.qp_base = qp
    fc.qp_delta_chroma = 0   # proxy
    fc.slice_rows = SLICE_ROWS
    fc.sar_num = fc.sar_den = 1
    fc.reserved[0] = entropy
    fc.reserved[1] = aq
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
    for p, s, (h, w) in zip(fr.planes, src, dims):
        rec = np.frombuffer(p, dtype="<u2", count=h * w)
        out.append(psnr(rec.reshape(h, w), s))
    return out


def clone(fc: _CFrameConfig) -> _CFrameConfig:
    f2 = _CFrameConfig()
    ctypes.memmove(ctypes.byref(f2), ctypes.byref(fc), ctypes.sizeof(_CFrameConfig))
    return f2


def enc_planes(planes) -> list:
    return [p.tobytes() for p in planes]


def product_path(codec: ToposCodec, frames, target: int, entropy: int, aq: int,
                 w: int, h: int):
    """产品码控路径镜像。返回逐帧记录列表。"""
    fc = make_fc(entropy, aq, w, h)
    recs = []
    # 首帧 sized + 守卫 + seed
    pkt, st, qp_used = codec.encode_sized(fc, enc_planes(frames[0]), target, 0, QP_MAX)
    if aq and qp_used >= QP_MAX and len(pkt) > target:
        fcg = clone(fc)
        fcg.reserved[1] = 0
        pkt2, st2, _ = codec.encode_sized(fcg, enc_planes(frames[0]), target, QP_MAX, QP_MAX)
        if len(pkt2) < len(pkt):
            pkt, st = pkt2, st2
    rc = ToposRateFeedback(target, qp_max=QP_MAX)
    rc.seed(qp_used, len(pkt))
    p = frame_psnr(codec, pkt, frames[0], w, h)
    recs.append(dict(frame=0, qp=qp_used, bytes=len(pkt), ratio=len(pkt) / target,
                     y=p[0], u=p[1], v=p[2]))
    for i in range(1, len(frames)):
        fc.qp_base = rc.qp
        pkt, st = codec.encode_frame(fc, enc_planes(frames[i]))
        if aq and int(rc.qp) >= QP_MAX and len(pkt) > target:
            fcg = clone(fc)
            fcg.reserved[1] = 0
            pkt2, st2 = codec.encode_frame(fcg, enc_planes(frames[i]))
            if len(pkt2) < len(pkt):
                pkt, st = pkt2, st2
        rc.note(len(pkt))
        p = frame_psnr(codec, pkt, frames[i], w, h)
        recs.append(dict(frame=i, qp=int(fc.qp_base), bytes=len(pkt),
                         ratio=len(pkt) / target, y=p[0], u=p[1], v=p[2]))
    return recs


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixture", default="/tmp/aqrepro/2k.raw")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--frames", type=int, default=8)
    ap.add_argument("--entropies", type=int, nargs="+", default=[8, 1],
                    help="reserved[0]：8=rans2(默认) 1=V2（战役第5步口径）")
    ap.add_argument("--curve-qps", type=int, nargs="+",
                    default=[70, 72, 74, 76, 77, 78, 79, 80, 81, 82, 84, 88, 95])
    ap.add_argument("--output", type=Path,
                    default=REPO / "native" / "topos_codec" / "tools"
                    / "aq_rate_repro_2026-09-11.json")
    args = ap.parse_args()

    W2K, H2K = args.width, args.height
    frames = load_raw(Path(args.fixture), W2K, H2K, args.frames)
    if not frames:
        raise SystemExit(f"fixture 不可用: {args.fixture}")
    target = max(1, round(PROXY_BPP * W2K * H2K / 8.0))
    print(f"[aq] fixture={args.fixture} {W2K}x{H2K} × {len(frames)} 帧 "
          f"target={target}B (proxy bpp={PROXY_BPP})")

    codec = ToposCodec()
    result = dict(target=target, proxy_bpp=PROXY_BPP, frames=len(frames),
                   fixture=str(args.fixture), width=W2K, height=H2K, runs=[])

    # ---- L1：固定 qp 码流曲线（AQ on/off，帧 0）----
    for entropy in args.entropies:
        for aq in (0, 1):
            curve = []
            for qp in args.curve_qps:
                fc = make_fc(entropy, aq, W2K, H2K, qp)
                pkt, st = codec.encode_frame(fc, enc_planes(frames[0]))
                curve.append(dict(qp=qp, bytes=len(pkt)))
            print(f"[aq] L1 curve ent={entropy} aq={aq}: " + " ".join(
                f"{c['qp']}:{c['bytes']}" for c in curve))
            result["runs"].append(dict(layer="L1_curve", entropy=entropy, aq=aq,
                                       points=curve))

    # ---- L2+L3：产品码控路径 ----
    for entropy in args.entropies:
        for aq in (0, 1):
            recs = product_path(codec, frames, target, entropy, aq, W2K, H2K)
            avg = sum(r["bytes"] for r in recs) / len(recs)
            avg_y = sum(r["y"] for r in recs) / len(recs)
            avg_u = sum(r["u"] for r in recs) / len(recs)
            avg_v = sum(r["v"] for r in recs) / len(recs)
            traj = " ".join(f"f{r['frame']}:q{r['qp']}:{r['ratio']:.2f}" for r in recs)
            print(f"[aq] L2/L3 ent={entropy} aq={aq} avg={avg / target:.3f}×T "
                  f"Y={avg_y:.2f} U={avg_u:.2f} V={avg_v:.2f}")
            print(f"     traj {traj}")
            result["runs"].append(dict(layer="L2L3_product", entropy=entropy, aq=aq,
                                       records=recs, avg_ratio=avg / target,
                                       avg_y=avg_y, avg_u=avg_u, avg_v=avg_v))

    args.output.write_text(json.dumps(result, indent=1))
    print(f"[aq] written {args.output}")


if __name__ == "__main__":
    main()
