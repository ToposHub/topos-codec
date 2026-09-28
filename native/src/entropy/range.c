#include "range.h"

#include <string.h>

#define TC_RANGE_TOTAL 4096u
#define TC_RANGE_HALF UINT32_C(0x80000000)
#define TC_RANGE_QUARTER UINT32_C(0x40000000)
#define TC_RANGE_THREE_QUARTER UINT32_C(0xC0000000)

static void context_update(tc_range_ctx* ctx, uint32_t bit)
{
    uint32_t p = ctx->p0;
    if (bit == 0u) {
        p += (TC_RANGE_TOTAL - 1u - p) >> 5u;
    } else {
        p -= p >> 5u;
    }
    if (p == 0u) { p = 1u; }
    if (p >= TC_RANGE_TOTAL) { p = TC_RANGE_TOTAL - 1u; }
    ctx->p0 = (uint16_t)p;
}

void tc_range_contexts_init(tc_range_ctx contexts[TC_RANGE_CONTEXTS])
{
    if (contexts == NULL) { return; }
    for (uint32_t i = 0u; i < TC_RANGE_CONTEXTS; ++i) {
        contexts[i].p0 = (uint16_t)(TC_RANGE_TOTAL / 2u);
    }
}

static int32_t encoder_put_bit(tc_range_encoder* rc, uint32_t bit)
{
    if (rc->error != TC_OK) { return rc->error; }
    uint32_t hold = (uint32_t)rc->hold;
    rc->hold = (uint8_t)((hold << 1u) | (bit & 1u));
    rc->hold_bits++;
    if (rc->hold_bits != 8u) { return TC_OK; }
    if (rc->pos >= rc->cap) {
        rc->error = TC_ERR_BUFFER_TOO_SMALL;
        return rc->error;
    }
    rc->data[rc->pos++] = rc->hold;
    rc->hold = 0u;
    rc->hold_bits = 0u;
    return TC_OK;
}

static int32_t encoder_output(tc_range_encoder* rc, uint32_t bit)
{
    int32_t result = encoder_put_bit(rc, bit);
    while (result == TC_OK && rc->pending != 0u) {
        result = encoder_put_bit(rc, bit == 0u ? 1u : 0u);
        rc->pending--;
    }
    return result;
}

static int32_t encoder_normalize(tc_range_encoder* rc)
{
    for (;;) {
        if (rc->high < TC_RANGE_HALF) {
            int32_t result = encoder_output(rc, 0u);
            if (result != TC_OK) { return result; }
        } else if (rc->low >= TC_RANGE_HALF) {
            int32_t result = encoder_output(rc, 1u);
            if (result != TC_OK) { return result; }
            rc->low -= TC_RANGE_HALF;
            rc->high -= TC_RANGE_HALF;
        } else if (rc->low >= TC_RANGE_QUARTER &&
                   rc->high < TC_RANGE_THREE_QUARTER) {
            rc->pending++;
            rc->low -= TC_RANGE_QUARTER;
            rc->high -= TC_RANGE_QUARTER;
        } else {
            break;
        }
        rc->low <<= 1u;
        rc->high = (rc->high << 1u) | 1u;
    }
    return TC_OK;
}

void tc_range_encoder_init(tc_range_encoder* rc, uint8_t* data, size_t cap)
{
    memset(rc, 0, sizeof(*rc));
    rc->high = UINT32_MAX;
    rc->data = data;
    rc->cap = cap;
}

int32_t tc_range_encode_bit(tc_range_encoder* rc, tc_range_ctx* ctx, uint32_t bit)
{
    if (rc == NULL || ctx == NULL || bit > 1u) { return TC_ERR_INVALID_ARGUMENT; }
    if (rc->error != TC_OK) { return rc->error; }
    uint32_t p0 = ctx->p0;
    if (p0 == 0u) { p0 = 1u; }
    if (p0 >= TC_RANGE_TOTAL) { p0 = TC_RANGE_TOTAL - 1u; }
    uint64_t range = (uint64_t)rc->high - (uint64_t)rc->low + 1u;
    uint32_t split = rc->low + (uint32_t)((range * (uint64_t)p0) /
                                          (uint64_t)TC_RANGE_TOTAL) - 1u;
    if (bit == 0u) {
        rc->high = split;
    } else {
        rc->low = split + 1u;
    }
    context_update(ctx, bit);
    return encoder_normalize(rc);
}

int32_t tc_range_encoder_finish(tc_range_encoder* rc)
{
    if (rc == NULL) { return TC_ERR_INVALID_ARGUMENT; }
    if (rc->error != TC_OK) { return rc->error; }
    rc->pending++;
    int32_t result = rc->low < TC_RANGE_QUARTER
                   ? encoder_output(rc, 0u) : encoder_output(rc, 1u);
    if (result != TC_OK) { return result; }
    for (uint32_t i = 0u; i < 32u && result == TC_OK; ++i) {
        result = encoder_put_bit(rc, (rc->low >> (31u - i)) & 1u);
    }
    if (result == TC_OK && rc->hold_bits != 0u) {
        while (rc->hold_bits != 0u && result == TC_OK) {
            result = encoder_put_bit(rc, 0u);
        }
    }
    return result;
}

static int32_t decoder_read_bit(tc_range_decoder* rc, uint32_t* bit)
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

int32_t tc_range_decoder_init(tc_range_decoder* rc, const uint8_t* data, size_t size)
{
    if (rc == NULL || (data == NULL && size != 0u)) { return TC_ERR_INVALID_ARGUMENT; }
    memset(rc, 0, sizeof(*rc));
    rc->high = UINT32_MAX;
    rc->data = data;
    rc->size = size;
    for (uint32_t i = 0u; i < 32u; ++i) {
        uint32_t bit = 0u;
        int32_t result = decoder_read_bit(rc, &bit);
        if (result != TC_OK) { return result; }
        rc->code = (rc->code << 1u) | bit;
    }
    return TC_OK;
}

static int32_t decoder_normalize(tc_range_decoder* rc)
{
    for (;;) {
        if (rc->high < TC_RANGE_HALF) {
            /* no offset */
        } else if (rc->low >= TC_RANGE_HALF) {
            rc->low -= TC_RANGE_HALF;
            rc->high -= TC_RANGE_HALF;
            rc->code -= TC_RANGE_HALF;
        } else if (rc->low >= TC_RANGE_QUARTER &&
                   rc->high < TC_RANGE_THREE_QUARTER) {
            rc->low -= TC_RANGE_QUARTER;
            rc->high -= TC_RANGE_QUARTER;
            rc->code -= TC_RANGE_QUARTER;
        } else {
            break;
        }
        rc->low <<= 1u;
        rc->high = (rc->high << 1u) | 1u;
        uint32_t bit = 0u;
        int32_t result = decoder_read_bit(rc, &bit);
        if (result != TC_OK) { return result; }
        rc->code = (rc->code << 1u) | bit;
    }
    return TC_OK;
}

int32_t tc_range_decode_bit(tc_range_decoder* rc, tc_range_ctx* ctx,
                            uint32_t* bit)
{
    if (rc == NULL || ctx == NULL || bit == NULL) { return TC_ERR_INVALID_ARGUMENT; }
    if (rc->error != TC_OK) { return rc->error; }
    uint32_t p0 = ctx->p0;
    if (p0 == 0u) { p0 = 1u; }
    if (p0 >= TC_RANGE_TOTAL) { p0 = TC_RANGE_TOTAL - 1u; }
    uint64_t range = (uint64_t)rc->high - (uint64_t)rc->low + 1u;
    uint32_t split = rc->low + (uint32_t)((range * (uint64_t)p0) /
                                          (uint64_t)TC_RANGE_TOTAL) - 1u;
    if (rc->code <= split) {
        *bit = 0u;
        rc->high = split;
    } else {
        *bit = 1u;
        rc->low = split + 1u;
    }
    context_update(ctx, *bit);
    return decoder_normalize(rc);
}

size_t tc_range_encoder_size(const tc_range_encoder* rc)
{
    return rc != NULL ? rc->pos + (rc->hold_bits != 0u ? 1u : 0u) : 0u;
}

size_t tc_range_decoder_bytes_used(const tc_range_decoder* rc)
{
    return rc != NULL ? rc->pos : 0u;
}
