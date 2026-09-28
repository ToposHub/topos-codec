# Topos 图片规格体积实测（2026-09-07）

本报告使用 `tests/test_videos/2K_h264.mp4` 第 0 帧（1920×1080，源 `yuv420p`），在提交 `db8661e5` 的 Release `toos` 上测量。输入帧分别转换为 4:2:2 10-bit 和 GBR 4:4:4 12-bit；每个 `.toos` 文件均通过 `toos verify --deep`。

## 结果

| 规格 | 输入/位深 | 文件大小 | 相对 422 High | 编码进程耗时 | 解码 p50 | 解码 p95 |
|---|---|---:|---:|---:|---:|---:|
| Topos 422-low | 4:2:2 / 10-bit | 1.117 MB (1116689 B) | 0.63× | 75.5 ms | 4.044 ms | 4.632 ms |
| Topos 422-medium | 4:2:2 / 10-bit | 1.365 MB (1364980 B) | 0.77× | 77.1 ms | 4.657 ms | 5.882 ms |
| Topos 422-high | 4:2:2 / 10-bit | 1.770 MB (1769571 B) | 1.00× | 80.0 ms | 6.071 ms | 7.829 ms |
| Topos 422-ultra | 4:2:2 / 10-bit | 2.794 MB (2793851 B) | 1.58× | 87.0 ms | 7.971 ms | 8.693 ms |
| Topos 444-high | GBR 4:4:4 / 12-bit | 7.001 MB (7001195 B) | 3.96× | 116.1 ms | 16.265 ms | 17.747 ms |
| Topos 444-ultra | GBR 4:4:4 / 12-bit | 7.643 MB (7643358 B) | 4.32× | 123.1 ms | 16.148 ms | 18.336 ms |

## 同一帧参考格式

| 格式 | 文件大小 | 备注 |
|---|---:|---|
| PNG 8-bit RGB | 2.994 MB (2994150 B) | 8 |
| PNG 16-bit RGB | 3.941 MB (3941403 B) | 16 |
| JPEG q2 | 0.619 MB (619347 B) | ~8 |

## 观察

- 这张真实 H.264 帧上，Topos 422 High 为 1.770 MB，约为 8-bit PNG 的 0.59×；422 Low/Medium 分别为 1.117 / 1.365 MB。
- 422 Ultra 使用 flat 量化矩阵，体积约为 422 High 的 1.58×；4:4:4 12-bit 保留三个全分辨率 GBR 平面，因此体积约为 422 High 的 3.96–4.32×。
- `toos benchmark` 的 decode 是热缓存、20 次迭代；编码耗时是单次 CLI 进程墙钟时间，包含 raw 文件读取和编码，不包含视频帧抽取。
- 白皮书中的 MB/帧是锚点素材，不是固定上限；熵编码后的实际大小会随画面纹理、噪声和色度复杂度变化。

## 可复现命令

```bash
ffmpeg -i tests/test_videos/2K_h264.mp4 -frames:v 1 -pix_fmt yuv422p10le -f rawvideo /tmp/frame_422p10.raw
ffmpeg -i tests/test_videos/2K_h264.mp4 -frames:v 1 -pix_fmt gbrp12le -f rawvideo /tmp/frame_gbrp12.raw
native/topos_codec/build/release/toos encode /tmp/frame_422p10.raw -o /tmp/422-high.toos --width 1920 --height 1080 --tier 422-high --overwrite
native/topos_codec/build/release/toos benchmark /tmp/422-high.toos --iters 20
native/topos_codec/build/release/toos verify /tmp/422-high.toos --deep
```

完整机器信息、SHA-256、每档计时原始值见同名 JSON。
