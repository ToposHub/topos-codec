# ADR-C012 — R1 数据正确性修复（编码边界六项 + alpha 标度语义）

> 状态：已采纳（R1 交付，2026-08-30）
> 输入：审计 §3 P0-2/P0-3/P1-1/P1-4 + R1 任务清单；测试先行（修复前
> `TestR1DataCorrectness` 10 failed / 2 passed，修复后全绿）。

## 1. 背景

审计在 ToposVideoEncoder 边界确认四类静默数据错误，本轮侦察又在代理链发现
两个同类问题，加上测试先行过程中暴露的 alpha 标度错位，共七项：

| # | 缺陷 | 位置 |
| --- | --- | --- |
| P0-2 | packed 通用 `[..., ::-1]`：BGRA→ARGB，颜色与 Alpha 同时错（复现：纯红 Y=877 而非 250） | `topos_encoder._prepare_packed` |
| P0-3 | `alpha_premultiplied` 从未传入 movie/frame config；代理链同样丢失 | `topos_encoder.open` / `proxy_generator` |
| P1-1 | planar 只查 `'422' in fmt`：yuv422p12le 被当 10-bit 接受；码值超范围不拒绝 | `topos_encoder._prepare_planar` |
| P1-4 | color range 未知值静默按 limited（注释承诺显式失败） | `topos_encoder._map_color_codes` |
| 新-1 | 代理链 alpha 位深硬编码 12：a8 源被压成近全透明、a16 源被静默截断 | `proxy_generator` |
| 新-2 | 码值域/白名单外的 packed 格式（如 'gray'）被当 RGB 静默编码 | `topos_encoder._prepare_packed` |
| 新-3 | **alpha 标度错位**：spec §8.6 规定码流内 alpha 为 16-bit 满刻度容器，应用层一直按"声明位深满刻度"喂值——a12 文件实际只写 ~6% 不透明度、a8 退化为二值 {0, 256} | 编码/解码/代理三处 |

## 2. 决策

### C-96 packed 通道映射显式化（P0-2）

四通道禁止整轴翻转：`bgra*` → `[..., [2,1,0,3]]`；三通道 `bgr*` → `[::-1]`。
仅接受 `rgb*/bgr*` 前缀的 3/4 通道布局，`argb/abgr/灰度` 等显式
RuntimeError（新-2）。未选 Alpha 输出但输入含第 4 通道 → 显式警告
（与 planar 既有行为对齐）。

### C-97 color range 严格校验（P1-4）

`_map_color_codes`：`''`（未指定）→ limited；仅接受 `limited/full`；其余
（含 `from_metadata` 产生的 `'unknown'`）显式失败——与 primaries/transfer/
matrix 既有行为一致。代理链读取侧同步严格化。

### C-98 planar 严格校验（P1-1）

- 格式白名单：3 平面 `{yuv422p10le}`、4 平面 `{topos_yuva422p10a8/10/12/16}`；
- PlaneInfo 位深核验（颜色=10；alpha=声明值）与紧排列 stride 核验；
- 颜色码值 ≤1023、alpha ≤ N-bit 满刻度，超出显式拒绝（12-bit 数据误标
  10-bit 的典型防御）；
- 码值扫描是 planar 直通路径唯一整帧遍历（SIMD 归约），导出路径可忽略。

### C-99 premultiplied 整段固定语义（P0-3）

- `FFmpegEncoderConfig.alpha_premultiplied: bool = False`（默认 straight＝
  应用内合成链约定）；
- open() 写入 movie config + frame config，tpcC 持久化，解码侧
  `extra['topos_alpha_premultiplied']` 回读；
- planar 帧自带标志与配置不一致 → 显式 RuntimeError（整段导出必须固定
  语义）；packed 帧不携带逐帧标志，不与配置冲突（配置声明即语义来源）；
- 代理链从源首帧 `extra` 传递，再编码不丢标志。

### C-100 alpha 位深从输入解析（新-1）

编码器从 `config.pix_fmt` 尾缀 `a{8,10,12,16}` 解析（a16→mode1 无损，
其余 mode2）；代理链从解码平面 `plane_infos[3].bit_depth` 取权威值。
`TOPOS_DEFAULT_ALPHA_BIT_DEPTH`（12）仅作无声明时的缺省。

### C-101 alpha 16-bit 容器标度对齐（新-3，破坏性修正）

spec §8.6：码流内 alpha 为 16-bit 满刻度容器，`alpha_bit_depth` 是顶层
N-bit 预量化精度。应用层平面语义统一为"声明位深满刻度"（阶段 7 GPU
上传按 `plane_infos[3].bit_depth` 归一化的契约不变）：

- 编码：N-bit 满刻度 `<<(16−N)` 后入 native（topos_encoder 两条路径 +
  代理链）；
- 解码：`topos_source` 对 a<16 平面 `>>(16−N)`（concealment 中性值 65535
  右移后 = 满刻度不透明，语义保持）；
- 收益：N-bit matte 经 mode2 N-bit 预量化后**无损** roundtrip（a12 误差
  从 ±15 降为 0；a8 从二值恢复为全精度）；
- **兼容性**：修复前写出的 a<16 文件 alpha 标度错误（a12≈6% 不透明度），
  V1 Preview 无存量文件承诺（未分发），旧文件重导出即正确；a16 文件
  本就正确且不受影响。

### C-102 生命周期卫生

close() 成功定稿后 `self._mux = None`（abort 幂等不再依赖已关闭 mux 对象
的内部守卫）；close/abort 任意顺序重复调用、编码中途失败后 abort 的行为
由 `test_close_abort_idempotent_and_temp_cleaned` 钉死（无临时文件残留、
不触碰已产出文件）。

## 3. 明确不做（留 R2+）

- 时间线导出 Alpha 压平（P0-1，`timeline_export._extract_frame_data` 只出
  BGR）与 HDR `[0,1]` clamp（P1-2）——整改 R2（真实时间线 Alpha/HDR 链）；
- Alpha 预算比例/硬上限自动执行与逐帧元数据（P1-3）——整改 R3；
- 12-bit/4:4:4 接受能力——R4。

## 4. 验证

- 失败先行：修复前 `TestR1DataCorrectness` 12 用例 = 10 failed（含审计
  复现值：BGRA 纯红 Y=877、a8 alpha 二值 {0,256}、`yuv422p12le` 静默接受、
  range='mystery' 静默 limited）+ 2 passed（rgba 契约钉死、生命周期幂等——
  设计上先通过的回归钉子）；
- 修复后：`tests/media/test_topos_export.py` 41/41；审计 §5 矩阵
  `tests/native + test_topos_source + test_topos_alpha_pipeline +
  test_topos_export` 88/88；
- 全量门禁 `bash native/topos_codec/run_tests.sh`（native 代码零变更，
  门禁复跑确认无交叉影响）。

## 5. 风险变化

- R-05（Alpha 链路丢失）备注更新：编码边界语义已修复，时间线主链仍待 R2；
- R-28 缓解证据 +1（失败先行测试制度在本轮实际执行）。
