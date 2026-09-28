/* V7-A band-local scalar segment encoder（enhancement band 自适应选择
 * active-block map 或全块 pair_count map；B1..B4 的 k_dc 字段作为 k_run）。
 * 规范：docs/bitstream_spec_v7.md §6.1；实现阶段：RD2-06。
 */
#ifndef TOPOS_INTERNAL_BAND_CODEC_H
#define TOPOS_INTERNAL_BAND_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "band_tokens.h"
#include "bitio.h"

typedef struct tc_v7_band_input {
    uint32_t block_count;
    const uint32_t* dc_m;       /* B0 必填：[block_count] */
    const uint16_t* pair_count; /* [block_count]，按 band 分组后的 token 数 */
    const uint8_t* local_runs;  /* [token_count]，block raster 顺序 */
    const uint32_t* level_m;    /* [token_count]，mapped AC level */
    uint32_t token_count;       /* 必须等于 Σ pair_count */
} tc_v7_band_input;

/* 精确返回 segment 的 bit 数（包括 4-byte segment syntax header 和尾部 pad
 * 之前的有效位，不包括 byte padding）。输入错误不写 out_bits。 */
/* B0 使用 k_dc 编码 DC；B1..B4 将同一字段解释为 local_run 的 Rice k。 */
int32_t tc_v7_band_bits(uint32_t band_index, uint32_t k_dc, uint32_t k_level,
                        const tc_v7_band_input* input, uint64_t* out_bits);

/* 将一条 band segment 写入一个已 reset 且 byte-aligned 的 bitwriter；函数负责
 * 选择 map、zero-pad、返回实际 payload bytes 和 IEEE CRC。band payload 不含目录
 * descriptor。 */
int32_t tc_v7_band_encode(uint32_t band_index, uint32_t k_dc, uint32_t k_level,
                          const tc_v7_band_input* input, tc_bitwriter* bw,
                          uint32_t* payload_size, uint32_t* payload_crc32);

/* 解码一条 segment，并把它合并进调用方的 q_zig[block_count][64]。调用方须在
 * 首次 B0 decode 前将 q_zig 清零；B0 写 DC+B0，B1..B4 只写各自 AC。 */
int32_t tc_v7_band_decode(const uint8_t* payload, size_t payload_size,
                          uint32_t band_index, uint32_t block_count, uint32_t block_cols,
                          int32_t* q_zig);

#endif /* TOPOS_INTERNAL_BAND_CODEC_H */
