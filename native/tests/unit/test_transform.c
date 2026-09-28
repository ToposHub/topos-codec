/* 变换：表自验（C 侧重验正交性/W 推导）+ 已知值 + 全链路 roundtrip + 界探测 */
#include "simd/dispatch.h"
#include "entropy/scan.h"
#include "transform/transform.h"
#include "transform/quant.h"
#include "transform/sparse_inverse.h"
#include "mini_test.h"

#include <stdio.h>
#include <string.h>

#define FWD_MAX (2 * 2047 * 13 * 8 * 13 * 8) /* 22,140,352 — spec §7.6 */

int main(void)
{
    const int16_t* M = tc_transform_matrix();
    const uint32_t* W = tc_transform_weights();
    const uint32_t* E = tc_transform_energies();

    /* —— 表自验：行两两正交、|M| ≤ 13、W = round_half_up(2^32/(E_uE_v)) —— */
    for (int i = 0; i < 8; ++i) {
        for (int j = i + 1; j < 8; ++j) {
            int32_t d = 0;
            for (int k = 0; k < 8; ++k) { d += M[i * 8 + k] * M[j * 8 + k]; }
            MT_CHECK_EQ_I64(d, 0);
        }
    }
    for (int i = 0; i < 64; ++i) {
        int32_t v = M[i];
        MT_CHECK(v >= -13 && v <= 13);
    }
    for (int u = 0; u < 8; ++u) {
        int32_t e = 0;
        for (int k = 0; k < 8; ++k) { e += M[u * 8 + k] * M[u * 8 + k]; }
        MT_CHECK_EQ_I64((int64_t)e, (int64_t)E[u]);
        for (int v = 0; v < 8; ++v) {
            uint64_t denom = (uint64_t)E[u] * (uint64_t)E[v];
            uint32_t expect = (uint32_t)((( (uint64_t)1 << 32 ) + denom / 2) / denom);
            MT_CHECK_EQ_U64(W[u * 8 + v], expect);
        }
    }

    /* —— 已知值：flat 块 —— */
    {
        int16_t x[64];
        int32_t F[64];
        for (int i = 0; i < 64; ++i) { x[i] = 100; }
        tc_transform_forward_8x8(x, F);
        MT_CHECK_EQ_I64(F[0], 10816 * 100);   /* (8·13)²·c */
        for (int i = 1; i < 64; ++i) { MT_CHECK_EQ_I64(F[i], 0); }
        /* F=0 反变换 → 0 */
        int32_t zero[64] = { 0 };
        int32_t xh[64];
        tc_transform_inverse_8x8(zero, xh);
        for (int i = 0; i < 64; ++i) { MT_CHECK_EQ_I64(xh[i], 0); }
    }

    /* —— 已知值：单脉冲（64 个位置）F[u][v] = V·M[u][py]·M[v][px] —— */
    for (int pos = 0; pos < 64; ++pos) {
        const int py = pos / 8, px = pos % 8;
        int16_t x[64];
        int32_t F[64];
        for (int i = 0; i < 64; ++i) { x[i] = 0; }
        x[pos] = 2047;
        tc_transform_forward_8x8(x, F);
        for (int u = 0; u < 8; ++u) {
            for (int v = 0; v < 8; ++v) {
                int32_t expect = 2047 * (int32_t)M[u * 8 + py] * (int32_t)M[v * 8 + px];
                MT_CHECK_EQ_I64(F[u * 8 + v], expect);
            }
        }
    }

    /* —— qp=4（Q=1）近无损 roundtrip：误差 ≤ 2；flat 块精确 —— */
    {
        const tc_qmatrix_set* qm = tc_qmatrix_by_id(0);
        int max_err = 0;
        for (int t = 0; t < 4000; ++t) {
            int16_t x[64];
            for (int i = 0; i < 64; ++i) {
                x[i] = (int16_t)((int64_t)(mt_rand_u64() % 4095u) - 2047);
            }
            int32_t F[64], q[64], Fp[64], xh[64];
            tc_transform_forward_8x8(x, F);
            tc_quant_block(F, qm->luma, 4, q);
            tc_dequant_block(q, qm->luma, 4, Fp);
            tc_transform_inverse_8x8(Fp, xh);
            for (int i = 0; i < 64; ++i) {
                int e = xh[i] - (int)x[i];
                if (e < 0) { e = -e; }
                if (e > max_err) { max_err = e; }
            }
        }
        MT_CHECK(max_err <= 2);
        printf("qp=4 Q=1 roundtrip max|err| = %d\n", max_err);
    }

    /* —— 图案链路（常量/渐变/棋盘/随机 × qp 集）：像素域钳位 + 误差记录 —— */
    {
        const tc_qmatrix_set* qm = tc_qmatrix_by_id(0);
        static const uint8_t qps[5] = { 0, 4, 20, 40, 63 };
        int32_t worst_by_qp[5] = { 0, 0, 0, 0, 0 };
        for (int qi = 0; qi < 5; ++qi) {
            const uint8_t qp = qps[qi];
            for (int pat = 0; pat < 7; ++pat) {
                for (int t = 0; t < 60; ++t) {
                    int16_t x[64];
                    for (int i = 0; i < 64; ++i) {
                        const int y = i / 8, xx = i % 8;
                        int32_t v;
                        switch (pat) {
                        case 0: v = (t % 2) ? 2047 : -2047; break;
                        case 1: v = ((t + xx) % 2) ? 2047 : -2047; break;   /* 棋盘 */
                        case 2: v = (xx + y) * 64 - 2047; break;            /* 对角渐变 */
                        case 3: v = xx * 291 - 2047; break;                 /* 水平渐变 */
                        case 4: v = ((xx / ((t % 4) + 1) + y / ((t % 3) + 1)) & 1) ? 2047 : -2047; break;
                        case 5: v = 0; break;
                        default: v = (int32_t)(mt_rand_u64() % 4095u) - 2047; break;
                        }
                        if (v > 2047) { v = 2047; }
                        if (v < -2047) { v = -2047; }
                        x[i] = (int16_t)v;
                    }
                    int32_t F[64], q[64], Fp[64], xh[64];
                    tc_transform_forward_8x8(x, F);
                    tc_quant_block(F, qm->luma, qp, q);
                    tc_dequant_block(q, qm->luma, qp, Fp);
                    tc_transform_inverse_8x8(Fp, xh);
                    for (int i = 0; i < 64; ++i) {
                        const int32_t mid = 2048; /* 12-bit 域验证钳位 */
                        int32_t px = xh[i] + mid;
                        if (px < 0) { px = 0; }
                        if (px > 4095) { px = 4095; }
                        (void)px;
                        int e = xh[i] - (int)x[i];
                        if (e < 0) { e = -e; }
                        if (e > worst_by_qp[qi]) { worst_by_qp[qi] = e; }
                    }
                }
            }
        }
        printf("链路 max|err| by qp {0,4,20,40,63}: %d %d %d %d %d\n",
               worst_by_qp[0], worst_by_qp[1], worst_by_qp[2],
               worst_by_qp[3], worst_by_qp[4]);
        /* 粗门槛：qp=0/4 近无损；qp=63 全量程量化的误差有界且有限 */
        MT_CHECK(worst_by_qp[0] <= 8);
        MT_CHECK(worst_by_qp[1] <= 8);
        MT_CHECK(worst_by_qp[4] <= 4096);
    }

    /* —— 界探测：对抗输入（±max 与 M 行符号对齐）下的 |F| 与反变换钳位 —— */
    {
        int32_t maxF = 0;
        /* 行 0 全 13：x=±2047 全同号 → F00 最大 */
        int16_t x[64];
        for (int i = 0; i < 64; ++i) { x[i] = 2047; }
        int32_t F[64];
        tc_transform_forward_8x8(x, F);
        for (int i = 0; i < 64; ++i) { if (F[i] > maxF) { maxF = F[i]; } }
        /* 随机符号对抗 */
        for (int t = 0; t < 20000; ++t) {
            for (int i = 0; i < 64; ++i) {
                x[i] = (mt_rand_u64() & 1) ? 2047 : -2047;
            }
            tc_transform_forward_8x8(x, F);
            for (int i = 0; i < 64; ++i) {
                int32_t a = F[i] < 0 ? -F[i] : F[i];
                if (a > maxF) { maxF = a; }
            }
        }
        printf("对抗输入 max|F| = %d（解析界 %d, 2^25=%d）\n", maxF, FWD_MAX, 1 << 25);
        MT_CHECK((int64_t)maxF <= FWD_MAX);
        MT_CHECK(maxF < (1 << 25));

        /* 反变换在钳位边界不溢出（s64 路径）。
         * 解析界：|x̂'| ≤ 64·169·(2^25·W_max)/2^32 ≈ 861k，断言 ±1M */
        int32_t big[64], xh[64];
        for (int i = 0; i < 64; ++i) { big[i] = (i & 1) ? (1 << 25) : -(1 << 25); }
        tc_transform_inverse_8x8(big, xh);
        for (int i = 0; i < 64; ++i) { MT_CHECK(xh[i] > -1000000 && xh[i] < 1000000); }
    }

    /* —— 解码深化：融合反量化+逆变换 vs 分离两步 差分（逐系数相等） ——
     * 覆盖：低频聚集稀疏（高频整行零）、全零+单系数、稠密、钳位极值；
     * qp 两档 × 两 qmatrix；AVX2 零跳过与 compose 数学恒等。 */
    {
        const tc_qmatrix_set* qms[2] = {tc_qmatrix_by_id(0), tc_qmatrix_by_id(1)};
        for (int qmi = 0; qmi < 2; ++qmi) {
            if (qms[qmi] == NULL) { continue; }
            for (int qp_i = 0; qp_i < 2; ++qp_i) {
                const uint32_t qp = qp_i == 0 ? 10u : 57u;
                tc_quant_ctx ctx;
                tc_quant_ctx_init(&ctx, qms[qmi]->luma, qp);
                for (int trial = 0; trial < 4000; ++trial) {
                    int32_t q[64];
                    for (int i = 0; i < 64; ++i) {
                        uint64_t r = mt_rand_u64() >> 33;
                        if (trial % 4u == 0u) {
                            q[i] = (i / 8 < 2 && (r & 3u) != 0u) ? 0
                                : (int32_t)(r % 4096u) - 2048;
                        } else if (trial % 4u == 1u) {
                            q[i] = 0;
                        } else if (trial % 4u == 2u) {
                            q[i] = (int32_t)(r % 33u) - 16;
                        } else {
                            q[i] = (r & 1u) ? -(1 << 24) : (1 << 24);
                        }
                    }
                    if (trial % 4u == 1u) {
                        q[mt_rand_u64() >> 60] = (int32_t)(mt_rand_u64() >> 33) - 1024;
                    }
                    /* 解码侧同式行掩码：bit u = 行 u 存在非零系数（超集
                     * 语义——DC 恒 bit0 与行内零系数置位均合法） */
                    uint32_t rm = 1u;
                    for (int r = 0; r < 8; ++r) {
                        for (int cc = 0; cc < 8; ++cc) {
                            if (q[r * 8 + cc] != 0) { rm |= 1u << r; break; }
                        }
                    }
                    int32_t fp[64], xr[64], xf[64];
                    tc_dequant_block_ctx(&ctx, q, fp);
                    tc_transform_inverse_8x8(fp, xr);
                    tc_simd_dequant_inverse_8x8(&ctx, q, xf, rm);
                    for (int i = 0; i < 64; ++i) {
                        if (xf[i] != xr[i]) {
                            mt_report(__FILE__, __LINE__, "fused dequant+inverse diverge");
                            MT_CHECK_EQ_I64((int64_t)xf[i], (int64_t)xr[i]);
                            break;
                        }
                    }
                }
            }
        }
    }

    /* —— M10-2B：稀疏 basis 逆变换 vs 稠密内核差分（随机/极值块 ≥ 50,000） ——
     * 稀疏侧直接给 (p, natural 序号) 对（p = clamp(level·Q[nat])，与解码
     * 扫描侧收集同构）；稠密侧走 q[64] + 行掩码融合内核。覆盖：均匀随机
     * 位置/值、低频聚集、钳位极值（±2^26 → clamp ±2^25）、DC 极值、64 个
     * natural 位逐一单独置位（含相邻对消极值）。两 qmatrix × 三 qp。 */
    {
        const tc_qmatrix_set* qms[2] = {tc_qmatrix_by_id(0), tc_qmatrix_by_id(1)};
        for (int qmi = 0; qmi < 2; ++qmi) {
            if (qms[qmi] == NULL) { continue; }
            for (int qp_i = 0; qp_i < 3; ++qp_i) {
                const uint32_t qp = qp_i == 0 ? 4u : (qp_i == 1 ? 30u : 57u);
                tc_quant_ctx ctx;
                tc_quant_ctx_init(&ctx, qms[qmi]->luma, qp);
                for (int trial = 0; trial < 50000 / 6; ++trial) {
                    int32_t q[64];
                    uint8_t nat[17];
                    int32_t p[17];
                    uint32_t np = 0u; /* 非零 AC 对数 */
                    for (int i = 0; i < 64; ++i) { q[i] = 0; }
                    q[0] = (int32_t)(mt_rand_u64() >> 40) - 512; /* DC */
                    uint32_t want = (uint32_t)(mt_rand_u64() >> 60) + 1u; /* 1..16 */
                    const uint32_t mode = trial % 5u;
                    for (uint32_t k = 0u; k < want; ++k) {
                        uint32_t slot = mode == 0u
                            ? (uint32_t)(mt_rand_u64() >> 58)     /* 均匀位置 */
                            : (k < 8u ? k + 1u
                                      : (uint32_t)(mt_rand_u64() >> 58)); /* 低频聚集 */
                        int32_t lvl;
                        uint64_t r = mt_rand_u64() >> 33;
                        if (mode == 3u) {
                            lvl = (r & 1u) ? (1 << 26) : -(1 << 26); /* 钳位极值 */
                        } else if (mode == 4u) {
                            lvl = (int32_t)(r % 1024u) - 512;
                        } else {
                            lvl = (int32_t)(r % 2048u) - 1024;
                        }
                        q[slot] = lvl; /* 同位后写覆盖：去重语义 */
                    }
                    /* 先数 np，再同步填充 p[i] ↔ nat[i]（含 DC 位 0） */
                    uint32_t cnt = 0u;
                    for (int i = 1; i < 64; ++i) {
                        if (q[i] != 0) { cnt++; }
                    }
                    if (cnt > 16u) { continue; } /* 超稀疏上限：只覆盖稠密侧 */
                    np = cnt;
                    nat[0] = 0u;
                    {
                        int64_t v0 = (int64_t)q[0] * (int64_t)ctx.Q[0];
                        if (v0 > (1 << 25)) { v0 = 1 << 25; }
                        if (v0 < -(1 << 25)) { v0 = -(1 << 25); }
                        p[0] = (int32_t)v0;
                        uint32_t j = 1u;
                        for (int i = 1; i < 64 && j <= np; ++i) {
                            if (q[i] != 0) {
                                int64_t v = (int64_t)q[i] * (int64_t)ctx.Q[i];
                                if (v > (1 << 25)) { v = 1 << 25; }
                                if (v < -(1 << 25)) { v = -(1 << 25); }
                                p[j] = (int32_t)v;
                                nat[j] = (uint8_t)i;
                                j++;
                            }
                        }
                    }
                    uint32_t rm = 1u;
                    for (int r = 0; r < 8; ++r) {
                        for (int cc = 0; cc < 8; ++cc) {
                            if (q[r * 8 + cc] != 0) { rm |= 1u << r; break; }
                        }
                    }
                    int32_t xs[64], xd[64];
                    tc_sparse_inverse(p, nat, np + 1u, xs);
                    tc_simd_dequant_inverse_8x8(&ctx, q, xd, rm);
                    for (int i = 0; i < 64; ++i) {
                        if (xs[i] != xd[i]) {
                            mt_report(__FILE__, __LINE__, "sparse inverse diverge");
                            MT_CHECK_EQ_I64((int64_t)xs[i], (int64_t)xd[i]);
                            break;
                        }
                    }
                }
                /* 64 个 natural 位逐一单独置位（DC 恒在；相邻位交替符号） */
                for (int bit = 1; bit < 64; ++bit) {
                    int32_t q[64];
                    int32_t p[2];
                    uint8_t nat2[2] = {0u, 0u};
                    for (int i = 0; i < 64; ++i) { q[i] = 0; }
                    q[0] = 137;
                    q[bit] = (bit & 1) ? -(1 << 26) : (1 << 26);
                    int64_t v0 = (int64_t)q[0] * (int64_t)ctx.Q[0];
                    if (v0 > (1 << 25)) { v0 = 1 << 25; }
                    if (v0 < -(1 << 25)) { v0 = -(1 << 25); }
                    p[0] = (int32_t)v0;
                    int64_t v1 = (int64_t)q[bit] * (int64_t)ctx.Q[bit];
                    if (v1 > (1 << 25)) { v1 = 1 << 25; }
                    if (v1 < -(1 << 25)) { v1 = -(1 << 25); }
                    p[1] = (int32_t)v1;
                    nat2[1] = (uint8_t)bit;
                    uint32_t rm = 1u | (1u << (bit >> 3));
                    int32_t xs[64], xd[64];
                    tc_sparse_inverse(p, nat2, 2u, xs);
                    tc_simd_dequant_inverse_8x8(&ctx, q, xd, rm);
                    for (int i = 0; i < 64; ++i) {
                        if (xs[i] != xd[i]) {
                            mt_report(__FILE__, __LINE__, "sparse single-bit diverge");
                            MT_CHECK_EQ_I64((int64_t)xs[i], (int64_t)xd[i]);
                            break;
                        }
                    }
                }
            }
        }
    }

    /* —— M10-2C：四块 SoA 批量内核 vs 逐块内核差分（≥50,000 组 × 4 块） ——
     * 覆盖：均匀随机位置/值、稠密（全 64 位）、DC-only 块混批、钳位极值
     * （±2^26 → clamp ±2^25）、行掩码悬殊混批（并集语义）。 */
    {
        const tc_qmatrix_set* qms[2] = {tc_qmatrix_by_id(0), tc_qmatrix_by_id(1)};
        for (int qmi = 0; qmi < 2; ++qmi) {
            if (qms[qmi] == NULL) { continue; }
            for (int qp_i = 0; qp_i < 3; ++qp_i) {
                const uint32_t qp = qp_i == 0 ? 4u : (qp_i == 1 ? 30u : 57u);
                tc_quant_ctx ctx;
                tc_quant_ctx_init(&ctx, qms[qmi]->luma, qp);
                for (int trial = 0; trial < 50000 / 6; ++trial) {
                    int32_t q4[4][64];
                    int64_t qsoa[64 * 4];
                    uint32_t rm_union = 0u;
                    for (int k = 0; k < 4; ++k) {
                        for (int i = 0; i < 64; ++i) { q4[k][i] = 0; }
                        q4[k][0] = (int32_t)(mt_rand_u64() >> 40) - 512;
                        uint32_t nnz = (uint32_t)(mt_rand_u64() >> 59); /* 0..31 */
                        if (trial % 7u == 0u) { nnz = 64u; }            /* 稠密 */
                        for (uint32_t n = 0u; n < nnz; ++n) {
                            uint32_t slot = (uint32_t)(mt_rand_u64() >> 58);
                            uint64_t r = mt_rand_u64() >> 33;
                            int32_t lvl = (trial % 11u == 0u)
                                ? ((r & 1u) ? (1 << 26) : -(1 << 26))
                                : (int32_t)(r % 4096u) - 2048;
                            q4[k][slot] = lvl;
                        }
                        if (trial % 13u == 3u * (uint32_t)k) { /* DC-only 块混批 */
                            for (int i = 1; i < 64; ++i) { q4[k][i] = 0; }
                        }
                    }
                    for (int k = 0; k < 4; ++k) {
                        for (int i = 0; i < 64; ++i) {
                            qsoa[i * 4 + k] = (int64_t)q4[k][i];
                            if (q4[k][i] != 0) { rm_union |= 1u << (i >> 3); }
                        }
                    }
                    int32_t xh4[4 * 64];
                    tc_simd_dequant_inverse_8x8x4(&ctx, qsoa, xh4, rm_union);
                    for (int k = 0; k < 4; ++k) {
                        uint32_t rm = 1u;
                        for (int r = 0; r < 8; ++r) {
                            for (int c = 0; c < 8; ++c) {
                                if (q4[k][r * 8 + c] != 0) { rm |= 1u << r; break; }
                            }
                        }
                        int32_t ref[64];
                        tc_simd_dequant_inverse_8x8(&ctx, q4[k], ref, rm);
                        for (int i = 0; i < 64; ++i) {
                            if (xh4[k * 64 + i] != ref[i]) {
                                mt_report(__FILE__, __LINE__, "soa4 inverse diverge");
                                MT_CHECK_EQ_I64((int64_t)xh4[k * 64 + i], (int64_t)ref[i]);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    /* target-size preview 只求块内采样点：与完整逆变换逐位一致。 */
    {
        const tc_qmatrix_set* qm = tc_qmatrix_by_id(0);
        tc_quant_ctx ctx;
        tc_quant_ctx_init(&ctx, qm->luma, 27u);
        for (uint32_t trial = 0u; trial < 2000u; ++trial) {
            int32_t q[64];
            for (uint32_t i = 0u; i < 64u; ++i) {
                q[i] = (int32_t)(mt_rand_u64() % 4097u) - 2048;
            }
            int32_t full[64];
            uint32_t rm = 1u;
            for (uint32_t r = 0u; r < 8u; ++r) {
                for (uint32_t cc = 0u; cc < 8u; ++cc) {
                    if (q[r * 8u + cc] != 0) { rm |= 1u << r; break; }
                }
            }
            tc_simd_dequant_inverse_8x8(&ctx, q, full, rm);
            uint8_t xs[7], ys[7];
            uint32_t count = (uint32_t)(mt_rand_u64() % 7u) + 1u;
            for (uint32_t i = 0u; i < count; ++i) {
                xs[i] = (uint8_t)(mt_rand_u64() & 7u);
                ys[i] = (uint8_t)(mt_rand_u64() & 7u);
            }
            int32_t sampled[7];
            tc_dequant_inverse_samples(&ctx, q, xs, ys, count, sampled);
            for (uint32_t i = 0u; i < count; ++i) {
                MT_CHECK_EQ_I64(sampled[i], full[(uint32_t)ys[i] * 8u + xs[i]]);
            }
        }
    }

    /* RD3-02：固定低频 band sampled kernel 与 scalar masked reference 逐点
     * bit-exact；覆盖 scalar、运行时 SIMD 和 full(63) 回退。 */
    {
        const tc_qmatrix_set* qm = tc_qmatrix_by_id(1);
        const uint8_t limits[5] = { 4u, 10u, 16u, 24u, 63u };
        const int modes[3] = { TC_SIMD_SCALAR, TC_SIMD_AUTO, TC_SIMD_FORCE };
        tc_quant_ctx ctx;
        tc_quant_ctx_init(&ctx, qm->luma, 31u);
        for (size_t mi = 0u; mi < sizeof(modes) / sizeof(modes[0]); ++mi) {
            tc_dev_set_simd_mode(modes[mi]);
            for (uint32_t trial = 0u; trial < 256u; ++trial) {
                int32_t q[64];
                int32_t fp[64];
                for (uint32_t i = 0u; i < 64u; ++i) {
                    q[i] = (int32_t)(mt_rand_u64() % 8193u) - 4096;
                }
                tc_dequant_block_ctx(&ctx, q, fp);
                uint8_t xs[8], ys[8];
                for (uint32_t i = 0u; i < 8u; ++i) {
                    xs[i] = (uint8_t)(mt_rand_u64() & 7u);
                    ys[i] = (uint8_t)(mt_rand_u64() & 7u);
                }
                for (size_t li = 0u; li < sizeof(limits) / sizeof(limits[0]); ++li) {
                    int32_t masked[64];
                    for (uint32_t i = 0u; i < 64u; ++i) {
                        masked[i] = kTcZigzagInv[i] <= limits[li] ? fp[i] : 0;
                    }
                    int32_t reference[8];
                    int32_t actual[8];
                    tc_transform_inverse_8x8_samples_scalar(
                        masked, xs, ys, 8u, reference);
                    tc_dequant_inverse_samples_limited(
                        &ctx, q, xs, ys, 8u, limits[li], actual);
                    for (uint32_t i = 0u; i < 8u; ++i) {
                        MT_CHECK_EQ_I64(actual[i], reference[i]);
                    }
                }
            }
        }
        tc_dev_set_simd_mode(TC_SIMD_AUTO);
    }

    /* T2：精确 1/2 采样内核与通用采样入口逐点 bit-exact。覆盖完整 4×4
     * 目标网格和边缘块的行主序前缀，三路后端及全部冻结低频 band。 */
    {
        const tc_qmatrix_set* qm = tc_qmatrix_by_id(1);
        const uint8_t limits[5] = { 4u, 10u, 16u, 24u, 63u };
        const uint32_t counts[5] = { 1u, 4u, 7u, 12u, 16u };
        const int modes[3] = { TC_SIMD_SCALAR, TC_SIMD_AUTO, TC_SIMD_FORCE };
        static const uint8_t xs[16] = {
            0u, 2u, 4u, 6u, 0u, 2u, 4u, 6u,
            0u, 2u, 4u, 6u, 0u, 2u, 4u, 6u
        };
        static const uint8_t ys[16] = {
            0u, 0u, 0u, 0u, 2u, 2u, 2u, 2u,
            4u, 4u, 4u, 4u, 6u, 6u, 6u, 6u
        };
        tc_quant_ctx ctx;
        tc_quant_ctx_init(&ctx, qm->luma, 31u);
        for (size_t mi = 0u; mi < sizeof(modes) / sizeof(modes[0]); ++mi) {
            tc_dev_set_simd_mode(modes[mi]);
            for (uint32_t trial = 0u; trial < 256u; ++trial) {
                int32_t q[64];
                for (uint32_t i = 0u; i < 64u; ++i) {
                    q[i] = (int32_t)(mt_rand_u64() % 8193u) - 4096;
                }
                for (size_t li = 0u; li < sizeof(limits) / sizeof(limits[0]); ++li) {
                    for (size_t ci = 0u; ci < sizeof(counts) / sizeof(counts[0]); ++ci) {
                        int32_t generic[16];
                        int32_t specialized[16];
                        tc_dequant_inverse_samples_limited(
                            &ctx, q, xs, ys, counts[ci], limits[li], generic);
                        tc_dequant_inverse_samples_half_limited(
                            &ctx, q, counts[ci], limits[li], specialized);
                        for (uint32_t i = 0u; i < counts[ci]; ++i) {
                            MT_CHECK_EQ_I64(specialized[i], generic[i]);
                        }
                    }
                }
            }
        }
        tc_dev_set_simd_mode(TC_SIMD_AUTO);
    }

    /* ---- 批 4 阶段 2：i32 前向三路差分（scalar/AVX2/NEON 编译可得即测；
     * bd16 满摆全域 + 12-bit 界 + DC/脉冲对抗块，逐位一致） ---- */
    {
        static const int modes[3] = { TC_SIMD_SCALAR, TC_SIMD_AUTO, TC_SIMD_FORCE };
        for (size_t mi = 0u; mi < 3u; ++mi) {
            tc_dev_set_simd_mode(modes[mi]);
            for (int blk = 0; blk < 512; ++blk) {
                int32_t x[64];
                const uint32_t kind = (uint32_t)(mt_rand_u64() % 6ull);
                for (int i = 0; i < 64; ++i) {
                    const uint32_t r = (uint32_t)(mt_rand_u64() % 100ull);
                    if (kind == 0u) { x[i] = (r < 50u) ? 32767 : -32767; }
                    else if (kind == 1u) { x[i] = (r < 50u) ? 2047 : -2047; }
                    else if (kind == 2u) { x[i] = (int32_t)(mt_rand_u64() % 65535ull) - 32767; }
                    else if (kind == 3u) { x[i] = 32767; }
                    else if (kind == 4u) { x[i] = (i == 0) ? -32767 : 0; }
                    else { x[i] = (int32_t)(mt_rand_u64() % 4095ull) - 2047; }
                }
                int32_t ref[64], got[64];
                tc_transform_forward_8x8_i32_scalar(x, ref);
                tc_transform_forward_8x8_i32(x, got); /* 经分发 */
                MT_CHECK(memcmp(ref, got, sizeof(ref)) == 0);
            }
        }
        tc_dev_set_simd_mode(TC_SIMD_AUTO);
    }

    /* ---- 批 4 阶段 2：bd16 宽域全链（前向分发 → Q=1 量化 → 反量化 →
     * scalar 逆变换）。W = round_half_up(2^32/(EuEv)) 的舍入残差随 |F|
     * 线性放大：12-bit 稠密随机块实测 ≤2，bd16 满摆等比放大 → 界断言；
     * 12-bit 尺度稀疏块恒等精确。内容级 maxerr=0（预测稀疏化后）由
     * test_codec TRAW 16-bit 无损段承载。 ---- */
    {
        uint16_t qmf[64];
        for (int i = 0; i < 64; ++i) { qmf[i] = 16; } /* qmatrix_id 0 = flat16 */
        tc_quant_ctx ctx;
        tc_quant_ctx_init_bd(&ctx, qmf, 0u, 16u);
        MT_CHECK_EQ_U64(ctx.Q[0], 1ull);
        MT_CHECK_EQ_U64(ctx.exact_div, 1ull);
        int worst = 0;
        for (int blk = 0; blk < 512; ++blk) {
            int32_t x[64];
            const int small_scale = (blk % 2 == 0);
            for (int i = 0; i < 64; ++i) {
                x[i] = small_scale
                    ? ((i == 0) ? ((blk % 4) ? 2047 : -2047)
                                : (int32_t)(mt_rand_u64() % 255ull) - 127)
                    : (int32_t)(mt_rand_u64() % 65535ull) - 32767;
            }
            int32_t F[64], q[64], Fp[64], xh[64];
            tc_transform_forward_8x8_i32(x, F);
            tc_quant_block_ctx(&ctx, F, q);
            tc_dequant_block_ctx(&ctx, q, Fp);
            tc_transform_inverse_8x8_scalar(Fp, xh);
            for (int i = 0; i < 64; ++i) {
                int e = xh[i] - x[i];
                if (e < 0) { e = -e; }
                if (e > worst) { worst = e; }
                if (small_scale) { MT_CHECK_EQ_I64(xh[i], x[i]); } /* 恒等精确 */
            }
        }
        MT_CHECK(worst <= 48);
        printf("bd16 wide roundtrip (dense full-swing) max|err| = %d\n", worst);

        /* 无损数学保证：预失真后满摆稠密块 Q=1 往返恒等（残差界
         * 0.5·Σ M²/(EuEv) ≈ 0.008 ≪ 0.5 → 解码舍入精确还原） */
        for (int blk = 0; blk < 512; ++blk) {
            int32_t x[64];
            for (int i = 0; i < 64; ++i) {
                x[i] = (int32_t)(mt_rand_u64() % 65535ull) - 32767;
            }
            int32_t F[64], q[64], Fp[64], xh[64];
            tc_transform_forward_8x8_i32(x, F);
            tc_transform_forward_predistort_i32(F);
            tc_quant_block_ctx(&ctx, F, q);
            tc_dequant_block_ctx(&ctx, q, Fp);
            tc_transform_inverse_8x8_scalar(Fp, xh);
            for (int i = 0; i < 64; ++i) { MT_CHECK_EQ_I64(xh[i], x[i]); }
        }
    }

    return MT_MAIN_RETURN();
}
