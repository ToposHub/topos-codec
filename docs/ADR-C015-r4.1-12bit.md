# ADR-C015：R4.1 —— 12-bit YUV 4:2:2（v1.2 枚举扩展）

- 日期：2026-08-30
- 状态：已接受（实施完成）
- 上下文：`docs/Topos_V2.0_完成度审计与整改计划_2026-08-30.md` §4 R4
  子阶段 1（"12-bit YUV 4:2:2 scalar"；六子阶段严格串行，本 ADR 只覆盖 1）
- 关联：ADR-C002（变换/量化整数界按 12-bit 推导——本轮的可行性根基）、
  ADR-C004（量化表冻结）、bitstream_spec v1.2

## 1. 背景

审计 R4 要求补齐 V2.0 核心格式承诺，顺序为 12-bit 4:2:2 → 444 → GBR →
Pro444/Extreme 档位 → SIMD 差分 → 应用层协商，且"不得一次性修改全部
profile"。子阶段 1 交付 12-bit YUV 4:2:2 全链（native + 绑定 + 应用层）。

关键事实（侦察确认）：变换/量化/SIMD 层自阶段 1–2 起就按 12-bit 界设计
（`|x'| ≤ 2047` → `|F| ≤ 2²⁵`，ADR-C002 C-20；AVX2/NEON 内核同一界推导，
golden_transform 已含 depths{10,12}）——**12-bit 唯一缺的是 codec.c 的
MID/MAX 常量参数化、帧头/query 校验放开与测试面**。

## 2. 决策

### C-118：位深参数化（level shift / 重建钳位 / concealment 中性值）

`TC_BD10_MID/MAX` 编译期常量改为按 `fh->bit_depth` 推导
（`mid = 1<<(bd−1)`、`max = (1<<bd)−1`），作用于编码 level shift、解码
重建钳位（§7.7）与 concealment 中性值（12-bit = 2048）。变换/量化/SIMD
零改动（界已备）。

### C-119：v1.2 minor 版本标记（扩展枚举流 =1，v1.0 语义流恒 =0）

bit_depth=12 属 spec §13.1 "新增枚举值"（minor bump）。实现为**按流标记**：
帧头 `version_minor`（byte 7，内部结构新增镜像字段）在使用扩展枚举
（bd=12 / 未来 pf≠0）的流上写 1，v1.0 语义流恒 0——既有 golden 与产物
逐字节不变。解码规则：minor ≤ 1；minor=0 的流出现扩展枚举 →
`UNSUPPORTED_VERSION`（枚举已知但版本声明不符；旧解码器对 bd=12 仍是
干净的 UNSUPPORTED_PIXEL_FORMAT 拒绝）；minor=1 + v1.0 枚举 = 前向兼容
writer，按 §13.1 完全一致解码。编码侧同一规则（直接构造 fh 忘记置 minor
会被 encode 拒绝，测试覆盖）。

### C-120：12-bit 等效量化偏移 qp_eff += 4（spec A.4 v1.2）

qp_scale 每 4 qp 翻倍；位深 +2 → 系数幅度 ×2 → 同 qp 相对量化精度变细
2×、码率 ≈×2。为保持同 qp 的跨位深可比性（crf 语义/码控定标），编码侧
`qp_eff += 4`（bd=12）。**偏移只加在编码侧**：qp_eff 经 slice
qp_delta_biased 完整入流，解码侧照读，无需位深知识（码流自描述）。
qp>59 时 qp_eff 钳位 63——极端 qp 下等效步长饱和（文档化）。
实测：同图案同 qp 下 size12 ≤ size10×1.6（无偏移 ≈×2）。

### C-121：golden 用独立文件，v1 字节冻结不动

`golden_codec_v1.bin` 的 RECORD_COUNT 编译期绑定 kCases（check 硬校验
count）——扩用例必碎。决策：golden_codec 工具参数化用例集（`bd12` 参数，
新 kCases12 五配置：grain/mixed/detail/gradient/flat × 无 alpha/mode1/
mode2），新文件 `golden_codec_v1_bd12.bin`（10 记录，fold
`1017cb22f67dd52e`）+ 独立 ctest `conformance_codec_bd12`。同理
packet_synth 的 cfg 结构尾部加 bit_depth 字段（kCfgs 位置初始化默认 0→10，
golden_bitstream 56 记录 fold 不变实测验证），fuzz corpus 生成器单独追加
bd12 种子（seed/截断×5/翻转×4，corpus 174→184）。

### C-122：应用层位深协商全链

- `topos_binding.movie_config(bit_depth=10)` 参数化（profile=3/pf=0 固定）；
- `ToposVideoEncoder`：主位深从 pix_fmt 解析（`yuv422p{10,12}le` /
  `topos_yuva422p{10,12}a*`），planar 白名单扩 12-bit 族，RGB→YUV 转换与
  码值域校验按位深定标（12-bit limited Y ∈ [256,3760]），帧头
  profile/pixel_format/bit_depth 显式设置（此前靠 0→默认）；
- `topos_source`：pixel_format 名按位深（`yuv422p12le` /
  `topos_yuva422p12a*`），metadata 与帧两级一致；
- `frame.py` 注册 `yuv422p12le` + 12-bit topos alpha 变体；Edit 页 GPU
  预检 `_YUV422_FORMATS` 集合同步（GPU shader 位深早已参数化，零内核改动）；
- `topos_profiles`：hq 档 `bit_depths=(10,12)`；LABEL 升为
  "Intra 4:2:2 10/12-bit"（与 header/query/SDK 同步的诚实能力声明）。

### C-123：hq 档 12-bit 为显式 opt-in（时间线默认不变）

时间线 codec_map 的 TOPOS_HQ 保持 `yuv422p10le` 默认——12-bit 经导出
配置显式 pix_fmt 选择（`pix_fmt='yuv422p12le'`）。档位 UI 展示与 bpp
重定标属 R4.4（Pro444/Extreme 档位）与 R4.6（UI 协商）。

## 3. 备选与否决

- **新 qmatrix_id=2（表 ×2）替代 qp 偏移**：需扩表 + spec A.3 修订 +
  独立矩阵 golden；qp 偏移零表面变更且数学等价（qp_scale ×2）。否决
  （独立矩阵留给 R4.4 档位差异化时再议）。
- **所有流统一写 minor=1**：旧解码器将拒绝新库产出的全部 10-bit 流
  （语义未变却不可解），破坏 §13.1 精神。否决——按流标记。
- **重生成 golden_codec_v1.bin 并入 12-bit 用例**：破坏 A.8 字节冻结与
  fold 基准。否决（C-121 独立文件）。

## 4. 验证

- 失败先行：实现前 `tc_query_support(3,0,12)` 恒拒——旧
  `test_abi_compat`/`test_stage10_concurrency` 恰好钉死该拒绝（反向证据）；
  帧头旧测试钉死 minor≠0 拒绝与 bd=12 拒绝。新增测试在旧代码上不可能通过。
- native：test_codec 12-bit 段（枚举域 11/13 拒 / 往返+确定性 / 帧头
  minor=1 / maxerr ≤96 / 同 qp 码率 ≤×1.6 / concealment=2048 整带 /
  多代稳定+无色偏）；test_frame_header v1.2 段（正例/负例/编码侧规则）；
  stage9 backend parity 10/12 双轮换；golden v1_bd12 + v1 双 check；
  golden_bitstream 56 记录 fold 不变；corpus selftest + fuzz replay；
  ctest debug 36/36（五配置门禁见 run_tests.sh）。
- Python：TestR4FormatExpansion 6 例（query 契约 / planar 12-bit 往返+
  帧头字节 / packed BGRA→12-bit Y=998±60 / 位深交叉拒绝 / hq 档声明 /
  12-bit+a12 alpha 组合）+ 既有矩阵 119 全绿（含按 v1.2 语义更新的
  R1 位深交叉用例）。
- 完整门禁 + 广谱回归：见提交信息。

## 5. 遗留（后续子阶段）

- R4.2：YUV 4:4:4 10/12-bit（pixel_format=1：几何派生/plane_geometry/
  packet_bound 三处镜像分派 + golden v1_pf444 + 应用白名单）；
- R4.3：GBR 4:4:4（level shift 语义 ADR 先行——GBR 无中心化惯例）；
- R4.4：Pro444/Extreme 档位（available 翻真 + 独立矩阵/码控重定标）；
- R4.5：SIMD 差分已随本轮覆盖 12-bit（内核本就格式无关），444/GBR 时扩用例；
- R4.6：UI 能力协商（hq 12-bit 展示、导出面 pix_fmt 选项）；
- quality CLI 的 12-bit 报表（psnr 满刻度 + bd 选项）随 R4.4 定标一起做。
