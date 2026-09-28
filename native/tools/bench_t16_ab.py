#!/usr/bin/env python3
"""ADR-C037 试点测量：8×8（现行）vs 混合 16×16 vs 浮点 DCT-16 参照。

零位流成本的三方 RD 对比——回答两个问题：
  1) 16×16 方向在同画质下能省多少码率（A8 vs C16f = 方向上限）；
  2) 精确整数正交的混合构造能拿到其中多少（A8 vs B16 = 可实施收益）。

口径（与 bench_ctx_ceiling.py 同源）：
  - 符号成本 = 逐 slice 精确 order-0 理想熵 + suffix 位（(cat−1)/符号）；
    表成本 = 每 slice |字母表| 字节（8×8: 121B = 29+64+28 与现行一致；
    16×16: 313B = 29+256+28，run 字母表扩到 256）；
  - token 语义逐位镜像 fill_color_band_tokens（DC 中值预测、每 slice 首
    行 has_top=0、zigzag 非零 (run,lvl) 对、EOB 计入 run 族、bitlen 类别）；
  - 量化逐位镜像 tc_quant_block（mag+Q/2、AC 死区 Q/4 整除）；
    16×16 的 Q16 = Q8[u>>1][v>>1]·√(E16uE16v)/√(E8uE8v) —— 像素域量化
    步逐系数对齐，RD 差异纯粹来自变换形状；
  - 整数变换用 numpy 镜像 C 语义（int64 全整数无溢出域），启动时与
    lib 的 C 实现逐位交叉校验通过后才会跑测量；
  - 锚点：A8 路径 bits vs 真实编码器（binding, flat, rans2）payload。

用法：
    TOPOS_CODEC_LIB=build/topos_codec.dylib python3 tools/bench_t16_ab.py \
        --qps 12 20 28 36 44 52 60 68 --frames 2
"""
from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "topos_codec"

# scan.h 冻结的 8×8 zigzag（交叉校验本工具的 zigzag 生成器）
K_ZZ8 = [
    0, 1, 8, 16, 9, 2, 3, 10,
    17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
]
BD = 10
MID = 1 << (BD - 1)
PEAK = (1 << BD) - 1
SLICE_ROWS = 16  # 产品默认（8×8 块行/slice；16×16 路径取半，同像素条带）

I16P = ctypes.POINTER(ctypes.c_int16)
I32P = ctypes.POINTER(ctypes.c_int32)


def zigzag(n: int) -> np.ndarray:
    order = []
    for d in range(2 * n - 1):
        ys = range(max(0, d - n + 1), min(d, n - 1) + 1)
        if d % 2 == 0:
            ys = reversed(list(ys))
        for y in ys:
            order.append(y * n + (d - y))
    return np.array(order, dtype=np.int64)


def load_lib() -> ctypes.CDLL:
    path = os.environ.get("TOPOS_CODEC_LIB", str(NATIVE / "build" / "topos_codec.dylib"))
    lib = ctypes.CDLL(path)
    lib.tc_qmatrix_by_id.argtypes = [ctypes.c_uint8]
    lib.tc_qmatrix_by_id.restype = ctypes.c_void_p
    lib.tc_quant_step.argtypes = [ctypes.c_uint16, ctypes.c_uint32]
    lib.tc_quant_step.restype = ctypes.c_uint32
    lib.tc_transform_matrix.restype = ctypes.c_void_p
    lib.tc_transform_weights.restype = ctypes.c_void_p
    lib.tc_transform_energies.restype = ctypes.c_void_p
    lib.tc_transform16_matrix.restype = ctypes.c_void_p
    lib.tc_transform16_weights.restype = ctypes.c_void_p
    lib.tc_transform16_energies.restype = ctypes.c_void_p
    lib.tc_transform_forward_8x8.argtypes = [I16P, I32P]
    lib.tc_transform_inverse_8x8.argtypes = [I32P, I32P]
    lib.tc_transform16_forward.argtypes = [I16P, I32P]
    lib.tc_transform16_inverse.argtypes = [I32P, I32P]
    return lib


def c_arr(lib, sym, ctype, n):
    return np.ctypeslib.as_array(
        ctypes.cast(getattr(lib, sym)(), ctypes.POINTER(ctype)), shape=(n,)).copy()


def rice_map(v: np.ndarray) -> np.ndarray:
    v = v.astype(np.int64)
    return np.where(v >= 0, v << 1, ((-v) << 1) - 1)


def bitlen(m: np.ndarray) -> np.ndarray:
    m = m.astype(np.int64)
    out = np.zeros(m.shape, dtype=np.int64)
    nz = m > 0
    out[nz] = np.frexp(m[nz].astype(np.float64))[1]
    return out


def round_shift32(v: np.ndarray) -> np.ndarray:
    """符号拆分 + 四舍五入（镜像 tc_round_shift32）。"""
    v = v.astype(np.int64)
    return np.where(v >= 0, (v + (1 << 31)) >> 32, -((-v + (1 << 31)) >> 32))


def dct16_orthonormal() -> np.ndarray:
    n = 16
    d = np.zeros((n, n))
    for u in range(n):
        a = math.sqrt(1.0 / n) if u == 0 else math.sqrt(2.0 / n)
        d[u] = [a * math.cos((2 * x + 1) * u * math.pi / (2 * n)) for x in range(n)]
    return d


class T16:
    def __init__(self) -> None:
        self.lib = load_lib()
        self.M8 = c_arr(self.lib, "tc_transform_matrix", ctypes.c_int16, 64).reshape(8, 8).astype(np.int64)
        self.W8 = c_arr(self.lib, "tc_transform_weights", ctypes.c_uint32, 64).reshape(8, 8).astype(np.int64)
        self.E8 = c_arr(self.lib, "tc_transform_energies", ctypes.c_uint32, 8).astype(np.int64)
        self.M16 = c_arr(self.lib, "tc_transform16_matrix", ctypes.c_int16, 256).reshape(16, 16).astype(np.int64)
        self.W16 = c_arr(self.lib, "tc_transform16_weights", ctypes.c_uint32, 256).reshape(16, 16).astype(np.int64)
        self.E16 = c_arr(self.lib, "tc_transform16_energies", ctypes.c_uint32, 16).astype(np.int64)
        self.zz8 = zigzag(8)
        self.zz16 = zigzag(16)
        assert self.zz8.tolist() == K_ZZ8, "zigzag(8) 与 scan.h 冻结表不一致"
        self.D16 = dct16_orthonormal()
        self._crosscheck()

    def _crosscheck(self) -> None:
        """numpy 整数镜像 vs C 实现逐位一致才继续（各 32 随机块）。"""
        rng = np.random.default_rng(7)
        for _ in range(32):
            x8 = rng.integers(-2047, 2048, size=(8, 8), dtype=np.int64)
            F = self.M8 @ x8 @ self.M8.T
            cF = np.zeros(64, dtype=np.int32)
            cx = np.ascontiguousarray(x8, dtype=np.int16)
            self.lib.tc_transform_forward_8x8(cx.ctypes.data_as(I16P), cF.ctypes.data_as(I32P))
            assert np.array_equal(F.ravel(), cF.astype(np.int64)), "fwd8 numpy != C"
            coef = np.clip(F, -(1 << 25), 1 << 25)
            G = coef * self.W8
            xh = round_shift32(self.M8.T @ (G @ self.M8))
            cX = np.zeros(64, dtype=np.int32)
            cc = np.ascontiguousarray(coef, dtype=np.int32)
            self.lib.tc_transform_inverse_8x8(cc.ctypes.data_as(I32P), cX.ctypes.data_as(I32P))
            assert np.array_equal(xh.ravel(), cX.astype(np.int64)), "inv8 numpy != C"

            x16 = rng.integers(-2047, 2048, size=(16, 16), dtype=np.int64)
            F16 = self.M16 @ x16 @ self.M16.T
            cF16 = np.zeros(256, dtype=np.int32)
            cx16 = np.ascontiguousarray(x16, dtype=np.int16)
            self.lib.tc_transform16_forward(cx16.ctypes.data_as(I16P), cF16.ctypes.data_as(I32P))
            assert np.array_equal(F16.ravel(), cF16.astype(np.int64)), "fwd16 numpy != C"
            coef16 = np.clip(F16, -(1 << 27), 1 << 27)
            G16 = coef16 * self.W16
            xh16 = round_shift32(self.M16.T @ (G16 @ self.M16))
            cX16 = np.zeros(256, dtype=np.int32)
            cc16 = np.ascontiguousarray(coef16, dtype=np.int32)
            self.lib.tc_transform16_inverse(cc16.ctypes.data_as(I32P), cX16.ctypes.data_as(I32P))
            assert np.array_equal(xh16.ravel(), cX16.astype(np.int64)), "inv16 numpy != C"
        print("[t16] numpy↔C 交叉校验：fwd/inv 8×8+16×16 各 32 块逐位一致")

    def qmatrix(self, qmid: int, chroma: bool) -> np.ndarray:
        ptr = self.lib.tc_qmatrix_by_id(qmid)
        vals = np.ctypeslib.as_array(
            ctypes.cast(ptr, ctypes.POINTER(ctypes.c_uint16)), shape=(128,)).copy()
        return (vals[64:] if chroma else vals[:64]).astype(np.int64).reshape(8, 8)

    def q8_table(self, qm: np.ndarray, qp: int) -> np.ndarray:
        q = [int(self.lib.tc_quant_step(int(v), qp)) for v in qm.ravel()]
        return np.array(q, dtype=np.int64).reshape(8, 8)

    def q16_table(self, qm: np.ndarray, qp: int) -> np.ndarray:
        """Q16[u][v] = Q8[u>>1][v>>1]·√(E16u·E16v)/√(E8u8·E8v8)。"""
        q8 = self.q8_table(qm, qp)
        idx = np.arange(16) >> 1
        q8_16 = q8[idx][:, idx]
        e16 = (self.E16[:, None] * self.E16[None, :]).astype(np.float64)
        e8 = (self.E8[idx][:, None] * self.E8[idx][None, :]).astype(np.float64)
        scale = np.sqrt(e16 / e8)
        q16 = np.rint(q8_16 * scale).astype(np.int64)
        return np.maximum(q16, 1)


def quant_int(F: np.ndarray, Q: np.ndarray) -> np.ndarray:
    """逐位镜像 tc_quant_block（F[..., i] 对应自然序 i）。"""
    Qf = Q.ravel()
    mag = np.abs(F)
    num = mag + (Qf >> 1)
    dz = np.where(np.arange(Qf.size) == 0, 0, Qf >> 2)
    num_eff = np.where(num > dz, num - dz, 0)
    qmag = num_eff // Qf
    return np.where(F < 0, -qmag, qmag).astype(np.int64)


def dequant_int(q: np.ndarray, Q: np.ndarray, clamp: int) -> np.ndarray:
    return np.clip(q * Q.ravel(), -clamp, clamp)


def tokens_cost(q_nat: np.ndarray, zz: np.ndarray, slice_rows: int,
                eob: int, alpha_dc: int, alpha_run: int, alpha_lvl: int):
    """q_nat: (nby, nbx, n²) 自然序量化索引。返回 (bits, n_slices)。"""
    nby, nbx, _ = q_nat.shape
    q_zig = q_nat[..., zz]
    dc = q_zig[..., 0]

    # DC 预测（raster 序；has_left = 列>0，has_top = 行>0 且非 slice 首行）
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
    s = left.astype(np.int64) + top.astype(np.int64)
    pred_both = np.where(s >= 0, (s + 1) >> 1, -((-s) >> 1))
    pred = np.where(has_left & has_top, pred_both,
                    np.where(has_left, left, np.where(has_top, top, 0)))
    dc_cat = bitlen(rice_map(dc - pred))

    slice_of_row = np.arange(nby) // slice_rows
    n_slices = int(slice_of_row[-1]) + 1
    sr_flat = slice_of_row.repeat(nbx)

    dc_hist = np.bincount(sr_flat * alpha_dc + dc_cat.ravel(),
                          minlength=n_slices * alpha_dc).reshape(n_slices, alpha_dc)

    # AC 对：非零扫描位置 → (run, lvl)
    ac = q_zig[..., 1:]
    byr, bxc, sp = np.nonzero(ac != 0)
    p = sp + 1
    if p.size > 0:
        blk = byr * nbx + bxc
        same = np.zeros(p.shape, dtype=bool)
        same[1:] = blk[1:] == blk[:-1]
        prev_p = np.where(same, np.concatenate(([0], p[:-1])), 0)
        run = p - prev_p - 1
        lvl_cat = bitlen(rice_map(ac[byr, bxc, sp]))
        sl = slice_of_row[byr]
    else:
        run = np.zeros(0, dtype=np.int64)
        lvl_cat = np.zeros(0, dtype=np.int64)
        sl = np.zeros(0, dtype=np.int64)

    run_hist = np.bincount(sl * alpha_run + run,
                           minlength=n_slices * alpha_run).reshape(n_slices, alpha_run)
    eob_per_slice = np.bincount(sr_flat, minlength=n_slices)
    run_hist[:, eob] += eob_per_slice
    lvl_hist = np.bincount(sl * alpha_lvl + lvl_cat,
                           minlength=n_slices * alpha_lvl).reshape(n_slices, alpha_lvl)

    assert dc_cat.max() < alpha_dc, f"dc cat {dc_cat.max()} 超字母表"
    assert lvl_cat.size == 0 or lvl_cat.max() < alpha_lvl, f"lvl cat 超字母表"
    assert run.size == 0 or run.max() < eob, f"run {run.max()} 超域"

    def fam_bits(hist):
        n = hist.sum(axis=1).astype(np.float64)
        c = hist.astype(np.float64)
        logc = np.where(c > 0, c * np.log2(np.where(c > 0, c, 1.0)), 0.0).sum(axis=1)
        ent = np.zeros(hist.shape[0])
        m = n > 0
        ent[m] = n[m] * np.log2(n[m]) - logc[m]
        suf = (hist * (np.arange(hist.shape[1])[None, :] - 1.0).clip(min=0)).sum()
        return ent.sum(), suf

    dc_sym, dc_suf = fam_bits(dc_hist)
    run_sym, _ = fam_bits(run_hist)
    lvl_sym, lvl_suf = fam_bits(lvl_hist)
    table_bits = n_slices * (alpha_dc + alpha_run + alpha_lvl) * 8.0
    bits = dc_sym + dc_suf + run_sym + lvl_sym + lvl_suf + table_bits
    return bits, n_slices


def pad_plane(plane: np.ndarray, blk: int):
    h, w = plane.shape
    ph = ((h + blk - 1) // blk) * blk
    pw = ((w + blk - 1) // blk) * blk
    padded = np.pad(plane.astype(np.int64), ((0, ph - h), (0, pw - w)), mode="edge")
    return padded, ph, pw


def blockify(padded: np.ndarray, ph: int, pw: int, blk: int):
    nby, nbx = ph // blk, pw // blk
    return padded.reshape(nby, blk, nbx, blk).swapaxes(1, 2).reshape(nby, nbx, blk, blk), nby, nbx


def unblockify(xh, nby, nbx, blk):
    return xh.reshape(nby, nbx, blk, blk).swapaxes(1, 2).reshape(nby * blk, nbx * blk)


def measure_frame(t: T16, planes, qp: int, qmid: int, path: str):
    """返回 (bits, se_sum, npix_sum, se_y)。"""
    bits = 0.0
    se_sum = 0.0
    npix_sum = 0
    se_y = 0.0
    for pi, src in enumerate(planes):
        q8 = t.q8_table(t.qmatrix(qmid, pi > 0), qp)
        if path == "A8":
            blk = 8
            padded, ph, pw = pad_plane(src, blk)
            B, nby, nbx = blockify(padded, ph, pw, blk)
            F = t.M8 @ (B - MID) @ t.M8.T
            q = quant_int(F.reshape(nby, nbx, 64), q8)
            G = dequant_int(q, q8, 1 << 25).reshape(nby, nbx, 8, 8) * t.W8
            xh = round_shift32(t.M8.T @ (G @ t.M8)) + MID
            b, _ = tokens_cost(q, t.zz8, SLICE_ROWS, 63, 29, 64, 28)
        elif path == "B16":
            blk = 16
            q16 = t.q16_table(t.qmatrix(qmid, pi > 0), qp)
            padded, ph, pw = pad_plane(src, blk)
            B, nby, nbx = blockify(padded, ph, pw, blk)
            F = t.M16 @ (B - MID) @ t.M16.T
            q = quant_int(F.reshape(nby, nbx, 256), q16)
            G = dequant_int(q, q16, 1 << 27).reshape(nby, nbx, 16, 16) * t.W16
            xh = round_shift32(t.M16.T @ (G @ t.M16)) + MID
            b, _ = tokens_cost(q, t.zz16, SLICE_ROWS // 2, 255, 29, 256, 28)
        elif path == "C16f":
            blk = 16
            q16 = t.q16_table(t.qmatrix(qmid, pi > 0), qp)
            step = q16.astype(np.float64) / np.sqrt(
                t.E16.astype(np.float64)[:, None] * t.E16.astype(np.float64)[None, :])
            dz = step / 4.0
            dz[0, 0] = 0.0
            padded, ph, pw = pad_plane(src, blk)
            B, nby, nbx = blockify(padded, ph, pw, blk)
            x = (B - MID).astype(np.float64)
            ncoef = t.D16 @ x @ t.D16.T
            # 逐位镜像整数量化语义：q = floor((|n| + step/2 − step/4)/step)
            # （有效死区 0.75·step，与 tc_quant_block 一致——勿再叠加 +0.5）
            num_eff = np.abs(ncoef) + step / 4.0
            qf = np.floor(num_eff / step)
            q = np.where(ncoef < 0, -qf, qf).reshape(nby, nbx, 256).astype(np.int64)
            nrec = q.astype(np.float64).reshape(nby, nbx, 16, 16) * step
            xh = np.rint(t.D16.T @ nrec @ t.D16).astype(np.int64) + MID
            b, _ = tokens_cost(q, t.zz16, SLICE_ROWS // 2, 255, 29, 256, 28)
        else:
            raise ValueError(path)

        rec = np.clip(unblockify(xh, nby, nbx, blk), 0, PEAK)
        vis = rec[:src.shape[0], :src.shape[1]].astype(np.float64) - src.astype(np.float64)
        se = float((vis * vis).sum())
        se_sum += se
        npix_sum += vis.size
        if pi == 0:
            se_y = se
        bits += b
    return bits, se_sum, npix_sum, se_y


def rd_ratios(points_a, points_p):
    """分域对比：
    - 无损平台（哨兵 200）：同 qp 双方皆无损 → 直接 bits 比；
    - 有损段：在 A 的真实 PSNR 点插值 P 的 bits（线性 bits×PSNR），
      哨兵点不参与插值（平台内不同 qp 不可互插）。"""
    qa, bits_a, psnr_a = points_a[0], points_a[1], points_a[2]
    qp_p, bits_p, psnr_p = points_p[0], points_p[1], points_p[2]
    lossless = []
    for i, q in enumerate(qa):
        if psnr_a[i] >= 200.0 and q in qp_p:
            j = qp_p.index(q)
            if psnr_p[j] >= 200.0:
                lossless.append((int(q), float(bits_p[j] / bits_a[i])))
    m = (np.asarray(psnr_p) < 200.0)
    xs = np.asarray(psnr_p)[m]
    ys = np.asarray(bits_p)[m]
    order = np.argsort(xs)
    xs, ys = xs[order], ys[order]
    matched = []
    for q, ab, ap in zip(qa, bits_a, psnr_a):
        if ap >= 200.0 or xs.size < 2 or not (xs[0] <= ap <= xs[-1]):
            continue
        matched.append((int(q), float(ap), float(np.interp(ap, xs, ys) / ab)))
    return matched, lossless


def anchor_check(t: T16, frames, qp: int, entropy: int = 8) -> float:
    sys.path.insert(0, str(ROOT))
    from src.shared.codec import topos_binding as tb

    codec = tb.ToposCodec()
    h, w = frames[0][0].shape
    fc = tb._CFrameConfig()
    fc.struct_size = ctypes.sizeof(tb._CFrameConfig)
    fc.abi_version = tb.TOPOS_CODEC_ABI_VERSION
    fc.visible_width = w
    fc.visible_height = h
    fc.profile = 3
    fc.pixel_format = 0
    fc.bit_depth = BD
    fc.qmatrix_id = 0
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
    fc.slice_rows = SLICE_ROWS
    fc.reserved[0] = entropy  # 7 = rans(order-0) / 8 = rans2(order-1)
    payload = 0
    for planes in frames:
        _pkt, st = codec.encode_frame(fc, [p.tobytes() for p in planes])
        payload += st.color_payload_bytes
    bits_a = 0.0
    for planes in frames:
        b, _, _, _ = measure_frame(t, planes, qp, 0, "A8")
        bits_a += b
    return payload * 8.0 / bits_a


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


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=2)
    ap.add_argument("--qps", type=int, nargs="+",
                    default=[12, 20, 28, 36, 44, 52, 60, 68, 80])
    ap.add_argument("--fixture", default="/tmp/p5m/src1080.raw")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--matrices", nargs="+", default=["flat", "standard"],
                    choices=["flat", "standard"])
    ap.add_argument("--anchor", action=argparse.BooleanOptionalAction, default=True)
    ap.add_argument("--output", type=Path,
                    default=NATIVE / "tools" / "t16_pilot_2026-09-11.json")
    args = ap.parse_args()

    frames = load_raw(Path(args.fixture), args.width, args.height, args.frames)
    if not frames:
        raise SystemExit(f"fixture 不可用: {args.fixture}")
    print(f"[t16] fixture: {args.fixture} {args.width}x{args.height} × {len(frames)} 帧")
    t = T16()

    anchors = []
    if args.anchor:
        # flat qp≤44 实测像素级无损（量化误差低于重建舍入粒度）——
        # 锚点取无损域(28)与有损域(60)各一；rans(7) 对齐 order-0 理想口径，
        # rans2(8) 的差值即上下文增益（ADR-C036 实测 −1.2~−2.8%）。
        for qp in (28, 60):
            for ent, name in ((7, "rans"), (8, "rans2")):
                r = anchor_check(t, frames, qp, ent)
                anchors.append({"qp": qp, "entropy": name,
                                "encoder_payload_over_ideal": r})
                print(f"[t16] 锚点 qp={qp} {name}: 编码器 payload / A8 理想成本 = "
                      f"{r:.4f}（rans 应 ~1.00±0.03；rans2 约 0.97-0.99）")

    qmids = {"flat": 0, "standard": 1}
    runs = []
    for mname in args.matrices:
        qmid = qmids[mname]
        pts = {}
        for path in ("A8", "B16", "C16f"):
            qps, bits_l, psnr_l, psnr_all_l = [], [], [], []
            for qp in args.qps:
                bits = 0.0
                se_sum = 0.0
                npix = 0
                se_y = 0.0
                npix_y = 0
                for planes in frames:
                    b, se, npx, sey = measure_frame(t, planes, qp, qmid, path)
                    bits += b
                    se_sum += se
                    npix += npx
                    se_y += sey
                    npix_y += frames[0][0].size
                qps.append(qp)
                bits_l.append(bits)
                mse_y = se_y / npix_y
                mse_all = se_sum / npix
                # 无损平台（mse=0）以 200.0 哨兵表示——高于任何真实 PSNR，
                # 平台内不同 qp 是不同工作点（无损但子舍入损失不同），
                # 不可跨点插值，只在同 qp 直接比（见 rd_ratios 分域）。
                psnr_l.append(200.0 if mse_y == 0 else 10.0 * math.log10(PEAK * PEAK / mse_y))
                psnr_all_l.append(200.0 if mse_all == 0
                                  else 10.0 * math.log10(PEAK * PEAK / mse_all))
            pts[path] = (qps, np.array(bits_l, dtype=float), np.array(psnr_l),
                         psnr_all_l)
            print(f"[t16] {mname} {path}: " + " ".join(
                f"qp{q}={b/8192:.0f}KB/{p:.2f}dB"
                for q, b, p in zip(qps, bits_l, psnr_l)))

        for other in ("B16", "C16f"):
            matched, lossless = rd_ratios(pts["A8"], pts[other])
            if lossless:
                geo_l = math.exp(sum(math.log(r) for _, r in lossless) / len(lossless))
                print(f"[t16] {mname} A8→{other} 无损域（同 qp 皆无损）: "
                      f"码率比 {geo_l:.4f}（{lossless}）")
            if matched:
                geo = math.exp(sum(math.log(r) for _, _, r in matched) / len(matched))
                lo = min(r for _, _, r in matched)
                hi = max(r for _, _, r in matched)
                print(f"[t16] {mname} A8→{other} 有损域匹配画质: 码率比 "
                      f"几何均值 {geo:.4f}（范围 {lo:.4f}..{hi:.4f}，{len(matched)} 点："
                      + " ".join(f"qp{q}@{p:.1f}dB={r:.3f}" for q, p, r in matched) + "）")
                runs.append({"matrix": mname, "pair": f"A8_vs_{other}",
                             "lossless_same_qp": lossless,
                             "lossless_geo": geo_l if lossless else None,
                             "matched_psnr": matched, "geo": geo, "min": lo, "max": hi})
            elif lossless:
                runs.append({"matrix": mname, "pair": f"A8_vs_{other}",
                             "lossless_same_qp": lossless,
                             "lossless_geo": geo_l})
        runs.append({"matrix": mname, "curves": {
            p: {"qp": pts[p][0], "bits": pts[p][1].tolist(),
                "psnr_y": pts[p][2].tolist(), "psnr_all": pts[p][3]}
            for p in pts}})

    args.output.write_text(json.dumps({
        "tool": "bench_t16_ab.py", "tool_version": 1,
        "fixture": {"path": args.fixture, "w": args.width, "h": args.height,
                    "frames": len(frames)},
        "qps": args.qps, "slice_rows": SLICE_ROWS, "anchors": anchors,
        "cost_model": "per-slice ideal order-0 + suffix + table bytes "
                      "(8x8: 121B, 16x16: 313B per slice)",
        "runs": runs,
    }, ensure_ascii=False, indent=2) + "\n")
    print(f"[t16] {len(runs)} runs → {args.output}")


if __name__ == "__main__":
    main()
