/* color_convert 的 AVX2 内核（u8 输入族；2026-09-04 P-速③b）。
 *
 * 与 color_convert.c 标量路径**逐位一致**（契约推导见 color_convert_simd.h）：
 *  - div_ps/div_ss 同为 IEEE 正确舍入；round_ps(NE)=rintf 默认舍入；
 *  - 逐算子顺序 = cvt_yuv_pixel/cvt_q/cvt_box2/cvt_alpha 的显式内联重演
 *    （全部 intrinsic，无 C 浮点表达式 → 无 FMA 收缩面）；
 *  - 值域：量化结果 ∈ [0, scale] ⊂ [0, 65535]，packus_epi32 不触发饱和；
 *    422 box 输入为已钳位整型（≤ 2·4095，f32 精确）。
 * 平台：gcc/clang 函数级 target("avx2")；MSVC 整 TU /arch:AVX2
 * （CMake per-source，同 transform_avx2.c）。非 x86_64 编译切片为空。
 * 行尾 <8 像素经 tail 回调落回标量（同一份冻结数学，零重复）。 */
#if defined(__x86_64__) || defined(_M_X64)

#if defined(_MSC_VER)
#define TOPOS_SIMD_TARGET
#else
#define TOPOS_SIMD_TARGET __attribute__((target("avx2")))
#endif

#include <immintrin.h>
#include <string.h>

#include "../common/color_convert_simd.h"
#include "topos_codec.h" /* TOPOS_CVT_OUT_*（public include 路径） */

/* 舍入模式 imm：0x08=半到偶 | 0x04=不抛异常（= _MM_FROUND_TO_NEAREST_EVEN
 * |_MM_FROUND_NO_EXC 的数值；target 属性不影响预处理器宏，故直接写值） */
#define CVT_RNE (0x08 | 0x04)

/* ---- nch=3 通道抽取（24 字节 → 8 通道值）----
 * pos ∈ {0,1,2}（字节偏移；BGR/RGB 族由调用方传对应偏移）。
 * v0 = 字节 0..15，v1 = 字节 16..31（24..31 为栈 pad，shuffle 恒不选中）。 */
static TOPOS_SIMD_TARGET __m128i extract3(__m128i v0, __m128i v1, uint32_t pos)
{
    __m128i t0, t1, m;
    switch (pos) {
    case 0u: /* 字节 0,3,6,9,12,15 | 18,21 */
        t0 = _mm_shuffle_epi8(v0, _mm_setr_epi8(0, 3, 6, 9, 12, 15, -1, -1,
                                                -1, -1, -1, -1, -1, -1, -1, -1));
        t1 = _mm_shuffle_epi8(v1, _mm_setr_epi8(2, 5, -1, -1, -1, -1, -1, -1,
                                                -1, -1, -1, -1, -1, -1, -1, -1));
        t1 = _mm_slli_si128(t1, 6);
        m = _mm_setr_epi8(0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
        break;
    case 1u: /* 字节 1,4,7,10,13 | 16,19,22 */
        t0 = _mm_shuffle_epi8(v0, _mm_setr_epi8(1, 4, 7, 10, 13, -1, -1, -1,
                                                -1, -1, -1, -1, -1, -1, -1, -1));
        t1 = _mm_shuffle_epi8(v1, _mm_setr_epi8(0, 3, 6, -1, -1, -1, -1, -1,
                                                -1, -1, -1, -1, -1, -1, -1, -1));
        t1 = _mm_slli_si128(t1, 5);
        m = _mm_setr_epi8(0, 0, 0, 0, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
        break;
    default: /* 字节 2,5,8,11,14 | 17,20,23 */
        t0 = _mm_shuffle_epi8(v0, _mm_setr_epi8(2, 5, 8, 11, 14, -1, -1, -1,
                                                -1, -1, -1, -1, -1, -1, -1, -1));
        t1 = _mm_shuffle_epi8(v1, _mm_setr_epi8(1, 4, 7, -1, -1, -1, -1, -1,
                                                -1, -1, -1, -1, -1, -1, -1, -1));
        t1 = _mm_slli_si128(t1, 5);
        m = _mm_setr_epi8(0, 0, 0, 0, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
        break;
    }
    return _mm_blendv_epi8(t0, t1, m);
}

/* ---- nch=4 通道抽取（32 字节 = 恰两载荷，无 pad）---- */
static TOPOS_SIMD_TARGET __m128i extract4(__m128i v0, __m128i v1, uint32_t pos)
{
    const __m128i mask = _mm_setr_epi8((int8_t)(pos + 0), (int8_t)(pos + 4),
                                       (int8_t)(pos + 8), (int8_t)(pos + 12),
                                       -1, -1, -1, -1, -1, -1, -1, -1,
                                       -1, -1, -1, -1);
    const __m128i t0 = _mm_shuffle_epi8(v0, mask); /* 像素 0..3 → 字节 0..3 */
    const __m128i t1 = _mm_shuffle_epi8(v1, mask); /* 像素 4..7 → 字节 0..3 */
    /* t1 的 4 值左移到字节 4..7 后按字混合（字 2,3 = 字节 4..7 → imm 0x0C）：
     * 结果 = [v0 组 | v1 组] */
    return _mm_blend_epi16(t0, _mm_slli_si128(t1, 4), 0x0C);
}

/* rint(NE) → clamp[0, scale]（= 标量 cvt_q 的舍入/钳位序列，f32 域） */
static TOPOS_SIMD_TARGET __m256 cvt_clampf(__m256 f, __m256 zero, __m256 hi)
{
    __m256 r = _mm256_round_ps(f, CVT_RNE);
    r = _mm256_max_ps(r, zero);
    return _mm256_min_ps(r, hi);
}

/* 8×f32（已钳位整型）→ 8×u16 顺序存储 */
static TOPOS_SIMD_TARGET void store8_u16(uint16_t* dst, __m256 f, __m256 zero,
                                         __m256 hi)
{
    const __m256i v = _mm256_cvtps_epi32(cvt_clampf(f, zero, hi));
    const __m256i p = _mm256_permute4x64_epi64(_mm256_packus_epi32(v, v), 0x88);
    _mm_storeu_si128((__m128i*)dst, _mm256_castsi256_si128(p));
}

/* 422 色度 box：8×已钳位整型 f32 → 偶/奇配对 rint((a+b)*0.5) → 4×u16 */
static TOPOS_SIMD_TARGET void store4_box(uint16_t* dst, __m256 q, __m128 half)
{
    const __m128 lo = _mm256_castps256_ps128(q);
    const __m128 hi = _mm256_extractf128_ps(q, 1);
    const __m128 ev = _mm_shuffle_ps(lo, hi, 0x88);
    const __m128 od = _mm_shuffle_ps(lo, hi, 0xDD);
    const __m128 box = _mm_round_ps(
        _mm_mul_ps(_mm_add_ps(ev, od), half), CVT_RNE);
    const __m128i v = _mm_packus_epi32(_mm_cvtps_epi32(box),
                                       _mm_cvtps_epi32(box));
    _mm_storel_epi64((__m128i*)dst, v);
}

TOPOS_SIMD_TARGET
void tc_cvt_rows_u8_avx2(const tc_cvt_simd_ctx* c, uint32_t y0, uint32_t y1,
                         tc_cvt_tail_fn tail, void* user)
{
    const uint32_t w = c->width;
    const uint32_t vec_end = w & ~(uint32_t)7u;
    const uint32_t nch = c->nch;
    const __m256 nd = _mm256_set1_ps(c->norm_div);
    const __m256 krv = _mm256_set1_ps(c->kr);
    const __m256 kfv = _mm256_set1_ps(c->kf);
    const __m256 kbv = _mm256_set1_ps(c->kb);
    const __m256 duv = _mm256_set1_ps(c->du);
    const __m256 dvv = _mm256_set1_ps(c->dv);
    const __m256 ymv = _mm256_set1_ps(c->y_mult);
    const __m256 yov = _mm256_set1_ps(c->y_off);
    const __m256 cmv = _mm256_set1_ps(c->c_mult);
    const __m256 cov = _mm256_set1_ps(c->c_off);
    const __m128 half4 = _mm_set1_ps(0.5f);
    const __m256 half = _mm256_set1_ps(0.5f);
    const __m256 scale = _mm256_set1_ps(c->scale);
    const __m256 zero = _mm256_setzero_ps();
    const __m256 a_nmax = _mm256_set1_ps(c->a_nmax);
    const int is422 = (c->mode == TOPOS_CVT_OUT_YUV422);
    const int gbr = (c->mode == TOPOS_CVT_OUT_GBR);

    for (uint32_t y = y0; y < y1; ++y) {
        const uint8_t* row = c->src + (size_t)y * c->src_stride;
        uint16_t* yout = c->y_out + (size_t)y * c->y_st;
        uint16_t* uout = c->u_out + (size_t)y * c->u_st;
        uint16_t* vout = c->v_out + (size_t)y * c->v_st;
        uint16_t* aout = c->a_out != NULL
            ? c->a_out + (size_t)y * c->a_st : NULL;

        for (uint32_t x = 0u; x < vec_end; x += 8u) {
            __m128i v0, v1;
            if (nch == 4u) {
                v0 = _mm_loadu_si128((const __m128i*)(row + x * 4u));
                v1 = _mm_loadu_si128((const __m128i*)(row + x * 4u + 16u));
            } else {
                /* 24 字节 → 32 字节栈缓冲（pad 恒不被 shuffle 选中；
                 * 严格行内读取，无越界） */
                uint8_t buf[32];
                memcpy(buf, row + x * 3u, 24u);
                v0 = _mm_loadu_si128((const __m128i*)buf);
                v1 = _mm_loadu_si128((const __m128i*)(buf + 16));
            }

            __m128i B8 = nch == 4u ? extract4(v0, v1, c->bo)
                                   : extract3(v0, v1, c->bo);
            __m128i G8 = nch == 4u ? extract4(v0, v1, c->go)
                                   : extract3(v0, v1, c->go);
            __m128i R8 = nch == 4u ? extract4(v0, v1, c->ro)
                                   : extract3(v0, v1, c->ro);

            const __m256 bf = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(B8));
            const __m256 gf = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(G8));
            const __m256 rf = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(R8));

            /* 归一化除法（IEEE 正确舍入 = 标量除法逐位一致，禁乘倒数） */
            const __m256 rn = _mm256_div_ps(rf, nd);
            const __m256 gn = _mm256_div_ps(gf, nd);
            const __m256 bn = _mm256_div_ps(bf, nd);

            __m256 yq, uq, vq; /* 量化目标（f32，钳位在 store 内完成） */
            if (gbr) {
                yq = _mm256_mul_ps(gn, scale);
                uq = _mm256_mul_ps(bn, scale);
                vq = _mm256_mul_ps(rn, scale);
            } else {
                /* y = kr*r + kf*g; y = y + kb*b（两步加法同序） */
                __m256 yv = _mm256_add_ps(_mm256_mul_ps(krv, rn),
                                          _mm256_mul_ps(kfv, gn));
                yv = _mm256_add_ps(yv, _mm256_mul_ps(kbv, bn));
                const __m256 uv = _mm256_div_ps(_mm256_sub_ps(bn, yv), duv);
                const __m256 vv = _mm256_div_ps(_mm256_sub_ps(rn, yv), dvv);
                yq = _mm256_add_ps(_mm256_mul_ps(yv, ymv), yov);
                uq = _mm256_add_ps(_mm256_mul_ps(_mm256_add_ps(uv, half), cmv),
                                   cov);
                vq = _mm256_add_ps(_mm256_mul_ps(_mm256_add_ps(vv, half), cmv),
                                   cov);
            }

            store8_u16(yout + x, yq, zero, scale);
            if (!is422) {
                store8_u16(uout + x, uq, zero, scale);
                store8_u16(vout + x, vq, zero, scale);
            } else {
                /* 422：对已钳位整型做 box（偶/奇提取 → rint((a+b)*0.5)） */
                const __m256 uqc = cvt_clampf(uq, zero, scale);
                const __m256 vqc = cvt_clampf(vq, zero, scale);
                store4_box(uout + (x >> 1), uqc, half4);
                store4_box(vout + (x >> 1), vqc, half4);
            }

            if (aout != NULL) {
                if (c->opaque || nch == 3u) {
                    _mm_storeu_si128((__m128i*)(aout + x),
                                     _mm_set1_epi16((short)(
                                         (uint16_t)(0xFFFFu << c->alpha_shift))));
                } else {
                    const __m128i A8 = extract4(v0, v1, 3u);
                    const __m256 an = _mm256_div_ps(_mm256_cvtepi32_ps(
                        _mm256_cvtepu8_epi32(A8)), nd);
                    const __m256 aq = cvt_clampf(
                        _mm256_mul_ps(an, a_nmax), zero, a_nmax);
                    const __m256i ai = _mm256_slli_epi32(
                        _mm256_cvtps_epi32(aq), (int)c->alpha_shift);
                    const __m256i p = _mm256_permute4x64_epi64(
                        _mm256_packus_epi32(ai, ai), 0x88);
                    _mm_storeu_si128((__m128i*)(aout + x),
                                     _mm256_castsi256_si128(p));
                }
            }
        }
        if (vec_end < w) { tail(user, y, vec_end); }
    }
}

#else /* 非 x86_64 编译切片：空 TU（符号引用由 color_convert.c 守卫免除） */
typedef int tc_cvt_avx2_tu_not_empty; /* ISO C 禁空 TU */
#endif
