# 解码提速第一阶段：V3 快路与分项计时

日期：2026-09-07 更新。状态：已实现第一批低风险改造、V3 worker-slot scratch、D1 源层审计、D2 首轮坐标路径优化、D3 context worker 预算、E1 编码热点画像，并完成 C1 独立实验位流首轮对拍；默认 VLC 未替换。

## 本阶段完成内容

1. V3 intra 解码复用 decoder context 的 worker-slot DC 行池，并新增 grow-only 的 top/bottom 重建边界池；稳定几何下不再为每个 slice 分配这两类 scratch。无 context、池不可用或尺寸扩容失败时仍保留有界的 slice 临时分配回退。
2. V3 重建复用帧入口解析的 `tc_dequant_inverse_fn`。x86 构建因此可以走现有 AVX2 融合反量化/逆变换，scalar 仍保留为兜底和差分 oracle。
3. V3 使用已有的 DC-only 闭式重建；残差为常量时只生成一个残差值，再与方向预测逐像素合成，保持当前整数舍入、钳位和边界更新语义。
4. V3 profile 按固定 1/64 块抽样分别记录熵读取和预测/反量化/逆变换/写回时间。slice 墙钟仍用于闭合未抽样的小项，避免把重建时间错误地全部记到 `entropy_ns`。

涉及文件：

- `native/topos_codec/src/codec/intra.c`
- `native/topos_codec/src/codec/codec.c`
- `native/topos_codec/src/codec/codec.h`
- `native/topos_codec/src/codec/decoder_ctx.c`
- `native/topos_codec/src/codec/intra.h`
- `native/topos_codec/src/bitstream/slice_codec.c`
- `native/topos_codec/src/codec/color_store.h`
- `tools/bench_decode_stages.py`（更新 profile 口径说明）

## 正确性验证

- Release 构建：通过。
- Release CTest：`60/60` 通过，包含 V3 golden、V3 fuzz 回放、slice 截断、OOM、批量/context、SIMD 差分和 MOV 链路。
- ASan/UBSan 构建与全量 CTest：`60/60` 通过（包括重型 `fuzz_encode_replay`，总计约 403 秒）。
- 本次 scratch 改动后的 ASan/UBSan 重点回归：`6/6` 通过（`unit_codec`、`unit_intra`、`unit_stress`、`unit_m10`、`conformance_codec_intra`、`fuzz_intra_replay`，约 78 秒）。
- V3 context scratch 稳态门：预热后重复 32 次解码，codec 分配计数为 `0`；Release/ASan `unit_intra` 均通过。
- Python 导出与 native binding：`130 passed`。测试必须显式指向本次构建的动态库；若误加载旧的默认库，V3 测试会把 `reserved[0]=3` 报成非法，这是库路径不一致而非当前实现结果。
- D3 worker budget/context setter 与 ToposMediaSource 回归：`49 passed`（含预算公平重分配、原子 setter、两个 live source、绑定层和源层测试）。
- `git diff --check`：通过。

V3 输出包字节数保持不变；已有 V3 golden 与逐像素对照没有改动。新增快路只替换解码重建实现，不能改变输入包的量化系数或模式语义。

## 初步速度观察

基准：Intel i9-9900K，macOS，Release，4 slice worker，Python wrapper + 无状态完整帧 API；`real25.raw` 和 `real125.raw` 各一帧，重复 5 次取中位数。scratch 改动后的结果经过 `bench_manifest.v1` 校验，原始 JSON 为本地生成的 `build/intra-bench/after_scratch_r5_manifest.json`（工作树 dirty 已显式标记）。这不是达芬奇结果，也不是稳定的产品端到端播放结果。

| 输入 / 档位 | V3 编码变化 | V3 解码变化 | 包字节数 |
| --- | ---: | ---: | ---: |
| real25 / low | 65.39 ms | 11.80 ms | 870,509 |
| real25 / medium | 65.40 ms | 9.82 ms | 965,053 |
| real25 / high | 77.02 ms | 14.96 ms | 1,122,798 |
| real25 / ultra | 86.06 ms | 17.27 ms | 1,689,597 |
| real125 / low | 58.73 ms | 7.73 ms | 499,287 |
| real125 / medium | 61.22 ms | 8.39 ms | 604,230 |
| real125 / high | 62.88 ms | 12.07 ms | 764,537 |
| real125 / ultra | 88.89 ms | 13.68 ms | 1,106,189 |

旧阶段的同配置 V3 编码约 58.6～75.9 ms/frame、解码约 8.5～14.1 ms/frame；两次运行的缓存、调度和系统负载不可完全锁定，当前结果只能说明快路已保持包大小不变，并有内容相关的变化，不能宣称稳定提速。下一步要用 profile 和更大矩阵定位离群值。

## 当前 profile 冒烟

在 `TOPOS_CODEC_PROFILE=1` 下：

- 现有 1920×1080 V2 Topos 片段：wall `3.58 ms/frame`，CPU 累计 entropy `6.71 ms/frame`、dequant+IDCT `16.33 ms/frame`，说明 worker CPU 累计不能直接当帧墙钟。
- 256×256 V3 垂直条纹冒烟：wall `0.14 ms/frame`，抽样闭合后 entropy `0.32 ms/frame`、dequant+IDCT `0.04 ms/frame`。该文件只有一个样本，重复读取只验证计时字段和池化路径可用，不作性能结论。

下一次正式 benchmark 要求至少 12 个独立场景、每段 300 帧，并记录 core、取帧和完整播放三个层级。profile 开启时用于找热点，正式速度门关闭 profile。

## D1 播放源层审计

新增 `tools/bench_topos_playback_path.py`，把实际 `ToposMediaSource` 路径单独测量到 CPU planar 帧交付边界，并把 `decode_path`、read-ahead 深度、平面池容量、输入 SHA 和动态库 manifest 写入结果。它不包含 GPU 上传、调色、合成、present 或 Qt 调度，因此不能代替完整播放结论。

在 Intel i9-9900K、Release、4 个 slice worker、允许 dirty 的开发快照上，2K 422 默认路径为 `context`、read-ahead `0`，120 帧顺序读的三轮结果为 p50 `5.08～5.10 ms`、p95 `6.51～6.97 ms`、p99 `7.21～8.03 ms`，吞吐约 `192～194 fps`；16 次固定随机 seek 的 p50 `4.48 ms`、p95 `4.92 ms`。4K 422 默认路径同样为 `context`、read-ahead `0`，16 帧两轮为 p50 `16.62～17.48 ms`、p95 `17.87～19.63 ms`、吞吐约 `56.8～59.2 fps`；随机 seek p95 `21.54 ms`。

此前自动同步批量在 2K 使用深度 8、4K 使用深度 3；同一脚本观察到首个批量显示帧的 p95 约 `38～57 ms`。显式深度 2 可把 4K p95 降到约 `33～37 ms`，但仍高于逐帧 context 路径的尾延迟。基于这组证据，默认播放策略改为关闭同步 read-ahead，`TOPOS_READAHEAD` 或构造参数仍可显式开启实验路径；包体积与像素语义不变。

## D2 V2 默认 CPU 路径首轮

profile 显示 V2 2K 片段的 dequant+IDCT/重建存储约占累计 CPU 的 72%～75%，因此先检查热路径中可证明的重复工作。`tc_color_scan_to_plane` 的扫描循环已经持有 `bx/by`，但原来的 `tc_color_store_block` 和 `tc_color_store_pixels` 仍从线性 block index 重算 `% cols`、`/ cols`。新增带显式坐标的 `tc_color_store_block_xy`、`tc_color_store_pixels_xy` 及稀疏对应入口；生产 V2 direct-scan 和稀疏实验路径直接传入已有坐标，generic sink/oracle 保持原包装入口，像素与错误语义不变。

在同一 1920×1080 V2 片段、Release、16 个 slice worker、profile 开启的探索性 A/B 中，关闭坐标快路的三轮 wall 为 `3.83/4.06/3.94 ms/frame`，开启后的三轮为 `3.66/3.96/3.60 ms/frame`；中位数约 `3.94→3.66 ms/frame`。累计 dequant+IDCT 抽样估计中位数约 `20.22→18.51 ms/frame`。这是同机短样本，未作为跨素材速度承诺；4K 当前快路观测为 wall `9.51 ms/frame`、dequant+IDCT `92.10 ms/frame`（32 帧），仍需完成配对矩阵。一次“逆变换直接回调写 plane”的实验在相同口径下出现明显回退，已撤回，避免以局部内存操作换取额外回调开销。

D2 的正确性门已通过：Release 重点 CTest `15/15`（含 transform、slice、codec、MOV、V3 conformance、fuzz）通过，坐标入口由 direct-scan 与标量参考逐像素对拍覆盖。完整 60 项和 sanitizer 矩阵仍按提交前检查执行。

## C0 冻结 token 符号预算

现有 M9 统计钩子此前只有全局 DC/RUN/LEVEL 类别直方图；本阶段增加按颜色平面（Y/U/V）的同源累计接口 `tc_dev_symbol_hist_plane_get`，并新增 `native/topos_codec/tools/bench_symbol_budget.py`。工具对固定量化系数同时记录 token 数、DC、AC 对、EOB、每平面分布、当前 packet 的颜色 payload/header 和现有码表的 pooled single-book 位数估计；它不修改量化、码表或默认策略。

当前小矩阵（4 类 640×360 合成素材 + 2K ProRes 取样，2 帧/配置，QP 24/48，V1 Rice 与 V2 VLC 各一轮，共 20 个 run）显示：V1/V2 的 DC、AC 对和 EOB 数逐项相同，差异只来自熵表示。2K ProRes QP24 的 V1/V2 packet 为 `8,207,837→6,785,022 B`，QP48 为 `2,696,070→1,926,384 B`；颜色 payload 分别为 `6,783,998 B` 和 `1,925,360 B`（V2）。QP24 汇总 token 为 `129,600` DC、`5,152,657` AC 对、`129,600` EOB；Y/U/V AC 对分别为 `3,024,733/1,098,663/1,029,261`。这些数字量化了可重表示的工作量，不等于新熵后端的可达收益。

以同一 2K QP24 直方图计算，现有码表的 pooled single-book 位数约 `54.28 Mbit`，分族理想熵约 `19.97 Mbit`；实际编码按 slice 选择码书并另付 slice padding/header，因此两者只用于预算上下文，不能直接宣称可节省 63%。C0 结论是仍有原型空间，但必须把表头、状态初始化、escape、尾部和解码分支计入；下一步先做 C1 常见 `(run, level-category)` 联合 token 原型，再决定是否值得进入多状态 rANS/FSE。

## C1 `(run, level-category)` 联合 token 原型

新增 `tc_dev_symbol_pair_hist_get`，按颜色平面累计联合 pair 直方图；`native/topos_codec/tools/bench_c1_acpair.py` 在不改位流的前提下，用同一 token 流构造 seen-only 和 full-Laplace 两种静态 Huffman 预算。候选保留现有 level 的符号/幅度后缀，只把 run 与 level-category 合并为一个码字，因此每个 AC 对少一个熵码字，DC/EOB 和量化系数完全不变。

2K ProRes 取样（2 帧、QP24/48）中，full-Laplace 估计的完整 packet 约为 QP24 `6,244,681 B`、QP48 `1,821,135 B`，相对当前 V2 VLC 的 `6,785,022 B`、`1,926,384 B` 分别约省 `7.96%`、`5.46%`；seen-only 只多省约 0.1 个百分点，但需要为未观测 pair 设计 escape。每帧减少的熵码字数等于 AC 对数：该片段 QP24 为 `5,152,657`，QP48 为 `2,093,076`。估计已计入 1 byte/码长表项、当前非 payload 开销和观测到的 padding gap，仍未计入真正 bitstream 的截断语义和 decoder cycles。

C1 实验位流已落地为 major=4 / entropy_mode=2 / codebook_version=2。颜色 slice 的 DC 和 escape level 仍使用冻结 VLC；AC 常见 `(run<8, level-category≤7)` 用 6-bit 联合 token，62 为 escape（后接 6-bit run + LEVEL_CAT），63 为 EOB；未知 token、level=0、越界位置、截断和尾随字节均拒绝。V1/V2 流路径没有改写。

在固定 640×360 texture、QP24、3 帧、Release、公开 `ToposCodec.decode` 路径的首轮 A/B 中，V1/V2/C1 平均 packet 分别为 `1,023,484/1,000,024/1,734,484 B`；C1 相对 V2 反而增大约 `73.4%`。平均解码为 `2.610/2.066/3.018 ms/frame`，按 7,200 块折算为 `362/287/419 ns/block`；C1 相对 V2 也慢约 `46%`。这组结果说明固定 token 语法没有通过“文件更小且解码不慢”的价值门，不能进入默认链。原始结果保存在 `native/topos_codec/tools/c1_bitstream_2026-09-07.json`，探针为 `native/topos_codec/tools/bench_c1_bitstream.py`。

C1 结论：正确性和错误语义门通过，压缩率/解码速度门失败，因此保留为可对拍的实验版本。

## C2 per-slice canonical table 实验

C2 使用 major=5 / entropy_mode=3 / codebook_version=3。每个颜色 slice 在
AC 数据前写 64-byte table（58 个活动 pair 符号 + 6 个保留零），由该 slice
量化系数统计构建 canonical book；DC/LEVEL 仍复用冻结 VLC，escape/EOB 和
所有域/截断检查保持精确。C2 的块级 roundtrip、整帧 CRC、自检和持久 context
直写都已覆盖。

同一固定 640×360 texture、QP24、Release、3 帧、公开 binding 解码路径中，
V2 平均 packet `1,000,024 B`，C2 平均 `1,333,837 B`（约大 `33.4%`）；C2
平均解码 `2.875 ms/frame`，V2 为 `1.859 ms/frame`（约慢 `54.7%`）。C2 的
table 初始化和当前 pair 字母表没有通过“文件更小且解码不慢”门，因此不进入
默认链；原始结果保存在 `native/topos_codec/tools/c2_bitstream_2026-09-07.json`
（探针脚本版本 2）。C2 结论是停止当前字母表设计，后续只有重做活动 pair
集合或切换 rANS/FSE 并通过同一门槛才继续。

## D1/D2 真实素材播放与阶段画像复测

在提交 `af513a25`、Release 动态库 `ec9c603f…`、4 个有效 slice worker、read-ahead
关闭的条件下，`ToposMediaSource` 已完成同一口径的 2K/4K 复测：

| 输入 | 顺序 p50 / p95 | 稳态吞吐 | 随机 seek p50 / p95 |
| --- | ---: | ---: | ---: |
| 2K Topos，32 帧，2 轮 | `5.04–5.18 / 5.33–5.57 ms` | `191–198 fps` | `4.94 / 5.41 ms` |
| 4K Topos，16 帧，2 轮 | `16.79–17.04 / 17.56–19.43 ms` | `57–59 fps` | `18.32 / 32.84 ms` |

两种分辨率都走 `decode_path=context`，平面池为 5，单 context 获得 4 个 worker，
没有 oversubscribed。4K 顺序路径暖态 p95 低于 20 ms，但这仍只覆盖 CPU 平面交付，
不包含 GPU 上传、调色、合成和 present。原始 2K/4K 结果分别归档在
`bench_out/d1_playback/af513a25/ec9c603f/d0002963/20260907T034655477642Z/result.json`
和 `bench_out/d1_playback/af513a25/ec9c603f/87822d33/20260907T034629540340Z/result.json`；
该目录按仓库规则保留为本机基准产物。

同一 release 下 `bench_decode_stages.py --frames 30` 的 native 阶段画像为：2K
墙钟 `3.87 ms/frame`、dequant+IDCT 累计 CPU `19.16 ms/frame`（阶段 CPU 的 72%）；
4K 墙钟 `9.75 ms/frame`、dequant+IDCT `92.23 ms/frame`（77%）。阶段 CPU 值按
slice worker 累加，不能直接当端到端延迟；它足以说明下一笔内核优化应优先验证
dequant/IDCT 的 SIMD 融合、缓存访问和平台分发，而不是继续扩大当前 C1/C2 字母表。

## D3 context worker 预算与并发调度

新增 `src/shared/codec/decode_worker_budget.py` 作为进程内协作式预算登记表。`ToposMediaSource` 和 `ParallelSequentialDecoder` 在创建 context 时登记 playback/export 角色、请求上限和 slice pool 容量；多个 context 同时存在时按至少 1 worker、剩余 worker 轮询分配，并通过 `tc_decoder_set_max_slice_workers` 原子更新已存在 context。正在执行的帧使用开始时采样的预算，下一帧采用新预算；不会重建 context，也不改变像素语义。`ToposMediaSource.diagnostics` 保存 active、assigned_total、capacity、oversubscribed 和每个 lease，未接入调度器的路径不会被隐瞒。

先做单路 2K 32 帧、3 轮短样本（Release，进程 slice pool=4）：`decode_workers=0/2/4` 的 p50 约为 `7.50/12.01/7.62 ms`，p95 约为 `9.52/16.96/9.51 ms`；单路 2 worker 在本机明显落后，因此默认保持完整 pool。并行导出 2 路时，`bench_decode_worker_budget.py` 观察到 playback 2 + export 1 + 1，总分配 `4/4`，播放 p50 `10.37 ms`、p95 `14.07 ms`、p99 `14.61 ms`、约 `91 fps`。这些样本只证明总预算和池饱和状态可观测，不等同于完整 GPU 播放掉帧结论；结果写入 `bench_out/d3_budget/`。

D3 决策：单 context 仍可获得当前进程 pool 的全部预算，并发 context 自动公平收敛到总预算；不把 2 worker 限额设成默认。后续完整播放 trace 必须同时报告这份预算快照。

## E1 编码端热点画像

新增 `ToposCodec.encode_stage_stats_reset/get` 类型化绑定和 `tools/bench_encode_stages.py`。工具把 `ToposVideoEncoder` 的输入准备、native 调用、mux 写入和总墙钟分开，并同时记录 native 的 fill、entropy、CRC、copy、assemble、sized probe 计数和闭合字段；结果写入 `bench_out/e1_encode/`，机器相关输出不纳入源代码提交。

在同一 2K Topos 片段、Release、4 个 slice worker、固定 QP24、warmup 3 + 测量 9 帧的首轮画像中：总墙钟 `18.19 ms/frame`，Python 输入准备 `0.054 ms`，native 调用 `15.514 ms`，mux `2.278 ms`，其它 `0.342 ms`。native worker CPU 累计为 fill `13.70 ms`、entropy `25.00 ms`、CRC `2.04 ms`；这些累计值可超过墙钟，因为 slice worker 并行。packed BGR24 互补探针观察到转换 `1.4 ms/frame`、native `8.2 ms/frame`、mux `0.63 ms/frame`。

E1 决策：当前产品路径没有测出可单独删除的 Python 全平面拷贝或重复 mux 分配；主要热点是量化/填充和熵 token 编码。默认路径先保持不变，把编码端的下一笔投入与 C1 联合 token 实验合并验收，避免局部微优化掩盖整体语法收益。

## Q0 软件对照首轮

在 Release 动态库 `b0907366` 上运行 `tools/bench_topos_vs_dnxhr_prores.py`，完成
2K（1920×1080，8 帧，2 轮）和 4K（3840×2160，4 帧，1 轮）FFmpeg 软件参考。
这组数据不代表达芬奇内部解码后端，也不含 GPU 上传、调色、合成和 present。

2K e2e 编码为 Topos `11.1 ms/frame`、ProRes 422 HQ `38.7 ms/frame`、DNxHR
HQX `89.4 ms/frame`；Topos 解码 `5.8 ms/frame`，ProRes `18.2 ms/frame`，DNxHR
`19.4 ms/frame`。2K 配对比值 95% CI 分别为 ProRes 编码 `0.254–0.317`、DNxHR
编码 `0.108–0.140`、ProRes 解码 `0.315–0.320`、DNxHR 解码 `0.297–0.301`。

4K 单轮方向性样本的 e2e 编码为 Topos `47.7 ms/frame`、ProRes `241.8 ms/frame`、
DNxHR `662.0 ms/frame`；解码为 Topos `24.4 ms/frame`、ProRes `68.4 ms/frame`、
DNxHR `73.1 ms/frame`。4K 只有一轮，没有 CI，不能据此宣称统计胜负。原始结果：
`bench_out/b0907366/63f18e17/8623acfd/20260907T033859Z/result.json` 和
`bench_out/b0907366/63f18e17/a8cc28fd/20260907T034147Z/result.json`。

Q0 尚未完成的部分是完整 GPU/UI 播放 trace、Resolve 黑盒时间线和质量矩阵；默认
V2 位流和默认编码策略保持不变。

## 正确性与异常输入门复测

当前 commit `ffb05dba` 的 Release、ASan 和 UBSan 构建都完成了完整 native CTest，
结果分别为 `60/60`、`60/60`、`60/60`。ASan/UBSan 的 `fuzz_encode_replay` 已覆盖
V1/V2/V3/C1/C2 配置选择（编码 fuzz 的 entropy 选择范围已扩展到 `reserved[0]=5`），
未发现越界、use-after-free、未定义移位或整数未定义行为。Python Release binding
回归为 `24/24`；这组门只证明当前实现的错误语义和内存安全，不代表 C1/C2 的压缩
候选已经通过大小/速度门。

同一动态库路径下，媒体源、播放帧 trace、路径分类和 YUV 上传 API 的 Python
回归合计 `114/114`；其中 `ToposMediaSource` 相关项全部通过，确认此前的失败是
测试进程误加载旧 SDK 库造成的环境问题。

## 下一步

1. 用 batch、尺寸切换、OOM 和取消场景扩大 worker-slot scratch 回归，并在真实素材上比较稳定几何下的分配计数和 p95/p99。
2. 将 D1 观测接入真实播放 trace，记录 read/pread、CPU 平面就绪、GPU 上传、调色/合成和 present 的 p50/p95/p99，确认瓶颈是否仍在 codec。
3. 扩大 D2 的 2K/4K、10/12-bit、422/444 和尺寸切换配对矩阵；若收益不能稳定复现则回退坐标快路。
4. C1/C2 独立 slice 级实验位流已完成 escape、表初始化、截断对拍、完整文件 A/B 和 ns/block 观测；因体积与解码门失败，保留实验、不替换默认 VLC。只有重做 pair 集合或新的 rANS/FSE 原型通过同一门槛后才继续。
5. 只有在 C0 符号统计证明仍有足够空间后，才原型化新的精确系数表示；该工作与本阶段解码快路分开验收。
