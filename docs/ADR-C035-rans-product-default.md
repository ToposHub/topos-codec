# ADR-C035：V7-R rANS 转产品默认熵

- **状态**：已采纳（2026-09-10）；**被取代** → ADR-C038（rans2 转产品默认；V7-R 代际
  已于 2026-09-13 退役，见 ADR-C046）
- superseded-by: ADR-C038-rans2-product-default.md
- **背景**：ADR-C034 验收门全过（体积 qp24 −8.68% / qp48 −9.05%，
  解码 1.14×/1.19× 反超 V2，debug+ASan 76/76；A/B 报告
  `docs/codec/topos_rans_ab_2026-09-10.md`）
- **决策**：产品编码默认熵 `entropy_mode` 'v2' → **'rans'**
  （reserved[0]=7 → major=7/entropy 6）。'v2'/'v1' 显式可配（码流自描述，
  历史文件永远可读；本产品无存量部署包袱——切换零迁移成本）。

## 前提工程（本 ADR 随附交付）

1. **m7 精确探针**：rANS 原走 legacy 整帧 probe（sized 搜索每候选
   qp 全帧重编码，产品档位导出慢 3~5×）。现已并入 DCT-once 路径：
   `color_tokens_rans_encode` 拆分核心（bw=NULL 计位探针）与落盘，
   probe 与最终编码同代码路径 → 逐字节一致。实测 1440p 档位导出
   46 fps（30 帧 0.65 s vs v2 0.43 s）。
2. **444/GBR 差分覆盖**：pf=1（YUV 4:4:4）/pf=2（GBR identity）
   加入 test_rans_slice 差分（pro444/extreme 档域）。

## 产品语义影响

- **质量锚定路径（crf/qp）零变化**：rANS 为无损熵替换——同 qp 量化
  系数与重建逐位一致（差分测试钉死）。
- **码率锚定路径（tier 目标）**：P5 tier 目标是 ProRes 锚定的 bpp 值，
  与熵无关——sized 搜索自适应落点。实测（1440p 真实素材 30 帧，
  档位目标）：

| 档位 | 目标 | v2 落点 qp / 利用率 | rans 落点 qp / 利用率 |
|---|---:|---:|---:|
| proxy | 28 Mbps | 82 / 94.6% | **78** / 92.4% |
| lt | 100 Mbps | 71 / 91.8% | **70** / 94.4% |
| standard | 145 Mbps | 69 / 92.1% | **68** / 91.9% |
| hq | 235 Mbps | 65 / 95.2% | 65 / 91.5% |

同码率下 rANS 落点 qp 恒低 0~4 档 = 画质严格更优（proxy 档最显著）。
解码同速或更快（hq 档 30 帧 194 ms vs 200 ms）。

- **图片序列路径例外**：TPIM 容器 IDSC payload 域为 TPIC V1..V6
  （image_container.c 域校验）——`ToposImageSequenceEncoder` 在
  构造后覆写回 v2（含跟随新默认的情形），容器域扩展（major=7 入
  TPIM）另行立项。
- **RDO**：`rdo_mode='on'` 仍要求显式 `entropy_mode='v2'`（码率模型
  基于 VLC 符号位；rANS 域的 RDO 重定标另行立项）。

## 测试

- 单测：test_rans_slice 增 444/GBR 差分；m7 探针路径经 sized CLI
  A/B 验证（qp 落点单调、贴目标）。
- 产品：test_topos_export 111/111（RDO 测试显式 v2；帧头 major pin
  2→7 ×7 处）；tests/media + tests/native 全套 2740 通过，失败集 =
  HEAD 预存 7 项（VT zero-copy / FixturesManifest×6）无新增。

## 风险与后续

- 编码开销 1.35~1.55×（后向遍逐符号除法）——Barrett 倒数乘法/4 状态
  交错为既定后手；当前离线导出 46 fps@1440p 可接受。
- TPIM 容器域扩展（图片序列享用 rANS）。
- rANS 域 RDO 重定标（+444 家族 flat 矩阵验证）——与 ADR-C033 遗留
  合并为下一批次。
