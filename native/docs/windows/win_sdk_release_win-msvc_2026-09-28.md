# Windows SDK 打包与消费者验证（win-msvc，2026-09-28）

> 对应计划 W15、W16、W17（首段）+ W03（ABI 显式记录）。
> 状态：**W15/W16 完成；W17 依赖扫描完成**（干净机验证与文档化转 W20/W23 批次）。

## W03：ABI（显式记录，ctest 内亦通过）

`Debug/Release × {test_abi, test_abi_compat, test_abi_cpp}` 共 6 项，全部 exit 0
（`log_abi_win-msvc_2026-09-28.txt`）。test_abi_compat 按 LLP64 Windows x64 钉死
结构大小/偏移/错误码；test_abi_cpp 验证 C++ 可包含性。外部消费者由 W16 冒烟覆盖。

## W15：SDK 打包（package_codec.py，exit 0）

- 构建：独立目录 `build/win-msvc-rel`（`-DCMAKE_BUILD_TYPE=Release -DTOPOS_ENABLE_TESTS=OFF`），
  仅构建 `topos_codec` 目标。
- 包内容：`lib/topos_codec.dll`（PE x64，580,096 B）+ `lib/topos_codec.lib`、3 个公共头、
  LICENSE/NOTICE、规范与能力清单（docs/）、`lib/cmake/ToposCodec/` config、
  examples/、BUILDINFO.txt、RELEASE-MANIFEST.json。
- 打包内置加载测试通过（abi=2，qp20@10bit roundtrip maxdiff≤64）。

## W16：仓库外消费者（含中文+空格路径）

- SDK 复制到 `C:\Users\heng\Topos SDK 测试 目录\topos-codec-sdk`（仓库外、含空格与中文）。
- `sdk_consumer_smoke.py`：外部临时 CMake 工程 `find_package(ToposCodec CONFIG)` →
  链接 `ToposCodec::topos_codec` → 编译运行最小消费者 → **OK（exit 0）**。
- 目录搬迁（`→ 搬迁后SDK`）后重跑 → **再次 OK**（CMake config 无绝对路径耦合）。
- RELEASE-MANIFEST.json 全部 20 个文件 SHA-256 复算一致。

## W17：CRT 策略与依赖扫描（dumpbin）

| 产物 | 依赖 |
| --- | --- |
| Release DLL | KERNEL32.dll、VCRUNTIME140.dll、UCRT（api-ms-win-crt-*，Win10+ 系统 内置） |
| Debug DLL | KERNEL32.dll、VCRUNTIME140D.dll、ucrtbased.dll（Debug CRT） |

结论：发行物为 `/MD` 动态链接 VC 运行库；干净机需 VCRUNTIME140.dll（随包说明或
VC_redist x64，W20 文档落实）；**Debug CRT 未混入发行包**（dist 仅含 Release 产物）。

## 已知限制（如实记录）

- 本机即开发机，非"无编译器干净机"；干净机验收留给 W23 发布批。
- 签名未做（无证书），当前包等同 unsigned Preview（W21 处理）。
