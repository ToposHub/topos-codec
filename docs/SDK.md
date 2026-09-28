# libtopos_codec SDK 指南（阶段 10 + 复验 O5 修订）

面向独立集成方（脱离 Topos Color 仓库使用本库）的最小完整文档。

> **能力范围（能力单一真相源：`docs/capability_manifest.json`）**：
> 本库为 **Topos Video Codec V1 Preview（10/12/16-bit 4:2:2/4:4:4/GBR 帧内
> + 帧间 LP 档，experimental）**——稳定的受限核心，不是完整 V2.0。已交付
> 格式域：12-bit（v1.2）、YUV 4:4:4（v1.3）、GBR 4:4:4（v1.4，平面序 G,B,R
> + matrix=0 恒等）、profile 5/6（Pro444/Extreme，格式交叉由帧头强制）、
> **16-bit（v1.7 扩展，bd16；宽域熵 rans2/v8）**、**Topos 422 LP 帧间档**
> （v1.8，ADR-C056：载体 V7-R3 zero-motion IP-2 微 GOP，锚 qp72，no-alpha，
> 规范 `bitstream_spec_v7r3_microgop.md`）、**视频 RAW 声明**
> （TC_TIER_RAW=8 / 帧 profile 7 + pf=3 CFA；D3 单 tier 参数化，编码链随
> M4-R7 激活）；七档 capability（Proxy/LT/Standard/HQ/LP = profile 3 质量预设，
> Pro444/Extreme = 独立帧头 profile，RAW = 声明中；量化矩阵帧内档统一
> id=1，外部显示名按 ProRes 惯例：422 Proxy/422 LT/422/422 HQ/4444/
> 4444 XQ/422 LP（M10-6.4 起产品 UI/i18n/媒体信息统一；
> 内部 tier_id 与 profile 字节不变），ADR-C023）。**未闭合项（复验
> 2026-08-31 口径）**：跨平台发布门禁（R7）、独立复审（R8）。

库本体：`native/topos_codec`（C11，零第三方依赖，仅 libc + pthread/Win32 线程）。

- 位流规范：`docs/bitstream_spec_v1.md`（v1.8 域：v1.1 冻结 +
  12-bit/4:4:4/GBR/bd16/sRGB(13) 枚举扩展 R4.1–R4.3/批 4/H1）；
  帧间载体 `docs/bitstream_spec_v7r3_microgop.md`（V7-R3，em 8）
- 容器规范：`docs/container_spec_v1.md`（MOV **v1.9**：v1 冻结 + tpcB（v1.1）
  + ratio_saturated（v1.3）+ mdat 强制（O3）+ **音频轨（v1.1，ADR-C051）**
  + **tpcD tier 域 0..8（v1.9：+RAW=8）**）
- 可执行示例：`native/topos_codec/examples/encode_decode.c`（约 200 行，
  合成帧 → encode → mux 到 .mov（文件 IO 回调）→ 重新打开 → decode → 校验）
- Python 侧等价示例：`native/topos_codec/examples/topos_roundtrip.py`

### 0.4 容器元数据 tpcD（tier 写入，container_spec v1.9）

`topos_movie_meta`（tier_id + vendor/label）随 mux 写入 stsd 内可选子 atom
`tpcD`；域 **0..8**（v1.9：+ TC_TIER_RAW=8，M4-R5/D3 单 tier）。写侧
`tc_mux_set_movie_meta` 与读侧 `parse_stsd` 镜像同一校验（tier 域检查先于
CRC，越界 = MALFORMED）；Python 镜像 `topos_meta.py`（TIER_ID_MAX=8）。
展示名规则见白皮书 §4.5 命名规则（N-1~N-8）。

## 1. 能力协商与版本

```c
#include "topos_codec.h"

if (tc_abi_version() != TOPOS_CODEC_ABI_VERSION) { /* 拒绝加载 */ }
int32_t rc = tc_query_support(3u /*profile Standard*/, 0u /*YUV422*/, 10u, 2u /*alpha mode2*/);
/* TC_OK = 可解码；TC_ERR_UNSUPPORTED_PROFILE / _PIXEL_FORMAT / _ALPHA_MODE = 不支持 */

topos_version_info v = { 0 };   /* 全 0 初始化即可（lib 写 struct_size） */
tc_version(&v);                  /* v.version_major/minor/patch、git_commit、build_target */
topos_cpu_features cf = { 0 };
tc_query_cpu_features(&cf);      /* TOPOS_CPU_X86_AVX2/FMA/AVX512F、TOPOS_CPU_ARM_NEON */
```

错误处理约定：**返回状态码的函数**使用 int32_t；`tc_frame_packet_bound`
返回 size_t，状态文字与错误详情函数返回库拥有的字符串。失败详情用同线程的
`tc_last_error()`（返回 lib 拥有的字符串，有效期至本线程下一次 lib 调用）。
`tc_status_message(status)` 给任意码（含未知值）一个非空描述。

## 2. 错误码表（值冻结；新增只能追加更负值并升 ABI 版本）

| 码 | 值 | 含义 | 集成方应对 |
| --- | ---: | --- | --- |
| TC_OK | 0 | 成功 | — |
| TC_WARN_CONCEALED | 1 | 坏 slice 已填中性值，帧仍交付 | 检查 `slice_status[]`；可继续用帧 |
| TC_ERR_INVALID_ARGUMENT | -1 | 参数/配置非法 | 编程错误，修调用方 |
| TC_ERR_OUT_OF_MEMORY | -2 | 分配失败 | 释放内存或终止本次任务 |
| TC_ERR_UNSUPPORTED_VERSION | -3 | 位流/ABI 版本不支持 | 拒绝文件 |
| TC_ERR_UNSUPPORTED_PROFILE | -4 | profile 不支持 | 拒绝文件 |
| TC_ERR_UNSUPPORTED_PIXEL_FORMAT | -5 | 像素格式/位深不支持 | 拒绝文件 |
| TC_ERR_UNSUPPORTED_MATRIX | -6 | 量化矩阵 id 不支持 | 拒绝文件 |
| TC_ERR_UNSUPPORTED_ALPHA_MODE | -7 | alpha 模式不支持 | 拒绝文件 |
| TC_ERR_LIMIT_EXCEEDED | -8 | 超硬上限（尺寸/采样数/256MiB 包） | 拒绝文件 |
| TC_ERR_MALFORMED | -9 | 结构非法 | 拒绝文件或该帧 |
| TC_ERR_TRUNCATED | -10 | 截断 | 提示文件不完整 |
| TC_ERR_CHECKSUM_MISMATCH | -11 | CRC 不符 | 该帧按 concealment 处理 |
| TC_ERR_STATE | -12 | 调用顺序/上下文状态错 | 修调用方 |
| TC_ERR_CANCELLED | -13 | 取消（预留给宿主循环） | 终止任务 |
| TC_ERR_IO | -14 | IO 回调失败 | 检查磁盘/权限 |
| TC_ERR_BUFFER_TOO_SMALL | -15 | 调用方缓冲不足（`*need_size` 给精确值） | 重分配重试 |
| TC_ERR_NOT_IMPLEMENTED | -16 | 能力已预留未实现 | 降级路径 |

## 3. 帧编码/解码（elementary stream）

```c
topos_frame_config cfg = { 0 };      /* 全 0 = 最小合法配置，再覆盖字段 */
cfg.struct_size = sizeof cfg;
cfg.visible_width = 1920; cfg.visible_height = 1080;
cfg.qp_base = 28;                     /* 0..63 主码率旋钮 */
cfg.qmatrix_id = 1;                   /* 0=flat / 1=Standard / 2=444 Compact / 3=422 Low Compact */
cfg.alpha_mode = 2; cfg.alpha_bit_depth = 12;   /* 受限近似 alpha（可选） */
cfg.bit_depth = 12;   /* 可选：v1.2 枚举（默认 10）；帧头 minor=1 自动标记，
                         编码 qp_eff += 4 保持同 qp 跨位深码率可比 */

size_t cap = tc_frame_packet_bound(&cfg);        /* 保守上界（分配参考） */
topos_frame_stats st;
rc = tc_frame_encode(&cfg, &input, buf, cap, &st);   /* 两遍校验，确定性输出 */

/* 码控变体：确定性 qp 搜索（≤24 次内部迭代），target 字节预算 */
uint8_t qp_used;
rc = tc_frame_encode_sized(&cfg, &input, target, qp_min, qp_max,
                           &qp_used, buf, cap, &st);

/* 解码：两段式（先查几何再出像素），strides 单位 uint16 元素（0=tight） */
topos_frame_output info;
rc = tc_frame_decode(pkt, size, NULL, NULL, &info);        /* 仅解析 */
uint32_t w, h;
tc_frame_plane_geometry(&info, 0, &w, &h);                  /* 每平面分配 */
rc = tc_frame_decode(pkt, size, planes, strides, &info);    /* 出像素 */
```

统一 request 入口可在不改变旧 `tc_frame_decode` ABI 的前提下选择预览路径：

```c
topos_decode_request req = { 0 };
req.struct_size = sizeof req;
req.abi_version = TOPOS_CODEC_ABI_VERSION;
req.mode = TC_DECODE_MODE_AUTO_2K;       /* 或 REDUCED */
req.scale = TC_DECODE_SCALE_FULL;        /* REDUCED 时改为 HALF/THIRD/QUARTER/EIGHTH */
req.memory_type = TC_DECODE_MEMORY_CPU;

rc = tc_frame_decode_request(pkt, size, &req, NULL, NULL, &info); /* 查询目标几何 */
/* 以 tc_frame_plane_geometry(&info, p, ...) 分配目标平面后再次调用出像素 */
rc = tc_frame_decode_request(pkt, size, &req, planes, strides, &info);
```

旧 V1–V6 packet 在 `AUTO_2K` 下使用固定比例 reduced fallback；调用方应以查询
后的 `info.visible_width/height` 为准，不把 12K→约 1.5K 的 1/8 输出误报成精确 2K。
CLI 等价用法：

```text
topos_decoder_cli --input source.mov --scale auto2k --output preview.raw --report
topos_decoder_cli --input source.mov --scale 1/3 --output third.raw
```

CLI 首先打印 `request`、实际 `path` 和目标几何；`full`、`reduced-1/2`、
`reduced-1/3`、`reduced-1/4`、`reduced-1/8` 均可复核。MOV 的 `bitstream_major`
也会随摘要输出，旧文件不会因读取而被重写。

`topos_inspect` 现在按 packet 真实的 `version_major.minor` 输出，不再把 V2
packet 误显示为 V1.0；`topos_probe_cli` 的文本和 JSON 摘要也会输出 MOV 的
`bitstream_major`。V7-A elementary-frame reader 已发布，工具仍不会伪造默认 MOV
writer 或 V7-B layer/segment 能力，全部以 `tc_query_capabilities()` 返回的
capability bits 为准。

输入平面：planar `uint16` 满刻度码值。**几何按 pixel_format 分派**（复验
P1-19——按旧文档恒 `ceil(w/2)×h` 分配 U/V 会在 4:4:4/GBR 下越界写）：

| pixel_format | plane 0 | plane 1 | plane 2 | plane 3 |
| --- | --- | --- | --- | --- |
| 0（YUV 4:2:2） | Y `w×h` | U `ceil(w/2)×h` | V `ceil(w/2)×h` | A `w×h` |
| 1（YUV 4:4:4） | Y `w×h` | U `w×h` | V `w×h` | A `w×h` |
| 2（GBR） | G `w×h` | B `w×h` | R `w×h` | A `w×h` |

`planes[i]==NULL` 的平面必须 `i ≥ plane_count`（alpha 关闭时 planes[3] 传
NULL）。GBR 平面序为 (G,B,R) 且 `color_matrix=0`（恒等契约，v1.4）。
`tc_frame_plane_geometry` 在解码侧给出同样的每平面尺寸——第三方分配
一律以它为准。

## 4. MOV 容器（回调式 IO，库不做任何文件操作）

```c
topos_io sink = { 0 };
sink.struct_size = sizeof sink; sink.abi_version = TOPOS_CODEC_ABI_VERSION;
sink.ctx = my_file; sink.write = my_append_write; sink.seek_write = my_overwrite_at;

topos_mux* mux;
tc_mux_create(&movie_cfg, &sink, &mux);
tc_mux_add_packet(mux, pkt, size, pts_tick, dur_tick);  /* packet 字段须与 tpcC 一致 */

/* 可选：Alpha 预算元数据（R3；finish 前调用，仅 alpha_mode≠0 电影）。
 * 记录目标/实际比例（×10000 基点）、跨帧最大误差、策略标志与统计帧数；
 * 写入 stsd/tpcC 之后的 tpcB atom（container_spec v1.1 §4）。 */
topos_alpha_budget_info bud = { 0 };
bud.struct_size = sizeof bud; bud.abi_version = TOPOS_CODEC_ABI_VERSION;
bud.target_ratio_bp = 2500; bud.actual_ratio_bp = 2417;
bud.max_abs_error = 9; bud.flags = TC_ALPHA_BUDGET_FLAG_ADAPTED;
bud.frame_count = frames;
tc_mux_set_alpha_budget(mux, &bud);
tc_mux_finish(mux);   /* 失败/成功后都要 tc_mux_free(mux) */

topos_io src = { 0 };  /* 只需 read + length */
topos_movie* mov;
tc_movie_open(&src, &mov);
tc_movie_packet(mov, i, buf, cap, &need);   /* cap 不足 → BUFFER_TOO_SMALL + need */
tc_movie_alpha_budget(mov, &bud);           /* 无 tpcB → TC_ERR_STATE（旧文件兼容） */

```

读回调契约：从绝对 offset 读**恰好** len 字节；短读/失败一律返回 `TC_ERR_IO`。

## 5. 线程契约（R6 修订）

- 帧级 API（encode/decode/sized/bound/validate/query_*）为**纯函数**：可任意多线程
  并发调用，错误为线程局部。
- `topos_mux` 单实例不可重入；`topos_movie` 打开后**只读共享安全**
  （多线程并发 `tc_movie_packet*` 实测 + TSan 干净，见 test_robust_io）；
  `tc_movie_close` 必须由单线程在所有读完成后执行。
- 库内部 slice 级并行（R6 起为**常驻有界线程池**，ADR-C022；M10 起
  **动态领取分发**——共享原子游标逐任务领取，每任务恰好执行一次，
  执行线程不定；结果由调用方按索引汇合，对 slice 大小悬殊的批次消除
  静态条带尾部失衡）：
  - 位流与单线程逐字节一致（1 vs 8 线程 parity 门禁）。
  - 池上限默认 `min(ncpu, 16)`、硬上限 16；`TOPOS_SLICE_THREADS`（1..16）
    或 `tc_dev_set_thread_count()` / Python `ToposCodec.set_slice_threads(n)`
    覆盖。并发调用方共享同一组常驻 worker——总执行线程数
    = 池内 worker + 活跃调用方数（结构性回压，不再随调用方数相乘）。
  - 失败路径（初始化/spawn/OOM/64 outstanding 批次上限）一律顺序执行
    （P1-07 起各退化路径有 telemetry：`tc_dev_tpool_stats_get`）；
    worker 内嵌套 `tc_parallel_for` 退化为串行（防互等死锁）；fork 后
    子进程惰性重建池（pthread_atfork）。
  - **应用调度建议**：前台重负载（导出）设 `min(8, cpu)`；后台任务
    （渲染缓存等）设 1。Topos Color 已内置该策略（timeline_export 满配、
    render_cache_encoder 1 线程）。进程级全局设置在并发编码器间
    「后设先得」（`SliceThreadsPolicy` 登记表，注销恢复基线；P1-05 起
    登记带角色，跨角色接管记 QoS warning）；per-context 线程数需
    ABI 扩展，留待 V2.1。
- `encode_sized` qp 搜索（R6 起为插值快速路径 + legacy 回退，ADR-C022）：
  结果与 legacy 线性搜索**逐字节一致**（14 语料差分门禁 test_r6_perf）；
  `TOPOS_SIZED_SEARCH=linear` / `tc_dev_set_sized_search(1)` 强制 legacy。

## 6. ABI 稳定政策

- `TOPOS_CODEC_ABI_VERSION == 2`：9 个公共结构体布局（未变）、公共符号、
  全部错误码数值已钉死（`tests/unit/test_abi_compat.c` +
  `tests/abi/public_symbols_v1.txt` + 门禁 `nm` 检查 + C++ TU）。
  v2（R3）为纯追加：`topos_alpha_budget_info` +
  `tc_mux_set_alpha_budget`/`tc_movie_alpha_budget`；既有结构偏移不变。
- 所有公共 struct 首字段 `struct_size`：**调用方全 0 初始化即可**；lib 只接受
  0（旧调用方）或精确 sizeof（当前头）。追加字段只能放入尾部 `reserved[]`，
  不得改变既有偏移——任何布局变更必须 +1 ABI 版本并发布迁移说明。
- 头文件可直接用于 C++（extern "C" 包裹；`test_abi_cpp.cpp` 持续编译验证）。

## 7. 构建

```bash
cmake -S native/topos_codec -B build -DCMAKE_BUILD_TYPE=Release -DTOPOS_ENABLE_TESTS=OFF
cmake --build build -j            # → build/topos_codec.{dylib,so,dll}
cmake --install build --prefix /path/to/topos-sdk
```

macOS universal：`-DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"`。
SIMD（AVX2/NEON）运行时分发，库本体保持基线指令集——无需按 CPU 分发多份库。

独立 C/C++ 项目可用 `-DCMAKE_PREFIX_PATH=/path/to/topos-sdk` 配置，然后：

```cmake
find_package(ToposCodec 0.1 CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE ToposCodec::topos_codec)
```

安装目录包含公共头文件、版本化动态库、CMake package config、LICENSE 和 NOTICE。
预编译发布包也包含同名 CMake package config；以发布包根目录作为
`CMAKE_PREFIX_PATH` 即可。发布门禁会运行
`scripts/sdk_consumer_smoke.py <sdk-dist>`，从包外配置、链接和执行最小 C 程序。

Python 独立包由 `scripts/package_opensource.py` 生成。源码轮需由使用方提供
动态库（`TOPOS_CODEC_LIB` 或 `topos_codec/lib/`）；要制作当前平台自包含 wheel：

```bash
python3 native/topos_codec/scripts/package_opensource.py \
  --dest /path/to/topos-codec --include-lib --lib /path/to/topos_codec.dylib
cd /path/to/topos-codec
python3 -m pip wheel .
```

Linux/Windows 分别传入 `.so`/`.dll`。内嵌动态库的 wheel 带平台标记，
应分别在目标平台构建并验证，不能将单平台 wheel 当作通用 wheel 发布。

### 7.5 Windows 快速开始（PowerShell，VS 2022 + CMake 3.21+）

系统要求：Windows 10 x64 及以上；VC 运行库 `VCRUNTIME140.dll`（VC_redist
x64 或随包分发），UCRT 系统内置。解压预编译 SDK 到任意路径（含中文/空格
均可）：

```powershell
# C / C++ 消费者（CMake 会自动定位 Visual Studio）
cmake -S my_consumer -B build -DCMAKE_PREFIX_PATH=C:\path\to\topos-codec-sdk
cmake --build build --config Release
#   CMakeLists.txt 只需两行：
#     find_package(ToposCodec 0.1 CONFIG REQUIRED)
#     target_link_libraries(app PRIVATE ToposCodec::topos_codec)

# Python（平台 wheel，py3-none-win_amd64）
py -3 -m venv .venv; .venv\Scripts\activate
pip install topos_codec-0.1.0-py3-none-win_amd64.whl
python -c "from topos_codec import ToposCodec; print(ToposCodec().abi_version)"

# 冒烟（仅 Python 标准库）
python examples\dll_load_test.py lib\topos_codec.dll
```

Windows 注意事项（2026-09 首轮实机验收结论）：

- **路径编码**：库与 CLI 以系统 ANSI 代码页解释窄字符路径；中文系统
  （CP936）下中文/空格路径实测可用（cmd/PowerShell/资源管理器启动均经
  Unicode 命令行协商）；超出本机代码页覆盖的字符不保证——跨语言环境
  请用 ASCII 路径或经 Python 绑定传入。
- **线程栈**：库内 worker 线程以 stack=0 创建、继承宿主 exe 的 PE 栈
  大小；Windows 默认 1MB 在深流水线（长 GOP/高位深）下可能不足，嵌入方
  建议 8MB 链接（`/STACK:8388608`，对齐 POSIX 默认）。
- **CRT**：发行 DLL 为 `/MD`（依赖 VCRUNTIME140.dll + UCRT）；Debug CRT
  不进入发行包。

## 8. SDK 发布物（`scripts/build_sdk.sh`）

```
topos-codec-sdk-<ver>+<commit>/
  include/topos_codec.h topos_codec_version.h topos_image.h
  lib/topos_codec.0.1.0.dylib + 运行时名称副本    # universal（或 .so / .dll）
  lib/topos_codec.lib                            # Windows DLL 对应导入库
  lib/cmake/ToposCodec/*                         # 预编译包 CMake find_package
  examples/encode_decode.c dll_load_test.py      # 可执行的最小闭环（C / Python ctypes）
  SDK.md BUILDINFO.txt LICENSE NOTICE.txt       # 本文档 / 构建信息 / 完整许可和声明
  docs/bitstream_spec_v1.md container_spec_v1.md capability_manifest.json
  docs/bitstream_spec_v7r3_microgop.md
  docs/image/SDK.md topos_image_file_spec_v0.md capability_manifest.json
  RELEASE-MANIFEST.json                         # R7：机器/编译器/commit/逐文件 sha256
```

`RELEASE-MANIFEST.json` 由 `scripts/package_codec.py` 生成（schema
`topos-codec-release/1`），Linux/Windows CI 发布 job 与本脚本共用同一
实现；`scripts/dll_load_test.py`（stdlib-only ctypes）在打包时对最终
lib 做 ABI + 最小 encode/decode roundtrip 冒烟——加载测试结论随清单留档。
解压发布包后也可手动运行 `python3 examples/dll_load_test.py lib/<实际库文件名>`；
此示例仅用 Python 标准库。高层 Python 绑定示例随独立源码包提供。

macOS 签名/公证：默认 ad-hoc；`TOPOS_SIGN_IDENTITY` + `TOPOS_NOTARY_PROFILE`
环境变量驱动正式签名与 `notarytool` 公证（脚本内已实现，需 Apple 开发者凭据）。

## 9. 已知限制（诚实清单）

- 本库为 **V1 Preview（Intra 4:2:2/4:4:4/GBR 10/12/16-bit）**：已验收 profile
  3（Standard，不限格式）/5（Pro444，4:4:4 10/12）/6（Extreme，4:4:4 12）
  （对外显示名 Topos 422 / Topos 4444 / Topos 4444 XQ）；
  12-bit（R4.1）、4:4:4（R4.2）、GBR（R4.3）与 profile 5/6 激活（R4.4）
  均已交付；Proxy/LT/HQ 三档原生 profile（1/2/4）不实现——应用层以
  profile3 + 质量预设表达（ADR-C011/C018）。
- **音频轨（v1.1 解冻，ADR-C051）**：容器最多支持 16 条音轨（lpcm 无损/AAC 交付档，
  2.0/5.1/7.1 + mono stems v1.6）；编码在应用层（native 只存包），当前宿主导出仍为单母带。
  回放缓存/
  代理仍为纯视频（ADR-C008 边界）。容器能力查询：Python binding
  `TOPOS_MOVIE_CAPS`（audio_tracks 已随 v1.1/ADR-C051 解冻为 True；
  field_order/edit_lists 仍为 False）；宿主导出 preflight 在编码开始前
  以 `topos.video_only` 警告告知（无音频轨的流）。
- **MOV 支持矩阵已冻结（container_spec v1.2 §3.1，R5）**：reader 忽略
  `colr`/`pasp`/`mvhd`/`tkhd`（tpcC 为色彩/SAR 唯一权威源），未知 atom
  四级跳过，多 trak/重复 moov 拒绝；互操作由三方 oracle 验收（独立
  parser + PyAV demux 级 + ffprobe 包级，`tests/interop/interop_oracle.py`）。
- 宿主应用（Topos Color）的时间线 Alpha/HDR 导出链已于 2026-08-30 整改
  R2 闭环（ADR-C013）：RGBA 合成、Alpha 全链不压平、comp clip 显式 OETF。
  已知残留：FFmpeg 编码路径的 BT.2020 矩阵 + Alpha 组合显式不可用
  （R-29，改用 Topos 编码器）。
- **Alpha 预算执行已闭环（R3，ADR-C014）**：宿主 ToposVideoEncoder 逐帧消费
  native 统计，mode2 隐式位深 12→10→8 自适应（文件级定深，首帧探测——
  代表性限制见风险 R-30）、超硬上限三态策略（record/error/continue）、
  tpcB 元数据读写往返；代理链 record-only。库本身的预算策略语义由集成方
  依据 `topos_frame_stats` + `tc_mux_set_alpha_budget` 自行实现。
- 性能门槛 v2（R6，ADR-C022）：原「scalar 单线程 ≤8ms」1080p 解码门槛按
  冻结位流的实测成本模型正式调整——现行门槛与达成状态见
  `benchmark_protocol.md` §6（v2）与 `bench_perf_r6_2026-08-30.md`；
  R6 交付熵符号层 zigzag 融合、sized 迭代削减、常驻线程池与调度回压
  （导出满配 / 后台 1 线程）。1080p 代理剪辑流畅，4K 预览建议走代理。
- Windows/MSVC：代码路径已备（tpool Win32/cpuid/懒初始化），CI 矩阵已
  移除 `continue-on-error`（R7）。**2026-09-28 首轮 Windows 实机验收完成**：
  MSVC 2022 Debug/Release 构建、CTest 93/93（两配置）、PE 符号门、DLL 加载
  roundtrip、CLI 链路、线程扫描（1–24）、scalar/AVX2 逐位对拍、golden 全量、
  SDK 打包与仓库外消费者（含中文+空格路径、目录搬迁）、win_amd64 wheel 独立
  venv 验证（证据见 `docs/windows/`）；正式签名与非 AVX2 真机仍为未完成项
  （发布包以 unsigned Preview 对待）。
- 本机（Apple clang）无 libFuzzer runtime：门禁中的"fuzz"是 sanitizer
  （ASan/UBSan）配置下的**确定性回放**（入库 corpus + 逐前缀截断/翻转矩阵 +
  OOM/IO 故障注入），不是长时间覆盖率引导模糊；Linux CI 上的真 libFuzzer 任务
  在整改 R7 落地。
- `tc_dev_*` 前缀为**内部 dev API**，不入稳定性承诺。

## 10. 迁移说明（v1 → 未来版本）

- v1 码流永久可解码承诺：golden vectors（`tests/conformance/*.bin`，fold 校验）
  与提交入库的 fuzz corpus 在每次门禁全量回放——解码兼容性回归是硬门禁。
- 未来新增 profile（如 V2.1 Micro-GOP IP-2/IP-4）将通过 `tc_query_support` 的
  profile 参数暴露，不改变既有字段语义；老二进制遇到新 profile 得到
  `TC_ERR_UNSUPPORTED_PROFILE`（而非崩溃或误读）。
- 结构体演进只允许消费 `reserved[]`；调用方应始终全 0 初始化并传精确
  `struct_size = sizeof(你的头文件里的类型)`，即可跨版本二进制兼容。
