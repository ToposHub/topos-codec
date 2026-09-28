# ADR-C030：V7-B shared transform pyramid（共享分析金字塔）

- 状态：Accepted（P1-01 冻结；实现见 P1-02..P2，门禁见 P4）
- 日期：2026-09-09
- 范围：`native/topos_codec` V7-B minor-4 目录布局、分析/合成变换族、
  边界延拓、422 chroma 相位、舍入、level/tile 几何、错误隐藏与版本升级
- 取代：无（RD4-08 的像素 residual 架构不在本 ADR 内修正，由本 ADR 的
  后继实现替代其写入路径；minor-1/2/3 读取兼容不受影响）
- 关联：`docs/topos_v7b_reduced_decode_master_plan_2026-09-09.md` §5 P1、
  ADR-C029（V7-A band directory）、`capability_manifest.json`

## 1. 背景与决策

RD4-08 证明「先独立编码 base、再对每源像素写 residual」的架构不可能同时
满足质量匹配与一次分析预算：enhancement 覆盖全分辨率逐像素残差，编码端
必然多一次源尺寸扫描；且 base 与 residual 各持一套低频表示，互相重复。

**决策：V7-B minor-4 采用共享分析金字塔。** 每个源平面只做一次确定性
整数 lifting 分析，产出：

```text
源平面（每平面独立，含 chroma/alpha 自身网格）
  └─ Level k（按冻结级联表逐级 1/2 或 1/3）
       ├─ LL_k（最后一级 LL = base 平面，喂现有帧编码器）
       └─ 高频子带（每 tile 独立 offset/size/CRC，可整 tile 跳过）
预览 = 只解 base；完整 = base + 高频子带 → 同一套合成 → 逐位还原源平面
```

不变量（违反任何一条即实现缺陷）：

1. 每个源采样在分析输出中**恰好出现一次**（某个 LL 采样或某个 detail
   系数）；不存在第二份源尺寸像素/残差平面。
2. 无量化配置下 base⊕detail 合成**逐位还原**源平面（含 odd、非 8 对齐、
   12-bit、16-bit alpha）。
3. 低频与高频由**同一次**分析产生、互补无重复；base 编码质量参数只影响
   base 段自身的量化，不改变分析本身。

## 2. 变换族与精确算术（冻结）

全部运算为整数运算；`floor(a/b)` 为数学下取整（对负数同样下取整），
参考实现必须使用可移植写法（C 预先 C23 对负数 `>>` 实现定义）。
`bd` 为平面位深（10/12/16）。采样域 `[0, 2^bd - 1]`。

### 2.1 N=2 级（dyadic，CDF 5/3 风格 predict/update）

设行长 `W`，偶采样 `x[2j]`、奇采样 `x[2j+1]`（`j` 从 0 起）：

```text
d[j] = x[2j+1] - floor((x[2j] + x[2j+2]) / 2)        j = 0 .. floor(W/2)-1
L[j] = clamp(x[2j] + floor((d[j-1] + d[j] + 2) / 4))  j = 0 .. ceil(W/2)-1
```

- 边界延拓：`x[-1] := x[0]`、`x[W] := x[W-1]`（半样本对称）；若
  `2j+2 >= W` 且 `x[2j+2]` 落在延拓位，用 `x[2j]` 替代（末对钳位）。
- detail 边界：`d[-1] := 0`；`j` 上界使 `d` 不访问未定义位。
- `clamp` 与逃逸通道见 §2.4。

### 2.2 N=3 级（triadic，中位锚定 predict/update）

每组三采样 `a = x[3i]`、`b = x[3i+1]`、`c = x[3i+2]`：

```text
d0[i] = a - b
d1[i] = c - b
L[i]  = clamp(b + rdiv3(d0[i] + d1[i]))
rdiv3(v) = floor((v + 1) / 3)          /* v/3 四舍五入到 +∞，冻结 */
```

- 尾组（`W mod 3 = 1` 或 `2`）：复制末采样补满三元组后按上述变换；
  合成后丢弃复制位。逆运算只依赖已重建量，逐位精确。
- `rdiv3` 对负数同样按公式执行（即 round-half-up）。

### 2.3 级联表（冻结；超出表中比例的几何拒绝该布局）

| 源:base 比例（每轴） | 级联（执行顺序） | 典型源 |
| ---: | --- | --- |
| 2 | [N=2] | 3840×2160 |
| 3 | [N=3] | 5760×3240 |
| 4 | [N=2, N=2] | 7680×4320 |
| 6 | [N=3, N=2] | 11520×6480 |

- 冻结级联：比例 4 = 两级 N=2（`7680→3840→1920`）；比例 6 = 一级
  N=3 后一级 N=2（`11520→3840→1920`）。禁止其它分解（如 4 = 3 后
  非整数）。比例不在表内（如 16/5 的 6144 互操作尺寸）不走本布局，
  沿用现有 max-dim 路径并在目录如实记录几何。
- 每级只在上一级 LL 上继续分析；上级高频子带不再分解。
- 级数上限：**4**（目录字段校验）。

### 2.4 值域钳位与逃逸通道（exactness 的唯一豁口，冻结）

update 项可能把 `L_raw` 推出 `[0, 2^bd - 1]`。冻结规则：

```text
L      = min(max(L_raw, 0), 2^bd - 1)
escape = L_raw - L                       /* 有符号，通常为 0 */
```

`escape != 0` 的位置记入该平面的**逃逸系数通道**（与高频子带同级的
稀疏段，Rice/Exp-Golomb 编码，空段允许 size=0）。合成时
`L_raw = L + escape` 后按 §2.1/§2.2 逆变换。据此 base 平面恒在
uint16 采样域内，现有帧编码器无需改动；逃逸通道为空时完全无损路径。

自然图像中 update 溢出仅出现在极端梯度邻黑/邻白处，逃逸通道期望为空
或近空；若 corpus 实测逃逸率 > 0.1%，P2 rate control 必须显式计入预算。

### 2.5 2D 顺序与子带命名（冻结）

每级先**水平**后**垂直**（行方向先）。N=2 级产生 4 个子带
`LL, LH（水平低-垂直高）, HL, HH`；N=3 级产生 9 个子带
（3×3 相位组合）。垂直遍历对水平分析产生的**全部**子带执行。子带顺序
按字典序 `(L=0<相位0<相位1)` 行优先冻结，写入目录。

### 2.6 逆变换

按 §2.1/§2.2 公式逐层逆推（先细后粗，与级联表逆序）。N=2 逆：

```text
x[2j]   = L'[j] - floor((d[j-1] + d[j] + 2) / 4)     /* L' = L + escape 还原 */
x[2j+1] = d[j] + floor((x[2j] + x[2j+2]) / 2)
```

注意 `x[2j+1]` 只依赖偶位（已全部重建），无前向依赖。N=3 逆：

```text
b = L'[i] - rdiv3(d0[i] + d1[i]);  a = b + d0[i];  c = b + d1[i]
```

## 3. 422/444/GBR chroma 相位与 Alpha

- 金字塔**逐平面独立**作用在该平面自身网格（422 chroma 宽
  `ceil(w/2)`、444/GBR 全宽），不做任何重采样或相位偏移；
  `chroma_siting` 元数据原样透传，显示端语义不变。
- Alpha（straight 16-bit / restricted 8-12-bit）同样走金字塔；alpha
  lossless profile 要求下全链（含逃逸通道）必须逐位精确，禁止量化
  alpha 高频子带。

## 4. Level/tile 几何与目录（minor-4）

- 子带段内按 **64×64 系数 tile** 切分的目标粒度分阶段交付：detail 段头
  携带 `tile_version` 字段；`0` = 段级粒度（整带一个编码单元，段级
  offset/size/CRC 已满足预览跳过与段级隐藏，即 minor-4 v1 实现）、
  `1` = 段内 64×64 tile 表（每 tile `offset/size/crc32`，size=0 表示全零
  tile 可跳过），为逐 tile 隐藏激活时预留；读侧遇到未知 tile_version 必须拒绝。
- 目录沿用 V7-B 外层目录框架（TPLD），`directory_version_minor = 4`；
  新增 per-level 子目录段，字段全部带上限：
  - `level_count ≤ 4`；`band_count_per_level ≤ 9`；
  - `tile_rows/tile_cols ≤ 1024`；`coefficient_tile = 64`（常量，不读流）；
  - `plane_count ≤ 4`（同帧头）；逃逸通道每平面至多 1 段。
- base 段保持「独立可解码完整 Topos 包」（minor-1/2 语义），位于 payload
  首段；minor-4 读者未实现时可仅读 base 段。
- **未知 minor → `TC_ERR_UNSUPPORTED_VERSION`**，不做猜测解析；minor-4
  之后的参数变化（量化表、熵参数）必须升 minor，旧读者按冻结算术永远
  可解 minor-4。

## 5. 错误隐藏与安全

- 每 tile CRC32 校验失败 → 该 tile 按全零 detail 隐藏，对应
  slice_status 记 CONCEALED（与现有 slice 隐藏语义一致）；帧不整体失败。
- base 段损坏 → 沿用现有 V7-B base 段错误路径。
- 目录反解析沿用 minor-1/2 的严格契约检查（重叠、溢出、gap 全拒），
  新增字段同标准：所有长度/数量先 `tc_umul/tc_uadd` 校验后使用。
- fuzz 基线：截断目录、tile 越界、未知 flags、逃逸段超限均须稳定错误码。

## 6. 验收（对齐计划 P1-01）

1. **双实现逐样本一致**：P1-02 交付 C scalar 参考
   （`src/transform/pyramid_transform.c`）；独立第二实现为测试内
   Python 镜像（逐公式独立翻译），对 flat/渐变/细线/颗粒/随机 +
   odd/非 8 对齐/12-bit/16-bit alpha 输入比对 bit-exact。
2. **无量化 bit-exact**：`analyze ∘ synthesize = id` 对全部上述输入。
3. **字段上限齐全**：§4 表列出的每个字段都有写读双侧校验。
4. 单遍分析：一次源平面遍历完成全部级（读每采样一次）。

## 7. 后果与风险

- 编码端不再产生源尺寸第二像素平面（P1-03 的所有权规则由本 ADR §1
  不变量约束）；encode p50 预算改为一次分析 + base 编码 + 子带熵编码。
- 压缩经济性取决于高频子带熵编码效率，风险集中在颗粒/细线类内容；
  由 P2-03 联合 rate control 与 P4-01 匹配质量门禁判定，不在本 ADR
  预支结论。
- minor-3 试验布局维持只读；不再新增任何 minor-3 写出。
