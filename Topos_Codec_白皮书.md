# Topos Video Codec 白皮书

**帧内中间片编码引擎 · V1 Preview（10/12/16-bit 4:2:2 / 4:4:4 / GBR）**

版本 1.1 · 2026-09-21 · Apache License 2.0 开源发布

---

## 1. 引言

Topos Video Codec（`libtopos_codec`）是一套面向**中间片（mezzanine）制作流程**的全帧内视频编解码引擎，定位与 Apple ProRes、Avid DNxHR 同一生态位：为剪辑、调色、合成与多代渲染提供**恒定质量、逐帧独立、解码廉价、多代稳定**的中间码流。

与同类格式不同，Topos 从第一天起就是为**实时高分辨率工作流**设计的：

> **商标与关联声明**：Topos Video Codec 为独立自研格式，与 Apple、Avid、
> Blackmagic Design 等公司无任何关联，未获其赞助或背书。本文中出现的
> Apple、ProRes、QuickTime（Apple Inc. 商标）、DNxHR（Avid Technology, Inc.
> 商标）、DaVinci Resolve（Blackmagic Design Pty Ltd. 商标）等名称，仅用于
> 指称并对比相应技术；文中对比数据均以 FFmpeg 开源软件实现为测量对象
> （见基准协议），不代表上述公司的官方实现。

- **纯 C11、零第三方依赖**——全库仅链接 libc 与 pthread/Win32 线程原语，没有任何 FFmpeg/第三方库依赖，可嵌入任何宿主应用而不引入传递依赖；
- **质量与码率显式可查询**——七档 Profile 以"能力声明"而非隐式参数暴露，码率、位深、Alpha 策略全部机器可读；
- **帧级确定性码控**——给定目标码率，每帧独立执行精确 qp 搜索，输出可复现；
- **自描述码流**——解码器按帧头协商格式，旧版本文件永远可读。

本文面向集成方、工作流工程师与格式评测者，说明 Topos 的设计目标、档位家族、技术规格与使用方法。更底层的细节见配套规范（`docs/bitstream_spec_v2.md`、`docs/container_spec_v1.md`、`docs/SDK.md`）。

---

## 2. 设计目标

| 目标 | 说明 |
| --- | --- |
| 制作级画质 | 10/12/16-bit 量化、精确可逆整数变换、冻结量化矩阵（QM v1.1），多代渲染无质量漂移 |
| 实时解码 | 全帧内 + O(1) 索引随机访问：1080p 解码 ≈ 5 ms/帧，4K ≈ 13 ms/帧（详见 §8） |
| 确定性输出 | 同参数同输入必得同码流；golden 一致性测试逐字节钉死 |
| 零依赖可移植 | C11 + libc + pthread；Linux / macOS（universal）/ Windows（MSVC、MinGW）三平台 CI 常驻 |
| 显式能力协商 | 档位 = 可查询能力（`tc_query_support` / capability manifest），不允许隐式参数猜测 |
| 中间片纯度 | 像素格式与色彩元数据完整随流携带；v1.1 起可选单音频轨（lpcm 无损 / AAC 交付档，ADR-C051），纯视频文件形态不变 |

---

## 3. 技术概览

**编码管线**（帧内档每帧独立，无帧间预测；LP 帧间档除外，见 §4.1）：

```
输入平面 (YUV 4:2:2/4:4:4, GBR; 10/12/16-bit; 可选 Alpha 平面)
  → 精确正交整数变换（无浮点漂移，多代零偏移）
  → 冻结量化矩阵 QM v1.1 + 档位目标码率 qp 搜索（或显式 QP）
  → 熵编码：V2 canonical VLC（默认）/ 有界 Rice（V1 可选）
  → slice 并行封装（有界常驻线程池，运行时 CPU 特性分发）
  → MOV 派生容器（.mov，FastStart 元数据前置，O(1) 采样索引）
```

关键特性：

- **熵编码双模**：默认 V2 canonical VLC 在同画质下较 V1 有界 Rice 节省 **23–32%** 码率；解码侧码流自描述，V1 文件永远可读。
- **SIMD 运行时分发**：AVX2（变换 + RGB↔YUV 色彩转换）、NEON（Apple Silicon），按 `cpuid` 运行时选择，无多版本二进制。
- **精确色彩转换**：RGB↔YUV 支持完整/受限范围与 BT.709/BT.2020 等矩阵，转换结果与参考浮点实现逐位对齐。
- **切片级 concealment**：码流局部损坏时解码器按切片隐藏错误，整帧不失败（返回 `TC_WARN_CONCEALED`）。
- **流式解码器**：持久 decoder context 支持逐帧/批量解码，零拷贝平面输出。
- **Alpha 通道**：帧内档全部可选携带 Alpha（近似模式默认 12-bit 内部保真，交付 a8/a10/a12/a16），码率预算独立闭环（每档声明 Alpha 预算比例与硬上限）；LP 帧间档显式 no-alpha（§3.7 收缩）。

---

## 4. Topos 档位家族

Topos 家族八个档位的命名与定位对标 ProRes 家族，便于工作流映射。其中 **422 Net Proxy / 422 Proxy / 422 LT / 422 / 422 HQ** 共享同一 4:2:2 码流 profile（profile=3），区别只是目标码率与 Alpha 策略（Net Proxy 为低码率网络档，非 ProRes 对标档）；**4444**（profile=5）与 **4444 XQ**（profile=6）是独立的码流 profile，由帧头字节区分；**422 LP**（v1.8，ADR-C056）是首个非帧内档——帧间微 GOP（zero-motion IP-2，码流载体 V7-R3），固定锚 qp=72、no-alpha、无码控，体积带 ≈ LT 的 89~94%，编码实时（2K 分段 107 fps、4K 79 fps，ADR-C050）。tier（质量档位）与帧间性正交：帧间性由码流包头自描述，tier 只描述质量档位语义。视频 **RAW** 档（v1.9，ADR-C058，容器 tier 8）为声明先行，随视频 RAW 产品化（R7 编码链）激活。

### 4.1 档位规格总表

| 档位 | 像素格式 | 位深 | 1080p25 参考数据率¹ | 目标 bpp² | Alpha 预算/硬上限 | 主要用途 |
| --- | --- | --- | --- | --- | --- | --- |
| **Topos 422 Net Proxy** | YUV 4:2:2 | 10 | 15 Mbps | 0.291 | 20% / 25% | 低带宽远程预览、网络流转码（非 ProRes 对标档） |
| **Topos 422 Proxy** | YUV 4:2:2 | 10 | 33 Mbps | 0.645 | 20% / 25% | 离线代理、远程剪辑 |
| **Topos 422 LT** | YUV 4:2:2 | 10 | 70 Mbps | 1.352 | 20% / 25% | 轻量中间缓存、粗剪 |
| **Topos 422**（默认档） | YUV 4:2:2 | 10 | 109 Mbps | 2.112 | 25% / 30% | 常规剪辑、渲染缓存 |
| **Topos 422 HQ** | YUV 4:2:2 / 4:4:4 / GBR | 10/12/16 | 163 Mbps | 3.151 | 25% / 30% | 调色、中间母版 |
| **Topos 4444** | YUV/GBR 4:4:4 | 10/12/16 | 221 Mbps | 4.263 | 25% / 30% | VFX、动态图形、合成 |
| **Topos 4444 XQ** | YUV/GBR 4:4:4 | 12 | 326 Mbps | 6.288 | 30% / 30% | 高宽容度 HDR、多代制作 |
| **Topos 422 LP**（帧间档） | YUV 4:2:2 | 10 | ≈42–64 Mbps（固定锚 qp72） | —（无码控） | no-alpha | 实时粗剪缓存、冗余素材快速转码 |

> ¹ 参考值取自同母版 ProRes 对标语料（2026-09-21 重标定，v1.9）。LP 为推算参考带：实码率随素材冗余度浮动（P 帧占比越高收益越大，切镜密集段趋近 LT）。
> ² bits-per-pixel 定标值：目标码率随分辨率与帧率线性缩放，即 `码率 ≈ bpp × 宽 × 高 × fps`。档位是"调参目标"而非格式上限——调用方也可用固定 QP（0–95）完全替代档位目标码率，或用字节精确预算（`max_video_bytes` / `max_file_bytes`）做文件大小硬上限。档位声明单一来源：主仓库 `src/shared/codec/topos_profiles.py` / 发布包 `python/topos_codec/topos_profiles.py`（与 `docs/capability_manifest.json` 交叉钉死）。
>
> **v1.9 重标定（2026-09-21）**：422 家族档位由"等画质定标"改为
> "ProRes 容量上限下的默认目标"（同母版实测 ProRes bpp × 0.98 × 0.95）；
> 旧 Proxy 低码率定标保留为独立的 **Net Proxy** 档（网络代理，非 ProRes
> 对标档）。语料落点为同档 ProRes 文件体积的 80–94%，98% 上限由预算
> 接口硬约束。
>
> **v1.10 量化矩阵（2026-09-21）**：422 档位启用 `qmatrix_id=4`
> 「边缘均衡」——低频略粗、高频略细（HF:LF 步长比 ≈0.4），把 flat 矩阵
> 的低频盈余换成边缘精度。同体积实测（487 帧同母版语料）：边缘 PSNR
> 2K Proxy/Standard/HQ +0.66/+0.54/+0.29 dB，4K Proxy/LT +1.01/+0.78 dB；
> 编解码速度不变。宽 ≥3840 时 Standard/HQ 工作点回退 flat（该域实测负
> 收益，Y≈52–60 dB 近透明）。详见
> `docs/topos_edge_jag_diagnosis_2026-09-21.md`。
>
> **v1.11 输出特性（2026-09-21，"V7-R4/R5"）**：帧头 **flags** bit2 =
> *输出去块*（解码端后环 8×8 网格滤波，强度随 qp 自适应、真实边缘由
> 局部梯度阈值保护；spec 见 `src/codec/deblock.h`）——全部 4:2:2 10-bit
> 视频档启用。bit3 = *细化 qp 表*（qp≥64 翻倍率由每 4 qp 放缓至每 6 qp；
> qp≤63 与冻结表逐值一致）——仅 4K Standard/HQ 启用（A/B：+0.53 dB @
> −0.1% 体积；2K/4K proxy 域实测负收益，维持经典表）。特性经编码器
> `reserved[5]` 选载，位流侧落在原保留 flags 位上：旧解码器按保留位
> 规则干净拒读，绝不静默误读。target-size 缩放交付本版暂不过滤。

### 4.2 典型目标码率（由 bpp 定标换算）

| 档位 | 1080p25 | 2160p25 (4K) |
| --- | ---: | ---: |
| Topos 422 Net Proxy | ≈ 15 Mbps | ≈ 60 Mbps |
| Topos 422 Proxy | ≈ 33 Mbps | ≈ 134 Mbps |
| Topos 422 LT | ≈ 70 Mbps | ≈ 280 Mbps |
| Topos 422 | ≈ 109 Mbps | ≈ 438 Mbps |
| Topos 422 HQ | ≈ 163 Mbps | ≈ 653 Mbps |
| Topos 4444 | ≈ 221 Mbps | ≈ 884 Mbps |
| Topos 4444 XQ | ≈ 326 Mbps | ≈ 1304 Mbps |
| Topos 422 LP | ≈ 42–64 Mbps（锚定，无定标） | ≈ 168–256 Mbps |

### 4.3 与 ProRes 家族的映射

| Topos | 对标 ProRes | 码流关系 |
| --- | --- | --- |
| Topos 422 Proxy | Apple ProRes 422 Proxy | 同一 422 profile 的低码率预设 |
| Topos 422 LT | Apple ProRes 422 LT | 同上 |
| Topos 422 | Apple ProRes 422 | 同上（家族默认档） |
| Topos 422 HQ | Apple ProRes 422 HQ | 同上（高码率预设，可跨格式到 444/GBR） |
| Topos 4444 | Apple ProRes 4444 | 独立 4:4:4 profile + Alpha |
| Topos 4444 XQ | Apple ProRes 4444 XQ | 独立 4:4:4 12-bit profile |
| Topos 422 LP | —（同体积带对标 ProRes 422 LT） | 帧间微 GOP（IP-2），非帧内预设 |

> 映射说明定位与用途，不构成码率等价声明：两者参考帧率口径不同（Topos 参考系为 1080p25），但各档中值数据率与 ProRes 对应档位接近。

### 4.4 码流格式演进

| 版本 | 帧头 minor | 新增能力 |
| --- | --- | --- |
| v1.0 | 0 | YUV 4:2:2 10-bit（profile 3） |
| v1.2 | 1 | 12-bit 枚举扩展 |
| v1.3 | 2 | YUV 4:4:4 枚举扩展 |
| v1.4 | 3 | GBR 4:4:4 枚举扩展；profile 5/6 激活 |
| v1.5 | — | qp 域扩展 0–95（帧内各档锚点重定标，ADR-C031/C043） |
| V7-R3 | （熵载体 em 8） | 帧间微 GOP 载体（LP 档，帧头 profile 仍为 3；ADR-C047/C048） |

全部扩展向后兼容：解码器按帧头自描述协商，旧文件永远可读。

### 4.5 命名规则（产品档位识别指南）

全部输出规格选项（导出面板 / Quick Export / CLI / probe 显示 / 容器元数据 label）由**单一档位注册表**派生（N-7），任何入口看到的档位名一致。命名规则总纲：

| 规则 | 内容 | 示例 |
| --- | --- | --- |
| N-1 家族前缀 | 视频线 `Topos`（大写，MOV 容器）；图片线 `toos`（小写，`.toos` 文件） | `Topos 422 HQ` / `toos High 12bit` |
| N-2 视频中间片档 | `Topos 422 <后缀>` / `Topos 4444 <后缀>`：Proxy / LT /（默认档无后缀）/ HQ / 4444 / 4444 XQ / LP | `Topos 422 LT`、`Topos 4444 XQ`、`Topos 422 LP` |
| N-3 RAW 档（规格名） | `Topos RAW <bd>-bit <N>:1`：位深必带 `-bit`、比率必带 `:1`、N ∈ {2,4,6,8,12,16}；模式 ≡ 位深（12-bit = log 制作档、16-bit = linear 归档档，Log/Linear 不进档位名）；视频/图片共用同一张 12 档表与 qp 锚点 | `Topos RAW 12-bit 4:1` |
| N-4 图片整数档 | `toos <Quality> <bd>bit`，Quality ∈ {Low, Medium, High, Ultra}（图片线专属，详见图片白皮书 §5.1） | `toos High 12bit` |
| N-5 图片浮点档 | `toos <Quality> <bd>bit float`（HALF 域；未来 32-bit 档仅换位深数字） | `toos Ultra 16bit float` |
| N-6 图片 RAW 档（显示名/TMET） | `toos RAW <bd>bit <N>:1`——图片家族小写、位深无连字符；规格文档层一律用 N-3 带连字符形式 | `toos RAW 12bit 4:1` |
| N-7 单一注册表 | 全部输出规格选择面由同一份档位注册表派生，任何入口同名（本节首段） | — |
| N-8 档位名不承载的词汇 | Log/Linear、无损、实验性等定位词只进描述文案与文档，不进档位名 | — |

识别指南：**家族前缀**判产品线（`Topos` = MOV 视频 / `toos` = `.toos` 图片文件）；**位深与比率词汇**判能力域（`422` = 4:2:2 色度子采样、`4444`/`444` = 4:4:4 全色度、`RAW <N>:1` = 相机 RAW CFA 压缩档）；**后缀**（Proxy/LT/HQ/XQ/LP 或 Low..Ultra）判质量档。RAW 档比率（N:1）是 CQ 定位标签——体积随内容浮动，不承诺精确比率。视频 RAW 档位（`Topos RAW 12-bit 2:1` … `16-bit 16:1`，容器 tier 8）随视频 RAW 产品化（ADR-C058）落地，规格词与本节一致。

### 4.6 视频 RAW 档（TRAW，v1.9 激活）

视频 RAW 是单一 tier（容器 tier id 8）之上的**位深 × 比率参数化档位族**：
载荷为 Bayer CFA 4 相位平面（R/Gr/Gb/B，各 ceil(W/2)×ceil(H/2)，uint16
满刻度），帧内全 I 编码，qmatrix 冻结 flat（qm0）、full range、matrix=0
identity、primaries 占位 1（真实色彩语义由应用层 RAW 参数/debayer 承载，
不在共享码流域私造 camera-native 枚举）。

| 规格名 | 位深 | Transfer（冻结对） | 比率标签 | qp 锚（12 档与图片线共享） | 载体 |
| --- | --- | --- | --- | --- | --- |
| Topos RAW 12-bit 2:1 … 16:1 | 12 | PWL-log12（LOG0，码 20） | 2/4/6/8/12/16:1 | 45/59/64/66/68/70 | V1 minor 5 扩展代；熵 rANS（V7-R2，默认）或 V1 |
| Topos RAW 16-bit 2:1 … 16:1 | 16 | Linear（码 8） | 同上 | 51/72/76/78/80/82 | V1 minor 6 扩展代；bd≥13 只许宽域熵（rANS 必选） |

> ¹ 比率（N:1）是 CQ 定位标签：名义锚点按 1080p 单帧体积反推（16-bit 容器
> 名义 12.44 MB/帧 ÷ N），实际体积随素材冗余度浮动，不构成比率承诺。
> ² TRAW 与 Alpha 互斥（no-alpha）；色彩元数据冻结（调用方标签不进入码流）；
> RAW→RAW 直出语义（R2）：CFA 相位平面直达编码器，任何像素级处理
> （debayer/调色/几何）都在直出车道之外，保证导出物与源逐位同域。

---

## 5. 色彩与 Alpha

- **色彩元数据随流携带**：primaries / transfer / matrix / range 全部写入帧头（BT.709、BT.2020、PQ、HLG、DCI-P3 等），并写入 `tpcC` 规则与 MOV 容器元数据；应用层可将 SMPTE ST 2086 mastering display 与 MaxCLL/MaxFALL 映射到容器。
- **GBR 直通**：4444/4444 XQ 档支持 GBR 4:4:4（matrix=0），RGB 素材合成不经 YUV 往返。
- **Alpha**：所有档位可选；默认近似模式（mode 2）以 12-bit 内部保真执行，实测码率占比与无损模式几乎相同；编码端执行"Alpha 预算闭环"——按档位声明的预算比例控制 Alpha 码流占比，超限自动收敛并上报统计。

---

## 6. 容器与互操作

- **MOV 派生容器（.mov）**：mux/demux 自包含实现（零 I/O 依赖，I/O 走用户回调）；支持 **FastStart**（元数据前置，网络播放即点即开）；采样索引 O(1)，随机访问与顺序读同速。
- **FFmpeg 生态**：官方补丁向 libavcodec 注册 `libtopos` 解码器（链接 `libtopos_codec`），FFmpeg/ffplay 可直接解码 Topos .mov；构建脚本见 `packaging/ffmpeg/`（发布仓库）。
- **能力清单**：`docs/capability_manifest.json` 机器可读地声明全部档位/格式/版本，供集成方运行时校验（与 native 查询 API 交叉钉死）。
- **音频轨（v1.1）**：可选单音频轨（无损 lpcm / AAC 交付档，2.0/5.1/7.1）；
  编码在应用层（native 只存包）。回放缓存/代理仍为纯视频（ADR-C051 边界）。

---

## 7. 使用方法

### 7.1 构建（无任何外部依赖）

```bash
cmake -S native -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
# 或一键构建+全量测试（Debug/ASan/UBSan/TSan/Fuzz）：
bash native/run_tests.sh
```

### 7.2 C API（节选）

```c
#include "topos_codec.h"

/* 能力协商：显式查询，不支持即返回错误码 */
tc_query_support(3, 0, 10, 2);

/* 帧级编码：plain 模式（qp 直控）或 sized 模式（帧级目标字节数） */
tc_frame_encode(&cfg, planes, &packet, &stats);
tc_frame_encode_sized(&cfg, planes, target_bytes, &packet, &stats);

/* 容器：mux（支持 Alpha 预算）→ FastStart → 随机访问 demux */
tc_mux_create(...); tc_mux_add_packet(...); tc_mux_finish(...);
tc_movie_open(path, &movie); tc_movie_packet(&movie, pts, ...);
```

完整可编译示例：`native/examples/encode_decode.c`（C）与 `native/examples/topos_roundtrip.py`（Python）。

### 7.3 Python 绑定（ctypes，无 CPython 扩展、不挑 Python 版本）

```python
from topos_codec import ToposCodec, ToposMuxFile, ToposMovieFile

codec = ToposCodec()                       # 自动搜索/TOPOS_CODEC_LIB 加载动态库
codec.query_support(3, 0, 10, 2)           # profile/格式/位深/alpha 显式协商

mux = ToposMuxFile(codec, "out.mov", codec.movie_config(width=1920, height=1080))
for i, planes in enumerate(frames):        # planes: 紧凑 u16 平面字节列表
    packet, stats = codec.encode_frame(fc, planes)
    mux.add_packet(packet, i * 1000, 1000)
mux.finish(); mux.close()

movie = ToposMovieFile(codec, "out.mov")
frame = codec.decode(movie.packet(0))      # O(1) 随机访问
```

动态库定位顺序：显式路径 / `TOPOS_CODEC_LIB` 环境变量 → 包内 `lib/` 目录 → 仓库构建目录。发布包把 dylib/so/dll 放入 `python/topos_codec/lib/` 即可免配置使用；`pip install ./python` 后作为 `topos_codec` 包导入。

### 7.4 CLI 工具

| 工具 | 用途 |
| --- | --- |
| `topos_encoder_cli` / `topos_decoder_cli` | 命令行编码/解码 |
| `topos_probe_cli` | 码流/容器探测 |
| `topos_inspect` | 包级诊断（随库常驻） |
| `topos_quality` | 画质/码率/速度测量与 QM 调优（`perf quick` 一键基准） |

### 7.5 高层编码封装

`topos_encoder.ToposVideoEncoder`（随发布包附带）提供应用级闭环：档位→bpp→帧级 sized 码控、Alpha 预算闭环、临时文件原子替换、切片线程策略、色彩元数据（SDR/HDR10/HLG/DCI-P3）写入。

---

## 8. 性能

以下为同机实测（Apple M1 Pro 级 8 核，release 构建 `-O3`，p50 口径；完整数据与协议见 `docs/bench_*_2026-08-31.md`）。

**解码**（完整产品路径，含容器读取 + 解码 + 平面装配，8 线程）：

| 分辨率 | 每帧耗时 | 吞吐 |
| --- | ---: | ---: |
| 1080p | 4.93 ms | ≈ 203 fps |
| 4K (3840×2160) | 13.40 ms | ≈ 75 fps |

全帧内 + O(1) 索引：随机访问与顺序读性能一致；4→8 线程扩展增益 −36%。

**编码**（qp20，plain = 直控；sized = 帧级目标码控，含精确 qp 搜索，平均 5 次探测）：

| 配置 | plain | sized（码控） |
| --- | ---: | ---: |
| 1080p | 26.4 ms/帧（≈ 38 fps） | 50.2 ms/帧（≈ 20 fps） |
| 4K | 90.2 ms/帧（≈ 11 fps） | 180.4 ms/帧（≈ 5.5 fps） |

与 FFmpeg 软件编码 ProRes/DNxHR 的同机对比（同视觉质量口径）：Topos 编码速度约为其 **1.4–3.4×**，且码控输出逐帧确定性可复现。编码器 SIMD 累计优化轨迹：1080p plain 2.58×、sized 4.10×（详见 `docs/bench_topos_encode_2026-08-31.md`）。

> 性能随 CPU/内存子系统差异浮动，建议用随库 `topos_quality perf quick` 在目标硬件复测。

---

## 9. 质量保证

Topos 的工程门禁与格式冻结绑定：

- **单元测试**：31 个原生测试（ABI/位流/变换/量化/容器/鲁棒性/OOM/压力）；
- **Golden 一致性**：7 组冻结码流（变换 1380 条、位流 56 条、全链路编码、容器双冻结、profile 矩阵、VLC 扩展）——任何改动破坏既有码流即刻失败；
- **Fuzz**：5 个入口（primitives/bitstream/codec/mov/encode）+ 确定性回放 driver + seed corpus 自检门；
- **ABI 钉死**：公共符号清单门禁（`public_symbols_v1.txt`），C++ 兼容测试；
- **CI 三平台矩阵**：Linux (GCC) / macOS (universal) / Windows (MSVC required + MinGW)。

---

## 10. 许可

Topos Video Codec 以 **Apache License 2.0** 开源发布（见 `LICENSE`）。选择 Apache-2.0 的原因：显式**专利授予**条款对编解码技术尤为重要——贡献者与用户双向获得专利授权与终止保护，这是 MIT/BSD 缺失的部分；同时保持宽松商用与 GPL(v3) 单向兼容。

---

## 11. 规范文档索引

| 文档 | 内容 |
| --- | --- |
| `docs/bitstream_spec_v1.md` / `docs/bitstream_spec_v2.md` | 码流规范（V1 Rice / V2 canonical VLC） |
| `docs/container_spec_v1.md` | MOV 派生容器规范 |
| `docs/SDK.md` | 独立集成方 SDK 指南 |
| `docs/capability_manifest.json` | 机器可读能力清单 |
| `docs/ADR-INDEX.md`（ADR-C001…C028） | 全部架构决策记录 |
| `docs/benchmark_protocol.md` | 性能测量协议 |

---

*Topos Video Codec · Copyright (c) 2026 Topos Color 项目 · Apache License 2.0*

*商标归属：Apple、ProRes、QuickTime 为 Apple Inc. 商标；DNxHR 为 Avid Technology, Inc. 商标；DaVinci Resolve 为 Blackmagic Design Pty Ltd. 商标。上述名称仅用于指称与对比相应技术（正当使用），不构成任何关联或背书。*
