# TPIC V4.0 实验：C1 AC pair

本文件记录开发期 C1 实验语法。它不改变 V1/V2/V3 分发路径，也没有把
C1 设为任何产品默认值。

## 帧入口

`topos_frame_config.reserved[0]=4` 生成 `version_major=4`、
`entropy_mode=2`、`codebook_version=2`、`coding_mode=0`。AQ/RDO 必须关闭。
major=4 的颜色 slice 将 `k1/k2/k3` 解释为冻结 DC、LEVEL、pair 辅助书
编号（当前实验固定使用书 3）；Alpha 仍使用原有 Rice 语义。major=4、
entropy_mode=2、codebook_version=2 的组合由帧头严格校验，其他组合返回
`TC_ERR_UNSUPPORTED_VERSION`。

## 颜色块语法

DC 使用现有 `DC_CAT` canonical VLC 及幅度后缀。AC 沿 zigzag 扫描，每个
非零系数输出一个 6-bit token：

| token | 语义 |
| ---: | --- |
| `0..55` | `run = token / 7`（0..7），`level_category = token % 7 + 1`（1..7），随后写 `category-1` 位幅度后缀 |
| `62` | escape；随后写 6-bit `run`（0..62），再写现有 `LEVEL_CAT` 码字和幅度后缀 |
| `63` | EOB；块结束 |
| `56..61` | 保留，解码返回 `TC_ERR_MALFORMED` |

每块仍要求 `pos + run ≤ 63`，level 映射域和 DC 域检查与 V2 相同。所有
slice 负载 byte-align；解码必须恰好消费负载，截断、非法 token、level=0、
越界位置和尾随字节都拒绝。量化系数、重建和像素输出不改变。

## 验收状态

`test_block_coding` 覆盖 common pair、escape、极值和截断；
`tests/native/test_topos_binding.py::test_c1_acpair_roundtrip_and_major_header`
覆盖整帧 major/entropy 字段、CRC、自检和无 concealment 解码。

首轮完整文件探针见
`native/topos_codec/tools/bench_c1_bitstream.py` 和
`native/topos_codec/tools/c1_bitstream_2026-09-07.json`。在固定
640×360 texture、QP24、Release 测试中，C1 相对 V2 体积增加约 73.4%，
解码时间增加约 46%，因此该语法只作为实验 oracle 保留，默认仍为 V2 VLC。
