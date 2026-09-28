#!/usr/bin/env python3
"""C036 前置测量：rANS order-1 上下文建模天花板。

在生产编码遍内（dev 捕获钩子 tc_dev_ctx_*）按解码序因果上下文累积
条件联合直方图，逐 slice 以理想（未量化）模型计算符号位数，与现行
per-slice order-0 相减即纯上下文增益。零位流成本——这是格式扩展前的
决策测量，不改任何编码输出。

口径：
  - 比较基线 a0 = 逐 slice 精确 order-0 理想熵（与现行 121B 量化表相比
    略乐观，但两侧同口径，差值即上下文增益）；
  - 表开销 allowance = 每 slice 每多一个上下文模型多 (C−1)×|族| 字节
    （8-bit 粒度；4-bit 减半）——net 列已扣除；
  - 后缀位（suffix bits）与模型无关，不进差值；百分比同时给
    符号位口径与整 payload 口径（后者含表/后缀/CRC 的稀释）。

用法：
    TOPOS_CODEC_LIB=... python3 tools/bench_ctx_ceiling.py --frames 8 \
        --qps 20 32 44 56 68
"""

from __future__ import annotations

import argparse
import ctypes
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "topos_codec"
sys.path.insert(0, str(ROOT))

# 镜像 ctx_ceiling.h TC_CTX_ACC_*（勿单侧改布局）
ACC = {
    "slices": 0, "a0_dc": 1, "a0_run": 2, "a0_lvl": 3,
    "a1_R1": 4, "a1_R2": 5, "a1_R3": 6,
    "a1_L1": 7, "a1_L2": 8, "a1_L3": 9, "a1_L4": 10, "a1_D1": 11,
    "adapt_run": 12, "adapt_lvl": 13, "adapt_dc": 14,
    "sym_dc": 15, "sym_run": 16, "sym_lvl": 17, "eob": 18,
}
ACC_COUNT = 19

# 模型 → (族, 上下文数)；表开销 allowance 用
MODELS = {
    "R1": ("run", 8), "R2": ("run", 7), "R3": ("run", 4),
    "L1": ("lvl", 6), "L2": ("lvl", 5), "L3": ("lvl", 30), "L4": ("lvl", 4),
    "D1": ("dc", 5),
}
FAM_SYMS = {"dc": 29, "run": 64, "lvl": 28}
MODEL_DESC = {
    "R1": "run|前系数位置(8)", "R2": "run|前run(7)", "R3": "run|块DC类(4)",
    "L1": "lvl|同对run(6)", "L2": "lvl|前lvl(5)", "L3": "lvl|run×前lvl(30)",
    "L4": "lvl|系数位置(4)", "D1": "dc|前块DC类(5)",
}
RAW_FIXTURES = [
    ("/tmp/p5m/src1440.raw", 2560, 1440, "real_1440p_raw"),
    ("/tmp/p5m/src720.raw", 1280, 720, "real_720p_raw"),
]


def load_raw_frames(path: Path, w: int, h: int, n: int):
    fsz = (w * h + 2 * (w // 2) * h) * 2  # 422 10-bit LE u16
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


def load_prores_frames(path: Path, n: int):
    import av
    frames = []
    with av.open(str(path)) as cont:
        for i, frame in enumerate(cont.decode(video=0)):
            if i % 10 != 0:
                continue
            frames.append([
                np.frombuffer(bytes(frame.planes[k]), dtype=np.uint16)
                .reshape(frame.planes[k].height, frame.planes[k].width).copy()
                for k in range(3)
            ])
            if len(frames) >= n:
                break
    return frames


def frame_config(tb, width: int, height: int, qp: int):
    fc = tb._CFrameConfig()
    fc.struct_size = ctypes.sizeof(tb._CFrameConfig)
    fc.abi_version = tb.TOPOS_CODEC_ABI_VERSION
    fc.visible_width = width
    fc.visible_height = height
    fc.profile = 3
    fc.pixel_format = 0
    fc.bit_depth = 10
    fc.qmatrix_id = 0   # flat（ADR-C033 产品默认）
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
    fc.slice_rows = 16  # 产品默认
    fc.reserved[0] = 7  # rANS（V7-R）
    return fc


def run_one(codec, tb, name: str, frames, qp: int) -> dict:
    lib = codec._lib
    acc_buf = (ctypes.c_double * ACC_COUNT)()

    lib.tc_dev_ctx_reset()
    codec.symbol_hist_reset()
    lib.tc_dev_ctx_enable(1)
    codec.symbol_hist_enable(1)
    payload = headers = packet_bytes = 0
    slices = 0
    try:
        for planes in frames:
            _pkt, st = codec.encode_frame(
                frame_config(tb, int(planes[0].shape[1]), int(planes[0].shape[0]), qp),
                [p.tobytes() for p in planes])
            payload += st.color_payload_bytes
            headers += st.color_header_bytes
            packet_bytes += st.packet_size
            slices += st.slice_count
    finally:
        lib.tc_dev_ctx_enable(0)
        codec.symbol_hist_enable(0)

    lib.tc_dev_ctx_get(acc_buf)
    acc = {k: float(acc_buf[i]) for k, i in ACC.items()}
    dc, run, lvl = codec.symbol_hist_get()
    suffix_bits = sum(n * (s - 1) for s, n in enumerate(dc) if s > 1)
    suffix_bits += sum(n * (s - 1) for s, n in enumerate(lvl) if s > 1)

    a0 = {"dc": acc["a0_dc"], "run": acc["a0_run"], "lvl": acc["a0_lvl"]}
    sym_bits = sum(a0.values())
    payload_bits = payload * 8

    models = {}
    for m, (fam, ctx_n) in MODELS.items():
        delta = a0[fam] - acc[f"a1_{m}"]                      # 毛增益（符号位）
        allow8 = 8.0 * acc["slices"] * (ctx_n - 1) * FAM_SYMS[fam]
        allow4 = allow8 / 2.0
        models[m] = {
            "family": fam, "ctx": ctx_n,
            "gross_save_bits": round(delta, 1),
            "gross_pct_of_sym": round(100.0 * delta / sym_bits, 3) if sym_bits else 0.0,
            "net8_bits": round(delta - allow8, 1),
            "net8_pct_of_payload": round(100.0 * (delta - allow8) / payload_bits, 3),
            "net4_pct_of_payload": round(100.0 * (delta - allow4) / payload_bits, 3),
        }

    adapt = {}
    for fam, key in (("run", "adapt_run"), ("lvl", "adapt_lvl"), ("dc", "adapt_dc")):
        delta = a0[fam] - acc[key]
        cs = [c for f, c in MODELS.values() if f == fam]
        lo, hi = min(cs), max(cs)
        adapt[fam] = {
            "gross_save_bits": round(delta, 1),
            # 自适应逐 slice 可换模型：表开销给界（min/max ctx）
            "net8_pct_bounds": (
                round(100.0 * (delta - 8.0 * acc["slices"] * (hi - 1) * FAM_SYMS[fam])
                      / payload_bits, 3),
                round(100.0 * (delta - 8.0 * acc["slices"] * (lo - 1) * FAM_SYMS[fam])
                      / payload_bits, 3)),
        }

    return {
        "name": name, "qp": qp, "frames": len(frames),
        "slices": int(acc["slices"]),
        "symbols": {"dc": int(acc["sym_dc"]), "run": int(acc["sym_run"]),
                    "lvl": int(acc["sym_lvl"]), "eob": int(acc["eob"])},
        "bits": {
            "payload": payload_bits,
            "color_headers": headers * 8,
            "shipped_tables": int(acc["slices"]) * 121 * 8,
            "a0_order0_ideal_sym": round(sym_bits, 1),
            "suffix": suffix_bits,
            "shipped_sym_estimate": payload_bits - int(acc["slices"]) * 121 * 8,
        },
        "models": models, "adaptive": adapt,
        "acc_raw": acc,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=8)
    ap.add_argument("--qps", type=int, nargs="+", default=[20, 32, 44, 56, 68])
    ap.add_argument("--output", type=Path,
                    default=NATIVE / "tools" / "ctx_ceiling_2026-09-10.json")
    args = ap.parse_args()

    from src.shared.codec import topos_binding as tb

    codec = tb.ToposCodec()
    ver = codec.version()

    corpus = []
    for path, w, h, name in RAW_FIXTURES:
        p = Path(path)
        if p.is_file():
            frames = load_raw_frames(p, w, h, args.frames)
            if frames:
                corpus.append((name, frames))
    prores = ROOT / "tests" / "test_videos" / "2K_prores422HQ.mov"
    if prores.is_file():
        try:
            frames = load_prores_frames(prores, args.frames)
            if frames:
                corpus.append(("real_2k_prores422hq", frames))
        except ImportError:
            print("[ctx] PyAV 不可用，跳过 2K prores 语料")

    if not corpus:
        raise SystemExit("无语料：raw fixture 与 prores 均不可用")

    runs = []
    for name, frames in corpus:
        for qp in args.qps:
            r = run_one(codec, tb, name, frames, qp)
            runs.append(r)
            b = r["bits"]
            best = max(r["models"].items(), key=lambda kv: kv[1]["net8_pct_of_payload"])
            mn, best_net = best[0], best[1]["net8_pct_of_payload"]
            print(f"[ctx] {name} qp={qp}: sym={b['a0_order0_ideal_sym']/1e6:.2f}Mbit "
                  f"payload={b['payload']/1e6:.2f}Mbit slices={r['slices']} | "
                  f"最优 {mn} net8={best_net:+.3f}%payload")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({
        "tool": "bench_ctx_ceiling.py",
        "tool_version": 1,
        "lib_version": {"major": ver.major, "minor": ver.minor, "patch": ver.patch,
                        "git_commit": ver.git_commit},
        "corpus": [{"name": n, "frames": len(f),
                    "width": int(f[0][0].shape[1]), "height": int(f[0][0].shape[0])}
                   for n, f in corpus],
        "model_desc": MODEL_DESC,
        "runs": runs,
    }, ensure_ascii=False, indent=2) + "\n")
    print(f"[ctx] {len(runs)} runs → {args.output}")


if __name__ == "__main__":
    main()
