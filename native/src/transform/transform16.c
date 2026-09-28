/* 16×16 混合变换 scalar 规范性实现（ADR-C037 试点）。结构镜像
 * transform.c 的 8×8 版本（plain 双趟矩阵乘；对称减乘优化留给 SIMD 期）。 */
#include "transform/transform16.h"

#include "transform/transform16_tables.h"

static int32_t tc16_round_shift32(int64_t v)
{
    /* 规范禁止对负数右移：符号拆分 + 四舍五入（同 tc_round_shift32） */
    if (v >= 0) {
        return (int32_t)((v + ((int64_t)1 << 31)) >> 32);
    }
    return -(int32_t)((-v + ((int64_t)1 << 31)) >> 32);
}

void tc_transform16_forward(const int16_t x[256], int32_t F[256])
{
    /* A = x · M16ᵀ（水平 pass）：A[y][v] = Σ_xx x[y][xx]·M16[v][xx] */
    int32_t A[256];
    for (int y = 0; y < 16; ++y) {
        for (int v = 0; v < 16; ++v) {
            int32_t acc = 0;
            for (int xx = 0; xx < 16; ++xx) {
                acc += (int32_t)x[y * 16 + xx] * (int32_t)kTransform16M[v * 16 + xx];
            }
            A[y * 16 + v] = acc;
        }
    }
    /* F = M16 · A（垂直 pass）：F[u][v] = Σ_y M16[u][y]·A[y][v] */
    for (int u = 0; u < 16; ++u) {
        for (int v = 0; v < 16; ++v) {
            int32_t acc = 0;
            for (int y = 0; y < 16; ++y) {
                acc += (int32_t)kTransform16M[u * 16 + y] * A[y * 16 + v];
            }
            F[u * 16 + v] = acc;
        }
    }
}

void tc_transform16_inverse(const int32_t coef[256], int32_t xhat[256])
{
    /* G = coef ⊙ W16（含 1/(E_uE_v) 归一；s64，|G| < 2^39） */
    int64_t G[256];
    for (int i = 0; i < 256; ++i) {
        int32_t c = coef[i];
        if (c > TC_TRANSFORM16_MAX_ABS_F) { c = TC_TRANSFORM16_MAX_ABS_F; }
        if (c < -TC_TRANSFORM16_MAX_ABS_F) { c = -TC_TRANSFORM16_MAX_ABS_F; }
        G[i] = (int64_t)c * (int64_t)kTransform16W[i];
    }
    /* B[u][x] = Σ_v G[u][v]·M16[v][x] */
    int64_t B[256];
    for (int u = 0; u < 16; ++u) {
        for (int x = 0; x < 16; ++x) {
            int64_t acc = 0;
            for (int v = 0; v < 16; ++v) {
                acc += G[u * 16 + v] * (int64_t)kTransform16M[v * 16 + x];
            }
            B[u * 16 + x] = acc;
        }
    }
    /* x̂[y][x] = Σ_u M16[u][y]·B[u][x]，整体 >>32 四舍五入 */
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            int64_t acc = 0;
            for (int u = 0; u < 16; ++u) {
                acc += (int64_t)kTransform16M[u * 16 + y] * B[u * 16 + x];
            }
            xhat[y * 16 + x] = tc16_round_shift32(acc);
        }
    }
}

const int16_t* tc_transform16_matrix(void) { return kTransform16M; }
const uint32_t* tc_transform16_weights(void) { return kTransform16W; }
const uint32_t* tc_transform16_energies(void) { return kTransform16E; }
