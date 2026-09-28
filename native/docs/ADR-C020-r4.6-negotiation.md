# ADR-C020：R4.6 —— 应用层能力协商收口（R4 整轮完成）

- 日期：2026-08-30
- 状态：已接受（实施完成）
- 上下文：`docs/Topos_V2.0_完成度审计与整改计划_2026-08-30.md` §4 R4
  子阶段 6（"Python binding、MediaSource、GPU upload、导出和 UI 能力协商"；
  六子阶段严格串行）——本 ADR 后 R4 整轮完成
- 关联：ADR-C015…C019（R4.1–R4.5 各格式/档位/差分交付）、ADR-C006/C008
  （阶段 6/8 应用接入原始决策）

## 1. 背景

审计 R4 第 6 项要求新能力贯通到应用协商面。逐项盘点：Python binding
（movie_config pf/profile、ToposFrameInfo.pixel_format——R4.2/4.4 已交付）、
MediaSource（topos_source 命名/几何/元数据——R4.2/4.3/4.4 已交付）、
GPU upload（_YUV444_FORMATS/_RGB_PLANAR_FORMATS 路由——R4.2/R4.3 已接线）。
本轮收口**剩余的导出/UI 注册点**：ExportCodec 枚举、timeline codec_map、
deliver 容器矩阵与 UI 列表——遗漏任一处 = UI 选不到 444 档或静默回退。

## 2. 决策

### C-144：六档进导出枚举与 codec_map

`ExportCodec` 新增 `TOPOS_PRO444`/`TOPOS_EXTREME`；codec_map 默认
pix_fmt：Pro444 → `yuv444p10le`、Extreme → `yuv444p12le`（tier 格式
交叉的 UI 侧默认值；用户可改 gbrp* / topos_*a{N} 变体，encoder 配置期
校验兜底）。quality/crf 旋钮路径（codec=='topos'）对六档统一生效。

### C-145：deliver 矩阵与 UI 列表

`video_export_formats` 容器表 + `video_export_job` codec 映射 +
`video_export_settings_panel` UI 列表追加 `topos_pro444`/`topos_extreme`
（自研 MOV only，Alpha 可选，质量控件关闭——与既有四档一致）。
UI 与 preflight 矩阵一致性由既有 `test_ui_matrix_and_preflight_matrix_agree`
自动覆盖（新键进表即被双向校验）。

### C-146：GPU 路由回归钉住（非新接线）

`TestFormatClassification` 追加 444/GBR topos 名断言
（topos_yuva444* → 'yuv444'、topos_gbrap*/gbrp* → 'rgb_planar'）。
R4.2/R4.3 已完成实际接线，本断言把路由表成员关系钉进回归网
（端到端渲染验证仍以 classify 路由 + 既有渲染测试覆盖）。

## 3. 验证

失败先行：`TestRegistrationPoints`（枚举 + codec_map）、
`test_topos_444_tiers_in_container_matrix` 三处先行失败，实现后全绿
（GPU 路由断言首跑即绿——属回归钉住，见 C-146）。codec 相关 Python
套件 93 项通过；广域回归 deliver+video 1 / media 8 / edit 11 与基线
集合逐项一致；完整 STAGE-10 门禁见 run_tests.sh。

## 4. R4 整轮小结（2026-08-30）

R4.1 12-bit（v1.2）→ R4.2 4:4:4（v1.3）→ R4.3 GBR（v1.4）→ R4.4
profile 5/6 + 六档收口 → R4.5 SIMD 差分矩阵 → R4.6 协商收口。
审计完成门槛达成：六档 capability 无占位，header/query/UI/SDK 一致
（spec v1.4 / ADR-C015…C020 / SDK 与 native README 同步）。

## 5. 遗留

- R5：MOV 与媒体元数据收口（atom 冻结/fixture/oracle/纯视频预检声明）。
- 导出 UI 的 pix_fmt 选择器（444 档下 gbrp/a{N} 变体的显式选择控件）
  目前以 codec_map 默认值 + 高级配置覆盖，专属控件随 R7 UI 轮评估。
