# Windows 画质/性能对照报告（W09–W14，win-msvc，2026-09-28）

> 同机同 commit（cfe3b9cc + 工具修正）；素材 `tests/test_videos/`；工具
> `tools/win_rd_prores_matrix.py`（PyAV，无 ffmpeg CLI 依赖）。

## W09：母版矩阵

`win_master_manifest_2026-09-28.json`：8 部母版 SHA-256 + 几何/像素格式/帧数。
要点：**444 家族有原生 12-bit 母版**（ProRes4444/4444XQ 与 DNxHR444 12bit 均为
yuv444p12le）；原生 16-bit 视频母版缺席（记为缺口；TRAW16 是图片 CFA 车道，
不作视频 16-bit 证据）。RD 用母版：2K/4K_prores422HQ（yuv422p10le）与
4K_prores4444（yuv444p12le）。

## W10/W11：TOPOS ↔ ProRes 同名档对照（32 帧/档，分通道 PSNR，整数码值域）

| 档位 | topos bpp / PSNR Y/U/V | ProRes bpp / PSNR Y/U/V | 读法 |
| --- | --- | --- | --- |
| 2K proxy | 0.618 / 36.7,44.4,41.6 | 0.786 / 38.5,44.7,42.0 | topos 码率 ~79%，Y 低 ~1.8dB |
| 2K lt | 1.333 / 41.6,49.8,47.3 | 1.754 / 43.5,49.5,47.1 | topos 码率 ~76%，Y 低 ~1.8dB，色度持平 |
| 2K standard | 2.017 / 44.3,52.9,50.4 | 2.502 / 46.4,51.6,49.6 | topos ~81%，Y 低 2.2dB |
| 2K hq | 3.106 / 47.1,56.3,53.7 | 3.722 / 50.9,54.3,52.9 | topos ~83%，Y 低 3.8dB，色度反超 |
| 4K proxy | 0.619 / 52.6,58.8,57.9 | 0.395 / 53.7,58.9,58.0 | ProRes 码率更低且 Y 略优 |
| 4K lt | 1.284 / 55.9,62.1,61.5 | 1.435 / 58.2,63.3,62.7 | ProRes 同码率段更优 |
| 4K standard | 2.025 / 57.7,64.3,63.8 | 2.086 / 60.4,65.1,64.7 | 近同码率，ProRes Y +2.8dB |
| 4K hq | 2.968 / 59.4,66.8,66.4 | 3.167 / 64.9,68.4,68.2 | ProRes Y +5.5dB |
| 4K 4444 | 4.161 / 56.0,64.6,62.3 | 4.369 / 63.1,64.8,63.3 | ProRes Y 显著更优 |
| 4K 4444xq | 5.213 / 61.1,68.9,66.2 | **N/A**（prores_ks 不收 p12 输入） | 无对应规格不强配 |

多代漂移（1/3/5/10 代，同档重编码链）：2K hq 46.98→46.80dB（-0.18dB 收敛）；
4K 4444 55.97→55.55dB（-0.42dB 收敛）——无漂移失控。

**如实结论**：本机 Windows 实测下，422 家族 topos 同名档以 ~76–83% 码率换取
Y 通道 1.8–5.5dB 的 PSNR 落后（色度多档持平或反超）；444 档差距更大。
不宣称"优于 ProRes"；以上为同机同素材双轴数据，供后续 W13 优化对拍。

### 方法论要点（复现必读）

- **PyAV `to_ndarray` 陷阱**：对 yuv422p10le 会把色度上采样为 444（左半 ≠ 真实
  色度）——必须按 plane 缓冲逐行提取；编码侧同理由 `plane.update` 手工填充。
- **prores_ks 语义**：444 家族只接受 yuv444p10le 输入且**恒出 12-bit 容器**
  （10-bit 码 <<2 嵌入，解码须 >>2 还原）；4444xq+p12 为 EINVAL → xq@12 记 N/A。
- 444 家族 bd10 档的 12-bit 母版以 >>2 下变换（参考=同一 10-bit 版，与
  run_bench_tiers 的 `-pix_fmt yuv444p10le` 口径一致）；422 母版本身 10-bit 不移位。
- ProRes 码率为 mov 容器口径（含容器开销）；topos 为包字节均值。

## W12/W13：性能基线与第一轮优化

- `topos_quality perf`（完整 30 迭代）：1080p qp20 enc/dec/合计 p50≈9.9/8.6/48.7ms
  量级；4K qp20+A 218.5/72.8/245.9ms；**峰值 RSS=494,408 KB、句柄 68、线程 17**
  （W07 口径）。详见 `log_quality_perf_full_win_2026-09-28.txt`。
- 基准门：`topos_v7_size_gate`（**修复后**通过，见下）、`scalable_encode_gate`、
  `scalable_preview_decode`、`tier_decode_matrix`（2K→8K 全过，QPC 计时）。
- **W13 已实施**：MSVC fastdiv/rans2 `_umul128` 乘高位（位流逐位不变，ctest 93/93）；
  本机 1K² 快拍非热点（A/B 噪声内）。
- **发现并修复**：`v7_size_gate` 本地模型滞后于 `b41346b2`（B3 对计数位宽 3→4）
  在 texture 用例假失败——非 Windows 特有；模型已同步（基准恢复 16/16 用例）。

### 遗留观察（W12 记录在案）

- **V8 编码不并行**：threads=1/8/16 下 V8 编码 150/160/156 ms/帧（零扩展），
  V7 107/24/22.5 ms/帧（~4.8× 扩展）。V8 批 2 段化状态机为串行实现——
  设计现状而非 Windows 回退；V8 解码 ~7–9ms 且 ratio<1.3 门通过（忙机 1.09–1.34
  抖动，属共享机噪声敏感门，按计划 §4 归专用基准机）。
- 编码墙钟（本矩阵）：4K 422 hq ≈52ms/帧（~19fps）、4K 4444 ≈59ms/帧。

## W14（部分）

编解码层长压力：`test_stress` 3 轮×{1,4,24} 线程 + `unit_robust_io` 故障注入
（ctest）通过。产品入口（导出→重导入→seek）属 Windows GPU 计划管辖，
本轮未跑（无 GPU 依赖用例之外的产品链路）。>4GiB 真实 MOV mux 留待长序列基准。

## 证据

`win_rd_prores_win_2026-09-28.json`、`win_master_manifest_2026-09-28.json`、
`log_quality_perf_full_win_2026-09-28.txt`、`log_topos_*_win_2026-09-28.txt`、
`log_v7_size_gate_win_2026-09-28.txt`。
