/* RD2-03：量化结果一次 AC 遍历的 B0..B4 band-local token 分桶。 */
#include "bitstream/band_tokens.h"
#include "bitstream/band_codec.h"

#include <stdlib.h>
#include <string.h>

#include "mini_test.h"

typedef struct token_capture {
    tc_v7_band_token tokens[8];
    uint32_t count;
} token_capture;

static void capture_token(const tc_v7_band_token* token, void* opaque)
{
    token_capture* capture = (token_capture*)opaque;
    if (capture->count < 8u) { capture->tokens[capture->count] = *token; }
    capture->count++;
}

int main(void)
{
    /* V 代际收纳：回放构建专属测试——运行期放行退役代际（frame_header.c 运行期门） */
#if defined(_WIN32)
    _putenv("TOPOS_DEV=1");
#elif defined(__APPLE__)
    setenv("TOPOS_DEV", "1", 1);
#else
    if (setenv("TOPOS_DEV", "1", 1) != 0) { return 2; }
#endif
    tc_v7_band_bucket bucket;
    tc_v7_band_bucket_init(&bucket, 42u);
    token_capture capture = { { { 0u } }, 0u };

    const uint8_t positions[8] = { 1u, 4u, 5u, 10u, 11u, 17u, 25u, 63u };
    const uint8_t bands[8] = { 0u, 0u, 1u, 1u, 2u, 3u, 4u, 4u };
    const uint8_t local_runs[8] = { 0u, 2u, 0u, 4u, 0u, 0u, 0u, 37u };
    for (uint32_t i = 0u; i < 8u; ++i) {
        MT_CHECK_EQ_I64(tc_v7_band_bucket_push(&bucket, positions[i], 100u + i,
                                               capture_token, &capture), TC_OK);
    }
    MT_CHECK_EQ_U64(capture.count, 8u);
    for (uint32_t i = 0u; i < 8u; ++i) {
        MT_CHECK_EQ_U64(capture.tokens[i].block_index, 42u);
        MT_CHECK_EQ_U64(capture.tokens[i].band_index, bands[i]);
        MT_CHECK_EQ_U64(capture.tokens[i].scan_pos, positions[i]);
        MT_CHECK_EQ_U64(capture.tokens[i].local_run, local_runs[i]);
        MT_CHECK_EQ_U64(capture.tokens[i].level_m, 100u + i);
    }
    MT_CHECK_EQ_U64(bucket.pair_count[0], 2u);
    MT_CHECK_EQ_U64(bucket.pair_count[1], 2u);
    MT_CHECK_EQ_U64(bucket.pair_count[2], 1u);
    MT_CHECK_EQ_U64(bucket.pair_count[3], 1u);
    MT_CHECK_EQ_U64(bucket.pair_count[4], 2u);

    tc_v7_band_stats stats;
    tc_v7_band_stats_reset(&stats);
    MT_CHECK_EQ_I64(tc_v7_band_stats_add_block(&stats, &bucket), TC_OK);
    MT_CHECK_EQ_U64(stats.active_block_count[0], 1u);
    MT_CHECK_EQ_U64(stats.active_block_count[4], 1u);
    MT_CHECK_EQ_U64(stats.local_run_sum[4], 37u);
    MT_CHECK_EQ_U64(stats.level_sum[2], 104u);

    MT_CHECK_EQ_I64(tc_v7_band_bucket_push(&bucket, 0u, 1u, NULL, NULL),
                    TC_ERR_INVALID_ARGUMENT);
    MT_CHECK_EQ_I64(tc_v7_band_bucket_push(&bucket, 64u, 1u, NULL, NULL),
                    TC_ERR_INVALID_ARGUMENT);
    MT_CHECK_EQ_I64(tc_v7_band_bucket_push(NULL, 1u, 1u, NULL, NULL),
                    TC_ERR_INVALID_ARGUMENT);

    /* RD2-03: one legacy token sequence is partitioned into independently
     * encodable bands.  The second block exercises a band transition and the
     * B4 maximum local run (scan position 63). */
    const uint32_t dc_m[2] = { 0u, 0u };
    const uint16_t full_pairs[2] = { 4u, 3u };
    const uint8_t full_runs[7] = { 0u, 3u, 5u, 51u, 3u, 12u, 7u };
    const uint32_t full_levels[7] = {
        2u, 3u, 4u, 5u, 6u, 7u, 8u
    };
    uint32_t band_dc[TC_V7_BAND_COUNT][2] = { { 0u } };
    uint16_t band_pairs[TC_V7_BAND_COUNT][2] = { { 0u } };
    uint8_t band_runs[TC_V7_BAND_COUNT][7] = { { 0u } };
    uint32_t band_levels[TC_V7_BAND_COUNT][7] = { { 0u } };
    tc_v7_band_storage storage[TC_V7_BAND_COUNT];
    memset(storage, 0, sizeof(storage));
    for (uint32_t band = 0u; band < TC_V7_BAND_COUNT; ++band) {
        storage[band].dc_m = band_dc[band];
        storage[band].pair_count = band_pairs[band];
        storage[band].local_runs = band_runs[band];
        storage[band].level_m = band_levels[band];
        storage[band].block_capacity = 2u;
        storage[band].token_capacity = 7u;
    }
    MT_CHECK_EQ_I64(tc_v7_band_partition_tokens(2u, dc_m, full_pairs, full_runs,
                                                full_levels, 7u, storage), TC_OK);
    const uint16_t expected_pairs[TC_V7_BAND_COUNT][2] = {
        { 1u, 1u }, { 1u, 0u }, { 1u, 0u }, { 0u, 1u }, { 1u, 1u }
    };
    const uint8_t expected_runs[TC_V7_BAND_COUNT][2] = {
        { 0u, 3u }, { 0u, 0u }, { 0u, 0u }, { 0u, 0u }, { 38u, 0u }
    };
    const uint32_t expected_levels[TC_V7_BAND_COUNT][2] = {
        { 2u, 6u }, { 3u, 0u }, { 4u, 0u }, { 0u, 7u }, { 5u, 8u }
    };
    for (uint32_t band = 0u; band < TC_V7_BAND_COUNT; ++band) {
        MT_CHECK_EQ_U64(storage[band].pair_count[0], expected_pairs[band][0]);
        MT_CHECK_EQ_U64(storage[band].pair_count[1], expected_pairs[band][1]);
        MT_CHECK_EQ_U64(storage[band].token_count,
                        (uint32_t)expected_pairs[band][0] + expected_pairs[band][1]);
        uint32_t token = 0u;
        for (uint32_t block = 0u; block < 2u; ++block) {
            for (uint32_t pair = 0u; pair < storage[band].pair_count[block]; ++pair) {
                MT_CHECK_EQ_U64(storage[band].local_runs[token], expected_runs[band][block]);
                MT_CHECK_EQ_U64(storage[band].level_m[token], expected_levels[band][block]);
                ++token;
            }
        }
    }
    MT_CHECK_EQ_I64(tc_v7_band_partition_tokens(2u, dc_m, full_pairs, full_runs,
                                                full_levels, 6u, storage),
                    TC_ERR_MALFORMED);

    return MT_MAIN_RETURN();
}
