# ADR-C010 — 阶段 10：健壮性、分发与 SDK

日期：2026-08-29 ｜ 状态：已接受 ｜ 对应计划 §12 阶段 10

## 0. 背景与目标

阶段 9 交付性能（4.0×/6.8× 编解码）后，本阶段把库从"能用"推进到"可长期维护、
可受控商业部署"：故障注入测试（OOM/IO）、TSan、压力浸泡、ABI 钉死、构建矩阵、
universal 分发、签名/公证管线与独立 SDK 文档。所有验证仍以既有门禁
（`run_tests.sh`，现扩为五配置）为唯一口径。

## 1. 决策

### C-78 内部分配路由 + OOM 故障注入（tc_alloc）

库内全部 140 处 `malloc/calloc/realloc/free` 机械替换为 `tc_alloc/tc_calloc/
tc_realloc/tc_free`（`src/common/alloc.{h,c}`），默认直通 libc；dev API
`tc_dev_set_alloc_fault(n)` 使第 n 次分配返回 NULL（每次设置清零计数，
保证逐点对齐）。测试侧 `tests/support/` 刻意**不**走该路由——OOM 注入只打
库内分配点，IO 缓冲增长不得混入计数。
**test_oom** 对 encode / encode_sized / decode / mux 全序列 / movie_open /
faststart 做穷举 sweep：第 1..N 次分配失败全部返回 `TC_ERR_OUT_OF_MEMORY`、
状态可恢复（下一轮干净运行成功）、句柄可释放；泄漏由同二进制 ASan 配置承担。

### C-79 修复：分配失败不得伪装成 concealment（真 bug）

穷举暴露：`tc_color_slice_decode` 的 slice 级 DC 缓冲分配失败返回 OOM，
但 `dec_slice_job` 颜色路径把一切非 OK 当码流损坏 → 帧返回
`TC_WARN_CONCEALED`。对中间片这是**误诊**（应用会以为文件损坏，实际是内存
不足）。修复：OOM 记入 `fatal_rc` 整帧上报（alpha 路径阶段 9 已有此机制）。
回归即 test_oom（修复前 decode sweep 在 n=4 即失败）。

### C-80 IO 故障注入与并发读契约（fault_io + test_robust_io）

`tests/support/fault_io.{h,c}`：`topos_io` 回调实现，读/写/seek_write 可从
第 k 次调用起返回 `TC_ERR_IO`（权限拒绝/磁盘满/用户取消），EOF 越界读归
IO 错误；计数器 C11 原子（同一 movie 多线程共享读时 TSan 干净——第一版
非原子计数被 TSan 当场抓获，测试设施自身的修复）。
test_robust_io 覆盖：读全拒 → open IO；写故障逐点 sweep（mux 序列每一步
可失败可释放，含 seek_write 回填失败）；播放中途开始失败 → 立即 IO
（不挂起）；**两种布局（标准/FastStart）逐前缀截断 sweep**（每个前缀
open/逐 sample 读取均落定义码集，截断中段 sample 区间越界返回
MALFORMED "不影响其它帧"）；4 线程共享同一 `topos_movie` 随机读
（字节与顺序读一致）+ 4 线程各自 open/read/close。close 契约按头文件
钉死：join 后单线程执行。

### C-81 TSan 成为第五门禁配置

`run_config tsan`（`-fsanitize=thread`）加入 run_tests.sh。全 35+ 测试
（含 stage9 并行、压力、并发读）TSan 干净——库内零数据竞争（静态条带 +
slot 绑定的正确性设计得到工具级确认）。

### C-82 压力浸泡（test_stress）

模拟应用负载：160 帧（env 可放大浸泡）× 5 类内容轮换 × 4 档 qp 交替 +
每 8 帧 sized 码控，316×180 非 8 倍宽（padding 路径）+ alpha mode2；
mux → 标准/FastStart 双布局 → 顺序播放 + 500 次随机拖拽 seek + 150 次
随机位翻转损坏，全程 `tc_dev_set_thread_count(4)`。不变量：完好码流
`rc==TC_OK && concealed==0`（中间片不允许无声劣化）；重复解码逐字节一致；
损坏码流落定义码集且 `concealed_slices` 与 `slice_status[]` 逐项一致。

### C-83 ABI 钉死（布局/错误码/符号/C++）

- `test_abi_compat`：9 个公共结构体 sizeof + 首尾字段偏移逐值钉死
  （LP64 与 LLP64 布局一致：全部定宽类型/指针/size_t）；18 个错误码数值
  全量冻结；`struct_size` 容忍语义钉死为"0 或精确 sizeof，其余拒绝"
  （v1 严格策略，变更必须升 ABI 版本）。
- `tests/abi/public_symbols_v1.txt`：23 个公共符号清单；门禁 `nm` 逐个
  断言导出（macOS `-gU` / Linux `--defined-only` 自适应）。
- `test_abi_cpp.cpp`：公共头在 C++ 编译单元下 static_assert 布局 +
  调用查询类 API（CMake CheckLanguage 可选启用，无 C++ 工具链时跳过）。

### C-84 能力协商 `tc_query_support`（新公共符号）

`tc_query_support(profile, pixel_format, bit_depth, alpha_mode)` →
TC_OK 或字段级 `TC_ERR_UNSUPPORTED_*`（bit_depth 归 PIXEL_FORMAT：v1 的
pf0 即"YUV 4:2:2 10-bit"整体能力）。为 V2.1 profile 扩展预铺协商面。
绑定层 `ToposCodec.query_support()` 同步暴露。SDK.md §1 为规范用法。

### C-85 编码器侧 fuzz（fuzz_encode）

既有 fuzz 全在解码侧；新增 encode target：fuzz 字节驱动合法域配置
（几何 8..320 含非 8 倍数/qp/qm/alpha 组合）+ 像素值。不变量：合法域配置
必须编码成功；同输入重复编码逐字节一致（§11.1）；产物被自家解码器接受；
sized 有界返回。回放 driver（ctest 常驻）+ libFuzzer（可用时门禁 20k runs）
双入口。

### C-86 Windows/MSVC 就绪 + 构建矩阵（引导期）

- tpool Win32 路径：CreateThread spawn-per-call + WaitForSingleObject，
  与 pthread 路径同条带语义/同上界（`_WIN32` 分支；无线程平台保序兜底）。
- cpudetect MSVC：`__cpuid/__cpuidex + _xgetbv`（含 OSXSAVE/YMM 状态检查，
  AVX512F 加验 opmask/ZMM；保守判定）。
- SIMD TU：**懒初始化替代 constructor**（C-77：MSVC 无 constructor 属性，
  亦解除 dylib 卸载顺序依赖；CAS 单写者，与 crc32 同模式）；MSVC 经
  CMake per-source `/arch:AVX2` 整 TU 编译；**两个 SIMD TU 常驻编入**，
  按编译切片架构宏各自取舍（universal 构建各出一半，非匹配切片编译为空，
  占位 typedef 满足 ISO 空 TU 限制）。
- CI：`.github/workflows/codec-matrix.yml`（linux-gcc/linux-clang/
  macos-arm64 全量门禁 + windows-msvc `continue-on-error` 引导期——
  MSVC 无本机验证手段，首次全绿后摘除豁免，见 R-26）。

### C-87 SDK 分发（universal + 签名/公证管线）

`native/topos_codec/scripts/build_sdk.sh`：
- macOS 默认 universal（`CMAKE_OSX_ARCHITECTURES=x86_64;arm64`，实测
  lipo 双架构 ✓；无 arm64 SDK 自动降级单架构并警告）；
- 产物 `dist/topos-codec-sdk-<ver>+<commit>/`：include/ lib/ examples/
  SDK.md BUILDINFO.txt（版本/git/构建时间/已知限制）NOTICE.txt（专有声明
  + 零第三方依赖）——满足计划"发布包包含版本、许可证、构建信息和已知限制"；
- install_name 归一 `@rpath/libtopos_codec.dylib`（单文件分发，示例以
  `-Wl,-rpath` 链接运行）；
- 签名：默认 ad-hoc；`TOPOS_SIGN_IDENTITY` 真实签名；加
  `TOPOS_NOTARY_PROFILE` 时 notarytool 提交--wait + staple（凭据由环境
  注入，脚本不落任何账号信息）；
- `--smoke`：门禁快速路径（复用 build/perf，staging + 示例 roundtrip）。

### C-88 PyInstaller 集成（R-15 关闭）

`packaging/pyinstaller/base_spec.py`：`_find_topos_codec_lib()`（env
TOPOS_CODEC_LIB → dist SDK → release/perf/debug 构建目录）自动把
libtopos_codec 收进产品包（`binaries=[(lib, ".")]`，绑定层已在
`sys._MEIPASS` 根查找——路径闭环；无库时显式 WARNING 而非静默缺功能）。
冻结应用不依赖开发机绝对路径（安装包内自包含）。

### C-89 SDK 文档与双语言示例（gate 验证）

`docs/SDK.md`：能力协商/错误码全表（含集成方应对列）/帧与容器
API/线程契约/ABI 政策/构建/发布物/已知限制（无音频、实时门未达、Windows
引导期）/v1→未来迁移政策。示例 `examples/encode_decode.c`（真实文件 IO
回调全闭环）与 `examples/topos_roundtrip.py`（绑定层等价闭环）均入
run_tests.sh（C 侧经 build_sdk --smoke，Python 侧直接运行断言 PASS）。

## 2. 被拒绝的备选

| 备选 | 拒绝理由 |
| --- | --- |
| malloc 交互注入（DYLD_INTERPOSE/zone hook） | 平台私有、侵入链接契约；tc_alloc 路由可移植且语义精确（恰好第 n 次） |
| OOM 允许按 concealment 交付 | 误诊：concealment 语义 = 数据可疑；系统态必须如实上报（C-79） |
| ABI 演进允许"只增字段"宽松 struct_size | v1 无多版本实例，宽松 = 掩盖漂移；钉死"0 或精确值"，升版本才放开 |
| 符号表全量钉死（含内部 tc_*） | 内部重构将被迫每次更新 golden；只钉 23 个公共符号（ABI 承诺面） |
| Windows 矩阵直接置必过 | 无本机 MSVC 验证手段；引导期 continue-on-error + R-26 跟踪更诚实 |
| SDK 带 dylib 版本化链接名（libtopos_codec.0.dylib） | 多文件分发易缺 symlink；归一 @rpath 单文件 + rpath 链接 |
| PyInstaller hook 文件（hooks/ 目录） | base_spec 单点 binaries 收集更少活动部件，产品 spec 无需逐个登记 |

## 3. 验证证据

- 五配置门禁（debug/asan/ubsan/tsan/fuzz）ctest 全绿（36 项/配置，
  含新增 oom/robust_io/stress/abi_compat/abi_cpp + fuzz_encode_replay）；
- TSan 全套干净（唯一报告为测试设施非原子计数，已修复为 C11 原子）；
- test_oom 穷举：encode 3 + sized N + decode 11 + mux 序列 + open +
  faststart 全部分配点 OOM 传播正确（修复 C-79 后）；
- 符号表钉死：23/23 公共符号导出（nm 断言）；
- universal dylib：`lipo -archs` = x86_64 arm64；ad-hoc codesign 通过；
  SDK 示例（C）在 staged SDK 上编译 + roundtrip PASS；Python 示例 PASS；
- Python 并发（4 线程共享 codec 随机 seek/解码一致）+ RSS 收敛浸泡通过；
- 回归：tests/native + tests/media 三件套与 stage-9 基线一致。

## 4. 风险变化

- R-14（损坏流 DoS）→ 已关闭：decode+encode 双侧 fuzz 五配置常驻；
- R-15（原生库打包）→ 已关闭：universal SDK + PyInstaller 自动收集闭环；
- R-22（平台验证延后）→ 已缓解：macOS universal 实测交付 + Linux CI 全量
  门禁 + Windows 引导期矩阵；MSVC 首绿后彻底关闭；
- 新增 R-26（MSVC 路径无本机验证）、R-27（公证流程无凭据实测）；
- R-25（线程 p95 尾部）维持打开（空载复测义务在 perf_report_stage9）。

## 5. 完成门槛对照（计划 §12 阶段 10）

| 门槛 | 状态 |
| --- | --- |
| 旧 v1 码流可由新 decoder 解码 | ✅ golden 四套（transform/bitstream/codec/mov）+ 提交入库 corpus 每次门禁全量回放 |
| sanitizer、fuzz 和资源泄漏门禁通过 | ✅ ASan/UBSan/TSan/fuzz 五配置 + OOM 穷举 + 浸泡 RSS 收敛 |
| 安装包不依赖开发机绝对路径 | ✅ PyInstaller 自包含（_MEIPASS 根查找 + binaries 收集） |
| SDK 文档能由独立示例完成 encode/decode | ✅ C/Python 双示例入 gate（build_sdk --smoke + topos_roundtrip.py） |
| 发布包包含版本、许可证、构建信息和已知限制 | ✅ BUILDINFO.txt / NOTICE.txt / SDK.md §9 |

## 6. 遗留与后续

- MSVC 首绿：windows-latest CI 摘除 continue-on-error（R-26）；
- 公证全流程实测（需 Apple 开发者凭据，R-27）；
- Windows runtime 打包（DLL + signtool）随 Windows CI 就绪后补；
- stage-9 遗留继续有效：熵符号层重构、encode_sized 迭代压缩、
  1080p25 实时门（decode p95 ~6× 差距）、阶段 7/8 应用层遗留；
- 浸泡放大：`TOPOS_STRESS_FRAMES/_DRAG/_CORRUPT` 环境变量（默认 160/300/150）。
