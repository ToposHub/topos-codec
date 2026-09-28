#!/usr/bin/env python3
"""P0 剖析批驱动（速度计划 M0.2，2026-09-11）：编码热点采样基座。

长循环产品码控编码（帧常驻内存剥离 IO，perf_counter 纯调用计时），
供 `sample <pid> <secs>` 符号级归因；每轮打印 fps 与首帧 sized 搜索 /
稳态 encode_frame 的耗时拆分（M0.2 的搜索成本问题）。

  --dump-packets DIR   末轮把稳态 qp 帧的包存 .tpc（供 prof_decode.c
                       采样解码，M0.3 用）。

非产品代码（dev 工具）。源：444 = DNxHR 444 解码、422 = prores422HQ
解码（ADR-C042 共享源口径）。
"""
from __future__ import annotations

import argparse
import ctypes
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from src.shared.codec.topos_binding import (  # noqa: E402
    TOPOS_CODEC_ABI_VERSION, ToposCodec, _CFrameConfig,
)
from src.shared.export.topos_rate_control import ToposRateFeedback  # noqa: E402
from src.shared.codec.topos_profiles import TOPOS_PROFILE_TIERS  # noqa: E402

QP_MAX = 95


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


def plane_dims(pixel_format: int, w: int, h: int):
    if pixel_format == 0:   # 4:2:2
        return [(h, w), (h, w // 2), (h, w // 2)]
    return [(h, w), (h, w), (h, w)]   # 4:4:4


def load_frames(path: Path, w: int, h: int, n: int, pf: int) -> list:
    """帧常驻内存：list[list[bytes]]（每帧每平面）。"""
    dims = plane_dims(pf, w, h)
    per_frame_px = sum(hh * ww for hh, ww in dims)
    src = np.memmap(path, dtype="<u2", mode="r")
    frames = []
    for i in range(n):
        off = i * per_frame_px
        fr, c = [], 0
        for hh, ww in dims:
            px = hh * ww
            fr.append(np.ascontiguousarray(
                src[off + c:off + c + px]).tobytes())
            c += px
        frames.append(fr)
    return frames


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixture", required=True)
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--frames", type=int, default=24)
    ap.add_argument("--tier", default="pro444",
                    choices=tuple(TOPOS_PROFILE_TIERS.keys()))
    ap.add_argument("--seconds", type=float, default=14.0,
                    help="总运行时长（供 sample 采样）")
    ap.add_argument("--dump-packets", default=None,
                    help="末轮稳态包输出目录（.tpc）")
    args = ap.parse_args()

    W, H, N = args.width, args.height, args.frames
    t = TOPOS_PROFILE_TIERS[args.tier]
    pf = 0 if args.tier in ("proxy", "lt", "standard", "hq") else 1
    bd = 10 if 10 in t.bit_depths else 12
    target = max(1, round(t.target_bpp * W * H / 8.0))

    codec = ToposCodec()
    frames = load_frames(Path(args.fixture), W, H, N, pf)
    if len(frames) < N:
        raise SystemExit(f"fixture 帧不足: {len(frames)} < {N}")
    print(f"[p0] {W}x{H} pf={pf} bd={bd} tier={args.tier} "
          f"target={target}B/帧，帧已常驻内存", flush=True)

    fc = make_fc(W, H, t.native_profile, pf, bd,
                 t.qmatrix_id, t.chroma_qp_offset)
    codec.encode_frame(fc, frames[0])          # 预热（库/线程池摊销）

    t_end = time.monotonic() + args.seconds
    rnd = 0
    last_pkts = None
    while time.monotonic() < t_end:
        rc = ToposRateFeedback(target, qp_max=QP_MAX)
        sized_secs = steady_secs = 0.0
        total = 0
        qps = []
        pkts = []
        t_r0 = time.monotonic()
        for i in range(N):
            t0 = time.perf_counter()
            if i == 0:
                pkt, st, qp_used = codec.encode_sized(
                    fc, frames[i], target, 0, QP_MAX)
                rc.seed(qp_used, len(pkt))
                sized_secs += time.perf_counter() - t0
                qps.append(qp_used)
            else:
                fc.qp_base = rc.qp
                pkt, st = codec.encode_frame(fc, frames[i])
                steady_secs += time.perf_counter() - t0
                qps.append(int(fc.qp_base))
                rc.note(len(pkt))
            total += len(pkt)
            pkts.append(pkt)
        wall = time.monotonic() - t_r0
        print(f"[p0] r{rnd}: {N / wall:7.1f} fps | sized {sized_secs * 1e3:7.1f} ms"
              f" | steady {(steady_secs / (N - 1)) * 1e3:7.1f} ms/帧"
              f" | qp {min(qps)}-{max(qps)} 均 {np.mean(qps):.1f}"
              f" | {total * 8.0 / (W * H * N):.3f} bpp", flush=True)
        last_pkts = pkts
        rnd += 1

    if args.dump_packets:
        out = Path(args.dump_packets)
        out.mkdir(parents=True, exist_ok=True)
        # 跳过首帧（sized 搜索帧），存稳态反馈帧
        for i, pkt in enumerate(last_pkts[1:], start=1):
            (out / f"{args.tier}_{W}x{H}_f{i:02d}.tpc").write_bytes(pkt)
        print(f"[p0] packets → {out} ({len(last_pkts) - 1} 帧)", flush=True)


if __name__ == "__main__":
    main()
