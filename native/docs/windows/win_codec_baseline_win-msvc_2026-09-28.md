# Windows 编解码基线报告（win-msvc，2026-09-28）

> 对应计划：`docs/windows_topos_codec_sdk_optimization_plan_2026-09-28.md` W01、W02。
> 状态：**W01/W02 完成**（本机实机证据，非 CI 外推）。

## 机器与环境

| 项 | 值 |
| --- | --- |
| 机器 | ASUS System Product Name，Intel 16C/24T，64 GiB RAM |
| OS | Windows 11 企业版 10.0.26200（x64） |
| 编译器 | MSVC 19.44.35228（VS 2022 Build Tools 14.44.35207，vcvarsall amd64） |
| Windows SDK | 10.0.26100.0 |
| CMake/CTest | 3.31.6-msvc6（VS 内置） |
| Python | 3.12.10（`.venv-win` 独立 venv） |
| commit | 7200e65e（W01 修复后）；构建始于 a77733b4 |

## W01：MSVC x64 构建基线

- 生成器 `Visual Studio 17 2022 -A x64`，`TOPOS_ENABLE_TESTS=ON`，构建目录 `build/win-msvc`。
- **Debug 与 Release 均 exit 0**。Release 产物：`topos_codec.dll` 580,096 B（PE x64 0x8664），`topos_codec.lib` 112,550 B。
- DLL SHA-256：Debug `c9ac7e10…37ca321`（修复前）；Release `deaba532…4bed7477`。详见 `build_artifacts_win-msvc_2026-09-28.txt`。
- 首次构建暴露并修复 7 处可移植性缺陷（见 git 提交 7200e65e）：macOS 专有头、无守卫 `__int128`/`__builtin_clzll`/`__builtin_bswap32`、POSIX 时钟、VLA、`EDTS[32]` 初始化器超界、`small`/`big` 与 rpcndr.h 宏冲突。
- 第二轮问题：**Windows 1MB 默认栈溢出（0xC00000FD）**——`golden_v9`/`golden_v73`/`unit_mov_audio` 在 Debug/Release 崩溃。已在 CMakeLists 加 `/STACK:8388608`（MSVC）与 `-Wl,--stack,8388608`（MinGW）对齐 POSIX 8MB；tpool worker 以 stack=0 创建、继承 exe PE 头，主线程与 worker 同时覆盖。
- Release 剩余 57 条 C4701 类告警（mov.c goto 清理模式的启发式误报为主），未开 `/WX`，不阻塞；列入 W13 观察项。

## W02：测试全集与门禁（全部 exit 0）

| 门 | 命令摘要 | 结果 |
| --- | --- | --- |
| CTest Debug | `ctest -C Debug` | **93/93 通过**，115.05 s |
| CTest Release | `ctest -C Release` | **93/93 通过**，58.44 s |
| PE 符号门 | `check_exports.py Debug/topos_codec.dll` | OK：导出 474 ≥ 清单 62 |
| DLL 加载+往返 | `dll_load_test.py`（Debug 与 Release 各一次） | OK：abi=2，qp20@10bit roundtrip maxdiff≤64 |
| CLI 链路 | rawgen(96x64×4,kind2)→encoder(qp24,qm1)→probe --verify→decoder | OK：verify 4/4；decoded=4 concealed_frames=0 |

跳过项：无（93 项全数执行，无 skip）。

## 结论

W01、W02 达成：MSVC Debug/Release 可构建、可测试、可加载、CLI 链路可用。
跨平台 golden（conformance_*）在 Windows 与冻结向量逐位一致，构成 W06 的 Windows 侧首轮证据（macOS/Linux 侧对拍沿用 CI）。

## 证据文件

- `env_win-msvc_2026-09-28.txt`、`cmake_system_information_2026-09-28.txt`、`CMakeCache_win-msvc_2026-09-28.txt`
- `log_configure/build_debug/build_release_win-msvc_2026-09-28.txt`
- `log_ctest_debug_win-msvc_2026-09-28.txt`、`log_ctest_release_win-msvc_2026-09-28.txt`
- `log_cli_smoke_win-msvc_2026-09-28.txt`、`build_artifacts_win-msvc_2026-09-28.txt`
