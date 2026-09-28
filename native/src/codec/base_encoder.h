/* RD4-02：scalable base 的输入适配与独立 base packet 编码。 */
#ifndef TOPOS_INTERNAL_BASE_ENCODER_H
#define TOPOS_INTERNAL_BASE_ENCODER_H

#include <stddef.h>
#include <stdint.h>

#include "topos_codec.h"

typedef struct tc_base_frame {
    topos_frame_config config;
    topos_frame_input input;
    uint16_t* owned_planes[TC_FRAME_MAX_PLANES];
    size_t plane_bytes[TC_FRAME_MAX_PLANES];
    uint32_t plane_width[TC_FRAME_MAX_PLANES];
    uint32_t plane_height[TC_FRAME_MAX_PLANES];
    uint8_t owns_plane[TC_FRAME_MAX_PLANES];
} tc_base_frame;

typedef struct tc_base_budget {
    uint32_t total_bytes;
    uint32_t base_bytes;
    uint32_t residual_bytes;
} tc_base_budget;

/* Split one frame budget by caller-provided size hints. The outputs sum exactly
 * to total_bytes; no layer performs an independent target search. */
int32_t tc_base_split_budget(uint32_t total_bytes, uint32_t base_hint,
                            uint32_t residual_hint, tc_base_budget* out);

/* Prepare an independent <= max_dim base view. Source planes remain caller-owned;
 * only the reduced destination planes are allocated. A source plane is borrowed
 * when its geometry already matches the base geometry. */
int32_t tc_base_frame_prepare(const topos_frame_config* source_config,
                              const topos_frame_input* source_input,
                              uint32_t max_dim,
                              tc_base_frame* out);

void tc_base_frame_release(tc_base_frame* base);

/* Encode the prepared base through the existing frame packet writer. This is an
 * RD4-02 base-only packet primitive; V7-B directory/residual muxing is RD4-03+. */
int32_t tc_base_frame_encode(const topos_frame_config* source_config,
                             const topos_frame_input* source_input,
                             uint32_t max_dim,
                             uint8_t* out, size_t out_cap,
                             topos_frame_stats* stats);

int32_t tc_base_frame_encode_sized(const topos_frame_config* source_config,
                                   const topos_frame_input* source_input,
                                   uint32_t max_dim, uint32_t target_bytes,
                                   uint8_t qp_min, uint8_t qp_max,
                                   uint8_t* qp_used, uint8_t* out, size_t out_cap,
                                   topos_frame_stats* stats);

#endif /* TOPOS_INTERNAL_BASE_ENCODER_H */
