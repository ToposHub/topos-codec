# ADR-C014：R3 Alpha 预算、统计与元数据闭环

- 日期：2026-08-30
- 状态：已接受（实施完成）
- 上下文：`docs/Topos_V2.0_完成度审计与整改计划_2026-08-30.md` §3 P1-3、§4 R3
- 关联：ADR-C008（编码器架构）、ADR-C012 C-101（alpha 16-bit 容器标度）、
  container_spec_v1.md（v1.1 扩展）、bitstream_spec §8.6/§11.3

## 1. 背景与决策范围

审计 P1-3 指出：`alpha_budget_ratio`/`alpha_hard_cap` 只在 `topos_profiles.py`
声明为 schema，从未被执行——native 已提供 `color_payload_bytes`/
`alpha_payload_bytes`/`alpha_max_abs_error` 逐帧统计，但 Python 应用层
（ToposVideoEncoder 与代理链）编码后直接丢弃；tpcC 也未记录计划 §2.2 要求的
目标比例/实际比例/最大误差。R3 任务：消费统计、mode2 按 profile 尝试
12→10→8、超硬上限三态策略、lossless 绝不降质、向后兼容元数据、预算测试矩阵。

## 2. 决策

### C-110：比例语义 = alpha_payload / color_payload（载荷比）

计划 §2.2 "Alpha 目标占颜色主码流比例"落地为
`alpha_payload_bytes / color_payload_bytes`（不含 slice header 开销，与
spec §11.3 "分别暴露颜色/Alpha 载荷" 对齐；CLI `quality.c` 同口径）。
注意该比例**可 > 1**（噪声 alpha + sized 码控把颜色压到下限时常见 3–6）。

### C-111：mode2 自适应 = 文件级定深，首帧探测（非逐帧切换）

`tc_mux_add_packet` 强制每帧 `alpha_bit_depth == tpcC` 值（container_spec §4
冻结，mov.c 一致性检查），mdat append-only 无法撤销已写帧——因此**逐帧变深
需要容器规范 major bump，明确不做**。实现为：mode2 + 隐式位深（见 C-112）时
mux 延迟到首帧，按 12→10→8 依次试编码（复用生产路径：qp 覆盖或 sized 同
口径），取首个满足档位目标比例的深度固定全文件；全部超目标则取下限 8，
超硬上限交给 C-114 策略。决策是输入的纯函数（spec §11.1 确定性，测试
`test_budget_deterministic_bytes` 字节级验证）。

**限制（已接受，风险 R-30）**：深度由首帧代表。后续帧内容更差时不会重降
（无法重降），只走超限策略；首帧不代表全片的素材应在导出设置显式选低档。

### C-112：位深来源二分——显式声明 = 格式契约，隐式默认 = 质量旋钮

- **显式**（pix_fmt 带 `a{N}` 后缀：planar 直通、代理再编码、a16 无损）：
  位深是调用方声明的格式契约，**超预算不降档**，只记录 + 策略提示；
- **隐式**（裸 `yuv422p10le` + `has_alpha=True`：时间线合成导出）：
  位深是编码器内部质量选择，允许 C-111 自适应。
- mode1（a16 无损）任何来源都不降质（spec §11.3 硬规则）。

这避免了"passthrough a12 静默变成 a10 文件"的静默格式漂移（与审计
R-13 哲学一致），同时时间线路径获得完整自适应能力。

### C-113：平面预量化深度与码流深度分离

探测口径必须是稳态口径：首帧平面按**请求深度**（默认 12）预量化进 16-bit
容器，探测/native 顶层再量化到候选深度；自适应选定后**后续帧仍按请求深度
预量化**（`_alpha_bit_depth` 不变），仅码流配置（tpcC/frame header）使用
选定深度（`_alpha_stream_depth`）。若改为后续帧按新深度预量化，探测测得的
比例与实际生产不符（实测：12-bit 域 ±8 级抖动在 a8 再量化坍缩为单值 →
比例 0.008；直接 a8 预量化则落在量化格边界 → 双值 → 比例 1.87，探测通过
但稳态超限）。预量化保持请求深度同时保证最高精度进 native。

### C-114：超硬上限三态策略（`FFmpegEncoderConfig.alpha_budget_policy`）

| 值 | 行为 | tpcB 标志 |
|---|---|---|
| `record`（默认） | 警告日志 + 照常交付 + 记录超限 | overrun=1 |
| `error` | `RuntimeError` 结构化诊断（比例/累计/cap/target/深度/档位），输出不落盘 | （无输出） |
| `continue` | 用户授权交付（UI 未来接线） | overrun=1, authorized=1 |

逐帧检查（帧比例或累计比例首次越 cap 时触发一次）。mode1 超限文案显式
声明"绝不自动降质"。默认 record 而非 error：中间片交付优先 + 审计要求
"任何超限都有结构化诊断"（警告 + tpcB 记录即满足）；严格工作流可显式
选 error。UI 授权开关属 R7 范围（leftover）。

### C-115：tpcB 元数据 atom（container_spec v1.1 追加，向后兼容）

新增 stsd/TPIC 子 atom `tpcB`（排在 tpcC 之后）：

```
载荷 24B（大端）：'TPCB' | ver=1(u16) | alpha_mode(u8) | alpha_bit_depth(u8)
  | target_ratio_bp(u16) | actual_ratio_bp(u16) | max_abs_error(u16)
  | flags(u16) | frame_count(u32) | CRC32(前 20B)
比例单位基点 ×10000；target 0xFFFF=未声明；actual 可 >10000（u16 饱和 65535）
flags: bit0=overrun bit1=authorized bit2=adapted
```

兼容性依据：旧 reader 的 stsd 子 atom 循环只取 tpcC、跳过未知子 atom
（mov.c 既有行为），FFmpeg oracle 同样跳过——纯追加，无需 major bump；
golden_mov_v1 不受影响（不 set 即不写）。读侧严格校验：长度 24/magic/
version/CRC；**mode/depth 必须与 tpcC 一致、frame_count 必须 == sample 数**
（tpcC 式一致性，违反 → MALFORMED）。FastStart 重建 moov 时原样保留。

### C-116：ABI v2（纯追加）

`TOPOS_CODEC_ABI_VERSION` 1→2：追加 `topos_alpha_budget_info` +
`tc_mux_set_alpha_budget` + `tc_movie_alpha_budget`。既有结构/函数未动
（`test_abi_compat.c` offsetof 断言全部不变）；版本不匹配在 `_verify_abi`
与各镜像检查处显式失败（杜绝旧 dylib 静默混用）。

### C-117：解码侧暴露与代理链闭环

- `ToposMovieFile.alpha_budget() -> dict|None`（无 tpcB → None，旧文件兼容）；
- `ToposMediaSource` open 时读入，逐帧挂 `extra['topos_alpha_budget']`
  （共享 dict 引用，零拷贝）；
- 代理再编码链同样消费 stats 写 tpcB（**record-only**：代理可再生，超限仅
  警告 + 记录，不抛错不降档——位深沿用源声明 = C-112 格式契约）。

## 3. 备选方案与否决理由

- **逐帧变深（tpcC 豁免 alpha_bit_depth）**：违反 container_spec §4 冻结
  （"tpcC 与每帧 packet header 的重复字段必须一致"），需 major bump +
  golden 重冻结 + 解码侧逐帧 shift 语义改造——收益（单帧粒度预算）不值得
  破坏容器不变量。否决。
- **tpcC version 2（塞进 reserved 2B）**：reserved 仅 2B 装不下
  target/actual/max_err/flags/frames；且旧 reader 对 v2 是"干净拒绝"而非
  忽略——比新 atom 更差。否决。
- **探测窗口（前 N 帧）**：需要缓冲 N 帧或延迟 mux 更久，内存/复杂度换
  代表性提升有限；首帧 + C-114 策略兜底已满足审计门槛。否决（R-30 记录）。
- **预算驱动 sized 目标调整（target/(1+ratio) 分配给颜色）**：属于码控
  重设计，超出 R3 范围（审计未要求）；现有行为（sized 目标含 alpha，
  qp 搜索自然挤压颜色）保持不变，R4/R6 再评估。

## 4. 验证

- 失败先行：Python 13 用例失败（无 tpcB 可读/TypeError/无数值断言成立）+
  native 编译失败（unknown type）确认于实现前；
- `TestR3AlphaBudget` 14 用例通过（四档元数据往返 / 12→8 自适应 + 8-bit
  域误差 ≤2 / error 结构化报错不落盘 / record 交付+标志+警告 / continue
  授权 / mode1 无损 bit-exact + 显式提示 + error 报错 / 显式位深锁定 /
  常量+硬边零误差 / 字节级确定性 / 代理链 tpcB）；
- 内容参数实测校准（64×40，sized 码控）：smooth ≈0.008（全档位安全）、
  12-bit 域 ±8 级抖动 a12≈2.75/a10≈2.56/a8≈0.008、全随机 a8≈5.6；
  实测 maxerr a10≤32、a8≤192 < spec 界 63/255；
- native：test_mov tpcB 六段（默认不写+STATE / 往返 / frame_count 不一致
  MALFORMED / CRC 破坏 / mode 不一致（CRC 修复后）MALFORMED / finish 后
  STATE + 无 alpha 拒绝）；ctest 35/35（除长跑 fuzz 单独通过）；
- 全量门禁 `run_tests.sh`（含 golden_mov_v1 字节不变、ABI offsetof、
  fuzz、示例）+ Python 矩阵；
- 广谱回归（media / video+deliver / edit）与基线失败集一致，零新增。

## 5. 实施痕迹

- native：`include/topos_codec.h`（ABI v2 + 结构/常量/声明）、
  `src/mov/mov.c`（mux 预算状态 + build_tpcb + set/get + 解析一致性 +
  faststart 保留）、`tests/unit/test_mov.c`（tpcB 六段）
- Python：`topos_binding.py`（ABI 2 + 镜像 + 方法 + 注册）、
  `video_encoder.py`（`alpha_budget_policy` 字段）、
  `topos_encoder.py`（探测/记账/策略/元数据 + 深度分离）、
  `proxy_generator.py`（record-only 闭环）、`topos_source.py`（extra 暴露）
- 测试：`tests/media/test_topos_export.py::TestR3AlphaBudget`、
  `tests/native/test_topos_binding.py::test_mux_alpha_budget_roundtrip`

## 6. 遗留（非 R3 范围）

- `alpha_budget_policy` 的 UI 授权开关（R7 交付面收口时接线）；
- 预算驱动的 sized 目标分配重设计（若 R4/R6 码控 revisit 再启）;
- R-30（首帧代表性）已登记风险登记册（已接受 + 文档化缓解）。
