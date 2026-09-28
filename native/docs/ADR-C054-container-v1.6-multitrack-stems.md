# ADR-C054：容器 v1.6——多音轨 / stems 角色轨

日期：2026-09-20 · 状态：**已采纳（容器/绑定/探测/导出侧全量实施；
导出侧记录见 §4）**
关联：ADR-C051/C052（音频 v1.1/v1.4）、ADR-C055（tmcd 轨序规则——本 ADR
为其 N 轨推广）、container_spec_v1.md 附录 A.3/A.7、计划 §2.5/§3 M-B8

## 1. 背景

FCP "Roles as Multitrack QuickTime"：按角色（对白/音乐/音效）每角色一条
离散轨，每轨独立 mono/stereo/f32 混合格式；广播交付 8 mono 轨常见
（计划 §0.5）。v1.1 的"单音轨 + reserved 槽位声明"无法表达 N 轨
（槽位仅剩 [6..7]）且每轨格式需独立。

计划风险条款（§6）钉死两步走：**先纯重构（单轨行为逐字节不变，golden +
全量回归钉死）再叠加多轨**；旧单轨 API 保留为轨 0 糖衣。

## 2. 决定

**D1（两步走已执行）**：

- 第一步（`2b3afc66`）：mux 散装 a_* 字段收纳为 `audio_track_state
  a_trk[16] + n_a`（恒 {0,1}），golden_mov_v1 + golden_mov_audio 字节
  冻结全绿——纯重构零行为变更。
- 第二步（本 ADR 主体）：多轨能力叠加。

**D2（逐轨声明 API，弃槽位扩容）**：`tc_mux_add_audio_track(cfg)` 逐轨
声明（五元组 + sample_format + name），格式校验与 reserved 槽位**共用
同一函数**（`audio_fields_validate`——无第二套标准）；reserved 槽位声明
的轨恒为轨 0，可与逐轨声明混用。按轨操作 API
`tc_mux_set_audio_track_asc/priming`、`tc_mux_add_audio_to(track, …)`。
v1.1 单轨 API 语义不变 = 轨 0（`add_audio` 无声明仍 STATE——v1.1 测试
字节级兼容）。轨数上限 16（LIMIT_EXCEEDED）。

**D3（轨序与命名）**：video=1、audio=2..N+1（声明序）、tmcd=N+2 恒末轨
（ADR-C055 规则推广，next_track_id 联动）。轨名走 QuickTime 标准通道：
trak/udta/©nam（u16 长度 + UTF-8 + u16 0 终结，movenc 同构）+ hdlr 名
同值；Resolve/Premiere 据此显示。reader 对越界轨名 fail-safe（装饰性
元数据不拒文件）。mono 布局解冻（1ch↔mono；chan tag 0x00640001 =
ffmpeg MOV_CH_LAYOUT_MONO 同值），写侧 chan 与读侧 chan/推导三处同启。

**D4（demux 多轨 + 兼容视图）**：soun trak 声明序分派（>16 →
MALFORMED）；逐轨表级校验 = v1.1 singleton 规则逐轨适用。查询 API
`tc_movie_audio_track_count` / `tc_movie_audio_info_at` /
`tc_movie_read_audio_at`；**旧 `tc_movie_audio_info/read_audio` 恒轨 0**，
单轨文件新旧 API 读出逐字节一致（专项测试钉死）。
`topos_audio_track_info` 增 `name[64]`（struct_size/abi 镜像纪律不变）。

**D5（faststart）**：逐轨重定位，轨内 chunk 顺序 = 声明序 = 布局序；
tmcd track_id 联动（2+N）。

## 3. 后果

- stems 容器面就绪：三轨混合格式（f32 立体声 Music + s16 mono Dialogue
  + mp4a 5.1 Effects）mux→demux 逐字节 roundtrip、轨名读写一致、
  ffprobe 识别 3 轨/各自声道格式/轨名（oracle 实测）。
- 绑定层 `ToposAudioTrackConfig` / `add_audio_track` /
  `read_audio_samples_at` 等全量落位；probe 元数据 `audio_tracks`
  逐轨元组（旧 `has_audio/channels/…` 恒首轨视图，消费方零破坏）。
- 兼容：无音轨/单轨文件与 v1.5 逐字节一致（goldens 不动，89/89）。

## 4. 导出侧（后续批次，已实施）

容器/绑定/探测层落地后的导出侧收口（本节即计划 §3 M-B8 任务 4 的
决策与验收记录）：

- **轨位语义**：轨 0 = 主混音（既有 `audio_*` 配置、reserved 槽位声明，
  旧 reader 兼容视图不变），stems 追加轨 1..N 按声明序挂在轨 0 之后
  ——单轨导出路径逐字节回归（M-A6 套件不变），旧播放器仍听到混音。
- **声明配置**：`TimelineExportConfig.audio_stems`（`AudioStemExportTrack`
  元组：stem_group / 轨名 / codec / 声道 / 布局 / 码率）→ 交付作业
  `AudioConfig.stems`（`StemTrackConfig` 快照，序列化兼容旧 schema）
  → 编码器 `add_audio_track_stream` / `write_audio_track_samples`。
  校验 fail-fast 于配置层（空组名/重复组/声道域）；空角色组在声明前
  剔除（容器"声明即须有样本"，finish fail-fast）；编码器不支持/
  声明失败整体降级单轨（all-or-nothing，部分轨丢失属静默交付缺陷）。
- **分组渲染**：`_render_audio_master` 增 `only_track_ids` /
  `channels_override`——同一解码/混音/Bus/Master 节点链按角色过滤
  （等价 solo 该组；被排除轨贡献恒零）；loudness 归一仅主混音做
  （normalize 增益按混合测量，对角色轨无意义）。母带渲染仍在导出
  音频线程，写入编码器串行（mux 单线程语义不变）。
- **UI**：Deliver 音频节增 "Stems 多音轨" 开关 + 按序列检测的角色组行
  （每轨独立 codec/声道映射）；仅 topos MOV 档可见（`_on_codec_changed`
  门控）。
- **验收（e2e）**：双角色（对白+音乐）时间线 → 分组母带 → 容器三轨：
  master == Σ(stems)（容差 1e-5）、逐轨读回逐位一致（f32 直通零量化）、
  轨名 ffprobe handler_name 识别；fail-fast 四态（先于主混音声明/空轨
  名/越界写入/首帧后声明）；单轨回归（`tests/media/
  test_topos_stems_export.py` 8 用例）。Resolve/Premiere 多轨导入呈现
  归入手动清单（M-B0 矩阵扩展，与容器层同条目）。
