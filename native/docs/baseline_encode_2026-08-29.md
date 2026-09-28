# Topos Codec 阶段 0 基线：macOS CPU 编码速度与画质（2026-08-29）

- 目的：为 Topos Video Codec 建立现有中间片编码器的**编码速度/码率/画质**参考基线
  （此前仓库只有解码基线与 Windows AMF 编码脚本，无 macOS 编码数据）。
- 方法：FFmpeg 8.1 CLI，`-frames:v 120`，`time.perf_counter` 计壁钟时间。
  **计时含输入解码**（CLI 限制）：`prores→prores/dnxhr` 组的输入解码很快，可视为
  「接近纯编码」的下界估计；`h264→` 组含 8-bit 源解码 + 像素格式转换，数值偏保守。
  精确方法（预解码进内存后编码）见 `benchmark_protocol.md` §4.2，阶段 9 复测时采用。

## 环境

| 项 | 值 |
| --- | --- |
| 机型 | Intel i9-9900K @ 3.60GHz（8C16T），32GB |
| 系统 | macOS 15.3.1 (24D70) x86_64 |
| 编码器 | ffmpeg 8.1（/usr/local/bin，built with Apple clang 16） |
| 素材 | `tests/test_videos/2K_h264.mp4`（1920×1080 yuv420p 8-bit 25fps，胶片颗粒重）；`2K_prores422HQ.mov`（ProRes 422 HQ 10-bit 422） |

## 结果

| 源 | 编码器 | 时间(s/120帧) | fps（含输入解码） | 输出(MB) | 码率 @25fps |
| --- | --- | ---: | ---: | ---: | ---: |
| h264 8-bit | prores_ks 422 HQ | 4.02 | 29.8 | 112.2 | 186.9 Mb/s |
| prores422HQ | prores_ks 422 HQ | 2.64 | 45.5 | 68.8 | 114.6 Mb/s |
| h264 8-bit | dnxhr_hqx | 2.50 | 47.9 | 110.2 | 183.6 Mb/s |
| prores422HQ | dnxhr_hqx | 2.06 | 58.3 | 110.1 | 183.5 Mb/s |

**画质**（对 2K_h264 源，双侧 `format=yuv422p10le` 后 psnr 滤波器，120 帧）：

| 编码 | PSNR-Y | PSNR-U | PSNR-V | average | min | max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ProRes 422 HQ | 45.49 | 55.92 | 58.50 | **48.21** | 28.60 | 73.04 |
| DNxHR HQX | 45.81 | 57.18 | 59.41 | **48.57** | 28.58 | 71.73 |

## 观察

1. 颗粒重的 8-bit 源使 ProRes 输出码率膨胀到 186.9 Mb/s（干净 10-bit 源为 114.6），
   验证了计划 §2.3「高噪声导致爆码」风险与帧内 VBR 的必要性。
2. DNxHR HQX 在两种源上码率几乎恒定（183.5/183.6）——固定码率特征，可作 Topos
   「码率可控性」的对照参考。
3. 该素材上 DNxHR 编码速度 ≈ ProRes 的 1.2–1.6×；两者画质（对有损源）几乎同级。
4. Topos Standard 目标区间 14–19 MB/s（112–152 Mb/s @1080p25）落在两者之间。

## 复现命令

```bash
cd "<repo>"
SRC8="tests/test_videos/2K_h264.mp4"; SRC10="tests/test_videos/2K_prores422HQ.mov"
time ffmpeg -y -i "$SRC10" -frames:v 120 -c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le /tmp/out_prores.mov
time ffmpeg -y -i "$SRC10" -frames:v 120 -c:v dnxhd -profile:v dnxhr_hqx -pix_fmt yuv422p10le /tmp/out_dnxhr.mov
ffmpeg -i /tmp/out_prores.mov -i "$SRC8" -frames:v 120 -lavfi \
  "[0:v]format=yuv422p10le[a];[1:v]format=yuv422p10le[b];[a][b]psnr" -f null -
```

4K 与 12-bit 组、多代退化曲线：阶段 4 `quality_report_scalar.md` 一并建立（见
`benchmark_protocol.md` §5）。
