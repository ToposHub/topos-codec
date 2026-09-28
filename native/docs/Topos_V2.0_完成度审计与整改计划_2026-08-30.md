# Topos Video Codec V2.0 完成度审计与整改计划

> 审计日期：2026-08-30  
> 审计对象：`docs/专属极速帧内中间片编码引擎（Topos V2.0）.txt` 及其对应实现  
> 文档性质：现状纠偏、缺陷修复顺序和重新验收标准  
> 结论：当前成果应定义为“稳定的 V1 受限核心 + 部分 Topos Color 接入”，不能继续标记为完整 V2.0
>
> **⚠️ 2026-08-31 复验更正**：本文档 R0–R6 整改的完成状态已由独立复验报告
> `Topos_V2.0_R0-R6_复验与优化计划_2026-08-31.md` 全面复核并降级——
> R0 部分完成（一致性门复发）、R1 部分完成（P0-01）、R2 未完成（P0-02/03/04）、
> R3 条件完成（P1-11）、R4 部分完成（P1-06/08/09/10）、R5 部分完成（P1-12/13/14）、
> R6 部分完成（P0-05/P1-15–18）。本文档 §4 的 R0–R6 任务描述仍然有效，但任何
> 轮次的「已交付/完成」结论以复验报告 §2 表为准；重新整改按复验 §5 的
> O0–O5 路线执行，O0–O5 完成前 R7/R8 不启动。

---

## 1. 审计结论

当前工程已经具备质量较好的基础能力：10-bit YUV 4:2:2、Standard native profile、
帧内 elementary stream、私有 TPIC MOV、C ABI、Python binding、基础媒体源、部分导出接入、
Alpha 平面编码、AVX2/NEON 代码与多线程、sanitizer/fuzz replay/ABI/SDK 门禁。

但是，原计划第 12 节把阶段 0–10 全部标记为完成，与实现和报告中的证据不一致。
问题不是少量文案错误，而是同时存在产品范围缺失、真实数据错误、端到端验收缺口、
性能门槛未达和发布门禁未闭合。因此本轮不对个别函数做孤立修补，先按本文重新排期。

### 1.1 可以保留的成果

- native v1 的 10-bit YUV 4:2:2 Standard 编解码闭环和 golden vectors；
- checked arithmetic、边界校验、损坏输入处理、OOM/IO/并发测试基础；
- TPIC MOV 的现有 writer/reader、FastStart、co64 路径和 CLI；
- C ABI、ctypes binding、符号表和 SDK 示例；
- 现有 Alpha plane、planar decode、YUV GPU upload 和部分 UI/Deliver 注册；
- AVX2 路径、NEON 实现、slice 并行和性能测量工具；
- 当前受限范围内的自动化门禁。

### 1.2 不能继续宣称完成的能力

- 12-bit 颜色编解码；
- YUV/GBR 4:4:4、Pro 444、Extreme；
- 六个独立 native profile；
- Alpha 目标比例/硬上限的自动执行和逐帧元数据记录；
- 时间线透明度端到端导出；
- premultiplied Alpha 的编码语义保持；
- HDR 浮点高光不裁剪的导出链；
- 原计划规定的 1080p/4K 实时性能门槛；
- Windows 必过、正式签名/公证和完整跨平台发布；
- 完整 V2.0 产品定义对应的“全部完成”。

---

## 2. 证据与阶段重分类

| 原阶段 | 原状态 | 审计后状态 | 主要证据/原因 |
| --- | --- | --- | --- |
| 0 决策冻结 | 已完成 | 部分完成 | 初始范围冻结为 Standard 10-bit 4:2:2，但后续文档仍用六档、10/12-bit 和 444 作为全项目完成定义，范围没有统一 |
| 1 Native 骨架 | 已完成 | 完成（受限范围） | CMake、ABI、安全基础设施和门禁存在 |
| 2 变换与量化 | 已完成 | 完成（受限范围） | 标量/golden 存在；12-bit 仅做数值域验证，不等于 12-bit codec |
| 3 Bitstream/熵 | 已完成 | 完成（v1 受限语法） | v1 只接受 Standard、YUV422、10-bit |
| 4 完整标量 Codec | 已完成 | 部分完成 | `native/topos_codec/src/version.c:42` 明确只支持 profile=3、pixel_format=0、bit_depth=10；Alpha 比例策略未执行 |
| 5 MOV/CLI | 已完成 | 条件完成 | 现有自研最小 MOV 路径通过当前测试，但与原计划“使用成熟容器库”冲突；需把偏差、支持边界和互操作证据重新冻结 |
| 6 应用解码 | 已完成 | 完成（受限格式） | TPIC 10-bit 422 可探测、播放、seek；不能代表 12-bit/444/全部 profile |
| 7 Alpha/HDR | 已完成 | 未完成 | GPU 上传分段测试存在，但时间线导出会丢 Alpha；Edit RGBA8 仍是已知限制；HDR 导出会裁剪高光 |
| 8 编码/代理/Deliver | 已完成 | 部分完成 | UI 仅暴露四档，且四档共享 native Standard；Pro444/Extreme 明确不可用；Alpha/色彩存在真实错误 |
| 9 SIMD/GPU/并行 | 已完成 | 未达门槛 | `docs/perf_report_stage9.md` 第 4 节明确记录 1080p/4K 解码和 1080p 编码均未达门槛 |
| 10 健壮性/分发/SDK | 已完成 | 部分完成 | Windows job 为 `continue-on-error`；正式公证未实测；风险 R-25/R-26/R-27 仍打开 |
| V2.1 Micro-GOP | 预留 | 不得启动 | V2.0 的 Alpha、HDR、profile、性能和发布门尚未闭合 |

重新命名建议：在整改完成前，对外使用“Topos Codec V1 Preview（10-bit 4:2:2 Intra）”；
不要把当前二进制或 UI 选项称为完整 V2.0。

---

## 3. 已确认 BUG 与不足

### P0：会造成静默数据错误或产品承诺失真

#### P0-1 时间线 Alpha 导出实际被压平

- `src/features/edit/core/timeline_export.py:3706` 的 `_extract_frame_data()` 对 BGRA/RGBA
  只返回三通道 BGR；
- `src/features/edit/core/timeline_export.py:3867` 再把三通道数据包装成 `bgr24/bgr48le`；
- `ToposVideoEncoder` 在 `has_alpha=True` 且输入无第四平面时写入全不透明 Alpha；
- 现有 `TestPassthroughExport` 只验证 MediaSource → ExportFrameProvider → Encoder 的直通，
  没有经过真实 TimelineExporter 合成/导出主路径。

影响：Deliver 中勾选 Alpha 可能生成带 Alpha 标记但画面全不透明的文件。

#### P0-2 BGRA 通道翻转错误

- `src/shared/export/topos_encoder.py:529` 对所有 `bgr*` 输入执行 `rgbf[..., ::-1]`；
- 三通道 BGR 这样处理正确，但四通道 BGRA 会变成 ARGB；
- 随后前三通道被当 RGB，第四通道被当 Alpha，颜色与透明度同时错误。

复现：BGRA `[10,20,30,40]` 的 Alpha 目标约为 `40/255*4095≈642`，当前得到 161，
即错误使用了 B 通道值 10。

#### P0-3 premultiplied Alpha 标志在编码时丢失

- native ABI 和 tpcC 均有 `alpha_premultiplied`；
- `ToposMediaSource` 也把该标志放入 `DecodedFrame.extra`；
- `ToposVideoEncoder.open()` 构造 movie/frame config 时没有从输入或配置传递该标志；
- planar 直通会保留预乘像素数值，却把文件标成 straight，解码/GPU 边界会按错误语义处理。

影响：透明边缘可能出现黑边、亮边或错误 unpremultiply。

### P1：格式能力、HDR/Alpha 语义和验收缺口

#### P1-1 12-bit planar 输入可被误当成 10-bit 接受

- `src/shared/export/topos_encoder.py:453` 只检查像素格式字符串含 `422` 和 dtype 为 `uint16`；
- 没有核验 `pixel_format`、`PlaneInfo.bit_depth` 和实际码值范围必须为 10-bit；
- 已复现 `yuv422p12le`、码值 4095 被 Python 层直接接受并送入 10-bit native 配置。

要求：不支持的输入必须显式拒绝，不能以 10-bit header 编码 12-bit 码值。

#### P1-2 HDR 浮点高光被裁到 1.0

- `src/shared/export/topos_encoder.py:523` 对浮点 packed frame 执行 `clip(0.0, 1.0)`；
- 这与 `.clinerules` 的 HDR 约束和计划中的高亮度中间片目标冲突；
- 当前 10-bit YUV 输出若只允许归一化信号值，必须在调用边界明确完成 OETF/色彩变换，
  不能在编码器内部无条件静默裁剪。

#### P1-3 Alpha 预算只有 schema，没有执行策略

- `alpha_budget_ratio` / `alpha_hard_cap` 只在 `topos_profiles.py` 声明；
- `ToposVideoEncoder` 固定使用 12-bit near-lossless Alpha，不读取统计后自适应降到 10/8-bit，
  也不在超上限时返回可诊断错误；
- native 虽提供 `color_payload_bytes`、`alpha_payload_bytes` 和 `alpha_max_abs_error`，
  Python 应用层编码后直接丢弃这些统计；
- tpcC 未完成原计划要求的目标比例、实际比例、最大误差逐帧/文件级记录闭环。

#### P1-4 未知 color range 被静默当 limited

- `_map_color_codes()` 只验证 primaries/transfer/matrix；
- `range_code = 1 if range == 'full' else 0` 会把任意未知值映射为 limited；
- 已复现 `range='mystery'` 返回 limited，而函数注释承诺未知标签显式失败。

#### P1-5 “六档 profile”与 native 能力不一致

- Proxy/LT/Standard/HQ 当前只是同一个 native Standard profile 的四组目标码率；
- Pro444/Extreme 在 `topos_profiles.py` 中明确 `available=False`；
- native header/profile query 只支持 profile 3；
- 因此可称为“四个质量预设”，不能称为“六个已完成 codec profile”。

#### P1-6 真实端到端 Alpha/HDR 测试缺失

缺少以下完整链路测试：

`Timeline composition → effects/subtitles/transitions → ToposVideoEncoder → MOV →
ToposMediaSource → GPU upload/composite readback`。

现有测试分别覆盖 native round-trip、planar passthrough、GPU uploader 和登记点，
不能证明真实时间线导出不丢 Alpha、不降位深且颜色一致。

#### P1-7 性能完成标记错误

阶段 9 报告已经明确：

- 1080p25 解码 p95：49.1 ms，目标 ≤8 ms；
- 4K25 解码 p95：111.6 ms，目标 ≤32 ms；
- 1080p25 编码：16 fps，目标 ≥25 fps。

在门槛修改或实测达标前，阶段 9 不得标记完成。

#### P1-8 发布门禁没有闭合

- `.github/workflows/codec-matrix.yml:34` 的 Windows/MSVC job 仍允许失败；
- Windows runtime/DLL 打包和签名未闭合；
- 正式 macOS 签名/公证没有凭据实测；
- codec CI 使用 `--skip-python`，应用接入测试不在该矩阵内；
- 风险登记册 R-25、R-26、R-27 仍为打开状态。

### P2：文档、容器与测试治理不足

- 原计划要求成熟 MOV library，实际使用自研最小 MOV；ADR 有理由，但主计划和风险 R-06
  没有形成一致的最终决策与边界；
- 风险登记册存在“关闭=本轮不实现”的用法，容易把未实现能力误认为风险已消失；
- 报告中固定测试数量已经过期（当前 native 每配置 36 项、Python 76 项）；
- 当前 MOV 为纯视频，音频只警告后忽略；这可以是明确非目标，但 UI/预检应在任务开始前阻止
  用户误以为音频会被保留；
- SAR、field order、timecode、VFR/每帧 duration、旋转和高级色彩元数据需要逐项声明支持、
  拒绝或保留，不能用默认值代替验收；
- libFuzzer runtime 在当前 Apple clang 环境不可用，现状是 sanitizer 下的确定性 replay，
  不应把它描述成持续长时间 fuzz 服务。

---

## 4. 整改实施路线

以下阶段严格串行。R1 未完成前，不开发 12-bit/444；R2–R3 未完成前，不启动性能重构；
R7 未完成前，不恢复“V2.0 完成”标记。

### R0：状态纠偏与发布范围冻结

目标：建立唯一、诚实、可验收的产品范围。

任务：

1. 把原计划阶段 4–10 的状态改为本文重分类结果，并在原文顶部链接本文；
2. 将当前能力命名为 `V1 Preview / Intra 422 10-bit`；
3. 明确 Proxy/LT/Standard/HQ 是质量预设还是独立 bitstream profile；推荐先定义为预设；
4. 冻结 V2.0 最终范围：至少包含 10/12-bit 422、10/12-bit 444/GBR、Alpha 策略和
   原计划性能/发布门槛；
5. 更新 risk register：未实现、未实测和被否决必须分别标记，禁止一律写“已关闭”；
6. 为自研 MOV 路线补充最终 ADR，明确支持 atom、非目标和外部 oracle 范围。

完成门槛：spec、capability query、UI 名称、SDK 和计划表对同一能力范围给出相同答案。

### R1：P0/P1 数据正确性修复

目标：先消除静默错误，不扩展格式。

任务：

1. 修复 BGR/BGRA 与 RGB/RGBA 的显式通道映射，禁止通用 `[..., ::-1]` 处理四通道；
2. `_map_color_codes()` 对 range 仅接受 `limited/full`，未知值显式失败；
3. planar 输入严格校验 pixel format、PlaneInfo、dtype、几何、stride 和码值范围；
4. 定义 `alpha_premultiplied` 的来源和一致性规则：整段导出固定语义，或逐帧不一致时报错；
5. 修复异常/close/abort 后的 context、mux 和临时文件状态，增加重复调用测试；
6. 对上述每个 BUG 先添加失败测试，再修改实现。

必测用例：BGRA/RGBA 8/16-bit、透明/半透明像素、错误 range、yuv422p12le 拒绝、
超 10-bit 码值拒绝、premultiplied round-trip、0 Alpha 除零边界。

完成门槛：每个 BUG 有最小回归测试，且测试在修复前失败、修复后通过。

### R2：真实时间线 Alpha 与 HDR 导出链

目标：让 Deliver 的 Alpha/HDR 选项对应真实输出，而不只是配置字段。

任务：

1. TimelineExporter 的内部合成帧改为明确的高精度 RGBA 表示；
2. 所有效果、字幕、转场、Composite/Comp Clip 和调整层明确 straight/premultiplied 契约；
3. Topos Alpha 导出不得经过只保留 BGR 的 `_extract_frame_data()`；
4. HDR 工作值与编码信号值之间增加显式色彩变换/OETF，不允许无条件 `[0,1]` clamp；
5. 预览与导出共享颜色元数据和 Alpha 解释，差异必须可诊断；
6. 保持 UI 线程不做 GPU 工作，资源走既有 deferred-release。

端到端素材：透明渐变、硬边/羽化、premultiplied 黑边样本、PQ/HLG 高光 >1、字幕、
转场、多层 Composite、奇数宽高、取消/失败/关闭。

完成门槛：真实 TimelineExporter 产物经 ToposMediaSource 解码后，Alpha 最大误差、
颜色误差和 HDR 高光均满足冻结指标；不能用 passthrough 测试替代。

### R3：Alpha 预算、统计与元数据闭环

目标：实现计划第 2.2 节，而不只是声明比例。

任务：

1. 编码器读取 native stats，计算 `alpha_payload/color_payload`；
2. near-lossless 模式按 profile 尝试 12→10→8-bit，直到目标比例；
3. 超过 hard cap 时执行明确策略：返回错误、经用户授权继续，或记录近似误差；
4. lossless Alpha 绝不自动降质，超限必须明确提示；
5. 设计向后兼容的元数据扩展，记录目标比例、实际比例、模式和最大误差；
6. 增加随机/噪声/硬边/全透明/全不透明 Alpha 的预算与质量测试。

完成门槛：所有可用质量预设在测试矩阵中满足各自策略；任何超限都有结构化诊断。

### R4：12-bit、4:4:4/GBR 与 profile 扩展

目标：补齐原 V2.0 的核心格式承诺。

顺序：

1. 12-bit YUV 4:2:2 scalar；
2. 10/12-bit YUV 4:4:4；
3. 10/12-bit GBR 4:4:4；
4. Pro444/Extreme profile 与独立矩阵/码控；
5. AVX2/NEON differential parity；
6. Python binding、MediaSource、GPU upload、导出和 UI 能力协商。

每个子阶段都必须新增 bitstream version/capability、golden、坏流、OOM、fuzz、
多代质量、Alpha 和应用 round-trip；不得一次性修改全部 profile。

完成门槛：六档 capability 不再用 `available=False` 占位，且 header/query/UI/SDK 一致。

### R5：MOV 与媒体元数据收口

目标：把自研最小 MOV 从“当前可用”提升为边界清楚、可互操作、可维护。

任务：

1. 冻结自研 MOV 的正式支持 atom 和拒绝策略；
2. 增加大型 co64、长时文件、FastStart/非 FastStart、未知 atom、损坏 sample table、
   VFR duration、SAR、field order 和色彩 atom fixtures；
3. 用 ffprobe/PyAV/至少一个独立 atom parser 做 oracle，不把“未知 codec 无崩溃”当成解码互操作；
4. UI/preflight 在开始编码前明确“纯视频、无音频”，而不是编码期间才 warning；
5. 决定 V2.0 是否支持音频；如不支持，将其写入产品非目标和容器能力查询。

完成门槛：容器 spec、writer/reader、probe、SDK 和 UI 的支持矩阵完全一致。

### R6：性能门槛整改

目标：用相同硬件、素材和统计口径重新达到或正式调整阶段 0 门槛。

优先级：

1. 熵符号层和 zigzag/run-level 融合；
2. `encode_sized` 搜索迭代削减；
3. 逆变换与内存布局；
4. 常驻有界线程池，避免每次调用 spawn；
5. 应用调度与 codec slice 并行的 oversubscription/回压；
6. 只有 CPU 数据证明值得时再评估 GPU。

验收必须记录 p50/p95/p99、冷/热缓存、功耗、峰值内存和线程数；门槛若要调整，
必须先更新 benchmark protocol 和产品需求，不能在报告中写“未达”后仍标记完成。

### R7：CI、跨平台和发布闭环

目标：消除“允许失败也算完成”的发布状态。

任务：

1. Windows MSVC job 首绿并移除 `continue-on-error`；
2. 增加 Windows DLL/runtime 打包与加载测试；
3. Linux gcc/clang、macOS x86_64/arm64 执行 golden 和 ABI 对拍；
4. 在 CI 中加入 Python binding 和阶段 6–8 应用测试，不再全部 `--skip-python`；
5. 建立可用 libFuzzer 的 Linux 持续任务和 corpus artifact；
6. 用真实证书执行 macOS sign/notarize/staple；
7. 验证 PyInstaller 四个产品包均能在干净机器加载 codec；
8. 所有发布 job 必须保留机器、编译器、commit 和产物哈希。

完成门槛：所有必需平台 job 为 required 且全绿；R-25/R-26/R-27 关闭需附实测证据。

### R8：最终独立复审与 V2.0 发布

目标：只在证据完整时恢复“V2.0 完成”。

检查：

- 逐条复核原计划第 11 节 DoD；
- 运行 native 五配置、真实 libFuzzer、Python 应用矩阵、全链 Alpha/HDR、质量/性能、
  长文件和跨平台包测试；
- 检查所有报告可由脚本重建，禁止手写无法复现的通过数量；
- 检查 risk register 无高影响打开项；
- 由未参与实现的 Review 执行一次代码与架构约束审查；
- 更新原计划状态、SDK known limitations 和 release notes。

完成门槛：不存在“报告已知未达，但总表仍写完成”的情况。

---

## 5. 测试与门禁调整

### 每次提交必跑

```bash
bash native/topos_codec/run_tests.sh
```

并增加：

```bash
.venv/bin/python -m pytest \
  tests/native \
  tests/media/test_topos_source.py \
  tests/media/test_topos_alpha_pipeline.py \
  tests/media/test_topos_export.py -q
```

### R1 必须新增的 focused tests

- `test_bgra_channel_order_and_alpha`；
- `test_rgba_channel_order_and_alpha`；
- `test_unknown_color_range_rejected`；
- `test_planar_12bit_rejected_by_v1`；
- `test_planar_out_of_range_sample_rejected`；
- `test_premultiplied_flag_roundtrip`。

### R2 必须新增的真实集成测试

- `test_timeline_topos_alpha_export_roundtrip`；
- `test_timeline_topos_premultiplied_edge_no_fringe`；
- `test_timeline_topos_hdr_highlight_not_clipped`；
- `test_timeline_topos_cancel_releases_gpu_and_native_resources`；
- `test_composite_to_topos_alpha_roundtrip`。

测试不得只断言文件存在、codec 字符串或 UI 登记点；必须读取输出像素/平面并比较数值。

---

## 6. 本次审计实际验证结果

2026-08-30 在当前 macOS 开发机执行：

- Debug：36/36；
- ASan：36/36；
- UBSan：36/36；
- TSan：36/36；
- Fuzz/replay 配置：36/36；
- 公共符号表、CLI、FastStart、ffprobe 结构 oracle、SIMD/4-thread golden parity、
  perf quick、SDK C/Python 示例：通过；
- Python binding + Topos source/Alpha/export：76/76。

这些结果只证明“现有测试覆盖的受限功能”稳定，不推翻第 3 节中已复现的未覆盖 BUG，
也不能替代 12-bit/444、真实时间线 Alpha/HDR、性能和跨平台发布验收。

---

## 7. 下一步执行顺序

1. 先执行 R0，纠正状态和命名；
2. 再执行 R1，修复六个最小数据正确性问题；
3. 完成 R2/R3 后，才允许把 Alpha/HDR 标记为完成；
4. R4–R7 分别补格式、容器、性能和发布；
5. R8 独立复审通过后，才启动 V2.1 Micro-GOP。

