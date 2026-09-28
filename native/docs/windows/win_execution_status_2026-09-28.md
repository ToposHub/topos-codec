# Windows 首切实机执行总结（2026-09-28）

> 对象：`docs/windows_topos_codec_sdk_optimization_plan_2026-09-28.md` 全部任务。
> 环境：Windows 11 26200 / i7 16C24T / 64GB / VS2022 BuildTools MSVC 19.44.35228 /
> WinSDK 10.0.26100 / CMake 3.31.6 / Python 3.12.10（`.venv-win`）。
> 起点 commit a77733b4 → 执行至本批提交（逐批 git 记录）。

## 任务状态总表

| ID | 状态 | 证据 |
| --- | --- | --- |
| W01 构建 | ✅ 完成 | Debug/Release exit 0；7 处可移植性修复 + 8MB 栈；`win_codec_baseline_*` |
| W02 测试门 | ✅ 完成 | ctest 93/93×2 配置、符号门 474≥62、dll_load×2、CLI 链路 4/4 |
| W03 ABI | ✅ 完成 | test_abi/_compat/_cpp ×2 配置 exit 0 + W16 外部消费者 |
| W04 并发 | ✅ 完成（MSVC ASan 未做） | 线程 1/2/4/8/24×3 轮、槽位回归、池统计、RSS 浸泡 |
| W05 SIMD | ✅ 完成（非 AVX2 真机缺） | scalar↔AVX2 位流+像素逐位一致×6 组合；`simd_parity_check.py` |
| W06 golden | ✅ 完成（Windows 侧） | conformance 全量位级一致；macOS/Linux 由既有 CI 同 commit 承担 |
| W07 RSS | ✅ 完成 | PeakWorkingSetSize 口径 + 句柄/线程计数；实测 494MB/68/17 |
| W08 IO | ✅ 完成（>4GiB 实 mux 降级稀疏探测） | 中文+空格链路、5GiB 稀疏、只读目录 exit1 无半成品、故障注入 |
| W09 母版 | ✅ 完成 | `win_master_manifest_*`（含原生 12-bit 444 母版；16-bit 缺口记录） |
| W10 ProRes 对位 | ✅ 完成 | 双轴数据 10 档配对；xq@12 N/A（prores_ks 域限制） |
| W11 画质评分 | ✅ 完成 | 分通道 PSNR + 1/3/5/10 代漂移收敛；SSIM/误差图未做（列入后续） |
| W12 性能基线 | ✅ 完成 | quality perf 全表、3 个基准门、V8 串行发现（150ms@1T==156ms@16T） |
| W13 优化 | ◐ 第一轮（fastdiv _umul128，位流不变）；V8 并行化为后续项 | A/B 记录于 `win_quality_perf_*` |
| W14 压力/产品 | ◐ codec 层过；产品入口归 Windows GPU 计划 | `win_p0_stability_*` |
| W15 打包 | ✅ 完成 | dist/release-x64；manifest 20 文件哈希复算 |
| W16 消费者 | ✅ 完成 | 中文+空格路径外消费 + 搬迁重跑通过 |
| W17 CRT | ✅ 完成（干净机待 W23） | /MD、VCRUNTIME140+UCRT；Debug CRT 未混入 |
| W18 wheel | ✅ 完成 | py3-none-win_amd64；独立 venv 127+ 通过；卸载无残留 |
| W19 CI 矩阵 | ◐ 配置+本机就绪（128 passed/3 skip 环境 skip）；runner 绿灯待 push | codec-matrix.yml python-app+windows |
| W20 上手/API | ✅ 完成 | SDK.md §7.5 Windows 快速开始 + 限制清单；示例自动化在 W16/W18 内 |
| W21 签名 | ❌ 未做（无证书）——按 unsigned Preview 对待 | — |
| W22 升级回退 | ◐ 格式回退方向由 golden/corpus 回放承担；多版本二进制矩阵待发布多版本后 | — |
| W23 下载发布 | ❌ 未做（需发布机/制品库；本机已完成包级三路线中的 CMake+Python+CLI） | — |
| W24 适配层 | 未启动（可选，需求触发） | — |

## 交付中的代码变更（全部入库，逐批提交）

1. **可移植性修复 7+3 处**：malloc 头 shim、`__int128`/`clzll`/`bswap32` 守卫、
   POSIX 时钟→QPC、VLA、EDTS 尺寸、`small/big` 宏冲突、`/STACK:8388608`。
2. **W07**：`quality.c` Windows 峰值 RSS/句柄/线程。
3. **W13**：fastdiv/rans2 的 MSVC `_umul128` 乘高位（位流逐位不变）。
4. **工具链修复**：package_codec 编译器探测（CMakeCCompiler.cmake + 配置子目录）、
   manifest POSIX 路径；package_opensource 测试排除与布局重写；
   v7_size_gate 本地模型同步 b41346b2；绑定层与 3 个测试的 VS 多配置目录发现。
5. **新增工具**：`scripts/simd_parity_check.py`、`tools/win_master_manifest.py`、
   `tools/win_rd_prores_matrix.py`（PyAV，含 to_ndarray 422 陷阱规避）。

## 阶段门结论（对照计划 §5）

- **G0（W01–W08）：达成**——MSVC 双配置、CTest、golden、ABI、损坏/并发/大文件
  均有真机记录。
- **G1（W09–W14）：基本达成**——真实母版、ProRes 对照、同机基线与一轮 A/B；
  画质结论如实（422 家族 ~76–83% 码率、Y 落后 1.8–5.5dB；不宣称优于 ProRes）。
- **G2（W15–W20）：达成（CI 绿灯一项待 push 后归档）**。
- **G3（W21–W23）：未达成**——证书/发布机缺，维持 **unsigned Preview** 定位。

## 遗留清单（下轮优先级）

1. V8 编码并行化（现串行，1K² 150ms；V7 同机 22ms@16T）。
2. 非 AVX2 真机（或虚拟机降级）验证 + MSVC ASan 五配置。
3. SSIM/误差图与边缘/色偏指标（W11 增强）；>4GiB 实际 mux。
4. W21 签名流程（证书就位后）；W23 下载发布三路线。
5. `ratio<1.3` 解码门在共享机的噪声敏感问题——移专用基准机严格门。
