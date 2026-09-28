# ADR-C028 — 编码自检 CRC 折进组装拷贝（§11.5 校验时序重构）

- 日期：2026-09-02
- 状态：**已实施并按 §3 门 4 整案回退**（2026-09-02；负结果归档 bench_out/m11_stage2b/20260902_c028_crc_fold_reverted.txt）
- 修订记录：D-4 原文指定 SSE4.2 crc32q 属**错误多项式**（Castagnoli ≠ 本库冻结的 IEEE 802.3）——实施时修订为 PCLMULQDQ 折叠（推导与常量见附录 A）
- 关联：ADR-C022（R6 性能门槛与回退纪律）、ADR-C026（验收门先例）；
  `bench_out/m11_stage2b/20260902_stage2b_kernel_crc_attempts.txt`（2b-B 并行化否证 +
  本方向量测依据）；`bitstream_spec_v1.md` §11.5；codec.c `asm_append`/自检点、
  packet.c `tc_packet_scan`/`tc_packet_parse_structure`

## 0. 背景与问题

§11.5 契约：编码器输出必须通过自身完整结构校验（plain 编码恒开；sized 迭代期关、
最终包统一复验）。现状为**两级 CRC**：

1. slice emit（worker 内，bitwriter 数据 L1 热）：`sh.slice_crc32 = tc_crc32(...)` 
   （codec.c:1557）——参考值写入 slice header，随后 payload memcpy 进 slot arena；
2. 组装完成后（主线程串行）：自检 `tc_packet_scan(out, ...)` 重读整个输出包逐 
   slice 复核 CRC（packet.c:65）——`check_wall = 2.24 ms/帧`（纯 C 15.1ms 口径
   的 ~15%，4K hq 素材）。

2b-B 已否证并行化路线：逐 slice 微任务（51 jobs）与 ≤8 连续区块两种派发**总墙钟
均净亏 1.5~2 ms**——band 大规模 join 后线程池深度休眠，自检阶段二次唤醒风暴成本
超过 2.24 ms 串行 CRC 本体（check_wall 并行版可降至 0.41 ms，但总墙钟反升）。

剩余可行方向（本 ADR 冻结）：把复核 CRC 折进组装 memcpy——组装是这些字节在冷却
前的最后一站，省去整包冷重读（热 L1 数据 ~1.1 ms，净省 ~1 ms/帧）。

## 1. 决策

### D-1 `asm_append_crc`：拷贝 + CRC 同循环

- `asm_buf` 增加 CRC 累计态；新增 `asm_append_crc(asm_buf* a, const void* data,
  size_t n, uint32_t* crc_out)`——拷贝与 slicing-by-8 CRC 在同一循环（8 字节步进，
  尾部逐字节），错误路径（LIMIT_EXCEEDED 继续计数）语义与 `asm_append` 一致。
- **三处组装循环接入**（payload append 替换为融合版；17B slice header append
  不变——CRC 契约只覆盖 payload，与现状 scan 侧一致）：
  plain 路径（codec.c:1711-1712）、sized 最终装配（1895-1896）、alpha 装配
  （2369-2370）。
- `enc_band_task` / alpha band 结构携带 `uint32_t slice_crc32`（emit 时从 `sh`
  复制）供组装侧就地比对；比对失败置位组装级失败标志（首个失败 slice 序号留档）。

### D-2 §11.5 自检复核改为「结构-only 扫描 + 折叠校验标志」

- 两处自检点（plain codec.c:2029、sized 最终包 3001）：`tc_packet_scan` →
  `tc_packet_parse_structure`（header 解码/尺寸一致/slice 边界/trailing/
  slice-map 校验**全保留**）+ 逐 slice 检查 D-1 折叠 CRC 的比对结果。
- 错误语义逐字保持：失败 → `TC_ERR_MALFORMED`，文案 "self-check: slice %u crc"；
  结构扫描失败路径同理（"self-check scan failed: %s"）。
- `tc_packet_scan` 本体保留不动（内部接口；解码侧不受影响——解码 CRC 自 M1 起
  就是 slice worker 单次执行，与编码自检无关）。

### D-3 校验时序语义（§11.5 修订论证）

- 覆盖链不变：CRC 参考值仍来自 emit 时 bitwriter 数据；复核仍覆盖
  bitwriter→arena→out 全写链的逻辑错误（偏移错、覆写、乱序、长度错）。
- 唯一时序差异：复核 CRC 从「组装完成后重读 out」提前到「拷贝写入时对源字节」
  ——检验窗口**更早、更小**。「dst 写入后至复核间」的硬件瞬时翻转不在威胁模型内
  （重读版同样无法覆盖其后的翻转；两类实现对宇宙线级故障等价盲）。
- 确定性（§11.1）：多项式/表冻结不变，wire 字节零变化，golden byte-exact 门维持。

### D-4 可选加速件：SSE4.2 `crc32q` 硬件分发

- 融合循环吞吐受表驱动 CRC 限制（slicing-by-8 ≈1.5–2 GB/s）；本机（i9-9900K）
  具备 HW CRC32（~8 GB/s，crc32q 三链并行更高）。`tc_crc32` 增加 CPU feature
  dispatch（cpudetect 框架现成）可使融合近似零成本。
- 该件**独立可回退**：逐字节结果恒等（test_crc32 运行时参考实现交叉验证模式
  扩展到 HW 路径），失败不影响 D-1~D-3 语义。

## 2. 变更面

| 文件 | 变更 |
| --- | --- |
| codec.c | `asm_buf`/`asm_append_crc`；三组装环接入；两自检点改造；任务结构 +`slice_crc32` |
| crc32.c/.h | 可选：HW dispatch + 融合用增量 CRC 接口（`crc32_update` 形态） |
| packet.h/.c | **不动**（`tc_packet_scan` 保留） |
| 公共 ABI / 码流格式 | **零变化** |

## 3. 验收门（实现交付时一并执行；任一不过 → 整案回退）

1. **差分**：融合 vs 未融合，全几何/pf/alpha/qp 档输出逐位一致（复用 stage9
   差分形态）；
2. **golden×6 byte-exact**；
3. release / ASan / TSan 45/45；
4. 同窗成对 A/B 编码 wall **≥3%**（4K hq 素材；含 D-4）；
5. 剖析断言：`check_wall_ns` 降至 ≤1.2 ms/帧量级；
6. **故障注入**：人为翻转一 slice payload 字节 → 自检仍以同一错误码/文案拒绝
   （触发时点提前到组装侧，属预期差异）。

## 4. 风险

- 融合循环慢于纯 memcpy（CRC 计算约束）→ D-4 缓解；若含 D-4 仍 <3% → 回退；
- 对齐/尾部正确性 → 门 1 + test_crc32 交叉验证；
- sized 双复核点遗漏 → D-2 已列举两处，门 6 对 plain 与 sized 各注入一次。


## 附录 A：PCLMULQDQ 折叠推导（实施时数值闭包验证，供未来复用）

- 正规域（poly 0x104C11DB7，MSB-first）递推：acc' ≡ acc·x^128 ⊕ m_be·x^32
  (mod P)；m_be = 16 字节块 big-endian 位串（首字节最高次）。
- 折叠常量：K64 = x^64 mod P = **0x490d678d**、K128 = x^128 mod P =
  **0xe8a45605**、K192 = x^192 mod P = **0xc5b9cd4c**。
- limb 展开（V = acc·x^128 ⊕ m·x^32）：V0 = m_lo<<32；V1 = (m_hi<<32)⊕
  (m_lo>>32)；V2 = acc_lo⊕(m_hi>>32)；V3 = acc_hi；
  acc' = V3×K192 ⊕ V2×K128 ⊕ V1×K64 ⊕ V0（≤96 位）。
- 反射域包装：输入逐字节位反转（SIMD nibble 表 11 指令 + 字节反序）、
  种子 rev32(raw)、尾段回反射域走已验证表、出口 rev32(S)^0xFFFFFFFF。
- 验证：Python 对拍 zlib.crc32（8000 随机 + 3000 链式）后移植；
  C 移植陷阱：v0 高 lane 必须清零（m_lo 的高 lane 是 m_hi）、96 位
  长除法 d≥64 时除数高位须进 hi、rev32 需四段（缺字节序交换 =
  逐字节位反转）、尾段串行步输入位应注入寄存器 LSB。
- 实测吞吐：融合拷贝+CRC 2.6GB/s（表驱动 0.45GB/s）。

## 附录 B：回退归因（门 4 数据）

t16 成对 A/B（10 对，4K qp55 9MB payload）：剖析串行段 24.0→8.3ms
（省 15.7ms/帧，10/10）但 wall −7.5%——并行段等效膨胀 ~18ms；
t1 判别打平且 check 自检在 t1 仅 3.9ms（t16 为 19.2ms，缓存态强依赖）——
"折叠省 ~1ms"的收益前提（2b-B 外推）不成立。机制深挖随案关闭。
