# Topos Video Codec — MOV Container Specification v1

- 版本：**v1.9（规范性）**（v1.4=elst/priming+全家族采样率+float32，
  v1.5=tmcd 时间码轨，v1.6=多音轨/stems，v1.7=tpcD 电影元数据，
  v1.8=tpcD tier 域扩展至 7（Topos 422 LP 帧间档产品化，ADR-C056），
  v1.9=tpcD tier 域扩展至 8（Topos RAW 产品档 TC_TIER_RAW，ADR-C058）
  ——详见 §4 与 ADR-C052/055/054/056）。v1 实现于阶段 5，由 `golden_mov_v1.bin`（字节级
  冻结）与四配置门禁冻结（ADR-C005）；**v1.1（2026-08-30，R3/ADR-C014）追加
  可选子 atom `tpcB`（§4）**——纯尾部追加，旧 reader 按未知子 atom 跳过，
  golden 与既有 writer 产物字节不变；**v1.2（2026-08-30，R5/ADR-C021）冻结
  正式支持矩阵（§3.1/§5/§7 增补）与产品非目标（§10）**——纯文档收口，
  字节布局零变更，golden 不动；**v1.3（2026-09-19，ADR-C051）音频轨**
  （附录 A）——解冻 §10 音频非目标：moov 可带第二 trak（soun，单轨
  lpcm/mp4a），纯视频文件字节布局不变（golden 钉死），§3.1 支持矩阵
  同步增补；**v1.4（2026-09-20，ADR-C052）音频 elst 最小子集 + mp4a 声样
  描述互操作修正**（附录 A）——解冻 §10 edit list 的音频 trak 单条目
  `edts/elst`（AAC priming 对齐）；mp4a stsd 改为 QuickTime 规范形态
  （wave 包裹 + ISO 14496-1 可变长描述符 + chan）；**v1.5（2026-09-20，
  ADR-C055）tmcd 时间码轨**（附录 A.6）——moov 可带末位 tmcd trak（单样本
  4B 起始帧计数），视频 trak 附 `tref{'tmcd'}`；字段语义以 QT 规范为准、
  字节布局以 `ffmpeg -write_tmcd` 产物对拍为准；无时码文件字节布局不变。
  **v1.6（2026-09-20，ADR-C054）多音轨 / stems 角色轨**（附录 A.7）——
  音频 trak ≤ 16（声明序 = 轨 id 2..N+1），逐轨独立格式与轨名
  （trak/udta/©nam）；mono 布局解冻；tmcd 顺延恒末轨；v1.1 单轨族 API
  语义不变（轨 0 视图），v1.1 文件与新 API 读出完全一致。
  **v1.7（2026-09-21）电影元数据 `tpcD`**（§4）——stsd entry 尾部追加可选
  子 atom：导出档位（TC_TIER_*）+ 厂商标识（vendor/label UTF-8 串），声明
  经 `tc_mux_set_movie_meta`（不设置则不写，golden 与既有 writer 产物字节
  不变）；读侧档位经 `tc_movie_info.reserved[1]` 回读（reserved 槽位复用，
  零 ABI 变更）；FastStart 重建原样保留。
- 关联：`bitstream_spec_v1.md`（elementary packet，容器不重新解释其任何字节）；
  `ADR-C005-stage5-container.md`（路线决策与 spike 证据）；
  `ADR-C014-r3-alpha-budget.md`（tpcB 语义与策略）；
  `ADR-C021-r5-mov-metadata.md`（R5 支持矩阵与非目标决策）；
  `ADR-C051-container-v1.1-audio-track.md`（音频轨解冻决策与边界）；
  `ADR-C052-container-v1.4-audio-elst-priming.md`（elst/priming 校准决策）；
  `ADR-C055-container-v1.5-tmcd-timecode.md`（tmcd 轨决策与 ffmpeg 差异表）。
- 状态词约定：**必须**（读实现违反 = 不合格）；**冻结**（变更需提升本规范版本）。

## 1. 范围

定义 **Topos MOV**：承载 Topos elementary frame packet 序列的 QuickTime 结构文件。
单视频轨、全 intra、每 sample 恰为一个完整 TPIC packet。容器层**不**提供 slice 级
concealment（那是 packet 内部机制，spec §9）；容器层错误一律硬错误。

自研 reader/writer 的合规性由三方 oracle 交叉验收（§9）：独立 atom parser
（纯 struct 实现）、PyAV/FFmpeg **demux 级**（逐包 count/size/duration 对拍，
见 §9——"未知 codec 不崩溃"不构成互操作证据）与 ffprobe 包级断言。
本库 reader 只承诺读取本规范定义的子集，遇外来 MOV（非 `TPIC` entry）返回
`TC_ERR_MALFORMED` 而非尝试解码。

## 2. 顶层布局

```
标准布局:      ftyp(20) mdat moov
FastStart布局: ftyp(20) moov mdat
```

- `ftyp`（20 字节，冻结）：major `qt  `，minor `0x00000200`，compatible
  `[qt  ]`（与 movenc 输出一致，阶段 5 spike 验证被 FFmpeg 接受）。
- `mdat` 头必须用 64 位 size 形式：`[size=1(4B)][mdat(4B)][u64 实际长度(8B)]`
  共 **16 字节**。流式写出时先写占位 0，finish 时经 `seek_write` 回填实际长度
  （写 moov 前记录 payload 终点）。写入器要求 sink 可 seek。
- `moov` 生成时间字段（mvhd/tkhd 的 creation/modification time）必须写 **0**
  （确定性要求，ADR-C005 C-40；golden 冻结前置条件）。
- 未知顶层 atom 必须可跳过（reader 行为）；writer 不产生本文未列出的顶层 atom。

## 3. moov 结构（writer 产出形态，冻结）

```
moov
  mvhd (v0, 108B)              timescale=1000, duration=按轨换算, 速率矩阵单位阵
  trak
    tkhd (v0, 92B)             flags=0x000007(Enabled|InMovie|InPreview), 尺寸=可见宽高
    mdia
      mdhd (v0, 32B)           timescale=轨 timescale, duration=Σ dur ticks
      hdlr (52B)               handler='vide', name="Topos Video Handler\0"
      minf
        vmhd (v0, 20B)         flags=1
        hdlr (45B)             handler='dhlr', name="Data Handler\0"
        dinf > dref(12B) > url (12B, flags=1 自包含)
        stbl
          stsd (v0)            entry_count=1, 单一 TPIC entry（§4）
          stts (v0)            等间隔 run 合并（CFR 通常 1 条）
          stss (v0)            全部 sample（全 intra；显式列出便于第三方 reader）
          stsc (v0)            单条: first_chunk=1, samples_per_chunk=1, id=1
          stsz (v0, non-const) 每 sample 精确字节数
          stco | co64 (v0)     每 chunk 一条偏移（每 sample 一 chunk）
```

- **stco/co64 选择规则**（冻结）：finish 时若全部 chunk 偏移 ≤ `0xFFFFFFFF`
  写 stco，否则整体写 co64（混合非法；reader 两者皆收）。
- tkhd 宽高为 **visible** 尺寸（16.16 定点）；编码尺寸只存在于 packet 内部头。
- 不写 edts/elst（无 edit list，PTS 直接来自 stts）。**pts 无空洞约束**
  （冻结）：`tc_mux_add_packet` 要求首帧 `pts=0`、后续 `pts == 前帧
  pts+dur`（无 gap、无重叠）——无 elst 布局下 gap 无法表达，违反 →
  `TC_ERR_INVALID_ARGUMENT`；VFR 通过逐帧可变 `dur` 表达（stts 多 run）。

### 3.1 支持矩阵（v1.2 冻结——writer 产出 / reader 接受 / reader 忽略 / reader 拒绝）

| 层级 | atom | writer | reader |
| --- | --- | --- | --- |
| 顶层 | `ftyp` | 恒写（§2 冻结形态） | 必须存在；首个 atom |
| 顶层 | `mdat` | 恒写（64 位头） | 必须存在；截断收敛见 §7.2a |
| 顶层 | `moov` | 恒写（§3 形态） | 必须存在；**恰好一个**，重复 → MALFORMED |
| 顶层 | 未知 | 不产生 | **跳过**（含文件尾追加的未知 atom） |
| moov 子 | `trak` | 恰一个视频（v1.6 起：可另带至多 16 个音频 trak，附录 A.7，声明序；v1.5 起：可另带一个 tmcd trak，附录 A.6，恒末轨） | 视频恰一个（多视频 → MALFORMED）；soun trak ≤ 16（超限 → MALFORMED；未知子类型 trak → MALFORMED）；tmcd trak ≤ 1（重复 → MALFORMED，附录 A.6） |
| moov 子 | `mvhd` | 恒写 | **忽略**（duration 不取 mvhd，由 stts 重算） |
| trak/mdia/minf 子 | `tkhd`/`mdhd`/`hdlr`/`minf`… | 恒写 | mdhd 取 timescale、hdlr 校验 vide、minf 下钻；**tkhd 忽略** |
| stbl 子 | `stsd/stts/stsz/stsc/stco|co64/stss` | 恒写（stss/stsc 帧数>0 时） | 解析（§5 输入域） |
| stbl 子 | 未知 | 不产生 | 跳过 |
| stsd entry 子 | `tpcC` | 恒写 | **必需**（CRC 校验；色彩/SAR 等唯一权威源） |
| stsd entry 子 | `tpcB` | 可选（v1.1） | 可选；出现即严格校验（§4） |
| stsd entry 子 | `colr`/`pasp` | 恒写 / sar≠1/1 时写 | **忽略**（第三方互操作通道；读侧一律以 tpcC 为准——pasp/colr 被篡改不影响读取结果） |
| stsd entry 子 | 未知（含 `fiel` 等） | 不产生 | 跳过 |

**FastStart 重写规则**（冻结）：`tc_movie_faststart` 重建 `ftyp+moov+mdat`，
按**新**偏移重新适用 stco/co64 选择规则——小文件从手工 co64 输入降级为
stco 输出是合法结果；tpcB 原样保留；已是 FastStart → 幂等空输出。

## 4. TPIC sample entry（冻结）

ISO VideoSampleEntry（78 字节 body）+ 子 atom，字段：

| 字段 | 值 |
| --- | --- |
| format | `TPIC` |
| data_reference_index | 1 |
| width / height | 可见宽 / 高（u16） |
| horiz/vertresolution | 0x00480000 (72 dpi) |
| frame_count | 1 |
| compressorname | Pascal 串 `"Topos Video Codec 2.0"`（长度 22，32 字段填满） |
| depth | 0x0018 (24) |
| pre_defined | 0xFFFF |

子 atom 顺序（冻结）：`colr`、`pasp`（sar≠1/1 时）、`tpcC`、`tpcB`（v1.1
追加，可选——仅 `tc_mux_set_alpha_budget` 设置后写入）、`tpcD`（v1.7
追加，可选——仅 `tc_mux_set_movie_meta` 设置后写入）。

- `colr`（18B）：colour_type `nclc`，primaries/transfer/matrix 来自轨配置，
  range 无字段（QuickTime nclc 无 range 位；full-range 语义以 tpcC 为准）。
- `pasp`（16B）：sar_num/sar_den。
- `tpcC`（轨级配置，**二进制**）：

```
  0  4B  magic 'TPCC'
  4  2B  version = 1 (u16be)
  6  1B  profile                （含义同 frame header §4.1）
  7  1B  pixel_format             （0=4:2:2；1=4:4:4 v1.3；2=GBR v1.4，
                                    R4.2/R4.3 枚举扩展）
  8  1B  bit_depth
  9  1B  qmatrix_id
 10  1B  qp_base
 11  1B  alpha_mode
 12  1B  alpha_bit_depth
 13  1B  flags  bit0=alpha_premultiplied bit1=full_range 其余 0
 14  1B  color_primaries
 15  1B  color_transfer
 16  1B  color_matrix
 17  1B  chroma_siting
 18  2B  sar_num (u16be)
 20  2B  sar_den (u16be)
 22  2B  reserved = 0
 24  4B  crc32 (IEEE, 覆盖字节 0..23)
  -- 共 28 字节
```

- `tpcB`（Alpha 预算元数据，v1.1 追加，**二进制**，可选）：

```
  0  4B  magic 'TPCB'
  4  2B  version = 1 (u16be)
  6  1B  alpha_mode            （与 tpcC 冗余——读侧一致性校验）
  7  1B  alpha_bit_depth       （同上）
  8  2B  target_ratio_bp       （目标比例 ×10000；0xFFFF=未声明）
 10  2B  actual_ratio_bp       （文件级实际 alpha_payload/color_payload ×10000；
                                可 >10000——u16 饱和 65535bp）
 12  2B  max_abs_error         （跨帧最大 |源−近似|，16-bit 域；mode1 恒 0）
 14  2B  flags  bit0=overrun(超硬上限交付) bit1=authorized(经授权)
                bit2=adapted(位深经 12→10→8 自适应)
                bit3=ratio_saturated(actual_ratio_bp 钳到 65535，v1.3
                定义——真实比例 ≥6.5535 与恰 6.5535 可区分；旧 reader 按
                未知位忽略) 其余 0
 16  4B  frame_count           （统计覆盖帧数；读侧强制 == sample 数）
 20  4B  crc32 (IEEE, 覆盖字节 0..19)
  -- 共 24 字节
```

规则：
- tpcC 与每帧 packet header 的重复字段**必须一致**（`tc_mux_add_packet` 拒绝
  不一致 packet）；**例外：qp 系字段**（qp_base 为电影标称值，逐帧 qp 可变）。
- sar 0/0 = 未指定：原样入 tpcC（pasp 不写），读取侧归一为 1/1。
- reader 打开时校验 tpcC CRC，读取 sample 时不重复校验（packet CRC 已覆盖）。
- tpcC 是加速/预检通道；解码仍以每帧 header 为权威（packet 自包含原则不变）。
- 帧 QP 逐帧可变（qp_delta 不入 tpcC）。
- **tpcB 兼容性（v1.1，向后兼容）**：旧 reader 的子 atom 循环按 §2 跳过未知
  子 atom（只取 tpcC），FFmpeg oracle 同样跳过——不写入即不出现
  （golden_mov_v1 不变）；出现时 reader **必须**校验长度 24/magic/version/
  CRC、alpha_mode 与 alpha_bit_depth 与 tpcC 一致、frame_count == sample 数，
  违反 → `TC_ERR_MALFORMED`/`TC_ERR_CHECKSUM_MISMATCH`。
  FastStart 重建 moov 时原样保留（`tc_movie_faststart`）。语义与策略详见
  ADR-C014 与 bitstream_spec §11.3。
- `tpcD`（电影元数据：导出档位 + 厂商标识，v1.7 追加，**二进制**，可选）：

```
  0  4B  magic 'TPCD'
  4  2B  version = 1 (u16be)
  6  1B  tier_id   0=未声明；1=proxy 2=lt 3=standard 4=hq 5=4444 6=4444xq
                7=lp 8=raw（v1.9 域 0..8；>8 → TC_ERR_INVALID_ARGUMENT/
                MALFORMED）
            7=lp（v1.8；TC_TIER_*；与 tpcC.profile 的关系：1..4 与 7 均为
             profile 3 之上的档位语义——7 是唯一帧间档（载体 V7-R3），
             帧间性由包头自描述、不入 tier 语义；5/6 即独立 profile 5/6
             的档位名）
  7  1B  reserved = 0
  8  2B  vendor_len (u16be, ≤15)   （0 = 不携带 vendor 串）
 10  2B  label_len  (u16be, ≤63)   （0 = 不携带 label 串）
 12  N   vendor UTF-8（无 NUL 收尾，N = vendor_len；如 "TOPOS"）
 12+M M   label UTF-8（无 NUL 收尾，M = label_len；如 "Topos 422 HQ"）
 12+N+M 4B  crc32 (IEEE, 覆盖字节 0..11+N+M)
  -- 共 16..94 字节（定长域 12 + 串域 ≤78 + CRC 4）
```

- **tpcD 兼容性（v1.7，向后兼容；v1.8 tier 域扩展）**：与 tpcB 同款规则
  ——不写入即不出现（golden 不变）；出现时 reader **必须**校验
  magic/version=1/reserved=0/tier_id ≤ 8（v1.8 起含 TC_TIER_LP，v1.9 起含 TC_TIER_RAW，
  ADR-C056）/串域长度一致（12+vendor_len+label_len+4 == 载荷长）/
  CRC，违反 → `TC_ERR_MALFORMED`/`TC_ERR_CHECKSUM_MISMATCH`；重复
  `tpcD` → `TC_ERR_MALFORMED`。FastStart 重建 moov 时原样保留。
  读侧 `tc_movie_info` 的 `reserved[1]` = tier_id（无 tpcD 或未声明 = 0；
  reserved 槽位复用惯例，info 结构 ABI sizeof 不变）。vendor/label 串仅
  落盘供外部工具自描述，应用层由 tier_id → 本地档位表取展示名。

## 5. 采样表语义（reader 必须支持的输入域）

writer 恒定产出 §3 的形态；reader 按完整 QuickTime 语义解析，接受：

- stsc 任意 chunk 布局（逐 sample 推进展开，溢出防护见 §7）；
- stco 与 co64 任一；
- stts 多 run（VFR；run 内 delta≥1，delta=0 → MALFORMED）；
- stss 缺失 = 全部 sample 为同步点（QT 语义）；
- stsz 的 uniform size 形式（sample_size≠0）；
- 未知 atom 四级跳过：顶层 / moov 子级 / stbl 子级 / stsd entry 子级
  （§3.1 矩阵；跳过不得影响后续 atom 定位——size 字段自描述）。

PTS 计算：`pts[0]=0`，`pts[i]=pts[i-1]+dur[i-1]`，dur 来自 stts 展开。
duration ticks 与 mdhd timescale 构成秒换算。

## 6. API 映射

- 写：`tc_mux_create(cfg, sink)` → `tc_mux_add_packet(pkt,size,pts,dur)` →
  `tc_mux_finish()`。sink 为 `write+seek` 回调（库不做 I/O）。
- 音频（v1.1/v1.4）：`tc_mux_add_audio` → `tc_mux_set_audio_asc`（mp4a）→
  `tc_mux_set_audio_priming`（v1.4，mp4a priming/elst，finish 前至多一次）；
  读 `tc_movie_audio_info`（reserved[0]=priming_samples）/ `tc_movie_read_audio`。
- 时间码（v1.5）：`tc_mux_set_timecode(hh,mm,ss,ff,fps,df)`（finish 前至多
  一次，附录 A.6 校验）→ `tc_movie_timecode`（无 tmcd 轨 → TC_ERR_STATE）。
- FastStart：`tc_movie_faststart(io_in, sink_out)` —— 独立后处理 pass
  （qt-faststart 语义），偏移重定位 + 必要时 stco→co64 升级。
- 读：`tc_movie_open(io)`（read 回调，只解析 moov 建索引，不读 mdat 数据）→
  `tc_movie_info` / `tc_movie_packet(i,…)` O(1) / `tc_movie_packet_pts` →
  `tc_movie_close`。

## 7. 错误与鲁棒性规则（reader，规范性）

按检测顺序：

1. io 读失败/短读 → `TC_ERR_IO`。
2. atom size 越界（size < 8 或越过父容器/文件尾）→ `TC_ERR_MALFORMED`；
   **例外（2a）**：顶层 `mdat` 声明长度越过文件尾（截断下载/未完成写）→
   按实际剩余长度收敛继续解析（moov 完整即可索引；单 sample 越界按 6 处理）。
3. 缺 moov / 重复 moov / moov 内无 trak / **多 trak** / hdlr 非 vide /
   stsd entry_count≠1 / entry fourcc ≠ `TPIC` → `TC_ERR_MALFORMED`
   （外来 MOV 的明确拒绝）。
4. tpcC 缺失或 magic/version/CRC 不符 → `TC_ERR_MALFORMED`。
5. 采样表不一致（stts 展开总数、stsz 数、stsc 展开总数互不等；stsc
   first_chunk 非严格递增或首块不连续；stco 条目不足；stts delta=0；
   stss 索引越界；chunk 偏移+size 越过 io 长度）→ `TC_ERR_MALFORMED`。
6. sample 读取时 offset+size 越界 → 该 sample 读取失败（`TC_ERR_MALFORMED`），
   **不**静默置空；其余 sample 不受影响（单帧损坏不污染下一帧在容器层的对应）。
7. 所有计数/索引算术必须 64 位无符号并逐步检查溢出；恶意采样表不得触发
   超额分配（上限 = sample_count 声明值 ≤ 2^26 且与 stsz 计数一致）。

索引内存（可测量指标）：`offsets u64[N] + sizes u32[N] + pts u64[N] +
dur u32[N] + sync bitmap N/8` ≈ 25 字节/sample。

## 8. 确定性（冻结）

同一输入（配置 + packet 序列）必须产出**逐字节相同**的文件：
- 无时间戳、无随机、无版本字符串渗入（moov 时间字段=0）；
- atom 顺序与字段值全部由本规范固定；
- 由 `golden_mov_v1` 门禁冻结。

## 9. 验收 oracles（v1.2：三方，逐包断言）

互操作证据的最低标准是 **demux 级对拍**——"未知 codec 不崩溃"不构成证据。

- **独立 atom parser**（`tests/interop/interop_oracle.py`，纯 struct 实现，
  与本库 reader 无共享代码）：树结构（ftyp 品牌、单 moov/mdat、布局）、
  tpcC CRC、采样表交叉一致、每样本绝对偏移落在 mdat 载荷内且以
  `TPIC` 魔数开头。
- **PyAV/FFmpeg demux 级**：`av.open` + demux 逐包 count/size 与 stsz 对拍、
  duration 合计与 stts 对拍（FFmpeg mov demuxer 对未知 codec 在流尾补一个
  0 字节虚包承载末帧 duration——真实样本不可能是 0 字节，按非空过滤）。
- **ffprobe**（存在时执行；缺失显式 SKIP 不算通过也不算失败）：
  `-show_packets` 包数/size、`codec_tag_string=TPIC`。
- 本 reader：标准/FastStart/手工 co64（双布局）/未知 atom（四级）/损坏
  sample table（六类）/VFR/SAR/长时文件 fixtures（`tests/unit/test_mov.c`
  R5 节），全部为确定性单测，非随机 fuzz 碰运气。

## 10. 产品非目标（v1.2 冻结）

- **音频轨**：~~V2.0 不支持~~ → **v1.1 解冻**（2026-09-19，ADR-C051）：
  单音频轨（lpcm/mp4a）由本规范 v1.1 附录 A 规范化，时间线导出携带。
  回放/渲染缓存/代理仍不带音频（ADR-C008"回放音频恒取 master"不变）。
  多音轨与音频编辑仍为非目标。
- **field order**（`fiel`）：不写不读（逐行/隔行语义属源素材元数据，中间片
  逐行假设；`fiel` 若出现按未知 stsd 子 atom 跳过）。
- **edit list**（`edts/elst`）：~~不写不读~~ → **v1.4 解冻音频 trak 最小子集**
  （2026-09-20，ADR-C052）：仅音频轨、单条目 v0 elst（AAC priming 对齐，
  附录 A.5）；**视频 trak 仍禁止**（pts 无空洞约束 §3 不变，视频轨出现
  edts → MALFORMED）。

---

## 附录 A：音频轨（v1.1，2026-09-19 · 规范性）

由 ADR-C051 解冻 §10"音频非目标"引入。mux 写入 moov 第二 trak；reader
输入域：soun trak ≤ 1，声样描述 version ∈ {0,1,2}（v1.4 增 V2：>65535Hz
或 float32），其表级规则与视频轨一致（§5/§7：singleton 重复
拒绝、stsz/stts 一致性、mdat 归属复验）。无音频 trak 的文件与 v1 形态
逐字节一致（golden 钉死）。

### A.1 原子布局（writer 产出形态）

```
trak #2
├─ tkhd (v0, flags=0x000007)   track_id=2, volume=0x0100, 无几何,
│                              duration = 采样帧数（tick=采样帧）
├─ edts > elst (v0)            可选（v1.4，A.5；仅 mp4a + priming>0）
└─ mdia
   ├─ mdhd (v0)                timescale = 采样率, duration = 采样帧数, und
   ├─ hdlr (v0)                subtype='soun', name="Topos Audio Handler"
   └─ minf
      ├─ smhd (v0)             balance=0
      ├─ hdlr (v0)             'dhlr', "Data Handler"
      ├─ dinf > dref > 'url '  自包含（同视频轨）
      └─ stbl
         ├─ stsd (v0)          entry_count=1（A.2）
         ├─ stts (v0)          等 dur RLE（与视频轨同一写法）
         ├─ stsc (v0)          单条: first_chunk=1, samples_per_chunk=1, id=1
         ├─ stsz (v0, non-const) 逐 chunk 精确字节数
         └─ stco | co64 (v0)   轨内独立判定（≤0xFFFFFFFF → stco）
```

### A.2 stsd entry（二选一）

**lpcm 档（无损）**——两种声样描述形态（v1.4）：
```
A. rate ≤ 65535 且整数样本 → SoundDescription（version=0）：
  reserved[6]=0, data_reference_index=1,
  channels(2B), sample_size=bits(2B), compressionID=0, packetSize=0,
  sampleRate 16.16(4B)
  lpcm 扩展（20B，Apple TN2120）：
    formatFlags：0x0000000E=signed|big-endian|packed（'twos'，整数档）
                0x0000000B=float|big-endian|packed（float32 档）
    constBitsPerChannel=bits, formatSpecificFlags=0,
    constBytesPerAudioPacket=channels*bits/8, constLPCMFramesPerAudioPacket=1
B. rate > 65535 或 float32 → SoundDescriptionV2（version=2）：
  公共段 16B 后接 V2 扩展 48B（always3/always16/alwaysMinus2/always0、
  soundRate 16.16=1.0、sizeOfStructOnly=20、sampleRate float64、
  numChannels、always7F000000、constBitsPerChannel、formatSpecificFlags
  （同上 formatFlags 语义）、constBytesPerAudioPacket、
  constLPCMFramesPerAudioPacket）；TN2120 字段并入主体
'chan' 子原子（两形态均写）：version/flags=0, channelLayoutTag, bitmap=0,
  #descriptions=0
  布局 tag：stereo=0x00650002, 5.1(A)=0x008C0006, 7.1(A)=0x008E0008

float32 用 V2 的原因（M-B4 实测）：ffmpeg/VLC 对 V0/V1 lpcm 不读
formatFlags float 位（48k f32 直挂被识别为 pcm_s32be）；V2 的
formatSpecificFlags 才是各引擎一致识别的 float 声明位。
```

**mp4a 档（AAC 包原样存储；编码在应用层；v1.4 互操作修正后的形态）**：
```
'mp4a' SoundDescriptionV1（version=1）：
  同上标准字段（sampleSize=16, compressionID=0xFFFE"V1 惯例"）+ 16B v1 扩展
  （samplesPerPacket=1024, bytesPerPacket=0, bytesPerFrame=0, bytesPerSample=2）
'wave' 子原子（QuickTime 声样扩展，与 ffmpeg movenc mov 产物同构）：
  'frma'('mp4a') → 'mp4a'12B 空原子 → 'esds' → 8B null 终结子
'esds'（full atom v0；ISO 14496-1 描述符：tag 1B + 可变长 7-bit 续位长度，
  四描述符为同级链——DSI/SL 是 DCD 的兄弟而非子级）：
  ES_Descriptor(0x03) → ES_ID=2, flags=0
  → DecoderConfigDescriptor(0x04, 13B 叶子): objectTypeIndication=0x40(AAC),
    streamType=0x15, bufferSizeDB=0, max/avgBitrate=0
  → DecSpecificInfo(0x05): AudioSpecificConfig 原样（应用层 extradata）
  → SLConfigDescriptor(0x06): payload=2
'chan' 子原子（v1.4 起随 mp4a 写出）：同 lpcm 档布局 tag 集

互操作注（v1.4，M-B0 实测）：早期 v1.1 写法（固定 u32 描述符长度 + DSI/SL
嵌套 DCD + 直挂 esds 无 wave 包裹）ffmpeg/VLC 可容错，但 CoreAudio 严格
解析读出长度 0（0 packets）、AVFoundation 整文件拒开——以上即修正后的
规范形态，与 QuickTime/ffmpeg 生态产物字节同构。

### A.3 采样表语义与校验（reader 规范性）

- 音频"样本"（chunk）≈ 1024 采样帧（AAC 天然；PCM 人为分帧，末 chunk 可短）。
- `stts` dur = 该 chunk 覆盖的采样帧数（tick，timescale=采样率）；
  delta=0 → MALFORMED（同视频规则）。
- 与视频轨相同的表级校验全部适用：singleton 表重复 → MALFORMED；
  stsz 计数 ≠ stts 展开 → MALFORMED；size=0 → MALFORMED；
  chunk 不落在 mdat payload → MALFORMED；mdhd timescale == stsd sampleRate。
- 声明域（writer 校验，create 时 fail-fast）：codec ∈ {0=无,1=lpcm,2=mp4a}；
  采样率 ∈ {44100, 48000, 88200, 96000, 176400, 192000}（v1.4 全家族，
  对齐 Apple 交付规范）；声道/布局配对 1↔mono（v1.6 逐轨 API）、2↔stereo、6↔5.1、8↔7.1；
  lpcm bits ∈ {16,24,32}；mp4a 不接受位深声明；codec=NONE 时其余槽位须全 0。
  reserved[5] 样本格式（v1.4）：0=有符号整数、bit0=float32（仅 lpcm+32bit）。
- finish 时"声明音频但零 chunk"→ `TC_ERR_INVALID_ARGUMENT`（fail-fast，
  防半声明产物）。

### A.4 FastStart

`tc_movie_faststart` 带音频文件：重建双 trak moov；音频 chunk 偏移随视频
顺序重排（视频样本后接音频 chunk），轨内 stco/co64 独立适用选择规则；
mdat 声明长度 = 视频+音频字节总量 + 头。已是 FastStart → 幂等空输出
（不变）。

### A.5 edts/elst 最小子集（v1.4，2026-09-20 · 规范性）

由 ADR-C052 解冻 §10"edit list 非目标"引入：**仅音频 trak**、仅 mp4a 档、
仅 AAC priming 对齐用途。lpcm 档采样精确，恒不写。

```
trak #2
├─ tkhd
├─ edts                        ← 仅 priming > 0 时写（finish 前
│  └─ elst (v0, flags=0)          tc_mux_set_audio_priming 声明）
│     ├─ segment_duration u32  movie timescale（mvhd=1000）轨时长（向上取整）
│     ├─ media_time       i32  音频 timescale（采样率）下的 priming 采样数（>0）
│     └─ media_rate       0x00010000（1.0 倍速）
└─ mdia …
```

- **writer**：`tc_mux_set_audio_priming(m, samples)`——finish 前至多一次；
  仅 mp4a 合法（lpcm/NONE → INVALID_ARGUMENT）；samples 须 > 0 且 < 已写入
  音频总采样数。取值 = AAC 编码器自报 priming（首包 pts 的负值；native
  aac = 1024；aac_at = 2112），即**流固有序曲延迟**——ADR-C052 校准实证
  无 elst 端到端延迟恰等于它，写它即端到端零残差。
- **reader**（fail-fast）：仅 soun trak 接受 edts；恰一个 elst；version=0、
  entry_count=1、media_time > 0、media_rate == 0x00010000、media_time <
  轨总采样数——违反任一 → MALFORMED。**视频 trak 出现 edts → MALFORMED**
  （§3 pts 无空洞约束不变）。
- **读侧暴露**：`tc_movie_audio_info` 的 reserved[0] = priming_samples
  （0 = 无 elst/旧文件；`tc_movie_read_audio` 语义不变，仍返回原始存储
  字节——时间轴裁剪由应用层按 priming 执行，容器不代劳）。
- **faststart**：moov 重建时 elst 随音频 trak 保留。
- 已知边界（ADR-C052）：忽略 elst 的播放器（部分老 Android/硬解）按未裁剪
  流播放（AAC 档音频晚 priming/采样率 ≈21ms@1024@48k）；K1/K2 见互操作
  报告 `topos_audio_interop_report_2026-09-20.md`。

### A.6 tmcd 时间码轨（v1.5，2026-09-20 · 规范性）

由 ADR-C055 引入：专业中间片起始时码（ProRes/FCP 对标）。单样本 4B 大端
起始帧计数，逐帧时码由应用层按 start + n 推导（O(1)，无逐帧表）。**轨序
钉死：video=1、audio=2、tmcd=3（恒末轨）**——M-B8 多音轨沿用 audio 占
2..N+1、tmcd 顺延的分配规则。

```
moov
├─（视频 trak 尾部追加）tref{'tmcd': [tmcd_track_id]}
└─ trak（末位）
   ├─ tkhd  flags=0x2（in-movie）、无几何、volume=0、dur=视频轨 movie-ts 时长
   ├─ edts{elst(v0): seg_dur=同 tkhd、media_time=0、rate=1.0}
   └─ mdia
      ├─ mdhd  df=1: timescale=标称fps×1000、dur=帧数×1001
      │        df=0: timescale=标称fps×512、 dur=帧数×512（oracle 网格）
      ├─ hdlr  'tmcd'（本库 ISO 风格；oracle 为 mhlr+pascal 名，可差）
      └─ minf
         ├─ gmhd{gmin + tmcd{tcmi 零值}}（oracle 另带 text/tcmi 字体装饰，可差）
         ├─ hdlr 'dhlr' + dinf{dref{url 自包含}}
         └─ stbl
            ├─ stsd 单 'tmcd' entry（36B）：
            │   res6 + dri2 + 保留u32(0) + flags u32 + timeScale u32
            │   + frameDuration u32 + fps u8 + pad3
            │   flags bit0=DF；timeScale/frameDuration = mdhd 同源；
            │   fps = 标称 fps（u8）
            ├─ stts 单条目 {帧数, frameDuration}
            ├─ stsc 单条目 {1,1,1}；stsz uniform 4B×1；stco 单条目
            └─ 样本：i32 BE 起始帧计数（相对时码原点；负值不支持）
```

- **声明**：`tc_mux_set_timecode(m, hh, mm, ss, ff, fps, df)`——finish 前
  至多一次。校验（INVALID_ARGUMENT）：fps 白名单 {24,25,30,48,50,60}
  （NTSC 分数帧率以标称值 + df 表达）；df ∈ {0,1} 且仅标称 30/60 合法；
  hh<24、mm/ss<60、ff<fps；**DF 分钟首帧标签有效性**（与 media 层 Timecode
  同规）：非 10 分钟整的分钟 ss=0 时 30DF 拒 ff<2、60DF 拒 ff<4
  （SMPTE 12M 跳帧：30DF 每分钟跳 2、60DF 跳 4；ffmpeg oracle 实测
  '00:01:00;04'@60DF → 计数 3600）。负时码 flags bit2 恒 0。
- **计数换算**（writer）：count = (hh×3600+mm×60+ss)×fps + ff −
  跳帧系数×(总分钟 − 总分钟/10)，跳帧系数 = 60DF?4:2。
- **逆换算**（reader，`tc_movie_timecode`）：块内第 0 分钟 wall=count、
  其余 = count+跳帧系数；fpm=fps×60−系数、fp10=fps×600−9×系数。
  无 tmcd 轨 → TC_ERR_STATE。
- **reader（fail-fast）**：tmcd trak ≤ 1；stsd entry ≥ 36B 且 fourcc='tmcd'；
  stts 恰 1 条目；stsz 恰 uniform 4B×1；stco 恰 1 条目且样本可读恰 4B；
  fps 白名单外或 DF 配非 30/60 → MALFORMED。视频 trak edts 规则不变
  （A.5；tmcd trak 自带 edts 属合法形态）。
- **faststart**：tmcd 元数据随 moov 重建保留，4B 样本重定位至 mdat 尾。
- **零变化原则**：未声明时码的文件与 v1.4 形态逐字节一致。
- **已知差异/边界（ADR-C055 差异表）**：K1 hdlr/gmhd 装饰形态（ISO vs
  QuickTime Pascal 名；播放器不解析）；K2 ffmpeg 重封装（mov→mov copy）
  的自动补轨以**视频流名义帧率**生成 tmcd fps——本容器视频 trak 呈现
  ts=24000/帧 网格，超出 movenc u8 fps 域 → 须 `-write_tmcd 0` 重封装；
  K3 movenc 重封装既有 tmcd 数据流时 timecode_flags 恒 0（DF 标志丢失、
  计数值保留）；K4 ffmpeg ≥8 的 `-write_tmcd`（默认开）会向 MP4 塞非标
  tmcd data 轨——标准 MP4 无 tmcd 定义，标准读取方不识别；
  K5 精确 30/60 + DF 的文件按 NTSC 网格（ts=×1000/fd=1001）写，与
  oracle 对精确整数率输入的 512 网格不同——本 API 以 df 表达 NTSC 语义，
  二者比率等价、时长口径一致。

### A.7 多音轨 / stems 角色轨（v1.6，2026-09-20 · 规范性）

由 ADR-C054 解冻"音频 trak ≤ 1"为 ≤ 16（QuickTime 惯例上限）。**轨序
钉死：video=1、audio=2..N+1（声明序）、tmcd=N+2 恒末轨**（ADR-C055
规则的 N 轨推广）。逐轨独立：格式（codec/rate/channels/layout/bits/
format）、采样表（stco/co64 逐轨独立判定）、priming/elst、轨名。

- **声明**：逐轨 API `tc_mux_add_audio_track(cfg)`（cfg 含五元组 +
  format + name；格式校验与 reserved 槽位共用同一规则——新增
  1ch↔mono 布局档，chan tag 0x00640001 与 ffmpeg MOV_CH_LAYOUT_MONO
  同值）；`tc_mux_set_audio_track_asc/priming`、`tc_mux_add_audio_to`
  按轨索引（0 起）操作。reserved 槽位声明的轨 = 轨 0，与逐轨声明可
  混用（reserved 在前）。
- **轨名**：trak/udta/©nam（文本 = u16 BE 长度 + UTF-8 + u16 0 终结，
  movenc 同构）+ hdlr 名同值（缺省回退 "Topos Audio Handler"）。
  reader 对越界长度 fail-safe（按无名处理，不拒文件）。
- **半声明**：任何声明轨在 finish 时 chunk 数为 0 → INVALID_ARGUMENT。
- **reader**：soun trak 声明序分派至轨槽（> 16 → MALFORMED）；逐轨
  表级校验与 v1.1 一致（附录 A.3 singleton 规则逐轨适用）；查询
  `tc_movie_audio_track_count` / `tc_movie_audio_info_at(track)` /
  `tc_movie_read_audio_at(track, …)`；**v1.1 旧 API 恒轨 0**（单轨文件
  新旧 API 读出逐字节一致）。
- **faststart**：逐轨重定位（轨内 chunk 顺序 = 声明序 = 布局序）。
- **兼容**：无音轨或单轨文件与 v1.5 形态逐字节一致（golden 钉死）；
  多轨文件被 v1.5 以前 reader 按各自规则处理（≤ 1 音轨 reader 拒第二
  轨或忽略——非本规范域）。
- **已知边界（ADR-C054）**：导出侧按角色分组渲染多母带 + 输出轨映射
  UI 为后续批次（容器/绑定/探测层已就绪；现导出仍单母带 = 轨 0）。

