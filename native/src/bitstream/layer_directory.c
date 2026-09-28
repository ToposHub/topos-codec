#include "layer_directory.h"

#include <string.h>

#include "../common/checked.h"
#include "../common/crc32.h"
#include "../common/endian.h"
#include "../common/error.h"

static int32_t dir_fail(int32_t code, const char* what, unsigned long long value)
{
    tc_set_error(code, "v7a directory %s: %llu", what, value);
    return code;
}

static int32_t validate_v7_writer_geometry(const topos_frame_header* fh,
                                           const tc_v7_slice_geometry* slices)
{
    uint32_t covered[4] = { 0u, 0u, 0u, 0u };
    uint8_t seen_plane[4] = { 0u, 0u, 0u, 0u };
    for (uint32_t i = 0u; i < (uint32_t)fh->slice_count; ++i) {
        const tc_v7_slice_geometry* slice = &slices[i];
        if (slice->plane_index >= fh->plane_count || slice->block_h == 0u) {
            return dir_fail(TC_ERR_MALFORMED, "writer slice identity", i);
        }
        uint32_t end = 0u;
        uint32_t rows = fh->plane_block_rows[slice->plane_index];
        if (rows == 0u || slice->block_y0 != covered[slice->plane_index] ||
            !tc_uadd_u32(slice->block_y0, (uint32_t)slice->block_h, &end) ||
            end > rows) {
            return dir_fail(TC_ERR_MALFORMED, "writer slice geometry", i);
        }
        covered[slice->plane_index] = end;
        seen_plane[slice->plane_index] = 1u;
    }
    for (uint32_t plane = 0u; plane < (uint32_t)fh->plane_count; ++plane) {
        if (seen_plane[plane] == 0u || covered[plane] != fh->plane_block_rows[plane]) {
            return dir_fail(TC_ERR_MALFORMED, "writer slice coverage", plane);
        }
    }
    return TC_OK;
}

int32_t tc_v7_packet_header_decode(const uint8_t* packet, size_t packet_size,
                                   topos_frame_header* fh)
{
    return tc_frame_header_decode_v7a(packet, packet_size, fh);
}

int32_t tc_v7_directory_write(const topos_frame_header* fh,
                              const tc_v7_slice_geometry* slices,
                              const tc_v7_segment_input* segments,
                              uint8_t* out, size_t out_cap,
                              size_t* packet_size)
{
    if (fh == NULL || slices == NULL || segments == NULL || packet_size == NULL ||
        (out == NULL && out_cap != 0u)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a writer argument is NULL/invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    int32_t rc = tc_frame_header_validate_v7a_contract(fh);
    if (rc != TC_OK) { return rc; }
    if (fh->slice_count == 0u || fh->slice_count > TC_MAX_SLICE_COUNT ||
        fh->plane_count == 0u || fh->plane_count > 4u) {
        return dir_fail(TC_ERR_LIMIT_EXCEEDED, "writer header count", fh->slice_count);
    }
    topos_frame_header geometry = *fh;
    rc = tc_frame_derive_geometry(&geometry);
    if (rc != TC_OK) { return rc; }
    rc = validate_v7_writer_geometry(&geometry, slices);
    if (rc != TC_OK) { return rc; }

    uint32_t segment_count = 0u;
    uint32_t slice_bytes = 0u;
    uint32_t segment_bytes = 0u;
    uint32_t directory_size = TC_V7_TPLD_FIXED_SIZE;
    if (!tc_umul_u32((uint32_t)fh->slice_count, TC_V7_BAND_COUNT, &segment_count) ||
        !tc_umul_u32((uint32_t)fh->slice_count, TC_V7_SLICE_DESC_SIZE, &slice_bytes) ||
        !tc_umul_u32(segment_count, TC_V7_SEGMENT_DESC_SIZE, &segment_bytes) ||
        !tc_uadd_u32(directory_size, TC_V7_LAYER_DESC_SIZE, &directory_size) ||
        !tc_uadd_u32(directory_size, slice_bytes, &directory_size) ||
        !tc_uadd_u32(directory_size, segment_bytes, &directory_size) ||
        directory_size > TC_V7_MAX_DIRECTORY_SIZE) {
        return dir_fail(TC_ERR_LIMIT_EXCEEDED, "writer directory arithmetic", fh->slice_count);
    }

    size_t payload_size = 0u;
    for (uint32_t i = 0u; i < segment_count; ++i) {
        if (segments[i].size != 0u && segments[i].data == NULL) {
            return dir_fail(TC_ERR_INVALID_ARGUMENT, "writer segment data", i);
        }
        if (!tc_uadd_size(payload_size, (size_t)segments[i].size, &payload_size)) {
            return dir_fail(TC_ERR_LIMIT_EXCEEDED, "writer payload arithmetic", i);
        }
    }
    size_t packet_size_sz = (size_t)TC_FRAME_HEADER_SIZE + (size_t)directory_size;
    if (!tc_uadd_size(packet_size_sz, payload_size, &packet_size_sz) ||
        packet_size_sz > (size_t)TC_MAX_PACKET_SIZE || packet_size_sz > UINT32_MAX) {
        return dir_fail(TC_ERR_LIMIT_EXCEEDED, "writer packet size", packet_size_sz);
    }
    *packet_size = packet_size_sz;
    if (out == NULL || out_cap < packet_size_sz) {
        tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "v7a writer needs %zu bytes", packet_size_sz);
        return TC_ERR_BUFFER_TOO_SMALL;
    }

    memset(out + TC_FRAME_HEADER_SIZE, 0, directory_size);
    uint8_t* directory = out + TC_FRAME_HEADER_SIZE;
    memcpy(directory, "TPLD", 4u);
    tc_store_be16(directory + 4u, 1u);
    tc_store_be16(directory + 6u, 0u);
    tc_store_be16(directory + 12u, 1u);
    tc_store_be16(directory + 14u, fh->slice_count);
    directory[16] = TC_V7_BAND_COUNT;
    directory[17] = fh->plane_count;
    tc_store_be16(directory + 18u, TC_V7_LAYER_DESC_SIZE);
    tc_store_be16(directory + 20u, TC_V7_SLICE_DESC_SIZE);
    tc_store_be16(directory + 22u, TC_V7_SEGMENT_DESC_SIZE);
    tc_store_be32(directory + 24u, directory_size);
    tc_store_be32(directory + 28u, TC_FRAME_HEADER_SIZE + directory_size);

    uint8_t* layer = directory + TC_V7_TPLD_FIXED_SIZE;
    layer[0] = 0u;
    layer[1] = 0u;
    tc_store_be32(layer + 8u, fh->slice_count);
    tc_store_be32(layer + 16u, segment_count);
    tc_store_be16(layer + 20u, 1u);

    uint8_t* slice_desc = layer + TC_V7_LAYER_DESC_SIZE;
    for (uint32_t i = 0u; i < (uint32_t)fh->slice_count; ++i) {
        uint8_t* raw = slice_desc + (size_t)i * TC_V7_SLICE_DESC_SIZE;
        tc_store_be16(raw + 0u, (uint16_t)i);
        tc_store_be32(raw + 4u, i * TC_V7_BAND_COUNT);
        tc_store_be32(raw + 8u, TC_V7_BAND_COUNT);
        tc_store_be32(raw + 12u, slices[i].block_y0);
        tc_store_be16(raw + 16u, slices[i].block_h);
        raw[18] = slices[i].plane_index;
        raw[19] = TC_V7_BAND_COUNT;
    }

    uint8_t* segment_desc = slice_desc + slice_bytes;
    size_t cursor = (size_t)TC_FRAME_HEADER_SIZE + (size_t)directory_size;
    for (uint32_t i = 0u; i < segment_count; ++i) {
        uint8_t* raw = segment_desc + (size_t)i * TC_V7_SEGMENT_DESC_SIZE;
        uint32_t slice = i / TC_V7_BAND_COUNT;
        uint32_t band = i % TC_V7_BAND_COUNT;
        tc_store_be16(raw + 12u, (uint16_t)slice);
        raw[14] = slices[slice].plane_index;
        raw[15] = (uint8_t)band;
        if (segments[i].size != 0u) {
            tc_store_be32(raw + 0u, (uint32_t)cursor);
            tc_store_be32(raw + 4u, segments[i].size);
            tc_store_be32(raw + 8u, tc_crc32(segments[i].data, segments[i].size));
            memmove(out + cursor, segments[i].data, segments[i].size);
            cursor += (size_t)segments[i].size;
        }
    }
    if (cursor != packet_size_sz) {
        return dir_fail(TC_ERR_MALFORMED, "writer payload cursor", cursor);
    }
    tc_store_be32(directory + 32u, 0u);
    tc_store_be32(directory + 32u, tc_crc32(directory, directory_size));

    topos_frame_header header = *fh;
    header.frame_packet_size = (uint32_t)packet_size_sz;
    rc = tc_frame_header_encode_v7a(&header, out);
    if (rc != TC_OK) { return rc; }
    return TC_OK;
}

static uint32_t directory_crc_without_field(const uint8_t* directory, uint32_t size)
{
    static const uint8_t zeros[4] = { 0u, 0u, 0u, 0u };
    uint32_t crc = 0u;
    crc = tc_crc32_update(crc, directory, 32u);
    crc = tc_crc32_update(crc, zeros, sizeof(zeros));
    crc = tc_crc32_update(crc, directory + 36u, (size_t)size - 36u);
    return crc;
}

static int32_t get_layer_raw(const tc_v7_directory_view* view, uint32_t index,
                             const uint8_t** raw)
{
    if (view == NULL || raw == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a layer getter has NULL argument");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (index >= 1u) {
        return dir_fail(TC_ERR_INVALID_ARGUMENT, "layer index", index);
    }
    size_t off = (size_t)TC_V7_TPLD_FIXED_SIZE;
    if (!tc_offset_in_bounds((size_t)view->directory_size, off,
                             TC_V7_LAYER_DESC_SIZE)) {
        return dir_fail(TC_ERR_MALFORMED, "layer descriptor bounds", index);
    }
    *raw = view->directory + off;
    return TC_OK;
}

static int32_t get_slice_raw(const tc_v7_directory_view* view, uint32_t index,
                             const uint8_t** raw)
{
    if (view == NULL || raw == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a slice getter has NULL argument");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (index >= (uint32_t)view->slice_count) {
        return dir_fail(TC_ERR_INVALID_ARGUMENT, "slice index", index);
    }
    size_t off = (size_t)TC_V7_TPLD_FIXED_SIZE + TC_V7_LAYER_DESC_SIZE;
    size_t add = (size_t)index * TC_V7_SLICE_DESC_SIZE;
    if (!tc_uadd_size(off, add, &off) ||
        !tc_offset_in_bounds((size_t)view->directory_size, off,
                             TC_V7_SLICE_DESC_SIZE)) {
        return dir_fail(TC_ERR_MALFORMED, "slice descriptor bounds", index);
    }
    *raw = view->directory + off;
    return TC_OK;
}

static int32_t get_segment_raw(const tc_v7_directory_view* view, uint32_t index,
                               const uint8_t** raw)
{
    if (view == NULL || raw == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a segment getter has NULL argument");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (index >= view->segment_count) {
        return dir_fail(TC_ERR_INVALID_ARGUMENT, "segment index", index);
    }
    size_t off = (size_t)TC_V7_TPLD_FIXED_SIZE + TC_V7_LAYER_DESC_SIZE;
    size_t slice_bytes = (size_t)view->slice_count * TC_V7_SLICE_DESC_SIZE;
    if (!tc_uadd_size(off, slice_bytes, &off)) {
        return dir_fail(TC_ERR_MALFORMED, "slice descriptor offset", index);
    }
    size_t segment_bytes = (size_t)index * TC_V7_SEGMENT_DESC_SIZE;
    if (!tc_uadd_size(off, segment_bytes, &off) ||
        !tc_offset_in_bounds((size_t)view->directory_size, off,
                             TC_V7_SEGMENT_DESC_SIZE)) {
        return dir_fail(TC_ERR_MALFORMED, "segment descriptor bounds", index);
    }
    *raw = view->directory + off;
    return TC_OK;
}

int32_t tc_v7_directory_get_layer(const tc_v7_directory_view* view, uint32_t index,
                                  tc_v7_layer_desc* out)
{
    const uint8_t* raw = NULL;
    int32_t rc = get_layer_raw(view, index, &raw);
    if (rc != TC_OK || out == NULL) {
        if (rc == TC_OK) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a layer output is NULL");
            return TC_ERR_INVALID_ARGUMENT;
        }
        return rc;
    }
    out->layer_id = raw[0];
    out->layer_kind = raw[1];
    out->layer_flags = tc_load_be16(raw + 2u);
    out->first_slice_descriptor = tc_load_be32(raw + 4u);
    out->slice_count = tc_load_be32(raw + 8u);
    out->first_segment_descriptor = tc_load_be32(raw + 12u);
    out->segment_count = tc_load_be32(raw + 16u);
    out->band_layout_version = tc_load_be16(raw + 20u);
    return TC_OK;
}

int32_t tc_v7_directory_get_slice(const tc_v7_directory_view* view, uint32_t index,
                                  tc_v7_slice_desc* out)
{
    const uint8_t* raw = NULL;
    int32_t rc = get_slice_raw(view, index, &raw);
    if (rc != TC_OK || out == NULL) {
        if (rc == TC_OK) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a slice output is NULL");
            return TC_ERR_INVALID_ARGUMENT;
        }
        return rc;
    }
    out->slice_index = tc_load_be16(raw + 0u);
    out->slice_flags = tc_load_be16(raw + 2u);
    out->first_segment_index = tc_load_be32(raw + 4u);
    out->segment_count = tc_load_be32(raw + 8u);
    out->block_y0 = tc_load_be32(raw + 12u);
    out->block_h = tc_load_be16(raw + 16u);
    out->plane_index = raw[18];
    out->band_count = raw[19];
    return TC_OK;
}

int32_t tc_v7_directory_get_segment(const tc_v7_directory_view* view, uint32_t index,
                                    tc_v7_segment_desc* out)
{
    const uint8_t* raw = NULL;
    int32_t rc = get_segment_raw(view, index, &raw);
    if (rc != TC_OK || out == NULL) {
        if (rc == TC_OK) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a segment output is NULL");
            return TC_ERR_INVALID_ARGUMENT;
        }
        return rc;
    }
    out->payload_offset = tc_load_be32(raw + 0u);
    out->payload_size = tc_load_be32(raw + 4u);
    out->payload_crc32 = tc_load_be32(raw + 8u);
    out->slice_index = tc_load_be16(raw + 12u);
    out->plane_index = raw[14];
    out->band_index = raw[15];
    out->segment_flags = tc_load_be16(raw + 16u);
    return TC_OK;
}

int32_t tc_v7_directory_segment_span(const tc_v7_directory_view* view, uint32_t index,
                                     const uint8_t** data, uint32_t* size)
{
    if (data == NULL || size == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a segment span output is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    tc_v7_segment_desc segment;
    int32_t rc = tc_v7_directory_get_segment(view, index, &segment);
    if (rc != TC_OK) { return rc; }
    if (segment.payload_size == 0u) {
        *data = NULL;
        *size = 0u;
        return TC_OK;
    }
    *data = view->packet + (size_t)segment.payload_offset;
    *size = segment.payload_size;
    return TC_OK;
}

int32_t tc_v7_directory_parse(const uint8_t* packet, size_t packet_size,
                              const topos_frame_header* fh,
                              tc_v7_directory_view* out)
{
    if (out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a directory output is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (packet == NULL || fh == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a directory packet/header is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }

    int32_t rc = tc_frame_header_validate_v7a_contract(fh);
    if (rc != TC_OK) { return rc; }
    if (packet_size != (size_t)fh->frame_packet_size) {
        return dir_fail(TC_ERR_MALFORMED, "packet size/header size", packet_size);
    }
    if (packet_size > (size_t)TC_MAX_PACKET_SIZE) {
        return dir_fail(TC_ERR_LIMIT_EXCEEDED, "packet size", packet_size);
    }
    if (fh->slice_count == 0u || fh->slice_count > TC_MAX_SLICE_COUNT ||
        fh->plane_count == 0u || fh->plane_count > 4u) {
        return dir_fail(TC_ERR_LIMIT_EXCEEDED, "header count", fh->slice_count);
    }
    if (!tc_offset_in_bounds(packet_size, TC_FRAME_HEADER_SIZE,
                             TC_V7_TPLD_FIXED_SIZE)) {
        return dir_fail(TC_ERR_TRUNCATED, "fixed header", packet_size);
    }
    const uint8_t* directory = packet + TC_FRAME_HEADER_SIZE;
    if (memcmp(directory, "TPLD", 4u) != 0) {
        return dir_fail(TC_ERR_MALFORMED, "magic", 0u);
    }
    if (tc_load_be16(directory + 4u) != 1u || tc_load_be16(directory + 6u) != 0u) {
        return dir_fail(TC_ERR_UNSUPPORTED_VERSION, "directory version",
                        ((unsigned long long)tc_load_be16(directory + 4u) << 16) |
                        (unsigned long long)tc_load_be16(directory + 6u));
    }
    if (tc_load_be32(directory + 8u) != 0u) {
        return dir_fail(TC_ERR_UNSUPPORTED_VERSION, "directory flags",
                        tc_load_be32(directory + 8u));
    }
    uint16_t layer_count = tc_load_be16(directory + 12u);
    uint16_t slice_count = tc_load_be16(directory + 14u);
    uint8_t band_count = directory[16];
    uint8_t plane_count = directory[17];
    if (layer_count != 1u || slice_count != fh->slice_count ||
        band_count != TC_V7_BAND_COUNT || plane_count != fh->plane_count) {
        return dir_fail(TC_ERR_MALFORMED, "fixed count contract", slice_count);
    }
    if (tc_load_be16(directory + 18u) != TC_V7_LAYER_DESC_SIZE ||
        tc_load_be16(directory + 20u) != TC_V7_SLICE_DESC_SIZE ||
        tc_load_be16(directory + 22u) != TC_V7_SEGMENT_DESC_SIZE) {
        return dir_fail(TC_ERR_UNSUPPORTED_VERSION, "descriptor size", 0u);
    }
    uint32_t segment_count = 0u;
    uint32_t slice_bytes = 0u;
    uint32_t segment_bytes = 0u;
    uint32_t expected_size = TC_V7_TPLD_FIXED_SIZE;
    if (!tc_umul_u32((uint32_t)slice_count, TC_V7_SLICE_DESC_SIZE, &slice_bytes) ||
        !tc_umul_u32((uint32_t)slice_count, TC_V7_BAND_COUNT, &segment_count) ||
        !tc_umul_u32(segment_count, TC_V7_SEGMENT_DESC_SIZE, &segment_bytes) ||
        !tc_uadd_u32(expected_size, TC_V7_LAYER_DESC_SIZE, &expected_size) ||
        !tc_uadd_u32(expected_size, slice_bytes, &expected_size) ||
        !tc_uadd_u32(expected_size, segment_bytes, &expected_size)) {
        return dir_fail(TC_ERR_LIMIT_EXCEEDED, "directory arithmetic", slice_count);
    }
    uint32_t directory_size = tc_load_be32(directory + 24u);
    uint32_t payload_offset = tc_load_be32(directory + 28u);
    uint32_t directory_end = 0u;
    if (directory_size > TC_V7_MAX_DIRECTORY_SIZE) {
        return dir_fail(TC_ERR_LIMIT_EXCEEDED, "directory size", directory_size);
    }
    if (directory_size != expected_size) {
        return dir_fail(TC_ERR_MALFORMED, "directory size", directory_size);
    }
    if (!tc_uadd_u32(TC_FRAME_HEADER_SIZE, directory_size, &directory_end)) {
        return dir_fail(TC_ERR_LIMIT_EXCEEDED, "directory end", directory_size);
    }
    if (payload_offset != directory_end) {
        return dir_fail(TC_ERR_MALFORMED, "payload offset", payload_offset);
    }
    if (!tc_offset_in_bounds(packet_size, TC_FRAME_HEADER_SIZE, (size_t)directory_size)) {
        return dir_fail(TC_ERR_TRUNCATED, "directory bounds", directory_size);
    }
    /* The field is not mutable input. Recompute with its four bytes treated as zero. */
    uint32_t expected_crc = directory_crc_without_field(directory, directory_size);
    if (expected_crc != tc_load_be32(directory + 32u)) {
        return dir_fail(TC_ERR_CHECKSUM_MISMATCH, "directory crc", 0u);
    }

    tc_v7_directory_view candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.packet = packet;
    candidate.packet_size = packet_size;
    candidate.frame_header = fh;
    candidate.directory = directory;
    candidate.directory_size = directory_size;
    candidate.payload_offset = payload_offset;
    candidate.slice_count = slice_count;
    candidate.plane_count = plane_count;
    candidate.segment_count = segment_count;
    candidate.layer_descriptors = directory + TC_V7_TPLD_FIXED_SIZE;
    candidate.slice_descriptors = candidate.layer_descriptors + TC_V7_LAYER_DESC_SIZE;
    candidate.segment_descriptors = candidate.slice_descriptors + slice_bytes;

    tc_v7_layer_desc layer;
    rc = tc_v7_directory_get_layer(&candidate, 0u, &layer);
    if (rc != TC_OK) { return rc; }
    if (layer.layer_id != 0u || layer.layer_kind != 0u || layer.layer_flags != 0u ||
        layer.first_slice_descriptor != 0u || layer.slice_count != slice_count ||
        layer.first_segment_descriptor != 0u || layer.segment_count != segment_count ||
        layer.band_layout_version != 1u ||
        tc_load_be16(candidate.layer_descriptors + 22u) != 0u) {
        return dir_fail(TC_ERR_UNSUPPORTED_VERSION, "layer contract", layer.layer_kind);
    }

    uint32_t covered[4] = { 0u, 0u, 0u, 0u };
    uint8_t seen_plane[4] = { 0u, 0u, 0u, 0u };
    for (uint32_t i = 0u; i < (uint32_t)slice_count; ++i) {
        tc_v7_slice_desc slice;
        rc = tc_v7_directory_get_slice(&candidate, i, &slice);
        if (rc != TC_OK) { return rc; }
        if (slice.slice_index != (uint16_t)i || slice.slice_flags != 0u ||
            slice.first_segment_index != i * TC_V7_BAND_COUNT ||
            slice.segment_count != TC_V7_BAND_COUNT || slice.band_count != TC_V7_BAND_COUNT ||
            slice.plane_index >= plane_count || slice.block_h == 0u) {
            return dir_fail(TC_ERR_MALFORMED, "slice contract", i);
        }
        uint32_t rows = fh->plane_block_rows[slice.plane_index];
        uint32_t end = 0u;
        if (rows == 0u || slice.block_y0 != covered[slice.plane_index] ||
            !tc_uadd_u32(slice.block_y0, (uint32_t)slice.block_h, &end) || end > rows) {
            return dir_fail(TC_ERR_MALFORMED, "slice geometry", i);
        }
        covered[slice.plane_index] = end;
        seen_plane[slice.plane_index] = 1u;
    }
    for (uint32_t plane = 0u; plane < (uint32_t)plane_count; ++plane) {
        if (seen_plane[plane] == 0u || covered[plane] != fh->plane_block_rows[plane]) {
            return dir_fail(TC_ERR_MALFORMED, "slice coverage", plane);
        }
    }

    size_t cursor = (size_t)payload_offset;
    for (uint32_t i = 0u; i < segment_count; ++i) {
        tc_v7_segment_desc segment;
        rc = tc_v7_directory_get_segment(&candidate, i, &segment);
        if (rc != TC_OK) { return rc; }
        uint32_t expected_slice = i / TC_V7_BAND_COUNT;
        uint32_t expected_band = i % TC_V7_BAND_COUNT;
        tc_v7_slice_desc owner;
        rc = tc_v7_directory_get_slice(&candidate, expected_slice, &owner);
        if (rc != TC_OK) { return rc; }
        if (segment.slice_index != (uint16_t)expected_slice ||
            segment.plane_index != owner.plane_index ||
            segment.band_index != (uint8_t)expected_band || segment.segment_flags != 0u) {
            return dir_fail(TC_ERR_MALFORMED, "segment identity", i);
        }
        if (tc_load_be16(candidate.segment_descriptors +
                         (size_t)i * TC_V7_SEGMENT_DESC_SIZE + 18u) != 0u) {
            return dir_fail(TC_ERR_UNSUPPORTED_VERSION, "segment reserved", i);
        }
        if (segment.payload_size == 0u) {
            if (segment.payload_offset != 0u || segment.payload_crc32 != 0u) {
                return dir_fail(TC_ERR_MALFORMED, "empty segment", i);
            }
            continue;
        }
        if ((size_t)segment.payload_offset != cursor ||
            !tc_offset_in_bounds(packet_size, (size_t)segment.payload_offset,
                                 (size_t)segment.payload_size)) {
            return dir_fail(TC_ERR_MALFORMED, "segment range", i);
        }
        if (!tc_uadd_size(cursor, (size_t)segment.payload_size, &cursor)) {
            return dir_fail(TC_ERR_LIMIT_EXCEEDED, "segment range overflow", i);
        }
    }
    if (cursor != packet_size) {
        return dir_fail(TC_ERR_MALFORMED, "payload trailing/gap", cursor);
    }
    *out = candidate;
    return TC_OK;
}
