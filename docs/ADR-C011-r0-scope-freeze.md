# ADR-C011 — R0 状态纠偏与发布范围冻结（2026-08-30 审计整改）

> 状态：已采纳（R0 交付）
> 输入：`docs/Topos_V2.0_完成度审计与整改计划_2026-08-30.md`（下称"审计"）
> 效力：本 ADR 与审计文档共同构成范围真相源；R8 独立复审通过前，任何文档
> 不得宣称 V2.0 完成。

## 1. 背景

审计确认：原计划 §12 曾把阶段 0–10 全部标记完成，但 12-bit、4:4:4/GBR、
六个独立 native profile、Alpha 预算自动执行、时间线透明度端到端导出、
premultiplied 编码语义、HDR 高光不裁剪导出、1080p/4K 实时门槛与跨平台发布
门禁均未闭合——"报告已知未达、总表仍写完成"是系统性失真，不是个别笔误。

## 2. 决策

### C-90 命名与完成标记

- 当前能力对外统一命名为 **Topos Video Codec V1 Preview（Intra 4:2:2 10-bit）**
  （`src/shared/codec/topos_profiles.py::TOPOS_CODEC_LABEL`）。
- 计划文档顶部加入效力覆盖声明 + 链接审计；§12 重写为审计重分类表；
  阶段 6–10 标题按证据重标（完成/受限完成/部分完成/未达门槛/未完成）。
- R8 通过前：不得恢复"V2.0 完成"标记；不得启动 V2.1 Micro-GOP。

### C-91 预设（preset）≠ bitstream profile（审计 P1-5）

- Proxy/LT/Standard/HQ 是同一 v1 码流 profile（profile=3，YUV 4:2:2 10-bit）
  之上的**质量预设**：区别仅目标码率（bpp 定标）与 Alpha 策略参数；
- Pro 444 / Extreme 是未来格式占位（`available=False`），实现属整改 R4；
- `topos_capability_summary()` 新增顶层 `_codec` 声明
  （label / bitstream_profiles / `tiers_are_presets=True` / audio=False），
  SDK.md / README / 计划表 / 能力查询对同一问题给出同一答案。

### C-92 V2.0 范围冻结（完成定义）

V2.0 完成至少包含：10/12-bit YUV 4:2:2、10/12-bit YUV/GBR 4:4:4、Alpha
预算自动执行与元数据闭环（计划 §2.2）、真实时间线 Alpha/HDR 导出链达标、
阶段 0 性能门槛（或经 benchmark_protocol 正式修订）、跨平台发布门禁闭合。
执行顺序 = 审计 §4 的 R0–R8 严格串行。

### C-93 风险登记册状态语义收紧（审计 P2）

状态分类改为：**打开 / 已缓解 / 已关闭（附实测证据）/ 未实现（整改轮跟踪）/
已否决（附 ADR 与重评条件）/ 已接受（文档化限制）**；禁止用"已关闭"表达
"本轮不实现"。本轮重标：R-06（最终决策见 C-95）、R-10/R-11/R-12（→已否决，
重评条件 ADR-C009 C-75 在案）、R-21（12-bit 部分→未实现，R4 跟踪）、
R-23（→已接受，文档化限制）、R-24（→已接受，定案）；新增 R-28
（能力声明与实现再次漂移的根因复发风险）。

### C-95 MOV 自研路线最终决策（取代"成熟容器库"初始设想）

- **决策**：自研最小 MOV writer/reader 是 v1 的**正式路线**，不是临时妥协。
  否决 libavformat 的原因不变（tpcC sample entry 无公共 API、字节确定性、
  门禁密封性，ADR-C005 C-38）；ADR-C005 升级为终案。
- **支持边界（container_spec_v1 冻结）**：标准布局 `ftyp(20) mdat moov` /
  FastStart `ftyp moov mdat`；moov = mvhd + trak(tkhd + mdia(mdhd + hdlr +
  minf(dinf/dref + stbl(stsd[TPIC: colr, pasp?, tpcC] + stts + stss? + stsc +
  stsz + stco|co64)))；writer 不产生未列出的顶层 atom；reader 跳过未知
  顶层 atom，结构违规按 §错误矩阵显式失败。
- **非目标**：v1 无音频轨（纯视频中间片，宿主音频管线单独处理——UI/预检
  须在任务开始前明示）；不支持原位编辑；不承诺被通用播放器解码 TPIC。
- **外部 oracle 范围**：ffprobe/PyAV 做**结构性**对拍（布局/轨数/尺寸/
  duration/sample table 语义）+ golden_mov_v1 字节冻结回放；"未知 codec 不
  崩溃"不计为互操作证据。fixtures 扩围（长文件/VFR/损坏 sample table/
  未知 atom/色彩 atom）属整改 R5。

## 3. 验证

- 计划 §12 与阶段标题、SDK.md §0/§9、native README、
  `topos_capability_summary()`、风险登记册对"当前能力是什么 / 预设语义 /
  音频非目标"给出一致答案（`tests/media/test_topos_export.py::
  TestToposProfiles` 覆盖 summary 字段）。
- 本轮无 native 代码变更；门禁全量复跑见 ADR-C012 §4。

## 4. 风险变化

- R-28 新增并缓解（制度：审计为真相源 + 失败先行测试 + ADR 证据链）；
- R-06 由"打开（描述与实际路线不符）"改为"已缓解（边界冻结，扩围在 R5）"。
