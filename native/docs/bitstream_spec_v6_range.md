# TPIC V6.1：方向预测 + 快速混合熵编码

状态：开发期实验格式，显式启用方式为 `reserved[0]=6` 或 CLI
`--entropy intra-range`。V6 与 V3 使用相同的 8×8 闭环空间预测、整数 DCT、
量化矩阵和切片边界；V6 只替换颜色残差的熵语法。Alpha 仍使用现有 MED +
Rice 语法。V6.1 把逐系数的范围解码收敛到块模式标志，DC/AC 符号改用切片内
选择的 canonical VLC 和 64-bit reservoir，以满足解码速度门禁。

## 1. 帧头分流

V6 保持 53 字节 TPIC 帧头和 17 字节 slice header。固定字段为：

| 字段 | V6.1 值 |
|---|---:|
| `version_major` | 6 |
| `version_minor` | 0 |
| `entropy_mode` | 4（模式自适应 + 快速 VLC 混合熵编码） |
| `codebook_version` | 4 |
| `coding_mode` | 1（像素预测残差） |

`frame_type/gop_id/ref_distance` 仍为 0；视频暂不含帧间预测。V6 颜色切片
必须携带像素预测闭环，V2 AQ/RDO 选项必须关闭。V1/V2/V3/V4/V5 分发路径
保持独立，未知组合拒绝。

## 2. 颜色切片语法

切片内块按行主序编码。V6 颜色切片 payload 先写一个 8 字节大端目录：
`range_bytes`（4 字节）和 `raw_bits`（4 字节）。目录后紧跟
`range_bytes` 个范围编码字节，再紧跟 `ceil(raw_bits/8)` 个原始后缀字节。
原始后缀的最后一个字节以零填充；`raw_bits` 不包含这些填充位。这样解码器
可以把昂贵的自适应符号和近似均匀的系数后缀分开，后缀走现有 64-bit reservoir
位读快路，而不改变系数值或预测重建。

范围编码器和解码器都在 slice 起点把 128 个二进制上下文初始化为
`p0=2048/4096`。V6.1 的范围流只使用模式上下文；每个上下文只保存“符号为
0”的概率，读写完一个符号后按 1/32 的步长更新，概率保持在 1..4095。

每个切片先根据 DC 类别、AC 幅度类别和 run 直方图选择 `k1/k2/k3` 三本
canonical VLC 书。每块先写：

1. `spatial_flag`：1 bit。0 表示 mode 0；1 后跟 3 bit `mode_minus_one`，
   取值 0..6 对应 mode 1..7，7 保留。
2. DC：将 `q[0] - dc_pred` 映射为无符号折叠整数，用 `k1` 指定的 DC VLC
   类别码和字面 suffix 写入 raw side stream。
3. AC：量化系数按 zigzag 顺序变成 `(run, level)` 对。`run` 是当前非零系数
   前的零数，使用 `k3` 指定的 run VLC；`run=63` 表示 EOB。非零时写一个
   原始 sign bit，再用 `k2` 指定的 level 类别码和字面 suffix 写入 raw side
   stream。解码器只遍历非零系数，跳过尾部已知零块。

范围流只携带 spatial flag 和非零模式的 3 bit mode id；它保留 slice 内
模式概率自适应，同时把高频 DC、run、level 和 sign 解码交给现有 VLC/LUT
快路。这样 V6.1 不再为每个系数调用范围更新，也不会改变系数值或预测重建。

范围编码器使用 32-bit 闭区间和 E1/E2/E3 重标定，切片末尾写入收尾标记；
两个子流都按字节边界结束。解码器按已知块数停止，允许范围子流最多 4 个
收尾字节由算术收尾留下，并要求 raw 子流只剩目录声明的零填充。slice CRC
仍覆盖完整 payload，任何目录、CRC 或语法错误只隐藏当前切片。

## 3. 预测与重建

预测模式、边界延拓、DC 邻居和重建顺序完全沿用
[`bitstream_spec_v3.md`](bitstream_spec_v3.md)。因此 V6 与 V3 在相同 QP 下
生成相同的预测残差和重建像素；方向预测本身没有额外画质损失。V6 不使用
source-DCT cache，`tc_frame_encode_sized` 继续使用逐 QP 完整编码搜索。

## 4. 安全边界

VLC 符号必须来自 `k1/k2/k3` 对应的有限 canonical 表，防止畸形码流造成无界
循环；raw suffix 按目录的 `raw_bits` 严格验尾。编码器按每块 2048 字节预留
切片临时 staging 缓冲，超过 256 MiB 总包上限即失败。所有
颜色 plane、10/12-bit、奇数尺寸、alpha 组合都必须通过同一 packet CRC、
解码和 sentinel stride 测试。

## 5. 解码快路

V6.1 的解码器在 `range_decode_bit_inline()` 中内联模式概率更新、区间重标定
和输入检查；DC/AC 使用现有 12-bit VLC 一级表和 raw reservoir。`(run, level)`
语法把每块的已知零位置从热循环移除，且不再为每个系数支付算术更新。这个
优化只改变熵表示，不改变 V3 的模式选择、量化系数或有效像素。解码仍按
slice 并行，目录使两个子流在每个 slice 内可独立验证。

## 6. 实验定位

V6 是用于测量“方向预测 + 自适应熵”组合收益的原型，暂不替换默认 V2。
应使用同一 QP、同一量化矩阵和同一素材分别比较 V2、V3、V6 的包大小、PSNR、
编码时间和解码时间；若要宣称同画质收益，必须再做 PSNR/SSIM 对齐的率失真
曲线。当前实现没有运动估计、参考帧、分块 RDO 或多变换核。V6 也不替换默认
V2；进入默认路径前必须以同一线程预算完成码率、有效像素逐值、异常输入和
端到端解码门禁。
