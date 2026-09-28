# ADR-C002：阶段 1–2 实施记录与变换构造决策

- 状态：已接受（2026-08-29）
- 前置：ADR-C001（阶段 0 决策冻结）
- 关联规范：`bitstream_spec_v1.md（原 v0，阶段 3 更名冻结）` §7、附录 A.1/A.3（本文决策已同步更新）

---

## 1. 阶段 1 交付记录（Native 工程骨架）

- `native/topos_codec/`：CMake ≥3.16、C11、公开/私有头边界（include/ vs src/common|transform）。
- 基础设施：`status.h`（公开枚举，spec §9 冻结值）、`error.c`（线程局部错误详情 +
  pthread 隔离测试）、`checked.h`（u32/u64/size 加减乘 + `tc_offset_in_bounds`，与
  `__builtin_*_overflow` 10 万组随机交叉验证）、`bufview.h`（空输入/NULL/SIZE_MAX 语义
  全测）、`endian.h`（BE 逐字节组装，非对齐安全，UBSan 门禁下验证）、`cpudetect.c`
  （AVX2/AVX512F/FMA/NEON 探测骨架，只报告不 dispatch）。
- 最小 C ABI：`tc_abi_version / tc_version / tc_query_cpu_features / tc_status_message /
  tc_last_error`；struct_size/abi_version 约定 + reserved 不触碰（测试断言 0xAA 保持）。
- 测试：mini_test 零依赖框架；fuzz 双形态（libFuzzer + 确定性回放 driver `-gen`）。
  **Apple clang 不随附 libFuzzer runtime**（链接探测证实）——fuzz 覆盖由回放 driver 在
  ASan/UBSan 配置下承担（ctest 常驻），libFuzzer target 在有 runtime 的环境自动启用。
- 绑定层：`src/shared/codec/topos_binding.py`（ctypes，候选路径搜索 + 显式路径严格模式），
  `tests/native/test_topos_binding.py` 6 项全绿（含 R-18 sizeof 镜像守卫）。
- 一键门禁：`bash native/topos_codec/run_tests.sh` = Debug/ASan/UBSan/Fuzz 四配置
  ctest + 绑定冒烟，全部通过。
- 核心库编译纪律：`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Werror` 零告警（`-Wconversion` 直接服务「无未检查外部算术」门槛）。

## 2. 决策 C-19：变换矩阵改为自构造「精确行正交」整数 DCT-8 近似

**背景**：spec v0 附录 A.1 原采用从公开资料回忆的 H.264 风格 8×8 矩阵。阶段 2 实施时
数值验证发现该矩阵**行间并非精确正交**（例：r1·r6 = −40，r1·r3/r1·r5 类点积 = ±2）。
非正交性意味着 Mᵀ·(F⊙对角权重)·M 存在**不可由对角补偿的空间串扰**——这不是调参问题，
是结构性失真。

**决策**：按对称/反对称结构自行构造：
- DCT-8 基中 u=0,2,4,6 行对称（row[7−x]=row[x]）、u=1,3,5,7 行反对称
  （row[7−x]=−row[x]）；对称×反对称点积**恒为 0**（结构性）。
- 组内（各 4 行）以理想 DCT 值 ×13 取整为中心，±3 整数域搜索满足精确两两正交、
  总偏离最小的组合。偶组手工即达精确（`(13,13,13,13)(12,5,−5,−12)(9,−9,−9,9)(5,−12,12,−5)`）；
  奇组最优解总偏离 8（见 `tools/gen_transform_tables.py` 内嵌冻结值）。
- 结果：8×8 全部 28 对行点积 = 0；|M| ≤ 13；与理想 DCT 基相关性 ≥ 0.996。

**反变换归一**：M·Mᵀ = diag(E)，E = [1352,680,676,740,648,740,676,680]。
逆变换为 `x̂' = round((Mᵀ·(F'⊙W)·M)/2^32)`，`W[u][v] = round_half_up(2^32/(E_uE_v))`
（2350..10241，u16 容量内）。精确正交保证该对角归一**无串扰**；W 为冻结常量表
（生成器 `tools/gen_transform_tables.py` 是单一事实来源，C 表头为生成产物）。
舍入采用符号拆分（负数不做算术右移，spec §2.4）。

**时序说明**：本变更发生在任何 golden vector 存在之前（spec v0 → 实现期校正，
符合 spec A.8 与计划 §9「先冻结 spec 与 golden」流程——golden 现已随本矩阵生成，
此后任何变更走 §13 版本规则）。

## 3. 决策 C-20：quant/dequant 的 ±2^25 钳位防御层

合法路径界：|F| ≤ 22,140,352 < 2^25（解析界，对抗输入实测**恰好达到**，见 §4）。
实现层面 `tc_quant_block`/`tc_dequant_block`/`tc_transform_inverse_8x8` 对输入做 ±2^25
钳位（含 INT32_MIN 取负前的钳位——UBSan 在本阶段真实拦截过一次该 UB）。
阶段 3 的位流域校验（符号值域 vs Q）是第一道防线；本钳位是模块级第二道防线，
保证未来任何上游 bug 也不会触发未定义行为。合法码流语义不受影响。

## 4. 界与质量验证报告（阶段 2 完成门槛证据）

环境：i9-9900K / macOS 15.3.1 / Apple clang 17 / Debug+ASan+UBSan 三配置。

| 验证项 | 结果 |
| --- | --- |
| M 行两两正交（28 对） | 全部 = 0（C 侧 test_transform 持续复验 + 生成器断言） |
| W = round_half_up(2^32/(E_uE_v)) 重推导 | 与冻结表逐项一致 |
| 前向已知值（flat / 64 位置单脉冲） | 全部精确命中解析式 |
| qp=4（Q=1）近无损往返（4000 随机块） | **max err = 0** |
| 全链路 max err（qp=0/4/20/40/63，7 图案×60） | 0 / 0 / 0 / 0 / 3 |
| 对抗输入（±2047 符号对齐 + 2 万随机）max \|F\| | 22,140,352 == 解析界 |
| 逆变换钳位边界（±2^25 全频满幅） | s64 路径无溢出，输出 ≤ ±10^6 界内 |
| sanitizer | ASan/UBSan 全绿（UBSan 拦截并修复 1 处 INT32_MIN 取负 UB） |
| 浮点参与 | 无（无 math.h，纯整数运算） |
| golden vectors | 1380 条（2 位深 × 5 qp × 138 图案）生成即校验**逐字节一致**，
  折叠校验和匹配；文件 `tests/conformance/golden_transform_v1.bin` 入库，ctest 常驻 |
| 跨平台 bit-exact | golden 为纯整数确定性枚举，结构上平台无关；多平台 CI 复验归阶段 10 构建矩阵 |

门禁命令：`bash native/topos_codec/run_tests.sh`（四配置 + conformance + 绑定冒烟全绿）。

## 5. 阶段 2 交付物清单

- `src/transform/`：transform.c/h（前向/逆变换）、quant.c/h（qp_scale/QM/quant/dequant）、
  plane.c/h（coded 尺寸/边缘复制填充/裁剪）、transform_tables.h（生成产物）
- `tools/gen_transform_tables.py`（表生成与验证，单一事实来源）
- `tests/unit/test_{transform,quant,plane}.c`（10 项单测，四配置）
- `tests/conformance/`：golden_util.h + golden_transform.c（gen/check 双模式）+
  golden_transform_v1.bin（1380 记录，1.4MB，入库）
- spec v0 更新：§7.3（Q 显式钳位）、§7.6（新界表）、§7.7（新逆变换）、A.1（新 M/E/W）

## 6. 未解决问题（移交后续阶段）

- QM id=1（Standard）调优矩阵：flat 占位，按 E 值先验在阶段 4 随码率目标调优并冻结。
- golden 记录目前覆盖变换/量化链（无熵编码）——阶段 3 位流 golden 落地后扩展为
  全链路向量（`test_vector_manifest.json` conformance_matrix 已预留）。
- 跨平台 bit-exact 复验：归阶段 10 构建矩阵（当前平台逐字节一致已证）。
