/* RD4-04：base-only packet 解码入口。 */
#ifndef TOPOS_INTERNAL_BASE_DECODER_H
#define TOPOS_INTERNAL_BASE_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "topos_codec.h"

/* Decode only an independently encoded base packet whose visible geometry is
 * bounded by max_dim. The packet is validated before any output is touched. */
int32_t tc_base_frame_decode(const uint8_t* data, size_t size, uint32_t max_dim,
                             uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                             const size_t strides[TC_FRAME_MAX_PLANES],
                             topos_frame_output* out_info);

#endif /* TOPOS_INTERNAL_BASE_DECODER_H */
