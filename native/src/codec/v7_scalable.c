#include "v7_scalable.h"

#include <string.h>

#include "base_encoder.h"
#include "base_residual.h"
#include "codec.h"
#include "deblock.h"
#include "../bitstream/frame_header.h"
#include "../common/alloc.h"
#include "../common/checked.h"
#include "../common/crc32.h"
#include "../common/endian.h"
#include "../common/error.h"
#include "../common/tpool.h"
#include "../entropy/rice.h"
#include "../transform/base_scale.h"
#include "../transform/pyramid_transform.h"
#include "../transform/quant.h"
#include "../bitstream/v7_frame_codec.h"

#define TC_V7B_DIRECTORY_FLAGS 1u
#define TC_V7B_DIRECTORY_FIXED_SIZE 36u
#define TC_V7B_DESCRIPTOR_SIZE 24u
#define TC_V7B_SEGMENT_DESCRIPTOR_SIZE 20u
#define TC_V7B_RESIDUAL_MAGIC "RSD1"

int tc_v7b_packet_is(const uint8_t* packet, size_t packet_size)
{
    const uint16_t directory_minor = packet != NULL &&
        packet_size >= (size_t)TC_FRAME_HEADER_SIZE + 8u
        ? tc_load_be16(packet + TC_FRAME_HEADER_SIZE + 6u) : 0u;
    return packet != NULL && packet_size >= (size_t)TC_FRAME_HEADER_SIZE + 8u &&
           memcmp(packet, TC_FRAME_MAGIC, 4u) == 0 && packet[6] == 7u &&
           memcmp(packet + TC_FRAME_HEADER_SIZE, "TPLD", 4u) == 0 &&
           (directory_minor == TC_V7B_DIRECTORY_VERSION_MINOR_REFERENCE ||
            directory_minor == TC_V7B_DIRECTORY_VERSION_MINOR_QUANTIZED ||
            directory_minor == TC_V7B_DIRECTORY_VERSION_MINOR_TRANSFORM_EXPERIMENTAL ||
            directory_minor == TC_V7B_DIRECTORY_VERSION_MINOR_PYRAMID);
}

typedef struct tc_v7b_directory_view {
    const uint8_t* packet;
    size_t packet_size;
    const uint8_t* directory;
    uint32_t directory_size;
    uint32_t payload_offset;
    uint16_t directory_version_minor;
    uint16_t slice_count;
    uint8_t plane_count;
    uint32_t segment_count;
    const uint8_t* segment_descriptors;
} tc_v7b_directory_view;

typedef struct tc_v7b_segment {
    uint32_t payload_offset;
    uint32_t payload_size;
    uint32_t payload_crc32;
    uint16_t slice_index;
    uint8_t plane_index;
    uint16_t segment_flags;
} tc_v7b_segment;

static int32_t v7b_fail(int32_t code, const char* what, unsigned long long value)
{
    tc_set_error(code, "v7b directory %s: %llu", what, value);
    return code;
}

static uint32_t v7b_ceil8(uint32_t value)
{
    return (value + 7u) & ~7u;
}

static int32_t v7b_make_header(const topos_frame_config* config,
                               uint32_t slice_count,
                               topos_frame_header* out)
{
    if (config == NULL || out == NULL || slice_count == 0u ||
        slice_count > TC_MAX_SLICE_COUNT) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b header arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    const uint32_t coded_width = v7b_ceil8(config->visible_width);
    const uint32_t coded_height = v7b_ceil8(config->visible_height);
    if (coded_width > UINT16_MAX || coded_height > UINT16_MAX) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7b coded geometry");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    memset(out, 0, sizeof(*out));
    out->version_major = 7u;
    out->version_minor = 0u;
    out->flags = config->alpha_premultiplied != 0u ? 1u : 0u;
    out->profile = config->profile;
    out->pixel_format = config->pixel_format;
    out->bit_depth = config->bit_depth;
    out->alpha_mode = config->alpha_mode;
    out->alpha_bit_depth = config->alpha_bit_depth;
    out->frame_type = 0u;
    out->gop_id = 0u;
    out->ref_distance = 0u;
    out->coded_width = (uint16_t)coded_width;
    out->coded_height = (uint16_t)coded_height;
    out->visible_width = config->visible_width;
    out->visible_height = config->visible_height;
    out->plane_count = (uint8_t)(config->pixel_format == 3u
                                     ? 4u
                                     : 3u + (config->alpha_mode != 0u ? 1u : 0u));
    out->qmatrix_id = config->qmatrix_id;
    out->qp_base = config->qp_base;
    out->slice_count = (uint16_t)slice_count;
    out->color_range = config->color_range;
    out->color_primaries = config->color_primaries;
    out->color_transfer = config->color_transfer;
    out->color_matrix = config->color_matrix;
    out->chroma_siting = config->chroma_siting;
    out->sar_num = config->sar_num;
    out->sar_den = config->sar_den;
    out->frame_packet_size = TC_FRAME_HEADER_SIZE;
    out->entropy_mode = 5u;
    out->codebook_version = 5u;
    out->coding_mode = 2u;
    int32_t rc = tc_frame_derive_geometry(out);
    if (rc != TC_OK) { return rc; }
    return tc_frame_header_validate_v7a_contract(out);
}

static uint32_t v7b_directory_crc(const uint8_t* directory, uint32_t size)
{
    static const uint8_t zeros[4] = { 0u, 0u, 0u, 0u };
    uint32_t crc = tc_crc32_update(0u, directory, 32u);
    crc = tc_crc32_update(crc, zeros, sizeof(zeros));
    return tc_crc32_update(crc, directory + 36u, (size_t)size - 36u);
}

static int32_t v7b_write_rice_directory(const topos_frame_header* fh,
                                        const topos_frame_header* base_fh,
                                        const uint8_t* base_packet, uint32_t base_size,
                                        const uint8_t* residual[TC_FRAME_MAX_PLANES],
                                        const uint32_t residual_size[TC_FRAME_MAX_PLANES],
                                        uint8_t* out, size_t out_cap, size_t* packet_size)
{
    if (fh == NULL || base_fh == NULL || base_packet == NULL || residual == NULL ||
        residual_size == NULL || packet_size == NULL ||
        (out == NULL && out_cap != 0u)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b rice writer arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    const uint32_t planes = fh->plane_count;
    const uint32_t slice_count = 1u + planes;
    uint32_t slice_bytes = 0u;
    uint32_t segment_bytes = 0u;
    uint32_t directory_size = TC_V7B_DIRECTORY_FIXED_SIZE;
    if (!tc_umul_u32(slice_count, TC_V7B_DESCRIPTOR_SIZE, &slice_bytes) ||
        !tc_umul_u32(slice_count, TC_V7B_SEGMENT_DESCRIPTOR_SIZE, &segment_bytes) ||
        !tc_uadd_u32(directory_size, 2u * TC_V7B_DESCRIPTOR_SIZE, &directory_size) ||
        !tc_uadd_u32(directory_size, slice_bytes, &directory_size) ||
        !tc_uadd_u32(directory_size, segment_bytes, &directory_size) ||
        directory_size > TC_V7_MAX_DIRECTORY_SIZE) {
        return v7b_fail(TC_ERR_LIMIT_EXCEEDED, "rice directory arithmetic", slice_count);
    }
    size_t payload_size = base_size;
    for (uint32_t p = 0u; p < planes; ++p) {
        if (residual[p] == NULL || residual_size[p] == 0u ||
            !tc_uadd_size(payload_size, residual_size[p], &payload_size)) {
            return v7b_fail(TC_ERR_LIMIT_EXCEEDED, "rice payload arithmetic", p);
        }
    }
    size_t needed = (size_t)TC_FRAME_HEADER_SIZE + directory_size;
    if (!tc_uadd_size(needed, payload_size, &needed) ||
        needed > (size_t)TC_MAX_PACKET_SIZE || needed > UINT32_MAX) {
        return v7b_fail(TC_ERR_LIMIT_EXCEEDED, "rice packet size", needed);
    }
    *packet_size = needed;
    if (out == NULL || out_cap < needed) {
        tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "v7b rice writer needs %zu bytes", needed);
        return TC_ERR_BUFFER_TOO_SMALL;
    }
    memset(out + TC_FRAME_HEADER_SIZE, 0u, directory_size);
    uint8_t* directory = out + TC_FRAME_HEADER_SIZE;
    memcpy(directory, "TPLD", 4u);
    tc_store_be16(directory + 4u, 1u);
    tc_store_be16(directory + 6u, TC_V7B_DIRECTORY_VERSION_MINOR);
    tc_store_be32(directory + 8u, TC_V7B_DIRECTORY_FLAGS);
    tc_store_be16(directory + 12u, 2u);
    tc_store_be16(directory + 14u, (uint16_t)slice_count);
    directory[17] = (uint8_t)planes;
    tc_store_be16(directory + 18u, TC_V7B_DESCRIPTOR_SIZE);
    tc_store_be16(directory + 20u, TC_V7B_DESCRIPTOR_SIZE);
    tc_store_be16(directory + 22u, TC_V7B_SEGMENT_DESCRIPTOR_SIZE);
    tc_store_be32(directory + 24u, directory_size);
    tc_store_be32(directory + 28u, TC_FRAME_HEADER_SIZE + directory_size);

    uint8_t* layer = directory + TC_V7B_DIRECTORY_FIXED_SIZE;
    layer[1] = TC_V7B_LAYER_KIND_BASE;
    tc_store_be32(layer + 8u, 1u);
    tc_store_be32(layer + 16u, 1u);
    layer += TC_V7B_DESCRIPTOR_SIZE;
    layer[0] = 1u;
    layer[1] = TC_V7B_LAYER_KIND_RESIDUAL;
    tc_store_be32(layer + 4u, 1u);
    tc_store_be32(layer + 8u, planes);
    tc_store_be32(layer + 12u, 1u);
    tc_store_be32(layer + 16u, planes);

    uint8_t* slices = directory + TC_V7B_DIRECTORY_FIXED_SIZE +
        2u * TC_V7B_DESCRIPTOR_SIZE;
    tc_store_be32(slices + 8u, 1u);
    tc_store_be16(slices + 16u, base_fh->plane_block_rows[0]);
    for (uint32_t p = 0u; p < planes; ++p) {
        uint8_t* raw = slices + (size_t)(1u + p) * TC_V7B_DESCRIPTOR_SIZE;
        tc_store_be16(raw + 0u, (uint16_t)(1u + p));
        tc_store_be32(raw + 4u, 1u + p);
        tc_store_be32(raw + 8u, 1u);
        tc_store_be16(raw + 16u, fh->plane_block_rows[p]);
        raw[18] = (uint8_t)p;
    }
    uint8_t* segments = slices + (size_t)slice_count * TC_V7B_DESCRIPTOR_SIZE;
    size_t cursor = (size_t)TC_FRAME_HEADER_SIZE + directory_size;
    tc_store_be32(segments + 0u, (uint32_t)cursor);
    tc_store_be32(segments + 4u, base_size);
    tc_store_be32(segments + 8u, tc_crc32(base_packet, base_size));
    tc_store_be16(segments + 16u, TC_V7B_SEGMENT_FLAG_BASE_PACKET);
    memmove(out + cursor, base_packet, base_size);
    cursor += base_size;
    for (uint32_t p = 0u; p < planes; ++p) {
        uint8_t* raw = segments + (size_t)(1u + p) * TC_V7B_SEGMENT_DESCRIPTOR_SIZE;
        tc_store_be32(raw + 0u, (uint32_t)cursor);
        tc_store_be32(raw + 4u, residual_size[p]);
        tc_store_be32(raw + 8u, tc_crc32(residual[p], residual_size[p]));
        tc_store_be16(raw + 12u, (uint16_t)(1u + p));
        raw[14] = (uint8_t)p;
        tc_store_be16(raw + 16u, TC_V7B_SEGMENT_FLAG_RESIDUAL);
        memmove(out + cursor, residual[p], residual_size[p]);
        cursor += residual_size[p];
    }
    if (cursor != needed) {
        return v7b_fail(TC_ERR_MALFORMED, "rice payload cursor", cursor);
    }
    tc_store_be32(directory + 32u, v7b_directory_crc(directory, directory_size));
    topos_frame_header header = *fh;
    header.frame_packet_size = (uint32_t)needed;
    return tc_frame_header_encode_v7a(&header, out);
}

static int32_t v7b_get_segment(const tc_v7b_directory_view* view, uint32_t index,
                               tc_v7b_segment* out)
{
    if (view == NULL || out == NULL || index >= view->segment_count) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b segment index");
        return TC_ERR_INVALID_ARGUMENT;
    }
    const uint8_t* raw = view->segment_descriptors +
                         (size_t)index * TC_V7B_SEGMENT_DESCRIPTOR_SIZE;
    out->payload_offset = tc_load_be32(raw + 0u);
    out->payload_size = tc_load_be32(raw + 4u);
    out->payload_crc32 = tc_load_be32(raw + 8u);
    out->slice_index = tc_load_be16(raw + 12u);
    out->plane_index = raw[14];
    out->segment_flags = tc_load_be16(raw + 16u);
    return TC_OK;
}

static void v7b_stats_reset(tc_v7b_decode_stats* stats);

static int32_t v7b_parse_directory_transform(const uint8_t* packet, size_t packet_size,
                                              const topos_frame_header* fh,
                                              tc_v7b_directory_view* out);
static int32_t v7b_parse_directory_pyramid(const uint8_t* packet,
                                           size_t packet_size,
                                           const topos_frame_header* fh,
                                           tc_v7b_directory_view* out);
static int32_t v7b_parse_directory(const uint8_t* packet, size_t packet_size,
                                   const topos_frame_header* fh,
                                   tc_v7b_directory_view* out)
{
    if (packet == NULL || fh == NULL || out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b directory parse arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    int32_t rc = tc_frame_header_validate_v7a_contract(fh);
    if (rc != TC_OK) { return rc; }
    if (packet_size < (size_t)fh->frame_packet_size) {
        return v7b_fail(TC_ERR_TRUNCATED, "packet size", packet_size);
    }
    if (packet_size != (size_t)fh->frame_packet_size ||
        packet_size > (size_t)TC_MAX_PACKET_SIZE) {
        return v7b_fail(TC_ERR_MALFORMED, "packet size", packet_size);
    }
    if (!tc_offset_in_bounds(packet_size, TC_FRAME_HEADER_SIZE,
                             TC_V7B_DIRECTORY_FIXED_SIZE)) {
        return v7b_fail(TC_ERR_TRUNCATED, "fixed directory", packet_size);
    }
    const uint8_t* directory = packet + TC_FRAME_HEADER_SIZE;
    if (memcmp(directory, "TPLD", 4u) != 0) {
        return v7b_fail(TC_ERR_MALFORMED, "magic", 0u);
    }
    const uint16_t directory_version_minor = tc_load_be16(directory + 6u);
    if (tc_load_be16(directory + 4u) != 1u ||
        (directory_version_minor != TC_V7B_DIRECTORY_VERSION_MINOR_REFERENCE &&
         directory_version_minor != TC_V7B_DIRECTORY_VERSION_MINOR_QUANTIZED &&
         directory_version_minor != TC_V7B_DIRECTORY_VERSION_MINOR_TRANSFORM_EXPERIMENTAL &&
         directory_version_minor != TC_V7B_DIRECTORY_VERSION_MINOR_PYRAMID)) {
        return v7b_fail(TC_ERR_UNSUPPORTED_VERSION, "directory version", 0u);
    }
    if (tc_load_be32(directory + 8u) != TC_V7B_DIRECTORY_FLAGS) {
        return v7b_fail(TC_ERR_UNSUPPORTED_VERSION, "directory flags",
                        tc_load_be32(directory + 8u));
    }
    if (directory_version_minor == TC_V7B_DIRECTORY_VERSION_MINOR_TRANSFORM_EXPERIMENTAL) {
        return v7b_parse_directory_transform(packet, packet_size, fh, out);
    }
    if (directory_version_minor == TC_V7B_DIRECTORY_VERSION_MINOR_PYRAMID) {
        return v7b_parse_directory_pyramid(packet, packet_size, fh, out);
    }
    const uint16_t layer_count = tc_load_be16(directory + 12u);
    const uint16_t slice_count = tc_load_be16(directory + 14u);
    if (layer_count != 2u || slice_count != (uint16_t)(1u + fh->plane_count) ||
        directory[16] != 0u || directory[17] != fh->plane_count ||
        tc_load_be16(directory + 18u) != TC_V7B_DESCRIPTOR_SIZE ||
        tc_load_be16(directory + 20u) != TC_V7B_DESCRIPTOR_SIZE ||
        tc_load_be16(directory + 22u) != TC_V7B_SEGMENT_DESCRIPTOR_SIZE) {
        return v7b_fail(TC_ERR_UNSUPPORTED_VERSION, "fixed count/descriptor contract",
                        slice_count);
    }
    uint32_t slice_bytes = 0u;
    uint32_t segment_bytes = 0u;
    uint32_t expected_size = TC_V7B_DIRECTORY_FIXED_SIZE;
    if (!tc_umul_u32((uint32_t)slice_count, TC_V7B_DESCRIPTOR_SIZE, &slice_bytes) ||
        !tc_umul_u32((uint32_t)slice_count, TC_V7B_SEGMENT_DESCRIPTOR_SIZE, &segment_bytes) ||
        !tc_uadd_u32(expected_size, 2u * TC_V7B_DESCRIPTOR_SIZE, &expected_size) ||
        !tc_uadd_u32(expected_size, slice_bytes, &expected_size) ||
        !tc_uadd_u32(expected_size, segment_bytes, &expected_size)) {
        return v7b_fail(TC_ERR_LIMIT_EXCEEDED, "directory arithmetic", slice_count);
    }
    const uint32_t directory_size = tc_load_be32(directory + 24u);
    const uint32_t payload_offset = tc_load_be32(directory + 28u);
    uint32_t directory_end = 0u;
    if (directory_size != expected_size || directory_size > TC_V7_MAX_DIRECTORY_SIZE ||
        !tc_uadd_u32(TC_FRAME_HEADER_SIZE, directory_size, &directory_end) ||
        payload_offset != directory_end ||
        !tc_offset_in_bounds(packet_size, TC_FRAME_HEADER_SIZE, directory_size) ||
        tc_load_be32(directory + 32u) != v7b_directory_crc(directory, directory_size)) {
        return v7b_fail(TC_ERR_MALFORMED, "directory bounds/crc", directory_size);
    }

    const uint8_t* layer0 = directory + TC_V7B_DIRECTORY_FIXED_SIZE;
    const uint8_t* layer1 = layer0 + TC_V7B_DESCRIPTOR_SIZE;
    if (layer0[0] != 0u || layer0[1] != TC_V7B_LAYER_KIND_BASE ||
        tc_load_be16(layer0 + 2u) != 0u || tc_load_be32(layer0 + 4u) != 0u ||
        tc_load_be32(layer0 + 8u) != 1u || tc_load_be32(layer0 + 12u) != 0u ||
        tc_load_be32(layer0 + 16u) != 1u || tc_load_be16(layer0 + 20u) != 0u ||
        tc_load_be16(layer0 + 22u) != 0u ||
        layer1[0] != 1u || layer1[1] != TC_V7B_LAYER_KIND_RESIDUAL ||
        tc_load_be16(layer1 + 2u) != 0u || tc_load_be32(layer1 + 4u) != 1u ||
        tc_load_be32(layer1 + 8u) != fh->plane_count ||
        tc_load_be32(layer1 + 12u) != 1u || tc_load_be32(layer1 + 16u) != fh->plane_count ||
        tc_load_be16(layer1 + 20u) != 0u || tc_load_be16(layer1 + 22u) != 0u) {
        return v7b_fail(TC_ERR_UNSUPPORTED_VERSION, "layer contract", 0u);
    }

    const uint8_t* slices = layer1 + TC_V7B_DESCRIPTOR_SIZE;
    const uint8_t* base_slice = slices;
    if (tc_load_be16(base_slice + 0u) != 0u || tc_load_be16(base_slice + 2u) != 0u ||
        tc_load_be32(base_slice + 4u) != 0u || tc_load_be32(base_slice + 8u) != 1u ||
        tc_load_be32(base_slice + 12u) != 0u || tc_load_be16(base_slice + 16u) == 0u ||
        base_slice[18] != 0u || base_slice[19] != 0u ||
        tc_load_be32(base_slice + 20u) != 0u) {
        return v7b_fail(TC_ERR_MALFORMED, "base slice contract", 0u);
    }
    for (uint32_t p = 0u; p < fh->plane_count; ++p) {
        const uint8_t* raw = slices + (size_t)(1u + p) * TC_V7B_DESCRIPTOR_SIZE;
        if (tc_load_be16(raw + 0u) != (uint16_t)(1u + p) ||
            tc_load_be16(raw + 2u) != 0u || tc_load_be32(raw + 4u) != 1u + p ||
            tc_load_be32(raw + 8u) != 1u || tc_load_be32(raw + 12u) != 0u ||
            tc_load_be16(raw + 16u) != fh->plane_block_rows[p] ||
            raw[18] != (uint8_t)p || raw[19] != 0u ||
            tc_load_be32(raw + 20u) != 0u) {
            return v7b_fail(TC_ERR_MALFORMED, "residual slice contract", p);
        }
    }

    const uint8_t* segment_desc = slices + (size_t)slice_count * TC_V7B_DESCRIPTOR_SIZE;
    size_t cursor = (size_t)payload_offset;
    for (uint32_t i = 0u; i < (uint32_t)slice_count; ++i) {
        tc_v7b_segment segment;
        const uint8_t* raw = segment_desc + (size_t)i * TC_V7B_SEGMENT_DESCRIPTOR_SIZE;
        segment.payload_offset = tc_load_be32(raw + 0u);
        segment.payload_size = tc_load_be32(raw + 4u);
        segment.payload_crc32 = tc_load_be32(raw + 8u);
        segment.slice_index = tc_load_be16(raw + 12u);
        segment.plane_index = raw[14];
        segment.segment_flags = tc_load_be16(raw + 16u);
        const uint32_t expected_plane = i == 0u ? 0u : i - 1u;
        const uint32_t expected_flag = i == 0u ? TC_V7B_SEGMENT_FLAG_BASE_PACKET
                                               : TC_V7B_SEGMENT_FLAG_RESIDUAL;
        if (segment.slice_index != (uint16_t)i || segment.plane_index != expected_plane ||
            segment.segment_flags != expected_flag || segment.payload_size == 0u ||
            (size_t)segment.payload_offset != cursor ||
            !tc_offset_in_bounds(packet_size, segment.payload_offset,
                                 segment.payload_size) ||
            tc_load_be16(raw + 18u) != 0u) {
            return v7b_fail(TC_ERR_MALFORMED, "segment contract", i);
        }
        if (!tc_uadd_size(cursor, (size_t)segment.payload_size, &cursor)) {
            return v7b_fail(TC_ERR_LIMIT_EXCEEDED, "segment cursor", i);
        }
    }
    if (cursor != packet_size) {
        return v7b_fail(TC_ERR_MALFORMED, "payload trailing/gap", cursor);
    }
    out->packet = packet;
    out->packet_size = packet_size;
    out->directory = directory;
    out->directory_size = directory_size;
    out->payload_offset = payload_offset;
    out->directory_version_minor = directory_version_minor;
    out->slice_count = slice_count;
    out->plane_count = fh->plane_count;
    out->segment_count = slice_count;
    out->segment_descriptors = segment_desc;
    return TC_OK;
}

/* Minor 3 has the same fixed descriptor widths as the original directory,
 * but its enhancement packet is one all-plane segment instead of three raw
 * per-plane streams.  Keep this parser separate so minor 1/2 retain their
 * frozen exact-layout validation. */
static int32_t v7b_parse_directory_transform(const uint8_t* packet, size_t packet_size,
                                              const topos_frame_header* fh,
                                              tc_v7b_directory_view* out)
{
    if (packet == NULL || fh == NULL || out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b transform directory arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    int32_t rc = tc_frame_header_validate_v7a_contract(fh);
    if (rc != TC_OK) { return rc; }
    if (packet_size < (size_t)fh->frame_packet_size) {
        return v7b_fail(TC_ERR_TRUNCATED, "transform packet size", packet_size);
    }
    if (packet_size != (size_t)fh->frame_packet_size ||
        packet_size > (size_t)TC_MAX_PACKET_SIZE ||
        !tc_offset_in_bounds(packet_size, TC_FRAME_HEADER_SIZE,
                             TC_V7B_DIRECTORY_FIXED_SIZE)) {
        return v7b_fail(TC_ERR_MALFORMED, "transform packet bounds", packet_size);
    }
    const uint8_t* directory = packet + TC_FRAME_HEADER_SIZE;
    if (memcmp(directory, "TPLD", 4u) != 0 ||
        tc_load_be16(directory + 4u) != 1u ||
        tc_load_be16(directory + 6u) != TC_V7B_DIRECTORY_VERSION_MINOR_TRANSFORM_EXPERIMENTAL ||
        tc_load_be32(directory + 8u) != TC_V7B_DIRECTORY_FLAGS ||
        tc_load_be16(directory + 12u) != 2u ||
        tc_load_be16(directory + 14u) != 2u || fh->slice_count != fh->plane_count ||
        directory[16] != 0u || directory[17] != fh->plane_count ||
        tc_load_be16(directory + 18u) != TC_V7B_DESCRIPTOR_SIZE ||
        tc_load_be16(directory + 20u) != TC_V7B_DESCRIPTOR_SIZE ||
        tc_load_be16(directory + 22u) != TC_V7B_SEGMENT_DESCRIPTOR_SIZE) {
        return v7b_fail(TC_ERR_UNSUPPORTED_VERSION, "transform directory contract", 0u);
    }
    const uint32_t expected_size = TC_V7B_DIRECTORY_FIXED_SIZE +
        2u * TC_V7B_DESCRIPTOR_SIZE + 2u * TC_V7B_DESCRIPTOR_SIZE +
        2u * TC_V7B_SEGMENT_DESCRIPTOR_SIZE;
    const uint32_t directory_size = tc_load_be32(directory + 24u);
    const uint32_t payload_offset = tc_load_be32(directory + 28u);
    uint32_t directory_end = 0u;
    if (directory_size != expected_size || directory_size > TC_V7_MAX_DIRECTORY_SIZE ||
        !tc_uadd_u32(TC_FRAME_HEADER_SIZE, directory_size, &directory_end) ||
        payload_offset != directory_end ||
        !tc_offset_in_bounds(packet_size, TC_FRAME_HEADER_SIZE, directory_size) ||
        tc_load_be32(directory + 32u) != v7b_directory_crc(directory, directory_size)) {
        return v7b_fail(TC_ERR_MALFORMED, "transform directory bounds/crc", directory_size);
    }

    const uint8_t* layer0 = directory + TC_V7B_DIRECTORY_FIXED_SIZE;
    const uint8_t* layer1 = layer0 + TC_V7B_DESCRIPTOR_SIZE;
    if (layer0[0] != 0u || layer0[1] != TC_V7B_LAYER_KIND_BASE ||
        tc_load_be16(layer0 + 2u) != 0u || tc_load_be32(layer0 + 4u) != 0u ||
        tc_load_be32(layer0 + 8u) != 1u || tc_load_be32(layer0 + 12u) != 0u ||
        tc_load_be32(layer0 + 16u) != 1u || tc_load_be16(layer0 + 20u) != 0u ||
        tc_load_be16(layer0 + 22u) != 0u ||
        layer1[0] != 1u || layer1[1] != TC_V7B_LAYER_KIND_RESIDUAL ||
        tc_load_be16(layer1 + 2u) != 0u || tc_load_be32(layer1 + 4u) != 1u ||
        tc_load_be32(layer1 + 8u) != 1u || tc_load_be32(layer1 + 12u) != 1u ||
        tc_load_be32(layer1 + 16u) != 1u || tc_load_be16(layer1 + 20u) != 0u ||
        tc_load_be16(layer1 + 22u) != 0u) {
        return v7b_fail(TC_ERR_UNSUPPORTED_VERSION, "transform layer contract", 0u);
    }

    const uint8_t* slices = layer1 + TC_V7B_DESCRIPTOR_SIZE;
    const uint8_t* base_slice = slices;
    const uint8_t* enhancement_slice = slices + TC_V7B_DESCRIPTOR_SIZE;
    if (tc_load_be16(base_slice + 0u) != 0u || tc_load_be16(base_slice + 2u) != 0u ||
        tc_load_be32(base_slice + 4u) != 0u || tc_load_be32(base_slice + 8u) != 1u ||
        tc_load_be32(base_slice + 12u) != 0u || tc_load_be16(base_slice + 16u) == 0u ||
        base_slice[18] != 0u || base_slice[19] != 0u ||
        tc_load_be32(base_slice + 20u) != 0u ||
        tc_load_be16(enhancement_slice + 0u) != 1u ||
        tc_load_be16(enhancement_slice + 2u) != 0u ||
        tc_load_be32(enhancement_slice + 4u) != 1u ||
        tc_load_be32(enhancement_slice + 8u) != 1u ||
        tc_load_be32(enhancement_slice + 12u) != 0u ||
        tc_load_be16(enhancement_slice + 16u) != fh->plane_block_rows[0] ||
        enhancement_slice[18] != 0u || enhancement_slice[19] != 0u ||
        tc_load_be32(enhancement_slice + 20u) != 0u) {
        return v7b_fail(TC_ERR_MALFORMED, "transform slice contract", 0u);
    }

    const uint8_t* segment_desc = slices + 2u * TC_V7B_DESCRIPTOR_SIZE;
    size_t cursor = (size_t)payload_offset;
    for (uint32_t i = 0u; i < 2u; ++i) {
        const uint8_t* raw = segment_desc + (size_t)i * TC_V7B_SEGMENT_DESCRIPTOR_SIZE;
        const uint16_t expected_flag = i == 0u ? TC_V7B_SEGMENT_FLAG_BASE_PACKET
                                                : TC_V7B_SEGMENT_FLAG_RESIDUAL;
        if (tc_load_be16(raw + 12u) != (uint16_t)i || raw[14] != 0u || raw[15] != 0u ||
            tc_load_be16(raw + 16u) != expected_flag || tc_load_be16(raw + 18u) != 0u ||
            tc_load_be32(raw + 4u) == 0u || (size_t)tc_load_be32(raw + 0u) != cursor ||
            !tc_offset_in_bounds(packet_size, tc_load_be32(raw + 0u),
                                 tc_load_be32(raw + 4u)) ||
            !tc_uadd_size(cursor, (size_t)tc_load_be32(raw + 4u), &cursor)) {
            return v7b_fail(TC_ERR_MALFORMED, "transform segment contract", i);
        }
    }
    if (cursor != packet_size) {
        return v7b_fail(TC_ERR_MALFORMED, "transform payload trailing/gap", cursor);
    }
    out->packet = packet;
    out->packet_size = packet_size;
    out->directory = directory;
    out->directory_size = directory_size;
    out->payload_offset = payload_offset;
    out->directory_version_minor = TC_V7B_DIRECTORY_VERSION_MINOR_TRANSFORM_EXPERIMENTAL;
    out->slice_count = 2u;
    out->plane_count = fh->plane_count;
    out->segment_count = 2u;
    out->segment_descriptors = segment_desc;
    return TC_OK;
}

/* Keep the V7-B base lookup in one place.  The reduced decoder used to call
 * tc_v7b_frame_decode() once to learn the base geometry and again to decode
 * it.  Besides parsing the directory twice, that hashed the complete base
 * packet twice.  A 2K base can be several megabytes, so the duplicate CRC
 * sweep was visible in exactly the preview path V7-B is intended to speed up.
 */
typedef struct tc_v7b_base_view {
    tc_v7b_directory_view directory;
    tc_v7b_segment segment;
    const uint8_t* packet;
    topos_frame_header header;
} tc_v7b_base_view;

static int32_t v7b_prepare_base(const uint8_t* packet, size_t packet_size,
                                const topos_frame_header* outer_fh,
                                uint32_t max_dim, tc_v7b_base_view* out)
{
    if (packet == NULL || outer_fh == NULL || out == NULL || max_dim == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b base preparation arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    int32_t rc = v7b_parse_directory(packet, packet_size, outer_fh, &out->directory);
    if (rc != TC_OK) { return rc; }
    rc = v7b_get_segment(&out->directory, 0u, &out->segment);
    if (rc != TC_OK) { return rc; }
    out->packet = packet + out->segment.payload_offset;
    if (tc_crc32(out->packet, out->segment.payload_size) != out->segment.payload_crc32) {
        tc_set_error(TC_ERR_CHECKSUM_MISMATCH, "v7b base segment crc");
        return TC_ERR_CHECKSUM_MISMATCH;
    }
    rc = tc_frame_header_decode(out->packet, out->segment.payload_size, &out->header);
    if (rc != TC_OK) { return rc; }
    if (out->header.visible_width > max_dim || out->header.visible_height > max_dim ||
        out->header.plane_count != outer_fh->plane_count || out->header.alpha_mode != 0u ||
        out->header.pixel_format != outer_fh->pixel_format ||
        out->header.bit_depth != outer_fh->bit_depth ||
        out->header.chroma_siting != outer_fh->chroma_siting) {
        tc_set_error(TC_ERR_MALFORMED, "v7b base/source contract");
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

static void v7b_record_base_stats(const topos_frame_header* outer_fh,
                                  const tc_v7b_base_view* base,
                                  int base_only, tc_v7b_decode_stats* stats)
{
    if (outer_fh == NULL || base == NULL || stats == NULL) { return; }
    stats->segments_requested = 1u;
    stats->segments_read = 1u;
    stats->bytes_read = base->segment.payload_size;
    stats->base_bytes_read = base->segment.payload_size;
    if (base_only == 0) { return; }
    const uint32_t skipped = base->directory.segment_count - 1u;
    stats->segments_skipped = skipped;
    stats->unchecked_segment_count = skipped;
    for (uint32_t i = 1u; i < base->directory.segment_count; ++i) {
        tc_v7b_segment residual;
        (void)v7b_get_segment(&base->directory, i, &residual);
        stats->bytes_skipped += residual.payload_size;
    }
}

int32_t tc_v7b_read_base_packet(tc_v7b_range_read_fn read_fn, void* read_ctx,
                                uint64_t packet_size, uint8_t* base_out,
                                size_t base_cap, size_t* base_size,
                                topos_frame_header* outer_fh,
                                tc_v7b_decode_stats* stats)
{
    if (read_fn == NULL || packet_size == 0u || packet_size > TC_MAX_PACKET_SIZE ||
        (base_out == NULL && base_cap != 0u)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b base range arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    v7b_stats_reset(stats);
    if (base_size != NULL) { *base_size = 0u; }
    if (packet_size > (uint64_t)SIZE_MAX) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7b base range packet size");
        return TC_ERR_LIMIT_EXCEEDED;
    }

    /* The fixed prefix is enough to authenticate the outer header and learn
     * the bounded directory size.  No payload byte is touched here. */
    if (packet_size < (uint64_t)TC_FRAME_HEADER_SIZE + TC_V7B_DIRECTORY_FIXED_SIZE) {
        tc_set_error(TC_ERR_TRUNCATED, "v7b base range prefix");
        return TC_ERR_TRUNCATED;
    }
    uint8_t fixed[(size_t)TC_FRAME_HEADER_SIZE + TC_V7B_DIRECTORY_FIXED_SIZE];
    int32_t rc = read_fn(read_ctx, 0u, fixed, sizeof(fixed));
    if (rc != TC_OK) { return rc; }

    topos_frame_header parsed_outer;
    rc = tc_v7_packet_header_decode(fixed, (size_t)packet_size, &parsed_outer);
    if (rc != TC_OK) { return rc; }
    const uint8_t* fixed_directory = fixed + TC_FRAME_HEADER_SIZE;
    const uint32_t directory_size = tc_load_be32(fixed_directory + 24u);
    if (directory_size < TC_V7B_DIRECTORY_FIXED_SIZE ||
        directory_size > TC_V7_MAX_DIRECTORY_SIZE) {
        return v7b_fail(TC_ERR_MALFORMED, "base range directory size", directory_size);
    }
    size_t prefix_size = 0u;
    if (!tc_uadd_size((size_t)TC_FRAME_HEADER_SIZE, (size_t)directory_size,
                      &prefix_size)) {
        return v7b_fail(TC_ERR_LIMIT_EXCEEDED, "base range prefix size", directory_size);
    }
    if ((uint64_t)prefix_size > packet_size) {
        return v7b_fail(TC_ERR_TRUNCATED, "base range directory", directory_size);
    }

    uint8_t* prefix = (uint8_t*)tc_alloc(prefix_size);
    if (prefix == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "v7b base range directory %zu", prefix_size);
        return TC_ERR_OUT_OF_MEMORY;
    }
    memcpy(prefix, fixed, sizeof(fixed));
    if (prefix_size > sizeof(fixed)) {
        rc = read_fn(read_ctx, sizeof(fixed), prefix + sizeof(fixed),
                     prefix_size - sizeof(fixed));
    }
    if (rc == TC_OK) {
        tc_v7b_directory_view directory;
        rc = v7b_parse_directory(prefix, (size_t)packet_size, &parsed_outer,
                                 &directory);
        if (rc == TC_OK) {
            if (outer_fh != NULL) { *outer_fh = parsed_outer; }
            tc_v7b_segment base_segment;
            rc = v7b_get_segment(&directory, 0u, &base_segment);
            if (rc == TC_OK) {
                if (base_size != NULL) { *base_size = base_segment.payload_size; }
                if (stats != NULL) {
                    stats->segments_requested = 1u;
                    stats->segments_read = 1u;
                    const uint32_t skipped = directory.segment_count - 1u;
                    stats->segments_skipped = skipped;
                    stats->unchecked_segment_count = skipped;
                    stats->bytes_read = base_segment.payload_size;
                    stats->base_bytes_read = base_segment.payload_size;
                    for (uint32_t i = 1u; i < directory.segment_count; ++i) {
                        tc_v7b_segment residual;
                        if (v7b_get_segment(&directory, i, &residual) == TC_OK) {
                            stats->bytes_skipped += residual.payload_size;
                        }
                    }
                }
                if (base_out == NULL || base_cap < (size_t)base_segment.payload_size) {
                    tc_set_error(TC_ERR_BUFFER_TOO_SMALL,
                                 "v7b base packet needs %u bytes",
                                 (unsigned)base_segment.payload_size);
                    rc = TC_ERR_BUFFER_TOO_SMALL;
                } else {
                    rc = read_fn(read_ctx, base_segment.payload_offset, base_out,
                                 base_segment.payload_size);
                    if (rc == TC_OK &&
                        tc_crc32(base_out, base_segment.payload_size) !=
                            base_segment.payload_crc32) {
                        tc_set_error(TC_ERR_CHECKSUM_MISMATCH,
                                     "v7b base range segment crc");
                        rc = TC_ERR_CHECKSUM_MISMATCH;
                    }
                    if (rc == TC_OK) {
                        topos_frame_header base_fh;
                        rc = tc_frame_header_decode(base_out,
                                                    base_segment.payload_size,
                                                    &base_fh);
                        if (rc == TC_OK &&
                            (base_fh.visible_width > parsed_outer.visible_width ||
                             base_fh.visible_height > parsed_outer.visible_height ||
                             base_fh.plane_count != parsed_outer.plane_count ||
                             base_fh.alpha_mode != 0u ||
                             base_fh.pixel_format != parsed_outer.pixel_format ||
                             base_fh.bit_depth != parsed_outer.bit_depth ||
                             base_fh.chroma_siting != parsed_outer.chroma_siting)) {
                            tc_set_error(TC_ERR_MALFORMED,
                                         "v7b base/source range contract");
                            rc = TC_ERR_MALFORMED;
                        }
                    }
                }
            }
        }
    }
    tc_free(prefix);
    return rc;
}

static int32_t v7b_plane_alloc(uint32_t width, uint32_t height,
                               uint16_t** pixels, size_t* stride)
{
    if (pixels == NULL || stride == NULL || width == 0u || height == 0u) {
        return TC_ERR_INVALID_ARGUMENT;
    }
    size_t count = 0u;
    size_t bytes = 0u;
    if (!tc_umul_size((size_t)width, (size_t)height, &count) ||
        !tc_umul_size(count, sizeof(uint16_t), &bytes)) {
        return TC_ERR_LIMIT_EXCEEDED;
    }
    *pixels = (uint16_t*)tc_alloc(bytes);
    if (*pixels == NULL) { return TC_ERR_OUT_OF_MEMORY; }
    memset(*pixels, 0, bytes);
    *stride = width;
    return TC_OK;
}

static int32_t v7b_validate_planes(const topos_frame_header* fh,
                                   uint16_t* const planes[TC_FRAME_MAX_PLANES],
                                   const size_t strides[TC_FRAME_MAX_PLANES])
{
    if (planes == NULL) { return TC_OK; }
    for (uint32_t p = 0u; p < fh->plane_count; ++p) {
        if (planes[p] == NULL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b output plane %u is NULL", p);
            return TC_ERR_INVALID_ARGUMENT;
        }
        const size_t width = fh->plane_visible_w[p];
        const size_t stride = strides != NULL && strides[p] != 0u
                            ? strides[p] : width;
        if (stride < width) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT,
                         "v7b output stride[%u]=%zu < visible width %zu", p, stride, width);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    return TC_OK;
}

static int32_t v7b_decode_residual(const uint8_t* data, uint32_t size,
                                   int32_t* residual, uint32_t width, uint32_t height,
                                   size_t stride, uint16_t directory_version_minor)
{
    if (data == NULL || size < TC_V7B_RESIDUAL_HEADER_SIZE || residual == NULL ||
        memcmp(data, TC_V7B_RESIDUAL_MAGIC, 4u) != 0 || data[7] != 0u ||
        data[4] > TC_RICE_K_MAX || data[5] > TC_RICE_K_MAX) {
        tc_set_error(TC_ERR_MALFORMED, "v7b residual header");
        return TC_ERR_MALFORMED;
    }
    uint32_t quant_step = 1u;
    if (directory_version_minor == TC_V7B_DIRECTORY_VERSION_MINOR_REFERENCE) {
        if (data[6] != 0u) {
            tc_set_error(TC_ERR_MALFORMED, "v7b reference residual quant step");
            return TC_ERR_MALFORMED;
        }
    } else if (directory_version_minor == TC_V7B_DIRECTORY_VERSION_MINOR_QUANTIZED) {
        if (data[6] == 0u) {
            tc_set_error(TC_ERR_MALFORMED, "v7b quantized residual step");
            return TC_ERR_MALFORMED;
        }
        quant_step = data[6];
    } else {
        tc_set_error(TC_ERR_UNSUPPORTED_VERSION, "v7b residual directory version");
        return TC_ERR_UNSUPPORTED_VERSION;
    }
    size_t count = 0u;
    size_t bytes = 0u;
    if (!tc_umul_size((size_t)width, (size_t)height, &count) ||
        !tc_umul_size(stride, (size_t)height, &bytes)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7b residual geometry");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    (void)bytes;
    memset(residual, 0, bytes * sizeof(int32_t));
    tc_bitreader br;
    tc_bitreader_init(&br, data + TC_V7B_RESIDUAL_HEADER_SIZE,
                      (size_t)size - TC_V7B_RESIDUAL_HEADER_SIZE);
    int32_t rc = tc_base_residual_decode(&br, residual, width, height, stride,
                                         data[4], data[5]);
    if (rc != TC_OK) { return rc; }
    rc = tc_base_residual_scale_in_place(residual, width, height, stride, quant_step);
    if (rc != TC_OK) { return rc; }
    const uint64_t consumed = tc_bitreader_bits_consumed(&br);
    const uint64_t end = br.bit_size;
    for (uint64_t bit = consumed; bit < end; ++bit) {
        const uint8_t value = data[TC_V7B_RESIDUAL_HEADER_SIZE +
                                    (size_t)(bit >> 3u)];
        if ((value & (uint8_t)(1u << (7u - (uint32_t)(bit & 7u)))) != 0u) {
            tc_set_error(TC_ERR_MALFORMED, "v7b residual nonzero padding");
            return TC_ERR_MALFORMED;
        }
    }
    return TC_OK;
}

static void v7b_stats_reset(tc_v7b_decode_stats* stats)
{
    if (stats != NULL) { memset(stats, 0, sizeof(*stats)); }
}

typedef struct v7b_rice_job {
    const topos_frame_config* source_config;
    const topos_frame_input* source_input;
    const topos_frame_header* outer_fh;
    const topos_frame_header* base_fh;
    const uint16_t* base_plane;
    size_t base_stride;
    uint32_t plane;
    uint8_t quant_step;
    int32_t* residual;
    uint8_t* payload;
    uint32_t payload_size;
    int32_t rc;
} v7b_rice_job;

static uint8_t v7b_rice_quant_step(const topos_frame_config* config, uint32_t plane)
{
    const tc_qmatrix_set* qms = config != NULL ? tc_qmatrix_by_id(config->qmatrix_id) : NULL;
    if (qms == NULL) { return 1u; }
    const uint16_t* matrix = plane == 0u ? qms->luma : qms->chroma;
    const uint32_t step = tc_quant_step(matrix[27], config->qp_base);
    return step == 0u ? 1u : (step > 255u ? 255u : (uint8_t)step);
}

static void v7b_encode_rice_plane(void* opaque)
{
    v7b_rice_job* job = (v7b_rice_job*)opaque;
    const uint32_t p = job->plane;
    const uint32_t width = job->outer_fh->plane_visible_w[p];
    const uint32_t height = job->outer_fh->plane_visible_h[p];
    size_t count = 0u;
    size_t bytes = 0u;
    if (!tc_umul_size(width, height, &count) || !tc_umul_size(count, sizeof(int32_t), &bytes)) {
        job->rc = TC_ERR_LIMIT_EXCEEDED;
        return;
    }
    job->residual = (int32_t*)tc_alloc(bytes);
    if (job->residual == NULL) { job->rc = TC_ERR_OUT_OF_MEMORY; return; }
    const size_t source_stride = job->source_input->strides[p] != 0u
        ? job->source_input->strides[p] : (size_t)width;
    uint32_t k_level = 0u;
    uint32_t k_run = 0u;
    job->rc = tc_base_residual_build_quantized_u16_with_params(
        job->source_input->planes[p], width, height, source_stride, job->base_plane,
        job->base_fh->plane_visible_w[p], job->base_fh->plane_visible_h[p], job->base_stride,
        job->residual, width, job->source_config->bit_depth, job->quant_step,
        &k_level, &k_run);
    if (job->rc != TC_OK) { return; }
    tc_bitwriter bw;
    job->rc = tc_bitwriter_init(&bw);
    if (job->rc == TC_OK) {
        job->rc = tc_base_residual_encode_with_params(job->residual, width, height,
                                                       width, &bw, k_level, k_run);
    }
    if (job->rc == TC_OK) { job->rc = tc_bitwriter_flush_zero_pad(&bw); }
    const size_t bitstream_size = job->rc == TC_OK ? tc_bitwriter_byte_size(&bw) : 0u;
    size_t payload_size = 0u;
    if (job->rc == TC_OK &&
        (!tc_uadd_size(TC_V7B_RESIDUAL_HEADER_SIZE, bitstream_size, &payload_size) ||
         payload_size > UINT32_MAX)) {
        job->rc = TC_ERR_LIMIT_EXCEEDED;
    }
    if (job->rc == TC_OK) {
        job->payload = (uint8_t*)tc_alloc(payload_size);
        if (job->payload == NULL) {
            job->rc = TC_ERR_OUT_OF_MEMORY;
        } else {
            memcpy(job->payload, TC_V7B_RESIDUAL_MAGIC, 4u);
            job->payload[4] = (uint8_t)k_level;
            job->payload[5] = (uint8_t)k_run;
            job->payload[6] = job->quant_step;
            job->payload[7] = 0u;
            memcpy(job->payload + TC_V7B_RESIDUAL_HEADER_SIZE,
                   tc_bitwriter_data(&bw), bitstream_size);
            job->payload_size = (uint32_t)payload_size;
        }
    }
    tc_bitwriter_free(&bw);
}

static int32_t v7b_frame_encode_rice(const topos_frame_config* source_config,
                                      const topos_frame_input* source_input,
                                      uint32_t max_dim,
                                      uint8_t* out, size_t out_cap,
                                      topos_frame_stats* stats)
{
    if (source_config == NULL || source_input == NULL ||
        (out == NULL && out_cap != 0u) || max_dim == 0u || max_dim > TC_PLANE_MAX_DIM) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b rice encode arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    topos_frame_config normalized = *source_config;
    if (normalized.profile == 0u) { normalized.profile = 3u; }
    if (normalized.bit_depth == 0u) { normalized.bit_depth = 10u; }
    if (normalized.color_primaries == 0u) { normalized.color_primaries = 1u; }
    if (normalized.color_transfer == 0u) {
        /* pf=3（TRAW）默认 LOG0（与 cfg_to_frame_header 同规则） */
        normalized.color_transfer = normalized.pixel_format == 3u
                                        ? TC_TRANSFER_TRAW_LOG0 : 1u;
    }
    if (normalized.color_matrix == 0u && normalized.pixel_format != 2u &&
        normalized.pixel_format != 3u) { /* TRAW CFA：无矩阵语义，保持 identity */ 
        normalized.color_matrix = 1u;
    }
    source_config = &normalized;
    int32_t rc = tc_frame_config_validate(source_config);
    if (rc != TC_OK) { return rc; }
    if (source_config->alpha_mode != 0u) {
        tc_set_error(TC_ERR_NOT_IMPLEMENTED, "v7b rice alpha residual mux");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    tc_base_frame base;
    memset(&base, 0, sizeof(base));
    rc = tc_base_frame_prepare(source_config, source_input, max_dim, &base);
    if (rc != TC_OK) { return rc; }
    size_t base_cap = tc_frame_packet_bound(&base.config);
    if (base_cap == 0u || !tc_uadd_size(base_cap, 65536u, &base_cap) ||
        base_cap > (size_t)TC_MAX_PACKET_SIZE) {
        tc_base_frame_release(&base);
        return TC_ERR_LIMIT_EXCEEDED;
    }
    uint8_t* base_packet = (uint8_t*)tc_alloc(base_cap);
    uint16_t* base_recon[TC_FRAME_MAX_PLANES] = { NULL, NULL, NULL, NULL };
    size_t base_strides[TC_FRAME_MAX_PLANES] = { 0u, 0u, 0u, 0u };
    v7b_rice_job jobs[TC_FRAME_MAX_PLANES];
    uint8_t* payloads[TC_FRAME_MAX_PLANES] = { NULL, NULL, NULL, NULL };
    uint32_t payload_sizes[TC_FRAME_MAX_PLANES] = { 0u, 0u, 0u, 0u };
    memset(jobs, 0, sizeof(jobs));
    if (base_packet == NULL) { rc = TC_ERR_OUT_OF_MEMORY; goto v7b_rice_cleanup; }
    topos_frame_stats base_stats;
    memset(&base_stats, 0, sizeof(base_stats));
    rc = tc_frame_encode(&base.config, &base.input, base_packet, base_cap, &base_stats);
    if (rc != TC_OK) { goto v7b_rice_cleanup; }
    topos_frame_header base_fh;
    rc = tc_frame_header_decode(base_packet, base_stats.packet_size, &base_fh);
    if (rc != TC_OK) { goto v7b_rice_cleanup; }
    for (uint32_t p = 0u; p < base_fh.plane_count; ++p) {
        rc = v7b_plane_alloc(base_fh.plane_visible_w[p], base_fh.plane_visible_h[p],
                             &base_recon[p], &base_strides[p]);
        if (rc != TC_OK) { goto v7b_rice_cleanup; }
    }
    {
        uint16_t* planes[TC_FRAME_MAX_PLANES] = {base_recon[0], base_recon[1],
                                                  base_recon[2], base_recon[3]};
        topos_frame_output base_output;
        rc = tc_frame_decode(base_packet, base_stats.packet_size, planes, base_strides,
                             &base_output);
    }
    if (rc != TC_OK) { goto v7b_rice_cleanup; }
    topos_frame_header outer_fh;
    rc = v7b_make_header(source_config, 1u + 3u, &outer_fh);
    if (rc != TC_OK) { goto v7b_rice_cleanup; }
    outer_fh.slice_count = 1u + outer_fh.plane_count;
    rc = tc_frame_derive_geometry(&outer_fh);
    if (rc != TC_OK) { goto v7b_rice_cleanup; }
    tc_job tasks[TC_FRAME_MAX_PLANES];
    memset(tasks, 0, sizeof(tasks));
    for (uint32_t p = 0u; p < outer_fh.plane_count; ++p) {
        jobs[p].source_config = source_config;
        jobs[p].source_input = source_input;
        jobs[p].outer_fh = &outer_fh;
        jobs[p].base_fh = &base_fh;
        jobs[p].base_plane = base_recon[p];
        jobs[p].base_stride = base_strides[p];
        jobs[p].plane = p;
        jobs[p].quant_step = v7b_rice_quant_step(source_config, p);
        tasks[p].fn = v7b_encode_rice_plane;
        tasks[p].ctx = &jobs[p];
    }
    uint32_t workers = (uint32_t)tc_dev_thread_count();
    if (workers > outer_fh.plane_count) { workers = outer_fh.plane_count; }
    (void)tc_parallel_for(tasks, outer_fh.plane_count, workers);
    for (uint32_t p = 0u; p < outer_fh.plane_count; ++p) {
        payloads[p] = jobs[p].payload;
        payload_sizes[p] = jobs[p].payload_size;
        if (jobs[p].rc != TC_OK) { rc = jobs[p].rc; goto v7b_rice_cleanup; }
    }
    {
        const uint8_t* payload_const[TC_FRAME_MAX_PLANES] = {
            payloads[0], payloads[1], payloads[2], payloads[3]
        };
        size_t written = 0u;
        rc = v7b_write_rice_directory(&outer_fh, &base_fh, base_packet,
                                      base_stats.packet_size, payload_const, payload_sizes,
                                      out, out_cap, &written);
        if (stats != NULL) {
            memset(stats, 0, sizeof(*stats));
            stats->struct_size = (uint32_t)sizeof(*stats);
            stats->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
            stats->packet_size = written > UINT32_MAX ? 0u : (uint32_t)written;
            stats->slice_count = outer_fh.slice_count;
            stats->qp_base = source_config->qp_base;
            stats->color_header_bytes = (uint32_t)(TC_FRAME_HEADER_SIZE +
                TC_V7B_DIRECTORY_FIXED_SIZE + 2u * TC_V7B_DESCRIPTOR_SIZE +
                (1u + outer_fh.plane_count) *
                    (TC_V7B_DESCRIPTOR_SIZE + TC_V7B_SEGMENT_DESCRIPTOR_SIZE));
            stats->color_payload_bytes = written >= stats->color_header_bytes
                ? stats->packet_size - stats->color_header_bytes : 0u;
        }
    }

v7b_rice_cleanup:
    for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
        tc_free(base_recon[p]);
        tc_free(jobs[p].residual);
        tc_free(jobs[p].payload);
    }
    tc_free(base_packet);
    tc_base_frame_release(&base);
    return rc;
}

/* —— ADR-C030 minor-4：shared transform pyramid 写入/读出 ——
 *
 * 目录布局（冻结）：
 *   fixed36 + layer0(base) + layer1(detail) + 几何表 + N×20B 段描述符
 *   段序：seg0 base 包；seg 1..P 每平面逃逸（全零允许 size=0）；
 *         之后按 plane→level→band 顺序的 detail 段。
 * payload：
 *   ESC1：magic4 + k_level + k_run + quant(=1) + reserved + Rice(稠密逃逸)
 *   DET1：magic4 + k_level + k_run + quant_step + tile_version(=0,
 *         band 粒度；64×64 tile 表为 tile_version=1 预留) + Rice(整带)
 */

#define TC_V7B_PYRAMID_MAGIC_ESCAPE "ESC1"
#define TC_V7B_PYRAMID_MAGIC_DETAIL "DET1"

/* 比例选择（计划 §1.1 语义）：源已 ≤ max_dim 时不做金字塔（base=源，
 * 回落原样路径）；否则取**最小可行**冻结比例——base 尽量贴近预览上限
 * （4K→1/2、6K→1/3、8K→1/4、12K→1/6，base 均精确 1920×1080）。 */
static uint32_t v7b_pyramid_ratio_for(uint32_t width, uint32_t height,
                                      uint32_t max_dim)
{
    if (width <= max_dim && height <= max_dim) {
        return 0u;
    }
    static const uint32_t candidates[] = { 2u, 3u, 4u, 6u };
    for (size_t i = 0u; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        const uint32_t r = candidates[i];
        if (width % r == 0u && height % r == 0u &&
            width / r <= max_dim && height / r <= max_dim) {
            return r;
        }
    }
    return 0u;
}

typedef struct v7b_payload_buf {
    uint8_t* data;
    uint32_t size;
} v7b_payload_buf;

static void v7b_payload_free(v7b_payload_buf* buf)
{
    if (buf != NULL) {
        tc_free(buf->data);
        buf->data = NULL;
        buf->size = 0u;
    }
}

/* 通用 int32 平面 Rice payload（ESC1/DET1 头 + run-level Rice）。 */
static int32_t v7b_pyramid_encode_plane_payload(const int32_t* values,
                                                uint32_t width, uint32_t height,
                                                size_t stride, uint32_t quant_step,
                                                const char* magic,
                                                v7b_payload_buf* out)
{
    if (values == NULL || out == NULL || magic == NULL ||
        quant_step == 0u || quant_step > 255u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "pyramid payload arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    size_t count = 0u;
    if (!tc_umul_size((size_t)width, (size_t)height, &count)) {
        return TC_ERR_LIMIT_EXCEEDED;
    }
    int32_t* work = (int32_t*)tc_alloc(count * sizeof(int32_t));
    if (work == NULL) { return TC_ERR_OUT_OF_MEMORY; }
    for (uint32_t y = 0u; y < height; ++y) {
        memcpy(work + (size_t)y * width, values + (size_t)y * stride,
               (size_t)width * sizeof(int32_t));
    }
    int32_t rc = tc_base_residual_quantize_in_place(work, width, height, width,
                                                    quant_step);
    uint32_t k_level = 0u;
    uint32_t k_run = 0u;
    tc_bitwriter bw;
    int have_bw = 0;
    if (rc == TC_OK) {
        rc = tc_bitwriter_init(&bw);
        have_bw = rc == TC_OK;
    }
    if (rc == TC_OK) {
        rc = tc_base_residual_encode(work, width, height, width, &bw,
                                     &k_level, &k_run);
    }
    if (rc == TC_OK) {
        rc = tc_bitwriter_flush_zero_pad(&bw);
    }
    const size_t bitstream_size = rc == TC_OK ? tc_bitwriter_byte_size(&bw) : 0u;
    size_t payload_size = 0u;
    if (rc == TC_OK &&
        (!tc_uadd_size(TC_V7B_RESIDUAL_HEADER_SIZE, bitstream_size,
                       &payload_size) ||
         payload_size > UINT32_MAX)) {
        rc = TC_ERR_LIMIT_EXCEEDED;
    }
    if (rc == TC_OK) {
        out->data = (uint8_t*)tc_alloc(payload_size == 0u ? 1u : payload_size);
        if (out->data == NULL) {
            rc = TC_ERR_OUT_OF_MEMORY;
        } else {
            memcpy(out->data, magic, 4u);
            out->data[4] = (uint8_t)k_level;
            out->data[5] = (uint8_t)k_run;
            out->data[6] = (uint8_t)quant_step;
            out->data[7] = 0u;
            if (bitstream_size > 0u) {
                memcpy(out->data + TC_V7B_RESIDUAL_HEADER_SIZE,
                       tc_bitwriter_data(&bw), bitstream_size);
            }
            out->size = (uint32_t)payload_size;
        }
    }
    if (have_bw) { tc_bitwriter_free(&bw); }
    tc_free(work);
    return rc;
}

static int32_t v7b_pyramid_decode_plane_payload(const uint8_t* data,
                                                uint32_t size,
                                                const char* magic,
                                                int32_t* values,
                                                uint32_t width, uint32_t height,
                                                size_t stride)
{
    if (data == NULL || magic == NULL || values == NULL ||
        size < TC_V7B_RESIDUAL_HEADER_SIZE ||
        memcmp(data, magic, 4u) != 0 || data[7] != 0u ||
        data[4] > TC_RICE_K_MAX || data[5] > TC_RICE_K_MAX ||
        data[6] == 0u || data[6] > 255u) {
        tc_set_error(TC_ERR_MALFORMED, "pyramid payload header");
        return TC_ERR_MALFORMED;
    }
    size_t bytes = 0u;
    if (!tc_umul_size(stride, (size_t)height, &bytes)) {
        return TC_ERR_LIMIT_EXCEEDED;
    }
    memset(values, 0, bytes * sizeof(int32_t));
    tc_bitreader br;
    tc_bitreader_init(&br, data + TC_V7B_RESIDUAL_HEADER_SIZE,
                      (size_t)size - TC_V7B_RESIDUAL_HEADER_SIZE);
    int32_t rc = tc_base_residual_decode(&br, values, width, height, stride,
                                         data[4], data[5]);
    if (rc != TC_OK) { return rc; }
    rc = tc_base_residual_scale_in_place(values, width, height, stride,
                                         data[6]);
    if (rc != TC_OK) { return rc; }
    const uint64_t consumed = tc_bitreader_bits_consumed(&br);
    const uint64_t end = br.bit_size;
    for (uint64_t bit = consumed; bit < end; ++bit) {
        const uint8_t value = data[TC_V7B_RESIDUAL_HEADER_SIZE +
                                    (size_t)(bit >> 3u)];
        if ((value & (uint8_t)(1u << (7u - (uint32_t)(bit & 7u)))) != 0u) {
            tc_set_error(TC_ERR_MALFORMED, "pyramid payload nonzero padding");
            return TC_ERR_MALFORMED;
        }
    }
    return TC_OK;
}

/* 几何表读写：per plane { level_count, per level { factor, ll_w, ll_h,
 * band_count, per band { w, h } } }。写入用实际 pyr 结构；读取回填
 * 同构 view 供校验与解码分配。 */
typedef struct v7b_pyramid_band_view {
    uint32_t width;
    uint32_t height;
    uint32_t segment_index;
} v7b_pyramid_band_view;

typedef struct v7b_pyramid_plane_view {
    uint32_t level_count;
    uint32_t factors[2];
    uint32_t ll_width[2];
    uint32_t ll_height[2];
    uint32_t band_count[2];
    v7b_pyramid_band_view bands[2][TC_PYRAMID_MAX_BANDS_PER_LEVEL];
    uint32_t escape_segment;
    uint32_t plane_width;
    uint32_t plane_height;
    uint32_t base_width;
    uint32_t base_height;
} v7b_pyramid_plane_view;

static size_t v7b_pyramid_geometry_size(uint32_t plane_count)
{
    /* 上界：每平面 2 级 × (1+2+2+1 + 8×4) + 1 */
    return (size_t)plane_count * (1u + 2u * (4u + 8u * 4u));
}

static void v7b_pyramid_write_geometry(uint8_t* table,
                                       const v7b_pyramid_plane_view* views,
                                       uint32_t plane_count)
{
    uint8_t* cur = table;
    for (uint32_t p = 0u; p < plane_count; ++p) {
        const v7b_pyramid_plane_view* v = &views[p];
        *cur++ = (uint8_t)v->level_count;
        for (uint32_t k = 0u; k < v->level_count; ++k) {
            *cur++ = (uint8_t)v->factors[k];
            tc_store_be16(cur, (uint16_t)v->ll_width[k]);
            cur += 2u;
            tc_store_be16(cur, (uint16_t)v->ll_height[k]);
            cur += 2u;
            *cur++ = (uint8_t)v->band_count[k];
            for (uint32_t b = 0u; b < v->band_count[k]; ++b) {
                tc_store_be16(cur, (uint16_t)v->bands[k][b].width);
                cur += 2u;
                tc_store_be16(cur, (uint16_t)v->bands[k][b].height);
                cur += 2u;
            }
        }
    }
}

/* 读取 + 冻结契约校验：因子 2/3、级联上限、band 数与几何必须与
 * factor 决定的冻结表一致；级 0 输入 = 平面源几何、级 1 输入 = 级 0 LL。 */
static int32_t v7b_pyramid_read_geometry(const uint8_t* table, size_t table_size,
                                         uint32_t plane_count,
                                         const topos_frame_header* fh,
                                         v7b_pyramid_plane_view* views)
{
    if (table == NULL || views == NULL || plane_count == 0u ||
        plane_count > TC_FRAME_MAX_PLANES) {
        tc_set_error(TC_ERR_MALFORMED, "pyramid geometry table arguments");
        return TC_ERR_MALFORMED;
    }
    const uint8_t* cur = table;
    const uint8_t* end = table + table_size;
    for (uint32_t p = 0u; p < plane_count; ++p) {
        v7b_pyramid_plane_view* v = &views[p];
        memset(v, 0, sizeof(*v));
        if (!tc_base_plane_dimensions(fh->visible_width, fh->visible_height,
                                      fh->pixel_format, p, fh->chroma_siting,
                                      &v->plane_width, &v->plane_height)) {
            return v7b_fail(TC_ERR_MALFORMED, "pyramid plane geometry", p);
        }
        if (end - cur < 1) {
            return v7b_fail(TC_ERR_MALFORMED, "pyramid geometry truncated", p);
        }
        v->level_count = *cur++;
        if (v->level_count == 0u || v->level_count > TC_PYRAMID_MAX_LEVELS) {
            return v7b_fail(TC_ERR_MALFORMED, "pyramid level count", p);
        }
        uint32_t in_w = v->plane_width;
        uint32_t in_h = v->plane_height;
        for (uint32_t k = 0u; k < v->level_count; ++k) {
            if (end - cur < 6) {
                return v7b_fail(TC_ERR_MALFORMED, "pyramid level truncated", k);
            }
            v->factors[k] = *cur++;
            if (v->factors[k] != 2u && v->factors[k] != 3u) {
                return v7b_fail(TC_ERR_UNSUPPORTED_VERSION, "pyramid factor",
                                v->factors[k]);
            }
            v->ll_width[k] = tc_load_be16(cur);
            cur += 2u;
            v->ll_height[k] = tc_load_be16(cur);
            cur += 2u;
            v->band_count[k] = *cur++;
            const uint32_t expect_bands = v->factors[k] == 2u ? 3u : 8u;
            if (v->band_count[k] != expect_bands) {
                return v7b_fail(TC_ERR_MALFORMED, "pyramid band count", k);
            }
            const uint32_t expect_w =
                v->factors[k] == 2u ? (in_w + 1u) / 2u : (in_w + 2u) / 3u;
            const uint32_t expect_h =
                v->factors[k] == 2u ? (in_h + 1u) / 2u : (in_h + 2u) / 3u;
            if (v->ll_width[k] != expect_w || v->ll_height[k] != expect_h) {
                return v7b_fail(TC_ERR_MALFORMED, "pyramid ll geometry", k);
            }
            if (end - cur < (ptrdiff_t)(4u * v->band_count[k])) {
                return v7b_fail(TC_ERR_MALFORMED, "pyramid bands truncated", k);
            }
            for (uint32_t b = 0u; b < v->band_count[k]; ++b) {
                v->bands[k][b].width = tc_load_be16(cur);
                cur += 2u;
                v->bands[k][b].height = tc_load_be16(cur);
                cur += 2u;
                uint32_t bw = 0u;
                uint32_t bh = 0u;
                if (v->factors[k] == 2u) {
                    const uint32_t dw = in_w / 2u;
                    const uint32_t dh = in_h / 2u;
                    if (b == 0u) { bw = expect_w; bh = dh; }
                    else if (b == 1u) { bw = dw; bh = expect_h; }
                    else { bw = dw; bh = dh; }
                } else {
                    bw = expect_w;
                    bh = expect_h;
                }
                if (v->bands[k][b].width != bw || v->bands[k][b].height != bh) {
                    return v7b_fail(TC_ERR_MALFORMED, "pyramid band geometry", b);
                }
            }
            in_w = v->ll_width[k];
            in_h = v->ll_height[k];
        }
        v->base_width = v->ll_width[v->level_count - 1u];
        v->base_height = v->ll_height[v->level_count - 1u];
    }
    if (cur != end) {
        return v7b_fail(TC_ERR_MALFORMED, "pyramid geometry trailing", 0u);
    }
    return TC_OK;
}

static int32_t v7b_write_pyramid_directory(
    const topos_frame_header* fh, const uint8_t* base_packet, uint32_t base_size,
    const v7b_pyramid_plane_view* views,
    const v7b_payload_buf* escape_payloads,
    const v7b_payload_buf* detail_payloads, /* plane-major: p*Σbands + idx */
    uint32_t segment_count, uint8_t* out, size_t out_cap, size_t* packet_size)
{
    if (fh == NULL || base_packet == NULL || views == NULL ||
        escape_payloads == NULL || detail_payloads == NULL ||
        packet_size == NULL || (out == NULL && out_cap != 0u)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "pyramid writer arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    const uint32_t planes = fh->plane_count;
    /* 目录内声明/占位均为精确尺寸；上界只用于解析侧防越界。 */
    size_t geometry_exact = 0u;
    for (uint32_t p = 0u; p < planes; ++p) {
        geometry_exact += 1u;
        for (uint32_t k = 0u; k < views[p].level_count; ++k) {
            geometry_exact += 6u + 4u * views[p].band_count[k];
        }
    }
    const size_t geometry_size = geometry_exact;
    if (geometry_size > v7b_pyramid_geometry_size(planes)) {
        return v7b_fail(TC_ERR_LIMIT_EXCEEDED, "pyramid geometry size",
                        (unsigned long long)geometry_size);
    }
    uint32_t directory_size = 0u;
    uint32_t segment_bytes = 0u;
    if (!tc_uadd_u32(TC_V7B_DIRECTORY_FIXED_SIZE,
                     2u * TC_V7B_DESCRIPTOR_SIZE, &directory_size) ||
        !tc_uadd_u32(directory_size, (uint32_t)geometry_size, &directory_size) ||
        !tc_umul_u32(segment_count, TC_V7B_SEGMENT_DESCRIPTOR_SIZE,
                     &segment_bytes) ||
        !tc_uadd_u32(directory_size, segment_bytes, &directory_size) ||
        directory_size > TC_V7_MAX_DIRECTORY_SIZE ||
        segment_count > TC_MAX_SLICE_COUNT) {
        return v7b_fail(TC_ERR_LIMIT_EXCEEDED, "pyramid directory arithmetic",
                        segment_count);
    }
    size_t payload_size = base_size;
    for (uint32_t p = 0u; p < planes; ++p) {
        if (!tc_uadd_size(payload_size, escape_payloads[p].size, &payload_size)) {
            return v7b_fail(TC_ERR_LIMIT_EXCEEDED, "pyramid escape payload", p);
        }
    }
    for (uint32_t i = 0u; i < segment_count - 1u - planes; ++i) {
        if (!tc_uadd_size(payload_size, detail_payloads[i].size, &payload_size)) {
            return v7b_fail(TC_ERR_LIMIT_EXCEEDED, "pyramid detail payload", i);
        }
    }
    size_t needed = (size_t)TC_FRAME_HEADER_SIZE + directory_size;
    if (!tc_uadd_size(needed, payload_size, &needed) ||
        needed > (size_t)TC_MAX_PACKET_SIZE || needed > UINT32_MAX) {
        return v7b_fail(TC_ERR_LIMIT_EXCEEDED, "pyramid packet size", needed);
    }
    *packet_size = needed;
    if (out == NULL || out_cap < needed) {
        tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "pyramid writer needs %zu bytes",
                     needed);
        return TC_ERR_BUFFER_TOO_SMALL;
    }
    memset(out + TC_FRAME_HEADER_SIZE, 0u, directory_size);
    uint8_t* directory = out + TC_FRAME_HEADER_SIZE;
    memcpy(directory, "TPLD", 4u);
    tc_store_be16(directory + 4u, 1u);
    tc_store_be16(directory + 6u, TC_V7B_DIRECTORY_VERSION_MINOR_PYRAMID);
    tc_store_be32(directory + 8u, TC_V7B_DIRECTORY_FLAGS);
    tc_store_be16(directory + 12u, 2u);
    tc_store_be16(directory + 14u, (uint16_t)segment_count);
    directory[17] = fh->plane_count;
    tc_store_be16(directory + 18u, (uint16_t)geometry_size);
    tc_store_be16(directory + 22u, TC_V7B_SEGMENT_DESCRIPTOR_SIZE);
    tc_store_be32(directory + 24u, directory_size);
    tc_store_be32(directory + 28u, TC_FRAME_HEADER_SIZE + directory_size);

    uint8_t* layer = directory + TC_V7B_DIRECTORY_FIXED_SIZE;
    layer[1] = TC_V7B_LAYER_KIND_BASE;
    tc_store_be32(layer + 8u, 1u);
    tc_store_be32(layer + 16u, 1u);
    layer += TC_V7B_DESCRIPTOR_SIZE;
    layer[0] = 1u;
    layer[1] = TC_V7B_LAYER_KIND_DETAIL;
    tc_store_be32(layer + 4u, 1u);
    tc_store_be32(layer + 8u, segment_count - 1u);
    tc_store_be32(layer + 12u, 0u);
    tc_store_be32(layer + 16u, planes);

    v7b_pyramid_write_geometry(
        directory + TC_V7B_DIRECTORY_FIXED_SIZE + 2u * TC_V7B_DESCRIPTOR_SIZE,
        views, planes);

    uint8_t* segments = directory + TC_V7B_DIRECTORY_FIXED_SIZE +
        2u * TC_V7B_DESCRIPTOR_SIZE + geometry_size;
    size_t cursor = (size_t)TC_FRAME_HEADER_SIZE + directory_size;
    uint32_t seg = 0u;
    uint32_t slice = 0u;
    /* base 段 */
    {
        uint8_t* raw = segments;
        tc_store_be32(raw + 0u, (uint32_t)cursor);
        tc_store_be32(raw + 4u, base_size);
        tc_store_be32(raw + 8u, tc_crc32(base_packet, base_size));
        tc_store_be16(raw + 16u, TC_V7B_SEGMENT_FLAG_BASE_PACKET);
        memmove(out + cursor, base_packet, base_size);
        cursor += base_size;
        seg = 1u;
        slice = 1u;
    }
    /* 逃逸段（全零 → size 0 可跳过） */
    for (uint32_t p = 0u; p < planes; ++p) {
        uint8_t* raw = segments + (size_t)seg * TC_V7B_SEGMENT_DESCRIPTOR_SIZE;
        tc_store_be32(raw + 0u, (uint32_t)cursor);
        tc_store_be32(raw + 4u, escape_payloads[p].size);
        if (escape_payloads[p].size > 0u) {
            tc_store_be32(raw + 8u, tc_crc32(escape_payloads[p].data,
                                             escape_payloads[p].size));
            memmove(out + cursor, escape_payloads[p].data,
                    escape_payloads[p].size);
            cursor += escape_payloads[p].size;
        }
        tc_store_be16(raw + 12u, (uint16_t)slice);
        raw[14] = (uint8_t)p;
        tc_store_be16(raw + 16u, TC_V7B_SEGMENT_FLAG_RESIDUAL);
        ++seg;
        ++slice;
    }
    /* detail 段 */
    uint32_t detail_index = 0u;
    for (uint32_t p = 0u; p < planes; ++p) {
        for (uint32_t k = 0u; k < views[p].level_count; ++k) {
            for (uint32_t b = 0u; b < views[p].band_count[k]; ++b) {
                uint8_t* raw =
                    segments + (size_t)seg * TC_V7B_SEGMENT_DESCRIPTOR_SIZE;
                tc_store_be32(raw + 0u, (uint32_t)cursor);
                tc_store_be32(raw + 4u, detail_payloads[detail_index].size);
                tc_store_be32(raw + 8u, tc_crc32(detail_payloads[detail_index].data,
                                                 detail_payloads[detail_index].size));
                memmove(out + cursor, detail_payloads[detail_index].data,
                        detail_payloads[detail_index].size);
                cursor += detail_payloads[detail_index].size;
                tc_store_be16(raw + 12u, (uint16_t)slice);
                raw[14] = (uint8_t)p;
                tc_store_be16(raw + 16u, TC_V7B_SEGMENT_FLAG_DETAIL);
                ++seg;
                ++slice;
                ++detail_index;
            }
        }
    }
    if (cursor != needed || seg != segment_count) {
        return v7b_fail(TC_ERR_MALFORMED, "pyramid payload cursor", cursor);
    }
    tc_store_be32(directory + 32u, v7b_directory_crc(directory, directory_size));
    topos_frame_header header = *fh;
    header.frame_packet_size = (uint32_t)needed;
    return tc_frame_header_encode_v7a(&header, out);
}

static int32_t v7b_parse_directory_pyramid(const uint8_t* packet,
                                           size_t packet_size,
                                           const topos_frame_header* fh,
                                           tc_v7b_directory_view* out)
{
    if (packet == NULL || fh == NULL || out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "pyramid directory arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    int32_t rc = tc_frame_header_validate_v7a_contract(fh);
    if (rc != TC_OK) { return rc; }
    if (packet_size < (size_t)fh->frame_packet_size) {
        return v7b_fail(TC_ERR_TRUNCATED, "pyramid packet size", packet_size);
    }
    if (packet_size != (size_t)fh->frame_packet_size ||
        packet_size > (size_t)TC_MAX_PACKET_SIZE ||
        !tc_offset_in_bounds(packet_size, TC_FRAME_HEADER_SIZE,
                             TC_V7B_DIRECTORY_FIXED_SIZE)) {
        return v7b_fail(TC_ERR_MALFORMED, "pyramid packet bounds", packet_size);
    }
    const uint8_t* directory = packet + TC_FRAME_HEADER_SIZE;
    const uint16_t segment_count = tc_load_be16(directory + 14u);
    if (memcmp(directory, "TPLD", 4u) != 0 ||
        tc_load_be16(directory + 4u) != 1u ||
        tc_load_be16(directory + 6u) != TC_V7B_DIRECTORY_VERSION_MINOR_PYRAMID ||
        tc_load_be32(directory + 8u) != TC_V7B_DIRECTORY_FLAGS ||
        tc_load_be16(directory + 12u) != 2u ||
        directory[16] != 0u || directory[17] != fh->plane_count ||
        tc_load_be16(directory + 20u) != 0u ||
        tc_load_be16(directory + 22u) != TC_V7B_SEGMENT_DESCRIPTOR_SIZE ||
        segment_count == 0u || segment_count > TC_MAX_SLICE_COUNT ||
        (uint32_t)segment_count != fh->slice_count) {
        return v7b_fail(TC_ERR_UNSUPPORTED_VERSION, "pyramid fixed contract", 0u);
    }
    const uint16_t geometry_size = tc_load_be16(directory + 18u);
    if (geometry_size == 0u ||
        geometry_size > v7b_pyramid_geometry_size(fh->plane_count)) {
        return v7b_fail(TC_ERR_MALFORMED, "pyramid geometry size", geometry_size);
    }
    uint32_t expected_size = 0u;
    uint32_t segment_bytes = 0u;
    if (!tc_uadd_u32(TC_V7B_DIRECTORY_FIXED_SIZE,
                     2u * TC_V7B_DESCRIPTOR_SIZE, &expected_size) ||
        !tc_uadd_u32(expected_size, geometry_size, &expected_size) ||
        !tc_umul_u32((uint32_t)segment_count, TC_V7B_SEGMENT_DESCRIPTOR_SIZE,
                     &segment_bytes) ||
        !tc_uadd_u32(expected_size, segment_bytes, &expected_size)) {
        return v7b_fail(TC_ERR_LIMIT_EXCEEDED, "pyramid directory arithmetic",
                        segment_count);
    }
    const uint32_t directory_size = tc_load_be32(directory + 24u);
    const uint32_t payload_offset = tc_load_be32(directory + 28u);
    uint32_t directory_end = 0u;
    if (directory_size != expected_size || directory_size > TC_V7_MAX_DIRECTORY_SIZE ||
        !tc_uadd_u32(TC_FRAME_HEADER_SIZE, directory_size, &directory_end) ||
        payload_offset != directory_end ||
        !tc_offset_in_bounds(packet_size, TC_FRAME_HEADER_SIZE, directory_size) ||
        tc_load_be32(directory + 32u) != v7b_directory_crc(directory, directory_size)) {
        return v7b_fail(TC_ERR_MALFORMED, "pyramid directory bounds/crc",
                        directory_size);
    }
    const uint8_t* layer0 = directory + TC_V7B_DIRECTORY_FIXED_SIZE;
    const uint8_t* layer1 = layer0 + TC_V7B_DESCRIPTOR_SIZE;
    if (layer0[0] != 0u || layer0[1] != TC_V7B_LAYER_KIND_BASE ||
        tc_load_be32(layer0 + 4u) != 0u || tc_load_be32(layer0 + 8u) != 1u ||
        tc_load_be32(layer0 + 12u) != 0u ||
        tc_load_be32(layer0 + 16u) != 1u ||
        layer1[0] != 1u || layer1[1] != TC_V7B_LAYER_KIND_DETAIL ||
        tc_load_be32(layer1 + 4u) != 1u ||
        tc_load_be32(layer1 + 8u) != (uint32_t)segment_count - 1u ||
        tc_load_be32(layer1 + 12u) != 0u ||
        tc_load_be32(layer1 + 16u) != fh->plane_count) {
        return v7b_fail(TC_ERR_UNSUPPORTED_VERSION, "pyramid layer contract", 0u);
    }
    /* 段契约：0=base；1..P=escape（size 0 允许）；其余=DETAIL。
     * 段偏移连续递增；size>0 才要求落在包内并参与游标推进。 */
    const uint8_t* segment_desc =
        directory + TC_V7B_DIRECTORY_FIXED_SIZE + 2u * TC_V7B_DESCRIPTOR_SIZE +
        geometry_size;
    const uint32_t planes = fh->plane_count;
    size_t cursor = (size_t)payload_offset;
    for (uint32_t i = 0u; i < (uint32_t)segment_count; ++i) {
        const uint8_t* raw =
            segment_desc + (size_t)i * TC_V7B_SEGMENT_DESCRIPTOR_SIZE;
        const uint32_t offset = tc_load_be32(raw + 0u);
        const uint32_t size = tc_load_be32(raw + 4u);
        const uint16_t slice_index = tc_load_be16(raw + 12u);
        const uint8_t plane_index = raw[14];
        const uint16_t flags = tc_load_be16(raw + 16u);
        uint16_t expect_flags = 0u;
        int plane_ok = 1;
        if (i == 0u) {
            expect_flags = TC_V7B_SEGMENT_FLAG_BASE_PACKET;
            plane_ok = plane_index == 0u;
        } else if (i <= planes) {
            expect_flags = TC_V7B_SEGMENT_FLAG_RESIDUAL;
            plane_ok = plane_index == (uint8_t)(i - 1u);
        } else {
            expect_flags = TC_V7B_SEGMENT_FLAG_DETAIL;
            /* 归属平面由几何表段序决定；这里只拒绝越界。 */
            plane_ok = plane_index < planes;
        }
        if (slice_index != (uint16_t)i || !plane_ok ||
            flags != expect_flags || tc_load_be16(raw + 18u) != 0u ||
            (size_t)offset != cursor) {
            return v7b_fail(TC_ERR_MALFORMED, "pyramid segment contract", i);
        }
        if (size == 0u) {
            if (i != 0u && i > planes) {
                return v7b_fail(TC_ERR_MALFORMED, "pyramid zero-size detail", i);
            }
            if (i == 0u) {
                return v7b_fail(TC_ERR_MALFORMED, "pyramid zero-size base", i);
            }
        } else {
            if (!tc_offset_in_bounds(packet_size, offset, size)) {
                return v7b_fail(TC_ERR_MALFORMED, "pyramid segment bounds", i);
            }
            if (!tc_uadd_size(cursor, (size_t)size, &cursor)) {
                return v7b_fail(TC_ERR_LIMIT_EXCEEDED, "pyramid segment cursor", i);
            }
        }
    }
    if (cursor != packet_size) {
        return v7b_fail(TC_ERR_MALFORMED, "pyramid payload trailing/gap", cursor);
    }
    out->packet = packet;
    out->packet_size = packet_size;
    out->directory = directory;
    out->directory_size = directory_size;
    out->payload_offset = payload_offset;
    out->directory_version_minor = TC_V7B_DIRECTORY_VERSION_MINOR_PYRAMID;
    out->slice_count = segment_count;
    out->plane_count = fh->plane_count;
    out->segment_count = segment_count;
    out->segment_descriptors = segment_desc;
    return TC_OK;
}

int32_t tc_v7b_frame_encode_pyramid(const topos_frame_config* source_config,
                                    const topos_frame_input* source_input,
                                    uint32_t max_dim,
                                    uint8_t* out, size_t out_cap,
                                    topos_frame_stats* stats)
{
    if (source_config == NULL || source_input == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "pyramid encode arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    topos_frame_config normalized = *source_config;
    if (normalized.profile == 0u) { normalized.profile = 3u; }
    if (normalized.bit_depth == 0u) { normalized.bit_depth = 10u; }
    if (normalized.color_primaries == 0u) { normalized.color_primaries = 1u; }
    if (normalized.color_transfer == 0u) {
        /* pf=3（TRAW）默认 LOG0（与 cfg_to_frame_header 同规则） */
        normalized.color_transfer = normalized.pixel_format == 3u
                                        ? TC_TRANSFER_TRAW_LOG0 : 1u;
    }
    if (normalized.color_matrix == 0u && normalized.pixel_format != 2u &&
        normalized.pixel_format != 3u) { /* TRAW CFA：无矩阵语义，保持 identity */ 
        normalized.color_matrix = 1u;
    }
    source_config = &normalized;
    int32_t rc = tc_frame_config_validate(source_config);
    if (rc != TC_OK) { return rc; }
    if (source_config->alpha_mode != 0u) {
        tc_set_error(TC_ERR_NOT_IMPLEMENTED, "pyramid alpha mux");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    if (max_dim == 0u) { max_dim = TC_CODEC_AUTO_2K_MAX_DIM; }
    const uint32_t ratio = v7b_pyramid_ratio_for(
        source_config->visible_width, source_config->visible_height, max_dim);
    if (ratio == 0u) {
        /* 互操作/非整除几何：回落通用 minor-2 路径，目录如实记录几何。 */
        return v7b_frame_encode_rice(source_config, source_input, max_dim,
                                     out, out_cap, stats);
    }
    const tc_pyramid_ratio pyramid_ratio = ratio == 2u ? TC_PYRAMID_RATIO_2
        : ratio == 3u ? TC_PYRAMID_RATIO_3
        : ratio == 4u ? TC_PYRAMID_RATIO_4 : TC_PYRAMID_RATIO_6;

    topos_frame_header outer_fh;
    rc = v7b_make_header(source_config, 1u + 3u, &outer_fh);
    if (rc != TC_OK) { return rc; }

    /* 每平面一次共享分析（P1-03 不变量：无第二份源尺寸像素平面）。 */
    tc_pyramid_plane pyramids[TC_FRAME_MAX_PLANES];
    memset(pyramids, 0, sizeof(pyramids));
    v7b_pyramid_plane_view views[TC_FRAME_MAX_PLANES];
    memset(views, 0, sizeof(views));
    for (uint32_t p = 0u; p < outer_fh.plane_count; ++p) {
        uint32_t pw = 0u;
        uint32_t ph = 0u;
        if (!tc_base_plane_dimensions(outer_fh.visible_width,
                                      outer_fh.visible_height,
                                      outer_fh.pixel_format, p,
                                      outer_fh.chroma_siting, &pw, &ph)) {
            rc = v7b_fail(TC_ERR_INVALID_ARGUMENT, "pyramid plane geometry", p);
            goto pyramid_cleanup;
        }
        const size_t src_stride = source_input->strides[p] != 0u
            ? source_input->strides[p] : (size_t)pw;
        rc = tc_pyramid_analyze_plane(source_input->planes[p], src_stride,
                                      pw, ph, source_config->bit_depth,
                                      pyramid_ratio, &pyramids[p]);
        if (rc != TC_OK) { goto pyramid_cleanup; }
        views[p].level_count = pyramids[p].level_count;
        views[p].base_width = pyramids[p].base_width;
        views[p].base_height = pyramids[p].base_height;
        for (uint32_t k = 0u; k < pyramids[p].level_count; ++k) {
            views[p].factors[k] = pyramids[p].levels[k].factor;
            views[p].ll_width[k] = pyramids[p].levels[k].ll_width;
            views[p].ll_height[k] = pyramids[p].levels[k].ll_height;
            views[p].band_count[k] = pyramids[p].levels[k].band_count;
            for (uint32_t b = 0u; b < pyramids[p].levels[k].band_count; ++b) {
                views[p].bands[k][b].width =
                    pyramids[p].levels[k].bands[b].width;
                views[p].bands[k][b].height =
                    pyramids[p].levels[k].bands[b].height;
            }
        }
    }

    /* base 帧编码：金字塔低频直通现有帧编码器（P2-01）。 */
    topos_frame_config base_config = *source_config;
    base_config.struct_size = (uint32_t)sizeof(base_config);
    base_config.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    base_config.visible_width = (uint16_t)pyramids[0].base_width;
    base_config.visible_height = (uint16_t)pyramids[0].base_height;
    topos_frame_input base_input;
    memset(&base_input, 0, sizeof(base_input));
    base_input.struct_size = (uint32_t)sizeof(base_input);
    base_input.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    for (uint32_t p = 0u; p < outer_fh.plane_count; ++p) {
        base_input.planes[p] = pyramids[p].base;
        base_input.strides[p] = pyramids[p].base_width;
    }
    size_t base_cap = tc_frame_packet_bound(&base_config);
    if (base_cap == 0u || !tc_uadd_size(base_cap, 65536u, &base_cap) ||
        base_cap > (size_t)TC_MAX_PACKET_SIZE) {
        rc = TC_ERR_LIMIT_EXCEEDED;
        goto pyramid_cleanup;
    }
    uint8_t* base_packet = (uint8_t*)tc_alloc(base_cap);
    if (base_packet == NULL) {
        rc = TC_ERR_OUT_OF_MEMORY;
        goto pyramid_cleanup;
    }
    topos_frame_stats base_stats;
    memset(&base_stats, 0, sizeof(base_stats));
    rc = tc_frame_encode(&base_config, &base_input, base_packet, base_cap,
                         &base_stats);
    if (rc != TC_OK) { goto pyramid_cleanup_with_packet; }
    topos_frame_header base_fh;
    rc = tc_frame_header_decode(base_packet, base_stats.packet_size, &base_fh);
    if (rc != TC_OK) { goto pyramid_cleanup_with_packet; }

    /* 逃逸与 detail payload。 */
    const uint32_t planes = outer_fh.plane_count;
    v7b_payload_buf* escape_payloads =
        (v7b_payload_buf*)tc_alloc(planes * sizeof(v7b_payload_buf));
    uint32_t total_bands = 0u;
    for (uint32_t p = 0u; p < planes; ++p) {
        for (uint32_t k = 0u; k < pyramids[p].level_count; ++k) {
            total_bands += pyramids[p].levels[k].band_count;
        }
    }
    v7b_payload_buf* detail_payloads =
        (v7b_payload_buf*)tc_alloc((size_t)total_bands * sizeof(v7b_payload_buf));
    if (escape_payloads == NULL || detail_payloads == NULL) {
        tc_free(escape_payloads);
        tc_free(detail_payloads);
        rc = TC_ERR_OUT_OF_MEMORY;
        goto pyramid_cleanup_with_packet;
    }
    memset(escape_payloads, 0, planes * sizeof(v7b_payload_buf));
    memset(detail_payloads, 0, (size_t)total_bands * sizeof(v7b_payload_buf));
    for (uint32_t p = 0u; p < planes; ++p) {
        const tc_pyramid_plane* pyr = &pyramids[p];
        uint32_t zero = 0u;
        for (uint32_t i = 0u; i < pyr->base_width * pyr->base_height; ++i) {
            if (pyr->escape[i] != 0) { zero = 1u; break; }
        }
        if (zero != 0u) {
            rc = v7b_pyramid_encode_plane_payload(
                pyr->escape, pyr->base_width, pyr->base_height,
                pyr->base_width, 1u, TC_V7B_PYRAMID_MAGIC_ESCAPE,
                &escape_payloads[p]);
            if (rc != TC_OK) { goto pyramid_cleanup_payloads; }
        }
    }
    {
        uint32_t idx = 0u;
        for (uint32_t p = 0u; p < planes; ++p) {
            /* P2-03 v1 联合规则：匹配质量优先。base 编码质量即全解质量
             * 上限；detail 量化误差只会额外拉低它，故 detail 恒取无损
             * 步长 1，保证全解质量 == base 编码质量（P4-01 禁止以低
             * 质量换体积）。lambda 联合分配待 V2 有损档位门禁时引入。 */
            const uint32_t quant_step = 1u;
            for (uint32_t k = 0u; k < pyramids[p].level_count; ++k) {
                for (uint32_t b = 0u; b < pyramids[p].levels[k].band_count; ++b) {
                    const tc_pyramid_band* band =
                        &pyramids[p].levels[k].bands[b];
                    rc = v7b_pyramid_encode_plane_payload(
                        band->coeffs, band->width, band->height, band->width,
                        quant_step, TC_V7B_PYRAMID_MAGIC_DETAIL,
                        &detail_payloads[idx]);
                    if (rc != TC_OK) { goto pyramid_cleanup_payloads; }
                    ++idx;
                }
            }
        }
    }

    {
        const uint32_t segment_count = 1u + planes + total_bands;
        outer_fh.slice_count = (uint16_t)segment_count;
        rc = tc_frame_derive_geometry(&outer_fh);
        if (rc != TC_OK) { goto pyramid_cleanup_payloads; }
        size_t written = 0u;
        rc = v7b_write_pyramid_directory(&outer_fh, base_packet,
                                         base_stats.packet_size, views,
                                         escape_payloads, detail_payloads,
                                         segment_count, out, out_cap, &written);
        if (stats != NULL && (rc == TC_OK || rc == TC_ERR_BUFFER_TOO_SMALL)) {
            memset(stats, 0, sizeof(*stats));
            stats->struct_size = (uint32_t)sizeof(*stats);
            stats->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
            stats->packet_size = written > UINT32_MAX ? 0u : (uint32_t)written;
            stats->slice_count = outer_fh.slice_count;
            stats->qp_base = source_config->qp_base;
        }
    }

pyramid_cleanup_payloads:
    for (uint32_t i = 0u; i < planes; ++i) {
        v7b_payload_free(&escape_payloads[i]);
    }
    for (uint32_t i = 0u; i < total_bands; ++i) {
        v7b_payload_free(&detail_payloads[i]);
    }
    tc_free(escape_payloads);
    tc_free(detail_payloads);
pyramid_cleanup_with_packet:
    tc_free(base_packet);
pyramid_cleanup:
    for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
        tc_pyramid_plane_release(&pyramids[p]);
    }
    return rc;
}

static int32_t v7b_plane_alloc_i32(uint32_t width, uint32_t height,
                                   int32_t** pixels)
{
    if (pixels == NULL || width == 0u || height == 0u) {
        return TC_ERR_INVALID_ARGUMENT;
    }
    size_t count = 0u;
    if (!tc_umul_size((size_t)width, (size_t)height, &count)) {
        return TC_ERR_LIMIT_EXCEEDED;
    }
    *pixels = (int32_t*)tc_alloc(count * sizeof(int32_t));
    if (*pixels == NULL) { return TC_ERR_OUT_OF_MEMORY; }
    memset(*pixels, 0, count * sizeof(int32_t));
    return TC_OK;
}

/* minor-4 全解：base + escape + detail → 合成源平面。 */
static int32_t v7b_decode_pyramid_full(const uint8_t* packet,
                                       const topos_frame_header* outer_fh,
                                       const tc_v7b_base_view* base,
                                       uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                       const size_t strides[TC_FRAME_MAX_PLANES],
                                       topos_frame_output* out_info,
                                       tc_v7b_decode_stats* stats)
{
    const uint32_t planes = outer_fh->plane_count;
    const uint16_t geometry_size =
        tc_load_be16(base->directory.directory + 18u);
    const uint8_t* geometry = base->directory.directory +
        TC_V7B_DIRECTORY_FIXED_SIZE + 2u * TC_V7B_DESCRIPTOR_SIZE;
    v7b_pyramid_plane_view* views =
        (v7b_pyramid_plane_view*)tc_alloc(planes * sizeof(*views));
    uint16_t** base_planes =
        (uint16_t**)tc_alloc(planes * sizeof(uint16_t*));
    size_t* base_strides = (size_t*)tc_alloc(planes * sizeof(size_t));
    int32_t rc = TC_ERR_OUT_OF_MEMORY;
    if (views == NULL || base_planes == NULL || base_strides == NULL) {
        goto pyramid_decode_cleanup;
    }
    memset(base_planes, 0, planes * sizeof(uint16_t*));
    rc = v7b_pyramid_read_geometry(geometry, geometry_size, planes, outer_fh,
                                   views);
    if (rc != TC_OK) { goto pyramid_decode_cleanup; }
    for (uint32_t p = 0u; p < planes; ++p) {
        rc = v7b_plane_alloc(views[p].base_width, views[p].base_height,
                             &base_planes[p], &base_strides[p]);
        if (rc != TC_OK) { goto pyramid_decode_cleanup; }
    }
    {
        topos_frame_output base_output;
        uint16_t* decode_planes[TC_FRAME_MAX_PLANES] = {
            base_planes[0], planes > 1u ? base_planes[1] : NULL,
            planes > 2u ? base_planes[2] : NULL, NULL
        };
        size_t decode_strides[TC_FRAME_MAX_PLANES] = { 0u, 0u, 0u, 0u };
        for (uint32_t p = 0u; p < planes; ++p) {
            decode_strides[p] = base_strides[p];
        }
        rc = tc_frame_decode(base->packet, base->segment.payload_size,
                             decode_planes, decode_strides, &base_output);
    }
    if (rc != TC_OK) { goto pyramid_decode_cleanup; }
    for (uint32_t p = 0u; p < planes; ++p) {
        const v7b_pyramid_plane_view* v = &views[p];
        tc_pyramid_plane pyr;
        memset(&pyr, 0, sizeof(pyr));
        int32_t* escape = NULL;
        rc = v7b_plane_alloc_i32(v->base_width, v->base_height, &escape);
        if (rc != TC_OK) { goto pyramid_decode_plane_fail; }
        {
            tc_v7b_segment segment;
            rc = v7b_get_segment(&base->directory, 1u + p, &segment);
            if (rc != TC_OK) { goto pyramid_decode_plane_fail; }
            if (segment.payload_size > 0u) {
                const uint8_t* data = packet + segment.payload_offset;
                if (tc_crc32(data, segment.payload_size) !=
                    segment.payload_crc32) {
                    tc_set_error(TC_ERR_CHECKSUM_MISMATCH,
                                 "pyramid escape crc %u", p);
                    rc = TC_ERR_CHECKSUM_MISMATCH;
                    goto pyramid_decode_plane_fail;
                }
                rc = v7b_pyramid_decode_plane_payload(
                    data, segment.payload_size, TC_V7B_PYRAMID_MAGIC_ESCAPE,
                    escape, v->base_width, v->base_height, v->base_width);
                if (rc != TC_OK) { goto pyramid_decode_plane_fail; }
            }
            if (stats != NULL) {
                stats->segments_requested++;
                stats->segments_read++;
                stats->bytes_read += segment.payload_size;
            }
        }
        pyr.levels[0].factor = v->factors[0];
        if (v->level_count > 1u) { pyr.levels[1].factor = v->factors[1]; }
        /* detail 段索引：1 + planes + Σ_{q<p} bands(前序平面全部级) + 已遍历 */
        uint32_t bands_before = 0u;
        for (uint32_t q = 0u; q < p; ++q) {
            bands_before += views[q].band_count[0] + views[q].band_count[1];
        }
        pyr.source_width = v->plane_width;
        pyr.source_height = v->plane_height;
        pyr.bit_depth = outer_fh->bit_depth;
        pyr.base_width = v->base_width;
        pyr.base_height = v->base_height;
        pyr.level_count = v->level_count;
        pyr.base = base_planes[p];
        pyr.escape = escape;
        for (uint32_t k = 0u; k < v->level_count; ++k) {
            pyr.levels[k].input_width = k == 0u ? v->plane_width
                                                : v->ll_width[k - 1u];
            pyr.levels[k].input_height = k == 0u ? v->plane_height
                                                 : v->ll_height[k - 1u];
            pyr.levels[k].ll_width = v->ll_width[k];
            pyr.levels[k].ll_height = v->ll_height[k];
            pyr.levels[k].band_count = v->band_count[k];
            for (uint32_t b = 0u; b < v->band_count[k]; ++b) {
                tc_pyramid_band* band = &pyr.levels[k].bands[b];
                band->width = v->bands[k][b].width;
                band->height = v->bands[k][b].height;
                rc = v7b_plane_alloc_i32(band->width, band->height,
                                         &band->coeffs);
                if (rc != TC_OK) { goto pyramid_decode_plane_fail; }
                const uint32_t detail_index = 1u + planes + bands_before;
                tc_v7b_segment segment;
                rc = v7b_get_segment(&base->directory, detail_index, &segment);
                if (rc != TC_OK) { goto pyramid_decode_plane_fail; }
                const uint8_t* data = packet + segment.payload_offset;
                if (tc_crc32(data, segment.payload_size) !=
                    segment.payload_crc32) {
                    tc_set_error(TC_ERR_CHECKSUM_MISMATCH,
                                 "pyramid detail crc %u", detail_index);
                    rc = TC_ERR_CHECKSUM_MISMATCH;
                    goto pyramid_decode_plane_fail;
                }
                rc = v7b_pyramid_decode_plane_payload(
                    data, segment.payload_size, TC_V7B_PYRAMID_MAGIC_DETAIL,
                    band->coeffs, band->width, band->height, band->width);
                if (rc != TC_OK) { goto pyramid_decode_plane_fail; }
                if (stats != NULL) {
                    stats->segments_requested++;
                    stats->segments_read++;
                    stats->bytes_read += segment.payload_size;
                }
                ++bands_before;
            }
        }
        {
            const size_t output_stride = strides != NULL && strides[p] != 0u
                ? strides[p] : (size_t)v->plane_width;
            pyr.base = base_planes[p];
            rc = tc_pyramid_synthesize_plane(&pyr, planes_out[p], output_stride);
        }
    pyramid_decode_plane_fail:
        /* pyr.base/escape 借用本函数分配的缓冲；只释放 bands。 */
        for (uint32_t k = 0u; k < TC_PYRAMID_MAX_LEVELS; ++k) {
            for (uint32_t b = 0u; b < TC_PYRAMID_MAX_BANDS_PER_LEVEL; ++b) {
                tc_free(pyr.levels[k].bands[b].coeffs);
            }
        }
        tc_free(escape);
        if (rc != TC_OK) { goto pyramid_decode_cleanup; }
    }
    for (uint32_t i = 0u; i < outer_fh->slice_count; ++i) {
        out_info->slice_status[i] = TC_FRAME_SLICE_OK;
    }

pyramid_decode_cleanup:
    for (uint32_t p = 0u; p < planes; ++p) {
        tc_free(base_planes != NULL ? base_planes[p] : NULL);
    }
    tc_free(base_planes);
    tc_free(base_strides);
    tc_free(views);
    return rc;
}

int32_t tc_v7b_frame_encode(const topos_frame_config* source_config,
                            const topos_frame_input* source_input,
                            uint32_t max_dim,
                            uint8_t* out, size_t out_cap,
                            topos_frame_stats* stats)
{
    /* The public V7-B writer stays on the stable minor-2 Rice layout.  The
     * minor-3 transform enhancement failed its RD4-08 quality/size/speed
     * gates and its writer was removed; the minor-3 reader below remains so
     * already-written experimental packets stay decodable until the shared
     * pyramid writer (minor 4) replaces it. */
    return v7b_frame_encode_rice(source_config, source_input, max_dim, out, out_cap, stats);
}

int32_t tc_frame_encode_scalable(const topos_frame_config* cfg,
                                 const topos_frame_input* input,
                                 uint32_t base_max_dim,
                                 uint8_t* out, size_t out_cap,
                                 topos_frame_stats* stats)
{
    if (base_max_dim == 0u) { base_max_dim = TC_CODEC_AUTO_2K_MAX_DIM; }
    if (base_max_dim > TC_CODEC_AUTO_2K_MAX_DIM) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED,
                     "v7b public base_max_dim %u > %u",
                     (unsigned)base_max_dim,
                     (unsigned)TC_CODEC_AUTO_2K_MAX_DIM);
        return TC_ERR_LIMIT_EXCEEDED;
    }
    return tc_v7b_frame_encode(cfg, input, base_max_dim, out, out_cap, stats);
}

static int32_t v7b_reconstruct_transform_plane(const uint16_t* base,
                                                uint32_t base_w, uint32_t base_h,
                                                size_t base_stride,
                                                const uint16_t* enhancement,
                                                uint32_t width, uint32_t height,
                                                size_t enhancement_stride,
                                                uint16_t* output, size_t output_stride,
                                                uint8_t bit_depth)
{
    if (base == NULL || enhancement == NULL || output == NULL || base_w == 0u ||
        base_h == 0u || width == 0u || height == 0u || base_w > width ||
        base_h > height || base_stride < base_w || enhancement_stride < width ||
        output_stride < width || bit_depth != 10u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b transform reconstruction arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* The enhancement packet is 12-bit, so its neutral code is 2048 even
     * though the reconstructed output is the original 10-bit plane. */
    const int32_t center = 1 << 11u;
    const int32_t max_value = (1 << bit_depth) - 1;
    for (uint32_t y = 0u; y < height; ++y) {
        const uint16_t* enhancement_row = enhancement + (size_t)y * enhancement_stride;
        uint16_t* output_row = output + (size_t)y * output_stride;
        for (uint32_t x = 0u; x < width; ++x) {
            const int32_t prediction = tc_base_upsample_pixel_unchecked_u16(
                base, base_w, base_h, base_stride, width, height, x, y,
                (uint32_t)max_value);
            int32_t value = prediction + (int32_t)enhancement_row[x] - center;
            if (value < 0) { value = 0; }
            if (value > max_value) { value = max_value; }
            output_row[x] = (uint16_t)value;
        }
    }
    return TC_OK;
}

static int32_t v7b_decode_transform_full(const uint8_t* packet,
                                          const topos_frame_header* outer_fh,
                                          const tc_v7b_base_view* base,
                                          uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                          const size_t strides[TC_FRAME_MAX_PLANES],
                                          topos_frame_output* out_info,
                                          tc_v7b_decode_stats* stats)
{
    uint16_t* base_planes[TC_FRAME_MAX_PLANES] = { NULL, NULL, NULL, NULL };
    size_t base_strides[TC_FRAME_MAX_PLANES] = { 0u, 0u, 0u, 0u };
    uint16_t* enhancement_planes[TC_FRAME_MAX_PLANES] = { NULL, NULL, NULL, NULL };
    size_t enhancement_strides[TC_FRAME_MAX_PLANES] = { 0u, 0u, 0u, 0u };
    int32_t rc = TC_OK;
    for (uint32_t p = 0u; p < outer_fh->plane_count; ++p) {
        rc = v7b_plane_alloc(base->header.plane_visible_w[p],
                             base->header.plane_visible_h[p],
                             &base_planes[p], &base_strides[p]);
        if (rc != TC_OK) { goto v7b_transform_cleanup; }
        rc = v7b_plane_alloc(outer_fh->plane_visible_w[p],
                             outer_fh->plane_visible_h[p],
                             &enhancement_planes[p], &enhancement_strides[p]);
        if (rc != TC_OK) { goto v7b_transform_cleanup; }
    }
    topos_frame_output base_output;
    rc = tc_frame_decode(base->packet, base->segment.payload_size,
                         base_planes, base_strides, &base_output);
    if (rc != TC_OK) { goto v7b_transform_cleanup; }
    tc_v7b_segment segment;
    rc = v7b_get_segment(&base->directory, 1u, &segment);
    if (rc != TC_OK) { goto v7b_transform_cleanup; }
    const uint8_t* enhancement_packet = packet + segment.payload_offset;
    if (tc_crc32(enhancement_packet, segment.payload_size) != segment.payload_crc32) {
        tc_set_error(TC_ERR_CHECKSUM_MISMATCH, "v7b transform enhancement crc");
        rc = TC_ERR_CHECKSUM_MISMATCH;
        goto v7b_transform_cleanup;
    }
    topos_frame_header enhancement_header;
    rc = tc_frame_header_decode(enhancement_packet, segment.payload_size,
                                &enhancement_header);
    if (rc != TC_OK) { goto v7b_transform_cleanup; }
    if (enhancement_header.visible_width != outer_fh->visible_width ||
        enhancement_header.visible_height != outer_fh->visible_height ||
        enhancement_header.plane_count != outer_fh->plane_count ||
        enhancement_header.pixel_format != outer_fh->pixel_format ||
        enhancement_header.alpha_mode != 0u || enhancement_header.bit_depth != 12u) {
        tc_set_error(TC_ERR_MALFORMED, "v7b transform enhancement contract");
        rc = TC_ERR_MALFORMED;
        goto v7b_transform_cleanup;
    }
    topos_frame_output enhancement_output;
    rc = tc_frame_decode(enhancement_packet, segment.payload_size,
                         enhancement_planes, enhancement_strides, &enhancement_output);
    if (rc != TC_OK) { goto v7b_transform_cleanup; }
    if (stats != NULL) {
        stats->segments_requested++;
        stats->segments_read++;
        stats->bytes_read += segment.payload_size;
    }
    for (uint32_t p = 0u; p < outer_fh->plane_count; ++p) {
        const size_t output_stride = strides != NULL && strides[p] != 0u
            ? strides[p] : (size_t)outer_fh->plane_visible_w[p];
        rc = v7b_reconstruct_transform_plane(
            base_planes[p], base->header.plane_visible_w[p],
            base->header.plane_visible_h[p], base_strides[p], enhancement_planes[p],
            outer_fh->plane_visible_w[p], outer_fh->plane_visible_h[p],
            enhancement_strides[p], planes_out[p], output_stride, outer_fh->bit_depth);
        if (rc != TC_OK) { goto v7b_transform_cleanup; }
    }
    for (uint32_t i = 0u; i < outer_fh->slice_count; ++i) {
        out_info->slice_status[i] = TC_FRAME_SLICE_OK;
    }

v7b_transform_cleanup:
    for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
        tc_free(base_planes[p]);
        tc_free(enhancement_planes[p]);
    }
    return rc;
}

int32_t tc_v7b_frame_decode(const uint8_t* packet, size_t packet_size,
                            uint32_t max_dim, tc_v7b_decode_mode mode,
                            uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                            const size_t strides[TC_FRAME_MAX_PLANES],
                            topos_frame_output* out_info,
                            tc_v7b_decode_stats* stats)
{
    if (packet == NULL || out_info == NULL || max_dim == 0u ||
        max_dim > TC_PLANE_MAX_DIM || mode > TC_V7B_DECODE_FULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b decode arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = (uint32_t)sizeof(*out_info);
    out_info->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    v7b_stats_reset(stats);
    topos_frame_header outer_fh;
    int32_t rc = tc_v7_packet_header_decode(packet, packet_size, &outer_fh);
    if (rc != TC_OK) { return rc; }
    tc_v7b_base_view base;
    rc = v7b_prepare_base(packet, packet_size, &outer_fh, max_dim, &base);
    if (rc != TC_OK) { return rc; }
    v7b_record_base_stats(&outer_fh, &base, mode == TC_V7B_DECODE_BASE_ONLY, stats);
    if (mode == TC_V7B_DECODE_BASE_ONLY) {
        if (planes_out == NULL) {
            tc_fill_output_info(&base.header, out_info);
            return TC_OK;
        }
        return tc_frame_decode(base.packet, base.segment.payload_size,
                               planes_out, strides, out_info);
    }

    /* TRAW（批 3）：V7-B FULL（残差合并）路径对 pf=3 显式门控——
     * minor-2 量化残差路径本身处于 reference 状态（P4 匹配质量门禁未过，
     * 视频默认 writer 同样被门控），UHD 大几何下未收敛。TRAW 完整画质
     * 由 rans2（V7-R2）直连流承载（位还原）；V7-B TRAW 文件的消费面 =
     * base-only 预览/代理档（预览/ROI 段跳过已验证）。 */
    if (outer_fh.pixel_format == 3u) {
        tc_set_error(TC_ERR_NOT_IMPLEMENTED,
                     "TRAW V7-B full (residual) decode is gated behind V7-B "
                     "P4 quality gates; use the rans2 stream for full quality");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    tc_fill_output_info(&outer_fh, out_info);
    rc = v7b_validate_planes(&outer_fh, planes_out, strides);
    if (rc != TC_OK || planes_out == NULL) { return rc; }
    if (base.directory.directory_version_minor ==
        TC_V7B_DIRECTORY_VERSION_MINOR_TRANSFORM_EXPERIMENTAL) {
        return v7b_decode_transform_full(packet, &outer_fh, &base, planes_out,
                                         strides, out_info, stats);
    }
    if (base.directory.directory_version_minor ==
        TC_V7B_DIRECTORY_VERSION_MINOR_PYRAMID) {
        return v7b_decode_pyramid_full(packet, &outer_fh, &base, planes_out,
                                       strides, out_info, stats);
    }
    uint16_t* base_planes[TC_FRAME_MAX_PLANES] = { NULL, NULL, NULL, NULL };
    size_t base_strides[TC_FRAME_MAX_PLANES] = { 0u, 0u, 0u, 0u };
    int32_t* residual[TC_FRAME_MAX_PLANES] = { NULL, NULL, NULL, NULL };
    for (uint32_t p = 0u; p < outer_fh.plane_count; ++p) {
        rc = v7b_plane_alloc(base.header.plane_visible_w[p], base.header.plane_visible_h[p],
                             &base_planes[p], &base_strides[p]);
        if (rc != TC_OK) { goto v7b_decode_cleanup; }
    }
    topos_frame_output base_output;
    rc = tc_frame_decode(base.packet, base.segment.payload_size,
                         base_planes, base_strides, &base_output);
    if (rc != TC_OK) { goto v7b_decode_cleanup; }
    for (uint32_t p = 0u; p < outer_fh.plane_count; ++p) {
        tc_v7b_segment segment;
        rc = v7b_get_segment(&base.directory, 1u + p, &segment);
        if (rc != TC_OK) { goto v7b_decode_cleanup; }
        const uint8_t* data = packet + segment.payload_offset;
        if (tc_crc32(data, segment.payload_size) != segment.payload_crc32) {
            tc_set_error(TC_ERR_CHECKSUM_MISMATCH, "v7b residual segment %u crc", p);
            rc = TC_ERR_CHECKSUM_MISMATCH;
            goto v7b_decode_cleanup;
        }
        size_t count = 0u;
        size_t bytes = 0u;
        if (!tc_umul_size((size_t)outer_fh.plane_visible_w[p],
                          (size_t)outer_fh.plane_visible_h[p], &count) ||
            !tc_umul_size(count, sizeof(int32_t), &bytes)) {
            rc = TC_ERR_LIMIT_EXCEEDED;
            goto v7b_decode_cleanup;
        }
        residual[p] = (int32_t*)tc_alloc(bytes);
        if (residual[p] == NULL) {
            rc = TC_ERR_OUT_OF_MEMORY;
            goto v7b_decode_cleanup;
        }
        rc = v7b_decode_residual(data, segment.payload_size, residual[p],
                                 outer_fh.plane_visible_w[p],
                                 outer_fh.plane_visible_h[p],
                                 outer_fh.plane_visible_w[p],
                                 base.directory.directory_version_minor);
        if (rc != TC_OK) { goto v7b_decode_cleanup; }
        if (stats != NULL) {
            stats->segments_requested++;
            stats->segments_read++;
            stats->bytes_read += segment.payload_size;
        }
        rc = tc_base_full_reconstruct_u16(
            base_planes[p], base.header.plane_visible_w[p], base.header.plane_visible_h[p],
            base_strides[p], residual[p], outer_fh.plane_visible_w[p],
            outer_fh.plane_visible_h[p], outer_fh.plane_visible_w[p],
            planes_out[p], strides != NULL && strides[p] != 0u
                ? strides[p] : (size_t)outer_fh.plane_visible_w[p], 1u, 1u,
            p == 3u ? 65535u : (uint16_t)(1u << (outer_fh.bit_depth - 1u)),
            outer_fh.bit_depth);
        if (rc != TC_OK) { goto v7b_decode_cleanup; }
        out_info->slice_status[1u + p] = TC_FRAME_SLICE_OK;
    }
    out_info->slice_status[0] = TC_FRAME_SLICE_OK;
    /* V7-R4/R5（P6，2026-09-21）：flags bit2 → 输出去块。全尺寸交付
     * （mode == FULL）在重建完成、交付前就地滤波；reduced/缩放交付
     * 不过滤（V7-R2 产品路径已在 codec.c:dec_frame_finalize 接入目标网格
     * 滤波，见 tc_deblock_plane_scaled；本 V7-B band 路径归后续批次）。
     * alpha 平面（plane 3）不过滤；强度 qp = 帧级 qp_base（产品流
     * 全帧单一 qp）；QPT2 流滤波强度随细化表同源。 */
    if (rc == TC_OK && mode == TC_V7B_DECODE_FULL
            && (outer_fh.flags & 0x0004u) != 0u) {
        const uint32_t qtbl = tc_qtbl_of_flags(outer_fh.flags);
        for (uint32_t p = 0; p < outer_fh.plane_count && p < 3u; ++p) {
            const size_t dstride = strides != NULL && strides[p] != 0u
                ? strides[p] : (size_t)outer_fh.plane_visible_w[p];
            tc_deblock_plane(planes_out[p], (int32_t)dstride,
                             outer_fh.plane_visible_w[p],
                             outer_fh.plane_visible_h[p],
                             (uint32_t)outer_fh.qp_base, qtbl,
                             (uint8_t)outer_fh.bit_depth);
        }
        out_info->concealed_slices = 99;
    }
    rc = TC_OK;

v7b_decode_cleanup:
    for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
        tc_free(base_planes[p]);
        tc_free(residual[p]);
    }
    return rc;
}

static int32_t v7b_target_geometry(const topos_frame_header* fh,
                                   uint32_t target_width, uint32_t target_height,
                                   uint32_t widths[TC_FRAME_MAX_PLANES],
                                   uint32_t heights[TC_FRAME_MAX_PLANES])
{
    if (fh == NULL || widths == NULL || heights == NULL || target_width == 0u ||
        target_height == 0u || target_width > fh->visible_width ||
        target_height > fh->visible_height || target_width > UINT16_MAX ||
        target_height > UINT16_MAX) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b reduced target geometry");
        return TC_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t p = 0u; p < fh->plane_count; ++p) {
        if (!tc_base_plane_dimensions(target_width, target_height, fh->pixel_format,
                                      p, fh->chroma_siting, &widths[p], &heights[p])) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b reduced plane %u geometry", p);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    return TC_OK;
}

static int32_t v7b_validate_target_planes(const topos_frame_header* fh,
                                          const uint32_t widths[TC_FRAME_MAX_PLANES],
                                          uint16_t* const planes[TC_FRAME_MAX_PLANES],
                                          const size_t strides[TC_FRAME_MAX_PLANES])
{
    if (fh == NULL || widths == NULL || planes == NULL) { return TC_OK; }
    for (uint32_t p = 0u; p < fh->plane_count; ++p) {
        if (planes[p] == NULL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b reduced output plane %u is NULL", p);
            return TC_ERR_INVALID_ARGUMENT;
        }
        const size_t stride = strides != NULL && strides[p] != 0u
            ? strides[p] : (size_t)widths[p];
        if (stride < (size_t)widths[p]) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT,
                         "v7b reduced output stride[%u]=%zu < width %u",
                         p, stride, widths[p]);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    return TC_OK;
}

int32_t tc_v7b_frame_decode_reduced(const uint8_t* packet, size_t packet_size,
                                    uint32_t target_width, uint32_t target_height,
                                    uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                    const size_t strides[TC_FRAME_MAX_PLANES],
                                    topos_frame_output* out_info,
                                    tc_v7b_decode_stats* stats)
{
    if (packet == NULL || out_info == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b reduced arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = (uint32_t)sizeof(*out_info);
    out_info->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    v7b_stats_reset(stats);

    topos_frame_header outer_fh;
    int32_t rc = tc_v7_packet_header_decode(packet, packet_size, &outer_fh);
    if (rc != TC_OK) { return rc; }
    uint32_t target_w[TC_FRAME_MAX_PLANES] = { 0u, 0u, 0u, 0u };
    uint32_t target_h[TC_FRAME_MAX_PLANES] = { 0u, 0u, 0u, 0u };
    rc = v7b_target_geometry(&outer_fh, target_width, target_height,
                             target_w, target_h);
    if (rc != TC_OK) { return rc; }
    tc_fill_output_info(&outer_fh, out_info);
    out_info->visible_width = (uint16_t)target_width;
    out_info->visible_height = (uint16_t)target_height;
    out_info->coded_width = (uint16_t)target_width;
    out_info->coded_height = (uint16_t)target_height;
    if (planes_out != NULL) {
        rc = v7b_validate_target_planes(&outer_fh, target_w, planes_out, strides);
        if (rc != TC_OK) { return rc; }
    }

    tc_v7b_base_view base;
    rc = v7b_prepare_base(packet, packet_size, &outer_fh,
                          TC_CODEC_AUTO_2K_MAX_DIM, &base);
    if (rc != TC_OK) { return rc; }
    v7b_record_base_stats(&outer_fh, &base, 1, stats);
    if (planes_out == NULL) { return TC_OK; }

    uint32_t base_w[TC_FRAME_MAX_PLANES] = { 0u, 0u, 0u, 0u };
    uint32_t base_h[TC_FRAME_MAX_PLANES] = { 0u, 0u, 0u, 0u };
    rc = v7b_target_geometry(&outer_fh, base.header.visible_width,
                             base.header.visible_height, base_w, base_h);
    if (rc != TC_OK) { return rc; }
    int same_geometry = 1;
    for (uint32_t p = 0u; p < outer_fh.plane_count; ++p) {
        if (base_w[p] != target_w[p] || base_h[p] != target_h[p]) {
            same_geometry = 0;
            break;
        }
    }
    if (same_geometry != 0) {
        return tc_frame_decode(base.packet, base.segment.payload_size,
                               planes_out, strides, out_info);
    }

    topos_frame_output base_info;
    uint16_t* base_planes[TC_FRAME_MAX_PLANES] = { NULL, NULL, NULL, NULL };
    size_t base_strides[TC_FRAME_MAX_PLANES] = { 0u, 0u, 0u, 0u };
    for (uint32_t p = 0u; p < outer_fh.plane_count; ++p) {
        rc = v7b_plane_alloc(base_w[p], base_h[p], &base_planes[p], &base_strides[p]);
        if (rc != TC_OK) { goto v7b_reduced_cleanup; }
    }
    rc = tc_frame_decode(base.packet, base.segment.payload_size,
                         base_planes, base_strides, &base_info);
    if (rc != TC_OK) { goto v7b_reduced_cleanup; }
    for (uint32_t p = 0u; p < outer_fh.plane_count; ++p) {
        const size_t output_stride = strides != NULL && strides[p] != 0u
            ? strides[p] : (size_t)target_w[p];
        int ok = 0;
        if (target_w[p] <= base_w[p] && target_h[p] <= base_h[p]) {
            ok = tc_base_downsample_u16(base_planes[p], base_w[p], base_h[p],
                                        base_strides[p], planes_out[p], target_w[p],
                                        target_h[p], output_stride, outer_fh.bit_depth);
        } else if (target_w[p] >= base_w[p] && target_h[p] >= base_h[p]) {
            ok = tc_base_upsample_u16(base_planes[p], base_w[p], base_h[p],
                                      base_strides[p], planes_out[p], target_w[p],
                                      target_h[p], output_stride, outer_fh.bit_depth);
        }
        if (ok == 0) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b reduced mixed-axis geometry plane %u", p);
            rc = TC_ERR_INVALID_ARGUMENT;
            goto v7b_reduced_cleanup;
        }
    }
    *out_info = base_info;
    out_info->visible_width = (uint16_t)target_width;
    out_info->visible_height = (uint16_t)target_height;
    out_info->coded_width = (uint16_t)target_width;
    out_info->coded_height = (uint16_t)target_height;
    rc = TC_OK;

v7b_reduced_cleanup:
    for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
        tc_free(base_planes[p]);
    }
    return rc;
}
