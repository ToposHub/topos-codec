#!/usr/bin/env bash
# 构建并打包 libtopos_codec SDK（阶段 10 分发）。
#
# 用法：
#   bash native/topos_codec/scripts/build_sdk.sh              # 完整打包 → dist/
#   bash native/topos_codec/scripts/build_sdk.sh --smoke      # 快速自检（示例 roundtrip，供门禁）
#
# macOS：默认 universal（x86_64+arm64，CMAKE_OSX_ARCHITECTURES）；
#   缺 arm64 SDK 时自动回退单架构并警告。
# 签名/公证（仅 macOS，环境变量门控）：
#   TOPOS_SIGN_IDENTITY  证书名或 team ID；未设 → ad-hoc（codesign --sign -）
#   TOPOS_NOTARY_PROFILE xcrun notarytool profile 名；与 identity 同设时提交公证 + staple
# 发布物：dist/topos-codec-sdk-<ver>+<commit>/{include,lib,examples,docs,SDK.md,BUILDINFO.txt,LICENSE,NOTICE.txt}
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # native/topos_codec
if [ "$(basename "$HERE")" = "topos_codec" ]; then
  ROOT="$(cd "$HERE/../.." && pwd)"
else
  ROOT="$(cd "$HERE/.." && pwd)"  # 独立开源仓库：<root>/native
fi
SMOKE=0
[ "${1:-}" = "--smoke" ] && SMOKE=1

GIT_COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
VERSION="$(awk '/TOPOS_CODEC_VERSION_MAJOR/{maj=$3} /TOPOS_CODEC_VERSION_MINOR/{min=$3} /TOPOS_CODEC_VERSION_PATCH/{pat=$3} END{printf "%s.%s.%s", maj, min, pat}' "$HERE/include/topos_codec_version.h")"
SDK_NAME="topos-codec-sdk-${VERSION}+${GIT_COMMIT}"

build_release() { # $1 = build dir, $2 = 额外 CMake 参数
  cmake -S "$HERE" -B "$1" -DCMAKE_C_COMPILER="${CC:-clang}" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
        -DTOPOS_GIT_COMMIT="$GIT_COMMIT" -DTOPOS_ENABLE_TESTS=OFF "${@:2}" >/dev/null
  cmake --build "$1" -j --target topos_codec >/dev/null
}

ensure_release_lib() { # $1 = build dir：已配置则增量构建（防陈旧库），未配置则全新配置
  if [ -f "$1/CMakeCache.txt" ]; then
    cmake --build "$1" -j --target topos_codec >/dev/null || build_release "$1"
  else
    build_release "$1"
  fi
}

stage_sdk() { # $1 = lib 文件路径, $2 = dist 目录
  local lib="$1" dist="$2"
  mkdir -p "$dist/include" "$dist/lib" "$dist/examples"
  cp "$HERE/include/topos_codec.h" "$HERE/include/topos_image.h" \
     "$HERE/include/topos_codec_version.h" "$dist/include/"
  cp "$lib" "$dist/lib/"
  if [ "$(uname)" = "Darwin" ]; then
    # 运行时名称与发布包中的实际文件名一致，别名由 package_codec.py 补齐。
    install_name_tool -id "@rpath/$(basename "$lib")" "$dist/lib/$(basename "$lib")"
  fi
  cp "$HERE/examples/encode_decode.c" "$dist/examples/"
  cp "$HERE/docs/SDK.md" "$HERE/LICENSE" "$HERE/NOTICE.txt" "$dist/"
  ABI_VERSION="$(sed -n 's/^#define TOPOS_CODEC_ABI_VERSION \([0-9]*\).*/\1/p' \
    "$HERE/include/topos_codec.h" | head -1)"
  [ -n "$ABI_VERSION" ] || ABI_VERSION=unknown
  cat > "$dist/BUILDINFO.txt" <<EOF
name:      topos-codec-sdk
version:   ${VERSION}
git:       ${GIT_COMMIT}
built:     $(date -u '+%Y-%m-%dT%H:%M:%SZ')
host:      $(cc -dumpmachine 2>/dev/null || echo unknown)
lib:       $(basename "$lib")
abi:       v${ABI_VERSION}（TOPOS_CODEC_ABI_VERSION，从头文件解析——不手写，复验 P2-08）
known-limitations: 音频轨可封装，AAC/LPCM 音频编码由宿主提供；应用 sized 路径 1080p 为离线/近实时档、4K=代理+离线交付
  （benchmark_protocol §9，复验 P0-05）；Windows/MSVC CI job 已 required 化（R7），
  首绿证据以真实 runner 记录为准（ADR-C010/C025）。
EOF
}

verify_example() { # $1 = dist 目录
  local dist="$1" tmp staged_lib
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN
  staged_lib="$(find "$dist/lib" -maxdepth 1 -type f -print -quit)"
  [ -n "$staged_lib" ] || { echo "发布包中没有动态库"; return 1; }
  ( cc "$dist/examples/encode_decode.c" -I "$dist/include" \
      "$staged_lib" -Wl,-rpath,"$dist/lib" \
      -o "$tmp/encode_decode" ) \
      || { echo "示例编译失败"; return 1; }
  ( cd "$tmp" && DYLD_LIBRARY_PATH="$dist/lib" LD_LIBRARY_PATH="$dist/lib" \
      ./encode_decode "$tmp/demo.mov" | grep -q "PASS" ) || { echo "示例 roundtrip 失败"; return 1; }
}

sign_and_notarize() { # $1 = dylib
  local lib="$1"
  if [ "$(uname)" != "Darwin" ]; then return 0; fi
  local identity="${TOPOS_SIGN_IDENTITY:--}"
  codesign --force --sign "$identity" "$lib" >/dev/null 2>&1 \
    || { echo "codesign 失败（identity=$identity）"; return 1; }
  echo "codesign: OK (identity=$identity)"
  if [ -n "${TOPOS_NOTARY_PROFILE:-}" ] && [ "${TOPOS_SIGN_IDENTITY:-}" != "" ]; then
    local zip="${lib}.zip"
    ditto -c -k --keepParent "$lib" "$zip"
    xcrun notarytool submit "$zip" --keychain-profile "$TOPOS_NOTARY_PROFILE" --wait
    xcrun stapler staple "$lib"
    rm -f "$zip"
    echo "notarize+staple: OK"
  fi
}

if [ "$SMOKE" -eq 1 ]; then
  # 门禁快速路径：优先复用 build/perf（Release -O2，run_tests.sh 阶段 9 段已构建）；
  # 增量重建目标，避免旧缓存库
  LIBDIR="$HERE/build/perf"
  ensure_release_lib "$LIBDIR"
  LIB="$LIBDIR/libtopos_codec.dylib"
  [ -f "$LIB" ] || LIB="$LIBDIR/topos_codec.dylib"
  [ -f "$LIB" ] || LIB="$LIBDIR/libtopos_codec.so"
  [ -f "$LIB" ] || LIB="$LIBDIR/topos_codec.so"
  DIST="$(mktemp -d)"
  trap 'rm -rf "$DIST"' EXIT
  stage_sdk "$LIB" "$DIST"
  verify_example "$DIST" || exit 1
  echo "SDK 示例 roundtrip: OK"
  exit 0
fi

# ---- 完整打包 ----
command -v python3 >/dev/null 2>&1 || {
  echo "完整 SDK 打包需要 python3 生成发布清单和验证外部消费者"; exit 1;
}
BUILD="$HERE/build/sdk"
ARCH_ARGS=()
UNIVERSAL=0
if [ "$(uname)" = "Darwin" ]; then
  if clang -arch arm64 -fsyntax-only \
       "$HERE/src/simd/transform_neon.c" -I "$HERE/src" >/dev/null 2>&1 \
     && clang -arch x86_64 -fsyntax-only \
       "$HERE/src/simd/transform_neon.c" -I "$HERE/src" >/dev/null 2>&1; then
    ARCH_ARGS=(-DCMAKE_OSX_ARCHITECTURES="x86_64;arm64")
    UNIVERSAL=1
  else
    echo "警告：universal 所需架构不可用 → 单架构打包"
  fi
fi
build_release "$BUILD" "${ARCH_ARGS[@]}"

LIB="$BUILD/libtopos_codec.dylib"
[ -f "$LIB" ] || LIB="$BUILD/topos_codec.dylib"
[ -f "$LIB" ] || LIB="$BUILD/libtopos_codec.so"
[ -f "$LIB" ] || LIB="$BUILD/topos_codec.so"
[ -f "$LIB" ] || { echo "构建成功但未找到 topos_codec 动态库：$BUILD"; exit 1; }
# CMake 的无版本文件通常是软链；发布真实版本化文件及其 SONAME 别名。
LIB="$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$LIB")"
DIST="$HERE/dist/$SDK_NAME"
rm -rf "$DIST"
stage_sdk "$LIB" "$DIST"
if [ "$(uname)" = "Darwin" ]; then
  LIPO_OUT="$(lipo -archs "$LIB" 2>/dev/null || echo host)"
  echo "lib 架构: $LIPO_OUT"
fi
sign_and_notarize "$DIST/lib/$(basename "$LIB")"
verify_example "$DIST" || exit 1
# R7：发布物保留机器/编译器/commit/逐文件 sha256（RELEASE-MANIFEST.json，
# 由 package_codec.py 生成——CI release job 与本脚本共用同一实现）
python3 "$HERE/scripts/package_codec.py" --lib "$DIST/lib/$(basename "$LIB")" \
  --dist "$DIST" --build-dir "$BUILD" || exit 1
python3 "$HERE/scripts/sdk_consumer_smoke.py" "$DIST" || exit 1
echo "SDK 就绪: $DIST"
[ "$UNIVERSAL" -eq 1 ] && echo "universal dylib: OK"
exit 0
