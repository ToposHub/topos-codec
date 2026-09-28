/* V7-A frame reader: directory-driven band selection plus existing pixel store. */
#ifndef TOPOS_INTERNAL_V7_FRAME_CODEC_H
#define TOPOS_INTERNAL_V7_FRAME_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "topos_codec.h"
#include "v7_band_decode.h"

/* max_band=0..4 opens B0, B0..B1, ..., B0..B4 respectively.  With target
 * geometry set to zero the output keeps source dimensions; otherwise the
 * existing target-grid sampled store writes the requested dimensions.  V7-A
 * remains a coefficient-band reader, not the resolution-independent V7-B
 * base layer. */
int32_t tc_v7_frame_decode(const uint8_t* packet, size_t packet_size,
                           uint32_t max_band,
                           uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                           const size_t strides[TC_FRAME_MAX_PLANES],
                           topos_frame_output* out_info,
                           tc_v7_decode_stats* stats);

int32_t tc_v7_frame_decode_scaled(const uint8_t* packet, size_t packet_size,
                                  uint32_t max_band,
                                  uint32_t target_width, uint32_t target_height,
                                  uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                  const size_t strides[TC_FRAME_MAX_PLANES],
                                  topos_frame_output* out_info,
                                  tc_v7_decode_stats* stats);

#endif /* TOPOS_INTERNAL_V7_FRAME_CODEC_H */
