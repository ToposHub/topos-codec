# ADR-C005：阶段 5 MOV 封装与 CLI —— 路线决策与实现记录

- 日期：2026-08-29
- 状态：已接受（阶段 5 交付；容器路线部分被取代）
- superseded-by（路线部分）: ADR-C011-r0-scope-freeze.md（自研最小 MOV
  为 v1 正式路线——本 ADR 的 libavformat 侧结论降级为 oracle 用途）
- 关联：`container_spec_v1.md`（规范性）、`bitstream_spec_v1.md` v1.1、
  风险登记 R-20、计划 §12 阶段 5
- 前置：ADR-C004（阶段 4 完整标量 Codec）

## 1. 背景

阶段 5 需要：成熟容器库 mux/demux、TPIC sample entry + tpcC、标准采样表、
co64、FastStart、颜色/帧率/SAR/Alpha 元数据、四个 CLI、损坏注入验证。
R-20 明确指出：**libavformat 私有 FourCC `TPIC` 写出/读回未验证**，首任务
为 spike；失败回退为「受 gate 控制的最小 MOV writer」。

## 2. Spike 证据（spikes/spike_movenc.c，FFmpeg 8.1 / libavformat 62.12）

| # | 验证点 | 结果 |
| --- | --- | --- |
| S1 | movenc 接受 `AV_CODEC_ID_NONE` + codec_tag `TPIC` | ✅ 写出成功 |
| S2 | 读回 packet 逐字节一致 + 恒定步进 PTS | ✅（timescale 被 movenc 提升为 12288，pts 须按 write_header 后的 time_base 换算） |
| S3 | ffprobe/ffmpeg CLI 对未知 codec | ✅ 安全跳过（`codec_name=unknown, codec_tag_string=TPIC`），无崩溃 |
| S4 | `+faststart` 与普通布局 | ✅ 均可打开 |
| S5 | 自定义元数据 | ✅ `use_metadata_tags` 写读（字符串，经 udta/meta） |
| S6 | **二进制 tpcC 进 sample entry 的公共 API** | ❌ 不存在。`extradata` 会以 `glbl` atom 原样写入并读回；改成 `tpcC` 名需手工注入 + 递归 patch 全部父 size + stco 偏移（spike 注入脚本 40 行出 2 个 bug，文件经 ffmpeg 8.1 验证可接受） |

## 3. 决策

### C-38 主路线 = 自研最小 MOV writer/reader；FFmpeg/PyAV 降级为外部 oracle

R-20 的「回退」升级为「主路线」，依据：

1. **规范要求的 `tpcC` sample entry 无公共 API**（S6）；绕开它需要自写全结构
   重写器——工作量已接近自研 writer，且脆弱性远高（见 spike 注入实验）。
2. **字节确定性不可得**：libavformat 输出随版本漂移（本机 brew 62.12 vs
   PyAV 62.3），与 golden 字节冻结方法论（spec §13.3 / container §8）冲突。
3. **门禁密封性**：四配置 gate（Debug/ASan/UBSan/Fuzz）+ Windows 构建会
   引入 FFmpeg dev 库链接依赖；自研保持零依赖 C11。
4. **指标可测**：索引内存 / hot-cold 延迟（阶段 5 验收门槛）必须测自有实现。
5. **阶段 10 漏洞挖掘**需要自有 demuxer 才能纳入 fuzz 门。
6. 生态兼容不丢失：ffprobe/ffmpeg CLI（run_tests.sh oracle 步骤）与 PyAV
   （Python 套件）持续交叉验收「非 Topos reader 安全」。

### C-39 布局与 FastStart

- 标准：`ftyp(20) + mdat(64 位头 16B) + moov`，流式写出，sink 需
  `seek_write` 回填 mdat 长度（写 moov 前记录 payload 终点）。
- FastStart：独立后处理 `tc_movie_faststart`——按索引**重建** moov（stts RLE、
  stco→co64 溢出自动升级，两遍收敛），非字节搬移。幂等（已 faststart 直接 OK）。

### C-40 确定性冻结

- mvhd/tkhd/mdhd 时间字段全 0；atom 顺序/字段值全部固定（container §8）。
- `golden_mov_v1.bin`：每用例双记录（文件字节 + 索引摘要），10 条记录，
  fold `f59e6aaf8202099f`。

### C-41 tpcC：二进制轨级配置

- 28B：`TPCC` + version + 配置镜像 + CRC32（container §4）。
- 与每帧 packet header 一致性在 `tc_mux_add_packet` 强制校验；
  **qp 系字段豁免**（逐帧可变，tpcC 存电影标称值）。
- colr(nclc)/pasp 兼容性子 atom 照常写入（第三方 reader 友好）。

### C-42 reader 防御规则（container §7）

- atom 边界检查逐步 64 位；采样表（stts 展开 = stsz 计数 = stsc×stco 展开）
  **一致性核对通过后才分配**索引；sample 上限 2^26。
- **§7.2a 截断 mdat 收敛**：声明长度越过文件尾 → 按实际剩余长度继续
  （moov 完整即可索引；与 ffmpeg 行为一致）；其余 atom 越界 → MALFORMED。
- 单 sample 区间越界只失败该 sample（容器级帧隔离）。

### C-43 io 回调 ABI

`topos_io`（read/write/seek_write 回调 + length）——库不做 I/O；fuzz 以内存
io 注入，CLI 以 stdio 实现，Python 绑定以 os.pread 实现（阶段 6 接入通道）。

## 4. 实现期缺陷记录（全部已修 + 测试固化）

| # | 缺陷 | 后果 | 修复 |
| --- | --- | --- | --- |
| D-1 | mdat 长度回填写到偏移 20（size 字段）而非 28（u64 扩展字段） | mdat type 被覆写 | 修偏移；`test_co64_and_layout_rules` 校验头布局 |
| D-2 | mdat size 公式把 finish 追加的 moov 计入 payload | mdat 吞掉 moov | 写 moov 前记录 payload 终点 |
| D-3 | ftyp 常量把 brand 当 atom type（漏 `ftyp` 字段） | reader 找不到 ftyp | 常量修正；测试校验 `[4..8)=='ftyp'` |
| D-4 | tkhd 保留区写 32B（标准 8B） | 结构非法（116B≠92B） | 修 2×u32；ffmpeg 复验 |
| D-5 | moov 子 atom walk 从 moov 头部而非 body 起 | 整棵树解析不到 | `moov_body` 起点 |
| D-6 | TPIC entry 子 atom 从 body+70 起而非 +78 | tpcC 永远找不到 | 固定字段 78B 对齐 |
| D-7 | **mdat 64 位头按 12B 数组分配，写 16B** | 数组越界 + 回填覆盖首帧前 4 字节 | `MOV_MDAT_HDR=16` |
| D-8 | encoder CLI 把 strides 传成**字节**（ABI 契约为 uint16 **元素**） | 每行越界读未初始化堆 → ASLR 相关输出不确定性（三跑三样） | CLI 修正 + **API 边界固化校验**：`stride 非 0 时必须 ≥ plane visible 宽`（codec.c） |

D-8 的教训：文档契约不足以防止单位类误用，必须在 API 边界拒绝。

## 5. 验收门槛对照（计划 §12 阶段 5）

| 门槛 | 证据 |
| --- | --- |
| 随机读取任意帧得到正确 index/PTS | `test_mov` verify_movie（全帧字节+pts+dur+sync）；`topos_quality mov` hot 随机探测 |
| 长文件索引内存可测量 | 24.13 B/帧（`tc_movie_info.index_bytes`，`topos_quality mov`） |
| hot lookup 与 cold first-frame 分开报告 | cold open 0.013 ms / cold 首帧 0.35 ms / hot 0.3 ms/帧（同报告） |
| FastStart 与非 FastStart 均可打开 | `test_mov::test_faststart` + ffprobe oracle（run_tests.sh） |
| 非 Topos reader 安全跳过 | ffprobe `codec_name=unknown` 优雅处理；ffmpeg `-f null` 无崩溃；外来 entry（ap4h）→ 明确 MALFORMED |
| 损坏 atom/错误 offset/截断 mdat | `test_mov::test_corruption_isolated`（坏 size/tpcC CRC/截断隔离）+ fuzz_mov 回放门禁 |

## 6. 交付物

- 库：`src/mov/mov.c`（mux/demux/faststart，~1100 行）+ `include/topos_codec.h`
  容器段（topos_io / topos_movie_config / topos_movie_info / 10 个函数）
- CLI：`topos_encoder_cli`（含 --faststart/--target-mb）、`topos_decoder_cli`
  （含 conceal 计数）、`topos_probe_cli`（JSON/--verify）、`topos_rawgen`
- 测试：`test_mov`（9 用例组）、`golden_mov_v1.bin`、`fuzz_mov`（回放 +
  libFuzzer）、CLI 链路冒烟（run_tests.sh）、ffprobe oracle 步骤
- 绑定：`ToposMovieFile` / `ToposMuxFile` / `faststart_file` + 3 个 pytest
- 指标：`topos_quality mov`
- 规范：`docs/container_spec_v1.md`（v1 冻结）

## 7. 遗留

- co64 实文件验证：>4GB mux 需阶段 9 性能窗口顺带（fixture 已由 golden 的
  force_co64 路径与 faststart 升级逻辑单测覆盖；实尺寸留待真实长片）。
- udta 自定义元数据（编辑器工程信息）未启用——packet 自包含原则下无刚需，
  留 v1.1 候选。
