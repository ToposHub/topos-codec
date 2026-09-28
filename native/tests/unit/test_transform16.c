/* ADR-C037 试点变换：表自验（正交性/W 推导/E=2·E8）+ 已知值 + roundtrip
 * + 界探测 + zigzag16 置换/单调性。 */
#include "entropy/scan.h"
#include "transform/transform.h"
#include "transform/transform16.h"
#include "transform/transform16_tables.h"
#include "mini_test.h"

#include <stdio.h>
#include <stdlib.h>

/* 生成器推导：行和 ≤ 208（行 0 = 16·13；u>0 = 2·Σ|M8 行|） */
#define F16_MAX (2 * 2047 * 208 * 208 / 2) /* 最坏单系数界近似，下面实测 */

static uint32_t rng_state = 0x20260911u;
static uint32_t rng32(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    const int16_t* M = tc_transform16_matrix();
    const uint32_t* W = tc_transform16_weights();
    const uint32_t* E = tc_transform16_energies();
    const int16_t* M8 = tc_transform_matrix();
    const uint32_t* E8u = tc_transform_energies();

    /* —— 表自验：行两两正交、|M16| ≤ 13、E16 = 2·E8[u>>1]、
     *        W16 = round_half_up(2^32/(E_uE_v))、行和（u>0 恒 0） —— */
    for (int i = 0; i < 16; ++i) {
        int32_t row_sum = 0;
        for (int k = 0; k < 16; ++k) { row_sum += M[i * 16 + k]; }
        if (i == 0) {
            MT_CHECK_EQ_I64(row_sum, 16 * 13);
        } else {
            MT_CHECK_EQ_I64(row_sum, 0);
        }
        for (int j = i + 1; j < 16; ++j) {
            int64_t d = 0;
            for (int k = 0; k < 16; ++k) {
                d += (int64_t)M[i * 16 + k] * M[j * 16 + k];
            }
            MT_CHECK_EQ_I64(d, 0);
        }
        int64_t e = 0;
        for (int k = 0; k < 16; ++k) {
            int32_t v = M[i * 16 + k];
            MT_CHECK(v >= -13 && v <= 13);
            e += (int64_t)v * v;
        }
        MT_CHECK_EQ_I64(e, (int64_t)E[i]);
        MT_CHECK_EQ_I64(e, 2 * (int64_t)E8u[i >> 1]);
        for (int j = 0; j < 16; ++j) {
            uint64_t denom = (uint64_t)E[i] * (uint64_t)E[j];
            uint32_t expect =
                (uint32_t)((((uint64_t)1 << 32) + denom / 2) / denom);
            MT_CHECK_EQ_U64(W[i * 16 + j], expect);
        }
    }

    /* —— 混合构造逐元素：M16[2m] = [M8[m] | rev(M8[m])]，
     *        M16[2m+1] = [M8[m] | −rev(M8[m])]（生成器单一事实来源） —— */
    for (int m = 0; m < 8; ++m) {
        for (int x = 0; x < 8; ++x) {
            const int16_t m8 = M8[m * 8 + x];
            MT_CHECK_EQ_I64(M[(2 * m) * 16 + x], m8);
            MT_CHECK_EQ_I64(M[(2 * m) * 16 + (15 - x)], m8);
            MT_CHECK_EQ_I64(M[(2 * m + 1) * 16 + x], m8);
            MT_CHECK_EQ_I64(M[(2 * m + 1) * 16 + (15 - x)], -m8);
        }
    }

    /* —— 已知值：flat 块 → 仅 DC = (16·13)²·c —— */
    {
        int16_t x[256];
        int32_t F[256];
        for (int i = 0; i < 256; ++i) { x[i] = 100; }
        tc_transform16_forward(x, F);
        /* (16·13)² = 43264；×100 = 4,326,400 */
        MT_CHECK_EQ_I64(F[0], 4326400);
        for (int i = 1; i < 256; ++i) { MT_CHECK_EQ_I64(F[i], 0); }
        int32_t zero[256] = { 0 };
        int32_t xh[256];
        tc_transform16_inverse(zero, xh);
        for (int i = 0; i < 256; ++i) { MT_CHECK_EQ_I64(xh[i], 0); }
    }

    /* —— 单脉冲：F[u][v] = V·M16[u][py]·M16[v][px] —— */
    for (int pos = 0; pos < 256; pos += 7) {
        const int py = pos / 16, px = pos % 16;
        int16_t x[256] = { 0 };
        int32_t F[256];
        x[py * 16 + px] = 37;
        tc_transform16_forward(x, F);
        for (int u = 0; u < 16; ++u) {
            for (int v = 0; v < 16; ++v) {
                int32_t expect = 37 * (int32_t)M[u * 16 + py]
                                   * (int32_t)M[v * 16 + px];
                MT_CHECK_EQ_I64(F[u * 16 + v], expect);
            }
        }
    }

    /* —— 无量化 roundtrip：W 舍入 + >>32 舍入误差（随机 2000 块） —— */
    {
        int max_err = 0;
        int32_t max_abs_F = 0;
        for (int t = 0; t < 2000; ++t) {
            int16_t x[256];
            int32_t F[256], xh[256];
            for (int i = 0; i < 256; ++i) {
                x[i] = (int16_t)((int32_t)(rng32() % 4095) - 2047);
            }
            tc_transform16_forward(x, F);
            for (int i = 0; i < 256; ++i) {
                int32_t a = F[i] < 0 ? -F[i] : F[i];
                if (a > max_abs_F) { max_abs_F = a; }
            }
            tc_transform16_inverse(F, xh);
            for (int i = 0; i < 256; ++i) {
                int e = xh[i] - x[i];
                if (e < 0) { e = -e; }
                if (e > max_err) { max_err = e; }
            }
        }
        printf("roundtrip max|err| = %d, max|F| = %d (< 2^27 = %d)\n",
               max_err, max_abs_F, TC_TRANSFORM16_MAX_ABS_F);
        MT_CHECK(max_err <= 2);
        MT_CHECK(max_abs_F < TC_TRANSFORM16_MAX_ABS_F);
        /* 生成器界：|F| < 2047·208² ≈ 88.6M = 2^26.4 */
        MT_CHECK(max_abs_F < 88600000);
    }

    /* —— zigzag16：精确置换 + (u+v) 对角单调不减 + 前 6 项 pin —— */
    {
        int seen[256] = { 0 };
        for (int i = 0; i < 256; ++i) {
            const uint32_t nat = kTcZigzag16[i];
            MT_CHECK(nat < 256u);
            MT_CHECK(seen[nat] == 0);
            seen[nat] = 1;
            MT_CHECK_EQ_I64((int64_t)kTcZigzag16Inv[nat], (int64_t)i);
        }
        for (int i = 1; i < 256; ++i) {
            const uint32_t a = kTcZigzag16[i - 1], b = kTcZigzag16[i];
            MT_CHECK((a / 16 + a % 16) <= (b / 16 + b % 16));
        }
        MT_CHECK_EQ_I64(kTcZigzag16[0], 0);
        MT_CHECK_EQ_I64(kTcZigzag16[1], 1);    /* (0,1) */
        MT_CHECK_EQ_I64(kTcZigzag16[2], 16);   /* (1,0) */
        MT_CHECK_EQ_I64(kTcZigzag16[3], 32);   /* (2,0) */
        MT_CHECK_EQ_I64(kTcZigzag16[4], 17);   /* (1,1) */
        MT_CHECK_EQ_I64(kTcZigzag16[5], 2);    /* (0,2) */
        MT_CHECK_EQ_I64(kTcZigzag16[255], 255);
    }

    MT_MAIN_RETURN();
}
