# ADR-C006 — 阶段 6：Topos Color 解码接入（应用层集成）

- 日期：2026-08-29 ｜ 状态：已采纳
- 输入：ADR-C001 C-14（施工图）、container_spec_v1、bitstream_spec §A.7、
  risk_register R-17/R-13、计划 §12 阶段 6。
- 交付：`src/shared/media/topos_source.py`（ToposMediaSource + is_topos_movie +
  check_topos_codec_available）、factory topos 注册与内容路由、`frame.py`
  PIXEL_FORMATS 登记、`error_handling.py` 顺序表修复、
  `tests/media/test_topos_source.py`（18 项）、`run_tests.sh` 阶段 6 门禁。

## 1. 决策

### C-44 恒 planar 输出，无 packed RGB 路径

`ToposMediaSource` 构造签名对齐 factory 闭包 `(path, target_size,
preserve_planes)`，但 `preserve_planes` 是 no-op：解码平面直接以
`np.frombuffer('<u2')` 只读视图交付（Y/U/V[/A]，tight，满刻度 10-bit）。
后果（有意为之）：master 车道也走 planar → GPU R16UI 上传路径，这正是
C-14 第 2 条期望的形态；无第二条 CPU RGB 转换链。`is_planar_mode=True`、
`output_mode='planar_yuv'`、`set_preserve_planes()` 存在以满足调度层
hasattr 探测。

### C-45 auto 路由 = 结构化内容探测，非扩展名、非裸 magic 扫描（R-17 关闭）

`.mov` 扩展名与通用 QuickTime 冲突 → auto 必须看内容。`is_topos_movie`
只认可写入器的两种**结构位置**（container_spec §2）：

- 标准布局：`ftyp(20)+mdat`，首 packet magic `TPIC` 恰在 mdat 头后
  （64 位 size 形式 @36 / 32 位 @28）；
- FastStart：`ftyp(20)+moov`，`TPIC` entry 位于 moov 头部 stsd 内
  （stsd 恒为 stbl 首子 atom，位置不随帧数增长，4096B 窗口足够）。

帧数据中随机出现的 `TPIC` 字节不会命中结构位置（ProRes 首字节 `icpf`、
H.264 起 start code，实际碰撞概率再降数个量级）；极端误探测在
`create()` 逐后端回退中自愈（topos open 失败 → ffmpeg）。
`_get_backend_order('auto', path)`：探测命中 → `['topos', 'ffmpeg']`，
否则 `['ffmpeg']`；显式 `backend='topos'` 只尝试 topos（选择即承诺）。

### C-46 pixel_format 命名：无 Alpha 复用 `yuv422p10le`，有 Alpha 用 `topos_yuva422p10a{8,10,12,16}`

Topos 无 Alpha 输出与 PyAV `yuv422p10le` **语义逐位一致**（U/V =
ceil(w/2)×h 全高、uint16 满刻度码值不左移）——直接复用注册名，GPU 上传/
分类器/导出链零新增接线。Alpha 变体因色彩面 10-bit 与 Alpha 面位深
（mode2 近似 8/10/12、mode1 无损 16）独立，单一格式级 bit_depth 表达不了，
按 ADR-C001 C-14 建议命名 `topos_yuva422p10a{N}`；**逐面位深以
`PlaneInfo.bit_depth` 为权威**（阶段 7 GPU 链消费）。

### C-47 fps = Rational(timescale, dur₀)；VFR 以首帧时长近似

`tc_movie_packet_pts(0)` 已返回 (pts, dur)——fps 推导无需新增 C API
（砍掉了本阶段原计划的 `tc_movie_packet_dur` ABI 扩展）。`Rational`
自动约分（24000/1000 → 24/1）。VFR 文件（reader 接受多 run stts）以首帧
时长近似为 CFR——阶段 8 encoder 只产 CFR，该近似只影响手工 VFR 文件，
已在 docstring 记录。

### C-48 色彩码翻译：复用 color_metadata map_*；color_range 例外

tpcC 色彩码即 H.273（bitstream_spec §A.7 冻结），与 color_metadata 的
FFmpeg int 映射同源——primaries/transfer/matrix 直接复用 `map_color_*`，
未知码 → `'unknown'`（诚实呈现；下游 `is_hdr` 等判据对 unknown 自然不
成立，这是 R-13 的缓解方式而非掩盖）。**color_range 例外**：tpcC/帧头
语义 0=limited/1=full，FFmpeg 枚举 0=unspecified/1=limited/2=full——
不经 `map_color_range`，在 topos_source 内直接翻译（单点注释标明）。

### C-49 target_size 代理缩小 = 纯 numpy 分离 box 滤波；放大回退

不引入 cv2/swscale 依赖：`np.add.reduceat` 变宽分箱，行/列两轴分离、
各舍入一次（uint32 中间累加，单轴最大分箱 8192×65535 < 2^32 无溢出；
误差 ≤1 LSB/轴，代理精度足够）。仅缩小：请求放大回退原始分辨率
（宽高语义不撒谎），`is_proxy` 仅在真实缩小时为 True。色度目标 =
ceil(tw/2) 保持 4:2:2 结构。测试以独立 numpy 期望逐元素比对（bit-exact）。

### C-50 线程/生命周期契约（阶段 6 完成门槛）

单实例不可重入（ToposMovieFile 原生句柄不自带锁；上层
`SourceManager._decode_lock`/`ClipSession.decode_lock` 串行化，docstring
双向标注）。close 幂等且全路径释放（ToposMovieFile.close 释 movie+fd）；
open 失败路径先 close 再抛（factory open_source 再兜底）；`__del__` 兜底；
支持 close → 重开、上下文管理器。测试覆盖。

### C-51 error_handling 顺序表同步按内容路由（R-17 第二处硬编码）

`FallbackStrategy._backend_order = ['ffmpeg']` 是调研新发现的隐藏硬编码
（error_handling.py L533，计划未列）；改为 `_try_backend_switch` 按文件
路径经 factory auto 顺序计算，factory 异常时回退原字面量表。

## 2. 交付映射（计划 §12 阶段 6 验收）

| 要求 | 交付 |
| --- | --- |
| ToposMediaSource | `src/shared/media/topos_source.py`（Protocol + `read_next_frame`，EOF StopIteration 对齐 FFmpeg） |
| factory 注册/自动选择 | `_ensure_backends_registered` topos 项 + `MediaBackend.TOPOS` + `_get_backend_order(path)` 内容路由 |
| 映射 VideoMetadata | tpcC → 全字段（色彩/SAR/Alpha/field_order），fps 见 C-47 |
| DecodedFrame/PlaneInfo/10-bit planar | C-44/C-46；stride=行字节数（应用层语义，与 C ABI 元素 stride 已换算） |
| 复用 FrameProvider/DecodeWorker/缓存/generation/取消 | 未新增任何调度器：worker 依赖的 `read_next_frame`/hasattr 探测/`_decode_lock` 串行化假设全部对齐；generation/缓存机制在调度层不变 |
| close/seek/cancel/proxy 切换测试 | close 幂等/重开/上下文管理器、seek 游标语义、随机访问 bit-exact、损坏隔离、proxy 缩小/放大回退；磁盘代理切换复用现有 frame_provider_service 机制（见遗留） |
| 禁止 UI/Python 线程碰 OpenGL | 模块只产 numpy 平面（docstring 原则条目） |
| 导入/探测/逐帧/随机拖拽 | 18 项测试 + factory 探测链（`probe_video_metadata` → codec_name='topos'，hwaccel 门用） |
| 不新增第二套全局播放调度器 | 零新增线程/队列/调度 |
| 关闭/换片/取消后无 native 泄漏 | close 全路径 + `__del__` 兜底 + ToposMovieFile fd/movie 成对释放 |
| 预览与导出一致色彩元数据 | 同一 VideoMetadata 源（tpcC），未知码 'unknown' 不猜（R-13） |

## 3. 过程修正记录

- D-1（测试助手，已修）：`_write_movie` 初版漏调 `mux.finish()`，moov 未
  写出 → `tc_movie_open` 报「缺 ftyp 或 moov」。对照独立 repro 定位，
  非库缺陷。
- D-2（脚本层，无代码影响）：冒烟脚本文件名笔误（`/tmp/smoke.mov` vs
  `/tmp/smoke_topos.mov`）曾误判 faststart 回归；以绑定测试 13/13 通过 +
  隔离 repro 澄清。

## 4. 遗留（归属后续阶段）

- 磁盘代理生成：ffmpeg/PyAV 无法读 TPIC 源 → 代理生成须走
  ToposMediaSource 解码 → 编码链（阶段 8 encoder 就位后接线；当前
  内存代理/缩小路径已可用）。
- Alpha GPU 链（R-05/R-23/R-24）消费 `topos_yuva422p10a*` 名称与逐面
  bit_depth——阶段 7。
- 12-bit 色彩 / 其他位深的格式名随 profile 扩展递增登记（当前 v1 仅 10-bit）。
- VFR 素材的逐帧 pts 已正确（`DecodedFrame.pts` 来自 stts 展开），
  仅 fps 元数据按首帧近似（C-47）。
- `read_audio_samples` 恒 None（C-18；Topos v1.0 无音频轨）。

## 5. 门禁

- `tests/media/test_topos_source.py` 18 项（media_core 标记；
  C 库未构建整体 skip）纳入 `run_tests.sh` Python 段。
- 回归：`tests/media + tests/native` 1587 passed / 49 skipped /
  8 failed（全部为 HEAD 上既有的环境性失败：fixtures manifest/ProRes
  素材缺失，stash 对照证实与本阶段无关）。
- C 侧零改动（本阶段纯应用层），四配置 ctest 随 run_tests.sh 常跑。
