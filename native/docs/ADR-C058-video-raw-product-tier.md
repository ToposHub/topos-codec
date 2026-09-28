# ADR-C058：视频 RAW 产品化——Topos RAW 产品档注册（tier 8）

日期：2026-09-21 · 状态：**已采纳**
关联：`docs/float_toos_and_raw_productization_plan_2026-09-21.md` §5 C-视频（R5–R12）、
`docs/codec/topos_traw_format_plan_2026-09-12.md`（TRAW 批 0~5：bitstream profile 7
冻结契约）、ADR-C056（LP：tier 域扩展先例）、container_spec v1.8→v1.9、
ADR-C021（MOV 追加原子模式）、RC1（TRWM 开发元数据块）

## 1. 背景

TRAW（bitstream profile 7：CFA 相位平面 R/Gr/Gb/B，bd∈{12,16}，transfer 冻结对
LOG0(20)/linear(8)）自 2026-09-13 批 0~5 完成压缩/容器/回放底座，但只是编码器
内部能力：容器无 RAW tier、encoder CLI 硬编码 profile 3、应用内无导出入口、
无产品档位表。图片 RAW 产品化（R1–R4）先行落地后，视频线沿用同一张
12 档表（`raw12-*`/`raw16-*`）完成档位注册。

## 2. 决定

**D1（容器 tier）**：新增 **TC_TIER_RAW = 8**，tpcD tier_id 域 0..7 → **0..8**
（`mov.c` setter + parse 同步校验），container_spec 升 **v1.9**。纯域扩展：
golden / 既有产物字节不变（LP 同型先例，ADR-C056 D2）。tier 9 两端显式拒绝
（INVALID_ARGUMENT / MALFORMED）。

**D2（单 tier + 参数化，产品计划 D3 拍板）**：不开 12 个 tier id。产品档
`Topos RAW <bd>-bit <N>:1`（N ∈ {2,4,6,8,12,16}）由**位深 × 比率二级选择**
驱动，锚 qp 走注册表比率表（与图片 CLI `kTierPresets` / 图片 manifest 同表：
raw12 → 45/59/64/66/68/70，raw16 → 51/72/76/78/80/82）。比率是 **CQ 定位
标签**：体积随内容浮动，不承诺精确比率（TRAW plan §1.1.1 冻结；UI 副行显示
标称数据率而非承诺比率）。

**D3（编码链收缩，fail-fast）**：raw tier ⟹ 强制**全 I 帧**（`gop='intra'`）、
no-alpha（TRAW 与 alpha 互斥，编码器双重拦截）、拒 AQ/RDO/v2 熵；bd≥13 ⟹
rans2。输入域 = `topos_cfa12le/16le` 4 相位平面直入（packed RGB 拒绝换头）；
码值域校验对齐 planar 路径（越流位深满刻度显式失败）。载体 V1/V7 帧内流
（V8/V9 载体拒 CFA）。

**D4（开发元数据）**：MOV `trwm` 原子（ADR-C021 追加模式）承载 as-shot
WB/EI/black level/cfa_layout/sensor_matrix_id（RC1，40B CRC 载荷）；导出端
`raw_meta_payload` 写入（含 R2 RAW→RAW 直出透传），读侧暴露
`extra['topos_trwm']` 作为应用层 as-shot 基线（RC4）。faststart 保留。

**D5（交付双路径，与 R2/RC6 的边界）**：

- **RAW→RAW 直出**（R2）：源即 CFA、同位深同几何、无任何像素级处理——
  fail-closed 门禁（`build_direct_topos_delivery_plan`），保留 RAW 语义；
- **debayer 交付**（RC6）：有处理/常规档输出经应用层 debayer→IDT gamut
  （用户 RAW 参数 > TRWM as-shot）→ 工作空间 BGR 再编码；RAW 域 transfer
  标签不透传到再编码输出（回退 SDR）。

debayer 是**应用层**职责（native 只压缩相位平面）——与 ProRes RAW 的
"容器携 RAW、宿主解马赛克"分工同构。

**D6（命名，N-3 总纲）**：规格名 `Topos RAW 12-bit 4:1` 形式（位深必带
`-bit`、比率必带 `:1`）；与图片线共用同一张表，图片侧文件内显示名走 N-6
小写形式（`toos RAW 12bit 4:1`）。

## 3. 后果

- 视频 RAW 有正式容器/规范/产品面（tier 8 + v1.9 + 本 ADR + 白皮书 §4.6）；
- 首版无音频（全 I 帧无 GOP 分段冲突；音频能力随 ADR-C053 全局状态）；
- CLI 侧 `--profile 7 --pf 3 --bd 12/16` 直达（raw 12 档预设表暂驻 Python
  注册表，C CLI 补档为独立跟进项）；
- 读端 `topos_source` pf=3 探测/解码/几何 + GPU debayer 链复用（图片 RAW
  体验一致）。

## 4. 验证

`tests/unit/test_mov.c`（tier 8 往返 + tier 9 双端拒绝）、
`tests/media/test_topos_export.py::test_traw_mov_roundtrip`（编码→tpcD
tier 8→probe→4 平面解码）、`tests/media/test_topos_raw_direct.py`（直出
资格矩阵 + RAW→RAW e2e）、`tests/media/test_cfa_gamut_debayer.py`（debayer
交付出口 + RAW 参数三链优先级）。
