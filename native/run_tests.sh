#!/usr/bin/env bash
# 一键构建/测试（ADR 四配置 + libFuzzer 短跑 + 阶段 5 CLI/oracle 冒烟 + Python 绑定/阶段 6 接入）
# 用法：bash native/topos_codec/run_tests.sh [--skip-python]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-clang}"
# Python 解释器：CI 用 TOPOS_PYTHON 指定（Windows 无 .venv）；本地默认 python3
PY_BIN="${TOPOS_PYTHON:-$(command -v python3 || command -v python)}"
GIT_COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"

run_config() {
  local name="$1"; shift
  local dir="$HERE/build/$name"
  echo "=== [$name] configure + build ==="
  cmake -S "$HERE" -B "$dir" -DCMAKE_C_COMPILER="$CC" -DCMAKE_BUILD_TYPE=Debug \
        -DTOPOS_GIT_COMMIT="$GIT_COMMIT" -DTOPOS_ENABLE_TESTS=ON "$@" >/dev/null
  cmake --build "$dir" -j >/dev/null
  echo "=== [$name] ctest ==="
  (cd "$dir" && ctest --output-on-failure)
}

run_config debug
run_config asan  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g"
run_config ubsan -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all -g"
run_config tsan  -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g"
run_config fuzz  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
                 -DTOPOS_ENABLE_FUZZING=ON

echo "=== 阶段 10：公共符号表钉死（tests/abi/public_symbols_v1.txt） ==="
# R7：统一走 stdlib 解析器（PE/ELF/Mach-O 同一路径；Windows 无 nm）
LIB="$HERE/build/debug/topos_codec.dylib"
[ -f "$LIB" ] || LIB="$HERE/build/debug/libtopos_codec.dylib"
[ -f "$LIB" ] || LIB="$HERE/build/debug/topos_codec.so"
[ -f "$LIB" ] || LIB="$HERE/build/debug/libtopos_codec.so"
"$PY_BIN" "$HERE/scripts/check_exports.py" "$LIB" || { echo "符号表钉死失败"; exit 1; }

echo "=== libFuzzer 短跑（20k runs，libFuzzer 可用时）==="
if [ -x "$HERE/build/fuzz/topos_fuzz_primitives" ]; then
  "$HERE/build/fuzz/topos_fuzz_primitives" -runs=20000 -max_len=4096 \
    "$HERE/tests/fuzz/seed_corpus" >/dev/null
  "$HERE/build/fuzz/topos_fuzz_bitstream" -runs=20000 -max_len=65536 \
    "$HERE/tests/fuzz/seed_corpus_bitstream" >/dev/null
  "$HERE/build/fuzz/topos_fuzz_codec" -runs=20000 -max_len=65536 \
    "$HERE/tests/fuzz/seed_corpus_bitstream" >/dev/null
  # mov 容器 fuzz 喂 mov 种子（真实布局起点；喂位流语料变异打不中
  # moov/stbl 结构——M-B2 时序缝隙修复）
  "$HERE/build/fuzz/topos_fuzz_mov" -runs=20000 -max_len=65536 \
    "$HERE/tests/fuzz/seed_corpus" >/dev/null
  "$HERE/build/fuzz/topos_fuzz_encode" -runs=20000 -max_len=65536 \
    "$HERE/tests/fuzz/seed_corpus_bitstream" >/dev/null
  # image envelope fuzz 纳入短跑门禁（阶段 1 交付；无专属 seed corpus，
  # 以 bitstream corpus 作起点，parser 引擎自行变异出 TPIM 结构）
  if [ -x "$HERE/build/fuzz/topos_fuzz_image" ]; then
    "$HERE/build/fuzz/topos_fuzz_image" -runs=20000 -max_len=65536 \
      "$HERE/tests/fuzz/seed_corpus_bitstream" >/dev/null
  fi
  echo "libFuzzer: OK"
else
  echo "libFuzzer 不可用（Apple clang 无 runtime）；由回放 driver 覆盖 fuzz"
fi

echo "=== 阶段 5 CLI 冒烟（rawgen → encoder → probe → decoder → faststart） ==="
TMPD="$(mktemp -d)"
trap 'rm -rf "$TMPD"' EXIT
"$HERE/build/debug/topos_rawgen" --width 96 --height 64 --frames 4 --kind 2 --out "$TMPD/s.raw" >/dev/null
"$HERE/build/debug/topos_encoder_cli" --width 96 --height 64 --fps 24 --qp 24 --qm 1 \
  --input "$TMPD/s.raw" --output "$TMPD/s.mov" >/dev/null
"$HERE/build/debug/topos_probe_cli" "$TMPD/s.mov" --verify | grep -q "verify: 4/4" \
  || { echo "probe verify 失败"; exit 1; }
# R5：--verify 是 packet_scan 级校验（magic+帧头 CRC+结构）——
# 坏帧必须被报出（mdat 载荷起点 = 20+16，帧 0 魔数第 4 字节改坏）
cp "$TMPD/s.mov" "$TMPD/bad.mov"
printf 'X' | dd of="$TMPD/bad.mov" bs=1 seek=39 conv=notrunc 2>/dev/null
# 复验 P2-05：坏帧 → 报告 3/4 且**非零退出**（pipefail 下不能直管道 grep）
set +e
BAD_OUT="$("$HERE/build/debug/topos_probe_cli" "$TMPD/bad.mov" --verify 2>&1)"
BAD_RC=$?
set -e
echo "$BAD_OUT" | grep -q "verify: 3/4" \
  || { echo "probe verify 未报出坏帧（未达 packet_scan 级）"; exit 1; }
[ "$BAD_RC" -eq 1 ] || { echo "probe --verify 检出坏帧必须非零退出（P2-05，got rc=$BAD_RC）"; exit 1; }
# 好文件全帧通过 → 退出 0
"$HERE/build/debug/topos_probe_cli" "$TMPD/s.mov" --verify >/dev/null \
  || { echo "probe --verify 好文件应退出 0"; exit 1; }
"$HERE/build/debug/topos_encoder_cli" --width 96 --height 64 --fps 24 --qp 24 --qm 1 \
  --input "$TMPD/s.raw" --output "$TMPD/fs.mov" --faststart >/dev/null
"$HERE/build/debug/topos_decoder_cli" --input "$TMPD/fs.mov" --output "$TMPD/dec.raw" \
  | grep -q "decoded=4 concealed_frames=0" || { echo "decoder 失败"; exit 1; }
# RD7-05：CLI request 入口必须真实走 reduced；小样本的 AUTO_2K 则按契约保留 full。
"$HERE/build/debug/topos_decoder_cli" --input "$TMPD/s.mov" --scale 1/2 --frames 1 --report \
  | grep -q "request=1/2 path=reduced output=48x32" \
  || { echo "decoder CLI 1/2 reduced 路径失败"; exit 1; }
"$HERE/build/debug/topos_decoder_cli" --input "$TMPD/s.mov" --scale auto2k --frames 1 --report \
  | grep -q "request=auto2k path=auto2k/full output=96x64" \
  || { echo "decoder CLI auto2k 路径失败"; exit 1; }
# FastStart 与标准布局输出确定性 + 一致性
"$HERE/build/debug/topos_encoder_cli" --width 96 --height 64 --fps 24 --qp 24 --qm 1 \
  --input "$TMPD/s.raw" --output "$TMPD/s2.mov" >/dev/null
cmp -s "$TMPD/s.mov" "$TMPD/s2.mov" || { echo "encoder 非确定性"; exit 1; }
echo "CLI 链路: OK"

echo "=== R5 外部 oracle（独立 atom parser + PyAV + ffprobe） ==="
PYVENV="${TOPOS_PYTHON:-$ROOT/.venv/bin/python}"
if [ -x "$PYVENV" ] || [ -n "${TOPOS_PYTHON:-}" ]; then
  "$PYVENV" "$HERE/tests/interop/interop_oracle.py" \
    --encoder-cli "$HERE/build/debug/topos_encoder_cli" \
    || { echo "interop oracle 失败"; exit 1; }
  echo "interop oracle: OK"
else
  # 复验 P2-04：硬 oracle（PyAV + 独立 parser）缺依赖必须**失败**而非静默
  # 跳过——显式豁免用 TOPOS_ALLOW_NO_VENV=1（仅限本机无 venv 的原生开发）
  if [ "${TOPOS_ALLOW_NO_VENV:-0}" = "1" ]; then
    echo "interop oracle 显式豁免（TOPOS_ALLOW_NO_VENV=1）"
  else
    echo "interop oracle 失败：无 .venv（PyAV/独立 parser 为硬 oracle，不可静默降级；显式豁免用 TOPOS_ALLOW_NO_VENV=1）"
    exit 1
  fi
fi

echo "=== 阶段 9：SIMD/并行检查 ==="
# 多线程 golden parity（4 线程跑 conformance，验证并行位流 bit-exact）
TOPOS_SLICE_THREADS=4 "$HERE/build/debug/topos_golden_codec" check \
  "$HERE/tests/conformance/golden_codec_v1.bin" >/dev/null \
  || { echo "多线程 golden codec 失败"; exit 1; }
TOPOS_SLICE_THREADS=4 "$HERE/build/debug/test_stage9" >/dev/null \
  || { echo "stage9 差分/parity 失败"; exit 1; }
echo "多线程 golden parity: OK"
# NEON 内核交叉编译检查（x86_64 宿主上验证 arm64 可编译；无 SDK 时跳过）
if [ "$(uname -m)" != "arm64" ] && clang -arch arm64 -fsyntax-only \
     "$HERE/src/simd/transform_neon.c" -I "$HERE/src" >/dev/null 2>&1; then
  echo "NEON arm64 交叉编译: OK"
else
  echo "NEON 交叉编译跳过（非 x86_64 宿主或无 arm64 SDK）"
fi
# perf 数值门禁（复验 P1-18）：文本 grep 不构成门禁——perf_gate.py 按
# benchmark_protocol §6 v2 阈值 × 环境余量判定（默认 3.0 吸收非专用
# runner 噪声；专用空载机 TOPOS_PERF_GATE_MULT=1 严格口径）
cmake -S "$HERE" -B "$HERE/build/perf" -DCMAKE_C_COMPILER="$CC" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
      -DTOPOS_GIT_COMMIT="$GIT_COMMIT" >/dev/null
cmake --build "$HERE/build/perf" -j --target topos_quality topos_codec >/dev/null
PY_GATE="$ROOT/.venv/bin/python"
[ -x "$PY_GATE" ] || PY_GATE="python3"
"$HERE/build/perf/topos_quality" perf quick \
  | "$PY_GATE" "$HERE/scripts/perf_gate.py" --json "$HERE/build/perf/perf_gate.json" \
  || { echo "perf 数值门禁失败（详见 build/perf/perf_gate.json）"; exit 1; }
"$HERE/build/perf/topos_quality" depth12 | grep -q "depth12: OK" \
  || { echo "depth12 冒烟失败"; exit 1; }
echo "perf gate + depth12: OK"

echo "=== 阶段 10：健壮性检查 ==="
# SDK 独立示例（encode → mux → demux → decode roundtrip；SDK.md 的可执行版）
if bash "$HERE/scripts/build_sdk.sh" --smoke >/dev/null; then
  echo "SDK 示例 roundtrip: OK"
else
  echo "SDK 示例构建/roundtrip 失败"; exit 1
fi

if [ "${1:-}" != "--skip-python" ]; then
  PY="${TOPOS_PYTHON:-$ROOT/.venv/bin/python}"
  if [ -x "$PY" ] || [ -n "${TOPOS_PYTHON:-}" ]; then
    echo "=== Python 绑定 + 阶段 6/7/8/10 + R1–R5 + TPIC image 接入冒烟 ==="
    TOPOS_CODEC_LIB="$HERE/build/debug/topos_codec.dylib" \
      "$PY" -m pytest "$ROOT/tests/native" \
        "$ROOT/tests/media/test_topos_source.py" \
        "$ROOT/tests/media/test_topos_alpha_pipeline.py" \
        "$ROOT/tests/media/test_topos_export.py" \
        "$ROOT/tests/media/test_topos_timeline_export.py" \
        "$ROOT/tests/media/test_topos_image_source.py" \
        "$ROOT/tests/media/test_topos_image_sequence.py" \
        "$ROOT/tests/media/test_capability_manifest.py" -q
    TOPOS_CODEC_LIB="$HERE/build/debug/topos_codec.dylib" \
      "$PY" "$HERE/examples/topos_roundtrip.py" \
      || { echo "Python SDK 示例失败"; exit 1; }
    echo "Python SDK 示例: OK"
  else
    echo "Python 绑定冒烟跳过（无 .venv）"
  fi
fi

echo "ALL STAGE-10 CHECKS PASSED"
