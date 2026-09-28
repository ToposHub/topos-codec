#!/usr/bin/env python3
"""组装 Topos Video Codec 开源发布目录（单命令产出可发布仓库）。

从本仓库抽取 codec 自包含闭环（native 库 + Python 绑定/编码器封装 +
文档 + 绑定测试 + CI 矩阵），重写 import 与路径引用后输出到独立目录。
产出目录可直接 `git init` 发布，或从产出目录执行 `pip install .` 使用。

布局（发布仓库根 = DEST）：
    DEST/
    ├── LICENSE NOTICE README.md pyproject.toml
    ├── Topos_Codec_Whitepaper.md / Topos_Codec_白皮书.md   ← 白皮书（门面文件）
    ├── native/                  ← native/topos_codec（剔除 build/dist/spikes）
    ├── python/topos_codec/      ← 绑定包（import 重写为包内相对引用）
    │   └── lib/                 ← --include-lib 时放入已构建动态库
    ├── python/tests/            ← 绑定测试（import/路径重写）
    ├── docs/                    ← native/topos_codec/docs（规范/ADR/基准报告）
    └── .github/workflows/       ← codec-*.yml（路径重写 native/topos_codec→native）

用法：
    python3 native/topos_codec/scripts/package_opensource.py \
        [--dest DIR] [--include-lib] [--lib PATH] [--keep]

默认 DEST = native/topos_codec/dist/opensource/topos-codec（已在 .gitignore）。
"""
from __future__ import annotations

import argparse
import py_compile
import re
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
CODEC_DIR = REPO_ROOT / "native" / "topos_codec"
DEFAULT_DEST = CODEC_DIR / "dist" / "opensource" / "topos-codec"

LIB_NAMES = (
    "topos_codec.dylib", "libtopos_codec.dylib",
    "topos_codec.so", "libtopos_codec.so",
    "topos_codec.dll",
)

# 发布包内的 Python 模块（源路径相对仓库根 → 包内文件名）
PACKAGE_FILES = {
    "src/shared/codec/topos_binding.py": "topos_binding.py",
    "src/shared/codec/topos_profiles.py": "topos_profiles.py",
    "src/shared/codec/color_metadata_config.py": "color_metadata_config.py",
    "src/shared/codec/slice_threads_policy.py": "slice_threads_policy.py",
    "src/shared/codec/topos_image_binding.py": "topos_image_binding.py",
    "src/shared/codec/topos_meta.py": "topos_meta.py",
    "src/shared/codec/decode_worker_budget.py": "decode_worker_budget.py",
    "src/shared/export/topos_encoder.py": "topos_encoder.py",
    "src/shared/export/topos_rate_control.py": "topos_rate_control.py",
}

# 包内模块：绝对 import → 包内相对 import
PACKAGE_REWRITES = [
    # 发布仓库的 native 树在 <root>/native/（无 topos_codec 层），仓库构建
    # 候选须同步重写，与 TEST_REWRITES 后的测试期望一致
    ('root / "native" / "topos_codec" / "build"', 'root / "native" / "build"'),
    ("from src.shared.codec.", "from ."),
    ("from src.shared.export.topos_encoder", "from .topos_encoder"),
    ("from src.shared.export.topos_rate_control", "from .topos_rate_control"),
    # 兜底：不应残留
    ("src.shared.codec", "topos_codec"),
    ("src.shared.export", "topos_codec"),
]

# 测试/示例/工作流：绝对引用 → 发布仓库布局
TEST_REWRITES = [
    ("src.shared.export.topos_encoder", "topos_codec.topos_encoder"),
    ("src.shared.export.topos_rate_control", "topos_codec.topos_rate_control"),
    ("src.shared.codec", "topos_codec"),
    # 模块 docs 路径先于通用 native 规则（更长前缀优先）
    ("native/topos_codec/docs/", "docs/"),
    ('"native" / "topos_codec" / "docs"', '"docs"'),
    ("native/topos_codec/", "native/"),
    ("native/topos_codec", "native"),
    ('"docs" / "codec"', '"docs"'),
    ('"native" / "topos_codec"', '"native"'),
    ('"src" / "shared" / "codec" / "topos_binding.py"',
     '"python" / "topos_codec" / "topos_binding.py"'),
    ("docs/codec/", "docs/"),
    ("docs/codec", "docs"),
]

EXAMPLE_REWRITES = [
    # native/examples/ → 发布根：示例内嵌 sys.path 指向 <发布根>/python
    ('"..", "..", "..", "src",', '"..", "..", "python",'),
    ("from shared.codec import", "from topos_codec import"),
    ("from shared.codec.", "from topos_codec."),
    # 仓库内文档口径路径 → 发布布局
    ("native/topos_codec/", "native/"),
    ("src/shared/codec/", "python/topos_codec/"),
]

WORKFLOW_REWRITES = [
    ("native/topos_codec/", "native/"),
    ("native/topos_codec", "native"),
    ("docs/codec/", "docs/"),
]

NATIVE_EXCLUDE_DIRS = {"build", "dist", "spikes", "__pycache__"}

# 内部计划/审计/路线文档不随开源发布走：既避免商业策略泄露，也移除文件名
# 直接含竞品商标的内部文档（合规口径见 docs/codec 白皮书的商标声明）。
# 报告类文档（bench_*/baseline/quality/protocol/ADR/spec/SDK/manifest）保留。
DOCS_EXCLUDE_PATTERN = re.compile(r"(计划|审计|复审|路线|整改)")


def apply_rewrites(text: str, rules) -> str:
    for old, new in rules:
        text = text.replace(old, new)
    return text


def copy_native(dest: Path) -> int:
    """复制 native 树（剔除构建产物/调研代码），返回文件数。"""
    count = 0
    for src in sorted(CODEC_DIR.rglob("*")):
        rel = src.relative_to(CODEC_DIR)
        if any(part in NATIVE_EXCLUDE_DIRS for part in rel.parts):
            continue
        target = dest / "native" / rel
        if src.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            if src.name == "topos_roundtrip.py":
                target.write_text(
                    apply_rewrites(src.read_text(encoding="utf-8"), EXAMPLE_REWRITES),
                    encoding="utf-8")
            else:
                shutil.copy2(src, target)
            count += 1
    return count


def copy_package(dest: Path) -> None:
    pkg_dir = dest / "python" / "topos_codec"
    pkg_dir.mkdir(parents=True, exist_ok=True)
    (dest / "python" / "topos_codec" / "lib").mkdir(exist_ok=True)
    (dest / "python" / "topos_codec" / "lib" / ".gitkeep").write_text("")
    for src_rel, name in PACKAGE_FILES.items():
        src = REPO_ROOT / src_rel
        text = apply_rewrites(src.read_text(encoding="utf-8"), PACKAGE_REWRITES)
        assert "src.shared" not in text, f"{src_rel} 重写后仍残留 src.shared 引用"
        (pkg_dir / name).write_text(text, encoding="utf-8")


def copy_tests(dest: Path) -> None:
    src_dir = REPO_ROOT / "tests" / "native"
    out_dir = dest / "python" / "tests"
    out_dir.mkdir(parents=True, exist_ok=True)
    # 排除依赖主仓库工具链（tools/measure_topos_native_domains）的测试：
    # 该工具不属于 SDK 发布物，包内无对应模块，复制必然 ImportError
    packaged_exclude = {
        # 依赖主仓库 tools/ 质量度量链或 scipy（均非 SDK 发布物组成部分）
        "test_topos_native_domain_metrics.py",
        "test_topos_paired_quality.py",
        "test_topos_quality_gate_binding.py",
        "test_topos_quality_measure_metrics.py",
    }
    for src in sorted(src_dir.glob("test_*.py")):
        if src.name in packaged_exclude:
            continue
        text = apply_rewrites(src.read_text(encoding="utf-8"), TEST_REWRITES)
        (out_dir / src.name).write_text(text, encoding="utf-8")
    (out_dir / "conftest.py").write_text(
        "import os\nimport sys\n\n"
        "# 发布布局：python/tests/ → python/ 进入 sys.path，直接导入 topos_codec 包\n"
        "sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))\n",
        encoding="utf-8")


def copy_docs(dest: Path) -> int:
    """模块自带文档（native/topos_codec/docs）提升为发布根 docs/。"""
    src_dir = CODEC_DIR / "docs"
    out_dir = dest / "docs"
    out_dir.mkdir(parents=True, exist_ok=True)
    count = 0
    for src in sorted(src_dir.iterdir()):
        if src.suffix not in (".md", ".json"):
            continue
        if DOCS_EXCLUDE_PATTERN.search(src.stem):
            continue
        shutil.copy2(src, out_dir / src.name)
        count += 1
    image_dir = dest / "docs" / "image"
    image_dir.mkdir(parents=True, exist_ok=True)
    for name in ("SDK.md", "topos_image_file_spec_v0.md", "capability_manifest.json"):
        shutil.copy2(REPO_ROOT / "docs" / "image" / name, image_dir / name)
        count += 1
    return count


def copy_workflows(dest: Path) -> int:
    src_dir = REPO_ROOT / ".github" / "workflows"
    out_dir = dest / ".github" / "workflows"
    out_dir.mkdir(parents=True, exist_ok=True)
    count = 0
    for src in sorted(src_dir.glob("codec-*.yml")):
        text = apply_rewrites(src.read_text(encoding="utf-8"), WORKFLOW_REWRITES)
        (out_dir / src.name).write_text(text, encoding="utf-8")
        count += 1
    return count


def write_generated(dest: Path) -> None:
    version_header = (CODEC_DIR / "include" / "topos_codec_version.h").read_text()
    version_parts = []
    for part in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"#define\s+TOPOS_CODEC_VERSION_{part}\s+(\d+)", version_header)
        if match is None:
            raise ValueError(f"缺少 TOPOS_CODEC_VERSION_{part}")
        version_parts.append(match.group(1))
    version = ".".join(version_parts)
    shutil.copy2(CODEC_DIR / "LICENSE", dest / "LICENSE")
    shutil.copy2(CODEC_DIR / "NOTICE.txt", dest / "NOTICE")
    # 白皮书放发布仓库根目录（门面文件，不淹没在 docs/ 里）
    for name in ("Topos_Codec_白皮书.md", "Topos_Codec_Whitepaper.md",
                 "Topos_Image_白皮书.md", "Topos_Image_Whitepaper.md"):
        shutil.copy2(CODEC_DIR / name, dest / name)
    (dest / "python" / "topos_codec" / "__init__.py").write_text(
        '"""Topos Video Codec — Python 绑定与编码器封装（开源发布包）。\n\n'
        '动态库定位：TOPOS_CODEC_LIB 环境变量 → 本包 lib/ 目录 → 仓库构建目录。\n'
        '"""\n'
        "from .topos_binding import (  # noqa: F401\n"
        "    TOPOS_CODEC_ABI_VERSION,\n"
        "    ToposCodec,\n"
        "    ToposCodecError,\n"
        "    ToposCpuFeatures,\n"
        "    ToposVersion,\n"
        "    load_library,\n"
        ")\n"
        "from .topos_profiles import (  # noqa: F401\n"
        "    TOPOS_PROFILE_TIERS,\n"
        "    available_topos_tiers,\n"
        "    default_topos_tier_id,\n"
        "    get_topos_tier,\n"
        "    get_topos_tier_or_none,\n"
        "    tier_pix_fmt_choices,\n"
        "    topos_capability_summary,\n"
        ")\n"
        "from .color_metadata_config import ColorMetadataConfig  # noqa: F401\n"
        "from .topos_rate_control import ToposRateFeedback  # noqa: F401\n"
        "from .slice_threads_policy import SLICE_THREADS_POLICY  # noqa: F401\n"
        "from .topos_encoder import ToposVideoEncoder, parse_topos_pix_fmt  # noqa: F401\n"
        "from .topos_image_binding import ToposImageCodec  # noqa: F401\n\n"
        '__all__ = [\n'
        '    "TOPOS_CODEC_ABI_VERSION", "ToposCodec", "ToposCodecError",\n'
        '    "ToposCpuFeatures", "ToposVersion", "load_library",\n'
        '    "TOPOS_PROFILE_TIERS", "available_topos_tiers", "default_topos_tier_id",\n'
        '    "get_topos_tier", "get_topos_tier_or_none", "tier_pix_fmt_choices",\n'
        '    "topos_capability_summary", "ColorMetadataConfig", "ToposRateFeedback",\n'
        '    "SLICE_THREADS_POLICY", "ToposVideoEncoder", "parse_topos_pix_fmt",\n'
        '    "ToposImageCodec",\n'
        ']\n',
        encoding="utf-8")
    (dest / "pyproject.toml").write_text(
        '[build-system]\n'
        'requires = ["setuptools>=77", "wheel"]\n'
        'build-backend = "setuptools.build_meta"\n\n'
        "[project]\n"
        'name = "topos-codec"\n'
        f'version = "{version}"\n'
        'description = "Topos Video Codec — zero-dependency intra-frame mezzanine codec '
        '(ctypes bindings and encoder wrapper)"\n'
        'readme = "README.md"\n'
        'requires-python = ">=3.10"\n'
        'license = "Apache-2.0"\n'
        'license-files = ["LICENSE", "NOTICE"]\n'
        'keywords = ["video", "codec", "mezzanine", "intra-frame", "prores"]\n'
        'dependencies = ["numpy>=1.22"]\n'
        "classifiers = [\n"
        '    "Programming Language :: C",\n'
        '    "Programming Language :: Python :: 3",\n'
        '    "Topic :: Multimedia :: Video :: Codecs",\n'
        "]\n\n"
        "[tool.setuptools]\n"
        'packages = ["topos_codec"]\n'
        'package-dir = {"" = "python"}\n\n'
        "[tool.setuptools.package-data]\n"
        'topos_codec = ["lib/*"]\n',
        encoding="utf-8")
    (dest / "README.md").write_text(
        "# Topos Video Codec\n\n"
        "A native C11 mezzanine video codec engine with intra profiles and "
        "an optional LP inter-frame tier (10/12-bit baseline, 16-bit extension; "
        "4:2:2 / 4:4:4 / GBR, optional alpha). "
        "Licensed under the **Apache License 2.0**.\n\n"
        "- **Video whitepaper**: [Topos_Codec_Whitepaper.md](Topos_Codec_Whitepaper.md) "
        "(English) · [Topos_Codec_白皮书.md](Topos_Codec_白皮书.md) (中文) — "
        "format overview, profile tiers, bitrate tables, performance\n"
        "- **Image whitepaper (.toos)**: [Topos_Image_Whitepaper.md](Topos_Image_Whitepaper.md) "
        "(English) · [Topos_Image_白皮书.md](Topos_Image_白皮书.md) (中文)\n"
        "- **Integration guide**: `docs/SDK.md` · bitstream specs: "
        "`docs/bitstream_spec_v1.md` / `docs/bitstream_spec_v2.md` · container: "
        "`docs/container_spec_v1.md` · ADRs: `docs/ADR-INDEX.md`\n\n"
        "## Profile family at a glance\n\n"
        "Six intra tiers named to map onto common mezzanine workflows. "
        "422 Proxy / LT / 422 / 422 HQ share one 4:2:2 bitstream profile "
        "(profile=3) and differ only in target bitrate and alpha policy; "
        "4444 (profile=5) and 4444 XQ (profile=6) are independent bitstream "
        "profiles. All tiers can optionally carry alpha (a8/a10/a12/a16).\n\n"
        "| Tier | Pixel format | Bit depth | ≈ 1080p25 | ≈ 2160p25 (4K) | Primary use |\n"
        "| --- | --- | --- | ---: | ---: | --- |\n"
        "| Topos 422 Proxy | YUV 4:2:2 | 10 | 31 Mbps | 126 Mbps | offline proxies, remote editing |\n"
        "| Topos 422 LT | YUV 4:2:2 | 10 | 64 Mbps | 256 Mbps | light intermediates, rough cuts |\n"
        "| Topos 422 *(default)* | YUV 4:2:2 | 10 | 97 Mbps | 389 Mbps | general editing, render caches |\n"
        "| Topos 422 HQ | YUV 4:2:2 / 4:4:4 / GBR | 10/12 | 143 Mbps | 571 Mbps | grading, intermediate mastering |\n"
        "| Topos 4444 | YUV/GBR 4:4:4 | 10/12 | 213 Mbps | 852 Mbps | VFX, motion graphics, compositing |\n"
        "| Topos 4444 XQ | YUV/GBR 4:4:4 | 12 | 351 Mbps | 1402 Mbps | HDR and multi-generation work |\n\n"
        "Bitrates are tier targets at 1080p25 / 2160p25, scaling linearly as "
        "`bitrate ≈ bpp × width × height × fps`; any tier can also be driven "
        "with a fixed QP (0–63) instead of a bitrate target. The full "
        "specification table (reference data-rate ranges, alpha budgets, "
        "bitstream profile bytes) is in the whitepaper §4.\n\n"
        "## Key features\n\n"
        "- **Intra profiles and optional LP tier** — frame-independent intra "
        "profiles support indexed random access; LP uses an IP-2 micro-GOP\n"
        "- **Deterministic rate control** — frame-level exact-QP search "
        "(sized mode) or fixed QP; byte-reproducible output pinned by frozen "
        "golden vectors\n"
        "- **V2 canonical VLC entropy coding** — 23–32% bitrate savings at "
        "equal quality over V1 Rice; self-describing bitstream, old files "
        "decode forever\n"
        "- **10/12-bit baseline plus 16-bit extension, 4:2:2 / 4:4:4 / GBR** "
        "— GBR pass-through avoids YUV "
        "round trips for RGB compositing sources; full/limited range, "
        "BT.709/BT.2020-class metadata in-band\n"
        "- **Runtime-dispatched SIMD** — AVX2 and NEON paths selected via "
        "CPU feature detection; slice-parallel encode and decode with "
        "bounded thread pool\n"
        "- **Zero third-party dependencies** — pure C11 + libc + pthread; "
        "first-class Linux / macOS (universal) / Windows (MSVC & MinGW) CI\n"
        "- **FFmpeg-friendly** — optional libavcodec patch decodes Topos .mov "
        "in FFmpeg/ffplay; MOV-derived container with FastStart\n\n"
        "## Topos Image (.toos) — still-image format on the same core\n\n"
        "`.toos` is a still-image intermediate format built on this same "
        "codec core: a TPIM envelope + one complete TPIC intra-frame packet, "
        "with pixel coding **100% shared with libtopos_codec** (the spec "
        "freezes a 'no second implementation' rule). GBR 4:4:4 pass-through, "
        "explicit three-state alpha, atomic writes, first-class render "
        "sequence support — and decoding 2.3× faster than PNG.\n\n"
        "| Tier (`toos encode --tier`) | Format | ≈MB/frame (1080p) | Use |\n"
        "| --- | --- | ---: | --- |\n"
        "| Topos 422 Low | YUV 4:2:2 10-bit | 1.11 | preview / review / proxies |\n"
        "| Topos 422 Medium | YUV 4:2:2 10-bit | 1.29 | smaller than PNG 8-bit on the same material |\n"
        "| Topos 422 High | YUV 4:2:2 10-bit | 1.71 | the PNG-anchored tier |\n"
        "| Topos 422 Ultra *(default)* | YUV 4:2:2 10-bit | 2.86 | visually lossless class |\n"
        "| Topos 444 High | GBR 4:4:4 12-bit | 5.10 | high-fidelity compositing |\n"
        "| Topos 444 Ultra | GBR 4:4:4 12-bit | 5.79 | 12-bit visually lossless class |\n\n"
        "Mathematically lossless output is `--qp 28` (10-bit) or a lossless "
        "container (PNG/EXR); full color-mode matrix, alpha semantics and "
        "sequence grammar are in the image whitepaper.\n\n"
        "## Quick start\n\n"
        "```bash\n"
        "# 1. Build the native library (or: bash native/run_tests.sh for the full gate)\n"
        "cmake -S native -B build/release -DCMAKE_BUILD_TYPE=Release\n"
        "cmake --build build/release -j\n\n"
        "# 2. Use the Python bindings (either way)\n"
        "TOPOS_CODEC_LIB=$(ls build/release/topos_codec*dylib* build/release/*.so 2>/dev/null | head -1) \\\n"
        "    PYTHONPATH=python python3 native/examples/topos_roundtrip.py\n"
        "# ...or drop the library into python/topos_codec/lib/ for configuration-free use:\n"
        "cp build/release/topos_codec* python/topos_codec/lib/\n"
        "pip install .\n"
        "```\n\n"
        "For a bundled platform wheel, build the native library first, then "
        "regenerate with `--include-lib` and run `python -m pip wheel .`. "
        "Each bundled wheel is for its build platform; source-only wheels "
        "require `TOPOS_CODEC_LIB` or a library in `topos_codec/lib/`.\n\n"
        "## Layout\n\n"
        "| Path | Contents |\n| --- | --- |\n"
        "| `native/` | C11 codec core (src/include/tests/tools/examples/scripts + CMake) |\n"
        "| `python/topos_codec/` | ctypes bindings, tier/capability declarations, high-level encoder wrapper |\n"
        "| `python/tests/` | binding tests (`pytest python/tests`) |\n"
        "| `docs/` | SDK guide, bitstream/container specs, ADRs, benchmark reports (whitepapers at repo root) |\n"
        "| `.github/workflows/` | Linux/macOS/Windows CI matrix |\n\n"
        "## Quality\n\n"
        "31 native unit suites, 7 frozen golden vector groups, fuzzing entry "
        "points with deterministic replay, and an ABI symbol manifest gate. "
        "One-shot gate: `bash native/run_tests.sh`.\n\n"
        "## Trademarks\n\n"
        "Topos Video Codec is an independent, originally developed format. It "
        "is not affiliated with, sponsored by, or endorsed by any other codec "
        "vendor. Apple, ProRes and QuickTime are trademarks of Apple Inc.; "
        "DNxHR is a trademark of Avid Technology, Inc.; DaVinci Resolve is a "
        "trademark of Blackmagic Design Pty Ltd. Those names appear in the "
        "documentation solely to identify and compare the respective "
        "technologies; comparative figures are measured against FFmpeg's "
        "open-source software implementations.\n\n"
        "## License\n\n"
        "Apache License 2.0 — see `LICENSE` and `NOTICE`.\n",
        encoding="utf-8")


def find_library(library: Path | None) -> Path | None:
    if library is not None:
        return library.resolve() if library.is_file() else None
    for cfg in ("release", "debug"):
        for name in LIB_NAMES:
            src = CODEC_DIR / "build" / cfg / name
            if src.is_file():
                return src.resolve()
    return None


def include_lib(dest: Path, library: Path) -> str:
    if library.name in LIB_NAMES:
        name = library.name
    elif library.name.endswith(".dylib"):
        name = "topos_codec.dylib"
    elif ".so." in library.name:
        name = "topos_codec.so"
    else:
        raise ValueError(f"不支持的动态库文件名: {library.name}")
    target = dest / "python" / "topos_codec" / "lib" / name
    shutil.copy2(library, target)
    return str(target.relative_to(dest))


def mark_platform_wheel(dest: Path) -> None:
    """随 wheel 打包动态库时，禁止生成错误的 py3-none-any 标记。"""
    (dest / "setup.py").write_text(
        "from setuptools import setup\n"
        "from wheel.bdist_wheel import bdist_wheel\n\n"
        "class PlatformWheel(bdist_wheel):\n"
        "    def finalize_options(self):\n"
        "        super().finalize_options()\n"
        "        self.root_is_pure = False\n\n"
        "    def get_tag(self):\n"
        "        _, _, platform = super().get_tag()\n"
        "        return 'py3', 'none', platform\n\n"
        "setup(cmdclass={'bdist_wheel': PlatformWheel})\n",
        encoding="utf-8",
    )


def verify(dest: Path) -> None:
    pkg_dir = dest / "python" / "topos_codec"
    for py in sorted(pkg_dir.glob("*.py")):
        py_compile.compile(str(py), doraise=True)
    for py in sorted((dest / "python" / "tests").glob("*.py")):
        py_compile.compile(str(py), doraise=True)
    for py in (dest / "native" / "examples").glob("*.py"):
        py_compile.compile(str(py), doraise=True)
    # 包内不得残留对主仓库的引用
    for py in sorted(pkg_dir.glob("*.py")):
        assert "src.shared" not in py.read_text(encoding="utf-8"), py


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dest", type=Path, default=DEFAULT_DEST)
    parser.add_argument("--include-lib", action="store_true",
                        help="把已构建的动态库复制进 python/topos_codec/lib/（自包含分发）")
    parser.add_argument("--lib", type=Path,
                        help="与 --include-lib 配合，指定要打包的动态库")
    parser.add_argument("--keep", action="store_true", help="不清理已存在的输出目录")
    args = parser.parse_args()
    if args.lib is not None and not args.include_lib:
        parser.error("--lib 必须与 --include-lib 一起使用")
    library = find_library(args.lib) if args.include_lib else None
    if args.include_lib and library is None:
        print("--include-lib 要求指定现存 --lib，或先构建 release/debug 动态库",
              file=sys.stderr)
        return 1
    dest = args.dest.resolve()

    if dest.exists():
        if args.keep:
            print(f"复用已存在目录：{dest}")
        else:
            shutil.rmtree(dest)
    dest.mkdir(parents=True, exist_ok=True)

    n_native = copy_native(dest)
    copy_package(dest)
    copy_tests(dest)
    n_docs = copy_docs(dest)
    n_ci = copy_workflows(dest)
    write_generated(dest)
    lib_info = include_lib(dest, library) if library is not None else None
    if lib_info is not None:
        mark_platform_wheel(dest)
    verify(dest)

    total = sum(1 for _ in dest.rglob("*") if _.is_file())
    print(f"✅ 开源发布目录已生成：{dest}")
    print(f"   native 文件 {n_native} 个；docs {n_docs} 个；CI workflow {n_ci} 个；共 {total} 个文件")
    print(f"   动态库：{lib_info or '未包含（构建后用 --include-lib 或放入 python/topos_codec/lib/）'}")
    print("   下一步：cd <dest> && git init && git add -A && git commit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
