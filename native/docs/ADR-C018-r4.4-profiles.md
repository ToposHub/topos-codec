# ADR-C018：R4.4 —— Pro444/Extreme profile 激活（六档 capability 收口）

- 日期：2026-08-30
- 状态：已接受（实施完成；**部分被取代**——"独立矩阵/码控"一节的原始预期
  已由 ADR-C023（2026-08-31 复验 O2）正式定案为"矩阵不独立、差异化 =
  格式约束 + 应用码控"，见 `ADR-C023-o2-profile-matrix-decision.md`）
- superseded-by（部分）: ADR-C023-o2-profile-matrix-decision.md
- 上下文：`docs/Topos_V2.0_完成度审计与整改计划_2026-08-30.md` §4 R4
  子阶段 4（"Pro444/Extreme profile 与独立矩阵/码控"；六子阶段严格串行）
- 关联：ADR-C016/C017（格式枚举扩展——本轮激活的格式地基）、
  ADR-C011（R0 范围冻结：Proxy/LT/HQ = profile3 预设的定位）、
  bitstream_spec §4.4

## 1. 背景

审计 R4 完成门槛："六档 capability 不再用 `available=False` 占位，且
header/query/UI/SDK 一致"。R4.1–R4.3 已把 12-bit/4:4:4/GBR 格式能力全部
交付（均以 profile3 交付），Pro444（5）/Extreme（6）的激活只剩 profile
域放开与格式交叉规则——v1.0 spec §4.4 早已定义这两个 profile 枚举，
旧解码器对 5/6 本就按 `UNSUPPORTED_PROFILE` 干净拒绝，**无需 minor bump**。

## 2. 决策

### C-136：profile 域 = {3, 5, 6}；Proxy/LT/HQ 永不实现为原生 profile

帧头/query 接受 `profile ∈ {3,5,6}`。1/2/4 保持"已定义未实现"——
应用层的 Proxy/LT/Standard/HQ 四档是 profile3 + bpp/Alpha 预设
（ADR-C011 P1-5 定位不变），码流层不为它们单设 profile。
`tc_profile_name` 不变。

### C-137：格式交叉规则（Pro444/Extreme 的 profile 语义强制）

- `profile=5`（Pro444）：`pixel_format ≠ 0`（4:4:4 族），bd ∈ {10,12}；
- `profile=6`（Extreme）：`pixel_format ≠ 0` **且** `bit_depth = 12`；
- 违反 → `TC_ERR_MALFORMED`（profile/format 声明自相矛盾，非能力缺失）；
- `profile=3`（Standard）不限格式（v1.2–v1.4 枚举扩展的交付载体）。

`tc_query_support` 同规则（组合非法 → `UNSUPPORTED_PIXEL_FORMAT`），
encoder 在配置期前置拦截（ValueError），三层同一答案。

### C-138：tier → native_profile 映射（capability 声明收口）

`ToposProfileTier` 新增 `native_profile` 字段（proxy/lt/standard/hq=3、
pro444=5、extreme=6）；两档 `available=True`、`unavailable_reason=""`，
`AVAILABLE_TOPOS_TIERS` 扩为六档；encoder `_build_configs` 按 tier 写
帧头/tpcC profile 字节（movie_config 增加 profile 参数，域 {3,5,6}）；
`topos_source._PROFILE_NAMES` 补 5/6 展示名。tier bpp 维持 luma 像素口径
（ADR-C016 C-129 决策延续——Pro444 37 / Extreme 56.5 MB/s 参考定标
已按 4:4:4 内容校准，无需按 pf 再折算）。

### C-139：golden/corps/test 面

`golden_codec_v1_profiles.bin`（12 记录 = 6 配置：Pro444×{pf1 bd10,
pf1 bd12+alpha, pf2 bd12}、Extreme×{pf1 bd12, pf2 bd12, pf2 bd12+alpha}，
fold `3c8b52c2720bd5a6`）。packet_synth 增加 profile 字段；corpus 追加
seed_pro444/seed_extreme 族（204 → 224 文件）。单测：交叉规则
（Pro444+4:2:2 / Extreme+10-bit → MALFORMED；profile4 → UNSUPPORTED）/
Pro444 与 Extreme+GBR+alpha 往返（pkt[10]==5/6）；test_mov Pro444 mux
往返 + tpcC profile 一致性双向（Standard 电影收 profile5 包 →
INVALID_ARGUMENT）。

### C-140：quality CLI 12-bit 报表（ADR-C015 遗留收口）

`topos_quality depth12`：960×544 MIXED × qp{12,24,36} × bd{10,12}
对照表，PSNR 按位深峰值定标（`psnr_plane_bd`，(2^bd−1)² 峰值——旧表
固定 1023² 跨位深不可比）。观察：合成内容在 qp≤24 逐位无损（99=钳位），
12-bit qp36 的 Y-PSNR 低于 10-bit 同 qp——qp 等效偏移 +4（C-120）的
可视化印证。门禁接线 `depth12: OK`。

## 3. 验证

失败先行：frame_header（profile 补丁矩阵 + 正反例）/ abi_compat /
stage10_concurrency / unit_mov / unit_codec（SEGFAULT）/ conformance_
codec_profiles / Python TestR44Tiers 全部先行失败，实现后全绿。
native debug 40/40（新增 conformance_codec_profiles）；Python 22 项
R4.x 套件 + 全量 export/binding/stage10 通过；完整 STAGE-10 门禁与
三套件广域回归见提交记录。

## 4. 遗留

- R4.5：AVX2/NEON differential parity（内核已格式无关；扩展测试面）。
- R4.6：Python binding/MediaSource/GPU/导出/UI 能力协商收口（六档展示、
  444/GBR pix_fmt 选项、timeline codec_map）。
- `topos_quality depth12` 目前覆盖 4:2:2；4:4:4/GBR 的 qp-PSNR 表随
  R4.6 质量面板需求再扩（单测/多代已覆盖格式正确性）。
