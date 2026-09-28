/* V7-A 目录驱动的按 band 解码入口。
 * 规范：docs/bitstream_spec_v7.md §7；实现阶段：RD2-05。
 */
#ifndef TOPOS_INTERNAL_V7_BAND_DECODE_H
#define TOPOS_INTERNAL_V7_BAND_DECODE_H

#include <stddef.h>
#include <stdint.h>

#include "layer_directory.h"

typedef struct tc_v7_decode_stats {
    uint32_t segments_requested;
    uint32_t segments_read;
    uint32_t segments_skipped;
    uint32_t unchecked_segment_count;
    uint64_t bytes_read;
    uint64_t bytes_skipped;
} tc_v7_decode_stats;

/* max_band=0..4 分别表示 1/8、1/4、1/3、1/2、full 所需的最高 band。
 * q_zig 必须容纳该 slice 的 block_count×64 个 int32，并在函数返回时合并
 * 已请求 bands；函数自身先清零 q_zig，保证 B0 DC 预测从干净 slice 开始。 */
int32_t tc_v7_directory_decode_slice(const tc_v7_directory_view* view,
                                     uint32_t slice_index, uint32_t max_band,
                                     uint32_t block_cols, int32_t* q_zig,
                                     tc_v7_decode_stats* stats);

#endif /* TOPOS_INTERNAL_V7_BAND_DECODE_H */
