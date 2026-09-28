# Topos Image（.toos）图片格式白皮书

**基于 Topos 视频编解码内核的静态图片中间格式 · V1 Preview**

版本 1.0 · 2026-09-05 · Apache License 2.0 开源发布

---

## 1. 引言

**Topos Image（`.toos`）** 是构建在 Topos Video Codec 内核之上的静态图片中间格式：一个 `.toos` 文件 = **TPIM 文件信封** + **一个完整的 TPIC 帧内码流包**（与视频链路完全相同的单帧编码包）。

设计上的核心约束只有一条，也是它最大的可靠性来源：**文件层（TPIM）禁止任何像素编解码数学**——封装、目录、CRC、图片语义元数据之外的像素编解码 100% 复用 `libtopos_codec` 同一套核心（精确整数变换、量化矩阵、V2 canonical VLC 熵编码）。规范以"禁止第二实现"冻结，杜绝视频/图片两条解码路径的分叉。

定位：面向 VFX 合成、调色、贴图与渲染缓存的高保真图片中间格式——**高画质 + 自包含元数据 + 原子写 + 序列友好**，同时保持比 PNG 更快的解码速度。

## 2. 设计目标

| 目标 | 说明 |
| --- | --- |
| 与视频同源 | 像素编解码 100% 复用视频内核，同一 golden 门禁覆盖，零实现分叉 |
| 确定性输出 | 相同输入逐字节相同的编码结果；`tc_image_write` 保证可复现 |
| 结构可校验 | 64B 前导 + 目录 + 逐 chunk CRC-32，`tpos verify --deep` 流式全检 |
| 元数据自包含 | ICC 色彩配置、OCIO 配置名、EXIF、XMP、SHA-256 指纹随图携带 |
| Alpha 一等公民 | straight（A16 无损）与 premultiplied（有界误差声明）两种语义显式区分 |
| 原子落盘 | 同目录临时文件 → 写满 → 校验 → rename，失败自动清理，绝不留半张图 |
| 显式边界 | 能力以 manifest + `tc_image_query_capabilities` 机器可读声明，不支持即拒绝，不静默猜测 |

## 3. 文件格式要点

- **扩展名**：`.toos`（唯一）；MIME `image/x-toos`；文件类型以 `TPIM` 魔数为最终依据。内层码流包沿用视频链的 `TPIC` magic。
- **容器结构**：64 字节 preamble（版本 / 文件大小 / 目录偏移 / CRC-32）→ 32 字节目录项 × n（chunk FourCC / flags / offset / size / crc32）→ chunks；**全 big-endian**，禁止序列化 C 结构体。
- **chunk 体系**：
  - critical：`IDSC`（128 字节图片描述，逐偏移冻结）+ `PIXL`（完整 TPIC 码流包，上限 256 MiB）；
  - optional：`ICCP`（ICC 配置）/ `OCIO`（配置名）/ `EXIF` / `XMP` / `THMB`（可丢弃缩略图）/ `HASH`（SHA-256）；
  - `TILE` / `MIPM` 等为 v1 预留，按 unknown 处理（未知 critical chunk 拒绝解码——前向安全）。
- **一致性门**：`IDSC` 与 `PIXL` 之间十项交叉校验（尺寸 / profile / 位深 / alpha / CICP / SAR / 头部 CRC 等），并提供 `tc_image_derive_idsc` 从码流包派生描述块，杜绝手拼不一致。
- **硬上限**：8192×8192 像素；解析期分配有界（目录 + IDSC ≤ 64 KiB）。
- **明确不在 v1 范围**：32-bit 浮点、tile/ROI、mipmap、多图、浏览器内嵌解码。（半精度浮点 HALF 样本域已随 spec §15 激活——§4/§5；相机 RAW 见 §4 Image RAW。）

## 4. 色彩模式

| ID | 名称 | 像素格式 | 位深 | 状态与定位 |
| --- | --- | --- | --- | --- |
| 0 | Image Preview | YUV 4:2:2 | 10/12 | 体积优先；有色度子采样损失（明确不宣称色彩无损） |
| 1 | Image HQ | GBR 4:4:4 | 10 | **VFX beauty / 贴图 / 渲染缓存主档**，RGB 直通不经 YUV |
| 2 | Image XQ | GBR 4:4:4 | 12 | 高精度整数中间片 |
| 3 | Image RAW（TRAW） | Bayer CFA 4 相位平面（R/Gr/Gb/B 各 W/2×H/2） | 12/16 | 相机 RAW 制作档：12-bit Log（transfer=TRAW_LOG0）制作 / 16-bit Linear 归档；解码器透传相位平面，debayer/ISP 由应用层消费。2026-09-13 由 reserved 值 3（Image Lossless）改派——无损语义由 RAW Linear 模式承载（ADR-I001 D9 相应作废） |
| 4 | Image HF（浮点） | GBR 4:4:4 | 16（float16） | HALF 样本域（spec §15，2026-09-19）：HDR 线性合成中间件/精确缓存；样本域由 IDSC sample_kind 自描述，经冻结单调映射进入同一整数编码内核 |

GBR 4:4:4 路径（identity matrix）让 RGB 合成素材完全避开 YUV 往返；RAW 路径让相机原生 CFA 数据避开非破坏前的 ISP 烘焙。

## 5. 编码档位体系

全 22 档（`toos encode --tier`；2026-09-21 与 CLI 预设表、`docs/capability_manifest.json` 三方对齐，应用/UI 取名自同一张注册表）。锚点为 1080p 实拍/真实素材的 CLI 实测单帧文件体积：

**整数家族**（YUV 4:2:2 与 GBR 4:4:4）：

| 档位 | 格式 | 量化矩阵 | qp | ≈MB/帧 (1080p) | 保真度与用途 |
| --- | --- | --- | ---: | ---: | --- |
| **Topos 422 Low** | YUV 4:2:2 10-bit | 422 Low Compact | 63 | 1.08 | 预览 / 审片 / 代理 |
| **Topos 422 Medium** | YUV 4:2:2 10-bit | Standard | 61 | 1.29 | 体积低于同素材 PNG 8-bit |
| **Topos 422 High** | YUV 4:2:2 10-bit | Standard | 58 | 1.71 | PNG 锚点档（同素材 PNG ≈ 2.04 MB） |
| **Topos 422 Ultra**（默认） | YUV 4:2:2 10-bit | flat | 58 | 2.86 | 视觉无损级 |
| **Topos 444 High** | GBR 4:4:4 10-bit | Standard | 63 | 1.30 | 全色度合成收缩档（修订六：与 ProRes 4444 同位深口径） |
| **Topos 444 Ultra** | GBR 4:4:4 10-bit | flat | 58 | 3.40 | 全色度视觉无损级 |

**浮点家族**（GBR 4:4:4 float16，image_profile 4 + transfer linear；v1.8 产品命名 ADR-C057——**不追求无损**，发挥中间片编码优势：较小体积 + 非常好画质 + 编解码实时；锚点 = 2K 真实视频帧三素材均值，spec §15.3）：

| 档位 | 格式 | qp | ≈MB/帧 (1080p) | 保真度与用途 |
| --- | --- | ---: | ---: | --- |
| **toos Low 16bit float** | GBR float16 | 82 | 0.38 | 审片代理（PSNR ≈ 36 dB） |
| **toos Medium 16bit float** | GBR float16 | 72 | 0.99 | 代理（PSNR ≈ 44 dB） |
| **toos High 16bit float** | GBR float16 | 58 | 4.45 | 高画质（PSNR ≈ 71 dB，2.6–3.2:1） |
| **toos Ultra 16bit float** | GBR float16 | 44 | 5.75 | 视觉透明（±2 half-ULP；小于 EXR half zip16 同帧 6568 KiB） |

**RAW 家族**（TRAW：pf=3 CFA 4 相位平面 + profile 7 + qm0 冻结；输入为 4 相位平面 R/Gr/Gb/B 依次拼接的 uint16 LE 原始数据。比率档名为 CQ 定位标签——体积随内容浮动，无码控机。12-bit = Log 制作档（transfer=TRAW_LOG0），16-bit = Linear 归档档）：

| 档位 | 格式 | qp | 名义锚¹ (≈MB/帧 1080p) | 定位 |
| --- | --- | ---: | ---: | --- |
| **Topos RAW 12-bit 2:1** | CFA 12-bit Log | 45 | 2.07 | 近无损制作 |
| **Topos RAW 12-bit 4:1**（RAW 默认） | CFA 12-bit Log | 59 | 1.04 | 透明线制作档 |
| **Topos RAW 12-bit 6:1** | CFA 12-bit Log | 64 | 0.69 | 制作档 |
| **Topos RAW 12-bit 8:1** | CFA 12-bit Log | 66 | 0.52 | 制作档 |
| **Topos RAW 12-bit 12:1** | CFA 12-bit Log | 68 | 0.35 | 制作档 |
| **Topos RAW 12-bit 16:1** | CFA 12-bit Log | 70 | 0.26 | 审片代理 |
| **Topos RAW 16-bit 2:1** | CFA 16-bit Linear | 51 | 2.07 | 近无损归档（实测 1.5–2.0:1） |
| **Topos RAW 16-bit 4:1**（RAW 默认） | CFA 16-bit Linear | 72 | 1.04 | 归档默认（实测 3.0–6.2:1） |
| **Topos RAW 16-bit 6:1** | CFA 16-bit Linear | 76 | 0.69 | 归档（实测 3.8–10.8:1） |
| **Topos RAW 16-bit 8:1** | CFA 16-bit Linear | 78 | 0.52 | 归档（实测 4.4–14.7:1） |
| **Topos RAW 16-bit 12:1** | CFA 16-bit Linear | 80 | 0.35 | 归档（实测 5.2–21.1:1） |
| **Topos RAW 16-bit 16:1** | CFA 16-bit Linear | 82 | 0.26 | 审片代理（实测 6.5–31.9:1） |

> ¹ RAW 锚点按名义比率反推（CFA 单通道 16-bit 容器名义 4.15 MB/1080p 帧 ÷ N），非实测承诺——实测比率随内容浮动（纹理重的航拍 7:1 封顶、干净画面 >30:1）。

### 5.1 命名规则（全产品线统一）

| 规则 | 内容 | 示例 |
| --- | --- | --- |
| N-1 家族前缀 | 视频线 `Topos`（大写，MOV 容器）；图片线 `toos`（小写，`.toos` 文件） | `Topos 422 HQ` / `toos High 12bit` |
| N-4 图片整数档 | `toos <Quality> <bd>bit`，Quality ∈ {Low, Medium, High, Ultra}（422 质量阶梯；444 家族不入阶梯，文件内展示回退 Image HQ 类名） | `toos High 12bit` |
| N-5 图片浮点档 | `toos <Quality> <bd>bit float`（HALF 域；未来 32-bit 档仅换位深数字，规则不变） | `toos Ultra 16bit float` |
| N-6 图片 RAW 档（文件内显示名/TMET） | `toos RAW <bd>bit <N>:1`——图片家族小写、位深无连字符；规格文档/白皮书层一律用 N-3 带连字符形式 `Topos RAW <bd>-bit <N>:1` | `toos RAW 12bit 4:1` |
| N-7 单一注册表 | 全部输出规格选择面（图像序列导出 / Quick Export / CLI `--tier` / probe 显示 / TMET label）由同一份档位注册表派生（主仓库 `src/shared/codec/topos_profiles.py` 的 `TOPOS_IMAGE_TIERS`） | — |
| N-8 档位名不承载的词汇 | Log/Linear、无损、实验性等定位词只进描述文案与文档，不进档位名 | — |

产品语义（如实声明）：

- 档位定位是**"很好画质 + 相对小体积"，不设真无损档**；数学无损走 `--qp 28`（10-bit）或 `--half --qp 20/32`（浮点域，spec §15）或 PNG/EXR/TIFF 无损容器。
- qp 域为 0–95（v1.5 扩展）；浮点/RAW 档位锚点落在 44–82。
- **与视频档的体积差距是结构性的**：图片档单帧自包含（随机访问 + 元数据 + 原子写），无视频档的逐帧码率闭环——这是特性不是缺陷，两者的体积不可直接对表。
- 代理场景推荐半分辨率工作流：960×540 + 422 Low ≈ 0.4 MB/帧（约为全分辨率 Low 的 30%）。
- 档位是**显式可查询的能力声明**：无 `--tier` 时整数默认 = 422 Ultra（qm0 qp58）；RAW 无 `--tier` 时默认 = raw12-4（log 制作透明线）。

## 6. Alpha 与元数据

- **三态显式**：absent / straight（A16 容器，无损直通 0..65535）/ premultiplied（有界，必须声明 `alpha_bit_depth` ∈ {8,10,12} 与实测 `alpha_max_abs_err`）。
- Alpha 为独立全分辨率平面：不降采样、不进颜色死区；alpha=0 处 RGB 编码值逐字保留。
- 元数据 chunk 带校验读取（ICC 必须含 `acsp` 签名、OCIO 名 UTF-8 可打印），冲突即硬错误，不静默消化。

## 7. 序列能力

面向渲染序列的一等支持（`ToposImageSequence` / MediaSource 协议）：

- **帧号语法冻结**：`<前缀><帧号><后缀>`，支持负号与可选补零；补零位宽严格区分（`frame_1` 与 `frame_0001` 属不同序列）。
- **step 锚点语义**：`(frame - anchor) % step == 0`，锚定绝对帧号，不依赖目录中恰好存在哪些文件。
- **缺帧策略三选一**：skip（跳到最近后继）/ error（区间空洞报错）/ hold（复用最近前驱，连续空洞超限报错）——缺帧绝不伪造成功。
- **资源有界**：单后台预取线程 + 世代取消（seek 即失效）+ 解码平面 LRU 字节上限；帧级并行 = 1 × slice 级并行，不过度订阅 CPU。

## 8. 使用方法

### 8.1 构建

图片层随 `libtopos_codec` 一起构建（同一 CMake 工程，无额外依赖）：

```bash
cmake -S native -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j          # 产出 libtopos_codec + toos CLI
```

### 8.2 C API（`topos_image.h`，ABI v1，38 项符号门禁）

```c
tc_image_probe(file, &info);                       /* 只读头部，零像素分配 */
tc_image_validate(file, TC_IMG_VALIDATE_DEEP);     /* 结构 + 逐 chunk 流式 CRC */
tc_image_decode(file, planes);                     /* 调用方提供平面，零拷贝直写 */
tc_image_query_capabilities(&caps);                /* 能力协商 */
tc_image_derive_idsc(packet, &idsc);               /* 从码流派生一致描述块 */
tc_image_write(file, &idsc, planes, &meta_io);     /* 确定性原子写 */
```

### 8.3 CLI（`toos`）

```bash
toos probe   image.toos                    # 解析图片描述
toos verify  image.toos --deep             # 结构 + 全量 CRC
toos encode in.raw -o image.toos --width 1920 --height 1080 \
            --tier 444-high                # 档位预设（pf/bd/qp/矩阵随档）
toos encode linear_f16.raw -o f16.toos --width 1920 --height 1080 \
            --tier float-ultra             # 浮点档（输入 = float16 位型 planar）
toos encode bayer.raw -o raw.toos --width 1920 --height 1080 --tier raw12-4
toos benchmark image.toos --iters 20       # probe/validate/decode 分位计时
```

无 `--tier` 时整数默认 = 422 Ultra（qm0 qp58，V7-R2 rANS 熵 + RDO）；RAW 无 `--tier` 默认 = raw12-4；显式参数覆盖档位。全 22 档清单见 §5。

### 8.4 Python

```python
from topos_codec.topos_image_binding import ToposImageCodec   # 发布包；主仓库为 src.shared.codec.topos_image_binding
img = ToposImageCodec()
info = img.probe("image.toos")
planes = img.decode("image.toos")            # numpy 零拷贝
```

应用侧另有 `ToposImageSource`（单图）与 `ToposImageSequence`（序列）实现 MediaSource 协议，像素格式注册名与元数据键与视频链完全一致。

### 8.5 PNG 互转

```bash
python scripts/toos_tools.py decode image.toos -o image.png     # 16-bit PNG
python scripts/toos_tools.py encode image.png -o image.toos \
       --bit-depth 10 --image-profile 1
```

YUV → PNG 转换被显式拒绝（不静默猜测 sRGB），杜绝色彩语义错误。

## 9. 性能

release 构建、1080p 实拍素材（ADR-I010 口径）：

| 指标 | 数值 |
| --- | --- |
| 编码（进程内，含保守余量） | 16–33 ms/帧 |
| 解码（V2 VLC） | ≈ 13.9 ms/帧 |
| 对照：PNG 8-bit 解码 | 31.5 ms（16-bit 56 ms）——**toos 解码快 2.3×** |
| 同序列体积（VLC 转正后） | 12.3 → 3.28 MB/帧（−73%），为 PNG 8-bit 的 1.6×（qp61 档 ≈ 1.2×） |
| probe（只读头部） | ≈ 0.017 ms |

封装层开销占整图解码 ≈2%（4K）~8%（1080p）；普通 pread 顺序读已达 ≈2.4 GB/s，mmap 保持关闭。

## 10. 完成度与边界（V1 Preview，如实声明）

- **已完成**：TPIM 解析/写入 + 安全基础设施、GBR 10-bit、12-bit + 元数据、完整 ABI + CLI + 原子写 + PNG 互转、性能实测；门禁 = 单测 + ASan/UBSan + fuzz + 38 项 ABI 符号钉死 + 5 组 image golden + 68 项 image pytest，旧视频 golden 零回归。
- **有Deferred 的部分**：应用导出/缩略图/导入探测三处接线、UI/播放链接入、Windows/Linux 构建与 ABI smoke、1000+ 帧压测、OIIO 插件往返验收（源码已交付，待 SDK 环境）。
- **明确范围外**：Nuke / Photoshop / After Effects 插件（后续独立立项）。
- **开放风险（标题级）**：高熵 8K RGBA 逼近 256 MiB 包上限；上游 codec 演进与 image golden 的回归门（由 golden 门禁承接）。

## 11. 许可

随 Topos Video Codec 以 **Apache License 2.0** 开源发布（见 `LICENSE`）。图片层与视频层同库同许可证。

**商标说明**：PNG、TIFF 为各自相应组织的公开格式名称；OpenEXR/OpenImageIO 为其相应项目的名称。上述名称仅用于互操作性与对比性的事实描述。

## 12. 规范文档索引

| 文档 | 内容 |
| --- | --- |
| `docs/topos_image_file_spec_v0.md` | TPIM 文件格式规范（v0 冻结） |
| `docs/SDK.md` | 集成指南 |
| `docs/capability_manifest.json` | 机器可读能力清单（色彩模式矩阵 / 档位） |
| `docs/ADR-I001…I011` | 图片层全部架构决策记录 |
| `docs/benchmark_protocol.md` | 性能测量协议 |

---

*Topos Image · Copyright (c) 2026 Topos Color 项目 · Apache License 2.0*
