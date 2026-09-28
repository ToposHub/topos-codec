#!/usr/bin/env python3
"""4K vs ProRes 质量差距分解（2026-09-11，当前默认口径 flat+rans2）。

P5 报告的 4K 差距（−0.9~−2.7 dB）是档位落点口径（Topos 落 0.79~0.85×
ProRes 实际码率）；码率对齐口径的最后测量停在 V1 Rice 时代。本工具在
当前产品默认（qm=flat、entropy=rans2、v1.5 qp 域、产品反馈码控）下：

  1. 码率对齐差距：Topos sized 到 ProRes 各 profile 实测总码率，
     逐平面 PSNR + 422 加权合成；
  2. 频率域误差分解：源-重建误差的 8×8 DCT 能量按 zigzag 带累计
     （DC / 低 / 中 / 中高 / 高），Topos vs ProRes 逐带能量比——
     差距住在颗粒（高频）还是结构（低频）；
  3. 档位落点对照：同帧以 P5 tier bpp 出流，分离"落点 0.8×"贡献；
  4. 2K 对照组（该口径下追平/反超）——频率域分解的赢/输对比定位
     内容依赖性（4K 素材高纹理）。

共享源：raw yuv422p10le（两编码器同输入；PSNR 各对同一源）。
ProRes = ffmpeg prores_ks profile 0/1/2/3（Proxy/LT/Standard/HQ）。
"""
from __future__ import annotations

import argparse
import ctypes
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from src.shared.codec.topos_binding import (  # noqa: E402
    TOPOS_CODEC_ABI_VERSION, ToposCodec, _CFrameConfig,
)
from src.shared.export.topos_rate_control import ToposRateFeedback  # noqa: E402

QP_MAX = 95
PROFILES = (("proxy", 0), ("lt", 1), ("standard", 2), ("hq", 3))
TIERS = {"proxy": 0.32310, "lt": 1.13412, "standard": 1.64528, "hq": 2.66245}
# 8×8 zigzag 带边界（扫描位）：DC=0；低 1-3；中 4-10；中高 11-21；高 22-63
BANDS = ((0, 1, "dc"), (1, 4, "low"), (4, 11, "mid"), (11, 22, "midhigh"),
         (22, 64, "high"))


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
            frames.append([
                np.frombuffer(b[:yh * 2], dtype="<u2").reshape(h, w).copy(),
                np.frombuffer(b[yh * 2:(yh + ch) * 2], dtype="<u2").reshape(h, w // 2).copy(),
                np.frombuffer(b[(yh + ch) * 2:], dtype="<u2").reshape(h, w // 2).copy(),
            ])
    return frames


def make_fc(w: int, h: int, qp: int = 20) -> _CFrameConfig:
    fc = _CFrameConfig()
    fc.struct_size = ctypes.sizeof(_CFrameConfig)
    fc.abi_version = TOPOS_CODEC_ABI_VERSION
    fc.visible_width = w
    fc.visible_height = h
    fc.profile = 3
    fc.pixel_format = 0
    fc.bit_depth = 10
    fc.qmatrix_id = 0      # flat（ADR-C033 终版）
    fc.qp_base = qp
    fc.slice_rows = 16
    fc.sar_num = fc.sar_den = 1
    fc.reserved[0] = 8     # rans2（ADR-C038 产品默认）
    return fc


def topos_encode_frames(codec, frames, w, h, target: int):
    """产品码控路径：首帧 sized + 反馈单遍。返回逐帧解码平面列表 + qp 轨迹。"""
    fc = make_fc(w, h)
    recs = []
    rc = ToposRateFeedback(target, qp_max=QP_MAX)
    for i, fr in enumerate(frames):
        if i == 0:
            pkt, st, qp_used = codec.encode_sized(
                fc, [p.tobytes() for p in fr], target, 0, QP_MAX)
            rc.seed(qp_used, len(pkt))
            qp = qp_used
        else:
            fc.qp_base = rc.qp
            pkt, st = codec.encode_frame(fc, [p.tobytes() for p in fr])
            qp = int(fc.qp_base)
            rc.note(len(pkt))
        recs.append((qp, len(pkt), decode_to_planes(codec, pkt, w, h)))
    return recs


def decode_to_planes(codec, pkt, w, h):
    fr = codec.decode(pkt)
    dims = [(h, w), (h, w // 2), (h, w // 2)]
    out = []
    for p, (hh, ww) in zip(fr.planes, dims):
        out.append(np.frombuffer(p, dtype="<u2", count=hh * ww).reshape(hh, ww).copy())
    return out


def prores_run(src_raw: Path, w: int, h: int, n: int, profile: int, tmp: Path):
    """prores_ks 编码 → (总字节, 逐帧解码平面)。"""
    mov = tmp / f"prores_{profile}.mov"
    dec = tmp / f"prores_{profile}.dec.raw"
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-f", "rawvideo",
         "-pixel_format", "yuv422p10le", "-video_size", f"{w}x{h}",
         "-framerate", "25", "-i", str(src_raw),
         "-c:v", "prores_ks", "-profile:v", str(profile), str(mov)],
        check=True)
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-i", str(mov),
         "-f", "rawvideo", "-pix_fmt", "yuv422p10le", str(dec)],
        check=True)
    total = mov.stat().st_size
    return total, load_raw(dec, w, h, n)


def mse(a: np.ndarray, b: np.ndarray) -> float:
    d = a.astype(np.float64) - b.astype(np.float64)
    return float(np.mean(d * d))


def psnr_from_mse(m: float) -> float:
    return 999.0 if m <= 0.0 else 10.0 * np.log10(1023.0 ** 2 / m)


def combined_psnr(mses: list) -> float:
    """422 10-bit 加权合成（像素份额 Y:U:V = 0.5:0.25:0.25）。"""
    m = 0.5 * mses[0] + 0.25 * mses[1] + 0.25 * mses[2]
    return psnr_from_mse(m)


_D8 = None


def dct8_matrix():
    global _D8
    if _D8 is None:
        n = np.arange(8)
        k = (2 * n[:, None] + 1) * n[None, :]  # [freq, sample]
        _D8 = np.cos(k * np.pi / 16) * np.sqrt(2.0 / 8)
        _D8[0] /= np.sqrt(2.0)
    return _D8


def error_band_energy(src: np.ndarray, rec: np.ndarray) -> np.ndarray:
    """误差图的 8×8 DCT 能量按 zigzag 扫描位累计 → [64]。"""
    err = src.astype(np.float64) - rec.astype(np.float64)
    h, w = err.shape
    hb, wb = h // 8, w // 8
    blocks = err[:hb * 8, :wb * 8].reshape(hb, 8, wb, 8).transpose(0, 2, 1, 3).reshape(-1, 8, 8)
    D = dct8_matrix()
    co = D @ blocks @ D.T                       # (nb,8,8) 自然位 [u][v]
    en = (co.reshape(-1, 64) ** 2).sum(axis=0)  # 按自然位的误差能量
    out = np.zeros(64)
    out[zigzag8()] = en                         # 重排到扫描位
    return out


_ZZ8 = None


def zigzag8() -> np.ndarray:
    """kTcZigzag 同构：扫描位 → 自然位（对角遍历）。"""
    global _ZZ8
    if _ZZ8 is None:
        order = []
        for d in range(15):
            if d % 2 == 0:
                rr = range(min(d, 7), max(0, d - 7) - 1, -1)
            else:
                rr = range(max(0, d - 7), min(d, 7) + 1)
            for r in rr:
                order.append(r * 8 + (d - r))
        _ZZ8 = np.array(order)
    return _ZZ8


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixture", default="/tmp/gap4k/src4k.raw")
    ap.add_argument("--width", type=int, default=3840)
    ap.add_argument("--height", type=int, default=2160)
    ap.add_argument("--frames", type=int, default=16)
    ap.add_argument("--label", default="4k")
    ap.add_argument("--bands-plane", default="y", choices=("y", "u", "v"))
    ap.add_argument("--output", type=Path, default=None)
    args = ap.parse_args()

    W, H, N = args.width, args.height, args.frames
    frames = load_raw(Path(args.fixture), W, H, N)
    if len(frames) < N:
        raise SystemExit(f"fixture 帧不足: {len(frames)} < {N}")
    codec = ToposCodec()
    tmp = Path(tempfile.mkdtemp(prefix="gap4k_"))
    print(f"[gap] {args.label} {W}x{H} × {N} 帧；ProRes prores_ks 0/1/2/3")

    result = dict(label=args.label, width=W, height=H, frames=N, profiles=[])
    pidx = {"y": 0, "u": 1, "v": 2}[args.bands_plane]

    for name, prof in PROFILES:
        pr_bytes, pr_planes = prores_run(Path(args.fixture), W, H, N, prof, tmp)
        pr_frame_bytes = pr_bytes // N
        # ProRes 逐帧质量
        pr_mse = [0.0, 0.0, 0.0]
        pr_band = np.zeros(64)
        for i in range(N):
            for k in range(3):
                pr_mse[k] += mse(frames[i][k], pr_planes[i][k]) / N
            pr_band += error_band_energy(frames[i][pidx], pr_planes[i][pidx]) / N

        # Topos @ ProRes 实测码率（逐帧目标 = 总/帧数）
        tp = topos_encode_frames(codec, frames, W, H, pr_frame_bytes)
        tp_bytes = sum(r[1] for r in tp)
        tp_mse = [0.0, 0.0, 0.0]
        tp_band = np.zeros(64)
        for i in range(N):
            for k in range(3):
                tp_mse[k] += mse(frames[i][k], tp[i][2][k]) / N
            tp_band += error_band_energy(frames[i][pidx], tp[i][2][pidx]) / N

        # Topos @ P5 档位落点
        tier_target = max(1, round(TIERS[name] * W * H / 8.0))
        tt = topos_encode_frames(codec, frames, W, H, tier_target)
        tt_bytes = sum(r[1] for r in tt)
        tt_mse = [0.0, 0.0, 0.0]
        for i in range(N):
            for k in range(3):
                tt_mse[k] += mse(frames[i][k], tt[i][2][k]) / N

        row = dict(
            tier=name, prores_profile=prof,
            prores_bytes=pr_bytes, prores_bpp=pr_bytes * 8.0 / (W * H * N),
            topos_matched_bytes=tp_bytes, matched_ratio=tp_bytes / pr_bytes,
            matched=dict(
                qp0=tp[0][0],
                psnr_y=psnr_from_mse(tp_mse[0]), psnr_u=psnr_from_mse(tp_mse[1]),
                psnr_v=psnr_from_mse(tp_mse[2]),
                combined=combined_psnr(tp_mse)),
            prores=dict(
                psnr_y=psnr_from_mse(pr_mse[0]), psnr_u=psnr_from_mse(pr_mse[1]),
                psnr_v=psnr_from_mse(pr_mse[2]),
                combined=combined_psnr(pr_mse)),
            tier_landing=dict(
                bytes=tt_bytes, ratio=tt_bytes / pr_bytes, qp0=tt[0][0],
                psnr_y=psnr_from_mse(tt_mse[0]),
                psnr_u=psnr_from_mse(tt_mse[1]), psnr_v=psnr_from_mse(tt_mse[2]),
                combined=combined_psnr(tt_mse)),
            bands={},
        )
        for lo, hi, bname in BANDS:
            row["bands"][bname] = dict(
                topos=float(tp_band[lo:hi].sum()),
                prores=float(pr_band[lo:hi].sum()),
                ratio=float(tp_band[lo:hi].sum() / max(pr_band[lo:hi].sum(), 1e-9)))
        d_m = row["matched"]["combined"] - row["prores"]["combined"]
        d_t = row["tier_landing"]["combined"] - row["prores"]["combined"]
        band_s = " ".join(
            f"{b}×{row['bands'][b]['ratio']:.2f}" for _, _, b in BANDS)
        print(f"[gap] {args.label}/{name}: 码率 {row['prores_bpp']:.3f}bpp "
              f"matched {tp_bytes/pr_bytes:.2f}× → Δ合成 {d_m:+.2f} dB "
              f"(Y {row['matched']['psnr_y']-row['prores']['psnr_y']:+.2f}) "
              f"tier落点 {tt_bytes/pr_bytes:.2f}× → Δ合成 {d_t:+.2f} dB")
        print(f"      误差带比(topos/prores, {args.bands_plane}): {band_s}")
        result["profiles"].append(row)

    out = args.output or (REPO / "native" / "topos_codec" / "tools"
                          / f"gap_decomp_{args.label}_2026-09-11.json")
    out.write_text(json.dumps(result, indent=1))
    print(f"[gap] written {out}")


if __name__ == "__main__":
    main()
