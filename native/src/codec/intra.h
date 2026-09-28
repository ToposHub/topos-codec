/* V3/V6 closed-loop 8x8 spatial prediction. V3 uses frozen VLC books; V6
 * uses the adaptive binary range syntax.
 * Colour slices are independent; alpha retains its existing Rice syntax. */
#ifndef TOPOS_INTERNAL_INTRA_H
#define TOPOS_INTERNAL_INTRA_H

#include "../bitstream/slice_codec.h"
#include "../transform/quant.h"

#define TC_INTRA_MODE_COUNT 8u

/* sh already supplies plane/block_y0/block_h/qp_delta; fills k1/k2/k3.
 * coded is the full padded source plane (tight coded-width stride).
 * Does not reset or byte-align bw; the normal slice assembly does that. */
int32_t tc_intra_slice_encode(const topos_frame_header* fh,
                              topos_slice_header* sh, const uint16_t* coded,
                              const tc_quant_ctx* qctx, tc_bitwriter* bw);

/* Coefficients passed to q_out/sink are natural-order prediction residuals.
 * The hash includes each mode followed by its 64 coefficients.
 * store != NULL enables closed-loop reconstruction and visible output crop;
 * dc_scratch/edge_scratch are optional decoder-context buffers. When supplied,
 * they must remain valid for this call and are cleared before use; otherwise
 * the function allocates bounded per-slice fallback buffers. */
int32_t tc_intra_slice_decode(const topos_frame_header* fh,
                              const topos_slice_header* sh,
                              const uint8_t* payload, size_t payload_size,
                              int32_t* q_out, uint64_t* symbol_hash,
                              tc_color_block_sink_fn sink, void* ctx,
                              struct tc_color_store_ctx* store,
                              tc_scan_dc_ctx* dc_scratch,
                              uint16_t* edge_scratch, size_t edge_elems);

#endif
