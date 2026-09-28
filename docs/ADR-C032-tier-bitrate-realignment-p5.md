# ADR-C032：产品档位码率对齐重定标（P5）

- **状态**：当前（2026-09-10）
- **范围**：产品层（`src/shared/codec/topos_profiles.py` + 产品编码 qp 域打通）；**不改码流格式**
- **前置**：ADR-C031（v1.5 qp 域 0..95，native 层已交付但产品层未接）
- **测量基础**：`docs/codec/topos_prores_real_footage_2026-09-10.md`（真实素材对比）

## 背景与问题

真实素材基准（2K/4K prores422HQ 各 120 帧，25fps）显示：**同名档位下 Topos
码率是 ProRes 的 1.7~4.2×**（换取 +0.2~4.0 dB PSNR）。根因：

1. **两套定标从未对过标**。Topos 档位 = 计划 §2.1 纸面数据率区间中值
   （_bpp_from_mb_s）× P4 质量钉扎比例（~0.68~0.74）；ProRes 档位 =
   20 年行业惯例的体积约定。Topos 阶梯跨度仅 4.55×（0.605→2.756 bpp），
   ProRes 实测跨度 8.1~8.3×（proxy→HQ），且 Apple Proxy 本就是重量化离线档
   （实测 0.27~0.37 bpp）——错位在 proxy 档最重（1.87×），hq 档最轻（1.04×）。
2. **产品层 qp 域仍是 0..63**。ADR-C031 扩展了 native 域，但
   `topos_encoder.py`/`topos_binding.py`/`topos_rate_control.py` 六处
   硬编码 63——低码率档在产品路径根本无法命中（proxy@4K 旧 qp63 地板
   超支 ≈+19%）。

产品决策（用户拍板）：**体积优先，码率先对齐 ProRes 档位语义；解码速度其次；
质量差距是后续编码器工程**（帧内预测/上下文编码，ADR-I011 方向）。

## 决策

### 1. 产品编码 qp 域打通 0..95（前置工程）

- `topos_binding.py`：新增 `TOPOS_QP_MAX = 95`；`encode_sized` 默认
  `qp_max` 63→95；
- `topos_encoder.py`：sized 调用 ×2、AQ qp 顶格守卫 ×2、反馈码控
  `ToposRateFeedback(qp_max=95)`、crf 接受域 0..95；
- `topos_rate_control.py`：默认 `qp_max` 63→95。

实测 qp 落点（真实素材，CLI sized）：proxy qp78~84、lt 61~67、
standard 58~64、hq 54~60——proxy 全部落在旧 63 地板之下（不可达区），
证明域打通是重定标的硬前置。

### 2. 四个 422 档 target_bpp 按 ProRes 实测体积重定标

锚点 = 本机 `prores_ks` profile 0/1/2/3 在两段真实素材上的实测体积
（字节精确，`/tmp/realign` 复现），`target_bpp = mean(bpp_2K, bpp_4K)`：

| 档位 | ProRes 2K bpp | ProRes 4K bpp | **新 target_bpp** | 旧（P4） | 旧/新 |
|---|---:|---:|---:|---:|---:|
| proxy | 0.27304 | 0.37317 | **0.32310** | 0.60509 | 1.87× |
| lt | 0.89826 | 1.36999 | **1.13412** | 1.23239 | 1.09× |
| standard | 1.31559 | 1.97497 | **1.64528** | 1.87491 | 1.14× |
| hq | 2.21072 | 3.11417 | **2.66245** | 2.75617 | 1.04× |

- `reference_mb_per_s` 同步换为两段素材实测落点区间（1080p25 MiB/s）；
- **pro444/extreme 不动**（无 ProRes 4444 真实素材锚点，P4 定标保留，
  排序 2.66 < 4.11 < 6.76 仍成立，待补测后另行重定标）；
- 阶梯跨度 4.55×→8.24×，恢复 ProRes 档位语义形状。

## 实测验证（重定标后，同机同口径）

### 码率对齐（Topos 产品档 vs ProRes 同名档实测）

| 档位 | 2K Topos/ProRes | 4K Topos/ProRes | 旧比率区间 |
|---|---:|---:|---:|
| proxy | 16.7/14.2 Mbps = 1.18× | 66.0/77.4 = 0.85× | 4.18× / 3.05× |
| lt | 55.6/46.6 = 1.19× | 221.8/284.1 = 0.78× | 2.60× / 1.70× |
| standard | 80.5/68.2 = 1.18× | 321.9/409.5 = 0.79× | 2.69× / 1.79× |
| hq | 129.0/114.6 = 1.13× | 522.5/645.8 = 0.81× | 2.37× / 1.70× |

两素材均值按构造 = 1.00×；**单素材残余 ±0.78~1.19× 是固定 bpp 档位 vs
ProRes 随内容浮动（两段素材同档 ProRes bpp 本身差 35~52%）的固有带宽**。
若要求单素材钉死，需内容自适应码控（非本 ADR 范围）。

### 速度与质量（摘要，全表见报告 §三）

- **解码**（16t，中位 3 次）：2K 314~912 fps、4K 102~285 fps——vs ProRes
  auto 0.55~1.23×（2K）/ 0.82~1.52×（4K），**4K proxy/lt 反超**；比旧定标
  解码吞吐近乎翻倍（码率减半 ⇒ 熵解码负载减半）。
- **编码**（16t 墙钟）：2K 116~161 fps（1.7~2.4× prores_ks）、4K 34~46
  fps（2.2~2.6×）——领先保持。
- **质量代价**：同码率 PSNR 落后 ProRes 1.0~7.9 dB（结构性差距：Rice/VLC
  无帧内预测 vs 20 年调校 DCT+VLC；4K 侧含 0.78~0.85× 码率落点因素）。
  质量追平属编码器工程（ADR-I011），不属档位定标。

## 连带影响（已验证/已处理）

1. **TPIC 代理**：proxy 档字节目标 −47%，媒体代理体积同比缩小（预算
   机制不变）。
2. **Alpha 预算**：qp95 域下微小帧（64×40 测试夹具）smooth 颜色流可被
   压到远低于旧 qp63 地板 → alpha/颜色比例整体抬升，自适应 a12→a8 更
   早触发；mode1 无损仍绝不降质（测试钉死）。预算比例/硬上限常量未动。
3. **crf 语义**：固定 QP 覆盖接受域 0..63→0..95（数值语义不变）。
4. **测试更新**：`test_topos_export.py`（档位数据率区间、alpha 预算组、
   crf 范围）、`tests/native/test_topos_binding.py`（qp64/95 合法、96
   越界——v1.5 契约在 Python 侧补钉）。

## 兼容性

- 码流格式零变更（v1.5 包络内，ADR-C031）；
- 旧 .topos 文件解码不受影响；
- **行为变更**：产品导出同名档位体积下降（proxy −47%、lt −8%、
  standard −12%、hq −3%），PSNR 相应下移——这正是本 ADR 的目的；
- 帧内搜索 qp 分布越 63（proxy 78~84）输出的流帧头 version_minor=4，
   旧（v1.5 前）解码器按未知 minor 干净拒绝。

## 复现

```bash
# ProRes 锚点（真实素材 raw 见报告 §复现）
ffmpeg -f rawvideo -pixel_format yuv422p10le -video_size 3840x2160 -framerate 25 \
  -i src4k.raw -c:v prores_ks -profile:v 0 -frames:v 120 p.mov   # ×4 档 ×2 分辨率
# Topos 新档位（整数 Mbps = target_bpp×W×H×25/1e6 四舍五入）
TOPOS_SLICE_THREADS=16 topos_encoder_cli --width 3840 --height 2160 --fps 25 --qm 1 \
  --target-mb 67 --frames 120 --input src4k.raw --output out.mov
# 解码：TOPOS_SLICE_THREADS={1|16} topos_decoder_cli → /dev/null ×3 取中位
```
