/* RD4 scalable base layer reference sampling.
 *
 * All coordinates are plane-local. Downsample is an exact integer box filter
 * over source pixel-cell overlap; upsample is half-pixel bilinear with clamp
 * edge extension. No floating point or platform scaler is involved.
 */
#ifndef TOPOS_INTERNAL_BASE_SCALE_H
#define TOPOS_INTERNAL_BASE_SCALE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "plane.h"

typedef enum tc_base_chroma_siting {
    TC_BASE_CHROMA_LEFT = 0,
    TC_BASE_CHROMA_CENTER = 1,
    TC_BASE_CHROMA_TOPLEFT = 2
} tc_base_chroma_siting;

/* Derive the <= max_dim luma base geometry. The long edge is never rounded
 * above max_dim; dimensions <= max_dim are preserved exactly. */
bool tc_base_scale_dimensions(uint32_t source_w, uint32_t source_h,
                              uint32_t max_dim,
                              uint32_t* target_w, uint32_t* target_h);

/* Derive a plane geometry from luma geometry. Topos YUV 4:2:2 keeps the
 * historical ceil(luma_width/2) chroma width and full height. GBR rejects a
 * non-zero chroma siting because it has no chroma plane. */
bool tc_base_plane_dimensions(uint32_t luma_w, uint32_t luma_h,
                              uint8_t pixel_format, uint32_t plane,
                              uint8_t chroma_siting,
                              uint32_t* plane_w, uint32_t* plane_h);

/* Exact integer area-average downsample. Source and destination buffers must
 * be valid non-overlapping planes, with dst dimensions <= source dimensions. */
bool tc_base_downsample_u16(const uint16_t* src, uint32_t src_w, uint32_t src_h,
                            size_t src_stride, uint16_t* dst,
                            uint32_t dst_w, uint32_t dst_h, size_t dst_stride,
                            uint8_t bit_depth);

/* Half-pixel bilinear upsample with clamp-to-edge extension. */
bool tc_base_upsample_u16(const uint16_t* src, uint32_t src_w, uint32_t src_h,
                          size_t src_stride, uint16_t* dst,
                          uint32_t dst_w, uint32_t dst_h, size_t dst_stride,
                          uint8_t bit_depth);

/* Same fixed predictor as tc_base_upsample_u16, exposed for residual coding so
 * encoder and decoder never grow separate interpolation formulas. */
bool tc_base_upsample_pixel_u16(const uint16_t* src, uint32_t src_w, uint32_t src_h,
                                size_t src_stride, uint32_t dst_w, uint32_t dst_h,
                                uint32_t dst_x, uint32_t dst_y,
                                uint8_t bit_depth, uint16_t* out);

/* Internal hot-loop form. All geometry and bit-depth arguments must already be
 * validated; max_value is sample_max(bit_depth). */
uint16_t tc_base_upsample_pixel_unchecked_u16(const uint16_t* src,
                                              uint32_t src_w, uint32_t src_h,
                                              size_t src_stride, uint32_t dst_w,
                                              uint32_t dst_h, uint32_t dst_x,
                                              uint32_t dst_y, uint32_t max_value);

#endif /* TOPOS_INTERNAL_BASE_SCALE_H */
