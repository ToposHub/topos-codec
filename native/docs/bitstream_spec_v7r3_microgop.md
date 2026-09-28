# 位流规范 V7-R3 —— 帧间微 GOP 载体（zero-motion IP-2）

状态：**正式规范面**（2026-09-21，产品化计划 E9；此前仅存于 ADR-C047/C048 与
topos_v9_micro_gop_plan）。规范家族：`bitstream_spec_v1.md` §13 版本规则。
产品档：**Topos 422 LP**（容器 tier 7，ADR-C056）。

## 1. 载体标识

| 域 | 值 | 说明 |
| --- | --- | --- |
| `version_major` | 7 | V7 熵载体族（与 V7-R2 静止图同族不同语义） |
| `entropy_mode`（cfg `reserved[0]`） | 8 | rANS2 上下文熵（V7-R2 产品默认熵） |
| 帧间性 | 包头 `frame_type`（0=I / 1=P） | 逐帧自描述；tier 不承载帧间性 |

载体字节契约：packet 偏移 6 = `major(=7)`、偏移 45 = `em(=8)`（与
`TestV7R3MicroGop` 钉扎一致）。

## 2. GOP 语义（zero-motion IP-2）

- GOP 长度 2：`I, P`；P 只参考**同 GOP 紧邻上一帧**（prev_sync 定位）。
- P 帧为 zero-motion 残差：无运动矢量语法；残差经与 I 帧同一变换/量化/
  rANS2 熵核。切镜/高残差时编码器决策回退 I（`force_intra`）。
- P 参考帧缓存有界（decoder GOP context，topos_source v9_micro_gop 路径
  批 5 交付）；`tc_gop_context_encode_frame` 首帧必须 I
  （`first frame must be I`），P 直启拒绝。
- 零漂锚：同输入 I/P 序列解码逐位可复现（`TestV7R3MicroGop` 零漂断言）。

## 3. 显式收缩（编码器 fail-fast，禁静默降级）

| 收缩项 | 值 | 依据 |
| --- | --- | --- |
| Alpha | **no-alpha**（alpha cfg → `NOT_IMPLEMENTED`，信息含 no-alpha） | micro-gop 计划 §3.7 |
| scaled decode | 不支持（`scaled_decode=false`） | 同上 |
| 码控 | 无 sized/反馈码控；固定锚 qp=72（ADR-C050） | P/I 决策在原生侧 |

## 4. 帧头与色彩域

- 帧头 `profile = 3`（4:2:2 10-bit）；`pixel_format = 0`；`bit_depth = 10`。
- 色彩标签域与 V1 同表（§A.7/H.273 对齐；含 H1 的 transfer=13 语义——
  载体 major≥7 时 `version_minor` 恒 0 自描述，v1.x minor 扩展代不适用）。
- 逐项校验复用 `frame_header.c` 单一事实源；`version_major=7` 分流先于
  v1.x minor 域检查。

## 5. 容器与档位

- 容器：MOV（tpcC track 配置）；tpcD `tier_id = 7`（TC_TIER_LP）。
- 相邻扩展：**Topos RAW**（视频）使用同一 rANS2 载体族、帧 `profile=7`/
  `pixel_format=3`/bd∈{12,16} + transfer 冻结对（12→LOG0/16→linear），
  容器 `tier_id = 8`（TC_TIER_RAW，v1.9 域扩展 0..8；D3 单 tier 参数化）。
  RAW 帧序恒全 I（intra；CFA 不进微 GOP 参考）。

## 6. 互操作

- 读侧：不支持 major=7 的旧解码器按 `UNSUPPORTED_VERSION` 干净拒绝；
  V7-aware reader 对未知语义 fail closed（见 bitstream_spec_v7 §8）。
- 字节冻结：既有 golden（V1/V2/…）不受影响；V7-R3 语义由
  `TestV7R3MicroGop` 与 golden_mov_v73 钉死。
