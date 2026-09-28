# ADR-C025：R7 CI/跨平台/发布闭环的交付边界与决策

- 状态：已接受（2026-08-31，R7）
- 范围：`.github/workflows/codec-matrix.yml`、`codec-fuzz.yml`、`codec-release.yml`、
  `native/topos_codec/scripts/{check_exports,dll_load_test,package_codec,fuzz_longrun}`、
  `run_tests.sh`、`build_sdk.sh`、`scripts/verify_product_bundle.py`、
  导出 UI Topos 控件（pix_fmt 选择器 + Alpha 预算策略）
- 关联：ADR-C010（阶段 10 引导期）、ADR-C014 C-114（alpha_budget_policy UI
  授权开关遗留）、ADR-C020（444 档 pix_fmt 选择器遗留）、ADR-C023（档位格式
  约束单一真相源）、复验报告 §5 O6

## 背景

审计整改计划 §4 R7 要求消除「允许失败也算完成」的发布状态：Windows MSVC
首绿并摘除 `continue-on-error`、Windows DLL 打包与加载测试、四平台 golden/ABI
对拍、CI 接入 Python 绑定与应用测试、libFuzzer 持续任务、真实证书公证、
PyInstaller 四产品干净机加载、发布 job 保留机器/编译器/commit/产物哈希。

**环境事实（诚实边界）**：仓库主 remote 为 gitee（`git@gitee.com:xiaoduoshare/
topos.git`），`.github/workflows` 为 GitHub Actions 语法的模板/镜像用途——本
开发环境**无法触发真实 CI runner**，也无 Apple 开发者证书。因此 R7 按
「机器全部交付 + 本地可验证部分全绿 + 运行时证据缺口显式登记」执行，不以
未运行的 CI 状态冒充完成。

## 决策

### C-117：windows-msvc 摘除 continue-on-error（required 化）

`codec-matrix.yml` 的 windows-msvc job 移除 `continue-on-error`，升级为完整
门禁（Debug 构建 + 全量 ctest + PE 符号钉死 + DLL ctypes 加载 roundtrip +
CLI 链路冒烟）。**首绿证据以真实 runner 运行记录为准**——R-26 在此之前保持
打开（结构缺陷已修，运行时验证待记录）。

### C-118：符号表钉死统一走 stdlib 解析器

新增 `scripts/check_exports.py`（纯 stdlib 解析 PE/ELF/Mach-O 导出表，
fat 二进制取首 slice），`run_tests.sh` 的符号检查从 `nm` 切换为该脚本——
三平台同一解析路径（Windows CI 无 `nm`；macOS `nm -gU` 语义平台相关）。
本地对拍：dylib 导出 119 ≥ 清单 23（与 nm 一致，thin Mach-O）。
**R8 复审更正**：PE 导出表字段与 fat 首 slice 偏移最初实现有误（真实
PE/发布 SDK universal dylib 上必败），已修复并以合成 PE32+ 与对齐填充
fat 二进制回归钉死（tests/native/test_release_scripts.py
TestR8SyntheticFormats）；发布 SDK universal dylib 实测通过。

### C-119：发布清单单一实现（机器/编译器/commit/哈希）

`scripts/package_codec.py`：staging SDK 骨架 → BUILDINFO（含 machine/
compiler 行）→ ctypes 加载测试 → `RELEASE-MANIFEST.json`（schema
`topos-codec-release/1`：version、git_commit、abi_version、machine
uname 五元组、compiler 路径+版本+dumpmachine、lib sha256、逐文件
sha256/size、load_test 结论）。`build_sdk.sh` 完整打包路径与
`codec-release.yml` 的 Linux/Windows job 共用该实现；macOS 发布 job 走
build_sdk.sh（universal + 签名/公证）后同样产出 manifest。

### C-120：CI 共享 runner 的性能门禁口径（R-25 残余收口）

CI 各 job 设 `TOPOS_PERF_GATE_MULT=4.0`：数值门禁保留（O4 交付），
阈值按 benchmark_protocol §6 放宽 4×——共享 runner 只拦**结构性 >4×
回退**；精确 p95/p99 尾延迟仍以空载固定机全轮留档为准（R-25 残余明示）。

### C-121：Python 测试接入 CI（废除一刀切 --skip-python）

`run_tests.sh` 支持 `TOPOS_PYTHON` 覆盖（oracle 与绑定冒烟两处）；
`codec-matrix.yml` 新增 python-app job（ubuntu+macos）：`pip install -e
".[dev]" + av` → 构建 codec → `tests/native` + 阶段 6–8 topos 应用测试 +
SDK 示例 + interop oracle。

### C-122：libFuzzer 持续任务与语料沉淀

`codec-fuzz.yml`：每晚 cron + 手动触发，Linux clang 构建 5 个 fuzz
target（ASan + fuzzer），`scripts/fuzz_longrun.sh` 长跑（默认每 target
300s）——可写语料目录上传为 corpus artifact（30 天），crash/leak/timeout
上传 90 天且 job 失败（不静默）。macOS Apple clang 无 libFuzzer runtime，
维持 codec-matrix 的确定性回放 driver 覆盖。

### C-126：产品包构建结构性坏包修复（R7 实建验证抓到）

对 topos-color 实建 PyInstaller 包时，新增的 `verify_codec_load` 检查暴露
两个互相掩盖的缺陷：

1. **spec 文件以 `_spec.py` 结尾**——PyInstaller 按 `.spec` 后缀区分
   spec 模式与入口脚本模式，`.py` 使其被当作**入口脚本**打包：自动生成的
   spec 不含 base_spec 的 binaries/hiddenimports（codec 未收集、入口=
   spec 源码而非 `apps.*.main`），产物是静默的结构坏包。
2. **build_product.sh 把验证失败降级为 WARNING**（exit 0）——正是 R7 要
   消灭的「允许失败也算完成」模式，使坏包一路绿灯。

修复：四个 spec 重命名为 `.spec`；spec 内改用 importlib **按文件路径**
加载 base_spec（pip 的 site-packages `packaging` 在 PyInstaller 运行期
已缓存于 sys.modules，包名导入会解析到错误包——路径加载免疫命名冲突）；
build_product.sh 验证失败改为 `exit 1` 硬失败。重命名后 build_product.sh
的 spec 查找同步更新。

### C-127：真实签名/公证经 secrets 门控（R-27 维持打开）

`codec-release.yml` macOS job 把 `TOPOS_SIGN_IDENTITY` /
`TOPOS_NOTARY_PROFILE` 从 secrets 注入 build_sdk.sh（阶段 10 已实现
codesign → notarytool --wait → staple 全链）；无凭据时显式提示 ad-hoc。
真实凭据首跑记录前 R-27 不关闭。

### C-124：产品包 codec 加载验证（干净机器语义）

`scripts/verify_product_bundle.py` 新增 `verify_codec_load`：定位包内
libtopos_codec（根或 `_internal`）→ 以**当前解释器**直接跑
`dll_load_test.py`（stdlib-only，无需 venv/numpy）→ ABI=2 + 最小
encode/decode roundtrip 数值校验。本地以 topos-color 实建验证（其余三
产品同一 spec 路径，CI product-matrix 全矩阵自动获得该检查）。

### C-125：Topos 导出控件收口（ADR-C014/C020 遗留）

- **pix_fmt 选择器**：档位格式域 = `topos_profiles.tier_pix_fmt_choices
  (tier, with_alpha)`（`format_bases × bit_depths`，R7 新增机器可读字段，
  capability_manifest.json 交叉钉死）；裸格式 = 隐式 alpha 探测，
  `topos_*a{N}` = 显式格式契约。链路：面板 → `VideoConfig.pixel_format` →
  `to_timeline_export_config`（域内才透传，非 Topos codec 置空——防
  `yuv420p` 默认值闯入编码域）→ `TimelineExportConfig.pixel_format` →
  `_get_codec_settings` 覆盖 codec_map 默认 → 编码器构造期单一解析表兜底。
- **alpha_budget_policy UI 授权开关**：record/error/continue 三态 combo →
  `VideoConfig.alpha_budget_policy` → TimelineExportConfig → 编码器
  kwargs。E2E 钉死：error=噪声素材超 proxy 硬上限时导出失败且无产物；
  continue=交付且 tpcB 记 `authorized` 标志。

## 后果

- 所有必需平台 job 的 YAML 已 required 化且本地可验证部分全绿；「允许失败
  也算完成」的结构性缺陷消除。
- 运行时证据缺口**显式登记**（risk register R-26/R-27/R-33）：CI 首绿、
  真证书公证、四产品包全矩阵干净机记录——在真实 runner/凭据可用前，
  stage_status R7 = conditional。
- `run_tests.sh` 符号检查不再依赖 nm（行为等价，已对拍）；CI 与本地门禁
  共用同一套脚本，路径分叉风险降低。

## 验证证据（2026-08-31，本机 macOS x86_64）

- check_exports.py：dylib 119 导出 ≥ 23 清单 OK；坏符号/非二进制反例 rc=1；
- dll_load_test.py：abi=2、64×48 YUV422 qp20 roundtrip maxdiff≤64 OK；
- package_codec.py：RELEASE-MANIFEST 字段/哈希复算/BUILDINFO 机器行齐备
  （tests/native/test_release_scripts.py 7/7）；
- UI 控件：tests/deliver/test_video_export_topos_controls.py 9/9；
  透传/E2E：TestR7ExportControlsPassthrough 4/4；manifest 域交叉 16/16；
- topos-color PyInstaller 实建 + verify_product_bundle（含 codec load）：
  见提交信息与 risk register R-33 注记。
