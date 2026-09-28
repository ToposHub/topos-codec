# libtopos_codec — Native 核心（V1 Preview：Intra 10/12-bit 4:2:2/4:4:4/GBR 帧内（V1 Preview））

Topos Video Codec 的 C11 核心工程。

**写域（V 代际收纳 2026-09-13，ADR-C046）**：`reserved[0]`（`--entropy` /
`entropy_mode`）合法域收缩为 **{0=V1, 1=V2-VLC, 8=V7-R2（产品默认）,
9=V8}**；实验代际 V2-Rice(em2)/V3(em3)/V4(em4)/V5(em5)/V6(em6)/V7-R(em7)
**写端已退役**——显式拒绝（INVALID_ARGUMENT，信息含 ADR-C046 指引），
不静默回落；读端同样退役（`UNSUPPORTED_VERSION`），归档流仅
TOPOS_DEV_REPLAY 构建 + `TOPOS_DEV=1` 可回放。V3/V4/V5/V6 语法规范
（docs/bitstream_spec_v3~v6.md）与实验报告保留作档案。

下一阶段：[不降低画质的压缩与编解码提速计划](docs/Topos_无画质损失压缩与解码提速计划_2026-09-06.md)
（包含周排期、逐像素一致门、解码热点核查及达芬奇对比方法）。
当前第一批实施记录见[解码提速第一阶段报告](docs/decode_speed_phase1_report_2026-09-06.md)。
Windows 本机 4K 4444→2K 422 原生缩放的后续执行表见
[后续优化计划](docs/Topos_4K4444到2K422后续优化计划_2026-09-20.md)。

> **白皮书（格式/档位/码率/性能综述）**：[Topos_Codec_白皮书.md](Topos_Codec_白皮书.md)（中文） ·
> [Topos_Codec_Whitepaper.md](Topos_Codec_Whitepaper.md)（English）。
> 规范/ADR/基准报告见 [docs/](docs/)。

## 档位速览（V1 Preview 全七帧内档 + RAW 档可用）

422 Proxy / LT / 422 / 422 HQ 共享同一 4:2:2 码流 profile（profile=3），区别只在
目标码率与 Alpha 策略；4444（profile=5）与 4444 XQ（profile=6）是独立码流
profile；422 LP（v1.8 帧间档，ADR-C056）为帧间微 GOP（zero-motion IP-2，
载体 V7-R3），固定锚 qp72、no-alpha、无码控。帧内档全部可选携带 Alpha
（a8/a10/a12/a16），LP 显式 no-alpha。Topos RAW（M4，ADR-C058）= 视频 RAW
产品线：bitstream profile 7（TRAW，CFA 相位平面）+ 容器 tier 8，位深 ×
比率 12 档（`raw12-2…16` / `raw16-2…16`，锚点与图片 RAW 共表），全 I 帧、
no-alpha、比率档为 CQ 语义。

| 档位 | 像素格式 | 位深 | ≈1080p25 | ≈2160p25 (4K) | 主要用途 |
| --- | --- | --- | ---: | ---: | --- |
| Topos 422 Proxy | YUV 4:2:2 | 10 | 15 Mbps | 60 Mbps | 离线代理、远程剪辑 |
| Topos 422 LT | YUV 4:2:2 | 10 | 49 Mbps | 195 Mbps | 轻量中间缓存、粗剪 |
| Topos 422（默认档） | YUV 4:2:2 | 10 | 78 Mbps | 312 Mbps | 常规剪辑、渲染缓存 |
| Topos 422 HQ | YUV 4:2:2 / 4:4:4 / GBR | 10/12/16 | 132 Mbps | 529 Mbps | 调色、中间母版 |
| Topos 4444 | YUV/GBR 4:4:4 | 10/12/16 | 221 Mbps | 884 Mbps | VFX、动态图形、合成 |
| Topos 4444 XQ | YUV/GBR 4:4:4 | 12 | 326 Mbps | 1304 Mbps | 高宽容度 HDR、多代制作 |
| Topos 422 LP（帧间档） | YUV 4:2:2 | 10 | 42–64 Mbps（锚定） | 168–256 Mbps | 实时粗剪缓存、快速转码 |
| Topos RAW（视频 RAW） | CFA 相位平面 | 12/16 | 比率档 12 选（CQ） | 同左（比率语义） | 相机 RAW 中间片（debayer 在应用层） |

码率为 1080p25 / 2160p25 的档位目标值，随分辨率与帧率线性缩放
（`码率 ≈ bpp × 宽 × 高 × fps`）；任意档位也可用固定 QP（0–95，v1.5 域）
替代目标码率。
参考数据率区间、Alpha 预算与码流 profile 字节等完整规格见白皮书 §4；档位
声明单一来源：`src/shared/codec/topos_profiles.py`（主仓库）/
`python/topos_codec/topos_profiles.py`（发布包）。

## 图片格式（.toos）速览

`.toos`（Topos Image）是构建在本内核之上的静态图片中间格式：TPIM 信封 +
一个完整 TPIC 帧内包，像素编解码 **100% 复用同一套 libtopos_codec 核心**
（规范冻结"禁止第二实现"）。GBR 4:4:4 直通、alpha 显式三态（straight A16
无损 / premultiplied 有界）、原子写、渲染序列一等支持；解码比 PNG 快 2.3×。

> **图片白皮书**：[Topos_Image_白皮书.md](Topos_Image_白皮书.md)（中文） ·
> [Topos_Image_Whitepaper.md](Topos_Image_Whitepaper.md)（English）

| 档位（`toos encode --tier`） | 格式 | ≈MB/帧 (1080p) | 用途 |
| --- | --- | ---: | --- |
| Topos 422 Low | YUV 4:2:2 10-bit | 1.08 | 预览 / 审片 / 代理 |
| Topos 422 Medium | YUV 4:2:2 10-bit | 1.29 | 体积低于同素材 PNG 8-bit |
| Topos 422 High | YUV 4:2:2 10-bit | 1.71 | PNG 锚点档 |
| Topos 422 Ultra（默认） | YUV 4:2:2 10-bit | 2.86 | 视觉无损级 |
| Topos 444 High | GBR 4:4:4 10-bit | 1.30 | 全色度合成收缩档 |
| Topos 444 Ultra | GBR 4:4:4 10-bit | 3.40 | 全色度视觉无损级 |
| toos Low/Medium/High/Ultra 16bit float | GBR float16 | 0.38–5.75 | 浮点中间片（HDR 线性合成；qp 82/72/58/44，视觉透明至审片代理） |
| Topos RAW 12-bit 2:1 … 16:1 | CFA 12-bit Log | 名义 2.07–0.26 | 相机 RAW 制作档（qp 45/59/64/66/68/70，默认 4:1） |
| Topos RAW 16-bit 2:1 … 16:1 | CFA 16-bit Linear | 名义 2.07–0.26 | 相机 RAW 归档档（qp 51/72/76/78/80/82，默认 4:1） |

数学无损走 `--qp 28`（10-bit）、`--half --qp 20/32`（浮点域，spec §15）或
PNG/EXR 无损容器；RAW 比率档名为 CQ 定位标签（体积随内容浮动）。全 22 档
规格、命名规则（N-1~N-8）、色彩模式矩阵、alpha 语义与序列语法见图片白皮书
（档位取名单一来源：主仓库 `src/shared/codec/topos_profiles.py`
`TOPOS_IMAGE_TIERS` 注册表）。

> **状态（2026-08-30 审计纠偏 + R4.1–R4.4 + v1.8 LP/浮点/RAW）**：当前交付为
> **V1 Preview**——稳定的受限核心（10/**12**/**16**-bit YUV 4:2:2/**4:4:4/GBR**
> 帧内 + 帧间 LP 档 + TPIC MOV + TRAW/HALF 图片档 + C ABI/SDK + 健壮性门禁），
> 不是完整 V2.0（跨平台发布未闭合，见
> `docs/Topos_V2.0_完成度审计与整改计划_2026-08-30.md`，范围冻结
> ADR-C011）。Proxy/LT/Standard/HQ 为应用层质量预设
>（`src/shared/codec/topos_profiles.py`），非独立 bitstream profile；
> 产品外部显示名与业界常见中间片档位命名对齐（Topos 422 Proxy/LT/422/HQ、
> Topos 4444、Topos 4444 XQ、Topos 422 LP），便于跨工作流映射；内部 tier_id
> 与 profile 字节不受影响。

规范见 `docs/bitstream_spec_v1.md`（v1.4：v1.1 冻结 + 12-bit 4:2:2 +
YUV 4:4:4/GBR 枚举扩展 + profile 5/6 激活，R4.1–R4.4）
与 `docs/container_spec_v1.md`（MOV 容器 v1.2：v1 冻结 + R3 追加可选
tpcB 预算元数据 atom + R5 冻结支持矩阵与非目标），决策见 `docs/ADR-C001…C014`（C-011…C-017
为审计整改 R0–R3），画质/速度报告见
`docs/quality_report_scalar.md`。应用层接入：解码（阶段 6）
`src/shared/media/topos_source.py`（ToposMediaSource + factory 内容路由）；
Alpha 渲染链（阶段 7）`src/shared/video/yuv_gpu_upload.py`（第四平面 GPU
上传/shader 采样，报告见 `docs/alpha_pipeline_report.md`）；编码与
Deliver（阶段 8）`src/shared/export/topos_encoder.py`（ToposVideoEncoder）+
`src/shared/codec/topos_profiles.py`（七档 capability schema + 图片 22 档注册表）+ 8 处 UI/任务
登记 + TPIC 磁盘代理自研链（`src/shared/media/proxy_generator.py`）。SIMD/熵
优化/slice 并行（阶段 9）`src/simd/`（AVX2/NEON 内核 + 运行时分发）与
`src/common/tpool.c`（R6 起为**常驻有界线程池**，静态条带语义不变），性能
报告见 `docs/perf_report_stage9.md`（1080p 编码 4.0× / 解码 6.8×，
bit-exact 门禁全绿）。审计整改 R6（性能门槛）：熵符号层 zigzag 序融合
（`tc_quant_block_zigzag`/`tc_block_encode_zigzag`）、encode_sized 插值
快速搜索 + legacy 回退（差分逐字节一致）、槽位缓冲常驻、常驻线程池与
调度回压（绑定 `set_slice_threads`；导出满配/后台 1 线程），门槛 v2 见
`docs/benchmark_protocol.md` §6 与 `docs/ADR-C022-r6-perf-thresholds.md`。
集成测试 `tests/media/test_topos_{source,alpha_pipeline,export}.py`
（随本门禁 Python 段运行）。

## 布局

```text
include/            公共稳定 C ABI（topos_codec.h：基础查询 + 阶段 4 帧编解码）
src/common/         私有基础设施：checked / bufview / endian / error / cpudetect /
                    crc32（slicing-by-8）/ tpool（阶段 9 有界条带并行原语）
src/transform/      阶段 2：精确正交整数 DCT / 量化（Standard QM v1.1 冻结）/
                    fastdiv（精确魔数除法）/ plane padding-crop（按行 memcpy）
src/bitstream/      阶段 3：bitio / frame_header / slice_map / packet / slice_codec
                    （阶段 4 增流式解码变体：逐块/逐对回调，O(1) 辅助内存）
src/entropy/        阶段 3：有界 Rice / zigzag / 块符号层（+ Alpha 对级流式解码）
src/codec/          阶段 4：完整标量 codec —— encode/decode/sized/bound 全链路 +
                    concealment + 统计（公共 ABI 的实现体；阶段 5 增 stride 契约校验）
src/mov/            阶段 5：MOV 容器 mux/demux/FastStart（topos_io 回调式字节源，
                    零 I/O 依赖；tpcC 轨级配置；co64/截断收敛；O(1) sample 索引。
                    R5：支持矩阵冻结 v1.2——未知 atom 四级跳过、多 trak 拒绝、
                    tpcC 唯一权威；容器音频轨已随 v1.1/ADR-C051 解冻）
src/simd/           阶段 9：AVX2/NEON 变换内核（值域证明 bit-exact）+ 运行时分发
                    （三态原子发布；tc_dev_set_simd_mode 供差分测试强制后端）
src/cli/            topos_inspect（packet 诊断）/ topos_quality（画质-码率-速度测量、
                    QM 调优 sweep、`mov` 容器指标、`perf` 阶段 9 分核/帧级基准）/
                    topos_encoder_cli / topos_decoder_cli / topos_probe_cli /
                    topos_rawgen（阶段 5 交付 CLI）
tests/unit/         单测（mini_test 零依赖框架，每文件独立可执行）
tests/support/      packet_synth（确定性合法包合成器）+ image_synth（确定性测试图像：
                    平场/梯度/颗粒/细节/混合五类内容）
tests/fuzz/         fuzz：primitives + bitstream + codec + mov（容器层）入口、
                    确定性回放 driver、corpus 生成与自检门
tests/conformance/  golden vectors：golden_transform_v1.bin（1380 条）+
                    golden_bitstream_v1.bin（56 条，fold 1fd98ecbe7ae0e99）+
                    golden_codec_v1.bin（阶段 4 全链路 16 条，fold 7912af9cd4510822）+
                    golden_mov_v1.bin（阶段 5 文件+索引双记录 10 条，fold f59e6aaf8202099f）
tests/abi/          公共符号表钉死（public_symbols_v1.txt，门禁 nm 断言）
examples/           SDK 独立示例：encode_decode.c（文件 IO 全闭环）+ topos_roundtrip.py
scripts/            build_sdk.sh（universal 打包 + ad-hoc/正式签名 + 公证管线）
spikes/             R-20 调研代码（libavformat TPIC spike，一次性，不入门禁）
dist/               SDK 发布物（gitignored，scripts/build_sdk.sh 产出）
build/              构建产物（gitignored）
```

## 一键构建 + 测试（Debug / ASan / UBSan / TSan / Fuzz + 绑定冒烟）

```bash
bash native/topos_codec/run_tests.sh            # 全部（五配置 + 符号表钉死 + CLI 链路 + ffprobe oracle + SDK 示例 + Python 绑定/阶段 6-10 接入）
bash native/topos_codec/run_tests.sh --skip-python
```

手动单个配置：

```bash
cmake -S native/topos_codec -B native/topos_codec/build/debug -DCMAKE_C_COMPILER=clang
cmake --build native/topos_codec/build/debug -j
(cd native/topos_codec/build/debug && ctest --output-on-failure)
```

速度/画质测量用 Release 构建（Debug -O0 数字无意义）：

```bash
cmake -S native/topos_codec -B native/topos_codec/build/release -DCMAKE_BUILD_TYPE=Release
cmake --build native/topos_codec/build/release -j
./build/release/topos_quality report
```

## 编码/解码（公共 ABI 摘要）

```c
topos_frame_config cfg = {0};          /* 全 0 初始化 = 合法最小配置 */
cfg.struct_size = sizeof(cfg);
cfg.visible_width = 1920; cfg.visible_height = 1080;
cfg.qp_base = 48; cfg.qmatrix_id = 1;  /* Standard */
/* alpha：mode 1 无损 / mode 2 近似（alpha_bit_depth ∈ {8,10,12}，误差界 2^s−1） */

size_t cap = tc_frame_packet_bound(&cfg);
tc_frame_encode(&cfg, &input, buf, cap, &stats);        /* stats 分计颜色/Alpha/总尺寸 */
/* 或目标码率：tc_frame_encode_sized(..., target_bytes, qp_min, qp_max, &qp_used, ...) */

tc_frame_decode(pkt, size, NULL, NULL, &info);          /* 查询几何/元数据 */
tc_frame_decode(pkt, size, planes_out, strides, &info); /* 全解码 + concealment */
/* 坏 slice：该带填中性值（颜色 512 / alpha 65535）→ TC_WARN_CONCEALED，
   info.slice_status[] 逐 slice 状态；header/结构坏 → 负值错误码、不产出帧 */
```

## 工具

```bash
# 诊断 elementary frame packet（合法 → 0；解析失败 → 1）
build/debug/topos_inspect tests/fuzz/seed_corpus_bitstream/seed_cfg3.bin

# 画质/码率/速度报告（quality_report_scalar.md 的数据源）与 QM 调优 sweep
build/release/topos_quality report
build/release/topos_quality sweep

# 重建 fuzz corpus（提交入库；改动合成器/语法时执行）
build/debug/topos_gen_bitstream_corpus -dump tests/fuzz/seed_corpus_bitstream

# 重建 golden（有意变更语法/熵语义/QM 时：先走 ADR + spec §13 版本规则）
build/debug/topos_golden_transform  gen tests/conformance/golden_transform_v1.bin
build/debug/topos_golden_bitstream  gen tests/conformance/golden_bitstream_v1.bin
build/debug/topos_golden_codec      gen tests/conformance/golden_codec_v1.bin
```

## 设计说明（记录性决策）

1. **内部基础设施 header-only**（`static inline`，无静态库）：checked/bufview/endian
   均为零开销原语，测试直接 `#include` 内部头，避免公共/私有边界污染。
2. **mini_test 零依赖框架**：ctest 已提供运行/报告层，框架层只做断言与确定性 PRNG
   （xorshift64\*，测试必须可复现）。
3. **fuzz 双形态**：libFuzzer target（clang）+ 确定性回放 driver（`-gen` 内置 LCG）。
   ctest 在**所有** sanitizer 配置下运行回放 driver，fuzz 覆盖不依赖 libFuzzer 存在。
   位流 corpus 自检门进一步要求：合法种子必须 scan+CRC+符号解码全通过，变异样本
   不越不变量（ADR-C003 C-29）；阶段 4 增加 codec 全解码 fuzz（ADR-C004 C-33/34）。
4. **零告警门禁**：核心库 `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
   -Wsign-conversion -Werror`——`-Wconversion` 强制外部 size 算术显式化。
5. **CPU 探测只报告不 dispatch**：`tc_query_cpu_features` 供上层展示与阶段 9 前置
   测量；核心路径不读它（计划阶段 1 约束）。
6. **粘滞错误模型（阶段 3）**：位读端越界后不可恢复、绝不推进；位写端上限 256 MiB。
   「任意截断不越界」「随机输入无死循环/无超大分配」是结构性质（packet 扫描零堆分配、
   符号解码 O(1) 指纹路径、解码流式 sink），由 fuzz 常驻验证。
7. **golden 双保险**：check 侧以与 gen 相同的确定性 builder 同源重建逐字节比对——
   编码器输出的任何无声漂移（重构、编译器、平台）都会在门禁失败。阶段 4 起
   golden_codec 同时冻结编码字节与解码平面指纹（后端一致性 §13.3）。
8. **确定性编码（阶段 4）**：k 估计（floor(log2(mean))）与 qp 选择均为输入纯函数；
   帧级目标码率走 `tc_frame_encode_sized`（≤24 次确定性搜索）。码率命中机制与
   业界常见中间片编码的帧级质控同构：矩阵固定频率响应（Standard v1.1 冻结），
   qp 承担内容自适应。
