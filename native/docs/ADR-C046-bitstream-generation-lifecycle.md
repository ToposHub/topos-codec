# ADR-C046：位流代际生命周期与退役规程（V 代际收纳）

- 状态：**已实施**（2026-09-13；批 0~5 全量门禁绿，执行记录见
  `docs/codec/topos_consolidation_audit_2026-09-13.md` §5b/§5c/§5d）
- 关联：topos_version_consolidation_plan_2026-09-13.md（上位计划）、
  topos_consolidation_audit_2026-09-13.md（批 0 审计与逐批补录）、
  ADR-C027（版本规则单点 D-8）、ADR-C034/C036/C038（V7-R/rANS2 设计与
  产品默认）、topos_v8_production_switch_2026-09-12.md（V8 切换拍板，
  §7 归档记录）、topos_v9_micro_gop_plan_2026-09-13.md（后续窗口引用本 ADR 纪律）
- 变更：**不动任何保留代际的位流语义**（保留代 golden SHA 全程零变化，
  硬门禁）；写面收缩 + 死轴退役 + 能力面对齐 + 本纪律入库。

## 1. 背景与裁决

8/29~9/12 两周实验梯子产出 8 个位流代际（V1..V8）。"每个实验轴各开一个
major"的纪律本身正确（旧位流零风险），但留下大量无产品价值的死轴：版本
规则分支、job 分派分支、fuzz 面、测试宿主、以及后续计划文档（TRAW/V9）
反复解释它们的认知成本。

**裁决**：代际只能收敛，不能合并——一代要么可读要么不可读，没有中间态。
收敛 = 写面收缩到最少代 + 读面保留最小必要集 + 死轴干净拒绝。

## 2. 代际终态表（2026-09-13 冻结）

cfg 选择器 em 与位流 (major, entropy) 是两套编号（计划期曾混用，审计 §0
勘误），本表以映射冻结：

| cfg em | 代际 | 位流 (major, em) | 终态 |
|---|---|---|---|
| 0 | V1 Rice 基线 | (1, –) | **保留读写**（显式回退入口） |
| 1 | V2-VLC | (2, 1) | **保留读写**（RDO 唯一宿主） |
| 2 | V2-Rice | (2, 0) | **退役**（读端 UNSUPPORTED_VERSION） |
| 3 | V3 帧内预测 | (3, 1) | **退役** |
| 4 | V4 C1 AC-pair | (4, 2) | **退役** |
| 5 | V5 C2 逐 slice 表 | (5, 3) | **退役** |
| 6 | V6 range intra | (6, 4) | **退役** |
| 7 | V7-R | (7, 6) | **退役** |
| 8 | V7-R2 | (7, 7) | **保留读写**（产品默认，ADR-C038） |
| 9 | V8 | (8, 8) | **保留读写**（8K GPU 唯一路径） |
| — | V7-A band | (7, 5) 专用语法 | **归档**（D1：零产品消费者） |
| — | V7-B 可伸缩 minor1..4 | (7, TPLD 目录) | **保留**（REFERENCE，TRAW 批 3 底座，本窗口未触碰） |

产品写面终态：**3 入口 1 默认**——`entropy_mode ∈ {v1, v2, rans2}`（缺省
'rans2'）+ CLI `--entropy v1|vlc|rans2|v8`；读面终态：V1、V2-VLC、V7-R2、
V7-B（opt-in）、V8 五代。

## 3. 退役规程（本 ADR 固化，后续退役照此执行）

1. **写端先收**：cfg 域校验对退役 em 返回 `INVALID_ARGUMENT`，信息含
   "retired (V consolidation …, see ADR-C0xx)" 与合法域——**禁止静默回落**
   （写拒绝 ≠ 回退 V1）。CLI 同口径（显式 error 行 + usage 更新）。
2. **读端干净拒绝**：`TC_ERR_UNSUPPORTED_VERSION`，信息含
   "generation N (名称) retired (… see ADR-C0xx)" 与回放指引。单点实现于
   frame_header（decode 接受清单 + validate 规则块；V7-A 在探测链/容器
   分支），错误文案是契约的一部分——诊断工具必须能解释文件为什么打不开。
3. **测试宿主先迁**：以退役代际为宿主的 golden/conformance/fuzz 在退役
   前迁到保留代际（等价门：case 网格逐格不变、用例总数不降、golden SHA
   零变化）；真实退役流归档 `tests/fuzz/corpus_retired/`。
4. **能力面同步（manifest 先行）**：capability_manifest.json 先改，native
   广播、公共头、binding、门测试随后，四方一致由门测试钉死。
5. **考古回放（TOPOS_DEV_REPLAY 双重门）**：退役代码不入删除（归档保留）；
   回放需同时满足 构建期 `-DTOPOS_DEV_REPLAY=ON` **且** 运行期
   `TOPOS_DEV=1`。生产/默认构建零回放面（行为级 + 能力位级双证）。
6. **退役 em/major 编号永久封存**：em 2..7 与 (6,4)/(7,5) 等 retired 组合
   不再分配给任何新语义；新代际走新编号（V9 = cfg em10 / 位流延续既有
   映射规则），防"同选择器不同代"考古灾难。

## 4. 版本预算纪律（防死轴再生产）

1. **新 major 只留给入产窗口**：开新 major 前必须在计划文档登记
   （a）入产判据与切换条件（参照 V8 三重开条件）、（b）golden/conformance
   隔离方案、（c）退役评审点。
2. **实验轴隔离**：实验性熵核/预测轴不占新 major——用 dev 旗标
   （`tc_dev_*`）或独立分支 + 测试构造（packet_synth/golden gen）隔离；
   实验证明入产价值后，按 §4.1 走正式 major 立项。
3. **进新 major 必须同步登记前序死轴退役评审点**：立项文档里逐代核对
   §2 表，确认没有"只进不出"的代际堆积；两次收纳窗口的间隔不应超过
   一个大计划周期（本窗口为两周八代的纠偏基准）。
4. **每代必须有宿主淘汰预案**：新增代际的 conformance/fuzz 宿主在立项时
   标注"若该代退役，宿主迁往何处"（本窗口迁移清单见审计 §3.4）。

## 5. 保留代保护条款

保留五代（V1/V2-VLC/V7-R2/V7-B/V8）的位流语义、熵核、量化矩阵、帧头
布局**任何变更都必须走 minor 版本流程 + golden 重新生成 ADR**； golden
SHA 变化在无 ADR 情况下即为 conformance 事故。V7-B 在 TRAW 批 3 落地前
维持 REFERENCE 状态（RD4-08 门禁未过，见 tc_query_scalable_status 注释）。

## 6. 内部规则域复用豁免（实施注记）

`validate_v7a_common_fields` 将 V7-A 元数据伪装成 (6,4) 复用 V6 域规则。
退役闸门实现为 `validate_impl(fh, allow_retired)`：公共入口恒 =
`tc_dev_replay_enabled()`，规则载体传 `allow_retired=1`。**教训**：退役
闸门落点前必须普查"复用退役代际规则域"的内部路径（本窗口由测试插桩
定位），后续退役照此检查。

## 7. D4 合并拍板：V8 生产切换记录

V8（cfg em9 / 位流 (8,8)）**已在读写两侧保留并交付**（8K GPU 唯一路径），
但"生产默认切换"独立拍板。该拍板并入本 ADR 归档，判定条件仍以
`docs/codec/topos_v8_production_switch_2026-09-12.md` 的三重开条件为准
（编码器批 2 落地、GPU 流水兼容验证、生产 A/B 达标），本 ADR 不改变其
独立判定语义；切换执行时在该文档登记即可，不再另开 ADR。
