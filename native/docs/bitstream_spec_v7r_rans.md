# TPIC V7.0-R：per-slice rANS 熵编码（ADR-C034）

V7-R 是 V2 符号语义 + per-slice 精确模型的 rANS 重编码。独立于 V1–V6。
**ADR-C035（2026-09-10）起为产品默认熵**（同码率档位落点 qp 低 0~4 档）。与 V7-A（major=7, entropy 5/5/2 的
分带实验格式）共享 major 编号，按 `entropy_mode` 区分：V7-R 唯一合法
三元组 `(entropy_mode, codebook_version, coding_mode) = (6, 0, 0)`。

## 1. 帧入口

`topos_frame_config.reserved[0]=7` 生成 `version_major=7`、
`entropy_mode=6`、`codebook_version=0`、`coding_mode=0`；`minor` 恒 0。
帧头其余字段、slice table、plane tiling、alpha（恒 Rice）与 V2 完全一致。
颜色 slice 的 `k1/k2/k3` 无语义，恒 0 入流（slice_map 拒绝非 0）。
编码选择域：`reserved[0] ∈ {0..7}`（8 起拒绝）。

## 2. 颜色 slice payload 布局

```
[121B 三族计数表][4B rANS 终态（大端）][rANS 数据字节（正序）]
```

- 表：三族连续——DC_CAT 29 字节、RUN 64 字节、LEVEL_CAT 28 字节。
  族内字节为**比例量化计数**：
  `byte[s] = count[s]==0 ? 0 : clamp(1,255, round(count[s]·255/family_total))`；
  全零族写 `{1,0,...}` 占位（保证可建模型；该族必无符号被编解码——
  合法流中仅 LEVEL_CAT 可能空，如极低码率 slice 无 AC 对）。
- 两侧从同一 121B 表重建模型（`tc_rans_model_build`：确定性
  largest-remainder 归一化到 Σfreq=4096，count>0 → freq≥1），
  表只承载比例、绝对尺度无关。
- **实现期对 ADR-C034 原文的修正**：ADR 冻结时表语义为
  `min(count,255)` 裸截断；实现时发现大 slice 中主导符号（如 qp48 的
  run=0，计数上千）会被压到 ~5% 概率、编码端按同表建模型导致体积爆炸，
  故改为比例量化——每符号概率量化到 1/255 粒度（KL 损耗 <0.01
  bit/符号），字节预算与 ADR 相同（121B/slice）。

## 3. 符号序列与 rANS 参数域

符号序列与 V2 canonical VLC 逐符号同源（差分测试钉死：同输入同 qp，
V7-R 与 V2 两流解码重建逐位一致）：

```
块 := DC_CAT [后缀] { RUN LEVEL_CAT [后缀] }* RUN(63)=EOB
```

- `DC_CAT = bitlen(map_signed(dc − pred))`（map = zigzag 有符号映射，
  符号折进 LSB）；后缀 = `cat−1` 个幅度位（cat≤1 无后缀）。
- `RUN ∈ 0..62`；`LEVEL_CAT = bitlen(map_signed(level))` + 同构后缀；
  `EOB = RUN 63`（每块恒有，含全零块）。
- 域校验同 V2：`m ≤ 2^27`(DC) / `2^26`(level)、`pos+run ≤ 63`、
  DC 重建 ±2^25 检查。

rANS（ryg byte 变体）：状态 `x ∈ [2^23, 2^31)`，逐字节重整化，
`x_max = 2^19·freq`；概率精度 12-bit（CDF 总 4096）；单状态**后向编码 /
  前向解码**；解码侧三族模型配 4096 槽 LUT（slot→sym O(1)）。类别后缀
  经**原始位旁路**（ryg 变体：b 位值直接折入状态流，编码
  `x = (x<<b)|v`、重整化阈值 `2^(31−b)`，解码 `v = x&mask` 后右移 +
  补位——恰 b bit 成本，与 V2 VLC 的字面后缀位序语义一致）。b ≤ 23
  单块；b ∈ (23,27] 拆 [高 b−16 位][低 16 位]两块发射（解码先读低块、
  `(hi<<16)|lo` 重组）——逐块不变量：运算输出域 ⊆ [2^23, 2^31)
  （配对推导见 `src/entropy/rans.h` 注释）。每符号 ≤2 数据字节 +
  终态 4B；流恰好消费（`dec.pos == dec.size`，多余尾字节 = MALFORMED）。

## 4. 确定性与错误语义

- 确定性：同输入 + 同配置 → 逐字节相同 packet（无墙钟/线程/浮点依赖；
  编码两遍——token 遍计数、后向遍编码——均在 slice 任务内完成）。
- 旧解码器对 major=7 白名单外/熵三元组不符 → `UNSUPPORTED_VERSION`
  干净拒绝；V7-A 识别（`tc_packet_is_v7a`）同步改为熵 5 判据，
  V7-R 包走常规 packet 解析。
- 畸形域：表不可建模型（全零族）、终态 `< 2^23`、CDF 空洞、域外幅度、
  尾随字节 → slice 级失败 → concealment（spec §9，与 CRC 失败同语义）。
- 帧级结构（header CRC、slice 边界、精确耗尽）与 V2 同一套校验。

## 5. 编码器集成与 sized 搜索

- 生产路径：token 遍（`fill_color_band[_from_f]_tokens`）复用 VLC 直方图
  采集得到三族精确计数 → `emit_color_tokens_rans` 写表 + 后向编码。
  token 存储不可用（OOM）时硬错误，不静默降级 Rice（同 V2-VLC 约束）。
- sized 搜索（`--target-mb`）：V7-R 走 legacy 整帧编码 probe
  （C1/C2 先例——精确位计数需真实后向编码，速度优化后置）。
- 解码：四个 slice 入口（数组/流式/scratch/to-plane）统一路由到
  `tc_color_rans_decode_core`（sink/rowmask 契约与 C1 流式版一致，
  融合 IDCT 与 reduced store 直接复用）。

## 6. 验收门（ADR-C034）

同 qp 体积 ≤ V2 −5%（qp24）/ −3%（qp48）；解码 ≥ 0.8× V2；
单测/ASan/fuzz 金样本绿。达标后再议产品默认切换。A/B 实测数据见
`docs/codec/topos_rans_ab_2026-09-10.md`。
