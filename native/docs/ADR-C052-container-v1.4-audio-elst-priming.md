# ADR-C052：容器 v1.4——音频 elst 最小子集与 AAC priming 校准

日期：2026-09-20 · 状态：**已采纳**
关联：ADR-C051（v1.1 音频轨）、container_spec_v1.md §10/附录 A（本次解冻
"edit list 非目标"的音频 trak 单条目子集 + A.5 规范化）、
`topos_container_audio_optimization_plan_2026-09-20.md` §2（M-B1）、
互操作报告 `topos_audio_interop_report_2026-09-20.md`（M-B0 实证）

## 1. 背景

v1.1 的 AAC（mp4a）档"原样存包"没有 edit list：解码器把 AAC-LC 编码器
固有的序曲延迟（priming，首包承载历史依赖的静音前缀）当作正常音频输出，
外部播放器播放 AAC 档时音频整体后移。行业标准做法（ffmpeg movenc、
QuickTime）是为音频 trak 写最小 `edts/elst`（media_time = priming），
解码侧据此裁剪。

计划期的两个前提假设（优化计划 §0.1/§2.2）在实施中被证伪/修正：

1. ~~"编码器自报 1024 与端到端实测 2048 差 1024，差额来自解码侧管线
   延迟，elst 须写端到端值"~~——FFT 全长互相关精确校准（2026-09-20，
   PyAV 17.0.1 + ffmpeg 8.1，多正弦母带 norm-xc 0.9996）证明：**无 elst
   端到端延迟恰为 1024**，等于编码器自报值；此前"2048"是弱相关窗口法对
   纯正弦的周期歧义伪影（440Hz 周期 109.09 采样，窗口搜索峰值漂移）。
2. ~~"校准值 X 须在 {0, priming_enc, 2×priming_enc} 中端到端搜索取
   |L(X)| ≤ 1"~~——实际 X = priming_enc（1024）即得**零残差**（lag=0
   精确成立）。

## 2. 决定

**D1（elst 最小子集）**：仅音频 trak、仅 mp4a 档、单条目 v0 elst
（segment_duration=movie timescale 轨时长向上取整、media_time=priming、
media_rate=0x00010000）。lpcm 采样精确恒不写；视频 trak 出现 edts →
MALFORMED（§3 "pts 无空洞约束"不变）。新 API `tc_mux_set_audio_priming`
（finish 前至多一次；仅 mp4a；samples ∈ (0, 已写总采样数)），demux 读回
经 `topos_audio_track_info.reserved[0]`（priming_samples，0=无 elst）；
`tc_movie_read_audio` 语义不变（容器不代应用做时间轴裁剪）。

**D2（priming 取值 = 编码器自报 priming_enc，运行时实测 + 常量兜底）**：
导出时 `_aac_encode_pcm` 顺序 pts 喂帧，首包 pts 的负值即 priming_enc，
直接作为 elst media_time；未捕获时兜底常量 `TOPOS_AAC_E2E_PRIMING_SAMPLES
= 1024`。理由：

- priming 是**流固有属性**（由编码器后端决定：native aac=1024，
  AudioToolbox aac_at=2112），不是解码器属性——写自报值即行业实践
  （ffmpeg movenc 对 native aac 同样写 1024；aac_at 产物写 2112）。
- 校准实证：elst=1024 时 ffmpeg 解码残差 = 0（skip_samples 精确等于
  media_time，FFT 位移恰为 1024/2048/2016 逐项吻合）。
- "解码器管线延迟"不是文件属性，无法由容器补偿——把计划期的"端到端
  校准"修正为"流内校准"（写 priming_enc），解码器自身的管线延迟由各
  解码器框架自行处理（这正是 elst 语义的设计本意）。

**D3（mp4a 声样描述互操作修正，M-B0 发现并入本版）**：M-B0 互操作矩阵
实测发现 v1.1 的 mp4a stsd 有两处编码缺陷（ffmpeg/VLC 容错、CoreAudio/
AVFoundation 拒绝）：

1. esds 描述符长度写成固定 u32 BE（规范为 ISO 14496-1 可变长 7-bit 续位
   编码，严格解析器读出长度 0），且 DSI/SL 被嵌进 DecoderConfigDescriptor
   内部而非兄弟节点；
2. esds 直挂 stsd 缺 QuickTime `wave` 包裹（`wave{frma('mp4a');'mp4a'12B
   空原子;esds;8B null}`）与 `chan` 声明。

v1.4 起写规范形态（与 ffmpeg movenc mov 产物字节同构）：规范可变长
描述符 + 同级链 + wave 包裹 + chan + compressionID=0xFFFE（V1 惯例）。
读侧同步：按规范长度严格解析（畸形 → MALFORMED），容忍直挂 esds 旧形态。
lpcm 档声样未动；样本表与 moov 布局不变；纯视频 golden 不受影响。

## 3. 备选方案（否决理由）

- **B1：elst media_time 写端到端校准值（2048）**——基于测量伪影；即便
  真实存在解码侧延迟，它因播放器而异（写死一个解码器的延迟会让其它
  解码器过裁剪提前 1024 采样 ≈21ms）。否决。
- **B2：应用层解码后裁剪（读 sidecar priming 元数据）**——容器外播放器
  （QuickTime/VLC/NLE）拿不到私有元数据，等于没修。否决。
- **B3：维持无 elst（文档化 AAC 档 ~21ms 偏移）**——逼近人眼口型同步
  可感知阈值（±45ms）的一半，且修复成本低、行业标准路径。否决。
- **B4（M-B0 备选）：给 esds 加 wave 包裹但不修描述符编码**——实测
  AVFoundation 对两种缺陷都是拒绝行为，单修其一不够；一并修（D3）。

## 4. 后果

- AAC 交付档 A/V 同步对齐行业标准：ffmpeg 家族端到端残差 0（≤1 采样
  断言钉死，PyAV/ffmpeg 升级漂移时测试先红）；忽略 elst 的播放器
  （部分老 Android/硬解）维持无裁剪行为（≈21ms@48k，K1 口径不变）。
- 规范升版 v1.4（§10 解冻 + 附录 A.5 + A.2 修正）；纯视频文件与既有
  v1.1/v1.3 音频文件读取行为不变；音频 golden 冻结（M-B6）落在本版之上。
- 绑定层新符号 `tc_mux_set_audio_priming`（fail-fast 要求新 dylib，
  沿用 v1.1 四符号纪律）。
- 互操作矩阵 AAC 组带 elst 复跑：18 PASS / 0 FAIL（CoreAudio 立体声
  组合在 elst 裁剪后从 SOFT-FAIL 转 PASS）。

## 5. 实施证据

| 项 | 值 |
|---|---|
| 无 elst 端到端延迟（FFT 精测） | 1024 采样（= priming_enc） |
| elst=1024 残差 | 0（FFT lag = 0，norm-xc 0.9996） |
| skip_samples vs media_time | 精确相等（1024/2048/2016 三组实测） |
| 互操作复跑（AAC 组，带 elst） | 18 PASS / 0 FAIL |
| C 单测 | elst roundtrip / API 校验 / 6 组畸形字节手术拒绝 |
