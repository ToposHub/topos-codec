#include "band_tokens.h"

#include <limits.h>
#include <string.h>

#include "../common/error.h"

static const uint8_t kBandStart[TC_V7_BAND_COUNT] = { 1u, 5u, 11u, 17u, 25u };
static const uint8_t kBandLength[TC_V7_BAND_COUNT] = { 4u, 6u, 6u, 8u, 39u };

static uint32_t band_for_scan_pos(uint8_t scan_pos)
{
    if (scan_pos <= 4u) { return 0u; }
    if (scan_pos <= 10u) { return 1u; }
    if (scan_pos <= 16u) { return 2u; }
    if (scan_pos <= 24u) { return 3u; }
    return 4u;
}

void tc_v7_band_bucket_init(tc_v7_band_bucket* bucket, uint32_t block_index)
{
    if (bucket == NULL) { return; }
    memset(bucket, 0, sizeof(*bucket));
    bucket->block_index = block_index;
    for (uint32_t band = 0u; band < TC_V7_BAND_COUNT; ++band) {
        bucket->last_scan_pos[band] = (uint8_t)(kBandStart[band] - 1u);
    }
}

int32_t tc_v7_band_bucket_push(tc_v7_band_bucket* bucket, uint8_t scan_pos,
                               uint32_t level_m, tc_v7_band_token_sink sink,
                               void* opaque)
{
    if (bucket == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a band bucket is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (scan_pos == 0u || scan_pos > 63u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a AC scan position %u", (unsigned)scan_pos);
        return TC_ERR_INVALID_ARGUMENT;
    }
    const uint32_t band = band_for_scan_pos(scan_pos);
    if (bucket->pair_count[band] == UINT16_MAX) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a band pair count");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    uint32_t local_run = (uint32_t)scan_pos - (uint32_t)bucket->last_scan_pos[band] - 1u;
    tc_v7_band_token token;
    token.block_index = bucket->block_index;
    token.band_index = (uint8_t)band;
    token.scan_pos = scan_pos;
    token.local_run = (uint8_t)local_run;
    token.level_m = level_m;
    bucket->pair_count[band]++;
    bucket->last_scan_pos[band] = scan_pos;
    bucket->local_run_sum[band] += (uint64_t)local_run;
    bucket->level_sum[band] += (uint64_t)level_m;
    if (sink != NULL) { sink(&token, opaque); }
    return TC_OK;
}

void tc_v7_band_stats_reset(tc_v7_band_stats* stats)
{
    if (stats != NULL) { memset(stats, 0, sizeof(*stats)); }
}

int32_t tc_v7_band_stats_add_block(tc_v7_band_stats* stats,
                                   const tc_v7_band_bucket* bucket)
{
    if (stats == NULL || bucket == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a band stats/bucket is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t band = 0u; band < TC_V7_BAND_COUNT; ++band) {
        if (UINT64_MAX - stats->pair_count[band] < (uint64_t)bucket->pair_count[band] ||
            UINT64_MAX - stats->active_block_count[band] <
                (bucket->pair_count[band] != 0u ? UINT64_C(1) : UINT64_C(0)) ||
            UINT64_MAX - stats->local_run_sum[band] < bucket->local_run_sum[band] ||
            UINT64_MAX - stats->level_sum[band] < bucket->level_sum[band]) {
            tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a band stats overflow");
            return TC_ERR_LIMIT_EXCEEDED;
        }
        stats->pair_count[band] += (uint64_t)bucket->pair_count[band];
        if (bucket->pair_count[band] != 0u) { stats->active_block_count[band]++; }
        stats->local_run_sum[band] += bucket->local_run_sum[band];
        stats->level_sum[band] += bucket->level_sum[band];
    }
    return TC_OK;
}

static int32_t partition_validate(uint32_t block_count,
                                  const uint32_t* dc_m,
                                  const uint16_t* pair_count,
                                  const uint8_t* runs,
                                  const uint32_t* level_m,
                                  uint32_t token_count,
                                  tc_v7_band_storage bands[TC_V7_BAND_COUNT])
{
    if (block_count == 0u || dc_m == NULL || pair_count == NULL ||
        (token_count != 0u && (runs == NULL || level_m == NULL)) || bands == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a token partition input is NULL/empty");
        return TC_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t band = 0u; band < TC_V7_BAND_COUNT; ++band) {
        if (bands[band].pair_count == NULL ||
            bands[band].block_capacity < block_count) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a token partition band %u storage",
                         (unsigned)band);
            return TC_ERR_INVALID_ARGUMENT;
        }
        if (band == 0u && bands[band].dc_m == NULL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a B0 token partition dc storage");
            return TC_ERR_INVALID_ARGUMENT;
        }
        if (bands[band].token_capacity != 0u &&
            (bands[band].local_runs == NULL || bands[band].level_m == NULL)) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a token partition arrays band %u",
                         (unsigned)band);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    uint64_t total = 0u;
    for (uint32_t block = 0u; block < block_count; ++block) {
        if (UINT64_MAX - total < (uint64_t)pair_count[block]) {
            tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a token partition count");
            return TC_ERR_LIMIT_EXCEEDED;
        }
        total += (uint64_t)pair_count[block];
    }
    if (total != (uint64_t)token_count) {
        tc_set_error(TC_ERR_MALFORMED, "v7a token partition input count %u != %llu",
                     (unsigned)token_count, (unsigned long long)total);
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

int32_t tc_v7_band_partition_tokens(uint32_t block_count,
                                    const uint32_t* dc_m,
                                    const uint16_t* pair_count,
                                    const uint8_t* runs,
                                    const uint32_t* level_m,
                                    uint32_t token_count,
                                    tc_v7_band_storage bands[TC_V7_BAND_COUNT])
{
    int32_t rc = partition_validate(block_count, dc_m, pair_count, runs, level_m,
                                     token_count, bands);
    if (rc != TC_OK) { return rc; }

    for (uint32_t band = 0u; band < TC_V7_BAND_COUNT; ++band) {
        memset(bands[band].pair_count, 0,
               (size_t)block_count * sizeof(*bands[band].pair_count));
        bands[band].token_count = 0u;
    }

    uint32_t out_index[TC_V7_BAND_COUNT] = { 0u, 0u, 0u, 0u, 0u };
    uint32_t input_index = 0u;
    for (uint32_t block = 0u; block < block_count; ++block) {
        bands[0].dc_m[block] = dc_m[block];
        uint32_t last_scan[TC_V7_BAND_COUNT] = {
            (uint32_t)kBandStart[0] - 1u,
            (uint32_t)kBandStart[1] - 1u,
            (uint32_t)kBandStart[2] - 1u,
            (uint32_t)kBandStart[3] - 1u,
            (uint32_t)kBandStart[4] - 1u
        };
        uint32_t scan_pos = 0u;
        for (uint16_t pair = 0u; pair < pair_count[block]; ++pair, ++input_index) {
            const uint32_t run = runs[input_index];
            const uint32_t level = level_m[input_index];
            if (run > 62u || scan_pos > 63u - run - 1u || level == 0u) {
                tc_set_error(TC_ERR_MALFORMED, "v7a legacy token at %u",
                             (unsigned)input_index);
                return TC_ERR_MALFORMED;
            }
            scan_pos += run + 1u;
            const uint32_t band = band_for_scan_pos((uint8_t)scan_pos);
            const uint32_t local_run = scan_pos - last_scan[band] - 1u;
            if (local_run >= (uint32_t)kBandLength[band] ||
                out_index[band] >= bands[band].token_capacity ||
                bands[band].pair_count[block] == UINT16_MAX) {
                tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a token partition band %u",
                             (unsigned)band);
                return TC_ERR_LIMIT_EXCEEDED;
            }
            const uint32_t dst = out_index[band]++;
            bands[band].local_runs[dst] = (uint8_t)local_run;
            bands[band].level_m[dst] = level;
            bands[band].pair_count[block]++;
            last_scan[band] = scan_pos;
        }
    }
    if (input_index != token_count) {
        tc_set_error(TC_ERR_MALFORMED, "v7a token partition input traversal");
        return TC_ERR_MALFORMED;
    }
    for (uint32_t band = 0u; band < TC_V7_BAND_COUNT; ++band) {
        bands[band].token_count = out_index[band];
    }
    return TC_OK;
}
