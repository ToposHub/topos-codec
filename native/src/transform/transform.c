#include "transform/transform.h"

#include "simd/dispatch.h"
#include "entropy/scan.h"
#include "transform/transform_tables.h"

static int32_t tc_round_shift32(int64_t v)
{
    /* 规范禁止对负数右移（spec §2.4）：符号拆分 + 四舍五入 */
    if (v >= 0) {
        return (int32_t)((v + ((int64_t)1 << 31)) >> 32);
    }
    return -(int32_t)((-v + ((int64_t)1 << 31)) >> 32);
}

/* ---- scalar 规范性实现（dispatch 的回退与差分测试基准；bit-exact 冻结） ---- */

void tc_transform_forward_8x8_scalar(const int16_t x[64], int32_t F[64])
{
    /* A = x · Mᵀ（水平 pass）：A[y][v] = Σ_xx x[y][xx]·M[v][xx] */
    int32_t A[64];
    for (int y = 0; y < 8; ++y) {
        for (int v = 0; v < 8; ++v) {
            int32_t acc = 0;
            for (int xx = 0; xx < 8; ++xx) {
                acc += (int32_t)x[y * 8 + xx] * (int32_t)kTransformM[v * 8 + xx];
            }
            A[y * 8 + v] = acc;
        }
    }
    /* F = M · A（垂直 pass）：F[u][v] = Σ_y M[u][y]·A[y][v] */
    for (int u = 0; u < 8; ++u) {
        for (int v = 0; v < 8; ++v) {
            int32_t acc = 0;
            for (int y = 0; y < 8; ++y) {
                acc += (int32_t)kTransformM[u * 8 + y] * A[y * 8 + v];
            }
            F[u * 8 + v] = acc;
        }
    }
}

/* 批 4：int32 输入前向（bd≥13 通用路）——与 int16 版同矩阵同语义，
 * 仅输入容器放宽（|x| ≤ 2^(bd-1)-1）。scalar 规范实现；SIMD int32 快路
 * 与三路差分机随批 4 后续（当前 bd≥13 全走 scalar，正确性优先）。 */
void tc_transform_forward_8x8_i32_scalar(const int32_t x[64], int32_t F[64])
{
    int64_t A[64];
    for (int y = 0; y < 8; ++y) {
        for (int v = 0; v < 8; ++v) {
            int64_t acc = 0;
            for (int xx = 0; xx < 8; ++xx) {
                acc += (int64_t)x[y * 8 + xx] * (int32_t)kTransformM[v * 8 + xx];
            }
            A[y * 8 + v] = acc;
        }
    }
    for (int u = 0; u < 8; ++u) {
        for (int v = 0; v < 8; ++v) {
            int64_t acc = 0;
            for (int y = 0; y < 8; ++y) {
                acc += (int64_t)kTransformM[u * 8 + y] * A[y * 8 + v];
            }
            F[u * 8 + v] = (int32_t)acc;
        }
    }
}

void tc_transform_inverse_8x8_scalar(const int32_t coef[64], int32_t xhat[64])
{
    /* G = coef ⊙ W（含 1/(E_uE_v) 归一；s64，宽钳位下 |G| < 2^44） */
    int64_t G[64];
    for (int i = 0; i < 64; ++i) {
        int32_t c = coef[i];
        /* 批 4：宽钳位（bd≥13 合法域；|coef| ≤ 2^25 时行为逐位不变） */
        if (c > TC_TRANSFORM_MAX_ABS_F_WIDE) { c = TC_TRANSFORM_MAX_ABS_F_WIDE; }
        if (c < -TC_TRANSFORM_MAX_ABS_F_WIDE) { c = -TC_TRANSFORM_MAX_ABS_F_WIDE; }
        G[i] = (int64_t)c * (int64_t)kTransformW[i];
    }
    /* B[u][x] = Σ_v G[u][v]·M[v][x]。
     * M4 对称性：M 的每个行 v 关于列下标满足 M[v][7−x] = εv·M[v][x]
     *（偶数行对称、奇数行反对称——行 0/2/4/6 与 1/3/5/7 逐对验证成立），
     * 故 (u, x<4) 的 8 个乘积同时给出 B[u][x] 与 B[u][7−x]：
     *   B[u][x]   = pe + po，B[u][7−x] = pe − po
     * 其中 pe/po 为偶/奇 v 行的部分和。整数加法在 int64 无溢出域内可精确
     * 重结合（|B| < 2^46 < 2^63），与旧逐项求和 bit-exact。乘法数减半。 */
    int64_t B[64];
    for (int u = 0; u < 8; ++u) {
        for (int x = 0; x < 4; ++x) {
            int64_t pe = 0;
            int64_t po = 0;
            for (int v = 0; v < 8; ++v) {
                int64_t term = G[u * 8 + v] * (int64_t)kTransformM[v * 8 + x];
                if ((v & 1) != 0) { po += term; } else { pe += term; }
            }
            B[u * 8 + x] = pe + po;
            B[u * 8 + (7 - x)] = pe - po;
        }
    }
    /* x̂[y][x] = Σ_u M[u][y]·B[u][x]，整体 >>32 四舍五入。
     * 同一对称性作用于 u 求和（M[u][7−y] = εu·M[u][y]）：
     *   x̂[y][x] = round(qe + qo)，x̂[7−y][x] = round(qe − qo)
     * 乘法数减半；部分和 qe/qo 为精确整数，舍入前组合与旧实现一致。 */
    for (int y = 0; y < 4; ++y) {
        for (int xx = 0; xx < 8; ++xx) {
            int64_t qe = 0;
            int64_t qo = 0;
            for (int u = 0; u < 8; ++u) {
                int64_t term = (int64_t)kTransformM[u * 8 + y] * B[u * 8 + xx];
                if ((u & 1) != 0) { qo += term; } else { qe += term; }
            }
            xhat[y * 8 + xx] = tc_round_shift32(qe + qo);
            xhat[(7 - y) * 8 + xx] = tc_round_shift32(qe - qo);
        }
    }
}

/* ---- 公共入口：SIMD 运行时分发（阶段 9）；无可用后端时即上述 scalar ---- */

void tc_transform_forward_8x8(const int16_t x[64], int32_t F[64])
{
    tc_simd_forward_8x8(x, F);
}

/* 批 4 阶段 2：i32 前向公共入口（AVX2/NEON 运行时分发；bd≤16 全域与
 * scalar bit-exact，三路差分由 test_transform 钉死） */
void tc_transform_forward_8x8_i32(const int32_t x[64], int32_t F[64])
{
    tc_simd_forward_8x8_i32(x, F);
}

/* 符号拆分四舍五入除法（负数不右移，spec §2.4） */
static int32_t predistort_div(int64_t num, int64_t w)
{
    if (num >= 0) { return (int32_t)((2 * num + w) / (2 * w)); }
    return -(int32_t)((-2 * num + w) / (2 * w));
}

void tc_transform_forward_predistort_i32(int32_t F[64])
{
    /* F' = round(F · 2^32/(W·EuEv))：解码端每系数增益 W/2^32 ≈ (1+ε)/(EuEv)
     * （|ε| ≤ 0.5/W 为 W 表项舍入），预除 (1+ε) 后解码总增益恰为 1/(EuEv)。
     * 域：|F| ≤ 2^29.4（f_clamp 16-bit 外推）× 2^32 = 2^61.4 < 2^63；
     * W·EuEv ≤ 2^32·(1+2.1e-4) < 2^63。 */
    for (int i = 0; i < 64; ++i) {
        const int64_t eu_ev = (int64_t)kTransformE[i >> 3] * (int64_t)kTransformE[i & 7];
        F[i] = predistort_div((int64_t)F[i] * ((int64_t)1 << 32),
                              (int64_t)kTransformW[i] * eu_ev);
    }
}

void tc_transform_inverse_8x8(const int32_t coef[64], int32_t xhat[64])
{
    tc_simd_inverse_8x8(coef, xhat);
}

void tc_transform_inverse_8x8_samples_scalar(const int32_t coef[64],
                                             const uint8_t* xs, const uint8_t* ys,
                                             uint32_t count, int32_t* out)
{
    if (coef == NULL || xs == NULL || ys == NULL || out == NULL) { return; }
    if (count == 0u) { return; }

    /* 与规范 scalar 逆变换相同的 G = coef ⊙ W。只保留 64 项系数，避免
     * 为每个目标采样点重复反量化；sample_count 通常是 1..4（8K→2K）。 */
    int64_t G[64];
    for (uint32_t i = 0u; i < 64u; ++i) {
        int32_t c = coef[i];
        if (c > TC_TRANSFORM_MAX_ABS_F_WIDE) { c = TC_TRANSFORM_MAX_ABS_F_WIDE; }
        if (c < -TC_TRANSFORM_MAX_ABS_F_WIDE) { c = -TC_TRANSFORM_MAX_ABS_F_WIDE; }
        G[i] = (int64_t)c * (int64_t)kTransformW[i];
    }
    for (uint32_t n = 0u; n < count; ++n) {
        const uint32_t x = xs[n] & 7u;
        const uint32_t y = ys[n] & 7u;
        int64_t acc = 0;
        for (uint32_t u = 0u; u < 8u; ++u) {
            int64_t bx = 0;
            for (uint32_t v = 0u; v < 8u; ++v) {
                bx += G[u * 8u + v] * (int64_t)kTransformM[v * 8u + x];
            }
            acc += (int64_t)kTransformM[u * 8u + y] * bx;
        }
        out[n] = tc_round_shift32(acc);
    }
}

void tc_transform_inverse_8x8_samples_scalar_limited(const int32_t coef[64],
                                                     const uint8_t* xs,
                                                     const uint8_t* ys,
                                                     uint32_t count,
                                                     uint8_t max_scan_pos,
                                                     int32_t* out)
{
    if (coef == NULL || xs == NULL || ys == NULL || out == NULL || count == 0u) {
        return;
    }
    const uint32_t limit = max_scan_pos > 63u ? 63u : (uint32_t)max_scan_pos;
    for (uint32_t n = 0u; n < count; ++n) {
        const uint32_t x = xs[n] & 7u;
        const uint32_t y = ys[n] & 7u;
        int64_t acc = 0;
        /* 固定 band 的 scan 列表避免遍历并判断 64 个自然序系数；
         * B0/B1/B2/B3 分别只走 5/11/17/25 项。 */
        for (uint32_t scan_pos = 0u; scan_pos <= limit; ++scan_pos) {
            const uint32_t natural = kTcZigzag[scan_pos];
            int32_t c = coef[natural];
            if (c > TC_TRANSFORM_MAX_ABS_F_WIDE) { c = TC_TRANSFORM_MAX_ABS_F_WIDE; }
            if (c < -TC_TRANSFORM_MAX_ABS_F_WIDE) { c = -TC_TRANSFORM_MAX_ABS_F_WIDE; }
            const uint32_t u = natural >> 3u;
            const uint32_t v = natural & 7u;
            acc += (int64_t)c * (int64_t)kTransformW[natural]
                 * (int64_t)kTransformM[v * 8u + x]
                 * (int64_t)kTransformM[u * 8u + y];
        }
        out[n] = tc_round_shift32(acc);
    }
}

void tc_transform_inverse_8x8_samples(const int32_t coef[64],
                                      const uint8_t* xs, const uint8_t* ys,
                                      uint32_t count, int32_t* out)
{
    tc_simd_inverse_8x8_samples(coef, xs, ys, count, out);
}

void tc_transform_inverse_8x8_samples_limited(const int32_t coef[64],
                                              const uint8_t* xs, const uint8_t* ys,
                                              uint32_t count, uint8_t max_scan_pos,
                                              int32_t* out)
{
    tc_simd_inverse_8x8_samples_limited(coef, xs, ys, count, max_scan_pos, out);
}

const int16_t* tc_transform_matrix(void) { return kTransformM; }
const uint32_t* tc_transform_weights(void) { return kTransformW; }
const uint32_t* tc_transform_energies(void) { return kTransformE; }
