# ADR-C055：容器 v1.5——tmcd 时间码轨（专业中间片起始时码）

日期：2026-09-20 · 状态：**已采纳**
关联：ADR-C051/C052（音频轨 v1.1/v1.4）、container_spec_v1.md 附录 A.6
（本次规范化）、`topos_container_audio_optimization_plan_2026-09-20.md`
§2.7/§3 M-B9、轨序约定与 M-B8（ADR-C054，多音轨）的轨 id 分配规则

## 1. 背景

ProRes/FCP 等专业中间片普遍携带 tmcd 轨承载起始时码；"回 Resolve/
Premiere 按 TC 对位继续剪辑"是中间片容器的刚需（计划 §0.5）。计划 §2.7
定设计：字段语义以 QT 规范为准、**字节布局以 `ffmpeg -write_tmcd` 产物
逐字节对拍为准**（oracle 定字节）。

实施中的三次 oracle 实证修正（对拍 dump 于 /tmp，记录见测试与 §4 差异表）：

1. **DF 每分钟跳帧数与帧率相关**：计划期按 29.97 语义写死"每分钟跳 2"。
   60fps DF oracle（`-timecode '00:01:00;04'` @ 60000/1001）样本值 = 3600
   = wall 3604 − **4**；`;02` → 3598 = 3602 − 4。即 **60DF 每分钟跳 4**
   （;00-;03 无效，SMPTE 12M），与本项目 media 层 `Timecode` 类文档一致
   （"59.94 DF 每分钟跳过帧号 00-03 共 4 帧"）。30DF 维持跳 2。
2. **stsd entry 比直觉多一个保留 u32**：oracle tmcd entry = 36B（载荷
   28B：res6+dri2+**保留u32**+flags4+ts4+fd4+fps1+pad3）。ffmpeg demuxer
   在 desc 头后 skip(4) 再读 flags/ts/fd/fps——缺该 u32 时 demuxer 把
   timeScale 读成 flags，整轨元数据错乱（实测我们首版 32B entry 无
   timecode tag、extradata 截断为 16B）。
3. **NTSC 分数族 timescale = 标称×1000**（30000/60000），fd=1001；
   非标称×1001（首版误写 30030/60060）。整数 fps 族维持 512 网格
   （ts=fps×512、fd=512）。

## 2. 决定

**D1（轨形态与轨序钉死）**：moov 可带末位 tmcd trak（≤1；重复 →
MALFORMED）；视频 trak 附 `tref{'tmcd': [tmcd_track_id]}`。**轨序：
video=1、audio=2..N+1、tmcd=N+2 恒末轨**——M-B8 多音轨的轨 id 分配
照此顺延，tmcd 永远最后。单样本 4B BE 起始帧计数，逐帧时码由应用层
按 start+n 推导（绑定层 `frame_timecode(index)` helper，DF 边界单测表
钉死；容器无逐帧表，O(1) 内存）。

**D2（语义规则）**：fps 白名单 {24,25,30,48,50,60}（NTSC 分数帧率以
标称值 + df 表达：23.976→24 NDF、29.97→30 DF 可选、59.94→60 DF 可选）；
**DF 仅标称 30/60 合法**；hh<24、mm/ss<60、ff<fps；负时码不支持
（flags bit2 恒 0）。**DF 分钟首帧标签有效性**与 media 层 `Timecode`
同规：非 10 分钟整的分钟 ss=0 时 30DF 拒 ff<2、60DF 拒 ff<4。
计数换算：count = wall − 系数×(总分钟 − 总分钟/10)，系数 = 60DF?4:2；
逆换算块内第 0 分钟 wall=count、其余 = count+系数。mdhd 网格：DF 走
NTSC（ts=标称×1000、dur=帧数×1001）、NDF 走 512 网格（ts=标称×512、
dur=帧数×512）。

**D3（API 与接线）**：写 `tc_mux_set_timecode(hh,mm,ss,ff,fps,df)`
（finish 前至多一次；STATE/INVALID fail-fast）；读 `tc_movie_timecode`
（无 tmcd → TC_ERR_STATE）。绑定层 `ToposMovieFile.timecode()` +
`frame_timecode(index)` + `format()`；probe 元数据 `timecode`（HH:MM:SS[;:]FF
显示形态）/`timecode_fps`/`timecode_drop_frame`（VideoMetadata 扩展，
无 tmcd 三者同 None）；导出接线：时间线序列 `start_timecode`
（format_settings）→ `ToposVideoEncoder(start_timecode=)` → tmcd——
**有则写、无则不写**（未声明文件与 v1.4 逐字节一致），帧率不在标称域或
分量非法时降级为不写并记日志（时码是元数据，不阻塞交付）。
代理转写保留源起始 TC 的可选任务：**决策 = 暂不做**——代理与母片由
同一导出管线分别驱动，TC 接线已在母片路径生效；代理路径（ADR-C008
纯视频边界）不在本次范围，留待代理管线需求提出时以同一 encoder 参数
透传（工作量 O(1)，无容器阻碍）。

**D4（reader fail-fast 输入域）**：tmcd entry ≥ 36B 且 fourcc='tmcd'；
stts 恰 1 条目；stsz 恰 uniform 4B×1；stco 恰 1 条目且样本恰 4B；
fps 白名单外或 DF 配非 30/60 → MALFORMED。tmcd trak 自带 edts 属合法
形态（其 media_time=0 不构成 pts 空洞）；视频 trak edts 禁令不变（A.5）。
faststart 保留 tmcd 元数据，4B 样本重定位 mdat 尾。

## 3. 后果

- 无时码文件零字节变化；声明时码后文件仅尾部多一 trak + 视频 trak 多
  tref（12+20B）+ mdat 尾 4B。
- 绑定/媒体层可见起始 TC：Resolve/Premiere 导入按 TC 对位成立；
  `frame_timecode` 边界表（`00:00:59;29 → 00:01:00;02`@30DF、
  count 3600 ↔ `00:01:00;04`@60DF）单测钉死，与 media 层 `Timecode`
  同一套跳帧语义，跨层无第二套公式。
- M-B8 轨 id 分配规则就此锁定（tmcd 恒末轨），多音轨落地时不回头改。

## 4. 与 ffmpeg oracle 的差异表（对拍记录）

字节级对拍基准：`ffmpeg 8.1 -c:v prores_ks -timecode … -write_tmcd 1`。

| 项 | oracle | 本库 | 定性 |
| --- | --- | --- | --- |
| tmcd stsd entry | 36B（含 dri 后保留 u32） | 同构 36B | **一致**（首版 32B 已修正） |
| stsd flags/ts/fd/fps 字段 | skip(4) 后读 | 同位读取 | **一致**（首版偏移 -4 已修正） |
| mdhd 网格（NTSC DF） | ts=标称×1000, fd=1001 | 同 | **一致**（首版 ×1001 已修正） |
| mdhd 网格（精确 30 + DF 输入） | 512 网格（输入无分数信息） | NTSC 网格（df 表达） | K5：本 API 语义选择，比率/时长等价 |
| hdlr | 'mhlr' + Pascal 名 | ISO 风格 + C 字符串名 | K1：播放器不解析，与音/视频轨同惯例 |
| gmhd | gmin+text+tmcd{tcmi 含字体} | gmin+tmcd{tcmi 零值} | K1：装饰性字段，demuxer 不读 |
| elst | video/tmcd trak 各一带 | 同 | 一致 |

重封装行为（外部引擎，非本库产物缺陷）：

- **K2**：mov→mov copy 默认路径下 movenc 会按视频流名义帧率自动补轨；
  本容器视频 trak 呈现 ts=24000/帧 网格 → "fps 24000 is too large"
  （movenc u8 fps 域）。用 `-write_tmcd 0` 重封装即保留既有 tmcd 轨。
  根因是视频 trak 呈现网格（既有形态，golden 钉死），如需消除属独立
  容器变更，不并入本次。
- **K3**：movenc 重封装既有 tmcd 数据流时 `timecode_flags` 恒 0 →
  DF 标志丢失、**帧计数值保留**（ffprobe 显示转 NDF 形态）。
- **K4**：ffmpeg ≥8 `-write_tmcd`（默认开）会向 MP4 塞非标 tmcd data 轨
  （`-write_tmcd 0` 关闭后 MP4 产物无时码）。标准 MP4 无 tmcd 定义——
  "MOV 族独有"指标准读取方（QuickTime/Resolve 对 .mp4 品牌不读该轨）。

## 5. 验收记录

- C：`test_mov_audio`（89/89）——矩阵 {24,25,30,48,50,60}×NDF + {30,60}×DF
  roundtrip、faststart 保留、校验拒绝矩阵（含 DF 分钟首帧标签）、
  畸形 tmcd 字节手术 → MALFORMED。
- 绑定/媒体：`tests/media/test_topos_timecode_container.py`（17 例）——
  矩阵 roundtrip、DF 边界表、probe 元数据接线、导出接线（NDF/DF/无/域外
  帧率降级）、ffprobe stream tag 逐字符一致（25 NDF/30DF/60DF）、
  mov→mov 保留 / mov→mp4 丢失语义对照（含 K2/K3/K4 注记）。
- 全量：`ctest` 89/89、`tests/media` topos 套件 1216 passed。
