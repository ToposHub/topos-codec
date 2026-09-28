/* AVX2 integer 4:4:4 -> 4:2:2 conversion.
 *
 * The operation is exactly (a + b + 1) >> 1 for each horizontal pair.  The
 * unsigned 16-bit average intrinsic has the same rounding semantics and
 * avoids widening the 4K chroma rows to uint32.  The scalar tail handles odd
 * widths and the final incomplete vector. */
#if defined(__x86_64__) || defined(_M_X64)

#if defined(_MSC_VER)
#define TOPOS_SIMD_TARGET
#else
#define TOPOS_SIMD_TARGET __attribute__((target("avx2")))
#endif

#include <immintrin.h>

#include "../common/planar_convert_simd.h"

static TOPOS_SIMD_TARGET void planar_convert_4_pairs(
    const uint16_t* src, uint16_t* dst)
{
    /* Four uint16 pairs per 128-bit lane; the high mask bytes are zeroed. */
    const __m128i even_mask = _mm_setr_epi8(
        0, 1, 4, 5, 8, 9, 12, 13, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i odd_mask = _mm_setr_epi8(
        2, 3, 6, 7, 10, 11, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m256i samples = _mm256_loadu_si256((const __m256i*)src);
    const __m128i lo = _mm256_castsi256_si128(samples);
    const __m128i hi = _mm256_extracti128_si256(samples, 1);
    const __m128i lo_avg = _mm_avg_epu16(
        _mm_shuffle_epi8(lo, even_mask), _mm_shuffle_epi8(lo, odd_mask));
    const __m128i hi_avg = _mm_avg_epu16(
        _mm_shuffle_epi8(hi, even_mask), _mm_shuffle_epi8(hi, odd_mask));
    _mm_storel_epi64((__m128i*)dst, lo_avg);
    _mm_storel_epi64((__m128i*)(dst + 4), hi_avg);
}

TOPOS_SIMD_TARGET
void tc_planar_convert_rows_avx2(const tc_planar_convert_simd_ctx* ctx,
                                  uint32_t y0, uint32_t y1)
{
    const uint32_t full_pairs = ctx->width / 2u;
    for (uint32_t y = y0; y < y1; ++y) {
        const uint16_t* u_src = ctx->u_in + (size_t)y * ctx->u_in_stride;
        const uint16_t* v_src = ctx->v_in + (size_t)y * ctx->v_in_stride;
        uint16_t* u_dst = ctx->u_out + (size_t)y * ctx->u_out_stride;
        uint16_t* v_dst = ctx->v_out + (size_t)y * ctx->v_out_stride;
        uint32_t x = 0u;
        for (; x + 8u <= full_pairs; x += 8u) {
            planar_convert_4_pairs(u_src + x * 2u, u_dst + x);
            planar_convert_4_pairs(v_src + x * 2u, v_dst + x);
        }
        for (; x < ctx->chroma_width; ++x) {
            const uint32_t sx = x * 2u;
            const uint32_t sx1 = sx + 1u < ctx->width ? sx + 1u : sx;
            u_dst[x] = (uint16_t)(((uint32_t)u_src[sx]
                                   + (uint32_t)u_src[sx1] + 1u) >> 1u);
            v_dst[x] = (uint16_t)(((uint32_t)v_src[sx]
                                   + (uint32_t)v_src[sx1] + 1u) >> 1u);
        }
    }
}

#else
typedef int tc_planar_convert_avx2_tu_placeholder;
#endif
