/* ARM NEON 前向变换内核（阶段 9）—— 与 scalar 逐位一致（交叉编译验证 + 差分测试）。
 *
 * 精确性：vmlaq_n_s32 全 i32 乘加，界与 AVX2 版相同（|A| < 2^18，|F| < 2^25，
 * 依据 ADR-C002 的 12-bit 规范界推导）→ 无舍入无溢出，bit-exact。
 *
 * 逆变换 NEON 化决策（ADR-C009）：i64 中间域（|acc| < 2^50）需 64 位乘加，
 * NEON 仅有 vmull 32×32→64，拆肢体实测收益不足 —— 阶段 9 保持标量参考路径
 * （dispatch 自动回退），待 ARM 硬件实测后再迭代。
 * 布局表懒生成（C-77）：首用 CAS 单写者填充（MSVC 无 constructor 属性；
 * 亦解除 dylib 卸载时的加载顺序依赖）。
 */
#include <stdatomic.h>
#include <string.h>

#include "../transform/transform.h"
#include "../transform/transform_tables.h"
#include "../transform/quant.h"
/* kTcZigzag（inverse samples 内核的扫描序→自然序翻译表）*/
#include "../entropy/scan.h"

#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON__) || \
    (defined(__ARM_NEON) && defined(__ARM_NEON_FP))
#include <arm_neon.h>

static int32_t kMT32[8][8]; /* kMT32[c][v] = M[v][c]（pass1 乘数列） */
static int32_t kM32[8][8];  /* kM32[u][y] = M[u][y]（pass2 乘数行） */
/* 融合反量化内核：kWMbN[u][v][i] = W[u][v]·M[v][i]（i = x0..3；
 * |W·M| ≤ 10228·13 < 2^18，i32 域）。AVX2 版以 i64 lane 低 32 位承载，
 * NEON 直接存 i32x4。 */
static int32_t kWMbN[8][8][4];
static atomic_int s_tables_state; /* 0 未生成 / 1 生成中 / 2 就绪 */

static void transform_neon_tables_fill(void)
{
    for (int a = 0; a < 8; ++a) {
        for (int b = 0; b < 8; ++b) {
            kM32[a][b] = (int32_t)kTransformM[a * 8 + b];
            kMT32[a][b] = (int32_t)kTransformM[b * 8 + a];
        }
    }
    for (int u = 0; u < 8; ++u) {
        for (int v = 0; v < 8; ++v) {
            for (int i = 0; i < 4; ++i) {
                kWMbN[u][v][i] = (int32_t)(
                    (int64_t)kTransformW[u * 8 + v]
                    * (int64_t)kTransformM[v * 8 + i]);
            }
        }
    }
    atomic_store(&s_tables_state, 2);
}

static void transform_neon_tables_ensure(void)
{
    if (atomic_load_explicit(&s_tables_state, memory_order_acquire) == 2) { return; }
    int expected = 0;
    if (atomic_compare_exchange_strong(&s_tables_state, &expected, 1)) {
        transform_neon_tables_fill();
        return;
    }
    while (atomic_load_explicit(&s_tables_state, memory_order_acquire) != 2) {
    }
}
#endif


#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON__) || \
    (defined(__ARM_NEON) && defined(__ARM_NEON_FP))

void tc_transform_forward_8x8_neon(const int16_t x[64], int32_t F[64])
{
    transform_neon_tables_ensure();
    /* pass1：每行两段 int32x4（v 0..3 / 4..7），寄存器保存 A */
    int32x4_t a[8][2];
    for (int y = 0; y < 8; ++y) {
        int32x4_t acc0 = vdupq_n_s32(0);
        int32x4_t acc1 = vdupq_n_s32(0);
        const int16_t* xr = x + y * 8;
        for (int c = 0; c < 8; ++c) {
            int32_t xv = (int32_t)xr[c];
            acc0 = vmlaq_n_s32(acc0, vld1q_s32(kMT32[c]), xv);
            acc1 = vmlaq_n_s32(acc1, vld1q_s32(kMT32[c] + 4), xv);
        }
        a[y][0] = acc0;
        a[y][1] = acc1;
    }
    /* pass2：F[u][:] = Σ_y M[u][y]·A[y][:] */
    for (int u = 0; u < 8; ++u) {
        int32x4_t acc0 = vdupq_n_s32(0);
        int32x4_t acc1 = vdupq_n_s32(0);
        for (int y = 0; y < 8; ++y) {
            int32_t my = kM32[u][y];
            acc0 = vmlaq_n_s32(acc0, a[y][0], my);
            acc1 = vmlaq_n_s32(acc1, a[y][1], my);
        }
        vst1q_s32(F + u * 8, acc0);
        vst1q_s32(F + u * 8 + 4, acc1);
    }
}

/* ---- 批 4 阶段 2：i32 输入前向内核（bd≥13 通用路） ----
 * 与 tc_transform_forward_8x8_i32_scalar 逐位一致：vmlaq 系 i32 乘加，
 * bd≤16 域界 |A| ≤ 8·32767·13 < 2^22、|F| ≤ 8·13·|A|max < 2^29（i32 通道
 * 无溢出），整数加法精确重结合 → 累加序差异不改变结果。 */
void tc_transform_forward_8x8_i32_neon(const int32_t x[64], int32_t F[64])
{
    transform_neon_tables_ensure();
    /* pass1：A[y][v] = Σ_c x[y][c]·M[v][c]——x 行按 c 广播（i32 域免转换） */
    int32x4_t a[8][2];
    for (int y = 0; y < 8; ++y) {
        int32x4_t acc0 = vdupq_n_s32(0);
        int32x4_t acc1 = vdupq_n_s32(0);
        const int32_t* xr = x + y * 8;
        for (int c = 0; c < 8; ++c) {
            const int32x4_t xv = vdupq_n_s32(xr[c]);
            acc0 = vmlaq_s32(acc0, vld1q_s32(kMT32[c]), xv);
            acc1 = vmlaq_s32(acc1, vld1q_s32(kMT32[c] + 4), xv);
        }
        a[y][0] = acc0;
        a[y][1] = acc1;
    }
    /* pass2：F[u][:] = Σ_y M[u][y]·A[y][:] */
    for (int u = 0; u < 8; ++u) {
        int32x4_t acc0 = vdupq_n_s32(0);
        int32x4_t acc1 = vdupq_n_s32(0);
        for (int y = 0; y < 8; ++y) {
            const int32x4_t my = vdupq_n_s32(kM32[u][y]);
            acc0 = vmlaq_s32(acc0, a[y][0], my);
            acc1 = vmlaq_s32(acc1, a[y][1], my);
        }
        vst1q_s32(F + u * 8, acc0);
        vst1q_s32(F + u * 8 + 4, acc1);
    }
}

/* 采样逆变换：AArch64 用 i64x2 并行计算两个目标点，保持与 scalar
 * 的整数乘法、符号四舍五入逐位一致；RD3-02 limited 入口按 band 遍历
 * 固定的 scan 前缀。 */
static void tc_transform_inverse_8x8_samples_neon_impl(
    const int32_t coef[64], const uint8_t* xs, const uint8_t* ys,
    uint32_t count, uint8_t max_scan_pos, int32_t* out)
{
    transform_neon_tables_ensure();
    if (count == 0u) { return; }
    const uint32_t limit = max_scan_pos > 63u ? 63u : (uint32_t)max_scan_pos;
    for (uint32_t base = 0u; base < count; base += 2u) {
        const uint32_t n = (count - base) < 2u ? (count - base) : 2u;
        int64_t sums[2] = { 0, 0 };
        for (uint32_t scan_pos = 0u; scan_pos <= limit; ++scan_pos) {
            const uint32_t natural = kTcZigzag[scan_pos];
            const uint32_t u = natural >> 3u;
            const uint32_t v = natural & 7u;
            int64_t weights[2];
            for (uint32_t lane = 0u; lane < 2u; ++lane) {
                const uint32_t x = lane < n ? (uint32_t)(xs[base + lane] & 7u) : 0u;
                const uint32_t y = lane < n ? (uint32_t)(ys[base + lane] & 7u) : 0u;
                weights[lane] = (int64_t)kTransformW[natural]
                              * (int64_t)kTransformM[v * 8u + x]
                              * (int64_t)kTransformM[u * 8u + y];
            }
            int32_t coeff = coef[natural];
            /* 批 4：宽钳位（bd≥13 合法域；i64 累加域 |acc| < 2^57 已证） */
            if (coeff > TC_TRANSFORM_MAX_ABS_F_WIDE) { coeff = TC_TRANSFORM_MAX_ABS_F_WIDE; }
            else if (coeff < -TC_TRANSFORM_MAX_ABS_F_WIDE) { coeff = -TC_TRANSFORM_MAX_ABS_F_WIDE; }
            /* 2-lane i64 乘加走标量：vmulq_s64 在 Apple clang≤12 的
             * arm_neon.h 缺失（该 intrinsic 对 2 lane 无向量收益，整数
             * 语义逐位一致）。 */
            for (uint32_t lane = 0u; lane < 2u; ++lane) {
                sums[lane] += (int64_t)coeff * weights[lane];
            }
        }
        for (uint32_t lane = 0u; lane < n; ++lane) {
            const int64_t av = sums[lane] >= 0 ? sums[lane] : -sums[lane];
            const int64_t rounded = (av + ((int64_t)1 << 31)) >> 32;
            out[base + lane] = sums[lane] >= 0 ? (int32_t)rounded : -(int32_t)rounded;
        }
    }
}

void tc_transform_inverse_8x8_samples_neon(const int32_t coef[64], const uint8_t* xs,
                                           const uint8_t* ys, uint32_t count, int32_t* out)
{
    tc_transform_inverse_8x8_samples_neon_impl(coef, xs, ys, count, 63u, out);
}

void tc_transform_inverse_8x8_samples_neon_limited(const int32_t coef[64],
                                                   const uint8_t* xs, const uint8_t* ys,
                                                   uint32_t count, uint8_t max_scan_pos,
                                                   int32_t* out)
{
    tc_transform_inverse_8x8_samples_neon_impl(coef, xs, ys, count,
                                               max_scan_pos, out);
}

/* ---- 解码深化批次：融合反量化 + 逆变换（解码 sink 热路径，NEON） ----
 * 与 tc_dequant_inverse_8x8_avx2 同构移植（数学推导与溢出域见该处）：
 *  - AVX2 的 4×i64 lane 表达为 2×int64x2（lo/hi）；乘法全走
 *    vmull_s32 32×32→64 加宽（p ≤ 2^25、|W·M| < 2^18、16-bit 分解
 *    分量均落 i32 域），无需 lane 漏斗；
 *  - 行稀疏掩码跳过（bit u = 频率行 u 存在非零系数）+ 对称输出
 *    （b[u][1] = rev(pe−po) 承载 x7..4；x̂[7−y] = te−to）与 AVX2 同源；
 *  - 与「tc_dequant_block_ctx + 逆变换」逐位一致（整数运算无舍入，
 *    test_transform 差分钉死）。溢出域：|p| ≤ 2^25、|WM| < 2^18 →
 *    |B| < 2^46；pass3 和 < 2^52（64-bit 精确）。
 * Apple clang 12 的 arm_neon.h 无 64-lane 乘法 intrinsic——本内核
 * 全部乘法经 vmull_s32 加宽完成，无此依赖。 */

/* 4×i64 lane 的 NEON 表达（lo = lane0,1；hi = lane2,3） */
typedef struct { int64x2_t lo, hi; } tc_i64x4;

static inline tc_i64x4 tc_i64x4_zero(void)
{
    tc_i64x4 r = { vdupq_n_s64(0), vdupq_n_s64(0) };
    return r;
}

static inline tc_i64x4 tc_i64x4_mul_bcast_i32(int32_t p, int32x4_t w)
{
    tc_i64x4 r = {
        vmull_s32(vdup_n_s32(p), vget_low_s32(w)),
        vmull_s32(vdup_n_s32(p), vget_high_s32(w)),
    };
    return r;
}

static inline int64x2_t tc_round32_i64(int64x2_t t, int64x2_t vzero, int64x2_t vr)
{
    /* t ≥ 0 → (t+2^31)>>32；t < 0 → −((−t+2^31)>>32)。逻辑右移作用于
     * |t|，符号经 xor/sub 恢复（与 AVX2 版 cmpgt/xor/sub 同式） */
    const uint64x2_t sg = vcgtq_s64(vzero, t);              /* t < 0 */
    const int64x2_t sgi = vreinterpretq_s64_u64(sg);        /* 全1/全0 */
    const int64x2_t av = vsubq_s64(veorq_s64(t, sgi), sgi); /* |t| */
    const uint64x2_t r = vshrq_n_u64(
        vreinterpretq_u64_s64(vaddq_s64(av, vr)), 32);
    return vsubq_s64(veorq_s64(vreinterpretq_s64_u64(r), sgi), sgi);
}

void tc_dequant_inverse_8x8_neon(const tc_quant_ctx* ctx, const int32_t q[64],
                                 int32_t xhat[64], uint32_t ac_rowmask)
{
    transform_neon_tables_ensure();
    const uint32_t* Q = ctx->Q;
    const uint32_t rowmask = ac_rowmask;

    /* 活动行紧凑列表：真实素材主流形态是低带稀疏（rowmask 多为
     * 0b1/0b11——DC 行 + 首个 AC 行），pass B/16-bit 分解只跑活动行、
     * pass3 免 8×8 全巡掩码检查。全零行贡献加零向量（数学恒等，
     * 差分钉死）。 */
    int act[8];
    int nact = 0;
    for (int u = 0; u < 8; ++u) {
        if ((rowmask & (1u << u)) != 0u) { act[nact++] = u; }
    }
    if (nact == 0u) {
        memset(xhat, 0, 64 * sizeof(int32_t)); /* round32(0) = 0 */
        return;
    }

    /* pass B（活动行，x0..3）：偶/奇 v 分解 → B[u][x0..3] = pe+po；
     * b[u][1] = rev(pe−po) 承载 x7..4（变换矩阵的棋盘符号对称性）。
     * 行内即做 16-bit 分解（B = BHi·2^16 + BLo；|BHi| < 2^30、
     * |BLo| < 2^16，i32 无损窄化——pass3 走 vmull_s32）；
     * 非活动行不分解不初始化（pass3 经 act 列表只读活动行）。 */
    int32x4_t bhi[8][2];
    int32x4_t blo[8][2];
    const int64x2_t m16 = vdupq_n_s64((int64_t)0xFFFF);
    for (int k = 0; k < nact; ++k) {
        const int u = act[k];
        tc_i64x4 pe = tc_i64x4_zero();
        tc_i64x4 po = tc_i64x4_zero();
        for (int v = 0; v < 8; ++v) {
            const int32_t c0 = q[u * 8 + v];
            /* 内联反量化（s64 精确 + 防御钳位，同 tc_dequant_block_ctx；
             * c0==0 → p=0，无分支） */
            int64_t p = (int64_t)c0 * (int64_t)Q[u * 8 + v];
            if (p > (int64_t)TC_TRANSFORM_MAX_ABS_F) {
                p = (int64_t)TC_TRANSFORM_MAX_ABS_F;
            } else if (p < -(int64_t)TC_TRANSFORM_MAX_ABS_F) {
                p = -(int64_t)TC_TRANSFORM_MAX_ABS_F;
            }
            const tc_i64x4 pr = tc_i64x4_mul_bcast_i32(
                (int32_t)p, vld1q_s32(&kWMbN[u][v][0]));
            if ((v & 1) == 0) {
                pe.lo = vaddq_s64(pe.lo, pr.lo);
                pe.hi = vaddq_s64(pe.hi, pr.hi);
            } else {
                po.lo = vaddq_s64(po.lo, pr.lo);
                po.hi = vaddq_s64(po.hi, pr.hi);
            }
        }
        {   /* (d0,d1|d2,d3) → (d3,d2|d1,d0)（对称承载 x7..4） */
            const int64x2_t dlo = vsubq_s64(pe.lo, po.lo);
            const int64x2_t dhi = vsubq_s64(pe.hi, po.hi);
            const tc_i64x4 b1 = { vextq_s64(dhi, dhi, 1), vextq_s64(dlo, dlo, 1) };
            const tc_i64x4 b0 = { vaddq_s64(pe.lo, po.lo), vaddq_s64(pe.hi, po.hi) };
            bhi[u][0] = vcombine_s32(
                vmovn_s64(vshrq_n_s64(b0.lo, 16)),
                vmovn_s64(vshrq_n_s64(b0.hi, 16)));
            blo[u][0] = vcombine_s32(
                vmovn_s64(vandq_s64(b0.lo, m16)),
                vmovn_s64(vandq_s64(b0.hi, m16)));
            bhi[u][1] = vcombine_s32(
                vmovn_s64(vshrq_n_s64(b1.lo, 16)),
                vmovn_s64(vshrq_n_s64(b1.hi, 16)));
            blo[u][1] = vcombine_s32(
                vmovn_s64(vandq_s64(b1.lo, m16)),
                vmovn_s64(vandq_s64(b1.hi, m16)));
        }
    }

    /* pass 3（x0..3 / x4..7）+ 对称输出 + round32（活动行紧凑迭代） */
    const int64x2_t vr = vdupq_n_s64((int64_t)1 << 31);
    const int64x2_t vzero = vdupq_n_s64(0);
    for (int y = 0; y < 4; ++y) {
        for (int g = 0; g < 2; ++g) {
            int64x2_t qe_hi_lo = vzero, qe_hi_hi = vzero;
            int64x2_t qe_lo_lo = vzero, qe_lo_hi = vzero;
            int64x2_t qo_hi_lo = vzero, qo_hi_hi = vzero;
            int64x2_t qo_lo_lo = vzero, qo_lo_hi = vzero;
            for (int k = 0; k < nact; ++k) {
                const int u = act[k];
                const int32x2_t my = vdup_n_s32(kM32[u][y]);
                const int64x2_t h_lo = vmull_s32(my, vget_low_s32(bhi[u][g]));
                const int64x2_t h_hi = vmull_s32(my, vget_high_s32(bhi[u][g]));
                const int64x2_t l_lo = vmull_s32(my, vget_low_s32(blo[u][g]));
                const int64x2_t l_hi = vmull_s32(my, vget_high_s32(blo[u][g]));
                if ((u & 1) == 0) {
                    qe_hi_lo = vaddq_s64(qe_hi_lo, h_lo);
                    qe_hi_hi = vaddq_s64(qe_hi_hi, h_hi);
                    qe_lo_lo = vaddq_s64(qe_lo_lo, l_lo);
                    qe_lo_hi = vaddq_s64(qe_lo_hi, l_hi);
                } else {
                    qo_hi_lo = vaddq_s64(qo_hi_lo, h_lo);
                    qo_hi_hi = vaddq_s64(qo_hi_hi, h_hi);
                    qo_lo_lo = vaddq_s64(qo_lo_lo, l_lo);
                    qo_lo_hi = vaddq_s64(qo_lo_hi, l_hi);
                }
            }
            const int64x2_t te_lo = vaddq_s64(
                vshlq_n_s64(qe_hi_lo, 16), qe_lo_lo);
            const int64x2_t te_hi = vaddq_s64(
                vshlq_n_s64(qe_hi_hi, 16), qe_lo_hi);
            const int64x2_t to_lo = vaddq_s64(
                vshlq_n_s64(qo_hi_lo, 16), qo_lo_lo);
            const int64x2_t to_hi = vaddq_s64(
                vshlq_n_s64(qo_hi_hi, 16), qo_lo_hi);
            const int64x2_t tp_lo = vaddq_s64(te_lo, to_lo);
            const int64x2_t tp_hi = vaddq_s64(te_hi, to_hi);
            const int64x2_t tm_lo = vsubq_s64(te_lo, to_lo);
            const int64x2_t tm_hi = vsubq_s64(te_hi, to_hi);
            int64_t op[4];
            int64_t om[4];
            vst1q_s64(&op[0], tc_round32_i64(tp_lo, vzero, vr));
            vst1q_s64(&op[2], tc_round32_i64(tp_hi, vzero, vr));
            vst1q_s64(&om[0], tc_round32_i64(tm_lo, vzero, vr));
            vst1q_s64(&om[2], tc_round32_i64(tm_hi, vzero, vr));
            for (int i = 0; i < 4; ++i) {
                xhat[y * 8 + g * 4 + i] = (int32_t)op[i];
                xhat[(7 - y) * 8 + g * 4 + i] = (int32_t)om[i];
            }
        }
    }
}

/* M10-1d 同款：公开入口——dispatch 在 resolve 时一次完成表生成，
 * 块循环内 per-call ensure 恒命中 acquire 快路 */
void tc_transform_neon_tables_ensure_public(void)
{
    transform_neon_tables_ensure();
}

/* ======================= M10-6.3B / P-速⑥：NEON 量化 =======================
 *
 * 语义 == tc_quant_block_zigzag / tc_quant_block_ctx（标量），与 AVX2 版
 * （transform_avx2.c）同一数学：
 *  - clamp ±2^25 → |·| → +half → 死区 max(num−dz, 0)——**有符号** max/sub
 *    （num−dz 可负；NEON 若走无符号回绕会破坏 max(nd,0) 语义）；
 *  - 快速除法 q=(n·magic)>>51 的恒等 64 位分解：magic=(m1<<26)|m0，
 *    X=n·m1（≤2^51）、Y=n·m0（≤2^52）→
 *      q = (X>>25) + (((X&(2^25−1))<<26) + Y)>>51（纯整数恒等式）；
 *    NEON 以 vmull_u32/vmull_high_u32 承载 32×32→64（i32x4 一组两次），
 *    无需 AVX2 的偶/奇 lane 漏斗与末端 shuffle 交织；
 *  - 符号恢复（q 恒 ≥ 0）：f<0 → −q 经「加符号位再异或」实现
 *    （(q + (−1)) ^ (−1) = −q，补码恒等；不依赖 aarch64 专属 intrinsic）。
 * 非零掩码：q 落 q_out 后标量取位（8 次 L1 热读，相对除法开销可忽略——
 * NEON 无 movemask，向量归并反而更贵）。 */

static inline int32x4_t tc_quant4_neon(int32x4_t f,
                                       int32x4_t half4, int32x4_t dz4,
                                       uint32x4_t m1_4, uint32x4_t m0_4)
{
    const int32x4_t vmax = vdupq_n_s32((int32_t)TC_TRANSFORM_MAX_ABS_F);
    const int32x4_t vzero = vdupq_n_s32(0);
    f = vminq_s32(vmaxq_s32(f, vnegq_s32(vmax)), vmax);
    const int32x4_t num = vaddq_s32(vabsq_s32(f), half4);
    const int32x4_t ne = vmaxq_s32(vsubq_s32(num, dz4), vzero);
    const uint32x4_t n = vreinterpretq_u32_s32(ne); /* ne ≥ 0，无损重解释 */

    const uint64x2_t x_lo = vmull_u32(vget_low_u32(n), vget_low_u32(m1_4));
    const uint64x2_t x_hi = vmull_high_u32(n, m1_4);
    const uint64x2_t y_lo = vmull_u32(vget_low_u32(n), vget_low_u32(m0_4));
    const uint64x2_t y_hi = vmull_high_u32(n, m0_4);
    const uint64x2_t m25 = vdupq_n_u64(UINT64_C(0x1FFFFFF));
    const uint64x2_t q_lo = vaddq_u64(
        vshrq_n_u64(x_lo, 25),
        vshrq_n_u64(vaddq_u64(
            vshlq_n_u64(vandq_u64(x_lo, m25), 26), y_lo), 51));
    const uint64x2_t q_hi = vaddq_u64(
        vshrq_n_u64(x_hi, 25),
        vshrq_n_u64(vaddq_u64(
            vshlq_n_u64(vandq_u64(x_hi, m25), 26), y_hi), 51));
    const int32x4_t q = vreinterpretq_s32_u32(
        vcombine_u32(vmovn_u64(q_lo), vmovn_u64(q_hi))); /* q < 2^26 无损窄化 */

    const int32x4_t sgn = vshrq_n_s32(f, 31);           /* f<0 → 全 1 */
    return veorq_s32(vaddq_s32(q, sgn), sgn);           /* f<0 → −q */
}

void tc_quant_block_zigzag_neon(const tc_quant_ctx* ctx, const int32_t* F,
                                int32_t* q_out, uint64_t* nz_zz)
{
    uint64_t nz = 0u;
    for (int v = 0; v < 8; ++v) {
        /* 扫描序 gather（NEON 无 gather 指令，标量装载 8 系数） */
        int32_t g[8];
        for (int i = 0; i < 8; ++i) {
            g[i] = F[kTcZigzag[v * 8 + i]];
        }
        const int32_t* hz = (const int32_t*)(ctx->half_zz + v * 8);
        const int32_t* dzz = (const int32_t*)(ctx->dz_zz + v * 8);
        vst1q_s32(q_out + v * 8, tc_quant4_neon(
            vld1q_s32(g), vld1q_s32(hz), vld1q_s32(dzz),
            vld1q_u32(ctx->fd_zz_m1 + v * 8), vld1q_u32(ctx->fd_zz_m0 + v * 8)));
        vst1q_s32(q_out + v * 8 + 4, tc_quant4_neon(
            vld1q_s32(g + 4), vld1q_s32(hz + 4), vld1q_s32(dzz + 4),
            vld1q_u32(ctx->fd_zz_m1 + v * 8 + 4),
            vld1q_u32(ctx->fd_zz_m0 + v * 8 + 4)));
        for (int sp = 0; sp < 8; ++sp) {
            if (q_out[v * 8 + sp] != 0) { nz |= (uint64_t)1 << (v * 8 + sp); }
        }
    }
    *nz_zz = nz;
}

void tc_quant_block_nat_neon(const tc_quant_ctx* ctx, const int32_t* F,
                             int32_t* q_out, uint64_t* nz_nat)
{
    uint64_t nz = 0u;
    for (int v = 0; v < 8; ++v) {
        const int32_t* hz = (const int32_t*)(ctx->half + v * 8);
        const int32_t* dzz = (const int32_t*)(ctx->dz + v * 8);
        vst1q_s32(q_out + v * 8, tc_quant4_neon(
            vld1q_s32(F + v * 8), vld1q_s32(hz), vld1q_s32(dzz),
            vld1q_u32(ctx->fd_nat_m1 + v * 8),
            vld1q_u32(ctx->fd_nat_m0 + v * 8)));
        vst1q_s32(q_out + v * 8 + 4, tc_quant4_neon(
            vld1q_s32(F + v * 8 + 4), vld1q_s32(hz + 4), vld1q_s32(dzz + 4),
            vld1q_u32(ctx->fd_nat_m1 + v * 8 + 4),
            vld1q_u32(ctx->fd_nat_m0 + v * 8 + 4)));
        for (int k = 0; k < 8; ++k) {
            if (q_out[v * 8 + k] != 0) { nz |= (uint64_t)1 << (v * 8 + k); }
        }
    }
    *nz_nat = nz;
}

#endif /* aarch64 / NEON */

/* 非 NEON 切片（universal 构建）编译为空：ISO 要求 TU 至少一个声明 */
typedef int topos_neon_tu_placeholder;
