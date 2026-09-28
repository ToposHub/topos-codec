# ADR-C019：R4.5 —— SIMD 差分一致性扩展（格式矩阵钉入门禁）

- 日期：2026-08-30
- 状态：已接受（实施完成）
- 上下文：`docs/Topos_V2.0_完成度审计与整改计划_2026-08-30.md` §4 R4
  子阶段 5（"AVX2/NEON differential parity"；六子阶段严格串行）
- 关联：ADR-C002（变换/量化整数界按 12-bit 推导——格式无关的根基）、
  ADR-C009（阶段 9 SIMD/并行交付与差分门禁）

## 1. 背景

审计 R4 子阶段 5 要求 AVX2/NEON 与标量参考在新格式上保持差分一致。
关键事实（侦察确认）：**SIMD 内核（正/逆变换、量化/反量化）自阶段 9 起
就与像素格式无关**——内核只吃 int16/int32 系数块，位深/色度结构/profile
全部是几何与枚举层的概念，不进内核；整数界按 12-bit 推导（ADR-C002
C-20，`|x'| ≤ 2047 → |F| ≤ 2²⁵`），golden_transform 自 R4.1 起已含
depths{10,12}。

因此 R4.5 是**验证扩展轮**（无运行时代码变更）：把"内核格式无关"这一
不变量以 R4.1–R4.4 的完整格式矩阵钉进门禁，任何后续内核改动对新格式
漂移立即失败。

## 2. 决策

### C-141：backend parity 扩为格式矩阵 + 解码端 + Alpha

`test_end_to_end_backend_parity`（test_stage9.c §5）从"bd 10/12 轮换"
扩展为：

- **组合矩阵**（11 组合法组合）：pf{0,1,2} × bd{10,12} × profile
  {3,5,6}（Pro444/Extreme 的合法子集，profile/格式交叉规则下 5+pf0、
  6+bd10 等非法组合不进入差分——它们在帧头校验即拒）；
- **编码端差分**：同输入 SCALAR vs FORCE 后端逐字节一致（含
  alpha mode2 a8 平面——MED 残差路径同样无格式分支）；
- **解码端差分**（R4.5 新增）：同一位流两后端解码，四平面
  （全宽/半宽 chroma 按 pf 几何）逐字节一致；
- 随机输入值域直接打满 `2^bd`（12-bit 域），全宽 chroma 用 pf 派生
  尺寸分配（R4.2 的 image_synth 越界教训：测试侧几何必须与 cfg 一致）。

### C-142：验证口径（first-run evidence，非 fail-first）

本轮无生产代码变更，扩展测试**首跑即绿**即是结论本身——"内核格式
无关"在完整格式矩阵上成立。fail-first 纪律不适用（无行为可先失败）；
门禁意义上该测试从此是回归网的一部分（5 sanitizer 配置全跑）。

### C-143：NEON 覆盖边界（如实声明）

本机（macOS x86_64）运行 AVX2 vs scalar 差分；NEON 侧由门禁的
arm64 交叉编译检查保证可编译，**运行时 NEON 差分在 x86 宿主不可执行**
——NEON 内核与 AVX2 共用同一整数界推导与同一标量参考（ADR-C009），
标量参考是两端的共同锚点。此边界记录在案，不声称已运行时验证。

## 3. 验证

- stage9 首跑全绿（本机 AVX2 FORCE 后端）；完整 STAGE-10 门禁
  （5 sanitizer 配置 × 40 测试）见 run_tests.sh。
- 广域回归：media 8 / video+deliver 1 / edit 11——失败集合与基线
  逐项一致（本轮零运行时变更，回归为防御性确认）。

## 4. 遗留

- R4.6：Python binding / MediaSource / GPU upload / 导出 / UI 能力
  协商收口（六档展示、444/GBR pix_fmt 选项、timeline codec_map、
  GPU 444/GBR 端到端渲染验证）。
- NEON 运行时差分：待 arm64 CI 宿主（当前仅交叉编译检查）。
