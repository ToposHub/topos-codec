# TPIC V7-A：可跳过 coefficient-band 位流规范

状态：**RD2-01～RD2-09 已实施，V7-A reader 已进入公共 elementary-frame API，仍未进入默认 writer**。本文定义
V7-A 的 packet 边界、目录、segment 范围、CRC、能力和拒绝语义；V7-A 仍不能
作为默认 writer，也不能替代 V7-B 的 2K spatial base。

关联：`ADR-C029-v7a-band-directory.md`、`bitstream_spec_v6_range.md`、
`ADR-C027-v2-schema-reconciliation.md`。除本文显式修订的条款外，公共 TPIC
frame header、色彩/Alpha/几何/packet 上限和错误模型沿用既有规范。

## 1. 版本和 packet 总体布局

V7-A 使用 TPIC frame header 的 53-byte 公共布局：

| 字段 | 约束 |
| --- | --- |
| `version_major` | `7` |
| `version_minor` | `0` |
| `frame_packet_size` | 必须等于实际 packet size，且 ≤ 256 MiB |
| `entropy_mode` | `5` |
| `codebook_version` | `5` |
| `coding_mode` | `2` |
| `frame_type/gop_id/ref_distance` | `0/0/0` |
| `flags` | 仅 bit0 `alpha_premultiplied`，其余为 0 |

V7-A packet 的字节布局为：

```text
0                                      53
| TPIC frame header                     |
53                                     89
| TPLD fixed header                     |
89                                     113
| layer descriptors                     |
113                                    113 + 24*S
| slice descriptors                     |
113 + 24*S                             directory_end (= 53 + directory_size)
| segment descriptors                   |
payload_offset (= directory_end)       frame_packet_size
| segment payload region                |
```

其中 `S = slice_count`，segment descriptor 数量为 `S × 5`。现有 TPIC 的
`slice_count` 是所有 plane 的 slice 总数；每个 slice 只属于一个 plane，
由 slice descriptor 的 `plane_index` 指定。V7-A 不使用 V1～V6 的 17-byte
slice header；slice 的几何和 payload 定位由 `TPLD` 目录提供。

## 2. TPLD fixed header（36 bytes）

所有多字节整数均为 BE；offset 从 TPIC frame packet 起点计。

| offset | size | 字段 | V7-A 规则 |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | ASCII `TPLD` |
| 4 | 2 | `directory_version_major` | `1` |
| 6 | 2 | `directory_version_minor` | `0` |
| 8 | 4 | `flags` | 必须为 0；未知非零 flag 按 critical 拒绝 |
| 12 | 2 | `layer_count` | 必须为 1 |
| 14 | 2 | `slice_count` | 必须等于 frame header 的 `slice_count` |
| 16 | 1 | `band_count` | 必须为 5 |
| 17 | 1 | `plane_count` | 必须等于 frame header 的 `plane_count`，且 1..4 |
| 18 | 2 | `layer_descriptor_size` | 必须为 24 |
| 20 | 2 | `slice_descriptor_size` | 必须为 24 |
| 22 | 2 | `segment_descriptor_size` | 必须为 20 |
| 24 | 4 | `directory_size` | `36 + 24 + 24*S + 20*(S*5)`，≤ 64 KiB |
| 28 | 4 | `payload_offset` | 必须等于 `53 + directory_size`，且 ≤ `frame_packet_size` |
| 32 | 4 | `directory_crc32` | 对 directory 原始字节计算，字段自身置 0 |

`P = plane_count`。`directory_size` 的乘法和加法必须通过 checked arithmetic
计算后再比较上限；不能用来自文件的值直接作为分配长度或循环边界。

## 3. Layer descriptor（24 bytes）

V7-A 只有一条 layer，但使用 descriptor 为 V7-B 预留明确分流点：

| offset | size | 字段 | V7-A 规则 |
| ---: | ---: | --- | --- |
| 0 | 1 | `layer_id` | `0` |
| 1 | 1 | `layer_kind` | `0` = band-only |
| 2 | 2 | `layer_flags` | 必须为 0 |
| 4 | 4 | `first_slice_descriptor` | `0` |
| 8 | 4 | `slice_count` | 必须等于 `S` |
| 12 | 4 | `first_segment_descriptor` | `0` |
| 16 | 4 | `segment_count` | 必须等于 `S*5` |
| 20 | 2 | `band_layout_version` | `1` |
| 22 | 2 | `reserved` | 必须为 0 |

所有 index/count 都在目录内部做 checked 范围验证。未知 `layer_kind`、未知
`band_layout_version` 或非零保留位返回 `TC_ERR_UNSUPPORTED_VERSION`。

## 4. Slice descriptor（24 bytes）

slice descriptor 按 `slice_index` 升序排列，且必须覆盖 frame header 声明的
所有 slice：

| offset | size | 字段 | 规则 |
| ---: | ---: | --- | --- |
| 0 | 2 | `slice_index` | 等于数组下标，`0..S-1` |
| 2 | 2 | `slice_flags` | 必须为 0 |
| 4 | 4 | `first_segment_index` | 等于 `slice_index * 5` |
| 8 | 4 | `segment_count` | 必须为 `5` |
| 12 | 4 | `block_y0` | 由现有 slice 几何推导，单位为 block row |
| 16 | 2 | `block_h` | ≥1，且不越过对应 plane 的 block rows |
| 18 | 1 | `plane_index` | `0..P-1`，与该 slice 的 5 个 segment 一致 |
| 19 | 1 | `band_count` | 必须为 5 |
| 20 | 4 | `reserved` | 必须为 0 |

slice 的几何校验必须复用现有 `slice_map` 规则；目录字段不得绕过 plane
尺寸、slice 覆盖或最后一带边界校验。

## 5. Segment descriptor（20 bytes）

segment descriptor 按以下固定顺序排列：

```text
slice 0: plane <slice[0].plane_index>: B0, B1, B2, B3, B4
slice 1: plane <slice[1].plane_index>: B0, B1, B2, B3, B4
...
```

| offset | size | 字段 | 规则 |
| ---: | ---: | --- | --- |
| 0 | 4 | `payload_offset` | packet 绝对 offset；空 segment 必须为 0 |
| 4 | 4 | `payload_size` | 非空 >0；空 segment =0 |
| 8 | 4 | `payload_crc32` | 非空 payload CRC；空 segment =0 |
| 12 | 2 | `slice_index` | 必须与所属 slice descriptor 一致 |
| 14 | 1 | `plane_index` | `0..P-1` |
| 15 | 1 | `band_index` | `0..4` |
| 16 | 2 | `segment_flags` | 必须为 0 |
| 18 | 2 | `reserved` | 必须为 0 |

非空 segment 必须满足：

```text
payload_offset >= TPLD.payload_offset
payload_offset + payload_size <= frame_packet_size
```

按 segment index 观察，所有非空区间必须连续、升序、互不重叠，第一段从
`TPLD.payload_offset` 开始，最后一段结束于 `frame_packet_size`。空 segment
没有字节区间，不参与连续性检查；其 CRC 语义固定为 0。

## 6. Coefficient band 定义

每个 plane 的每个 8×8 block 使用既有 zigzag 扫描编号 0..63。V7-A 固定：

| band | zigzag 范围 | 语义 |
| ---: | --- | --- |
| B0 | 0..4 | DC + 最低 4 个 AC；1/8 reduced 请求的最低必要 band |
| B1 | 5..10 | 1/4 reduced 的附加低频 |
| B2 | 11..16 | 1/3 reduced 的附加低频 |
| B3 | 17..24 | 1/2 reduced 的附加低频 |
| B4 | 25..63 | full reconstruction enhancement |

band 范围按 `band_layout_version=1` 永久冻结。V7-A reduced request 到 band 的
映射为：

| request | 打开 bands |
| --- | --- |
| 1/8 | B0 |
| 1/4 | B0..B1 |
| 1/3 | B0..B2 |
| 1/2 | B0..B3 |
| full | B0..B4 |

### 6.1 Segment payload scalar syntax（syntax version 1）

每个非空 segment 的 payload 从 byte boundary 开始，先写 4-byte 小头：

| offset | size | 字段 | 规则 |
| ---: | ---: | --- | --- |
| 0 | 1 | `syntax_version` | `1` |
| 1 | 1 | `k_dc_or_run` | B0 为 DC 的 Rice `k`；B1..B4 为 local_run 的 Rice `k`，均为 0..14 |
| 2 | 1 | `k_level` | AC level 的 Rice `k`，0..14 |
| 3 | 1 | `flags` | bit0=`pair_count_map`；其余位必须为 0 |

所有 Rice 符号沿用既有 bounded Rice 语法（`q≤30` 为 unary+suffix，
`q=31` 为 32-bit escape）。B0 的 DC `m` 上限为 `TC_RICE_M_MAX_DC`，
AC level `m` 上限为 `TC_RICE_M_MAX_AC_LEVEL`。

令 `N` 为该 slice 的 block 数（`block_h × plane_block_cols`），令
`index_bits = max(1, ceil(log2(N)))`。每一 band 内的 token 按 block raster
顺序排列；同一 block 内按递增 scan position 排列。`local_run` 是该 band
起点到当前非零系数前一个非零系数之间的零系数数目：首 token 的前一个
位置等于 band 起点减一。解码后必须满足累计位置仍在 band 范围内。

#### B0（band 0）

B0 需要每个源 block 的 DC，因此不写 active-block map。对每个 block 依次写：

```text
dc_m:     Rice(k_dc)
pair_count: 3 bits, 0..4
repeat pair_count times:
  local_run: 2 bits, 0..3
  level_m:   Rice(k_level)
```

`dc_m` 是当前 block 的 DC 与 slice 内左/上预测值之差的 signed mapping。
DC 预测边界和已有 slice 一样，在 slice 开始清空；B0 不写 EOB，
`pair_count` 是该 block 的终止标记。

#### B1～B4（enhancement bands）

增强段根据 `flags.bit0` 选择一种 map。编码器按精确 bit 数选择较小者；
同位数时选择 active-block map，保证旧的 0 flag 语义稳定。

当 `flags.bit0=0` 时，增强段先写一个 BE `active_block_count`（u32），随后只写有非零系数的
block。每个 active block 写：

```text
block_delta: index_bits bits
pair_count:  6 bits, 1..band_length
repeat pair_count times:
  local_run: Rice(k_dc_or_run)，上限为 `TC_RICE_M_MAX_RUN`
  level_m:   Rice(k_level)
```

第一个 active block 的前置 block index 为 0；之后
`block_delta = block_index - previous_block_index - 1`。因此 block index
严格递增，且最后一项不能越过 `N-1`。band length 依次为
`B1=6、B2=6、B3=8、B4=39`，local_run 的取值范围分别为 `0..5、0..5、0..7、0..38`，
实际位数由 `k_dc_or_run` 的 Rice 参数决定。
active count 必须等于实际条目数；`B1..B4` 不重复写 DC，不重复写 EOB；
其 `k_dc_or_run` 不再表示 DC，而表示 local_run 的 Rice 参数。

当 `flags.bit0=1` 时，增强段改写为全块 `pair_count map`，避免在 active block
密集时为每个 index 重复付出 `index_bits+6`：

```text
for each block in raster order:
  pair_count: 3 bits (B1/B2/B3) 或 6 bits (B4)
  repeat pair_count times:
    local_run: Rice(k_dc_or_run)
    level_m:   Rice(k_level)
```

`pair_count=0` 的块不写 token；该 map 仍然按 band 固定宽度消耗，
`pair_count` 不得超过该 band 的 coefficient 数量。

segment 末尾 zero-pad 到 byte boundary。reader 解析完目录声明的 block/token
后必须检查 padding 全为零并且恰好耗尽 segment payload，不接受尾随整字节。

## 7. 解析与校验顺序

V7 reader 必须按以下顺序执行，失败即停止：

1. `size >= 53`，再解码 TPIC frame header 和 header CRC；
2. `version_major/minor` 与 V7-A 组合校验；
3. `size >= 53+36`，检查 `TPLD` magic、directory version、固定字段和上限；
4. 使用 checked arithmetic 计算 descriptor 区间，验证整个目录落在 packet 内；
5. 校验 `directory_crc32`；
6. 校验 layer 数量/范围/能力，slice 数量和每条 slice 的几何覆盖；
7. 校验每条 segment 的身份、offset/size、连续性、不重叠和 packet 边界；
8. 对 decode request 选中的非空 segment 执行 CRC 和后续 token 语法校验；
9. full request 才执行所有剩余 segment 的 CRC 和 token 语法校验。

目录解析不得为 segment payload 创建与文件大小相等的临时副本；应使用指向
调用方 packet 的 bounded view。V7-A reduced 路径对未选中的段不得 `memcpy`、
不得初始化 entropy reader、不得调用 CRC。跳过数量和字节数必须进入诊断。

## 8. 错误模型和旧 reader 行为

| 条件 | 返回码 |
| --- | --- |
| packet/header/directory 不足固定长度 | `TC_ERR_TRUNCATED` |
| count、directory 或 packet 超过硬上限 | `TC_ERR_LIMIT_EXCEEDED` |
| offset/size 溢出、越界、重叠、身份或 slice map 非法 | `TC_ERR_MALFORMED` |
| header/directory/已请求 segment CRC 不匹配 | `TC_ERR_CHECKSUM_MISMATCH` |
| 未知 major/minor、layout、layer、descriptor size 或 critical flag | `TC_ERR_UNSUPPORTED_VERSION` |
| V7 token 尚未实现或请求能力不可用 | `TC_ERR_NOT_IMPLEMENTED` 或统一 request 的明确 fallback |

V1～V6 reader 必须在 frame-header major 分流处对 `version_major=7` 返回
`TC_ERR_UNSUPPORTED_VERSION`；它不能把 `TPLD` 当成 slice header，也不能在
拒绝前写输出 plane。V7-aware reader 对未知 critical 语义 fail closed；源 packet
保持原样，不自动重写成旧 major。

## 9. 兼容承诺

- V1～V6 packet 的字节布局、golden、reader 和 writer 不变。
- V7-A `band_layout_version=1` 的 band 边界、segment index、CRC 算法和空段
  语义不可变；新语义使用新 layout version 或新 major。
- V7-A 不等于 V7-B：它允许跳过高频系数段，但不能提供与源空间尺寸无关的 2K
  base 负载。任何 8K/12K/16K 的“2K 级”宣传必须等 RD4 base-only 门通过。
