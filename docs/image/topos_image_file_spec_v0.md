# Topos Image（.toos / TPIM）文件格式规范 v0

状态：**阶段 0 冻结候选**（ADR-I001 记录决策；实现前本文即为契约）。

关联文档：
- 计划输入：`docs/Topos Image（TPIC）静态图像格式与序列管线开发计划书.txt`
- 决策记录：`docs/image/ADR-I001-stage0-decisions.md`
- 内层 codec 规范：`docs/codec/bitstream_spec_v1.md` / `bitstream_spec_v2.md`
- codec 能力真相源：`docs/codec/capability_manifest.json`

---

## 1. 范围与分层

`.toos` 文件 = **TPIM File Envelope**（本规范）内嵌**一个完整 TPIC elementary frame packet**（现有 codec，规范见 bitstream_spec）。

职责分离（冻结）：

| 层 | 负责 | 不负责 |
| --- | --- | --- |
| TPIM 文件层 | 尺寸、目录、chunk CRC、图片语义（窗口/Alpha/色彩元数据）、扩展 | 任何像素编解码数学 |
| TPIC codec 层 | 平面压缩、slice、量化、熵编码、重建、frame CRC | 文件路径、扩展名、ICC、序列帧号 |

文件层**必须复用** codec 的 CRC-32（IEEE 802.3，`tc_crc32`）、checked arithmetic（`checked.h`）与错误模型；禁止第二份实现。

## 2. 全局规则（冻结）

1. 所有多字节整数 **big-endian**；
2. 禁止序列化 C struct，必须按固定 offset 显式 pack/unpack；
3. offset/size 使用 u64；所有来自文件的 offset、size、count、尺寸、元数据长度必须先做 checked arithmetic 与硬上限校验（§10）；
4. 所有 chunk 必须落在 `file_size` 内、互不重叠、不指向 preamble 或 directory 自身；
5. CRC-32 = IEEE 802.3（反射多项式 0xEDB88320，init=0xFFFFFFFF，xorout=0xFFFFFFFF，"123456789" → 0xCBF43926），与 codec 现行定义一致；
6. CRC 只用于损坏检测，不提供密码学认证；
7. 未知 **optional** chunk：跳过；可编辑 round-trip 中尽量保留（字节原样）；
8. 未知 **critical** chunk：返回 `TC_IMG_ERR_UNKNOWN_CRITICAL`，不得猜测；
9. 同一语义只允许一个权威 chunk；重复 critical chunk → `TC_IMG_ERR_BAD_DIRECTORY`；
10. 解析 preamble/directory/IDSC 阶段**不得分配像素平面**；
11. 解析校验顺序（冻结）：文件长度 ≥ 64 → preamble magic → preamble CRC → directory offset/size 越界 → directory CRC → 逐 entry（越界/重叠/乱序/重复 critical）→ 必需 chunk 存在性 → IDSC 字段规则 → IDSC↔PIXL 交叉校验。任一步失败立即返回，错误码见 §11。

## 3. 64-byte Preamble

| Offset | Size | 字段 | 类型 | 规则 |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | magic | u8[4] | `TPIM`（0x54 0x50 0x49 0x4D）；不匹配 → `TC_IMG_ERR_BAD_MAGIC` |
| 4 | 2 | file_version_major | u16 | v1 = 1；不兼容变更 +1；读端见到未知 major → `TC_IMG_ERR_UNSUPPORTED_VERSION` |
| 6 | 2 | file_version_minor | u16 | v1 = 0；向后兼容扩展 +1 |
| 8 | 4 | preamble_size | u32 | 必须 = 64；否则 `TC_IMG_ERR_BAD_PREAMBLE` |
| 12 | 4 | flags | u32 | 未定义位必须为 0，否则 `TC_IMG_ERR_BAD_PREAMBLE` |
| 16 | 8 | file_size | u64 | 必须等于实际文件长度；否则 `TC_IMG_ERR_BAD_PREAMBLE` |
| 24 | 8 | directory_offset | u64 | directory 绝对偏移；必须 ≥ 64 且 < file_size |
| 32 | 4 | directory_entry_size | u32 | 必须 = 32 |
| 36 | 4 | directory_count | u32 | ∈ [2, TC_IMG_MAX_CHUNKS=64]；超限 → `TC_ERR_LIMIT_EXCEEDED` |
| 40 | 4 | primary_image_index | u32 | v1 必须 = 0（多图预留） |
| 44 | 4 | compatibility_flags | u32 | 能力提示位（§9.3）；未定义位必须为 0 |
| 48 | 4 | header_crc32 | u32 | 对 64B 全体、本字段置 0 计算 |
| 52 | 4 | directory_crc32 | u32 | 对 directory 原始字节（count×32）计算 |
| 56 | 8 | reserved0 | u64 | 必须 = 0 |

布局自洽：4+2+2+4+4+8+8+4+4+4+4+4+4+8 = 64。

## 4. 32-byte Directory Entry

| Offset | Size | 字段 | 类型 | 规则 |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | chunk_type | FourCC | 见 §5；未识别值 → optional/critical 按 flags 判定 |
| 4 | 4 | chunk_flags | u32 | bit0 = critical；bit1 = optional；bit2 = preserve（可编辑 round-trip 保留）；bit3 = may_be_dropped（如 THMB）。bit0 与 bit1 互斥；两者都不置位视为 optional |
| 8 | 8 | chunk_offset | u64 | 绝对偏移；`chunk_offset ≥ 64` 且 `chunk_offset + chunk_size ≤ file_size`（checked），且区间不与 [0,64) 或 directory 区间重叠 |
| 16 | 8 | chunk_size | u64 | > 0；≤ 各 chunk 硬上限（§10） |
| 24 | 4 | chunk_crc32 | u32 | 对 chunk 原始字节计算 |
| 28 | 4 | reserved | u32 | 必须 = 0 |

目录项按 `chunk_offset` **升序**；乱序 → `TC_IMG_ERR_BAD_DIRECTORY`。同一 critical FourCC 重复出现 → `TC_IMG_ERR_BAD_DIRECTORY`。

## 5. Chunk 表（v1）

| FourCC | 必需性 | 用途 | 硬上限 |
| --- | --- | --- | --- |
| `IDSC` | critical | 图片描述（§6） | 128 B（固定） |
| `PIXL` | critical | 一个完整 TPIC elementary frame packet | 256 MiB（TC_MAX_PACKET_SIZE） |
| `ICCP` | optional | 原始 ICC profile | 8 MiB |
| `OCIO` | optional | OCIO colorspace 名称/配置标识（UTF-8，禁止路径/脚本/二进制） | 4 KiB |
| `XMP ` | optional | XMP packet（含尾空格 FourCC `0x58 4D 50 20`） | 16 MiB |
| `EXIF` | optional | TIFF/EXIF metadata | 16 MiB |
| `THMB` | optional | 可丢弃缩略图（自描述小 TPIC packet）；绝不作为主图真实性来源 | 4 MiB |
| `HASH` | optional | SHA-256（32B 原生摘要 + 1B algo id + 3B reserved = 36B）；资产去重用，不替代 chunk CRC | 36 B |
| `TMET` | optional | 容器元数据（2026-09-21 增补）：编码档位 + 厂商标识；载荷与 MOV `tpcD`（container_spec v1.7 §4）逐字节同构，编解码单一真相在 `src/shared/codec/topos_meta.py` | 128 B |

预留（v1 读写端都必须按 unknown 处理，**禁止**提前支持）：`TILE`、`TDIR`、`MIPM`、`AUXC`。

## 6. IDSC（图片描述 chunk，128 字节，冻结）

| Offset | Size | 字段 | 类型 | 规则 |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | magic | u8[4] | `IDSC` |
| 4 | 2 | idsc_version_major | u16 | = 1 |
| 6 | 2 | idsc_version_minor | u16 | = 0 |
| 8 | 4 | idsc_size | u32 | = 128 |
| 12 | 4 | flags | u32 | bit0 = orientation_normalized；其余必须 0 |
| 16 | 4 | display_x_min | i32 | display window（含端点，闭区间） |
| 20 | 4 | display_y_min | i32 | |
| 24 | 4 | display_x_max | i32 | ≥ display_x_min |
| 28 | 4 | display_y_max | i32 | ≥ display_y_min |
| 32 | 4 | data_x_min | i32 | data window |
| 36 | 4 | data_y_min | i32 | |
| 40 | 4 | data_x_max | i32 | ≥ data_x_min |
| 44 | 4 | data_y_max | i32 | ≥ data_y_min |
| 48 | 4 | orientation | u32 | 1..8（EXIF 语义）；v1 writer 只写 1；读端对 2..8 仅报告不旋转 |
| 52 | 4 | pixel_aspect_num | u32 | ≥ 1 |
| 56 | 4 | pixel_aspect_den | u32 | ≥ 1 |
| 60 | 1 | channel_model | u8 | 0 = RGB/GBR，1 = YUV，2 = GRAY；其余 reserved |
| 61 | 1 | channel_count | u8 | 3 或 4（含 Alpha 时为 4） |
| 62 | 1 | sample_kind | u8 | 0 = UINT；1 = HALF（2026-09-19 激活，§15；仅 image_profile 4）；2（FLOAT）reserved，v1 拒绝 |
| 63 | 1 | valid_bit_depth | u8 | ∈ {8,10,12,16}（16 = TRAW linear 归档（批 4）/ HALF 容器满域（§15））；≤ container_bit_depth |
| 64 | 1 | container_bit_depth | u8 | ∈ {10,12,16} |
| 65 | 1 | storage_layout | u8 | 1 = planar；0/其他 reserved，v1 拒绝 |
| 66 | 1 | subsampling | u8 | 0 = 4:4:4，1 = 4:2:2，2 = 4:2:0；GRAY → 0 |
| 67 | 1 | chroma_siting | u8 | 与内层 packet `chroma_siting` 同域；4:4:4 → 0 |
| 68 | 1 | alpha_presence | u8 | 0 = absent，1 = straight，2 = premultiplied |
| 69 | 1 | alpha_mode | u8 | 0/1/2，镜像内层 packet；absent → 0 |
| 70 | 1 | alpha_bit_depth | u8 | mode1 = 16；mode2 ∈ {8,10,12}；absent → 0 |
| 71 | 1 | alpha_max_abs_err | u8 | mode2 的最大绝对误差上界（code values）；mode1 = 0；absent → 0 |
| 72 | 1 | codec_id | u8 | 0 = TPIC elementary；其余 reserved，v1 拒绝 |
| 73 | 1 | payload_major | u8 | 必须等于内层 packet version_major（接受域随 codec 版本机：V1..V8，见 §14） |
| 74 | 1 | payload_minor | u8 | 必须等于内层 packet version_minor |
| 75 | 1 | image_profile | u8 | 0 = Image Preview，1 = Image HQ，2 = Image XQ；3 = Image RAW（TRAW，2026-09-13 改派）；4 = Image HF（HALF，2026-09-19 激活，§15）；5+ reserved，v1 拒绝 |
| 76 | 1 | codec_profile | u8 | 内层 TPIC profile（3/5/6/7），必须与内层一致 |
| 77 | 1 | pixel_format | u8 | 内层 pixel_format（0/1/2/3），必须与内层一致 |
| 78 | 1 | color_primaries | u8 | CICP/H.273 枚举，与内层一致 |
| 79 | 1 | color_transfer | u8 | 同上 |
| 80 | 1 | color_matrix | u8 | 同上；GBR identity → 0 |
| 81 | 1 | color_range | u8 | 0 = limited，1 = full；与内层一致 |
| 82 | 1 | iccp_ref | u8 | 0 = 无 ICC；1 = ICCP chunk 存在且为色彩表征权威 |
| 83 | 1 | ocio_ref | u8 | 0 = 无；1 = OCIO chunk 存在 |
| 84 | 4 | payload_header_crc32 | u32 | 绑定摘要：内层 TPIC frame header 的 header_crc32 字段（对 header 前 49 字节的 CRC-32）；读端必须复算并匹配 |
| 88 | 40 | reserved | u8[40] | 必须 = 0 |

布局自洽：0..87 为字段区（84+4=88），88..127 为 reserved，合计 128。

> **色彩 primaries 近似标注口径（H3，ADR-C057/H2 后注）**：H.273 无
> Adobe RGB primaries 枚举（行业标准现实）——Adobe RGB 载体 = ICCP
> chunk（`iccp_ref=1` 权威），IDSC `color_primaries` 以 `1`（BT.709）
> **近似标注**；读端按 §7 优先级以 ICCP 为色彩表征权威，CICP 数值码
> 仅作 fallback。同样的近似口径适用于其它无 H.273 枚举的工作空间
> （如 ProPhoto RGB）。

### 6.1 IDSC ↔ PIXL 交叉校验（冻结）

令内层 packet header 解析结果为 `fh`（`tc_frame_header_decode`）。以下任一不一致 → `TC_IMG_ERR_CHUNK_CONFLICT`（解析顺序固定，同一坏文件稳定返回同一错误）：

1. `data window 宽高 == fh.visible_width/visible_height`（逐项：`data_x_max-data_x_min+1 == visible_width`，`data_y_max-data_y_min+1 == visible_height`）；
2. `display window ⊇ data window`；
3. `fh.pixel_format == IDSC.pixel_format`；
4. `fh.bit_depth == IDSC.valid_bit_depth`（UINT 与 HALF 同规则——HALF 恒为 16）；
5. `fh.profile == IDSC.codec_profile`；
6. `fh.alpha_mode == IDSC.alpha_mode` 且 `fh.alpha_bit_depth == IDSC.alpha_bit_depth` 且 `fh.flags bit0（premultiplied）与 alpha_presence==2 一致`；
7. `fh.color_range/primaries/transfer/matrix/chroma_siting == IDSC 对应字段`；
8. `fh.sar_num/sar_den == IDSC.pixel_aspect_num/den`（内层 0/0 = 未指定 SAR，与 IDSC `1/1`（方形像素）视为等价；非零 SAR 必须精确相等）；
9. `IDSC.payload_header_crc32 == fh.header_crc32`（复算）；
10. `IDSC.payload_major/minor == fh.version_major/version_minor`。

channel_model ↔ pixel_format 映射：`pixel_format∈{0,1}` → channel_model=1（YUV）；`=2` → channel_model=0（RGB/GBR）。`fh.plane_count`（已含 alpha）必须等于 `IDSC.channel_count`。

**通道/平面顺序（冻结）**：pf=0/1（YUV）= Y,U,V；pf=2（GBR）= **G,B,R**（对齐 FFmpeg gbrp 契约，ADR-C017 C-130）；Alpha 恒为 plane 3。解码输出平面按此顺序交付，IDSC 不另设顺序字段（由内层 pixel_format 唯一确定）。

### 6.2 窗口坐标语义（冻结）

- 所有坐标为**画布绝对坐标**，原点在左上，x 向右、y 向下；
- data window 非零/负原点只改变画布位置，**不改变**内层 plane 存储尺寸；
- v1 单 PIXL：`display_window ⊇ data_window`；未来 tile/sparse 若允许 data ⊄ display 或多 PIXL，必须提升 file_version_major。

## 7. 色彩元数据优先级（冻结）

1. 内层 TPIC 的 pixel_format、range、matrix、chroma siting **始终**负责把存储样本重建为 RGB；ICC/OCIO/文件层 metadata 绝不能覆盖数值解释规则；
2. GBR identity payload：不执行 YUV matrix，但遵守 bit_depth、range、通道顺序；
3. 得到 RGB 后：有效 ICCP（且 `iccp_ref=1`）为该 RGB 的色彩表征权威；CICP 摘要仍保留并报告；
4. 无 ICC：按结构化 primaries/transfer（CICP）表征；
5. 元数据缺失 → 返回 `unspecified`，**不得**静默猜成 sRGB；
6. ICC、CICP、OCIO 三者互相矛盾 → `TC_IMG_ERR_METADATA_CONFLICT`（硬错误）；OCIO 名称仅是颜色空间标识，禁止携带脚本、动态库路径或机器绝对路径（writer 校验，reader 对明显路径形态报错）。

## 8. Alpha 语义（冻结）

- Alpha 为独立全分辨率平面，不降采样、不进颜色 DCT 死区；
- mode 1（A16 无损）：bit-exact；mode 2（受限近似）：必须记录 `alpha_bit_depth` 与 `alpha_max_abs_err`；
- 文件必须明确记录 straight / premultiplied（`alpha_presence`）；
- Alpha=0 处 RGB 的保留策略：**v1 冻结为"逐字保留编码值"**（codec 层已保证 plane 独立）；转换到 premultiplied 宿主时由调用方在明确线性边界执行，文件层不做隐式转换。

## 9. Profiles 与支持矩阵

### 9.1 Image Profile（IDSC.image_profile）

| ID | 名称 | codec 映射 | 定位 | v1 状态 |
| ---: | --- | --- | --- | --- |
| 0 | Image Preview | pf=0 YUV422（profile 3），10/12-bit | 预览/图库/代理 | v1 可写可读 |
| 1 | Image HQ | pf=2 GBR（profile 5），10-bit | VFX beauty/贴图/缓存 | v1 可写可读（codec 上游已过门禁） |
| 2 | Image XQ | pf=2 GBR（profile 5/6），12-bit | 高精度整数中间片 | v1 可写可读（codec 上游已过门禁） |
| 3 | Image RAW | pf=3 CFA（profile 7），12/16-bit | TRAW 制作/归档（2026-09-13 改派，原 Lossless 档移除） | v1 可写可读 |
| 4 | Image HF | pf=2 GBR（profile 5），16-bit half（§15） | HDR 线性合成中间件/精确缓存（2026-09-19 激活） | v1 可写可读 |

图片 Profile 名称与 codec Profile（3/5/6/7）一一映射但**不得**互相替代：名称冻结，不得复用 codec 名称掩盖像素/质量语义差异。

### 9.2 明确非 v1 能力（读写端必须拒绝或报 unsupported）

FLOAT（32-bit）像素、tile/ROI 随机解码、mipmap、多图（primary_image_index≠0）、任意 AOV/多层/deep、浏览器原生解码。（HALF 已于 2026-09-19 激活，见 §15。）

### 9.3 compatibility_flags（preamble，提示位，非权威）

bit0 = writer 支持 Image HQ（GBR 10）；bit1 = 支持 Image XQ（GBR 12）。解码能力以 `tc_image_query_capabilities()` + 内层 `tc_query_support()` 实测为准，此字段仅作快速路由提示。

## 10. 尺寸、大图与安全预算（冻结）

| 项 | 上限 | 超限错误 |
| --- | --- | --- |
| 画布 coded 尺寸 | 16384×16384（TC_MAX_CODED_DIM） | 内层 codec TC_ERR_* |
| 单 PIXL payload | 256 MiB | TC_ERR_LIMIT_EXCEEDED / TC_IMG_ERR_LIMIT |
| directory_count | 64 | TC_ERR_LIMIT_EXCEEDED |
| ICCP / XMP / EXIF | 8/16/16 MiB | TC_IMG_ERR_LIMIT |
| THMB | 4 MiB | TC_IMG_ERR_LIMIT |
| OCIO | 4 KiB | TC_IMG_ERR_LIMIT |
| 解析临时分配 | 目录+IDSC ≤ 64 KiB（不含像素） | TC_ERR_OUT_OF_MEMORY |

**高分辨率策略：单帧 v1 上限提升为 ≤16384、≤256 MiB packet**；16K 仍可能因高熵内容触发包上限，不能宣传“无限分辨率”或保证所有 16K RGBA 素材都可编码。结构上继续预留策略 3（TILE/TDIR FourCC，读写端按 unknown 处理）。

## 11. 错误码（image 层扩展，冻结）

复用 codec `TC_ERR_*`（-1..-16）语义不变；image 层新增（互不冲突的 -100 段）：

| 值 | 名称 | 语义 |
| ---: | --- | --- |
| -100 | TC_IMG_ERR_BAD_MAGIC | preamble magic 非 TPIM（含裸 TPIC packet 误当 .toos 打开） |
| -101 | TC_IMG_ERR_BAD_PREAMBLE | preamble 字段/版本/尺寸非法 |
| -102 | TC_IMG_ERR_BAD_DIRECTORY | 目录越界/重叠/乱序/重复 critical/缺失必需 chunk |
| -103 | TC_IMG_ERR_UNKNOWN_CRITICAL | 未知 critical chunk |
| -104 | TC_IMG_ERR_CHUNK_CONFLICT | IDSC ↔ PIXL 交叉校验失败（§6.1，含重复语义冲突） |
| -105 | TC_IMG_ERR_METADATA_CONFLICT | ICC/CICP/OCIO 矛盾（§7） |
| -106 | TC_IMG_ERR_LIMIT | image 层硬上限（§10） |
| -107 | TC_IMG_ERR_IO_WRITE_FAILED | 原子写失败（临时文件/rename/flush） |

`tc_image_status_message()` 提供全部 image 码文本；`tc_last_error()` 沿用 codec 线程局部详情。

## 12. 原子写流程（冻结）

1. 同文件系统创建唯一临时文件（`<target>.toos.tmp-<pid>-<rand>`）；
2. 写 preamble 占位 → chunks → directory；
3. 回填 file_size、directory_offset、全部 CRC；
4. flush + close，重新 probe/validate（完整结构校验）；
5. 按明确 overwrite policy 原子 rename；
6. 失败/取消：删除临时文件，返回 `TC_IMG_ERR_IO_WRITE_FAILED`；禁止留下 0 字节或半文件冒充成功；
7. 禁止编码失败后静默改写为其他格式或改扩展名。

## 13. 命名与兼容（冻结）

- 静态图片唯一扩展名 `.toos`（ADR-I008 由 `.tpic` 迁移）；MIME `image/x-toos`；macOS UTI / Windows ProgID 独立注册（后续分发任务）；
- `.topos` 永久保留为工程包，禁止作为图片别名；
- 文件类型判断以 `TPIM` magic 为最终依据，扩展名仅快速候选；
- 裸 TPIC packet：`.tpkt` 测试后缀 / `.bin` corpus / `topos_inspect --raw-packet`；`topos_inspect` 帮助文本中 `TPIC packet` 不再写作 `<file.toos>`；
- `.topos` 工程包内引用 `.toos` 素材为普通文件引用，不改变工程 schema。

## 14. 版本策略（冻结）

- file_version_major：IDSC 布局、chunk 必需性、窗口语义等不兼容变更时 +1；读端对未知 major 硬失败；
- file_version_minor：新增 optional chunk、新增 compatibility 位等 +1；读端必须容忍；
- 内层 TPIC packet 版本独立演进（payload_major/minor 随 codec），封装升级不得改变 codec 语义；
  - 2026-09-12：内层 packet 接受域 V1..V6 → **V1..V8**（V7 = rANS 族 / V8 = 段化多链；
    IDSC 十项交叉校验不变，payload 版本仍须与内层 header 精确相等）。旧读端
    （校验 V1..V6 的二进制）以 `UNSUPPORTED_VERSION` 明确拒绝新流——与
    file_version_major 的未知 major 硬失败语义一致；本变更不改变任何既有
    .toos 文件的合法性（纯接受域扩展）。实测依据：`tools/toos_entropy_2026-09-12.json`
    （V7-R2 较 vlc 码率 −7~12%、解码 −11~27%）。
- 未知 optional chunk 在可编辑 round-trip 中字节级保留（preserve 位）；无 preserve 位的 optional chunk 允许丢弃并记录。

## 15. HALF 样本域（2026-09-19 冻结）

`sample_kind=HALF`（=1）声明载荷样本为 **IEEE 754 binary16（half）经冻结单调映射转 u16 码值**后的 bd=16 整数码流。压缩管线（level shift → 整数 DCT → 量化 → 熵编码）与 UINT **逐字节同一实现，零分叉**——位流规范（bitstream_spec §2.4 禁浮点）不被触碰，浮点只存在于 host 侧映射边界。

### 15.1 冻结映射（规范参考实现）

```
正向  code(h)：m = h & 0x7FFF
               code = (h & 0x8000) ? 0x8000 − m : 0x8000 + m
逆向  half(c)：c ≥ 0x8000 → h = c − 0x8000
               c < 0x8000 → m = min(0x8000 − c, 0x7FFF)；h = 0x8000 | m
```

性质（golden 见 native `tests/unit/test_half_map.c` 与 Python `tests/media/test_topos_half_float.py`，两侧逐位一致互钉）：

- **单调**：有限值域码值序 == 浮点值序（sign-magnitude → 对称偏移二进制，本质是对数幅值域的符号扩展，近似 symlog）；
- **零归一**：+0 与 −0 同映射码 `0x8000`（恰为 bd16 level-shift 中值 → 浮点零编码后亦为整数零）；逆向恒返回 +0；
- **全域分配**：`-NaN < -Inf < 负有限 < 0 < 正有限 < +Inf < +NaN`，Inf/NaN 占据码值域两端（−NaN 起于 0x0001、+Inf=0xFC00 等）；NaN 载荷逐位可往返；
- **双射**：除 ±0 归一点外全部 65536 个 half 位模式与码值一一对应；前向值域 `[1, 0xFFFF]`（码 0 不可达；损坏流解出码 0 时逆向饱和为 `0xFFFF`，即 −NaN，确定性定义）；
- **码 0x8000 中值对齐**：映射后数据天然以浮点零为中心，DCT/DC 预测以零为中心工作，无整数码流的中值偏置。

### 15.2 与 codec/容器的交叉规则（native 强制）

- **唯一组合**：image_profile=4（Image HF）⇔ sample_kind=HALF ⇔ pf=2（GBR 4:4:4）⇔ bd=16 ⇔ codec profile 5；channel_model=RGB、subsampling=0、valid=container=16；transfer 冻结 linear（=8）。双向校验：UINT 流携带 profile 4、或 HALF 流携带其他 profile → `TC_IMG_ERR_CHUNK_CONFLICT`；
- **旧读端兼容**：sample_kind=2+（FLOAT 等仍 reserved）→ `UNSUPPORTED_VERSION`；老二进制对 sample_kind=1 同样按 reserved 干净拒绝，不存在误读路径（枚举激活沿 §6 既有 reserved 值语义，无版本字段变更）；
- **Alpha**：仅 mode 1（无损 16-bit）；alpha 平面样本同为 half 码值（负值/HDR>1 合法，无 [0,1] 钳制约定）。mode 2（近似 8/10/12）的码值误差上界语义对 half 域无定义 → 拒绝；
- **color_range/matrix 对 HALF 无数值语义**：half 码值的解释**仅**由 §15.1 映射给出；range 字段随内层帧头镜像保留（GBR 默认），消费端不得对 HALF 平面施加 range 缩放。

### 15.3 量化与无损语义（qp 域，bd16 偏移 +12 生效）

| qp_base | Q | 语义 |
| ---: | ---: | --- |
| 0–10 | 1 | **逐位无损**（量化恒等）；前提：内容系数在熵 token 域内——block DC 码值偏移 ≤0x3000、AC 幅度有界；越界编码器**显式拒绝**（`INVALID_ARGUMENT` + "raise qp"），不静默损坏 |
| 11–19 | 2–7 | 可编域随 Q 扩大；实测自然内容逐位往返（量化误差 < 0.5 码 → 重建舍入精确） |
| ≥20 | ≥8 | **全内容可编**（设计工作地板） |

**2K 真实视频帧实测**（2026-09-19，1920×1080，ProRes422HQ / DNxHR444 10-bit / H264 各一帧，[0,1] 线性域；half 原始 12288 KiB。产品阶梯（v1.8 ADR-C057）= 下表 qp44/58/72/82 四档，CLI `--tier float-ultra/high/medium/low`，TMET 展示名 'toos <Quality> 16bit float'；真无损不入阶梯，`--half --qp 20/32` 显式达成）：

| 档位 | qp | ProRes 帧 | DNxHR 帧 | H264 帧 | 保真 |
| --- | ---: | ---: | ---: | ---: | --- |
| （显式 --qp 20） | 20 | 10625 KiB (1.1:1) | 9358 KiB (1.3:1) | 9540 KiB (1.3:1) | **逐位无损** |
| （--qp 32） | 32 | 8353 KiB (1.5:1) | 7496 KiB (1.6:1) | 7387 KiB (1.6:1) | **逐位无损** |
| float-ultra | 44 | 6054 KiB (2.0:1) | 5612 KiB (2.2:1) | 5184 KiB (2.3:1) | 92.7% 像素逐位一致，PSNR ≈82 dB（视觉透明） |
| float-high | 58 | 4702 KiB (2.6:1) | 4501 KiB (2.7:1) | 3835 KiB (3.2:1) | PSNR ≈71 dB |
| float-medium | 72 | 920 KiB (13:1) | 1535 KiB (7.9:1) | 460 KiB (26:1) | PSNR ≈44 dB（代理） |
| float-low | 82 | 246 KiB (50:1) | 742 KiB (16:1) | 119 KiB (102:1) | PSNR ≈36 dB（审片代理） |

同帧参照：EXR half+zip16 = 6568 KiB（1.8:1，无损）；PNG16 = 9083 KiB；PNG8 = 2691 KiB。**float-ultra 以 ±2 half-ULP 的代价小于 EXR zip16**；无损档（qp32）大于 EXR zip16——DCT 无损对视频噪点不如 zip 预测器，合成/渲染类平滑内容则相反（见设计记录）。

推荐档位：**合成中间件默认 qp=20**（安全地板 + 实测无损）；严格归档 qp≤10 + 内容域约束（越界显式失败）。token 域边界系数量级：DC 增益 ≈10922（码值偏移 0x3100 → |Δdc| 越 2^27−1）。

### 15.4 API（host 侧便利，非码流语义）

- C：`tc_image_half_to_codes` / `tc_image_codes_to_half`（批量、可原地、与标量参考逐位一致）；
- Python：`topos_image_binding.half_bits_to_codes` / `codes_to_half_bits`（numpy 向量实现，与 native 全 65536 域逐位一致，测试互钉）；`ToposImageCodec.decode_float()` 解码 HALF 文件为 float16 平面（UINT 文件显式拒绝——样本域不混用）；
- 编码侧：`ToposImageSequenceEncoder` pix_fmt `gbrph16le` / `topos_gbraph16a16`（float 输入直通，无 [0,1] 契约；整数输入显式拒绝）。
