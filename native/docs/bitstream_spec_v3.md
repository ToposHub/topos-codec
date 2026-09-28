# TPIC V3.0：空间帧内预测 + canonical VLC

状态：开发期实验实现，尚未作为默认编码方式。实现与测试位于
`src/codec/intra.c`、`tests/unit/test_intra.c` 和
`tests/conformance/golden_codec_v3_intra.bin`。

## 1. 范围与版本

V3 保持固定 8×8 整数 DCT、原有量化矩阵及 qp 0..63，颜色块新增像素域
方向预测。Alpha 继续使用 V1/V2 的 MED + Rice。视频仍是全帧内编码，
没有运动估计、参考帧、帧间预测或算术编码。

帧头沿用 53 字节布局及 CRC 规则，固定字段为：

| 字节偏移 | 字段 | V3.0 值 |
|---|---|---|
| 6 | version_major | 3 |
| 7 | version_minor | 0 |
| 45 | entropy_mode | 1（canonical VLC） |
| 46 | codebook_version | 1（复用 V2 冻结码表） |
| 47 | coding_mode | 1（像素预测残差） |
| 48 | reserved | 0 |

其他格式、位深、profile、颜色标签、切片覆盖及限额规则沿用 V2；
frame_type、gop_id、ref_distance 必须为 0。未知版本或 coding/entropy
组合拒绝。MOV 的 tpcC version 写 3，同轨不得混合 major；TPIM/.toos
容器自身仍为 V1，内部 TPIC 包自描述为 V3。保留的 V1/V2 分发路径不改语义。

## 2. 颜色切片与模式语法

slice header 仍为 17 字节，k1/k2/k3 分别为 DC/LEVEL/RUN 码书索引
0..3，qp_delta_biased 和 payload CRC 规则不变。

每块按切片内行主序依次读取：

1. `spatial_flag`：1 bit。0 表示 mode 0；1 时再读 3-bit `mode_minus_one`。
2. `mode_minus_one` 0..6 对应 mode 1..7；7 保留，须报 MALFORMED。
3. 使用原 V2 VLC 语法解码残差的量化 DC 与 zigzag AC `(run, level)` + EOB。
   DC 仍是量化残差 DC 的左/上差分，预测上下文在每个 slice 清空。

mode 0 总计 1 bit，mode 1..7 总计 4 bits。切片最后不足一字节的填充
必须全为 0，不能有尾随字节。模式纳入符号 hash，随后依自然顺序纳入
该块的 64 个量化系数。语法检查无需像素重建，可用 O(块列数) 内存流式完成。

## 3. 规范预测与重建

令块内 x,y 为 0..7，M=2^(bit_depth−1)。T[i] 为已重建上一块行的底边，
从本块左边界起读取 i=0..15；超出 coded plane 右边界时重复最右样本。
L[i] 为已重建左块的右边，i=0..7。C 为左上角样本。

切片第一块行的 T 全取 M，每行第一个块的 L 全取 M；只要上或左不可用，
C=M。所有引用严格在同一切片内。L 超出 7 的扩展重复 L[7]，不读未来块。

| mode | 名称 | P(x,y) |
|---|---|---|
| 0 | midpoint | M |
| 1 | boundary DC | 可用 T[0..7]、L[0..7] 的均值，半向上取整；无邻边为 M |
| 2 | vertical | T[x] |
| 3 | horizontal | L[y] |
| 4 | planar | floor(((7−x)L[y]+(x+1)T[7]+(7−y)T[x]+(y+1)L[7]+8)/16) |
| 5 | diagonal down-right | x>y 时 T[x−y−1]；y>x 时 L[y−x−1]；否则 C |
| 6 | top-right | T[x+y+1] |
| 7 | left-down | L[min(x+y+1,7)] |

将解码系数按 V2 规则反量化（保持 ±2^25 钳位）和整数逆变换得到 R̂。
重建像素为 `clip(R̂+P, 0, 2^bit_depth−1)`。先重建完整 coded 块，更新
完整底边/右边，再只写回可见区域。不得用裁切后的可见输出替代预测边界。
不同切片可并行，单切片内块须顺序重建，不可使用旧四块独立 IDCT 提交路径。

## 4. 当前编码器策略（非解码规范）

模式预筛考虑 8 个固定模式的量化残差，用冻结码书 3 估计 DC 类别后缀、
AC 游程、level、EOB 和模式信令用位。对最便宜的至多两个空间候选计算
实际逆变换后的可见像素 SSE；只有用位更少且 SSE 不高于 midpoint 才接受。
相同用位按模式编号稳定决胜。保留 midpoint，无新增分块、多变换核、
逐系数精修或全组合 RDO。

10/12-bit 预测残差在 int16 范围内，但 12-bit 前向系数可能超过原 ±2^25
域。量化前剔除越域模式，不能通过提前钳位伪造收益；midpoint 始终合法。
输入 uint16 样本超过声明位深时，编码器返回 INVALID_ARGUMENT；此检查
在模式搜索前执行，以保证 midpoint 候选和残差值域的前提成立。
最终整切片直方图分别选择三族最短 VLC 码书，同分选择较小 book id。

编码器使用 O(slice_blocks×64) 的量化系数暂存与 O(coded_width) 的重建
边界缓存；解码仅 O(coded_width) 额外内存。当前 V3 scratch 尚未池化。

## 5. API 与码率搜索

`topos_frame_config.reserved[0]=3` 开启 V3；reserved[1]/[2] 必须为 0，
明确拒绝现有 V2 AQ/RDO 开关。ABI 结构尺寸和函数签名不变。

闭环邻像素依赖 QP，因此不得复用旧 M7 source-DCT cache。V3 sized
从 qp_min 到 qp_max 逐个完整编码，首个满足 target_bytes 的 QP 即结果，
至多 64 次，不假设 bytes(QP) 单调。无命中时交付 qp_max 包和真实大小，
调用方据此判定超预算。前期候选大于 out_cap 仍继续计数和搜索；最终选中
包大于 out_cap 才报告 BUFFER_TOO_SMALL。非容量类错误立即透传。

## 6. 验证边界

独立 V3 golden 冻结包字节和解码像素指纹；既有 V1/V2 golden 不重写。
focused tests 覆盖位深/格式/alpha/奇数尺寸、stride 哨兵、切片损坏隔离、
上下文与批量解码、scalar/SIMD 与线程确定性、非法语法、OOM、码率搜索。
fuzz encode 覆盖 V3 合法配置并执行完整像素重建；解码 replay 应喂入
合法 V3 包及其变异。吞吐与压缩收益以实测报告为准，不据模式数量推断。
