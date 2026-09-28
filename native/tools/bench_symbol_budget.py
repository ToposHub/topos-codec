#!/usr/bin/env python3
"""C0：冻结量化系数的符号预算测量。

统计直接来自编码器的 symbol_hist 钩子，并同时记录实际 packet 的
payload/header 开销。它不会改动量化系数、码表或默认编码策略；输出中的
``vlc_pooled_book_bits`` 是把汇总直方图交给单一本现有码书时的精确位数
估计（实际编码按 slice 选书，slice/padding/header 开销仍以 packet 统计为准）。

用法：
    TOPOS_CODEC_LIB=... python3 tools/bench_symbol_budget.py --frames 4
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import re
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "topos_codec"
sys.path.insert(0, str(ROOT))

DC_SYMS = 29
RUN_SYMS = 64
LVL_SYMS = 28
FAMILIES = ("dc", "run", "lvl")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_vlc_lengths() -> dict[str, list[list[int]]]:
    text = (NATIVE / "src" / "entropy" / "vlc_tables.h").read_text()
    specs = {
        "dc": ("tc_vlc_dc_len", 4, DC_SYMS),
        "run": ("tc_vlc_run_len", 4, RUN_SYMS),
        "lvl": ("tc_vlc_lvl_len", 4, LVL_SYMS),
    }
    out: dict[str, list[list[int]]] = {}
    for family, (name, rows, cols) in specs.items():
        m = re.search(rf"{name}[^=]*=\s*\{{(.*?)\}};", text, re.S)
        if m is None:
            raise RuntimeError(f"missing {name}")
        vals = [int(v) for v in re.findall(r"\d+", m.group(1))]
        if len(vals) != rows * cols:
            raise RuntimeError(f"{name}: {len(vals)} != {rows * cols}")
        out[family] = [vals[i * cols : (i + 1) * cols] for i in range(rows)]
    return out


def make_frames(kind: str, width: int, height: int, n: int, seed: int) -> list[list[np.ndarray]]:
    maxv = 1023
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float64)
    half = (width + 1) // 2
    rng = np.random.default_rng(seed)
    frames: list[list[np.ndarray]] = []
    for i in range(n):
        if kind == "flat":
            y = np.full((height, width), maxv * (0.35 + 0.03 * i))
            u = np.full((height, half), maxv * 0.5)
            v = np.full((height, half), maxv * 0.5)
        elif kind == "gradient":
            y = maxv * (0.5 + 0.3 * np.sin(xx / width * 5.0 + i * 0.2)
                        * np.cos(yy / height * 3.0))
            u = maxv * (0.5 + 0.15 * np.sin(np.arange(half)[None, :] / half * 4.0))
            u = np.repeat(u, height, axis=0)
            v = maxv * (0.5 + 0.15 * np.cos(yy / height * 4.0))[:, :half]
        elif kind == "texture":
            base = rng.normal(0.0, 1.0, (height, width))
            blur = (base + np.roll(base, 1, 0) + np.roll(base, -1, 0)
                    + np.roll(base, 1, 1) + np.roll(base, -1, 1)) / 5.0
            y = maxv * (0.5 + 0.12 * blur / max(float(blur.std()), 1e-9))
            u = maxv * (0.5 + 0.05 * blur[:, :half] / max(float(blur.std()), 1e-9))
            v = maxv * (0.5 - 0.05 * blur[:, :half] / max(float(blur.std()), 1e-9))
        elif kind == "noise":
            y = maxv * 0.5 + rng.normal(0.0, maxv * 0.06, (height, width))
            u = maxv * 0.5 + rng.normal(0.0, maxv * 0.02, (height, half))
            v = maxv * 0.5 + rng.normal(0.0, maxv * 0.02, (height, half))
        else:
            raise ValueError(kind)
        frames.append([
            np.clip(np.rint(a), 0, maxv).astype(np.uint16)
            for a in (y, u, v)
        ])
    return frames


def frame_config(tb, width: int, height: int, qp: int, entropy: int):
    fc = tb._CFrameConfig()
    fc.struct_size = ctypes.sizeof(tb._CFrameConfig)
    fc.abi_version = tb.TOPOS_CODEC_ABI_VERSION
    fc.visible_width = width
    fc.visible_height = height
    fc.profile = 3
    fc.pixel_format = 0
    fc.bit_depth = 10
    fc.qmatrix_id = 1
    fc.qp_base = qp
    fc.alpha_mode = 0
    fc.alpha_bit_depth = 0
    fc.color_range = 1
    fc.color_primaries = 1
    fc.color_transfer = 1
    fc.color_matrix = 1
    fc.chroma_siting = 0
    fc.sar_num = 1
    fc.sar_den = 1
    fc.reserved[0] = entropy  # 0=V1 Rice，1=V2 canonical VLC
    return fc


def bits_for_hist(hist: list[int], lengths: list[list[int]], family: str) -> tuple[int, int]:
    suffix = family in ("dc", "lvl")
    candidates = []
    for row in lengths:
        total = 0
        for sym, count in enumerate(hist):
            if count == 0 or row[sym] == 0:
                continue
            total += count * (row[sym] + (sym - 1 if suffix and sym > 1 else 0))
        candidates.append(total)
    best = min(candidates)
    return best, candidates.index(best)


def entropy_bits(hist: list[int]) -> float:
    total = sum(hist)
    if total == 0:
        return 0.0
    return -sum(n * np.log2(n / total) for n in hist if n)


def summarize_hist(dc: list[int], run: list[int], lvl: list[int], lengths: dict) -> dict:
    bits = {}
    books = {}
    for family, hist in (("dc", dc), ("run", run), ("lvl", lvl)):
        bits[family], books[family] = bits_for_hist(hist, lengths[family], family)
    run_total = sum(run)
    eob = run[63]
    return {
        "dc_symbols": sum(dc),
        "ac_pairs": run_total - eob,
        "run_symbols_including_eob": run_total,
        "eob_symbols": eob,
        "level_symbols": sum(lvl),
        "vlc_pooled_book_bits": sum(bits.values()),
        "vlc_pooled_book_bits_per_token": (
            sum(bits.values()) / (sum(dc) + run_total + sum(lvl))
            if sum(dc) + run_total + sum(lvl) else 0.0
        ),
        "ideal_entropy_bits": round(entropy_bits(dc) + entropy_bits(run) + entropy_bits(lvl), 3),
        "vlc_books_at_pooled_hist": books,
        "hist": {"dc": dc, "run": run, "lvl": lvl},
    }


def run_one(codec, tb, name: str, frames: list[list[np.ndarray]], qp: int,
            entropy: int, lengths: dict) -> dict:
    if not frames:
        raise ValueError("empty frame set")
    h, w = frames[0][0].shape
    cfg = frame_config(tb, w, h, qp, entropy)
    codec.symbol_hist_reset()
    codec.symbol_hist_enable(True)
    packet_bytes = color_payload = color_headers = slices = 0
    try:
        for planes in frames:
            _pkt, st = codec.encode_frame(cfg, [p.tobytes() for p in planes])
            packet_bytes += st.packet_size
            color_payload += st.color_payload_bytes
            color_headers += st.color_header_bytes
            slices += st.slice_count
    finally:
        codec.symbol_hist_enable(False)
    dc, run, lvl = codec.symbol_hist_get()
    planes_out = []
    for plane in range(3):
        pdc, prun, plvl = codec.symbol_hist_plane_get(plane)
        ps = summarize_hist(pdc, prun, plvl, lengths)
        ps.pop("hist")
        ps["plane"] = plane
        planes_out.append(ps)
    out = summarize_hist(dc, run, lvl, lengths)
    out.update({
        "name": name,
        "qp": qp,
        "entropy_mode": entropy,
        "entropy_name": "v2_vlc" if entropy == 1 else "v1_rice",
        "frames": len(frames),
        "width": w,
        "height": h,
        "packet_bytes": packet_bytes,
        "color_payload_bytes": color_payload,
        "color_header_bytes": color_headers,
        "non_color_overhead_bytes": packet_bytes - color_payload,
        "slice_count": slices,
        "planes": planes_out,
    })
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=4)
    ap.add_argument("--qps", type=int, nargs="+", default=[24, 48])
    ap.add_argument("--output", type=Path,
                    default=NATIVE / "tools" / "c0_symbol_budget_2026-09-06.json")
    args = ap.parse_args()

    from src.shared.codec import topos_binding as tb

    codec = tb.ToposCodec()
    ver = codec.version()
    lengths = load_vlc_lengths()
    corpus: list[tuple[str, list[list[np.ndarray]]]] = []
    for i, kind in enumerate(("flat", "gradient", "texture", "noise")):
        corpus.append((f"synthetic_{kind}_640x360", make_frames(kind, 640, 360,
                                                                  args.frames, 0xC000 + i)))

    real_path = ROOT / "tests" / "test_videos" / "2K_prores422HQ.mov"
    if real_path.is_file():
        import av
        real: list[list[np.ndarray]] = []
        with av.open(str(real_path)) as cont:
            for i, frame in enumerate(cont.decode(video=0)):
                if i % 10 != 0:
                    continue
                real.append([
                    np.frombuffer(bytes(frame.planes[k]), dtype=np.uint16)
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
                result = run_one(codec, tb, name, frames, qp, entropy, lengths)
                runs.append(result)
                print(f"[c0] {name} {result['entropy_name']} qp={qp}: "
                      f"{result['packet_bytes']} B, {result['dc_symbols']} DC, "
                      f"{result['ac_pairs']} AC pairs, {result['eob_symbols']} EOB")

    data = {
        "tool": "bench_symbol_budget.py",
        "tool_version": 1,
        "lib_version": {"major": ver.major, "minor": ver.minor, "patch": ver.patch,
                        "git_commit": ver.git_commit},
        "vlc_tables_sha256": sha256_file(NATIVE / "src" / "entropy" / "vlc_tables.h"),
        "corpus": [{"name": n, "frames": len(f), "width": int(f[0][0].shape[1]),
                    "height": int(f[0][0].shape[0])} for n, f in corpus],
        "runs": runs,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
    print(f"[c0] {len(runs)} runs → {args.output}")


if __name__ == "__main__":
    main()
