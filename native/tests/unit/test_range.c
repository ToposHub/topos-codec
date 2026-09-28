#include "entropy/range.h"

#include <string.h>

#include "mini_test.h"

int main(void)
{
    uint8_t bytes[4096];
    tc_range_encoder enc;
    tc_range_ctx contexts[TC_RANGE_CONTEXTS];
    tc_range_encoder_init(&enc, bytes, sizeof(bytes));
    tc_range_contexts_init(contexts);
    for (uint32_t i = 0u; i < 20000u; ++i) {
        uint32_t bit = ((i * 37u) ^ (i >> 3u)) & 1u;
        MT_CHECK_EQ_I64(tc_range_encode_bit(&enc, &contexts[i & 15u], bit), TC_OK);
    }
    MT_CHECK_EQ_I64(tc_range_encoder_finish(&enc), TC_OK);
    size_t size = tc_range_encoder_size(&enc);
    MT_CHECK(size > 0u && size < sizeof(bytes));

    tc_range_decoder dec;
    tc_range_ctx decode_contexts[TC_RANGE_CONTEXTS];
    MT_CHECK_EQ_I64(tc_range_decoder_init(&dec, bytes, size), TC_OK);
    tc_range_contexts_init(decode_contexts);
    for (uint32_t i = 0u; i < 20000u; ++i) {
        uint32_t bit = 0u;
        uint32_t expected = ((i * 37u) ^ (i >> 3u)) & 1u;
        MT_CHECK_EQ_I64(tc_range_decode_bit(&dec, &decode_contexts[i & 15u], &bit), TC_OK);
        MT_CHECK_EQ_U64(bit, expected);
    }
    MT_CHECK(tc_range_decoder_bytes_used(&dec) <= size);
    MT_CHECK(size - tc_range_decoder_bytes_used(&dec) <= 4u);

    uint8_t truncated[4];
    memcpy(truncated, bytes, sizeof(truncated));
    tc_range_contexts_init(decode_contexts);
    tc_range_decoder_init(&dec, truncated, sizeof(truncated));
    MT_CHECK_EQ_I64(dec.error, TC_OK);
    for (uint32_t i = 0u; i < 20000u && dec.error == TC_OK; ++i) {
        uint32_t bit = 0u;
        (void)tc_range_decode_bit(&dec, &decode_contexts[i & 15u], &bit);
    }
    MT_CHECK_EQ_I64(dec.error, TC_ERR_TRUNCATED);
    return MT_MAIN_RETURN();
}
