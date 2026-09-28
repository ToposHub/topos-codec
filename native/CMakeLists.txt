cmake_minimum_required(VERSION 3.16)
project(topos_codec VERSION 0.1.0 LANGUAGES C)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(TOPOS_ENABLE_TESTS "Build unit tests and fuzz replay driver" ON)
option(TOPOS_BUILD_OIIO_PLUGIN "Build OpenImageIO plugin for .toos" OFF)
# V 代际收纳（D2，2026-09-13）：退役代际读端回放（考古用）。
# 默认 OFF = 生产语义（退役代一律 UNSUPPORTED_VERSION，零回放面）；
# ON 时仍需运行期 TOPOS_DEV=1 才放行（frame_header.c 双重门）。
option(TOPOS_DEV_REPLAY "Enable retired-generation replay (needs TOPOS_DEV=1 at runtime)" OFF)
option(TOPOS_ENABLE_FUZZING "Build libFuzzer targets (clang, needs -fsanitize=fuzzer)" OFF)

# P0-04：configure 时自动注入 git stamp（tc_version() 可追溯性）。
# - 每次 configure 都重新探测 `git rev-parse --short HEAD`（脏树加 "-dirty"）
#   并 FORCE 刷新缓存。守卫式写法（cache 非空即跳过）会让 commit 之后旧
#   build 目录永远保留陈旧戳，benchmark manifest 断言（戳==HEAD）随之失败；
# - git 探测失败时沿用缓存已有值（含显式 -D 传入值）；全无来源 → "unknown"
#   （benchmark manifest 断言据此 fail）。
set(TOPOS_GIT_COMMIT_AUTO "")
find_package(Git QUIET)
if(Git_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE TOPOS_GIT_COMMIT_AUTO
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE TOPOS_GIT_COMMIT_RC)
  if(TOPOS_GIT_COMMIT_RC EQUAL 0 AND TOPOS_GIT_COMMIT_AUTO)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} diff-index --quiet HEAD --
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      ERROR_QUIET
      RESULT_VARIABLE TOPOS_GIT_DIRTY_RC)
    if(NOT TOPOS_GIT_DIRTY_RC EQUAL 0)
      string(APPEND TOPOS_GIT_COMMIT_AUTO "-dirty")
    endif()
    set(TOPOS_GIT_COMMIT "${TOPOS_GIT_COMMIT_AUTO}" CACHE STRING
        "git commit stamp baked into tc_version()" FORCE)
  endif()
endif()
if(NOT TOPOS_GIT_COMMIT)
  set(TOPOS_GIT_COMMIT "unknown" CACHE STRING
      "git commit stamp baked into tc_version()" FORCE)
endif()
message(STATUS "topos_codec: TOPOS_GIT_COMMIT = ${TOPOS_GIT_COMMIT}")

# ---- 库：阶段 1 安全基建 + 阶段 2 变换/量化 + 阶段 3 位流/熵 + 阶段 4 完整 codec
#        + 阶段 9 SIMD 运行时分发（transform_avx2/neon 按宿主架构编入） ----
add_library(topos_codec SHARED
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
  src/simd/dispatch.c)

# SIMD TU 均编入：按编译切片的架构宏各自取舍（universal 构建各出一半；
# 非匹配架构的切片编译为空 —— dispatch.c 同样按切片宏选择后端符号）
target_sources(topos_codec PRIVATE
  src/simd/transform_avx2.c
  src/simd/color_convert_avx2.c
  src/simd/planar_convert_avx2.c
  src/simd/transform_neon.c)

# P1-12：color_convert 与 numpy 参考实现的逐位一致契约依赖「禁 FMA 收缩」
# （乘加合并会用无限精度中间值，改变最后一个 ulp → rint 边界翻转）。MSVC
# 默认 /fp:precise 已不收缩，无需显式 flag。
set_source_files_properties(src/common/color_convert.c PROPERTIES
  COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/fp:precise,-ffp-contract=off>")
if(MSVC)
  # 阶段 10：MSVC 无函数级 target 属性 —— 该 TU 整体以 AVX2 编译，
  # 是否调用由 dispatch 的运行时 cpuid 判定（无 AVX2 CPU 不受影响）
  set_source_files_properties(src/simd/transform_avx2.c PROPERTIES
    COMPILE_OPTIONS "/arch:AVX2")
  # P-速③b：cvt AVX2 内核同规则（整 TU /arch:AVX2；调用侧按 cpuid 分发）
  set_source_files_properties(src/simd/color_convert_avx2.c PROPERTIES
    COMPILE_OPTIONS "/arch:AVX2")
  set_source_files_properties(src/simd/planar_convert_avx2.c PROPERTIES
    COMPILE_OPTIONS "/arch:AVX2")
endif()

target_include_directories(topos_codec
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Topos Image 阶段 8：OIIO 插件（可选；需在 topos_codec 目标定义之后引入）
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/oiio_plugin.cmake)

target_compile_definitions(topos_codec PRIVATE
  TOPOS_GIT_COMMIT="${TOPOS_GIT_COMMIT}"
  TOPOS_BUILD_TARGET="${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_C_COMPILER_ID}")

# V 代际收纳（D2，2026-09-13）：退役代际读端回放编译面（闸门实现于
# frame_header.c；ON 后仍需运行期 TOPOS_DEV=1 双重门）。
if(TOPOS_DEV_REPLAY)
  target_compile_definitions(topos_codec PRIVATE TOPOS_DEV_REPLAY)
endif()

if(MSVC)
  # 源码为 UTF-8（中文注释）：MSVC 默认跟随系统代码页（中文系统为 CP936），
  # 会把注释/字符串里的多字节序列误读为续行反斜杠（C4819/C2001）。
  # C11 <stdatomic.h>（tpool/rice/vlc 等）在 MSVC 需显式开启；clang-cl 用
  # clang 自带头文件原生支持 C11 atomics，该 flag 会被它当作未知参数告警。
  # 两处 flag 同时挂全局与 topos_codec 目标：该目标带 per-source
  # /arch:AVX2 属性时，VS 生成器曾被观测到丢弃全局 add_compile_options
  if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/utf-8 /experimental:c11atomics)
    target_compile_options(topos_codec PRIVATE
      /W4 /permissive- /utf-8 /experimental:c11atomics)
    # Windows 默认主线程栈 1MB（POSIX 8MB）：golden_v9/v73 的 run_sequence
    # 栈上 gframe×4（≈120KB）叠编码/解码内部帧栈在 1MB 下溢出（实测
    # 0xC00000FD）。tpool worker 以 stack=0 创建、继承 exe PE 头栈大小，
    # 故对 exe 链接 8MB 同时覆盖主线程与 worker；对 DLL 该选项无副作用。
    add_link_options(/STACK:8388608)
  else()
    add_compile_options(/utf-8)
    target_compile_options(topos_codec PRIVATE /W4 /permissive- /utf-8)
    if(MINGW)
      # MinGW 默认 2MB，同因对齐 8MB（ld 语法）
      add_link_options(-Wl,--stack,8388608)
    endif()
  endif()
else()
  # 阶段 1 门禁：核心库零告警（-Wconversion 保证外部 size 算术必须显式）
  target_compile_options(topos_codec PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror)
  if(MINGW)
    # MinGW/GCC（Windows）：__int128 扩展（-pedantic）、Win32 API 参数
    # 窄化（-Wconversion）与 GCC 特有启发式告警（type-limits /
    # maybe-uninitialized）降级为非致命，其余门禁保持原样
    target_compile_options(topos_codec PRIVATE
      -Wno-error=pedantic -Wno-error=conversion -Wno-error=sign-conversion
      -Wno-error=type-limits -Wno-error=maybe-uninitialized)
    # DLL 自包含：GCC/MinGW 需要静态链入 libgcc/winpthread（避免运行时
    # 依赖 MSYS2 PATH）。Zig 的 MinGW 兼容驱动不应接收 `-static`：Zig
    # 0.16 会把它解释为静态产物模式，并与 CMake 的 `-shared`/Windows
    # import-lib 选项冲突，最终把目标写成 `!<arch>` 静态归档（虽命名为
    # .dll，ctypes 加载时会报 WinError 193）。Zig 自带运行时，不需要
    # libgcc 静态链入，因此只对真正的 GCC/MinGW 使用这两个选项。
    get_filename_component(TOPOS_C_COMPILER_NAME "${CMAKE_C_COMPILER}" NAME)
    if(TOPOS_C_COMPILER_NAME MATCHES "^[Zz][Ii][Gg](\\.exe)?$")
      message(STATUS "topos_codec: Zig C compiler detected; keep shared DLL link mode")
      # Zig 0.16 基于 clang 21；项目的 GCC 专属
      # -Werror=maybe-uninitialized 在 clang 中只是未知告警，不能让它
      # 触发全局 -Werror。把兼容开关放在目标内，普通 GCC/MSVC 门禁不变。
      target_compile_options(topos_codec PRIVATE
        -Wno-unknown-warning-option -Wno-error=unused-parameter)
      # Zig 的 clang 前端没有 GCC 的 __cpu_model 运行时符号。cpudetect.c
      # 在 Windows 上改用 intrin.h 的 CPUID/XGETBV 路径；为 XGETBV 内建
      # 开启 xsave 目标特性（运行时仍先检查 OSXSAVE）。
      set_source_files_properties(src/common/cpudetect.c PROPERTIES
        COMPILE_OPTIONS "-mxsave")
    else()
      target_link_options(topos_codec PRIVATE
        -static-libgcc -static)
    endif()
  endif()
endif()

if(NOT WIN32)
  target_link_libraries(topos_codec PRIVATE pthread)
endif()

set_target_properties(topos_codec PROPERTIES
  OUTPUT_NAME topos_codec
  PREFIX ""
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  # 公共头无 __declspec(dllexport)（mac/Linux 依赖默认可见性）；MSVC 不会
  # 自动导出符号，必须显式打开（MinGW 忽略该属性，走自身 auto-export）
  WINDOWS_EXPORT_ALL_SYMBOLS ON)

# 供独立 C/C++ 项目通过 find_package(ToposCodec CONFIG) 接入。
set(TOPOS_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/ToposCodec")
install(TARGETS topos_codec EXPORT ToposCodecTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/topos_codec.h include/topos_codec_version.h
              include/topos_image.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE NOTICE.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/topos_codec)
install(EXPORT ToposCodecTargets
  FILE ToposCodecTargets.cmake
  NAMESPACE ToposCodec::
  DESTINATION ${TOPOS_CMAKE_INSTALL_DIR})
configure_package_config_file(
  cmake/ToposCodecConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/ToposCodecConfig.cmake"
  INSTALL_DESTINATION ${TOPOS_CMAKE_INSTALL_DIR})
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/ToposCodecConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/ToposCodecConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/ToposCodecConfigVersion.cmake"
  DESTINATION ${TOPOS_CMAKE_INSTALL_DIR})

# ---- topos_inspect：elementary packet 诊断 CLI（阶段 3 交付物，随库常驻构建）----
add_executable(topos_inspect src/cli/inspect.c)
target_link_libraries(topos_inspect PRIVATE topos_codec)
if(NOT MSVC)
  target_compile_options(topos_inspect PRIVATE -Wall -Wextra)
endif()

# ---- toos：Topos Image 静态图 CLI（阶段 4 交付物，随库常驻构建）----
add_executable(toos src/cli/image_cli.c)
target_link_libraries(toos PRIVATE topos_codec)
target_include_directories(toos PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(NOT MSVC)
  target_compile_options(toos PRIVATE -Wall -Wextra)
endif()

if(TOPOS_ENABLE_TESTS)
  enable_testing()

  set(TOPOS_UNIT_TESTS
    abi abi_compat checked bufview endian error cpudetect
    transform transform16 quant plane base_scale base_encoder base_residual base_decoder base_policy range
    crc32 bitio rice vlc rans rans2_slice ctx_ceiling block_coding frame_header slice_codec packet codec frame_scenario mov mov_audio stage9 oom r6_perf
    robust_io stress o4_pool_once m10 tpool_stats enc_cache_stats color_convert c1_scratch c2_sized_hint retired_read
    image_container image_reader image_writer half_map v7_scalable pyramid_transform v8_format v9_format v9_codec v7r3_format deblock out_atomicity mode_domain slot_watermark)

  # 测试支撑库：确定性 packet 合成器 + 图像合成器 + IO 故障注入 + TPIM 文件合成器
  #            （单测/fuzz corpus/golden 共用）
  add_library(topos_test_support STATIC
    tests/support/packet_synth.c
    tests/support/image_synth.c
    tests/support/fault_io.c
    tests/support/image_file_synth.c)
  target_include_directories(topos_test_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(topos_test_support PUBLIC topos_codec)
  if(NOT MSVC)
    target_compile_options(topos_test_support PRIVATE -Wall -Wextra)
  endif()

  # RD2-06：同一确定性量化 corpus 上比较 V2 VLC 与 V7-A band 语法。
  add_executable(topos_v7_size_gate tests/benchmark/v7_size_gate.c)
  target_include_directories(topos_v7_size_gate PRIVATE
    \${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(topos_v7_size_gate PRIVATE topos_test_support)
  if(NOT MSVC)
    target_compile_options(topos_v7_size_gate PRIVATE -Wall -Wextra)
  endif()

  # RD4-08：V2 全帧与 <=2K scalable base 原语的体积/编码耗时测量。
  # 内部 V7-B reference mux 已有独立单测，但最终 writer 仍需通过质量/速度门。
  add_executable(topos_scalable_encode_gate tests/benchmark/scalable_encode_gate.c)
  target_include_directories(topos_scalable_encode_gate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(topos_scalable_encode_gate PRIVATE topos_test_support)
  if(NOT MSVC)
    target_link_libraries(topos_scalable_encode_gate PRIVATE m)
  endif()
  # P4-02 / 计划 §6.3：codec-only 2K 预览解码对比（XK base-only vs 正常 2K）。
  add_executable(topos_scalable_preview_decode tests/benchmark/scalable_preview_decode.c)
  target_include_directories(topos_scalable_preview_decode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(topos_scalable_preview_decode PRIVATE topos_test_support)
  if(NOT MSVC)
    target_compile_options(topos_scalable_preview_decode PRIVATE -Wall -Wextra)
  endif()

  # 2026-09-10：分辨率 × 档位解码速率矩阵（产品 target_bpp 档位语义，
  # sized 搜索 qp 0..95 含 v1.5 域；ADR-C031 配套基准）。
  add_executable(topos_tier_decode_matrix tests/benchmark/tier_decode_matrix.c)
  target_include_directories(topos_tier_decode_matrix PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(topos_tier_decode_matrix PRIVATE topos_test_support)
  if(NOT MSVC)
    target_compile_options(topos_tier_decode_matrix PRIVATE -Wall -Wextra)
  endif()
  if(NOT MSVC)
    target_compile_options(topos_scalable_encode_gate PRIVATE -Wall -Wextra)
  endif()

  # RD3-03：固定 1/3 相位表 vs 通用 sampled 坐标/写回微基准。
  add_executable(topos_third_path_bench tests/benchmark/third_path.c)
  target_include_directories(topos_third_path_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(topos_third_path_bench PRIVATE topos_codec)
  if(NOT MSVC)
    target_compile_options(topos_third_path_bench PRIVATE -Wall -Wextra)
  endif()

  foreach(t IN LISTS TOPOS_UNIT_TESTS)
    add_executable(test_${t} tests/unit/test_${t}.c)
    target_include_directories(test_${t} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_link_libraries(test_${t} PRIVATE topos_test_support)
    if(NOT MSVC)
      target_compile_options(test_${t} PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME unit_${t} COMMAND test_${t})
  endforeach()

  target_compile_definitions(test_retired_read PRIVATE
    TOPOS_CORPUS_RETIRED_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus_retired")

  # V 代际收纳（D1，2026-09-13）：V7-A band 专属测试仅在回放构建编译注册
  # （生产构建零 V7-A 面）。测试 main 首行自带 setenv("TOPOS_DEV","1")。
  if(TOPOS_DEV_REPLAY)
    set(TOPOS_V7A_TESTS band_codec band_tokens v7_band_decode v7_frame_codec v7_frame_encoder layer_directory)
    foreach(t IN LISTS TOPOS_V7A_TESTS)
      add_executable(test_${t} tests/unit/test_${t}.c)
      target_include_directories(test_${t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
      target_link_libraries(test_${t} PRIVATE topos_test_support)
      if(NOT MSVC)
        target_compile_options(test_${t} PRIVATE -Wall -Wextra)
      endif()
      add_test(NAME unit_${t} COMMAND test_${t})
    endforeach()
  endif()

  # pthread（test_error 线程局部错误隔离 / test_robust_io 并发读契约）
  if(NOT WIN32)
    target_link_libraries(test_error PRIVATE pthread)
    target_link_libraries(test_robust_io PRIVATE pthread)
  endif()

  # C++ 兼容 TU（阶段 10）：公共头可被 C++ 集成方直接使用（无 C++ 工具链时跳过）
  include(CheckLanguage)
  check_language(CXX)
  if(CMAKE_CXX_COMPILER)
    enable_language(CXX)
    add_executable(test_abi_cpp tests/unit/test_abi_cpp.cpp)
    target_link_libraries(test_abi_cpp PRIVATE topos_codec)
    add_test(NAME unit_abi_cpp COMMAND test_abi_cpp)
  endif()

  # 阶段 2 golden vectors：conformance 校验（golden 文件入库；有意变更时手动重生成）
  add_executable(topos_golden_transform tests/conformance/golden_transform.c)
  target_include_directories(topos_golden_transform PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance)
  target_link_libraries(topos_golden_transform PRIVATE topos_codec)
  if(NOT MSVC)
    target_compile_options(topos_golden_transform PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME conformance_transform
    COMMAND topos_golden_transform check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_transform_v1.bin)

  # 阶段 3 golden bitstream：56 条记录（header/Rice 流/块 payload/完整 packet）字节级冻结
  add_executable(topos_golden_bitstream tests/conformance/golden_bitstream.c)
  target_include_directories(topos_golden_bitstream PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(topos_golden_bitstream PRIVATE topos_test_support)
  if(NOT MSVC)
    target_compile_options(topos_golden_bitstream PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME conformance_bitstream
    COMMAND topos_golden_bitstream check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_bitstream_v1.bin)

  # 阶段 4 golden codec：完整链路（packet 字节 + 解码平面指纹）字节级冻结
  add_executable(topos_golden_codec tests/conformance/golden_codec.c)
  target_include_directories(topos_golden_codec PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(topos_golden_codec PRIVATE topos_test_support)
  if(NOT MSVC)
    target_compile_options(topos_golden_codec PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME conformance_codec
    COMMAND topos_golden_codec check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_codec_v1.bin)
  # R4.1：12-bit YUV 4:2:2 扩展集（v1.2 枚举；独立文件，v1 保持字节冻结）
  add_test(NAME conformance_codec_bd12
    COMMAND topos_golden_codec check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_codec_v1_bd12.bin bd12)
  # R4.2：YUV 4:4:4 扩展集（v1.3 枚举，pf=1/minor=2；独立文件，前两文件冻结）
  add_test(NAME conformance_codec_pf444
    COMMAND topos_golden_codec check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_codec_v1_pf444.bin pf444)
  # R4.3：GBR 4:4:4 扩展集（v1.4 枚举，pf=2/minor=3/matrix=0；独立文件）
  add_test(NAME conformance_codec_gbr
    COMMAND topos_golden_codec check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_codec_v1_gbr.bin gbr)
  # R4.4：Pro444/Extreme profile 扩展集（独立文件）
  add_test(NAME conformance_codec_profiles
    COMMAND topos_golden_codec check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_codec_v1_profiles.bin profiles)

  # M9：V2 canonical VLC golden（golden_codec_v2_vlc.bin；spec v2 §5）
  add_test(NAME conformance_codec_vlc
    COMMAND topos_golden_codec check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_codec_v2_vlc.bin vlc)
  # V 代际收纳（2026-09-13）：V3 intra golden 归档至 tests/fuzz/corpus_retired/
  # （TOPOS_DEV 回放用），同一 case 网格在 V7-R2（cfg em=8）上重建等价覆盖
  add_test(NAME conformance_codec_v7r2_intra
    COMMAND topos_golden_codec check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_codec_v7r2_intra.bin intra-r2)

  # v1.6（TRAW 批 1）：CFA 4 相位平面 12-bit 扩展集（独立文件）
  add_test(NAME conformance_codec_traw
    COMMAND topos_golden_codec check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_codec_v1_traw.bin traw)

  # V8 golden（格式计划批 5）：真实编码 V8 packet 字节 + 解码像素双冻结
  add_executable(topos_golden_v8 tests/conformance/golden_v8.c)
  target_include_directories(topos_golden_v8 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance)
  target_link_libraries(topos_golden_v8 PRIVATE topos_codec)
  if(NOT MSVC)
    target_compile_options(topos_golden_v8 PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME conformance_v8
    COMMAND topos_golden_v8 check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_v8_v1.bin)

  # V9 zero-motion IP-2 conformance（micro-gop 计划批 2）：重编码逐字节 +
  # 帧型/gop 序列 + 零漂锚（参考精确重复 → recon 逐位不变）+ 回退序列
  add_executable(topos_golden_v9 tests/conformance/golden_v9.c)
  target_include_directories(topos_golden_v9 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(topos_golden_v9 PRIVATE topos_codec)
  if(NOT MSVC)
    target_compile_options(topos_golden_v9 PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME conformance_v9
    COMMAND topos_golden_v9 check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_v9_v1.bin
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_mov_v9_v1.bin)

  # V7-R3 zero-motion IP-2 conformance（ADR-C048 载体更换批 2）：V9 同构
  # 纪律，载体 (major 7, em 8)；MOV 段随批 3 扩展
  add_executable(topos_golden_v73 tests/conformance/golden_v73.c)
  target_include_directories(topos_golden_v73 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(topos_golden_v73 PRIVATE topos_codec)
  if(NOT MSVC)
    target_compile_options(topos_golden_v73 PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME conformance_v73
    COMMAND topos_golden_v73 check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_v73_v1.bin
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_mov_v73_v1.bin)

  # Topos Image 阶段 1 golden：.toos 文件字节冻结（docs/image/test_vector_manifest.json）
  add_executable(topos_golden_image tests/conformance/golden_image.c)
  target_include_directories(topos_golden_image PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(topos_golden_image PRIVATE topos_test_support)
  if(NOT MSVC)
    target_compile_options(topos_golden_image PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME conformance_image
    COMMAND topos_golden_image check ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance)

  # 阶段 4 CLI 冒烟：golden → probe/verify/deep/decode/benchmark
  add_test(NAME cli_image_probe
    COMMAND toos probe ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_image_v1_gbr10.bin)
  add_test(NAME cli_image_verify
    COMMAND toos verify ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_image_v1_gbr10.bin --deep)
  add_test(NAME cli_image_decode
    COMMAND toos decode ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_image_v1_gbr10.bin
            -o ${CMAKE_CURRENT_BINARY_DIR}/cli_decode.raw)
  add_test(NAME cli_image_bench
    COMMAND toos benchmark ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_image_v1_gbr10.bin
            --iters 3)

  # 阶段 5 golden mov：完整文件字节 + 索引摘要双冻结（container_spec §8 确定性）
  add_executable(topos_golden_mov tests/conformance/golden_mov.c)
  # M-B6：v1.4 音频轨字节级 golden（elst/esds/V2/f32 冻结）
  add_executable(topos_golden_mov_audio tests/conformance/golden_mov_audio.c)
  target_include_directories(topos_golden_mov_audio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(topos_golden_mov_audio PRIVATE topos_codec
    topos_test_support)
  target_include_directories(topos_golden_mov PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(topos_golden_mov PRIVATE topos_test_support)
  if(NOT MSVC)
    target_compile_options(topos_golden_mov PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME conformance_mov
    COMMAND topos_golden_mov check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_mov_v1.bin)
  add_test(NAME conformance_mov_audio
    COMMAND topos_golden_mov_audio check
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_mov_audio_v1.bin)

  # 位流 fuzz corpus（提交入库；gen 工具 -dump 重建）
  set(TOPOS_BITSTREAM_CORPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/seed_corpus_bitstream)

  # fuzz：所有配置都跑确定性回放 driver（含 seed corpus + 内置生成）
  add_executable(topos_fuzz_replay
    tests/fuzz/corpus_replay.c
    tests/fuzz/fuzz_primitives.c)
  target_include_directories(topos_fuzz_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(topos_fuzz_replay PRIVATE topos_codec)
  if(NOT MSVC)
    target_compile_options(topos_fuzz_replay PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME fuzz_replay COMMAND topos_fuzz_replay -gen)

  # 阶段 3：位流 fuzz（随机 LCG 回放 + corpus 自检门：种子必须全通过、变异不越不变量）
  add_executable(topos_fuzz_replay_bitstream
    tests/fuzz/corpus_replay.c
    tests/fuzz/fuzz_bitstream.c)
  target_include_directories(topos_fuzz_replay_bitstream PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(topos_fuzz_replay_bitstream PRIVATE topos_codec)
  if(NOT MSVC)
    target_compile_options(topos_fuzz_replay_bitstream PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME fuzz_bitstream_replay COMMAND topos_fuzz_replay_bitstream -gen)

  # V8 位流 + 解码 fuzz（格式计划批 5）：合成路径保证每输入深走 V8 全路径
  add_executable(topos_fuzz_replay_v8
    tests/fuzz/corpus_replay.c
    tests/fuzz/fuzz_v8.c)
  target_include_directories(topos_fuzz_replay_v8 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(topos_fuzz_replay_v8 PRIVATE topos_codec)
  if(NOT MSVC)
    target_compile_options(topos_fuzz_replay_v8 PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME fuzz_v8_replay COMMAND topos_fuzz_replay_v8 -gen 512)

  # V9 位流批 1 骨架 fuzz：合成 V8 包头部补丁成 V9（同构继承）→ 查询/
  # observe/实解码 NOT_IMPLEMENTED 契约 + 变异/截断定义集不变量
  add_executable(topos_fuzz_replay_v9
    tests/fuzz/corpus_replay.c
    tests/fuzz/fuzz_v9.c)
  target_include_directories(topos_fuzz_replay_v9 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(topos_fuzz_replay_v9 PRIVATE topos_codec)
  if(NOT MSVC)
    target_compile_options(topos_fuzz_replay_v9 PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME fuzz_v9_replay COMMAND topos_fuzz_replay_v9 -gen 512)

  # 阶段 4：完整解码路径 fuzz（随机输入 + concealment/重建稳健性）
  add_executable(topos_fuzz_replay_codec
    tests/fuzz/corpus_replay.c
    tests/fuzz/fuzz_codec.c)
  target_include_directories(topos_fuzz_replay_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(topos_fuzz_replay_codec PRIVATE topos_codec)
  if(NOT MSVC)
    target_compile_options(topos_fuzz_replay_codec PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME fuzz_codec_replay COMMAND topos_fuzz_replay_codec -gen)
  add_executable(topos_fuzz_intra_replay tests/fuzz/intra_replay.c tests/fuzz/fuzz_codec.c)
  target_include_directories(topos_fuzz_intra_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(topos_fuzz_intra_replay PRIVATE topos_codec)
  add_test(NAME fuzz_intra_replay COMMAND topos_fuzz_intra_replay
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/golden_codec_v7r2_intra.bin)

  # 阶段 5：MOV 容器层 fuzz（任意字节 → open/索引/sample 读取/faststart 往返不变量）
  add_executable(topos_fuzz_replay_mov
    tests/fuzz/corpus_replay.c
    tests/fuzz/fuzz_mov.c)
  target_include_directories(topos_fuzz_replay_mov PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(topos_fuzz_replay_mov PRIVATE topos_codec)
  if(NOT MSVC)
    target_compile_options(topos_fuzz_replay_mov PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME fuzz_mov_replay COMMAND topos_fuzz_replay_mov -gen)
  # M-B2：音频种子回放（合法双轨文件走通 open/读取/faststart 成功分支）
  add_test(NAME fuzz_mov_audio_seeds COMMAND topos_fuzz_replay_mov
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/seed_corpus/seed_audio_lpcm16_48k.mov
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/seed_corpus/seed_audio_lpcm24_44k.mov
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/seed_corpus/seed_audio_mp4a_48k.mov
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/seed_corpus/seed_audio_mp4a_primed.mov
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/seed_corpus/seed_audio_mp4a_48k_fs.mov
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/seed_corpus/seed_audio_multitrack.mov
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/seed_corpus/seed_audio_tmcd.mov)

  # Topos Image 阶段 1：TPIM 容器层 fuzz（任意字节 + 合法文件变异 → 不变量）
  add_executable(topos_fuzz_replay_image
    tests/fuzz/corpus_replay.c
    tests/fuzz/fuzz_image.c)
  target_include_directories(topos_fuzz_replay_image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(topos_fuzz_replay_image PRIVATE topos_test_support)
  if(NOT MSVC)
    target_compile_options(topos_fuzz_replay_image PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME fuzz_image_replay COMMAND topos_fuzz_replay_image -gen)

  # 阶段 10：编码器 fuzz（合法域对抗输入 → 确定性/自家解码/sized 有界）
  add_executable(topos_fuzz_replay_encode
    tests/fuzz/corpus_replay.c
    tests/fuzz/fuzz_encode.c)
  target_include_directories(topos_fuzz_replay_encode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(topos_fuzz_replay_encode PRIVATE topos_codec)
  if(NOT MSVC)
    target_compile_options(topos_fuzz_replay_encode PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME fuzz_encode_replay COMMAND topos_fuzz_replay_encode -gen)

  add_executable(topos_gen_bitstream_corpus
    tests/fuzz/gen_bitstream_corpus.c
    tests/fuzz/fuzz_bitstream.c)
  target_include_directories(topos_gen_bitstream_corpus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(topos_gen_bitstream_corpus PRIVATE topos_test_support)
  if(NOT MSVC)
    target_compile_options(topos_gen_bitstream_corpus PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME fuzz_bitstream_corpus
    COMMAND topos_gen_bitstream_corpus -selftest ${TOPOS_BITSTREAM_CORPUS_DIR})

  # topos_inspect 冒烟：合法种子 → 0；截断样本 → 非零（WILL_FAIL 反转）
  if(EXISTS ${TOPOS_BITSTREAM_CORPUS_DIR}/seed_cfg0.bin)
    add_test(NAME inspect_cli_ok
      COMMAND topos_inspect ${TOPOS_BITSTREAM_CORPUS_DIR}/seed_cfg0.bin)
    add_test(NAME inspect_cli_truncated
      COMMAND topos_inspect ${TOPOS_BITSTREAM_CORPUS_DIR}/trunc_cfg0_8.bin)
    set_tests_properties(inspect_cli_truncated PROPERTIES WILL_FAIL TRUE)
  endif()

  # 阶段 5 CLI 交付物：topos_encoder_cli / topos_decoder_cli / topos_probe_cli（+ rawgen 辅助）
  add_executable(topos_encoder_cli src/cli/encoder_cli.c)
  target_link_libraries(topos_encoder_cli PRIVATE topos_codec)
  add_executable(topos_decoder_cli src/cli/decoder_cli.c)
  target_link_libraries(topos_decoder_cli PRIVATE topos_codec)
  add_executable(topos_probe_cli src/cli/probe_cli.c)
  target_link_libraries(topos_probe_cli PRIVATE topos_codec)
  add_executable(topos_rawgen src/cli/rawgen_cli.c)
  target_link_libraries(topos_rawgen PRIVATE topos_test_support)
  # P0-03：冻结 scalable 基准 corpus 生成器（raw 平面 + V2 质量参考工件）
  add_executable(topos_scalable_corpus src/cli/scalable_corpus_cli.c)
  target_link_libraries(topos_scalable_corpus PRIVATE topos_test_support topos_codec)
  target_compile_definitions(topos_scalable_corpus PRIVATE
    TOPOS_GIT_COMMIT="${TOPOS_GIT_COMMIT}")
  if(NOT MSVC)
    foreach(cli IN ITEMS topos_encoder_cli topos_decoder_cli topos_probe_cli
                       topos_rawgen topos_scalable_corpus)
      target_compile_options(${cli} PRIVATE -Wall -Wextra)
    endforeach()
  endif()

  # topos_quality：阶段 4 画质/码率/速度测量与 QM 调优（dev 工具，随测试构建）
  add_executable(topos_quality src/cli/quality.c)
  target_include_directories(topos_quality PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(topos_quality PRIVATE topos_test_support)
  # M10-0.3：报告元数据注入真实构建参数（此前硬编码 "-O2 -DNDEBUG" 与
  # Release 实际的 -O3 不符，曾诱导按错误口径做 O2/O3 收益判断）
  string(TOUPPER "${CMAKE_BUILD_TYPE}" _topos_report_cfg)
  target_compile_definitions(topos_quality PRIVATE
    TOPOS_REPORT_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    TOPOS_REPORT_BUILD_FLAGS="${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${_topos_report_cfg}}"
    TOPOS_GIT_COMMIT="${TOPOS_GIT_COMMIT}")
  unset(_topos_report_cfg)
  if(NOT MSVC)
    target_compile_options(topos_quality PRIVATE -Wall -Wextra)
  endif()
  # POSIX 需 -lm；Windows 数学函数在 CRT 内（MSVC/clang-cl 无 m.lib）
  if(NOT WIN32)
    target_link_libraries(topos_quality PRIVATE m)
  endif()

  if(TOPOS_ENABLE_FUZZING)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
    check_c_source_compiles("int main(void){return 0;}" TOPOS_HAVE_LIBFUZZER)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(TOPOS_HAVE_LIBFUZZER)
      add_executable(topos_fuzz_primitives tests/fuzz/fuzz_primitives.c)
      target_include_directories(topos_fuzz_primitives PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
      target_link_libraries(topos_fuzz_primitives PRIVATE topos_codec)
      target_compile_options(topos_fuzz_primitives PRIVATE -fsanitize=fuzzer-no-link)
      target_link_options(topos_fuzz_primitives PRIVATE -fsanitize=fuzzer)

      add_executable(topos_fuzz_bitstream tests/fuzz/fuzz_bitstream.c)
      target_include_directories(topos_fuzz_bitstream PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
      target_link_libraries(topos_fuzz_bitstream PRIVATE topos_codec)
      target_compile_options(topos_fuzz_bitstream PRIVATE -fsanitize=fuzzer-no-link)
      target_link_options(topos_fuzz_bitstream PRIVATE -fsanitize=fuzzer)

      add_executable(topos_fuzz_codec tests/fuzz/fuzz_codec.c)
      target_include_directories(topos_fuzz_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
      target_link_libraries(topos_fuzz_codec PRIVATE topos_codec)
      target_compile_options(topos_fuzz_codec PRIVATE -fsanitize=fuzzer-no-link)
      target_link_options(topos_fuzz_codec PRIVATE -fsanitize=fuzzer)

      add_executable(topos_fuzz_mov tests/fuzz/fuzz_mov.c)
      target_include_directories(topos_fuzz_mov PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
      target_link_libraries(topos_fuzz_mov PRIVATE topos_codec)
      target_compile_options(topos_fuzz_mov PRIVATE -fsanitize=fuzzer-no-link)
      target_link_options(topos_fuzz_mov PRIVATE -fsanitize=fuzzer)

      add_executable(topos_fuzz_image tests/fuzz/fuzz_image.c)
      target_include_directories(topos_fuzz_image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
      target_link_libraries(topos_fuzz_image PRIVATE topos_test_support)
      target_compile_options(topos_fuzz_image PRIVATE -fsanitize=fuzzer-no-link)
      target_link_options(topos_fuzz_image PRIVATE -fsanitize=fuzzer)

      add_executable(topos_fuzz_encode tests/fuzz/fuzz_encode.c)
      target_include_directories(topos_fuzz_encode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
      target_link_libraries(topos_fuzz_encode PRIVATE topos_codec)
      target_compile_options(topos_fuzz_encode PRIVATE -fsanitize=fuzzer-no-link)
      target_link_options(topos_fuzz_encode PRIVATE -fsanitize=fuzzer)
    else()
      message(WARNING
        "libFuzzer runtime unavailable (e.g. Apple clang 未随附 libclang_rt.fuzzer)；"
        "fuzz 覆盖由 ctest 中的确定性回放 driver（ASan/UBSan 下运行）承担")
    endif()
  endif()
endif()
