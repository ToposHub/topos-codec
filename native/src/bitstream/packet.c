#include "packet.h"

#include <string.h>

#include "../common/checked.h"
#include "../common/crc32.h"
#include "../common/endian.h"
#include "../common/error.h"

/* M1 拆分：结构解析（header/边界/覆盖校验，无 payload CRC）。
 * 解码主路径使用——CRC 由 slice worker 各自对唯一 payload 做且只做一次，
 * 避免主线程串行预扫整帧 payload。slice_crc_ok 保持 0（未验证语义）。 */
int32_t tc_packet_parse_structure(const uint8_t* data, size_t size, topos_packet_view* view)
{
    memset(view, 0, sizeof(*view));

    int32_t rc = tc_frame_header_decode(data, size, &view->fh);
    if (rc != TC_OK) { return rc; }

    if (view->fh.version_major == 8u || view->fh.version_major == 9u) {
        /* V8/V9 包结构不同（段目录/瓦片表，slice 字段复用为瓦片数）——
         * V7 扫描入口明确拒绝（V8/V9 走 tc_packet_scan_v8，major 域
         * {8,9}；V9 布局同构继承，topos_v9_micro_gop_plan 批 1）。 */
        tc_set_error(TC_ERR_UNSUPPORTED_VERSION,
                     "v8/v9 packet: use tc_packet_scan_v8 (V7 scanner rejects major>=8)");
        return TC_ERR_UNSUPPORTED_VERSION;
    }

    if ((uint64_t)view->fh.frame_packet_size != (uint64_t)size) {
        tc_set_error(TC_ERR_MALFORMED, "frame_packet_size %u != actual %zu",
                     (unsigned)view->fh.frame_packet_size, size);
        return TC_ERR_MALFORMED;
    }

    uint16_t n = view->fh.slice_count;
    size_t off = TC_FRAME_HEADER_SIZE;
    for (uint16_t i = 0; i < n; ++i) {
        if (!tc_offset_in_bounds(size, off, TC_SLICE_HEADER_SIZE)) {
            tc_set_error(TC_ERR_TRUNCATED, "slice %u header out of packet at offset %zu",
                         (unsigned)i, off);
            return TC_ERR_TRUNCATED;
        }
        rc = tc_slice_header_decode(data + off, TC_SLICE_HEADER_SIZE, &view->slices[i]);
        if (rc != TC_OK) { return rc; }
        off += TC_SLICE_HEADER_SIZE;

        uint64_t payload = (uint64_t)view->slices[i].slice_payload_size;
        if (!tc_offset_in_bounds(size, off, (size_t)payload)) {
            tc_set_error(TC_ERR_TRUNCATED,
                         "slice %u payload %llu out of packet at offset %zu", (unsigned)i,
                         (unsigned long long)payload, off);
            return TC_ERR_TRUNCATED;
        }
        view->payloads[i] = data + off;
        if (!tc_uadd_size(off, (size_t)payload, &off)) {
            tc_set_error(TC_ERR_LIMIT_EXCEEDED, "slice %u payload overflow", (unsigned)i);
            return TC_ERR_LIMIT_EXCEEDED;
        }
    }
    if (off != size) {
        tc_set_error(TC_ERR_MALFORMED, "packet has %zu trailing bytes after last slice",
                     size - off);
        return TC_ERR_MALFORMED;
    }
    view->slice_count = n;
    return tc_slice_map_validate(&view->fh, view->slices, n);
}

int32_t tc_packet_scan(const uint8_t* data, size_t size, topos_packet_view* view)
{
    int32_t rc = tc_packet_parse_structure(data, size, view);
    if (rc != TC_OK) { return rc; }
    for (uint16_t i = 0; i < view->slice_count; ++i) {
        view->slice_crc_ok[i] =
            (tc_crc32(view->payloads[i], (size_t)view->slices[i].slice_payload_size)
             == view->slices[i].slice_crc32) ? 1u : 0u;
    }
    return TC_OK;
}

/* ---- V8 扫描（批 1；布局契约见 packet.h 注释）---- */

int32_t tc_packet_is_v8(const uint8_t* data, size_t size)
{
    if (data == NULL || size < TC_FRAME_HEADER_SIZE) { return 0; }
    if (memcmp(data, TC_FRAME_MAGIC, 4u) != 0) { return 0; }
    return data[6] == 8u ? 1 : 0;
}

int32_t tc_packet_is_v9(const uint8_t* data, size_t size)
{
    if (data == NULL || size < TC_FRAME_HEADER_SIZE) { return 0; }
    if (memcmp(data, TC_FRAME_MAGIC, 4u) != 0) { return 0; }
    return data[6] == 9u ? 1 : 0;
}

static int32_t vfail8(int32_t code, const char* what, unsigned long long detail)
{
    tc_set_error(code, "v8 packet %s = %llu rejected", what, detail);
    return code;
}

int32_t tc_packet_scan_v8_ex(const uint8_t* data, size_t size,
                             topos_v8_packet_view* view, int verify_crc)
{
    if (data == NULL || view == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v8 scan args NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(view, 0, sizeof(*view));

    int32_t rc = tc_frame_header_decode(data, size, &view->fh);
    if (rc != TC_OK) { return rc; }
    if (view->fh.version_major != 8u && view->fh.version_major != 9u) {
        /* major 域 {8,9}：V9 包布局同构继承（topos_v9_micro_gop_plan
         * 批 1；帧级差异仅在 53B 头的 major/熵三元组/GOP 字段）。 */
        return vfail8(TC_ERR_UNSUPPORTED_VERSION, "version_major(not v8/v9)",
                      view->fh.version_major);
    }
    if ((uint64_t)view->fh.frame_packet_size != (uint64_t)size) {
        tc_set_error(TC_ERR_MALFORMED, "frame_packet_size %u != actual %zu",
                     (unsigned)view->fh.frame_packet_size, size);
        return TC_ERR_MALFORMED;
    }

    /* 扩展头（域校验 + 保留恒 0） */
    if (!tc_offset_in_bounds(size, TC_FRAME_HEADER_SIZE, TC_V8_EXT_HEADER_SIZE)) {
        tc_set_error(TC_ERR_TRUNCATED, "v8 ext header out of packet");
        return TC_ERR_TRUNCATED;
    }
    const uint8_t* ext = data + TC_FRAME_HEADER_SIZE;
    const uint32_t sb_log2 = ext[0];
    const uint32_t tr_log2 = ext[1];
    if (sb_log2 < 3u || sb_log2 > 5u) {
        return vfail8(TC_ERR_MALFORMED, "segment_blocks_log2", sb_log2);
    }
    if (tr_log2 != 0u && (tr_log2 < 4u || tr_log2 > 6u)) {
        return vfail8(TC_ERR_MALFORMED, "tile_rows_log2", tr_log2);
    }
    for (uint32_t i = 2u; i < TC_V8_EXT_HEADER_SIZE; ++i) {
        if (ext[i] != 0u) {
            return vfail8(TC_ERR_MALFORMED, "ext reserved(!=0)", ext[i]);
        }
    }
    view->segment_blocks = 1u << sb_log2;
    view->tile_rows = tr_log2 == 0u ? 0u : (1u << tr_log2);

    /* 平面区基址顺序推进 + 瓦片段映射 + 目录环校验 */
    size_t off = TC_FRAME_HEADER_SIZE + TC_V8_EXT_HEADER_SIZE;
    uint32_t tile_total = 0u;
    for (uint32_t p = 0; p < view->fh.plane_count; ++p) {
        const uint32_t rows = view->fh.plane_block_rows[p];
        const uint32_t cols = view->fh.plane_block_cols[p];
        const uint32_t blocks = rows * cols;
        const uint32_t segs = (blocks + view->segment_blocks - 1u)
                            / view->segment_blocks;
        const uint32_t tiles = (view->tile_rows == 0u || rows == 0u)
            ? 1u
            : (rows + view->tile_rows - 1u) / view->tile_rows;
        view->segs_per_plane[p] = segs;
        view->tiles_per_plane[p] = tiles;
        const uint32_t dir_bytes = segs * TC_V8_DIR_ENTRY_BYTES;
        const uint32_t table_bytes = tiles * TC_V8_TABLE_BYTES;
        const uint32_t crc_bytes = tiles * 4u;
        view->plane_dir_off[p] = (uint32_t)off;
        if (!tc_offset_in_bounds(size, off, dir_bytes)) {
            tc_set_error(TC_ERR_TRUNCATED, "v8 plane %u dir out of packet", p);
            return TC_ERR_TRUNCATED;
        }
        off += dir_bytes;
        view->plane_table_off[p] = (uint32_t)off;
        if (!tc_offset_in_bounds(size, off, table_bytes)) {
            tc_set_error(TC_ERR_TRUNCATED, "v8 plane %u tables out of packet", p);
            return TC_ERR_TRUNCATED;
        }
        off += table_bytes;
        view->plane_stream_off[p] = (uint32_t)off;
        /* 目录环校验：off_0==0、off_{i+1}==off_i+len_i、len≥终态、Σlen 有界；
         * 流区长度 = Σlen（不含独立长度字段——精确耗尽由包尾对账兜底） */
        const uint8_t* dir = data + view->plane_dir_off[p];
        uint64_t stream_len_p = 0u;
        for (uint32_t s = 0; s < segs; ++s) {
            const uint8_t* e = dir + (size_t)s * TC_V8_DIR_ENTRY_BYTES;
            const uint32_t so = tc_load_be32(e);
            const uint32_t sl = tc_load_be32(e + 4u);
            if (sl < TC_V8_STATE_BYTES) {
                return vfail8(TC_ERR_MALFORMED, "segment stream_len(<state)", sl);
            }
            if (s == 0u) {
                if (so != 0u) { return vfail8(TC_ERR_MALFORMED, "seg0 stream_off", so); }
            } else {
                const uint8_t* pe = e - TC_V8_DIR_ENTRY_BYTES;
                if (so != tc_load_be32(pe) + tc_load_be32(pe + 4u)) {
                    return vfail8(TC_ERR_MALFORMED, "segment dir gap/off", so);
                }
            }
            stream_len_p += sl;
            if (stream_len_p > TC_MAX_PACKET_SIZE) {
                return vfail8(TC_ERR_LIMIT_EXCEEDED, "plane stream bytes",
                              stream_len_p);
            }
        }
        if (!tc_offset_in_bounds(size, off, (size_t)stream_len_p)) {
            tc_set_error(TC_ERR_TRUNCATED, "v8 plane %u stream out of packet", p);
            return TC_ERR_TRUNCATED;
        }
        off += (size_t)stream_len_p;
        view->plane_crc_off[p] = (uint32_t)off;
        if (!tc_offset_in_bounds(size, off, crc_bytes)) {
            tc_set_error(TC_ERR_TRUNCATED, "v8 plane %u crc out of packet", p);
            return TC_ERR_TRUNCATED;
        }
        /* 瓦片记录：成员段 = [⌊首块/sb⌋, ⌊次瓦片首块/sb⌋)（段归首块瓦片） */
        for (uint32_t t = 0; t < tiles; ++t) {
            topos_v8_tile_info* ti = &view->tiles[tile_total + t];
            const uint64_t fb = view->tile_rows == 0u
                ? 0u : (uint64_t)t * view->tile_rows * cols;
            const uint64_t fn = view->tile_rows == 0u
                ? (uint64_t)blocks : (uint64_t)(t + 1u) * view->tile_rows * cols;
            uint32_t sf = (uint32_t)(fb / view->segment_blocks);
            uint32_t sn = (uint32_t)(fn / view->segment_blocks);
            if (sf > segs) { sf = segs; }
            if (sn > segs) { sn = segs; }
            if (sn < sf) { sn = sf; }
            ti->plane = p;
            ti->tile_id = t;
            ti->seg_first = sf;
            ti->seg_count = sn - sf;
            ti->table_off = view->plane_table_off[p] + t * TC_V8_TABLE_BYTES;
        }
        tile_total += tiles;
        off += crc_bytes;
    }
    if (off != size) {
        tc_set_error(TC_ERR_MALFORMED, "v8 packet has %zu trailing bytes",
                     size - off);
        return TC_ERR_MALFORMED;
    }
    if (tile_total != view->fh.slice_count) {
        return vfail8(TC_ERR_MALFORMED, "slice_count(!= tile total)",
                      ((unsigned long long)view->fh.slice_count << 16) | tile_total);
    }
    view->tile_count = tile_total;

    /* 瓦片流基址/字节（目录推导）+ CRC 验证（不失败；conceal 判定归解码） */
    for (uint32_t t = 0; t < tile_total; ++t) {
        topos_v8_tile_info* ti = &view->tiles[t];
        const uint8_t* dir = data + view->plane_dir_off[ti->plane];
        const uint8_t* e0 = dir + (size_t)ti->seg_first * TC_V8_DIR_ENTRY_BYTES;
        ti->stream_off = view->plane_stream_off[ti->plane] + tc_load_be32(e0);
        ti->stream_bytes = 0u;
        for (uint32_t s = 0; s < ti->seg_count; ++s) {
            ti->stream_bytes += tc_load_be32(e0 + (size_t)s * TC_V8_DIR_ENTRY_BYTES + 4u);
        }
        const uint8_t* crcp = data + view->plane_crc_off[ti->plane]
                            + (size_t)ti->tile_id * 4u;
        ti->crc_stored = tc_load_be32(crcp);
        ti->crc_ok = 0u;
        if (verify_crc != 0) {
            uint32_t crc = tc_crc32_update(0u, data + ti->table_off, TC_V8_TABLE_BYTES);
            crc = tc_crc32_update(crc, data + ti->stream_off, ti->stream_bytes);
            ti->crc_ok = crc == ti->crc_stored ? 1u : 0u;
        }
    }
    return TC_OK;
}

int32_t tc_packet_scan_v8(const uint8_t* data, size_t size, topos_v8_packet_view* view)
{
    return tc_packet_scan_v8_ex(data, size, view, 1);
}
