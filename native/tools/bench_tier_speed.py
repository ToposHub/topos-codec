#!/usr/bin/env python3
"""Topos 产品档位编码速度+码率矩阵（2K/4K × 六档，2026-09-11 口径）。

C032~C042 各轮落地后的产品终态测量：六档（proxy/lt/standard/hq/
pro444/extreme）× 2K/4K，产品码控路径（首帧 sized + ToposRateFeedback
反馈单遍、rans2、flat；444 档含 ADR-C042 重定标与色度偏移归零）：

  - 编码纯时间：perf_counter 只累计 encode 调用（源读取/tobytes/解码
    回读全部剥离；native 内部按 tc_dev_thread_count() 切片并行，
    与产品路径一致）；
  - 码率：落地 bpp / Mbps（25p）/ 贴目标倍数 / qp 轨迹；
  - prores_ks 同档参照：ffmpeg 同源同 profile 墙钟 fps（"编码 N×
    领先"口径刷新）。

源：422 家族 = prores422HQ 解码 yuv422p10le；444 家族 = DNxHR 444
解码 yuv444p10le（pro444）/ yuv444p12le（extreme）。N 帧（默认 60）。
"""
from __future__ import annotations

import argparse
import ctypes
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from src.shared.codec.topos_binding import (  # noqa: E402
    TOPOS_CODEC_ABI_VERSION, ToposCodec, ToposDecoder, _CFrameConfig,
)
from src.shared.export.topos_rate_control import ToposRateFeedback  # noqa: E402
from src.shared.codec.topos_profiles import TOPOS_PROFILE_TIERS  # noqa: E402

QP_MAX = 95
FPS = 25.0
# prores_ks 同档参照 profile（档位语义锚，ADR-C032/C042）
PRORES_ANCHOR = {"proxy": "0", "lt": "1", "standard": "2", "hq": "3",
                 "pro444": "4444", "extreme": "4444xq"}


def make_fc(w: int, h: int, profile: int, pixel_format: int, bit_depth: int,
            qmatrix: int, dchroma: int, qp: int = 20) -> _CFrameConfig:
    fc = _CFrameConfig()
    fc.struct_size = ctypes.sizeof(_CFrameConfig)
    fc.abi_version = TOPOS_CODEC_ABI_VERSION
    fc.visible_width = w
    fc.visible_height = h
    fc.profile = profile
    fc.pixel_format = pixel_format
    fc.bit_depth = bit_depth
    fc.qmatrix_id = qmatrix
    fc.qp_base = qp
    fc.qp_delta_chroma = dchroma
    fc.slice_rows = 16
    fc.sar_num = fc.sar_den = 1
    fc.reserved[0] = 8     # rans2（ADR-C038 产品默认）
    return fc


def plane_count_bytes(pixel_format: int, w: int, h: int):
    """每帧平面数与各平面字节数（u16）。"""
    if pixel_format == 0:   # 4:2:2
        return [(h, w), (h, w // 2), (h, w // 2)]
    return [(h, w), (h, w), (h, w)]   # 4:4:4


def topos_tier_run(codec, src: np.memmap, w: int, h: int, n: int, tier_id: str,
                   bit_depth: int) -> dict:
    """产品码控路径单档测量。返回 fps/码率/qp 轨迹。"""
    # The report keeps the historical 4:4:4 labels (pro444/extreme), while
    # the product profile registry uses the public export ids (4444/4444xq).
    # Resolve the alias here so the benchmark remains usable after the
    # profile-id migration.
    profile_id = {"pro444": "4444", "extreme": "4444xq"}.get(tier_id, tier_id)
    t = TOPOS_PROFILE_TIERS[profile_id]
    pf = 0 if tier_id in ("proxy", "lt", "standard", "hq") else 1
    dims = plane_count_bytes(pf, w, h)
    per_frame_px = sum(hh * ww for hh, ww in dims)
    target = max(1, round(t.target_bpp * w * h / 8.0))
    fc = make_fc(w, h, t.native_profile, pf, bit_depth,
                 t.qmatrix_id, t.chroma_qp_offset)

    def frame_bytes(i: int) -> list:
        off = i * per_frame_px
        out, c = [], 0
        for hh, ww in dims:
            px = hh * ww
            out.append(np.ascontiguousarray(
                src[off + c:off + c + px], dtype="<u2").tobytes())
            c += px
        return out

    # 预热一帧（库初始化/LUT/线程池摊销不进计时）
    warm = frame_bytes(0)
    fc.qp_base = 40
    codec.encode_frame(fc, warm)

    rc = ToposRateFeedback(target, qp_max=QP_MAX)
    total_bytes = 0
    enc_secs = 0.0
    qps = []
    pkts = []   # 解码测速留存（内存换计时口径）
    for i in range(n):
        fr = frame_bytes(i)
        t0 = time.perf_counter()
        if i == 0:
            pkt, st, qp_used = codec.encode_sized(fc, fr, target, 0, QP_MAX)
            rc.seed(qp_used, len(pkt))
            qp = qp_used
        else:
            fc.qp_base = rc.qp
            pkt, st = codec.encode_frame(fc, fr)
            qp = int(fc.qp_base)
            rc.note(len(pkt))
        enc_secs += time.perf_counter() - t0
        qps.append(qp)
        total_bytes += len(pkt)
        pkts.append(pkt)
    bpp = total_bytes * 8.0 / (w * h * n)

    # 解码测速（产品路径：常驻 decoder + 池化 plane 视图；首帧预热不计时）
    planes = [np.zeros(d, dtype="<u2") for d in dims]
    with ToposDecoder(codec) as dec:
        views = dec.make_views(planes)
        dec.decode_views(pkts[0], views, len(planes))
        t0 = time.perf_counter()
        for pkt in pkts:
            dec.decode_views(pkt, views, len(planes))
        dec_secs = time.perf_counter() - t0
    return dict(
        tier=tier_id, target_frame_bytes=target, bytes=total_bytes,
        bpp=bpp, target_bpp=t.target_bpp, ratio=bpp / t.target_bpp,
        mbps=total_bytes * 8.0 / (n / FPS) / 1e6,
        encode_secs=enc_secs, fps=n / enc_secs,
        realtime=(n / enc_secs) / FPS,
        qp0=qps[0], qp_mean=float(np.mean(qps)),
        qp_min=min(qps), qp_max_seen=max(qps),
        decode_secs=dec_secs, decode_fps=n / dec_secs,
        decode_realtime=(n / dec_secs) / FPS,
    )


def prores_ref(src_path: Path, pix_fmt: str, w: int, h: int, n: int,
               profile: str, tmp: Path) -> dict:
    mov = tmp / f"pr_{profile}_{pix_fmt}.mov"
    cmd = ["ffmpeg", "-y", "-v", "error", "-f", "rawvideo",
           "-pixel_format", pix_fmt, "-video_size", f"{w}x{h}",
           "-framerate", str(int(FPS)), "-i", str(src_path),
           "-frames:v", str(n), "-c:v", "prores_ks", "-profile:v", profile,
           str(mov)]
    t0 = time.perf_counter()
    subprocess.run(cmd, check=True)
    wall = time.perf_counter() - t0
    return dict(profile=profile, bytes=mov.stat().st_size, wall=wall,
                fps=n / wall, realtime=(n / wall) / FPS,
                mbps=mov.stat().st_size * 8.0 / (n / FPS) / 1e6,
                bpp=mov.stat().st_size * 8.0 / (w * h * n))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--family", required=True, choices=("422", "444"))
    ap.add_argument("--src-fmt", required=True,
                    help="yuv422p10le / yuv444p10le / yuv444p12le")
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--label", required=True)
    ap.add_argument("--output", type=Path, default=None)
    args = ap.parse_args()

    W, H, N = args.width, args.height, args.frames
    bd = 12 if args.src_fmt.endswith("12le") else 10
    src = np.memmap(args.source, dtype="<u2", mode="r")
    codec = ToposCodec()
    tmp = Path(tempfile.mkdtemp(prefix="tierspeed_"))
    tiers = ("proxy", "lt", "standard", "hq") if args.family == "422" else (
        ("pro444",) if bd == 10 else ("pro444", "extreme"))
    print(f"[speed] {args.label}/{args.family} {args.src_fmt} {W}x{H} × {N} 帧；"
          f"档位 {'/'.join(tiers)}")

    result = dict(label=args.label, family=args.family, src_fmt=args.src_fmt,
                  width=W, height=H, frames=N, bit_depth=bd, rows=[])
    for tier in tiers:
        row = topos_tier_run(codec, src, W, H, N, tier, bd)
        ref = prores_ref(Path(args.source), args.src_fmt, W, H, N,
                         PRORES_ANCHOR[tier], tmp)
        row["prores_ref"] = ref
        row["speed_vs_prores"] = row["fps"] / ref["fps"]
        print(f"[speed] {args.label}/{tier}: {row['bpp']:.4f}bpp "
              f"({row['ratio']:.3f}×T, {row['mbps']:.1f}Mbps) "
              f"qp{row['qp0']}→均{row['qp_mean']:.1f} | 编码 {row['fps']:.1f}fps "
              f"({row['realtime']:.2f}×实时@25p) | 解码 {row['decode_fps']:.1f}fps "
              f"({row['decode_realtime']:.2f}×实时) | prores_ks {ref['fps']:.1f}fps "
              f"→ {row['speed_vs_prores']:.2f}×")
        result["rows"].append(row)

    out = args.output or (REPO / "native" / "topos_codec" / "tools"
                          / f"tierspeed_{args.label}_{args.family}_2026-09-11.json")
    out.write_text(json.dumps(result, indent=1))
    print(f"[speed] written {out}")


if __name__ == "__main__":
    main()
