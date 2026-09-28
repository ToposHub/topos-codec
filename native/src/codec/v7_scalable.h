/* RD4：V7-B scalable base + signed residual reference packet. */
#ifndef TOPOS_INTERNAL_V7_SCALABLE_H
#define TOPOS_INTERNAL_V7_SCALABLE_H

#include <stddef.h>
#include <stdint.h>

#include "topos_codec.h"
#include "../bitstream/frame_header.h"

enum {
    /* Minor 1 carried a lossless signed-Rice residual.  Minor 2 adds the
     * per-plane scalar reconstruction step in RSD1[6], while preserving the
     * directory and base-range contracts used by preview readers. */
    TC_V7B_DIRECTORY_VERSION_MINOR_REFERENCE = 1u,
    TC_V7B_DIRECTORY_VERSION_MINOR_QUANTIZED = 2u,
    /* Minor 3 replaces one raw-Rice residual per plane with one complete
     * transform-coded enhancement packet spanning all color planes. */
    TC_V7B_DIRECTORY_VERSION_MINOR_TRANSFORM_EXPERIMENTAL = 3u,
    /* Minor 4 is the ADR-C030 shared transform pyramid: one deterministic
     * lifting analysis per plane, minor-2-style embedded base packet, and
     * skippable detail/escape segments. */
    TC_V7B_DIRECTORY_VERSION_MINOR_PYRAMID = 4u,
    /* The public opt-in writer remains on minor 2 until the pyramid layout
     * clears the quality, size, and speed gates. */
    TC_V7B_DIRECTORY_VERSION_MINOR = TC_V7B_DIRECTORY_VERSION_MINOR_QUANTIZED,
    TC_V7B_LAYER_KIND_BASE = 1u,
    TC_V7B_LAYER_KIND_RESIDUAL = 2u,
    TC_V7B_LAYER_KIND_DETAIL = 3u,
    TC_V7B_SEGMENT_FLAG_BASE_PACKET = 1u,
    TC_V7B_SEGMENT_FLAG_RESIDUAL = 2u,
    TC_V7B_SEGMENT_FLAG_DETAIL = 4u,
    TC_V7B_RESIDUAL_HEADER_SIZE = 8u
};

typedef enum tc_v7b_decode_mode {
    TC_V7B_DECODE_BASE_ONLY = 0,
    TC_V7B_DECODE_FULL = 1
} tc_v7b_decode_mode;

typedef struct tc_v7b_decode_stats {
    uint32_t segments_requested;
    uint32_t segments_read;
    uint32_t segments_skipped;
    uint32_t unchecked_segment_count;
    uint64_t bytes_read;
    uint64_t bytes_skipped;
    /* The embedded base decode contributes its own legacy packet bytes to the
     * process telemetry. Keep this split so outer accounting avoids double
     * counting the base payload. */
    uint64_t base_bytes_read;
} tc_v7b_decode_stats;

/* Relative byte reader used by container readers.  The callback must fill the
 * complete requested range or return a Topos error; it is deliberately
 * relative to the elementary sample so the MOV layer can keep its index
 * offsets private. */
typedef int32_t (*tc_v7b_range_read_fn)(void* ctx, uint64_t offset,
                                       void* buf, size_t len);

/* Lightweight dispatch probe. It checks only the V7/TPLD signature; full
 * header, directory, CRC, and layer validation remain decoder responsibilities. */
int tc_v7b_packet_is(const uint8_t* packet, size_t packet_size);

/* Build the first complete V7-B reference packet. The base is an embedded
 * independently decodable Topos packet; residual segments use the bounded
 * signed Rice pair syntax from base_residual.c. This is intentionally
 * internal until the size/speed gate and MOV capability rollout are complete. */
int32_t tc_v7b_frame_encode(const topos_frame_config* source_config,
                            const topos_frame_input* source_input,
                            uint32_t max_dim,
                            uint8_t* out, size_t out_cap,
                            topos_frame_stats* stats);

/* Decode only the embedded <=max_dim base, or reconstruct the full source
 * frame from base plus signed residuals. A NULL plane array is query mode. */
int32_t tc_v7b_frame_decode(const uint8_t* packet, size_t packet_size,
                            uint32_t max_dim, tc_v7b_decode_mode mode,
                            uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                            const size_t strides[TC_FRAME_MAX_PLANES],
                            topos_frame_output* out_info,
                            tc_v7b_decode_stats* stats);

/* Read only the bounded base and resample that base to the requested preview
 * grid. The source-resolution residual layer is never touched. */
int32_t tc_v7b_frame_decode_reduced(const uint8_t* packet, size_t packet_size,
                                    uint32_t target_width, uint32_t target_height,
                                    uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                    const size_t strides[TC_FRAME_MAX_PLANES],
                                    topos_frame_output* out_info,
                                    tc_v7b_decode_stats* stats);

/* ADR-C030 minor-4 shared pyramid writer（实验内部入口；未过 P4 门禁前
 * 公开 tc_frame_encode_scalable 不路由至此）。仅当源几何能被冻结级联
 * {2,3,4,6} 精确整除且 base 落在 max_dim 内时写 minor-4，否则回落到
 * minor-2 Rice 路径（返回值与包均为合法 V7-B）。 */
int32_t tc_v7b_frame_encode_pyramid(const topos_frame_config* source_config,
                                    const topos_frame_input* source_input,
                                    uint32_t max_dim,
                                    uint8_t* out, size_t out_cap,
                                    topos_frame_stats* stats);

/* Read only the independently decodable base packet from a V7-B sample.
 * Header + bounded directory are inspected first; the residual byte ranges
 * are never read.  A NULL/zero output is a size probe and returns
 * TC_ERR_BUFFER_TOO_SMALL with base_size filled. */
int32_t tc_v7b_read_base_packet(tc_v7b_range_read_fn read_fn, void* read_ctx,
                                uint64_t packet_size, uint8_t* base_out,
                                size_t base_cap, size_t* base_size,
                                topos_frame_header* outer_fh,
                                tc_v7b_decode_stats* stats);

#endif /* TOPOS_INTERNAL_V7_SCALABLE_H */
