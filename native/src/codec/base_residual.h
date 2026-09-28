/* RD4-03：base 预测、signed residual 和独立残差熵流参考原语。 */
#ifndef TOPOS_INTERNAL_BASE_RESIDUAL_H
#define TOPOS_INTERNAL_BASE_RESIDUAL_H

#include <stddef.h>
#include <stdint.h>

#include "../bitstream/bitio.h"

/* Build source - upsample(base) without converting the signed residual to u16. */
int32_t tc_base_residual_build_u16(const uint16_t* source,
                                   uint32_t source_w, uint32_t source_h,
                                   size_t source_stride,
                                   const uint16_t* base,
                                   uint32_t base_w, uint32_t base_h,
                                   size_t base_stride,
                                   int32_t* residual, size_t residual_stride,
                                   uint8_t bit_depth);

/* Build the residual and collect the Rice parameters in the same source
 * traversal.  The ordinary builder remains the compatibility wrapper; V7-B
 * uses this form to avoid a second full-resolution statistics pass. */
int32_t tc_base_residual_build_u16_with_params(
                                   const uint16_t* source,
                                   uint32_t source_w, uint32_t source_h,
                                   size_t source_stride,
                                   const uint16_t* base,
                                   uint32_t base_w, uint32_t base_h,
                                   size_t base_stride,
                                   int32_t* residual, size_t residual_stride,
                                   uint8_t bit_depth,
                                   uint32_t* k_level, uint32_t* k_run);

/* Build a scalar-quantized source - upsample(base) residual.  quant_step is
 * the reconstruction step stored by the V7-B writer: each signed value is
 * rounded symmetrically before entropy coding, then multiplied by this step
 * for full-resolution reconstruction.  A step of one is bit-exact with the
 * compatibility builder above. */
int32_t tc_base_residual_build_quantized_u16_with_params(
                                   const uint16_t* source,
                                   uint32_t source_w, uint32_t source_h,
                                   size_t source_stride,
                                   const uint16_t* base,
                                   uint32_t base_w, uint32_t base_h,
                                   size_t base_stride,
                                   int32_t* residual, size_t residual_stride,
                                   uint8_t bit_depth, uint32_t quant_step,
                                   uint32_t* k_level, uint32_t* k_run);

/* In-place reconstruction scaling for a quantized residual plane. */
int32_t tc_base_residual_scale_in_place(int32_t* residual,
                                        uint32_t width, uint32_t height,
                                        size_t residual_stride,
                                        uint32_t quant_step);

/* 对称舍入量化（ADR-C030 minor-4 detail 系数用；与 scale_in_place 互逆）。 */
int32_t tc_base_residual_quantize_in_place(int32_t* coefficients,
                                           uint32_t width, uint32_t height,
                                           size_t stride, uint32_t quant_step);

/* Reconstruct clamp(upsample(base) + residual) in the source geometry. */
int32_t tc_base_residual_reconstruct_u16(const uint16_t* base,
                                         uint32_t base_w, uint32_t base_h,
                                         size_t base_stride,
                                         const int32_t* residual,
                                         uint32_t source_w, uint32_t source_h,
                                         size_t residual_stride,
                                         uint16_t* output, size_t output_stride,
                                         uint8_t bit_depth);

/* Full-resolution reconstruction for one output band. base_valid=0 conceals
 * the band with neutral_value; residual_valid=0 keeps only the upsampled base. */
int32_t tc_base_full_reconstruct_band_u16(const uint16_t* base,
                                          uint32_t base_w, uint32_t base_h,
                                          size_t base_stride,
                                          const int32_t* residual,
                                          uint32_t source_w, uint32_t source_h,
                                          size_t residual_stride,
                                          uint16_t* output, size_t output_stride,
                                          uint32_t y0, uint32_t band_height,
                                          uint8_t base_valid, uint8_t residual_valid,
                                          uint16_t neutral_value, uint8_t bit_depth);

int32_t tc_base_full_reconstruct_u16(const uint16_t* base,
                                     uint32_t base_w, uint32_t base_h,
                                     size_t base_stride,
                                     const int32_t* residual,
                                     uint32_t source_w, uint32_t source_h,
                                     size_t residual_stride,
                                     uint16_t* output, size_t output_stride,
                                     uint8_t base_valid, uint8_t residual_valid,
                                     uint16_t neutral_value, uint8_t bit_depth);

/* Encode/decode a signed residual plane as a bounded Rice pair stream. The
 * returned k values are part of the future V7-B segment header contract. */
int32_t tc_base_residual_encode(const int32_t* residual,
                                uint32_t width, uint32_t height,
                                size_t residual_stride, tc_bitwriter* bw,
                                uint32_t* k_level, uint32_t* k_run);

/* Encode with parameters collected during the build traversal.  This keeps
 * the bitstream identical to tc_base_residual_encode while avoiding another
 * full residual scan in the scalable writer. */
int32_t tc_base_residual_encode_with_params(const int32_t* residual,
                                uint32_t width, uint32_t height,
                                size_t residual_stride, tc_bitwriter* bw,
                                uint32_t k_level, uint32_t k_run);

int32_t tc_base_residual_decode(tc_bitreader* br, int32_t* residual,
                                uint32_t width, uint32_t height,
                                size_t residual_stride,
                                uint32_t k_level, uint32_t k_run);

#endif /* TOPOS_INTERNAL_BASE_RESIDUAL_H */
