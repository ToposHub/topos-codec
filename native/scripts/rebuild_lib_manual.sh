#!/bin/bash
# rebuild_lib_manual.sh <out-dir> [extra CFLAGS...]
# 直接用 clang 全量重编 libtopos_codec（绕过 cmake；用于无法运行 cmake 的环境）。
set -e
ROOT="/Users/heng/Documents/vscode/topos-color-multitask /topos-color01/native/topos_codec"
OUT="$1"; shift
EXTRA=("$@")
mkdir -p "$OUT"
CC=/usr/bin/clang
SRCS="src/version.c src/common/error.c src/common/alloc.c src/common/color_convert.c src/common/cpudetect.c src/common/crc32.c src/common/tpool.c src/transform/transform.c src/transform/sparse_inverse.c src/transform/quant.c src/transform/fastdiv.c src/transform/plane.c src/bitstream/bitio.c src/bitstream/frame_header.c src/bitstream/slice_map.c src/bitstream/packet.c src/bitstream/slice_codec.c src/entropy/rice.c src/entropy/vlc.c src/entropy/block_coding.c src/codec/codec.c src/codec/decoder_ctx.c src/mov/mov.c src/image/image_container.c src/image/image_reader.c src/image/image_writer.c src/simd/dispatch.c src/simd/transform_avx2.c src/simd/color_convert_avx2.c src/simd/transform_neon.c"
cd "$ROOT"
OBJS=()
for s in $SRCS; do
  EXTRA2=()
  [ "$s" = "src/common/color_convert.c" ] && EXTRA2=(-ffp-contract=off)
  OBJ="$OUT/$(echo "$s" | tr '/' '_').o"
  "$CC" -std=c11 -fPIC -Wall -Wextra "${EXTRA[@]}" "${EXTRA2[@]}" -I "$ROOT/include" -I "$ROOT/src" -c "$s" -o "$OBJ"
  OBJS+=("$OBJ")
done
"$CC" -dynamiclib -Wl,-headerpad_max_install_names -current_version 0.1.0 \
  -o "$OUT/topos_codec.0.1.0.dylib" -install_name @rpath/topos_codec.0.dylib \
  "${OBJS[@]}" -lpthread
echo "rebuilt $OUT/topos_codec.0.1.0.dylib"
