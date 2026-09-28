# ADR-C027 — V2 schema reconciliation：major=2 字段注册表唯一真相源

- 日期：2026-08-31
- 状态：已接受（M9 前置交付，计划 §12.0 强制项）
- 关联：`Topos_编解码性能超越_ProRes_DNxHR_优化计划_2026-08-31.md` §12（P7/M9）；
  `Topos_V2.1_Micro-GOP_可落地升级计划_2026-08-30.md` §5（bitstream v2 草案边界）；
  ADR-C003（bitstream v1 冻结）、ADR-C026（M8 判定启动 M9）、ADR-C011（编号占用）；
  `bitstream_spec_v1.md` §4.2/§13.1；`capability_manifest.json`（O5 单一真相源）

## 0. 背景与问题

两份计划同时主张 major=2 的字段，互不知情：

- **Micro-GOP 计划（2026-08-30，先写）**：major=2 用于 I/P 帧间编码，激活 V1 已预留的
  `frame_type`（偏移 15）/`gop_id`（16–17）/`ref_distance`（18），新增 `coding_mode`
  （IP2/IP4），字段不足时走可跳过扩展 header；切片新增 `inter_color` kind。
- **性能计划 §12.1（2026-08-31，后写）**：把候选字段放在 `reserved0`（45–48）——
  byte 45 `entropy_mode`、byte 46 `codebook_version`、47–48 必须 0；并主张 mode=1 时
  slice `k1/k2/k3` 重解释为 `dc_book/level_book/run_book`，另立 22-byte tile slice header。

二者无直接字节冲突，但**都没有建立注册表**：Micro-GOP 的 coding_mode 尚无字节归属
（只说「不足再加扩展 header」），性能计划的 47–48 必须为 0 与之潜在相撞；且 Micro-GOP
计划引用的决策 ADR 编号 `ADR-C011` 已被 r0-scope-freeze 占用。若不先裁决，两个项目
各自实现后必然产生两个「V2」。

本 ADR 建立**唯一的 V2 frame-header/slice-header 字段注册表**，作为两个项目共同的
布局真相源；各项目自己的 ADR 只冻结**语义枚举**，不得再改**字节布局**。

## 1. 决策

### D-1 V2 frame header：保留 53 字节基础布局，reserved0 四拆

major=2 保留 V1 §4.2 的全部 53 字节偏移（`header_size` 仍 53），仅激活/拆分以下字段。
`reserved0`（u32 @45）在 V2 拆为四个 u8：

| 偏移 | V2.0 字段 | 归属 | V2.0 约束 |
| ---: | --- | --- | --- |
| 45 | `entropy_mode` | **VLC/M9** | 0=Rice（V1 语义）/ 1=canonical VLC；其他 → `UNSUPPORTED_VERSION` |
| 46 | `codebook_version` | **VLC/M9** | entropy_mode=0 → 0；=1 → 1；其他 → `UNSUPPORTED_VERSION` |
| 47 | `coding_mode` | **Micro-GOP/V2.1** | V2.0 只允许 0（All-Intra）；1/2（IP2/IP4）由 V2.1 的 ADR 冻结枚举后启用 |
| 48 | `reserved_v2_0` | 注册表 | 必须 0，非 0 → `MALFORMED` |

性能计划 §12.1 的候选「47–48 必须 0」**修订为「47=coding_mode（归属 Micro-GOP）、48=0」**。
该计划自身已声明候选布局以本 ADR 为准，故无违例。

### D-2 V1 已预留帧字段在 V2 的归属

| 偏移 | 字段 | 归属 | V2.0 约束 |
| ---: | --- | --- | --- |
| 15 | `frame_type` | Micro-GOP | 0=I；1=P → V2.0 `UNSUPPORTED_VERSION`（V2.1 启用） |
| 16–17 | `gop_id` | Micro-GOP | V2.0 必须 0 |
| 18 | `ref_distance` | Micro-GOP | V2.0 必须 0 |

V1 解码路径对这些字段的「必须 0」校验**永久保留**（major=1 恒走 V1 规则）。

### D-3 flags 位分配（u16 @8）

V1 仅 bit0=`alpha_premultiplied`，1–15 必须 0。V2 激活：

| bit | 字段 | 归属 | 语义 |
| ---: | --- | --- | --- |
| 0 | `alpha_premultiplied` | V1 冻结 | 同 V1 |
| 1 | `tile_layout` | **VLC/M9 §12.4** | 0=全宽水平带（17B slice header）；1=tile 网格（22B slice header）。V2.0 只实现 0 |
| 2–15 | reserved | 注册表 | 必须 0；启用任一位须先在本注册表登记 + minor 递增 |

### D-4 slice header 的熵模式重解释

- `entropy_mode=0`：V1 17 字节语义原样（k1/k2/k3 为 Rice k，0..14）。
- `entropy_mode=1`（颜色 slice）：k1/k2/k3 位置重解释为
  `dc_book/level_book/run_book`，各 0..3；任一 >3 → `MALFORMED`。
  （0..3 与 Rice k 的 0..14 有交集，但熵模式本身由 frame header 判别，无歧义。）
- **Alpha slice 在 V2.0 恒为 Rice**（k1/k2 语义同 V1），`entropy_mode=1` 不改变
  alpha 路径；Alpha-VLC 属未来 codebook_version 递增，须重登注册表。
- `tile_layout=1`（未来）：22-byte tile slice header 按性能计划 §12.4 候选布局
  （payload_size/plane/x0/block_w/y0/block_h/qp_delta_biased/param1..3/flags/crc32，
  BE，CRC 只覆盖 payload）；颜色 tile `param1/2/3 = dc/level/run book`，alpha tile
  `param1/2 = k_level/k_run`、`param3=0`。字段冻结规则以性能计划 §12.4 为准。
- `inter_color` slice kind（Micro-GOP）：kind 值由 V2.1 ADR 从本注册表领取
  （候选：plane 字段高半字节或扩展值，布局未定前 V2.1 不得实现）；其与
  entropy_mode 的组合矩阵由 V2.1 ADR 冻结，VLC 的 token 语法（§12.1）对
  intra 残差块与 inter 残差块同构（同为 8×8 量化系数），技术上可组合。

### D-5 跨字段校验矩阵（V2.0，解码侧逐字段序执行）

1. magic → header_crc32 → 逐字段（V1 §4.2 顺序，major 改为 ==2 分支）；
2. `entropy_mode=1` 且 `codebook_version` 未实现 → `UNSUPPORTED_VERSION`；
3. `entropy_mode=1` 且颜色 slice book 字段 >3 → `MALFORMED`（slice 层）；
4. `frame_type=1` / `gop_id≠0` / `ref_distance≠0` / `coding_mode≠0` /
   `tile_layout=1` → V2.0 一律 `UNSUPPORTED_VERSION`（占位字段干净拒绝，
   为 V2.1/V2.1-tile 留位不留语义）；
5. `entropy_mode=0` 且 `frame_type=0` 的 V2 流：**重建必须与相同量化系数的
   V1 bit-exact**（性能计划 §12.6 验收项；V2-Rice 是 parity 锚，不是新格式）。

### D-6 minor / feature-bit 扩展规则冻结

- V2 minor 从 0 起（V2.0）。新增 entropy_mode 取值、新增 codebook_version、
  启用 coding_mode/tile_layout，均须：登记本注册表 → `bitstream_spec_v2.md`
  增补 → minor 递增（破坏读取语义时禁止）→ 新 golden。
- **codebook_version 表不可变**：冻结表一经发布永不改码字；扩域/调表 = 新
  version 值，旧值解码行为逐位不变。
- 未声明字节/位保持必须 0；禁止把信息塞入未声明字节（沿 Micro-GOP 计划红线
  与 V1 §13.1）；字段不足时走可跳过扩展 header + `header_size` 递增，并在
  本注册表登记扩展区起点。
- 旧解码器见 major=2 必须 `TC_ERR_UNSUPPORTED_VERSION` 且不产出帧（永久）；
  V1 golden 永久保留，V1 解码路径永不删除。

### D-7 容器与能力真相源

- **tpcC 原子 u16 version ↔ bitstream major 绑定**：major=1 轨道写原子
  version=1（现状不变）；major=2 轨道写原子 version=2（V2.0 原子 schema 不变，
  仅版本号作为轨级声明）。reader：原子 version 2 而库仅支持 V1 →
  `UNSUPPORTED_VERSION` 干净拒绝；轨内每帧 packet major 必须与原子版本一致
  （不一致 → `MALFORMED`）。解码权威仍在每帧 header（包自包含原则不变）。
- **capability_manifest.json** 增设 `bitstream` 块（schema_version 1→2，
  追加不破坏现有键）：`major_supported: [1,2]`、`entropy_modes: {0: "rice",
  1: "canonical-vlc"}`、`codebook_versions: {1: …}`、`coding_modes: {0:
  "all-intra"}`。M9 代码落地批次同步更新 manifest 与
  `tests/media/test_capability_manifest.py` 机器校验。
- SDK query / CLI：`tc_version()` 能力串、`topos_transcode --bitstream-major`
  归 M9 工具批次；不覆盖源文件、不静默重写旧文件（计划 §12.5）。

### D-8 decoder dispatch 单点

major 分流只存在于 `frame_header.c`（validate/encode/decode 同一文件同一顺序），
按 major 选择 V1 规则组或 V2 规则组；VLC/Micro-GOP 不得各自在 packet/slice 层
重复判 major。golden 命名：V1 golden 原名不动；V2 golden 以
`v2_<entmode>_<frameshape>` 前缀新增（如 `v2_vlc_i`），Micro-GOP 后续按
`v2_*_p` 扩展。

### D-9 Micro-GOP 计划勘误（登记于案）

Micro-GOP 计划（2026-08-30）引用的 `ADR-C011-micro-gop-decisions.md` 编号失效
（C011 = r0-scope-freeze）。其决策 ADR 在项目启动时从 C028+ 顺延分配；
其 header 字段的**字节归属以本 ADR 注册表为准**（frame_type/gop_id/ref_distance/
coding_mode 已预分配），其 ADR 只冻结枚举语义、搜索算法与错误恢复，不再动布局。

## 2. 注册表（权威表，后续变更须修订本 ADR）

| 字节/位 | 字段 | V1 | V2.0 | 归属项目 |
| --- | --- | --- | --- | --- |
| 6 | version_major | ==1 | ==2 | 注册表 |
| 7 | version_minor | 0..3 枚举 | ==0 | 注册表 |
| 8 bit0 | alpha_premultiplied | 冻结 | 同 V1 | V1 |
| 8 bit1 | tile_layout | 必须 0 | 0（V2.0） | VLC/M9 |
| 8 bit2–15 | reserved | 0 | 0 | 注册表 |
| 15 | frame_type | ==0 | ==0（P 待 V2.1） | Micro-GOP |
| 16–17 | gop_id | ==0 | ==0（待 V2.1） | Micro-GOP |
| 18 | ref_distance | ==0 | ==0（待 V2.1） | Micro-GOP |
| 45 | entropy_mode | reserved0=0 | 0/1 | VLC/M9 |
| 46 | codebook_version | 同上 | 0/1 | VLC/M9 |
| 47 | coding_mode | 同上 | ==0（待 V2.1） | Micro-GOP |
| 48 | reserved_v2_0 | 同上 | ==0 | 注册表 |
| slice 10–12 | k1/k2/k3 | Rice k 0..14 | mode=1 颜色→book 0..3；alpha 恒 Rice | VLC/M9 |

## 3. 验证

- `frame_header.c` 头文件内 `static assert`：V2 字段偏移常量与上表一致；
  表驱动单测覆盖 D-5 全部拒绝路径（含 V1 golden 不受影响回归）。
- `bitstream_spec_v2.md`（本批次交付）为 V2.0 正式语法文档；两份计划文档与
  本 ADR 冲突处以本 ADR + spec v2 为准。
- Micro-GOP 启动条件：先修订其计划文档中的 ADR 编号引用，再按 §D-9 分配。
