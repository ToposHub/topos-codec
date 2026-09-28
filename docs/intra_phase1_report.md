# V3 方向预测第一阶段：核查、实现与实测

日期：2026-09-06。状态：第一阶段实验路径已实现；**尚未达到替换默认编码器的收益和速度要求**。

## 核查结论

- **成立**：视频为全帧内，`frame_type/gop_id/ref_distance` 的非零值被拒绝。
  图片与视频共用 `tc_frame_encode` 内核；视频尺寸搜索调用相同编码过程。
  元数据中虽有参考帧预留字段，但没有已实现的运动估计/补偿。
- **成立**：改动前颜色通道没有像素域方向预测，仅对量化 DC 使用切片内
  左/上预测。现有熵编码为 Rice 或冻结 canonical VLC，没有自适应算术编码。
- **需修正**：“AC 裸编”不准确，AC 已经过量化、zigzag、零游程和 VLC/Rice。
  Alpha 已有 MED 像素预测，不能将颜色通道结论扩展到 Alpha。
- **需修正**：1.11 MB 是特定素材、格式、矩阵、QP 和编码策略下的测量值，
  不是所有 1080p 图像的固定下限。共享内核意味着共享限制，不能推出图片
  和视频有相同字节数的“地板”。代码也已存在平面级 chroma QP 偏移。
- **未证实**：方向预测必然节省 20–35%、high 降到 1.0–1.2 MB、视频 Proxy
  超支同步解决等量化预期。它们需要完整率失真曲线和更大素材集支撑。

## 本次实现

增加 V3.0 像素残差链，固定 8×8 DCT + 8 种预测模式，仍使用现有 VLC。
编码与解码共用预测公式，参考切片内已重建像素，包含 coded padding；
逐块重建后才裁切可见输出。模式、熵方式和主版本均有明确语法。

第一版只按 SAD 选择模式，实拍出现体积/画质双退化，已替换为量化系数
码长预筛 + 至多两个空间候选的可见像素误差检查。保留 midpoint 候选，
方向模式须同时降低估计用位、保证可见 SSE 不高于 midpoint。
模式 0 用 1 bit，其他模式用 4 bits。未增加分块、多变换核、系数精修
或全组合 RDO。规范见 [bitstream_spec_v3.md](bitstream_spec_v3.md)。

V3 的残差依赖 QP 下的重建邻居，禁用旧 source-DCT cache。尺寸搜索从
qp_min 到 qp_max 完整重编码，至多 64 次，返回最小命中 QP；无命中时
交付 qp_max 包及真实大小。V2 AQ/RDO 与 V3 的组合明确拒绝。

新增/修复的失败路径包括：

- 初始候选大于输出缓冲时继续搜索，避免较高 QP 明明能放下却提前失败。
- 10/12-bit 输入含越位深 uint16 样本时，V3 明确返回 INVALID_ARGUMENT，
  避免所有预测候选被剔除后访问未初始化系数。12-bit 合法残差超原变换
  系数域时剔除该模式，midpoint 保底。
- 非法模式扩展、非零 padding、trailing bytes、CRC 正确的损坏 payload
  走明确错误/切片隐藏；OOM 不伪装成成功。
- 原 fuzz 解码器状态白名单遗漏 TC_OK，随机无效字节从未触达此缺陷；
  加入合法 V3 golden 回放后发现并修正。编码 fuzz 修正 size=8 时的取模零，
  并改成断言失败时 abort，确保 libFuzzer 不会忽略返回值。

## 实拍结果

旧 `/tmp` 标定原始帧在恢复运行后已不存在，本次没有复用白皮书的 1.11 /
1.71 / 2.86 MB 数据。从本机视频 `Ep04 30s-给调色.mov` 抽取第 25 和 125 帧：
源为 1920×1080、25 fps、YUV 4:2:2 10-bit、BT.709，直接输出同像素格式。
**两个来自同一视频的样本不足以代表通用压缩能力。**

平台：macOS x86_64，Intel i9-9900K；CMake Release，4 个 slice worker。
每配置预热一次，编码重复 3 次取中位数。测量包含 Python 包装调用；
解码为无状态完整帧 API。原始结果及输入 SHA-256 见
[intra_phase1_results.json](intra_phase1_results.json)。

下表与 **V2 VLC，AQ/RDO 均关闭** 对比，隔离预测效果。
QP/矩阵相同，PSNR 单独列出；这不是插值得到的严格同 PSNR 率失真曲线。
大小为 TPIC packet 字节数，不含 MOV/TPIM 外壳。

| 帧 | 档位参数 | V2 字节 | V3 字节 | 体积变化 | Y PSNR 变化 |
|---|---|---:|---:|---:|---:|
| 25 | low：qm1 qp63 | 879,040 | 870,509 | −0.97% | +0.169 dB |
| 25 | medium：qm1 qp61 | 973,615 | 965,053 | −0.88% | +0.150 dB |
| 25 | high：qm1 qp58 | 1,130,901 | 1,122,798 | −0.72% | +0.140 dB |
| 25 | ultra：qm0 qp58 | 1,694,863 | 1,689,597 | −0.31% | +0.170 dB |
| 125 | low：qm1 qp63 | 499,845 | 499,287 | −0.11% | +0.039 dB |
| 125 | medium：qm1 qp61 | 605,391 | 604,230 | −0.19% | +0.036 dB |
| 125 | high：qm1 qp58 | 768,018 | 764,537 | −0.45% | +0.061 dB |
| 125 | ultra：qm0 qp58 | 1,115,154 | 1,106,189 | −0.80% | +0.085 dB |

实拍 V3 编码约 60–71 ms/帧，是本次 V2 无 AQ/RDO 基线的约 9–13 倍。
这一成本还未包含 sized 多 QP 探测。**没有达到 4K 实时编码要求。**

与当前图片 CLI 的 **V2 + AQ/RDO** 默认策略相比，V3 在这些样本上反而
更大（约 +2.1% 至 +37.6%），两者 PSNR 不同，不能将该百分比当同画质结论。
因此没有把 V3 设为默认，没有改六档 QP、目标码率或白皮书历史锚点。

一个单独的方向性诊断图（256×256，三列宽垂直条纹，所有行相同）在
ultra 下从 31,085 B 降到 4,679 B（−84.95%），两端像素均与输入完全相同。
这说明跨块方向预测通路确实生效，**不代表实拍收益**。

## 启用与复现

构建新库后可显式使用：

```sh
cmake -S native/topos_codec -B native/topos_codec/build/intra-release -DCMAKE_BUILD_TYPE=Release
cmake --build native/topos_codec/build/intra-release -j 8
ctest --test-dir native/topos_codec/build/intra-release --output-on-failure

native/topos_codec/build/intra-release/toos encode input.raw -o output.toos \
  --width 1920 --height 1080 --tier 422-high --entropy intra

native/topos_codec/build/intra-release/topos_encoder_cli \
  --width 1920 --height 1080 --qp 58 --qm 1 --entropy intra \
  --input input.raw --output output.mov
```

C API：`cfg.reserved[0]=3; cfg.reserved[1]=cfg.reserved[2]=0;`。
Python 视频导出：`entropy_mode="intra"`，`aq_mode="off"`、`rdo_mode="off"`。
此变更没有覆盖安装好的 dist/SDK 二进制；加载新构建库使用 `TOPOS_CODEC_LIB`。

基准工具 `tools/bench_intra.py` 接受独立原始帧、显式尺寸、线程数和输出 JSON，
验证精确布局、10-bit 值域、编码确定性及无 concealment；每个输入记录 SHA-256。
提供本机帧文件路径即可重测，不依赖联网下载。

## 验证与下一步界限

新增 V3 golden（8 组、16 条记录），独立模式码/保留码测试，所有像素格式
与 10/12-bit、Alpha、奇数尺寸、stride 哨兵、跨线程/SIMD、批量/context
解码、切片损坏、OOM、容量受限 sized 回归。V3 对 V2 无 AQ/RDO 的可见
逐平面 SSE 不增测试覆盖三种格式、两种位深、两种矩阵、QP 0/25/58/63。

新增 `fuzz_intra_replay` 从 V3 golden 读取真实合法包，对每 slice 做
64 个确定性 payload 位变异并重算 CRC，以及帧截断回放；编解码均在
ASan/UBSan 配置执行。最终验收：

- Release CTest：60/60 通过。
- Debug + AddressSanitizer + UndefinedBehaviorSanitizer：60/60 通过。
- Python 导出与绑定：130/130 通过。
- `toos` 编码 → deep verify → 解码，以及 MOV 编码 → verify → 解码通过，
  无隐藏切片/帧。
- 严格告警作为错误的核心库构建通过；`git diff --check` 通过。

本机完整构建、测试与冒烟日志保存在 `build/intra-bench/`（不入库）。

本阶段交付的是可验证、可显式启用的第一阶段内核路径，**不是“压缩问题
已解决”**。真实素材收益不足、编码吞吐与 sized 成本仍是阻断默认启用的
问题。下一阶段应先扩大真实素材率失真测试、优化模式筛选与 scratch 复用，
再决定是否引入自适应算术编码；本次未实现算术编码，也未实现帧间压缩。
