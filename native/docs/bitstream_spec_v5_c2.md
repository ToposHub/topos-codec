# TPIC V5.0 实验：C2 per-slice pair table

V5 是 C2 的实验版本。它独立于 V1/V2/V3/V4，当前不参与默认档位。

## 帧入口和 slice table

`topos_frame_config.reserved[0]=5` 生成 `version_major=5`、
`entropy_mode=3`、`codebook_version=3`、`coding_mode=0`。AQ/RDO 必须关闭。
颜色 slice 的 `k1/k2` 是冻结 DC/LEVEL VLC 书编号，当前编码器使用书 0；
`k3` 仅保留为 0。Alpha 仍使用 Rice。

每个颜色 slice 负载开头写 64 个字节：前 58 个是 pair canonical code
length，后 6 个必须为 0。58 个活动符号由 56 个常见 `(run<8,
level-category≤7)` pair、escape 和 EOB 组成。解码器用这些长度重建 canonical
book，然后读取块数据；表由当前 slice 的量化系数统计确定，未观测符号保留
权重 1，确保表完整且可独立解码。

## 块语法和错误语义

常见 pair 码字后写 `category-1` 位幅度后缀；escape 后写 6-bit run 和
现有 `LEVEL_CAT` 码字；EOB 结束块。`pos + run ≤ 63`、DC/level 域、保留
token、截断和尾随字节检查与 C1 相同。量化系数和重建保持不变。

## 首轮结果

`native/topos_codec/tools/bench_c1_bitstream.py` 的 V5 行用于完整文件 A/B。
固定 640×360 texture、QP24、Release、3 帧首轮中，C2 平均 packet 为
约 `1.45 MB`，V2 为约 `1.00 MB`；C2 的 per-slice table 仍未通过文件大小
门，且解码增加 table 重建和慢路径。C2 作为可复现的 table oracle 保留，
不替换默认 V2；若继续攻关，应先把活动 pair 字母表和 escape 重新设计，
再决定是否进入 rANS/FSE。
