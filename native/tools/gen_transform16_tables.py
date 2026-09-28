#!/usr/bin/env python3
"""生成 src/transform/transform16_tables.h —— 16×16 变换试点（ADR-C037）。

M16 构造：Haar2 ⊗ M8 混合，从 DCT-16 的 pair-split 恒等式直接导出：
  y[2m]   = M8 行 m 作用在 pair-sum (f[x]+f[15−x]) 上 —— 偶行与真 DCT-16
            偶行相关性 = M8 对真 DCT-8 的相关性（0.996..1.000，逐位复用）；
  y[2m+1] = M8 行 m 作用在 pair-diff (f[x]−f[15−x]) 上 —— 奇行是偶频形状
            对奇频的替代（频谱上摊到全部奇频）。
  行向量：M16[2m]   [x<8]=M8[m][x]，[x≥8]=M8[m][15−x]；
          M16[2m+1] [x<8]=M8[m][x]，[x≥8]=−M8[m][15−x]。

为什么不是「直接整数 DCT-16」：奇行需要 twiddle 旋转（本仓库
gen_transform16 前身做过整数旋转链/退火搜索实验，均可证不可行）——
(a) 整数旋转 (a,b;−b,a) 的乘积只在标量倍时保持对角 Gram，链式嵌套
    必破坏（(AB)(AB)ᵀ = A·D_B·Aᵀ 非对角）；
(b) Pythagorean 有理旋转精确但角覆盖稀疏（角误差 3°+）且分母连乘
    爆炸（5^7 ≈ 78K → E16 域失控）；
(c) 近正交 + 范数表（AV1 式）改变本码流的规范性整数逆变换模型。
混合构造是「精确行正交整数」约束下的最优可达形状；其偶行零近似损失，
奇行保守 —— 试点以浮点 DCT-16 参照同时测出方向上限与混合的捕获率。

E16 = 2·E8（统一因子 2，量化域与 8×8 同构）；zigzag16 = 8×8 冻结表
同一对角遍历算法在 N=16 的扩展（生成器以 N=8 自检与 scan.h 一致）。

变更表必须：改本脚本 → 重新生成 → 更新 test_transform16 与 ADR-C037。
"""
from __future__ import annotations

import math
import re
from pathlib import Path

N = 16
HERE = Path(__file__).resolve().parents[1]
OUT = HERE / "src" / "transform" / "transform16_tables.h"
M8_HEADER = HERE / "src" / "transform" / "transform_tables.h"


def load_frozen_m8() -> list[list[int]]:
    """单一事实来源：解析 transform_tables.h 冻结的 kTransformM。"""
    txt = M8_HEADER.read_text()
    m = re.search(r"kTransformM\[64\] = \{(.*?)\};", txt, re.S)
    if not m:
        raise SystemExit("transform_tables.h 中未找到 kTransformM")
    vals = [int(v) for v in re.findall(r"-?\d+", m.group(1))]
    assert len(vals) == 64
    return [vals[i * 8:(i + 1) * 8] for i in range(8)]


def build_rows(m8: list[list[int]]) -> list[list[int]]:
    rows = []
    for m in range(8):
        even = list(m8[m]) + [m8[m][15 - x] for x in range(8, N)]
        odd = list(m8[m]) + [-m8[m][15 - x] for x in range(8, N)]
        rows.append(even)
        rows.append(odd)
    return rows


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def verify(rows: list[list[int]]) -> list[int]:
    assert len(rows) == N and all(len(r) == N for r in rows)
    for i in range(N):
        for k in range(i + 1, N):
            d = dot(rows[i], rows[k])
            assert d == 0, f"rows {i},{k} not orthogonal: {d}"
    for u in range(1, N):  # 常数块 → 仅 DC
        assert sum(rows[u]) == 0, f"row {u} sum {sum(rows[u])}"
    E = [dot(r, r) for r in rows]
    return E


def dct16_true() -> list[list[float]]:
    a = [[0.0] * N for _ in range(N)]
    for u in range(N):
        al = math.sqrt(1.0 / N) if u == 0 else math.sqrt(2.0 / N)
        for x in range(N):
            a[u][x] = al * math.cos((2 * x + 1) * u * math.pi / (2 * N))
    return a


def row_corr(rows, E, true):
    """每行与真 DCT-16 行的相关性（诚实度数据，进 ADR）。"""
    out = []
    for u in range(N):
        tv = true[u]
        tn = math.sqrt(dot(tv, tv))
        cn = math.sqrt(E[u])
        out.append(dot(rows[u], tv) / (cn * tn))
    return out


def zigzag(n: int) -> list[int]:
    """对角遍历（与 8×8 冻结表同算法）：奇对角从 (0,d) 向左下，偶对角从
    (d,0) 向右上。返回 scan→natural（y*n+x）。"""
    order = []
    for d in range(2 * n - 1):
        ys = range(max(0, d - n + 1), min(d, n - 1) + 1)
        if d % 2 == 0:
            ys = reversed(list(ys))
        for y in ys:
            order.append(y * n + (d - y))
    return order


def selftest_zigzag() -> None:
    kTcZigzag8 = [
        0, 1, 8, 16, 9, 2, 3, 10,
        17, 24, 32, 25, 18, 11, 4, 5,
        12, 19, 26, 33, 40, 48, 41, 34,
        27, 20, 13, 6, 7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36,
        29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46,
        53, 60, 61, 54, 47, 55, 62, 63,
    ]
    assert zigzag(8) == kTcZigzag8, "N=8 自检失败：与 scan.h 冻结表不一致"


def main() -> None:
    selftest_zigzag()
    m8 = load_frozen_m8()
    rows = build_rows(m8)
    E = verify(rows)
    corr = row_corr(rows, E, dct16_true())
    assert all(c > 0 for c in corr), corr  # 行序与真 DCT-16 对齐（无符号翻转）
    W = [[round((1 << 32) / (E[u] * E[v])) for v in range(N)] for u in range(N)]
    zz = zigzag(N)
    assert sorted(zz) == list(range(N * N))
    zz_inv = [0] * (N * N)
    for pos, nat in enumerate(zz):
        zz_inv[nat] = pos

    lines = [
        "/* GENERATED FILE — 不要手改；由 tools/gen_transform16_tables.py 生成。",
        " * ADR-C037 16×16 变换试点：Haar2⊗M8 混合（DCT-16 pair-split 恒等式导出）。",
        " * 精确行正交（M16·M16ᵀ = diag(E16)）；E16 = 2·E8 —— 量化域与 8×8 同构。",
        " * 偶行 = 真 DCT-16 偶行（复用 M8 整数化精度）；奇行为保守替代，",
        " * 上限对照见 tools/bench_t16_ab.py 的浮点 DCT-16 参照路径。",
        " * 验证：tests/unit/test_transform16.c 在 C 侧重验正交性与 W 推导。 */",
        "#ifndef TOPOS_INTERNAL_TRANSFORM16_TABLES_H",
        "#define TOPOS_INTERNAL_TRANSFORM16_TABLES_H",
        "",
        "#include <stdint.h>",
        "",
        f"/* 16×16 变换矩阵 M16，行主序 [u][x]，|M16| ≤ {max(abs(v) for r in rows for v in r)}；",
        " * 行两两正交；行 0 之外行和精确为 0（常数块 → 仅 DC） */",
        "static const int16_t kTransform16M[256] = {",
    ]
    for r in rows:
        lines.append("    " + ", ".join(f"{v:3d}" for v in r) + ",")
    lines += [
        "};",
        "",
        "/* 行能量 E16[u] = Σ_x M16[u][x]² = 2·E8[u>>1] */",
        "static const uint32_t kTransform16E[16] = {",
        "    " + ", ".join(str(e) for e in E) + ",",
        "};",
        "",
        "/* 反变换归一权重 W16[u][v] = round(2^32/(E_u·E_v)) */",
        "static const uint32_t kTransform16W[256] = {",
    ]
    for r in W:
        lines.append("    " + ", ".join(f"{v:5d}" for v in r) + ",")
    lines += [
        "};",
        "",
        "/* Zigzag16：与 8×8 冻结表同一对角遍历算法在 N=16 的扩展。",
        " * kTcZigzag16[i] = 第 i 个扫描位置的 natural 索引（u*16+v）。 */",
        "static const uint8_t kTcZigzag16[256] = {",
    ]
    for i in range(0, 256, 16):
        lines.append("    " + ", ".join(f"{v:3d}" for v in zz[i:i + 16]) + ",")
    lines += [
        "};",
        "",
        "/* natural → 扫描位置（kTcZigzag16 的精确逆） */",
        "static const uint8_t kTcZigzag16Inv[256] = {",
    ]
    for i in range(0, 256, 16):
        lines.append("    " + ", ".join(f"{v:3d}" for v in zz_inv[i:i + 16]) + ",")
    lines += [
        "};",
        "",
        "#endif /* TOPOS_INTERNAL_TRANSFORM16_TABLES_H */",
        "",
    ]
    OUT.write_text("\n".join(lines))
    ec = corr[0::2]
    oc = corr[1::2]
    print(f"[t16] M16 → {OUT}")
    print(f"[t16] E16=[{min(E)}..{max(E)}] 与真 DCT-16 行相关："
          f"偶行 {min(ec):.4f}..{max(ec):.4f}，奇行 {min(oc):.4f}..{max(oc):.4f}")


if __name__ == "__main__":
    main()
