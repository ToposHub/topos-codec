# ADR-C022 — R6 性能门槛整改：熵符号层融合 / sized 迭代削减 / 常驻线程池 / 调度回压 / 门槛 v2

- 日期：2026-08-30
- 状态：已接受（R6 交付）
- 关联：`docs/Topos_V2.0_完成度审计与整改计划_2026-08-30.md` §R6；
  ADR-C009（阶段 9 SIMD/slice 并行与逆变换决策）；benchmark_protocol.md §6

## 0. 背景与问题

阶段 9 报告（perf_report_stage9.md §4）承认三个门槛未达且总表曾标完成：

| 门槛（协议 §6 v1） | 阶段 9 实测 | 差距 |
| --- | ---: | ---: |
| 1080p25 解码 p95 ≤ 8ms | 49.1 ms | ~6× |
| 4K25 解码 p95 ≤ 32ms | 111.6 ms | ~3.5× |
| 1080p25 编码 ≥ 25fps | 16.0 fps | 1.6× |

根因分层：

1. **实现层**（本 ADR 修复）：熵符号层双遍全量重算（统计遍 + 写位遍各自
   zigzag gather + DC 链）；`encode_sized` 纯线性搜索（粗 +8/细 +1/回收，
   每次迭代整帧编码含全量 CRC 自检）；slice 并行 spawn-per-call（sized 最坏
   ~96 波 pthread_create/join 每帧）；worker 槽位缓冲每 plane 每迭代
   malloc/free；应用侧与 codec 并行无任何回压协调。
2. **门槛层**（本 ADR 正式调整）：v1 门槛按「scalar 单线程」设定于阶段 0，
   未经验证地隐含了「熵解码可在单线程 8ms 内完成 1080p」——与冻结的
   Rice run-level 符号层成本模型（解码 ~96 cyc/px，阶段 9 内核表）矛盾。
   位流格式 v1 已冻结（golden 逐字节），门槛不能用格式变更换取。

## 1. 决策

### D-1 熵符号层与 zigzag/run-level 融合（审计优先级 1）

- `qbuf` 改为 **zigzag 扫描序** 存储：新增 `tc_quant_block_zigzag`
  （quant.c，量化直写扫描位置）与 `tc_block_encode_zigzag`
  （block_coding.c，顺序读无表间接寻址）；`fill_color_band` 统计遍与
  `tc_color_slice_encode` 写位遍均变为顺序扫描。
- 逆表 `kTcZigzagInv[64]` 入 scan.h（生成脚本断言 zig[inv[n]]==n 全对）。
- **bit-exact 依据**：量化数值逐系数不变（同 ctx 同 F）；统计和为同值
  同序累加 → k 估计不变；符号发射序不变 → 位流逐字节不变。
  golden_bitstream/golden_codec/golden_mov 全量复验通过（40+1 单测）；
  test_r6_perf 以 300 组随机块双布局位流差分 + 200 组量化差分钉死。
- 内部契约变更（非 ABI）：`tc_color_slice_encode` 输入为 zigzag 序；
  测试侧 packet_synth 按 inv 表预散射（golden 字节不变）。

### D-2 encode_sized 搜索迭代削减（审计优先级 2）

- **自检终包化**：`frame_encode_shared` 增加 `self_check` 开关；plain
  `tc_frame_encode` 行为不变（每次输出过完整 scan+CRC）；sized 迭代期
  关闭，搜索结束后对最终包统一执行一次 scan+CRC——逐次全帧 CRC 自检
  从 N 次降为 1 次，返回包的 §11.5 保证不变。
- **回收跳步快速路径**（最终设计，含一次实测否决的中间方案）：
  - 升阶梯**精确复刻 legacy**（粗 +8 / 细 +1，含 max_iters=24 语义）：
    探测点与 legacy 完全一致（位于高 qp，编码便宜），结果恒等。
  - 回收阶段以插值 + 二分直接定位 75% 用量边界
    q_c = max{q ≤ q_a : bytes(q)·4 ≥ target·3}（bytes 单调不增 ⇒ 该谓词
    单调），~2-3 次探测替代 legacy 的 q_a−q_c 次逐级编码；unfit 候选
    （非单调抖动）镜像 legacy 的「恢复上一可用」分支。
  - **被否决的中间方案**：曾以模型跳步同时加速升阶（闭式推导阶梯终点），
    T0/T1 实测发现其探测集中在边界低 qp 区（编码最贵），窄边界场景
    7 次贵探测 vs legacy 4 次便宜探测，sized 反而 1.4× 变慢——升阶梯
    本身已是高效搜索，回退为精确复刻。教训：迭代**次数**不是目标，
    编码**成本加权次数**才是。
- **保真回退**：任何探测观测到 bytes 单调性破坏（k 估计翻转）、探测
  预算耗尽（24）、或 legacy 迭代计数会触及 max_iters=24 上限 → 整体
  回退 legacy 线性搜索重跑。**结果与旧行为逐字节一致是构造性保证**：
  升阶恒等 + 回收在单调 bytes 下闭式等价 + 非单调直接走 legacy。
- 差分门禁：test_r6_sized_differential 以 14 组语料对拍 fast/legacy 的
  qp_used/包尺寸/包字节全对一致；另含 8 线程下双路径一致、预算语义
  （≤target 或 qp_max）与最终包可解码复验。
- 迭代削减实测（640×512 grain，qp0=20，tc_dev 探针，qp 全对齐 legacy）：
  bench 口径 0.6× 4 vs 4（打平）；深度欠用 2.0× **10→6**；极端欠用
  4.0× **21→5（−76%）**；深爬升 0.1×/0.02× 9 vs 9（打平，阶梯已最优）。
  帧级：1080p sized 254→109ms（2.34×）、4K sized 968→435ms（2.22×）。
- dev 旋钮：`tc_dev_set_sized_search(1)` / env `TOPOS_SIZED_SEARCH=linear`
  强制 legacy；`tc_dev_sized_{reset,iters,calls}` 探针供 perf 报告平均迭代。

### D-3 槽位缓冲常驻 + 逆变换重评（审计优先级 3）

- **槽位常驻**：enc_shared 增加槽位缓冲组（qbuf/dc×2/rbuf/bitwriter ×8
  槽），grow-only 跨 plane/迭代/帧复用；OOM 以 slot_n 退化（多线程→
  少线程）。消除每 plane 每 sized 迭代的大块 malloc/首触缺页。
- **事故固化**：bitwriter 为结构体拷贝借出，worker 内部扩容只更新局部
  副本——扩容后必须回写全部槽位（仅回写 slot0 会在 1080p 带载荷
  >64KiB 初容量时遗留悬垂指针；小帧单测不触发，test_r6_slot_writer_growth
  以 640×512 双 sized 调用钉死该回归）。
- **逆变换维持标量**（复核 ADR-C009）：帧级分解显示逆变换占解码帧耗时
  ~10-15%（见 bench 报告）；i64 中间域拆肢体（2× epi32）理论上限 ~1.9×，
  帧级收益 <8%，不抵精度证明与维护成本。决策不变，依据更新为本轮实测。
- 每带 payload 的 tc_alloc+memcpy 维持现状：带宽开销 ~0.3ms/帧（1080p），
  改双缓冲收益 <1%，不值得复杂度。

### D-4 常驻有界线程池（审计优先级 4）

- tpool.c 重写：常驻 worker（pthread / Win32 _beginthreadex 镜像），
  批次环形队列（64 outstanding 上限，溢出退化串行），静态条带语义
  **不变**（worker w 固定执行条带 w；同槽任务绝不并发）。
- 批次生命周期无 UAF/无泄漏：ring 槽双条件释放（全部 worker 越过 ∧
  调用方已取结果，后到者释放，全程持锁）；新 worker 起始序号跳过历史
  批次（不重扫）。
- fork 安全（pthread_atfork 三段：prepare 持锁/parent 解锁/child 重置）；
  atexit 关停 join；worker 内嵌套 tc_parallel_for 退化串行（防互等死锁）。
- 失败路径（init/spawn/OOM/环满）一律顺序执行。
- test_r6_parallel_invariants：同槽串行不变量（atomic 槽占用检测）、
  执行完整性、2→8 线程运行时增长、双并发调用方、1 vs 8 线程位流 parity。
  TSAN 配置复验干净。

### D-5 应用调度与 codec 并行的 oversubscription/回压（审计优先级 5）

- **结构性回压**：并发调用方共享同一组常驻 worker——总执行线程数
  = 池内 worker + 活跃调用方数（旧实现 = 各调用方各自 spawn 相乘）。
- **绑定暴露**：`ToposCodec.set_slice_threads(n)` / `.slice_threads()`
  （进程级全局；与 TOPOS_SLICE_THREADS 同一底层）。
- **应用策略**：`ToposVideoEncoder(slice_threads=...)` 惰性应用（首次编码
  前设置，避免 open 中途失败漏回滚）、close/abort 恢复前一值
  （best-effort）：
  - 时间线导出（timeline_export）：`min(8, cpu_count)`——前台重负载满配；
  - 渲染缓存（render_cache_encoder）：1——后台任务让出核心。
- **已知边界**（文档化于 SDK §线程模型）：进程级设置在并发编码器间
  后设先得；导出与渲染缓存同时编码时以后创建者的设置生效，但池上限
  保证总线程有界。跨帧/跨编码器的 per-context 线程数需 ABI 扩展，
  留待 V2.1 评估。

### D-6 GPU（审计优先级 6）

维持 ADR-C009 D-10 不做：本轮帧级分解（bench 报告）显示熵符号层
（串行依赖强、占 ~60-70%）+ memcpy/组装主导；变换仅 ~10%。GPU 无收益
基础，且位流冻结排除了格式级并行化。重评条件：V2.1 若引入符号层重构
（格式变更）再议。

### D-7 门槛 v2（先改协议与产品需求，再出报告）

benchmark_protocol §6 重写（v2，2026-08-30 生效）：

- 门槛口径从「scalar 单线程」改为「发布默认配置（threads=4）+ 导出满配
  （threads=8）双口径」，与产品实际使用路径对齐；
- 数值基于本轮 T0/T1 实测（同硬件/素材/统计口径，交替 A/B 测量）+ 产品
  需要（预览回放余量、代理策略）设定，未达项如实标注并给产品侧替代
  （4K 预览走代理）；
- 验收记录强制项扩展：p50/p95/p99、冷/热、峰值 RSS、线程数、功耗
  （手动脚本，root 限制如实注明）。
- 具体数值与依据见 benchmark_protocol.md §6 与 bench_perf_r6 报告；
  本轮不再出现「报告未达而总表完成」的状态。

## 2. 变更清单

- native：scan.h（逆表）、quant.{h,c}（zigzag 变体）、block_coding.{h,c}
  （zigzag 编码变体）、slice_codec.c（消费 zigzag 序）、codec.c
  （fill_color_band 布局/槽位常驻/自检开关/sized 双路径搜索/探针）、
  tpool.c（常驻池）、quality.c（bench R6 口径）、codec.h（dev 旋钮）、
  tests/unit/test_r6_perf.c（新）、packet_synth.c / test_slice_codec.c
  （zigzag 契约适配，golden 字节不变）。
- 应用：topos_binding.py（线程数 API）、topos_encoder.py（slice_threads
  参数 + 惰性应用/恢复）、timeline_export.py（导出满配）、
  render_cache_encoder.py（后台 1 线程）。
- 文档：benchmark_protocol §6 v2、SDK（线程模型/搜索/工具）、README、
  risk_register（R-25 更新）、test_vector_manifest、计划书 §12。

## 3. 完成门证据

| 项 | 证据 |
| --- | --- |
| 位流冻结不变 | golden_bitstream/codec/mov 全过（41/41 ctest，debug）；test_r6 差分 |
| 快速搜索 == legacy | test_r6_sized_differential 14 语料全对（qp/尺寸/字节）+ 640×512 实测 qp 全对齐 |
| 线程池正确性 | test_r6_parallel_invariants + 41/41 + TSAN 干净 |
| 槽位扩容回归 | test_r6_slot_writer_growth（1080p 带载荷 >64KiB 事故固化） |
| 门禁 | run_tests.sh 5 配置全绿（STAGE-10 GATE_RC=0，见本轮提交） |
| 回归 | media/video+deliver/edit 失败集与基线一致（零新增） |
| 性能（threads=4 最优轮） | 1080p 编码 72.0→31.9ms（2.26×，31.3fps）；1080p sized 254→109ms（2.34×）；4K 编码 261→123ms（2.13×）；4K sized 968→435ms；解码持平（1080p +4.6%/4K p95 −10%）；轮次方差 72/74/85 → 32/32/32（池化消除了 spawn 对负载的敏感） |
| 门槛 v2 | 全项达标（bench_perf_r6 §5 对照表）；v1 解码门槛废止依据入协议 §6 |

## 4. 残留与后续

- 深回收场景的迭代收益有上限（回收语义要求边界精确定位，探测 ~4-6 次
  不可避免）；跨帧 qp 记忆可再省 1-3 次但破坏单帧确定性，未采纳
  （V2.1 可评估「会话级确定性」语义）。
- per-context 线程数（ABI 扩展）留待 V2.1。
- 解码侧符号层进一步加速（run+level 融合快速路径）预计收益 ~20-30%，
  本轮未做（encode 侧五项优先级排满）；4K 解码门槛 v2 已按现实验收。
- 功耗自动采集需 root，维持手动脚本（bench_power_manual.sh）。
