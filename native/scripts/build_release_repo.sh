#!/bin/bash
# build_release_repo.sh [extra CFLAGS...]
# 仓库相对路径版 release 直编脚本（无 cmake 依赖）。
# 输出 <repo>/native/topos_codec/build/release/topos_codec.dylib，
# 与 topos_binding.candidate_library_paths() 的首个候选名严格对齐
# （勿改成带版本号文件名，绑定层只认 topos_codec.dylib）。
# 源文件清单以 CMakeLists.txt 的 add_library(topos_codec ...) 为权威基准
# （rebuild_lib_manual.sh 的清单已过时，勿以其为准）；增删源文件两处同步。
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT="$ROOT/build/release"
CC="${CC:-/usr/bin/clang}"
BASE_FLAGS=(-std=c11 -fPIC -Wall -Wextra -O3 -DNDEBUG)
# 与 CMake release 构建一致：把 git commit 戳进 tc_version()
GIT_COMMIT="$(cd "$ROOT" && git rev-parse --short HEAD 2>/dev/null || true)"
if [ -n "$GIT_COMMIT" ] && [ -z "$(cd "$ROOT" && git status --porcelain 2>/dev/null)" ]; then :;
elif [ -n "$GIT_COMMIT" ]; then GIT_COMMIT="${GIT_COMMIT}-dirty"; fi
DEFINES=(-DTOPOS_GIT_COMMIT="\"${GIT_COMMIT:-unknown}\"")
# 顺序与 add_library 一致；simd 两个 TU 靠架构宏自行取舍（同 target_sources）
SRCS="
src/version.c
src/common/error.c
src/common/alloc.c
src/common/color_convert.c
src/common/planar_convert.c
src/common/cpudetect.c
src/common/crc32.c
src/common/tpool.c
src/transform/transform.c
src/transform/transform16.c
src/transform/sparse_inverse.c
src/transform/quant.c
src/transform/fastdiv.c
src/transform/plane.c
src/transform/base_scale.c
src/transform/pyramid_transform.c
src/bitstream/bitio.c
src/bitstream/frame_header.c
src/bitstream/slice_map.c
src/bitstream/packet.c
src/bitstream/band_tokens.c
src/bitstream/band_codec.c
src/bitstream/layer_directory.c
src/bitstream/v7_band_decode.c
src/bitstream/v7_frame_codec.c
src/bitstream/slice_codec.c
src/entropy/rice.c
src/entropy/vlc.c
src/entropy/rans.c
src/entropy/range.c
src/entropy/block_coding.c
src/codec/codec.c
src/codec/ctx_ceiling.c
src/codec/base_encoder.c
src/codec/base_residual.c
src/codec/base_decoder.c
src/codec/base_policy.c
src/codec/deblock.c
src/codec/v7_scalable.c
src/codec/intra.c
src/codec/decoder_ctx.c
src/codec/gop_context.c
src/mov/mov.c
src/image/image_container.c
src/image/image_reader.c
src/image/image_writer.c
src/image/half_map.c
src/simd/dispatch.c
src/simd/transform_avx2.c
src/simd/color_convert_avx2.c
src/simd/planar_convert_avx2.c
src/simd/transform_neon.c
"
mkdir -p "$OUT"
cd "$ROOT"
OBJS=()
for s in $SRCS; do
  EXTRA2=()
  [ "$s" = "src/common/color_convert.c" ] && EXTRA2=(-ffp-contract=off)
  OBJ="$OUT/$(echo "$s" | tr '/' '_').o"
  "$CC" "${BASE_FLAGS[@]}" "${DEFINES[@]}" "$@" "${EXTRA2[@]}" \
    -I "$ROOT/include" -I "$ROOT/src" -c "$s" -o "$OBJ"
  OBJS+=("$OBJ")
done
"$CC" -dynamiclib -Wl,-headerpad_max_install_names -current_version 0.1.0 \
  -o "$OUT/topos_codec.dylib" -install_name @rpath/topos_codec.dylib \
  "${OBJS[@]}" -lpthread
echo "rebuilt $OUT/topos_codec.dylib"
