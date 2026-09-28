#include "common/alloc.h"
#include "slice_codec.h"

#include <stdlib.h>
#include <string.h>

#include "../common/checked.h"
#include "../common/error.h"
#include "../entropy/block_coding.h"
#include "../entropy/color_scan.h"
#include "../entropy/rans.h"
#include "../entropy/vlc.h"
#include "../codec/intra.h"

/* V2 分发（ADR-C027 D-8）：major=2 且 entropy_mode=1 的颜色 slice 走 VLC；
 * k1/k2/k3 位置承载 dc/level/run book id（0..3，slice_map 域校验）。
 * Alpha 恒 Rice（spec v2 §3）。 */
static int tc_slice_use_vlc(const topos_frame_header* fh)
{
    return fh->version_major == 2u && fh->entropy_mode == 1u;
}

static int tc_slice_use_c1(const topos_frame_header* fh)
{
    return fh->version_major == 4u && fh->entropy_mode == 2u;
}

static int tc_slice_use_c2(const topos_frame_header* fh)
{
    return fh->version_major == 5u && fh->entropy_mode == 3u;
}

static int tc_slice_use_range(const topos_frame_header* fh)
{
    return fh->version_major == 6u && fh->entropy_mode == 4u;
}

/* V7-R（ADR-C034）：major=7 且 entropy_mode=6 的颜色 slice 走 per-slice
 * rANS（三族精确计数表 + 单状态流）；alpha 恒 Rice（与 C1/C2/V6 同策略） */
static int tc_slice_use_rans(const topos_frame_header* fh)
{
    return fh->version_major == 7u && fh->entropy_mode == 6u;
}

/* V7-R2（ADR-C036）：entropy_mode=7 = V7-R + order-1 上下文扩展。
 * V7-R3（ADR-C048）：entropy_mode=8 = R2 同构熵机承载帧间微 GOP。 */
static int tc_slice_use_rans2(const topos_frame_header* fh)
{
    return fh->version_major == 7u &&
           (fh->entropy_mode == 7u || fh->entropy_mode == 8u);
}

/* 定义于本文件后段（C1 流式版之后）；四个解码入口共享 */
static int32_t tc_color_rans_decode_core(const topos_frame_header* fh,
                                         const topos_slice_header* sh,
                                         const uint8_t* payload, size_t payload_size,
                                         tc_color_block_sink_fn sink, void* ctx,
                                         uint64_t* symbol_hash,
                                         tc_scan_dc_ctx* dc);

static int32_t tc_color_rans2_decode_core(const topos_frame_header* fh,
                                          const topos_slice_header* sh,
                                          const uint8_t* payload, size_t payload_size,
                                          tc_color_block_sink_fn sink, void* ctx,
                                          uint64_t* symbol_hash,
                                          tc_scan_dc_ctx* dc);

typedef struct rans_arr_sink_ctx { int32_t* q_out; } rans_arr_sink_ctx;

/* 数组版 sink：块号即行主序下标（by*cols+bx），与 q_out 布局一致 */
static int32_t rans_arr_sink(void* vctx, uint32_t idx,
                             const int32_t q[64], uint32_t rowmask)
{
    (void)rowmask;
    rans_arr_sink_ctx* c = (rans_arr_sink_ctx*)vctx;
    memcpy(c->q_out + (size_t)idx * 64u, q, 64u * sizeof(int32_t));
    return TC_OK;
}

static int32_t slice_books(const topos_frame_header* fh, const topos_slice_header* sh,
                           const tc_vlc_book** dc, const tc_vlc_book** lvl,
                           const tc_vlc_book** run)
{
    int32_t rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_DC, sh->k1);
    if (rc != TC_OK) { return rc; }
    rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_LVL, sh->k2);
    if (rc != TC_OK) { return rc; }
    rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_RUN, sh->k3);
    if (rc != TC_OK) { return rc; }
    *dc = tc_vlc_book_get(TC_VLC_FAMILY_DC, sh->k1);
    *lvl = tc_vlc_book_get(TC_VLC_FAMILY_LVL, sh->k2);
    *run = tc_vlc_book_get(TC_VLC_FAMILY_RUN, sh->k3);
    if (*dc == NULL || *lvl == NULL || *run == NULL) {
        tc_set_error(TC_ERR_STATE, "vlc book unavailable");
        return TC_ERR_STATE;
    }
    (void)fh;
    return TC_OK;
}

int32_t tc_color_slice_encode(const topos_frame_header* fh, const topos_slice_header* sh,
                              const int32_t* q_blocks, tc_bitwriter* bw)
{
    if (fh->version_major == 3u || tc_slice_use_range(fh)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "V3 encoding requires pixels and intra modes");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (tc_slice_use_rans(fh) || tc_slice_use_rans2(fh)) {
        /* rANS 编码经 codec.c token 路径组装（表 + 后向流写位写器）；
         * 本入口是 qbuf/逐块位写器契约，防御性拒绝 */
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "rans encode requires token path");
        return TC_ERR_INVALID_ARGUMENT;
    }
    uint32_t cols = fh->plane_block_cols[sh->plane];
    uint32_t band = sh->block_h;
    size_t need = 0;
    if (!tc_umul_size((size_t)cols * 64u, (size_t)band, &need)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "color slice block count overflow");
        return TC_ERR_LIMIT_EXCEEDED;
    }

    /* 码书逐 slice 一次解析（R7 提升出逐块循环：每块 6 次跨 TU 调用 +
     * 原子 acquire，4K 每帧 259200 块时为可测开销；结构对齐解码路径） */
    int32_t rc = TC_OK;
    const tc_vlc_book* dc_b = NULL;
    const tc_vlc_book* lvl_b = NULL;
    const tc_vlc_book* run_b = NULL;
    if (tc_slice_use_vlc(fh) || tc_slice_use_c1(fh)) {
        rc = slice_books(fh, sh, &dc_b, &lvl_b, &run_b);
        if (rc != TC_OK) { return rc; }
    }

    tc_vlc_book c2_book;
    uint8_t c2_lengths[64];
    const int use_c2 = tc_slice_use_c2(fh);
    if (use_c2 != 0) {
        const size_t block_count = (size_t)band * (size_t)cols;
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_DC, sh->k1);
        if (rc != TC_OK) { return rc; }
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_LVL, sh->k2);
        if (rc != TC_OK) { return rc; }
        rc = tc_c2_pair_book_build(q_blocks, block_count, &c2_book, c2_lengths);
        if (rc != TC_OK) { return rc; }
        for (uint32_t i = 0u; i < 64u; ++i) {
            rc = tc_bitwriter_put_bits_inline(bw, 8u, c2_lengths[i]);
            if (rc != TC_OK) { return rc; }
        }
        dc_b = tc_vlc_book_get(TC_VLC_FAMILY_DC, sh->k1);
        lvl_b = tc_vlc_book_get(TC_VLC_FAMILY_LVL, sh->k2);
        run_b = &c2_book;
    }

    for (uint32_t by = 0u; by < band; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            const int32_t* blk = q_blocks + ((size_t)by * (size_t)cols + (size_t)bx) * 64u;
            int has_left = bx > 0u ? 1 : 0;
            int has_top = by > 0u ? 1 : 0;
            int32_t left_dc = 0;
            int32_t top_dc = 0;
            if (has_left != 0) { left_dc = blk[-64]; }                 /* 左邻块 DC（blk−64 处即其 [0]） */
            if (has_top != 0) {
                const int32_t* top_blk = blk - (size_t)cols * 64u;     /* 上邻块起点 */
                top_dc = top_blk[0];
            }
            /* R6：q_blocks 为 zigzag 扫描序（fill_color_band 直写）→ 顺序扫描变体 */
            if (use_c2 != 0) {
                rc = tc_block_encode_zigzag_c2(bw, dc_b, lvl_b, run_b, blk,
                                               has_left, left_dc, has_top, top_dc,
                                               NULL);
            } else if (tc_slice_use_c1(fh)) {
                rc = tc_block_encode_zigzag_c1(bw, dc_b, lvl_b, run_b, blk,
                                               has_left, left_dc, has_top, top_dc,
                                               NULL);
            } else if (dc_b != NULL) {
                rc = tc_block_encode_zigzag_vlc(bw, dc_b, lvl_b, run_b, blk,
                                                has_left, left_dc, has_top, top_dc,
                                                NULL);
            } else {
                rc = tc_block_encode_zigzag(bw, sh->k1, sh->k2, sh->k3, blk,
                                            has_left, left_dc, has_top, top_dc, NULL);
            }
            if (rc != TC_OK) { return rc; }
        }
    }
    return TC_OK;
}

int32_t tc_color_slice_decode(const topos_frame_header* fh, const topos_slice_header* sh,
                              const uint8_t* payload, size_t payload_size,
                              int32_t* q_out, uint64_t* symbol_hash)
{
    if (fh->version_major == 3u || tc_slice_use_range(fh)) {
        return tc_intra_slice_decode(fh, sh, payload, payload_size, q_out,
                                    symbol_hash, NULL, NULL, NULL, NULL, NULL, 0u);
    }
    if (tc_slice_use_rans(fh)) {
        tc_scan_dc_ctx dc;
        const size_t n = fh->plane_block_cols[sh->plane] > 0u
                       ? (size_t)fh->plane_block_cols[sh->plane] : 1u;
        dc.elems = n;
        dc.prev_row = (int32_t*)tc_alloc(n * sizeof(int32_t));
        dc.row = (int32_t*)tc_alloc(n * sizeof(int32_t));
        if (dc.prev_row == NULL || dc.row == NULL) {
            tc_free(dc.prev_row); tc_free(dc.row);
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "rans color DC buffers");
            return TC_ERR_OUT_OF_MEMORY;
        }
        rans_arr_sink_ctx actx = { q_out };
        int32_t rc = tc_color_rans_decode_core(fh, sh, payload, payload_size,
                                               q_out != NULL ? rans_arr_sink : NULL,
                                               &actx, symbol_hash, &dc);
        tc_free(dc.prev_row); tc_free(dc.row);
        return rc;
    }
    if (tc_slice_use_rans2(fh)) {
        tc_scan_dc_ctx dc;
        const size_t n = fh->plane_block_cols[sh->plane] > 0u
                       ? (size_t)fh->plane_block_cols[sh->plane] : 1u;
        dc.elems = n;
        dc.prev_row = (int32_t*)tc_alloc(n * sizeof(int32_t));
        dc.row = (int32_t*)tc_alloc(n * sizeof(int32_t));
        if (dc.prev_row == NULL || dc.row == NULL) {
            tc_free(dc.prev_row); tc_free(dc.row);
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "rans2 color DC buffers");
            return TC_ERR_OUT_OF_MEMORY;
        }
        rans_arr_sink_ctx actx = { q_out };
        int32_t rc = tc_color_rans2_decode_core(fh, sh, payload, payload_size,
                                                q_out != NULL ? rans_arr_sink : NULL,
                                                &actx, symbol_hash, &dc);
        tc_free(dc.prev_row); tc_free(dc.row);
        return rc;
    }
    tc_bitreader br;
    tc_bitreader_init(&br, payload, payload_size);

    uint32_t cols = fh->plane_block_cols[sh->plane];
    uint32_t band = sh->block_h;
    const int want_hash = symbol_hash != NULL; /* NULL 时跳过逐系数指纹（阶段 9） */
    uint64_t hash = 0;
    int32_t blk[64];
    int32_t* prev_row_dc = NULL;
    int32_t* row_dc = NULL;

    /* 行内只需上一行 DC 与左邻 DC：两块小缓冲即可流式（O(cols) 内存） */
    size_t dc_elems = cols > 0u ? (size_t)cols : 1u;
    prev_row_dc = (int32_t*)tc_calloc(dc_elems, sizeof(int32_t));
    row_dc = (int32_t*)tc_calloc(dc_elems, sizeof(int32_t));
    if (prev_row_dc == NULL || row_dc == NULL) {
        tc_free(prev_row_dc); tc_free(row_dc);
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "color slice dc buffers");
        return TC_ERR_OUT_OF_MEMORY;
    }

    int32_t rc = TC_OK;
    const int use_vlc = tc_slice_use_vlc(fh);
    const int use_c1 = tc_slice_use_c1(fh);
    const int use_c2 = tc_slice_use_c2(fh);
    const tc_vlc_book* dc_b = NULL;
    const tc_vlc_book* lvl_b = NULL;
    const tc_vlc_book* run_b = NULL;
    tc_vlc_book c2_book;
    if (use_vlc != 0 || use_c1 != 0) {
        rc = slice_books(fh, sh, &dc_b, &lvl_b, &run_b);
        if (rc != TC_OK) {
            tc_free(prev_row_dc); tc_free(row_dc);
            return rc;
        }
    }
    if (use_c2 != 0) {
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_DC, sh->k1);
        if (rc != TC_OK) { tc_free(prev_row_dc); tc_free(row_dc); return rc; }
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_LVL, sh->k2);
        if (rc != TC_OK) { tc_free(prev_row_dc); tc_free(row_dc); return rc; }
        uint8_t lengths[64];
        for (uint32_t i = 0u; i < 64u; ++i) {
            uint32_t v = 0u;
            rc = tc_bitreader_read_bits(&br, 8u, &v);
            if (rc != TC_OK) { tc_free(prev_row_dc); tc_free(row_dc); return rc; }
            lengths[i] = (uint8_t)v;
        }
        for (uint32_t i = TC_C2_PAIR_SYMS; i < 64u; ++i) {
            if (lengths[i] != 0u) {
                tc_bitreader_fail(&br, TC_ERR_MALFORMED);
                tc_free(prev_row_dc); tc_free(row_dc); return TC_ERR_MALFORMED;
            }
        }
        rc = tc_vlc_book_build(&c2_book, lengths, TC_C2_PAIR_SYMS, TC_VLC_KIND_RUN);
        if (rc != TC_OK) { tc_bitreader_fail(&br, TC_ERR_MALFORMED); tc_free(prev_row_dc); tc_free(row_dc); return TC_ERR_MALFORMED; }
        dc_b = tc_vlc_book_get(TC_VLC_FAMILY_DC, sh->k1);
        lvl_b = tc_vlc_book_get(TC_VLC_FAMILY_LVL, sh->k2);
        run_b = &c2_book;
    }
    for (uint32_t by = 0u; by < band && rc == TC_OK; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            int has_left = bx > 0u ? 1 : 0;
            int has_top = by > 0u ? 1 : 0;
            if (use_c2 != 0) {
                rc = tc_block_decode_c2(&br, dc_b, lvl_b, run_b,
                                        has_left, row_dc[bx > 0u ? bx - 1u : 0u],
                                        has_top, prev_row_dc[bx], blk);
            } else if (use_c1 != 0) {
                rc = tc_block_decode_c1(&br, dc_b, lvl_b, run_b,
                                        has_left, row_dc[bx > 0u ? bx - 1u : 0u],
                                        has_top, prev_row_dc[bx], blk);
            } else if (use_vlc != 0) {
                rc = tc_block_decode_vlc(&br, dc_b, lvl_b, run_b,
                                         has_left, row_dc[bx > 0u ? bx - 1u : 0u],
                                         has_top, prev_row_dc[bx], blk);
            } else {
                rc = tc_block_decode(&br, sh->k1, sh->k2, sh->k3,
                                     has_left, row_dc[bx > 0u ? bx - 1u : 0u],
                                     has_top, prev_row_dc[bx], blk);
            }
            if (rc != TC_OK) { break; }
            row_dc[bx] = blk[0];
            if (want_hash) {
                for (uint32_t i = 0u; i < 64u; ++i) {
                    hash = tc_symbol_hash_mix(hash, (uint64_t)(int64_t)blk[i]);
                }
            }
            if (q_out != NULL) {
                memcpy(q_out + ((size_t)by * (size_t)cols + (size_t)bx) * 64u, blk,
                       64u * sizeof(int32_t));
            }
        }
        int32_t* tmp = prev_row_dc; prev_row_dc = row_dc; row_dc = tmp;
    }
    tc_free(prev_row_dc);
    tc_free(row_dc);
    if (rc != TC_OK) { return rc; }

    /* payload 必须被恰好消费（对齐填充允许；额外数据 = MALFORMED） */
    int32_t arc = tc_bitreader_align_byte(&br);
    if (arc != TC_OK) { return arc; }
    if (tc_bitreader_bits_consumed(&br) != (uint64_t)payload_size * 8u) {
        tc_set_error(TC_ERR_MALFORMED, "color slice has %llu trailing bytes",
                     (unsigned long long)(payload_size - tc_bitreader_bits_consumed(&br) / 8u));
        return TC_ERR_MALFORMED;
    }
    if (symbol_hash != NULL) { *symbol_hash = hash; }
    return TC_OK;
}

/* C1 uses the same DC-row/sink contract as the production scanner, but keeps
 * its experimental pair syntax in this small reference path.  This is the
 * authoritative truncation/byte-for-byte oracle until the syntax graduates. */
static int32_t tc_color_c1_decode_stream(const topos_frame_header* fh,
                                         const topos_slice_header* sh,
                                         const uint8_t* payload, size_t payload_size,
                                         tc_color_block_sink_fn sink, void* ctx,
                                         uint64_t* symbol_hash, tc_scan_dc_ctx* dc,
                                         int c2)
{
    tc_bitreader br;
    tc_bitreader_init(&br, payload, payload_size);
    const uint32_t cols = fh->plane_block_cols[sh->plane];
    const uint32_t band = sh->block_h;
    const size_t dc_bytes = (size_t)cols * sizeof(int32_t);
    memset(dc->prev_row, 0, dc_bytes);
    memset(dc->row, 0, dc_bytes);
    int32_t rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_DC, sh->k1);
    if (rc != TC_OK) { return rc; }
    rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_LVL, sh->k2);
    if (rc != TC_OK) { return rc; }
    if (c2 == 0) {
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_RUN, sh->k3);
        if (rc != TC_OK) { return rc; }
    }
    const tc_vlc_book* dc_b = tc_vlc_book_get(TC_VLC_FAMILY_DC, sh->k1);
    const tc_vlc_book* lvl_b = tc_vlc_book_get(TC_VLC_FAMILY_LVL, sh->k2);
    const tc_vlc_book* pair_b = c2 == 0 ? tc_vlc_book_get(TC_VLC_FAMILY_RUN, sh->k3) : NULL;
    tc_vlc_book c2_book;
    if (c2 != 0) {
        uint8_t lengths[64];
        for (uint32_t i = 0u; i < 64u; ++i) {
            uint32_t v = 0u;
            rc = tc_bitreader_read_bits(&br, 8u, &v);
            if (rc != TC_OK) { return rc; }
            lengths[i] = (uint8_t)v;
        }
        for (uint32_t i = TC_C2_PAIR_SYMS; i < 64u; ++i) {
            if (lengths[i] != 0u) { tc_bitreader_fail(&br, TC_ERR_MALFORMED); return TC_ERR_MALFORMED; }
        }
        rc = tc_vlc_book_build(&c2_book, lengths, TC_C2_PAIR_SYMS, TC_VLC_KIND_RUN);
        if (rc != TC_OK) { tc_bitreader_fail(&br, TC_ERR_MALFORMED); return TC_ERR_MALFORMED; }
        pair_b = &c2_book;
    }
    if (dc_b == NULL || lvl_b == NULL || pair_b == NULL) {
        tc_set_error(TC_ERR_STATE, "C1 VLC book unavailable");
        return TC_ERR_STATE;
    }
    uint64_t hash = 0u;
    const int want_hash = symbol_hash != NULL;
    int32_t blk[64];
    for (uint32_t by = 0u; by < band; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            rc = c2 != 0
                 ? tc_block_decode_c2(&br, dc_b, lvl_b, pair_b,
                                      bx != 0u, bx != 0u ? dc->row[bx - 1u] : 0,
                                      by != 0u, dc->prev_row[bx], blk)
                 : tc_block_decode_c1(&br, dc_b, lvl_b, pair_b,
                                    bx != 0u, bx != 0u ? dc->row[bx - 1u] : 0,
                                    by != 0u, dc->prev_row[bx], blk);
            if (rc != TC_OK) { goto done; }
            dc->row[bx] = blk[0];
            uint32_t rowmask = 1u;
            if (want_hash) { hash = tc_symbol_hash_mix(hash, (uint64_t)(int64_t)blk[0]); }
            for (uint32_t i = 1u; i < 64u; ++i) {
                if (blk[i] != 0) { rowmask |= 1u << (i >> 3); }
                if (want_hash) { hash = tc_symbol_hash_mix(hash, (uint64_t)(int64_t)blk[i]); }
            }
            if (sink != NULL) {
                rc = sink(ctx, by * cols + bx, blk, rowmask);
                if (rc != TC_OK) { goto done; }
            }
        }
        tc_scan_dc_swap(dc);
    }
done:
    if (rc != TC_OK) { return rc; }
    rc = tc_bitreader_align_byte(&br);
    if (rc != TC_OK) { return rc; }
    if (tc_bitreader_bits_consumed(&br) != (uint64_t)payload_size * 8u) {
        tc_set_error(TC_ERR_MALFORMED, "C1 color slice trailing bytes");
        return TC_ERR_MALFORMED;
    }
    if (symbol_hash != NULL) { *symbol_hash = hash; }
    return TC_OK;
}

typedef struct tc_c1_store_sink_ctx { tc_color_store_ctx* store; } tc_c1_store_sink_ctx;

static int32_t tc_c1_store_sink(void* vctx, uint32_t idx,
                                const int32_t q[64], uint32_t rowmask)
{
    tc_c1_store_sink_ctx* c = (tc_c1_store_sink_ctx*)vctx;
    return tc_color_store_block_reduced(c->store, idx, q, rowmask);
}

/* ---- V7-R rANS 颜色 slice 解码（ADR-C034）----
 * payload = [121B 三族比例表][rANS 流：终态 4B 大端 + 数据正序]。
 * 符号序列与 V2 VLC 同构：每块 DC_CAT(+后缀) → (RUN, LEVEL_CAT(+后缀))*
 * → RUN=63 EOB。sink/hash/rowmask 契约与 C1 流式版一致。 */
static int32_t tc_color_rans_decode_core(const topos_frame_header* fh,
                                         const topos_slice_header* sh,
                                         const uint8_t* payload, size_t payload_size,
                                         tc_color_block_sink_fn sink, void* ctx,
                                         uint64_t* symbol_hash,
                                         tc_scan_dc_ctx* dc)
{
    if (payload_size < TC_RANS_TABLE_BYTES + TC_RANS_STATE_BYTES) {
        tc_set_error(TC_ERR_TRUNCATED, "rans slice payload %zu < %u",
                     payload_size, (unsigned)(TC_RANS_TABLE_BYTES + TC_RANS_STATE_BYTES));
        return TC_ERR_TRUNCATED;
    }
    tc_rans_model dc_m, run_m, lvl_m;
    int32_t rc = tc_rans_table_decode(payload, &dc_m, &run_m, &lvl_m);
    if (rc != TC_OK) {
        tc_set_error(TC_ERR_MALFORMED, "rans slice table invalid");
        return TC_ERR_MALFORMED;
    }
    tc_rans_lut dc_lut, run_lut, lvl_lut;
    tc_rans_lut_build(&dc_lut, &dc_m);
    tc_rans_lut_build(&run_lut, &run_m);
    tc_rans_lut_build(&lvl_lut, &lvl_m);
    tc_rans_dec dec;
    rc = tc_rans_dec_init(&dec, payload + TC_RANS_TABLE_BYTES,
                          payload_size - TC_RANS_TABLE_BYTES);
    if (rc != TC_OK) { return rc; }

    const uint32_t cols = fh->plane_block_cols[sh->plane];
    const uint32_t band = sh->block_h;
    memset(dc->prev_row, 0, (size_t)cols * sizeof(int32_t));
    memset(dc->row, 0, (size_t)cols * sizeof(int32_t));
    uint64_t hash = 0;
    const int want_hash = symbol_hash != NULL;
    int32_t blk[64];
    for (uint32_t by = 0u; by < band; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            memset(blk, 0, sizeof(blk));
            uint32_t cat = 0u;
            rc = tc_rans_get_lut(&dec, &dc_lut, &dc_m, &cat);
            if (rc != TC_OK) { return rc; }
            uint32_t m = 0u;
            rc = tc_rans_get_suffix(&dec, cat, &m);
            if (rc != TC_OK) { return rc; }
            if (m > tc_rice_m_max_dc_bd(fh->bit_depth)) {
                tc_set_error(TC_ERR_MALFORMED, "rans dc magnitude %u", (unsigned)m);
                return TC_ERR_MALFORMED;
            }
            const int32_t pred = tc_dc_predict(bx != 0u,
                                               bx != 0u ? dc->row[bx - 1u] : 0,
                                               by != 0u, dc->prev_row[bx]);
            /* P0-07 同源：DC 域检查（spec §7.6），越界即恶意码流 */
            rc = tc_dc_reconstruct_checked_bd(pred, m, fh->bit_depth, &blk[0]);
            if (rc != TC_OK) { return rc; }

            uint32_t rowmask = 1u;
            uint32_t pos = 1u;
            for (;;) {
                uint32_t run_sym = 0u;
                rc = tc_rans_get_lut(&dec, &run_lut, &run_m, &run_sym);
                if (rc != TC_OK) { return rc; }
                if (run_sym == 63u) { break; } /* EOB */
                const uint64_t idx = (uint64_t)pos + (uint64_t)run_sym;
                if (idx > 63u) {
                    tc_set_error(TC_ERR_MALFORMED, "rans ac index %llu > 63",
                                 (unsigned long long)idx);
                    return TC_ERR_MALFORMED;
                }
                rc = tc_rans_get_lut(&dec, &lvl_lut, &lvl_m, &cat);
                if (rc != TC_OK) { return rc; }
                rc = tc_rans_get_suffix(&dec, cat, &m);
                if (rc != TC_OK) { return rc; }
                if (m > tc_rice_m_max_level_bd(fh->bit_depth)) {
                    tc_set_error(TC_ERR_MALFORMED, "rans level magnitude %u", (unsigned)m);
                    return TC_ERR_MALFORMED;
                }
                const uint32_t natural = kTcZigzag[idx];
                blk[natural] = tc_rice_unmap_signed(m);
                rowmask |= 1u << (natural >> 3);
                pos = (uint32_t)idx + 1u;
            }
            /* 指纹口径与 V2/C1 路径一致：自然序全 64 系数（含零） */
            if (want_hash) {
                for (uint32_t i = 0u; i < 64u; ++i) {
                    hash = tc_symbol_hash_mix(hash, (uint64_t)(int64_t)blk[i]);
                }
            }
            dc->row[bx] = blk[0];
            if (sink != NULL) {
                rc = sink(ctx, by * cols + bx, blk, rowmask);
                if (rc != TC_OK) { return rc; }
            }
        }
        tc_scan_dc_swap(dc);
    }
    if (dec.pos != dec.size) {
        tc_set_error(TC_ERR_MALFORMED, "rans slice %zu trailing bytes",
                     dec.size - dec.pos);
        return TC_ERR_MALFORMED;
    }
    if (symbol_hash != NULL) { *symbol_hash = hash; }
    return TC_OK;
}

/* ---- V7-R2 order-1 上下文 rANS 颜色 slice 解码（ADR-C036）----
 * payload = [flags 1B][dc 表][run 表 64B][lvl 表][rANS 流]。条件表每
 * ctx 一行（行内 8-bit 比例量化，全零行占位不可达）；模型/LUT per-ctx，
 * 符号按解码序因果上下文选行（本系数位置 / 前一对 lvl 类 / 前块 DC 类）。
 * 域检查与 V7-R 同构：flags 保留位或保留模型非零 → MALFORMED；模型行
 * 不可建（全零行无占位）→ MALFORMED（上层按 §9 concealment）。 */
static int32_t tc_color_rans2_decode_core(const topos_frame_header* fh,
                                          const topos_slice_header* sh,
                                          const uint8_t* payload, size_t payload_size,
                                          tc_color_block_sink_fn sink, void* ctx,
                                          uint64_t* symbol_hash,
                                          tc_scan_dc_ctx* dc)
{
    /* 最小前缀 = flags + 全 order-0 表（29+64+28）+ 终态 4B = 126 */
    if (payload_size < 1u + TC_RANS_TABLE_BYTES + TC_RANS_STATE_BYTES) {
        tc_set_error(TC_ERR_TRUNCATED, "rans2 slice payload %zu < min",
                     payload_size);
        return TC_ERR_TRUNCATED;
    }
    const uint8_t flags = payload[0];
    const uint32_t lvl_model = flags & 0x3u;
    const uint32_t dc_model = (flags >> 2) & 0x1u;
    if ((flags & 0xF8u) != 0u || lvl_model > TC_RANS2_LVL_PREV) {
        tc_set_error(TC_ERR_MALFORMED, "rans2 flags %u", (unsigned)flags);
        return TC_ERR_MALFORMED;
    }
    const uint32_t lvl_ctx_n = lvl_model == TC_RANS2_LVL_POS ? TC_RANS2_POS_CTX
                             : (lvl_model == TC_RANS2_LVL_PREV ? TC_RANS2_PREV_CTX
                                                               : 1u);
    const uint32_t dc_ctx_n = dc_model == TC_RANS2_DC_PREV ? TC_RANS2_DC_CTX : 1u;
    const size_t prefix = 1u + (size_t)dc_ctx_n * TC_RANS_DC_SYMS
                        + TC_RANS_RUN_SYMS + (size_t)lvl_ctx_n * TC_RANS_LVL_SYMS;
    if (payload_size < prefix + TC_RANS_STATE_BYTES) {
        tc_set_error(TC_ERR_TRUNCATED, "rans2 slice payload %zu < prefix %zu + 4",
                     payload_size, prefix);
        return TC_ERR_TRUNCATED;
    }

    tc_rans_model dc_m[TC_RANS2_DC_CTX];
    tc_rans_model lvl_m[TC_RANS2_PREV_CTX];
    tc_rans_model run_m;
    size_t off = 1u;
    for (uint32_t c = 0u; c < dc_ctx_n; ++c) {
        if (tc_rans2_row_decode(payload + off, TC_RANS_DC_SYMS, &dc_m[c]) != TC_OK) {
            tc_set_error(TC_ERR_MALFORMED, "rans2 dc row %u invalid", (unsigned)c);
            return TC_ERR_MALFORMED;
        }
        off += TC_RANS_DC_SYMS;
    }
    if (tc_rans2_row_decode(payload + off, TC_RANS_RUN_SYMS, &run_m) != TC_OK) {
        tc_set_error(TC_ERR_MALFORMED, "rans2 run row invalid");
        return TC_ERR_MALFORMED;
    }
    off += TC_RANS_RUN_SYMS;
    for (uint32_t c = 0u; c < lvl_ctx_n; ++c) {
        if (tc_rans2_row_decode(payload + off, TC_RANS_LVL_SYMS, &lvl_m[c]) != TC_OK) {
            tc_set_error(TC_ERR_MALFORMED, "rans2 lvl row %u invalid", (unsigned)c);
            return TC_ERR_MALFORMED;
        }
        off += TC_RANS_LVL_SYMS;
    }

    /* LUT 一块堆分配：[0..dc_ctx_n) dc / [dc_ctx_n] run / 之后 lvl */
    const uint32_t lut_n = dc_ctx_n + 1u + lvl_ctx_n;
    tc_rans_lut* luts = (tc_rans_lut*)tc_alloc((size_t)lut_n * sizeof(tc_rans_lut));
    if (luts == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "rans2 luts");
        return TC_ERR_OUT_OF_MEMORY;
    }
    tc_rans_lut* dc_luts = luts;
    tc_rans_lut* run_lut = luts + dc_ctx_n;
    tc_rans_lut* lvl_luts = luts + dc_ctx_n + 1u;
    for (uint32_t c = 0u; c < dc_ctx_n; ++c) { tc_rans_lut_build(&dc_luts[c], &dc_m[c]); }
    tc_rans_lut_build(run_lut, &run_m);
    for (uint32_t c = 0u; c < lvl_ctx_n; ++c) { tc_rans_lut_build(&lvl_luts[c], &lvl_m[c]); }

    tc_rans_dec dec;
    int32_t rc = tc_rans_dec_init(&dec, payload + prefix, payload_size - prefix);
    if (rc != TC_OK) { tc_free(luts); return rc; }

    const uint32_t cols = fh->plane_block_cols[sh->plane];
    const uint32_t band = sh->block_h;
    memset(dc->prev_row, 0, (size_t)cols * sizeof(int32_t));
    memset(dc->row, 0, (size_t)cols * sizeof(int32_t));
    uint64_t hash = 0;
    const int want_hash = symbol_hash != NULL;
    int32_t blk[64];
    uint32_t prev_dc_cat = 0u;
    int first_block = 1;
    for (uint32_t by = 0u; by < band; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            memset(blk, 0, sizeof(blk));
            const uint32_t dctx_d = first_block != 0
                ? 0u : tc_rans2_dc_ctx(prev_dc_cat, 1);
            const tc_rans_model* dmv = dc_model == TC_RANS2_DC_PREV
                ? &dc_m[dctx_d] : &dc_m[0];
            const tc_rans_lut* dlv = dc_model == TC_RANS2_DC_PREV
                ? &dc_luts[dctx_d] : &dc_luts[0];
            uint32_t cat = 0u;
            rc = tc_rans_get_lut(&dec, dlv, dmv, &cat);
            if (rc != TC_OK) { goto done; }
            uint32_t m = 0u;
            rc = tc_rans_get_suffix(&dec, cat, &m);
            if (rc != TC_OK) { goto done; }
            if (m > tc_rice_m_max_dc_bd(fh->bit_depth)) {
                tc_set_error(TC_ERR_MALFORMED, "rans2 dc magnitude %u", (unsigned)m);
                rc = TC_ERR_MALFORMED;
                goto done;
            }
            const int32_t pred = tc_dc_predict(bx != 0u,
                                               bx != 0u ? dc->row[bx - 1u] : 0,
                                               by != 0u, dc->prev_row[bx]);
            rc = tc_dc_reconstruct_checked_bd(pred, m, fh->bit_depth, &blk[0]);
            if (rc != TC_OK) { goto done; }
            prev_dc_cat = cat;
            first_block = 0;

            uint32_t rowmask = 1u;
            uint32_t pos = 1u;
            uint32_t prev_cat = 0u;
            int has_prev = 0;
            for (;;) {
                uint32_t run_sym = 0u;
                rc = tc_rans_get_lut(&dec, run_lut, &run_m, &run_sym);
                if (rc != TC_OK) { goto done; }
                if (run_sym == 63u) { break; } /* EOB */
                const uint64_t idx = (uint64_t)pos + (uint64_t)run_sym;
                if (idx > 63u) {
                    tc_set_error(TC_ERR_MALFORMED, "rans2 ac index %llu > 63",
                                 (unsigned long long)idx);
                    rc = TC_ERR_MALFORMED;
                    goto done;
                }
                const tc_rans_model* lmv;
                const tc_rans_lut* llv;
                if (lvl_model == TC_RANS2_LVL_POS) {
                    const uint32_t c = tc_rans2_pos_ctx((uint32_t)idx);
                    lmv = &lvl_m[c];
                    llv = &lvl_luts[c];
                } else if (lvl_model == TC_RANS2_LVL_PREV) {
                    const uint32_t c = tc_rans2_prevlvl_ctx(prev_cat, has_prev);
                    lmv = &lvl_m[c];
                    llv = &lvl_luts[c];
                } else {
                    lmv = &lvl_m[0];
                    llv = &lvl_luts[0];
                }
                rc = tc_rans_get_lut(&dec, llv, lmv, &cat);
                if (rc != TC_OK) { goto done; }
                rc = tc_rans_get_suffix(&dec, cat, &m);
                if (rc != TC_OK) { goto done; }
                if (m > tc_rice_m_max_level_bd(fh->bit_depth)) {
                    tc_set_error(TC_ERR_MALFORMED, "rans2 level magnitude %u",
                                 (unsigned)m);
                    rc = TC_ERR_MALFORMED;
                    goto done;
                }
                const uint32_t natural = kTcZigzag[idx];
                blk[natural] = tc_rice_unmap_signed(m);
                rowmask |= 1u << (natural >> 3);
                prev_cat = cat;
                has_prev = 1;
                pos = (uint32_t)idx + 1u;
            }
            if (want_hash) {
                for (uint32_t i = 0u; i < 64u; ++i) {
                    hash = tc_symbol_hash_mix(hash, (uint64_t)(int64_t)blk[i]);
                }
            }
            dc->row[bx] = blk[0];
            if (sink != NULL) {
                rc = sink(ctx, by * cols + bx, blk, rowmask);
                if (rc != TC_OK) { goto done; }
            }
        }
        tc_scan_dc_swap(dc);
    }
    if (dec.pos != dec.size) {
        tc_set_error(TC_ERR_MALFORMED, "rans2 slice %zu trailing bytes",
                     dec.size - dec.pos);
        rc = TC_ERR_MALFORMED;
        goto done;
    }
    if (symbol_hash != NULL) { *symbol_hash = hash; }
    rc = TC_OK;
done:
    tc_free(luts);
    return rc;
}

/* A2-4 生产融合发射版：与 tc_color_rans2_decode_core 同语义/同错误码/
 * 同消费位（差分测试钉死），差异仅在输出侧——符号环内 (nat,level) 对
 * 直接打包 u64 发射进 CSR，无 blk[64] 稠密中转、无 memset、无 sink
 * 调用；DC 重建值双写（dc 池行 + plane 稠密发射区）。域检查全部保留
 * （MALFORMED 时调用方负责清零该 slice 的 CSR 段 = GPU 重建 mid，
 * 与 conceal_band 的 bd_mid 填充逐值一致）。 */
int32_t tc_color_rans2_decode_sparse(const topos_frame_header* fh,
                                     const topos_slice_header* sh,
                                     const uint8_t* payload, size_t payload_size,
                                     tc_scan_dc_ctx* dc, tc_sp_emit_ctx* em)
{
    if (payload_size < 1u + TC_RANS_TABLE_BYTES + TC_RANS_STATE_BYTES) {
        tc_set_error(TC_ERR_TRUNCATED, "rans2 sparse payload %zu < min",
                     payload_size);
        return TC_ERR_TRUNCATED;
    }
    const uint8_t flags = payload[0];
    const uint32_t lvl_model = flags & 0x3u;
    const uint32_t dc_model = (flags >> 2) & 0x1u;
    if ((flags & 0xF8u) != 0u || lvl_model > TC_RANS2_LVL_PREV) {
        tc_set_error(TC_ERR_MALFORMED, "rans2 sparse flags %u", (unsigned)flags);
        return TC_ERR_MALFORMED;
    }
    const uint32_t lvl_ctx_n = lvl_model == TC_RANS2_LVL_POS ? TC_RANS2_POS_CTX
                             : (lvl_model == TC_RANS2_LVL_PREV ? TC_RANS2_PREV_CTX
                                                               : 1u);
    const uint32_t dc_ctx_n = dc_model == TC_RANS2_DC_PREV ? TC_RANS2_DC_CTX : 1u;
    const size_t prefix = 1u + (size_t)dc_ctx_n * TC_RANS_DC_SYMS
                        + TC_RANS_RUN_SYMS + (size_t)lvl_ctx_n * TC_RANS_LVL_SYMS;
    if (payload_size < prefix + TC_RANS_STATE_BYTES) {
        tc_set_error(TC_ERR_TRUNCATED, "rans2 sparse payload %zu < prefix %zu + 4",
                     payload_size, prefix);
        return TC_ERR_TRUNCATED;
    }

    tc_rans_model dc_m[TC_RANS2_DC_CTX];
    tc_rans_model lvl_m[TC_RANS2_PREV_CTX];
    tc_rans_model run_m;
    size_t off = 1u;
    for (uint32_t c = 0u; c < dc_ctx_n; ++c) {
        if (tc_rans2_row_decode(payload + off, TC_RANS_DC_SYMS, &dc_m[c]) != TC_OK) {
            tc_set_error(TC_ERR_MALFORMED, "rans2 sparse dc row %u invalid", (unsigned)c);
            return TC_ERR_MALFORMED;
        }
        off += TC_RANS_DC_SYMS;
    }
    if (tc_rans2_row_decode(payload + off, TC_RANS_RUN_SYMS, &run_m) != TC_OK) {
        tc_set_error(TC_ERR_MALFORMED, "rans2 sparse run row invalid");
        return TC_ERR_MALFORMED;
    }
    off += TC_RANS_RUN_SYMS;
    for (uint32_t c = 0u; c < lvl_ctx_n; ++c) {
        if (tc_rans2_row_decode(payload + off, TC_RANS_LVL_SYMS, &lvl_m[c]) != TC_OK) {
            tc_set_error(TC_ERR_MALFORMED, "rans2 sparse lvl row %u invalid", (unsigned)c);
            return TC_ERR_MALFORMED;
        }
        off += TC_RANS_LVL_SYMS;
    }

    const uint32_t lut_n = dc_ctx_n + 1u + lvl_ctx_n;
    tc_rans_lut* luts = (tc_rans_lut*)tc_alloc((size_t)lut_n * sizeof(tc_rans_lut));
    if (luts == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "rans2 sparse luts");
        return TC_ERR_OUT_OF_MEMORY;
    }
    tc_rans_lut* dc_luts = luts;
    tc_rans_lut* run_lut = luts + dc_ctx_n;
    tc_rans_lut* lvl_luts = luts + dc_ctx_n + 1u;
    for (uint32_t c = 0u; c < dc_ctx_n; ++c) { tc_rans_lut_build(&dc_luts[c], &dc_m[c]); }
    tc_rans_lut_build(run_lut, &run_m);
    for (uint32_t c = 0u; c < lvl_ctx_n; ++c) { tc_rans_lut_build(&lvl_luts[c], &lvl_m[c]); }

    tc_rans_dec dec;
    int32_t rc = tc_rans_dec_init(&dec, payload + prefix, payload_size - prefix);
    if (rc != TC_OK) { tc_free(luts); return rc; }

    const uint32_t cols = fh->plane_block_cols[sh->plane];
    const uint32_t band = sh->block_h;
    memset(dc->prev_row, 0, (size_t)cols * sizeof(int32_t));
    memset(dc->row, 0, (size_t)cols * sizeof(int32_t));
    uint32_t prev_dc_cat = 0u;
    int first_block = 1;
    for (uint32_t by = 0u; by < band; ++by) {
        int32_t* dc_row_out = em->dc
            + ((size_t)em->block_y0 + by) * (size_t)em->plane_cols;
        uint32_t* off_row = em->off + (size_t)by * (size_t)cols;
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            off_row[bx] = em->cnt;
            const uint32_t dctx_d = first_block != 0
                ? 0u : tc_rans2_dc_ctx(prev_dc_cat, 1);
            const tc_rans_model* dmv = dc_model == TC_RANS2_DC_PREV
                ? &dc_m[dctx_d] : &dc_m[0];
            const tc_rans_lut* dlv = dc_model == TC_RANS2_DC_PREV
                ? &dc_luts[dctx_d] : &dc_luts[0];
            uint32_t cat = 0u;
            rc = tc_rans_get_lut(&dec, dlv, dmv, &cat);
            if (rc != TC_OK) { goto done; }
            uint32_t m = 0u;
            rc = tc_rans_get_suffix(&dec, cat, &m);
            if (rc != TC_OK) { goto done; }
            if (m > tc_rice_m_max_dc_bd(fh->bit_depth)) {
                tc_set_error(TC_ERR_MALFORMED, "rans2 sparse dc magnitude %u",
                             (unsigned)m);
                rc = TC_ERR_MALFORMED;
                goto done;
            }
            const int32_t pred = tc_dc_predict(bx != 0u,
                                               bx != 0u ? dc->row[bx - 1u] : 0,
                                               by != 0u, dc->prev_row[bx]);
            int32_t dcval = 0;
            rc = tc_dc_reconstruct_checked_bd(pred, m, fh->bit_depth, &dcval);
            if (rc != TC_OK) { goto done; }
            dc_row_out[bx] = dcval;
            dc->row[bx] = dcval;
            prev_dc_cat = cat;
            first_block = 0;

            uint32_t pos = 1u;
            uint32_t prev_cat = 0u;
            int has_prev = 0;
            for (;;) {
                uint32_t run_sym = 0u;
                rc = tc_rans_get_lut(&dec, run_lut, &run_m, &run_sym);
                if (rc != TC_OK) { goto done; }
                if (run_sym == 63u) { break; } /* EOB */
                const uint64_t idx = (uint64_t)pos + (uint64_t)run_sym;
                if (idx > 63u) {
                    tc_set_error(TC_ERR_MALFORMED, "rans2 sparse ac index %llu > 63",
                                 (unsigned long long)idx);
                    rc = TC_ERR_MALFORMED;
                    goto done;
                }
                const tc_rans_model* lmv;
                const tc_rans_lut* llv;
                if (lvl_model == TC_RANS2_LVL_POS) {
                    const uint32_t c = tc_rans2_pos_ctx((uint32_t)idx);
                    lmv = &lvl_m[c];
                    llv = &lvl_luts[c];
                } else if (lvl_model == TC_RANS2_LVL_PREV) {
                    const uint32_t c = tc_rans2_prevlvl_ctx(prev_cat, has_prev);
                    lmv = &lvl_m[c];
                    llv = &lvl_luts[c];
                } else {
                    lmv = &lvl_m[0];
                    llv = &lvl_luts[0];
                }
                rc = tc_rans_get_lut(&dec, llv, lmv, &cat);
                if (rc != TC_OK) { goto done; }
                rc = tc_rans_get_suffix(&dec, cat, &m);
                if (rc != TC_OK) { goto done; }
                if (m > tc_rice_m_max_level_bd(fh->bit_depth)) {
                    tc_set_error(TC_ERR_MALFORMED, "rans2 sparse level magnitude %u",
                                 (unsigned)m);
                    rc = TC_ERR_MALFORMED;
                    goto done;
                }
                const uint32_t natural = kTcZigzag[idx];
                em->pairs[em->cnt++] = ((uint64_t)(int64_t)tc_rice_unmap_signed(m) << 6)
                                    | (uint64_t)natural;
                prev_cat = cat;
                has_prev = 1;
                pos = (uint32_t)idx + 1u;
            }
        }
        tc_scan_dc_swap(dc);
    }
    em->off[(size_t)band * (size_t)cols] = em->cnt;
    if (dec.pos != dec.size) {
        tc_set_error(TC_ERR_MALFORMED, "rans2 sparse %zu trailing bytes",
                     dec.size - dec.pos);
        rc = TC_ERR_MALFORMED;
        goto done;
    }
    rc = TC_OK;
done:
    tc_free(luts);
    return rc;
}

int32_t tc_color_slice_decode_stream(const topos_frame_header* fh, const topos_slice_header* sh,
                                     const uint8_t* payload, size_t payload_size,
                                     tc_color_block_sink_fn sink, void* ctx,
                                     uint64_t* symbol_hash)
{
    if (sink == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "color stream sink == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* 解码深化批次：热路径整体替换为 color_scan.h 融合版（LUT/码书预提升 +
     * reader 状态跨块驻留 + 免 q[1..63] 非零扫描）；离散版语义对拍见
     * test_slice_codec（融合 vs tc_block_decode 逐截断位差分）。 */
    if (fh->version_major == 3u || tc_slice_use_range(fh)) {
        return tc_intra_slice_decode(fh, sh, payload, payload_size, NULL,
                                    symbol_hash, sink, ctx, NULL, NULL, NULL, 0u);
    }
    if (tc_slice_use_c1(fh) || tc_slice_use_c2(fh)) {
        tc_scan_dc_ctx dc;
        const size_t n = fh->plane_block_cols[sh->plane] > 0u
                       ? (size_t)fh->plane_block_cols[sh->plane] : 1u;
        dc.elems = n;
        dc.prev_row = (int32_t*)tc_alloc(n * sizeof(int32_t));
        dc.row = (int32_t*)tc_alloc(n * sizeof(int32_t));
        if (dc.prev_row == NULL || dc.row == NULL) {
            tc_free(dc.prev_row); tc_free(dc.row);
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "C1 color DC buffers");
            return TC_ERR_OUT_OF_MEMORY;
        }
        int32_t rc = tc_color_c1_decode_stream(fh, sh, payload, payload_size,
                                               sink, ctx, symbol_hash, &dc,
                                               tc_slice_use_c2(fh));
        tc_free(dc.prev_row); tc_free(dc.row);
        return rc;
    }
    if (tc_slice_use_rans(fh)) {
        tc_scan_dc_ctx dc;
        const size_t n = fh->plane_block_cols[sh->plane] > 0u
                       ? (size_t)fh->plane_block_cols[sh->plane] : 1u;
        dc.elems = n;
        dc.prev_row = (int32_t*)tc_alloc(n * sizeof(int32_t));
        dc.row = (int32_t*)tc_alloc(n * sizeof(int32_t));
        if (dc.prev_row == NULL || dc.row == NULL) {
            tc_free(dc.prev_row); tc_free(dc.row);
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "rans color DC buffers");
            return TC_ERR_OUT_OF_MEMORY;
        }
        int32_t rc = tc_color_rans_decode_core(fh, sh, payload, payload_size,
                                               sink, ctx, symbol_hash, &dc);
        tc_free(dc.prev_row); tc_free(dc.row);
        return rc;
    }
    if (tc_slice_use_rans2(fh)) {
        tc_scan_dc_ctx dc;
        const size_t n = fh->plane_block_cols[sh->plane] > 0u
                       ? (size_t)fh->plane_block_cols[sh->plane] : 1u;
        dc.elems = n;
        dc.prev_row = (int32_t*)tc_alloc(n * sizeof(int32_t));
        dc.row = (int32_t*)tc_alloc(n * sizeof(int32_t));
        if (dc.prev_row == NULL || dc.row == NULL) {
            tc_free(dc.prev_row); tc_free(dc.row);
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "rans2 color DC buffers");
            return TC_ERR_OUT_OF_MEMORY;
        }
        int32_t rc = tc_color_rans2_decode_core(fh, sh, payload, payload_size,
                                                sink, ctx, symbol_hash, &dc);
        tc_free(dc.prev_row); tc_free(dc.row);
        return rc;
    }
    return tc_color_scan_decode(fh, sh, payload, payload_size, sink, ctx, symbol_hash);
}

int32_t tc_color_slice_decode_stream_scratch(const topos_frame_header* fh,
                                     const topos_slice_header* sh,
                                     const uint8_t* payload, size_t payload_size,
                                     tc_color_block_sink_fn sink, void* ctx,
                                     uint64_t* symbol_hash, tc_scan_dc_ctx* dc)
{
    if (sink == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "color stream sink == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (dc == NULL || dc->prev_row == NULL || dc->row == NULL
            || dc->elems < (size_t)fh->plane_block_cols[sh->plane]) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "color stream dc scratch invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (fh->version_major == 3u || tc_slice_use_range(fh)) {
        return tc_intra_slice_decode(fh, sh, payload, payload_size, NULL,
                                    symbol_hash, sink, ctx, NULL, NULL, NULL, 0u);
    }
    if (tc_slice_use_c1(fh) || tc_slice_use_c2(fh)) {
        return tc_color_c1_decode_stream(fh, sh, payload, payload_size, sink, ctx,
                                         symbol_hash, dc, tc_slice_use_c2(fh));
    }
    if (tc_slice_use_rans(fh)) {
        return tc_color_rans_decode_core(fh, sh, payload, payload_size, sink, ctx,
                                         symbol_hash, dc);
    }
    if (tc_slice_use_rans2(fh)) {
        return tc_color_rans2_decode_core(fh, sh, payload, payload_size, sink, ctx,
                                          symbol_hash, dc);
    }
    return tc_color_scan_decode_scratch(fh, sh, payload, payload_size, sink, ctx,
                                        symbol_hash, dc);
}

int32_t tc_color_slice_decode_to_plane(const topos_frame_header* fh,
                                       const topos_slice_header* sh,
                                       const uint8_t* payload, size_t payload_size,
                                       tc_scan_dc_ctx* dc,
                                       struct tc_color_store_ctx* store)
{
    if (store == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "color to_plane store == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (dc == NULL || dc->prev_row == NULL || dc->row == NULL
            || dc->elems < (size_t)fh->plane_block_cols[sh->plane]) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "color to_plane dc scratch invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (fh->version_major == 3u || tc_slice_use_range(fh)) {
        return tc_intra_slice_decode(fh, sh, payload, payload_size, NULL,
                                    NULL, NULL, NULL, store, NULL, NULL, 0u);
    }
    if (tc_slice_use_c1(fh) || tc_slice_use_c2(fh)) {
        tc_c1_store_sink_ctx c = { store };
        return tc_color_c1_decode_stream(fh, sh, payload, payload_size,
                                         tc_c1_store_sink, &c, NULL, dc,
                                         tc_slice_use_c2(fh));
    }
    if (tc_slice_use_rans(fh)) {
        tc_c1_store_sink_ctx c = { store };
        return tc_color_rans_decode_core(fh, sh, payload, payload_size,
                                         tc_c1_store_sink, &c, NULL, dc);
    }
    if (tc_slice_use_rans2(fh)) {
        tc_c1_store_sink_ctx c = { store };
        return tc_color_rans2_decode_core(fh, sh, payload, payload_size,
                                          tc_c1_store_sink, &c, NULL, dc);
    }
    return tc_color_scan_to_plane(fh, sh, payload, payload_size, dc, store);
}

int32_t tc_alpha_slice_encode(const topos_frame_header* fh, const topos_slice_header* sh,
                              const int32_t* residuals, size_t residual_count, tc_bitwriter* bw)
{
    size_t expect = (size_t)fh->plane_coded_w[3] * ((size_t)sh->block_h * 8u);
    if (residual_count != expect) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "alpha residual count %zu != band pixels %zu",
                     residual_count, expect);
        return TC_ERR_INVALID_ARGUMENT;
    }
    return tc_alpha_residuals_encode(bw, sh->k1, sh->k2, residuals, residual_count);
}

int32_t tc_alpha_slice_decode(const topos_frame_header* fh, const topos_slice_header* sh,
                              const uint8_t* payload, size_t payload_size,
                              int32_t* residuals_out, size_t residual_count,
                              uint64_t* symbol_hash)
{
    size_t expect = (size_t)fh->plane_coded_w[3] * ((size_t)sh->block_h * 8u);
    if (residual_count != 0u && residual_count != expect) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "alpha residual count %zu != band pixels %zu",
                     residual_count, expect);
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (residual_count == 0u) { residual_count = expect; }

    /* 整带一次解码：run 可跨任意距离，终结符属于整带（分块会破坏语义）。
       residuals_out == NULL 时 O(1) 内存（域校验 + 指纹）。 */
    tc_bitreader br;
    tc_bitreader_init(&br, payload, payload_size);
    uint64_t hash = 0;
    int32_t rc = tc_alpha_residuals_decode(&br, sh->k1, sh->k2, residuals_out,
                                           residual_count, &hash);
    if (rc != TC_OK) { return rc; }

    int32_t arc = tc_bitreader_align_byte(&br);
    if (arc != TC_OK) { return arc; }
    if (tc_bitreader_bits_consumed(&br) != (uint64_t)payload_size * 8u) {
        tc_set_error(TC_ERR_MALFORMED, "alpha slice trailing bytes");
        return TC_ERR_MALFORMED;
    }
    if (symbol_hash != NULL) { *symbol_hash = hash; }
    return TC_OK;
}

int32_t tc_alpha_slice_decode_stream(const topos_frame_header* fh, const topos_slice_header* sh,
                                     const uint8_t* payload, size_t payload_size,
                                     tc_alpha_pair_sink2_fn sink, void* ctx,
                                     uint64_t* symbol_hash)
{
    if (sink == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "alpha stream sink == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    size_t expect = (size_t)fh->plane_coded_w[3] * ((size_t)sh->block_h * 8u);
    tc_bitreader br;
    tc_bitreader_init(&br, payload, payload_size);
    int32_t rc = tc_alpha_pairs_decode(&br, sh->k1, sh->k2, expect, sink, ctx, symbol_hash);
    if (rc != TC_OK) { return rc; }

    int32_t arc = tc_bitreader_align_byte(&br);
    if (arc != TC_OK) { return arc; }
    if (tc_bitreader_bits_consumed(&br) != (uint64_t)payload_size * 8u) {
        tc_set_error(TC_ERR_MALFORMED, "alpha slice trailing bytes");
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}
