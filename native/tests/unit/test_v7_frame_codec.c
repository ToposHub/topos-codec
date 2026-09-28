/* RD2-05：V7-A packet writer + directory-driven pixel reconstruction. */
#include "bitstream/band_codec.h"
#include "bitstream/layer_directory.h"
#include "bitstream/packet.h"
#include "bitstream/v7_frame_codec.h"

#include <stdlib.h>
#include <string.h>

#include "mini_test.h"

#define TEST_SLICES 3u
#define TEST_SEGMENTS (TEST_SLICES * TC_V7_BAND_COUNT)

static topos_frame_header test_header(void)
{
    topos_frame_header fh;
    memset(&fh, 0, sizeof(fh));
    fh.version_major = 7u;
    fh.version_minor = 0u;
    fh.profile = 3u;
    fh.pixel_format = 0u;
    fh.bit_depth = 10u;
    fh.coded_width = 8u;
    fh.coded_height = 8u;
    fh.visible_width = 8u;
    fh.visible_height = 8u;
    fh.plane_count = TEST_SLICES;
    fh.qmatrix_id = 0u;
    fh.qp_base = 0u;
    fh.slice_count = TEST_SLICES;
    fh.color_primaries = 1u;
    fh.color_transfer = 1u;
    fh.color_matrix = 1u;
    fh.entropy_mode = 5u;
    fh.codebook_version = 5u;
    fh.coding_mode = 2u;
    return fh;
}

static int32_t encode_band(uint32_t band, uint8_t* out, uint32_t* size)
{
    const uint32_t dc_m = 0u;
    const uint16_t pairs = band == 0u ? 0u : 1u;
    const uint8_t run = 0u;
    const uint32_t level_m = 1000000u;
    const tc_v7_band_input input = {
        1u, band == 0u ? &dc_m : NULL, &pairs,
        band == 0u ? NULL : &run,
        band == 0u ? NULL : &level_m,
        band == 0u ? 0u : 1u
    };
    tc_bitwriter bw;
    int32_t rc = tc_bitwriter_init(&bw);
    if (rc != TC_OK) { return rc; }
    uint32_t crc = 0u;
    rc = tc_v7_band_encode(band, 0u, 0u, &input, &bw, size, &crc);
    if (rc == TC_OK) { memcpy(out, tc_bitwriter_data(&bw), *size); }
    tc_bitwriter_free(&bw);
    return rc;
}

static int buffers_differ(const uint16_t* a, const uint16_t* b, size_t count)
{
    for (size_t i = 0u; i < count; ++i) {
        if (a[i] != b[i]) { return 1; }
    }
    return 0;
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
    topos_frame_header fh = test_header();
    tc_v7_slice_geometry slices[TEST_SLICES];
    for (uint32_t i = 0u; i < TEST_SLICES; ++i) {
        slices[i].block_y0 = 0u;
        slices[i].block_h = 1u;
        slices[i].plane_index = (uint8_t)i;
    }

    uint8_t payloads[TEST_SEGMENTS][128];
    tc_v7_segment_input segments[TEST_SEGMENTS];
    for (uint32_t i = 0u; i < TEST_SEGMENTS; ++i) {
        uint32_t size = 0u;
        MT_CHECK_EQ_I64(encode_band(i % TC_V7_BAND_COUNT, payloads[i], &size), TC_OK);
        segments[i].data = payloads[i];
        segments[i].size = size;
    }

    size_t packet_size = 0u;
    MT_CHECK_EQ_I64(tc_v7_directory_write(&fh, slices, segments, NULL, 0u,
                                           &packet_size), TC_ERR_BUFFER_TOO_SMALL);
    uint8_t* packet = (uint8_t*)malloc(packet_size);
    MT_CHECK(packet != NULL);
    if (packet == NULL) { return MT_MAIN_RETURN(); }
    MT_CHECK_EQ_I64(tc_v7_directory_write(&fh, slices, segments, packet, packet_size,
                                           &packet_size), TC_OK);

    uint16_t low_y[64] = { 0u }, low_u[64] = { 0u }, low_v[64] = { 0u };
    uint16_t full_y[64] = { 0u }, full_u[64] = { 0u }, full_v[64] = { 0u };
    uint16_t* low[TC_FRAME_MAX_PLANES] = { low_y, low_u, low_v, NULL };
    uint16_t* full[TC_FRAME_MAX_PLANES] = { full_y, full_u, full_v, NULL };
    size_t strides[TC_FRAME_MAX_PLANES] = { 8u, 4u, 4u, 0u };
    topos_frame_output low_info, full_info;
    tc_v7_decode_stats low_stats, full_stats;

    MT_CHECK_EQ_I64(tc_v7_frame_decode(packet, packet_size, 0u, low, strides,
                                        &low_info, &low_stats), TC_OK);
    MT_CHECK_EQ_U64(low_info.slice_count, TEST_SLICES);
    MT_CHECK_EQ_U64(low_stats.segments_read, TEST_SLICES);
    MT_CHECK_EQ_U64(low_stats.segments_skipped, TEST_SLICES * 4u);
    MT_CHECK(low_stats.bytes_read != 0u && low_stats.bytes_skipped != 0u);

    MT_CHECK_EQ_I64(tc_v7_frame_decode(packet, packet_size, 4u, full, strides,
                                        &full_info, &full_stats), TC_OK);
    MT_CHECK_EQ_U64(full_stats.segments_read, TEST_SEGMENTS);
    MT_CHECK_EQ_U64(full_stats.segments_skipped, 0u);
    MT_CHECK(buffers_differ(low_y, full_y, 64u) ||
             buffers_differ(low_u, full_u, 64u) ||
             buffers_differ(low_v, full_v, 64u));

    /* Corruption in an unrequested band is not touched by B0-only preview;
     * full decode still verifies it and fails closed. */
    {
        topos_frame_header decoded;
        tc_v7_directory_view directory;
        MT_CHECK_EQ_I64(tc_v7_packet_header_decode(packet, packet_size, &decoded), TC_OK);
        MT_CHECK_EQ_I64(tc_v7_directory_parse(packet, packet_size, &decoded, &directory), TC_OK);
        tc_v7_segment_desc segment;
        MT_CHECK_EQ_I64(tc_v7_directory_get_segment(&directory, 1u, &segment), TC_OK);
        uint8_t* corrupted = (uint8_t*)malloc(packet_size);
        MT_CHECK(corrupted != NULL);
        if (corrupted != NULL) {
            memcpy(corrupted, packet, packet_size);
            corrupted[segment.payload_offset] ^= 1u;
            MT_CHECK_EQ_I64(tc_v7_frame_decode(corrupted, packet_size, 0u, low,
                                               strides, &low_info, NULL), TC_OK);
            MT_CHECK_EQ_I64(tc_v7_frame_decode(corrupted, packet_size, 4u, full,
                                               strides, &full_info, NULL),
                            TC_ERR_CHECKSUM_MISMATCH);
            free(corrupted);
        }
    }

    /* Query mode still validates the packet and directory but does not require
     * caller output planes. */
    topos_frame_output query;
    MT_CHECK_EQ_I64(tc_v7_frame_decode(packet, packet_size, 2u, NULL, NULL,
                                        &query, NULL), TC_OK);
    MT_CHECK_EQ_U64(query.visible_width, 8u);
    MT_CHECK_EQ_U64(query.visible_height, 8u);

    /* A legacy packet parser must reject the complete V7 packet before any
     * attempt to interpret TPLD as old slice headers. */
    topos_packet_view legacy;
    MT_CHECK_EQ_I64(tc_packet_parse_structure(packet, packet_size, &legacy),
                    TC_ERR_UNSUPPORTED_VERSION);

    free(packet);
    return MT_MAIN_RETURN();
}
