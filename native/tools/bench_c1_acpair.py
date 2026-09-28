#!/usr/bin/env python3
"""C1：精确 token 流的 (run, level-category) 联合码原型。

这是离线预算工具，不改变 Topos 位流。它用编码器同源的联合直方图构造
两种 Huffman 估计：seen-only（乐观、只覆盖观测到的 pair）和 full-Laplace
（完整 64×28 字母表 + EOB）。level 的符号/幅度后缀仍按现有规则保留，
因此候选只减少一次 run/level 类别码字，不改变任何量化系数或重建结果。
"""

from __future__ import annotations

import argparse
import heapq
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE_TOOLS = ROOT / "native" / "topos_codec" / "tools"
sys.path.insert(0, str(NATIVE_TOOLS))
sys.path.insert(0, str(ROOT))

import bench_symbol_budget as c0

PAIR_RUNS = 64
PAIR_CATS = 28
PAIR_SYMS = PAIR_RUNS * PAIR_CATS
EOB_SYM = PAIR_SYMS


def huffman_lengths(weights: list[int]) -> list[int]:
    """确定性 Huffman 长度；堆键含 symbol id，避免并列结果漂移。"""
    heap: list[tuple[int, int, tuple[int, ...]]] = []
    for sym, weight in enumerate(weights):
        if weight > 0:
            heapq.heappush(heap, (weight, sym, (sym,)))
    if len(heap) < 2:
        return [1 if w else 0 for w in weights]
    lengths = [0] * len(weights)
    serial = len(weights)
    while len(heap) > 1:
        wa, _ka, a = heapq.heappop(heap)
        wb, _kb, b = heapq.heappop(heap)
        for sym in a:
            lengths[sym] += 1
        for sym in b:
            lengths[sym] += 1
        merged = a + b
        heapq.heappush(heap, (wa + wb, serial, merged))
        serial += 1
    return lengths


def pair_bits(pair_hist: list[int], eob: int, mode: str) -> tuple[int, int, int]:
    if len(pair_hist) != PAIR_SYMS:
        raise ValueError("pair histogram size")
    if mode == "seen-only":
        weights = [n for n in pair_hist] + [eob]
    elif mode == "full-laplace":
        weights = [n + 1 for n in pair_hist] + [eob + 1]
    else:
        raise ValueError(mode)
    lens = huffman_lengths(weights)
    bits = lens[EOB_SYM] * eob
    suffix = 0
    pairs = 0
    for sym, count in enumerate(pair_hist):
        if count == 0:
            continue
        run = sym // PAIR_CATS
        cat = sym % PAIR_CATS
        bits += count * lens[sym]
        # 现有 LEVEL_CAT 的幅度后缀：cat>1 时为 cat-1 bit。
        suffix += count * (cat - 1 if cat > 1 else 0)
        pairs += count
    return bits + suffix, len([x for x in lens if x]), pairs


def current_pooled_bits(run: dict, lengths: dict) -> int:
    bits = 0
    for family in ("dc", "run", "lvl"):
        h = run["hist"][family]
        b, _book = c0.bits_for_hist(h, lengths[family], family)
        bits += b
    return bits


def enrich(run: dict, pair_hists: list[list[int]], lengths: dict) -> dict:
    dc, run_h, lvl = run["hist"]["dc"], run["hist"]["run"], run["hist"]["lvl"]
    current_bits = current_pooled_bits(run, lengths)
    dc_bits, _ = c0.bits_for_hist(dc, lengths["dc"], "dc")
    pair_total = sum(x for x in run_h[:63])
    rows = []
    total_pair_bits = {"seen-only": 0, "full-laplace": 0}
    total_symbols = {"seen-only": 0, "full-laplace": 0}
    for plane, ph in enumerate(pair_hists):
        if isinstance(ph, dict):
            pair = ph["pair"]
            p_run = ph["run"]
            peob = p_run[63]
        else:
            pair = ph
            peob = 0
        item = {"plane": plane, "ac_pairs": sum(pair)}
        for mode in ("seen-only", "full-laplace"):
            b, alphabet, pairs = pair_bits(pair, peob, mode)
            total_pair_bits[mode] += b
            total_symbols[mode] += alphabet
            item[mode] = {"pair_bits": b, "alphabet_symbols": alphabet,
                          "table_bytes_1byte_lengths": alphabet}
        rows.append(item)
    # pooled current 表示：DC + run + level；候选：DC + 联合 pair + EOB。
    # pooled pair 位数按每平面求和，避免丢失 run/category 相关性。
    candidates = {}
    for mode in ("seen-only", "full-laplace"):
        candidate_bits = dc_bits + total_pair_bits[mode]
        # 当前 pooled 估计与实际 packet 的差值是 slice 选书/填充误差，单独列出。
        padding_gap = max(0, run["color_payload_bytes"] * 8 - current_bits)
        table_bytes = sum(x[mode]["table_bytes_1byte_lengths"] for x in rows)
        est_payload_bits = candidate_bits + padding_gap + table_bytes * 8
        candidates[mode] = {
            "candidate_bits_before_table": candidate_bits,
            "estimated_payload_bytes_with_table": math.ceil(est_payload_bits / 8),
            "estimated_total_bytes_with_table": (
                math.ceil(est_payload_bits / 8)
                + run["packet_bytes"] - run["color_payload_bytes"]
            ),
            "joint_alphabet_symbols": total_symbols[mode],
            "table_bytes_1byte_lengths": table_bytes,
            "saved_ac_codewords_per_frame": pair_total,
            "current_entropy_codewords": run["dc_symbols"]
            + sum(run_h) + sum(lvl),
            "candidate_entropy_codewords": run["dc_symbols"] + pair_total
            + run["eob_symbols"],
        }
        candidates[mode]["estimated_total_ratio"] = (
            candidates[mode]["estimated_total_bytes_with_table"]
            / run["packet_bytes"]
        )
        candidates[mode]["estimated_savings_ratio"] = 1.0 - candidates[mode]["estimated_total_ratio"]
    return {
        "name": run["name"], "qp": run["qp"], "entropy_mode": run["entropy_mode"],
        "entropy_name": run["entropy_name"], "frames": run["frames"],
        "current_packet_bytes": run["packet_bytes"],
        "current_color_payload_bytes": run["color_payload_bytes"],
        "dc_symbols": run["dc_symbols"], "ac_pairs": run["ac_pairs"],
        "eob_symbols": run["eob_symbols"],
        "current_pooled_vlc_bits": current_bits,
        "pair_planes": rows, "candidates": candidates,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=2)
    ap.add_argument("--qps", type=int, nargs="+", default=[24, 48])
    ap.add_argument("--output", type=Path,
                    default=NATIVE_TOOLS / "c1_acpair_budget_2026-09-06.json")
    args = ap.parse_args()

    from src.shared.codec import topos_binding as tb

    codec = tb.ToposCodec()
    lengths = c0.load_vlc_lengths()
    corpus = []
    for i, kind in enumerate(("flat", "gradient", "texture", "noise")):
        corpus.append((f"synthetic_{kind}_640x360",
                       c0.make_frames(kind, 640, 360, args.frames, 0xC100 + i)))
    real_path = ROOT / "tests" / "test_videos" / "2K_prores422HQ.mov"
    if real_path.is_file():
        import av
        real = []
        with av.open(str(real_path)) as cont:
            for i, frame in enumerate(cont.decode(video=0)):
                if i % 10:
                    continue
                real.append([
                    __import__("numpy").frombuffer(bytes(frame.planes[k]), dtype="uint16")
                    .reshape(frame.planes[k].height, frame.planes[k].width).copy()
                    for k in range(3)
                ])
                if len(real) >= args.frames:
                    break
        if real:
            corpus.append(("real_2k_prores422hq", real))

    runs = []
    for name, frames in corpus:
        for qp in args.qps:
            for entropy in (0, 1):
                r = c0.run_one(codec, tb, name, frames, qp, entropy, lengths)
                per_plane = []
                for plane in range(3):
                    _dc, p_run, _lvl = codec.symbol_hist_plane_get(plane)
                    per_plane.append({"run": p_run,
                                     "pair": codec.symbol_pair_hist_get(plane)})
                # run_one 保留了完整 hist；重新从总 API 读取，确保与联合
                # 直方图在同一 reset/encode 窗口内。
                dc, all_run, lvl = codec.symbol_hist_get()
                r["hist"] = {"dc": dc, "run": all_run, "lvl": lvl}
                runs.append(enrich(r, per_plane, lengths))

    data = {
        "tool": "bench_c1_acpair.py", "tool_version": 1,
        "alphabet": {"pair_symbols": PAIR_SYMS, "eob_symbol": EOB_SYM,
                      "run_values": PAIR_RUNS, "level_categories": PAIR_CATS},
        "runs": runs,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
    for r in runs:
        if r["entropy_mode"] == 1 and r["name"].startswith("real"):
            print(r["name"], "qp", r["qp"], r["candidates"])
    print(f"[c1] {len(runs)} runs → {args.output}")


if __name__ == "__main__":
    main()
