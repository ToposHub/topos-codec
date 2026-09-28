/* V8 批 1：格式骨架——头部版本机、扩展头/段目录/瓦片表/瓦片 CRC 结构扫描、
 * V7 入口明确拒绝、编解码入口 NOT_IMPLEMENTED 占位。
 *
 * 合成几何：coded 64×320，422 10bit（luma 块 8×40=320，chroma 块 4×40=160），
 * segment_blocks=16（S=20/10/10），tile_rows=16（T=3/plane，全帧 9 瓦片），
 * 段流全部取最小 4B 终态（扫描只验结构，不碰符号层）。 */
#include "bitstream/frame_header.h"
#include "bitstream/packet.h"

#include <stdlib.h>
#include <string.h>

#include "common/crc32.h"
#include "common/endian.h"
#include "common/error.h"
#include "mini_test.h"
#include "topos_codec.h"

#define PLANE_COUNT 3u
#define SB 16u
#define TILE_ROWS 16u
#define COLS0 8u
#define COLSC 4u
#define ROWS 40u
#define T_P 3u /* 每平面瓦片 = ceil(40/16) */
#define TILE_COUNT (T_P * PLANE_COUNT)
#define SEG_LEN 4u /* 每段仅 4B 终态 */

static uint32_t v8_cols(uint32_t p) { return p == 0u ? COLS0 : COLSC; }

static topos_frame_header v8_base_fh(uint32_t packet_size)
{
    topos_frame_header fh;
    memset(&fh, 0, sizeof(fh));
    fh.version_major = 8u;
    fh.version_minor = 0u;
    fh.profile = 3u;
    fh.pixel_format = 0u;
    fh.bit_depth = 10u;
    fh.alpha_mode = 0u;
    fh.coded_width = 64u;
    fh.coded_height = 320u;
    fh.visible_width = 60u;
    fh.visible_height = 316u;
    fh.plane_count = PLANE_COUNT;
    fh.qmatrix_id = 0u;
    fh.qp_base = 30u;
    fh.slice_count = (uint16_t)TILE_COUNT;
    fh.color_range = 1u;
    fh.color_primaries = 1u;
    fh.color_transfer = 1u;
    fh.color_matrix = 1u;
    fh.entropy_mode = 8u;
    fh.frame_packet_size = packet_size;
    return fh;
}

/* 合成 V8 包。size_override：0 = 精确布局；否则 frame_packet_size 写该值
 * 且缓冲多给 1 字节（截断/拖尾用例）。返回 malloc 缓冲。 */
static uint8_t* build_v8_packet(size_t* size_out, uint8_t sb_log2, uint8_t tr_log2,
                                uint16_t slice_count, uint32_t size_override)
{
    const uint32_t sb = 1u << sb_log2;
    const uint32_t tile_rows = tr_log2 == 0u ? 0u : (1u << tr_log2);
    size_t total = TC_FRAME_HEADER_SIZE + TC_V8_EXT_HEADER_SIZE;
    uint32_t segs[4] = {0u, 0u, 0u, 0u};
    uint32_t tiles_p = 0u;
    for (uint32_t p = 0u; p < PLANE_COUNT; ++p) {
        segs[p] = ((ROWS * v8_cols(p)) + sb - 1u) / sb;
        tiles_p = (tile_rows == 0u) ? 1u : (ROWS + tile_rows - 1u) / tile_rows;
        total += (size_t)segs[p] * TC_V8_DIR_ENTRY_BYTES
               + (size_t)tiles_p * TC_V8_TABLE_BYTES
               + (size_t)segs[p] * SEG_LEN
               + (size_t)tiles_p * 4u;
    }
    const size_t buf_cap = total + 1u;
    uint8_t* buf = (uint8_t*)calloc(1u, buf_cap);
    if (buf == NULL) { return NULL; }

    topos_frame_header fh = v8_base_fh((uint32_t)(size_override != 0u ? size_override : total));
    fh.slice_count = slice_count;
    if (tc_frame_header_encode(&fh, buf) != TC_OK) { free(buf); return NULL; }

    buf[TC_FRAME_HEADER_SIZE] = sb_log2;
    buf[TC_FRAME_HEADER_SIZE + 1u] = tr_log2;
    /* [2..7] 保留已 calloc 清零 */

    size_t off = TC_FRAME_HEADER_SIZE + TC_V8_EXT_HEADER_SIZE;
    for (uint32_t p = 0u; p < PLANE_COUNT; ++p) {
        const uint32_t cols = v8_cols(p);
        /* 段目录 */
        uint32_t cum = 0u;
        for (uint32_t s = 0u; s < segs[p]; ++s) {
            uint8_t* e = buf + off + (size_t)s * TC_V8_DIR_ENTRY_BYTES;
            tc_store_be32(e, cum);
            tc_store_be32(e + 4u, SEG_LEN);
            e[8] = 64u; /* qp_delta_biased = +0 */
            cum += SEG_LEN;
        }
        off += (size_t)segs[p] * TC_V8_DIR_ENTRY_BYTES;
        /* 瓦片表（定长 350B；flags=0 + 行字节图案） */
        for (uint32_t t = 0u; t < tiles_p; ++t) {
            uint8_t* tb = buf + off + (size_t)t * TC_V8_TABLE_BYTES;
            tb[0] = 0u; /* lvl=NONE, dc=NONE */
            for (uint32_t i = 1u; i < TC_V8_TABLE_BYTES; ++i) {
                tb[i] = (uint8_t)(i * 7u + t);
            }
        }
        off += (size_t)tiles_p * TC_V8_TABLE_BYTES;
        const uint32_t stream_base = (uint32_t)off;
        off += (size_t)segs[p] * SEG_LEN;
        /* 瓦片 CRC（表 350B + 成员段流连续区间；段归首块瓦片） */
        const uint32_t blocks = ROWS * cols;
        for (uint32_t t = 0u; t < tiles_p; ++t) {
            const uint64_t fb = tile_rows == 0u ? 0ull : (uint64_t)t * tile_rows * cols;
            const uint64_t fn = tile_rows == 0u ? (uint64_t)blocks
                                                : (uint64_t)(t + 1u) * tile_rows * cols;
            uint32_t sf = (uint32_t)(fb / sb);
            uint32_t sn = (uint32_t)(fn / sb);
            if (sf > segs[p]) { sf = segs[p]; }
            if (sn > segs[p]) { sn = segs[p]; }
            if (sn < sf) { sn = sf; }
            uint32_t crc = tc_crc32_update(0u,
                                           buf + stream_base
                                               - (size_t)tiles_p * TC_V8_TABLE_BYTES
                                               + (size_t)t * TC_V8_TABLE_BYTES,
                                           TC_V8_TABLE_BYTES);
            crc = tc_crc32_update(crc, buf + stream_base + (size_t)sf * SEG_LEN,
                                  (size_t)(sn - sf) * SEG_LEN);
            tc_store_be32(buf + off + (size_t)t * 4u, crc);
        }
        off += (size_t)tiles_p * 4u;
    }
    *size_out = total;
    return buf;
}

int main(void)
{
    /* ---- 头部版本机：V8 三元组 (8,0,0)、minor 恒 0 ---- */
    {
        topos_frame_header fh = v8_base_fh(12345u);
        uint8_t enc[TC_FRAME_HEADER_SIZE];
        MT_CHECK_EQ_I64(tc_frame_header_encode(&fh, enc), TC_OK);
        topos_frame_header back;
        MT_CHECK_EQ_I64(tc_frame_header_decode(enc, sizeof(enc), &back), TC_OK);
        MT_CHECK_EQ_U64(back.version_major, 8ull);
        MT_CHECK_EQ_U64(back.entropy_mode, 8ull);
        MT_CHECK_EQ_U64(back.plane_block_cols[0], 8ull);
        MT_CHECK_EQ_U64(back.plane_block_rows[0], 40ull);
        MT_CHECK_EQ_U64(back.plane_block_cols[1], 4ull);

        topos_frame_header bad = fh;
        bad.entropy_mode = 7u; /* V8 只认 (8,0,0) */
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_UNSUPPORTED_VERSION);
        bad = fh;
        bad.codebook_version = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_UNSUPPORTED_VERSION);
        bad = fh;
        bad.version_minor = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_UNSUPPORTED_VERSION);
        bad = fh;
        bad.coding_mode = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_UNSUPPORTED_VERSION);
    }

    /* ---- 结构扫描：派生几何 / 区基址 / 瓦片段映射 / CRC ---- */
    size_t total = 0u;
    uint8_t* pkt = build_v8_packet(&total, 4u /*sb=16*/, 4u /*tile=16 行*/,
                                   (uint16_t)TILE_COUNT, 0u);
    MT_CHECK(pkt != NULL);
    if (pkt != NULL) {
        MT_CHECK_EQ_U64(tc_packet_is_v8(pkt, total), 1ull);
        topos_v8_packet_view view;
        MT_CHECK_EQ_I64(tc_packet_scan_v8(pkt, total, &view), TC_OK);
        MT_CHECK_EQ_U64(view.segment_blocks, 16ull);
        MT_CHECK_EQ_U64(view.tile_rows, 16ull);
        MT_CHECK_EQ_U64(view.tile_count, TILE_COUNT);
        MT_CHECK_EQ_U64(view.segs_per_plane[0], 20ull);
        MT_CHECK_EQ_U64(view.segs_per_plane[1], 10ull);
        MT_CHECK_EQ_U64(view.segs_per_plane[2], 10ull);
        MT_CHECK_EQ_U64(view.tiles_per_plane[0], T_P);
        MT_CHECK_EQ_U64(view.tiles_per_plane[1], T_P);
        /* 区基址顺序：dir → tables → stream → crc */
        MT_CHECK_EQ_U64(view.plane_dir_off[0], TC_FRAME_HEADER_SIZE + TC_V8_EXT_HEADER_SIZE);
        MT_CHECK(view.plane_table_off[0] > view.plane_dir_off[0]);
        MT_CHECK(view.plane_stream_off[0] > view.plane_table_off[0]);
        MT_CHECK(view.plane_crc_off[0] > view.plane_stream_off[0]);
        MT_CHECK_EQ_U64(view.plane_dir_off[1],
                        view.plane_crc_off[0] + T_P * 4ull);
        /* 瓦片段映射：luma 瓦片 t 成员段 = [8t, 8t+8)（瓦片 2 截到 20） */
        MT_CHECK_EQ_U64(view.tiles[0].seg_first, 0ull);
        MT_CHECK_EQ_U64(view.tiles[0].seg_count, 8ull);
        MT_CHECK_EQ_U64(view.tiles[1].seg_first, 8ull);
        MT_CHECK_EQ_U64(view.tiles[1].seg_count, 8ull);
        MT_CHECK_EQ_U64(view.tiles[2].seg_first, 16ull);
        MT_CHECK_EQ_U64(view.tiles[2].seg_count, 4ull);
        /* chroma 瓦片 2：seg [8,10) */
        MT_CHECK_EQ_U64(view.tiles[5].seg_first, 8ull);
        MT_CHECK_EQ_U64(view.tiles[5].seg_count, 2ull);
        MT_CHECK_EQ_U64(view.tiles[5].stream_bytes, 2ull * SEG_LEN);
        /* CRC 全 OK */
        for (uint32_t t = 0u; t < TILE_COUNT; ++t) {
            MT_CHECK_EQ_U64(view.tiles[t].crc_ok, 1ull);
        }

        /* ---- 瓦片 CRC 损伤 → 标记（不失败；conceal 归解码） ---- */
        {
            pkt[view.tiles[4].stream_off] ^= 0xFFu;
            topos_v8_packet_view v2;
            MT_CHECK_EQ_I64(tc_packet_scan_v8(pkt, total, &v2), TC_OK);
            MT_CHECK_EQ_U64(v2.tiles[4].crc_ok, 0ull);
            MT_CHECK_EQ_U64(v2.tiles[3].crc_ok, 1ull);
            MT_CHECK_EQ_U64(v2.tiles[5].crc_ok, 1ull);
            pkt[view.tiles[4].stream_off] ^= 0xFFu;
        }

        /* ---- V7 扫描入口对 V8 明确拒绝（旧二进制语义钉死） ---- */
        {
            topos_packet_view v7view;
            MT_CHECK_EQ_I64(tc_packet_scan(pkt, total, &v7view),
                            TC_ERR_UNSUPPORTED_VERSION);
        }

        /* ---- 解码入口：查询模式 OK；实解码（合成包全零流 → 全段 conceal
         *      → mid 填充；批 3） ---- */
        {
            topos_frame_output info;
            MT_CHECK_EQ_I64(tc_frame_decode(pkt, total, NULL, NULL, &info), TC_OK);
            MT_CHECK_EQ_U64(info.visible_width, 60ull);
            uint16_t* planes[4] = {0};
            size_t strides[4] = {0};
            static uint16_t py[64u * 320u], pcb[64u * 320u], pcr[64u * 320u];
            planes[0] = py; planes[1] = pcb; planes[2] = pcr;
            strides[0] = 64u; strides[1] = 64u; strides[2] = 64u;
            MT_CHECK_EQ_I64(tc_frame_decode(pkt, total, planes, strides, &info), TC_OK);
            MT_CHECK_EQ_U64(info.concealed_slices, 9ull); /* 合成流全坏 → 9 瓦片 conceal */
            for (uint32_t i = 0u; i < info.slice_count; ++i) {
                MT_CHECK_EQ_U64(info.slice_status[i], TC_FRAME_SLICE_CONCEALED);
            }
            /* mid 填充（10bit → 512） */
            MT_CHECK_EQ_U64(py[0], 512ull);
            MT_CHECK_EQ_U64(pcb[0], 512ull);
        }

        free(pkt);
    }

    /* ---- 结构变异（fuzz/边界） ---- */
    {
        const struct {
            uint8_t sb_log2;
            uint8_t tr_log2;
            uint16_t slice_count;
            int32_t expect;
            const char* tag;
        } cases[] = {
            {2u, 4u, TILE_COUNT, TC_ERR_MALFORMED, "sb_log2=2"},
            {6u, 4u, TILE_COUNT, TC_ERR_MALFORMED, "sb_log2=6"},
            {4u, 3u, TILE_COUNT, TC_ERR_MALFORMED, "tile_log2=3"},
            {4u, 7u, TILE_COUNT, TC_ERR_MALFORMED, "tile_log2=7"},
            {4u, 0u, 3u, TC_OK, "tile=whole-plane (T=3)"},
            {3u, 4u, TILE_COUNT, TC_OK, "sb=8"},
            {5u, 4u, TILE_COUNT, TC_OK, "sb=32"},
            {4u, 5u, 6u, TC_OK, "tile=32 行 (T=6)"},
            {4u, 6u, 3u, TC_OK, "tile=64 行 (T=3)"},
            {4u, 4u, 8u, TC_ERR_MALFORMED, "slice_count mismatch"},
        };
        for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            size_t sz = 0u;
            uint8_t* p = build_v8_packet(&sz, cases[i].sb_log2, cases[i].tr_log2,
                                         cases[i].slice_count, 0u);
            MT_CHECK(p != NULL);
            if (p == NULL) { continue; }
            topos_v8_packet_view view;
            const int32_t rc = tc_packet_scan_v8(p, sz, &view);
            if (rc != cases[i].expect) {
                fprintf(stderr, "FAIL case %s: expect %d got %d (%s)\n",
                        cases[i].tag, cases[i].expect, rc, tc_last_error());
                mt_failures++;
            }
            free(p);
        }
    }
    { /* 扩展头保留位非 0 */
        size_t sz = 0u;
        uint8_t* p = build_v8_packet(&sz, 4u, 4u, (uint16_t)TILE_COUNT, 0u);
        MT_CHECK(p != NULL);
        if (p != NULL) {
            p[TC_FRAME_HEADER_SIZE + 3u] = 1u;
            topos_v8_packet_view view;
            MT_CHECK_EQ_I64(tc_packet_scan_v8(p, sz, &view), TC_ERR_MALFORMED);
            free(p);
        }
    }
    { /* 段目录裂缝：seg1 的 off ≠ seg0 off+len */
        size_t sz = 0u;
        uint8_t* p = build_v8_packet(&sz, 4u, 4u, (uint16_t)TILE_COUNT, 0u);
        MT_CHECK(p != NULL);
        if (p != NULL) {
            size_t dir0 = TC_FRAME_HEADER_SIZE + TC_V8_EXT_HEADER_SIZE;
            uint32_t o1 = tc_load_be32(p + dir0 + TC_V8_DIR_ENTRY_BYTES);
            tc_store_be32(p + dir0 + TC_V8_DIR_ENTRY_BYTES, o1 + 8u);
            topos_v8_packet_view view;
            MT_CHECK_EQ_I64(tc_packet_scan_v8(p, sz, &view), TC_ERR_MALFORMED);
            free(p);
        }
    }
    { /* 段 len < 4B 终态 */
        size_t sz = 0u;
        uint8_t* p = build_v8_packet(&sz, 4u, 4u, (uint16_t)TILE_COUNT, 0u);
        MT_CHECK(p != NULL);
        if (p != NULL) {
            size_t dir0 = TC_FRAME_HEADER_SIZE + TC_V8_EXT_HEADER_SIZE;
            tc_store_be32(p + dir0 + 4u, 3u);
            topos_v8_packet_view view;
            MT_CHECK_EQ_I64(tc_packet_scan_v8(p, sz, &view), TC_ERR_MALFORMED);
            free(p);
        }
    }
    { /* 截断（frame_packet_size 同步缩小 → 布局越界） */
        size_t sz = 0u;
        uint8_t* p = build_v8_packet(&sz, 4u, 4u, (uint16_t)TILE_COUNT, 0u);
        MT_CHECK(p != NULL);
        if (p != NULL) {
            size_t sz2 = 0u;
            uint8_t* q = build_v8_packet(&sz2, 4u, 4u, (uint16_t)TILE_COUNT,
                                         (uint32_t)(sz - 1u));
            MT_CHECK(q != NULL);
            if (q != NULL) {
                topos_v8_packet_view view;
                /* frame_packet_size 已写 sz2−1：以截断长度扫描 → 布局越界 */
                MT_CHECK_EQ_I64(tc_packet_scan_v8(q, sz2 - 1u, &view), TC_ERR_TRUNCATED);
                free(q);
            }
            free(p);
        }
    }
    { /* 拖尾字节 */
        size_t sz = 0u;
        uint8_t* p = build_v8_packet(&sz, 4u, 4u, (uint16_t)TILE_COUNT,
                                     (uint32_t)(sz + 1u));
        MT_CHECK(p != NULL);
        if (p != NULL) {
            topos_v8_packet_view view;
            MT_CHECK_EQ_I64(tc_packet_scan_v8(p, sz, &view), TC_ERR_MALFORMED);
            free(p);
        }
    }

    /* ---- 编码入口：reserved[0]=9 → V8 plain 编码（批 2 真实现） ---- */
    {
        topos_frame_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.struct_size = (uint32_t)sizeof(cfg);
        cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        cfg.visible_width = 64u;
        cfg.visible_height = 320u;
        cfg.profile = 3u;
        cfg.pixel_format = 0u;
        cfg.bit_depth = 10u;
        cfg.qp_base = 30u;
        cfg.reserved[0] = 9u;
        uint16_t planes[3][64u * 320u];
        memset(planes, 0, sizeof(planes));
        topos_frame_input in;
        memset(&in, 0, sizeof(in));
        in.struct_size = (uint32_t)sizeof(in);
        in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        in.planes[0] = planes[0];
        in.planes[1] = planes[1];
        in.planes[2] = planes[2];
        size_t cap = tc_frame_packet_bound(&cfg);
        uint8_t* buf = (uint8_t*)malloc(cap != 0u ? cap : 1u);
        MT_CHECK(buf != NULL);
        if (buf != NULL) {
            /* 批 2：plain 编码真实现——产出可过 scan_v8 的 V8 包 */
            topos_frame_stats st;
            MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, buf, cap, &st), TC_OK);
            MT_CHECK(st.packet_size > 0u);
            topos_v8_packet_view view;
            MT_CHECK_EQ_I64(tc_packet_scan_v8(buf, st.packet_size, &view), TC_OK);
            MT_CHECK_EQ_U64(view.tile_count, (uint64_t)view.fh.slice_count);
            for (uint32_t t = 0u; t < view.tile_count; ++t) {
                MT_CHECK_EQ_U64(view.tiles[t].crc_ok, 1ull);
            }
            MT_CHECK_EQ_U64(view.segment_blocks, 16ull);   /* 批 0 默认档 */
            MT_CHECK_EQ_U64(view.tile_rows, 32ull);
            /* 确定性：同输入重编码逐字节一致 */
            uint8_t* buf2 = (uint8_t*)malloc(cap);
            MT_CHECK(buf2 != NULL);
            if (buf2 != NULL) {
                topos_frame_stats st2;
                MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, buf2, cap, &st2), TC_OK);
                MT_CHECK(memcmp(buf, buf2, st.packet_size) == 0);
                free(buf2);
            }
            /* sized 路径仍占位（批 5 码控集成） */
            uint8_t qp_used = 0u;
            MT_CHECK_EQ_I64(tc_frame_encode_sized(&cfg, &in, 1000u, 0u, 95u,
                                                  &qp_used, buf, cap, &st),
                            TC_ERR_NOT_IMPLEMENTED);
            free(buf);
        }
    }

    return MT_MAIN_RETURN();
}
