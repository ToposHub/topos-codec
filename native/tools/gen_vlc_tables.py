#!/usr/bin/env python3
"""gen_vlc_tables.py —— V2 canonical VLC 冻结码表生成器（M9，spec v2 §4.2）。

用法（native/topos_codec 目录下执行）：
    python3 tools/gen_vlc_tables.py collect   # 训练集 → 符号直方图（vlc_train_hists.json）
    python3 tools/gen_vlc_tables.py build     # 直方图 → package-merge → vlc_tables.h + manifest
    python3 tools/gen_vlc_tables.py check     # 复核已生成头文件与 manifest 一致、Kraft=1

设计约束（计划 §12.2 / ADR-C027 / spec v2）：
- 训练符号与编码器生产路径同源：native fill_color_band* 的 dev 钩子累计
  DC_CAT/RUN/LEVEL_CAT 频次（无独立复刻，杜绝统计漂移）；
- 训练集覆盖平场/渐变/纹理/噪声/动画/实拍 × 422/444/GBR × 10/12-bit，
  qp 分 4 档（12/24/36/{48,57}）→ 每族 4 本码书（book id = 档位）；
- package-merge 长度受限（≤20 bit）最优码长，canonical 指派码字（C 侧由
  长度表确定性重建，头文件只冻结长度）；
- 确定性：合成素材固定种子、tie-break 确定性、生成物无时间戳；
- 冻结：表一经发布永不改码字（ADR-C027 D-6），重训 = 新 codebook_version。
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]  # 仓库根（src/shared/… 可导入）
sys.path.insert(0, str(ROOT))

NATIVE = ROOT / "native" / "topos_codec"
HIST_PATH = NATIVE / "tools" / "vlc_train_hists.json"
MANIFEST_PATH = NATIVE / "tools" / "vlc_tables_manifest.json"
HEADER_PATH = NATIVE / "src" / "entropy" / "vlc_tables.h"

GENERATOR_VERSION = "vlc-tables-gen 1.0"

MAX_CODE_BITS = 20
BOOKS = 4
DC_SYMS = 29    # cat 0..28
RUN_SYMS = 64   # run 0..62 + EOB 63
LVL_SYMS = 28   # cat 0..27；索引 0 非法（len=0）
FAMILIES = ("dc", "run", "lvl")

# qp 档位（book id = 档位索引）；T4 汇聚 48+57 两个高 qp 点
TIERS = (("t1_qp12", (12,)), ("t2_qp24", (24,)), ("t3_qp36", (36,)),
         ("t4_qp48_57", (48, 57)))

SYN_W, SYN_H = 640, 360          # 8 的倍数；素材尺寸（块统计足够多样）
SYN_FRAMES = 6
SYN_CLASSES = ("flat", "gradient", "texture", "noise", "animation")
SYN_FORMATS = (                  # (pf, color_matrix, 说明)
    (0, 1, "yuv422"), (1, 1, "yuv444"), (2, 0, "gbr identity"))
SYN_DEPTHS = (10, 12)

REAL_SOURCES = (                 # 实拍（10-bit 422，PyAV 解码）
    ("real_2k", ROOT / "tests/test_videos/2K_prores422HQ.mov"),
    ("real_4k", ROOT / "tests/test_videos/4K_prores422HQ.mov"),
)
REAL_FRAMES = 16                 # 每 qp 档取样帧数（间隔 10 采样）


# ----------------------------------------------------------------------------
# 合成素材（确定性）
# ----------------------------------------------------------------------------

def syn_frames(cls: str, bd: int, n: int) -> "list[list]":
    import numpy as np

    maxv = (1 << bd) - 1
    rng = np.random.RandomState(hash(("vlc-train", cls, bd)) & 0xFFFFFFFF)
    w, h = SYN_W, SYN_H
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    half = (w + 1) // 2

    def plane(a: np.ndarray) -> np.ndarray:
        return np.clip(np.round(a), 0, maxv).astype(np.uint16)

    out = []
    for f in range(n):
        t = float(f)
        if cls == "flat":
            y = plane(np.full((h, w), maxv * (0.4 + 0.02 * f)))
            u = plane(np.full((h, half), maxv * 0.5))
            v = plane(np.full((h, half), maxv * 0.5))
        elif cls == "gradient":
            y = maxv * (0.5 + 0.35 * np.sin(xx / w * 6.0 + t * 0.3)
                        * np.cos(yy / h * 4.0))
            u = maxv * (0.5 + 0.2 * np.sin(xx / w * 3.0))[:, ::2]
            v = maxv * (0.5 + 0.2 * np.cos(yy / h * 3.0))[:, ::2]
            y, u, v = plane(y), plane(u), plane(v)
        elif cls == "texture":
            tex = rng.randn(h, w)
            for _ in range(3):                       # 低通 → 自然纹理
                tex = np.apply_along_axis(
                    lambda r: np.convolve(r, np.ones(5) / 5.0, mode="same"), 1, tex)
                tex = np.apply_along_axis(
                    lambda c: np.convolve(c, np.ones(5) / 5.0, mode="same"), 0, tex)
            sd = max(1e-9, tex.std())
            y = plane(maxv * (0.5 + 0.12 * tex / sd))
            u = plane(maxv * (0.5 + 0.05 * tex[:, ::2] / sd))
            v = plane(maxv * (0.5 - 0.05 * tex[:, ::2] / sd))
        elif cls == "noise":
            y = plane(maxv * 0.5 + rng.randn(h, w) * (maxv * 0.06))
            u = plane(maxv * 0.5 + rng.randn(h, half) * (maxv * 0.02))
            v = plane(maxv * 0.5 + rng.randn(h, half) * (maxv * 0.02))
        elif cls == "animation":
            y = maxv * (0.3 + 0.2 * yy / h)          # 渐变背景
            x0 = int((w / 4) * (0.5 + 0.4 * np.sin(t * 0.7)))
            y0 = int((h / 4) * (0.5 + 0.4 * np.cos(t * 0.5)))
            y[y0:y0 + h // 5, x0:x0 + w // 4] = maxv * 0.9   # 移动硬边方块
            u = np.full((h, half), maxv * 0.5)
            v = np.full((h, half), maxv * 0.5)
            u[y0:y0 + h // 5, x0 // 2:(x0 + w // 4) // 2] = maxv * 0.3
            v[y0:y0 + h // 5, x0 // 2:(x0 + w // 4) // 2] = maxv * 0.7
            y, u, v = plane(y), plane(u), plane(v)
        else:
            raise AssertionError(cls)
        out.append([y, u, v])
    return out


def real_frames(path: Path, n: int) -> "list[list]":
    """PyAV 解码 ProRes 素材（10-bit 422），间隔 10 取前 n 帧。"""
    import av
    import numpy as np

    c = av.open(str(path))
    st = next(s for s in c.streams if s.type == "video")
    got, out = 0, []
    for i, frame in enumerate(c.decode(st)):
        if i % 10 != 0:
            continue
        planes = [np.frombuffer(bytes(frame.planes[k]), dtype=np.uint16)
                  .reshape(frame.planes[k].height, frame.planes[k].width)
                  .copy() for k in range(3)]
        out.append(planes)
        got += 1
        if got >= n:
            break
    c.close()
    return out


# ----------------------------------------------------------------------------
# collect：训练编码 → 直方图
# ----------------------------------------------------------------------------

def _frame_cfg(codec, w: int, h: int, pf: int, bd: int, matrix: int, qp: int):
    import ctypes

    import src.shared.codec.topos_binding as _tb
    from src.shared.codec.topos_binding import TOPOS_CODEC_ABI_VERSION

    fc = _tb._CFrameConfig()
    fc.struct_size = ctypes.sizeof(_tb._CFrameConfig)
    fc.abi_version = TOPOS_CODEC_ABI_VERSION
    fc.visible_width = w
    fc.visible_height = h
    fc.profile = 3
    fc.pixel_format = pf
    fc.bit_depth = bd
    fc.qmatrix_id = 1
    fc.qp_base = qp
    fc.alpha_mode = 0
    fc.alpha_bit_depth = 0
    fc.alpha_premultiplied = 0
    fc.color_range = 1
    fc.color_primaries = 1
    fc.color_transfer = 1
    fc.color_matrix = matrix
    fc.chroma_siting = 0
    fc.sar_num, fc.sar_den = 1, 1
    return fc


def cmd_collect() -> dict:
    from src.shared.codec.topos_binding import ToposCodec

    codec = ToposCodec()
    ver = codec.version()
    print(f"[collect] libtopos_codec {ver.major}.{ver.minor}.{ver.patch}"
          f" commit={ver.git_commit!r}")

    real_cache = {}
    for name, path in REAL_SOURCES:
        if not path.exists():
            print(f"[collect] 跳过缺失素材 {path}")
            continue
        real_cache[name] = real_frames(path, REAL_FRAMES)
        print(f"[collect] {name}: {len(real_cache[name])} 帧 {path.name}")

    runs = []

    def run(label: str, pf: int, matrix: int, bd: int, qp: int, frames) -> None:
        if not frames:
            return
        fc = _frame_cfg(codec, frames[0][0].shape[1], frames[0][0].shape[0],
                        pf, bd, matrix, qp)
        codec.symbol_hist_reset()
        codec.symbol_hist_enable(True)
        try:
            for planes in frames:
                codec.encode_frame(fc, [p.tobytes() for p in planes])
        finally:
            codec.symbol_hist_enable(False)
        dc, run_h, lvl = codec.symbol_hist_get()
        runs.append({
            "label": label, "pf": pf, "bd": bd, "qp": qp,
            "frames": len(frames), "dc": dc, "run": run_h, "lvl": lvl,
        })
        print(f"[collect] {label} qp={qp}: {sum(dc)} DC / {sum(run_h)} RUN /"
              f" {sum(lvl)} LVL 符号")

    for pf, matrix, fmt_name in SYN_FORMATS:
        for bd in SYN_DEPTHS:
            for cls in SYN_CLASSES:
                frames = syn_frames(cls, bd, SYN_FRAMES)
                for _, qps in TIERS:
                    for qp in qps:
                        run(f"syn_{cls}_{fmt_name}_bd{bd}", pf, matrix, bd,
                            qp, frames)
    for name, frames in real_cache.items():
        for _, qps in TIERS:
            for qp in qps:
                run(name, 0, 1, 10, qp, frames)

    tiers = {}
    for tier_name, qps in TIERS:
        qset = set(qps)
        pooled = {f: [0] * n for f, n in
                  (("dc", DC_SYMS), ("run", RUN_SYMS), ("lvl", LVL_SYMS))}
        for r in runs:
            if r["qp"] in qset:
                for f in FAMILIES:
                    for i, v in enumerate(r[f]):
                        pooled[f][i] += v
        tiers[tier_name] = pooled

    data = {
        "generator": GENERATOR_VERSION,
        "lib_commit": ver.git_commit,
        "corpus": {
            "synthetic": {
                "classes": list(SYN_CLASSES),
                "formats": [f"{n}(pf{pf})" for pf, _, n in SYN_FORMATS],
                "depths": list(SYN_DEPTHS), "frames_per_run": SYN_FRAMES,
                "seed": "RandomState(hash(('vlc-train', cls, bd)) & 0xFFFFFFFF)",
            },
            "real": [
                {"name": n, "path": str(p.relative_to(ROOT)),
                 "sha256": _sha256_file(p)} for n, p in REAL_SOURCES if p.exists()
            ],
            "real_frames_per_run": REAL_FRAMES,
        },
        "tiers": {t: list(qs) for t, qs in TIERS},
        "runs": runs,
        "pooled": tiers,
    }
    HIST_PATH.write_text(json.dumps(data))
    print(f"[collect] {len(runs)} runs → {HIST_PATH.name}")
    return data


# ----------------------------------------------------------------------------
# package-merge（长度受限最优前缀码）+ canonical 指派
# ----------------------------------------------------------------------------

def package_merge_lengths(weights: "list[int]", max_len: int) -> "list[int]":
    """Larmore–Hirschberg package-merge（维基归约口径）：min Σ w·l s.t.
    l ≤ max_len 且 Kraft 取等。

    归约：每符号 max_len 枚硬币（面额 2^-1..2^-L，numismatic value = w）。
    从最小面额列表起逐层「相邻配对打包（落单的最高值硬币丢弃）→ 与下一档
    面额的新币归并排序」；max_len 轮后得到 1-dollar 列表（仅包、无新币），
    取最便宜 n−1 项，逐层展开成硬币多重集——符号 i 被数到的硬币数即码长。
    tie-break 确定（同值时叶子先于包；同层按结构键）→ 输出确定。

    注意：包内同一符号可出现多次（不同面额硬币被包在一起），计数必须按
    多重集——不得去重。
    """
    n = len(weights)
    if n < 2:
        raise ValueError("alphabet >= 2")
    if max_len < (n - 1).bit_length():
        raise ValueError(f"max_len {max_len} < ceil(log2 {n})")

    fresh = [(w, (0, i), {i: 1}) for i, w in enumerate(weights)]
    fresh_sorted = sorted(fresh, key=lambda t: (t[0], t[1]))
    level = fresh_sorted                      # 面额 2^-L 列表
    for step in range(max_len):               # 逐层上升到 1-dollar
        pkgs = []
        for a, b in zip(level[0::2], level[1::2]):
            ms = dict(a[2])
            for sym, c in b[2].items():      # 多重集相加（不去重）
                ms[sym] = ms.get(sym, 0) + c
            pkgs.append((a[0] + b[0], (1, (a[1], b[1])), ms))
        if step < max_len - 1:
            level = sorted(pkgs + fresh,
                           key=lambda t: (t[0], t[1]))  # 下一档：包 + 新币
        else:
            level = pkgs                                   # 1-dollar：仅包
    if len(level) < n - 1:
        raise AssertionError("1-dollar 列表不足 n-1 项")
    counts: dict = {}
    for _, _, ms in level[: n - 1]:
        for sym, c in ms.items():
            counts[sym] = counts.get(sym, 0) + c
    return [counts.get(i, 0) for i in range(n)]


def canonical_codes(lengths: "list[int]") -> "list[int]":
    """canonical 指派：按 (len, symbol) 升序分配连续码字；len=0 → 无码（0）。"""
    order = sorted((l, s) for s, l in enumerate(lengths) if l > 0)
    codes = [0] * len(lengths)
    code, prev = 0, order[0][0]
    for l, s in order:
        code <<= (l - prev)
        codes[s] = code
        code += 1
        prev = l
    return codes


def kraft_ok(lengths: "list[int]", max_len: int) -> bool:
    total = sum(1 << (max_len - l) for l in lengths if l > 0)
    return total == (1 << max_len) and all(0 <= l <= max_len for l in lengths)


def _suffix_bits(family: str, sym: int) -> int:
    return (sym - 1) if family in ("dc", "lvl") and sym > 0 else 0


def _entropy_bits(weights: "list[int]") -> float:
    import math
    total = sum(weights)
    return -sum(w * math.log2(w / total) for w in weights if w > 0)


def _cat_mid_m(cat: int) -> int:
    """cat 区间内均匀假设的代表 m（Rice 诊断基线用；VLC 位数为精确值）。"""
    if cat == 0:
        return 0
    lo = 1 << (cat - 1)
    hi = min(1 << cat, 1 << 27)
    return (lo + hi - 1) // 2


def _rice_sym_bits(m: int, k: int) -> int:
    q = m >> k
    return q + 1 + k if q <= 30 else 63


# ----------------------------------------------------------------------------
# build：直方图 → 码长 → vlc_tables.h + manifest
# ----------------------------------------------------------------------------

def _alphabet(family: str) -> "list[int]":
    if family == "dc":
        return list(range(DC_SYMS))          # cat 0..28 全合法
    if family == "run":
        return list(range(RUN_SYMS))         # 0..63 全合法
    return list(range(1, LVL_SYMS))          # cat 1..27；cat 0 非法


def cmd_build(hists: dict) -> None:
    tables = {f: [] for f in FAMILIES}
    proofs = {}
    for tier_name, _ in TIERS:
        pooled = hists["pooled"][tier_name]
        for family in FAMILIES:
            syms = _alphabet(family)
            hist = pooled[family]
            weights = [hist[s] + 1 for s in syms]     # Laplace：全符号入码
            lens = package_merge_lengths(weights, MAX_CODE_BITS)
            if not kraft_ok(lens, MAX_CODE_BITS):
                raise AssertionError(f"Kraft != 1: {family}/{tier_name}")
            row = [0] * len(hist)
            for s, l in zip(syms, lens):
                row[s] = l
            tables[family].append(row)

            total_w = sum(weights)
            vlc_bits = sum(w * (l + _suffix_bits(family, s))
                           for w, s, l in zip(weights, syms, lens))
            # Rice 基线（同分布、最优 k；cat 代表值近似）
            if family == "run":
                rice_bits = min(
                    sum(w * _rice_sym_bits(s, k) for w, s in zip(weights, syms))
                    for k in range(15))
            else:
                rice_bits = min(
                    sum(w * _rice_sym_bits(_cat_mid_m(s), k)
                        for w, s in zip(weights, syms))
                    for k in range(15))
            proofs[f"{family}/{tier_name}"] = {
                "symbols": len(syms),
                "max_len": max(lens),
                "entropy_bits_per_sym": round(_entropy_bits(weights) / total_w, 4),
                "vlc_bits_per_sym": round(vlc_bits / total_w, 4),
                "rice_best_bits_per_sym": round(rice_bits / total_w, 4),
            }

    manifest = {
        "generator": GENERATOR_VERSION,
        "codebook_version": 1,
        "max_code_bits": MAX_CODE_BITS,
        "books": BOOKS,
        "tiers": [t for t, _ in TIERS],
        "corpus": hists["corpus"],
        "lib_commit_at_collect": hists.get("lib_commit"),
        "tables_sha256": {},
        "proofs": proofs,
    }
    for family in FAMILIES:
        blob = json.dumps(tables[family]).encode()
        manifest["tables_sha256"][family] = hashlib.sha256(blob).hexdigest()

    HEADER_PATH.write_text(_render_header(tables, manifest))
    MANIFEST_PATH.write_text(json.dumps(manifest, indent=1))
    print(f"[build] {HEADER_PATH.name} + {MANIFEST_PATH.name}")
    for key, d in proofs.items():
        print(f"[build] {key}: vlc {d['vlc_bits_per_sym']} bit/sym"
              f"（H={d['entropy_bits_per_sym']}，Rice≈{d['rice_best_bits_per_sym']}）")


def _render_header(tables: dict, manifest: dict) -> str:
    msha = hashlib.sha256(
        json.dumps({k: manifest[k] for k in
                    ("generator", "codebook_version", "max_code_bits", "books",
                     "tiers", "corpus", "tables_sha256")},
                   sort_keys=True).encode()).hexdigest()

    def fmt_rows(rows: "list[list[int]]", per: int) -> str:
        out = []
        for row in rows:
            vals = [f"{v:2d}" for v in row]
            lines = [", ".join(vals[i:i + per]) for i in range(0, len(vals), per)]
            out.append("    { " + ",\n      ".join(lines) + " }")
        return "{\n" + ",\n".join(out) + "\n}"

    return f"""/* V2 canonical VLC 冻结码长表（M9，spec v2 §4.2；ADR-C027 D-6）。
 *
 * 由 tools/gen_vlc_tables.py 生成——冻结表，永不改码字；重训 = 递增
 * codebook_version。码字由 vlc.c 的 tc_vlc_book_init 从长度表 canonical
 * 指派（确定性：(len, symbol) 升序连续分配），头文件只冻结长度。
 *
 * book id = qp 档位（0=qp12 / 1=qp24 / 2=qp36 / 3=qp48+57）；编码端逐族
 * 选最小 total_bits（含后缀位），同分取最小 book id（spec v2 §4.2）。
 * LEVEL 的 cat=0 不在字母表（len=0）——解码即 MALFORMED（spec v2 §4.1）。
 *
 * manifest: tools/vlc_tables_manifest.json
 * manifest_sha256: {msha}
 */
#ifndef TOPOS_INTERNAL_VLC_TABLES_H
#define TOPOS_INTERNAL_VLC_TABLES_H

#include <stdint.h>

#define TC_VLC_CODEBOOK_VERSION 1
#define TC_VLC_MAX_CODE_BITS 20
#define TC_VLC_BOOKS 4
#define TC_VLC_DC_SYMS 29
#define TC_VLC_RUN_SYMS 64
#define TC_VLC_LVL_SYMS 28

static const uint8_t tc_vlc_dc_len[TC_VLC_BOOKS][TC_VLC_DC_SYMS] =
{fmt_rows(tables['dc'], 16)};

static const uint8_t tc_vlc_run_len[TC_VLC_BOOKS][TC_VLC_RUN_SYMS] =
{fmt_rows(tables['run'], 16)};

static const uint8_t tc_vlc_lvl_len[TC_VLC_BOOKS][TC_VLC_LVL_SYMS] =
{fmt_rows(tables['lvl'], 16)};

#endif /* TOPOS_INTERNAL_VLC_TABLES_H */
"""


# ----------------------------------------------------------------------------
# check：复核生成物
# ----------------------------------------------------------------------------

def cmd_check() -> None:
    text = HEADER_PATH.read_text()
    manifest = json.loads(MANIFEST_PATH.read_text())

    def parse_table(name: str, rows: int, cols: int) -> "list[list[int]]":
        m = re.search(re.escape(name) + r"(?:\[[^]]*\])+\s*=\s*\{(.*?)\};", text,
                      re.S)
        if not m:
            raise AssertionError(f"{name} 未找到")
        vals = [int(v) for v in re.findall(r"\d+", m.group(1))]
        if len(vals) != rows * cols:
            raise AssertionError(f"{name}: {len(vals)} != {rows * cols}")
        return [vals[i * cols:(i + 1) * cols] for i in range(rows)]

    got = {
        "dc": parse_table("tc_vlc_dc_len", BOOKS, DC_SYMS),
        "run": parse_table("tc_vlc_run_len", BOOKS, RUN_SYMS),
        "lvl": parse_table("tc_vlc_lvl_len", BOOKS, LVL_SYMS),
    }
    for family in FAMILIES:
        blob = json.dumps(got[family]).encode()
        sha = hashlib.sha256(blob).hexdigest()
        if sha != manifest["tables_sha256"][family]:
            raise AssertionError(f"{family} sha 漂移")
        for b, row in enumerate(got[family]):
            if not kraft_ok(row, MAX_CODE_BITS):
                raise AssertionError(f"{family} book{b} Kraft != 1")
            canonical_codes(row)  # 可指派性
    print("[check] vlc_tables.h ↔ manifest 一致；4×3 本码书 Kraft 全部取等")


# ----------------------------------------------------------------------------

def _sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> None:
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    data = None
    if mode in ("collect", "all"):
        data = cmd_collect()
    if mode in ("build", "all"):
        if data is None:
            data = json.loads(HIST_PATH.read_text())
        cmd_build(data)
    if mode == "check":
        cmd_check()


if __name__ == "__main__":
    main()
