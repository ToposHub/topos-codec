# Topos V2.1 Micro-GOP 可落地升级计划

> 日期：2026-08-30  
> 目标：在保留 Topos Intra 的兼容性、编辑响应和画质语义前提下，增加可选的短依赖帧间模式  
> 适用基线：Topos Codec v1.1（当前仅 Standard / YUV 4:2:2 / 10-bit）  
> 文档性质：架构、格式、实施顺序、测试矩阵与发布门禁；不是性能承诺  
> 前置整改：`docs/Topos_V2.0_完成度审计与整改计划_2026-08-30.md`

---

## 0. 结论

Micro-GOP 方向可行，但外部提案只能作为概念草图，不能直接施工。它正确地选择了短闭合 GOP、
仅前向参考和无 B 帧；但严重低估了 bitstream、C ABI、MOV、随机访问、错误传播、Alpha、缓存和
测试工作，并给出了没有基准支持的体积与性能数字。

本计划采用以下落地策略：

1. **保留现有 Intra v1，新增独立的 Inter bitstream v2 与有状态 API**，不修改旧 golden；
2. **只先做 IP-2**，并先实现“零运动预测 + 残差”，证明状态、格式和 seek 正确后再加入运动搜索；
3. **标量参考优先**，所有搜索、舍入、重建和回退必须确定且可生成 golden；
4. **参考帧必须是编码器重建帧**，绝不引用未经量化的源帧；
5. **V2.1 首版 Alpha 仍逐帧 Intra**，只让颜色平面帧间预测，避免透明度错误扩散；
6. **所有 P 帧均可回退 I 帧**，场景切换、噪声、码率失控、参考无效和取消都必须受控恢复；
7. **性能和体积只用实测决定是否推荐**，不预先承诺“0.5ms、+20%、减半”。

---

## 1. 外部方案的问题

| 外部说法 | 问题 | 本计划处理 |
| --- | --- | --- |
| “轻量级长 GOP” | N=2/4 是 Micro-GOP，不是 Long-GOP；命名会误导 seek 和兼容预期 | 统一称 closed Micro-GOP |
| 搜索范围限制在“ 或 像素” | 关键数字缺失，无法实现或生成一致码流 | 先做基准 spike，再冻结 ±R 与搜索算法 |
| 只增加 `topos_motion.c` | 当前解码、mux、source 都假设帧独立；实际需要有状态 context、P slice、stss、seek/cache 等 | 按 native→container→binding→app 分阶段实施 |
| “无需重写 bitstream” | 当前 parser 明确拒绝 P，slice 也没有 mode/MV/residual syntax | 新建 bitstream major 2，保留 v1 不变 |
| 原 DCT 管线直接处理残差 | 当前颜色路径先减 512、解码后加 512 并 clamp 0..1023；P 残差是有符号 −1023..1023 | 复用 transform/quant/entropy 原语，新增 residual plane 路径 |
| Skip 只看 SAD | 未校准 SAD 会改变画质；Skip 必须基于重建参考和显式失真界 | 第一版只允许 bit-exact zero residual Skip，阈值 Skip 后置 |
| P 帧参考上一“帧” | 若参考源帧会产生 encoder/decoder drift | 只参考上一张成功提交的 reconstructed frame |
| CPU 编码仅 +20%、解码 +10% | 没有实现或素材矩阵；运动搜索、I/P 双编码选择可能远高于此 | 以阶段基准和 adoption gate 决定 |
| N=4 减少 40%–55% | 静止素材可能更高，噪声/切镜可能无收益；不能作为通用承诺 | 按素材分层报告，P 变大自动回退 I |
| 最坏 seek <0.5ms | 当前 1080p Intra 解码 p95 为 49.1ms，与该数字冲突 | 分解 index/read/decode/GPU/present，测 p50/p95/p99 |
| P 块使用 16×16 搜索 | 当前变换为 8×8，4:2:2 chroma 为半宽；未定义宏块到残差块和 chroma MV 映射 | 明确 16×16 luma MB → 4Y+2U+2V 的 8×8 residual blocks |
| 没有讨论坏 P 帧 | 损坏参考会污染同 GOP 后续帧 | reference-invalid 状态阻断依赖，下一 I 帧恢复 |
| 没有讨论 Alpha | Alpha 动画、premultiplied 和颜色预测可相互影响 | V2.1 Alpha Intra；未来单独立项 Alpha temporal |
| 没有讨论剪切/remux | 从 P 帧开头直接拷贝会得到不可解码片段 | 非同步点裁切必须带前置 I 或重编码首个局部 GOP |

---

## 2. 当前代码基线与影响范围

### 2.1 已有可复用能力

- 53-byte frame header 已预留 `frame_type/gop_id/ref_distance`；
- 8×8 integer transform、quant/dequant、Rice、run/level、slice CRC 和 packet CRC；
- slice 级有界并行、AVX2/NEON transform、OOM/IO/fuzz/golden 基建；
- MOV reader 已解析 `stss`，binding 已有 `is_sync()`；
- FrameProvider、DecodeWorker、generation/cache 和应用媒体抽象已存在；
- native v1 API 和旧码流 golden 可以作为永久兼容基线。

### 2.2 必须修改的关键区域

```text
docs/
  ADR-C011-micro-gop-decisions.md
  bitstream_spec_v2.md
  container_spec_v2.md
  micro_gop_benchmark_protocol.md
  micro_gop_quality_report.md
  micro_gop_perf_report.md

native/topos_codec/
  include/topos_codec.h                  # 只追加 context API/新结构，不改旧结构
  src/inter/motion.{h,c}                 # SAD、搜索、MV 规则
  src/inter/residual.{h,c}               # signed residual transform/reconstruction
  src/inter/gop_encoder.{h,c}             # transactional reconstructed reference
  src/inter/gop_decoder.{h,c}             # reference validation/reset/cancel
  src/bitstream/inter_slice.{h,c}          # P slice mode/MV/coeff syntax
  src/bitstream/frame_header.c             # v1/v2 分流校验
  src/mov/mov.c                            # I-only stss、GOP sequence validation
  src/cli/{encoder_cli,decoder_cli,inspect,probe_cli,quality}.c
  tests/{unit,conformance,fuzz}/

src/shared/codec/topos_binding.py          # ToposGopEncoder/Decoder
src/shared/codec/topos_profiles.py         # intra/ip2/ip4 capabilities
src/shared/media/topos_source.py           # sync seek + decode-forward + bounded GOP cache
src/shared/export/topos_encoder.py         # sequential context + abort/reset
src/shared/media/proxy_generator.py
src/features/edit/core/render_cache_encoder.py
src/features/edit/core/timeline_export.py
src/features/deliver/
tests/{native,media,edit,deliver}/
```

禁止新建第二套全局播放线程池。Inter codec 只能使用现有调用线程和 native 有界 slice 并行。

---

## 3. 版本与兼容策略

### 3.1 三种版本不得混用

| 层 | 当前 | Micro-GOP |
| --- | --- | --- |
| 产品名称 | Topos Codec V2.0/Intra | Topos 产品 V2.1 |
| elementary bitstream | major 1 | **major 2** |
| MOV tpcC | version 1 | **version 2** |
| C ABI | ABI v1 旧函数 | ABI v1 兼容追加符号；如结构兼容无法保证再升 ABI v2 |

产品 V2.1 不等于 elementary version 1.1。所有日志、probe、SDK 和错误信息必须同时显示
产品/profile 与实际 bitstream/container version。

### 3.2 向后兼容规则

- 现有 `tc_frame_encode()` / `tc_frame_decode()`、public structs 和 v1 golden 完全不变；
- 新 decoder 必须继续解码所有旧 Intra v1 packet/MOV；
- 旧 decoder 遇 tpcC v2 或 bitstream major 2 必须返回明确 `UNSUPPORTED_VERSION/PROFILE`；
- `profile` 继续只表示像素格式/位深/采样能力（首版仍是 `3=Standard 42210`）；
- `coding_mode` 作为独立维度表示 `INTRA/IP2/IP4`，不得用新 profile id 代替 GOP 模式；
- FourCC 继续使用 `TPIC`，由 tpcC version/profile 区分；若外部 oracle 证明会误路由，
  再通过 ADR 决定新 FourCC，不能实现中途临时更换；
- 任何旧 golden 变化都视为回归，不得以“升级”为由重生成覆盖。

---

## 4. V2.1 规范性编码模型

### 4.1 GOP 与参考规则

- `IP-2`：最大 GOP 长度 2，`I P`；第一阶段唯一交付的 Inter coding mode；
- `IP-4`：最大 GOP 长度 4，`I P P P`；仅在 IP-2 发布门通过后开发；
- 场景切换或回退允许提前插入 I，因此实际 GOP 可短于最大长度；
- 无 B 帧、开放 GOP、跨 GOP 引用、长期参考或隐式参考；
- P 帧 `ref_distance=1`，只参考同 GOP 中紧邻的上一张**重建帧**；
- `gop_id` 在每个 I 帧递增，u16 回绕规则写入 spec；P 必须与参考的 gop_id 相同；
- 编码失败、取消或输出缓冲不足时不得提交新参考；context 保持上一成功状态或显式失效。

### 4.2 第一版预测层级

为降低一次性风险，严格按以下顺序：

1. `P0`：零运动预测，只有 exact Skip 与 residual；
2. `P1`：16×16 luma 宏块，整像素运动，小范围有界搜索；
3. `P2`：经实测后才考虑阈值 Skip、搜索优化或更小分块；
4. 不做亚像素、双向预测、可变分块、加权预测、全局运动模型或 CABAC 类状态机。

### 4.3 残差数学

```text
pred       = reference_reconstructed(x + mv.x, y + mv.y)
residual   = current_source - pred                    # signed
coef       = DCT8x8(residual)                         # 不做 512 level shift
qcoef      = quantize(coef, P_qmatrix, P_qp)
residual'  = IDCT8x8(dequantize(qcoef))
recon      = clip(pred + residual', 0, 2^bit_depth-1)
```

要求：

- residual 输入域、transform 中间界、量化和重建饱和必须重新证明并测试；
- 复用的是 transform/quant/entropy **原语**，不是当前硬编码 level-shift 的整帧颜色路径；
- encoder 的下一参考必须是 `recon`，不得使用 `current_source`；
- scalar 是规范性结果，SIMD 必须 bit-exact；
- P 的 qmatrix/QP 可与 I 不同，但必须由 profile 固定策略或显式 header 字段决定。

### 4.4 宏块、chroma 与边界

- 当前 4:2:2 profile：一个 16×16 luma MB 对应 4 个 Y 8×8、2 个 U 8×8、2 个 V 8×8；
- partial edge MB 使用与 Intra 相同的 deterministic edge padding；
- 4:2:2 为避免 chroma 亚像素，首版只允许**偶数水平 luma MV**；chroma dx=`luma_dx/2`，dy 不变；
- 运动候选必须保证引用矩形在 coded reference 内；decoder 对越界 MV 返回 malformed，禁止 clamp 猜测；
- MV predictor、mode/Rice 状态均在 inter slice 边界重置；reference frame 只读，slice 输出区域不重叠。

### 4.5 Skip 和 I 回退

第一版 exact Skip 条件：以重建参考形成预测，整个 MB 的 Y/U/V residual 全为 0。

阈值 Skip 只有在完成 rate-distortion 标定后才能启用，并必须同时约束：

- max absolute reconstruction error；
- SSE/PSNR 或 profile 定义的 distortion；
- Alpha/premultiplied 语义；
- 多代编码漂移。

帧级 I 回退触发：

- 第一帧、周期 I、scene cut；
- 参考缺失/失效、gop_id 不一致、取消后恢复；
- P candidate 超出帧/GOP预算；
- P packet 不小于 I candidate（标量参考编码器可双编码比较）；
- 运动搜索异常、残差范围异常或质量界不满足。

优化版可以用廉价预判减少 I/P 双编码，但输出必须与冻结决策规则一致，或作为新的 encoder
版本记录；不得为了速度取消安全回退。

### 4.6 Alpha 策略

V2.1 首版采用：**颜色 P + Alpha Intra**。

- Alpha 每帧继续走 v1 的独立预测/熵编码，不引用上一帧；
- straight/premultiplied 标志在整轨固定，颜色与 Alpha 语义一致；
- Alpha 损坏不会污染下一帧；
- Alpha payload 继续执行 V2.0 整改后的预算/误差策略；
- Alpha temporal prediction 作为后续独立 profile 能力，必须有独立参考、回退和 golden，
  不纳入 V2.1 首次发布门槛。

---

## 5. Bitstream v2 草案边界

最终字节布局必须在 M1 spike 后通过 ADR-C011 和 `bitstream_spec_v2.md` 冻结。最低要求：

### 5.1 Frame header

- 保留 53-byte 基础字段位置；`version_major=2`；
- `frame_type=0/1` 表示 I/P；
- `profile=3` 仍表示 Standard 4:2:2 10-bit；新扩展字段 `coding_mode=IP2/IP4`
  表示帧间模式，与 profile 正交；
- `gop_id/ref_distance` 激活并强校验；
- 未使用 reserved bits 仍必须为 0；如字段不足，增加可跳过的扩展 header 并提升
  `header_size`，禁止把信息塞入未声明字节；
- packet 自身必须能在不依赖 MOV 的情况下验证其依赖关系。

### 5.2 I frame

- I 的重建语义与 v1 Intra bit-exact；
- 可复用 v1 slice 语法，但 v2 profile/header 必须有独立 golden；
- I 解码成功且无 concealment 后才能安装为 reference。

### 5.3 P inter slice

P frame 使用独立的 `inter_color` slice kind，一个 slice 同时覆盖相同 luma 行带内的 Y/U/V，
避免 U/V slice 依赖另一个 Y slice 的 MV 数据。payload 必须自描述并有界，至少包含：

```text
inter_slice_prefix
  syntax_version
  mb_cols / mb_rows
  mode_section_bytes
  mv_section_bytes
  residual_section_bytes
mode section       # 每 MB: SKIP / INTER_ZMV / INTER_MV / reserved
motion section     # 仅 INTER_MV，固定宽或有界 Rice，逐分量验证 ±R
residual section   # 每 MB residual-present mask + 已出现 8×8 blocks 的 qcoef stream
```

- 三个 section 的 size/和必须 checked；总和必须等于 slice payload；
- 模式数量严格等于 slice MB 数，不得提前结束或多读；
- residual-present mask 固定映射 4Y+2U+2V；
- skipped block 的 DC predictor 值定义为 0；所有预测状态在 slice 起点清零；
- outer slice CRC 覆盖整个 inter payload；任何错误只可 conceal 当前带，但该帧不得成为后续参考。

### 5.4 解码错误传播

- header/依赖错误：整帧拒绝；
- P slice CRC/语法错误：当前带可用 reference copy conceal，并返回 warning；
- 任何 concealed I/P 都不得安装为有效 reference；
- 同 GOP 后续 P 返回 `REFERENCE_INVALID`，直到下一 I；
- 调用方可选择跳到下一同步点，不得静默沿用污染参考；
- fuzz 必须覆盖首帧为 P、ref_distance=0/>1、跨 GOP 引用、gop 回绕、坏 MV、坏 section size、
  丢包/重排/重复包和坏参考后的恢复。

---

## 6. C ABI 与 context 生命周期

旧单帧 API 保留。新增 opaque context，建议接口：

```c
tc_gop_encoder_create(config, &ctx)
tc_gop_encoder_encode(ctx, input, out, cap, &packet_info, &stats)
tc_gop_encoder_reset(ctx)          // 下一帧强制 I
tc_gop_encoder_cancel(ctx)         // 原子取消，slice/MB 边界检查
tc_gop_encoder_free(ctx)

tc_gop_decoder_create(config, &ctx)
tc_gop_decoder_decode(ctx, packet, size, planes, strides, &frame_info)
tc_gop_decoder_reset(ctx)          // seek/换片/错误恢复
tc_gop_decoder_cancel(ctx)
tc_gop_decoder_free(ctx)

tc_packet_query(packet, size, &packet_info)
tc_movie_prev_sync(movie, sample_index, &sync_index)
```

生命周期规则：

- 单 context 不可重入，不同 context 可并发；
- reference 与 scratch 由 context 所有，free/reset/cancel/OOM 路径全部释放或恢复一致状态；
- encode/decode 采用 transactional commit：packet 和 recon 全部成功后才推进 frame/gop/reference；
- query 不分配大平面，不需要 reference；
- context 配置记录 max GOP、search range、内存上限和允许 profile；
- 结构全部带 `struct_size`/version；新增 ABI layout/symbol/C++ tests；
- 4K 4:2:2+A 的 encoder/decoder internal peak memory 必须有公式上界和实测报告，
  禁止按码流声明无界分配。

---

## 7. MOV、seek 与编辑行为

### 7.1 MOV v2

- tpcC v2 记录 bitstream major、inter profile、max GOP、block size、搜索能力和 Alpha temporal mode；
- `stss` 只列 I samples；不再把所有帧标为 sync；
- 无 B 帧，所以 DTS=PTS，不引入 ctts；stts/VFR 规则保持；
- mux 从已验证 packet 推导 sync，调用方不能单独传一个可能不一致的 keyframe flag；
- mux 验证第一 sample 为 I、P 的 gop/ref chain、profile/tpcC 一致性；
- FastStart、co64 和 sync table 一起进入新 golden；
- reader 提供 nearest previous sync 查询并对恶意稀疏/无 stss 输入有界处理。

### 7.2 ToposMediaSource

当前 `seek_frame()` 只移动游标，必须改为：

1. 找到 `target` 之前最近 I；
2. reset decoder context；
3. 从 I 顺序解码到 target，最多 N 帧；
4. 只返回 target，但可把同 GOP 中间结果放入每 source 的有界小缓存；
5. 顺序播放若 context 已持有 target−1 参考则直接继续；
6. generation 变化/取消/换片/close 时 reset/free；
7. 坏参考时跳到下一 I 或返回结构化错误，不能展示旧帧冒充新帧。

不得新增全局调度器。FrameProvider、DecodeWorker、现有 cache key/revision/generation 继续作为唯一调度层。

### 7.3 剪辑语义

- 正常渲染/导出可从前一 I 解码，输出帧号/PTS 必须仍精确；
- reverse playback 在 cache miss 时按 GOP 正向解码并填充最多 N 帧缓存；
- 从 P 帧开始的 smart-copy/remux 有两种合法策略：保留前置 I 并用 edit list 隐藏，或重编码首个局部 GOP；
- 首版建议禁用 TPIC Inter 的 packet-level smart-copy，统一走 decode→encode，避免生成断引用片段；
- ripple trim、split、replace、代理切换和 relink 不得改变可见帧内容；
- proxy/render cache 可选择 IP-2；调色中间母版默认仍 Intra，除非用户显式选择 Inter。

---

## 8. 分阶段实施表

### M0：前置门与基线

前置：完成 V2.0 整改计划至少 R0–R3、R6；Micro-GOP 对外发布前必须完成 R8。

任务：冻结 Intra golden、真实 Alpha/HDR round-trip、Intra seek/decode/encode p50/p95/p99、
内存和素材 corpus。研究代码只能放 `spikes/`，不得进入产品路径。

门槛：基线可一键重建；当前已知 Alpha/色彩/planar 输入错误全部修复。

### M1：算法 spike 与决策冻结

实现离线 spike，对比：

- zero-motion only；
- diamond ±2/±4/±8（水平 MV 对 4:2:2 取偶数）；
- exhaustive ±4 仅作质量/最优 cost oracle；
- MB 16×16，必要时对照 8×8，但不直接承诺可变块；
- exact Skip 与若干阈值候选；
- IP-2 场景：静止、慢移、平移、手持、切镜、闪光、颗粒、随机噪声、屏幕/UI、Alpha。

输出 `ADR-C011`：选择搜索算法/R、cost、scene-cut、Skip、I fallback 和内存预算。

门槛：数字来自同一机器/构建/素材；收益不足时允许决定“只做 zero-motion IP-2”或停止项目。

### M2：bitstream v2 与 context 骨架

先写 spec、packet builder/parser、inter slice size/mode/MV 边界测试、context 状态机测试，
再写编码算法。生成最小手工 I/P golden 和恶意 vectors。

门槛：不做图像重建也能完整 scan/query GOP；任意截断不越界、不无限循环、不大分配；v1 全绿。

### M3：标量 IP-2 zero-motion reference codec

实现 signed residual、exact Skip、P reconstruction、transactional reference、强制 I、scene-cut 占位和
Alpha Intra。编码器可双编码 I/P 后选更小合法结果。

门槛：I/P/IP/II 序列 bit-exact golden；decoder 与 encoder recon 逐像素一致；1/3/5/10 代曲线；
坏参考不会污染下一 GOP；ASan/UBSan/TSan/OOM 全绿。

### M4：有界整数运动估计

实现 M1 冻结的 SAD、candidate order、early termination、MV syntax、chroma mapping 和 I fallback。
candidate 遍历顺序必须规范化，避免平台/线程导致码流变化。

门槛：scalar deterministic；边界/奇数尺寸/最大 MV/golden；搜索开销和压缩收益达到 M1 gate，
否则保留 zero-motion 并不发布 motion mode。

### M5：MOV v2 与 CLI

实现 tpcC v2、I-only stss、mux GOP validation、prev-sync、FastStart/co64；CLI 增加
`--gop intra|ip2`、probe/inspect 显示依赖，decoder 使用 context。

门槛：随机 sample sync/PTS 正确；从每个 I 起解码；从 P 直接启动明确失败；外部 oracle 安全解析；
新 golden_mov_v2 永久入库。

### M6：Python binding 与 ToposMediaSource

新增 context wrapper、显式 close/reset/cancel，改造 seek/read_next_frame 和 bounded GOP cache。

门槛：顺播、随机 seek、连续拖拽、反向、并发不同 source、同 source 串行、cancel/close/reopen、
损坏 GOP 恢复测试；不新增调度器；RSS 收敛。

### M7：编码、代理、缓存与 Deliver

ToposVideoEncoder 持有 sequential GOP encoder；abort 不提交参考、不留文件；新增 IP-2 capability、
preflight、preset/schema/UI/i18n；proxy/cache opt-in，母版默认 Intra。

门槛：时间线真实导出→解码像素比较；项目重开参数保留；不支持的 12-bit/444/Alpha temporal
显式拒绝；取消和失败原子。

### M8：质量、错误传播与长时间压力

覆盖 scene cut、高运动、噪声、重复帧、渐变、HDR、Alpha、损坏/丢包/重排、长文件、GOP id 回绕、
OOM/磁盘满/权限和随机 seek/close。

门槛：参考失效规则与诊断完全一致；无跨 GOP 漂移；fuzz 序列状态机长期运行无 crash/leak/hang。

### M9：SIMD 与性能优化

顺序：测量 → residual subtract/add → SAD → motion copy → transform/quant → slice 并行。
AVX2/NEON 必须与 scalar 的 MV、packet 和 recon bit-exact。避免应用线程池与 codec 并行 oversubscription。

门槛：达到第 10 节 adoption gate；未达时 profile 保持 experimental/hidden。

### M10：IP-4

只改 max GOP 和相关 rate-control/seek/cache，禁止顺带加入新运动工具。复用全部 IP-2 语法。

门槛：任意目标最多从 I 前向 3 张 P；seek/reverse/cancel/坏第 1/2/3 张 P 全覆盖；IP-2 行为不变。

### M11：跨平台与发布

Linux gcc/clang、macOS x86_64/arm64、Windows MSVC required；旧 v1/new v2 golden、C/Python 示例、
PyInstaller、签名/公证、SDK/capability/migration 文档全部更新。

门槛：所有 required jobs 全绿、无 `continue-on-error`；独立 Review 无 Critical/Suggestion 遗留。

---

## 9. 测试矩阵

### Native unit/conformance

- SAD/MV candidate order、tie-break、边界和 4:2:2 偶数 dx；
- residual transform 范围、rounding、clip、quant/dequant；
- mode map、MV stream、section size、residual mask；
- context create/reset/cancel/free、失败不提交 reference；
- IP、II、IPI、IPIP、提前 I、gop wrap；
- v1/v2 packet/MOV/query/capability；
- scalar/AVX2/NEON、1/4/8 thread parity；
- OOM 每分配点、IO 每调用点、ASan/UBSan/TSan。

### Fuzz

- 单 P packet parser；
- 合法/非法 packet sequence state machine；
- first=P、错 gop/ref、缺帧、重复、重排、截断、bit flip、超长码字、越界 MV；
- MOV stss/tpcC/sample table 与 packet chain 不一致；
- encoder config/source pixels/scene cut/cancel；
- corpus 保留所有旧 v1 seeds，并增加 v2 valid/invalid seeds。

### 应用集成

- import/probe/play/seek/scrub/reverse/cache/close/reopen；
- decode worker generation cancellation 和 proxy/source switching；
- Timeline/Composite/Deliver/RenderCache 真实输出；
- straight/premultiplied、Alpha Intra、HDR 高光和色彩标签；
- 精确 in/out、非 I 起始 trim、ripple/split/relink、项目持久化；
- CPU frame ready、GPU frame ready、first presented 分段测量。

### 素材

- 静止、慢 pan、快速 pan、缩放、旋转、手持、运动模糊；
- hard cut、fade/dissolve、闪光、灯光变化；
- 肤色、天空、霓虹、文字/UI、线条、渐变；
- 胶片颗粒、传感器噪声、随机噪声；
- SDR/PQ/HLG、limited/full；
- 全透明/不透明、硬边/羽化/噪声 Alpha；
- 奇数宽高、非 8/16 倍数、1080p/4K、短/长文件。

---

## 10. 质量、性能与采用门槛

以下是“允许成为推荐 profile”的 gate，不是预先保证的结果。M1 可依据产品测试硬件更新一次，
更新必须先走 ADR，不能在结果出来后为通过而降低。

### 10.1 正确性与画质

- decoder recon 与 encoder recon bit-exact；
- 同一输入和配置下，线程数变化不得改变 packet；
- IP-2/IP-4 不跨 GOP 漂移；
- 与 Intra 在等质量口径比较 Y/U/V PSNR、SSIM、色差、渐变和 1/3/5/10 代；
- Alpha Intra 保持 V2.0 已冻结误差；
- scene cut/noise 若无收益应回退 I，禁止强行 P 导致尺寸和画质双输。

### 10.2 压缩收益

- IP-2 代理素材 corpus：等质量下文件体积中位数至少比 Intra 小 20%；
- IP-4 成为推荐前：中位数至少小 30%；
- 任一素材因 fallback 后不应比同配置 Intra 大超过 3%（容器/GOP metadata 开销除外）；
- 分内容类别报告，不对所有素材宣传 40%–55%。

### 10.3 CPU 与 seek

- 顺序播放总解码 p95：IP-2 不高于 Intra 的 1.15×；
- 编码 p95：IP-2 不高于 Intra 的 1.50×；
- 随机 decoded-frame-ready p95：IP-2 ≤ Intra seek 的 1.75×，IP-4 ≤2.75×；
- 当 Intra 达到原目标后，1080p IP-2 random seek p95 目标 ≤16ms、IP-4 ≤24ms；
- index lookup、packet read、I decode、每张 P decode、GPU upload、first presented 必须分开报告；
- 后台负载、冷/热缓存、线程数和功耗必须记录。

未达到这些 gate 时，功能可以保留为 experimental，但 UI 默认隐藏，不能宣称“剪辑手感无差别”。

### 10.4 内存

- 报告 source/reference/recon/coded scratch/motion map/packet 各部分；
- context 创建时按 geometry/profile 计算硬上限，超限在分配前失败；
- GOP 长度增加不得线性保留所有历史 reference；解码只需上一重建帧，应用 GOP cache 独立有界；
- 4K 4:2:2+A 的峰值必须实测，取消/seek/close 后 RSS 收敛。

---

## 11. 回退、发布与完成定义

### 安全回退

- 文件级 profile 明确，不能把 Inter 静默当 Intra；
- encoder 可在任意帧插 I，但不可切换为其他 codec；
- native Inter 不可用时导出 preflight 失败，不生成伪成功文件；
- decoder reference invalid 时跳下一 I 或报错，不使用陈旧 reference；
- feature flag 默认关闭，直到 M9/M11 完成；Intra 始终是可用 fallback。

### V2.1 IP-2 完成定义

- bitstream v2、tpcC v2、C context ABI 和 capability 已版本化；
- 旧 v1 全量 golden 不变，新 IP-2 golden 永久入库；
- reconstructed-reference、exact Skip、motion、I fallback 和 Alpha Intra 符合 spec；
- MOV 只标 I 为 sync，任意 seek 最多 I+1P；
- Topos Color import/play/scrub/reverse/export/proxy/cache/close/cancel 全链通过；
- 损坏 P 不污染后续 GOP；fuzz/sanitizer/OOM/TSan 通过；
- 质量、压缩、CPU、seek 和内存达到第 10 节 gate；
- Windows/macOS/Linux required CI 与 SDK/打包通过；
- 文档不保留未经实测的“0.5ms、+20%、减半”承诺。

### IP-4 启动条件

IP-2 在真实项目中完成一次发布候选浸泡，所有 P0/P1 问题关闭，且没有通过增大 cache、
隐藏错误或降低画质来换取指标，才能启动 M10。

---

## 12. 当前状态

| 阶段 | 状态 | 备注 |
| --- | --- | --- |
| M0 前置门 | 阻塞 | 先完成 V2.0 整改与真实 Intra 性能基线 |
| M1 算法 spike | 未开始 | 不得先冻结搜索范围或宣传收益 |
| M2 bitstream/context | 未开始 | 新 major，不改 v1 golden |
| M3 IP-2 zero-motion | 未开始 | 第一条可解码 P 路径 |
| M4 motion | 未开始 | 由 M1 数据决定是否实施 |
| M5 MOV/CLI | 未开始 | I-only stss |
| M6 App decode/seek | 未开始 | 无第二套调度器 |
| M7 Export/proxy/UI | 未开始 | Intra 仍为母版默认 |
| M8 Robustness/quality | 未开始 | sequence fuzz + reference invalidation |
| M9 SIMD/perf | 未开始 | 未达 gate 则 experimental |
| M10 IP-4 | 禁止启动 | 等 IP-2 发布候选通过 |
| M11 发布 | 未开始 | 全平台 required |
