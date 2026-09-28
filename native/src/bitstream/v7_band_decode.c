#include "v7_band_decode.h"

#include <string.h>

#include "../common/checked.h"
#include "../common/crc32.h"
#include "../common/error.h"
#include "band_codec.h"

static int32_t stats_add_u32(uint32_t* value)
{
    if (*value == UINT32_MAX) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a decode stats count");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    ++*value;
    return TC_OK;
}

static int32_t stats_add_bytes(uint64_t* value, uint32_t bytes)
{
    if (UINT64_MAX - *value < (uint64_t)bytes) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a decode stats bytes");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    *value += (uint64_t)bytes;
    return TC_OK;
}

int32_t tc_v7_directory_decode_slice(const tc_v7_directory_view* view,
                                     uint32_t slice_index, uint32_t max_band,
                                     uint32_t block_cols, int32_t* q_zig,
                                     tc_v7_decode_stats* stats)
{
    if (view == NULL || view->frame_header == NULL || q_zig == NULL ||
        max_band >= TC_V7_BAND_COUNT || block_cols == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a slice decode parameters");
        return TC_ERR_INVALID_ARGUMENT;
    }
    tc_v7_slice_desc slice;
    int32_t rc = tc_v7_directory_get_slice(view, slice_index, &slice);
    if (rc != TC_OK) { return rc; }
    if (slice.plane_index >= view->frame_header->plane_count ||
        block_cols != (uint32_t)view->frame_header->plane_block_cols[slice.plane_index]) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a slice block columns");
        return TC_ERR_INVALID_ARGUMENT;
    }
    size_t block_count_size = 0u;
    if (!tc_umul_size((size_t)slice.block_h, (size_t)block_cols, &block_count_size) ||
        block_count_size == 0u || block_count_size > (size_t)UINT32_MAX) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a slice block count");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    uint32_t block_count = (uint32_t)block_count_size;
    size_t q_elems = 0u;
    if (!tc_umul_size(block_count_size, 64u, &q_elems) ||
        q_elems > SIZE_MAX / sizeof(*q_zig)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a q output size");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    memset(q_zig, 0, q_elems * sizeof(*q_zig));

    for (uint32_t band = 0u; band < TC_V7_BAND_COUNT; ++band) {
        tc_v7_segment_desc segment;
        rc = tc_v7_directory_get_segment(view,
                                         slice_index * TC_V7_BAND_COUNT + band,
                                         &segment);
        if (rc != TC_OK) { return rc; }
        int selected = band <= max_band;
        if (stats != NULL) {
            rc = stats_add_u32(&stats->segments_requested);
            if (rc != TC_OK) { return rc; }
        }
        if (selected == 0) {
            if (stats != NULL) {
                rc = stats_add_u32(&stats->segments_skipped);
                if (rc != TC_OK) { return rc; }
                rc = stats_add_u32(&stats->unchecked_segment_count);
                if (rc != TC_OK) { return rc; }
                rc = stats_add_bytes(&stats->bytes_skipped, segment.payload_size);
                if (rc != TC_OK) { return rc; }
            }
            continue;
        }
        if (segment.payload_size == 0u) { continue; }
        const uint8_t* payload = NULL;
        uint32_t payload_size = 0u;
        rc = tc_v7_directory_segment_span(view,
                                          slice_index * TC_V7_BAND_COUNT + band,
                                          &payload, &payload_size);
        if (rc != TC_OK) { return rc; }
        if (stats != NULL) {
            rc = stats_add_u32(&stats->segments_read);
            if (rc != TC_OK) { return rc; }
            rc = stats_add_bytes(&stats->bytes_read, payload_size);
            if (rc != TC_OK) { return rc; }
        }
        if (tc_crc32(payload, (size_t)payload_size) != segment.payload_crc32) {
            tc_set_error(TC_ERR_CHECKSUM_MISMATCH, "v7a segment %u crc",
                         slice_index * TC_V7_BAND_COUNT + band);
            return TC_ERR_CHECKSUM_MISMATCH;
        }
        rc = tc_v7_band_decode(payload, (size_t)payload_size, band, block_count,
                               block_cols, q_zig);
        if (rc != TC_OK) { return rc; }
    }
    return TC_OK;
}
