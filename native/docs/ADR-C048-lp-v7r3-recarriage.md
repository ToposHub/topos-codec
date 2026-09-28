# ADR-C048：LP/帧间微 GOP 载体从 V8 换为 V7-R3（major 7 子代际）

日期：2026-09-13 · 状态：**已采纳**（LP 重启立项的载体决策）
关联：ADR-C046（版本预算纪律）、ADR-C047（zero-motion IP-2 拍板）、
ADR-C034/C036（V7-R→V7-R2 子代际先例）、`docs/codec/topos_lp_gate_review_2026-09-14.md` §5/§5.1（实测依据）

## 1. 背景

V9 复审 + 端到端实测（gate review §5/§5.1）暴露 V8 同构载体的两个结构性代价：

1. **编码 ~10× 慢**：`v8_encode_plane` 单线程串行（V7-R2 是 band 并行），
   4K 静态帧 V8 机器地板 ~155ms（LT 全帧 27~36ms）；加残差/重建标量环
   ~130ms → LP 4K 编码 3~4 fps，距实时差 8×。
2. **同 qp 体积代差**：V8 段化 order-0 联合模型 vs V7-R2 order-1 上下文
   ——实测视内容 1.04×（纯噪声）~1.33×（静态 1124KB vs 847KB@qp72）。
   端到端 LP 只在冗余高的内容上赢产品 LT，运动/颗粒层净负。

## 2. 决定

**D1（载体）**：帧间微 GOP（LP）的像素载体从 V8 同构换为 **V7-R2 同构**
——新增子代际 **V7-R3：(major 7, entropy_mode 8)**，cfg 选择子
**reserved[0]=11**。编码/解码直连现行 V7 band 并行机器（预计 4K P 帧
编码 30~40ms、解码 7~12ms，机器代差归零）。

**D2（代际纪律）**：**不重定义已冻结的 V9**——V9（major 9，三元组
(9,0,0)）规格、golden_v9/golden_mov_v9、读端行为原样保留（experimental
能力，可读可写）；V7-R3 是增量新代际，先例 = (7,6)→(7,7)（ADR-C034→
C036）。cfg 域 reserved[0] 扩为 {0,1,8,9,10,11}（11 = V7-R3；2..7 维持
退役封存）。**不消耗 major 10**（版本预算纪律，ADR-C046 §4）。

**D3（帧头）**：53B 帧头的 V2.1 占位字节在 (7,8) 激活，规则组与 V9
§3.2 完全同构：三元组唯一 (8,0,0)；frame_type∈{0=I,1=P}；ref_distance
I=0/P=1；gop_id 全域 u16。(7,6)/(7,7) 维持 V2.1 强制零，行为不变。

**D4（读端语义）**：P 包无状态像素解码 → TC_ERR_STATE（查询模式开放，
与 V9 同构）；GOP 序列/参考事务全部复用 topos_gop_context（载体插头化：
scan/encode/decode 三个触点按代际分派）。

**D5（容器）**：tpcC major=7 不变（无新 major 接线）；mov 的 V9 逐
sample 同步链/GOP 链校验条件扩展为「major 9 或 (major 7 ∧ em 8)」。

**D6（门，重申）**：V1..V9 全部 golden SHA 零变化仍是硬门禁；V7-R3 新
golden 另立。**§7 体积出货门不因换载体重赛**——同机器对比下载体抵消，
中位收益预期仍在 ~2~13%（<20%），LP 是否注册产品档仍是独立的产品决策；
本 ADR 解决的是「重启引擎的技术底座」，不是出货裁决。

## 3. 被否备选

- **并行化 V8 机器**（~95% 时间可并行，预估 4K 40~55ms）：周级工程，
  改的是共享机器（V8 读端/v8 生产切换候选同受影响），且体积代差仍在
  ——两账并算不如换载体。
- **major 10 新代际**：版本预算消耗 + 全链 plumbing（tpcC/scan/accept
  list/decoder-ctx），收益仅为「代际标识更醒目」，被 (7,6)→(7,7) 先例
  否决。

## 4. 影响

- 计划：`docs/codec/topos_v7r3_micro_gop_plan_2026-09-13.md`（批 0~5）。
- capability manifest 增 v7r3 段（先改 manifest 再实现，批 4 前置）。
- `ToposVideoEncoder gop='ip2'` 写端切至 em=11（批 4）。
- 追记（2026-09-14）：R3 载体实时化落地——GOP 残差/重建环行带池并行
  + 绑定层每帧 bound 缓冲重建修复，4K LP 编码 6.4~7.1 → **49.7~57.7 fps**
  （≈2× 25p 实时），码率逐字节不变；实测与分账见 gate review §5.3。
