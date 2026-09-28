#!/usr/bin/env python3
"""生成 src/transform/transform_tables.h —— 变换矩阵 M / 能量 E / 归一权重 W。

单一事实来源：本脚本内嵌冻结的 M（阶段 2 构造，见 ADR-C002），运行时只做
验证 + 产出表头。变更矩阵必须：改本脚本 → 重新生成 → 重生成 golden vectors
→ 更新 bitstream_spec 与 ADR（spec §13 版本规则）。

M 的构造：DCT-8 基按对称（u=0,2,4,6）/反对称（u=1,3,5,7）结构整数化
（×13 缩放）；对称×反对称行积恒为 0，组内经整数搜索达到精确两两正交。
"""
from __future__ import annotations

import itertools
from pathlib import Path

# 冻结的 8×8 变换矩阵（行主序；u=频率行索引）
M = [
    (13, 13, 13, 13, 13, 13, 13, 13),
    (13, 11,  7,  1, -1, -7, -11, -13),
    (12,  5, -5, -12, -12, -5,  5, 12),
    (11, -4, -13, -8,  8, 13,  4, -11),
    ( 9, -9, -9,  9,  9, -9, -9,  9),
    ( 8, -13,  4, 11, -11, -4, 13, -8),
    ( 5, -12, 12, -5, -5, 12, -12,  5),
    ( 1, -7, 11, -13, 13, -11,  7, -1),
]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def verify() -> list[int]:
    assert len(M) == 8 and all(len(r) == 8 for r in M)
    for i, j in itertools.combinations(range(8), 2):
        d = dot(M[i], M[j])
        assert d == 0, f"rows {i},{j} not orthogonal: {d}"
    E = [dot(r, r) for r in M]
    assert all(648 <= e <= 1352 for e in E), E
    assert max(abs(v) for r in M for v in r) == 13
    return E


def weights(E: list[int]) -> list[list[int]]:
    """W[u][v] = round(2^32 / (E_u·E_v))：把 1/(E_uE_v) 归一折进反变换。"""
    return [[round((1 << 32) / (E[u] * E[v])) for v in range(8)] for u in range(8)]


def emit(E: list[int], W: list[list[int]]) -> str:
    lines = [
        "/* GENERATED FILE — 不要手改；由 tools/gen_transform_tables.py 生成。",
        " * 阶段 2 冻结（ADR-C002）：精确行正交整数 DCT-8 近似。",
        " * 验证：tests/unit/test_transform.c 在 C 侧重验正交性与 W 推导。 */",
        "#ifndef TOPOS_INTERNAL_TRANSFORM_TABLES_H",
        "#define TOPOS_INTERNAL_TRANSFORM_TABLES_H",
        "",
        "#include <stdint.h>",
        "",
        "/* 变换矩阵 M，行主序 [u][x]，|M| ≤ 13；行两两正交（M·Mᵀ = diag(E)） */",
        "static const int16_t kTransformM[64] = {",
    ]
    for r in M:
        lines.append("    " + ", ".join(f"{v:3d}" for v in r) + ",")
    lines += [
        "};",
        "",
        "/* 行能量 E[u] = Σ_x M[u][x]² */",
        "static const uint32_t kTransformE[8] = {",
        "    " + ", ".join(str(e) for e in E) + ",",
        "};",
        "",
        "/* 反变换归一权重 W[u][v] = round(2^32/(E_u·E_v))，u16 容量内 */",
        "static const uint32_t kTransformW[64] = {",
    ]
    for r in W:
        lines.append("    " + ", ".join(f"{v:5d}" for v in r) + ",")
    lines += [
        "};",
        "",
        "#endif /* TOPOS_INTERNAL_TRANSFORM_TABLES_H */",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    E = verify()
    W = weights(E)
    assert all(0 < w <= 0xFFFF for r in W for w in r), "W 超出 u16"
    out = Path(__file__).resolve().parents[1] / "src/transform/transform_tables.h"
    out.write_text(emit(E, W), encoding="utf-8")
    print(f"verified M (orthogonal, |M|<=13); E={E}")
    print(f"W range: {min(min(r) for r in W)}..{max(max(r) for r in W)}")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
