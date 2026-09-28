# ADR-C023 — Pro444/Extreme 档位差异化正式定案（矩阵不独立）

> 日期：2026-08-31（复验整改 O2）
> 状态：已接受（取代 ADR-C018 中"Pro444/Extreme 独立矩阵/码控"的原始任务表述）
> supersedes: ADR-C018 §档位激活中关于"独立量化矩阵"的预期
> 关联：复验报告 P1-06 / P2-09；bitstream_spec v1.4；risk R-21

## 背景

R4.4 激活 profile 5/6 时，六档 `qmatrix_id` 恒为 1，native 只按
`qmatrix_id` 查表（与 profile 无关）。复验最小复现 8 证实：同输入同 QP 下
profile 3 与 profile 5 的码流除帧头 profile 字节与 header CRC 外逐字节相同。
原 R4 任务中的"Pro444/Extreme 独立量化矩阵"从未实现，但测试/ADR 未明确
承认这一点——构成能力声明漂移。

## 决策

**Pro444/Extreme 的产品差异化 = 格式约束 + 应用码控，不引入独立量化矩阵：**

- Pro444（profile 5）：强制 4:4:4（YUV 或 GBR）10/12-bit，应用侧
  `target_bpp` 定标更高（37 MB/s 参考中值）；
- Extreme（profile 6）：强制 4:4:4 + 12-bit，`target_bpp` 56.5 MB/s；
- 全部六档量化矩阵维持 `qmatrix_id=1`（v1 冻结矩阵）；
- 档位语义在 capability summary 中按档区分：profile 3 四档为**质量预设**
  （`is_preset=True`），profile 5/6 为**独立帧头 profile**（`is_preset=False`）。

## 理由

1. **bit-exactness 约束**：V2.0 码流与 golden 冻结（golden_bitstream /
   golden_codec / golden_mov），为 profile 5/6 引入新矩阵表会改变全部
   profile 5/6 码流字节，破坏已交付的确定性保证；
2. **无质量证据**：现有矩阵（id=1）在 10/12-bit 全码率区间的质量-码率
   表现未显示出需要按档分表的实证缺口（depth12 报表同 qp 对照）；
3. **格式即差异化**：4:4:4 + 12-bit 本身已构成画质/用途差异（VFX/HDR），
   码率由 `target_bpp` 定标承担。

## 重评条件

出现以下证据时重评（届时走 minor 版本 + 新 qmatrix_id 注册，不改动
id=1 既有流）：

- 实测显示 4:4:4 高码率区（>30 MB/s @1080p25）下 id=1 矩阵的感知质量
  存在可量化的档位间缺口；
- V2.1 及以后需要视觉无损档位的率失真优化。

## 后续动作（本轮已执行）

- capability summary `bitstream_profiles` 显式列出 profile5/6（同一张表），
  `tiers_are_presets=False` + 逐档 `is_preset`（P2-09）；
- binding `make_movie_config` 对非法 profile/pixel_format/bit_depth/
  alpha_mode/alpha_bit_depth 显式抛错（P1-07）；
- tpcB 增加 `ratio_saturated` 标志位（0x0008，v1.3，P1-11）；
- Quick Export / deliver controller / render cache / i18n 六档收口 +
  未知 codec 显式失败（P1-08）；
- GBR 解码平面组件 `G/B/R/A` + `output_mode='planar_rgb'`（P1-09）。

## 图片层修订（2026-09-07）

本 ADR 的“全部六档 qmatrix_id=1”结论适用于视频 profile 3/5/6 的 MOV
输出。图片层新增 `qmatrix_id=2`（Topos 444 Compact），仅允许
GBR 4:4:4、12-bit、profile 5/6，由 `toos encode --tier 444-high|444-ultra`
使用；视频导出路径保持 qmatrix_id=1。Compact 表只提高中高频量化步长，
不改变 profile、平面布局或解码算法，目标是把 1080p 图片 444 档控制在约
4 MB/帧。图片层的详细决策与实测数据见 `docs/image/ADR-I012-444-compact-matrix.md`。
