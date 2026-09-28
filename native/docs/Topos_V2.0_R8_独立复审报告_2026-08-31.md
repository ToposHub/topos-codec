# Topos Video Codec V2.0 — R8 独立复审报告（2026-08-31）

复审人：未参与 R0–R7 实现的独立 agent（全部结论来自亲跑命令或 file:line
引用）；整改执行：R8 轮（本报告 §3）。

裁决：**有条件通过（conditional）——`v2_0_complete` 维持 false**。

---

## 1. 独立复跑证据（复审人亲跑）

- **native 全量门禁**：`run_tests.sh` exit 0，五配置（debug/asan/ubsan/
  tsan/fuzz）各 **42/42**，`ALL STAGE-10 CHECKS PASSED`；符号钉死/
  CLI 冒烟（含坏帧 rc=1）/interop oracle（parser+PyAV+ffprobe ×3）/
  多线程 golden parity/NEON arm64 交叉编译/perf gate 数值门 8/8/
  SDK C+Python 示例全 OK。
- **Python 应用矩阵**：tests/native + 5 个 topos media 套件 +
  tests/deliver = **312 passed**，0 失败。
- **R7 抽查**：check_exports（thin dylib）OK；dll_load_test（abi=2、
  qp20@10bit roundtrip maxdiff≤64）OK；verify_product_bundle 当时的
  PASS（后证实其中排除断言存在盲区，见 C-3）；RELEASE-MANIFEST 字段
  齐备（commit/abi/machine/compiler/逐文件 sha256）。

## 2. 复审发现（整改前）

### Critical（3 项，全部实证）

| # | 缺陷 | 实证 |
|---|------|------|
| C-1 | `check_exports.py` PE 导出目录字段错位（`<IIHHIIII` 只解 28B，把 Name RVA 当 NumberOfNames、NumberOfNames 当 AddressOfNames） | 合成布局正确的 PE32+ DLL 上报 `RVA 0x2 不在任何 section`，rc=1——Windows 符号钉死首跑必红，R-26「已缓解」依赖的机制实际不可用 |
| C-2 | fat/universal 首 slice 偏移硬编码 `8+nfat*20`，忽略 fat_arch.offset 对齐 | 发布 SDK universal dylib（slice0 实际 offset=16384）上报 `fat slice 不支持: 0x0` |
| C-3 | 产品包排除链死代码：`base_spec.generate_analysis` 先 `or []` 归一化 None，再判 `is None` → `get_preset_exclude_dirs` 永不执行；同时 build_product.sh 已移除 CLI 排除参数 | 解析 dist 可执行内嵌 PYZ（15912 模块）：edit=292、composite=239、teaching_animation=167 个模块**在包内**，与产品预设/spec docstring/commit 声明全部矛盾；verify_product_bundle 目录形态检查对此是盲的 |

### Suggestion（5 项）

S-1 verify_feature_dirs 目录不存在仅 WARN（spec 打包模块进 PYZ 不落盘
→ 排除验证失效）；S-2 video_export_job 跨模块引私有 `_parse_topos_pix_fmt`；
S-3 R-26「已缓解」与 ADR-C025「保持打开」措辞矛盾；S-4 ADR C-118
「本地已对拍」声明强于实证（仅 thin dylib）；S-5 发布清单 machine 含
主机名（内网信息外泄）。

## 3. R8 整改（全部完成，测试钉死）

1. **C-1 修复**：PE 导出目录按 winnt.h 完整布局解析（11 字段，读至
   AddressOfNames）；新增合成 PE32+ DLL 回归（成功 + 缺符号反例）
   ——`tests/native/test_release_scripts.py::TestR8SyntheticFormats`。
2. **C-2 修复**：fat 解析读 `fat_arch.offset`（对齐填充感知）；新增
   合成 fat（slice offset=0x4000）+ 发布 SDK universal dylib 实测回归。
3. **C-3 修复**：`generate_analysis` 的 None 判定移到 `or []` 归一化
   之前（预设推导成为活路径）；重建 topos-color 实证 PYZ 15912→**13510**
   模块、`PYZ exclusion: OK (3 feature dirs)`、`codec load: OK`、包体
   119.8→101.4MB。
4. **S-1 修复**：`verify_product_bundle.py` 新增 `verify_pyz_exclusions`
   ——复用 PyInstaller CArchiveReader/ZlibArchiveReader 解析内嵌 PYZ
   模块表，排除目录模块在包内即 FAIL（对修复前旧包实测正确拦截：
   composite 238/edit 291/teaching_animation 166）。
5. **S-2 修复**：`topos_encoder.parse_topos_pix_fmt` 公开别名，deliver
   侧不再引私有名。
6. **S-3/S-4 修复**：R-26 状态改回「打开」（关闭条件=真实 runner 首绿，
   与 ADR-C025 C-117 一致）；ADR-C025 C-118 补记 R8 更正（对拍范围如实：
   thin 本地 + 合成 PE/fat 回归 + 发布 universal dylib 实测）。
7. **S-5 修复**：RELEASE-MANIFEST machine.node 脱敏为 `(redacted)`。

整改后复验：release/manifest/timeline/deliver-UI 套件 **48+9 全绿**；
SDK 重打包（universal + manifest）+ 全门禁重跑见 §4。

## 4. §11 完成定义逐条（复审判定，整改后维持）

| # | 条目 | 判定 | 缺口 |
|---|------|------|------|
| 1 | 规格版本化 | 满足 | — |
| 2 | 永久 golden vectors | 满足 | — |
| 3 | 10/12-bit + Alpha 质量 | 满足 | 主观感知质量无正式报告（合成基线判定） |
| 4 | 损坏输入鲁棒性 | 满足 | — |
| 5 | Topos Color 全流程 | 满足 | edit 套件 12 项失败 + waveform 段错误为 **R7 前预存在**（stash 对照证实，非 codec 范畴） |
| 6 | Alpha 无静默丢失/降位深 | 满足（R-23 文档化例外） | — |
| 7 | scalar/AVX2/NEON 一致性 | **部分** | NEON 运行时差分未执行（仅交叉编译；闭合需 macos-arm64 CI 实跑） |
| 8 | 性能按 profile+环境报告 | 满足（含声明残余） | R-25 空载固定机全轮 raw/置信区间口径 |
| 9 | 资源释放正确 | 满足 | — |
| 10 | 旧码流兼容策略 | 满足 | — |
| 11 | 构建/打包/SDK/限制文档 | 满足 | C-3 整改后实物与文档一致 |

## 5. `v2_0_complete=true` 前置证据清单（未闭合项）

1. **R-33**：codec-matrix 四平台（含 macos-arm64 → 同时闭合 DoD 7 的
   NEON 运行时差分）真实 runner 首绿记录；codec-release 真证书
   sign/notarize/staple 记录；四产品包干净机加载记录（topos-color 已
   本地实建验证，其余三产品同一 spec/预设路径）；
2. **R-25 残余**：空载固定机全轮 raw + median 置信区间基准轮（或在
   风险表显式降级为已接受）；
3. **R-26**：随 1 首绿记录关闭；
4. 复审人附注：工作区存在未跟踪 V2.1 计划草稿（用户文档，未提交）——
   不构成「V2.1 启动」，R8 收口维持禁令。

## 6. 结论

R0–O5 交付经独立复跑成立；R7 的三处 Critical 已在 R8 全部修复并以
回归测试钉死；「报告已知未达但总表仍写完成」在 stage_status/计划书/
risk register 三方抽查中**未发现**现行实例（R7 提交信息中对排除表的
声明曾是过度声明，已由本报告更正并存档）。V2.0 完成标记按规则维持
false，直至 §5 清单补齐。
