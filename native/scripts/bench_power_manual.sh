#!/usr/bin/env bash
# R6 基准功耗采集（手动；macOS powermetrics 需要 root，bench 工具本身不采集）。
#
# 用法：
#   sudo bash native/topos_codec/scripts/bench_power_manual.sh [quick]
#
# 输出：终端打印 CPU package 功耗采样（间隔 1s），包裹一次 topos_quality perf。
# 报告入库时把采样区间（去首尾各 1 个样本）的平均/峰值 watts 写进环境块
# 「功耗」字段，并注明采样方式与样本数。
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
MODE="${1:-quick}"
BIN="$HERE/../build/perf/topos_quality"

if [ ! -x "$BIN" ]; then
    echo "未找到 $BIN —— 先构建 perf 配置（README §性能）" >&2
    exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
    echo "powermetrics 需要 root：sudo bash $0 $MODE" >&2
    exit 1
fi

echo "=== 功耗采样包裹 perf $MODE（间隔 1s；去首尾样本后取均值/峰值） ==="
powermetrics --samplers cpu_power -i 1000 -a \
    | grep -E 'CPU.*Watts|Package' \
    | tee /tmp/topos_power_samples.txt &
PM_PID=$!
trap 'kill $PM_PID 2>/dev/null || true' EXIT

"$BIN" perf "$MODE"

sleep 1
kill $PM_PID 2>/dev/null || true
echo "=== 样本已存 /tmp/topos_power_samples.txt；统计： ==="
python3 - <<'PY'
import re
vals = []
for line in open('/tmp/topos_power_samples.txt'):
    m = re.search(r'([0-9]+\.?[0-9]*)\s*W', line)
    if m:
        vals.append(float(m.group(1)))
if len(vals) >= 3:
    core = vals[1:-1]
    print(f"样本 {len(core)} 个：avg={sum(core)/len(core):.2f} W, peak={max(core):.2f} W")
else:
    print(f"样本不足（{len(vals)}）——检查 powermetrics 输出格式")
PY
