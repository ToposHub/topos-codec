#include "base_residual.h"

#include <string.h>

#include "../common/alloc.h"
#include "../common/checked.h"
#include "../common/error.h"
#include "../entropy/block_coding.h"
#include "../entropy/rice.h"
#include "../transform/base_scale.h"

static int32_t residual_plane_validate(uint32_t width, uint32_t height,
                                       size_t stride)
{
    if (width == 0u || height == 0u || width > TC_PLANE_MAX_DIM ||
        height > TC_PLANE_MAX_DIM || stride < (size_t)width) {
        return TC_ERR_INVALID_ARGUMENT;
    }
    size_t bytes = 0u;
    if (!tc_umul_size(stride, (size_t)height, &bytes)) {
        return TC_ERR_LIMIT_EXCEEDED;
    }
    (void)bytes;
    return TC_OK;
}

static uint32_t residual_max(uint8_t bit_depth)
{
    if (bit_depth == 0u || bit_depth > 16u) { return 0u; }
    return bit_depth == 16u ? 65535u : (1u << bit_depth) - 1u;
}

/* The residual path visits every source pixel.  Calling the generic
 * half-pixel predictor for each pixel repeats four integer divisions per
 * sample.  Cache the exact axis results once per plane; this keeps the
 * predictor bit-exact while making the hot loop multiply/load dominated. */
typedef struct residual_axis_map {
    uint32_t* storage;
    uint32_t* i0;
    uint32_t* i1;
    uint32_t* frac;
    uint32_t denominator;
} residual_axis_map;

static void residual_axis_map_release(residual_axis_map* map)
{
    if (map == NULL) { return; }
    tc_free(map->storage);
    memset(map, 0, sizeof(*map));
}

static uint32_t residual_gcd(uint32_t a, uint32_t b)
{
    while (b != 0u) {
        const uint32_t next = a % b;
        a = b;
        b = next;
    }
    return a;
}

static int residual_axis_map_init(uint32_t dst, uint32_t src,
                                  residual_axis_map* map)
{
    if (map == NULL || dst == 0u || src == 0u) { return 0; }
    memset(map, 0, sizeof(*map));
    size_t one_bytes = 0u;
    size_t total_bytes = 0u;
    if (!tc_umul_size((size_t)dst, sizeof(uint32_t), &one_bytes) ||
        !tc_umul_size(one_bytes, 3u, &total_bytes)) {
        return 0;
    }
    map->storage = (uint32_t*)tc_alloc(total_bytes);
    if (map->storage == NULL) { return 0; }
    map->i0 = map->storage;
    map->i1 = map->i0 + dst;
    map->frac = map->i1 + dst;
    /* All four bilinear weights and the denominator carry gcd(dst, src) as
     * a common factor.  Cancel it in the axis table once: 4K→2K becomes a
     * 4-wide denominator (instead of 7680), 8K→2K becomes 8, and the 6K
     * 6144→1920 path becomes 10.  The final rounded quotient is identical,
     * but the predictor no longer needs wide products in its hot loop. */
    const uint32_t common = residual_gcd(dst, src);
    const uint32_t reduced_dst = dst / common;
    map->denominator = reduced_dst * 2u;
    const uint64_t full_den = (uint64_t)dst * 2u;
    const uint64_t last = (uint64_t)(src - 1u) * full_den;
    for (uint32_t d = 0u; d < dst; ++d) {
        const int64_t num = (int64_t)(2ull * d + 1ull) * (int64_t)src -
                            (int64_t)dst;
        if (num <= 0) {
            map->i0[d] = 0u;
            map->i1[d] = 0u;
            map->frac[d] = 0u;
        } else if ((uint64_t)num >= last) {
            map->i0[d] = src - 1u;
            map->i1[d] = src - 1u;
            map->frac[d] = 0u;
        } else {
            map->i0[d] = (uint32_t)((uint64_t)num / full_den);
            map->i1[d] = map->i0[d] + 1u;
            map->frac[d] = (uint32_t)(((uint64_t)num % full_den) / common);
        }
    }
    return 1;
}

static uint64_t residual_divide_common(uint64_t value, uint64_t denominator)
{
    /* The canonical preview geometries hit these exact squared denominators:
     * source==base (4), 4K→2K (16), 6K→2K (100), 8K→2K (64), and
     * 12K→2K (144).  Keeping each denominator as an immediate lets the
     * compiler emit a shift or reciprocal multiply instead of a divq inside
     * the per-pixel predictor. */
    switch (denominator) {
    case 4u: return value >> 2u;
    case 16u: return value >> 4u;
    case 64u: return value >> 6u;
    case 100u: return value / 100u;
    case 144u: return value / 144u;
    default: return value / denominator;
    }
}

static uint16_t residual_predict_mapped(const uint16_t* src,
                                        size_t src_stride,
                                        const residual_axis_map* x_map,
                                        const residual_axis_map* y_map,
                                        uint32_t x, uint32_t y,
                                        uint32_t max_value,
                                        uint64_t denominator,
                                        uint64_t reciprocal)
{
    const uint32_t x0 = x_map->i0[x];
    const uint32_t x1 = x_map->i1[x];
    const uint32_t y0 = y_map->i0[y];
    const uint32_t y1 = y_map->i1[y];
    const uint64_t wx1 = x_map->frac[x];
    const uint64_t wy1 = y_map->frac[y];
    const uint64_t wx0 = (uint64_t)x_map->denominator - wx1;
    const uint64_t wy0 = (uint64_t)y_map->denominator - wy1;
    const uint16_t* row0 = src + (size_t)y0 * src_stride;
    const uint16_t* row1 = src + (size_t)y1 * src_stride;
    const uint64_t sum = (uint64_t)row0[x0] * wx0 * wy0 +
                         (uint64_t)row0[x1] * wx1 * wy0 +
                         (uint64_t)row1[x0] * wx0 * wy1 +
                         (uint64_t)row1[x1] * wx1 * wy1;
    const uint64_t rounded = sum + denominator / 2u;
    if (denominator <= 4096u) {
        const uint64_t value = residual_divide_common(rounded, denominator);
        return (uint16_t)(value > max_value ? max_value : value);
    }
#if defined(__SIZEOF_INT128__)
    /* reciprocal=ceil(2^64/denominator), with one correction handling the
     * exact-multiple and near-boundary cases. The predictor numerator is well
     * below 2^64 for all legal Topos geometries and sample depths. */
    uint64_t value = (uint64_t)(((__uint128_t)rounded * reciprocal) >> 64u);
    while (value > 0u && (__uint128_t)value * denominator > rounded) { --value; }
    while ((__uint128_t)(value + 1u) * denominator <= rounded) { ++value; }
#else
    const uint64_t value = rounded / denominator;
#endif
    return (uint16_t)(value > max_value ? max_value : value);
}

typedef struct residual_horizontal_cache {
    uint64_t* storage;
    uint64_t* row0;
    uint64_t* row1;
    uint32_t width;
    uint32_t cached_y0;
    uint32_t cached_y1;
    uint8_t valid;
} residual_horizontal_cache;

static int residual_horizontal_cache_init(uint32_t width,
                                          residual_horizontal_cache* cache)
{
    if (cache == NULL || width == 0u) { return 0; }
    memset(cache, 0, sizeof(*cache));
    size_t one_bytes = 0u;
    size_t total_bytes = 0u;
    if (!tc_umul_size((size_t)width, sizeof(uint64_t), &one_bytes) ||
        !tc_umul_size(one_bytes, 2u, &total_bytes)) {
        return 0;
    }
    cache->storage = (uint64_t*)tc_alloc(total_bytes);
    if (cache->storage == NULL) { return 0; }
    cache->row0 = cache->storage;
    cache->row1 = cache->row0 + width;
    cache->width = width;
    return 1;
}

static void residual_horizontal_cache_release(residual_horizontal_cache* cache)
{
    if (cache == NULL) { return; }
    tc_free(cache->storage);
    memset(cache, 0, sizeof(*cache));
}

static void residual_horizontal_cache_fill(
    const uint16_t* base, size_t base_stride,
    const residual_axis_map* x_map, uint32_t y0, uint32_t y1,
    residual_horizontal_cache* cache)
{
    const uint16_t* source_row0 = base + (size_t)y0 * base_stride;
    const uint16_t* source_row1 = base + (size_t)y1 * base_stride;
    const uint64_t denominator = x_map->denominator;
    (void)denominator;
    for (uint32_t x = 0u; x < cache->width; ++x) {
        const uint32_t x0 = x_map->i0[x];
        const uint32_t x1 = x_map->i1[x];
        const uint64_t wx1 = x_map->frac[x];
        const uint64_t wx0 = (uint64_t)x_map->denominator - wx1;
        cache->row0[x] = (uint64_t)source_row0[x0] * wx0 +
                         (uint64_t)source_row0[x1] * wx1;
        cache->row1[x] = (uint64_t)source_row1[x0] * wx0 +
                         (uint64_t)source_row1[x1] * wx1;
    }
    cache->cached_y0 = y0;
    cache->cached_y1 = y1;
    cache->valid = 1u;
}

static uint16_t residual_predict_cached(
    const residual_horizontal_cache* cache, const residual_axis_map* y_map,
    uint32_t y, uint32_t x, uint32_t max_value,
    uint64_t denominator, uint64_t reciprocal)
{
    const uint64_t wy1 = y_map->frac[y];
    const uint64_t wy0 = (uint64_t)y_map->denominator - wy1;
    const uint64_t sum = cache->row0[x] * wy0 + cache->row1[x] * wy1;
    const uint64_t rounded = sum + denominator / 2u;
    if (denominator <= 4096u) {
        const uint64_t value = residual_divide_common(rounded, denominator);
        return (uint16_t)(value > max_value ? max_value : value);
    }
#if defined(__SIZEOF_INT128__)
    uint64_t value = (uint64_t)(((__uint128_t)rounded * reciprocal) >> 64u);
    while (value > 0u && (__uint128_t)value * denominator > rounded) { --value; }
    while ((__uint128_t)(value + 1u) * denominator <= rounded) { ++value; }
#else
    const uint64_t value = rounded / denominator;
#endif
    return (uint16_t)(value > max_value ? max_value : value);
}

static uint32_t residual_rice_k(uint64_t sum, uint64_t count)
{
    if (count == 0u || sum == 0u) { return 0u; }
    uint64_t mean = sum / count;
    uint32_t k = 0u;
    while (k < (uint32_t)TC_RICE_K_MAX && (mean >> (k + 1u)) != 0u) { ++k; }
    return k;
}

typedef struct residual_rice_accumulator {
    uint64_t level_sum;
    uint64_t level_count;
    uint64_t run_sum;
    uint64_t run_count;
    uint64_t run;
} residual_rice_accumulator;

static int32_t residual_rice_accumulate(residual_rice_accumulator* acc,
                                        int32_t value)
{
    if (value == 0) {
        ++acc->run;
        return TC_OK;
    }
    const uint32_t mapped = tc_rice_map_signed(value);
    if (value > 65535 || value < -65535 ||
        mapped > TC_RICE_M_MAX_ALPHA_LEVEL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base residual level domain");
        return TC_ERR_INVALID_ARGUMENT;
    }
    acc->run_sum += acc->run;
    acc->run_count++;
    acc->level_sum += mapped;
    acc->level_count++;
    acc->run = 0u;
    return TC_OK;
}

static void residual_rice_finish(residual_rice_accumulator* acc,
                                 uint32_t* k_level, uint32_t* k_run)
{
    if (acc->run != 0u) {
        acc->run_sum += acc->run;
        acc->run_count++;
    }
    *k_level = residual_rice_k(acc->level_sum, acc->level_count);
    *k_run = residual_rice_k(acc->run_sum, acc->run_count);
}

static int32_t residual_arguments(const uint16_t* base, uint32_t base_w,
                                  uint32_t base_h, size_t base_stride,
                                  uint32_t source_w, uint32_t source_h,
                                  const int32_t* residual, size_t residual_stride,
                                  uint8_t bit_depth)
{
    if (base == NULL || residual == NULL || residual_max(bit_depth) == 0u ||
        residual_plane_validate(base_w, base_h, base_stride) != TC_OK ||
        residual_plane_validate(source_w, source_h, residual_stride) != TC_OK ||
        base_w > source_w || base_h > source_h) {
        return TC_ERR_INVALID_ARGUMENT;
    }
    return TC_OK;
}

static int32_t residual_quantize_signed(int32_t value, uint32_t quant_step)
{
    if (quant_step == 1u || value == 0) { return value; }
    const uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
    const uint32_t quantized = (magnitude + (quant_step >> 1u)) / quant_step;
    return value < 0 ? -(int32_t)quantized : (int32_t)quantized;
}

/* ADR-C030 minor-4：detail 系数平面量化（对称舍入，与
 * residual_quantize_signed 同一算术；反量化见 tc_base_residual_scale_in_place）。 */
int32_t tc_base_residual_quantize_in_place(int32_t* coefficients,
                                           uint32_t width, uint32_t height,
                                           size_t stride, uint32_t quant_step)
{
    if (coefficients == NULL ||
        residual_plane_validate(width, height, stride) != TC_OK ||
        quant_step == 0u || quant_step > 65535u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "residual quantize arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (quant_step == 1u) { return TC_OK; }
    for (uint32_t y = 0u; y < height; ++y) {
        int32_t* row = coefficients + (size_t)y * stride;
        for (uint32_t x = 0u; x < width; ++x) {
            row[x] = residual_quantize_signed(row[x], quant_step);
        }
    }
    return TC_OK;
}

int32_t tc_base_residual_build_quantized_u16_with_params(
                                   const uint16_t* source,
                                   uint32_t source_w, uint32_t source_h,
                                   size_t source_stride,
                                   const uint16_t* base,
                                   uint32_t base_w, uint32_t base_h,
                                   size_t base_stride,
                                   int32_t* residual, size_t residual_stride,
                                   uint8_t bit_depth, uint32_t quant_step,
                                   uint32_t* k_level, uint32_t* k_run)
{
    if (source == NULL || residual_arguments(base, base_w, base_h, base_stride,
                                              source_w, source_h, residual,
                                              residual_stride, bit_depth) != TC_OK ||
        source_stride < (size_t)source_w ||
        quant_step == 0u || quant_step > 65535u ||
        (k_level == NULL) != (k_run == NULL)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base residual build arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    size_t source_bytes = 0u;
    if (!tc_umul_size(source_stride, (size_t)source_h, &source_bytes)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "base residual source stride");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    (void)source_bytes;
    const uint32_t max_value = residual_max(bit_depth);
    residual_rice_accumulator acc;
    memset(&acc, 0, sizeof(acc));
    residual_axis_map x_map;
    residual_axis_map y_map;
    memset(&x_map, 0, sizeof(x_map));
    memset(&y_map, 0, sizeof(y_map));
    const int x_mapped = residual_axis_map_init(source_w, base_w, &x_map);
    const int y_mapped = residual_axis_map_init(source_h, base_h, &y_map);
    const int mapped = x_mapped != 0 && y_mapped != 0;
    if (mapped != 0) {
        const uint64_t denominator = (uint64_t)x_map.denominator *
                                     (uint64_t)y_map.denominator;
        const uint64_t reciprocal = UINT64_MAX / denominator + 1u;
        residual_horizontal_cache cache;
        const int cache_ready = residual_horizontal_cache_init(source_w, &cache);
        for (uint32_t y = 0u; y < source_h; ++y) {
            const uint16_t* source_row = source + (size_t)y * source_stride;
            int32_t* residual_row = residual + (size_t)y * residual_stride;
            if (cache_ready != 0 &&
                (cache.valid == 0u || cache.cached_y0 != y_map.i0[y] ||
                 cache.cached_y1 != y_map.i1[y])) {
                residual_horizontal_cache_fill(base, base_stride, &x_map,
                                               y_map.i0[y], y_map.i1[y], &cache);
            }
            for (uint32_t x = 0u; x < source_w; ++x) {
                const uint16_t prediction = cache_ready != 0
                    ? residual_predict_cached(&cache, &y_map, y, x, max_value,
                                              denominator, reciprocal)
                    : residual_predict_mapped(base, base_stride, &x_map, &y_map,
                                               x, y, max_value, denominator,
                                               reciprocal);
                uint32_t source_value = source_row[x];
                if (source_value > max_value) { source_value = max_value; }
                residual_row[x] = residual_quantize_signed(
                    (int32_t)source_value - (int32_t)prediction, quant_step);
                if (k_level != NULL && residual_rice_accumulate(&acc, residual_row[x]) != TC_OK) {
                    residual_axis_map_release(&x_map);
                    residual_axis_map_release(&y_map);
                    return TC_ERR_INVALID_ARGUMENT;
                }
            }
        }
        residual_horizontal_cache_release(&cache);
        residual_axis_map_release(&x_map);
        residual_axis_map_release(&y_map);
        if (k_level != NULL) { residual_rice_finish(&acc, k_level, k_run); }
        return TC_OK;
    }
    residual_axis_map_release(&x_map);
    residual_axis_map_release(&y_map);
    for (uint32_t y = 0u; y < source_h; ++y) {
        const uint16_t* source_row = source + (size_t)y * source_stride;
        int32_t* residual_row = residual + (size_t)y * residual_stride;
        for (uint32_t x = 0u; x < source_w; ++x) {
            const uint16_t prediction = tc_base_upsample_pixel_unchecked_u16(
                base, base_w, base_h, base_stride, source_w, source_h, x, y,
                max_value);
            uint32_t source_value = source_row[x];
            if (source_value > max_value) { source_value = max_value; }
            residual_row[x] = residual_quantize_signed(
                (int32_t)source_value - (int32_t)prediction, quant_step);
            if (k_level != NULL && residual_rice_accumulate(&acc, residual_row[x]) != TC_OK) {
                return TC_ERR_INVALID_ARGUMENT;
            }
        }
    }
    if (k_level != NULL) { residual_rice_finish(&acc, k_level, k_run); }
    return TC_OK;
}

int32_t tc_base_residual_build_u16_with_params(
                                   const uint16_t* source,
                                   uint32_t source_w, uint32_t source_h,
                                   size_t source_stride,
                                   const uint16_t* base,
                                   uint32_t base_w, uint32_t base_h,
                                   size_t base_stride,
                                   int32_t* residual, size_t residual_stride,
                                   uint8_t bit_depth,
                                   uint32_t* k_level, uint32_t* k_run)
{
    return tc_base_residual_build_quantized_u16_with_params(
        source, source_w, source_h, source_stride, base, base_w, base_h,
        base_stride, residual, residual_stride, bit_depth, 1u, k_level, k_run);
}

int32_t tc_base_residual_build_u16(const uint16_t* source,
                                   uint32_t source_w, uint32_t source_h,
                                   size_t source_stride,
                                   const uint16_t* base,
                                   uint32_t base_w, uint32_t base_h,
                                   size_t base_stride,
                                   int32_t* residual, size_t residual_stride,
                                   uint8_t bit_depth)
{
    return tc_base_residual_build_u16_with_params(
        source, source_w, source_h, source_stride, base, base_w, base_h,
        base_stride, residual, residual_stride, bit_depth, NULL, NULL);
}

int32_t tc_base_residual_scale_in_place(int32_t* residual,
                                        uint32_t width, uint32_t height,
                                        size_t residual_stride,
                                        uint32_t quant_step)
{
    if (residual == NULL || residual_plane_validate(width, height, residual_stride) != TC_OK ||
        quant_step == 0u || quant_step > 65535u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base residual scale arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (quant_step == 1u) { return TC_OK; }
    for (uint32_t y = 0u; y < height; ++y) {
        int32_t* row = residual + (size_t)y * residual_stride;
        for (uint32_t x = 0u; x < width; ++x) {
            const int64_t value = (int64_t)row[x] * (int64_t)quant_step;
            if (value > INT32_MAX || value < INT32_MIN) {
                tc_set_error(TC_ERR_MALFORMED, "base residual scale overflow");
                return TC_ERR_MALFORMED;
            }
            row[x] = (int32_t)value;
        }
    }
    return TC_OK;
}

int32_t tc_base_residual_reconstruct_u16(const uint16_t* base,
                                         uint32_t base_w, uint32_t base_h,
                                         size_t base_stride,
                                         const int32_t* residual,
                                         uint32_t source_w, uint32_t source_h,
                                         size_t residual_stride,
                                         uint16_t* output, size_t output_stride,
                                         uint8_t bit_depth)
{
    if (output == NULL || residual_arguments(base, base_w, base_h, base_stride,
                                              source_w, source_h, residual,
                                              residual_stride, bit_depth) != TC_OK ||
        output_stride < (size_t)source_w) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base residual reconstruct arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    size_t output_bytes = 0u;
    if (!tc_umul_size(output_stride, (size_t)source_h, &output_bytes)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "base residual output stride");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    (void)output_bytes;
    const uint32_t max_value = residual_max(bit_depth);
    residual_axis_map x_map;
    residual_axis_map y_map;
    memset(&x_map, 0, sizeof(x_map));
    memset(&y_map, 0, sizeof(y_map));
    const int x_mapped = residual_axis_map_init(source_w, base_w, &x_map);
    const int y_mapped = residual_axis_map_init(source_h, base_h, &y_map);
    const int mapped = x_mapped != 0 && y_mapped != 0;
    if (mapped != 0) {
        const uint64_t denominator = (uint64_t)x_map.denominator *
                                     (uint64_t)y_map.denominator;
        const uint64_t reciprocal = UINT64_MAX / denominator + 1u;
        residual_horizontal_cache cache;
        const int cache_ready = residual_horizontal_cache_init(source_w, &cache);
        for (uint32_t y = 0u; y < source_h; ++y) {
            uint16_t* output_row = output + (size_t)y * output_stride;
            const int32_t* residual_row = residual + (size_t)y * residual_stride;
            if (cache_ready != 0 &&
                (cache.valid == 0u || cache.cached_y0 != y_map.i0[y] ||
                 cache.cached_y1 != y_map.i1[y])) {
                residual_horizontal_cache_fill(base, base_stride, &x_map,
                                               y_map.i0[y], y_map.i1[y], &cache);
            }
            for (uint32_t x = 0u; x < source_w; ++x) {
                const uint16_t prediction = cache_ready != 0
                    ? residual_predict_cached(&cache, &y_map, y, x, max_value,
                                              denominator, reciprocal)
                    : residual_predict_mapped(base, base_stride, &x_map, &y_map,
                                               x, y, max_value, denominator,
                                               reciprocal);
                int64_t value = (int64_t)prediction + (int64_t)residual_row[x];
                if (value < 0) { value = 0; }
                if (value > (int64_t)max_value) { value = (int64_t)max_value; }
                output_row[x] = (uint16_t)value;
            }
        }
        residual_horizontal_cache_release(&cache);
        residual_axis_map_release(&x_map);
        residual_axis_map_release(&y_map);
        return TC_OK;
    }
    residual_axis_map_release(&x_map);
    residual_axis_map_release(&y_map);
    for (uint32_t y = 0u; y < source_h; ++y) {
        uint16_t* output_row = output + (size_t)y * output_stride;
        const int32_t* residual_row = residual + (size_t)y * residual_stride;
        for (uint32_t x = 0u; x < source_w; ++x) {
            const uint16_t prediction = tc_base_upsample_pixel_unchecked_u16(
                base, base_w, base_h, base_stride, source_w, source_h, x, y,
                max_value);
            int64_t value = (int64_t)prediction + (int64_t)residual_row[x];
            if (value < 0) { value = 0; }
            if (value > (int64_t)max_value) { value = (int64_t)max_value; }
            output_row[x] = (uint16_t)value;
        }
    }
    return TC_OK;
}

int32_t tc_base_full_reconstruct_band_u16(const uint16_t* base,
                                          uint32_t base_w, uint32_t base_h,
                                          size_t base_stride,
                                          const int32_t* residual,
                                          uint32_t source_w, uint32_t source_h,
                                          size_t residual_stride,
                                          uint16_t* output, size_t output_stride,
                                          uint32_t y0, uint32_t band_height,
                                          uint8_t base_valid, uint8_t residual_valid,
                                          uint16_t neutral_value, uint8_t bit_depth)
{
    const uint32_t max_value = residual_max(bit_depth);
    if (output == NULL || max_value == 0u ||
        residual_plane_validate(base_w, base_h, base_stride) != TC_OK ||
        residual_plane_validate(source_w, source_h, output_stride) != TC_OK ||
        base_w > source_w || base_h > source_h || y0 >= source_h ||
        band_height == 0u || band_height > source_h - y0 ||
        output_stride < (size_t)source_w ||
        (base_valid != 0u && base == NULL) ||
        (residual_valid != 0u &&
         (residual == NULL || residual_plane_validate(source_w, source_h,
                                                       residual_stride) != TC_OK))) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base full reconstruction arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    size_t base_bytes = 0u, output_bytes = 0u;
    if (!tc_umul_size(base_stride, (size_t)base_h, &base_bytes) ||
        !tc_umul_size(output_stride, (size_t)source_h, &output_bytes)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "base full reconstruction stride");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    (void)base_bytes;
    (void)output_bytes;
    if (neutral_value > max_value) { neutral_value = (uint16_t)max_value; }
    for (uint32_t y = y0; y < y0 + band_height; ++y) {
        uint16_t* output_row = output + (size_t)y * output_stride;
        const int32_t* residual_row = residual_valid != 0u
            ? residual + (size_t)y * residual_stride : NULL;
        for (uint32_t x = 0u; x < source_w; ++x) {
            if (base_valid == 0u) {
                output_row[x] = neutral_value;
                continue;
            }
            const uint16_t prediction = tc_base_upsample_pixel_unchecked_u16(
                base, base_w, base_h, base_stride, source_w, source_h, x, y,
                max_value);
            int64_t value = (int64_t)prediction;
            if (residual_row != NULL) { value += (int64_t)residual_row[x]; }
            if (value < 0) { value = 0; }
            if (value > (int64_t)max_value) { value = (int64_t)max_value; }
            output_row[x] = (uint16_t)value;
        }
    }
    return TC_OK;
}

int32_t tc_base_full_reconstruct_u16(const uint16_t* base,
                                     uint32_t base_w, uint32_t base_h,
                                     size_t base_stride,
                                     const int32_t* residual,
                                     uint32_t source_w, uint32_t source_h,
                                     size_t residual_stride,
                                     uint16_t* output, size_t output_stride,
                                     uint8_t base_valid, uint8_t residual_valid,
                                     uint16_t neutral_value, uint8_t bit_depth)
{
    return tc_base_full_reconstruct_band_u16(
        base, base_w, base_h, base_stride, residual, source_w, source_h,
        residual_stride, output, output_stride, 0u, source_h, base_valid,
        residual_valid, neutral_value, bit_depth);
}

static int32_t residual_rice_params(const int32_t* residual, uint32_t width,
                                    uint32_t height, size_t stride,
                                    uint32_t* k_level, uint32_t* k_run)
{
    if (residual == NULL || k_level == NULL || k_run == NULL ||
        residual_plane_validate(width, height, stride) != TC_OK) {
        return TC_ERR_INVALID_ARGUMENT;
    }
    residual_rice_accumulator acc;
    memset(&acc, 0, sizeof(acc));
    for (uint32_t y = 0u; y < height; ++y) {
        const int32_t* row = residual + (size_t)y * stride;
        for (uint32_t x = 0u; x < width; ++x) {
            int32_t rc = residual_rice_accumulate(&acc, row[x]);
            if (rc != TC_OK) { return rc; }
        }
    }
    residual_rice_finish(&acc, k_level, k_run);
    return TC_OK;
}

int32_t tc_base_residual_encode_with_params(const int32_t* residual,
                                uint32_t width, uint32_t height,
                                size_t residual_stride, tc_bitwriter* bw,
                                uint32_t k_level, uint32_t k_run)
{
    if (bw == NULL || residual == NULL || residual_plane_validate(width, height,
                                                                    residual_stride) != TC_OK ||
        k_level > (uint32_t)TC_RICE_K_MAX || k_run > (uint32_t)TC_RICE_K_MAX) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base residual encode parameters");
        return TC_ERR_INVALID_ARGUMENT;
    }
    size_t count = 0u;
    if (!tc_umul_size((size_t)width, (size_t)height, &count)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "base residual count");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    uint64_t run = 0u;
    size_t pos = 0u;
    for (uint32_t y = 0u; y < height; ++y) {
        const int32_t* row = residual + (size_t)y * residual_stride;
        for (uint32_t x = 0u; x < width; ++x) {
            const int32_t value = row[x];
            if (value == 0) {
                ++run;
                continue;
            }
            if (value > 65535 || value < -65535) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT, "base residual level domain");
                return TC_ERR_INVALID_ARGUMENT;
            }
            int32_t rc = tc_rice_encode_inline(bw, k_run, (uint32_t)run);
            if (rc != TC_OK) { return rc; }
            rc = tc_rice_encode_inline(bw, k_level, tc_rice_map_signed(value));
            if (rc != TC_OK) { return rc; }
            pos = (size_t)y * (size_t)width + (size_t)x + 1u;
            run = 0u;
        }
    }
    if ((uint64_t)count - (uint64_t)pos > 0u) {
        int32_t rc = tc_rice_encode_inline(bw, k_run, (uint32_t)((uint64_t)count - pos));
        if (rc != TC_OK) { return rc; }
        rc = tc_rice_encode_inline(bw, k_level, 0u);
        return rc;
    }
    return TC_OK;
}

int32_t tc_base_residual_encode(const int32_t* residual,
                                uint32_t width, uint32_t height,
                                size_t residual_stride, tc_bitwriter* bw,
                                uint32_t* k_level, uint32_t* k_run)
{
    if (bw == NULL || k_level == NULL || k_run == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base residual bitwriter/params is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    int32_t rc = residual_rice_params(residual, width, height, residual_stride,
                                      k_level, k_run);
    if (rc != TC_OK) { return rc; }
    return tc_base_residual_encode_with_params(
        residual, width, height, residual_stride, bw, *k_level, *k_run);
}

typedef struct residual_decode_sink_ctx {
    int32_t* residual;
    uint32_t width;
    size_t stride;
} residual_decode_sink_ctx;

static int32_t residual_decode_sink(void* opaque, size_t pos,
                                    uint32_t run, int32_t level)
{
    residual_decode_sink_ctx* ctx = (residual_decode_sink_ctx*)opaque;
    size_t end = pos + (size_t)run;
    for (size_t i = pos; i < end; ++i) {
        ctx->residual[(i / ctx->width) * ctx->stride + (i % ctx->width)] = 0;
    }
    if (level != 0) {
        const size_t index = end;
        ctx->residual[(index / ctx->width) * ctx->stride + (index % ctx->width)] = level;
    }
    return TC_OK;
}

int32_t tc_base_residual_decode(tc_bitreader* br, int32_t* residual,
                                uint32_t width, uint32_t height,
                                size_t residual_stride,
                                uint32_t k_level, uint32_t k_run)
{
    if (br == NULL || residual == NULL || residual_plane_validate(width, height,
                                                                    residual_stride) != TC_OK ||
        k_level > (uint32_t)TC_RICE_K_MAX || k_run > (uint32_t)TC_RICE_K_MAX) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base residual decode arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    size_t count = 0u;
    if (!tc_umul_size((size_t)width, (size_t)height, &count)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "base residual decode count");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    residual_decode_sink_ctx ctx = {
        .residual = residual,
        .width = width,
        .stride = residual_stride
    };
    return tc_alpha_pairs_decode(br, k_level, k_run, count,
                                 residual_decode_sink, &ctx, NULL);
}
