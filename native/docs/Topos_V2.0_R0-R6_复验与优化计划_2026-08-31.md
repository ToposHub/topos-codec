# Topos Video Codec V2.0 — R0–R6 复验报告与优化计划

> 复验日期：2026-08-31  
> 复验基线：`dcd7fbfe`（R6 提交，当前 HEAD）  
> 复验对象：整改计划 R0–R6 的当前实现、测试、ADR、SDK、容器规范和性能报告  
> 文档性质：独立复验结论、问题全集、后续优化顺序与重新验收标准  
> 总结论：R0–R6 均有真实交付，但只有 R3 可判为“条件完成”；R0/R1/R4/R5/R6 为“部分完成”，R2 为“未完成”。R0 的范围冻结文档已落地，但“唯一真相源”在后续扩档后再次漂移。当前不得继续使用“R0–R6 全部完成”或“R4/R6 整轮完成”的结论。

---

## 1. 复验方法与判定原则

本次复验没有把提交说明、ADR 的“已完成”标记或 capability 的 `available=True`
当成实现证据，而是按以下层次交叉检查：

1. 对照 `Topos_V2.0_完成度审计与整改计划_2026-08-30.md` 的 R0–R6 任务和完成门槛；
2. 检查对应提交与当前 HEAD 的实际代码、所有调用入口和负路径；
3. 运行已有 focused tests、native 单测/golden/fuzz replay/oracle；
4. 为现有测试未覆盖的路径编写只读最小探针，直接观察像素、平面、header、退出码和线程状态；
5. 区分“实现存在”“已有测试通过”“原完成门槛已满足”三个不同结论。

优先级定义：

- **P0**：会静默写错像素/格式，或造成产品完成声明失真；
- **P1**：真实功能缺陷、线程/资源风险，或原 DoD 的关键缺口；
- **P2**：测试、容器 oracle、文档和治理不足，会掩盖 P0/P1；
- **P3**：维护性、注释、命名和低风险一致性问题。

---

## 2. R0–R6 复验结论

| 整改轮 | 建议完成度 | 复验判定 | 已确认成果 | 不能验收的主因 |
| --- | ---: | --- | --- | --- |
| R0 状态/范围冻结 | 65% | **部分完成（已复发）** | 提交 `4da22d83`、ADR-C011、V1 Preview 基础命名、前四档 preset 语义、V2.0 最终范围、risk 状态分类和自研 MOV 终案均真实落地 | spec/native query/Python summary/六档 UI/SDK/README/主计划已给出不同答案；Preview 未进实际 UI；risk 状态失真；前置验收门假绿且未机器化 |
| R1 数据正确性 | 80% | **部分完成** | packed BGR/BGRA/RGB/RGBA 编码器映射、range 拒绝、颜色位深/码值/几何校验、premultiplied 配置传递、普通 close/abort 测试有效 | planar 输入与输出格式未交叉核验；Alpha dtype/PlaneInfo/stride 校验不完整；mux close 失败仍发布文件 |
| R2 时间线 Alpha/HDR | 45% | **未完成** | BGRA 单层/多层、奇数尺寸和普通取消已经过真实 `export_timeline()`；字幕和显式 OETF 已有方法级测试 | RGBA 再次翻错；Topos planar 重导出破坏 Alpha/颜色；premultiplied 语义丢失；Alpha+crop 直接失败；普通 float 层会裁剪 >1 HDR；效果/转场/调整层/GPU 释放未验 |
| R3 Alpha 预算 | 90% | **条件完成** | native stats、12→10→8 首帧自适应、三态超限策略、lossless 不降质、tpcB、基础 Alpha 素材矩阵已闭环 | 未覆盖 Pro444/Extreme；实际比例字段会饱和且无饱和标志；首帧代表策略仍是已接受限制 |
| R4 格式/profile | 75%–80% | **部分完成** | 12-bit 422、YUV444、GBR、profile 5/6 header/query、golden/corpus、SIMD parity 主体存在 | Pro444/Extreme 独立矩阵未实现；binding/快速导出/GBR 语义未收口；OOM、多代、Alpha、GPU、六档真实 app round-trip 未闭合 |
| R5 MOV/元数据 | 70%–75% | **部分完成** | 支持矩阵文档、常规/FastStart、基本 co64 reader、损坏表、VFR/SAR、三方 oracle、音频非目标与直接音轨 preflight 已落地 | reader 接受无 mdat 文件；无真实 >4 GiB co64 与长 duration；nested audio 漏提示；VFR oracle/field order/probe/hard-oracle 仍有缺口 |
| R6 性能 | 55%–65% | **部分完成** | zigzag 融合、sized 搜索削减、常驻槽位/线程池、bit-exact 差分确有工程收益 | 应用实际走的 sized 路径未实时；门槛按同次结果重设；矩阵/原始数据/p99/cold/power/自动门禁不完整；线程池初始化、OOM、全局线程恢复有缺陷 |

建议同步降级：

- R0：`已交付` → `部分完成（一致性门失败）`，重新打开 R-28；
- 阶段 7：`完成` → `部分完成（R2 重新打开）`；
- 阶段 8：保持 `部分完成`，增加 R1/R3/R4.6 未闭合项；
- 阶段 9：`完成（门槛 v2）` → `部分完成（优化有效，产品性能门未闭合）`；
- R4/R5/R6 的“已交付/整轮完成”改为“主体已交付，复验未通过”。

### 2.1 R0 六项任务逐项结论

| R0 任务 | 复验判定 | 当前结论 |
| --- | --- | --- |
| 1. 阶段 4–10 重分类并链接审计 | **部分完成** | 顶部链接和初次重分类已落地；主计划 §12 的旧阶段行、R0–R6 总结行与当前代码/复验再次冲突 |
| 2. 统一命名 V1 Preview / Intra 422 10-bit | **部分完成** | `TOPOS_CODEC_LABEL` 保留 Preview 但只被 summary/测试消费；实际 UI 只显示档位名，MOV compressorname 仍为 `Topos Video Codec 2.0` |
| 3. 预设与 native profile 定义 | **主体完成** | Proxy/LT/Standard/HQ 均是 profile 3 预设；R4 后 Pro444/Extreme 为 profile 5/6，但全局 `tiers_are_presets=True` 已产生新歧义 |
| 4. 冻结 V2.0 最终范围 | **完成** | ADR-C011 已冻结 10/12-bit 422/444/GBR、Alpha/HDR、性能和跨平台发布完成定义 |
| 5. 收紧 risk register 状态语义 | **部分完成** | 分类和维护规则已建立；R-04/R-05/R-06/R-13/R-16/R-21/R-28/R-31/R-32 的状态或证据已失真/过期 |
| 6. 自研 MOV 终案 ADR | **完成** | ADR-C011 已定义正式路线、atom 边界、非目标和 oracle；R5 后续扩展了规范和三方对拍 |

R0 失败的不是“没写 ADR”，而是没把 ADR 变成可执行的一致性和阶段依赖门。
各整改轮的提交时间顺序形式上串行，但前置阶段凭不完整测试被误判完成，没有 CI 规则
阻止未达 DoD 的 R1/R2 放行后续 R4/R6 完成声明。

---

## 3. 测试与反例证据

### 3.1 已有门禁结果

- R0 capability/registration/formats/preflight focused tests：`45/45 passed`；
- R1/R2 时间线与导出 focused tests：`127 passed, 5 skipped`；
- Stage 7/Source/R1/R2 focused tests：`54 passed`；
- R3/R4 Python focused tests：`35 passed`；
- R3/R4 native codec/header/MOV/SIMD/OOM/golden focused：`10/10 passed`；
- deterministic fuzz replay/corpus focused：`6/6 passed`；
- R5 MOV、R6 perf unit、三方 oracle：通过；
- R5/R6 Python focused tests：`10 passed`；
- native 全配置 CTest：Debug、ASan、UBSan、TSan、Fuzz 均为 `41/41 passed`，
  共 `205/205`；TSan 的 encode fuzz replay 独立运行 546.04 s，未报数据竞争；
- 应用侧指定整组 Python 回归：`140/140 passed`；
- 独立 MOV interop oracle：standard VFR、hand-made co64、FastStart 均通过
  内置 parser、PyAV 和 ffprobe；
- SDK 示例编译与 encode/decode round-trip：通过。

这些通过结果证明现有已覆盖路径稳定，但不能覆盖下述反例。尤其是多个 P0
在全部现有 Python focused tests 通过后仍可稳定复现。

### 3.2 关键最小复现

1. RGBA `[10,20,30,40]` 经 `TimelineExporter._extract_frame_data(..., keep_alpha=True)`
   得到 `[40,30,20,10]`，正确 BGRA 应为 `[30,20,10,40]`。
2. `config.pix_fmt=yuv444p10le` 可接受 `frame.pixel_format=gbrp10le`，文件正常写出，
   但 G/B/R 被静默标成 Y/U/V。
3. Topos `yuv422p10+a8` 的满刻度 Alpha 255 经高精度时间线转换得到约 16336/65535，
   而不是 65535；`a12/a16` 后缀还会反向污染颜色位深推断。
4. 实际 Topos→TimelineExporter→Topos 重导出中，Alpha 样本由 2056 降到 128，
   导出仍返回成功。
5. Alpha 导出叠加非 identity crop，四通道分支立即因运行时未导入 `np` 抛 `NameError`。
6. 合法 MOV 的 `mdat` FourCC 改成 `JUNK` 后，reader 仍能 open、报告 9 samples，
   并读出首包 `TPIC`。
7. `topos_probe_cli --verify` 已报告 `verify: 3/4 frames OK` 时进程退出码仍为 0。
8. 相同 YUV444 10-bit/QP/输入下，native profile 3 与 profile 5 的 `packet[53:]`
   完全相同；差异仅为 profile header 与 header CRC。
9. `topos_capability_summary()` 返回六档和 native profile 3/5/6，但
   `_codec.bitstream_profiles` 只列 Standard v1–v1.4，同时全局返回
   `tiers_are_presets=True`。
10. 主 Deliver 面板有六档，Quick Export 只有四档；`deliver_controller`
    不识别 Pro444/Extreme，并会把未知 codec 静默改成 H.264。
11. binding 的非法 `(profile,pixel_format,bit_depth)=(999,999,999)` 被当场对拍为
    `(3,0,10)`；应用编码器的 `pix_fmt='totally_invalid'` 同样会被猜成 422/10-bit。

---

## 4. 全部发现问题

### P0：必须先修复

#### P0-01 planar 输入格式与输出配置可静默错配

- 证据：`src/shared/export/topos_encoder.py:822-847`；
- 现状：只检查输入位于全局 allowlist，几何和 header 只按 encoder config 解释；
- 可错配：422↔444、YUV444↔GBR，以及伪标格式；构造器对完全非法 pix_fmt 还可默认为 422/10；
- 影响：颜色平面语义被静默改写，文件可打开但像素错误。

#### P0-02 TimelineExporter 的 RGBA→BGRA 映射仍错误

- 证据：`src/features/edit/core/timeline_export.py:3891-3895`；
- 原因：四通道使用 `np.flip(..., axis=-1)`，把 Alpha 翻到首通道；
- 影响：RGBA 来源的颜色和 Alpha 同时错误，R1 的编码器修复没有覆盖时间线边界。

#### P0-03 Topos planar 素材重导出破坏颜色与 Alpha

- 证据：`timeline_export.py:3631-3654, 3705-3769, 3940-3986`；
- 真实 TPIC 重导出中，FFmpeg 的高深度和 8-bit source 都无法打开，随后落入
  `_convert_yuv_planes_to_bgr()`；打开失败不缓存，还可能每帧重试并打印异常；
- 低深度 fallback 没有消费 H.273 range/matrix 元数据，而是使用通用 OpenCV YUV 转换；
- `_convert_yuv_planes_to_bgr16()` 另有独立错误：回退按 dtype 宽度右移，
  `_bit_depth_for_pixel_format()` 会把 `a12/a16` 后缀误认为颜色位深；Alpha 使用颜色
  bit depth 归一化，没有使用 `PlaneInfo[3].bit_depth`；
- 影响：Topos 作为中间片再次进时间线时，重导出仍“成功”但像素已损坏。

#### P0-04 premultiplied Alpha 在时间线合成边界丢失

- 证据：`ToposMediaSource` 把标志放在 `DecodedFrame.extra`，但
  `_extract_frame_data()` 只返回 ndarray，CPU 合成前未按标志 unpremultiply；
- 现有“premultiplied edge”测试实际使用 straight BGRA，不构成验收证据；
- 影响：透明边缘可能出现黑边、亮边或错误颜色。

#### P0-05 R6 的“产品性能门完成”声明不成立

- 应用无 CRF 覆盖时实际走 `encode_sized`；R6 报告自身的 1080p sized p50 为
  108.8 ms，约 9.2 fps，不满足 25 fps 实时编码；
- 4K plain encode 122.6 ms 约 8.2 fps；
- 通过把门槛改为 160 ms 和“4K 用代理”只能形成新的受限产品策略，不能等价于
  原计划“各 profile 实时”的完成；
- 影响：完成度与用户可感知性能再次发生偏离。

### P1：关键功能、资源与验收缺口

#### P1-01 Alpha+Crop 路径会失败且破坏高精度语义

- `timeline_export.py:3258-3263` 运行时没有 `numpy as np`；
- 即使补导入，代码固定 `/255` 和 `uint8`，不支持 RGBA16；
- `apply_crop_premultiplied()` 的结果还可能被后续当 straight 合成。

#### P1-02 planar Alpha 严格校验未完成

- 只校验前三个颜色平面的 dtype，Alpha float 会被 `astype('<u4')` 静默截断；
- `PlaneInfo` 缺失时跳过位深/stride 检查；
- 实际 ndarray 非连续 stride 会被静默紧化而非按 R1 契约拒绝/明确转换。

#### P1-03 mux close 失败仍可能发布文件并遗失资源引用

- `src/shared/export/topos_encoder.py:598-602` 吞掉 close 异常，随后 `_mux=None`
  并继续 `os.replace()`；
- `_force_close_mux()` 在 `topos_encoder.py:1042-1048` 也吞掉 close 异常并立即丢弃
  `_mux` 引用，因此 abort、定稿失败和编码失败清理同样受影响；
- 无法证明 native mux/fd 已释放，也无法让调用方知道 close 不完整。

#### P1-04 R2 的真实 HDR 门槛未验证

- 端到端 PQ 用例输入只是 `uint8` 的 240/255 信号值；
- 场景线性 `>1.0` 只测静态 helper，没有经过真实 Comp Clip→TimelineExporter→
  Topos→decode；
- 已存在具体裁剪点：`timeline_export.py:2728-2742` 的 `_normalise_bgr_frame()`
  和 `_quantize_export_frame()` 会把普通 float layer 无条件裁到 `[0,1]`；目前只有
  Comp Clip 专用路径会先调用 OETF；
- 因此当前不只是“缺测试”，而是普通 HDR 层的工作值已知会被裁剪。

#### P1-05 效果、转场、调整层、Comp Clip 和资源释放矩阵未闭合

- ADR 已承认 Comp Clip 无 Alpha、effect halo 和 adjustment Alpha 未单独验证；
- 缺真实 crop/effect/subtitle/transition/adjustment/composite 的组合数值测试；
- 取消测试没有创建 GL bridge、Composite renderer、texture/fence，不能证明 GPU
  失败/取消/窗口关闭释放。

#### P1-06 Pro444/Extreme 的独立矩阵没有实现

- `topos_encoder.py:511` 所有档位固定 `qmatrix=1`；native 也只按 qmatrix id
  查表，不按 profile 选择；
- profile 5/6 当前主要是 header 标签、格式约束与应用 target_bpp；
- 与 R4.4“独立矩阵/码控”任务不一致。必须二选一：真正实现并冻结独立矩阵，
  或正式修改产品规范，禁止继续在测试/ADR 中宣称已实现。

#### P1-07 Python binding 对非法 capability 参数静默降级

- `src/shared/codec/topos_binding.py:1099-1101`：未知 profile→3、未知 pixel format→0、
  非 10/12-bit→10；
- 违背“域外输入显式失败”，会隐藏调用方 bug。

#### P1-08 R4.6 没有覆盖全部导出入口

- `src/shared/export/quick_export_dialog.py:557-561` 仍只列四档；
- `src/app/controllers/deliver_controller.py:687-708` 缺 Pro444/Extreme，未知 codec
  静默回退 H.264；
- 结果可能是用户选择 Topos 高档位却得到 H.264 文件。

#### P1-09 GBR 的应用层平面语义错误

- `src/shared/media/topos_source.py:507-519` 对 GBR 仍标 `Y/U/V/A`；
- output mode 仍恒为 `planar_yuv`；
- 下游按组件标签、色彩模型或 output mode 分派时会误解数据。

#### P1-10 R4 明文 DoD 测试矩阵未闭合

- OOM sweep 仅 Standard/422/10-bit；
- 444/GBR GPU 测试主要是分类断言，没有真实 decode→upload→readback；
- YUV444/GBR 的 12-bit 多代矩阵缺失；
- profile 5/6 缺多代质量，Pro444 缺 Alpha app round-trip；
- 六档缺真实 TimelineExporter/Deliver 产物往返。

#### P1-11 R3 在 R4 扩档后未重新闭合

- Alpha 预算测试只覆盖 Proxy/LT/Standard/HQ，没有 Pro444/Extreme；
- `actual_ratio_bp` 超过 6.5535 时饱和为 65535，但没有 saturation 标志，
  不能准确表达真实比例。

#### P1-12 MOV reader 不强制 mdat 与 sample containment

- `native/topos_codec/src/mov/mov.c:1081` 一带最终只要求 ftyp+moov；
- 没有验证 sample offset/size 落在某个 mdat payload；
- 与 container spec 的必需 atom/数据归属规则不一致。

#### P1-13 真正大型 co64 与长 duration 未验

- R5 co64 测试只把小于 4 GiB 的 stco 数值拓宽为 co64；
- 2000 帧 VFR 只有约 167 秒，不覆盖 32-bit duration 边界；
- writer 的 `mdhd/tkhd` v0 饱和路径、`total_dur*1000` 溢出和真实 sparse file
  offset 没有确定性验证。

#### P1-14 nested sequence 音频可能无预警丢失

- R5 preflight 只看父 sequence 的直接 `audio_tracks`；
- 真实导出会先展开 nested sequence 音频；
- 父序列无直接音轨、子序列有音频时，Topos 纯视频输出可能没有前置 warning。

#### P1-15 常驻线程池首次并发初始化存在竞争

- `pool_atexit_once()` 使用普通静态变量，`pool_init()` 没有 `pthread_once`、原子状态
  或初始化锁；
- 两个首次调用者可并发初始化同一 mutex/cond；现有并发测试先暖池，避开此路径。

#### P1-16 槽位缓冲 realloc 的 OOM 路径可能泄漏

- 多个 realloc 结果先覆盖旧指针，再统一检查；单项失败会丢失原分配；
- 随后缩短 `slot_n`，清理不再遍历失败槽；
- 现有 OOM sweep 在注入前已用同几何预热槽位，没有覆盖首次扩容失败。

#### P1-17 全局线程数恢复和“回压”并不线程安全

- Encoder A/B 交错设置/恢复进程级线程数会破坏运行中编码器设置，并可能最终恢复到
  非原始值；
- 环满后退化为调用线程同步执行，只限制 worker，不限制活跃调用方数量；
- per-context 线程数不能继续仅作为低风险已接受限制。

#### P1-18 性能报告和自动门禁不足

- 主表只覆盖 Standard 422 10-bit、无 Alpha；缺 12-bit/444/GBR/profile 5/6/Alpha；
- `run_tests.sh` 的 perf 只 grep 固定文字，不检查任何阈值，严重回退仍会全绿；
- 报告取各二进制最优轮，在自报 ±2× 噪声下有选择偏差；
- 缺完整逐帧 raw、全轮 p99/max/cold、功耗、T1 commit/hash；
- 物理下限推导的 cycles/px、样本数和毫秒换算需重新审计；
- seek 与 1000 帧 RSS 沿用旧证据，但 R6 实际改变了分配和线程生命周期。

#### P1-19 SDK 的 444/GBR 几何说明会误导第三方分配

- `docs/SDK.md:102` 仍声明 U/V 恒为 `ceil(w/2)×h`；
- YUV444/GBR 应为全宽；C ABI 又没有输入 buffer length，按旧文档分配可能导致越界读取风险。

#### P1-20 R0 的一致性和串行门没有变成可执行约束

- ADR-C011 冻结了命名、范围、preset/profile 和 R0–R8 串行顺序，但没有机器可读的
  单一 capability manifest、机器可读阶段状态或 prerequisite acceptance gate；
- 提交顺序形式上串行，但 R1/R2 在未达原 DoD 时已被假绿门放行，主计划也在
  R8 复审前再次出现
  R4/R6 整轮完成声明；
- R-28 所描述的根因已实际复发，不能继续标为“已缓解”。

### P2：测试与治理问题

#### P2-01 测试中存在永真断言

- `tests/media/test_topos_timeline_export.py:336` 使用 `assert ... or True`，该检查永远通过。

#### P2-02 R1 必测矩阵与真实 premultiplied 像素证据不足

- 没有直接覆盖 BGRA64/RGBA64；
- premultiplied 测试主要验证标志，没有验证真实预乘像素与 Alpha=0 的边缘行为。

#### P2-03 R5 field order/VFR 外部 oracle 证据偏弱

- field order 没有专门 `fiel` fixture，只用通用 JUNK；
- PyAV/ffprobe 没有逐包核对 VFR PTS/duration。

#### P2-04 “硬 oracle”仍可跳过并让总门禁成功

- `run_tests.sh` 在没有 `.venv` 时打印跳过但退出成功；
- 与 ADR 对 PyAV+独立 parser 的硬门禁声明不一致。

#### P2-05 probe 检出坏帧仍返回成功

- `--verify` 的坏帧计数不会反映到非零退出码；
- 读取/OOM 中断时还可能少验证帧却不给 CI 失败信号。

#### P2-06 R6 可复现脚本和数据包不完整

- 报告示例的 `$BIN_$b` shell 展开错误；
- 示例运行 `perf quick`，不能重建完整报告；
- JSON 声称包含全部原始轮次，但只保留少量聚合值；功耗没有实际数据。

#### P2-07 项目状态文档再次漂移

- 原计划 §12 的阶段 2/3/4/5/6/8 仍描述 R3–R5 前的旧能力；
- 阶段 4 仍写 native 只支持 profile3/422/10，阶段 8 仍写“Alpha 预算待 R3”；
- 阶段 7/9 又提前标为完成；
- ADR-C011 C-91 当时的 Pro444/Extreme 占位结论已被 ADR-C018 部分取代，但没有
  `superseded-by` 标记或机器可读 roll-up，导致旧结论继续被当作当前现状；
- 违反 R0/R-28 的“唯一范围真相源”约束。

#### P2-08 SDK/构建产物元数据互相矛盾

- `native/topos_codec/scripts/build_sdk.sh:58` 仍写 ABI v1，但当前 header/binding 是 ABI v2；
- BUILDINFO 仍写实时解码门未达并引用旧 stage9 报告；
- SDK 开头同一段先写 GBR/profile 5/6 未闭合，紧接着又写已交付；
- native README 标题仍是 422/10-bit，正文已是 10/12-bit 422/444/GBR；
- SDK、README、主计划对 GBR/六档/性能给出不同答案。

#### P2-09 capability summary 语义不完整

- `bitstream_profiles` 未列 profile 5/6；
- 全局 `tiers_are_presets=True` 与 Pro444/Extreme 被称为独立 bitstream profile 冲突；
- summary 没有驱动 UI，导致主 Deliver 六档、Quick Export/controller 四档同时存在；
- `tc_alpha_mode_name(2)` 仍显示 `restricted(reserved)`。

#### P2-10 risk register 表结构与状态治理仍有错误

- R-31 行缺一个表格列，`R5` 被写入缓解文本，状态落到“归属”列；
- R-28 仍标“已缓解”，但本次已证实能力声明漂移复发；
- R-16 声称八个注册点和 fallback 已闭合，但六档扩展后 Quick Export/controller 未闭合；
- R-04 仍以“四档预算矩阵”支撑关闭，R-06 仍写“R5 待扩围”，R-21 对 GPU 全链的证据过度；
- R-05/R-13 仍以 R2 已闭合为依据关闭，与当前 HDR/Alpha 反例冲突；
- R-08 仍打开但没有纳入 R6 完成结论，R-32 的全局线程恢复缓解已被证明不安全；
- R-25 的“取最优轮”不应作为尾延迟已缓解的充分证据。

#### P2-11 R0 一致性测试为假绿

- `TestRegistrationPoints` 的 container/job/settings/Quick Export/controller/i18n 仍只断言旧四档；
- `TestToposProfiles` 没有断言 `_codec.label`、`bitstream_profiles` 或
  `tiers_are_presets`；
- `test_ui_matrix_and_preflight_matrix_agree` 只比较 core table 和 preflight，并不读取真实 UI 列表；
- 没有 spec/native query/Python summary/UI/SDK/计划表的统一矩阵门、非法 capability
  拒绝测试、risk 表结构 lint 或未知 codec 禁止 fallback 负路径。

#### P2-12 V1 Preview 命名没有进入真实产品面

- `TOPOS_CODEC_LABEL` 只被 capability summary 和测试引用，主 Deliver/Quick Export 只显示
  `Topos Proxy/LT/Standard/HQ/Pro 444/Extreme`；
- native MOV sample entry 与 container spec 仍写 `Topos Video Codec 2.0`；
- 用户可见层没有统一告知 Preview/experimental 身份，与 R0 C-90 命名决策不符。

### P3：维护性问题

- `native/topos_codec/src/common/tpool.h` 仍描述旧 spawn-per-call 行为；
- Topos V1 Preview 与“V2.0 非目标”的用户提示混用；
- FastStart oracle 缺少工具参数时会少跑 case，而不是明确失败；
- `topos_profiles.py` 顶部仍残留“v1 仅 422/10-bit”等过期注释；
- `test_six_tiers_declared_four_available` 测试名仍是旧四档语义；
- Deliver UI 注释仍写“10-bit 422 四档”，紧邻实现已是六档；
- 中英文 i18n 缺 `codec_topos_pro444` 和 `codec_topos_extreme`。

---

## 5. 新优化实施路线

以下阶段严格串行。O1 未完成前不得继续扩展 codec 格式；O5 完成前不得恢复
“R0–R6 完成”状态；R7/R8 仍按原整改计划执行。

### O0：状态纠偏与失败测试先行

目标：先阻止错误完成声明继续传播，并把本次所有反例固化为红灯。

任务：

1. 主计划、阶段表、ADR 摘要把 R0/R1/R2/R4/R5/R6 降级；R3 标条件完成；
2. 重新打开 R-28，修正 R-04/R-05/R-06/R-13/R-16/R-21/R-31/R-32 的证据与状态；
3. 为 P0-01～P0-05 和 P1-01/P1-12/P1-15/P1-16/P1-17/P1-20 添加失败测试；
4. 删除 `or True`，保证新测试修复前稳定失败；
5. capability、UI、SDK 暂时只声明已经被端到端验证的组合。

完成门槛：每个 P0 都有独立失败测试和最小复现；状态表不再出现与证据冲突的“完成”。

### O1：重新完成 R1/R2 数据语义链

目标：任何来源进入 TimelineExporter 后，格式、颜色、位深和 Alpha 语义不丢失。

任务：

1. 建立输入 `pixel_format → (color_model, subsampling, bit_depth, alpha_depth)` 的单一解析器；
2. encoder config、frame.pixel_format、PlaneInfo、实际 ndarray dtype/shape/stride 必须逐项一致；
3. 四通道只重排 RGB 三通道，Alpha 永远保持末位；
4. 时间线帧不得只传裸 ndarray；至少携带 color model、颜色/Alpha 位深和 premultiplied 标志；
5. Topos planar 直接走 Topos/planar 适配路径，不再尝试 FFmpeg 解 TPIC；
6. 高深度 Alpha 使用独立 PlaneInfo bit depth；修复 a8/a10/a12/a16；
7. crop/effect/transition/subtitle/adjustment/Comp Clip 明确 straight/premultiplied 契约；
8. close/abort/failure/window-close 的 native/GPU 资源都必须可观察地释放。

必测：RGBA/BGRA 8/16、普通 float RGBA `>1.0`、422/444/GBR、a8/a10/a12/a16、
straight/premult、Alpha=0、Topos→Timeline→Topos、crop、effect、transition、subtitle、adjustment、
Comp Clip、取消/失败/关闭，以及 `mux.close()` 在 close/abort 两条路径的故障注入。

完成门槛：真实链路逐像素/逐平面对比通过；无 passthrough 替代；无 GPU/native/temp 泄漏。

### O2：重新完成 R3/R4 profile 与应用协商

目标：格式、profile、矩阵、码控、capability 和全部应用入口给出同一答案。

任务：

1. 决定并实现 Pro444/Extreme 独立量化矩阵；若产品决定不做，更新 R4.4 规范和命名；
2. binding 对非法 profile/pf/bit depth 立即抛错；
3. GBR 输出使用 `G/B/R/A` PlaneInfo 和明确 RGB-planar output mode；
4. Quick Export、Deliver controller、TimelineExporter、主 Deliver 面板全部使用同一 registry；
5. 禁止未知 codec 回退 H.264；
6. R3 预算矩阵扩至六档，并为比例饱和增加标志或扩大字段；
7. 扩展 OOM、坏流、fuzz、10 代质量、Alpha、GPU readback 和六档 app round-trip。

完成门槛：合法组合全通过、非法交叉全拒绝；profile 语义有真实 payload/画质差异或已诚实改名；
所有入口和 SDK capability 一致。

### O3：重新完成 R5 容器边界

目标：reader 接受域、writer 输出域、oracle 和产品 preflight 完全一致。

任务：

1. reader 必须确认 mdat 存在，所有 sample offset+size 落在 mdat payload 且 checked；
2. 用 sparse-file/虚拟 IO 验证 >4 GiB writer 自动 co64 与 FastStart 重定位；
3. 覆盖 v0 duration 边界、必要时输出 v1 header，所有乘加用 checked arithmetic；
4. 增加真实 `fiel`、VFR 逐包 PTS/duration、旋转/timecode 明确拒绝或保留 fixtures；
5. 硬 oracle 缺依赖必须失败，不能跳过；
6. probe 发现任何坏帧/未验证帧必须非零退出；
7. preflight 使用与真实导出相同的 nested-audio 展开结果。

完成门槛：spec、writer、reader、probe、PyAV、ffprobe、独立 parser、SDK/UI 支持矩阵逐项一致。

### O4：重新完成 R6 并发与性能

目标：先保证线程/资源正确，再用独立、可复现、面向应用路径的性能门验收。

任务：

1. 用 `pthread_once`/InitOnce 或等价原语完成线程池一次性初始化；
2. realloc 使用临时指针逐项提交，补首次增长 OOM sweep；
3. 把线程数迁到 per-context/session；迁移前禁止嵌套“保存/恢复全局值”；
4. 实现有界任务队列与调用方回压，不只限制 worker 数；
5. 性能主门以应用真实 `encode_sized`、decode、seek、open/close 为准；
6. 覆盖六档、10/12-bit、422/444/GBR、Alpha on/off、真实/合成素材；
7. 使用空载固定机、交替成对比值、median/置信区间；不得各自选最佳轮；
8. 保存逐帧 raw、p50/p95/p99/max、cold/warm、RSS、功耗、线程数、commit、产物 hash；
9. 把明确数值阈值接入 CI/perf runner，文本 grep 不算性能门禁；
10. 产品无法实时的组合必须标“离线/代理”，不能继续称实时门完成。

完成门槛：并发首次初始化、两个交错 encoder、OOM/TSan 全绿；应用路径在冻结门槛和完整矩阵下达标；
第三方能按文档命令重建报告。

### O5：文档、SDK 与单一真相源收口

目标：消除 R-28 再次复发。

任务：

1. 更新主计划 §12、审计状态、SDK、README、BUILDINFO、risk register；
2. 建立唯一机器可读 capability manifest，spec/query/UI/preflight/SDK/计划从其生成或校验；
3. 建立机器可读阶段状态/依赖 manifest 和 CI prerequisite rule；例如 R2 未独立验收时，
   禁止 R4/R6 被标为完成；
4. 为 ADR 增加 `supersedes/superseded-by` 元数据和当前决策 roll-up；
5. BUILDINFO 从 header/库查询 ABI 和 capability，不手写 ABI v1/v2；
6. capability summary 按档明确区分 preset 与 native profile；
7. 主 Deliver/Quick Export/controller/i18n 共用同一 registry，未知 codec 显式失败；
8. 用户可见 UI 和 MOV 元数据使用经产品决策确认的 Preview 命名；
9. SDK 按 422/444/GBR 分别给出 plane geometry；
10. 修复 R-31 表格、更新 R-08/R-25/R-28/R-32；
11. 报告中的测试数量和性能数据由脚本生成，不手抄。

完成门槛：spec/query/UI/SDK/BUILDINFO/主计划对任一 profile、格式、Alpha、音频和性能问题给出相同答案。

### O6：接回 R7/R8 发布闭环

O0–O5 全部通过后，继续原 R7/R8：Windows required、跨平台包、真签名/公证、
libFuzzer、Python 应用矩阵、干净机器安装和独立 Review。R8 通过前不得恢复 V2.0 完成标记。

---

## 6. 建议门禁调整

### 每次提交

```bash
bash native/topos_codec/run_tests.sh
```

并强制执行新增的反例套件：

```bash
.venv/bin/python -m pytest \
  tests/media/test_topos_export.py \
  tests/media/test_topos_timeline_export.py \
  tests/media/test_topos_alpha_pipeline.py \
  tests/deliver/test_video_export_preflight.py \
  tests/edit/test_timeline_export.py -q
```

### 新增测试名称建议

- `test_planar_input_format_must_match_encoder_config`；
- `test_capability_manifest_matches_native_query_all_legal_combinations`；
- `test_capability_manifest_drives_all_ui_registries`；
- `test_stage_status_requires_all_prerequisite_acceptance_gates`；
- `test_adr_supersession_rollup_has_one_current_decision`；
- `test_user_visible_topos_name_contains_preview_status`；
- `test_risk_register_rows_and_status_evidence_are_valid`；
- `test_timeline_rgba_to_bgra_keeps_alpha_last`；
- `test_topos_planar_reexport_preserves_independent_alpha_depths`；
- `test_timeline_premultiplied_source_unpremultiplies_before_composite`；
- `test_alpha_crop_rgba8_and_rgba16`；
- `test_binding_invalid_capability_rejected`；
- `test_quick_export_pro444_extreme_never_fallback_h264`；
- `test_gbr_plane_components_and_output_mode`；
- `test_movie_requires_mdat_and_samples_inside_mdat`；
- `test_writer_co64_over_4g_sparse_io`；
- `test_nested_audio_triggers_topos_video_only_preflight`；
- `test_thread_pool_two_first_callers`；
- `test_slot_growth_oom_no_leak`；
- `test_two_encoders_out_of_order_close_preserves_thread_policy`；
- `test_perf_thresholds_application_matrix`。

---

## 7. 重新验收顺序

1. 先完成 O0，纠偏状态、重新打开 R0/R-28，但不提前宣称 R0 已重新验收；
2. 关闭 P0-01～P0-04，禁止产生新的静默错误文件；
3. 完成 O1，重新验收 R1/R2；
4. 完成 O2，重新验收 R3/R4；
5. 完成 O3，重新验收 R5；
6. 完成 O4，重新验收 R6；
7. O5 收口所有状态、发布元数据和阶段依赖门，此时才最终重新验收 R0；
8. 最后执行 R7/R8 独立复审。

在上述顺序完成前，建议对外能力名称保持：

**Topos Codec V1 Preview — experimental 10/12-bit 4:2:2/4:4:4/GBR intra**，

并在 UI/SDK 明示：Alpha/HDR 时间线重导出、Pro444/Extreme profile 语义、
大文件 MOV、实时性能和跨平台发布仍在复验整改中。
