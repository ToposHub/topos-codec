/* TPIC V7-A TPLD directory：只读、无分配、bounded view 解析器。
 * 规范：docs/bitstream_spec_v7.md §2-§8；决策：ADR-C029。
 */
#ifndef TOPOS_INTERNAL_LAYER_DIRECTORY_H
#define TOPOS_INTERNAL_LAYER_DIRECTORY_H

#include <stddef.h>
#include <stdint.h>

#include "frame_header.h"
#include "band_tokens.h"

#define TC_V7_TPLD_FIXED_SIZE 36u
#define TC_V7_LAYER_DESC_SIZE 24u
#define TC_V7_SLICE_DESC_SIZE 24u
#define TC_V7_SEGMENT_DESC_SIZE 20u
#define TC_V7_MAX_DIRECTORY_SIZE (64u * 1024u)

typedef struct tc_v7_layer_desc {
    uint8_t layer_id;
    uint8_t layer_kind;
    uint16_t layer_flags;
    uint32_t first_slice_descriptor;
    uint32_t slice_count;
    uint32_t first_segment_descriptor;
    uint32_t segment_count;
    uint16_t band_layout_version;
} tc_v7_layer_desc;

typedef struct tc_v7_slice_desc {
    uint16_t slice_index;
    uint16_t slice_flags;
    uint32_t first_segment_index;
    uint32_t segment_count;
    uint32_t block_y0;
    uint16_t block_h;
    uint8_t plane_index;
    uint8_t band_count;
} tc_v7_slice_desc;

typedef struct tc_v7_segment_desc {
    uint32_t payload_offset;
    uint32_t payload_size;
    uint32_t payload_crc32;
    uint16_t slice_index;
    uint8_t plane_index;
    uint8_t band_index;
    uint16_t segment_flags;
} tc_v7_segment_desc;

typedef struct tc_v7_directory_view {
    const uint8_t* packet;
    size_t packet_size;
    const topos_frame_header* frame_header; /* 不拥有；packet/header 调用期有效 */
    const uint8_t* directory; /* packet + TC_FRAME_HEADER_SIZE；不拥有 */
    uint32_t directory_size;
    uint32_t payload_offset;
    uint16_t slice_count;
    uint8_t plane_count;
    uint32_t segment_count;
    const uint8_t* layer_descriptors;
    const uint8_t* slice_descriptors;
    const uint8_t* segment_descriptors;
} tc_v7_directory_view;

typedef struct tc_v7_slice_geometry {
    uint32_t block_y0;
    uint16_t block_h;
    uint8_t plane_index;
} tc_v7_slice_geometry;

typedef struct tc_v7_segment_input {
    const uint8_t* data;
    uint32_t size;
} tc_v7_segment_input;

/* Build one canonical V7-A packet without allocating or copying a full-size
 * staging frame.  The caller owns segment payloads; non-empty payloads are
 * copied directly into the final packet after the directory.  A NULL/zero
 * output buffer is a size probe and returns TC_ERR_BUFFER_TOO_SMALL. */
int32_t tc_v7_directory_write(const topos_frame_header* fh,
                              const tc_v7_slice_geometry* slices,
                              const tc_v7_segment_input* segments,
                              uint8_t* out, size_t out_cap,
                              size_t* packet_size);

/* Decode a V7-A header from a complete packet.  The legacy header decoder
 * intentionally continues to reject major=7. */
int32_t tc_v7_packet_header_decode(const uint8_t* packet, size_t packet_size,
                                   topos_frame_header* fh);

/* 解析 TPIC packet 中的 TPLD。不会验证 segment payload CRC，也不会为 payload
 * 或 descriptor 数量分配内存；成功后 view 内指针只在 packet 仍存活时有效。 */
int32_t tc_v7_directory_parse(const uint8_t* packet, size_t packet_size,
                              const topos_frame_header* fh,
                              tc_v7_directory_view* out);

int32_t tc_v7_directory_get_layer(const tc_v7_directory_view* view, uint32_t index,
                                  tc_v7_layer_desc* out);
int32_t tc_v7_directory_get_slice(const tc_v7_directory_view* view, uint32_t index,
                                  tc_v7_slice_desc* out);
int32_t tc_v7_directory_get_segment(const tc_v7_directory_view* view, uint32_t index,
                                    tc_v7_segment_desc* out);

/* 取已通过目录边界验证的 segment 字节范围；不做 segment CRC。 */
int32_t tc_v7_directory_segment_span(const tc_v7_directory_view* view, uint32_t index,
                                     const uint8_t** data, uint32_t* size);

#endif /* TOPOS_INTERNAL_LAYER_DIRECTORY_H */
