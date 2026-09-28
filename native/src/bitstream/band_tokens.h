/* V7-A coefficient band 分桶基础设施。
 * 规范：docs/bitstream_spec_v7.md §6；实现阶段：RD2-03。
 * 该模块只处理已经量化、已经转成 zigzag mask 的系数，不执行 DCT/量化。
 */
#ifndef TOPOS_INTERNAL_BAND_TOKENS_H
#define TOPOS_INTERNAL_BAND_TOKENS_H

#include <stdint.h>

#include "topos_codec.h"

#define TC_V7_BAND_COUNT 5u

typedef struct tc_v7_band_token {
    uint32_t block_index;
    uint8_t band_index;
    uint8_t scan_pos;
    uint8_t local_run;
    uint32_t level_m;
} tc_v7_band_token;

typedef void (*tc_v7_band_token_sink)(const tc_v7_band_token* token, void* opaque);

typedef struct tc_v7_band_bucket {
    uint32_t block_index;
    uint16_t pair_count[TC_V7_BAND_COUNT];
    uint8_t last_scan_pos[TC_V7_BAND_COUNT];
    uint64_t local_run_sum[TC_V7_BAND_COUNT];
    uint64_t level_sum[TC_V7_BAND_COUNT];
} tc_v7_band_bucket;

typedef struct tc_v7_band_stats {
    uint64_t pair_count[TC_V7_BAND_COUNT];
    uint64_t active_block_count[TC_V7_BAND_COUNT];
    uint64_t local_run_sum[TC_V7_BAND_COUNT];
    uint64_t level_sum[TC_V7_BAND_COUNT];
} tc_v7_band_stats;

/* Writable band-local views produced from one legacy token stream.  The
 * caller owns all arrays and supplies their capacities; the partitioner never
 * allocates.  dc_m is used only by B0, while pair_count is required for every
 * band.  local_runs/level_m may be NULL only when token_capacity is zero. */
typedef struct tc_v7_band_storage {
    uint32_t* dc_m;
    uint16_t* pair_count;
    uint8_t* local_runs;
    uint32_t* level_m;
    uint32_t block_capacity;
    uint32_t token_capacity;
    uint32_t token_count;
} tc_v7_band_storage;

/* 初始化一个 block 的 band-local 游程状态。B0 的首个 AC 位置是 1，
 * B1/B2/B3/B4 的首位置分别是 5/11/17/25。DC 不进入 AC token。 */
void tc_v7_band_bucket_init(tc_v7_band_bucket* bucket, uint32_t block_index);

/* 将一个已发现的非零 AC 放入所属 band；scan_pos 必须为 1..63。local_run
 * 按 band 起点重新计算，供 B1..B4 的 active-block local syntax 使用。 */
int32_t tc_v7_band_bucket_push(tc_v7_band_bucket* bucket, uint8_t scan_pos,
                               uint32_t level_m, tc_v7_band_token_sink sink,
                               void* opaque);

void tc_v7_band_stats_reset(tc_v7_band_stats* stats);
int32_t tc_v7_band_stats_add_block(tc_v7_band_stats* stats,
                                   const tc_v7_band_bucket* bucket);

/* Partition one already-quantized legacy token stream into B0..B4.  The
 * input run values are the original block-wide zigzag runs; the output runs
 * are recomputed relative to each band's fixed start.  This is deliberately
 * a single token pass: it performs no DCT, quantization, or entropy decode. */
int32_t tc_v7_band_partition_tokens(uint32_t block_count,
                                    const uint32_t* dc_m,
                                    const uint16_t* pair_count,
                                    const uint8_t* runs,
                                    const uint32_t* level_m,
                                    uint32_t token_count,
                                    tc_v7_band_storage bands[TC_V7_BAND_COUNT]);

#endif /* TOPOS_INTERNAL_BAND_TOKENS_H */
