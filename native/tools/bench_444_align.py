#!/usr/bin/env python3
"""444 家族 flat 矩阵验证 + pro444/extreme 码率对齐（2026-09-11）。

战役 item 4：444 两档自 P4 起沿用 Standard 矩阵（qmatrix_id=1，注释
"未验证 flat"）与旧定标 bpp（pro444 4.11063 / extreme 6.76296，无
4444 素材锚点）。本工具在真实 4:4:4 素材（Avid DNxHR 444 解码共享源，
chroma 实测全分辨率细节 U/Y≈0.89，非 422 上采样）上：

  1. 矩阵 A/B：flat(0) vs Standard(1)（产品配置 qp_delta_chroma=4）
     在 prores_ks 4444/4444xq 实测码率对齐点比较逐平面 PSNR；
     另设 flat+色度偏移 0 验证臂（tier 的 +4 系 P2 随 Standard 选定）；
  2. 码率对齐：prores_ks profile 4444/4444xq 实测 bpp（ADR-C032
     422 家族同口径：2K/4K 均值 → tier 重定标候选）；
  3. 档位落点：现行 bpp 与重定标 bpp 下 Topos 444 的落点/质量。

注：qmatrix_id=2（444Compact）被帧头规则限定 GBR+12bit（profile 5/6
pf=2），YUV 444 档位不适用，不在 A/B 内。共享源 = raw yuv444p10/12le
（两编码器同输入；PSNR 各对同一源）。源经 memmap 逐帧读取、MSE 增量
累加（4K 444 单流 ~800MB，不驻留）。
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
    TOPOS_CODEC_ABI_VERSION, ToposCodec, _CFrameConfig,
)
from src.shared.export.topos_rate_control import ToposRateFeedback  # noqa: E402

QP_MAX = 95
# P4 旧定标（现行 tier target_bpp）
TIER_BPP = {"pro444": 4.11063, "extreme": 6.76296}
# prores_ks profile（ffmpeg 名字）
PRORES_PROFILES = (("4444", "4444"), ("4444xq", "4444xq"))
AXES = {
    "10bit": dict(src_fmt="yuv444p10le", bit_depth=10, profiles=("4444",)),
    "12bit": dict(src_fmt="yuv444p12le", bit_depth=12, profiles=("4444", "4444xq")),
}


def src_frame(src: np.memmap, w: int, h: int, i: int) -> list:
    """第 i 帧 → 3 个全分辨率平面（uint16 视图，零拷贝）。"""
    px = w * h
    off = i * 3 * px
    return [src[off + k * px: off + (k + 1) * px].reshape(h, w) for k in range(3)]


def make_fc(w: int, h: int, profile: int, bit_depth: int, qmatrix: int,
            dchroma: int, qp: int = 20) -> _CFrameConfig:
    fc = _CFrameConfig()
    fc.struct_size = ctypes.sizeof(_CFrameConfig)
    fc.abi_version = TOPOS_CODEC_ABI_VERSION
    fc.visible_width = w
    fc.visible_height = h
    fc.profile = profile          # 5=Pro444 / 6=Extreme（R4.4）
    fc.pixel_format = 1           # YUV 4:4:4（R4.2）
    fc.bit_depth = bit_depth
    fc.qmatrix_id = qmatrix
    fc.qp_base = qp
    fc.qp_delta_chroma = dchroma  # 444 tier 声明值（P2）
    fc.slice_rows = 16
    fc.sar_num = fc.sar_den = 1
    fc.reserved[0] = 8            # rans2（ADR-C038 产品默认）
    return fc


class MseAcc:
    """逐平面 MSE 累加（避免整流驻留）。"""

    def __init__(self) -> None:
        self.s = [0.0, 0.0, 0.0]
        self.n = 0

    def add(self, planes: list, ref: list) -> None:
        for k in range(3):
            d = planes[k].astype(np.float64) - ref[k].astype(np.float64)
            self.s[k] += float(np.mean(d * d))
        self.n += 1

    def mses(self) -> list:
        return [v / max(self.n, 1) for v in self.s]

    def psnr(self, bit_depth: int) -> dict:
        peak = float((1 << bit_depth) - 1)
        m = self.mses()

        def p(x: float) -> float:
            return 999.0 if x <= 0.0 else 10.0 * np.log10(peak * peak / x)

        # 444 像素份额 Y:U:V = 1/3:1/3:1/3
        combined = p(sum(m) / 3.0)
        return dict(psnr_y=p(m[0]), psnr_u=p(m[1]), psnr_v=p(m[2]),
                    combined=combined)


def topos_run(codec, src: np.memmap, w: int, h: int, n: int, target: int,
              profile: int, bit_depth: int, qmatrix: int, dchroma: int):
    """产品码控路径（首帧 sized + 反馈单遍）。返回 (bytes, qp 轨迹, MSE)。"""
    fc = make_fc(w, h, profile, bit_depth, qmatrix, dchroma)
    total = 0
    qps = []
    acc = MseAcc()
    rc = ToposRateFeedback(target, qp_max=QP_MAX)
    for i in range(n):
        fr = [np.ascontiguousarray(p, dtype="<u2").tobytes() for p in src_frame(src, w, h, i)]
        if i == 0:
            pkt, st, qp_used = codec.encode_sized(fc, fr, target, 0, QP_MAX)
            rc.seed(qp_used, len(pkt))
            qp = qp_used
        else:
            fc.qp_base = rc.qp
            pkt, st = codec.encode_frame(fc, fr)
            qp = int(fc.qp_base)
            rc.note(len(pkt))
        qps.append(qp)
        total += len(pkt)
        rec = codec.decode(pkt)
        acc.add([np.frombuffer(p, dtype="<u2", count=w * h).reshape(h, w)
                 for p in rec.planes], src_frame(src, w, h, i))
    return total, qps, acc


def prores_run(src_path: Path, src_fmt: str, w: int, h: int, n: int,
               profile: str, tmp: Path) -> tuple:
    """prores_ks 编码 → (mov 字节, 帧字节均值的整倍数总字节, 解码 raw 路径)。"""
    mov = tmp / f"prores_{profile}_{src_fmt}.mov"
    dec = tmp / f"prores_{profile}_{src_fmt}.dec.raw"
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-f", "rawvideo",
         "-pixel_format", src_fmt, "-video_size", f"{w}x{h}",
         "-framerate", "25", "-i", str(src_path), "-frames:v", str(n),
         "-c:v", "prores_ks", "-profile:v", profile, str(mov)],
        check=True)
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-i", str(mov),
         "-f", "rawvideo", "-pix_fmt", src_fmt, str(dec)],
        check=True)
    return mov.stat().st_size, dec


def prores_psnr(dec_path: Path, src: np.memmap, w: int, h: int, n: int,
                bit_depth: int) -> dict:
    acc = MseAcc()
    px = w * h * 3 * 2
    with open(dec_path, "rb") as f:
        for i in range(n):
            b = f.read(px)
            if len(b) < px:
                raise SystemExit("prores 解码帧不足")
            planes = [np.frombuffer(b[k * w * h * 2:(k + 1) * w * h * 2],
                                    dtype="<u2").reshape(h, w) for k in range(3)]
            acc.add(planes, src_frame(src, w, h, i))
    return acc.psnr(bit_depth)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True, help="raw yuv444pXXle 源")
    ap.add_argument("--axis", required=True, choices=sorted(AXES))
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--frames", type=int, default=16)
    ap.add_argument("--label", required=True)
    ap.add_argument("--output", type=Path, default=None)
    args = ap.parse_args()

    W, H, N = args.width, args.height, args.frames
    ax = AXES[args.axis]
    bd = ax["bit_depth"]
    src = np.memmap(args.source, dtype="<u2", mode="r")
    if src.size < N * 3 * W * H:
        raise SystemExit(f"源帧不足: {src.size // (3 * W * H)} < {N}")
    codec = ToposCodec()
    tmp = Path(tempfile.mkdtemp(prefix="align444_"))
    print(f"[444] {args.label}/{args.axis} {W}x{H} × {N} 帧；锚点 "
          f"prores_ks {ax['profiles']}")

    result = dict(label=args.label, axis=args.axis, width=W, height=H,
                  frames=N, bit_depth=bd, anchors=[], runs=[])

    anchors = {}
    for prof in ax["profiles"]:
        sz, dec = prores_run(Path(args.source), ax["src_fmt"], W, H, N, prof, tmp)
        psnr = prores_psnr(dec, src, W, H, N, bd)
        frame_bytes = sz // N
        anchors[prof] = frame_bytes
        row = dict(anchor=prof, bytes=sz, bpp=sz * 8.0 / (W * H * N),
                   frame_bytes=frame_bytes, psnr=psnr)
        # 锚点自身质量（vs 共享源）
        print(f"[444] anchor {prof}: {row['bpp']:.4f} bpp "
              f"合成 {psnr['combined']:.2f} dB (Y {psnr['psnr_y']:.2f} "
              f"U {psnr['psnr_u']:.2f} V {psnr['psnr_v']:.2f})")
        result["anchors"].append(row)

    def run(name: str, target: int, profile: int, qm: int, dchroma: int):
        t0 = time.perf_counter()
        total, qps, acc = topos_run(codec, src, W, H, N, target, profile,
                                    bd, qm, dchroma)
        dt = time.perf_counter() - t0
        psnr = acc.psnr(bd)
        row = dict(run=name, profile=profile, qmatrix=qm, dchroma=dchroma,
                   target_frame_bytes=target, bytes=total,
                   bpp=total * 8.0 / (W * H * N), secs=dt,
                   qp0=qps[0], qp_mean=float(np.mean(qps)), psnr=psnr)
        print(f"[444] {name} qm{qm} dc{dchroma}: {row['bpp']:.4f} bpp "
              f"qp{qps[0]}→均{row['qp_mean']:.1f} 合成 {psnr['combined']:.2f} "
              f"(Y {psnr['psnr_y']:.2f} U {psnr['psnr_u']:.2f} "
              f"V {psnr['psnr_v']:.2f}) {dt:.1f}s")
        result["runs"].append(row)
        return row

    for prof in ax["profiles"]:
        tier_profile = 6 if (prof == "4444xq" and bd == 12) else 5
        fb = anchors[prof]
        # 矩阵 A/B（产品色度偏移 4）
        run(f"matched@{prof}/flat", fb, tier_profile, 0, 4)
        run(f"matched@{prof}/standard", fb, tier_profile, 1, 4)
        # 色度偏移验证臂（flat + 0；tier +4 系 P2 随 Standard 选定）
        run(f"matched@{prof}/flat-dc0", fb, tier_profile, 0, 0)

    # 档位落点：现行 bpp × {现行矩阵 1, flat}
    for tier, bpp in TIER_BPP.items():
        tier_profile = 5 if tier == "pro444" else 6
        if tier_profile == 6 and bd != 12:
            continue  # extreme 仅 12bit
        fb = max(1, round(bpp * W * H / 8.0))
        run(f"tier@{tier}/standard", fb, tier_profile, 1, 4)
        run(f"tier@{tier}/flat", fb, tier_profile, 0, 4)

    out = args.output or (REPO / "native" / "topos_codec" / "tools"
                          / f"align444_{args.label}_{args.axis}_2026-09-11.json")
    out.write_text(json.dumps(result, indent=1))
    print(f"[444] written {out}")


if __name__ == "__main__":
    main()
