# Windows Python wheel 与测试矩阵（win-msvc，2026-09-28）

> 对应计划 W18（wheel）、W19（CI 矩阵）；附带 W13 第一项优化（fastdiv）。
> 状态：**W18 完成；W19 配置就绪**（真实 CI 绿灯需 push 后在 runner 上取得）。

## W18：wheel 构建与独立 venv 验收（全部通过）

1. `package_opensource.py --include-lib --lib <Release DLL>` 生成自包含开源包（940 文件）。
2. `pip wheel` 产出 **`topos_codec-0.1.0-py3-none-win_amd64.whl`**——平台标签正确，非 `py3-none-any`。
3. 全新 venv（`.venv-wheel-test`）安装：`ToposCodec` 加载，ABI=2 协商一致；
   视频 encode→decode roundtrip、`.toos` 图片路径均通过。
4. 发布包自带 Python 测试套件（fresh venv + 包内 native 树构建 topos_inspect 后）：
   **106 passed / 14 skipped / 0 failed**（`log_pytest_packaged_suite_win_2026-09-28.txt`）。
5. `pip uninstall` 后 site-packages **无残留**。

### 过程中发现并修复的缺陷（均已入库）

| 缺陷 | 修复 |
| --- | --- |
| `package_codec.py` 在 VS 生成器下 compiler 字段全空（CMakeCache 无 CMAKE_C_COMPILER；`cl --version` 非零退出） | 解析 `CMakeFiles/*/CMakeCCompiler.cmake`（含 ID/版本），兼容配置子目录入参；子进程解码加 `errors="replace"` |
| manifest 文件路径 Windows 记为反斜杠，跨平台比对失败 | `relative_to(dist).as_posix()` |
| 打包测试混入依赖主仓库 `tools/`、scipy 的 4 个质量链测试（打包即 ImportError） | `copy_tests` 排除清单 |
| 打包 binding 仍探测 `native/topos_codec/build`，与重写后的打包布局 `native/build` 不一致（wheel 仓库内构建不可发现） | PACKAGE_REWRITES 增加 build 路径重写 |
| `test_stage10_concurrency` RSS 采样无 Windows 分支（读 `/proc` FileNotFoundError） | GetProcessMemoryInfo（工作集，KiB）；显式 argtypes（HANDLE 64 位截断 → GetLastError 122） |
| `test_v8_encode` 的 topos_inspect 路径无 `.exe`/VS 配置子目录候选 | 双候选探测 |

## W13 第一项：MSVC fastdiv 乘高位（bit-exact）

MSVC 无 `__int128`，`tc_fastdiv_apply`/`rans2_put_x` 原走 u32 真除兜底。改用
`_umul128` 高 64 位（`product>>51 == hi<<13 | lo>>51`，逐位等价）：
- CTest Release **93/93 通过**——golden 位流逐字节不变（等价性证明）。
- A/B（1K² qp30，3 次均值）：V8 编码 134.1→136.5 ms、V7 编码 21.7→21.1 ms——
  差异在噪声内，本机该几何下 fastdiv 非主导热点。
- **遗留观察（W12）**：V8 编码 ≈134 ms 显著慢于 V7 ≈21 ms（6×）。需 macOS 同
  commit 同素材对拍区分"设计代价 vs Windows 特有回退"；`ratio<1.3` 解码门在
  忙机上 1.09–1.34 抖动，属共享机噪声敏感门（按计划 §4 归专用基准机管辖）。

## W19：CI 矩阵加入 Windows

`codec-matrix.yml::python-app` 矩阵加入 `windows-latest`（所选用例均为非 GPU）：
- Resolve 步骤增加 VS 配置子目录候选 `build/debug/Debug/topos_codec.dll`，并解析
  `TOPOS_ENCODER_CLI`（含 `.exe`）供 interop oracle 使用。
- bash 语法步骤（`\` 续行、`${GITHUB_SHA::7}`）统一 `shell: bash`。
- YAML 结构校验通过。真实 runner 绿灯记录待 push 后补档。

## 证据

`log_pytest_release_scripts_win_2026-09-28.txt`（18 passed/1 skip）、
`log_package_opensource_win_2026-09-28.txt`、`log_pytest_packaged_suite_win_2026-09-28.txt`。
