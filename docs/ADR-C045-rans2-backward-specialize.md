# ADR-C045：rans2 后向环按模型专化 + 值语义 put 引擎（速度计划 S5）

- 状态：**已实施**（2026-09-11；回归门数据见 §4）
- 关联：docs/codec/topos_speed_optimization_plan_2026-09-11.md（S5/P2）、
  topos_speed_p0_profiling_2026-09-11.md（M0.2 编码剖析）、ADR-C044
  （S4 fastdiv——本 ADR 复用其 per-symbol 表）、ADR-C036/038（rans2 设计
  与产品默认）
- 变更：`color_tokens_rans2_encode` 后向遍历重构——
  1. **值语义 put 引擎三件套**（codec.c 内联）：`rans2_put_chunk_x` /
     `rans2_put_suffix_x` / `rans2_put_x`，x 与 `tc_fastdiv` 按值传递，
     fastdiv 字段预读于 renorm 存储之前；
  2. **按 lvl_model 专化三份块内对环**（NONE/POS/PREV），模型分支与
     argmin 双精度比较提出环外（每块一次分派）；
  3. **pos_block 前缀和仅 POS 模型计算**（NONE/PREV 不再支付该遍）。
  位流逐字节不变（§2 差分钉死）。

## 1. 动机与靶心勘误（P0 复剖析，S4 后）

S4 落地后按计划复跑 4K 444 编码剖析（sample 10s + noinline 定归因）：

| 成分 | 占 r2e | 占 band 作业 | 与 P0 对比 |
|---|---|---|---|
| `tc_rans_put_fast`（调用体） | 35.4% | ~28% | P0 put 环 41% → S4 后降 |
| **r2e 自体（后向环内联体）** | **~47%** | **~37%** | P0 归并为"38% 桶"的主体 |
| `rans2_collect_joints` | 14.0% | ~11% | 同 P0 方向 |
| `rans2_rows_cost` + 行编码/重建 | **<1%** | 不可见 | **P0"38% 桶"预期落空** |

P0 的"collect+精确代价模型 38%"桶系 display 行 pc 合并的粗归因——
精确拆分后**代价模型路径完全不可见**（rows_cost/log2/model_build 合计
<1.5%），原计划 S5 第二靶心的"argmin 加速"无收益空间；真实靶心 =
后向环自体。反汇编证实其成本构成：

1. **每对 lvl_model 三路分支 + argmin 双精度比较**（ucomisd）留在对环内；
2. **调用边界 + e->x 栈往返**：put 经 `tc_rans_enc*` 指针访问状态，
   renorm 的 uint8_t 存储按 C 别名规则可改写任意对象（char-store
   全别名），迫使每次 put 后从栈重载 `e->x`/fd 字段；
3. **pos_block 前缀和逐块无条件计算**——NONE/PREV 模型从不使用
   （产品 qp70 域 PREV 当选，整遍纯浪费）。

## 2. 位精确性论证

引擎与 `tc_rans_put_fast` / `tc_rans_put_chunk` / `tc_rans_put_rawbits`
**逐运算一致**（renorm 阈值/字节序、fastdiv 分派、suffix 高低块拆分
次序均镜像）；差异仅在值的载体（寄存器 vs 结构体字段）与防御检查：

- 省略的 sym/f 域检查在此为死码——token 由量化管线产出（lvl bitlen
  ≤27、dc bitlen ≤28、run ≤62 恒在字母表内），被发射符号计数 ≥1 ⟹
  freq ≥1（`tc_rans_model_build` 保底）；`enc_band_tok` 为 codec.c
  内部类型，外部输入不可达；
- 溢出路径逐函数镜像：`put_x` 置错误消息（同 put_fast），`chunk_x`
  不置（同 put_chunk——flush 统一补报）。

**差分**：4K 444 pro444 + 4K 422 hq 产品码控各 23 帧包，旧/新 dylib
SHA256 全一致；qp 轨迹/字节预算逐轮相同（码控回路位级复现）。

## 3. 系统级交错 A/B（过 +3% 门；进程级交替，median-of-medians，5 轮）

| 单元 | 16t（产品条件） | 1t（±1% 纪律） |
|---|---|---|
| 4K 444 pro444（qp70） | 40.6→37.0 ms/帧 = **+8.7%** | 228→198 = **+13.0%** |
| 2K 444 pro444（qp70） | 13.7→12.3 = **+10.2%** | 57→49 = **+14.8%** |
| 4K 422 hq（qp62） | 25.5→23.1 = **+9.6%** | 135→115 = **+15.2%** |

4K 444 稳态编码 37.0 ms/帧 ≈ **1.08× 实时 @25p**（P2 出口 ≥1.15×，
缺口归 collect_joints 11% 与 fill 25%——见 §5）。1t > 16t 同 S4 形态
（线程分摊稀释）。

## 4. 回归门（gate2，S5 + ubsan 修复同轮）

五配置 ctest：debug/asan/ubsan/tsan/fuzz **79/79 ×5**；exports pin、
golden conformance、PyAV interop oracle、多线程 golden 平价、perf 数值门
+ depth12、SDK roundtrip 全 OK；pytest **280/280**（首轮 279/280 为本
ADR 未入 INDEX 的门禁自检，补行后复绿）。ubsan 首轮（gate1）78/79 的
`unit_pyramid_transform` 系**预存在缺陷**（pyramid_transform.c:249
NULL+0，git stash 归因 HEAD 复现）：`pt_alloc_i32` 宽度 0 返回 OK+NULL，
f2 且 w==1 几何下 `sd + 0` 即 UB——本批独立修复（249/397 双处 NULL
保护，该几何下无 SD 元素，f2/f3 均不触），ubsan 转 79/79。

## 5. 二期候选（collect 融合）：已试且 REJECT（2026-09-11 同日）

**实现形态**：`enc_band_tok` 内嵌 `rans2_joints` + `joints_rans2` 标志；
`fill_color_band_tokens` / `fill_color_band_from_f` 在 rans2 模式（v7
entropy 7）下于 token 遍顺产 joints（bitlen/pos/前邻类复用现成值，
DC/pair 两处条件自增），`color_tokens_rans2_encode` 见标志免
collect_joints 重扫。**位精确通过**（444/422 各 23 帧包 SHA256 一致，
qp 轨迹同），但系统级交错 A/B（同 §3 协议）：

| 单元 | 16t | 判 |
|---|---|---|
| 4K 444 pro444 | +1.2% | 低于门 |
| 2K 444 pro444 | **−4.7%** | 回归 |
| 4K 422 hq | **−4.7%** | 回归 |

**机制**：fill 的 AC 对环是 ctz 驱动的紧循环，pos 累计链在下一对
迭代的关键路径上——并入 pos/prevlvl 桶链 + 2 次直方图 RMW 的代价
超过整遍 collect 重扫的节省；collect 本身已是 ~4-5 cycles/pair 的
访存地板（P0 复剖析），无净空间。按预注册纪律**回退零入库**，
负结果在此落档防重开（同 ADR-C041/S1 体例）。

## 5b. 后续边界（P2 终态）

- `rans2_collect_joints`（band ~11%）：**融合已判负（§5）**，微调上限
  ~2% band 低于立项门——编码轴 C 代码项至此收口；
- `fill_color_band_tokens`（band ~25%）：AVX2 nat 量化已在线，sparse/
  batch 门控已判负（codec.c dev knobs），无位精确窗口；
- 后向环自体经一期后贴近串行依赖地板（3 符号操作 + suffix chunk
  ≈30 cycles/pair），与解码侧 ADR-C041 平台判定同构；
- **4K 444 编码终态 ≈1.08× 实时 @25p**（P2 出口 1.15× 未达——剩余
  差距在 C 位精确域内无已立项项，窗口 = Metal/新 ISA 轨道）。

## 6. 复现

```bash
# 差分：旧/新 dylib 各跑 tools/prof_p0.py --dump-packets → shasum 比对
# A/B：tools/prof_p0.py --seconds 10 稳态列，进程级交替 5 轮取中位
# 归因：__attribute__((noinline)) rans2_collect_joints/rans2_rows_cost
#   临时构建 + sample 10s（勿入库）
```
