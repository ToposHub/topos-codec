# ADR-C009 —— 阶段 9：SIMD、熵优化与 slice 级并行

- 日期：2026-08-29
- 状态：已接受（阶段 9 交付）
- 关联：ADR-C002（变换值域）、ADR-C003（golden 一致性基准）、ADR-C004（量化/sized 搜索）、
  risk_register R-02/R-09/R-10/R-11/R-21、perf_report_stage9.md

## 1. 背景

计划 §12 阶段 9 要求：测量先行 → 内存布局/无效拷贝 → AVX2 → NEON → slice 级有界线程并行 →
（可选）AVX-512 / GPU，全程以「与 scalar 重建一致 + golden 位流 bit-exact」为硬约束，
且不以固定提升百分比为唯一验收。

阶段 8 末基线（`topos_quality perf`，-O2 单线程）：1080p 编码 252.4ms / 解码 190.2ms；
4K 编码 1016.4ms / 解码 759.7ms。内核归因：**熵编码 40,942 cycles/块、熵解码 28,647**
（逐位 put_bit/read_bit + 每 idiv 量化），正变换 619、量化 848、逆变换 1,036、CRC 单表 ~400MB/s。

## 2. 决策

### C-67 测量工具先行（perf 子命令）
`topos_quality perf`：内核微基准（TSC cycles/块/cycles-px）+ 帧级矩阵
（1080p/4K × qp20/qp50 × ±alpha × 编码/解码/sized，p50/p95/p99 各 30 迭代）。
quick 档进 `run_tests.sh` 冒烟。报告口径统一 `-O2 -DNDEBUG`（修正了 build/release
缓存 -O3 与文档口径的历史不一致，见 D-12）。

### C-68 位写器/读器重写（纯 C，全平台受益）
- 写端：`tc_bitwriter` 改 64 位累加器（hold/hold_bits，flush 不变量 hold_bits < 8）；
  `data/byte_size/bits_written` 语义不变（外部读者只在 flush 后取数据）。
- Rice 编码：unary 段一次 put_bits（`(1<<(q+1))-2`）；q+1+k ≤ 32 时与余数融合为单次写入。
- Rice 解码：按字节 unary 扫描（反转后 clz 计前导 1）；read_bits 经 8 字节大端窗口一次取出。
- 快速路径头文件内联（`bitio.h` static inline），增长/越界路径退回外部函数 —— 跨 TU
  调用开销消除，错误语义（粘滞、不推进、部分越界）逐条保持。
- 收益：熵编码 8.6×、熵解码 4.6×。

### C-69 量化 fastdiv + 预建上下文
- `tc_fastdiv`：round-up 魔数（m = ⌊2^51/d⌋+1，域 n ≤ 2^27、d < 2^23 ⇒ 2^51 > n·d，
  精确性证明写在 fastdiv.h；`__int128` 乘，MSVC 走除法兜底）。
- `tc_quant_ctx`：Q/half/dz/fastdiv 每 plane（编码）/每 slice（解码）建一次；
  deadzone 分支改 branchless（三元 → cmov，fastdiv(0)=0 保语义）。
- 旧 `tc_quant_block` 保留为兼容包装（tests/golden 直接调用者不受影响）。

### C-70 AVX2 正变换 + 运行时分发
- `src/simd/transform_avx2.c`：两趟 epi32 乘加（`set1×load + mullo + add`）。
  值域：|x| ≤ 2047、|M| ≤ 13 ⇒ |A| < 2^18、|F| < 2^25，i32 全程精确 ⇒ bit-exact。
  仅 `target("avx2")` 函数内启用，库本体基线指令集（无 AVX2 CPU 不受影响）。
- `src/simd/dispatch.c/h`：函数指针一次初始化（三态 CAS 发布，无锁读）；
  内部 dev API `tc_dev_set_simd_mode`（auto/scalar/force）供差分测试强制后端
  （跟随 `tc_dev_set_qmatrix_override` 的内部 dev API 先例，不动公共 ABI）。
- 标量实现更名 `*_scalar`（dispatch 回退 + 差分基准）；公共 `tc_transform_forward_8x8`
  走分发 —— golden_transform/bitstream/codec 在 AVX2 机器上自动验收 SIMD 路径。
- 收益：正变换 619 → 183 cycles/块（3.4×）。

### C-71 逆变换 NEON/AVX2 化否决（测量驱动）
逆变换中间域 s64（|G| < 2^39、|acc| < 2^50）：AVX2 无 64 位乘（mul_epu32 拆 hi/lo ×2
每项 ~10 op，估算劣于标量）；NEON vmull 32×32 需 3×13-bit 拆肢体，复杂度/收益比不足。
决策：双列 ILP 标量（求和序不变 ⇒ bit-exact）+ ARM 上同样回退标量；逆变换仅占帧解码
~18%（1067/6064 单线程），重评待熵侧进一步下降后（阶段 10+）。

### C-72 CRC slicing-by-8 + pad/crop 按行 memcpy
CRC 表 8 张由冻结基表确定性生成（懒初始化，CAS 单写者 + 自旋等待，无数据竞争）；
输出与逐字节实现恒等（test_crc32 已知向量 + 200 轮随机交叉验证）。pad 内区按行
memcpy、右/下边缘仍逐像素复制（≤7 像素）。

### C-73 slice 级有界线程并行（编码 + 解码）
- 原语 `src/common/tpool.c`：**按调用 spawn**（≤ min(任务数, TC_SLICE_MAX_THREADS=8)），
  静态条带分配（线程 t 恰处理下标 t, t+nw, …），无驻留线程/无全局队列 ——
  生命周期零负担（ASan 干净、dylib 卸载安全）；spawn 失败条带由调用线程补跑。
- 编码：band 任务化（每任务私有 qbuf/dc/rbuf/payload 拷贝；槽位 i%nw 绑定使同槽
  任务串行）；主线程按 band 序组装 ⇒ 位流与顺序版逐字节一致（parity 测试 + 4 线程
  golden 复跑门禁）。encode_sized ≤24 次迭代共享 grow-only 缓冲。
- 解码：按 plane 分组、组内 slice 并行（coded 平面带内行不相交）；conceal/状态
  写入位互斥；plane 收尾 crop 顺序执行。
- 配置：默认 min(4, ncpu)（峰值内存可控：每 worker 一份 band 缓冲）；
  `TOPOS_SLICE_THREADS` 环境变量或 `tc_dev_set_thread_count()`（内部 dev API）可
  置 1 关闭 —— 供应用层自有线程池时避免重复并行（计划阶段 9 任务项）。
- 回压/取消语义：任务数有界（≤512 slice），无排队增长；帧级取消由应用层
  （ToposVideoEncoder.abort）承担 —— 库内单帧调用为同步语义，文档化。

### C-74 优化路径的失败与退化
- worker 缓冲 OOM → 退化单线程路径（nw=1），编码仍完成；
- payload 自有拷贝 malloc 失败 → 任务记 OOM 错误，主线程按 band 序上报首个错误；
- SIMD 无可用后端（老 CPU/未知 arch）→ dispatch 指向 scalar，行为与阶段 8 完全一致。

### C-75 计划条目 6/8/9（AVX-512 / GPU transform / GPU 熵）的处置
- AVX-512：正变换已非瓶颈（183/块 vs 熵 4772），增益上限 <1% 帧耗时 —— 否决本轮实现；
- GPU transform/quant/alpha：变换+量化合计占帧 ~12%，且需 i64 精确整数路径
  （GLSL int64 扩展、D3D/Vulkan 64 位乘）+ 回读同步 —— R-10 的测量前提不成立，否决；
- GPU 熵编码（两阶段 prefix-sum 方案）：熵已 4.6–8.6× 优化后仍为主热点，但串行位依赖
  使 GPU 化复杂度高；计划规定「仅在基准证明有收益时启用」—— 现无证据，否决并记录
  重评条件（若阶段 10 CPU 符号层重构后熵仍 >50% 且有 M×N 并行度证据）。

### C-76 计划条目 7（CPU plane pool / 低拷贝上传）核查
Python 侧解码链：C 解码 → ctypes 数组 →（唯一一次）bytes 拷贝 → `np.frombuffer` 零拷贝视图。
1080p 拷贝 ~1.5MB ≈ 0.1ms/帧（<0.5% 帧耗时）；NULL 探测多一遍 scan+CRC ≈ 5%。
结论：拷贝本身可忽略；跨帧 plane 池化需帧所有权协议（防旧帧被复用覆写），风险/
  收益不成立 —— 记为不做（真实收益已由 C 侧缓冲复用兑现）。

## 3. 备选方案与否决理由

| 备选 | 否决理由 |
| --- | --- |
| madd_epi16 双趟正变换（16 位） | 第二趟 A > 32767 需 hi/lo 拆分，复杂度高于 epi32 广播且收益相当 |
| 逆变换 AVX2 mul_epu32 拆 hi/lo | 每乘法 ~10 op（2×mul_epu32+shl+add+unpack），估算慢于 ILP 标量 |
| 常驻线程池 | 生命周期/卸载/泄漏面扩大，spawn 开销实测 amortized（<1% 帧耗时） |
| 每帧 TLS 缓冲池（跨调用） | 解码 28ms 帧耗时下，每帧 malloc 的缺页已被组内复用消除大半；TLS 析构 + 容量上限引入的状态复杂度暂不偿还 |
| SIMD 量化 | fastdiv+branchless 后量化已 551/块（帧占比 ~7%），SIMD 拆 dz/符号分支复杂度不值 |

## 4. 验收证据

- ctest 30/30 × debug/asan/ubsan/fuzz（新增 unit_stage9）；
- golden_codec/bitstream/mov 在 **TOPOS_SLICE_THREADS=4 + AVX2 分发**下逐字节通过；
- test_stage9：fastdiv 域复验、quant ctx 差分、变换差分（±2047 极值）、后端强制端到端
  一致、1 vs 4 线程 parity（多带 + alpha）；
- 帧级：1080p 解码 6.8×、编码 4.0×；4K 解码 6.9×、编码 4.6×（完整表 perf_report_stage9.md）；
- 阶段 0 门槛差距如实报告（1080p25 解码 p95 差 ~6×），不以百分比达标自评。

## 5. 风险登记变更

- R-02（阶段门禁禁建 simd/）→ 关闭：阶段 9 已交付 simd/ 目录与分发层；
- R-09（SIMD 与 scalar 输出不同）→ 关闭：差分测试 + golden 双路径（强制后端）门禁化；
- R-10/R-11（GPU 同步回读/缓冲生命周期）→ 关闭（本轮无 GPU 路径；重评条件已记录 C-75）；
- 新增 R-25：线程默认值与宿主负载的交互（p95 尾部）—— 空载复测义务 + TOPOS_SLICE_THREADS=1 逃生口。

## 6. 遗留

- 熵符号层重构（zigzag 双遍融合、run/level 批量化）与 encode_sized 迭代削减 —— 阶段 10 评估；
- 逆变换 SIMD 重评（待熵占比下降）；AVX-512 同理；
- Windows 线程路径（tpool 的 Win32 实现）随阶段 10 构建矩阵；
- benchmark_protocol §6 实时门槛未全达，如实记录（§4 perf 报告）。
