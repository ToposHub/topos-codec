# ADR-C050：LP GOP 分段并行编码（应用层编排，流格式零变更）

日期：2026-09-14 · 状态：**已采纳**
关联：ADR-C047/C048（IP-2 与 V7-R3 载体）、ADR-C049（编码策略）、
gate review §5.4/§5.6（2K 反常归因 + 实测）

## 1. 背景

实时化两步落地后（§5.3/§5.5），单 GOP 参考链是剩余的串行结构：编码
帧必须逐帧经过同一 context（参考链因果）。4K band 机器已近饱和
（54~59fps），但 2K 及以下档在 E2E 口径下 prepare/mux/编码交替占用，
并行度不满。gate review §5.6 登记的下一步 = **GOP 分段并行**：流按
GOP 切段，段间独立（每段以 I 开新链），段级线程池并行——编解码吞吐
随 worker 数放大，格式零变更。

## 2. 决定

**D1（分段策略）**：`gop='ip2'` 消费 `FFmpegEncoderConfig.gop_size`
（段长，帧）与新增 `gop_workers`（默认 2）。gop_size 未显式设置：
**≤2560×1440 档默认开**（fps×2 帧 ≈2s GOP），4K 档默认关（band 机器
已近饱和，分段收益低于在途帧内存 workers×gop_len×帧字节）；gop_size=0
显式单 GOP（现行为，逐字节不变）。段首强制 I。

**D2（确定性）**：每段独立 GOP context（段 gop_id 恒 1，mux 链校验
天然支持——每段 I 重开链；解码端 I 接受任意 id）→ **包字节只依赖段内
帧**，worker 数/派发时序不改变任何字节（回归测试钉死 w1≡w4 逐字节，
含容器）。mux/pts/预算记账全在主线程有序回填（FIFO future），顺序与
串行一致。

**D3（内存界）**：在途帧上界 = workers×gop_len×帧字节（背压：在途段
满即阻塞回收最老段）；worker 为 Python 线程做 ctypes 原生调用（释放
GIL），与调用方帧准备天然重叠；native 侧并发 tc_parallel_for 批次由
常驻池统一调度（确定性不受影响，§5.6 验证）。

**D4（别名纪律）**：分段路径缓冲 planes 待 worker 消费——native 融合
cvt 的出参复用缓冲（`_cvt_bufs`）在分段模式下禁用（否则下一帧转换覆写
已缓冲平面 = 撕裂输入，集成期实测发现；顺序路径同步编码不受影响，
逐字节回归不变）。

**D5（绑定出包缓冲）**：`ToposGopContext._enc_buf` 起步 8MB、按
BUFFER_TOO_SMALL 需量增长重试（C 契约：BTS 全量不提交、重试确定）——
并行时每 ctx 常驻内存从 packet_bound（~268MB@4K 清零拷贝）降到实际
包长量级。

**D6（不改解码端）**：流格式/容器/读端零变更；分段流在产品解码路径
（ToposMediaSource GOP 链）全片可读（stss 多 I，seek 粒度随段长改善）。
解码侧并行（bulk decode 多 context）另行登记，本 ADR 不含。

## 3. 实测（E2E 产品路径，`tools/bench_gop_parallel.py`，64 帧）

| 场景 | 口径 | 基线(单GOP) | 分段16 w=2 | 分段16 w=4 |
|---|---|---|---|---|
| pan_2k | qp72（LP 锚） | 50.5 fps | 77.3 | **107.7 fps（2.1×）** |
| static_4k | qp72（LP 锚） | 56.4 fps | 72.7 | **79.1 fps（3.2× 实时）** |
| pan_2k | qp44（近无损重载） | 23.0 fps | 34.6 | **51.4 fps（2.2×）** |
| static_4k | qp44（近无损重载） | 9.5 fps | 13.6 | **14.5 fps（+53%）** |

段化体积代价：4K static +6.2%（gop16；gop50 默认 ≈+2%），2K pan 0
（全 I 内容）。段化开销 w1 近零（±4%）。全变体产品解码路径全片可读。

## 4. 被否备选

- **C 侧 context 内帧间并行**：参考链因果在单链内不可并行；等价物即
  本 ADR 的分段（在编排层做，不动 native API）。
- **默认全分辨率开分段**：4K 在途内存（w2×48 帧×33MB ≈ 3.2GB）不值
  其边际收益，4K 显式 gop_size 才启用。
- **rANS 交错流 + SIMD**：格式可见变更（须新子代际 ADR），留作格式
  演进储备（gate review §5.6 下一步清单）。
