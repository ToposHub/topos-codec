# ADR-C016：R4.2 —— YUV 4:4:4 10/12-bit（v1.3 枚举扩展）

- 日期：2026-08-30
- 状态：已接受（实施完成）
- 上下文：`docs/Topos_V2.0_完成度审计与整改计划_2026-08-30.md` §4 R4
  子阶段 2（"10/12-bit YUV 4:4:4"；六子阶段严格串行，本 ADR 只覆盖 2）
- 关联：ADR-C015（R4.1 12-bit——位深参数化与 minor 版本机制的先例）、
  bitstream_spec v1.3、container_spec（tpcC byte 7 语义）

## 1. 背景

审计 R4 顺序推进到 4:4:4。侦察确认：**编码器/解码器对所有平面按
`plane_block_cols/rows` 驱动逐块处理，变换/量化/熵编码与平面几何完全解耦**
——4:4:4 唯一缺的是三处几何镜像的 chroma 分派与枚举域放开：

1. `tc_frame_derive_geometry`（frame_header.c）：chroma coded/visible 宽度；
2. `tc_frame_packet_bound`（codec.c）：块数上界（chroma 半宽 → 全宽）；
3. `tc_frame_plane_geometry`（codec.c，解码出口）：U/V visible 宽度。

alpha 面几何本就 = luma（全宽），mode1/mode2 均不受影响；应用层
（encoder 白名单/几何校验、topos_source 平面重组、GPU 上传）按格式名分派。

## 2. 决策

### C-124：几何三镜像按 pixel_format 分派（4:2:2 保持逐字节不变）

`pf=0`（4:2:2）路径所有公式不变（`ceil(vw/2)` / `pad8` / 半宽块数），
既有 golden 与 fuzz 产物逐字节冻结；`pf=1`（4:4:4）chroma coded/visible
宽度 = luma 同值（`coded_width` / `visible_width`），块数上界按三平面
同块数推导。解码出口 `tc_frame_plane_geometry` 以 `info->pixel_format`
分派（容器/帧头一致性由 tpcC + mux 保证单一事实源）。

### C-125：v1.3 minor 版本标记（pf=1 流 =2；逐扩展代递增）

R4.1 把 minor=1 定义为"bd=12 扩展代"。pf=1 是**新的**枚举扩展，按
spec §13.1"向后兼容扩展提升 version_minor"处理：`version_minor = 2`。

- 解码规则：minor ≤ 2；minor=0 + 扩展枚举 → `UNSUPPORTED_VERSION`；
  **minor=1 + pf=1 → `UNSUPPORTED_VERSION`**（v1.2 包络不含 pf=1——v1.2
  解码器虽会按枚举域干净拒绝，但版本声明本身已不一致，协议错误优先）；
  minor=2 + v1.0/v1.2 枚举 = 前向兼容 writer，完全一致解码。
- writer 取所属扩展代的**最低** minor（pf=1 → 2，即使 bd=10；bd=12 + pf=0
  → 1），最大化旧解码器可解域。bd=12 + pf=1 → 2（v1.3 包络包含 v1.2）。
- 编码侧同规则（`cfg_to_frame_header` 单点推导，测试覆盖直接构造 fh
  忘记置 minor 的拒绝路径）。

### C-126：枚举域与能力协商

`pixel_format ∈ {0,1}`；`2`（GBR 4:4:4）继续保留（R4.3 子阶段激活，
错误信息注明），`>2` 未知拒绝——均为 `UNSUPPORTED_PIXEL_FORMAT`。
`tc_query_support(3, pf∈{0,1}, bd∈{10,12}, *)` 全组合 TC_OK。
bit_depth 域不随 pf 变化（4:4:4 × {10,12} 都合法，qp 的 bd 偏移规则
沿用 ADR-C015 C-120）。

### C-127：独立 golden 文件（前两文件字节冻结）

`golden_codec_v1_pf444.bin`（12 记录 = 6 配置 × packet/解码指纹，
fold `8f659edc98ad814f`）：配置含 alpha mode1/mode2、非 8 倍尺寸（36×20
padding 路径）、qp_delta_chroma、**bd=12 + pf=1 组合**（帧头 minor=2
双扩展叠加）。v1/v1_bd12 文件不动。image_synth 增加 `chroma_format`
（0=4:2:2 历史不变 / 1=444 全宽合成）；packet_synth 增加 `pixel_format`
（minor 规则同编码器单点），fuzz corpus 追加 seed_pf444 族
（seed+trunc×5+flip×4，184 → 194 文件）。

### C-128：应用层接线（格式名契约）

- `frame.py` 注册 `topos_yuva444p{10,12}a{8,10,12,16}`（(4,4,4) 子采样）；
  无 alpha 复用 FFmpeg 既有名 `yuv444p10le/yuv444p12le`。
- encoder：`_CHROMA_FMT_RE`（`yuva?(422|444)p`）从 pix_fmt 解析
  `pf`；planar 白名单扩 444 名；planar 几何校验按 pf 期望全宽/半宽；
  packed 路径 `_rgb_to_yuv_planar(chroma_full=)` 不做水平 box 下采样；
  `fc.pixel_format` / `movie_config(pixel_format=)` 贯通（tpcC 一致性
  由 native mux 强制，pf 不匹配的包 → `INVALID_ARGUMENT`，测试覆盖）。
- topos_source：解码侧按 `info.pixel_format` 分派 U/V 几何（含代理
  缩小的 chroma 目标宽）与 `_pixel_format_name`。
- GPU：`_YUV444_FORMATS` 追加 8 个 topos_yuva444 名（`upload_yuv444`
  既有路径，零新内核）。
- binding：`movie_config(pixel_format=0)` 参数；`ToposFrameInfo` 暴露
  `profile/pixel_format`（几何分派的事实源）。

### C-129：tier 目标码率不随 pf 重定标（记录为 R4.4 输入）

`frame_target_bytes = bpp × w × h / 8` 以 luma 像素计，与 pf 无关：
4:4:4 同目标下有效 qp 略升（三平面分摊同一预算）。**接受**该行为
（中间片质量预算按画面而非码流定义），bpp 按 pf 的重定标归 R4.4
（Pro444/Extreme 档位定标时统一处理，届时 hq 档一并复核）。

## 3. 验证

- 失败先行：frame_header（minor=2 语义 + pf 补丁矩阵）/ abi_compat /
  stage10_concurrency / unit_mov / unit_codec（SEGFAULT：encode 拒绝 pf=1
  后空指针解引用）六处先行失败，实现后全绿。
- native：debug 单测 + 三 sanitizer 配置 + conformance 三 golden
  （v1 16 记录 fold `7912af9cd4510822` / v1_bd12 10 记录 / v1_pf444 12 记录）
  + corpus selftest（194 文件）；完整门禁见 run_tests.sh（STAGE-10）。
- 单测覆盖：枚举域（pf=1 OK；2/3 拒绝）、往返+确定性（pkt[7]==2、
  pkt[11]==1）、全宽几何出口、质量界（qp24 maxerr ≤ 96）、bound 覆盖、
  concealment（10-bit 中点 512）、444 多代（gen3==gen4、漂移 < ±1 LSB）、
  tpcC 双向一致性（pf=0 电影收 pf=1 包 → INVALID_ARGUMENT）。
- Python：`TestR42Yuv444` 6 测试（query 域 / planar 10-bit 往返 + 帧头
  byte7=2、byte11=1 + 全宽几何 / packed 拆分色度判别 / 444+a8 组合 /
  444+12-bit 组合）；既有 R1–R4.1 套件零回归。

## 4. 遗留

- R4.3：GBR 4:4:4（pf=2 激活）——需 level-shift 语义 ADR（GBR 无中点约定，
  color_matrix=0 identity 已保留）+ minor=3。
- R4.4：Pro444/Extreme 档位 + bpp 按 pf 重定标（C-129）+ quality CLI
  12-bit 报告。
- GPU 444 上传路径未经渲染级测试（路由表成员级验证；端到端归 R4.6
  UI 协商验证）。
