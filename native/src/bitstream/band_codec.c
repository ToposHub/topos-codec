/* RD2-06：V7-A enhancement map 与 local_run Rice 参数的体积门实现。 */
#include "band_codec.h"

#include <limits.h>
#include <string.h>

#include "../common/checked.h"
#include "../common/crc32.h"
#include "../common/error.h"
#include "../entropy/block_coding.h"
#include "../entropy/rice.h"

static const uint8_t kBandLength[TC_V7_BAND_COUNT] = { 4u, 6u, 6u, 8u, 39u };

static uint32_t ceil_log2_u32(uint32_t value)
{
    uint32_t bits = 0u;
    uint32_t limit = 1u;
    while (limit < value && bits < 32u) {
        limit <<= 1u;
        ++bits;
    }
    return bits == 0u ? 1u : bits;
}

static uint32_t pair_count_bits(uint32_t band_index)
{
    /* B3 covers scan positions 17..24, so a block may contain all eight
     * coefficients.  Three bits represent only 0..7; using that width
     * truncated a legal count of eight and desynchronised every following
     * block in the direct pair-count map. */
    if (band_index == 4u) { return 6u; }
    if (band_index == 3u) { return 4u; }
    return 3u;
}

static uint64_t active_map_bits(uint32_t block_count, uint32_t active_blocks)
{
    return 32u + (uint64_t)active_blocks *
        ((uint64_t)ceil_log2_u32(block_count) + 6u);
}

static uint32_t use_pair_count_map(uint32_t band_index, uint32_t block_count,
                                   uint32_t active_blocks)
{
    if (band_index == 0u) { return 0u; }
    return (uint64_t)block_count * pair_count_bits(band_index) <
           active_map_bits(block_count, active_blocks) ? 1u : 0u;
}

static uint32_t local_run_code_bits(uint32_t band_index, uint32_t k_run,
                                    uint32_t run)
{
    if (band_index == 0u) { return 2u; }
    uint64_t value = 0u;
    return tc_rice_code_word(k_run, run, &value);
}

static int32_t band_input_validate(uint32_t band_index, uint32_t k_dc, uint32_t k_level,
                                   const tc_v7_band_input* input,
                                   uint64_t* bits_out)
{
    if (input == NULL || bits_out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a band input/output is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (band_index >= TC_V7_BAND_COUNT || k_dc > (uint32_t)TC_RICE_K_MAX ||
        k_level > (uint32_t)TC_RICE_K_MAX || input->block_count == 0u ||
        input->pair_count == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a band encoder parameters");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (band_index == 0u && input->dc_m == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a B0 dc tokens are NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (input->block_count > UINT32_MAX / TC_V7_BAND_COUNT) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a block count");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    uint64_t expected_tokens = 0u;
    uint32_t active_blocks = 0u;
    for (uint32_t block = 0u; block < input->block_count; ++block) {
        uint16_t pairs = input->pair_count[block];
        if ((uint32_t)pairs > (uint32_t)kBandLength[band_index]) {
            tc_set_error(TC_ERR_MALFORMED, "v7a band pair count at block %u",
                         (unsigned)block);
            return TC_ERR_MALFORMED;
        }
        if (pairs != 0u) { ++active_blocks; }
        expected_tokens += (uint64_t)pairs;
    }
    if (expected_tokens != (uint64_t)input->token_count) {
        tc_set_error(TC_ERR_MALFORMED, "v7a band token count %u != %llu",
                     (unsigned)input->token_count,
                     (unsigned long long)expected_tokens);
        return TC_ERR_MALFORMED;
    }
    if (input->token_count != 0u &&
        (input->local_runs == NULL || input->level_m == NULL)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a band token arrays are NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }

    /* Validate each block's local positions once. The encoded stream carries
     * deltas, so checking each run independently is insufficient: the sum
     * must stay inside this band's fixed scan interval. */
    size_t validation_index = 0u;
    for (uint32_t block = 0u; block < input->block_count; ++block) {
        uint32_t next_pos = 0u;
        for (uint16_t pair = 0u; pair < input->pair_count[block]; ++pair) {
            uint8_t run = input->local_runs[validation_index];
            uint32_t level = input->level_m[validation_index];
            if (run >= kBandLength[band_index] ||
                next_pos > (uint32_t)kBandLength[band_index] - (uint32_t)run - 1u ||
                level == 0u || level > TC_RICE_M_MAX_AC_LEVEL) {
                tc_set_error(TC_ERR_MALFORMED, "v7a band local token at %zu",
                             validation_index);
                return TC_ERR_MALFORMED;
            }
            next_pos += (uint32_t)run + 1u;
            ++validation_index;
        }
    }

    uint64_t bits = 32u; /* version, k_dc, k_level, flags */
    size_t token_index = 0u;
    if (band_index == 0u) {
        for (uint32_t block = 0u; block < input->block_count; ++block) {
            uint32_t dc = input->dc_m[block];
            if (dc > TC_RICE_M_MAX_DC) {
                tc_set_error(TC_ERR_MALFORMED, "v7a B0 dc m at block %u", (unsigned)block);
                return TC_ERR_MALFORMED;
            }
            bits += (uint64_t)tc_rice_code_word(k_dc, dc, &(uint64_t){ 0u });
            bits += 3u; /* B0 pair_count */
            for (uint16_t pair = 0u; pair < input->pair_count[block]; ++pair) {
                uint8_t run = input->local_runs[token_index];
                uint32_t level = input->level_m[token_index];
                if (run >= kBandLength[band_index] || level == 0u ||
                    level > TC_RICE_M_MAX_AC_LEVEL) {
                    tc_set_error(TC_ERR_MALFORMED, "v7a B0 token at %zu", token_index);
                    return TC_ERR_MALFORMED;
                }
                bits += local_run_code_bits(band_index, k_dc, run);
                bits += (uint64_t)tc_rice_code_word(k_level, level, &(uint64_t){ 0u });
                ++token_index;
            }
        }
    } else {
        uint64_t map_bits = use_pair_count_map(band_index, input->block_count,
                                               active_blocks) != 0u
                          ? (uint64_t)input->block_count * pair_count_bits(band_index)
                          : active_map_bits(input->block_count, active_blocks);
        bits += map_bits;
        for (uint32_t block = 0u; block < input->block_count; ++block) {
            for (uint16_t pair = 0u; pair < input->pair_count[block]; ++pair) {
                uint8_t run = input->local_runs[token_index];
                uint32_t level = input->level_m[token_index];
                if (run >= kBandLength[band_index] || level == 0u ||
                    level > TC_RICE_M_MAX_AC_LEVEL) {
                    tc_set_error(TC_ERR_MALFORMED, "v7a band token at %zu", token_index);
                    return TC_ERR_MALFORMED;
                }
                bits += local_run_code_bits(band_index, k_dc, run);
                bits += (uint64_t)tc_rice_code_word(k_level, level, &(uint64_t){ 0u });
                ++token_index;
            }
        }
    }
    if (bits > UINT64_MAX - 7u) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a band bit count");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    *bits_out = bits;
    return TC_OK;
}

static int32_t put_u8(tc_bitwriter* bw, uint32_t value)
{
    return tc_bitwriter_put_bits_inline(bw, 8u, value & 0xFFu);
}

static int32_t read_u8(tc_bitreader* br, uint32_t* value)
{
    return tc_bitreader_read_bits_inline(br, 8u, value);
}

static int32_t put_local_run(tc_bitwriter* bw, uint32_t band_index, uint32_t k_run,
                             uint32_t run)
{
    if (band_index == 0u) {
        return tc_bitwriter_put_bits_inline(bw, 2u, run);
    }
    return tc_rice_encode_inline(bw, k_run, run);
}

static int32_t read_local_run(tc_bitreader* br, uint32_t band_index, uint32_t k_run,
                              const uint16_t* short_lut_row,
                              const uint16_t* lut_row, uint32_t* run)
{
    if (band_index == 0u) {
        return tc_bitreader_read_bits_inline(br, 2u, run);
    }
    return tc_rice_decode_lut_inline(br, short_lut_row, lut_row, k_run,
                                     TC_RICE_M_MAX_RUN, run);
}

int32_t tc_v7_band_bits(uint32_t band_index, uint32_t k_dc, uint32_t k_level,
                        const tc_v7_band_input* input, uint64_t* out_bits)
{
    return band_input_validate(band_index, k_dc, k_level, input, out_bits);
}

int32_t tc_v7_band_encode(uint32_t band_index, uint32_t k_dc, uint32_t k_level,
                          const tc_v7_band_input* input, tc_bitwriter* bw,
                          uint32_t* payload_size, uint32_t* payload_crc32)
{
    if (bw == NULL || payload_size == NULL || payload_crc32 == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a band encoder output is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    uint64_t bits = 0u;
    int32_t rc = band_input_validate(band_index, k_dc, k_level, input, &bits);
    if (rc != TC_OK) { return rc; }
    if (tc_bitwriter_bits_written(bw) != 0u) {
        tc_set_error(TC_ERR_STATE, "v7a band writer is not reset");
        return TC_ERR_STATE;
    }
    if (bits > (uint64_t)TC_BITWRITER_MAX_BYTES * 8u) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a band payload");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    rc = put_u8(bw, 1u);
    if (rc != TC_OK) { return rc; }
    rc = put_u8(bw, k_dc);
    if (rc != TC_OK) { return rc; }
    rc = put_u8(bw, k_level);
    if (rc != TC_OK) { return rc; }
    uint32_t active_blocks = 0u;
    if (band_index != 0u) {
        for (uint32_t block = 0u; block < input->block_count; ++block) {
            if (input->pair_count[block] != 0u) { ++active_blocks; }
        }
    }
    uint32_t pair_map = use_pair_count_map(band_index, input->block_count,
                                            active_blocks);
    rc = put_u8(bw, pair_map != 0u ? 1u : 0u);
    if (rc != TC_OK) { return rc; }

    size_t token_index = 0u;
    if (band_index == 0u) {
        for (uint32_t block = 0u; block < input->block_count; ++block) {
            rc = tc_rice_encode_inline(bw, k_dc, input->dc_m[block]);
            if (rc != TC_OK) { return rc; }
            rc = tc_bitwriter_put_bits_inline(bw, 3u, input->pair_count[block]);
            if (rc != TC_OK) { return rc; }
            for (uint16_t pair = 0u; pair < input->pair_count[block]; ++pair) {
                rc = put_local_run(bw, band_index, k_dc, input->local_runs[token_index]);
                if (rc != TC_OK) { return rc; }
                rc = tc_rice_encode_inline(bw, k_level, input->level_m[token_index]);
                if (rc != TC_OK) { return rc; }
                ++token_index;
            }
        }
    } else {
        if (pair_map != 0u) {
            const uint32_t count_bits = pair_count_bits(band_index);
            for (uint32_t block = 0u; block < input->block_count; ++block) {
                rc = tc_bitwriter_put_bits_inline(bw, count_bits,
                                                  input->pair_count[block]);
                if (rc != TC_OK) { return rc; }
                for (uint16_t pair = 0u; pair < input->pair_count[block]; ++pair) {
                    rc = put_local_run(bw, band_index, k_dc, input->local_runs[token_index]);
                    if (rc != TC_OK) { return rc; }
                    rc = tc_rice_encode_inline(bw, k_level, input->level_m[token_index]);
                    if (rc != TC_OK) { return rc; }
                    ++token_index;
                }
            }
            goto finish_payload;
        }
        rc = tc_bitwriter_put_bits_inline(bw, 32u, active_blocks);
        if (rc != TC_OK) { return rc; }
        uint32_t index_bits = ceil_log2_u32(input->block_count);
        uint32_t previous_end = 0u;
        for (uint32_t block = 0u; block < input->block_count; ++block) {
            if (input->pair_count[block] == 0u) { continue; }
            rc = tc_bitwriter_put_bits_inline(bw, index_bits, block - previous_end);
            if (rc != TC_OK) { return rc; }
            previous_end = block + 1u;
            rc = tc_bitwriter_put_bits_inline(bw, 6u, input->pair_count[block]);
            if (rc != TC_OK) { return rc; }
            for (uint16_t pair = 0u; pair < input->pair_count[block]; ++pair) {
                rc = put_local_run(bw, band_index, k_dc, input->local_runs[token_index]);
                if (rc != TC_OK) { return rc; }
                rc = tc_rice_encode_inline(bw, k_level, input->level_m[token_index]);
                if (rc != TC_OK) { return rc; }
                ++token_index;
            }
        }
    }
finish_payload:
    rc = tc_bitwriter_flush_zero_pad(bw);
    if (rc != TC_OK) { return rc; }
    size_t bytes = tc_bitwriter_byte_size(bw);
    if (bytes > UINT32_MAX) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a band payload bytes");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    *payload_size = (uint32_t)bytes;
    *payload_crc32 = tc_crc32(tc_bitwriter_data(bw), bytes);
    return TC_OK;
}

static int32_t decode_level(tc_bitreader* br, uint32_t k_level,
                            const uint16_t* short_lut_row,
                            const uint16_t* lut_row, uint32_t* level_m)
{
    int32_t rc = tc_rice_decode_lut_inline(br, short_lut_row, lut_row, k_level,
                                           TC_RICE_M_MAX_AC_LEVEL, level_m);
    if (rc != TC_OK) { return rc; }
    if (*level_m == 0u) {
        tc_bitreader_fail(br, TC_ERR_MALFORMED);
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

static int32_t decode_finish(tc_bitreader* br, size_t payload_size)
{
    uint32_t rem = (uint32_t)(tc_bitreader_bits_consumed(br) & 7u);
    if (rem != 0u) {
        uint32_t padding = 0u;
        int32_t rc = tc_bitreader_read_bits_inline(br, 8u - rem, &padding);
        if (rc != TC_OK) { return rc; }
        if (padding != 0u) {
            tc_bitreader_fail(br, TC_ERR_MALFORMED);
            return TC_ERR_MALFORMED;
        }
    }
    if (tc_bitreader_bits_consumed(br) != (uint64_t)payload_size * 8u) {
        tc_bitreader_fail(br, TC_ERR_MALFORMED);
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

int32_t tc_v7_band_decode(const uint8_t* payload, size_t payload_size,
                          uint32_t band_index, uint32_t block_count, uint32_t block_cols,
                          int32_t* q_zig)
{
    if (payload == NULL || q_zig == NULL || band_index >= TC_V7_BAND_COUNT ||
        block_count == 0u || block_cols == 0u || block_cols > block_count ||
        block_count % block_cols != 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a band decoder parameters");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (payload_size > (size_t)TC_BITWRITER_MAX_BYTES ||
        !tc_umul_size((size_t)block_count, 64u, &(size_t){ 0u })) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a band decode size");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    tc_bitreader br;
    tc_bitreader_init(&br, payload, payload_size);
    uint32_t value = 0u;
    int32_t rc = read_u8(&br, &value);
    if (rc != TC_OK) { return rc; }
    if (value != 1u) {
        tc_bitreader_fail(&br, TC_ERR_UNSUPPORTED_VERSION);
        return TC_ERR_UNSUPPORTED_VERSION;
    }
    uint32_t k_dc = 0u, k_level = 0u, flags = 0u;
    rc = read_u8(&br, &k_dc);
    if (rc != TC_OK) { return rc; }
    rc = read_u8(&br, &k_level);
    if (rc != TC_OK) { return rc; }
    rc = read_u8(&br, &flags);
    if (rc != TC_OK) { return rc; }
    if (k_dc > (uint32_t)TC_RICE_K_MAX || k_level > (uint32_t)TC_RICE_K_MAX ||
        (flags & ~1u) != 0u || (band_index == 0u && flags != 0u)) {
        tc_bitreader_fail(&br, TC_ERR_UNSUPPORTED_VERSION);
        return TC_ERR_UNSUPPORTED_VERSION;
    }
    /* RD3-01：只为已进入的 segment 预热并提升两级 LUT 行指针；被
     * directory reduced 路径跳过的 segment 不会创建 entropy reader。 */
    rc = tc_rice_lut_ensure(k_dc);
    if (rc != TC_OK) { return rc; }
    rc = tc_rice_lut_ensure(k_level);
    if (rc != TC_OK) { return rc; }
    const uint16_t* short_lut_dc = tc_rice_short_lut[k_dc];
    const uint16_t* lut_dc = tc_rice_lut[k_dc];
    const uint16_t* short_lut_level = tc_rice_short_lut[k_level];
    const uint16_t* lut_level = tc_rice_lut[k_level];

    const uint32_t band_length = (uint32_t)kBandLength[band_index];
    if (band_index == 0u) {
        for (uint32_t block = 0u; block < block_count; ++block) {
            uint32_t dc_m = 0u;
            rc = tc_rice_decode_lut_inline(&br, short_lut_dc, lut_dc, k_dc,
                                           TC_RICE_M_MAX_DC, &dc_m);
            if (rc != TC_OK) { return rc; }
            uint32_t by = block / block_cols;
            uint32_t bx = block % block_cols;
            int has_left = bx > 0u ? 1 : 0;
            int has_top = by > 0u ? 1 : 0;
            int32_t left = has_left != 0 ? q_zig[(size_t)(block - 1u) * 64u] : 0;
            int32_t top = has_top != 0 ? q_zig[(size_t)(block - block_cols) * 64u] : 0;
            int32_t dc = 0;
            rc = tc_dc_reconstruct_checked(tc_dc_predict(has_left, left, has_top, top),
                                           dc_m, &dc);
            if (rc != TC_OK) {
                tc_bitreader_fail(&br, rc);
                return rc;
            }
            q_zig[(size_t)block * 64u] = dc;
            uint32_t pair_count = 0u;
            rc = tc_bitreader_read_bits_inline(&br, 3u, &pair_count);
            if (rc != TC_OK) { return rc; }
            if (pair_count > band_length) {
                tc_bitreader_fail(&br, TC_ERR_MALFORMED);
                return TC_ERR_MALFORMED;
            }
            uint32_t next_pos = 0u;
            for (uint32_t pair = 0u; pair < pair_count; ++pair) {
                uint32_t run = 0u, level_m = 0u;
                rc = read_local_run(&br, band_index, k_dc, short_lut_dc,
                                    lut_dc, &run);
                if (rc != TC_OK) { return rc; }
                if (next_pos > band_length - run - 1u) {
                    tc_bitreader_fail(&br, TC_ERR_MALFORMED);
                    return TC_ERR_MALFORMED;
                }
                next_pos += run + 1u;
                rc = decode_level(&br, k_level, short_lut_level, lut_level,
                                  &level_m);
                if (rc != TC_OK) { return rc; }
                q_zig[(size_t)block * 64u + 1u + next_pos - 1u] =
                    tc_rice_unmap_signed(level_m);
            }
        }
    } else if ((flags & 1u) != 0u) {
        const uint32_t count_bits = pair_count_bits(band_index);
        for (uint32_t block = 0u; block < block_count; ++block) {
            uint32_t pair_count = 0u;
            rc = tc_bitreader_read_bits_inline(&br, count_bits, &pair_count);
            if (rc != TC_OK) { return rc; }
            if (pair_count > band_length) {
                tc_bitreader_fail(&br, TC_ERR_MALFORMED);
                return TC_ERR_MALFORMED;
            }
            uint32_t next_pos = 0u;
            for (uint32_t pair = 0u; pair < pair_count; ++pair) {
                uint32_t run = 0u, level_m = 0u;
                rc = read_local_run(&br, band_index, k_dc, short_lut_dc,
                                    lut_dc, &run);
                if (rc != TC_OK) { return rc; }
                if (next_pos > band_length - run - 1u) {
                    tc_bitreader_fail(&br, TC_ERR_MALFORMED);
                    return TC_ERR_MALFORMED;
                }
                next_pos += run + 1u;
                rc = decode_level(&br, k_level, short_lut_level, lut_level,
                                  &level_m);
                if (rc != TC_OK) { return rc; }
                const uint32_t scan_pos = (band_index == 1u ? 5u :
                                           band_index == 2u ? 11u :
                                           band_index == 3u ? 17u : 25u) + next_pos - 1u;
                q_zig[(size_t)block * 64u + scan_pos] = tc_rice_unmap_signed(level_m);
            }
        }
    } else {
        uint32_t active_count = 0u;
        rc = tc_bitreader_read_bits_inline(&br, 32u, &active_count);
        if (rc != TC_OK) { return rc; }
        if (active_count > block_count) {
            tc_bitreader_fail(&br, TC_ERR_MALFORMED);
            return TC_ERR_MALFORMED;
        }
        uint32_t index_bits = ceil_log2_u32(block_count);
        uint32_t previous_end = 0u;
        for (uint32_t active = 0u; active < active_count; ++active) {
            uint32_t delta = 0u;
            rc = tc_bitreader_read_bits_inline(&br, index_bits, &delta);
            if (rc != TC_OK) { return rc; }
            if (delta > block_count - previous_end) {
                tc_bitreader_fail(&br, TC_ERR_MALFORMED);
                return TC_ERR_MALFORMED;
            }
            uint32_t block = previous_end + delta;
            if (block >= block_count) {
                tc_bitreader_fail(&br, TC_ERR_MALFORMED);
                return TC_ERR_MALFORMED;
            }
            previous_end = block + 1u;
            uint32_t pair_count = 0u;
            rc = tc_bitreader_read_bits_inline(&br, 6u, &pair_count);
            if (rc != TC_OK) { return rc; }
            if (pair_count == 0u || pair_count > band_length) {
                tc_bitreader_fail(&br, TC_ERR_MALFORMED);
                return TC_ERR_MALFORMED;
            }
            uint32_t next_pos = 0u;
            for (uint32_t pair = 0u; pair < pair_count; ++pair) {
                uint32_t run = 0u, level_m = 0u;
                rc = read_local_run(&br, band_index, k_dc, short_lut_dc,
                                    lut_dc, &run);
                if (rc != TC_OK) { return rc; }
                if (next_pos > band_length - run - 1u) {
                    tc_bitreader_fail(&br, TC_ERR_MALFORMED);
                    return TC_ERR_MALFORMED;
                }
                next_pos += run + 1u;
                rc = decode_level(&br, k_level, short_lut_level, lut_level,
                                  &level_m);
                if (rc != TC_OK) { return rc; }
                const uint32_t scan_pos = (band_index == 1u ? 5u :
                                           band_index == 2u ? 11u :
                                           band_index == 3u ? 17u : 25u) + next_pos - 1u;
                q_zig[(size_t)block * 64u + scan_pos] = tc_rice_unmap_signed(level_m);
            }
        }
    }
    return decode_finish(&br, payload_size);
}
