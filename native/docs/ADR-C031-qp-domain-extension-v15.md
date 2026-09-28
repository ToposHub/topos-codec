# ADR-C031：qp 值域扩展 0..95（v1.5，Proxy/LT 低码率档打通）

日期：2026-09-10
状态：已实施（debug 74/74 + ASan/UBSan 全绿；spec v1.5 同步）
关联：ADR-I011 修订四（qp63 地板结论）、`topos_profiles.py`（Proxy/LT 目标 bpp）、
`docs/codec/topos_vs_prores_decode_2026-09-10.md`（档位对比中"Proxy/LT 不可达"缺口）

## 背景与根因

同码率档位对比（2026-09-10）发现低码率档不可达：qp 上限 63 时渐变类码率地板
36 Mbps、颗粒类 109 Mbps，而 ProRes Proxy（实测 11 Mbps）与渐变 LT（23 Mbps）
远低于该地板。符号级探针（`tc_color_slice_decode` 直方图）定位两层根因：

1. **平滑内容（渐变）**：qp63 时量化步长（DC=1722）除不动大幅值低频系数——
   Y 平面每块仍有 DC 电平 ±2805（块间差 ±18）与 AC[1]/AC[8] 电平 2-3，
   ~31 bits/块的 DC 差分 + EOB 构成地板；
2. **噪声内容（颗粒）**：qp63 时每块仍有 ~8 个 ±1 电平 AC 存活（噪声系数
   偶越步长阈值），跨全部频率行。

两者都指向"需要再粗 16-32× 量化"——qp 域必须上扩。帧头 offset 29 的
`qp_base` 本就是整字节（非 6-bit 打包；ADR-I011 修订四"头域硬上限（6-bit）"
的说法不成立，实为校验域限制），扩域不动码流布局。

## 决策

- **qp 域 0..63 → 0..95**，v1 流 `version_minor = 4`（v1.5）门控。
  命名空间消歧：本处 minor 是 **v1 帧头 version_minor**（§3 字节 7），
  与 V7-B 目录的 `directory_version_minor`（ADR-C030 的 minor-4 = 共享
  金字塔）是两个互不相关的版本空间。
  qp_scale 表按原公式 `max(1, round_half_up(2^((qp−4)/4)))` 精确扩展 32 项
  （整数四分幂交叉验证；qp95 = 7,053,950 ≈ qp63 的 256×）。
- **writer 取所属扩展代最低 minor**（与 v1.2/1.3/1.4 同规则）：qp≤63 流
  minor 与字节完全不变（既有 golden/corpus 零影响）；qp≥64 流 minor=4，
  旧解码器按 minor>3 干净拒绝。
- **qp_eff 条件钳位**：`qp_base + delta + 位深偏移` 的钳位上界取
  `tc_qp_eff_ceiling(qp_base)`——qp_base≤63 沿用 63（历史字节兼容，
  bd/AQ 偏移顶格压回 63 的行为不变），≥64 放开到 95。
- **V2 流（major=2）qp 域随本变更开放**（minor 恒 0，无门控）。
- sized 搜索（`tc_frame_encode_sized`）与编码配置校验同步放界至 95。

## 修复过程中发现的三个实现缺陷（随本 ADR 一并修复）

1. **m7/sized 路径 minor 不同步**：搜索选定 qp≥64 候选后直接改
   `fh.qp_base` 不重算 minor → 自检 scan 拒绝（UNSUPPORTED_VERSION）。
   修复：minor 选择抽为 `enc_fh_select_minor()` 单一真相源，m7_final 复用。
2. **fastdiv 域外静默恒等**：`tc_fastdiv_init` 对 d > D_MAX 的"保守回退"
   设 magic=0/shift=0，而 apply 把 magic==0 解释为 2 的幂路径 → 返回
   `n>>0`（除法变恒等）→ qp 高段量化电平未除尽（实测颗粒 qp95 每块出现
   ±2.5M 电平、码率反跳 5 倍）。修复：域外走真除（数值恒真），并按
   "N_MAX·D_MAX < 2^50 + round-up magic 精确条件 d² < 2^51"重定域
   N_MAX=D_MAX=2^25−1（覆盖冻结 qm655×qp95 → Q=18,045,205）。
   边界扫描（n=d−1/d/d+1 全域）+ 3M 随机对差分零违例。
3. **两处 qp_eff 钳位遗漏**（并行带循环 2441、AQ 任务 3297）随条件钳位统一。

## 测量（i9-9900K，1920×1080@24，合成素材冻结种子）

码率下探（qmatrix=1 Standard，V1 Rice）：

| qp | 渐变 Mbps | 颗粒 Mbps |
|---|---|---|
| 63 | 36.1 | 108.7 |
| 71 | 21.5 | 22.4 |
| 79 | 16.3 | 17.8 |
| 87 | 13.7 | 14.8 |
| 95 | 12.7 | 12.9 |

- **LT 全覆盖**：渐变 LT（23 Mbps）≈ qp71；颗粒 LT（87 Mbps）介于 qp63-67。
- **Proxy 接近**：qp95 两类 12.7-12.9 Mbps vs ProRes Proxy 实测 11 Mbps
  （差 ~15%）。剩余地板为每块 DC 差分 + EOB 的熵下限（~10 bits/块 ×
  64,800 块），继续下探需帧内预测/上下文算术编码（ADR-I011 修订四
  已列的实质修复方向），不属本 ADR 范围。

同码率档解码速度（16 线程，V1 Rice，默认切片；优化后实测）：新档位点全部
达到或超过 FFmpeg ProRes 软解——渐变 LT 1581 vs 873 fps（1.81×）、渐变
Proxy 2061 vs 886（2.32×）、颗粒 Proxy 2043 vs 845（2.42×）、颗粒 LT
721 vs 734（0.98×，且码率仅一半）；高码率颗粒（Standard/HQ）仍为
0.53-0.60×，根因剖面（idct ~60% / 熵 ~30% / CRC ~2%）与后续优化记录见
`docs/codec/topos_vs_prores_decode_2026-09-10.md`（v2 修订）。

## 兼容性声明

- 旧流（qp≤63，minor≤3）：读侧行为逐字节不变；golden/corpus 全部原样通过。
- 新流（qp≥64，minor=4）：旧解码器 UNSUPPORTED_VERSION 干净拒绝，不误解。
- 解码侧 clamp 语义：q·Q 钳位 ±2^25 不变；qp95 下 |F|/Q ≤ 22 → 电平域
  远小于 VLC cat 31 上限。

## 变更清单

- `src/transform/quant.{c,h}`：kQpScale 96 项；`TC_QP_MAX/TC_QP_V15_MIN/
  tc_qp_eff_ceiling`；uint64 中转防溢出。
- `src/transform/fastdiv.{c,h}`：域重定 + 域外真除修复。
- `src/bitstream/frame_header.c`：minor≤4；qp≤95 + minor 门控。
- `src/bitstream/slice_map.c`：slice 有效 qp ≤95。
- `src/codec/codec.c`：配置/sized 校验放界；`enc_fh_select_minor` 单一
  真相源（enc_frame_setup + m7_final）；4 处条件钳位。
- `src/bitstream/v7_frame_codec.c`：V7A 有效 qp 同步条件钳位。
- CLI/基准：encoder_cli --target-mb 搜索界 0..95、preview benchmark --qp 1..95。
- 测试：test_quant 锚点扩展、test_frame_header minor/qp 门控语义、
  test_codec qp64/95 往返 + 界 96、test_stage9 fastdiv 域扫描扩至 qp≤95。
- 规范：bitstream_spec_v1.md → v1.5（版本注记、帧头域、A.4 表）。

## 后续（不在本 ADR 内）

- 产品层：`topos_profiles.py` 档位 bpp 在 qp95 后是否重新定标（Proxy 目标
  0.605 bpp 在高分辨率素材上已可命中，待实拍素材验证）。
- 熵地板：帧内方向预测 + 上下文自适应算术编码（ADR-I010 未来事项）。
