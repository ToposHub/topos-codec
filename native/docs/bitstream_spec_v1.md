# Topos Video Codec — Elementary Bitstream Specification v1.5

- 版本：**v1.5（规范性）**。v1.0 由阶段 3 golden vectors（`golden_bitstream_v1.bin`，
  56 条记录）与四配置测试门禁冻结（ADR-C003）；v1.1 为阶段 4 向后兼容修订（见下方
  变更记录），全链路由 `golden_codec_v1.bin`（16 条记录）加冻；v1.2（R4.1，
  ADR-C015）为 12-bit YUV 4:2:2 枚举扩展，12-bit 流标记 `version_minor = 1`，
  由 `golden_codec_v1_bd12.bin`（10 条记录）加冻；v1.3（R4.2，ADR-C016）为
  YUV 4:4:4 枚举扩展（pixel_format=1），4:4:4 流标记 `version_minor = 2`，
  由 `golden_codec_v1_pf444.bin`（12 条记录）加冻；**v1.4（R4.3，ADR-C017）为
  GBR 4:4:4 枚举扩展（pixel_format=2，matrix=0 identity 契约）**——按 §13.1
  属 minor bump，GBR 流标记 `version_minor = 3`（v1.0 语义流恒 0，既有 golden
  字节不变），全链路由 `golden_codec_v1_gbr.bin`（12 条记录）加冻；**R4.4（ADR-C018）
激活 profile 5/6（Pro444/Extreme）**——profile 枚举 v1.0 已定义，激活不动
minor 语义（v1.0 解码器对 5/6 本就按 UNSUPPORTED_PROFILE 干净拒绝），全链路
由 `golden_codec_v1_profiles.bin`（12 条记录）加冻；**v1.5（ADR-C031）为
qp 值域扩展（Proxy/LT 低码率档）**——qp_base 64..95 的流标记
`version_minor = 4`（详见帧头 §3 与附录 A.4；旧解码器按 minor>3 干净拒绝，
qp≤63 流字节与 minor 完全不变，既有 golden 不受影响）。
- 位流版本字段：`version_major = 1`；`version_minor ∈ {0,1,2,3,4,5,6,7}`——每个扩展
  所属的最低 minor：bit_depth=12 须 ≥1、pixel_format=1 须 ≥2、
  pixel_format=2 须 ≥3、qp_base≥64 须 ≥4（writer 取所属扩展代的最低值，
  最大化旧解码器可解域）；
  minor 低于所需 → `TC_ERR_UNSUPPORTED_VERSION`（版本声明不一致）；
  minor 高于所需 = 前向兼容 writer，按 §13.1 完全一致解码；
  minor>4 → `TC_ERR_UNSUPPORTED_VERSION`。
- 关联决策：`docs/ADR-C001-stage0-decisions.md`（C-06…C-13）、
  `ADR-C002-stage1-2-transform.md`（C-19/C-20）、`ADR-C003-stage3-bitstream.md`（C-21…C-29）、
  `ADR-C004-stage4-codec.md`（C-30…C-37）。
- 图片层开发扩展（ADR-I012/I013）：`qmatrix_id=2` 仅用于 GBR 4:4:4 12-bit
  profile 5/6 的 444 Compact 档，`qmatrix_id=3` 仅用于 YUV 4:2:2 10-bit
  profile 3 的 422 Low Compact 档；视频 MOV 档位仍使用 id=1。该扩展不改变
  header 布局或解码算法。
- 状态词约定：**必须**（解码器违反 = 不合格实现）；**冻结**（任何变更需提升
  `version_minor`，破坏兼容的变更提升 `version_major`）。

### v1.0 → v1.1 变更记录（阶段 4）

1. `alpha_mode=2`（受限近似）由预留改为**已定义并实现**（§8.6）；
   `alpha_bit_depth` 相应扩展：mode2 时 ∈ {8,10,12}（§4.2）。
2. 附录 A.3 `qmatrix_id=1`（Topos Standard）数值由 flat 占位改为调优冻结表
   （几何递增频率加权，ADR-C004 C-31）。
3. §8.2 勘误：带内 `y>0 且 x=0` 时 `a`、`c` 均不可用 → `pred = b`
   （v1.0 文字「其余行 c 不可用场景不出现」未覆盖行首情形；v1.0 golden 未行使该
   情形，不受影响）。

### v0 → v1 变更记录（实现期校正，均在 golden 冻结前完成）

1. **§7.5.3 EOB 不变量修正**：v0 的「字面系数要求 `pos < 63` 且 `pos + run ≤ 62`」存在
   off-by-one——它会把「仅 c[63] 非零」的合法块（run = 62）误判 MALFORMED。修正为
   `pos + run ≤ 63`，EOB 接受于任意 `pos ≤ 64`（ADR-C003 C-22）。
2. **§7.5.2 DC 预测公式明确**：`(left+top+1)>>1` 当和为负时构成负数右移，违反 §2.4。
   明确为 round-half-up 符号拆分实现（ADR-C003 C-24）。
3. **§8.3/§8.4 Alpha 尾零终结**：v0 的「无 EOB、按像素计数终止」语义无法自终止
   （解码器在尾零段会多读一对符号 → TRUNCATED）。修正为尾零终结对 `(run, level=0)`，
   要求 `pos + run == count`（ADR-C003 C-23）。
4. **§4.2 解码校验顺序冻结**：`size ≥ 53 → magic → header_crc32 → 字段规则（表序）`（C-27）。
5. **§A.7 澄清**：`color_matrix = 0`（identity）仅限 GBR 像素格式（保留）；v1.0 的
   YUV 4:2:2 流必须使用 1/5/9，出现 0 → `TC_ERR_MALFORMED`（C-28）。
6. **§6.2 符号域上限精确化**：给出 DC/AC/Alpha/run 的 m 上限十六进制常量（C-21）。

---

## 1. 范围

本文定义 **Topos elementary frame packet**：单个视频帧的完整独立压缩表示（容器无关）。
容器（MOV/TPIC/tpcC）映射见 §12，细节在阶段 5 定稿。

V2.0 只定义 Intra profile：每帧独立、无帧间依赖、可随机访问、slice 可独立解码。

## 2. 约定与全局规则

1. **字节序**：所有多字节字段 **big-endian**。
2. **位序**：熵编码位流在字节内 **MSB-first**（先发高位）。
3. **对齐**：frame header 与每个 slice header 字节对齐；slice payload 末尾用 `0` 位填充到字节边界。
4. **整数语义**：规范性路径只允许整数运算（比较、加、减、乘、除、移位）。
   - 除法 `/` 一律整数除法（向零截断）；移位仅对非负右移（`>>`），算术右移负数不出现在规范中
     （所有移位对象先取绝对值或保证非负）。
   - 饱和：仅 §7.7 重建钳位一处；其余任何中间值不得溢出其声明宽度（界证明 §7.6）。
   - **禁止浮点**进入编码/解码任何规范性步骤（表生成脚本可用浮点，产出的表是冻结整数）。
5. **checked arithmetic**：解码器对任何来自码流的 size/offset/count，必须先以 u64 验证
   `a + b ≤ MAX` 与 `a ≤ 容器剩余` 再使用（阶段 1 提供统一 helper；fuzz 覆盖）。
6. **CRC-32**：IEEE 802.3（反射多项式 `0xEDB88320`，init `0xFFFFFFFF`，输出再异或
   `0xFFFFFFFF`，逐字节 LSB-first 处理）。自测向量：ASCII `"123456789"` → `0xCBF43926`。
7. **平面数据布局**（输入/输出契约，非压缩域）：planar、行主序、uint16 **little-endian**、
   tight（stride = width × 2）、码值范围 `0 .. 2^bit_depth − 1`。与 Topos Color
   `DecodedFrame.planes` / `PlaneInfo` 契约一致（`yuv_gpu_upload.py` R16UI 与
   `color_decode.py` 满刻度约定）。

## 3. 首版支持矩阵（v1.0）

| 项 | 值 | 说明 |
| --- | --- | --- |
| pixel_format | `0`（YUV 4:2:2）/ `1`（YUV 4:4:4，v1.3 须 minor=2）/ `2`（GBR 4:4:4，v1.4 须 minor=3，平面序 G,B,R）/ `3`（CFA/TRAW，v1.6 须 minor=5 或载体 major≥7；4 相位平面 R/Gr/Gb/B 各 ceil(W/2)×ceil(H/2)，仅 profile 7） | 其余 → `TC_ERR_UNSUPPORTED_PIXEL_FORMAT` |
| bit_depth | `10` / `12`（v1.2 枚举扩展，须 minor≥1；4:4:4 下同域）/ `16`（v1.7 扩展，须 minor≥6 或载体 major≥7；bd≥13 须宽域熵 rans2/v8） | 其余 → `TC_ERR_UNSUPPORTED_PIXEL_FORMAT` |
| alpha_mode | `0`（无）或 `1`（无损） | `2`（受限近似）：v1.1 起已定义（§8.6） |
| alpha_bit_depth | `0`（无 Alpha）或 `16` | mode2 时 ∈ {8,10,12}（近似精度，§8.6）；其余值 → `TC_ERR_MALFORMED` |
| profile | `3`（Standard）/ `5`（Pro444）/ `6`（Extreme，R4.4 激活） | `1/2/4` 已定义未实现（§4.4）→ `TC_ERR_UNSUPPORTED_PROFILE` |
| frame_type | `0`（I） | `1`（P）为 V2.1 预留，v1.0 必须拒绝 |
| 场序 | progressive | 隔行字段保留在容器元数据，不进 elementary 流 |

## 4. Frame Packet 结构

### 4.1 总体布局

```text
┌─────────────────────────────── FramePacket (frame_packet_size bytes) ───────────────────────────────┐
│ magic "TPIC"(4) │ header fields(45) │ header_crc32(4) │ slice[0] │ slice[1] │ … │ slice[slice_count−1] │
└──────────────────────────────────────────────────────────────────────────────────────────────────────┘
slice = slice header(17) + slice payload(slice_payload_size bytes，字节对齐、末尾零填充)
```

### 4.2 Frame Header（固定 53 字节，冻结）

**解码校验顺序（冻结，C-27）**：`size ≥ 53`（否则 `TC_ERR_TRUNCATED`）→ `magic`
（`TC_ERR_MALFORMED`）→ `header_crc32`（`TC_ERR_CHECKSUM_MISMATCH`）→ 逐字段规则
（按下表偏移序，首个不通过者返回）。编码器执行同一套字段规则。

| 偏移 | 字节 | 字段 | 类型 | v1.0 约束 |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | `magic` | u8[4] | `'T','P','I','C'`（0x54 0x50 0x49 0x43），不符 → `TC_ERR_MALFORMED` |
| 4 | 2 | `header_size` | u16 | 必须 `== 53`（大于 53 见 §13.1 扩展规则） |
| 6 | 1 | `version_major` | u8 | `== 1`，否则 `TC_ERR_UNSUPPORTED_VERSION` |
| 7 | 1 | `version_minor` | u8 | v1.0 语义流 `== 0`；bd=12 扩展流（v1.2）`== 1`；pf=1 扩展流（v1.3）`== 2`；pf=2 扩展流（v1.4）`== 3`；qp≥64 扩展流（v1.5）`== 4`；pf=3/profile=7 扩展流（v1.6，TRAW）`== 5`；bd=16 扩展流（v1.7）`== 6`；transfer=13 sRGB 扩展流（v1.8，H1）`== 7`（仅 major=1 域——载体 major≥7 时 minor 恒 0） |
| 8 | 2 | `flags` | u16 | bit0 = `alpha_premultiplied`（存储的 Alpha 为 premultiplied；默认 0 = straight）。bit1..15 保留，必须 0，非 0 → `TC_ERR_MALFORMED` |
| 10 | 1 | `profile` | u8 | 已验收 `3/5/6`（§4.4，R4.4 激活 5/6）；`1/2/4/其余` → `TC_ERR_UNSUPPORTED_PROFILE`；交叉规则——5/6 须 pf≠0（`MALFORMED`），6 须 bd=12 |
| 11 | 1 | `pixel_format` | u8 | `0`（4:2:2）/ `1`（4:4:4，v1.3 须 minor=2）/ `2`（GBR，v1.4 须 minor=3）/ `3`（CFA 相位平面，v1.6 须 minor=5，TRAW）；低 minor 出现 → `TC_ERR_UNSUPPORTED_VERSION`；其余 → `TC_ERR_UNSUPPORTED_PIXEL_FORMAT` |
| 12 | 1 | `bit_depth` | u8 | ∈ {10,12,16}（12 为 v1.2 扩展须 minor=1；16 为 v1.7 扩展须 minor=6）；其余 → `TC_ERR_UNSUPPORTED_PIXEL_FORMAT` |
| 13 | 1 | `alpha_mode` | u8 | `0` 无 / `1` 无损 / `2` 受限近似（v1.1 §8.6） |
| 14 | 1 | `alpha_bit_depth` | u8 | `alpha_mode==0` → 0；`==1` → 16；`==2` → {8,10,12}；违反 → `TC_ERR_MALFORMED` |
| 15 | 1 | `frame_type` | u8 | v1.0 必须 `0`；`1`(P) → `TC_ERR_UNSUPPORTED_VERSION`（V2.1 能力） |
| 16 | 2 | `gop_id` | u16 | v1.0 必须 0（V2.1 预留） |
| 18 | 1 | `ref_distance` | u8 | v1.0 必须 0（V2.1 预留） |
| 19 | 2 | `coded_width` | u16 | `≤ 8192` 且 `%8==0` 且 `≥ visible_width`，否则 `TC_ERR_LIMIT_EXCEEDED` |
| 21 | 2 | `coded_height` | u16 | 同上 |
| 23 | 2 | `visible_width` | u16 | `≥ 1` |
| 25 | 2 | `visible_height` | u16 | `≥ 1` |
| 27 | 1 | `plane_count` | u8 | `== 3 + (alpha_mode != 0 ? 1 : 0)`，否则 `TC_ERR_MALFORMED` |
| 28 | 1 | `qmatrix_id` | u8 | `0`=flat / `1`=Standard / `2`=444 Compact（仅 GBR 4:4:4 12-bit profile 5/6）/ `3`=422 Low Compact（仅 YUV 4:2:2 10-bit profile 3）；其他 → `TC_ERR_UNSUPPORTED_MATRIX` |
| 29 | 1 | `qp_base` | u8 | `0..63`；v1.5（minor=4）扩展 `64..95`，低 minor 流出现 → `TC_ERR_UNSUPPORTED_VERSION`；`>95` 恒 `TC_ERR_MALFORMED` |
| 30 | 2 | `slice_count` | u16 | `1..512`；`≥ plane_count`；每平面 `≥ 1` |
| 32 | 1 | `color_range` | u8 | `0`=limited / `1`=full |
| 33 | 1 | `color_primaries` | u8 | §A.7 表；未知 → `TC_ERR_UNSUPPORTED`（错误码用 `TC_ERR_MALFORMED` + 详情注明 tag） |
| 34 | 1 | `color_transfer` | u8 | §A.7 表；同上 |
| 35 | 1 | `color_matrix` | u8 | §A.7 表；R4.3 交叉规则——pf=2（GBR）必须 `0`（identity），pf∈{0,1} 仅 1/5/9（出现 0 → `TC_ERR_MALFORMED`，C-28 修订） |
| 36 | 1 | `chroma_siting` | u8 | `0`=left / `1`=center / `2`=topleft |
| 37 | 2 | `sar_num` | u16 | `0/0` 表示未指定（应用按 1/1 处理并保留 None） |
| 39 | 2 | `sar_den` | u16 | 同上；`num=0` 当且仅当 `den=0`，否则 `TC_ERR_MALFORMED` |
| 41 | 4 | `frame_packet_size` | u32 | `≤ 256 MiB` 且 `==` 实际包字节数（容器 sample size） |
| 45 | 4 | `reserved0` | u32 | 必须 0，非 0 → `TC_ERR_MALFORMED` |
| 49 | 4 | `header_crc32` | u32 | 对偏移 `0..48`（49 字节）计算；不符 → `TC_ERR_CHECKSUM_MISMATCH`（整帧拒绝，不 concealment） |

### 4.3 Slice Header（固定 17 字节，冻结）与排列

| 偏移 | 字节 | 字段 | 类型 | 约束 |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | `slice_payload_size` | u32 | 后续 payload 字节数（含末尾零填充）；必须使 header+payload 落在包内 |
| 4 | 1 | `plane` | u8 | `0`=Y `1`=U `2`=V `3`=A；`3` 仅当 `alpha_mode!=0` |
| 5 | 2 | `block_y0` | u16 | 该 plane 块网格内的起始块行 |
| 7 | 2 | `block_h` | u16 | 带高（块行数），`≥ 1` |
| 9 | 1 | `qp_delta_biased` | u8 | 有效 `qp = qp_base + (qp_delta_biased − 64)`，结果必须 `0..95`（v1.5；qp_base≥64 须 minor=4），否则 `TC_ERR_MALFORMED` |
| 10 | 1 | `k1` | u8 | 颜色：DC 残差 Rice k；Alpha：level Rice k。`0..14` |
| 11 | 1 | `k2` | u8 | 颜色：AC level Rice k；Alpha：run Rice k。`0..14` |
| 12 | 1 | `k3` | u8 | 颜色：run Rice k；Alpha：必须 0 |
| 13 | 4 | `slice_crc32` | u32 | 对本 slice 的 payload 字节计算（不含 header） |

**排列与覆盖（解码器必须验证，违反 → `TC_ERR_MALFORMED`，整帧拒绝）**：

- slice 按平面顺序排列：`Y 全部 slice → U → V → [A]`；每平面内按 `block_y0` 升序。
- 每平面的 slice 必须**恰好平铺**该平面的完整块行范围 `[0, plane_block_rows)`：
  无缝、无重叠、无缺漏；`block_h ≥ 1`。
- v1.0 slice 是**全宽水平带**：`block_x0 ≡ 0`、`block_w ≡ plane 块列数`（不在 header 中传输，
  由几何推导）。水平切分留作 minor 版本扩展（§13.1）。
- **slice 独立性**：任何 slice 的解码不得依赖其他 slice 的任何状态（预测、Rice 参数、DC 链全部
  在 slice 内闭合）。这是 slice 级并行与受控 concealment 的基础。

### 4.4 Profile ID 表（冻结）

| id | 名称 | 颜色 | 位深 | 备注 |
| ---: | --- | --- | --- | --- |
| 1 | Topos Proxy | YUV 4:2:2 | 10 | 后续版本实现 |
| 2 | Topos LT | YUV 4:2:2 | 10 | 后续版本实现 |
| 3 | **Topos Standard** | YUV 4:2:2/4:4:4/GBR | 10/12 | **v1.0 唯一实现**；v1.2–v1.4 枚举扩展均以 profile3 交付（不限格式）；参考码率 14–19 MB/s @1080p25 |
| 4 | Topos HQ | YUV 4:2:2 | 10/12 | 已定义未实现（应用层以 profile3 + 质量预设表达） |
| 5 | **Topos Pro 444** | YUV/GBR 4:4:4 | 10/12 | **R4.4 激活**：pf≠0 强制（4:2:2 → `MALFORMED`） |
| 6 | **Topos Extreme** | YUV/GBR 4:4:4 | 12 | **R4.4 激活**：pf≠0 + bd=12 强制 |

Profile 决定编码器的量化矩阵/qp 选择策略（编码器行为），**不改变位流语法**；
v1.0 解码器对 id≠3 返回 `TC_ERR_UNSUPPORTED_PROFILE`（语法可解析但未验收一致性）。

## 5. 平面几何

| 平面 | coded 尺寸 | visible 尺寸 |
| --- | --- | --- |
| Y | `coded_width × coded_height` | `visible_width × visible_height` |
| U / V（4:2:2，pf=0） | `cw_c × coded_height`，`cw_c = ceil(ceil(visible_width/2) / 8) × 8` | `ceil(visible_width/2) × visible_height` |
| U / V（4:4:4，pf=1，v1.3） | `coded_width × coded_height`（与 Y 同） | `visible_width × visible_height` |
| G / B / R（GBR，pf=2，v1.4） | `coded_width × coded_height`（三平面同尺寸；平面序 G,B,R） | `visible_width × visible_height` |
| A | `coded_width × coded_height` | `visible_width × visible_height` |

- **每平面独立**向 8 的倍数 padding（chroma coded 宽度独立于 luma coded 宽度计算，允许不相等）。
- padding 规则：边缘像素复制（clamp 坐标）。确定性要求：编码器与任何重新编码实现必须产出
  完全相同的 padding。
- 解码输出裁剪到 visible 尺寸（tight，stride = visible_width × 2）。

## 6. 熵编码（唯一方案）

### 6.1 有符号映射

对带符号值 `v`（编码前）：

```text
m = (v ≥ 0) ? 2·v : −2·v − 1          // 非负映射值
v = (m 为偶数) ? m/2 : −(m+1)/2        // 逆映射
```

### 6.2 Rice 码（有界，冻结）

对非负 `m` 与参数 `k ∈ 0..14`：

```text
q = m >> k
若 q ≤ 30：  发送 q 个 '1'，1 个 '0'，随后 k 位 (m & (2^k − 1))，MSB-first
若 q > 30：  发送 31 个 '1'（escape 标记），随后 32 位 m 的字面值，MSB-first
```

**解码（必须按此实现）**：逐位读 '1'，最多计数 31；若在计数 `c ≤ 30` 时读到 '0'，
则 `q = c`，再读 k 位余数，`m = (q << k) | rem`；若累计 31 个 '1'，读 32 位字面值 `m`。
读位越过 payload 末尾 → `TC_ERR_TRUNCATED`（触发该 slice concealment）。

- **unary 上限 31 位是规范的一部分**：任何实现不得接受更长的 unary 前缀（禁无界 unary /
  无界 Exp-Golomb，计划 §4.4）。
- 单符号最长 63 位（31 + 32）。
- **域校验（m 上限，冻结，C-21）**：每个符号解码后立即校验 `m ≤ 上限`，
  超界 → `TC_ERR_MALFORMED`（触发 concealment）：

| 符号 | 上限（十六进制） | 推导 |
| --- | --- | --- |
| DC 残差（k1） | `0x08000000`（2^27） | \|v\| ≤ 2^26 → m = 2\|v\| ≤ 2^27 |
| AC level（k2） | `0x04000000`（2^26） | \|level\| ≤ 2^25 → m ≤ 2^26 |
| 颜色 run（k3） | `63` | 63 保留为 EOB |
| Alpha level（k1） | `0x00020000`（2^17） | \|r\| ≤ 65535 → m ≤ 131070；终结 level=0 也覆盖 |
| Alpha run（k2） | `剩余像素数` | 数据对 ≤ 剩余−1；终结对 == 剩余（§8.3） |

## 7. 颜色平面编码

每个 8×8 块依次经过以下步骤（全部整数、确定性）：

### 7.1 Level shift

`x' = x − 2^(bit_depth−1)`（10-bit：`−512..511`；12-bit：`−2048..2047`，
变换整数界按 12-bit 推导，ADR-C002 C-20）。

### 7.2 正向整数变换

```text
F = M · x' · Mᵀ
实现为两次一维整数矩阵乘（先行后列或先列后行等价——中间无舍入）：
  A = x' · Mᵀ        （水平 pass，s32 累加）
  F = M · A          （垂直 pass，s32 累加）
```

`M` 见附录 A.1（冻结）。中间不舍入、不移位。

### 7.3 量化步长

```text
Q[u][v] = max(1, ((QM[plane][u][v] · qp_scale[qp_eff]) + 128) >> 8)   // ≥ 1，显式钳位
// v1.2（R4.1）：编码侧等效量化偏移 qp_eff = qp + 4·(bit_depth−10)/2，钳位 63
// ——位深 +2 → 系数幅度 ×2 → qp_scale ×2 保持同 qp 相对精度；偏移计入
// slice 的 qp_delta_biased（码流自描述，解码侧无需位深知识；qp>59 饱和）
```

`QM` 由 `qmatrix_id` 查附录 A.3（luma/chroma 各一张 8×8 u16 表，值域 1..4095）；
`qp_scale` 见附录 A.4（u32，冻结公式与表值）。

### 7.4 量化与反量化（deadzone 均匀量化，冻结）

```text
dz    = (u==0 && v==0) ? 0 : (Q >> 2)          // DC 无死区，AC 死区 = Q/4
mag   = |F[u][v]|
qmag  = (mag + (Q >> 1) − dz) / Q              // 整数除法（向零截断）
q[u][v] = (F < 0) ? −qmag : qmag               // qmag==0 → q=0
反量化：F'[u][v] = q[u][v] · Q                  // 精确整数乘
```

### 7.5 符号化与扫描

1. **Zigzag**：按附录 A.2 表将 `q[u][v]` 展开为 64 长序列 `c[0..63]`（`c[0]` 为 DC）。
2. **DC 差分**（量化值域预测，slice 内）：
   - 可用性：`left` = 同 slice 内左邻块的 `q[0][0]`；`top` = 同 slice 内上邻块的 `q[0][0]`。
     块行首（slice 内第一块行）无 `top`；块列首（x=0）无 `left`。**跨 slice 一律不可用。**
   - `pred = 两可用 ? round_half_up((left + top) / 2) : 仅 left ? left : 仅 top ? top : 0`
     round_half_up 即数学 `floor((s+1)/2)`（s = left+top，可负）。实现必须符号拆分
     （s ≥ 0 → `(s+1)>>1`；s < 0 → `−((−s)>>1)`），**负数直接算术右移违反 §2.4**（C-24）。
   - 符号：`v_dc = c[0] − pred` → 有符号映射 → Rice(k1)。
3. **AC (run, level)**：沿 `c[1..63]`，每遇到非零系数输出一对：
   - `run` = 跳过的零个数（`0..62`），按**无符号值**直接 Rice(k3)（不做有符号映射）；
   - `level` = 系数值，有符号映射 → Rice(k2)。
   - **EOB**：最后一批非零系数之后必须紧跟一个 `run` 符号值 `63`（Rice(k3)）。
     全零 AC 块也输出 EOB（c[63] 为字面系数时同样必须在 EOB 前输出）。
     `run ∈ 0..62` 为真实游程，`63` 保留为 EOB，解码出 `>63` → MALFORMED。
   - **解码不变量（v1 修正冻结，C-22）**：维护下一 AC 槽位 `pos`（初始 1，上界 64）。
     字面系数索引 `idx = pos + run` 要求 `idx ≤ 63`（v0 的 `≤ 62` 为笔误，会误杀
     「仅 c[63] 非零」的合法块）；写入后 `pos = idx + 1`。EOB 接受于任意 `pos ≤ 64`。
     每块符号数 ≤ 65（1 DC + 63 对 + EOB），解码循环严格有界。

### 7.6 中间值界（阶段 2 已实测复证，见 ADR-C002）

以 `bit_depth = 12`（最宽）推导，10-bit 严格更小；`max|M| = 13`（附录 A.1）：

| 量 | 界 | 依据 |
| --- | --- | --- |
| `\|x'\|` | ≤ 2047 | 12-bit level shift |
| Pass1 `\|A\|` | ≤ 8·13·2047 = 212,888 < 2^18 | 每元素 ≤ max(M)·8 项 |
| Pass2 `\|F\|` | ≤ 8·13·212,888 = **22,140,352** < 2^25 | 同上（对抗输入实测恰达此值） |
| `Q` | ≤ (4095·27555+128)>>8 = 440,772 < 2^19 | A.3 值域 × A.4 最大 |
| `\|q\|` | < 2^26（实现钳位 ±2^25） | §7.4 |
| `\|F'\| = \|q·Q\|` | < 2^25（合法路径 ≤ `\|F\|+Q/2`；实现再钳位） | 反量化 |
| `G = F'·W` | ≤ 2^25·10,241 < 2^39 | s64，W ≤ 10,241（A.1） |
| 逆变换 acc | ≤ 8·13·8·13·2^39 < 2^53 | s64 必需 |

**实现要求**：正向两 pass 累加器 s32（值 < 2^25）；**逆变换全程 s64**（G/B/acc）；
`quant`/`dequant` 对输入做 ±2^25 钳位（防御层：合法码流不会触及，阶段 3 域校验兜底）。

### 7.7 重建

M 的行**精确两两正交**（M·Mᵀ = diag(E)，A.1；因此 1/(E_uE_v) 的对角归一是精确的，
不产生空间串扰）：

```text
W[u][v]   = round_half_up(2^32 / (E_u · E_v))                  // 附录 A.1，冻结，≤ 10241
G[u][v]   = F'[u][v] · W[u][v]                                 // s64
acc[y][x] = Σ_u Σ_v M[u][y] · G[u][v] · M[v][x]                // s64
x̂'[y][x] = (acc ≥ 0) ? (acc + 2^31) >> 32
                     : −((−acc + 2^31) >> 32)                   // 符号拆分四舍五入（负数不右移）
pixel     = clamp(x̂' + 2^(bit_depth−1), 0, 2^bit_depth − 1)
```

## 8. Alpha 平面编码（alpha_mode=1，无损）

Alpha **不经过** level shift / DCT / 量化（计划 §2.2），是独立的无损预测 + 熵编码路径。

### 8.1 几何与顺序

- 全分辨率（luma 同尺寸），slice 带几何与颜色一致（8 像素行的块行带）。
- 编码顺序：slice 带内像素光栅序（行主序）。

### 8.2 预测（MED，JPEG-LS 式三邻域；仅用 slice 带内像素）

邻域：`a` = 左，`b` = 上，`c` = 左上（全部位于**同一 slice 带内**）。

```text
带首行（y == 0：b、c 不可用）：pred = a（行首 x == 0 时 pred = 0）
行首列（y > 0 且 x == 0：a、c 不可用）：pred = b          ← v1.1 勘误明确
一般情形（a、b、c 均可用）：
  c ≥ max(a,b) → pred = min(a,b)
  c ≤ min(a,b) → pred = max(a,b)
  否则         → pred = a + b − c
```

残差 `r = x − pred`（`−65535..65535`）。

### 8.3 残差流（run, level 对 + 尾零终结对；v1 修正冻结，C-23）

从带首像素起：`run` = 连续零残差个数，`level` = 其后第一个非零残差。
`run` 无符号直接 Rice(k2)；`level` 有符号映射 Rice(k1)。

- **数据对**：`level ≠ 0`（由构造保证），要求 `pos + run ≤ count − 1`
  （level 落在剩余像素内）。写入后 `pos += run + 1`。
- **尾零终结对**：若流末存在零游程（含整带全零），最后输出一对
  `(trailing_run, level = 0)`，要求 `pos + trailing_run == count`（恰好填满）。
  数据对中不可能出现 `level = 0`，终结语义无歧义——v0 的「无 EOB、按计数终止」
  在尾零段会让解码器多读一对符号（TRUNCATED），故修正。
- 解码循环：`while pos < count`，每次迭代要么 `pos` 严格增加，要么终结返回；
  循环严格有界（≤ count 对）。

### 8.4 域校验

- 数据对 `run ≤ 剩余像素数 − 1`；终结对 `run == 剩余像素数`；违反 → MALFORMED；
- 解码后重建像素必须精确等于源像素（无损承诺，测试强制 bit-exact）；
- 映射值上界：`|r| ≤ 65535 → m ≤ 131070 ≤ 2^17`（escape 字面值容量内）。

### 8.5 concealment

Alpha slice 损坏时填充 **65535**（不透明）——保持画面可见，返回 `TC_WARN_CONCEALED`。

### 8.6 受限近似模式（alpha_mode=2，v1.1 激活；ADR-C004 C-32）

Alpha 平面在**进入 §8 无损路径之前**做 N-bit 预量化（N = `alpha_bit_depth` ∈ {8,10,12}），
其后的 MED 预测、残差流、解码全部与 mode 1 **逐字节同构**：

```text
s   = 16 − N
a_q = ((a + 2^(s−1)) >> s) << s          // round-half-up 到 2^s 倍数
a_q = min(a_q, (0xFFFF >> s) << s)        // 顶钳位（65535 侧最近可用倍数）
残差/熵编码按 a_q 平面执行（无损路径原样复用）
```

- **误差界**：`|a − a_q| ≤ 2^s − 1`（N=12 → 15；N=10 → 63；N=8 → 255）。实际最大
  误差由编码器统计并经 ABI 暴露（`topos_frame_stats.alpha_max_abs_error`），容器
  侧经 **tpcB** atom 文件级记录（v1.1/ADR-C014：跨帧最大值 + 目标/实际比例；
  tpcC 本体布局冻结不变）。
- **解码不变量**：mode 2 码流解出的每个 Alpha 值必须是 `2^s` 的倍数；违反 →
  `TC_ERR_MALFORMED` → 该 slice concealment（填 65535，填充值不受该不变量约束——
  concealment 是显式中性值而非码流重建）。
- v1.0 解码器对 mode 2 码流按冻结规则拒绝（`UNSUPPORTED_ALPHA_MODE`）；
  未使用 mode 2 的码流在 v1.0/v1.1 解码器间逐字节一致。

## 9. 错误模型与状态码（冻结）

返回值 `int32`：`0` 成功；`>0` 警告（帧仍交付）；`<0` 错误（无输出）。
所有错误必须可经 `tc_last_error(ctx, buf, cap)` 取得 NUL 结尾详情串。

| 值 | 名称 | 触发 |
| ---: | --- | --- |
| 0 | `TC_OK` | 成功 |
| 1 | `TC_WARN_CONCEALED` | ≥1 个 slice 被 concealment，帧已交付 |
| −1 | `TC_ERR_INVALID_ARGUMENT` | 调用方参数非法（NULL、0 容量等） |
| −2 | `TC_ERR_OUT_OF_MEMORY` | 分配失败（不泄漏部分状态） |
| −3 | `TC_ERR_UNSUPPORTED_VERSION` | major≠1 / frame_type≠0 / minor 超出已知 |
| −4 | `TC_ERR_UNSUPPORTED_PROFILE` | profile 未实现 |
| −5 | `TC_ERR_UNSUPPORTED_PIXEL_FORMAT` | pixfmt/bit_depth 未实现 |
| −6 | `TC_ERR_UNSUPPORTED_MATRIX` | qmatrix_id 未知 |
| −7 | `TC_ERR_UNSUPPORTED_ALPHA_MODE` | alpha_mode==2 |
| −8 | `TC_ERR_LIMIT_EXCEEDED` | 超过 §10 任一硬限制 |
| −9 | `TC_ERR_MALFORMED` | 结构/覆盖/flags/保留字段/值域非法 |
| −10 | `TC_ERR_TRUNCATED` | 读位越过 payload/包末尾 |
| −11 | `TC_ERR_CHECKSUM_MISMATCH` | CRC 不符（header CRC → 整帧拒绝；slice CRC → concealment） |
| −12 | `TC_ERR_STATE` | 调用顺序错误（未 open/已 close） |
| −13 | `TC_ERR_CANCELLED` | 取消标志在 slice 边界被观察到 |
| −14 | `TC_ERR_IO` | IO callbacks 失败 |
| −15 | `TC_ERR_BUFFER_TOO_SMALL` | 调用方输出缓冲不足（附所需尺寸） |
| −16 | `TC_ERR_NOT_IMPLEMENTED` | 预留能力被显式请求 |

**Concealment 规则（v1.0 策略，冻结）**：

| 故障 | 行为 |
| --- | --- |
| frame header CRC 失败 / 结构验证失败 | 整帧拒绝（错误码返回，不产出帧） |
| slice 几何/覆盖验证失败 | 整帧拒绝 `TC_ERR_MALFORMED`（几何谎言无法局部化） |
| slice payload CRC 失败 / 熵解码 TRUNCATED/MALFORMED | **仅该 slice** 填中性值：颜色 `2^(bit_depth−1)`（中灰）、Alpha `65535`；帧返回 `TC_WARN_CONCEALED`，逐 slice 状态可查询 |
| 单 slice 故障 | 不得影响其他 slice、其他平面、后续帧 |

## 10. 解码器硬限制（冻结）

| 限制 | 值 |
| --- | --- |
| coded_width × coded_height | 各 ≤ 8192，%8 == 0 |
| slice_count（全帧） | ≤ 512 |
| frame_packet_size | ≤ 256 MiB（268,435,456） |
| plane_count | ∈ {3,4} |
| 每块 AC 符号对 | ≤ 63（EOB 前） |
| Rice k | ≤ 14 |
| unary 前缀 | ≤ 31 位 |
| escape 字面值 | 32 位 |
| qp（base 与 slice 有效值） | 0..63（v1.5/minor=4 扩展至 0..95） |
| 有界完成性 | 每 slice 解码符号数有上界（块数×65 / 像素数×2），任何实现不得出现无界循环或无界分配 |

## 11. 编码器一致性要求

1. **确定性**：相同输入帧 + 相同配置 → **bit-exact 相同的 frame packet**。
   禁止依赖墙钟、线程调度、浮点环境；帧级分析（qp 选择）必须是输入的纯函数。
2. **slice 划分**：编码器自由选择带高，但必须满足 §4.3 覆盖规则；并行实现不得改变输出。
3. **统计与预算**：编码器必须分别统计并经 ABI 暴露 颜色 payload、Alpha payload、总 payload
   字节数。Alpha 目标比例/硬上限（profile 对应 25%–30%，计划 §2.2）是编码器策略约束：
   - 无损模式（v1.0）：超上限时**不得静默降质**——要么带警告交付（调用方明确允许），
     要么返回可诊断错误；
   - 受限近似模式（阶段 4）：在硬上限内执行显式近似并记录最大绝对误差
     （应用层写入容器 tpcB atom，container_spec §4 v1.1；模式内允许 12→10→8
     位深自适应——文件级定深，决策为输入的纯函数，ADR-C014）。
4. **最大帧大小**：超出配置上限 → `TC_ERR_LIMIT_EXCEEDED`，明确诊断，禁止静默截断。
5. **禁止输出 concealment**：编码器产出的码流必须全部通过自身解码验证（阶段 4 起强制
   self-round-trip gate）。
6. **padding/crop**：按 §5 规则确定性填充；visible 区域外的 padding 值不构成兼容性承诺
   （解码器必须裁剪）。

## 12. 容器映射摘要（阶段 5 定稿）

- MOV video sample entry FourCC：`TPIC`；每个 video sample = 一个 frame packet（整字节）。
- 私有配置 atom `tpcC`（codec configuration）：位流版本、profile、pixel format、位深、
  色彩集（range/primaries/transfer/matrix/siting）、SAR、Alpha 模式/目标比例/实际比例/
  最大误差、GOP 结构标志、编码器版本串。与 frame header 冗余字段冲突时以**先读到的容器值
  做路由、逐帧 header 值做解码**，冲突 = 文件级错误。
- 标准采样表（stsd/stts/stsc/stsz/stco 或 co64）；>4GB 必须 co64；FastStart 为封装后重排步骤。
- 未识别私有 atom 按 atom size 安全跳过；非 Topos reader 应能忽略未知 codec 而不崩溃。
- 音频经 libavformat 常规流（ADR C-18），不进 elementary packet。

## 13. 版本化与扩展规则

### 13.1 minor 版本（向后兼容扩展）

- 允许：在 header CRC 前追加字段（`header_size` 增大）；新增 profile/pixfmt/矩阵/色彩 tag
  枚举值；新增 slice 类型。旧解码器行为必须是「干净拒绝」（UNSUPPORTED 类错误码）或
  「完全一致解码」，**绝不允许部分理解或猜测**。
- v1.x 中，凡 v1.0 可解的配置，其保留字段必须为 0、slice 布局不变。
- 未知 `color_primaries/transfer/matrix` → 拒绝（色彩解释错误比拒绝更糟）。

### 13.2 major 版本（不兼容变更）

任何无法满足 13.1 的变更（语法、语义、错误模型变化）→ `version_major += 1`，
旧解码器返回 `TC_ERR_UNSUPPORTED_VERSION` 且不产出帧。

### 13.3 实现后端无关性

规范只定义**编码字节流**与**解码输出平面**。标量、AVX2、NEON、任何 GPU/混合后端必须：
编码产出 bit-exact 相同码流；解码产出 bit-exact 相同平面。后端差异只允许出现在吞吐/延迟，
一经 conformance 发现输出差异即不合格（计划 §9 风险表）。

---

## 附录 A（规范性数值表）

### A.1 变换矩阵 M / 行能量 E / 归一权重 W（阶段 2 冻结，ADR-C002）

M 由 DCT-8 基的对称（u=0,2,4,6）/反对称（u=1,3,5,7）结构整数化构造（×13 缩放，
组内整数搜索达到**精确两两正交**）；生成与验证脚本：`native/topos_codec/tools/gen_transform_tables.py`。

```text
M（行主序 [u][x]，|M| ≤ 13）：
u=0:  13  13  13  13  13  13  13  13
u=1:  13  11   7   1  -1  -7 -11 -13
u=2:  12   5  -5 -12 -12  -5   5  12
u=3:  11  -4 -13  -8   8  13   4 -11
u=4:   9  -9  -9   9   9  -9  -9   9
u=5:   8 -13   4  11 -11  -4  13  -8
u=6:   5 -12  12  -5  -5  12 -12   5
u=7:   1  -7  11 -13  13 -11   7  -1

E[u]（行能量）：1352, 680, 676, 740, 648, 740, 676, 680
W[u][v] = round_half_up(2^32/(E_u·E_v))（u16 容量内，2350..10241）：
      v=0    v=1    v=2    v=3    v=4    v=5    v=6    v=7
u=0:  2350   4672   4699   4293   4902   4293   4699   4672
u=1:  4672   9288   9343   8535   9747   8535   9343   9288
u=2:  4699   9343   9399   8586   9805   8586   9399   9343
u=3:  4293   8535   8586   7843   8957   7843   8586   8535
u=4:  4902   9747   9805   8957  10228   8957   9805   9747
u=5:  4293   8535   8586   7843   8957   7843   8586   8535
u=6:  4699   9343   9399   8586   9805   8586   9399   9343
u=7:  4672   9288   9343   8535   9747   8535   9343   9288
```

性质（tests/unit/test_transform.c 在 C 侧持续复验）：任意 u≠v 行点积 = 0；
u≠0 行和 = 0（非 DC 行对 flat 块零贡献）；与理想 DCT-8 基相关性 ≥ 0.996。

### A.2 Zigzag 扫描表（行主序索引，冻结）

```text
 0  1  8 16  9  2  3 10
17 24 32 25 18 11  4  5
12 19 26 33 40 48 41 34
27 20 13  6  7 14 21 28
35 42 49 56 57 50 43 36
29 22 15 23 30 37 44 51
58 59 52 45 38 31 39 46
53 60 61 54 47 55 62 63
```

### A.3 量化矩阵集（结构冻结；id=1 数值 v1.1 冻结，id=2 为图片 444 体积扩展）

- 值域：每项 u16，`1..4095`；luma/chroma 各一张 8×8（自然序 `[u][v]`）。
- `qmatrix_id = 0`：flat —— luma/chroma 全部 `16`（**已冻结**）。
- `qmatrix_id = 1`：**Topos Standard（v1.1 冻结，阶段 4 调优）**——几何递增频率加权：
  `luma[u][v] = round(16·1.25^(u+v))`、`chroma[u][v] = round(16·1.18^(u+v))`
  （luma 高频比 chroma 粗：控噪主导码率；chroma 精细：键控/肤色工作流。
  调优记录与依据见 `quality_report_scalar.md` §6、ADR-C004 C-31）。

```text
luma（自然序 [u][v]）：
  16  20  25  31  39  49  61  76
  20  25  31  39  49  61  76  95
  25  31  39  49  61  76  95 119
  31  39  49  61  76  95 119 149
  39  49  61  76  95 119 149 186
  49  61  76  95 119 149 186 233
  61  76  95 119 149 186 233 291
  76  95 119 149 186 233 291 364

chroma（自然序 [u][v]）：
  16  19  22  26  31  37  43  51
  19  22  26  31  37  43  51  60
  22  26  31  37  43  51  60  71
  26  31  37  43  51  60  71  84
  31  37  43  51  60  71  84  99
  37  43  51  60  71  84  99 117
  43  51  60  71  84  99 117 138
  51  60  71  84  99 117 138 162
```

- `qmatrix_id = 2`：**Topos 444 Compact（图片层开发扩展）**——只允许
  `pixel_format=2`、`bit_depth=12`、`profile∈{5,6}`。它在 Standard 的频率
  形状上提高中高频步长（luma 约 1.8×、chroma 约 1.5×，DC 保持 16），用于
  将 1920×1080 GBR 4:4:4 12-bit 的 High/Ultra 档控制在约 4 MB/帧；不改变
  平面布局、色彩矩阵或解码算法。

```text
luma（444 Compact）：
  16  36  45  56  70  88 110 137
  36  45  56  70  88 110 137 171
  45  56  70  88 110 137 171 214
  56  70  88 110 137 171 214 268
  70  88 110 137 171 214 268 335
  88 110 137 171 214 268 335 419
 110 137 171 214 268 335 419 524
 137 171 214 268 335 419 524 655

chroma（444 Compact）：
  16  28  33  39  46  56  64  76
  28  33  39  46  56  64  76  90
  33  39  46  56  64  76  90 106
  39  46  56  64  76  90 106 126
  46  56  64  76  90 106 126 148
  56  64  76  90 106 126 148 176
  64  76  90 106 126 148 176 207
  76  90 106 126 148 176 207 243
```

- `qmatrix_id = 3`：**Topos 422 Low Compact（图片层开发扩展）**——只允许
  `pixel_format=0`、10-bit、`profile=3`。它在 Standard 的频率形状上做约
  1.08× luma / 1.05× chroma 的轻度加权，DC 保持 16，用于 Low 档在 qp=63
  已达上限时再收缩少量高频码量；不改变平面布局或解码路径。

```text
luma（422 Low Compact）：
  16 22 27 33 42 53 66 82
  22 27 33 42 53 66 82 103
  27 33 42 53 66 82 103 129
  33 42 53 66 82 103 129 161
  42 53 66 82 103 129 161 201
  53 66 82 103 129 161 201 252
  66 82 103 129 161 201 252 314
  82 103 129 161 201 252 314 393

chroma（422 Low Compact）：
  16 20 23 27 33 39 45 54
  20 23 27 33 39 45 54 63
  23 27 33 39 45 54 63 75
  27 33 39 45 54 63 75 88
  33 39 45 54 63 75 88 104
  39 45 54 63 75 88 104 123
  45 54 63 75 88 104 123 145
  54 63 75 88 104 123 145 170
```

- `4..255`：保留（解码器 → `TC_ERR_UNSUPPORTED_MATRIX`）。

### A.4 qp_scale 表（公式与数值冻结；v1.5 扩展 64..95）

公式：`qp_scale[qp] = max(1, round_half_up(2^((qp−4)/4)))`，`qp ∈ 0..63`（v1.0 冻结）；
**v1.5（ADR-C031，minor=4）同公式扩展 `qp ∈ 64..95`**（整数四分幂精确验证；
qp63 表值 27555 为 v1.0 历史冻结值——公式真值 27554.49，随 golden 冻结不改）。

```text
qp:      0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
scale:   1  1  1  1  1  1  1  2  2  2  3  3  4  5  6  7
qp:     16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
scale:   8 10 11 13 16 19 23 27 32 38 45 54 64 76 91 108
qp:     32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47
scale: 128 152 181 215 256 304 362 431 512 609 724 861 1024 1218 1448 1722
qp:     48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63
scale: 2048 2435 2896 3444 4096 4871 5793 6889 8192 9742 11585 13777 16384 19484 23170 27555
qp:       64     65     66     67      68      69      70      71
scale:  32768  38968  46341  55109   65536   77936   92682   110218
qp:       72      73      74      75      76      77       78       79
scale: 131072  155872 185364  220436  262144  311744  370728   440872
qp:       80       81       82       83        84        85        86        87
scale: 524288   623487  741455  881744   1048576   1246974   1482910   1763488
qp:        88        89        90        91        92        93        94       95
scale:  2097152   2493948  2965821  3526975   4194304   4987896   5931642  7053950
```

v1.5 域上界依据：qp95 × A.3 冻结矩阵最大值（qm=655，444 Compact 表尾）
→ Q = 18,045,205 < 快速除法域上界 2^25−1（`fastdiv.h`：N_MAX·D_MAX < 2^50，
域外保守回退真除，数值恒真）。

### A.5 有符号映射（公式冻结）

见 §6.1。示例：`0→0, 1→2, −1→1, 2→4, −2→3, …`

### A.6 CRC-32（定义冻结）

IEEE 802.3 / zlib 兼容（poly `0xEDB88320` 反射，init `0xFFFFFFFF`，final xor `0xFFFFFFFF`）。
自测：`"123456789"` → `0xCBF43926`（实现必须以此 vector 自检）。

### A.7 色彩标签枚举（冻结；对齐 H.273 编号）

| 字段 | 值→语义（与应用层 `VideoMetadata` 字符串映射） |
| --- | --- |
| color_primaries | `1`=bt709（'bt709'）、`6`=smpte170m（'smpte170m'）、`9`=bt2020（'bt2020'）、`12`=smpte431p3（'smpte431' DCI-P3） |
| color_transfer | `1`=bt709（'bt709'）、`8`=linear、`16`=smpte2084（'smpte2084' PQ）、`18`=arib-std-b67（'arib-std-b67' HLG） |
| color_matrix | `0`=identity（GBR 预留）、`1`=bt709（'bt709'）、`5`=smpte170m（'smpte170m'）、`9`=bt2020nc（'bt2020nc'） |
| color_range | `0`=limited、`1`=full |
| chroma_siting | `0`=left、`1`=center、`2`=topleft（应用层映射到未来的 `VideoMetadata.chroma_siting`） |

### A.8 冻结规则

本 v1.1 的语法与语义（header/slice 布局、§6 熵方案与域上限、§7.4 量化公式、§7.5 扫描与
预测不变量、§8 Alpha 路径（含 §8.6 近似模式）、§9/§10 错误与限制、附录全部数值表）由
两组 golden 与四配置测试门禁冻结：

- `native/topos_codec/tests/conformance/golden_bitstream_v1.bin`（56 条记录，fold
  `1fd98ecbe7ae0e99`）—— 位流层（header/熵/块符号/packet）。
- `native/topos_codec/tests/conformance/golden_codec_v1.bin`（16 条记录，fold
  `7912af9cd4510822`）—— **阶段 4 全链路**：8 组确定性配置的完整 packet 字节 +
  解码输出平面指纹（含 A.3 Standard 表值、k 估计、padding、slice 划分、重建）。

任何后续变更必须走 §13 版本规则、记录 ADR，并重新生成 golden（破坏字节兼容的变更提升
`version_major`，向后兼容扩展提升 `version_minor`）。A.3 `qmatrix_id=1` 的调优豁免
**已于 v1.1 关闭**（表值随 golden_codec_v1.bin 冻结）。
