# ADR-C017：R4.3 —— GBR 4:4:4 10/12-bit（v1.4 枚举扩展）

- 日期：2026-08-30
- 状态：已接受（实施完成）
- 上下文：`docs/Topos_V2.0_完成度审计与整改计划_2026-08-30.md` §4 R4
  子阶段 3（"10/12-bit GBR 4:4:4"；六子阶段严格串行，本 ADR 只覆盖 3）
- 关联：ADR-C016（R4.2 4:4:4——几何分派与 minor 代际机制的直接先例）、
  bitstream_spec v1.4、container_spec（tpcC byte 7）

## 1. 背景

审计 R4 顺序推进到 GBR。R4.2 已把三处几何镜像改为 `pf==0 ? 半宽 : 全宽` 的
二分派——GBR 几何与 4:4:4 同构（三平面全宽），native 数学层零新增分支。
GBR 的特有面只有**语义契约**：无 YUV 矩阵（identity）、无色度采样位置、
平面序与值域约定。

关键侦察：GPU 预览链已有 `rgb_planar` 上传路由（gbrp 按 G,B,R 序交换后
`upload_yuv444` + 恒等变换上传）——平面序若与 FFmpeg gbrp 对齐，应用层
可零新内核接入。

## 2. 决策

### C-130：平面契约 = FFmpeg gbrp（G, B, R 顺序，满刻度全幅）

`pf=2` 三平面序为 **G, B, R**（plane0=G、plane1=B、plane2=R），值域
0..2^bd−1 满刻度，第 4 平面（如有）为 Alpha。与 FFmpeg `gbrp10le/gbrp12le`
解码输出零重排直通，且直接复用 `_RGB_PLANAR_FORMATS` GPU 路由与
`map_color_space(0)='gbr'` 既有映射。

### C-131：level shift 沿用中点（语义无关的 DC 重定心）

编码 `-2^(bd−1)` / 解码 `+2^(bd−1)` 的中点平移对 GBR **不是**视频级
"limited range 黑位"语义——它只是 DCT 前的 DC 重定心，roundtrip 与值域
无损无涉。concealment 中性值 = mid（G/B/R 各中点 = 中性灰）。
qp 的 bd 偏移（ADR-C015 C-120）沿用。

### C-132：matrix/siting 交叉规则（identity 契约入格式）

- `pf=2` → `color_matrix == 0`（identity），携带 1/5/9 → `MALFORMED`；
- `pf∈{0,1}` → `color_matrix ∈ {1,5,9}`，出现 0 → `MALFORMED`
  （C-28 修订：0 从"保留"转为"GBR 专用"）；
- `pf=2` → `chroma_siting == 0`（无子采样即无采样位置），非 0 → `MALFORMED`；
- `color_range` 不加规则（纯下游转换 tag；应用层恒写 full=1）。

config 层实现：`cfg.matrix=0` 语义是"默认"（公共头零值约定），cfg_to_frame_header
按 pf 取默认（GBR → 0、YUV → 1）——config 层无法给 YUV 显式声明 identity，
交叉规则天然闭合；显式 YUV 矩阵 + pf=2 在 header 校验拒绝。

### C-133：v1.4 minor 标记（pf=2 流 =3；逐扩展代递增先例延续）

minor=3；`minor==2 && pf=2` → `UNSUPPORTED_VERSION`（v1.3 包络不含 pf=2）。
minor 高于枚举所需 = 前向兼容 writer（完全一致解码）；`minor>3` 拒绝。
bd=12 + pf=2 → minor=3（v1.4 包络包含全部前代）。

### C-134：独立 golden + 测试面

`golden_codec_v1_gbr.bin`（12 记录 = 6 配置 × 双记录，fold
`b31795ee8e9f1afc`）：含 alpha mode1/mode2、36×20 padding、qp_delta_chroma、
bd=12+pf=2 组合。image_synth `chroma_format` 判定修正为 `!=0`（R4.2 的
`==1` 在 golden 直通 pf=2 时漏判——曾致半宽分配/全宽读取的未初始化越界，
gen 双跑 fold 漂移当场暴露并修复）；packet_synth `pixel_format=2` 支持；
corpus 追加 seed_gbr 族（194 → 204 文件）。单测：枚举域/交叉规则/
往返+确定性（pkt[7]==3、pkt[11]==2、pkt[35]==0）/全宽几何/concealment
（mid 中性）/多代/test_mov（GBR mux 往返 + matrix=1 配置在 mux_create
即拒）。

### C-135：应用层接线

- encoder：`_GBR_FMT_RE`（`gbra?p\d`）解析 pf=2；白名单加 `gbrp10le/
  gbrp12le` + `topos_gbrap{10,12}a{8,10,12,16}`；packed 路径
  `_rgb_to_gbrp`（通道直通、满刻度、无矩阵/range 折算，G,B,R 序）；
  `_build_configs` 对 pf=2 强制 matrix=0/siting=0/range=full（双写
  movie cfg 与 frame cfg，tpcC 一致性闭合）；planar 几何校验 `pf!=0` 全宽
  （顺带修复 R4.2 遗留的 `==1` 判定镜像）。
- topos_source：命名分支（无 alpha 复用 FFmpeg `gbrp{10,12}le`；alpha
  `topos_gbrap{bd}a{N}`）+ U/V 几何/代理 chroma 目标 `pf!=0`（同上修复）；
  matrix=0 经 `map_color_space` → `'gbr'`（既有映射）。
- frame.py 注册 8 个 `topos_gbrap*`；GPU `_RGB_PLANAR_FORMATS` 追加同名
  （复用 rgb_planar 路由：G,B,R → R,G,B 交换 + 恒等上传，零新内核）。
- binding：`movie_config(pixel_format)` 域放开 {0,1,2}（R4.2 的
  `==1 else 0` 截断曾把 pf=2 塌缩为 0，tpcC 一致性测试当场拦截）。
- tier bpp 不随 pf 重定标（沿用 ADR-C016 C-129，R4.4 统一复核）。

## 3. 验证

- 失败先行：frame_header（minor=3/交叉规则/siting）/ abi_compat /
  stage10_concurrency / unit_mov / unit_codec / conformance_codec_gbr /
  Python TestR43Gbr 全部先行失败，实现后全绿。
- 过程缺陷三枚（均被既有门禁拦截并当日修复）：image_synth chroma_format
  `==1` 漏判（golden gen 双跑 fold 漂移暴露未初始化越界）；packet_synth
  后置 `matrix=1` 覆盖 pf 感知赋值（corpus selftest MALFORMED）；
  encoder GBR 正则 `gbrap?` 写反 + binding pf 截断（Python 套件拦截）。
- native：debug + 三 sanitizer 配置单测、conformance 四 golden（v1/v1_bd12/
  v1_pf444/v1_gbr）、corpus selftest（204 文件）全绿；完整 STAGE-10 门禁
  见 run_tests.sh。
- Python：`TestR43Gbr` 6 测试 + R1–R4.2 既有套件（87 通过含 stage10/
  binding/export 全量）；广域回归 media/video+deliver/edit 与基线集合一致。

## 4. 遗留

- R4.4：Pro444/Extreme 档位（available 置真、独立矩阵/码控、bpp 按 pf
  重定标、quality CLI 12-bit 报表）。
- GPU rgb_planar 路由的 GBR 帧端到端渲染验证归 R4.6（UI 协商轮）；
  本轮仅路由表成员级接线。
- color_range 对 GBR 的有限域折算语义（若未来引入 limited RGB 源）：
  当前应用层恒 full，无需求。
