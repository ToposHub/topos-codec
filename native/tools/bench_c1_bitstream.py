#!/usr/bin/env python3
"""C1 actual bitstream probe.

Encodes the same quantized input through V1 Rice, V2 VLC, and the experimental
major=4 C1 pair syntax.  Decode is timed through the public binding so the
probe covers packet parsing, CRC, slice dispatch, and reconstruction.  The
probe is intentionally small and deterministic; it is evidence for the plan,
not a production quality gate.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "native" / "topos_codec" / "tools"))

import bench_symbol_budget as c0
from src.shared.codec import topos_binding as tb


def run(codec: tb.ToposCodec, frames, qp: int, entropy: int, repeats: int) -> dict:
    packets = []
    encode_ns = []
    decode_ns = []
    for planes in frames:
        cfg = c0.frame_config(tb, planes[0].shape[1], planes[0].shape[0], qp, entropy)
        t0 = time.perf_counter_ns()
        packet, _stats = codec.encode_frame(cfg, [p.tobytes() for p in planes])
        encode_ns.append(time.perf_counter_ns() - t0)
        packets.append(packet)
    for packet in packets:
        codec.decode(packet)  # warm parser/decoder tables
    for _ in range(repeats):
        for packet in packets:
            t0 = time.perf_counter_ns()
            frame = codec.decode(packet)
            decode_ns.append(time.perf_counter_ns() - t0)
            if frame.info.concealed_slices:
                raise RuntimeError("C1 probe produced concealed slices")
    return {
        "entropy": {0: "v1_rice", 1: "v2_vlc", 4: "c1_pair", 5: "c2_pair_table"}[entropy],
        "qp": qp,
        "frames": len(frames),
        "packet_bytes": [len(p) for p in packets],
        "mean_packet_bytes": sum(map(len, packets)) / len(packets),
        "encode_ms_per_frame": sum(encode_ns) / len(encode_ns) / 1e6,
        "decode_ms_per_frame": sum(decode_ns) / len(decode_ns) / 1e6,
        "blocks_per_frame": ((frames[0][0].shape[1] + 7) // 8
                              * ((frames[0][0].shape[0] + 7) // 8)
                              + 2 * ((frames[0][1].shape[1] + 7) // 8)
                              * ((frames[0][1].shape[0] + 7) // 8)),
        "decode_ns_per_block": (
            sum(decode_ns) / len(decode_ns)
            / (((frames[0][0].shape[1] + 7) // 8)
               * ((frames[0][0].shape[0] + 7) // 8)
               + 2 * ((frames[0][1].shape[1] + 7) // 8)
               * ((frames[0][1].shape[0] + 7) // 8))),
        "decode_ms_samples": [round(x / 1e6, 4) for x in decode_ns],
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=360)
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--qp", type=int, default=24)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--output", type=Path,
                    default=ROOT / "native" / "topos_codec" / "tools"
                    / "c1_bitstream_2026-09-07.json")
    args = ap.parse_args()
    codec = tb.ToposCodec()
    frames = c0.make_frames("texture", args.width, args.height, args.frames, 0xC1A1)
    rows = [run(codec, frames, args.qp, mode, args.repeats)
            for mode in (0, 1, 4, 5)]
    result = {"tool": "bench_c1_bitstream.py", "tool_version": 2,
              "width": args.width, "height": args.height, "rows": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
