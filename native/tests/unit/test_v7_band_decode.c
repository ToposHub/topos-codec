/* RD2-05：V7-A 目录驱动的选择性 band 解码与“跳过即不读”契约。 */
#include "bitstream/band_codec.h"
#include "bitstream/layer_directory.h"
#include "bitstream/v7_band_decode.h"

#include <stdlib.h>
#include <string.h>

#include "common/crc32.h"
#include "common/endian.h"
#include "mini_test.h"

#define TEST_SLICE_COUNT 3u
#define TEST_SEGMENT_COUNT (TEST_SLICE_COUNT * TC_V7_BAND_COUNT)
#define TEST_DIRECTORY_SIZE 432u
#define TEST_PAYLOAD_OFFSET (TC_FRAME_HEADER_SIZE + TEST_DIRECTORY_SIZE)

static uint32_t test_directory_crc(const uint8_t* directory, uint32_t size)
{
    static const uint8_t zeros[4] = { 0u, 0u, 0u, 0u };
    uint32_t crc = tc_crc32_update(0u, directory, 32u);
    crc = tc_crc32_update(crc, zeros, sizeof(zeros));
    return tc_crc32_update(crc, directory + 36u, (size_t)size - 36u);
}

static int32_t test_encode_band(uint32_t band, uint8_t* out, uint32_t* size,
                                uint32_t* crc)
{
    const uint32_t dc_m = 0u;
    const uint16_t pair_count = 1u;
    const uint8_t local_run = 0u;
    const uint32_t level_m = band + 1u;
    const tc_v7_band_input input = {
        1u,
        band == 0u ? &dc_m : NULL,
        &pair_count,
        &local_run,
        &level_m,
        1u
    };
    tc_bitwriter bw;
    int32_t rc = tc_bitwriter_init(&bw);
    if (rc != TC_OK) { return rc; }
    rc = tc_v7_band_encode(band, 0u, 0u, &input, &bw, size, crc);
    if (rc == TC_OK) {
        if (*size > 128u) {
            rc = TC_ERR_LIMIT_EXCEEDED;
        } else {
            memcpy(out, tc_bitwriter_data(&bw), *size);
        }
    }
    tc_bitwriter_free(&bw);
    return rc;
}

static int32_t test_build_packet(uint8_t** packet_out, size_t* packet_size_out,
                                 topos_frame_header* fh_out)
{
    uint8_t payloads[TEST_SEGMENT_COUNT][128];
    uint32_t payload_sizes[TEST_SEGMENT_COUNT];
    uint32_t payload_crcs[TEST_SEGMENT_COUNT];
    size_t payload_total = 0u;
    for (uint32_t i = 0u; i < TEST_SEGMENT_COUNT; ++i) {
        int32_t rc = test_encode_band(i % TC_V7_BAND_COUNT, payloads[i],
                                       &payload_sizes[i], &payload_crcs[i]);
        if (rc != TC_OK) { return rc; }
        payload_total += payload_sizes[i];
    }

    size_t packet_size = (size_t)TEST_PAYLOAD_OFFSET + payload_total;
    uint8_t* packet = (uint8_t*)calloc(1u, packet_size);
    if (packet == NULL) { return TC_ERR_OUT_OF_MEMORY; }

    topos_frame_header fh;
    memset(&fh, 0, sizeof(fh));
    fh.version_major = 7u;
    fh.profile = 3u;
    fh.pixel_format = 0u;
    fh.bit_depth = 10u;
    fh.coded_width = 8u;
    fh.coded_height = 8u;
    fh.visible_width = 8u;
    fh.visible_height = 8u;
    fh.plane_count = 3u;
    fh.qmatrix_id = 0u;
    fh.slice_count = TEST_SLICE_COUNT;
    fh.frame_packet_size = (uint32_t)packet_size;
    fh.entropy_mode = 5u;
    fh.codebook_version = 5u;
    fh.coding_mode = 2u;
    for (uint32_t plane = 0u; plane < fh.plane_count; ++plane) {
        fh.plane_block_cols[plane] = 1u;
        fh.plane_block_rows[plane] = 1u;
    }

    uint8_t* directory = packet + TC_FRAME_HEADER_SIZE;
    memcpy(directory, "TPLD", 4u);
    tc_store_be16(directory + 4u, 1u);
    tc_store_be16(directory + 6u, 0u);
    tc_store_be32(directory + 8u, 0u);
    tc_store_be16(directory + 12u, 1u);
    tc_store_be16(directory + 14u, TEST_SLICE_COUNT);
    directory[16] = TC_V7_BAND_COUNT;
    directory[17] = fh.plane_count;
    tc_store_be16(directory + 18u, TC_V7_LAYER_DESC_SIZE);
    tc_store_be16(directory + 20u, TC_V7_SLICE_DESC_SIZE);
    tc_store_be16(directory + 22u, TC_V7_SEGMENT_DESC_SIZE);
    tc_store_be32(directory + 24u, TEST_DIRECTORY_SIZE);
    tc_store_be32(directory + 28u, TEST_PAYLOAD_OFFSET);
    tc_store_be32(directory + 32u, 0u);

    uint8_t* layer = directory + TC_V7_TPLD_FIXED_SIZE;
    layer[0] = 0u;
    layer[1] = 0u;
    tc_store_be16(layer + 2u, 0u);
    tc_store_be32(layer + 4u, 0u);
    tc_store_be32(layer + 8u, TEST_SLICE_COUNT);
    tc_store_be32(layer + 12u, 0u);
    tc_store_be32(layer + 16u, TEST_SEGMENT_COUNT);
    tc_store_be16(layer + 20u, 1u);
    tc_store_be16(layer + 22u, 0u);

    uint8_t* slices = layer + TC_V7_LAYER_DESC_SIZE;
    for (uint32_t i = 0u; i < TEST_SLICE_COUNT; ++i) {
        uint8_t* slice = slices + (size_t)i * TC_V7_SLICE_DESC_SIZE;
        tc_store_be16(slice + 0u, (uint16_t)i);
        tc_store_be16(slice + 2u, 0u);
        tc_store_be32(slice + 4u, i * TC_V7_BAND_COUNT);
        tc_store_be32(slice + 8u, TC_V7_BAND_COUNT);
        tc_store_be32(slice + 12u, 0u);
        tc_store_be16(slice + 16u, 1u);
        slice[18] = (uint8_t)i;
        slice[19] = TC_V7_BAND_COUNT;
        tc_store_be32(slice + 20u, 0u);
    }

    uint8_t* segments = slices + (size_t)TEST_SLICE_COUNT * TC_V7_SLICE_DESC_SIZE;
    size_t payload_offset = TEST_PAYLOAD_OFFSET;
    for (uint32_t i = 0u; i < TEST_SEGMENT_COUNT; ++i) {
        uint8_t* segment = segments + (size_t)i * TC_V7_SEGMENT_DESC_SIZE;
        tc_store_be32(segment + 0u, (uint32_t)payload_offset);
        tc_store_be32(segment + 4u, payload_sizes[i]);
        tc_store_be32(segment + 8u, payload_crcs[i]);
        tc_store_be16(segment + 12u, (uint16_t)(i / TC_V7_BAND_COUNT));
        segment[14] = (uint8_t)(i / TC_V7_BAND_COUNT);
        segment[15] = (uint8_t)(i % TC_V7_BAND_COUNT);
        tc_store_be16(segment + 16u, 0u);
        tc_store_be16(segment + 18u, 0u);
        memcpy(packet + payload_offset, payloads[i], payload_sizes[i]);
        payload_offset += payload_sizes[i];
    }
    tc_store_be32(directory + 32u, test_directory_crc(directory, TEST_DIRECTORY_SIZE));

    *packet_out = packet;
    *packet_size_out = packet_size;
    *fh_out = fh;
    return TC_OK;
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
    uint8_t* packet = NULL;
    size_t packet_size = 0u;
    topos_frame_header fh;
    MT_CHECK_EQ_I64(test_build_packet(&packet, &packet_size, &fh), TC_OK);
    if (packet == NULL) { return MT_MAIN_RETURN(); }

    tc_v7_directory_view directory;
    MT_CHECK_EQ_I64(tc_v7_directory_parse(packet, packet_size, &fh, &directory), TC_OK);

    int32_t q_zig[64] = { 0 };
    tc_v7_decode_stats stats;
    memset(&stats, 0, sizeof(stats));
    MT_CHECK_EQ_I64(tc_v7_directory_decode_slice(&directory, 0u, 1u, 1u,
                                                 q_zig, &stats), TC_OK);
    MT_CHECK_EQ_U64(stats.segments_requested, 5u);
    MT_CHECK_EQ_U64(stats.segments_read, 2u);
    MT_CHECK_EQ_U64(stats.segments_skipped, 3u);
    MT_CHECK_EQ_U64(stats.unchecked_segment_count, 3u);
    MT_CHECK_EQ_U64(q_zig[0], 0u);
    MT_CHECK_EQ_I64(q_zig[1], -1);
    MT_CHECK_EQ_I64(q_zig[5], 1);
    MT_CHECK_EQ_U64(q_zig[11], 0u);
    MT_CHECK_EQ_U64(q_zig[25], 0u);

    memset(&stats, 0, sizeof(stats));
    MT_CHECK_EQ_I64(tc_v7_directory_decode_slice(&directory, 0u, 4u, 1u,
                                                 q_zig, &stats), TC_OK);
    MT_CHECK_EQ_U64(stats.segments_requested, 5u);
    MT_CHECK_EQ_U64(stats.segments_read, 5u);
    MT_CHECK_EQ_U64(stats.segments_skipped, 0u);
    MT_CHECK_EQ_U64(stats.unchecked_segment_count, 0u);
    MT_CHECK_EQ_I64(q_zig[11], -2);
    MT_CHECK_EQ_I64(q_zig[17], 2);
    MT_CHECK_EQ_I64(q_zig[25], -3);

    tc_v7_segment_desc segment;
    MT_CHECK_EQ_I64(tc_v7_directory_get_segment(&directory, 2u, &segment), TC_OK);
    packet[segment.payload_offset] ^= 0x01u;
    memset(&stats, 0, sizeof(stats));
    MT_CHECK_EQ_I64(tc_v7_directory_decode_slice(&directory, 0u, 1u, 1u,
                                                 q_zig, &stats), TC_OK);
    MT_CHECK_EQ_I64(tc_v7_directory_decode_slice(&directory, 0u, 4u, 1u,
                                                 q_zig, &stats), TC_ERR_CHECKSUM_MISMATCH);
    packet[segment.payload_offset] ^= 0x01u;

    MT_CHECK_EQ_I64(tc_v7_directory_get_segment(&directory, 1u, &segment), TC_OK);
    packet[segment.payload_offset] ^= 0x01u;
    MT_CHECK_EQ_I64(tc_v7_directory_decode_slice(&directory, 0u, 1u, 1u,
                                                 q_zig, NULL), TC_ERR_CHECKSUM_MISMATCH);

    free(packet);
    return MT_MAIN_RETURN();
}
