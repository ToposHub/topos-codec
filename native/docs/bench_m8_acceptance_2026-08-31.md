# M8 V1 总验收矩阵报告（2026-08-31）

- 依据计划：`Topos_编解码性能超越_ProRes_DNxHR_优化计划_2026-08-31.md` §13/§14 M8
- 工具：`tools/bench_m8_matrix.py`（gen/decode/quality/cold 四相）；raw JSON：
  `bench_out/m8/{gen,decode,quality,cold}_{1080p,4k}.json`
- 结论先行：**达 V1-kernel 里程碑（解码时序四项全过）但未达 ProRes 正式门
  （顺序解码仍慢 1.7–2.6×、同码率 Y-PSNR 差 4.4–5.2 dB）→ 按计划决策树
  启动 M9（V2 canonical VLC + 量化升级）**；随机访问、内存、多代稳定三项
  已形成对 ProRes/DNxHR 的明确优势区间。
- 附带产出：M8 抓出并修复产品路径 P0 缺陷——binding 每帧分配 packet_bound
  最坏上限输出缓冲（1080p≈199MB / 4K≈797MB，仅零填充即 ~27ms/帧），
  提交 `41f0bf0d`（持久 grow-only 缓冲 + -15 重试，逐字节差分 0 不匹配）。

---

## 1. 环境块（协议 §1）

```yaml
machine:      MacPro7,1 / i9-9900K @3.60GHz / 8 核 / 32GB（协议冻结基准机）
os:           macOS 15.3.1 (24D70)
compiler:     Apple clang 17.0.0；release dylib（-O3 -DNDEBUG，M7 后重建）
codec_build:  libtopos_codec 0.1.0 @ 41f0bf0d（M6/M7 + binding 缓冲修复）
threads:      全部 8（Topos set_slice_threads=8 / 产品默认；PyAV thread_count=8）
cache_state:  warm（12 帧预热丢弃）；cold 为子进程隔离首帧（3 轮 median）
content:      tests/test_videos/{2K,4K}_prores422HQ.mov 前 120 帧解码帧
              （yuv422p10le，同源；值确定性与解码线程无关）
对照:         FFmpeg 8.1（PyAV 17.0.1）prores_ks profile=hq / dnxhd dnxhr_hqx
measurement:  预解码进内存口径（§4.2 精确法：编码计时不含源解码）；
              time.perf_counter 逐帧；1080p/4K 各 120 帧、计 108 帧
```

公平性口径：三编解码器同源、同线程、同 120 帧；码率锚点 = prores_ks hq
整文件码率，Topos plain 以 qp 校准至 ±5% 内（1080p +0.58% / 4K −5.1%）；
DNxHR HQX 为固定码率 profile，按其实测码率报告（1080p +60% / 4K +13%）。
产品路径：Topos 编码 = `ToposVideoEncoder.encode_frame`（planar 直通），
Topos 解码 = `ToposMediaSource.read_next_frame/read_frame`；对照 = PyAV
同构路径（demux+decode+plane 拷贝）。

## 2. 编码矩阵（§13.2；avg/p50/p95 ms/帧，108 帧统计）

| 分辨率 | 编码器 | avg | p50 | p95 | 码率 Mbps | 备注 |
|---|---|---:|---:|---:|---:|---|
| 1080p | prores_ks hq | 53.6 | 48.9 | 66.2 | 114.6 | 码率锚点 |
| 1080p | DNxHR HQX | 7.3 | 6.6 | 11.4 | 183.5 | 固定码率 |
| **1080p** | **Topos plain qp57** | **33.5** | 33.2 | 39.0 | 115.3（+0.6%） | **1.60× 快于 prores_ks** |
| 1080p | Topos sized | 68.9 | 67.6 | 79.1 | 命中 −18.4% | 逐帧贴目标下沿（见 §6） |
| 4K | prores_ks hq | 240.9 | 238.8 | 261.3 | 645.8 | 码率锚点 |
| 4K | DNxHR HQX | 30.8 | 29.9 | 35.1 | 728.3 | 固定码率 |
| **4K** | **Topos plain qp57** | **82.8** | 82.5 | 92.5 | 612.8（−5.1%） | **2.91× 快于 prores_ks** |
| 4K | Topos sized | 188.2 | 187.9 | 205.1 | 命中 −20.5% | 同上 |

- Topos plain 编码：同码率下快于 prores_ks（1080p 1.60× / 4K 2.91×），
  慢于 DNxHR HQX（1080p 4.6× / 4K 2.7×）——DNxHR 编码端是本机最快参照。
- sized（encode_sized）实际码率系统性低于目标 18–21%：V1 语义为
  「首个不超过目标的阶梯点」（确定性 parity 优先，ADR-C022），帧间复杂度
  波动使逐帧命中点偏保守。属语义特性而非缺陷；V2 码控窗口（M10）再议。
- 1/2/4/8 线程 parity：两分辨率编码 packet 逐字节一致、解码像素一致（全绿）。

## 3. 解码矩阵（§13.1；同源同线程，产品路径，100 帧统计）

| 分辨率 | 解码器 | 顺序 avg | 顺序 p95 | 随机 avg | 随机 p95 | RSS p50 | 均核占用 |
|---|---|---:|---:|---:|---:|---:|---:|
| 1080p | **Topos** | **4.02** | **5.23** | **4.09** | **5.19** | 89 MB | 4.16 |
| 1080p | ProRes422HQ | 1.57 | 5.28 | 13.96 | 18.20 | 187 MB | 7.85 |
| 1080p | DNxHR HQX | 1.16 | 3.42 | 10.99 | 15.55 | 287 MB | 7.64 |
| 4K | **Topos** | **14.50** | **17.08** | **14.01** | **15.80** | 183 MB | 6.08 |
| 4K | ProRes422HQ | 8.81 | 24.02 | 71.11 | 78.47 | 566 MB | 7.68 |
| 4K | DNxHR HQX | 5.92 | 17.51 | 52.08 | 63.00 | 972 MB | 7.53 |

### 3.1 对照门槛（§13.1 临时参考门 → ADR-C026 冻结正式门）

| 单元 | Topos 实测 | V1-kernel 门 | ProRes 参考门 | 实际对照 | 判定 |
|---|---:|---:|---:|---:|---|
| 1080p 顺序 avg | 4.02 | ≤5 ✅ | ≤2.3 ✗ | ProRes 1.57 | 过 kernel / 未过 ProRes |
| 1080p 随机 p95 | 5.19 | ≤7 ✅ | ≤3.4 ✗ | **ProRes 18.20** | **过 ProRes（3.5×）** |
| 4K 顺序 avg | 14.50 | ≤18 ✅ | ≤9.2 ✗ | ProRes 8.81 | 过 kernel / 未过 ProRes（差 1.65×） |
| 4K 随机 p95 | 15.80 | ≤23 ✅ | ≤10.5 ✗ | **ProRes 78.47** | **过 ProRes（5.0×）** |

- **V1-kernel 里程碑四项全过**（顺序 + 随机 × 1080p/4K）。
- 顺序解码：Topos 仍慢于 ProRes 2.6×（1080p）/ 1.65×（4K），慢于 DNxHR
  3.5×/2.4×——熵解码仍是第一热点（M5 后 ~55ms CPU/4K 帧），M9 的直接靶子。
- 随机访问：Topos 全 intra + O(1) 索引，随机≈顺序（−1~−8%）；ProRes/DNxHR
  随机付出 9–11× 代价（mov demuxer seek）。**剪贴板式编辑工作负载下
  Topos 已实质领先 2.7–5.1×。**
- 内存：Topos 解码 RSS 为 ProRes 的 1/2.1–1/3.1、DNxHR 的 1/3.2–1/5.3。
- 核均占用：Topos 4.2 核（1080p）/6.1 核（4K）——粒度受限，与 M3 记录一致；
  对照解码器吃满 7.5–7.9 核。

### 3.2 稳态零分配（M2 遗留验收项闭环）

- 4K：后 40 帧窗口 RSS 漂移 **+2.95MB**（前 80 帧窗口 +9.39MB 为池预热），
  稳态无逐帧原生分配（原生平面泄漏量级应为 24.9MB/帧，可排除）。
- 1080p：后 40 帧窗口仍 +15.4MB（≈190KB/帧）——量级排除原生泄漏，判定为
  Python 装配层分配器棘轮（同码流 4K 反而稳定，与逐帧原生分配特征矛盾）；
  记为 M2 遗留观察项移交 `frame_provider_service` 后续收敛时复查。
- 冷启动（子进程首帧，含解释器+库装载，3 轮 median）：Topos 264/282ms vs
  ProRes 124/196ms——差额为 binding 导入链（进程一次性行为，不计入逐帧）。

## 4. 同码率质量矩阵（§13.2 第四表；PSNR 120 帧累计，SSIM 60 帧）

| 分辨率 | 编码器 | 码率 | Y-PSNR | U-PSNR | V-PSNR | Y-SSIM | maxErr(Y,U,V) |
|---|---|---:|---:|---:|---:|---:|---|
| 1080p | Topos qp57 | 115.3 | 60.78 | 69.17 | 69.24 | 0.99927 | 15/7/7 |
| 1080p | ProRes422HQ | 114.6 | **65.14** | 69.78 | 69.66 | **0.99975** | 15/14/12 |
| 1080p | DNxHR HQX | 183.5（+60%） | 65.27 | 69.39 | 69.45 | 0.99967 | 7/5/5 |
| 4K | Topos qp57 | 612.8 | 60.39 | 67.56 | 67.20 | 0.99903 | 13/8/9 |
| 4K | ProRes422HQ | 645.8 | **65.58** | 69.21 | 69.00 | **0.99972** | 16/17/16 |
| 4K | DNxHR HQX | 728.3（+13%） | 64.07 | 68.05 | 67.88 | 0.99963 | — |

- **同码率下 Topos V1 落后 ProRes 4.4dB（1080p）/ 5.2dB（4K）Y-PSNR**：
  匹配 ProRes 码率需 qp57（近满量程），Rice 熵 + 固定 qmatrix 的率失真
  上限显现——这是 V1 结构性短板，与顺序解码热点共同指向 M9/M10。
- 色度（U/V）差距仅 0.6–1.9dB：短板集中在亮度高频细节。

### 4.1 多代退化（1/3/5/10 代同配置重编码链，60 帧，Y-PSNR vs 源）

| 代 | Topos 1080p | ProRes 1080p | Topos 4K | ProRes 4K |
|---:|---:|---:|---:|---:|
| 1 | 61.27 | 65.71 | 60.02 | 65.13 |
| 3 | 61.06 | 61.69 | 59.81 | 61.33 |
| 5 | 60.92 | 60.06 | 59.72 | 59.50 |
| 10 | **60.77**（−0.50） | 57.99（−7.72） | **59.65**（−0.37） | 57.15（−7.98） |

- **Topos 多代近乎幂等**（10 代 −0.4~−0.5dB，曲线平滑无色偏）；ProRes
  10 代退化 −7.7~−8.0dB。中间片多代往返场景 Topos 明确占优（V1 全 intra +
  固定 qmatrix 的意外收益）。

## 5. 资源与功耗代理

- 功耗：macOS 无 root 不可采 powermetrics（协议已注明）；以 CPU 核均占用
  为代理——Topos 解码 4.2/6.1 核 vs 对照 7.5–7.9 核（Topos 单位帧能耗更低，
  但墙钟也相应更长，两者乘积≈CPU 时间：4K 解 88.3ms·核 vs ProRes 67.6ms·核）。
- RSS：见 §3 表；编码侧 RSS 未单列（预解码进内存口径无意义）。

## 6. M8 期间发现与修复

1. **release dylib 陈旧**：M6/M7 期间未重建 build/release，产品路径一度
   量在旧库上（教训：性能提交须同步重建 release 并记录 dylib commit）。
2. **binding 每帧 199MB 分配（已修复 41f0bf0d）**：`ToposCodec.encode` 每帧
   `create_string_buffer(packet_bound)`，1080p 仅零填充 ~27ms/帧，掩盖
   M6/M7 全部编码收益（84.7ms vs native 24.6ms）。修复后 1080p binding
   编码 84.7→34.5ms；产品路径编码矩阵（§2）均为修复后数据。
3. **DNxHR HQX 编码极快**（1080p 7.3ms）：固定码率 + 轻量变换，是编码端
   真正的对标线；Topos plain 已过 prores_ks，未过 DNxHR。
4. **sized 码率系统性低于目标 18–21%**：legacy「首个不超过」语义 + 帧间
   复杂度波动。parity 优先不改；V2 码控窗口重设计（M10）。

## 7. Go/No-Go 判定（计划 §14 M8 行）

| 判定项 | 结果 |
|---|---|
| 解码时序四项过 V1-kernel 门 | ✅（4.02/5.19/14.50/15.80 vs 5/7/18/23） |
| 顺序解码追平 ProRes/DNxHR | ❌（慢 1.65–3.5×） |
| 同码率编码追平较快对照 | ◐（过 prores_ks 1.6–2.9×；未过 DNxHR） |
| 同码率质量无不可接受倒退 | ❌（Y-PSNR −4.4~−5.2dB，SSIM −0.0005~−0.0007） |
| 随机访问/内存/多代稳定 | ✅（2.7–5.1× / 2.1–5.3× / 15–16× 代间稳定） |

**判定：达 V1-kernel、未达 ProRes 正式门 → 启动 M9**（V2 canonical VLC
+ 量化率失真升级；先做 §12.0 schema reconciliation ADR）。未触发 No-Go
复盘条件。宣传口径维持「阶段性性能提升 / 随机访问与多代稳定优势区间」，
不得声称「超过 ProRes/DNxHR」（§16 条件未满足）。

## 8. 剩余与移交

- M9 前置：V2 schema reconciliation ADR（与 Micro-GOP 计划协调 major=2 字段）。
- 顺序解码差距的分解靶子：熵解码 CPU 55ms/4K 帧（M5 记录）→ V2 VLC 两级表。
- 质量差距的分解靶子：固定 qmatrix + Rice 后缀冗余 → M9 训练表 + 率失真
  量化（qp57 档位粒度过粗同时是 sized 偏低 18–21% 的成因）。
- 1080p Python 层 RSS 棘轮观察项（§3.2）；NEON 内核待 ARM runner。
- §16 全量完成定义中的跨平台 runner（Intel/ARM64/Win）与 TSan 全绿为
  发布级条件，超出 M8 单机范围，由 M10/M11 承接。
