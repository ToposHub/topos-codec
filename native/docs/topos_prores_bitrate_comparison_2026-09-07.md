# Topos 与 ProRes 不同规格码率对比（2K / 1080p25）

测试使用同一批 96 帧素材，输出为 MOV，码率按实际文件大小换算。Topos 使用产品默认 V2 canonical VLC；ProRes 使用本机 FFmpeg `prores_ks` 软件编码器。

| 对应规格 | Topos 位深 | Topos Mbps | ProRes 位深 | ProRes Mbps | Topos/ProRes |
|---|---:|---:|---:|---:|---:|
| 422 Proxy | 10-bit | 32.56 | 10-bit | 12.69 | 2.57x |
| 422 LT | 10-bit | 59.48 | 10-bit | 40.92 | 1.45x |
| 422 | 10-bit | 91.79 | 10-bit | 60.62 | 1.51x |
| 422 HQ | 10-bit | 136.77 | 10-bit | 106.47 | 1.28x |
| 4444 | 10-bit | 214.14 | 10-bit | 290.44 | 0.74x |
| 4444 XQ | 12-bit | 427.45 | 10-bit | 428.20 | 1.00x |

## 观察

- 4:2:2 档位中，Topos 实测码率约为 ProRes 对应档的 1.28–2.57 倍；随着档位升高，差距缩小。
- Topos 4444 10-bit 为 214.14 Mbps，ProRes 4444 为 290.44 Mbps，Topos 约为 0.74 倍。
- Topos 4444 XQ 为 12-bit、427.45 Mbps；本机 ProRes 4444 XQ 因输入限制使用 10-bit，428.20 Mbps，不能作为严格同位深结论。
- 同名档位的 PSNR 不完全相同，因此这张表回答的是“实际码率对比”，不是同画质率失真结论。

## 可复现命令

```sh
TOPOS_CODEC_LIB="$PWD/native/topos_codec/build/v6-dev/topos_codec.dylib" \
python3 tools/bench_codecs_matrix.py --res 2k --families 422 --frames 96 --runs 3 --threads 16
TOPOS_CODEC_LIB="$PWD/native/topos_codec/build/v6-dev/topos_codec.dylib" \
python3 tools/bench_codecs_matrix.py --res 2k --families 44410,44412 --frames 96 --runs 3 --threads 16 --allow-dirty
```

原始矩阵结果已保存在 `bench_out/`，汇总数据见同目录 JSON。
