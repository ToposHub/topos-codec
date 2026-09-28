"""P1-14：SDK 文档与实现一致性门（过期文本不得进入 release）。

以实现为单一事实源解析常量（tpool.h 硬上限 / binding docstring /
CLI usage），比对 docs/SDK.md 的相应陈述。常量改动而文档未跟
时，本测试失败——取代"文档改了没人知道"的静默漂移。
"""
from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SDK_MD = REPO / "docs" / "SDK.md"
TPOOL_H = REPO / "native" / "src" / "common" / "tpool.h"
BINDING = REPO / "python" / "topos_codec" / "topos_binding.py"


def _tpool_max_threads() -> int:
    text = TPOOL_H.read_text(encoding="utf-8")
    m = re.search(r"#define\s+TC_SLICE_MAX_THREADS\s+(\d+)", text)
    assert m, "tpool.h 缺少 TC_SLICE_MAX_THREADS 定义"
    return int(m.group(1))


def test_sdk_doc_matches_tpool_thread_constants() -> None:
    """SDK.md 线程池默认/上限/env 范围必须与 TC_SLICE_MAX_THREADS 一致。"""
    n = _tpool_max_threads()
    sdk = SDK_MD.read_text(encoding="utf-8")
    assert f"硬上限 {n}" in sdk, f"SDK.md 线程池硬上限与 tpool.h({n}) 不一致"
    assert f"TOPOS_SLICE_THREADS`（1..{n}）" in sdk, \
        "SDK.md TOPOS_SLICE_THREADS 取值范围与 tpool.h 不一致"
    assert f"min(ncpu, {n})" in sdk, "SDK.md 线程池默认值与 tpool.h 不一致"
    # 陈旧值直接拒绝（历史上曾写 min(4,ncpu)/上限 8）
    assert "硬上限 8" not in sdk
    assert "min(4, ncpu)" not in sdk


def test_binding_docstring_matches_tpool_constants() -> None:
    """ToposCodec.slice_threads docstring 的默认值口径须与 tpool.h 一致。"""
    n = _tpool_max_threads()
    text = BINDING.read_text(encoding="utf-8")
    assert f"min(ncpu, {n})" in text, \
        "topos_binding.slice_threads docstring 默认值与 tpool.h 不一致"


def test_sdk_no_static_stripe_claim() -> None:
    """M10 起为动态领取分发；SDK.md 不得再声称静态条带语义。"""
    sdk = SDK_MD.read_text(encoding="utf-8")
    assert "静态条带语义不变" not in sdk, \
        "SDK.md 仍声称静态条带——与 tpool 动态领取实现不一致（P1-14）"
