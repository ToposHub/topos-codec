# ADR-C034：V2-R per-slice 自适应 rANS 熵编码（立项 + 核心原语交付）

- **状态**：位流集成已交付（2026-09-10）——**验收门全部通过**
  （体积 qp24 −8.68% / qp48 −9.05%，解码 1.14×/1.19× V2，debug+ASan
  76/76 绿）；产品默认切换另议（见「验收与后续」）
- **范围**：新实验编码选择（reserved[0]=7 → version_major=7，
  entropy_mode=6）；V1/V2 语义零变更
- **规范**：`docs/bitstream_spec_v7r_rans.md`；A/B 报告
  `docs/codec/topos_rans_ab_2026-09-10.md`
- **前置调研**：Explore 全景（coding selection 0..6 / VLC 多表结构 /
  C1/C2/V3/V6 实验状态 / k1k2k3 域 / 帧头闲置字段）

## 动机与天花板（2026-09-10 实测）

V2 canonical VLC 的"上下文"仅 slice 级静态 4 选 1（k1/k2/k3 = 书 id）。
以真实素材直方图对拍（suffix 位计入，修正 C0 工具口径）：

| corpus | qp | V2 池化全位 | order-0 理想 + suffix | 节省 |
|---|---:|---:|---:|---:|
| real_2k_prores422hq | 24 | 54,281,657 | 49,919,914 | **8.0%** |
| real_2k_prores422hq | 48 | 15,425,105 | 14,720,376 | **4.6%** |
| synthetic_texture | 24 | 15,995,579 | 14,227,492 | 11.1% |

- 这是 per-slice **精确模型**算术编码对现有符号族的上限（书拟合 + 整数
  码长双份损耗）；per-slice 定制严格优于池化口径 → 实际 ≥ 此数。
- 表传输成本：1B/符号 ×（29+64+28）= 121B/slice，qp24 占 <0.06%、
  qp48 <0.15% —— 可忽略。
- 换算 ≈ +0.3~0.5 dB（同码率）。既有实验路径评估：C1（+73% 体积的
  6-bit 定长实现）不可救、C2（+33%）同、V3 intra（−0.1~−1%、编码
  9~13×）不达门——rANS 是 C2 规范自指的方向。
- **同模型 rANS 零收益**（码长 2^-len 即概率模型 → 位数不变）：收益
  必须来自更好的概率模型 = per-slice 精确计数。

## 设计（冻结）

1. **符号语义与 V2 完全同源**：DC_CAT(29) / RUN(64，含 EOB) /
   LEVEL_CAT(28) + 原始位（suffix (cat−1) 位 + 符号位）经均匀二值模型
   逐位恰 1 bit——无需双流框架，语义与裸写逐位一致。
2. **rANS 参数域**（ryg byte 变体，`src/entropy/rans.{h,c}` 已交付）：
   状态 x ∈ [2^23, 2^32)，逐字节重整化，x_max = (L>>12)<<8 × freq；
   概率精度 12-bit（CDF 总 4096）；流布局 [终态 4B 大端][数据]。
   单状态后向编码 / 前向解码；确定性（同输入同字节）。
3. **模型传输**：每颜色 slice payload 前缀 121B = 三族计数（各符号
   1B，cap 255；0 = 不出现）。解码侧 tc_rans_model_build 重建（确定性
   largest-remainder + count>0 → freq≥1 不变量）。
4. **slice 头**：k1/k2/k3 保持 0（不再选书）；alpha slice 恒 Rice
   （与 C1/C2/V6 同策略）。
5. **帧头**：major=7、entropy_mode=6、coding_mode=0、codebook=0；
   byte48 仍 0。旧解码器 major 白名单干净拒绝。
6. **编码两遍**：第一遍收三族计数（复用现有 token 遍），写表 + rANS
   后向编码。sized 搜索逐 qp 全程可用（C1/C2 先例）。

## 已交付（本批次）

- `src/entropy/rans.{h,c}`：模型构建（不变量钉死）/ 后向编码器 /
  前向解码器 / TRUNCATED·MALFORMED·BUFFER_TOO_SMALL 域校验；
- `tests/unit/test_rans.c`：确定性/往返（8 种子 × 32 试验 × 混排模型）/
  布局/畸形域/均匀位成本，debug 75/75 绿。
- 开发中实修的两个原语 bug（已回归）：flush 布局（数据反转错区）、
  归一化饥饿（count>0 → freq≥1）。参数域教训：L 必须 ≥ T·2^k 保证
  编码后状态 ≥ L（L=2^16 会解码失步——推导见 rans.h 注释）。

## 已交付（位流集成批次，2026-09-10）

- 帧头/配置：`cfg_to_frame_header` selection 7；`tc_frame_header_validate`
  major=7 熵三元组 (6,0,0) 白名单（V7-A (5,5,2) 仍只经专用入口）；
  `tc_packet_is_v7a`/mux 包解析按 entropy_mode 分流（V7-R 走常规
  packet 路径）；slice_map rANS 颜色 slice k1/k2/k3==0 契约。
- 编码：token 遍复用 VLC 直方图采集（单一统计真相源）→
  `emit_color_tokens_rans` 写 121B 表 + 后向编码；OOM 不降级
  （同 V2-VLC 约束）；sized 搜索走 legacy 整帧 probe（C1/C2 先例）。
- 解码：四入口统一路由 `tc_color_rans_decode_core`（sink/rowmask
  契约与 C1 一致，融合 IDCT/reduced store 直接复用）。
- CLI `--entropy rans`；Python `entropy_mode='rans'`（reserved[0]=7）。
- 热路径优化（参考实现解码曾 5.1× 慢于 V2）：三族 4096 槽 LUT +
  rawbits 后缀旁路 → 解码 1.14×/1.19× **快于** V2。
- 测试：test_rans_slice（选择域/差分 vs V2/确定性/体积界/畸形域——
  slice 级失败走 concealment）；test_rans 增混排 rawbits 往返
  （b>23 分块回归钉子）+ LUT 等价性。

## 实现期对冻结设计的两处修正（均已入规范 §2/§3）

1. **表字节语义**：比例量化替代 min(count,255) 裸截断——裸截断在大
   slice 会把主导符号概率压到 ~5%，体积爆炸（推导见 spec §2）。
2. **后缀位旁路**：替代"均匀二值模型逐位恰 1 bit"——逐位 rANS 状态
   运算实测成为解码瓶颈；rawbits 单次旁路恰 b bit 同价且免状态机。
   配对不变量：运算输出域 ⊆ [2^23, 2^31)（b>23 拆块，推导见 rans.h）。

## 验收与后续

- 验收门全过（数字见 A/B 报告）：体积 −8.68%/−9.05%（门 −5%/−3%）、
  解码 1.14×/1.19×（门 0.8×）、debug+ASan 76/76。
- 产品默认切换另启 ADR：P5 tier 校准以 V2 体积为基线，切换需重跑
  或按 −9% 平移 qp 映射；444/alpha 组合的实测补全。
- 遗留优化：编码端除法消除（Barrett 倒数）；2/4 状态交错（编码
  实时场景需要时）；交错多线程 slice 级并行已天然具备。
