# Windows P0 稳定性批次报告（W04–W08，win-msvc，2026-09-28）

> 环境同 `win_codec_baseline_win-msvc_2026-09-28.md`（MSVC 19.44 / Win11 26200 / 16C24T）。

## W04：线程池与并发压力 —— 通过

- `test_stress`（新增 `TOPOS_STRESS_THREADS` 旋钮）：**1/2/4/8/24 线程全部 exit 0**，
  再加 3 轮 × {1,4,24} 重复全过；耗时 2.08s@1T → 1.06s@4T → 0.97s@24T（小帧几何，
  扩展受限于单帧并行度，符合预期）。无死锁、无越界。
- 槽位回归（高→降→扩容→再升，`test_slot_watermark`）：exit 0。
- `test_tpool_stats`（池遥测/嵌套回退）、`unit_robust_io`（IO 故障注入含并发读）：exit 0。
- RSS 收敛浸泡（Python `test_decode_soak_rss_converges`，新增 Windows 采样分支）：通过。
- 说明：MSVC ASan 五配置构建未在本轮展开（Linux/macOS CI 已有 ASan 门）；
  Windows ASan 专项列为后续项。

## W05：AVX2/scalar 分发一致性 —— 通过（位流+像素逐位一致）

新增 `scripts/simd_parity_check.py`（可入 CI）：同进程经 `tc_dev_set_simd_mode`
切换 SCALAR/AUTO，对 6 组合做"同输入编码比位流 + 同包解码比像素"：

| 组合 | 位流 | 像素 |
| --- | --- | --- |
| 422 bd10 / bd12 / bd16(V8 熵) | 一致 | 逐位一致 |
| 444 bd10 | 一致 | 逐位一致 |
| GBR bd12 | 一致 | 逐位一致 |
| 422 bd10 + alpha2 | 一致 | 逐位一致 |

后端名确认实际切换（scalar ↔ avx2）。过程记录：bd16 帧级编码须 `reserved[0]=8`
（V8 段化 rANS）；`reserved[0]=9`（微 GOP）用帧级一次性解码会 concealed——属路径
误配非缺陷（gop 语境由 conformance_v9 覆盖，ctest 已过）。
非 AVX2 真机仍缺（本机 AVX2 无法关闭）；已用强制 scalar 钩子完成等价性证据，
真实非 AVX2 CPU 留待硬件条件（计划允许的替代路径）。

## W06：跨平台 golden —— Windows 侧通过

CTest Release 93/93 含全部 conformance_*（transform/bitstream/codec/bd12/pf444/gbr/
profiles/vlc/v7r2/traw/v8/v9/v73/image×7/mov/mov_audio）：**Windows 解码与冻结向量
逐位一致、重编码逐字节一致**。冻结向量由 macOS 生成并经 Linux/macOS CI 常驻校验，
Windows 侧一致 ⇒ 三平台位流兼容闭合（CI 同 commit 运行记录由既有矩阵承担）。

## W07：perf_peak_rss_kb —— 已实现（不再返回伪 0）

`src/cli/quality.c` Windows 分支实现：
- **峰值 RSS = `GetProcessMemoryInfo(PeakWorkingSetSize)`，工作集口径**（与 ru_maxrss
  同为"历史峰值驻留"语义；报告行明示口径，不与私有字节/提交量混用）；
- 同时输出**句柄数**（GetProcessHandleCount）与**线程数**（Toolhelp 线程快照）。
- 实测（`topos_quality perf`，quick）：峰值 RSS=492,284 KB、句柄=76、线程=17——
  与 4K 基准的工作集量级吻合（任务管理器交叉目视一致）。

## W08：大文件/Unicode 路径/IO 边界 —— 通过（一项降级记录）

- **中文+空格路径**：`C:\Users\heng\视频 测试\素材 01\` 下 rawgen→encoder→probe
  --verify 4/4→decoder decoded=4 concealed=0 全链路 exit 0（本机 ACP=936；
  窄字节 fopen 语义与 CRT Unicode→ANSI argv 转换自洽。超出 ACP 覆盖字符的场景
  需 `_wfopen`，记入 W20 文档限制项）。
- **>4 GiB 定位**：5 GiB 稀疏文件经 probe（内部 `_fseeki64 END` 长度探测）→
  干净拒绝 `-9 缺 ftyp/moov`，无 IO 错误。真实 >4GiB MOV mux 留待 W12 长序列基准
  （避免本轮生成 4GiB+ 素材）。
- **只读目录**：deny-write ACL 下 encoder 输出 `cannot open output`、**exit 1、
  无半成品文件**。
- **短读短写/磁盘满/并发读**：`unit_robust_io`（故障注入）ctest 通过。
- CLI 共享头已按 MSVC 使用 `_fseeki64/_ftelli64`（64 位定位无 _FILE_OFFSET_BITS 概念）。

## 证据文件

`log_thread_scaling_win_2026-09-28.txt`、`log_thread_rounds_win_2026-09-28.txt`、
`log_simd_parity_win_2026-09-28.txt`（脚本 `scripts/simd_parity_check.py`）。
