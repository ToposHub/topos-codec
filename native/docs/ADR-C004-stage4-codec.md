# ADR-C004 — 阶段 4：完整标量图像编解码与 Profiles

- 日期：2026-08-29
- 关联：计划 §7 阶段 4；spec v1.1（`bitstream_spec_v1.md`）；ADR-C001/C002/C003
- 交付物：elementary encoder/decoder（C ABI）、Standard profile（QM 冻结）、
  Alpha round-trip（无损 + 受限近似）、`quality_report_scalar.md`

## 目标与完成门槛对照

| 门槛 | 结果 |
| --- | --- |
| Alpha 无损模式 bit-exact | ✅ 单测强制（`test_codec.c` mode1 逐像素断言） |
| 颜色画质达阶段 0 基线 | ✅ 合成 grain @113.2 Mb/s → PSNR avg 52.4 dB（ProRes 参考 48.2 dB @114.6–186.9 Mb/s）；真实素材复测留阶段 5 |
| 多代退化可解释且无明显色偏 | ✅ qp48 六代：PSNR 单调收敛（64.79→62.90 dB），亮度均值漂移 −0.034 LSB |
| 单帧损坏不污染下一帧 | ✅ 解码器无跨帧状态 + 单测（损坏帧后干净帧逐字节一致） |
| encoder/decoder 失败路径无泄漏 | ✅ ASan/UBSan 四配置门禁（曾抓出测试侧 use-after-free，已修） |

## C-30 编码架构：带缓冲两段式 + 纯函数 k 估计

- 每 plane：pad → coded 平面；每带（默认 32 块行）：变换+量化入带缓冲（复用分配）→
  由同一遍扫描统计符号均值 → `k = floor(log2(mean))`（clamp 0..14）→ 熵编码。
- 编码器选带自由（§11.2）兑现为 `slice_rows` 配置（1..255，验证全帧 ≤ 512 slice；
  0 = 默认 32）。
- **确定性**（§11.1）：无墙钟/线程/浮点；`k` 估计与 qp 选择均为输入纯函数——
  golden_codec 以字节级冻结该行为。
- 编码输出自检：组包后强制 `tc_packet_scan` + 全 slice CRC 校验（§11.5 的库内落实）。

## C-31 Standard QM 冻结与码率机制（调优发现）

**发现**：整数 DCT 的像素域量化粒度极细（W 归一后 Q=128 → 像素误差 ≈0.1 LSB；
F00 量化误差 ±64 经基函数摊薄后亚舍入），叠加 qp_scale 温和的 2^((qp−4)/4) 增长——
平缓矩阵（v0 占位 flat 16）下 **qp 全域近无损**，qp≤44 对多数内容码率不敏感；
全谱噪声的码率由低频 AC 主导，单靠矩阵无法命中目标带。

**决策**：
1. Standard 表 = 几何递增频率加权：`luma = 16·1.25^(u+v)`（→16..364）、
   `chroma = 16·1.18^(u+v)`（→16..162）。luma 高频比 chroma 粗（HVS 对亮度高频噪声
   最不敏感），chroma 精细（键控/肤色工作流）。表值 v1.1 冻结（spec A.3 + golden）。
2. **码率命中以帧级 qp 搜索为主机制**（与 ProRes 帧级质量控制同构）：矩阵固定频率
   响应，qp 承担内容自适应。工作区：qp50–63（grain@qp63 → 113 Mb/s @50.4 dB，
   落入 112–152 Mb/s 目标带）。
3. slice 级调节：`qp_delta_luma/chroma`（每 plane 偏移，钳位后写入 slice header；
   编码器一致性要求单测断言解码侧读回一致）。

## C-32 alpha_mode=2（受限近似）语义

N-bit 预量化（`a_q = ((a+2^(s−1))>>s)<<s`，顶钳位到 `(0xFFFF>>s)<<s`）后**原样复用**
mode 1 无损路径（MED/残差流逐字节同构）。误差界 `2^s−1` 由构造保证；实际最大误差
经 `topos_frame_stats.alpha_max_abs_error` 暴露（tpcC 记录用）。解码不变量：mode 2
的每个 Alpha 值必须是 `2^s` 倍数（违反 → MALFORMED → conceal）。v1.0 解码器对
mode 2 仍干净拒绝（UNSUPPORTED_ALPHA_MODE）——未使用 mode 2 的码流跨版本一致。
实测占比：无损 28.0% / 12-bit 27.5% / 10-bit 25.9% / 8-bit 20.6%（1080p grain）。

## C-33 concealment 执行（spec §9 策略落地）

逐 slice：CRC 坏或熵解码失败（TRUNCATED/MALFORMED）→ 该带填中性值（颜色 512 /
alpha 65535）并重写整带（覆盖部分写入），帧交付 + `TC_WARN_CONCEALED`；
`topos_frame_output.slice_status[512]` 逐 slice 状态 + `concealed_slices` 计数。
header/结构失败 → 整帧拒绝（packet_scan 路径）。

## C-34 流式解码 sink API（不可信输入的分配上界）

新增 `tc_color_slice_decode_stream`（逐块回调：反量化→逆 DCT→钳位→写平面）与
`tc_alpha_slice_decode_stream`/`tc_alpha_pairs_decode`（逐 (run,level) 对回调 +
MED 在线重建）。解码路径 O(cols)/O(width) 辅助内存，**不受码流声明的 band_h 摆布**
（对抗 8192² 全带声明时不再放大分配）；原整缓冲 API 保留为薄包装（字节语义不变）。

## C-35 tc_frame_encode_sized：确定性目标码率搜索

粗升（qp+8）→ 细升（+1）→ 预算远未用满（< 3/4）时下探回收画质（失败步重编码恢复）。
≤ 24 次编码硬上界；起点钳入 [qp_min, qp_max]（否则首帧即越界——实现期缺陷 4）。
best-effort 语义（qp_max 仍超预算时尽力返回）；`qp_used` 暴露实际 qp。

## C-36 解码内存策略

按 plane 顺序处理（slice 排列保证平面连续）：coded 平面 alloc → 该平面 slice 全解 →
crop 到调用方 visible 缓冲 → 立即 free。峰值 = 单平面 coded + 输出（8192² 最坏 ≈134 MB），
而非全帧 4 平面。

## C-37 统计 ABI（§11.3）

`topos_frame_stats`：`color_payload/alpha_payload/color_header/alpha_header/packet_size`
分计（单测断言四项之和 == packet_size）+ `slice_count/qp_base/alpha_max_abs_error`。
dry-run：`out=NULL, cap=0` 或 cap 不足时继续计数并返回**精确所需尺寸**
（`BUFFER_TOO_SMALL` + stats.packet_size）。

## 实现期缺陷（测试/门禁抓出，共 4 处）

1. **解码块行偏移缺失**：color/alpha sink 以带内相对行写入（未加 `block_y0`），
   多带帧只有首带正确——`slice_rows=1` 用例暴露。
2. **alpha sink 未写目标平面**：重建值留在行缓冲，输出全零——8×8 探针定位。
3. **encode_sized 起点未钳位**：`qp_base > qp_max` 时（上游 sized 调用残留 qp）首帧
   即越界，`qp_used` 返回区间外值。
4. **`frame_packet_size=0` 被 header 校验拒绝**：编码侧在组包前校验需占位 53。
   （另有测试侧 use-after-free 1 处，ASan 门禁抓出。）

另：v0 表（flat 16）曾使「量化未生效」误判——实为像素域粒度极细（C-31 发现），
链路本身正确（平场探针 q[0]=15886 = F/128 验证）。

## 交付物与验证

- 库：`src/codec/codec.c`（encode/decode/sized/bound/geometry + dev QM 覆盖钩子）、
  `slice_codec.c` 流式变体、`block_coding.c` 对级解码、`frame_header.c` mode2 规则、
  `quant.c` Standard 表、`include/topos_codec.h` ABI 扩展（4 struct + 6 函数）。
- 工具：`topos_quality`（report/sweep）；`topos_golden_codec`（gen/check）。
- 测试：`test_codec.c`（配置校验/往返/确定性/mode1 bit-exact/mode2 误差界与倍数
  不变量/stride 无关性/slice_rows 与 qp delta/concealment 两种触发/帧隔离/截断矩阵/
  失败路径 dry-run/encode_sized 等价性/多代稳定化 + 有损收敛）；`golden_codec_v1.bin`
  （16 条，fold `7912af9cd4510822`）；`fuzz_codec.c`（全解码 fuzz：随机 + corpus 回放 +
  libFuzzer）。
- 绑定：`topos_binding.py` 新增 `_CFrameConfig/Input/Stats/Output` 镜像 +
  encode/encode_sized/decode/plane_geometry 包装（R-18 守卫由 struct_size 校验延伸覆盖），
  pytest 10/10。
- 门禁：四配置（Debug/ASan/UBSan/Fuzz）各 26/26 + Python 10/10 +
  `ALL STAGE-4 CHECKS PASSED`。
- 报告：`docs/quality_report_scalar.md`（码率-画质、多代、目标码率、Alpha 占比、
  4K 标量速度基线 R-21、QM 调优记录）。

## 遗留与移交

- 速度：单线程标量 1080p ≈4.2 fps / 4K ≈1.1 fps（近无损最重负载）——阶段 9
  SIMD + slice 并行的优化基点（R-21 基线已建立）。
- 真实素材画质/码率复测：阶段 5 CLI 落地后按 `benchmark_protocol.md` §4.2 执行。
- 12-bit：整数界已备（ADR-C002），启用待 profile 扩展决策。
- concealment 填充值（512/65535）对 mode 2 不变量为例外（显式中性值）——spec §8.6 已注明。
