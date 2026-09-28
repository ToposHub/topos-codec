#!/usr/bin/env python3
"""Compare V2/V3/V6 on explicit planar 4:2:2 uint16 input; no image conversion.

Run from the repository root with TOPOS_CODEC_LIB pointing to a Release build.
Reports same-QP measurements, not a claim of matched-quality bitrate savings.
The benchmark manifest rejects a stale library or dirty tree unless
``--allow-dirty`` is supplied explicitly.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import statistics
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from src.shared.codec.topos_binding import ToposCodec, _CFrameConfig, TOPOS_CODEC_ABI_VERSION
from tools.bench_manifest import build_manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', type=Path, nargs='+')
    parser.add_argument('--width', type=int, required=True)
    parser.add_argument('--height', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--threads', type=int, default=4)
    parser.add_argument('--allow-dirty', action='store_true',
                        help='允许工作树有未提交改动；结果会标记 git_dirty')
    args = parser.parse_args()
    codec = ToposCodec()
    manifest = build_manifest(allow_dirty=args.allow_dirty)
    codec.set_slice_threads(args.threads)
    manifest["benchmark_slice_threads"] = codec.slice_threads()
    rows: list[dict] = []
    w, h = args.width, args.height
    n, c = w * h, ((w + 1) // 2) * h
    for path in args.inputs:
        raw = path.read_bytes()
        if len(raw) != (n + 2 * c) * 2:
            raise ValueError(f'{path}: expected {(n + 2 * c) * 2} bytes, got {len(raw)}')
        data = np.frombuffer(raw, dtype='<u2')
        if data.max() > 1023:
            raise ValueError(f'{path}: input exceeds 10-bit range')
        planes = [data[:n], data[n:n + c], data[n + c:]]
        for tier, qm, qp in [('low', 1, 63), ('medium', 1, 61),
                             ('high', 1, 58), ('ultra', 0, 58)]:
            for mode, coding, heuristics in [('v2', 1, 0), ('v2-aq-rdo', 1, 1),
                                             ('intra', 3, 0), ('intra-range', 6, 0)]:
                cfg = _CFrameConfig()
                cfg.struct_size = ctypes.sizeof(cfg)
                cfg.abi_version = TOPOS_CODEC_ABI_VERSION
                cfg.visible_width, cfg.visible_height = w, h
                cfg.profile, cfg.bit_depth, cfg.color_range = 3, 10, 1
                cfg.qmatrix_id, cfg.qp_base = qm, qp
                cfg.reserved[0] = coding
                cfg.reserved[1] = cfg.reserved[2] = heuristics
                packet = codec.encode(cfg, planes)  # warm caches
                elapsed: list[float] = []
                for _ in range(args.repeats):
                    start = time.perf_counter()
                    check = codec.encode(cfg, planes)
                    elapsed.append((time.perf_counter() - start) * 1000)
                    assert check == packet, 'non-deterministic encoding'
                start = time.perf_counter()
                decoded = codec.decode(packet)
                decode_ms = (time.perf_counter() - start) * 1000
                assert decoded.info.concealed_slices == 0
                psnr = []
                for source, reconstructed in zip(planes, decoded.planes):
                    difference = source.astype(np.float64) - np.frombuffer(reconstructed, '<u2')
                    mse = float(np.mean(difference ** 2))
                    psnr.append(10 * math.log10(1023 ** 2 / mse) if mse else None)
                row = dict(input=path.name, sha256=hashlib.sha256(raw).hexdigest(),
                           tier=tier, mode=mode, qm=qm, qp=qp, bytes=len(packet),
                           psnr_yuv=psnr, encode_ms=statistics.median(elapsed),
                           decode_ms=decode_ms)
                rows.append(row)
                print(json.dumps(row), flush=True)
    args.output.write_text(json.dumps(dict(manifest=manifest, width=w, height=h, bit_depth=10,
        pixel_format='yuv422p10le', threads=codec.slice_threads(), repeats=args.repeats,
        comparison='same QP; inspect PSNR separately', results=rows), indent=2) + '\n')


if __name__ == '__main__':
    main()
