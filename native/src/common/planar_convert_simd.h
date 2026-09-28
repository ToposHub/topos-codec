/* AVX2 fast path for the integer 4:4:4 -> 4:2:2 helper. */
#ifndef TOPOS_INTERNAL_PLANAR_CONVERT_SIMD_H
#define TOPOS_INTERNAL_PLANAR_CONVERT_SIMD_H

#include <stddef.h>
#include <stdint.h>

typedef struct tc_planar_convert_simd_ctx {
    const uint16_t* u_in;
    const uint16_t* v_in;
    uint16_t* u_out;
    uint16_t* v_out;
    size_t u_in_stride;
    size_t v_in_stride;
    size_t u_out_stride;
    size_t v_out_stride;
    uint32_t width;
    uint32_t chroma_width;
} tc_planar_convert_simd_ctx;

#if defined(__x86_64__) || defined(_M_X64)
void tc_planar_convert_rows_avx2(const tc_planar_convert_simd_ctx* ctx,
                                  uint32_t y0, uint32_t y1);
#endif

#endif /* TOPOS_INTERNAL_PLANAR_CONVERT_SIMD_H */
