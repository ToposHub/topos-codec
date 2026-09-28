# ADR-C036：V7-R order-1 上下文扩展（lvl 族条件模型）

- 状态：**已实施**（entropy_mode=7；'rans2' 已由 ADR-C038（2026-09-11）
  转为产品默认）
- 日期：2026-09-10
- 前置：ADR-C034（V7-R rANS）、ADR-C035（产品默认切换）
- 测量证据：`docs/codec/topos_ctx_ceiling_2026-09-10.md`（含两次勘误）
- 位流规范：`bitstream_spec_v7r2_ctx.md`

## 决策

在 V7-R 位流内为 LEVEL/DC 符号族引入 order-1 因果上下文条件模型，
per-slice 按"量化表精确代价 + 表字节"argmin 选模并经 flags 信令。
符号序列/后缀旁路/slice 契约不变——同 qp 重建与 V7-R 逐位一致。

> **勘误**：本 ADR 初版依据未修正的测量声称预期 −15~25% payload。
> 测量工具两处 bug（L 符号轴粗类化、D1 上下文非因果）修正后诚实
> 天花板为 +1.2~2.7%（详见天花板报告 §6）。实施按诚实数字验收。

## 实施范围

1. lvl 族候选 {order-0, 位置桶 L4(4ctx), 前 lvl 桶 L2(5ctx)}；
2. dc 族候选 {order-0, 前块 DC 类桶 D1(5ctx)}；
3. run 族不做（≤0.3%）；组合上下文不做（信息一维）；
4. 条件表 8-bit/symbol 行内比例量化（与 121B 表同语义；4-bit 弃用
   ——量化损耗抵消表字节节省）；前缀上界 350B/slice。

## 验收（实际）

| 门 | 结果 |
|----|------|
| 体积 vs V7-R（1440p 真实素材 qp20-84） | **−1.2~−2.75%**（峰 qp56），与勘误后天花板吻合 |
| 解码速度 | ~1×（±5% 噪声内） |
| 差分 bit-exact | vs V2-VLC 与 V7-R 全 qp 段逐位一致（5 图案 × 444/GBR × 12-bit × alpha） |
| 确定性 | 同输入逐字节一致 |
| 畸形域 | flags 保留位/模型 3/表行不可建/截断 → MALFORMED 或 concealment（单测覆盖） |
| 回归 | debug/ASan 单测全绿（含 test_rans2_slice） |

## 产品语义

- `entropy_mode='rans2'`（reserved[0]=8）；CLI `--entropy rans2`；
  RDO 仍须 'v2'（码率模型域）；TPIM 图片序列域内回退 v2。
- m7 sized 搜索：精确探针走同路径（逐字节一致），tier 模式可用。
- 默认不翻：−1~3% 量级不构成默认切换的充分理由（对照 C035 的
  −8.7~9.1%）；待长期 soak 后由 ADR-C037 类决策评估。

## 明确不做

- run 族上下文；CABAC 式在线自适应；跨 slice 表共享；
- 4-bit 表粒度（净负收益）。
