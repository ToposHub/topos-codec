# Topos Codec 阶段 9 性能报告（SIMD / 熵优化 / slice 并行）

> **R6 复测更新（2026-08-30）**：本报告 §4 的门槛差距已由 R6 整改闭环——
> 门槛 v2 与最新实测见 `benchmark_protocol.md` §6 与 `bench_perf_r6_2026-08-30.md`
> （1080p 编码 2.26×/31.3fps、sized 2.34×、常驻池）。本文保留为阶段 9 历史基线。

- 数据源：`topos_quality perf`（build/perf，`-O2 -DNDEBUG`，p50/p95/p99 覆盖 30 迭代）
- 环境：i9-9900K（x86_64 AppleClang，AVX2+FMA），threads=4（`TOPOS_SLICE_THREADS` 可覆盖）
- 测量时系统负载 5–7（LSP 等后台进程），p95/p99 含调度噪声；复测请在空载机器执行
- 口径：grain 合成内容（近无损最重负载），qmatrix=1，4:2:2 10-bit；cycles/px 以 luma 像素计

## 1. 帧级总览（优化前 → 后，p50 ms）

| 配置 | 模式 | 阶段 8 scalar | 阶段 9 | 加速 |
| --- | --- | ---: | ---: | ---: |
| 1080p qp20 | 编码 | 252.4 | 62.6 | **4.0×** |
| 1080p qp20 | 解码 | 190.2 | 27.8 | **6.8×** |
| 1080p qp20 | sized（≤24 迭代） | 900.8 | 238.2 | 3.8× |
| 1080p qp20+A12 | 编码 | 350.7 | 90.5 | 3.9× |
| 1080p qp20+A12 | 解码 | 267.3 | 43.3 | 6.2× |
| 4K qp20 | 编码 | 1016.4 | 222.1 | **4.6×** |
| 4K qp20 | 解码 | 759.7 | 109.9 | **6.9×** |
| 4K qp20 | sized | 3589.6 | 840.1 | 4.3× |
| 4K qp50 | 编码 | 744.5 | 173.8 | 4.3× |
| 4K qp50 | 解码 | 615.8 | 92.1 | 6.7× |
| 4K qp20+A12 | 解码 | 1041.6 | 170.7 | 6.1× |

完整 p50/p95/p99 表见 `topos_quality perf` 输出（本报告由其生成；quick 档供 CI 冒烟）。

## 2. 内核微基准（cycles/块，每块 64 像素）

| 内核 | 阶段 8 | 阶段 9 | 加速 | 优化手段 |
| --- | ---: | ---: | ---: | --- |
| 熵编码（块符号层） | 40,942 | 4,772 | **8.6×** | 64 位累加器位写器 + unary 批量 + 符号融合 + 内联快速路径 |
| 熵解码（块符号层） | 28,647 | 6,164 | **4.6×** | 8 字节窗口读 + 按字节 unary 扫描（clz）+ 内联 |
| 正变换 8×8 | 619 | 183 | **3.4×** | AVX2 epi32 乘加（值域证明精确）+ 运行时分发 |
| 反量化 | 506 | 154 | **3.3×** | 每 slice 预建 Q 表（消除逐系数表算） |
| 量化 | 848 | 551 | 1.5× | fastdiv 精确魔数除法 + ctx 预建 + deadzone branchless |
| 逆变换 8×8 | 1,036 | 1,067 | ~1× | 双列 ILP 标量（i64 中间域 SIMD 拆肢体不偿失，见 ADR-C009） |
| CRC32 | — | 2.4 GB/s | ~6× | slicing-by-8（表由冻结基表确定性生成） |
| pad / crop | 1.43 / 0.46 cyc/px | 0.31 / 0.32 | 4.6× / 1.4× | 按行 memcpy |

帧级 malloc 缺页：编码 `encode_sized` ≤24 次迭代间共享缓冲 grow-only 复用；
解码 coded 平面取全平面最大值单次分配跨 plane 复用（4K 每帧省 ~50MB 缺页流量）。

## 3. 一致性验收（bit-exact 门禁）

- golden_codec / golden_bitstream / golden_mov 全量通过（SIMD + 4 线程路径下位流逐字节冻结不变）；
- `test_stage9`：fastdiv 域证明复验（全可达 Q × 边界/随机 n）、quant ctx 差分、
  AVX2/标量变换差分（含 ±2047 极值）、强制后端端到端位流一致、**1 vs 4 线程 parity**
  （多带帧 + alpha mode2：packet 逐字节相同、解码输出/conceal 状态一致）；
- `run_tests.sh`：debug/asan/ubsan/fuzz 四配置 ctest 全过 + 多线程 golden 复跑 + NEON arm64
  交叉编译检查 + perf quick 冒烟（STAGE-9 门禁）。

## 4. 与阶段 0 性能门槛（benchmark_protocol §6）的差距

| 门槛 | 现状（p50 / p95） | 状态 |
| --- | --- | --- |
| 1080p25 解码 p95 ≤ 8ms | 27.8 / 49.1 ms | 未达（差 ~6×） |
| 4K25 解码 p95 ≤ 32ms | 109.9 / 111.6 ms | 未达（差 ~3.5×） |
| 1080p25 编码 ≥ 25fps | 16.0 fps（62.6ms） | 未达（差 1.6×） |
| 码率 14–19 MB/s @1080p Standard | 已达标（阶段 4） | ✓ |

单线程下熵符号建模仍占帧耗时 ~70%（74.6 cyc/px 编码 / 96.3 解码，帧总 72/32）。
下一杠杆（阶段 10+ 评估）：块符号层重构（zigzag 双遍融合、run/level 批量化）、
`encode_sized` qp 搜索迭代数削减、逆变换 i64 域 SIMD 重评。GPU 后端经测量论证：
熵编码串行依赖强、变换仅占 ~10%，当前不具收益基础（ADR-C009 D-10）。

## 5. 可复现

```bash
cmake -S native/topos_codec -B native/topos_codec/build/perf \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG"
cmake --build native/topos_codec/build/perf --target topos_quality topos_codec
native/topos_codec/build/perf/topos_quality perf          # 完整（~3 分钟）
TOPOS_SLICE_THREADS=1 … perf                              # 单线程口径
```
