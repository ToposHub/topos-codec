# ADR-C008 — 阶段 8：编码、代理、缓存与 Deliver 接入

- 日期：2026-08-29 ｜ 状态：已采纳
- 输入：ADR-C001 C-14（编码接入施工图 + 8 登记点清单）、risk_register
  R-16/R-13、计划 §2.1/§2.2（六档与 Alpha 策略）§12 阶段 8、
  ADR-C005（自研 MOV 容器）、ADR-C006/C007（解码与 Alpha 链）。
- 交付：`src/shared/export/topos_encoder.py`（ToposVideoEncoder）、
  `src/shared/codec/topos_profiles.py`（六档 capability schema）、
  `src/shared/codec/topos_binding.py` 色彩码表、timeline_export 分派与
  取消 abort、8 处登记点、`proxy_generator.py` TPIC 代理链、
  RenderCacheEncoder topos 档、`tests/media/test_topos_export.py`（29 项）、
  `run_tests.sh` STAGE-8 门禁。
- 决策编号：C-59…C-66（承接 ADR-C007 的 C-58）。

## 1. 决策

### C-59 duck-type 对齐 FFmpegVideoEncoder，不做 encoder 抽象基类

首版不强行抽象（C-14 结论维持）：ToposVideoEncoder 与 FFmpegVideoEncoder
同构造签名 `(output_path, FFmpegEncoderConfig)`、同生命周期
`open/encode_frame(DecodedFrame)/close`、同进度接口
（`get_progress()` 返回已编码帧数）、同属性（`frame_count/is_open/
output_path/config`）。新增 `abort()`（取消丢弃语义，见 C-61）——FFmpeg
编码器无此方法，调用方按 `hasattr` 探测，不破坏既有契约。

### C-60 双输入形态：planar 直通（免转换）+ packed numpy 正向转换

- **planar `yuv422p10le` / `topos_yuva422p10a*`**：满刻度 uint16 平面
  tobytes 直送 native 编码器——Topos 源再编码无 RGB 往返（中间片的核心
  收益）；几何/dtype 校验失败显式 RuntimeError。其它 planar 布局
  （420/444）**拒绝而不是静默重采样**（质量语义损失必须由调用方决策）。
- **packed BGR24/BGR48[BG]（时间线合成路径）**：numpy 正向矩阵
  RGB→YUV422p10（BT.709/BT.601/BT.2020nc；limited 跨度按 2^(n−8) 精确
  位移；4:2:2 水平 box 子采样奇宽边缘补列）——数学与
  FFmpegVideoEncoder._convert_rgb_to_yuv_bt2020（P0-COLOR-06）同源。
- Alpha：planar 第 4 平面直通；packed 第 4 通道量化到档位 Alpha 位深；
  `has_alpha` 且输入无 Alpha → 不透明写入 + 单次警告（显式，非静默）。

### C-61 临时文件 + 原子替换 + abort：取消/失败不留伪成功文件

- 编码始终写 `<output>.topos-tmp`；`close()` 仅在 **≥1 帧 + finish() +
  os.replace** 全部成功后才产出目标文件；0 帧定稿直接 RuntimeError
  （拒绝产出空文件）。
- `abort()`：mux 不 finish 直接关闭 + 删临时文件（用户取消路径）。
- `TimelineExporter` 主循环取消分支改为 `abort()` 优先（`hasattr` 探测），
  异常路径同样 abort——FFmpeg 编码器不受影响（无 abort 即走 close）。
- RenderCacheEncoder.abort 同步优先 abort（其 temp/replace 语义保留）。

### C-62 音频：v1 容器纯视频，显式警告忽略（不静默丢弃）

> **v1.1 修订（2026-09-19，ADR-C051）**：topos 容器解冻音频轨后，本决策的
> "纯视频忽略"分支仅对 GIF/图片序列生效（这些编码器不含音轨）；topos
> 导出写真实音轨（lpcm/mp4a）。
>
> **v1.4 再导入边界修订（2026-09-20，M-B3，plan §3）**：**用户把导出的
> topos 文件重新导入时间线时，该文件即 master**——回放读它自己的音轨
> （`AudioDecoder.open(path)` 纯 PyAV 直开路径，ffmpeg mov 通用 demuxer
> 直接读 lpcm/mp4a 双轨；再导入回放闭环见
> `tests/media/test_topos_reimport_playback.py`：lpcm 16bit 域逐位相等、
> AAC 互相关 > 0.9 零手动对齐——v1.4 elst 使解码自动裁剪 priming）。
> 本修订不改变"渲染缓存/素材代理纯视频"边界：代理是下游派生物，回放
> 音频仍取 master（本条语义原样适用于代理承接的场景）。

`add_audio_stream`/`write_audio_samples` 原实现为警告 + no-op（capability
schema `audio: false`）。选择忽略而非失败：中间片格式工作流（ProRes 母版
同类）音频走独立交付；但必须可诊断——首次调用 logger.warning 明确说明
"音频轨将被忽略（不写入输出文件）"，UI/文档同步披露。

### C-63 六档 capability schema：四档 QP/rate 可用、两档如实标注不可用

`src/shared/codec/topos_profiles.py`（中立层，media 代理与 export 编码
共同消费）：六档全部显式声明（tier_id/像素格式/位深/target_bpp/参考
数据率区间/Alpha 预算与硬上限/available/unavailable_reason/use_case）。
计划 §2.1 参考数据率（1080p25）中值换算为 **bits-per-pixel** 目标 →
`tc_frame_encode_sized` 帧级确定性 qp 搜索（阶段 4 已验证的码控路径），
随分辨率线性缩放。`pro444`/`extreme` 需 4:4:4/12-bit profile——
`available=False` + 明确原因，open() 时拒绝（UI 不展示为可选）。
`config.crf`（0–63）提供固定 QP 覆盖（quality_mode crf/auto）。

### C-64 8 处登记点 = 4 个 ExportCodec 成员（DNxHR 先例模式）

`TOPOS_PROXY/TOPOS_LT/TOPOS/TOPOS_HQ`（value: topos_proxy/topos_lt/
topos/topos_hq）——一档一成员复用 DNxHR 全部现有 plumbing（profile 三元
组、持久化字符串、UI 列表），不引入新的 profile 下拉机制。8 处登记
（R-16 清单，实际行号以代码为准）：ExportCodec 枚举、_get_codec_settings
（("topos","yuv422p10le",tier)，**不再落入 prores 兜底**）、
CODEC_CONTAINER_TABLE（仅 mov——自研容器无 mkv/mxf 写出）、_map_codec、
_CODEC_CATALOGUE（alpha=True 全档、crf=False）、_CONTAINER_CODECS（mov）、
perform_video_export codec_map（**不再静默回退 H264**）、i18n codec_topos*
（zh_CN/en_US）。`TestRegistrationPoints` 逐项断言（含 deliver 分派源码
检查）——R-16 关闭。

### C-65 TPIC 磁盘代理：自研 decode→（box 缩小）→自研 encode，输出同为 TPIC

`ProxyGenerator.generate()` 前置 `is_topos_movie` 内容嗅探 → 专属分支：

- 探测：`ToposMediaSource.probe()`（PyAV/ffprobe 对 TPIC 均不可用）；
- 生成：`ToposMediaSource(target_size=…)` 解码（阶段 6 分离 box 滤波即
  缩小路径）→ proxy 档 bpp `encode_sized` → `ToposMuxFile`；
- **编码尺寸取自解码平面**而非 probe（probe 报源原始分辨率；缩小后与
  movie_config 静默错配是测试抓出的真 bug，D-7）；
- 输出为 TPIC .mov → 播放侧 factory 内容嗅探自动路由（零改动），且比
  ProRes 代理更优（planar 满刻度直达 GPU，无封装内转码）；
- 取消/失败删除半成品；key/命名/失效复用现有 DiskProxyConfig 机制。

Alpha 透传（源有 Alpha → 代理 mode2 a12 保留）。

### C-66 持久化：codec 字符串走现有 schema v3；Alpha 转发补齐；不 bump 版本

- `VideoConfig.codec`（字符串）round-trip 已验证（schema v3 容忍未知值）；
  topos 参数暂无一等字段需求 → `extra` dict 兜底，**不 bump
  VIDEO_EXPORT_JOB_SCHEMA_VERSION**；工程内容 schema 不动（Topos 产物不
  嵌入 .topos 工程文件）。
- 补齐既有缺口：`to_timeline_export_config` 此前不转发 `VideoConfig.alpha`
  ——新增 `TimelineExportConfig.has_alpha` 字段并转发（Deliver 页 Alpha
  复选框首次真正生效）；`_create_encoder_config` 汇总显式开关与 yuva*
  pix_fmt 隐含两种来源。
- 渲染缓存：`RenderCacheSettings.codec='topos'` 经 `_parse_codec` 映射 +
  RenderCacheEncoder 分派（PyAV 依赖移出 topos 分支）。

## 2. 完成门槛对照（计划 §12 阶段 8）

| 门槛 | 证据 |
| --- | --- |
| 预览和导出颜色一致 | 同一 timeline 色彩元数据驱动 GPU 预览与导出 tpcC 标签；转换数学与 FFmpeg 路径同源（C-60）；roundtrip 测试验证 tag/limited-range |
| 取消不留伪成功文件 | C-61：encoder 级 temp/replace/abort + timeline 取消 abort 分支；`test_abort_and_zero_frame_leave_no_output` |
| Alpha profile 不被静默压平 | planar YUVA 直通 + `has_alpha` 语义 + 丢弃显式警告；`test_alpha_passthrough_not_flattened` / `test_alpha_plane_dropped_explicitly_without_flag` / `test_frame_provider_to_encoder_alpha_roundtrip`（双重生成 alpha 有界误差） |
| 工程重开后参数完整保留 | `test_job_persistence_roundtrip`（codec/alpha/extra 经 to_dict→from_dict 完整保留，schema v3） |
| 失败明确诊断、禁止静默回退 | 库缺失/档位不可用/色彩未知/planar 布局不支持均 RuntimeError 带原因；_get_codec_settings 与 codec_map 不再兜底 |

## 3. 修正与偏差记录

1. **D-6（render_cache 导入顺序）**：topos 分支先于 FFmpegEncoderConfig
   导入使用 → UnboundLocalError；导入上移到分派前。
2. **D-7（代理编码尺寸错配，测试抓出的真 bug）**：`_run_topos` 初版用
   `probe()`（源原始分辨率）配置 movie_config，而解码平面是 target_size
   缩小后的——native 编码器不校验平面/声明尺寸一致性，会静默产出声明
   尺寸错误的流。修复：编码尺寸取自首帧解码平面。
3. **D-8（quick_export i18n 键前缀，既有缺陷顺带记录）**：
   `_codec_display_key` 生成顶层键而 I18N.tr 按点号嵌套导航，Quick Export
   对所有 codec 实际显示 `codec_id.upper()` 兜底（如 PRORES_422HQ）。
   本阶段按既有约定把键放 `export` 段（与 codec_dnxhr_* 一致）；前缀
   修复属独立 UI 议题未纳入。
4. **D-9（tests/edit 既有段错误）**：`test_audio_waveform` 的 Qt 线程
   时序段错误在未修改 HEAD 上复现（exit 139）；回归以
   `--ignore=tests/edit/test_audio_waveform.py` 补完其余 7197 项。

## 4. 遗留

- Deliver 预检（video_export_preflight）对 topos 的深度校验（如原生库
  可用性预检）——当前 preflight 走通用 codec/container 检查；
- Topos 导出 UI 的 QP 覆盖控件（quality plumbing 已通，档位码控默认）；
- 代理 manifest 基础设施接线（ProxyManifest/AssetProxyManager 已有未接，
  非 topos 特有）；
- 4:4:4/12-bit profile 扩展（pro444/extreme 解锁）与 V2.1 Inter；
- Quick Export i18n 前缀缺陷（D-8）。
