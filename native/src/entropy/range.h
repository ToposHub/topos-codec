/* Lightweight binary range coder used by the V6 intra experiment.
 *
 * The coder is deliberately byte oriented: the surrounding TPIC slice still
 * owns the CRC and the payload boundary, while this module only carries the
 * adaptive binary symbols inside that payload.  Context probabilities are
 * kept in a 12-bit fixed point domain and update without division.
 */
#ifndef TOPOS_INTERNAL_RANGE_H
#define TOPOS_INTERNAL_RANGE_H

#include <stddef.h>
#include <stdint.h>

#include "topos_codec.h"

#define TC_RANGE_CONTEXTS 128u

typedef struct tc_range_ctx {
    uint16_t p0; /* probability of zero, 1..4095 */
} tc_range_ctx;

typedef struct tc_range_encoder {
    uint32_t low;
    uint32_t high;
    uint32_t pending;
    uint8_t* data;
    size_t cap;
    size_t pos;
    uint8_t hold;
    uint8_t hold_bits;
    int32_t error;
} tc_range_encoder;

typedef struct tc_range_decoder {
    uint32_t low;
    uint32_t high;
    uint32_t code;
    const uint8_t* data;
    size_t size;
    size_t pos;
    uint8_t hold;
    uint8_t hold_bits;
    uint64_t bit_pos;
    int32_t error;
} tc_range_decoder;

/*
 * V6 decodes a large number of adaptive symbols in a tight block loop.  Keep
 * the public, checked entry point below for API users and tests, but also
 * expose a checked-at-the-call-site inline path for the codec hot loop.  The
 * inline path is deliberately kept here with the decoder state so the
 * compiler can eliminate the function call and pointer validation for every
 * symbol without changing the wire format.
 */
#define TC_RANGE_TOTAL_INLINE 4096u
#define TC_RANGE_HALF_INLINE UINT32_C(0x80000000)
#define TC_RANGE_QUARTER_INLINE UINT32_C(0x40000000)
#define TC_RANGE_THREE_QUARTER_INLINE UINT32_C(0xC0000000)

static inline void tc_range_context_update_inline(tc_range_ctx* ctx, uint32_t bit)
{
    uint32_t p = ctx->p0;
    if (bit == 0u) {
        p += (TC_RANGE_TOTAL_INLINE - 1u - p) >> 5u;
    } else {
        p -= p >> 5u;
    }
    if (p == 0u) { p = 1u; }
    if (p >= TC_RANGE_TOTAL_INLINE) { p = TC_RANGE_TOTAL_INLINE - 1u; }
    ctx->p0 = (uint16_t)p;
}

static inline int32_t tc_range_decoder_read_bit_inline(tc_range_decoder* rc,
                                                         uint32_t* bit)
{
    if (rc->error != TC_OK) { return rc->error; }
    if (rc->hold_bits == 0u) {
        if (rc->pos >= rc->size) {
            rc->error = TC_ERR_TRUNCATED;
            return rc->error;
        }
        rc->hold = rc->data[rc->pos++];
        rc->hold_bits = 8u;
    }
    *bit = (uint32_t)(rc->hold >> 7u);
    rc->hold <<= 1u;
    rc->hold_bits--;
    rc->bit_pos++;
    return TC_OK;
}

static inline int32_t tc_range_decoder_normalize_inline(tc_range_decoder* rc)
{
    for (;;) {
        if (rc->high < TC_RANGE_HALF_INLINE) {
            /* no offset */
        } else if (rc->low >= TC_RANGE_HALF_INLINE) {
            rc->low -= TC_RANGE_HALF_INLINE;
            rc->high -= TC_RANGE_HALF_INLINE;
            rc->code -= TC_RANGE_HALF_INLINE;
        } else if (rc->low >= TC_RANGE_QUARTER_INLINE &&
                   rc->high < TC_RANGE_THREE_QUARTER_INLINE) {
            rc->low -= TC_RANGE_QUARTER_INLINE;
            rc->high -= TC_RANGE_QUARTER_INLINE;
            rc->code -= TC_RANGE_QUARTER_INLINE;
        } else {
            break;
        }
        rc->low <<= 1u;
        rc->high = (rc->high << 1u) | 1u;
        uint32_t next_bit = 0u;
        int32_t result = tc_range_decoder_read_bit_inline(rc, &next_bit);
        if (result != TC_OK) { return result; }
        rc->code = (rc->code << 1u) | next_bit;
    }
    return TC_OK;
}

/* Caller guarantees rc, ctx and bit are valid; input truncation is still
 * reported through rc->error exactly like tc_range_decode_bit(). */
static inline int32_t tc_range_decode_bit_inline(tc_range_decoder* rc,
                                                  tc_range_ctx* ctx,
                                                  uint32_t* bit)
{
    if (rc->error != TC_OK) { return rc->error; }
    uint32_t p0 = ctx->p0;
    if (p0 == 0u) { p0 = 1u; }
    if (p0 >= TC_RANGE_TOTAL_INLINE) { p0 = TC_RANGE_TOTAL_INLINE - 1u; }
    uint64_t range = (uint64_t)rc->high - (uint64_t)rc->low + 1u;
    uint32_t split = rc->low + (uint32_t)((range * (uint64_t)p0) /
                                          (uint64_t)TC_RANGE_TOTAL_INLINE) - 1u;
    if (rc->code <= split) {
        *bit = 0u;
        rc->high = split;
    } else {
        *bit = 1u;
        rc->low = split + 1u;
    }
    tc_range_context_update_inline(ctx, *bit);
    return tc_range_decoder_normalize_inline(rc);
}

void tc_range_contexts_init(tc_range_ctx contexts[TC_RANGE_CONTEXTS]);

void tc_range_encoder_init(tc_range_encoder* rc, uint8_t* data, size_t cap);
int32_t tc_range_encode_bit(tc_range_encoder* rc, tc_range_ctx* ctx, uint32_t bit);
int32_t tc_range_encoder_finish(tc_range_encoder* rc);

int32_t tc_range_decoder_init(tc_range_decoder* rc, const uint8_t* data, size_t size);
int32_t tc_range_decode_bit(tc_range_decoder* rc, tc_range_ctx* ctx,
                            uint32_t* bit);

size_t tc_range_encoder_size(const tc_range_encoder* rc);
size_t tc_range_decoder_bytes_used(const tc_range_decoder* rc);

#endif /* TOPOS_INTERNAL_RANGE_H */
