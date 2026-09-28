#!/usr/bin/env python3
"""交错 rANS 可行性测量——符号流提取器（2026-09-11，速度轴）。

从真实 2K 素材提取逐 slice 的逻辑符号序列（DC_CAT/RUN/LEVEL_CAT +
后缀幅度 m），上下文推导镜像生产 rans2 默认族（lvl|前 lvl 桶 PREV、
dc|前块桶 PREV——串行最重的配置，吞吐口径保守）。

输出二进制（供 bench_rans_ilv.c）：
  u32 n_slices；每 slice：
    u32 n_sym
    n_sym × 记录 { u8 family(0=dc,1=run,2=lvl), u8 ctx, u16 sym, u16 m }

DCT/量化/token 镜像复用 bench_t16_ab.T16（numpy↔C 已逐位交叉校验）。
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import numpy as np

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from bench_t16_ab import T16, bitlen, load_raw, quant_int, rice_map  # noqa: E402


def prevlvl_ctx(cat: int, has_prev: bool) -> int:
    if not has_prev:
        return 4
    return 0 if cat == 0 else 1 if cat == 1 else 2 if cat <= 3 else 3


def dc_ctx(cat: int, has_prev: bool) -> int:
    if not has_prev:
        return 0
    return 1 + (0 if cat == 0 else 1 if cat == 1 else 2 if cat <= 3 else 3)


def plane_symbols(t: T16, plane: np.ndarray, qm: np.ndarray, qp: int,
                  slice_rows: int = 16):
    """plane: (h, w) u16 → 每 slice 的 (family, ctx, sym, m) 列表。"""
    h, w = plane.shape
    nby, nbx = h // 8, w // 8
    blocks = (plane.reshape(nby, 8, nbx, 8)
              .transpose(0, 2, 1, 3).reshape(-1, 8, 8).astype(np.int64) - 512)
    F = blocks @ t.M8 @ t.M8.T          # (nblk, 8, 8) 自然序
    Q = t.q8_table(qm, qp)
    q_nat = quant_int(F.reshape(-1, 64), Q).reshape(-1, 8, 8)  # 自然序
    q_zig = q_nat.reshape(-1, 64)[:, t.zz8]
    dc = q_zig[:, 0].reshape(nby, nbx)

    # DC 预测（slice 首行无 top；列 0 无 left）——镜像 tokens_cost
    left = np.zeros_like(dc)
    left[:, 1:] = dc[:, :-1]
    top = np.zeros_like(dc)
    top[1:] = dc[:-1]
    for y0 in range(0, nby, slice_rows):
        top[y0] = 0
    has_left = np.zeros(dc.shape, dtype=bool)
    has_left[:, 1:] = True
    has_top = np.zeros(dc.shape, dtype=bool)
    has_top[1:, :] = True
    for y0 in range(0, nby, slice_rows):
        has_top[y0] = False
    s = left + top
    pred = np.where(has_left & has_top,
                    np.where(s >= 0, (s + 1) >> 1, -((-s) >> 1)),
                    np.where(has_left, left, np.where(has_top, top, 0)))
    dc_diff = dc - pred
    dc_m = rice_map(dc_diff)
    dc_cat = bitlen(dc_m)

    slices = []
    for y0 in range(0, nby, slice_rows):
        recs = []
        prev_dc_cat = 0
        first_block = True
        for by in range(y0, min(y0 + slice_rows, nby)):
            for bx in range(nbx):
                cat = int(dc_cat[by, bx])
                ctx = 0 if first_block else dc_ctx(prev_dc_cat, True)
                recs.append((0, ctx, cat, int(dc_m[by, bx])))
                prev_dc_cat = cat
                first_block = False
                row = q_zig[by * nbx + bx]
                pos = 1
                prev_cat = 0
                has_prev = False
                nz = np.nonzero(row[1:] != 0)[0] + 1
                for p in nz.tolist():
                    run = p - pos
                    recs.append((1, 0, run, 0))
                    level = int(row[p])
                    m = int(rice_map(np.int64(level)))
                    lcat = int(bitlen(np.int64(m)))
                    ctx = prevlvl_ctx(prev_cat, has_prev)
                    recs.append((2, ctx, lcat, m))
                    prev_cat = lcat
                    has_prev = True
                    pos = p + 1
                recs.append((1, 0, 63, 0))  # EOB
        slices.append(recs)
    return slices


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixture", default="/tmp/gap4k/src2k.raw")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--frames", type=int, default=2)
    ap.add_argument("--qp", type=int, default=62)
    ap.add_argument("--qmatrix", type=int, default=0)
    ap.add_argument("--slice-rows", type=int, default=16)
    ap.add_argument("--output", default="/tmp/rans_ilv/syms.bin")
    args = ap.parse_args()

    t = T16()
    frames = load_raw(Path(args.fixture), args.width, args.height, args.frames)
    if not frames:
        raise SystemExit(f"fixture 不可用: {args.fixture}")
    qm = t.qmatrix(args.qmatrix, chroma=False)

    all_slices = []
    for fr in frames[:args.frames]:
        for p in range(3):
            plane = fr[p]
            qmp = qm if p == 0 else t.qmatrix(args.qmatrix, chroma=True)
            all_slices.extend(plane_symbols(t, plane, qmp, args.qp,
                                            args.slice_rows))
    n_sym = sum(len(s) for s in all_slices)
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "wb") as f:
        f.write(struct.pack("<I", len(all_slices)))
        for s in all_slices:
            f.write(struct.pack("<I", len(s)))
            for fam, ctx, sym, m in s:
                f.write(struct.pack("<BBHH", fam, ctx, sym, m))
    print(f"[ilv] {len(all_slices)} slices, {n_sym} symbols, qp={args.qp} "
          f"→ {out} ({out.stat().st_size/1e6:.1f} MB)")


if __name__ == "__main__":
    main()
