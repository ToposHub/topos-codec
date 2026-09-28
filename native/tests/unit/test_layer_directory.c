/* V7-A TPLD 目录：checked 边界、CRC、身份映射和旧 reader 拒绝语义。 */
#include "bitstream/layer_directory.h"
#include "bitstream/packet.h"

#include <stdlib.h>
#include <string.h>

#include "common/crc32.h"
#include "common/endian.h"
#include "mini_test.h"
#include "packet_synth.h"

#define TEST_SLICE_COUNT 3u
#define TEST_PLANE_COUNT 3u
#define TEST_SEGMENT_COUNT (TEST_SLICE_COUNT * TC_V7_BAND_COUNT)
#define TEST_DIRECTORY_SIZE \
    (TC_V7_TPLD_FIXED_SIZE + TC_V7_LAYER_DESC_SIZE + \
     (TEST_SLICE_COUNT * TC_V7_SLICE_DESC_SIZE) + \
     (TEST_SEGMENT_COUNT * TC_V7_SEGMENT_DESC_SIZE))
#define TEST_PAYLOAD_OFFSET (TC_FRAME_HEADER_SIZE + TEST_DIRECTORY_SIZE)
#define TEST_PACKET_SIZE (TEST_PAYLOAD_OFFSET + TEST_SEGMENT_COUNT)

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
    fh.plane_count = TEST_PLANE_COUNT;
    fh.qmatrix_id = 0u;
    fh.slice_count = TEST_SLICE_COUNT;
    fh.frame_packet_size = TEST_PACKET_SIZE;
    fh.color_primaries = 1u;
    fh.color_transfer = 1u;
    fh.color_matrix = 1u;
    fh.entropy_mode = 5u;
    fh.codebook_version = 5u;
    fh.coding_mode = 2u;
    for (uint32_t plane = 0u; plane < TEST_PLANE_COUNT; ++plane) {
        fh.plane_block_rows[plane] = 1u;
    }
    return fh;
}

static void refresh_directory_crc(uint8_t* packet)
{
    uint8_t* directory = packet + TC_FRAME_HEADER_SIZE;
    tc_store_be32(directory + 32u, 0u);
    tc_store_be32(directory + 32u, tc_crc32(directory, TEST_DIRECTORY_SIZE));
}

static void build_packet(uint8_t packet[TEST_PACKET_SIZE])
{
    memset(packet, 0, TEST_PACKET_SIZE);
    uint8_t* directory = packet + TC_FRAME_HEADER_SIZE;
    memcpy(directory, "TPLD", 4u);
    tc_store_be16(directory + 4u, 1u);
    tc_store_be16(directory + 6u, 0u);
    tc_store_be16(directory + 12u, 1u);
    tc_store_be16(directory + 14u, TEST_SLICE_COUNT);
    directory[16] = TC_V7_BAND_COUNT;
    directory[17] = TEST_PLANE_COUNT;
    tc_store_be16(directory + 18u, TC_V7_LAYER_DESC_SIZE);
    tc_store_be16(directory + 20u, TC_V7_SLICE_DESC_SIZE);
    tc_store_be16(directory + 22u, TC_V7_SEGMENT_DESC_SIZE);
    tc_store_be32(directory + 24u, TEST_DIRECTORY_SIZE);
    tc_store_be32(directory + 28u, TEST_PAYLOAD_OFFSET);

    uint8_t* layer = directory + TC_V7_TPLD_FIXED_SIZE;
    layer[0] = 0u;
    layer[1] = 0u;
    tc_store_be32(layer + 8u, TEST_SLICE_COUNT);
    tc_store_be32(layer + 16u, TEST_SEGMENT_COUNT);
    tc_store_be16(layer + 20u, 1u);

    uint8_t* slices = layer + TC_V7_LAYER_DESC_SIZE;
    for (uint32_t i = 0u; i < TEST_SLICE_COUNT; ++i) {
        uint8_t* slice = slices + i * TC_V7_SLICE_DESC_SIZE;
        tc_store_be16(slice + 0u, (uint16_t)i);
        tc_store_be32(slice + 4u, i * TC_V7_BAND_COUNT);
        tc_store_be32(slice + 8u, TC_V7_BAND_COUNT);
        tc_store_be32(slice + 12u, 0u);
        tc_store_be16(slice + 16u, 1u);
        slice[18] = (uint8_t)i;
        slice[19] = TC_V7_BAND_COUNT;
    }

    uint8_t* segments = slices + TEST_SLICE_COUNT * TC_V7_SLICE_DESC_SIZE;
    for (uint32_t i = 0u; i < TEST_SEGMENT_COUNT; ++i) {
        uint8_t* segment = segments + i * TC_V7_SEGMENT_DESC_SIZE;
        uint32_t slice = i / TC_V7_BAND_COUNT;
        uint32_t band = i % TC_V7_BAND_COUNT;
        uint32_t payload_offset = TEST_PAYLOAD_OFFSET + i;
        packet[payload_offset] = (uint8_t)(i + 1u);
        tc_store_be32(segment + 0u, payload_offset);
        tc_store_be32(segment + 4u, 1u);
        tc_store_be32(segment + 8u, tc_crc32(packet + payload_offset, 1u));
        tc_store_be16(segment + 12u, (uint16_t)slice);
        segment[14] = (uint8_t)slice;
        segment[15] = (uint8_t)band;
    }
    refresh_directory_crc(packet);
}

static void check_old_reader_rejects_v7(void)
{
    uint8_t* old_packet = NULL;
    size_t old_size = 0u;
    MT_CHECK_EQ_I64(packet_synth_build(packet_synth_cfg_at(PACKET_SYNTH_CFG_TINY),
                                       &old_packet, &old_size), TC_OK);
    if (old_packet != NULL) {
        old_packet[6] = 7u;
        tc_store_be32(old_packet + 49u, tc_crc32(old_packet, 49u));
        topos_packet_view view;
        MT_CHECK_EQ_I64(tc_packet_parse_structure(old_packet, old_size, &view),
                        TC_ERR_UNSUPPORTED_VERSION);
        free(old_packet);
    }
}

static void check_writer_roundtrip(void)
{
    topos_frame_header fh = test_header();
    tc_v7_slice_geometry slices[TEST_SLICE_COUNT];
    for (uint32_t i = 0u; i < TEST_SLICE_COUNT; ++i) {
        slices[i].block_y0 = 0u;
        slices[i].block_h = 1u;
        slices[i].plane_index = (uint8_t)i;
    }

    uint8_t payload[TEST_SEGMENT_COUNT];
    tc_v7_segment_input segments[TEST_SEGMENT_COUNT];
    for (uint32_t i = 0u; i < TEST_SEGMENT_COUNT; ++i) {
        payload[i] = (uint8_t)(0xA0u + i);
        segments[i].data = &payload[i];
        segments[i].size = 1u;
    }

    size_t needed = 0u;
    MT_CHECK_EQ_I64(tc_v7_directory_write(&fh, slices, segments, NULL, 0u, &needed),
                    TC_ERR_BUFFER_TOO_SMALL);
    MT_CHECK_EQ_U64(needed, TEST_PACKET_SIZE);

    uint8_t* packet = (uint8_t*)malloc(needed);
    MT_CHECK(packet != NULL);
    if (packet == NULL) { return; }
    MT_CHECK_EQ_I64(tc_v7_directory_write(&fh, slices, segments, packet, needed, &needed),
                    TC_OK);
    MT_CHECK_EQ_U64(needed, TEST_PACKET_SIZE);

    topos_frame_header decoded;
    MT_CHECK_EQ_I64(tc_v7_packet_header_decode(packet, needed, &decoded), TC_OK);
    MT_CHECK_EQ_U64(decoded.version_major, 7u);
    MT_CHECK_EQ_U64(decoded.frame_packet_size, needed);
    MT_CHECK_EQ_U64(decoded.plane_block_rows[2], 1u);

    tc_v7_directory_view view;
    MT_CHECK_EQ_I64(tc_v7_directory_parse(packet, needed, &decoded, &view), TC_OK);
    MT_CHECK_EQ_U64(view.payload_offset, TEST_PAYLOAD_OFFSET);
    for (uint32_t i = 0u; i < TEST_SEGMENT_COUNT; ++i) {
        tc_v7_segment_desc segment;
        MT_CHECK_EQ_I64(tc_v7_directory_get_segment(&view, i, &segment), TC_OK);
        MT_CHECK_EQ_U64(segment.payload_size, 1u);
        const uint8_t* span = NULL;
        uint32_t span_size = 0u;
        MT_CHECK_EQ_I64(tc_v7_directory_segment_span(&view, i, &span, &span_size), TC_OK);
        MT_CHECK_EQ_U64(span_size, 1u);
        MT_CHECK(span != NULL && *span == payload[i]);
    }

    /* The legacy packet parser must not reinterpret a valid V7 packet as V1-V6. */
    topos_packet_view old_view;
    MT_CHECK_EQ_I64(tc_packet_parse_structure(packet, needed, &old_view),
                    TC_ERR_UNSUPPORTED_VERSION);

    /* A short destination is a non-mutating size error, not a partial packet. */
    uint8_t short_packet[TEST_PACKET_SIZE];
    memset(short_packet, 0xCC, sizeof(short_packet));
    MT_CHECK_EQ_I64(tc_v7_directory_write(&fh, slices, segments, short_packet,
                                           needed - 1u, &needed),
                    TC_ERR_BUFFER_TOO_SMALL);
    MT_CHECK(short_packet[0] == 0xCCu);

    free(packet);
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
    uint8_t packet[TEST_PACKET_SIZE];
    build_packet(packet);
    topos_frame_header fh = test_header();

    tc_v7_directory_view view;
    MT_CHECK_EQ_I64(tc_v7_directory_parse(packet, sizeof(packet), &fh, &view), TC_OK);
    MT_CHECK_EQ_U64(view.directory_size, TEST_DIRECTORY_SIZE);
    MT_CHECK_EQ_U64(view.segment_count, TEST_SEGMENT_COUNT);

    tc_v7_layer_desc layer;
    MT_CHECK_EQ_I64(tc_v7_directory_get_layer(&view, 0u, &layer), TC_OK);
    MT_CHECK_EQ_U64(layer.segment_count, TEST_SEGMENT_COUNT);
    tc_v7_slice_desc slice;
    MT_CHECK_EQ_I64(tc_v7_directory_get_slice(&view, 2u, &slice), TC_OK);
    MT_CHECK_EQ_U64(slice.plane_index, 2u);
    tc_v7_segment_desc segment;
    MT_CHECK_EQ_I64(tc_v7_directory_get_segment(&view, 11u, &segment), TC_OK);
    MT_CHECK_EQ_U64(segment.slice_index, 2u);
    MT_CHECK_EQ_U64(segment.band_index, 1u);
    const uint8_t* span = NULL;
    uint32_t span_size = 0u;
    MT_CHECK_EQ_I64(tc_v7_directory_segment_span(&view, 11u, &span, &span_size), TC_OK);
    MT_CHECK_EQ_U64(span_size, 1u);
    MT_CHECK(span != NULL && *span == 12u);

    /* 任意截断不崩溃，且不能以成功返回。 */
    for (size_t cut = 0u; cut < sizeof(packet); ++cut) {
        MT_CHECK(tc_v7_directory_parse(packet, cut, &fh, &view) < 0);
    }

    /* 目录 CRC 错误必须在结构/segment 之前被拒绝。 */
    {
        uint8_t bad[TEST_PACKET_SIZE];
        memcpy(bad, packet, sizeof(bad));
        bad[TC_FRAME_HEADER_SIZE + 32u] ^= 1u;
        MT_CHECK_EQ_I64(tc_v7_directory_parse(bad, sizeof(bad), &fh, &view),
                        TC_ERR_CHECKSUM_MISMATCH);
    }

    /* 未知 fixed flag、layer reserved 和 segment reserved 均 fail closed。 */
    {
        uint8_t bad[TEST_PACKET_SIZE];
        memcpy(bad, packet, sizeof(bad));
        bad[TC_FRAME_HEADER_SIZE + 8u] = 1u;
        refresh_directory_crc(bad);
        MT_CHECK_EQ_I64(tc_v7_directory_parse(bad, sizeof(bad), &fh, &view),
                        TC_ERR_UNSUPPORTED_VERSION);

        memcpy(bad, packet, sizeof(bad));
        bad[TC_FRAME_HEADER_SIZE + TC_V7_TPLD_FIXED_SIZE + 22u] = 1u;
        refresh_directory_crc(bad);
        MT_CHECK_EQ_I64(tc_v7_directory_parse(bad, sizeof(bad), &fh, &view),
                        TC_ERR_UNSUPPORTED_VERSION);

        memcpy(bad, packet, sizeof(bad));
        size_t seg0 = TC_FRAME_HEADER_SIZE + TC_V7_TPLD_FIXED_SIZE +
                      TC_V7_LAYER_DESC_SIZE + TEST_SLICE_COUNT * TC_V7_SLICE_DESC_SIZE;
        bad[seg0 + 18u] = 1u;
        refresh_directory_crc(bad);
        MT_CHECK_EQ_I64(tc_v7_directory_parse(bad, sizeof(bad), &fh, &view),
                        TC_ERR_UNSUPPORTED_VERSION);
    }

    /* 重叠、越过 packet 和恶意目录上限不得被接受。 */
    {
        uint8_t bad[TEST_PACKET_SIZE];
        size_t seg0 = TC_FRAME_HEADER_SIZE + TC_V7_TPLD_FIXED_SIZE +
                      TC_V7_LAYER_DESC_SIZE + TEST_SLICE_COUNT * TC_V7_SLICE_DESC_SIZE;
        memcpy(bad, packet, sizeof(bad));
        tc_store_be32(bad + seg0 + TC_V7_SEGMENT_DESC_SIZE, TEST_PAYLOAD_OFFSET);
        refresh_directory_crc(bad);
        MT_CHECK_EQ_I64(tc_v7_directory_parse(bad, sizeof(bad), &fh, &view),
                        TC_ERR_MALFORMED);

        memcpy(bad, packet, sizeof(bad));
        tc_store_be32(bad + seg0 + (TEST_SEGMENT_COUNT - 1u) * TC_V7_SEGMENT_DESC_SIZE,
                      (uint32_t)(TEST_PACKET_SIZE - 1u));
        tc_store_be32(bad + seg0 + (TEST_SEGMENT_COUNT - 1u) * TC_V7_SEGMENT_DESC_SIZE + 4u,
                      2u);
        refresh_directory_crc(bad);
        MT_CHECK_EQ_I64(tc_v7_directory_parse(bad, sizeof(bad), &fh, &view),
                        TC_ERR_MALFORMED);

        memcpy(bad, packet, sizeof(bad));
        tc_store_be32(bad + TC_FRAME_HEADER_SIZE + 24u, 0xFFFFFFFFu);
        refresh_directory_crc(bad);
        MT_CHECK_EQ_I64(tc_v7_directory_parse(bad, sizeof(bad), &fh, &view),
                        TC_ERR_LIMIT_EXCEEDED);
    }

    check_old_reader_rejects_v7();
    check_writer_roundtrip();
    return MT_MAIN_RETURN();
}
