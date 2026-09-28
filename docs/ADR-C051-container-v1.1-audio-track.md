# ADR-C051：容器 v1.1 音频轨——解冻"音频非目标"的决策与边界

日期：2026-09-19 · 状态：**已采纳**
关联：ADR-C005（阶段 5 容器）、ADR-C008（阶段 8 导出集成，"回放音频恒取
master"语义不变）、container_spec_v1.md §10（本次解冻的"音频非目标"条目）、
`docs/codec/topos_container_audio_v1_1_plan_2026-09-19.md`（实施计划，验收
实测数字回填于该文档）

## 1. 背景

container_spec §10 曾冻结"音频轨 V2.0 不支持"：Topos MOV 是纯视频中间片，
时间线导出丢音频，交付需 sidecar 音轨或宿主二次 mux。该折衷的代价随导出
链成熟而放大：用户可感知的交付缺口（preflight `topos.video_only` 警告即此
产生的补偿 UX）、以及"中间片也要二次封装"的流程摩擦。

项目处于开发期（2026-09-19 用户确认）：**无旧视频/旧项目兼容负担**，无需
为存量文件保留任何兼容性脚手架。

## 2. 决定

**D1（解冻范围）**：仅在 **时间线导出** 路径为 topos 容器增加音频轨
（mux + demux + 规范 + 绑定 + 导出接线）。渲染缓存、素材代理**维持纯视频**
——ADR-C008 的"回放音频始终取 master"语义不变（本期非目标，见 §4）。

**D2（编码选择）**：PCM（MOV `lpcm` 声样描述，WAV 的容器等价物）为默认
无损档（16/24/32-bit，大端 'twos' 语义）；AAC（`mp4a` + `esds`）为可选交
付档。**AAC 编码在应用层（PyAV）完成，native 只存包**——C 层零编码器
依赖。AudioSpecificConfig 由应用层经 `tc_mux_set_audio_asc` 声明（首个
add_audio 前恰好一次），esds 原样封装。

**D3（声道）**：2.0 / 5.1 / 7.1 三档（QuickTime `chan` 原子声明布局；
CoreAudio layout tag 0x00650002 / 0x008C0006 / 0x008E0008）。native 支持任意
channel_count，首期暴露三档。

**D4（声明方式）**：音频格式并入 `topos_movie_config`（创建时一次声明，
不设 setter 状态机）。实现上复用 `reserved[0..4]` 槽位承载五元组
{codec, sample_rate, channel_count, channel_layout, bits_per_sample}：
sizeof 稳定 → **零 ABI 变更**，全 0 = 无音轨 → 未显式声明的既有调用方
（topos_encoder / proxy_generator）行为逐字节不变，golden mov 零漂移。
比计划初稿的"尾部追加 topos_audio_format 结构"更优（那个形态会使 Python
ctypes 镜像的 struct_size 与 C 侧失配，需同步改 3 处调用方并制造
C/Python 不同步窗口）。

**D5（样本组织）**：音频按 ~1024 采样帧为一个"样本"（chunk；AAC 天然
1024，PCM 人为分帧），与视频帧交错追加进**同一个 mdat**（ISO-BMFF 允许；
本库规范恒单 mdat——不引入多 mdat 形态），视频/音频 stco 独立记账，视频
chunk 偏移不受影响。音频轨恒为 moov 第二 trak（track_id=2，自然布局：
TPIC 嗅探看第一轨 stsd 首项，保持不变）。

**D6（fail-fast，无兼容垫片）**：绑定层音频符号按新 dylib 唯一基线直配，
缺符号 AttributeError → 显式报错（不做旧库符号探测降级）。声明音频但
零样本 → `tc_mux_finish` 显式拒绝（防"半声明"产物）。

## 3. 原子布局（规范性，详见 container_spec_v1.1 附录 A）

```
moov
├─ mvhd（duration = max(视频, 音频) ms；next_track_id = 3）
├─ trak #1（视频，v1 布局冻结不变）
└─ trak #2（音频）
   ├─ tkhd（flags=0x7, track_id=2, volume=0x0100, 无几何, duration=采样帧数）
   ├─ mdia
   │  ├─ mdhd（timescale = 采样率, duration = 采样帧数, lang und）
   │  ├─ hdlr（'soun', "Topos Audio Handler"）
   │  └─ minf
   │     ├─ smhd（balance=0）
   │     ├─ hdlr（'dhlr', "Data Handler"）
   │     ├─ dinf/dref('url ')（自包含）
   │     └─ stbl
   │        ├─ stsd
   │        │  ├─ 'lpcm'（SoundDescription v0 + 20B lpcm 扩展
   │        │  │        formatFlags=0xE（signed|big-endian|packed）
   │        │  │        + 'chan' 子原子（布局 tag））
   │        │  └─ 'mp4a'（SoundDescriptionV1 + 'esds'（ES→DCD(0x40)
   │        │           →DSI(ASC 原样)→SL，4 字节扩展长度））
   │        ├─ stts（等 dur RLE）
   │        ├─ stsc（单条：1 sample/chunk）
   │        ├─ stsz（逐 chunk 精确尺寸——lpcm 末 chunk 可短、AAC 变长）
   │        └─ stco|co64（轨内独立判定）
```

## 4. 边界（本期非目标）

- **回放/渲染缓存/代理不取 topos 音频**：ADR-C008 语义不变；
  `ToposMediaSource.read_audio_samples` 的 mp4a 档返回 None（容器只存包，
  本层不解码；lpcm 档可读——服务校验与工具链）。
- **多音轨 / 音频编辑（增益、混音、均衡）**：不做。
- **GOP 分段并行 + 音频**：分段模式下 `add_audio_stream` 显式拒绝
  （段 mux 独立，音频注入待后续 ADR）。
- **edit list 音画对齐**：首版音频从 0 开始（无 elst）；导出侧按视频时长
  精确推导 PCM 长度，e2e 断言音视频时长差 < 1ms。

## 5. 后果

- (+) 时间线导出（normal + direct delivery）不再丢音频；交付链路少一次
  二次封装。
- (+) 零 ABI 变更（D4 reserved 复用）；纯视频文件**解码结果**不变
  （golden mov 逐字节钉死 + test_mov 全量回归）。
- (−) `topos.video_only` preflight 语义变更：受支持编解码不再告警；
  "未选音频但有音频内容"与"编解码不受支持"两个分支保留提示。
- (−) mp3/flac/opus 等编解码在 topos 档不可用（preflight 提示 +
  add_audio_stream 显式拒绝）。
- 中性：`read_audio_samples`（C-18 预留桩）真实现——lpcm 可读、mp4a
  返回 None（不解码）。
