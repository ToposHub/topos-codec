# Topos 解码性能优化实测记录（M0–M5，2026-08-31）

- 依据计划：`docs/Topos_编解码性能超越_ProRes_DNxHR_优化计划_2026-08-31.md`
- 素材：`tests/test_videos/2K_topos.mov`（1080p）、`tests/test_videos/4K_topos.mov`
- 口径：**完整产品路径** `ToposMediaSource.read_frame`（含 packet 读取、原生解码、
  Python 装配），release dylib（`-O3 -DNDEBUG`），预热 12 帧、正式 100 帧、
  时间序丢弃预热后再取分位（M0 修复后口径）
- 环境：本机（darwin x86_64），M1 Pro/等效 8 核以上；基线为计划 §2.2 今日实测

## 1. 总览（顺序 avg，默认线程）

| 分辨率 | 计划基线 | 本轮完成后 | 提升 | V1-kernel 里程碑 |
|---|---:|---:|---:|---|
| 1080p | 17.41 ms | **4.93 ms**（203 fps） | **3.53×** | ≤5 ms ✅ |
| 4K | 64.65 ms | **13.40 ms**（74.6 fps） | **4.83×** | ≤18 ms ✅ |

随机访问与顺序一致（全 intra + O(1) 索引，见下表 4/8 线程对照）。

## 2. 分里程碑贡献（4K 产品路径 avg）

| 提交 | 内容 | 4K avg | 增量 |
|---|---|---:|---:|
| （基线） | 计划 §2.2 | 64.65 ms | — |
| 998f94d8 M1 | CRC 拆分 + worker 前置校验 | （随 M3 测） | 正确性 |
| 6395b684 M3 | 直写最终 plane + 全 plane 统一队列 | 49.64 ms | -23% |
| 22aeca2e M2 | 持久 decoder context + 零拷贝产品路径 | 34.48 ms | -31% |
| 68ca1737 M4a | 逆变换对称化 + DC-only 快路 + 阶段计时 | 28.21 ms | -18% |
| 15b94cc1 M5 | Rice reservoir + 内联 + 12-bit LUT | 27.21 ms | -3.5% |
| 1cbce9ee M4b | AVX2 对称化 IDCT + SSE2 存储 | 22.07 ms | -19% |
| （本轮末） | 默认线程 4→8（统一队列后 8 线程不再饿死） | **13.40 ms** | -39% |

1080p 轨迹：18.15 → 14.21 → 9.52 → 8.24 → 7.96 → 6.40（M4b）→ **4.93 ms**。

## 3. 线程扩展（统一队列效果验证）

| 分辨率 | 4 threads | 8 threads | 增益 | M3 验收（≥30%）|
|---|---:|---:|---:|---|
| 1080p | 6.43 ms | 4.98 ms | -22% | — |
| 4K | 22.17 ms | 14.21 ms | **-36%** | ✅ |

## 4. 阶段画像（TOPOS_CODEC_PROFILE=1，4K/帧，4 worker 并行累计和）

| 阶段 | 优化前 | 优化后 | 说明 |
|---|---:|---:|---|
| 结构解析 scan | ~0 | ~0.001 ms | 可忽略 |
| CRC | 2×串行全帧 | 1.30 ms（并行后 ~0.33 ms/帧墙钟） | M1 达 ≤0.35 ms 目标 |
| 熵解码 | 61.7 ms | 54.9 ms | M5（-11%）；仍为第一热点 |
| 反量化+IDCT+存储 | 96.4 ms | ~30 ms（估算，M4a/b 后） | 对称化 + AVX2 + SSE2 |

剩余热点（采样）：rice 解码 > block 循环（q 清零/zigzag 散写）> IDCT。
下一步最大杠杆：M9 V2 canonical VLC（熵）与 4-block SoA IDCT。

## 5. 正确性门禁（每提交均通过）

- native debug ctest 42/42（golden byte-exact / fuzz 回放 / mov / ABI / inspect）
- ASan/UBSan 5/5、TSan 2/2（LUT 互斥、alpha 池、统一队列并发）
- 2 万组随机系数块 scalar vs AVX2 逐位一致；2000 组随机 Rice 符号流（含
  escape/长 unary/跨 refill）编码-解码差分一致
- Python：binding/source/alpha/export/timeline/concurrency 179 项全过；
  M2 快路径与旧路径逐像素 bit-exact
- perf 数值门禁 PASS（build/perf，mult=3.0 口径）

## 6. 未竟事项（按计划表）

- M6（熵编码 128-bit writer + token 化）、M7（encode_sized DCT 缓存 +
  exact bit-count ≤2 probes）：未开始——编码端仍在计划基线
- M8 总验收矩阵（同码率/同质量 vs ProRes/DNxHR、cold/warm、RSS/功耗）
- M9/M10 V2 VLC + tile；NEON 逆变换内核已留位（保持标量，待 ARM runner 差分）
- 每帧稳态零分配验证（M2 验收的 RSS 漂移项）；`frame_provider_service`
  decode_lock 粒度收敛（跨帧调度）
