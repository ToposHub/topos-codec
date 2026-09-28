# ADR-C003 — 阶段 3：Bitstream 与熵编码（含 v1 冻结）

- 日期：2026-08-29
- 状态：**已接受并冻结**（golden_bitstream_v1.bin + 四配置门禁全绿）
- 规范：`bitstream_spec_v1.md`（v0→v1 变更记录见该文档头部）
- 关联：ADR-C001（C-06…C-13）、ADR-C002（C-19/C-20）

## 决策

| # | 决策 | 理由 |
| --- | --- | --- |
| C-21 | 位 I/O 采用粘滞错误模型：reader 越界置 `TC_ERR_TRUNCATED` 后不可恢复、`bit_pos` 永不推进；writer 增长上限 256 MiB（`TC_BITWRITER_MAX_BYTES`），超限/分配失败粘滞 | 「任意截断不越界」成为结构性质而非调用方纪律；writer 绝不静默丢位。Rice 域校验经 `tc_bitreader_fail` 注入 MALFORMED，同一粘滞通道 |
| C-22 | **v0 笔误修正**：AC 解码不变量 `pos + run ≤ 63`（EOB 接受 pos ≤ 64）。v0 的 `≤ 62` 会把「仅 c[63] 非零」的合法块（run=62, idx=63）误判 MALFORMED | 构造用例 `q[63]≠0` 直接命中；已在 unit/golden/corpus 三处回归 |
| C-23 | **v0 语义修正**：Alpha 残差流增加尾零终结对 `(run, level=0)`，要求 `pos + run == count`；数据对 `level ≠ 0` 且 `run ≤ 剩余−1` | v0「无 EOB、按计数终止」在尾零段解码器必然多读一对符号 → TRUNCATED。数据对 level 恒非零，终结语义无歧义 |
| C-24 | DC 预测 `pred = round_half_up((left+top)/2)`，实现为符号拆分（s≥0 `(s+1)>>1`；s<0 `−((−s)>>1)`），负数不右移 | v0 字面 `(left+top+1)>>1` 在和为负时违反 §2.4「无非负外右移」；符号拆分与数学 floor((s+1)/2) 全域等价（编解码共用同一函数，字节精确性不受影响） |
| C-25 | 符号域上限取 2 的幂整：DC m ≤ 2^27、AC level m ≤ 2^26、Alpha level m ≤ 2^17、run ≤ 63（Alpha run 按剩余像素动态） | 映射 m=2\|v\|：上界 2^k−1 会误拒 v=+2^(k−1)。实现期曾把十六进制写错两档（0x40000000=2^30 而非 2^26=0x04000000），被域界对抗测试当场抓获——先写域界测试的价值的直接证明 |
| C-26 | 计划中「restart」语义由 **slice 独立性**承载（每 slice 头重置全部熵/预测状态），不设独立 restart marker 语法 | v1.0 slice 为全宽水平带，无带间状态；独立 marker 是冗余语法。水平切分留 minor 版本（§13.1） |
| C-27 | 帧头校验顺序冻结：`size ≥ 53 → magic → header_crc32 → 字段规则（表序）` | CRC 先于语义解释：字段错误码不会被随机篡改伪装；顺序确定保证跨实现错误码一致 |
| C-28 | `color_matrix=0`（identity/GBR 保留）在 v1.0 YUV 4:2:2 流中出现即 MALFORMED | identity 矩阵对 YUV 无意义且必然是编码器 bug；「解释错误比拒绝更糟」（§13.1） |
| C-29 | 单一确定性合成器 `tests/support/packet_synth.c` 同时驱动单测、fuzz corpus 与 golden（同一配置表 6 条） | 三道门的期望字节同源重建，编码器任何无声漂移必然同时被 golden（字节级）与 corpus 自检（种子必须全通过）拦截 |

## 交付物

**库（native/topos_codec/src/）**

- `common/crc32.{h,c}` — IEEE 表实现（256 项表逐项与位级参考交叉验证；修复了一处
  转录笔误 index 63）
- `bitstream/bitio.{h,c}` — checked 位读写（MSB-first、粘滞、256 MiB 上限、可增长）
- `entropy/rice.{h,c}` — 有界 Rice（unary ≤31、escape=31×'1'+32 位字面值、域校验、
  有符号映射 int64 中转杜绝 INT32_MIN 取负）
- `entropy/scan.h` — zigzag 表（对角遍历算法交叉验证 + 置换性检查）
- `entropy/block_coding.{h,c}` — 颜色块 DC/AC 符号层 + Alpha 残差流（含 hash 折叠）
- `bitstream/frame_header.{h,c}` — 53B 帧头编解码（全部 §4.2 规则 + 派生几何 + 色彩命名）
- `bitstream/slice_map.{h,c}` — 17B slice 头 + 全帧排列/覆盖验证（Y→U→V→[A] 恰好平铺）
- `bitstream/packet.{h,c}` — 整包扫描（checked 边界、精确耗尽、逐片 CRC 标记；**零堆分配**）
- `bitstream/slice_codec.{h,c}` — slice 符号层：颜色带块编解码（DC 上下文闭合于带内）+
  Alpha 带解码（`residuals_out=NULL` 时 O(1) 内存）
- `cli/inspect.c` — `topos_inspect`（结构化 dump + 逐片 CRC/符号状态 + 退出码）

**测试与门禁**

- 单元测试 7 个新文件：crc32 / bitio / rice / block_coding / frame_header / slice_codec /
  packet（含逐字节截断矩阵、逐字节翻转矩阵、伪造字段矩阵、排列破坏、CRC 行为）
- `tests/support/packet_synth.{h,c}` — 确定性合成器（6 配置：最小 8×8 / 16×16 /
  奇数尺寸 36×20 / alpha 多带 / 128×72 / 非整除分割；xorshift64* 驱动，双重构建 bit-exact）
- `tests/fuzz/fuzz_bitstream.c` + `gen_bitstream_corpus.c` — fuzz 入口（不变量：无崩溃/
  无挂起/返回码 ∈ §9 集合/无堆放大）+ corpus 生成与自检门（174 个提交样本：6 合法种子 ×
  {截断 1/16 边界、LCG 位翻转、伪造 frame/slice 长度、全 1 超长码字} + 纯随机短输入）
- `tests/conformance/golden_bitstream.c` + `golden_bitstream_v1.bin` — 56 条记录
  （8 帧头 / 30 Rice 流（k=0..14×2，含 escape 密集）/ 12 块 payload / 6 完整包），
  fold `1fd98ecbe7ae0e99`，check 侧同源重建逐字节比对

## 验证报告

| 门禁 | 结果 |
| --- | --- |
| run_tests.sh 四配置（Debug/ASan/UBSan/Fuzz） | 23/23 × 4 全绿 |
| conformance_bitstream（56 条字节精确 + fold） | 通过 |
| fuzz_bitstream_corpus 自检（种子全通过 + 变异不变量） | 通过（ASan/UBSan 下运行） |
| fuzz_bitstream_replay（LCG 随机 4096 输入） | 通过（四配置） |
| inspect CLI 冒烟（合法 → 0 / 截断 → 1） | 通过 |
| Python 绑定冒烟 | 6/6 |
| synth 确定性（同配置重建 memcmp 相等） | 通过 |

实现期被测试抓获的缺陷（全部已修）：

1. CRC 表 index 63 转录笔误（0xB6663D3D → 0xB6662D3D）——随机交叉验证抓获；
2. Rice 映射 `-2u * s` 无符号回绕 UB——评审发现，改 int64 中转；
3. 符号域上限十六进制写错两档（见 C-25）——域界对抗测试抓获；
4. 测试自身：zigzag 参考生成算法错误（单步反弹 vs 对角遍历）、`tc_bitwriter**`
   误传（`&bw` 在形参为指针的 helper 内）、粘滞语义误解——独立探针二分定位。

## 未决 / 移交

- A.3 `qmatrix_id=1` 调优数值：阶段 4 随 profile 实现冻结（当前 = flat 16 占位）
- QM/qp 选择策略、帧级码率控制：阶段 4
- concealment 策略执行（per-slice 中灰/不透明填充）：阶段 4（本阶段的 `slice_crc_ok`
  与符号解码错误码已提供其全部输入）
- SIMD（阶段 9）：以 golden_bitstream_v1 为一致性基准
