# Topos 编码性能优化实测记录（M6–M7，2026-08-31）

配套文档：`bench_topos_decode_2026-08-31.md`（解码侧 M0–M5）。
本文记录编码侧 M6（熵编码与 bit writer）与 M7（encode_sized 根治）的
实测数据、验收结论与已知偏差。基准工具：`topos_quality perf quick`
（grain 素材，qmatrix=1，qp20，12 次迭代 p50；同机后台噪声 ±10%）。

## 1. 总览（p50 ms，计划起点基线 → M6 → M7）

| 配置 | 模式 | 基线 | M6 后 | M7 后 | 累计加速 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1920×1080 qp20 | plain 编码 | 68.1 | 27.0 | 26.4 | **2.58×** |
| 3840×2160 qp20 | plain 编码 | 186.7 | 90.5 | 90.2 | **2.07×** |
| 1920×1080 qp20 | sized（码控） | 205.8 | 82.2 | 50.2 | **4.10×** |
| 3840×2160 qp20 | sized（码控） | 560.3 | 264.5 | 180.4 | **3.11×** |
| 1920×1080 qp20 | 解码（参照） | 15.0 | — | 16.4 | （M0–M5 已完成） |

cycles/px@p50：1080p 编码 79→31，4K 54→26；1080p sized 238→58，4K 211→52。

## 2. 阶段画像（TOPOS_CODEC_PROFILE=1，编码基线，4K/帧 4+ worker 并行累计和）

优化前编码 CPU 时间分布（决定 M6 优先级）：

| 阶段 | CPU ms | 占比 |
| --- | ---: | ---: |
| pad（visible→coded） | 4.1 | 0.5% |
| fill（采样加载+DCT+量化+k 统计） | 100.8 | 11.7% |
| **entropy（Rice 编码+writer）** | **738.4** | **85.6%** |
| CRC | 12.4 | 1.4% |
| payload 拷贝+slice header | 3.9 | 0.5% |
| 组装（asm_append） | 2.8 | 0.3% |

M6 后 entropy 降至 198ms（−73%）；M7 将 sized 的重复 fill/entropy 整体
换为一次 DCT + 廉价精确计数 probe。

## 3. 分项贡献（1080p plain，基线 68.1ms）

| 项 | 内容 | 实测 | 提交 |
| --- | --- | ---: | --- |
| M6a | 128-bit writer reservoir（hi:lo 移位追加，≥64 位单次 bswap 8 字节提交）+ tc_rice_encode 内联 | 62.7→40.0（1.57×） | 98d39794 |
| M6b | token 化：量化遍直接产 (run,level) token + DC 残差，熵编码遍免 64 系数二次扫描与 DC 预测重推 | →28.0（累计 2.24×） | 98d39794 |
| M6c | per-slot payload chunk arena：稳态零 per-slice malloc/free | →27.7（拷贝段仅 ~1ms，纪律项） | 98d39794 |
| M7 | DCT-once F-cache + exact bit-count probe + alpha 一次编码（仅 sized 路径） | sized 82.2→50.2 | dd672d43 |

4K 同趋势：M6a 1.64×、M6b 累计 1.94×；M7 sized 264.5→180.4（4K F-cache
66MB，4:2:2 三平面合计 259,200 块）。

## 4. M7 probe 成本与迭代数

- probe = F-cache 量化 + token 化 + 精确位计数（无 emit/CRC/组装）：
  1080p ~3.5ms/次（整帧编码 27ms 的 ~13%）
- sized 均迭代 5.0（4 probe + 1 final 落盘；perf quick 口径 target=qp20 包 60%）
- **已知偏差**：计划 §11.4 的「平均 ≤2 exact probes」与 legacy 决策
  parity 不兼容——+8/+1 阶梯的每个探测点都可能成为 legacy 的停止点
  （ADR-C022 教训），跳步会改变 q_a 从而改变最终包。parity 优先，
  probe 已是廉价精确计数，墙钟目标全部达标。
- §11.1 跨帧 encoder context（tc_encoder_create/...）：评估收益仅为
  首帧缓冲预热（<10%），按计划「单项收益 <10% 不合入」暂缓；其另一
  用途（上一帧 qp 初值）同样受 parity 约束，需 V2 语义窗口。

## 5. 正确性门禁（M6/M7 每提交均通过）

- golden bitstream（56 记录）/ golden codec（16 记录）逐字节一致，fold 不变
- token vs qbuf 双遍路径差分（7 素材 × qp 4/12/16/20/28/30/52 × alpha）逐字节一致
- M7 三方差分（exact-probe vs 旧整帧 probe vs legacy 线性，10 用例含 alpha）：
  qp_used/包字节/stats 字段全一致；最终包可解码、concealed=0
- native debug/asan/ubsan/perf ctest 42/42；Python codec/source/alpha 75 项通过
- 位级差分：新 writer vs 朴素参考位串（5 万组随机 (w,v) 流，含 0/32 位宽与
  64KiB 增长路径）；M5 预存 ubsan 违例（bitreader NULL+0）一并修复

## 6. V1 编码端里程碑结论

- §10 验收（M6）：V1 输出逐字节不变 ✓；entropy cycles/block ≥2×
  （实测 CPU 2.98×/3.7×）✓；plain 1080p/4K ≥1.7×（2.52×/2.06×）✓；
  稳态无 per-slice malloc/free ✓
- §11 验收（M7）：sized 1080p/4K ≥3×（4.10×/3.11×）✓；plain ≥2×
  （2.58×/2.07×）✓；与 legacy oracle 逐字节一致 ✓；同码率质量不退化
  （bit-exact）✓；「probe ≤2」为已知偏差（见 §4）

## 7. 未竟事项（按计划表）

- M8 总验收矩阵（同码率/同质量 vs ProRes/DNxHR、cold/warm、RSS/功耗、
  1/2/4/8 线程 parity）——编码侧数据已就绪，可与解码侧合并出报告
- M9+ V2 canonical VLC + tile（需先做 §12.0 schema reconciliation ADR）
- NEON 逆变换内核（待 ARM runner 差分）；alpha 残差编码未 token 化
  （成本占比小，收益 <10%）
