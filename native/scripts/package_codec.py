#!/usr/bin/env python3
"""package_codec.py — 跨平台 codec 发布打包 + 机器/编译器/commit/哈希清单（R7）。

Windows DLL / Linux so / macOS dylib 共用：staging SDK 骨架 → BUILDINFO →
加载测试（dll_load_test.py）→ RELEASE-MANIFEST.json（含每个文件的
sha256/size、宿主 machine、编译器版本、git commit）。这是 R7「所有发布
job 保留机器、编译器、commit 和产物哈希」的单一实现——build_sdk.sh 的
完整打包路径与 CI release job 都经它出清单。

用法：
    python package_codec.py --lib <built-lib> --dist <out-dir> \
        [--import-lib <windows-import-lib>] [--build-dir <cmake-build-dir>] \
        [--skip-load-test]

退出码：0 = 打包+加载测试通过；1 = 任何一步失败。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent          # native/topos_codec/scripts
CODEC = HERE.parent                              # native/topos_codec 或独立仓库 native
ROOT = CODEC.parent.parent if CODEC.name == "topos_codec" else CODEC.parent


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _git_commit() -> str:
    try:
        out = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=10)
        return out.stdout.strip() if out.returncode == 0 else "unknown"
    except Exception:
        return "unknown"


def _version() -> str:
    hdr = (CODEC / "include" / "topos_codec_version.h").read_text()
    def grab(name: str) -> str:
        m = re.search(rf"#define\s+{name}\s+(\d+)", hdr)
        return m.group(1) if m else "0"
    return ".".join(grab(f"TOPOS_CODEC_VERSION_{p}") for p in ("MAJOR", "MINOR", "PATCH"))


def _abi() -> str:
    hdr = (CODEC / "include" / "topos_codec.h").read_text()
    m = re.search(r"#define\s+TOPOS_CODEC_ABI_VERSION\s+(\d+)", hdr)
    return m.group(1) if m else "unknown"


def _compiler(build_dir: Path | None) -> dict:
    info: dict = {"path": None, "version": None, "dumpmachine": None}
    cc = None
    if build_dir:
        # VS 多配置生成器不把 CMAKE_C_COMPILER 写进 CMakeCache.txt；编译器
        # 事实（含 ID/版本）在 CMakeFiles/<cmake-ver>/CMakeCCompiler.cmake。
        # 该文件同时覆盖单配置生成器，优先解析；MSVC 版本号由此获得
        # （cl.exe 无 --version，直接查询只有 usage 退出码）。调用方传入的
        # build_dir 可能是顶层（Unix 惯例 <build>/debug）也可能是 VS 生成器
        # 的配置子目录（<build>/Debug）——两处都探测。
        probe_dirs = [build_dir, build_dir.parent]
        for probe in probe_dirs:
            for ccmake in sorted((probe / "CMakeFiles").glob("*/CMakeCCompiler.cmake")):
                cid = None
                for line in ccmake.read_text(errors="replace").splitlines():
                    m = re.match(r'set\(CMAKE_C_COMPILER "([^"]+)"\)', line.strip())
                    if m and info["path"] is None:
                        info["path"] = m.group(1)
                        cc = m.group(1)
                    m = re.match(r'set\(CMAKE_C_COMPILER_ID "([^"]+)"\)', line.strip())
                    if m:
                        cid = m.group(1)
                    m = re.match(r'set\(CMAKE_C_COMPILER_VERSION "([^"]+)"\)', line.strip())
                    if m and info["version"] is None:
                        info["version"] = (f"{cid} {m.group(1)}" if cid else m.group(1))
            cache = probe / "CMakeCache.txt"
            if cc is None and cache.is_file():
                for line in cache.read_text(errors="replace").splitlines():
                    if line.startswith("CMAKE_C_COMPILER:"):
                        cc = line.split("=", 1)[1].strip()
                        break
            if cc is not None:
                break
    if not cc:
        cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cl")
    if not cc:
        return info
    if info["path"] is None:
        info["path"] = cc
    for args in (["--version"], ["-dumpmachine"]):
        try:
            out = subprocess.run([cc, *args], capture_output=True, text=True,
                                  timeout=15, errors="replace")
            if out.returncode == 0 and out.stdout.strip():
                key = "version" if args == ["--version"] else "dumpmachine"
                if info[key] is None:
                    info[key] = out.stdout.strip().splitlines()[0]
        except Exception:
            pass
    return info


def stage(lib: Path, dist: Path, import_lib: Path | None = None) -> list[Path]:
    (dist / "include").mkdir(parents=True, exist_ok=True)
    (dist / "lib").mkdir(parents=True, exist_ok=True)
    (dist / "examples").mkdir(parents=True, exist_ok=True)
    files = [
        (CODEC / "include" / "topos_codec.h", dist / "include"),
        (CODEC / "include" / "topos_image.h", dist / "include"),
        (CODEC / "include" / "topos_codec_version.h", dist / "include"),
        (lib, dist / "lib"),
        (CODEC / "examples" / "encode_decode.c", dist / "examples"),
        (CODEC / "docs" / "SDK.md", dist),
        (CODEC / "scripts" / "dll_load_test.py", dist / "examples"),
        (CODEC / "LICENSE", dist),
        (CODEC / "NOTICE.txt", dist),
        (CODEC / "docs" / "bitstream_spec_v1.md", dist / "docs"),
        (CODEC / "docs" / "bitstream_spec_v7r3_microgop.md", dist / "docs"),
        (CODEC / "docs" / "container_spec_v1.md", dist / "docs"),
        (CODEC / "docs" / "capability_manifest.json", dist / "docs"),
        (ROOT / "docs" / "image" / "SDK.md", dist / "docs" / "image"),
        (ROOT / "docs" / "image" / "topos_image_file_spec_v0.md", dist / "docs" / "image"),
        (ROOT / "docs" / "image" / "capability_manifest.json", dist / "docs" / "image"),
    ]
    if import_lib is not None:
        files.append((import_lib, dist / "lib"))
    staged = []
    for src, dst_dir in files:
        if not src.is_file():
            raise FileNotFoundError(f"SDK 必需文件不存在: {src}")
        dst_dir.mkdir(parents=True, exist_ok=True)
        dst = dst_dir / src.name
        if dst.exists() and dst.resolve() == src.resolve():
            staged.append(dst)  # 已在位（build_sdk 路径：lib 已 staged/签名）
            continue
        shutil.copy2(src, dst)
        staged.append(dst)
    staged.extend(_stage_library_aliases(lib, dist))
    staged.extend(_write_cmake_package(lib, dist, import_lib))
    return staged


def _write_cmake_package(lib: Path, dist: Path,
                         import_lib: Path | None) -> list[Path]:
    """让预编译 SDK 可以直接作为 CMAKE_PREFIX_PATH 使用。"""
    cmake_dir = dist / "lib" / "cmake" / "ToposCodec"
    cmake_dir.mkdir(parents=True, exist_ok=True)
    config = cmake_dir / "ToposCodecConfig.cmake"
    version = cmake_dir / "ToposCodecConfigVersion.cmake"
    import_line = (
        f'    IMPORTED_IMPLIB "${{_TOPOS_CODEC_SDK_ROOT}}/lib/{import_lib.name}"\n'
        if import_lib is not None else ""
    )
    config.write_text(
        'get_filename_component(_TOPOS_CODEC_SDK_ROOT '
        '"${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)\n'
        'if(NOT TARGET ToposCodec::topos_codec)\n'
        '  add_library(ToposCodec::topos_codec SHARED IMPORTED)\n'
        '  set_target_properties(ToposCodec::topos_codec PROPERTIES\n'
        '    INTERFACE_INCLUDE_DIRECTORIES "${_TOPOS_CODEC_SDK_ROOT}/include"\n'
        f'    IMPORTED_LOCATION "${{_TOPOS_CODEC_SDK_ROOT}}/lib/{lib.name}"\n'
        f'{import_line}'
        '  )\n'
        'endif()\n'
        f'set(ToposCodec_VERSION "{_version()}")\n',
        encoding="utf-8",
    )
    version.write_text(
        f'set(PACKAGE_VERSION "{_version()}")\n'
        'if(PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)\n'
        '  set(PACKAGE_VERSION_COMPATIBLE FALSE)\n'
        'elseif(PACKAGE_FIND_VERSION_MAJOR EQUAL '
        f'{_version().split(".", 1)[0]})\n'
        '  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n'
        'else()\n'
        '  set(PACKAGE_VERSION_COMPATIBLE FALSE)\n'
        'endif()\n'
        'if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)\n'
        '  set(PACKAGE_VERSION_EXACT TRUE)\n'
        'endif()\n',
        encoding="utf-8",
    )
    return [config, version]


def _stage_library_aliases(lib: Path, dist: Path) -> list[Path]:
    """保留 CMake SONAME/install_name 对应的文件名，供外部程序运行时加载。"""
    name = lib.name
    aliases: list[str] = []
    if ".so." in name:
        base, version = name.split(".so.", 1)
        major = version.split(".", 1)[0]
        if major.isdigit():
            aliases = [f"{base}.so.{major}", f"{base}.so"]
    else:
        match = re.fullmatch(r"((?:lib)?topos_codec)\.(\d+)\.(\d+(?:\.\d+)*)\.dylib", name)
        if match:
            base, major = match.group(1), match.group(2)
            aliases = [f"{base}.{major}.dylib", f"{base}.dylib"]
    source = dist / "lib" / name
    staged: list[Path] = []
    for alias in aliases:
        target = dist / "lib" / alias
        if target != source:
            shutil.copy2(source, target)
            staged.append(target)
    return staged


def write_buildinfo(dist: Path, lib: Path, compiler: dict, commit: str) -> None:
    un = platform.uname()
    (dist / "BUILDINFO.txt").write_text(
        f"name:      topos-codec-sdk\n"
        f"version:   {_version()}\n"
        f"git:       {commit}\n"
        f"built:     {datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}\n"
        f"host:      {compiler.get('dumpmachine') or un.machine}\n"
        f"machine:   {un.system} {un.release} {un.machine}\n"
        f"compiler:  {compiler.get('version') or compiler.get('path') or 'unknown'}\n"
        f"lib:       {lib.name}\n"
        f"abi:       v{_abi()}（TOPOS_CODEC_ABI_VERSION，从头文件解析——不手写）\n"
        f"known-limitations: 音频轨可封装，AAC/LPCM 音频编码由宿主提供；应用 sized 路径 1080p 为离线/近实时档、\n"
        f"  4K=代理+离线交付（benchmark_protocol §9）；Windows/MSVC CI job 已 required 化，\n"
        f"  首绿证据以真实 runner 记录为准（ADR-C010/C025）。\n",
        encoding="utf-8")


def write_manifest(dist: Path, lib: Path, compiler: dict, commit: str,
                   load_test: str, export_test: str) -> Path:
    entries = []
    for p in sorted(dist.rglob("*")):
        if p.is_file() and p.name != "RELEASE-MANIFEST.json":
            entries.append({"path": p.relative_to(dist).as_posix(),
                            "bytes": p.stat().st_size,
                            "sha256": _sha256(p)})
    manifest = {
        "schema": "topos-codec-release/1",
        "created_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "version": _version(),
        "git_commit": commit,
        "abi_version": _abi(),
        # R8/S-5：node（主机名）脱敏——发布物不向下游暴露内网名
        "machine": {**{k: getattr(platform.uname(), k) for k in
                       ("system", "release", "version", "machine")},
                    "node": "(redacted)"},
        "compiler": compiler,
        "lib": {"name": lib.name, "sha256": _sha256(dist / "lib" / lib.name)},
        "export_test": export_test,
        "load_test": load_test,
        "files": entries,
    }
    out = dist / "RELEASE-MANIFEST.json"
    out.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                   encoding="utf-8")
    return out


def run_export_test(lib: Path) -> str:
    proc = subprocess.run(
        [sys.executable, str(HERE / "check_exports.py"), str(lib)],
        capture_output=True, text=True, timeout=120)
    detail = (proc.stdout + proc.stderr).strip().splitlines()
    if proc.returncode != 0:
        print("\n".join(detail) or "公共符号表检查失败（无输出）")
        sys.exit(1)
    return detail[-1] if detail else "ok"


def run_load_test(lib: Path) -> str:
    proc = subprocess.run(
        [sys.executable, str(HERE / "dll_load_test.py"), str(lib)],
        capture_output=True, text=True, timeout=300)
    detail = (proc.stdout + proc.stderr).strip().splitlines()
    if proc.returncode != 0:
        print("\n".join(detail) or "加载测试失败（无输出）")
        sys.exit(1)
    return detail[-1] if detail else "ok"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", required=True, type=Path)
    ap.add_argument("--dist", required=True, type=Path)
    ap.add_argument("--import-lib", type=Path, default=None,
                    help="Windows DLL 对应的 .lib 导入库；发布 DLL 时必需")
    ap.add_argument("--build-dir", type=Path, default=None)
    ap.add_argument("--skip-load-test", action="store_true")
    args = ap.parse_args()

    lib = args.lib.resolve()
    if not lib.is_file():
        print(f"库不存在: {lib}")
        return 1
    import_lib = args.import_lib.resolve() if args.import_lib else None
    if lib.suffix.lower() == ".dll" and import_lib is None:
        print("Windows SDK 缺少 --import-lib，C/C++ 项目将无法链接 DLL")
        return 1
    if import_lib is not None and not import_lib.is_file():
        print(f"导入库不存在: {import_lib}")
        return 1
    dist = args.dist.resolve()
    dist.mkdir(parents=True, exist_ok=True)

    stage(lib, dist, import_lib)
    compiler = _compiler(args.build_dir)
    commit = _git_commit()
    write_buildinfo(dist, lib, compiler, commit)
    export_test = run_export_test(dist / "lib" / lib.name)
    load = ("skipped" if args.skip_load_test else run_load_test(dist / "lib" / lib.name))
    manifest = write_manifest(dist, lib, compiler, commit, load, export_test)
    print(f"发布包就绪: {dist}")
    print(f"清单: {manifest}")
    print(f"加载测试: {load}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
