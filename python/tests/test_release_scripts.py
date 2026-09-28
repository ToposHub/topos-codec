"""R7 发布机器测试：check_exports / dll_load_test / package_codec。

这些脚本承担 Windows DLL 加载测试与发布清单（机器/编译器/commit/哈希）
职责——CI 与本地门禁共用，因此按真实子进程调用验证（不 mock）。
库未构建时整体 skip。
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPTS = REPO / "native" / "scripts"


def _lib_path() -> Path | None:
    explicit = os.environ.get("TOPOS_CODEC_LIB")
    if explicit:
        p = Path(explicit)
        return p if p.is_file() else None
    for cand in (
            REPO / "native/build/debug",
            REPO / "native/build/release"):
        for name in ("libtopos_codec.dylib", "topos_codec.dylib",
                     "libtopos_codec.so", "topos_codec.so", "topos_codec.dll"):
            p = cand / name
            if p.is_file():
                return p
    return None


@pytest.fixture(scope="module")
def lib() -> Path:
    p = _lib_path()
    if p is None:
        pytest.skip("libtopos_codec 未构建；先运行 bash native/run_tests.sh")
    return p


@pytest.fixture(scope="module")
def lib_any_name(lib: Path) -> Path:
    """versioned dylib（libtopos_codec.0.1.0.dylib）也接受。"""
    return lib


def _run(script: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SCRIPTS / script), *map(str, args)],
        capture_output=True, text=True, timeout=300)


def _package_args(lib: Path) -> list[str]:
    if lib.suffix.lower() == ".dll":
        return ["--lib", str(lib), "--import-lib", str(lib.with_suffix(".lib"))]
    return ["--lib", str(lib)]


class TestCheckExports:
    def test_manifest_symbols_all_exported(self, lib_any_name: Path) -> None:
        proc = _run("check_exports.py", lib_any_name)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        assert "公共符号表: OK" in proc.stdout

    def test_missing_symbol_reported(self, lib_any_name: Path,
                                     tmp_path: Path) -> None:
        bad = tmp_path / "bad_symbols.txt"
        bad.write_text("tc_abi_version\nthis_symbol_does_not_exist\n")
        proc = _run("check_exports.py", lib_any_name, bad)
        assert proc.returncode == 1
        assert "缺失公共符号: this_symbol_does_not_exist" in proc.stdout

    def test_rejects_non_binary(self, tmp_path: Path) -> None:
        junk = tmp_path / "junk.bin"
        junk.write_bytes(b"not a binary at all")
        proc = _run("check_exports.py", junk)
        assert proc.returncode == 1


class TestDllLoadTest:
    def test_roundtrip_via_ctypes(self, lib_any_name: Path) -> None:
        proc = _run("dll_load_test.py", lib_any_name)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        assert "codec roundtrip: OK" in proc.stdout
        assert "abi=2" in proc.stdout

    def test_missing_lib_fails_cleanly(self, tmp_path: Path) -> None:
        proc = _run("dll_load_test.py", tmp_path / "nope.dylib")
        assert proc.returncode == 1


class TestPackageCodec:
    def test_release_manifest_fields(self, lib_any_name: Path,
                                     tmp_path: Path) -> None:
        dist = tmp_path / "sdk"
        proc = _run("package_codec.py", *_package_args(lib_any_name),
                    "--dist", dist, "--build-dir",
                    lib_any_name.parents[1] / "debug")
        assert proc.returncode == 0, proc.stdout + proc.stderr
        manifest = json.loads((dist / "RELEASE-MANIFEST.json").read_text())
        # R7 任务 8：机器/编译器/commit/产物哈希齐备
        assert manifest["schema"] == "topos-codec-release/1"
        assert manifest["git_commit"]
        assert manifest["abi_version"] == "2"
        assert manifest["machine"]["system"]
        assert manifest["machine"]["machine"]
        assert manifest["compiler"]["version"] or manifest["compiler"]["path"]
        assert len(manifest["lib"]["sha256"]) == 64
        assert "load_test" in manifest and "OK" in manifest["load_test"]
        assert "export_test" in manifest and "公共符号表: OK" in manifest["export_test"]
        files = {e["path"] for e in manifest["files"]}
        # macOS versioned dylib 经符号链解析（--lib resolve）→ staged 为真名
        assert f"lib/{lib_any_name.resolve().name}" in files
        assert "include/topos_codec.h" in files
        assert "include/topos_image.h" in files
        assert "BUILDINFO.txt" in files
        assert "LICENSE" in files
        assert "NOTICE.txt" in files
        assert "docs/image/SDK.md" in files
        assert "docs/image/topos_image_file_spec_v0.md" in files
        assert "examples/dll_load_test.py" in files
        assert "lib/cmake/ToposCodec/ToposCodecConfig.cmake" in files
        assert "lib/cmake/ToposCodec/ToposCodecConfigVersion.cmake" in files
        if lib_any_name.suffix.lower() == ".dll":
            assert f"lib/{lib_any_name.with_suffix('.lib').name}" in files
        if lib_any_name.resolve().name.endswith(".dylib"):
            assert "lib/topos_codec.dylib" in files
            assert "lib/topos_codec.0.dylib" in files
        # 每个文件条目都带 sha256 且能复算一致
        import hashlib
        for entry in manifest["files"][:3]:
            data = (dist / entry["path"]).read_bytes()
            assert hashlib.sha256(data).hexdigest() == entry["sha256"]

    def test_buildinfo_records_machine_and_compiler(
            self, lib_any_name: Path, tmp_path: Path) -> None:
        dist = tmp_path / "sdk2"
        proc = _run("package_codec.py", *_package_args(lib_any_name),
                    "--dist", dist, "--skip-load-test")
        assert proc.returncode == 0, proc.stdout + proc.stderr
        info = (dist / "BUILDINFO.txt").read_text()
        assert "machine:" in info and "compiler:" in info
        assert "git:" in info and "abi:" in info

    @pytest.mark.skipif(sys.platform == "win32", reason="Windows C 示例由 release workflow 验证")
    def test_published_c_example_links_and_runs(
            self, lib_any_name: Path, tmp_path: Path) -> None:
        dist = tmp_path / "sdk-c"
        proc = _run("package_codec.py", *_package_args(lib_any_name), "--dist", dist)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        exe = tmp_path / "encode_decode"
        compiler = subprocess.run(
            ["cc", str(dist / "examples" / "encode_decode.c"),
             "-I", str(dist / "include"),
             str(dist / "lib" / lib_any_name.resolve().name),
             f"-Wl,-rpath,{dist / 'lib'}", "-o", str(exe)],
            capture_output=True, text=True, timeout=60,
        )
        assert compiler.returncode == 0, compiler.stdout + compiler.stderr
        run = subprocess.run(
            [str(exe), str(tmp_path / "demo.mov")],
            cwd=tmp_path, capture_output=True, text=True, timeout=60,
        )
        assert run.returncode == 0, run.stdout + run.stderr
        assert "PASS" in run.stdout

    def test_prebuilt_sdk_cmake_consumer(
            self, lib_any_name: Path, tmp_path: Path) -> None:
        dist = tmp_path / "sdk-cmake"
        proc = _run("package_codec.py", *_package_args(lib_any_name),
                    "--dist", dist)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        consumer = _run("sdk_consumer_smoke.py", dist)
        assert consumer.returncode == 0, consumer.stdout + consumer.stderr
        assert "SDK CMake 消费者: OK" in consumer.stdout

    def test_prebuilt_python_ctypes_example(
            self, lib_any_name: Path, tmp_path: Path) -> None:
        dist = tmp_path / "sdk-python"
        proc = _run("package_codec.py", *_package_args(lib_any_name),
                    "--dist", dist, "--skip-load-test")
        assert proc.returncode == 0, proc.stdout + proc.stderr
        example = subprocess.run(
            [sys.executable, str(dist / "examples" / "dll_load_test.py"),
             str(dist / "lib" / lib_any_name.resolve().name)],
            cwd=tmp_path, capture_output=True, text=True, timeout=60,
        )
        assert example.returncode == 0, example.stdout + example.stderr
        assert "codec roundtrip: OK" in example.stdout

    def test_windows_dll_requires_import_library(self, tmp_path: Path) -> None:
        dll = tmp_path / "topos_codec.dll"
        dll.write_bytes(b"fixture")
        proc = _run("package_codec.py", "--lib", dll, "--dist", tmp_path / "sdk")
        assert proc.returncode == 1
        assert "--import-lib" in proc.stdout


def test_open_source_package_imports_without_topos_application(
        lib_any_name: Path, tmp_path: Path) -> None:
    """发布包必须能从独立目录导入并加载视频与图片 SDK。"""
    package = tmp_path / "topos-codec"
    proc = _run("package_opensource.py", "--dest", package)
    assert proc.returncode == 0, proc.stdout + proc.stderr

    env = os.environ.copy()
    env["PYTHONPATH"] = str(package / "python")
    env["TOPOS_CODEC_LIB"] = str(lib_any_name.resolve())
    smoke = subprocess.run(
        [sys.executable, "-c", "\n".join((
            "from topos_codec import ToposCodec, ToposImageCodec",
            "from topos_codec.decode_worker_budget import DECODE_WORKER_BUDGET",
            "assert ToposCodec().abi_version == 2",
            "assert ToposImageCodec().status_message(0)",
            "assert DECODE_WORKER_BUDGET is not None",
        ))],
        cwd=tmp_path,
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert smoke.returncode == 0, smoke.stdout + smoke.stderr


def test_extracted_repository_can_package_native_sdk(
        lib_any_name: Path, tmp_path: Path) -> None:
    """抽取后的 native/ 布局仍能独立生成可使用的 SDK。"""
    package = tmp_path / "topos-codec"
    proc = _run("package_opensource.py", "--dest", package)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    dist = tmp_path / "sdk"
    build = subprocess.run(
        [sys.executable, str(package / "native" / "scripts" / "package_codec.py"),
         *_package_args(lib_any_name), "--dist", str(dist)],
        cwd=tmp_path, capture_output=True, text=True, timeout=120,
    )
    assert build.returncode == 0, build.stdout + build.stderr
    assert (dist / "docs" / "image" / "SDK.md").is_file()
    consumer = subprocess.run(
        [sys.executable, str(package / "native" / "scripts" / "sdk_consumer_smoke.py"),
         str(dist)], cwd=tmp_path, capture_output=True, text=True, timeout=120,
    )
    assert consumer.returncode == 0, consumer.stdout + consumer.stderr


def test_open_source_package_builds_installable_wheel(tmp_path: Path) -> None:
    """README 的 pip 安装路径须产生包含绑定和许可的实际 wheel。"""
    pytest.importorskip("wheel")
    package = tmp_path / "topos-codec"
    proc = _run("package_opensource.py", "--dest", package)
    assert proc.returncode == 0, proc.stdout + proc.stderr

    wheels = tmp_path / "wheels"
    build = subprocess.run(
        [sys.executable, "-m", "pip", "wheel", "--no-deps",
         "--no-build-isolation", "--wheel-dir", str(wheels), str(package)],
        capture_output=True, text=True, timeout=120,
        env={**os.environ, "PIP_NO_INDEX": "1"},
    )
    assert build.returncode == 0, build.stdout + build.stderr
    import zipfile
    wheel_path = next(wheels.glob("topos_codec-*.whl"))
    with zipfile.ZipFile(wheel_path) as archive:
        names = archive.namelist()
    assert "topos_codec/topos_meta.py" in names
    assert "topos_codec/topos_image_binding.py" in names
    assert any(name.endswith("/LICENSE") for name in names)
    assert any(name.endswith("/NOTICE") for name in names)


def test_open_source_binary_wheel_loads_without_repository(
        lib_any_name: Path, tmp_path: Path) -> None:
    """内嵌动态库的 wheel 必须带平台标记，并在仓库外可直接加载。"""
    pytest.importorskip("wheel")
    package = tmp_path / "topos-codec"
    proc = _run("package_opensource.py", "--dest", package,
                "--include-lib", "--lib", lib_any_name)
    assert proc.returncode == 0, proc.stdout + proc.stderr

    wheels = tmp_path / "wheels"
    build = subprocess.run(
        [sys.executable, "-m", "pip", "wheel", "--no-deps",
         "--no-build-isolation", "--wheel-dir", str(wheels), str(package)],
        capture_output=True, text=True, timeout=120,
        env={**os.environ, "PIP_NO_INDEX": "1"},
    )
    assert build.returncode == 0, build.stdout + build.stderr
    import zipfile
    wheel_path = next(wheels.glob("topos_codec-*.whl"))
    assert "-py3-none-any.whl" not in wheel_path.name
    with zipfile.ZipFile(wheel_path) as archive:
        names = archive.namelist()
        metadata = archive.read(next(n for n in names if n.endswith("/WHEEL"))).decode()
    assert "Root-Is-Purelib: false" in metadata
    assert any("topos_codec/lib/topos_codec" in n for n in names)

    target = tmp_path / "site"
    install = subprocess.run(
        [sys.executable, "-m", "pip", "install", "--no-deps",
         "--target", str(target), str(wheel_path)],
        capture_output=True, text=True, timeout=120,
        env={**os.environ, "PIP_NO_INDEX": "1"},
    )
    assert install.returncode == 0, install.stdout + install.stderr
    env = os.environ.copy()
    env.pop("TOPOS_CODEC_LIB", None)
    env["PYTHONPATH"] = str(target)
    smoke = subprocess.run(
        [sys.executable, "-c", "from topos_codec import ToposCodec, ToposImageCodec; "
         "assert ToposCodec().abi_version == 2; "
         "assert ToposImageCodec().status_message(0)"],
        cwd=tmp_path, env=env, capture_output=True, text=True, timeout=60,
    )
    assert smoke.returncode == 0, smoke.stdout + smoke.stderr


def test_open_source_binary_package_rejects_missing_library(tmp_path: Path) -> None:
    package = tmp_path / "topos-codec"
    proc = _run("package_opensource.py", "--dest", package,
                "--include-lib", "--lib", tmp_path / "missing.dylib")
    assert proc.returncode == 1
    assert "--include-lib" in proc.stderr
    assert not package.exists()


# ── R8 复审 C-1/C-2 回归：合成 PE32+ 与 fat/universal 二进制 ──

def _build_synthetic_pe(exports: list) -> bytes:
    """最小可解析 PE32+ DLL：.text + .edata（导出表按 winnt.h 布局）。

    只构造 check_exports.py 解析所需的字段（MZ/e_lfanew/PE 签名/COFF
    nsec+optsize/PE32+ magic/数据目录[0]/两张 section 头），其余以零填充。
    """
    import struct

    n = len(exports)
    names_blob = b"".join(x.encode() + b"\x00" for x in exports)
    dll_name = b"synth.dll\x00"

    edata_raw_off, edata_rva = 0x500, 0x2000
    name_str_off = edata_raw_off
    dll_name_off = name_str_off + len(names_blob)
    exp_dir_off = dll_name_off + len(dll_name)
    addr_tbl_off = exp_dir_off + 40
    names_tbl_off = addr_tbl_off + 4 * n
    total_edata = names_tbl_off + 4 * n - edata_raw_off

    def rva(rel: int) -> int:
        return edata_rva + rel

    exp_dir = struct.pack(
        "<IIHHIIIIIII",
        0, 0, 0, 0, rva(dll_name_off - edata_raw_off),
        1, n, n,
        rva(addr_tbl_off - edata_raw_off), rva(names_tbl_off - edata_raw_off),
        rva(names_tbl_off - edata_raw_off))
    addr_tbl = struct.pack("<" + "I" * n, *([0x1000] * n))
    name_offsets = []          # 每个导出名字符串在 .edata 内的字节偏移
    cur = name_str_off
    for x in exports:
        name_offsets.append(cur)
        cur += len(x.encode()) + 1
    names_tbl = struct.pack(
        "<" + "I" * n,
        *[rva(off - edata_raw_off) for off in name_offsets])

    buf = bytearray(b"\x00" * 0x500)
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 0x3C, 0x40)
    buf[0x40:0x44] = b"PE\x00\x00"
    struct.pack_into("<HHIIIHH", buf, 0x44,
                     0x8664, 2, 0, 0, 0, 240, 0x2022)
    struct.pack_into("<H", buf, 0x40 + 24, 0x20B)  # PE32+ magic
    # 数据目录[0]（导出表）指向 .edata 内的导出目录（非段起始的名字串区）
    struct.pack_into("<II", buf, 0x40 + 24 + 112,
                     rva(exp_dir_off - edata_raw_off), total_edata)

    def sec_hdr(name, vsize, vaddr, rawsize, rawoff):
        return name.encode().ljust(8, b"\x00") + struct.pack(
            "<IIIIIIHHI", vsize, vaddr, rawsize, rawoff, 0, 0, 0, 0, 0)

    sec_off = 0x40 + 24 + 240
    buf[sec_off:sec_off + 40] = sec_hdr(".text", 0x10, 0x1000, 0x10, 0x400)
    buf[sec_off + 40:sec_off + 80] = sec_hdr(
        ".edata", total_edata, 0x2000, total_edata, edata_raw_off)
    buf[0x400:0x410] = b"\x90" * 0x10
    buf[edata_raw_off:] = (names_blob + dll_name + exp_dir
                           + addr_tbl + names_tbl)
    return bytes(buf)


class TestR8SyntheticFormats:
    """R8 独立复审 C-1/C-2：PE 导出表字段与 fat 首 slice 偏移回归。"""

    def test_pe_exports_parsed(self, tmp_path: Path) -> None:
        dll = tmp_path / "synth.dll"
        want = ["tc_abi_version", "tc_version", "tc_frame_encode"]
        dll.write_bytes(_build_synthetic_pe(want))
        symfile = tmp_path / "want.txt"
        symfile.write_text("\n".join(want) + "\n")
        proc = _run("check_exports.py", dll, symfile)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        assert "公共符号表: OK" in proc.stdout
        assert "导出 3 个" in proc.stdout

    def test_pe_missing_symbol_detected(self, tmp_path: Path) -> None:
        dll = tmp_path / "synth2.dll"
        dll.write_bytes(_build_synthetic_pe(["tc_abi_version"]))
        bad = tmp_path / "want.txt"
        bad.write_text("tc_abi_version\ntc_missing_one\n")
        proc = _run("check_exports.py", dll, bad)
        assert proc.returncode == 1
        assert "缺失公共符号: tc_missing_one" in proc.stdout

    def test_fat_first_slice_offset_honored(self, lib_any_name: Path,
                                            tmp_path: Path) -> None:
        """fat_arch.offset 含对齐填充（R8-C2：曾硬编码 8+nfat*20 必错）。"""
        import struct
        thin = lib_any_name.read_bytes()
        slice_off = 0x4000  # 2^14 对齐——不紧跟 fat 头
        fat = bytearray()
        fat += struct.pack(">I", 0xCAFEBABE)
        fat += struct.pack(">I", 1)
        # cputype=x86_64(0x01000007), cpusubtype=3, offset, size, align=14
        fat += struct.pack(">iiIII", 0x01000007, 3, slice_off, len(thin), 14)
        fat += b"\x00" * (slice_off - len(fat))
        fat += thin
        fatlib = tmp_path / "lib_fat.dylib"
        fatlib.write_bytes(bytes(fat))
        proc = _run("check_exports.py", fatlib)
        assert proc.returncode == 0, proc.stdout + proc.stderr
