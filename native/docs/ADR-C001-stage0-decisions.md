# ADR-C001：Topos Video Codec V2.0 阶段 0 决策冻结

- 状态：已接受（2026-08-29）
- 关联计划：`docs/专属极速帧内中间片编码引擎（Topos V2.0）.txt`（下称「计划」）
- 关联规范：`docs/bitstream_spec_v1.md（原 v0，阶段 3 更名冻结）`（位流规范 v0，本文决策 C-06/C-07/C-08/C-09 的载体）
- 调研依据：2026-08-29 对仓库 `src/shared/media`、`src/shared/video`、`src/shared/export`、
  `src/features/edit|deliver|color|composite`、`tools/`、`tests/` 的只读架构调研（结论已并入本文）。

> 编号规则：codec 专属 ADR 使用 `docs/ADR-C0xx`，与仓库既有 `docs/adr/000x`（HTML 渲染器）
> 互不冲突。本文是阶段 0 交付物「architecture decision record」，一次冻结全部首版决策；
> 后续阶段如推翻某条决策，新建 ADR-C0xx 并在此处标注被取代。

---

## 首版（V2.0 / 位流 v1.0）支持矩阵（决策 C-01）

| 维度 | 冻结值 |
| --- | --- |
| 颜色格式 | YUV 4:2:2 平面，10-bit（uint16 LE，码值 0..1023） |
| Profile | 仅 `Topos Standard`（profile id 3）；其余档位 ID 已在位流中预留 |
| Alpha | 可选，独立全分辨率平面，16-bit，**仅无损模式**（受限近似模式 ID 预留，阶段 4 实现） |
| Alpha 语义 | 默认 straight；premultiplied 输入必须显式转换或置 header 标志 |
| 帧结构 | 帧内独立 I 帧；progressive；CFR（时间信息由容器承载，elementary packet 不含 PTS） |
| 位深扩展 | 12-bit 字段已定义，v1.0 解码器可拒绝（UNSUPPORTED） |
| 输出 | CPU planar buffer（与 `DecodedFrame.planes` 契约一致） |
| 容器 | MOV（libavformat），FourCC `TPIC`，私有配置 atom `tpcC`；音频经容器旁路（C-18） |
| CLI | `topos_encoder_cli` / `topos_decoder_cli` / `topos_inspect_cli` / `topos_probe_cli` |
| 开发基准机 | macOS 15.3.1 x86_64，i9-9900K（8C16T，AVX2），32GB（标量路径必须可移植） |
| 平台目标 | macOS x86_64（开发）→ x86_64/arm64 的 macOS/Windows/Linux（标量优先） |

不进首版：4:4:4/GBR、12-bit 实现、受限近似 Alpha、Inter/Micro-GOP（V2.1）、GPU 后端、
非 CFR/VFR、B 帧、音频编码进 codec 核心。

---

## C-02 原生语言：C11（不用 C++/Rust）

- **决策**：`native/topos_codec/` 核心用 C11（`-std=c11`，禁用 VLA/边界外依赖），公共头是纯 C ABI。
- **理由**：计划要求「Stable Topos C ABI」；C11 无运行时、无异常、三平台编译器支持一致，
  SIMD（AVX2/NEON）直接用 intrinsics；确定性整数运算易于 sanitizer/fuzz 验证。
- **备选与否决**：
  - C++17：ABI 复杂（name mangling、STL 跨编译器边界），收益低；
  - Rust：内存安全收益真实，但三平台工具链 + PyInstaller 打包 + 与现有团队技能栈的集成成本高，
    且本 codec 的内存安全边界（bit reader、checked arithmetic）可以用 C + fuzz + sanitizer 管控。
- **后果**：约束写入规范——所有内存手动管理必须走统一 checked helper（阶段 1 建立）；
  禁止在核心内使用 `malloc` 之外的分配器抽象（后续如需池化再议）。

## C-03 构建：CMake ≥ 3.16 + CTest；Debug/ASan/UBSan/libFuzzer 四配置

- 仓库现状：**无 CMake**（唯一 native 先例 `src/native/decklink_bridge/topos_decklink.cpp` 由
  `tools/build_topos_decklink.py` 直接调 clang 编译，产物 `.so` 绑定 CPython 版本）。
- **决策**：新建 `native/topos_codec/CMakeLists.txt`，产出 `libtopos_codec.dylib/.so/.dll` +
  CLI 可执行；`ctest` 挂 unit/conformance/fuzz 冒烟；CI（或本地一键脚本）四个构建配置：
  `Debug`、`ASan`、`UBSan`、`Fuzz`（clang `-fsanitize=fuzzer,address`；无 libFuzzer 的平台退化为
  确定性 corpus 回放 driver）。
- 开发机已具备：Apple clang 17、CMake 4.4.2、系统 FFmpeg 8.1（`/usr/local/bin/ffmpeg`）。
- decklink 先例的教训（见 C-04）不影响本决策：库按 OS+CPU 架构分发，不绑 Python 小版本。

## C-04 Python 绑定：ctypes over 稳定 C ABI（不编 CPython 扩展模块）

- **决策**：绑定层用**标准库 ctypes**（ABI 模式），动态加载 `libtopos_codec`；
  Python 侧用 dataclass 镜像所有公开 struct，构造时填充 `struct_size`/`abi_version`，
  并对每个镜像做 `sizeof` 一致性测试（阶段 1 起常驻）。
- **理由**：
  1. 本仓库 Python 版本分裂已经是现实：`.venv` 是 3.12.13（PyAV 17.0.1），系统是 3.13.5；
     CPython 扩展模块（如 `topos_decklink.cpython-312-darwin.so`）每个小版本都要重编；
  2. 计划要求「稳定、可版本化 C ABI + Python 集成层」，ctypes 正是零编译消费 C ABI 的方式；
  3. PyInstaller 打包只需把 dylib 加进 `packaging/pyinstaller/*.spec` 的 binaries。
- **备选与否决**：cffi（多一个第三方依赖，收益有限）、pybind11/Cython（回到绑版本编译）、
  纯 Python 实现（无法满足性能与 fuzz 要求）。
- **后果**：回调（IO callbacks）走 ctypes `CFUNCCTYPE`，注意保持回调对象引用防 GC；
  数组传参显式传 `void*` + capacity，禁止隐式长度。

## C-05 容器：libavformat（CLI 侧 C 链接 / 应用侧 PyAV），手写 MOV parser 一票否决

- **决策**：MOV mux/demux 用 libavformat：
  - **CLI**（阶段 5）：native 直接 `find_package(FFmpeg)` 链接 libavformat；
  - **应用**（阶段 6/8）：Python 侧用 **PyAV**（复用 `.venv` 自带 FFmpeg 17.0.1）做 demux/mux，
    elementary packet 喂给 C 核心编解码——**C 核心不感知容器**。
- **理由**：仓库已有成熟 PyAV 管线与 `src/shared/ffmpeg_locator.py` 双路径定位；计划 §3.3 明令
  首版不手写完整 MOV parser。
- **风险与前置 spike（阶段 5 第一件事）**：libavformat movenc 对私有 FourCC `TPIC` +
  未知 codec sample entry 的写出/读回行为需要一个小 spike 验证（写 `AV_CODEC_ID_NONE` +
  `codec_tag='TPIC'` 的 stream 是否生成合法 stsd，未知 atom 是否可安全跳过）。
  若失败，回退方案是受 gate 控制的最小 MOV writer（仅 stsd/stts/stsc/stsz/stco/co64，
  conformance fixture 全覆盖），并升级风险 R-20。
- `tpcC`（codec configuration atom）字段集在 `bitstream_spec_v1.md（原 v0，阶段 3 更名冻结）` §12 定义。

## C-06 变换：H.264 风格 8×8 整数 DCT（矩阵、界、逆变换全整数控死）

- **决策**：forward `F = M·x'·Mᵀ`，M 为固定的 8×8 整数矩阵（值见 spec 附录 A），
  纯整数矩阵乘（无中间舍入），s32 累加；逆变换 `Mᵀ·F'·M` 后统一 `+131072 >> 18`，
  累加必须 s64（界证明见 spec §7.6）。level shift `x − 2^(depth−1)`。
- **理由**：矩阵公开可查、近似正交、业界验证充分；每一步可写成精确整数规范，
  标量与 SIMD 天然一致；无浮点进入规范性路径（计划 §7 阶段 2 完成门槛）。
- **备选与否决**：VC-2/Dirac 整数 lifting（实现复杂度高）、AV1 变换（过强、专利与复杂度不匹配）、
  浮点 DCT 定点化（舍入规约负担大且计划禁止）。
- **频率域能量差异**（M 各行能量 128–578 不等）通过量化矩阵（C-07）吸收，
  不引入额外归一化步骤——与 H.264 实践一致。

## C-07 量化：deadzone 均匀量化，表驱动，flat 矩阵为 v0 占位

- **决策**：量化/反量化公式在 spec §7.4 完整定义：
  `Q = (QM[u][v]·qp_scale[qp]+128)>>8`；`q = ±(|F| + Q/2 − dz)/Q`（DC dz=0，AC dz=Q>>2）；
  `F' = q·Q`。`qp ∈ 0..63`；`qp_scale` 由公式生成（spec 附录 A.4，v0 即冻结公式与表值）。
- **v0 占位**：`qmatrix_id=3`（Standard）先用 flat 矩阵（全部 16）；调优矩阵在阶段 2 产出
  golden vectors 时定稿——**只改表值不改结构**，位流格式不受影响（matrix id 语义不变，
  v1.0 golden 冻结前的调整不需要版本号；冻结后任何表值变更 = minor version 提升）。

## C-08 熵编码：单一方案——有界 Rice（31 位 unary 上限 + 32-bit escape）

- **决策**：全码流只有一种熵方案（计划 §4.4 要求「正文只保留一种正式方案」）：
  - 符号体系：颜色平面 = 每块 1 个 DC 差分符号 + (run, level) 对 + 恒定 EOB（run=63）；
    Alpha = 像素残差 (run, level) 对，以像素计数终止（无 EOB）。
  - Rice：`q = m>>k`；`q ≤ 30` → `q 个 1 + 0 + k 位余数（MSB 先发）`；
    `q > 30` → `31 个 1 + 32 位字面值`。unary 上限 31 位，解码器按此封顶（**禁无界 unary/Exp-Golomb**）。
  - k 参数每 slice 在 slice header 中显式传输（0..14），slice 边界全部重启，无跨 slice 自适应状态。
  - 域校验：解码出的每个符号立即对照其值域（run ≤ 62 / 系数 ≤ 变换上界 / 残差 ≤ 65535），
    越界即 `TC_ERR_MALFORMED`。
- **备选与否决**：无界 Exp-Golomb（违反计划）、CABAC/CAVLC（复杂度与并行性不匹配首版）。

## C-09 Alpha：独立 16-bit 无损平面（MED 预测 + 残差 Rice），不经过颜色 DCT 路径

- **决策**：Alpha 永不进入颜色量化路径（计划 §2.2）。v1.0 实现 `alpha_mode=1`（无损）：
  MED（JPEG-LS 式三邻域）预测 → 残差 → 与颜色相同的 Rice/escape 熵编码（独立 k 参数、独立 slice）。
  `alpha_mode=2`（受限近似）ID 预留，阶段 4 实现；v1.0 解码器遇之返回
  `TC_ERR_UNSUPPORTED_ALPHA_MODE`。
- **语义**：默认存储 straight alpha；输入 premultiplied 时编码器必须显式转换或置 header
  `alpha_premultiplied` 标志，禁止静默猜测。Alpha 目标比例/硬上限（25%–30%）是**编码器行为约束**
  （spec §11）+ 容器 `tpcC` 记录项，不是 elementary 位流字段。
- **依据**：调研确认 GPU 链路现状——YUV→RGB shader 硬编码 alpha=1.0
  （`src/shared/video/yuv_gpu_upload.py` L1806/L1928），唯一读 `planes[3]` 的 CPU fallback 把
  alpha 量化到 8-bit（`src/features/color/core/async_renderer.py` L4315-4323）。
  这意味着阶段 7 的 GPU 侧工作是**新增性质**（第四平面上传通道），不是修复性质。

## C-10 字节序与位序：多字节字段大端；位流 MSB-first；slice 字节对齐

- header/slice 多字节字段一律 big-endian；熵编码位流在字节内 MSB-first；
  每个 slice payload 末尾零填充到字节边界，slice 起始地址字节对齐（配合 slice_size 可安全跳过）。
- CRC-32（IEEE 802.3，poly `0xEDB88320`，init/final `0xFFFFFFFF`）用于 frame header 与每 slice payload；
  spec 附录 A.6 给自测向量（`"123456789"` → `0xCBF43926`）。

## C-11 解码器限制与错误模型（在 spec §10 全量冻结）

- 硬限制：coded ≤ 8192×8192 且 8 的倍数；slice 总数 ≤ 512；frame packet ≤ 256 MiB；
  plane_count ∈ {3,4}；每块 AC 对 ≤ 63；Rice k ≤ 14；unary ≤ 31。
- 错误码：`TC_OK=0`，警告 >0（`TC_WARN_CONCEALED`），错误 <0（16 个枚举，spec §10.3），
  全部经 `tc_last_error()` 提供可读详情。
- concealment：slice CRC 失败或结构非法时，仅该 slice 区域填充中性值（颜色 = 中灰
  `1<<(depth-1)`，Alpha = 65535 不透明），帧级返回 `TC_WARN_CONCEALED`；
  单 slice 损坏不得污染其他 slice 或后续帧。
- 所有外部 size/offset/count 算术走 checked helpers（阶段 1 建立，u64 预检 + 拒绝）。

## C-12 后端边界：CPU 标量是规范性实现；SIMD/GPU 是可替换执行后端

- 标量路径 = 唯一语义参考；AVX2/NEON（阶段 9）必须与标量 bit-exact，经同一套 golden vectors
  与 differential 测试。
- GPU 允许的扩展点（未来）：transform、quantization、Alpha 预测——**熵编码与码流写出留在 CPU**；
  混合数据流只允许回读最终压缩 packet，禁止中间系数作为 GPU↔CPU 常规接口（计划阶段 0 要求冻结）。
- GPU 经 opaque extension API（capability query + 独立版本号），device lost/不支持 → 可诊断回退 CPU；
  基础 ABI 首版只有 CPU plane buffer。
- V2.0 **不因未启用 GPU 后端而视为未完成**（计划 §11）。

## C-13 Inter/Micro-GOP 兼容边界（V2.1 预留，V2.0 只实现 Intra）

- frame header 已含 `frame_type`（0=I，1=P 预留）、`gop_id`、`ref_distance` 字段，V2.0 恒为 0；
  v1.0 解码器遇非 0 值返回 `TC_ERR_UNSUPPORTED`。
- V2.1 约束已在计划冻结：闭合 GOP、仅前向参考、无 B 帧、随机访问索引经容器 sample flags 扩展。

## C-14 应用集成挂点（阶段 6/8 的施工图，本阶段只冻结位置不动代码）

**解码接入（阶段 6）**：
1. 新文件 `src/shared/media/topos_source.py`：实现 `MediaSource` Protocol
   （`src/shared/media/source.py` L62）——`open/close/probe/read_frame/seek_frame` + 属性
   `is_open/current_frame/path`，**并额外提供 `read_next_frame()`**（worker 实际依赖，
   协议未列；参照 `FFmpegMediaSource.read_next_frame` L1098）。
2. 输出 `DecodedFrame`：planar `planes=(Y,U,V[,A])`，uint16、码值 0..2^N−1、
   tight contiguous（stride = width×itemsize）、`plane_infos` per-plane bit_depth——
   与 `yuv_gpu_upload.py` R16UI 路径及 `color_decode.py` 满刻度约定一致。
3. 注册：`MediaSourceFactory.register_backend('topos', create_topos, check_topos_available)`
   （`src/shared/media/factory.py` L103）；**必须同时改 `_get_backend_order`（L245-251，
   现硬编码 `['ffmpeg']`）或 `_select_backend`（L206）**，使 auto 模式按文件探测（magic
   `TPIC`/容器 probe）路由到 topos 后端。
4. 线程契约：单实例不可重入（对齐 `ClipSession.decode_lock` 的单 lane 假设）；
   native context 的 close/seek/cancel 必须全路径释放（阶段 6 完成门槛）。
5. `pixel_format` 名称需在 `src/shared/media/frame.py` `PIXEL_FORMATS` 注册表新增
   （建议 `topos_yuv422p10` / `topos_yuva422p10a16`，4:2:2 全高、Alpha 全分辨率不子采样）。

**编码接入（阶段 8）**：
1. 仓库**没有 encoder factory/protocol**（调研确认）；首版不强行抽象——
   `ToposVideoEncoder`（`src/shared/export/topos_encoder.py`）**duck-type 对齐
   `FFmpegVideoEncoder` 接口**：`__init__(output_path, config)` / `open()` /
   `encode_frame(DecodedFrame)` / `close()` / `get_progress()` / `frame_count` / `is_open`
   （`src/shared/export/video_encoder.py` L500-1580）。
2. 注入点：`TimelineExporter._create_encoder()`（`src/features/edit/core/timeline_export.py` L2333）
   按 codec 分派；`RenderCacheEncoder.open()`（`render_cache_encoder.py` L38）同理。
3. 枚举/映射/UI/persist 共 8 处登记点（阶段 8 清单）：`ExportCodec`（timeline_export.py L162）、
   `_get_codec_settings`（L2306）、`CODEC_CONTAINER_TABLE`（video_export_formats.py L27）、
   `_map_codec`（video_export_job.py L781）、`_CODEC_CATALOGUE`（video_export_settings_panel.py L111）、
   `_CONTAINER_CODECS`（quick_export_dialog.py L557）、`perform_video_export` codec_map
   （deliver_controller.py L689）、i18n `codec_*`（zh_CN/en_US）。
4. 持久化：新 codec 参数走 `VideoConfig.extra` dict（免 schema bump）；仅当需要一等字段时
   bump `VIDEO_EXPORT_JOB_SCHEMA_VERSION`；`.topos` 结构变更走
   `CURRENT_EDIT_CONTENT_SCHEMA_VERSION` + `ContentMigration`（project_migration.py L109）。

**GPU/Alpha（阶段 7，已知缺口清单）**：
`YUVTexturePool` 增第四平面纹理 + `upload_yuv422_r16ui`/float16 版 alpha 上传；
三处 shader `vec4(rgb,1.0)` → 采样 alpha 纹理（`_create_yuv_program` L1806、
`_create_r16ui_yuv_program` L1928、NV12 两处）；`async_renderer._do_upload_video_planes`（L4112）
alpha 分支；**替换 CPU fallback 8-bit alpha 降级**（L4315-4323）；
Edit 页 `_classify_planar_format`（L298）YUVA 分类 + track FBO 'f1'→'f2'；
Composite 页 `video_frame_node.py` planar 分支（L1051-1064）。R16UI 需 NEAREST +
macOS 自检门控回退 float16（`run_r16ui_self_test` L2536）。

## C-15 测试与基线资产：全部复用现有体系

- 素材：`tests/test_videos/`（9 个真实文件 ~16GB）+ `tests/media/MATERIAL_MATRIX.md` 矩阵
  + `tests/media/fixtures/` 合成素材惯例（`synthetic_fixture_generator.py`）。
- pytest：新测试落 `tests/media/test_topos_*`、`tests/native/`（C 侧用 ctest），
  沿用 `media_core/real_media/benchmark` 标记（pytest.ini）。
- 解码基线（已入库）：`docs/decode_speed_benchmark_2026-08-29.md/.json`
  （FFmpeg 软解 vs VT，i9-9900K，ProRes/DNxHR/H.264/HEVC 真实素材，p50/p95/最差/吞吐）。
- 编码基线：Windows AMF 脚本（`scripts/bench_hw_encode_windows.py`）存在但**无入库数据**；
  本阶段在开发机补测 macOS CPU 基线（ProRes 422 HQ / DNxHR HQX，结果写入
  `benchmark_protocol.md` §5 与 `docs/baseline_encode_2026-08-29.md`）。

## C-16 代码布局（计划 §3.2 落地确认）

```text
native/topos_codec/            # C11 核心 + CLI + ctest/fuzz（CMake，见 C-03）
  include/topos_codec.h        # 稳定 C ABI（阶段 1 定稿骨架）
  include/topos_codec_version.h
  src/{common,bitstream,transform,entropy,encoder,decoder,profiles,simd,container}/
  tests/{unit,conformance,fuzz}/
  tools/{topos_encoder_cli,topos_decoder_cli,topos_inspect_cli,topos_probe_cli}/
src/shared/media/topos_source.py     # 阶段 6
src/shared/export/topos_encoder.py   # 阶段 8
src/shared/codec/topos_binding.py    # ctypes 绑定层（阶段 1 建骨架，阶段 4 完整）
tests/media/test_topos_{source,roundtrip,alpha_pipeline,export}.py
tests/native/                         # 绑定层/ABI 一致性测试（pytest 侧）
```

（仓库现有 native 先例在 `src/native/decklink_bridge/`；codec 放顶层 `native/` 是为了
与 Python 包结构解耦、独立 CMake 与 CI，PyInstaller 经 spec binaries 收集 dylib。）

## C-17 C ABI 形态（阶段 1 定稿骨架，此处冻结原则）

- opaque context（`ToposDecoderRef*` / `ToposEncoderRef*`），所有公开 struct 带
  `struct_size` + `abi_version` + 保留字段 + 明确所有权注释。
- **可重入性契约**：单个 decoder/encoder context **不可重入**；不同 context 可并发。
  这与 `ClipSession.decode_lock`（单 lane 解码）的应用层假设一致，codec 内部阶段 9 才做
  有界 slice 并行（对调用方透明）。
- 取消模型：`tc_decoder_cancel(ctx)` 置原子标志，`decode` 在 slice 边界检查并返回
  `TC_ERR_CANCELLED`——不依赖线程抢占，不新增全局线程调度（计划 §0 原则 7）。
- IO：首版文件路径直开 + 可选 IO callbacks（读/写/seek 三回调 + `void* userdata`），
  callbacks 经 ctypes `CFUNCCTYPE` 桥接（C-04 后果项）。
- frame buffer 描述：per-plane `data/capacity/stride/width/height/sample_type/bit_depth/
  byte_order/alignment/ownership`（计划 §6.3），v1.0 输出仅
  `uint16 LE planar tight`（stride=width×2），ABI 仍按完整字段设计。

## C-18 音频策略：codec 核心纯视频；音频经 libavformat 旁路

- elementary packet 只含视频平面；MOV 内音频流由容器层处理（应用侧 PyAV 编码/拷贝，
  CLI 侧 libavformat）。导出带音频的 Topos MOV 时，`ToposVideoEncoder` 复用
  `FFmpegVideoEncoder` 的音频管线模式（`add_audio_stream`/`write_audio_samples` duck-type 兼容）。
- 后果：`ToposMediaSource.read_audio_samples` v1.0 经 PyAV demux 音频轨实现（或明确
  `NotImplementedError`，与现有 `FFmpegMediaSource.read_audio_samples` 现状一致）。

---

## 阶段 0 完成门槛对照

| 计划门槛 | 状态 |
| --- | --- |
| 所有字段、整数范围、Alpha 语义和错误模型均有书面定义 | ✅ `bitstream_spec_v1.md（原 v0，阶段 3 更名冻结）` |
| 不存在两套互相冲突的熵编码方案 | ✅ 单一 bounded Rice（C-08） |
| CPU 规范性路径与 SIMD/GPU 边界、回退、兼容策略已写入文档 | ✅ C-12 + spec §13 |
| 下一阶段不需要靠猜测补齐格式 | ✅ 阶段 1 只依赖 CMake/ABI 骨架（C-03/C-16/C-17），无格式歧义 |
