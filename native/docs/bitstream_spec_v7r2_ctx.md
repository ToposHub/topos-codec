# V7-R2 位流规范：rANS order-1 上下文扩展（ADR-C036）

状态：**已实施**（entropy_mode=7；选择值 reserved[0]=8）。V7-R
（`bitstream_spec_v7r_rans.md`）的超集：符号序列、后缀旁路、EOB、
slice 契约完全不变——仅 LEVEL/DC 符号族可按解码序因果上下文条件化。

## 1. 帧头与选择

- `version_major=7`，`entropy_mode=7`，`codebook=0`，`coding=0`
  （帧头白名单唯一合法三元组 (7,0,0)）。
- 编码配置 `reserved[0]=8`（`cfg_to_frame_header` em==8 分支；
  `tc_frame_config_validate` 域 {0..8}）。
- V7-A（entropy 5）/V7-R（entropy 6）分流判据不受影响
  （`tc_packet_is_v7a`、`movie_parse_packet_structure` 依旧按
  entropy==5 识别 V7-A）。
- 颜色 slice：k1/k2/k3 恒 0（slice_map 契约同 V7-R）；alpha 恒 Rice。

## 2. 颜色 slice payload 布局

```
[flags 1B][dc 表][run 表 64B][lvl 表][rANS 流（V7-R 同布局）]
```

- **flags**：`[1:0]` lvl 模型（0=order-0，1=位置桶，2=前 lvl 桶；
  3 保留）；`[2]` dc 模型（0=order-0，1=前块 DC 类桶）；
  `[7:3]` 保留恒 0（非零 → MALFORMED）。
- **dc 表**：order-0 → 29B；D1 → 5×29B（每 ctx 一行）。
- **run 表**：恒 64B order-0（测量：run 族条件化 ≤1.5% 且高 qp
  负收益——不做）。
- **lvl 表**：order-0 → 28B；L4 → 4×28B；L2 → 5×28B。
- 行内 8-bit 比例量化（`tc_rans2_row_encode`，与 V7-R 121B 表同语义：
  `byte[s] = count[s]==0 ? 0 : clamp(1,255,round(count·255/row_total))`；
  全零行占位 `{1,0,...}`——该行无符号永不查询）。行序 = ctx 0..C−1。
- 前缀上界 `TC_RANS2_PREFIX_MAX = 350`B；最小 payload = 126B。

## 3. 上下文域（全部为解码序因果状态——零解码端成本）

| 模型 | ctx 数 | 上下文（桶函数） |
|------|-------|----------------|
| lvl L4（位置） | 4 | 本系数 zigzag 位置 {1-7, 8-15, 16-31, 32-63} |
| lvl L2（前 lvl） | 5 | 前一对（同块）lvl 类 {0, 1, 2-3, ≥4} + 块首 |
| dc D1（前块） | 5 | 前块（raster）DC 类 {0, 1, 2-3, ≥4} + slice 首块 |

## 4. 编码端 per-slice 模型选择

token 前向一遍重建条件联合（`rans2_collect_joints`），对每族候选
{order-0, L4, L2} × {order-0, D1} 计算量化表下的精确符号位
（`rans2_rows_cost`：行表比例量化 → 模型 freq → Σ count·(12−log₂ f)，
表字节计入），族间独立 argmin（候选序 NONE→POS→PREV，严格小于才
切换），结果经 flags 信令——解码端无条件复现，位流确定性不受影响。

**实现注记（§11.1 确定性口径）**：选择代价使用 `tc_ctx_log2`
（自实现 atanh 级数，IEEE-754 确定性纯函数，|err| < 1.5e-6，不引
libm 依赖）。这是编码器内唯一的浮点使用点；同输入同输出可复现
（纯算术、无 libm/平台近似），且选择结果入流信令——即便跨平台
选择不同，解码输出仍由码流自描述决定。

## 5. 确定性与错误语义

- 同输入 → 同字节（含模型选择；单测钉死）。
- 畸形域：flags 保留位/lvl 模型 3 → MALFORMED；条件表行不可建
  （全零无占位）→ MALFORMED；DC/level 幅度越界、AC idx > 63、
  流尾 trailing bytes、终态 < L —— 与 V7-R 同构，经 §9 concealment
  观测（单测覆盖）。
- 解码 LUT：per-ctx（dc ≤5 + run 1 + lvl ≤5 = ≤11 × 4KB，堆分配）。

## 6. 实测（2026-09-10，/tmp/p5m/src1440.raw 2560×1440×10 帧）

| qp | vs V7-R 体积 | 解码速度 |
|----|-------------|---------|
| 20 | −1.23% | ~1× |
| 24 | −1.34% | ~1× |
| 32 | −1.61% | ~1× |
| 44 | −2.29% | ~1× |
| 56 | −2.75% | ~1× |
| 64 | −2.34% | ~1× |
| 68 | −1.83% | ~1× |
| 76 | −1.81% | ~1× |
| 84 | −2.14% | ~1× |

与勘误后的天花板测量（`docs/codec/topos_ctx_ceiling_2026-09-10.md`：
qp20-56 净 +1.2~2.7%）吻合——实现拿满诚实天花板。产品默认仍为
V7-R（'rans'）；'rans2' 显式选用，转正另议。

## 7. 勘误记录（与本格式直接相关的两次测量修正）

1. 天花板测量的 L 族符号轴误用 4 桶粗类（`lvlcat_b`）而非完整
   28 类别——测得粗类条件熵，把 lvl 族天花板高估一个量级；
2. D1 上下文误用本块自身 DC 类（非因果自条件，对角 joint）——
   高 qp 段虚标（qp76 曾误报 +12.7%）。两处已修正并重测；
   详见天花板报告 §勘误。
