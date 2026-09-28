#!/usr/bin/env python3
"""DNxHR HQX（422 10-bit 母版档）编解码速度参照——真实素材矩阵对照。

与 ``bench_tier_speed.py`` 的 prores_ref 同口径：同源 raw yuv422p10le、
ffmpeg 墙钟计时（含 demux/IO/进程启动，25p），fps = 帧数/墙钟。每组
3 次取中位数（页缓存热）。HQX 码率由 profile+分辨率固定（-b:v 0
自选），输出 .mov。

双口径（进程启动 ~0.2-0.6s 会污染短跑墙钟，两口径都报）：

- **wall**：60 帧单次墙钟（= 既有 prores_ref 口径，短片体感）；
- **marginal**：60 帧 vs 300 帧斜率（长片口径，扣固定开销；对 ffmpeg
  更公平，Topos 侧纯调用计时本就是边际口径）。编码 300 帧 =
  ``-stream_loop 4`` 循环同源；解码 300 帧 = concat 5×60 帧 mov。

用法：
  python bench_dnxhr_hqx.py --source 2K_prores422HQmov.yuv \
      --width 1920 --height 1080 --frames 60 --label real2k_422 \
      --output tools/tierspeed_dnxhr_hqx_2k_2026-09-12.json
"""
from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

FPS = 25


def _wall(cmd: list[str]) -> float:
    t0 = time.perf_counter()
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    return time.perf_counter() - t0


def _med(walls: list[float]) -> float:
    return statistics.median(walls)


def bench(src: Path, pix_fmt: str, w: int, h: int, n: int,
          label: str, runs: int = 3) -> dict:
    tmp = Path(tempfile.mkdtemp(prefix="dnxhr_"))
    mov60 = tmp / "hqx60.mov"
    base_in = ["-f", "rawvideo", "-pixel_format", pix_fmt,
               "-video_size", f"{w}x{h}", "-framerate", str(FPS),
               "-i", str(src)]
    enc = ["-c:v", "dnxhd", "-profile:v", "dnxhr_hqx", "-b:v", "0"]
    subprocess.run(["ffmpeg", "-y", "-v", "error", *base_in,
                    "-frames:v", str(n), *enc, str(mov60)], check=True)
    size = mov60.stat().st_size

    # 300 帧解码源：concat 5×60（-c copy，不计时）
    lst = tmp / "list.txt"
    lst.write_text("".join(f"file '{mov60}'\n" for _ in range(5)))
    mov300 = tmp / "hqx300.mov"
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-f", "concat", "-safe",
                    "0", "-i", str(lst), "-c", "copy", str(mov300)], check=True)

    # 编码：60 帧（墙钟口径）+ 300 帧 stream_loop（斜率口径）
    enc60 = [_wall(["ffmpeg", "-y", "-v", "error", *base_in,
                    "-frames:v", str(n), *enc, str(tmp / "e60.mov")])
             for _ in range(runs)]
    enc300 = [_wall(["ffmpeg", "-y", "-v", "error", "-stream_loop", "4",
                     *base_in, "-frames:v", str(n * 5), *enc,
                     str(tmp / "e300.mov")])
              for _ in range(runs)]
    # 解码：60 帧 + 300 帧 → null
    dec60 = [_wall(["ffmpeg", "-v", "error", "-i", str(mov60),
                    "-f", "null", "-"]) for _ in range(runs)]
    dec300 = [_wall(["ffmpeg", "-v", "error", "-i", str(mov300),
                     "-f", "null", "-"]) for _ in range(runs)]

    def slope(t_hi: float, t_lo: float) -> float:
        return (t_hi - t_lo) / float(n * 4)   # ms→s 每 240 帧

    enc_marg = 1.0 / slope(_med(enc300), _med(enc60))
    dec_marg = 1.0 / slope(_med(dec300), _med(dec60))
    return dict(
        label=label, codec="dnxhr_hqx", src_fmt=pix_fmt,
        width=w, height=h, frames=n,
        mbps=size * 8.0 / (n / FPS) / 1e6,
        bpp=size * 8.0 / (w * h * n),
        encode_fps=n / _med(enc60),
        encode_realtime=(n / _med(enc60)) / FPS,
        encode_marginal_fps=enc_marg,
        encode_marginal_realtime=enc_marg / FPS,
        decode_fps=n / _med(dec60),
        decode_realtime=(n / _med(dec60)) / FPS,
        decode_marginal_fps=dec_marg,
        decode_marginal_realtime=dec_marg / FPS,
        detail=dict(enc60_walls=enc60, enc300_walls=enc300,
                    dec60_walls=dec60, dec300_walls=dec300),
    )


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--src-fmt", default="yuv422p10le")
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--label", required=True)
    ap.add_argument("--output", type=Path, default=None)
    args = ap.parse_args()
    row = bench(Path(args.source), args.src_fmt, args.width, args.height,
                args.frames, args.label)
    txt = json.dumps(row, indent=1, ensure_ascii=False)
    print(txt)
    if args.output:
        args.output.write_text(txt)


if __name__ == "__main__":
    main()
