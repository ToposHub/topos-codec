#include "transform/base_scale.h"

#include <string.h>

#include "common/alloc.h"
#include "common/checked.h"

static bool scale_dims_valid(uint32_t w, uint32_t h, size_t stride)
{
    return w >= 1u && h >= 1u && w <= TC_PLANE_MAX_DIM && h <= TC_PLANE_MAX_DIM &&
           stride >= (size_t)w;
}

static uint32_t sample_max(uint8_t bit_depth)
{
    if (bit_depth == 0u || bit_depth > 16u) { return 0u; }
    return bit_depth == 16u ? 65535u : (1u << bit_depth) - 1u;
}

bool tc_base_scale_dimensions(uint32_t source_w, uint32_t source_h,
                              uint32_t max_dim,
                              uint32_t* target_w, uint32_t* target_h)
{
    if (target_w == NULL || target_h == NULL || source_w == 0u || source_h == 0u ||
        source_w > TC_PLANE_MAX_DIM || source_h > TC_PLANE_MAX_DIM ||
        max_dim == 0u || max_dim > TC_PLANE_MAX_DIM) {
        return false;
    }
    if (source_w <= max_dim && source_h <= max_dim) {
        *target_w = source_w;
        *target_h = source_h;
        return true;
    }
    if (source_w >= source_h) {
        *target_w = max_dim;
        *target_h = (uint32_t)(((uint64_t)source_h * max_dim) / source_w);
    } else {
        *target_h = max_dim;
        *target_w = (uint32_t)(((uint64_t)source_w * max_dim) / source_h);
    }
    if (*target_w == 0u) { *target_w = 1u; }
    if (*target_h == 0u) { *target_h = 1u; }
    return true;
}

bool tc_base_plane_dimensions(uint32_t luma_w, uint32_t luma_h,
                              uint8_t pixel_format, uint32_t plane,
                              uint8_t chroma_siting,
                              uint32_t* plane_w, uint32_t* plane_h)
{
    if (plane_w == NULL || plane_h == NULL || luma_w == 0u || luma_h == 0u ||
        luma_w > TC_PLANE_MAX_DIM || luma_h > TC_PLANE_MAX_DIM ||
        pixel_format > 3u || plane >= 4u || chroma_siting > 2u) {
        return false;
    }
    if (pixel_format == 2u && chroma_siting != TC_BASE_CHROMA_LEFT) {
        return false;
    }
    if (pixel_format == 3u) {
        /* TRAW（批 3）：CFA 全平面 (W/2)×(H/2)——无 luma/chroma 语义，
         * "luma" 参数即帧几何（与 frame_header derive_geometry 同规则） */
        *plane_w = (luma_w + 1u) / 2u;
        *plane_h = (luma_h + 1u) / 2u;
        return true;
    }
    const bool chroma422 = pixel_format == 0u && (plane == 1u || plane == 2u);
    *plane_w = chroma422 ? (luma_w + 1u) / 2u : luma_w;
    *plane_h = luma_h;
    return true;
}

static uint32_t overlap(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1)
{
    const uint32_t lo = a0 > b0 ? a0 : b0;
    const uint32_t hi = a1 < b1 ? a1 : b1;
    return hi > lo ? hi - lo : 0u;
}

static uint16_t clamp_sample(uint64_t value, uint32_t max_value)
{
    if (value > max_value) { value = max_value; }
    return (uint16_t)value;
}

/* The generic area loop below is the scalar reference for arbitrary geometry.
 * Base encoding, however, repeatedly asks for <=2K previews of large 16:9
 * frames. Accumulate the exact same rational cell weights in a target-sized
 * buffer while streaming source rows. This removes the nested y/x overlap
 * walk from the hot path without creating a source-sized intermediate plane.
 *
 * Return 1 when the fast path ran, 0 when target scratch is intentionally too
 * large or unavailable (the caller then uses the reference loop). */
static int downsample_area_streamed_u16(const uint16_t* src, uint32_t src_w,
                                        uint32_t src_h, size_t src_stride,
                                        uint16_t* dst, uint32_t dst_w,
                                        uint32_t dst_h, size_t dst_stride,
                                        uint32_t max_value)
{
    size_t target_count = 0u;
    size_t target_bytes = 0u;
    size_t axis_bytes = 0u;
    if (!tc_umul_size((size_t)dst_w, (size_t)dst_h, &target_count) ||
        !tc_umul_size(target_count, sizeof(uint64_t), &target_bytes) ||
        !tc_umul_size((size_t)dst_w, sizeof(uint32_t), &axis_bytes) ||
        target_count > (size_t)(8u * 1024u * 1024u)) {
        return 0;
    }
    uint64_t* accum = (uint64_t*)tc_alloc(target_bytes);
    uint32_t* x_begin = (uint32_t*)tc_alloc(axis_bytes);
    uint32_t* x_end = (uint32_t*)tc_alloc(axis_bytes);
    if (accum == NULL || x_begin == NULL || x_end == NULL) {
        tc_free(accum);
        tc_free(x_begin);
        tc_free(x_end);
        return 0;
    }
    memset(accum, 0, target_bytes);
    for (uint32_t dx = 0u; dx < dst_w; ++dx) {
        const uint32_t x0_num = dx * src_w;
        const uint32_t x1_num = (dx + 1u) * src_w;
        x_begin[dx] = x0_num / dst_w;
        x_end[dx] = (x1_num + dst_w - 1u) / dst_w;
    }

    const uint64_t denominator = (uint64_t)src_w * (uint64_t)src_h;
    for (uint32_t sy = 0u; sy < src_h; ++sy) {
        const uint32_t dy0 = (uint32_t)(((uint64_t)sy * dst_h) / src_h);
        uint32_t dy1 = (uint32_t)((((uint64_t)sy + 1u) * dst_h + src_h - 1u) /
                                  src_h);
        if (dy1 > dst_h) { dy1 = dst_h; }
        const uint16_t* srow = src + (size_t)sy * src_stride;
        for (uint32_t dx = 0u; dx < dst_w; ++dx) {
            const uint32_t x0_num = dx * src_w;
            const uint32_t x1_num = (dx + 1u) * src_w;
            uint64_t x_sum = 0u;
            for (uint32_t sx = x_begin[dx]; sx < x_end[dx]; ++sx) {
                const uint32_t wx = overlap(x0_num, x1_num,
                                             sx * dst_w, (sx + 1u) * dst_w);
                uint32_t value = srow[sx];
                if (value > max_value) { value = max_value; }
                x_sum += (uint64_t)value * (uint64_t)wx;
            }
            for (uint32_t dy = dy0; dy < dy1; ++dy) {
                const uint32_t y0_num = dy * src_h;
                const uint32_t y1_num = (dy + 1u) * src_h;
                const uint32_t wy = overlap(y0_num, y1_num,
                                             sy * dst_h, (sy + 1u) * dst_h);
                accum[(size_t)dy * dst_w + dx] += x_sum * (uint64_t)wy;
            }
        }
    }
    for (uint32_t dy = 0u; dy < dst_h; ++dy) {
        uint16_t* drow = dst + (size_t)dy * dst_stride;
        for (uint32_t dx = 0u; dx < dst_w; ++dx) {
            drow[dx] = clamp_sample((accum[(size_t)dy * dst_w + dx] +
                                     denominator / 2u) / denominator,
                                    max_value);
        }
    }
    tc_free(accum);
    tc_free(x_begin);
    tc_free(x_end);
    return 1;
}

bool tc_base_downsample_u16(const uint16_t* src, uint32_t src_w, uint32_t src_h,
                            size_t src_stride, uint16_t* dst,
                            uint32_t dst_w, uint32_t dst_h, size_t dst_stride,
                            uint8_t bit_depth)
{
    const uint32_t max_value = sample_max(bit_depth);
    if (src == NULL || dst == NULL || max_value == 0u ||
        !scale_dims_valid(src_w, src_h, src_stride) ||
        !scale_dims_valid(dst_w, dst_h, dst_stride) ||
        dst_w > src_w || dst_h > src_h) {
        return false;
    }
    size_t src_bytes = 0u, dst_bytes = 0u;
    if (!tc_umul_size(src_stride, (size_t)src_h, &src_bytes) ||
        !tc_umul_size(dst_stride, (size_t)dst_h, &dst_bytes)) {
        return false;
    }
    (void)src_bytes;
    (void)dst_bytes;

    if (downsample_area_streamed_u16(src, src_w, src_h, src_stride, dst,
                                     dst_w, dst_h, dst_stride, max_value) != 0) {
        return true;
    }

    /* overlap weights are expressed in the source-cell denominator dst_w/dst_h;
     * the complete destination cell area is src_w*src_h in that coordinate
     * system. */
    const uint64_t denominator = (uint64_t)src_w * (uint64_t)src_h;
    for (uint32_t dy = 0u; dy < dst_h; ++dy) {
        const uint32_t y0_num = dy * src_h;
        const uint32_t y1_num = (dy + 1u) * src_h;
        const uint32_t sy0 = y0_num / dst_h;
        const uint32_t sy1 = (y1_num + dst_h - 1u) / dst_h;
        uint16_t* drow = dst + (size_t)dy * dst_stride;
        for (uint32_t dx = 0u; dx < dst_w; ++dx) {
            const uint32_t x0_num = dx * src_w;
            const uint32_t x1_num = (dx + 1u) * src_w;
            const uint32_t sx0 = x0_num / dst_w;
            const uint32_t sx1 = (x1_num + dst_w - 1u) / dst_w;
            uint64_t sum = 0u;
            for (uint32_t sy = sy0; sy < sy1; ++sy) {
                const uint32_t wy = overlap(y0_num, y1_num,
                                             sy * dst_h, (sy + 1u) * dst_h);
                const uint16_t* srow = src + (size_t)sy * src_stride;
                for (uint32_t sx = sx0; sx < sx1; ++sx) {
                    const uint32_t wx = overlap(x0_num, x1_num,
                                                 sx * dst_w, (sx + 1u) * dst_w);
                    uint32_t value = srow[sx];
                    if (value > max_value) { value = max_value; }
                    sum += (uint64_t)value * (uint64_t)wx * (uint64_t)wy;
                }
            }
            drow[dx] = clamp_sample((sum + denominator / 2u) / denominator,
                                    max_value);
        }
    }
    return true;
}

static void bilinear_axis(uint32_t d, uint32_t dst, uint32_t src,
                          uint32_t* i0, uint32_t* i1, uint32_t* frac,
                          uint32_t* denominator)
{
    const uint64_t den = (uint64_t)dst * 2ull;
    const int64_t num = (int64_t)(2ull * d + 1ull) * (int64_t)src - (int64_t)dst;
    *denominator = (uint32_t)den;
    if (num <= 0) {
        *i0 = 0u; *i1 = 0u; *frac = 0u;
        return;
    }
    const uint64_t last = (uint64_t)(src - 1u) * den;
    if ((uint64_t)num >= last) {
        *i0 = src - 1u; *i1 = src - 1u; *frac = 0u;
        return;
    }
    *i0 = (uint32_t)((uint64_t)num / den);
    *i1 = *i0 + 1u;
    *frac = (uint32_t)((uint64_t)num % den);
}

uint16_t tc_base_upsample_pixel_unchecked_u16(const uint16_t* src,
                                              uint32_t src_w, uint32_t src_h,
                                              size_t src_stride, uint32_t dst_w,
                                              uint32_t dst_h, uint32_t dst_x,
                                              uint32_t dst_y, uint32_t max_value)
{
    uint32_t y0 = 0u, y1 = 0u, fy = 0u, yden = 0u;
    bilinear_axis(dst_y, dst_h, src_h, &y0, &y1, &fy, &yden);
    uint32_t x0 = 0u, x1 = 0u, fx = 0u, xden = 0u;
    bilinear_axis(dst_x, dst_w, src_w, &x0, &x1, &fx, &xden);
    const uint64_t wx0 = (uint64_t)xden - fx;
    const uint64_t wx1 = fx;
    const uint64_t wy0 = (uint64_t)yden - fy;
    const uint64_t wy1 = fy;
    const uint16_t* row0 = src + (size_t)y0 * src_stride;
    const uint16_t* row1 = src + (size_t)y1 * src_stride;
    const uint64_t sum = (uint64_t)row0[x0] * wx0 * wy0 +
                         (uint64_t)row0[x1] * wx1 * wy0 +
                         (uint64_t)row1[x0] * wx0 * wy1 +
                         (uint64_t)row1[x1] * wx1 * wy1;
    const uint64_t denominator = (uint64_t)xden * (uint64_t)yden;
    return clamp_sample((sum + denominator / 2u) / denominator, max_value);
}

bool tc_base_upsample_pixel_u16(const uint16_t* src, uint32_t src_w, uint32_t src_h,
                                size_t src_stride, uint32_t dst_w, uint32_t dst_h,
                                uint32_t dst_x, uint32_t dst_y,
                                uint8_t bit_depth, uint16_t* out)
{
    const uint32_t max_value = sample_max(bit_depth);
    if (out == NULL || src == NULL || max_value == 0u ||
        !scale_dims_valid(src_w, src_h, src_stride) ||
        !scale_dims_valid(dst_w, dst_h, (size_t)dst_w) ||
        dst_w < src_w || dst_h < src_h || dst_x >= dst_w || dst_y >= dst_h) {
        return false;
    }
    size_t src_bytes = 0u;
    if (!tc_umul_size(src_stride, (size_t)src_h, &src_bytes)) { return false; }
    (void)src_bytes;
    *out = tc_base_upsample_pixel_unchecked_u16(src, src_w, src_h, src_stride,
                                                dst_w, dst_h, dst_x, dst_y,
                                                max_value);
    return true;
}

bool tc_base_upsample_u16(const uint16_t* src, uint32_t src_w, uint32_t src_h,
                          size_t src_stride, uint16_t* dst,
                          uint32_t dst_w, uint32_t dst_h, size_t dst_stride,
                          uint8_t bit_depth)
{
    const uint32_t max_value = sample_max(bit_depth);
    if (src == NULL || dst == NULL || max_value == 0u ||
        !scale_dims_valid(src_w, src_h, src_stride) ||
        !scale_dims_valid(dst_w, dst_h, dst_stride) ||
        dst_w < src_w || dst_h < src_h) {
        return false;
    }
    size_t src_bytes = 0u, dst_bytes = 0u;
    if (!tc_umul_size(src_stride, (size_t)src_h, &src_bytes) ||
        !tc_umul_size(dst_stride, (size_t)dst_h, &dst_bytes)) {
        return false;
    }
    (void)src_bytes;
    (void)dst_bytes;

    for (uint32_t dy = 0u; dy < dst_h; ++dy) {
        uint16_t* drow = dst + (size_t)dy * dst_stride;
        for (uint32_t dx = 0u; dx < dst_w; ++dx) {
            drow[dx] = tc_base_upsample_pixel_unchecked_u16(src, src_w, src_h,
                                                            src_stride, dst_w,
                                                            dst_h, dx, dy,
                                                            max_value);
        }
    }
    return true;
}
