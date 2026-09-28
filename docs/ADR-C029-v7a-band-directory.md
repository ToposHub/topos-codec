# ADR-C029 — TPIC V7-A 可跳过 coefficient-band 目录

- 日期：2026-09-08
- 状态：已接受（RD2-01 规范冻结；实现由 RD2-02～RD2-09 分批交付）
- 关联：ADR-C027（帧头字段注册表）、ADR-C026（验收门）、
  `docs/bitstream_spec_v7.md`、
  `docs/topos_prores_style_reduced_decode_implementation_plan.md`

## 0. 背景

V1～V6 的 reduced 解码可以限制反量化/逆变换的系数数量，但旧熵语法是连续
的变长码流：解码器无法在不读取前置码字的情况下跳到某个 AC 频带。因此它
不是“只读取低频字节”的格式级能力，不能把 8K/12K 的工作量宣称为 2K 上限。

V7-A 是一个低风险的中间格式：保持现有 8×8 DCT、量化和 slice 几何，把
量化系数按固定频带写入独立、可定位、可校验的 segment。reduced reader 只
读取请求的 segment，full reader 才读取全部 segment。V7-A 不复制缩略图，
也不改变源空间块数量；8K/12K 的分辨率无关负载仍由 V7-B 负责。

## 1. 决策

### D-1：分配独立 major

1. V7-A 使用 `version_major=7`、`version_minor=0`。
2. 保留公共 TPIC 53-byte frame header；header 后紧跟 `TPLD` directory，
   V7 不再把旧的 17-byte slice header 当作 packet 目录。
3. V7-A frame header 的已注册组合为：

   | 字段 | V7-A 值 |
   | --- | ---: |
   | `entropy_mode`（byte 45） | 5 |
   | `codebook_version`（byte 46） | 5 |
   | `coding_mode`（byte 47） | 2 |
   | `reserved_v2_0`（byte 48） | 0 |
   | `frame_type/gop_id/ref_distance` | 0 |
   | `flags` | 仅 bit0 `alpha_premultiplied` |

   `entropy_mode=5` 表示 V7-A band-local scalar syntax；值 5 的具体 token
   语法只由 V7 spec 和 RD2-03/RD2-04 实现，不得解释成 V1/V2 熵模式。
4. V7-A 只支持 `band_layout_version=1`、`layer_kind=0`（band-only）、5 个
   coefficient bands 和当前已支持的 1～4 个 plane。`slice_count` 继续沿用现有
   语义：它是所有 plane 的 slice 总数；每个现有 slice 只属于一个 plane，
   对应 5 个 band segments。V7-B 使用新的 layer kind，不得复用
   `layer_kind=0` 的含义。

### D-2：频带含义永久固定

按每个 8×8 block 的 zigzag 扫描位置定义：

| `band_index` | 系数位置（含端点） | reduced 用途 |
| ---: | --- | --- |
| 0 | 0..4 | 1/8 |
| 1 | 5..10 | 1/4 |
| 2 | 11..16 | 1/3 |
| 3 | 17..24 | 1/2 |
| 4 | 25..63 | full enhancement |

`band_layout_version=1` 的含义永不改变。未来调整界限必须分配新的 layout
version，不得重解释已经发布的 segment。

### D-3：目录和 segment 均使用显式 BE pack

目录的所有整数均为 big-endian；禁止直接序列化 C struct。每个 segment 都
拥有 `payload_offset`、`payload_size` 和 `payload_crc32`。offset 是相对于
当前 TPIC frame packet 起点的绝对 packet offset，因 packet 上限为 256 MiB，
V7-A 使用 u32；所有加法、乘法、区间判断都必须使用 checked arithmetic。

V7-A 的 `TPLD` 本身是 critical。RD2-02 对未知的非零 critical/保留 flag、
未知 directory/layer/segment 版本一律返回 `TC_ERR_UNSUPPORTED_VERSION`，
不得猜测布局；RD2-01 不定义可静默忽略的 optional record。

### D-4：CRC 分层与 reduced 语义

1. `directory_crc32` 使用现有 IEEE 802.3 `tc_crc32`，覆盖整个 directory，
   计算时仅把 CRC 字段本身置零；不覆盖 payload。
2. 每个非空 segment 的 `payload_crc32` 覆盖该 segment 的原始 payload 字节。
   空 segment 固定为 `offset=0,size=0,crc=0`，不参与范围重叠检查。
3. 目录解析阶段必须验证 directory CRC、所有 segment 的边界、数量和不重叠；
   reduced decode 只对请求的 bands 读取并校验 CRC，跳过段不初始化 entropy
   reader、不读取、不校验 CRC。
4. full decode 必须打开并校验所有非空 segment。输出诊断记录：
   `segments_requested`、`segments_read`、`segments_skipped`、
   `unchecked_segment_count`；V7-A reduced 的 `segments_skipped` 是实际字节
   跳过，不得把“读取后丢弃系数”记作跳过。

### D-5：兼容和拒绝

- V1～V6 reader 在 frame-header major 分流处见到 7，必须立即返回
  `TC_ERR_UNSUPPORTED_VERSION`，不尝试按旧 slice 布局解析，也不分配像素输出。
- V7 reader 见到未知 `version_minor`、`band_layout_version`、`layer_kind`、
  critical flag 或 descriptor size，返回 `TC_ERR_UNSUPPORTED_VERSION`。
- 目录字段截断、checked arithmetic 溢出、segment 越过 packet、segment 重叠、
  非法 descriptor 关系返回 `TC_ERR_TRUNCATED`、`TC_ERR_LIMIT_EXCEEDED` 或
  `TC_ERR_MALFORMED`；固定校验字段不匹配返回 `TC_ERR_CHECKSUM_MISMATCH`。
- V7 reader 缺少所需 band 能力时必须显式失败或按统一 decode request 选择
  已声明的旧路径；不得把 V7-A packet 伪装成 V1～V6 reduced native decode。

## 2. 目录边界冻结

`TPLD` 的固定头为 36 bytes，后面依次为 layer descriptors、slice descriptors
和 segment descriptors。V7-A 的 descriptor size 固定，目录不允许隐式尾随字段：

```text
TPIC frame header (53)
TPLD fixed header (36)
layer[0] (24)
slice[0 .. slice_count-1] (24 each)
segment[0 .. slice_count*5-1] (20 each)
payload region
```

固定上限：`slice_count ≤ 512`、`plane_count ≤ 4`、segment 数量必须等于
`slice_count × 5`；`directory_size ≤ 64 KiB`，该上限覆盖最大合法组合的
63,548 bytes 目录。所有 descriptor 必须完整落在
`[53, directory_size)` 内，`payload_offset` 必须不小于目录末尾且不大于
`frame_packet_size`。

V7-A payload 中不允许目录与 segment 之间的隐藏数据：按 segment index 顺序，
所有非空 segment 必须从 `payload_offset` 开始连续排列，最后一个非空 segment
必须恰好结束于 `frame_packet_size`。这样 seek、字节计数和损坏定位不会依赖
未定义的 padding 语义。

## 3. 能力标识

V7-A 的 wire capability 由三元组唯一表示：

```text
bitstream_major = 7
layer_kind = 0 (band-only)
band_layout_version = 1
```

容器/图片层若声明 capability，必须完整携带这三个字段；只声明“V7”不足以
判断能否读取 band。公共 `tc_query_support()` 的扩展和 MOV/TPIM capability
同步列入 RD7，RD2-01 不借用现有 ABI 的 reserved 字段伪造查询结果。

## 4. 后果和边界

- V7-A 只解决高频 coefficient segment 的 byte-level skip；它不把每个源空间
  block 的 B0 解码成本变成 2K 成本。
- 旧文件保持可读，旧 writer 不变；V7 writer 在 RD2-06 体积门通过前只能由
  opt-in 策略触发，不能成为默认 profile。
- V7-A token 语法、单遍分桶、标量 encoder/decoder 和体积报告分别由 RD2-03
  至 RD2-06 交付；在这些任务完成前，本文档不能被解读为“V7 已可编码/解码”。
