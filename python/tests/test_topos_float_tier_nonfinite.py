"""有损 half-float 产品档不得静默把非有限值量化成普通数。"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parents[2]
# Unix 单配置惯例 build/release/ 直达；VS 多配置生成器在工具链命名目录的
# Release/ 子目录（与 test_v8_encode 的 topos_inspect 发现同策略）
_CLI_CANDIDATES = [
    REPO / ("native/build/release/toos"
            + (".exe" if sys.platform == "win32" else "")),
    *sorted((REPO / "native/build").glob("*/Release/toos.exe")),
]
CLI = next((p for p in _CLI_CANDIDATES if p.is_file()), _CLI_CANDIDATES[0])


def test_cli_float_tier_rejects_inf_without_output(tmp_path: Path) -> None:
    if not CLI.is_file():
        pytest.skip("toos CLI 未构建")
    source = tmp_path / "half.raw"
    with source.open("wb") as dest:
        for index in range(3):
            plane = np.full((32, 32), 0x3800, dtype="<u2")
            if index == 1:
                plane[0, 0] = 0x7C00
            dest.write(plane.tobytes())
    output = tmp_path / "nonfinite.toos"
    result = subprocess.run(
        [str(CLI), "encode", str(source), "-o", str(output),
         "--width", "32", "--height", "32", "--tier", "float-ultra"],
        capture_output=True, text=True,
    )
    assert result.returncode != 0
    assert "Inf/NaN" in result.stderr
    assert not output.exists()
