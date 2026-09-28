# ADR-C021：R5 —— MOV 与媒体元数据收口

- 日期：2026-08-30
- 状态：已接受（实施完成）
- 上下文：`docs/Topos_V2.0_完成度审计与整改计划_2026-08-30.md` §4 R5
  （五项任务：atom 冻结 / fixtures / oracle / 预检前置 / 音频非目标决策）
- 关联：ADR-C005（阶段 5 容器原始决策）、ADR-C014（tpcB/R3）、
  container_spec_v1.md v1.2（本轮规范性产出）

## 1. 背景

阶段 5 交付的自研最小 MOV"当前可用"，但审计指出四处收口缺口：
①正式支持 atom 与拒绝策略未冻结成文（reader 行为有码无据）；②co64 读取
分支、未知 atom、损坏 sample 表等路径只有 fuzz 随机覆盖，无确定性
fixtures；③oracle 停留在"ffprobe 认 TPIC + 未知 codec 不崩溃"，不构成
互操作证据；④"纯视频无音频"唯一提示在编码会话内（encoder logging），
用户在导出开始前不知情。

## 2. 决策

### C-147：支持矩阵冻结（container_spec v1.2 §3.1，纯文档收口）

四级（顶层/moov/stbl/stsd entry）× {writer 产出 / reader 解析 / reader
忽略 / reader 拒绝} 成表。关键钉死项：
- **tpcC 唯一权威**：`colr`/`pasp` 为第三方互操作通道，reader 忽略——
  篡改 pasp/colr 不影响读取结果（fixture 实证：pasp 改 9:16、colr
  primaries 改 9，读侧仍 5:4 / 1）。
- `mvhd`/`tkhd` 忽略（duration 由 stts 重算）；未知 atom 四级跳过；
  多 trak / 重复 moov → MALFORMED。
- **pts 无空洞约束**成文（无 elst 布局下 gap 无法表达，add_packet 拒绝）。
- FastStart 重写按**新**偏移重选 stco/co64——手工 co64 输入降级 stco
  输出合法（fixture 实证）。
- 字节布局零变更：golden_mov_v1 不动，纯文档升版 v1.2。

### C-148：确定性 fixtures 补齐（tests/unit/test_mov.c R5 节）

测试侧自带独立迷你 atom walker（oracle 思想下沉单测）：
- **手工 co64 双布局**：stco→co64 条目拓宽变换（标准布局条目值不变；
  快启布局 mdat 后移、条目值同步平移）→ 读回 7 帧逐字节一致；
  faststart(手工 co64) → stco 降级仍全帧一致；
- **未知 atom 四级**：顶层尾部追加 / moov / stbl / stsd entry 固定字段后
  插入 8B JUNK，四级均开档 + 全帧校验；
- **损坏 sample 表六类**：stsz 计数 ±1（一致性/表越界两分支）、stsc
  spc=2（展开不齐）、stco 条目 -1、stss 索引越界、stts delta=0——每类
  独立命中一条校验分支，全部 MALFORMED；
- **VFR 多 run**：dur 1/2/3 循环逐帧 pts/dur 展开；**SAR 权威**：sar 5/4
  写 pasp、sar 1/1 不写 pasp；
- **长时文件**：2000 帧 VFR（2000 stts run）——索引 ≤26B/帧线性界、
  抽查 O(1) 随机访问与 pts 公式。
开发插曲：fixture 变换器自身的两个 bug（t_chain 末段指针推进越界、
co64 suffix 目标偏移）被"标准布局 suffix 为空"掩盖到快启变体才暴露——
测试代码同样需要失败先行，最终以独立 Python walker 交叉定位。

### C-149：probe `--verify` 升级为 packet_scan 级（诚实性修复）

原实现仅 `memcmp(pkt,"TPIC",4)`，与头注释"tc_packet_scan 级校验"不符。
改为调 `tc_frame_decode`（内部即 packet_scan：magic + 帧头 CRC + 结构
自检），坏帧计数语义从"魔数比对"升级为"整包结构校验"。门禁新增坏包
断言：改坏帧 0 魔数第 4 字节 → `verify: 3/4` 必须报出。JSON 模式补
`read_errors` 计数（原静默跳过读不出的 sample）。

### C-150：三方互操作 oracle（tests/interop/interop_oracle.py，接入门禁）

- **独立 atom parser**：纯 struct 实现（与 reader 无共享代码），树结构/
  tpcC CRC(zlib)/采样表交叉一致/每样本偏移落 mdat 载荷且 TPIC 开头；
- **PyAV demux 级**：逐包 count/size 对拍 stsz、duration 合计对拍 stts。
  发现并文档化 FFmpeg 行为：对未知 codec 在流尾补 1 个 0 字节虚包承载
  末帧 duration（真实样本不可能 0 字节——stsz size=0 被 §7 拒绝——按
  非空过滤，虚包 >1 报错）；
- **ffprobe**：`-show_packets` 包级 + `codec_tag_string`（缺失显式 SKIP，
  不算通过也不算失败）。
fixtures 自产：standard VFR+SAR（binding mux）、手工 co64（Python 独立
变换——真实 FFmpeg demux co64 是比本库 reader 更强的外部证据）、CLI 产
FastStart。门禁替换原浅层 ffprobe 块（`nb_frames` + "优雅跳过"）。
校准记录：CLI 产 CFR 为 timescale 24000 / dur 1000 tick（非 1 tick）。

### C-151：音频非目标决策 + 编码前告知（preflight 前置）

**V2.0 不支持音频**（V2.1+ 亦不承诺）：Topos MOV 是纯视频中间片，音频由
宿主管线在最终交付时单独处理。落点三处：
- container_spec §10 非目标（规范性）；
- `topos_binding.TOPOS_MOVIE_CAPS`（容器能力查询：audio_tracks/
  field_order/edit_lists 均 False）；
- `video_export_preflight._check_topos_video_only`：topos 系 codec +
  include_audio + 序列含带 clip 音频轨 → 入队前 `topos.video_only`
  WARNING（不阻断——纯视频导出合法，但必须事先告知）。六档 codec 一致
  触发（test 钉死）；encoder 内编码期 logging 保留为纵深防御兜底。
field order（fiel）与 edit list（elst）同批写入非目标。

### C-152：oracle 分级与降级语义（门禁）

PyAV + 独立 parser 为**硬 oracle**（失败即门禁红；无 .venv 环境显式声明
"CI 需装齐"而非静默跳过）；ffprobe 为**软 oracle**（外部二进制，PATH
不可控，缺失打印 SKIP 行）。分级理由：PyAV 是项目既定依赖（P4.1 迁移），
ffprobe 是系统级外部工具。

## 3. 完成门槛对照（审计 R5）

| 门槛项 | 证据 |
| --- | --- |
| 正式支持 atom + 拒绝策略冻结 | container_spec v1.2 §3.1/§7（C-147） |
| 大型 co64/长时/双布局/未知 atom/损坏表/VFR/SAR/field order/色彩 fixtures | test_mov.c R5 节（C-148；field order 以非目标 + 未知子 atom 跳过覆盖） |
| ffprobe/PyAV/独立 parser oracle | interop_oracle.py 三方 × 三 fixtures 全绿（C-150） |
| 编码前明示"纯视频无音频" | preflight topos.video_only + UI 注释 + 门禁测试（C-151） |
| 音频决策入非目标 + 容器能力查询 | spec §10 + TOPOS_MOVIE_CAPS（C-151） |
| spec/writer/reader/probe/SDK/UI 矩阵一致 | §4 对照表 |

## 4. 矩阵一致性对照（spec ↔ 代码 ↔ 文档 ↔ UI）

| 声明 | spec | 代码 | 文档/UI |
| --- | --- | --- | --- |
| tpcC 唯一权威（colr/pasp 忽略） | §3.1 | mov.c stsd 子循环 + test 权威 fixture | SDK §9 |
| 未知 atom 四级跳过 | §3.1/§5 | mov.c 各级 walk + 四级 fixture | SDK §9 |
| 多 trak/重复 moov 拒绝 | §3.1/§7 | mov.c（既有） | SDK §9 |
| pts 无空洞 | §3 | tc_mux_add_packet（既有） | SDK §4 |
| FastStart 重选 stco/co64 | §3.1 | tc_movie_faststart + 降级 fixture | SDK §4 |
| 纯视频（无音频）非目标 | §10 | TOPOS_MOVIE_CAPS + preflight | SDK §9 + UI 注释 + 导出测试 |
| probe --verify = packet_scan 级 | §9 | probe_cli tc_frame_decode + 门禁坏包断言 | README |

## 5. 遗留与后续

- `topos_probe_cli` JSON 的 `samples` 列表仍只列前 N 帧（`--samples` 控制），
  read_errors 已显式化；全帧列表输出无需求，不加。
- oracle 的 ffprobe 分级为软门禁——R7（CI 闭环）若 CI 镜像装齐 ffmpeg，
  可升为硬门禁（届时同步更新 run_tests.sh 注释）。
- 长时文件上限 2^26 samples 的 LIMIT_EXCEEDED 分支仍由 fuzz 覆盖
  （确定性构造 2^26 条目不现实）；索引内存 25B/帧界已由本 fixtures 钉住。
