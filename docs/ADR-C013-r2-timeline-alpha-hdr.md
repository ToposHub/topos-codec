# ADR-C013 — R2 真实时间线 Alpha/HDR 导出链（2026-08-30 审计整改）

> 状态：已采纳（R2 交付）
> 输入：审计 §3 P0-1/P1-2 + R2 任务清单；测试先行（修复前
> `test_topos_timeline_export.py` 7 failed / 3 passed，修复后 10/10）。

## 1. 背景

审计确认 Deliver 的 Alpha/HDR 选项此前只是配置字段：`has_alpha` 从未到达
渲染侧，合成缓冲恒 3 通道、`_extract_frame_data` 在三处切片丢 alpha、
comp clip 场景线性帧被无条件 `clip[0,1]` 压平高光。本轮侦察另发现四个同类
问题：层 alpha 归一化硬编码 `/255`（uint16 层错 257 倍）、FFmpeg 编码器
四通道整轴 flip（BGRA→ARGB，与 R1 P0-2 同类）、YUVA planar 源与图片源的
alpha 在提取处丢弃、caption graphic 层预乘压平。

## 2. 决策

### C-103 Alpha 开关贯通渲染侧 + RGBA 合成缓冲

- `export_timeline` 记录 `_export_has_alpha = encoder_config.has_alpha`
  （与 `_use_high_precision_export` 同生命周期，finally 复位）；
- 勾选 Alpha 时 `_composite_layers` 工作缓冲为 4 通道 float32：
  **内部预乘表示、对外 straight**（量化前 `_unpremultiply_rgba`），
  层合成从"不透明底 opacity lerp"改为 **Porter-Duff over（自下而上，
  层永远盖在已累积内容之上）**；空时间线/黑帧 = 全透明黑；
- **未勾选 Alpha 时行为逐字节不变**（3 通道 opacity lerp、不透明黑帧）——
  7177 项 edit 套件零回归依赖此约束；
- wipe 预合成注入与 dip-to-color 在 4 通道下按预乘语义等价重写。

### C-104 straight-alpha 全链契约（审计 R2 任务 2）

| 参与方 | 输出契约 |
| --- | --- |
| RGBA/BGRA 解码帧（FFmpegMediaSource） | straight，第 4 通道保留 |
| teaching animation | straight BGRA（既有） |
| caption graphic 层 | Alpha 导出下改输出 straight BGRA（不再预乘压平） |
| 字幕烧录 | `composite_over` straight over，尊重底图真实 alpha（4ch 分支） |
| comp clip 层 | 渲染器只回 BGR（alpha=1），留后续扩展 |
| 调整层（Adjustment Clip） | 作用于预乘合成缓冲（线性算子在预乘域正确；文档化） |
| 编码文件语义 | `alpha_premultiplied=False`（straight，R1 C-99 契约） |

### C-105 帧边界 4 通道贯通（审计 R2 任务 3）

- `_extract_frame_data(keep_alpha)`：bgra/rgba/rgba64 三分支保留第 4
  通道（rgba64 同步降 8-bit）；planar 路径 `_convert_yuv_planes_to_bgr
  (keep_alpha)` 与 16-bit 变体附加 YUVA 源 alpha 平面；图片改
  `IMREAD_UNCHANGED`（灰度/灰度+alpha 归一）；
- **修复隐藏 bug**：层 alpha 归一化按 dtype 选满刻度
  （`_normalise_alpha_channel`，此前硬编码 /255）。

### C-106 编码器侧四通道（含 FFmpeg 同款翻转修复）

- `_encode_frame` 分派 `bgra` / `bgra64le`（Topos R1 路径与 FFmpeg
  `startswith('bgr')` 分支均消费）；
- `FFmpegVideoEncoder._convert_packed_frame`：四通道显式 `[2,1,0,3]`
  重排（整轴 flip 会把 BGRA 变 ARGB）；高位深 4ch 用 `rgba64le` 保
  alpha（此前误用 rgb48le 静默丢）；
- **BT.2020 矩阵 + Alpha 输入显式 RuntimeError**（numpy 精确转换无
  alpha 承载路径）——显式失败优于静默丢弃，实现列入遗留。

### C-107 HDR 显式色彩变换（审计 P1-2）

- **Topos 编码器浮点输入契约 = 信号域 [0,1]**（OETF 由调用方完成）：
  下界裁负、上界不预裁；>1.0 触发一次性警告后在码值级量化天花板可见
  裁剪——不再是无条件静默 clamp；
- comp clip 场景线性回读（`linear_f32`）经 `_linear_bgr_to_export_signal`
  显式 OETF（`transform_color(LINEAR_SCENE → 导出描述符)`，与
  composite_render_output 同一范式）后按交付位深量化（高位深 16-bit，
  修复此前恒 8-bit 的二次损失）；转换失败回退显式 clip + 警告；
- 事实记录：SDR 传输（bt709 等）无 >1.0 表示域——线性高光必须经 OETF
  （PQ/HLG 有绝对刻度头寸）或显式接受天花板裁剪；时间线常规层的
  display-referred [0,1] 信号不受影响。

### C-108/109 字幕、遮版与降噪的 4 通道语义

- 字幕烧录 4ch 分支：底图 alpha 参与 over，输出保持 4ch（直方与位深
  均不破坏）；
- program guide matte 在 Alpha 导出下仅作用于颜色通道（alpha 原样）；
- 导出降噪桥（3 通道纹理设计）在 4ch 输入下仅对颜色通道降噪。

## 3. 明确不做 / 遗留（如实清单）

- **FFmpeg BT.2020 + Alpha 精确转换**：显式报错（C-106）；HDR10+
  ProRes 4444 Alpha 组合待实现（影响：该组合须改用 Topos 或非 2020
  矩阵）；非 BT.2020 的 ProRes 4444 Alpha 路径已有端到端测试锁定；
- 调整层 alpha 感知混合：现按预乘缓冲线性处理（数学正确但与 3ch 路径
  的 blend 语义差异未单独验证）；effect（blur/glow）在 straight 4ch 层
  上未预乘先混（硬边缘可能有极轻微 halo）——均文档化，按需迭代；
- PNG/TIFF 序列的时间线导出仍映射 ProRes（既有行为，非本轮范围）；
- comp clip 渲染器 alpha 通道回读（现为 alpha=1 层）。

## 4. 验证

- 测试先行：`tests/media/test_topos_timeline_export.py` 前 10 用例修复前
  7 failed（alpha 全不透明 4095、字幕 4ch→3ch、linear 助手缺失、浮点
  无警告）/ 3 passed（回归钉：无 Alpha 路径、取消、HDR 元数据）→
  修复后全绿；
- 第 11 用例（FFmpeg ProRes 4444 + Alpha 端到端）锁定 R2 后**新可达**
  路径的可用性：rgba64le 重排/reformat → 解码 `yuva444p12le` 4 平面、
  alpha 梯度按位深保真（该组合在 R2 前 alpha 于合成层即被丢弃）；
- 审计命名的五个用例全部落地且数值断言：alpha roundtrip（a12 误差
  ≤64 = 1 LSB 量级）、premultiplied 边缘无镶边（Y 期望 ±20）、
  composite over（三层取样点 ±15）、HDR 高光（BT.2020+PQ 元数据
  roundtrip + 高光/暗部对比 >600）、取消资源释放；
- codec Python 矩阵 88/88；全量门禁 `run_tests.sh`（Python 段已纳入
  本文件）+ 广谱回归 media/video+deliver/edit 与基线逐类一致
  （edit 7177 通过零新增——"未勾选 Alpha 行为不变"约束成立）。

## 5. 风险变化

- R-05（Alpha 链路丢失）：导出链闭环——编码边界（R1）+ 时间线合成/
  提取/字幕/分派（R2）全链有数值断言；
- 新增 R-29：FFmpeg BT.2020+Alpha 组合显式不可用（遗留跟踪）。
