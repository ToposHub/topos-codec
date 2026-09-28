#!/usr/bin/env python3
"""性能门禁（复验 P1-18）：解析 topos_quality perf 输出并执行**数值阈值**。

此前 run_tests.sh 的 perf 段只 grep 固定文字（"帧级基准"），任何回退都
全绿——文本 grep 不构成性能门禁。本脚本把 benchmark_protocol §6 v2 的
门槛数值化：

- 基准门槛（threads=4 发布口径，protocol §6 v2）：
    1080p encode p50 ≤ 40 ms        1080p decode p95 ≤ 36 ms
    1080p sized  p50 ≤ 160 ms       4K encode p50 ≤ 160 ms
    4K decode p95 ≤ 130 ms          4K sized p50 ≤ 550 ms（2× 实测 435）
- 门禁余量：默认 mult=3.0 吸收非专用 runner 的环境噪声（复验证实同机
  后台负载可达 ±2×）。专用空载机跑严格口径：TOPOS_PERF_GATE_MULT=1。
- sized 平均迭代 ≤ 12（码控搜索发散哨兵）。

用法：
    topos_quality perf quick | python3 scripts/perf_gate.py [--json out.json]
退出码：0 = 全部达标；1 = 有超阈（CI 可据此失败）。
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

# (geometry, mode, stat, threshold_ms)
_THRESHOLDS = [
    ("1920x1080", "编码", "p50", 40.0),
    ("1920x1080", "解码", "p95", 36.0),
    ("1920x1080", "sized", "p50", 160.0),
    ("3840x2160", "编码", "p50", 160.0),
    ("3840x2160", "解码", "p95", 130.0),
    ("3840x2160", "sized", "p50", 550.0),
]
MAX_SIZED_ITERS = 12.0

_ROW_RE = re.compile(
    r"^\|\s*(?P<geo>\d+x\d+)\s+qp\d+\s*\|\s*(?P<mode>[^|]+?)\s*\|"
    r"\s*(?P<p50>[\d.]+)\s*\|\s*(?P<p95>[\d.]+)\s*\|\s*(?P<p99>[\d.]+)\s*\|"
    r"[^|]*\|[^|]*\|[^|]*\|\s*(?P<iters>[\d.]+|—)\s*\|")


def parse(text: str) -> dict:
    rows = {}
    sized_iters = {}
    for line in text.splitlines():
        m = _ROW_RE.match(line.strip())
        if not m:
            continue
        geo = m.group("geo")
        mode = m.group("mode")
        if mode.startswith("sized"):
            mode = "sized"
            try:
                sized_iters[(geo, "sized")] = float(m.group("iters"))
            except ValueError:
                pass
        rows[(geo, mode)] = {
            "p50": float(m.group("p50")),
            "p95": float(m.group("p95")),
            "p99": float(m.group("p99")),
        }
    return {"rows": rows, "sized_iters": sized_iters}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=None, help="结果 JSON 输出路径")
    args = ap.parse_args()

    mult = float(os.environ.get("TOPOS_PERF_GATE_MULT", "3.0"))
    text = sys.stdin.read()
    data = parse(text)
    rows = data["rows"]

    failures = []
    checks = []
    for geo, mode, stat, thr in _THRESHOLDS:
        got = rows.get((geo, mode), {}).get(stat)
        limit = thr * mult
        item = {
            "check": f"{geo} {mode} {stat}", "threshold_ms": thr,
            "multiplier": mult, "limit_ms": round(limit, 1), "measured_ms": got,
        }
        if got is None:
            item["status"] = "MISSING"
            failures.append(f"{geo} {mode} {stat}: 结果缺失（解析失败？）")
        elif got > limit:
            item["status"] = "FAIL"
            failures.append(
                f"{geo} {mode} {stat}={got:.1f}ms > 门限 {limit:.1f}ms"
                f"（基准 {thr} × 余量 {mult}）")
        else:
            item["status"] = "PASS"
        checks.append(item)

    for (geo, _), iters in sorted(data["sized_iters"].items()):
        item = {
            "check": f"{geo} sized 平均迭代", "threshold_ms": MAX_SIZED_ITERS,
            "measured_iters": iters, "status": "PASS",
        }
        if iters > MAX_SIZED_ITERS:
            item["status"] = "FAIL"
            failures.append(f"{geo} sized 平均迭代 {iters} > {MAX_SIZED_ITERS}"
                            "（码控搜索发散）")
        checks.append(item)

    result = {
        "gate": "perf_gate v1（复验 P1-18）", "multiplier": mult,
        "checks": checks, "failures": failures,
        "ok": not failures,
    }
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(result, fh, ensure_ascii=False, indent=2)
    for c in checks:
        mv = c.get("measured_ms", c.get("measured_iters"))
        print(f"[{c['status']}] {c['check']}: {mv} "
              f"(limit {c.get('limit_ms', c.get('threshold_ms'))})")
    if failures:
        print("\n".join(["PERF GATE FAIL:"] + failures))
        return 1
    print("PERF GATE PASS "
          f"(mult={mult}; 专用空载机可用 TOPOS_PERF_GATE_MULT=1 跑严格口径)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
