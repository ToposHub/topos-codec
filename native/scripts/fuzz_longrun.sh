#!/usr/bin/env bash
# libFuzzer 持续长跑（R7 任务 5；codec-fuzz.yml 调用）。
#
# 用法： bash fuzz_longrun.sh <build-dir> <max-total-time-seconds>
#
# 每个 target：seed corpus 拷到可写 corpus-out/<target>/（libFuzzer 会把
# 新发现单元写回该目录 → 上传 artifact 沉淀语料）；crash/leak/timeout 写
# artifacts/ 前缀。任一 target 崩溃 → 非零退出（CI job 失败，不静默）。
set -euo pipefail

BUILD="$(cd "$1" && pwd)"
MAX_TIME="${2:-300}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# topos_fuzz_mov 喂 mov 种子语料（seed_corpus = 容器布局真实样本；
# 此前误喂位流语料 seed_corpus_bitstream——变异 fuzz 从未命中容器结构）
TARGETS=(
  topos_fuzz_primitives:seed_corpus
  topos_fuzz_bitstream:seed_corpus_bitstream
  topos_fuzz_codec:seed_corpus_bitstream
  topos_fuzz_mov:seed_corpus
  topos_fuzz_encode:seed_corpus_bitstream
)

mkdir -p "$BUILD/corpus-out" "$BUILD/artifacts"
FAIL=0
for pair in "${TARGETS[@]}"; do
  target="${pair%%:*}"
  seed="${pair##*:}"
  bin="$BUILD/$target"
  if [ ! -x "$bin" ]; then
    echo "缺少 fuzz target: $bin（需 -DTOPOS_ENABLE_FUZZING=ON + clang）"
    exit 1
  fi
  out="$BUILD/corpus-out/$target"
  mkdir -p "$out"
  cp "$HERE/tests/fuzz/$seed/"* "$out/" 2>/dev/null || true
  echo "=== $target（max_total_time=${MAX_TIME}s，corpus=$(ls "$out" | wc -l | tr -d ' ')） ==="
  if ! "$bin" -max_total_time="$MAX_TIME" -max_len=65536 \
        -artifact_prefix="$BUILD/artifacts/" -print_final_stats "$out"; then
    echo "!!! $target 发现崩溃/泄漏/超时（见 $BUILD/artifacts/）"
    FAIL=1
  fi
done
[ "$FAIL" -eq 0 ] || exit 1
echo "libFuzzer 长跑: 全部 target 完成（crash=0）"
