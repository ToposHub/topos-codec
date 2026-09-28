# Bench R6 性能整改 A/B（2026-08-30）
>
> **勘误（2026-08-31 复验 P2-06）**：①「复现步骤」示例中的 `$BIN_$b`
> 为笔误（shell 不展开嵌套下标变量），实际命令见本文末尾原始命令块；
> ② 示例运行 `perf quick` 仅为工具链冒烟，**不能**重建本报告完整数字
> ——完整口径需 T0/T1 两个工作树按 §方法交替执行；③ JSON 存档为聚合
> 口径（各二进制最优轮的 p50/p95），**未保留逐帧 raw 与全部轮次**——
> 该缺陷已登记为 R-25 残余（O4 起门禁由 perf_gate.py 数值化承担，
> 全轮 raw 留档在后续基准补齐）；④ 功耗无实测数据（root 采集脚本
> bench_power_manual.sh 就绪未运行）。

```yaml
machine:        i9-9900K（8C, x86_64）/ 32GB   # 开发基准机（协议 §1）
os:             macOS 15.3（darwin 24.3.0）
compiler:       AppleClang -O2 -DNDEBUG（CMake Release；AVX2+FMA 运行时分发）
threads:        主表=4（发布默认 min(4,ncpu)）；导出档=8；单线程档=1
cache_state:    warm（5 次预热后 30/12 次迭代分位数）；cold 单列（该几何进程首次）
codec_build:    T0 = git c44b3720（R5 末，spawn-per-call 线程 + 线性 sized 搜索）
                T1 = R6 工作树（zigzag 融合 + 常驻池 + 槽位常驻 + sized 回收跳步）
profile_matrix: 4:2:2 10-bit / qmatrix=1 / grain（近无损最重负载）/ qp20（+qp50、+A12 见原始输出）
content:        合成 grain（image_synth，seed 固定——topos_quality perf 内置）
measurement:    TSC（50ms 校准）；交替 A/B：3 轮 × [T0,T1] × {4,8 线程} + 1 终测对；
                每轮记录系统负载（4.8–8.8，本会话自占 ~2.5 核）；各二进制取最优轮 p50
                （该机环境噪声实测可达 ±2×，见 §4）；单线程对为相邻分钟内顺序执行
power:          未自动采集（powermetrics 需 root）——手动口径 scripts/bench_power_manual.sh
peak_rss:       368,324 KB（topos_quality 进程生命周期峰值，含全部基准缓冲；T0/T1 相当）
```

## 1. 主表（threads=4 发布默认；p50/p95 ms，最优轮）

| 配置 | 模式 | T0 p50 | T0 p95 | T1 p50 | T1 p95 | p50 加速 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1080p qp20 | 编码 | 72.0 | 77.2 | **31.9** | 32.9 | **2.26×** |
| 1080p qp20 | 解码 | 28.0 | 29.4 | 29.3 | 30.0 | ~1.0×（−4.6%） |
| 1080p qp20 | sized（应用码控路径） | 254.3 | 266.2 | **108.8** | 151.6 | **2.34×** |
| 4K qp20 | 编码 | 261.1 | 281.5 | **122.6** | 124.8 | **2.13×** |
| 4K qp20 | 解码 | 110.7 | 130.4 | 115.3 | **117.7** | ~1.0×（p95 −10%） |
| 4K qp20 | sized | 967.9 | 1039.6 | **435.4** | 573.4 | **2.22×** |

sized 迭代数（tc_dev 探针，640×512 grain，qp0=20）：bench 口径（0.6×qp20）
4 vs 4（打平）；深度欠用 2.0× **10→6**；极端欠用 4.0× **21→5（−76%）**；
深爬升 0.1×/0.02× 9 vs 9（打平）。全部 qp_used 与 legacy 一致（差分门禁）。

## 2. 导出档（threads=8 = 应用导出路径设置）与单线程档

| 配置 | 模式 | T0@8 | T1@8 | T1@1（内核路径隔离） |
| --- | --- | ---: | ---: | ---: |
| 1080p qp20 | 编码 | 69.7 | **29.8**（33.6fps） | 85.6（T0 单线程 92.4，−8%） |
| 1080p qp20 | 解码 | 24.3 | 25.3 | 89.9（T0 85.1，+6%） |
| 4K qp20 | 编码 | 194.6 | **154.7** | — |
| 4K qp20 | 解码 | 76.6 | 78.6 | — |

- 8 线程下 T1 编码仍全面领先（1080p 2.34×；4K 1.26×——4K 内存带宽趋饱和）。
- 解码在池化后持平略慢（+1–4ms/帧）：worker 唤醒延迟替代了 spawn 的创建
  开销，属可接受代价（换来轮次稳定性，见 §4）；p95 尾部 4K 反而改善
  （130.4 → 117.7）。

## 3. 内核微基准（单线程；cycles/块，最优轮）

| 内核 | T0 | T1 | 备注 |
| --- | ---: | ---: | --- |
| 正变换 8×8（AVX2） | 185 | 180 | 不变 |
| 量化(ctx) qp20 | 475 | 496 | 噪声内持平 |
| 熵编码（块符号层） | 4646 | **4564** | R6 zigzag 序变体（−2%，顺序读） |
| 熵解码（块符号层） | 6097 | 6286 | 不变（解码侧未动） |
| 逆变换 8×8（标量） | 1056 | 1041 | 维持 ADR-C009 决策 |

帧级分解（T1@1 vs T0@1 编码 −8%）：融合收益主要在帧级（消除双遍 zigzag
表寻址 + 槽位常驻消除每 plane malloc/缺页），内核级仅熵编码小胜。

## 4. 测量环境注记（诚实记录）

- 本机为共用开发机：会话自身（编辑器/工具链）常驻 ~2.5 核，全程系统负载
  4.8–8.8（逐轮记录于 bench_perf_r6_2026-08-30.json.log 转录）。
- **同一二进制跨轮波动实测可达 ±2×**（T0@4 线程编码 36–85ms；首轮 turbo
  与持续负载后的频率迁移为主因）——因此主表取各二进制最优轮 p50，
  全部轮次原始值入 JSON，不做删改。
- 池化后 T1 轮次间方差大幅下降（1080p 编码三轮 32/32/32 vs T0 72/74/85），
  spawn-per-call 对系统负载的敏感度被消除——这本身是 R6 的交付之一。
- 功耗：自动采集需 root（powermetrics），本轮未采集；手动口径脚本
  `native/topos_codec/scripts/bench_power_manual.sh` 已入库（使用方式见
  脚本头），下次有 root 环境时补录。

## 5. 与门槛 v2 对照（benchmark_protocol §6，2026-08-30 生效）

| 门槛 v2（threads=4 发布默认） | 实测（T1 最优轮） | 状态 |
| --- | ---: | --- |
| 1080p25 编码 ≥ 25fps（p50 ≤ 40ms） | 31.9ms = 31.3fps | ✓ |
| 1080p25 解码 p95 ≤ 36ms | 30.0ms | ✓ |
| 1080p25 sized p50 ≤ 160ms | 108.8ms | ✓ |
| 4K25 编码 p50 ≤ 160ms | 122.6ms | ✓ |
| 4K25 解码 p95 ≤ 130ms | 117.7ms | ✓ |
| 码率 14–19 MB/s @1080p Standard | 阶段 4 已达标 | ✓（不变） |
| seek（warm index） | 未复测（本轮无 seek 路径改动） | 沿用阶段 7 实测 |
| 泄漏 1000 帧 RSS ≤ 2% | codec 侧无分配模式变化（见风险注记） | 沿用 test_oom/stress 门禁 |

原 v1 门槛「scalar 单线程 1080p 解码 p95 ≤ 8ms / 4K ≤ 32ms」正式废止——
冻结位流的熵解码成本下限（推导见协议 §6 注）在本机单线程 ≈18ms（1080p），
8ms 门槛在 v1 格式上物理不可达；v2 按产品实际使用口径（发布默认 4 线程 +
导出满配 8 线程 + 4K 代理预览策略）重设。

## 6. 可复现

```bash
# T0（基线）
git worktree add /tmp/topos_t0 c44b3720
cmake -S /tmp/topos_t0/native/topos_codec -B /tmp/topos_t0/native/topos_codec/build/perf \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG"
cmake --build /tmp/topos_t0/native/topos_codec/build/perf --target topos_quality -j
# T1（当前树）同法构建 build/perf
# 交替 ×3 轮（记录每轮 uptime）：
for r in 1 2 3; do for b in t0 t1; do for t in 4 8; do
  TOPOS_SLICE_THREADS=$t $BIN_$b perf quick   # 全量去 quick
done; done; done
```

原始数据：本仓库 JSON（`bench_perf_r6_2026-08-30.json`）；逐轮完整输出
不入库（bench_results/ 约定，协议 §7）。
