# ADR-C038: V7-R2 ('rans2') 转产品默认熵 + 死区加宽否决

- 状态：**已采纳（默认切换已实施）**
- 日期：2026-09-11
- 前置：ADR-C035（rans 转正先例）、ADR-C036（V7-R2 交付，"默认不翻，
  待 soak 后由 ADR-C037 类决策评估"）、ADR-C037（16×16 试点 + 勘误）
- 本 ADR 同时裁决本批的另一候选（无损平台死区加宽）：**否决**，理由见 §3。

## 1. 决策

产品视频路径默认熵模式 **'rans' → 'rans2'**（V7-R2 上下文 rANS，
version_major=7 / entropy_mode=7）。'rans'/'v2'/'v1' 保持显式可选，
历史码流自描述永远可读。

### 变更点

- `src/shared/export/video_encoder.py`：`entropy_mode` 默认值 + 注释；
- `src/shared/export/topos_encoder.py`：属性缺失回退默认 + 注释；
- `src/shared/codec/topos_profiles.py`：capability summary
  `entropy_default = "v7r2-rans2"`（无消费者钉死，已核实）；
- 图片序列（TPIM）路径维持域内回退 v2 不变；alpha 预算探测钉 V1 不变；
  native 演示 CLI 默认（v1 Rice）不变（C035 先例同样未动 CLI）。

### 依据（ADR-C036 交付时已测 + 本次 tier 级复验，1080p 真实调色素材）

- 固定 qp 路径：qp28/56/64/72 payload **−0.95% ~ −2.16%**（同 qp =
  逐位同画质，C036 全段 −1.2~2.75%）；
- sized 档位路径（同目标字节）：hq 档落点 qp 63→62 = **+1.05 dB**；
  proxy/lt/standard 单帧落点相同（搜索粒度所致，payload −0.6~−1.1%），
  多帧码控反馈下按 C035 先例累积为落点 qp 下移；
- 解码速度 spot：31.8 vs 30.8 ms/帧（~1× 包络内，C036 结论不变）；
- 回归：native debug/ASan 79/79；export 112/112；media 2702 通过，
  失败集与 HEAD 基线一致（VT zero-copy ×1 + fixtures manifest ×6）。

## 2. 无损平台表述勘误（连带 ADR-C037）

本批核实：sized 档位编码实测落点 qp54-84（ADR-C032/C035 落点表），
属**有损域**；无损平台（flat qp≤44 / std qp≤32 逐位无损，ADR-C037
发现）覆盖的是 crf/低 qp 导出路径。ADR-C037 初版"产品 tier qp 域
在无损平台内"的表述已在 ADR-C037/试点报告/战役清单同步勘误。
16×16 NO-GO 判定不受影响（其有损域数据 geo 1.18-1.21 恰覆盖档位域）。

## 3. 候选否决：无损平台死区加宽

立项动机：无损平台内量化误差低于重建舍入粒度 → 舍入翻转边界前加宽
编码端死区理论上纯省码率。否决依据：

1. **前扫频已覆盖工作域**：2026-09-03 战役 P3（TC_DZ_DEV 旋钮，
   dz 0~6/16 × 2K422/2K444/4K444/4K44412）结论——工作码率区间
   （1.4~3.6 bpp ≈ lt/standard/hq 档域）1/4 已最优（优于 2/16 ~0.5dB、
   0/1 ~0.9dB；仅近无损区 dz=0 占优）；sized 码控下 dz 为 RD 中性
   （qp 搜索补偿）。
2. **真平台内收益量级小**：颗粒素材无损档 nz≈94%（小系数本就少数），
   加宽死区零化的边际系数有限，预期 <1% 且仅惠及 crf 低 qp 路径。
3. **规范性成本高**：spec v1 §7.4 量化公式列入 A.8 冻结清单
   （golden_transform/golden_codec 门禁），编码字节输出是规范对象
   （§13.3）；改 dz = spec 修订 + 全套 golden 再生，成本与收益倒挂。

随批清理：`tc_quant_ctx_init` 的 TC_DZ_DEV 实验旋钮按其自注释
"扫完移除"删除（P3 已销账，全库无使用方）；冻结值注释的 spec 章节
号笔误（§7.3→§7.4）一并修正。编码输出零变化（golden 全绿即证）。

## 4. 影响与回退

- 新导出默认走 entropy=7（旧解码库拒绝未知熵 → 版本协商按既有
  ABI 机制处理；单库产品内无影响）；
- 回退 = 把三处默认值改回 'rans'（单一提交可逆）。

## 5. 版本

- 2026-09-11：采纳并实施；回归全绿。
