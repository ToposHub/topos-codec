# Topos Image SDK 文档（v1 Preview）

状态：O7 ABI/SDK 收口（2026-09-08）。规范：`topos_image_file_spec_v0.md`；
决策链：`ADR-I001`..`ADR-I006`。

## 1. 支持矩阵（真相源 docs/image/capability_manifest.json）

| Profile | 像素 | 位深 | Alpha | 状态 |
| --- | --- | --- | --- | --- |
| Image Preview (0) | YUV 4:2:2 | 10/12 | 无 / A16 无损 / bounded | ✅ supported |
| Image HQ (1) | GBR 4:4:4 | 10 | 同上 | ✅ supported |
| Image XQ (2) | GBR 4:4:4 | 12 | 同上 | ✅ supported |
| Image RAW (3) | Bayer CFA 4 相位平面（R/Gr/Gb/B 各 W/2×H/2） | 12（Log 制作）/ 16（Linear 归档） | 无（与 alpha 互斥） | ✅ supported（2026-09-13 由 reserved 改派；ADR-I001 D9 相应作废） |
| Image Float (4) | GBR 4:4:4（float16 线性，spec §15） | 16 | 无 / A16 无损 / premultiplied | ✅ supported（2026-09-19，ADR-C057） |

硬上限：16384×16384、单 PIXL ≤256 MiB、chunk 数 ≤64、
ICCP 8MiB / XMP 16MiB / EXIF 16MiB / OCIO 4KiB / THMB 4MiB / HASH 36B。
明确非目标：tile/ROI、FLOAT32、多图、mipmap、浏览器解码。

## 2. C ABI（include/topos_image.h；随 libtopos_codec 分发）

```c
const char* tc_image_status_message(int32_t status);      // 任意码→静态文本
int32_t tc_image_probe(const topos_io*, topos_image_info*);            // 只读头部
int32_t tc_image_validate(const topos_io*, uint32_t flags,
                          topos_image_info*);             // flags: TC_IMG_VALIDATE_DEEP
int32_t tc_image_decode(const topos_io*,
                        const topos_plane_view[4],
                        topos_frame_output*);              // 语义==直连 tc_frame_decode
int32_t tc_image_decode_preview(const topos_io*, uint32_t target_w,
                                uint32_t target_h,
                                const topos_plane_view[4],
                                topos_frame_output*);       // target-size native reconstruction
int32_t tc_image_query_decode_buffer(const topos_image_info*, uint32_t plane,
                                     uint32_t* w, uint32_t* h);
int32_t tc_image_read_metadata(const topos_io*, const topos_image_info*,
                               uint32_t chunk_type, void* buf, size_t cap,
                               size_t* out_size);
int32_t tc_image_query_capabilities(topos_image_capabilities*);
int32_t tc_image_derive_idsc(const void* packet, size_t size,
                             uint8_t image_profile, topos_image_idsc*);
int32_t tc_image_write(const topos_image_write_params*, const topos_io*,
                       uint64_t* out_size);
int32_t tc_image_half_to_codes(const uint16_t* half, size_t n, uint16_t* codes);   /* spec §15 冻结映射 */
int32_t tc_image_codes_to_half(const uint16_t* codes, size_t n, uint16_t* half);
```

### 错误模型

- 复用 codec `TC_ERR_*`（-1..-16）；image 层新增 -100..-107
  （BAD_MAGIC / BAD_PREAMBLE / BAD_DIRECTORY / UNKNOWN_CRITICAL /
  CHUNK_CONFLICT / METADATA_CONFLICT / LIMIT / IO_WRITE_FAILED）；
- 文本：`tc_image_status_message()` + 线程局部 `tc_last_error()` 详情；
- 同一坏文件按冻结的解析顺序（spec §2.11）稳定返回同一错误码。

### 所有权与线程

- 全部结构 caller 分配；`pixels` 缓冲 caller 提供（decode 零拷贝直写）；
- `topos_io` 回调 caller 所有，仅调用期间引用；
- 每个入口单发无阻塞、可多线程并发（错误详情线程局部）；无长循环，
  取消语义由序列层 generation 模型承载（`topos_image_sequence.py`）。

### 原子写（spec §12）

调用方（CLI/适配器）负责临时文件 + fsync + 写后自检 + rename；
核心 `tc_image_write` 保证确定性输出与失败语义（sink 失败 → TC_ERR_IO，
不留下半文件的责任在调用层按 §12 清理）。参考实现：`toos encode`。

## 3. CLI（build/<cfg>/toos）

```
toos probe <file>                      打印描述（profile/尺寸/平面/位深/色彩/PIXL 位置）
toos verify <file> [--deep]            结构+CRC（--deep 附 PIXL 结构探测）
toos decode <file> -o out.raw          planar uint16 LE + out.raw.json sidecar
                                       （通道名 G,B,R|Y,U,V,A；色彩标签显式）
toos encode in.raw -o out.toos         --width/--height/--pixel-format/--bit-depth
                                       [--alpha-mode --qp --entropy vlc|v1|rans2
                                        --image-profile --overwrite]

默认（2026-09-12 起跟随视频优化集，ADR-C038）：熵编码 = rans2（V7-R2 上下文
rANS，AQ 开启）；`--entropy vlc` = spec v2 canonical VLC（+AQ/RDO）；
`--entropy v1` 退回旧 Rice 位流；intra/intra-range/rans 已退役（ADR-C046）；
`--qp 28` = 10-bit 数学无损（归档用，不在档位菜单）。

档位（ADR-I011，`--tier <id>`；锚点 = 1080p 实拍素材帧0 实测，真相源
capability_manifest.json tiers；2026-09-21 复查对齐注册表 22 档全量——
修订六后 444 档为 10-bit qm1/qm0，下表旧"12-bit qm2 ≈4.18/4.46 MB"
口径已废止）：

| id | 标签 | 质量 | 格式 | qm | qp | MB/帧 |
|---|---|---|---|---|---|---|
| 422-low | Topos 422 Low | 低 | 4:2:2 10-bit | 3 | 63 | 1.08 |
| 422-medium | Topos 422 Medium | 中 | 4:2:2 10-bit | 1 | 61 | 1.29 |
| 422-high | Topos 422 High | 高 | 4:2:2 10-bit | 1 | 58 | 1.71 |
| 422-ultra | Topos 422 Ultra | 超高 | 4:2:2 10-bit | 0 | 58 | 2.86 |
| 444-high | Topos 444 High | 高 | GBR 4:4:4 10-bit | 1 | 63 | 1.30 |
| 444-ultra | Topos 444 Ultra | 超高 | GBR 4:4:4 10-bit | 0 | 58 | 3.40 |
| float-low | toos Low 16bit float | 低 | GBR HALF 16-bit float | 0 | 82 | 0.38 |
| float-medium | toos Medium 16bit float | 中 | GBR HALF 16-bit float | 0 | 72 | 0.99 |
| float-high | toos High 16bit float | 高 | GBR HALF 16-bit float | 0 | 58 | 4.45 |
| float-ultra | toos Ultra 16bit float | 超高 | GBR HALF 16-bit float | 0 | 44 | 5.75 |
| raw12-2 … raw12-16 | Topos RAW 12-bit N:1（6 档） | RAW | CFA 12-bit | 0 | 45/59/64/66/68/70 | 2.07…0.26 |
| raw16-2 … raw16-16 | Topos RAW 16-bit N:1（6 档） | RAW | CFA 16-bit | 0 | 51/72/76/78/80/82 | 2.07…0.26 |

能力边界（2026-09-28 复验）：`.toos` 整数 GBR 图片目前只有 10/12-bit
profile；16-bit 整数 GBR **视频**已可编码，不能据此推定有 16-bit 整数
GBR 图片档。`raw16-*` 是 CFA 传感器码值，`float-*` 是 half 浮点样本，
均不是整数 GBR16 图片的替代声明。四个 `float-*` 有损预设在写盘前拒绝
Inf/NaN，防止量化跨越有限/非有限类别；负值、超过 1.0 的有限值仍可输入。
显式 `--half --qp` 是独立的专业控制路径，特殊值往返语义须按所选 QP 验证。
RAW 档名中的 N:1 为用途锚点，实际压缩比随素材而变。

qm = 量化矩阵（0 = flat；1 = Standard；2 = 444 Compact，修订六后无档位
引用——仅显式 `--qmatrix 2` 可达；3 = 422 Low Compact，仅 422-low）。
ADR-I011 修订四：锚点素材修正——真 4:2:2（色度 2:1 降采样）下 4:2:2
家族比旧锚点低 10~16%（PSNR 不变）；422-low 在 qp=63 地板上启用轻度
422 Low Compact（qm3），medium/high 为 Standard 矩阵单轴 qp 61/58。
修订六（2026-09-12）：444 档 12-bit→10-bit（ProRes 4444 同位深口径），
qm2→qm1/qm0，锚点 1.30/3.40 MB。修订七（ADR-C057）：浮点四档
（HALF 域，spec §15；`decode_float()` 读出 float16 线性平面）。RAW 十二
档（TRAW，bitstream profile 7 / IDSC image_profile 3；输入 = 4 相位
平面 R/Gr/Gb/B 依次拼接 uint16 LE；比率档为 CQ 语义，体积随内容浮动）。
产品语义（修订三定案）：很好画质 + 相对体积，档位不设真无损——真无损
需求走 `--qp 28`（10-bit 数学无损）或无损容器。422-ultra = qm0 qp58
（视觉无损级 PSNR≈69 dB，与 high 同 qp 仅矩阵不同，**= 无 --tier 默认**）。
tier 是质量预设（命名空间与 image profile 分离）；显式
--pixel-format/--bit-depth/--qp/--qmatrix/--image-profile 覆盖档位。

视频 → toos 序列推荐流（4:2:2 10-bit，保色彩信息且解码快）：转出
yuv422p10le planar u16 LE 帧文件后逐帧 `toos encode --pixel-format 0
--bit-depth 10 --image-profile 0`（其余参数走默认）。

代理/审片序列推荐流（体积优先）：转出半分辨率（如 960×540）4:2:2 帧
后 `toos encode --tier 422-low`，≈0.4 MB/帧（约为全分辨率 low 的 30%，
PSNR≈47 dB@540p，代理观感良好）。tier 是质量预设，分辨率由输入决定。
toos benchmark <file> [--iters N]      probe/validate/decode 分项 p50/p95/p99/max
```

exit：0 成功 / 1 失败 / 2 用法。通道顺序冻结：YUV=Y,U,V；GBR=G,B,R（C-130）；A=plane3。

## 4. Python 绑定（src/shared/）

- `codec/topos_image_binding.py`：`ToposImageCodec`
  （probe/validate/decode/decode_float/decode_preview/read_metadata/
  capabilities），解码直接写 caller-owned numpy；`decode_preview(target_size)`
  保持宽高比且只缩小。当前 target-size 是完整 native decode 后的确定性
  planar 缩放，不宣称降低 native decode 峰值 RSS；见 `ADR-I016`；
  `decode_float()` 解码 HALF 样本域（sample_kind=HALF，spec §15）文件为
  float16 线性平面（UINT 文件显式拒绝——样本域不混用）；
- `media/topos_image_source.py`：`ToposImageSource`（MediaSource 协议，单帧），
  pixel_format 注册名复用视频链（gbrp10le/…）；
- `media/topos_image_sequence.py`：序列枚举 + 有界预取
  （generation cancel、平面 LRU 字节预算、实时指纹失效）；
- `export/topos_encoder.py`：`ToposImageSequenceEncoder` 复用视频导出的
  `ToposCodec`、编码缓冲、slice worker 和码率反馈，每帧只增加 TPIM envelope
  写入，输出独立的 `.toos` 文件；
- `media/topos_frame_extract.py`：`extract_topos_frame_to_toos()` 和可复用的
  `ToposFrameExtractor` 直接从 Topos MOV sample 提取 TPIC packet 并包装为
  `.toos`，不经过解码或重新编码；
- `media/factory.py`：auto 后端按 TPIM magic 路由（`toos_image`）。

## 5. 互转适配器

`scripts/toos_tools.py`：.toos ↔ PNG（cv2 16-bit PNG；GBR⇄RGB 显式交换；
YUV→PNG 显式拒绝——避免静默猜 sRGB；位深缩放舍入显式报告）。
刻度契约（ADR-I009 修复后）：彩色平面按声明位深满刻度换算（encode
四舍五入+饱和、decode 左移对齐），alpha 平面恒按 codec A16 容器
（0..65535）直通——8/16-bit PNG 的 alpha 语义均逐位正确。

质量语义与可重现报告：`scripts/validate_topos_image_profiles.py` 校验 Profile/tier
合同；`scripts/measure_topos_image_quality.py <reference> <candidate>` 输出
bit-exact、PSNR、SSIM、ΔE2000、RGB/Alpha 误差。profile 3 已于 2026-09-13
改派为 RAW（ADR-I008；v1 无 Lossless profile）；显式 `--qp 28` 的逐位
无损结果不能被写成 Lossless Profile，也不能替代
真实素材矩阵报告。

## 6. ABI smoke 与外部宿主

`python scripts/topos_abi_smoke.py <file.toos> [--library <libtopos_codec>]`
执行 capability、probe、decode 和平面几何检查，并输出平台、机器、库身份、
文件摘要和 decode 额外状态。发布包在 macOS arm64/x86_64、Linux x86_64 和
Windows x64 上各运行一次；不可用平台必须标记 skipped，不能写成 supported。

所有外部适配器只依赖 `topos_image.h`/`topos_codec.h` 的公开 ABI 或本 Python
参考 binding，不复制 TPIC 解析、色彩矩阵、Alpha 或熵编码。`struct_size`、
`abi_version`、capability mask 和负错误码是兼容协商的唯一入口。

## 7. 兼容与演进

- file_version 1.x：新增 optional chunk / compatibility 位 → minor +1；
  IDSC 布局/必需性/窗口语义变更 → major +1（读端对未知 major 硬失败）；
- 内层 TPIC packet 版本独立（payload_major/minor 随 codec），封装升级不改 codec 语义；
- 未知 optional chunk：跳过；可编辑 round-trip 字节级保留（preserve 位）；
  未知 critical：TC_IMG_ERR_UNKNOWN_CRITICAL；
- golden 兼容门禁：tests/conformance/golden_image_v1*.bin 在后续版本持续通过。

## 8. 门禁基线（2026-09-08；同日审计修订版）

- debug 34 测试二进制全过；ASan/UBSan 全过；fuzz（TPIM 层）4096 runs 全过
  （审计修订：topos_fuzz_image 已纳入 run_tests.sh libFuzzer 短跑门禁）；
- check_exports：38 项公共符号钉死全在（2026-09-21 增 `tc_image_half_to_codes`/`tc_image_codes_to_half`，E10）；
- golden：image 5 用例 + 旧视频 golden（transform/bitstream/codec/mov）零回归；
- pytest：image 专属 68 用例（source 14 + sequence 22 + toos_tools 16 +
  capability_manifest 16）全过；另有视频链 test_topos_source 25 用例作回归门禁。
  （口径修订：原"61 用例"把 9 个与 toos 无关的视频 factory 用例计入 image
  门禁，已纠正并如实分层；ADR-I009 二轮审计后新增 toos_tools 5 用例与
  sequence 2 并发回归用例；ADR-I010 新增默认编码策略 5 用例；ADR-I011
  三轮修订后 toos_tools 增至 16：档位回归 6（参数映射/体积序/ultra>high
  画质序/默认≡422-ultra 逐字节/显式覆盖/未知拒绝）+ 默认编码策略 5 +
  PNG 适配往返 5）；
- image pytest 与 topos_fuzz_image 已纳入 run_tests.sh（此前无自动化入口）；
- 性能基线：docs/image/perf_report_stage7.md（i9-9900K 实测）。

## 9. 未尽事项（如实声明）

- 应用层集成面：ToposImageSource 已可经 factory auto 路由消费，Deliver/Edit
  已接入 `Topos Image Sequence` 目录导出；单图导出、缩略图
  （thumbnail_provider 的 `.toos` 分支未接）、导入分辨率探测（project.py
  按扩展名走 cv2 路径）仍未全部接线——阶段 5 的"单图导出/缩略图同一
  metadata 语义"目标未全额兑现；
- 序列源（ToposImageSequenceSource）策略语义已冻结（skip/error/hold、重复
  帧拒绝、padding 严格位宽），但尚未接入任何 UI/播放链，未复用
  DecodeQueue/DecodedFrame 优先级模型——"1000+ 帧顺播/跳帧/反向"验收
  场景待接线后执行；
- 当前 C ABI 面向帧级编码和单文件 envelope 写入，尚未提供宿主插件专用的
  `tc_image_sequence_encoder` 会话 API。Premiere/Resolve 适配器可以直接链接
  `libtopos_codec`，但应在插件进程内长期保持一套编码配置、复用输出缓冲和
  slice worker，再逐帧调用 `tc_frame_encode` + `tc_image_write`；不要每帧
  启动 `toos` 子进程。后续可在保持现有 ABI 的前提下增加 C session facade，
  统一取消、文件命名、原子提交和批量解码语义；
- Windows/Linux 构建与 ABI smoke：待分发环境（计划书阶段 12 平台矩阵；
  CLI 层已补 Windows 可移植守卫：_fseeki64/_commit/MoveFileEx/QPC）；
- NAS/冷缓存性能矩阵、真实素材质量基线（PSNR/SSIM）、1000+ 帧长序列压测：
  按 benchmark_protocol 扩展执行；
- OIIO 插件 oiiotool 往返验收：待 OIIO SDK 环境；
- 深度 ICC primaries↔CICP 比对：v1.1 扩展（ADR-I003）；
- Nuke / Photoshop / After Effects 插件：后续独立立项（ADR-I001 D14）。
