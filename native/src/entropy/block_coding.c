#include "block_coding.h"

#include "rice.h"
#include "scan.h"
#include "vlc.h"

int32_t tc_dc_predict(int has_left, int32_t left_dc, int has_top, int32_t top_dc)
{
    if (has_left != 0 && has_top != 0) {
        /* round-half-up((left+top)/2) == floor((left+top+1)/2)；符号拆分，负数不右移 */
        int64_t s = (int64_t)left_dc + (int64_t)top_dc;
        if (s >= 0) { return (int32_t)((s + 1) >> 1); }
        return -(int32_t)((-s) >> 1);
    }
    if (has_left != 0) { return left_dc; }
    if (has_top != 0) { return top_dc; }
    return 0;
}

int32_t tc_block_encode(tc_bitwriter* bw, uint32_t k1, uint32_t k2, uint32_t k3,
                        const int32_t q_natural[64],
                        int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                        int32_t* dc_out)
{
    int32_t pred = tc_dc_predict(has_left, left_dc, has_top, top_dc);
    int32_t dc = q_natural[0];
    if (dc_out != NULL) { *dc_out = dc; }

    int32_t rc = tc_rice_encode_inline(bw, k1, tc_rice_map_signed(dc - pred));
    if (rc != TC_OK) { return rc; }

    /* 沿 zigzag 扫描 AC：输出 (run, level)，末尾 EOB（run=63） */
    uint32_t run = 0;
    for (uint32_t scan_pos = 1u; scan_pos < 64u; ++scan_pos) {
        int32_t coef = q_natural[kTcZigzag[scan_pos]];
        if (coef == 0) {
            run++;
            continue;
        }
        rc = tc_rice_encode_inline(bw, k3, run);
        if (rc != TC_OK) { return rc; }
        rc = tc_rice_encode_inline(bw, k2, tc_rice_map_signed(coef));
        if (rc != TC_OK) { return rc; }
        run = 0;
    }
    return tc_rice_encode_inline(bw, k3, (uint32_t)63u); /* EOB */
}

int32_t tc_block_encode_zigzag(tc_bitwriter* bw, uint32_t k1, uint32_t k2, uint32_t k3,
                               const int32_t q_zig[64],
                               int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                               int32_t* dc_out)
{
    int32_t pred = tc_dc_predict(has_left, left_dc, has_top, top_dc);
    int32_t dc = q_zig[0];
    if (dc_out != NULL) { *dc_out = dc; }

    int32_t rc = tc_rice_encode_inline(bw, k1, tc_rice_map_signed(dc - pred));
    if (rc != TC_OK) { return rc; }

    /* R6：输入已是扫描序 → 顺序读，无 zigzag 表间接寻址 */
    uint32_t run = 0;
    for (uint32_t scan_pos = 1u; scan_pos < 64u; ++scan_pos) {
        int32_t coef = q_zig[scan_pos];
        if (coef == 0) {
            run++;
            continue;
        }
        rc = tc_rice_encode_inline(bw, k3, run);
        if (rc != TC_OK) { return rc; }
        rc = tc_rice_encode_inline(bw, k2, tc_rice_map_signed(coef));
        if (rc != TC_OK) { return rc; }
        run = 0;
    }
    return tc_rice_encode_inline(bw, k3, (uint32_t)63u); /* EOB */
}

int32_t tc_block_decode(tc_bitreader* br, uint32_t k1, uint32_t k2, uint32_t k3,
                        int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                        int32_t q_natural[64])
{
    for (uint32_t i = 0; i < 64u; ++i) { q_natural[i] = 0; }

    uint32_t m = 0;
    int32_t rc = tc_rice_decode_inline(br, k1, TC_RICE_M_MAX_DC, &m);
    if (rc != TC_OK) { return rc; }
    int32_t pred = tc_dc_predict(has_left, left_dc, has_top, top_dc);
    /* P0-07：DC 域检查（spec §7.6 ±2^25），越界即恶意码流 */
    rc = tc_dc_reconstruct_checked(pred, m, &q_natural[0]);
    if (rc != TC_OK) {
        tc_bitreader_fail(br, rc);
        return rc;
    }

    /* pos = 下一个待填充 AC 的 zigzag 位置（1..64）；每块 ≤ 63 对 + EOB */
    uint32_t pos = 1u;
    for (;;) {
        rc = tc_rice_decode_inline(br, k3, TC_RICE_M_MAX_RUN, &m);
        if (rc != TC_OK) { return rc; }
        if (m == 63u) { return TC_OK; } /* EOB */
        uint64_t idx = (uint64_t)pos + (uint64_t)m;
        if (idx > 63u) {
            tc_bitreader_fail(br, TC_ERR_MALFORMED);
            return TC_ERR_MALFORMED;
        }
        rc = tc_rice_decode_inline(br, k2, TC_RICE_M_MAX_AC_LEVEL, &m);
        if (rc != TC_OK) { return rc; }
        q_natural[kTcZigzag[idx]] = tc_rice_unmap_signed(m);
        pos = (uint32_t)idx + 1u;
    }
}

/* ---- V2 canonical VLC 变体（spec v2 §4.1）---- */

int32_t tc_block_encode_zigzag_vlc(tc_bitwriter* bw,
                                   const tc_vlc_book* dc_book,
                                   const tc_vlc_book* lvl_book,
                                   const tc_vlc_book* run_book,
                                   const int32_t q_zig[64],
                                   int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                                   int32_t* dc_out)
{
    int32_t pred = tc_dc_predict(has_left, left_dc, has_top, top_dc);
    int32_t dc = q_zig[0];
    if (dc_out != NULL) { *dc_out = dc; }

    int32_t rc = tc_vlc_put_cat(bw, dc_book, tc_rice_map_signed(dc - pred));
    if (rc != TC_OK) { return rc; }

    uint32_t run = 0;
    for (uint32_t scan_pos = 1u; scan_pos < 64u; ++scan_pos) {
        int32_t coef = q_zig[scan_pos];
        if (coef == 0) {
            run++;
            continue;
        }
        rc = tc_vlc_put_sym(bw, run_book, run);
        if (rc != TC_OK) { return rc; }
        rc = tc_vlc_put_cat(bw, lvl_book, tc_rice_map_signed(coef));
        if (rc != TC_OK) { return rc; }
        run = 0;
    }
    return tc_vlc_put_sym(bw, run_book, 63u); /* EOB */
}

int32_t tc_block_decode_vlc(tc_bitreader* br,
                            const tc_vlc_book* dc_book,
                            const tc_vlc_book* lvl_book,
                            const tc_vlc_book* run_book,
                            int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                            int32_t q_natural[64])
{
    for (uint32_t i = 0; i < 64u; ++i) { q_natural[i] = 0; }

    /* DC_CAT 融合解码（码字+后缀+域校验一次完成）；LVL 表 cat=0 无码。 */
    uint32_t m = 0;
    int32_t rc = tc_vlc_decode_cat(br, dc_book, TC_RICE_M_MAX_DC, &m);
    if (rc != TC_OK) { return rc; }
    int32_t pred = tc_dc_predict(has_left, left_dc, has_top, top_dc);
    /* P0-07：DC 域检查（spec §7.6 ±2^25），越界即恶意码流 */
    rc = tc_dc_reconstruct_checked(pred, m, &q_natural[0]);
    if (rc != TC_OK) {
        tc_bitreader_fail(br, rc);
        return rc;
    }

    uint32_t pos = 1u;
    for (;;) {
        /* P1：run+lvl 融合对解码（共享错误检查/refill；EOB 由 is_eob 区分） */
        uint32_t run_sym = 0u, lvl_m = 0u, is_eob = 0u;
        rc = tc_vlc_decode_run_lvl(br, run_book, lvl_book,
                                   TC_RICE_M_MAX_AC_LEVEL, &run_sym, &lvl_m,
                                   &is_eob);
        if (rc != TC_OK) { return rc; }
        if (is_eob != 0u) { return TC_OK; } /* EOB */
        uint64_t idx = (uint64_t)pos + (uint64_t)run_sym;
        if (idx > 63u) {
            tc_bitreader_fail(br, TC_ERR_MALFORMED);
            return TC_ERR_MALFORMED;
        }
        q_natural[kTcZigzag[idx]] = tc_rice_unmap_signed(lvl_m);
        pos = (uint32_t)idx + 1u;
    }
}

/* ---- C1 experimental joint AC syntax ---- */

static int32_t c1_put_suffix(tc_bitwriter* bw, uint32_t cat, uint32_t m)
{
    const uint32_t n = cat > 1u ? cat - 1u : 0u;
    return n == 0u ? TC_OK : tc_bitwriter_put_bits_inline(bw, n,
                                                           m & ((1u << n) - 1u));
}

static int32_t c1_get_suffix(tc_bitreader* br, uint32_t cat, uint32_t* m)
{
    const uint32_t n = cat > 1u ? cat - 1u : 0u;
    uint32_t suffix = 0u;
    int32_t rc = tc_bitreader_read_bits(br, n, &suffix);
    if (rc != TC_OK) { return rc; }
    *m = cat == 0u ? 0u : (1u << (cat - 1u)) | suffix;
    return TC_OK;
}

static int32_t c1_put_pair(tc_bitwriter* bw, const tc_vlc_book* lvl_book,
                           const tc_vlc_book* pair_book, uint32_t run, uint32_t m)
{
    (void)pair_book;
    const uint32_t cat = tc_vlc_bitlen32(m);
    if (run < 8u && cat >= 1u && cat <= 7u) {
        /* id 0..55: seven categories for each of eight short runs.  C1 uses
         * a fixed six-bit token so the frozen V2 RUN code lengths cannot turn
         * a compact joint symbol into a long codeword. */
        const uint32_t id = run * 7u + (cat - 1u);
        int32_t rc = tc_bitwriter_put_bits_inline(bw, 6u, id);
        if (rc != TC_OK) { return rc; }
        return c1_put_suffix(bw, cat, m);
    }
    /* id 62 is an explicit escape.  The raw run is six bits (0..62), then
     * the existing LEVEL_CAT code carries the full signed magnitude. */
    int32_t rc = tc_bitwriter_put_bits_inline(bw, 6u, 62u);
    if (rc != TC_OK) { return rc; }
    rc = tc_bitwriter_put_bits_inline(bw, 6u, run);
    if (rc != TC_OK) { return rc; }
    return tc_vlc_put_cat(bw, lvl_book, m);
}

int32_t tc_block_encode_zigzag_c1(tc_bitwriter* bw,
                                  const tc_vlc_book* dc_book,
                                  const tc_vlc_book* lvl_book,
                                  const tc_vlc_book* pair_book,
                                  const int32_t q_zig[64],
                                  int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                                  int32_t* dc_out)
{
    const int32_t pred = tc_dc_predict(has_left, left_dc, has_top, top_dc);
    const int32_t dc = q_zig[0];
    if (dc_out != NULL) { *dc_out = dc; }
    int32_t rc = tc_vlc_put_cat(bw, dc_book, tc_rice_map_signed(dc - pred));
    if (rc != TC_OK) { return rc; }
    uint32_t run = 0u;
    for (uint32_t scan_pos = 1u; scan_pos < 64u; ++scan_pos) {
        const int32_t coef = q_zig[scan_pos];
        if (coef == 0) { run++; continue; }
        rc = c1_put_pair(bw, lvl_book, pair_book, run,
                         tc_rice_map_signed(coef));
        if (rc != TC_OK) { return rc; }
        run = 0u;
    }
    (void)pair_book;
    return tc_bitwriter_put_bits_inline(bw, 6u, 63u); /* EOB */
}

int32_t tc_block_decode_c1(tc_bitreader* br,
                           const tc_vlc_book* dc_book,
                           const tc_vlc_book* lvl_book,
                           const tc_vlc_book* pair_book,
                           int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                           int32_t q_natural[64])
{
    for (uint32_t i = 0u; i < 64u; ++i) { q_natural[i] = 0; }
    uint32_t m = 0u;
    int32_t rc = tc_vlc_decode_cat(br, dc_book, TC_RICE_M_MAX_DC, &m);
    if (rc != TC_OK) { return rc; }
    const int32_t pred = tc_dc_predict(has_left, left_dc, has_top, top_dc);
    rc = tc_dc_reconstruct_checked(pred, m, &q_natural[0]);
    if (rc != TC_OK) { tc_bitreader_fail(br, rc); return rc; }

    uint32_t pos = 1u;
    for (;;) {
        uint32_t id = 0u;
        (void)pair_book;
        rc = tc_bitreader_read_bits(br, 6u, &id);
        if (rc != TC_OK) { return rc; }
        if (id == 63u) { return TC_OK; }
        uint32_t run = 0u;
        uint32_t level_m = 0u;
        if (id == 62u) {
            rc = tc_bitreader_read_bits(br, 6u, &run);
            if (rc != TC_OK) { return rc; }
            if (run > 62u) { tc_bitreader_fail(br, TC_ERR_MALFORMED); return TC_ERR_MALFORMED; }
            rc = tc_vlc_decode_cat(br, lvl_book, TC_RICE_M_MAX_AC_LEVEL, &level_m);
            if (rc != TC_OK) { return rc; }
        } else if (id < 56u) {
            run = id / 7u;
            const uint32_t cat = (id % 7u) + 1u;
            rc = c1_get_suffix(br, cat, &level_m);
            if (rc != TC_OK) { return rc; }
        } else {
            tc_bitreader_fail(br, TC_ERR_MALFORMED);
            return TC_ERR_MALFORMED;
        }
        const uint64_t idx = (uint64_t)pos + (uint64_t)run;
        if (idx > 63u || level_m == 0u) {
            tc_bitreader_fail(br, TC_ERR_MALFORMED);
            return TC_ERR_MALFORMED;
        }
        q_natural[kTcZigzag[idx]] = tc_rice_unmap_signed(level_m);
        pos = (uint32_t)idx + 1u;
    }
}

/* ---- C2 per-slice canonical pair table ---- */

static uint32_t c2_pair_id(uint32_t run, uint32_t cat)
{
    return run < 8u && cat >= 1u && cat <= 7u
             ? run * 7u + (cat - 1u) : TC_C2_PAIR_ESCAPE;
}

int32_t tc_c2_pair_book_build(const int32_t* q_blocks, size_t block_count,
                              tc_vlc_book* book, uint8_t lengths_out[64])
{
    if (q_blocks == NULL || book == NULL || lengths_out == NULL || block_count == 0u) {
        return TC_ERR_INVALID_ARGUMENT;
    }
    uint64_t weight[TC_C2_PAIR_SYMS];
    int32_t parent[2u * TC_C2_PAIR_SYMS];
    uint64_t merged_weight[2u * TC_C2_PAIR_SYMS];
    for (uint32_t i = 0u; i < TC_C2_PAIR_SYMS; ++i) { weight[i] = 1u; parent[i] = -1; }
    for (size_t bi = 0u; bi < block_count; ++bi) {
        const int32_t* q = q_blocks + bi * 64u;
        uint32_t run = 0u;
        for (uint32_t sp = 1u; sp < 64u; ++sp) {
            const int32_t coef = q[sp];
            if (coef == 0) { run++; continue; }
            const uint32_t cat = tc_vlc_bitlen32(tc_rice_map_signed(coef));
            weight[c2_pair_id(run, cat)]++;
            run = 0u;
        }
        weight[TC_C2_PAIR_EOB]++;
    }
    for (uint32_t i = 0u; i < 2u * TC_C2_PAIR_SYMS; ++i) {
        merged_weight[i] = i < TC_C2_PAIR_SYMS ? weight[i] : 0u;
        parent[i] = -1;
    }
    uint32_t nodes = TC_C2_PAIR_SYMS;
    while (nodes < 2u * TC_C2_PAIR_SYMS - 1u) {
        int32_t a = -1, b = -1;
        for (uint32_t i = 0u; i < nodes; ++i) {
            if (parent[i] != -1) { continue; }
            if (a < 0 || merged_weight[i] < merged_weight[(uint32_t)a] ||
                (merged_weight[i] == merged_weight[(uint32_t)a] && i < (uint32_t)a)) {
                b = a; a = (int32_t)i;
            } else if (b < 0 || merged_weight[i] < merged_weight[(uint32_t)b] ||
                       (merged_weight[i] == merged_weight[(uint32_t)b] && i < (uint32_t)b)) {
                b = (int32_t)i;
            }
        }
        if (a < 0 || b < 0) { return TC_ERR_STATE; }
        parent[(uint32_t)a] = (int32_t)nodes;
        parent[(uint32_t)b] = (int32_t)nodes;
        merged_weight[nodes] = merged_weight[(uint32_t)a] + merged_weight[(uint32_t)b];
        parent[nodes] = -1;
        nodes++;
    }
    uint8_t lengths[64] = { 0 };
    for (uint32_t s = 0u; s < TC_C2_PAIR_SYMS; ++s) {
        uint32_t depth = 0u;
        for (int32_t n = (int32_t)s; parent[(uint32_t)n] >= 0; n = parent[(uint32_t)n]) {
            depth++;
        }
        if (depth == 0u || depth > TC_VLC_MAX_CODE_BITS) { return TC_ERR_LIMIT_EXCEEDED; }
        lengths[s] = (uint8_t)depth;
        lengths_out[s] = (uint8_t)depth;
    }
    for (uint32_t s = TC_C2_PAIR_SYMS; s < 64u; ++s) { lengths_out[s] = 0u; }
    return tc_vlc_book_build(book, lengths, TC_C2_PAIR_SYMS, TC_VLC_KIND_RUN);
}

static int32_t c2_put_pair(tc_bitwriter* bw, const tc_vlc_book* lvl_book,
                           const tc_vlc_book* pair_book, uint32_t run, uint32_t m)
{
    const uint32_t cat = tc_vlc_bitlen32(m);
    int32_t rc = tc_vlc_put_sym(bw, pair_book, c2_pair_id(run, cat));
    if (rc != TC_OK) { return rc; }
    if (run < 8u && cat >= 1u && cat <= 7u) {
        const uint32_t n = cat > 1u ? cat - 1u : 0u;
        return n == 0u ? TC_OK : tc_bitwriter_put_bits_inline(bw, n, m & ((1u << n) - 1u));
    }
    rc = tc_bitwriter_put_bits_inline(bw, 6u, run);
    if (rc != TC_OK) { return rc; }
    return tc_vlc_put_cat(bw, lvl_book, m);
}

int32_t tc_block_encode_zigzag_c2(tc_bitwriter* bw,
                                  const tc_vlc_book* dc_book,
                                  const tc_vlc_book* lvl_book,
                                  const tc_vlc_book* pair_book,
                                  const int32_t q_zig[64],
                                  int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                                  int32_t* dc_out)
{
    const int32_t pred = tc_dc_predict(has_left, left_dc, has_top, top_dc);
    const int32_t dc = q_zig[0];
    if (dc_out != NULL) { *dc_out = dc; }
    int32_t rc = tc_vlc_put_cat(bw, dc_book, tc_rice_map_signed(dc - pred));
    if (rc != TC_OK) { return rc; }
    uint32_t run = 0u;
    for (uint32_t sp = 1u; sp < 64u; ++sp) {
        if (q_zig[sp] == 0) { run++; continue; }
        rc = c2_put_pair(bw, lvl_book, pair_book, run, tc_rice_map_signed(q_zig[sp]));
        if (rc != TC_OK) { return rc; }
        run = 0u;
    }
    return tc_vlc_put_sym(bw, pair_book, TC_C2_PAIR_EOB);
}

int32_t tc_block_decode_c2(tc_bitreader* br,
                           const tc_vlc_book* dc_book,
                           const tc_vlc_book* lvl_book,
                           const tc_vlc_book* pair_book,
                           int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                           int32_t q_natural[64])
{
    for (uint32_t i = 0u; i < 64u; ++i) { q_natural[i] = 0; }
    uint32_t m = 0u;
    int32_t rc = tc_vlc_decode_cat(br, dc_book, TC_RICE_M_MAX_DC, &m);
    if (rc != TC_OK) { return rc; }
    const int32_t pred = tc_dc_predict(has_left, left_dc, has_top, top_dc);
    rc = tc_dc_reconstruct_checked(pred, m, &q_natural[0]);
    if (rc != TC_OK) { tc_bitreader_fail(br, rc); return rc; }
    uint32_t pos = 1u;
    for (;;) {
        uint32_t id = 0u;
        rc = tc_vlc_decode_sym(br, pair_book, &id);
        if (rc != TC_OK) { return rc; }
        if (id == TC_C2_PAIR_EOB) { return TC_OK; }
        uint32_t run = 0u, level_m = 0u;
        if (id == TC_C2_PAIR_ESCAPE) {
            rc = tc_bitreader_read_bits(br, 6u, &run);
            if (rc != TC_OK) { return rc; }
            if (run > 62u) { tc_bitreader_fail(br, TC_ERR_MALFORMED); return TC_ERR_MALFORMED; }
            rc = tc_vlc_decode_cat(br, lvl_book, TC_RICE_M_MAX_AC_LEVEL, &level_m);
            if (rc != TC_OK) { return rc; }
        } else if (id < 56u) {
            run = id / 7u;
            const uint32_t cat = (id % 7u) + 1u;
            const uint32_t n = cat > 1u ? cat - 1u : 0u;
            uint32_t suffix = 0u;
            rc = tc_bitreader_read_bits(br, n, &suffix);
            if (rc != TC_OK) { return rc; }
            level_m = cat == 0u ? 0u : (1u << (cat - 1u)) | suffix;
        } else {
            tc_bitreader_fail(br, TC_ERR_MALFORMED); return TC_ERR_MALFORMED;
        }
        const uint64_t idx = (uint64_t)pos + run;
        if (idx > 63u || level_m == 0u) {
            tc_bitreader_fail(br, TC_ERR_MALFORMED); return TC_ERR_MALFORMED;
        }
        q_natural[kTcZigzag[idx]] = tc_rice_unmap_signed(level_m);
        pos = (uint32_t)idx + 1u;
    }
}

int32_t tc_alpha_residuals_encode(tc_bitwriter* bw, uint32_t k_level, uint32_t k_run,
                                  const int32_t* residuals, size_t count)
{
    size_t pos = 0;
    uint64_t run = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t r = residuals[i];
        if (r == 0) {
            run++;
            continue;
        }
        int32_t rc = tc_rice_encode_inline(bw, k_run, (uint32_t)run);
        if (rc != TC_OK) { return rc; }
        rc = tc_rice_encode_inline(bw, k_level, tc_rice_map_signed(r));
        if (rc != TC_OK) { return rc; }
        pos = i + 1u;
        run = 0;
    }
    if ((uint64_t)count - (uint64_t)pos > 0u) {
        /* 尾零终结对 (trailing_run, level=0)：pos..count−1 全零 */
        int32_t rc = tc_rice_encode_inline(bw, k_run, (uint32_t)((uint64_t)count - (uint64_t)pos));
        if (rc != TC_OK) { return rc; }
        return tc_rice_encode_inline(bw, k_level, 0u);
    }
    return TC_OK;
}

int32_t tc_alpha_residuals_decode(tc_bitreader* br, uint32_t k_level, uint32_t k_run,
                                  int32_t* residuals_out, size_t count, uint64_t* hash_acc)
{
    if (residuals_out != NULL) {
        for (size_t i = 0; i < count; ++i) { residuals_out[i] = 0; }
    }
    uint64_t hash = hash_acc != NULL ? *hash_acc : 0u;
    size_t pos = 0;
    while (pos < count) {
        uint64_t remaining = (uint64_t)count - (uint64_t)pos;
        uint32_t run_m = 0;
        uint32_t level_m = 0;
        int32_t rc = tc_rice_decode_inline(br, k_run, (uint32_t)remaining, &run_m);
        if (rc != TC_OK) { return rc; }
        rc = tc_rice_decode_inline(br, k_level, TC_RICE_M_MAX_ALPHA_LEVEL, &level_m);
        if (rc != TC_OK) { return rc; }
        if (level_m == 0u) {
            /* 终结对：run 必须 == remaining（恰好填满尾零） */
            if ((uint64_t)run_m != remaining) {
                tc_bitreader_fail(br, TC_ERR_MALFORMED);
                return TC_ERR_MALFORMED;
            }
            for (uint64_t i = 0; i < remaining; ++i) {
                hash = tc_symbol_hash_mix(hash, 0u);
            }
            break;
        }
        /* 数据对：level 必须落在剩余像素内（run ≤ remaining−1） */
        if ((uint64_t)run_m + 1u > remaining) {
            tc_bitreader_fail(br, TC_ERR_MALFORMED);
            return TC_ERR_MALFORMED;
        }
        int32_t level = tc_rice_unmap_signed(level_m);
        for (uint32_t i = 0; i < run_m; ++i) {
            hash = tc_symbol_hash_mix(hash, 0u);
        }
        hash = tc_symbol_hash_mix(hash, (uint64_t)(int64_t)level);
        if (residuals_out != NULL) {
            residuals_out[pos + (size_t)run_m] = level;
        }
        pos += (size_t)run_m + 1u;
    }
    if (hash_acc != NULL) { *hash_acc = hash; }
    return TC_OK;
}

int32_t tc_alpha_pairs_decode(tc_bitreader* br, uint32_t k_level, uint32_t k_run,
                              size_t count, tc_alpha_pair_sink_fn sink, void* ctx,
                              uint64_t* hash_acc)
{
    (void)hash_acc; /* 流式回调路径不折叠指纹（调用方在 sink 内自行累计） */
    size_t pos = 0;
    while (pos < count) {
        uint64_t remaining = (uint64_t)count - (uint64_t)pos;
        uint32_t run_m = 0;
        uint32_t level_m = 0;
        int32_t rc = tc_rice_decode_inline(br, k_run, (uint32_t)remaining, &run_m);
        if (rc != TC_OK) { return rc; }
        rc = tc_rice_decode_inline(br, k_level, TC_RICE_M_MAX_ALPHA_LEVEL, &level_m);
        if (rc != TC_OK) { return rc; }
        if (level_m == 0u) {
            if ((uint64_t)run_m != remaining) {
                tc_bitreader_fail(br, TC_ERR_MALFORMED);
                return TC_ERR_MALFORMED;
            }
            rc = sink(ctx, pos, run_m, 0);
            return rc == TC_OK ? TC_OK : rc;
        }
        if ((uint64_t)run_m + 1u > remaining) {
            tc_bitreader_fail(br, TC_ERR_MALFORMED);
            return TC_ERR_MALFORMED;
        }
        int32_t level = tc_rice_unmap_signed(level_m);
        rc = sink(ctx, pos, run_m, level);
        if (rc != TC_OK) { return rc; }
        pos += (size_t)run_m + 1u;
    }
    return TC_OK;
}
