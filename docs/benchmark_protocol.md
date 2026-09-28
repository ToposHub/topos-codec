# Topos Video Codec — 基准与基线协议（阶段 0 交付物）

- 目的：定义 codec 全生命周期（阶段 2–10）统一使用的**性能/画质测量与报告协议**，
  并登记阶段 0 已建立的基线数据。
- 原则（计划 §8.3）：任何报告缺少环境块 = 无效报告；只报平均 FPS = 无效报告；
  cold/warm、p50/p95/p99、输入/解码/上传/呈现分项必须拆开。

---

## 1. 环境记录块（每份报告必须包含）

```yaml
machine:        # sysctl -n machdep.cpu.brand_string / machdep.cpu.core_count / hw.memsize
os:             # sw_vers（或 systeminfo / lsb_release）
compiler:       # clang/gcc/cmake 版本与优化开关（C: -O2；SIMD 阶段另注 -mavx2 等）
threads:        # 本基准使用的线程策略（scalar = 单线程；slice 并行注明 lane 数）
cache_state:    # cold / warm 及实现方式
codec_build:    # libtopos_codec 版本 + git commit
profile_matrix: # codec / profile / pixel format / bit depth / alpha
content:        # 素材清单（路径 + 规格 + 帧数）
measurement:    # 轮数、预热丢弃策略、计时器（time.perf_counter / std::chrono::steady_clock）
```

开发基准机（阶段 0 冻结）：i9-9900K / macOS 15.3.1 / 32GB / Apple clang 17 / CMake 4.4.2 /
FFmpeg 8.1 / Python 3.12.13(.venv, PyAV 17.0.1)。
**cold cache 定义**：进程首次打开该文件（macOS 无 root 无法 drop page cache，禁止声称
「系统级 cold」，只声称「进程 cold」）；warm = 同进程至少解码过一轮后。

## 2. 素材集

| 类别 | 资产 | 用途 |
| --- | --- | --- |
| 真实素材矩阵 | `tests/test_videos/`（2K/4K × H.264/HEVC/ProRes422HQ/ProRes4444/DNxHR HQX，25fps，~16GB） | 编码输入源、对照解码、端到端 |
| 合成素材 | `tests/media/fixtures/synthetic/` + `test_vector_manifest.json` 生成器 | 画质/边界/损坏测试 |
| 素材规格说明 | `tests/media/MATERIAL_MATRIX.md` | 维持同步更新 |

## 3. 指标定义

- **延迟族**：p50 / p95 / p99 / max（同一样本集，单帧毫秒）。
- **吞吐**：fps（整段，含全部开销，标注是否含 I/O）。
- **实时倍率**：吞吐 ÷ 帧率（25fps 素材）。
- **Seek 五段分解**（计划 §8.3）：① index lookup ② packet read ③ decoded frame ready
  ④ GPU frame ready ⑤ first frame presented——四段分别计时求和必须可对账。
- **画质**：PSNR（Y/U/V 分通道）、SSIM、（必要时）感知指标；Alpha：无损 = bit-exact 断言，
  近似 = 最大绝对误差；多代曲线 1/3/5/10 代。
- **资源**：峰值 RSS、native context 数（泄漏断言）、GPU 纹理/fence 余量（阶段 7+）。

## 4. 方法论

### 4.1 解码基准（沿用既有脚本方法，已验证）

`tools/bench_decode_speed_report.py` 方法论为权威模板：顺序解码前 120 帧、
样本 >20 时丢弃前 10 帧预热、`time.perf_counter` 逐帧计时、单轮 + 重复运行取分位数、
`open()` 冷启动单独计时。Topos 接入后新增：随机 seek 模式（步长 ±随机 200 帧，
测五段分解）与损坏文件 concealment 路径计时。

### 4.2 编码基准

- **精确法（阶段 4 起强制）**：与 `scripts/bench_hw_encode_windows.py` 相同——先预解码
  N 帧进内存，编码计时**不含**解码；每编码器 12 帧 warmup 丢弃；报 encode_fps / 输出 MB /
  颜色 payload / Alpha payload / 总 payload。
- **CLI 指示法（快速估计，必须标注「含输入解码」）**：阶段 0 基线即此法
  （`baseline_encode_2026-08-29.md`）。

### 4.3 画质基准

同码率对比流程（`quality_report_scalar.md` 模板，阶段 4）：
源 → Topos(profile P, qp q) 与 源 → ProRes/DNxHR（调整参数至码率差 <5%）
→ 各自解码 → 对源的 PSNR/SSIM 分通道 + 多代退化曲线。
工具：FFmpeg `psnr`/`ssim` 滤波器（`format=yuv422p10le` 对齐后计算），Alpha 用 numpy
bit-exact 断言（tests 内实现，不依赖主观观察）。

### 4.4 多代退化

1/3/5/10 代重编码链（每代同配置），记录每代 PSNR/SSIM 与色偏（均值漂移），
验收：退化曲线平滑可解释、无明显色偏（计划阶段 4 门槛）。

## 5. 阶段 0 已登记基线

### 5.1 解码基线（既有，已入库）

`docs/decode_speed_benchmark_2026-08-29.md` / `.json`（FFmpeg 软解 vs macOS VT，
同基准机，120 帧/素材）。要点：

| 素材 | FFmpeg p50 | VT p50 | FFmpeg 吞吐 | 备注 |
| --- | ---: | ---: | ---: | --- |
| 2K H.264 | 0.68 ms | ~3 ms | ~874 fps | FFmpeg 1.5–2.9× 快于 VT |
| 4K H.264 | ~7 ms | ~12 ms | ~140 fps | FFmpeg 最差单帧 88ms；VT ≤34ms |
| 4K HEVC | ~8 ms | ~9 ms | 119 vs 130 fps | 唯一 VT 吞吐反超项 |
| 4K ProRes 422 HQ | ~9 ms | ~13 ms | ~110 fps | FFmpeg 1.4–1.5× 快 |
| 4K ProRes 4444 | 16.99 ms | 24.20 ms | ~59 fps | FFmpeg 仍 1.5× 实时 |
| 4K DNxHR HQX | ~6.4 ms | n/a | 159 fps | VT 无解码器，强制 FFmpeg |

（精确值以 JSON 为准；VT 首会话冷启动 0.6–1.9s。）

### 5.2 编码基线（本阶段新测）

`docs/baseline_encode_2026-08-29.md`。要点（2K 120 帧，CLI 含输入解码）：
prores_ks 422 HQ 29.8–45.5 fps（114.6–186.9 Mb/s，随源复杂度）；
dnxhr_hqx 47.9–58.3 fps（~183.5 Mb/s 恒定）；对有损 8-bit 源 PSNR avg 48.21 / 48.57 dB。

### 5.3 画质基线

同 5.2 PSNR 数据 + 既有 `tools/verify_prores_export_quality.py` 流程；多代曲线待阶段 4。

## 6. Topos 性能门槛（v2，2026-08-30 R6 生效；v1 作废）

v1（阶段 0 暂定，「scalar 单线程」口径）正式废止。废止依据：冻结位流
（golden 逐字节）的 Rice run-level 符号层解码成本 ≈96 cyc/px（阶段 9 内核
表，R6 复测 6.1–6.3k cyc/块 持平）——1080p 4:2:2 约 3.1M 像素在该机单线程
物理下限 ≈18ms/帧，v1 的「p95 ≤ 8ms」在 v1 格式上不可达；门槛不能用格式
变更换取。v2 与产品实际使用口径对齐：

| 指标（开发基准机，threads=4 发布默认，grain 最坏内容） | 门槛 v2 | R6 实测 | 状态 |
| --- | --- | ---: | --- |
| Standard 1080p25 编码 | ≥ 25 fps（p50 ≤ 40ms） | 31.9ms = 31.3fps | ✓ |
| Standard 1080p25 解码 p95 | ≤ 36 ms（≥0.7× 实时预览 + 代理余量） | 30.0 ms | ✓ |
| Standard 1080p25 sized（应用码控路径）p50 | ≤ 160 ms | 108.8 ms | ✓ |
| Standard 4K25 编码 p50 | ≤ 160 ms | 122.6 ms | ✓ |
| Standard 4K25 解码 p95 | ≤ 130 ms（4K 预览走代理：产品策略） | 117.7 ms | ✓ |
| Standard 1080p25 码率 | 14–19 MB/s 区间内（计划 §2.1） | 阶段 4 达标 | ✓ |
| seek（warm index） | hot lookup ≤ 1ms；decode-ready p95 ≤ 2× 顺序 p95 | 阶段 7 实测（本轮无 seek 改动，沿用） | ✓ |
| 泄漏 | 1000 帧 open/decode/close RSS 增长 ≤ 2% | test_oom/stress 门禁（R6 无分配模式变化） | ✓ |

参考口径（非门槛，报告记录）：导出满配 threads=8（应用导出路径设置：
R6 起 `set_slice_threads(min(8,cpu))`）；单线程口径（内核归因隔离）。
SIMD（阶段 9）之前禁止以牺牲画质/正确性换速度的原则不变（R6 全部优化
bit-exact，golden 逐字节冻结验证）。

**测量口径要求（R6 追加）**：本开发机环境噪声实测可达 ±2×（同一二进制
跨轮 36–85ms，编辑器会话常驻 ~2.5 核 + 频率迁移）——性能对比一律采用
交替 A/B × ≥3 轮 + 逐轮负载记录 + 各二进制取最优轮；全部轮次原始值随
报告 JSON 入库。验收必须记录 p50/p95/p99、冷/热、峰值 RSS、线程数；
功耗以手动脚本（powermetrics 需 root）采集或如实注明未采集。CI 空载
runner 的性能回归断言留待 R7。

不达标处置：报告必须给出差距分析；门槛调整必须先更新本协议与产品需求
（先改协议再出报告），禁止「报告写未达、总表标完成」。

### 6.1 R6 基线登记

`docs/bench_perf_r6_2026-08-30.md` / `.json`（T0=c44b3720 vs T1=R6
交替 A/B；1080p 编码 2.26×、sized 2.34×、4K 编码 2.13×；解码持平、
p95 尾部稳定性显著改善；sized 迭代极端欠用 21→5）。

## 7. 结果入库约定（统一，解决现存不一致）

- 报告与数据：`docs/`（md + json 成对入库，文件名 `bench_<主题>_<日期>`）。
- 机器相关原始 dump（逐帧 CSV 等）：`bench_results/`（gitignored）。
- 与既有解码报告（docs/ 根）并行存在，不迁移；新报告一律走 `docs/`。

## 8. 报告模板头（复制即用）

```markdown
# Bench <主题> <日期>
环境块: （§1 完整填写）
方法: （§4.x 引用 + 偏差说明）
结果表: （指标 §3；至少 p50/p95/p99/max）
结论与门槛对照: （§6）
原始数据: bench_results/<file>（不入库路径注明）
```

## 9. 性能门禁（O4，2026-08-31 复验补章）

**复验 P1-18 纠偏**：本协议 §6 v2 门槛此前只有报告侧对照，run_tests.sh
的 perf 段仅 grep 固定文字（"帧级基准"）——文本 grep 不构成性能门禁，
严重回退仍全绿。自 O4 起：

- 数值门禁脚本 `native/topos_codec/scripts/perf_gate.py`：
  - 输入 `topos_quality perf quick` 报告文本，解析帧级表（p50/p95/p99
    + sized 平均迭代）；
  - 阈值 = §6 v2 基准 × 环境余量（默认 **mult=3.0**，吸收非专用 runner
    的 ±2× 环境噪声，复验实测同机后台负载所致）；超阈 → 退出码 1，
    CI 可据此失败；结果 JSON 落 `build/perf/perf_gate.json`；
  - 专用空载机跑严格口径：`TOPOS_PERF_GATE_MULT=1`；
  - CI 共享 runner（codec-matrix 各 job，R7/ADR-C025 C-120）设
    `TOPOS_PERF_GATE_MULT=4.0`：只拦结构性 >4× 回退；精确尾延迟结论仍以
    空载固定机全轮留档为准（R-25 残余口径）；
  - sized 平均迭代 > 12 判搜索发散（哨兵）。
- 门槛覆盖：1080p encode p50 / decode p95 / sized p50，4K encode p50 /
  decode p95 / sized p50（550ms，2× R6 实测 435——协议留 4K sized
  观察位，正式定标待 O4 全矩阵）。

**产品路径口径（P0-05 残余收口）**：§6 v2 的"达标"指基准口径
（threads=4、固定素材、专用会话）。**产品实时性如实标注**：

- 应用无 CRF 覆盖时走 `encode_sized`：1080p p50 ≈ 109–215 ms（4.6–9.2
  fps）——**离线/近实时档**，不是 25 fps 实时编码；
- 4K plain encode p50 ≈ 123–265 ms（3.9–8.2 fps）——产品策略为
  **代理预览 + 4K 离线交付**（复验 P0-05：不得再称"各 profile 实时"）；
- 实时预览由代理链（阶段 8 磁盘代理）承担；实时全分辨率编解码不在
  V2.0 完成定义内（解码 1080p p95 ≤ 36 ms 满足实时回放门槛）。
