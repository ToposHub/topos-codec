/* RD2-06：V2 canonical VLC 与 V7-A scalar band 语法的确定性体积门。
 *
 * 该工具不编码像素，而是在同一组确定性的量化 zigzag 系数上比较两种
 * bitstream 语法。这样可以把“语法开销”与 DCT/量化/RDO 噪声隔离；V7-A
 * 的目录、segment header、active-block map、尾部 padding 均计入总包大小。
 */
#include "bitstream/band_codec.h"
#include "bitstream/frame_header.h"
#include "bitstream/slice_codec.h"
#include "bitstream/slice_map.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "entropy/block_coding.h"
#include "entropy/rice.h"
#include "entropy/scan.h"
#include "entropy/vlc.h"

#define V7_DIRECTORY_BYTES 432u /* 36 + 24 + 24×3 + 20×(3×5) */
#define V7_PLANE_COUNT 3u
#define V7_BAND_COUNT 5u
#define HIST_MAX_M 4096u

typedef enum test_pattern {
    TEST_FLAT = 0,
    TEST_LOW = 1,
    TEST_TEXTURE = 2,
    TEST_NOISE = 3
} test_pattern;

typedef struct size_case {
    uint16_t width;
    uint16_t height;
    test_pattern pattern;
    const char* name;
    uint64_t seed;
} size_case;

typedef struct plane_result {
    uint64_t v2_bits;
    uint64_t v7_bits;
    uint64_t v2_eob_bits;
    uint64_t v7_active_map_bits;
    uint64_t v7_pair_map_bits;
    uint64_t v7_selected_map_bits;
    uint64_t v7_padding_bits;
    uint32_t v2_bytes;
    uint32_t v7_bytes;
    uint32_t tokens[V7_BAND_COUNT];
    uint32_t active_blocks[V7_BAND_COUNT];
} plane_result;

static uint64_t next_random(uint64_t* state)
{
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * UINT64_C(0x2545F4914F6CDD1D);
}

static uint32_t ceil_log2_u32(uint32_t value)
{
    uint32_t bits = 0u;
    uint32_t limit = 1u;
    while (limit < value && bits < 32u) {
        limit <<= 1u;
        ++bits;
    }
    return bits == 0u ? 1u : bits;
}

static int should_emit_ac(test_pattern pattern, uint32_t scan_pos, uint64_t random)
{
    switch (pattern) {
    case TEST_FLAT:
        return 0;
    case TEST_LOW:
        return scan_pos <= 10u && (random & 3u) == 0u;
    case TEST_TEXTURE:
        return (random % 3u) == 0u;
    case TEST_NOISE:
        return (random & 1u) == 0u;
    default:
        return 0;
    }
}

static uint32_t pattern_magnitude(test_pattern pattern, uint64_t random)
{
    switch (pattern) {
    case TEST_LOW:
        return (uint32_t)(random % 8u) + 1u;
    case TEST_TEXTURE:
        return (uint32_t)(random % 127u) + 1u;
    case TEST_NOISE:
        return (uint32_t)(random % 2047u) + 1u;
    default:
        return 1u;
    }
}

static void fill_q_zig(int32_t* q_zig, size_t block_count, test_pattern pattern,
                       uint64_t seed)
{
    uint64_t state = seed;
    int32_t dc = 256 + (int32_t)(next_random(&state) % 128u);
    for (size_t block = 0u; block < block_count; ++block) {
        int32_t* q = q_zig + block * 64u;
        memset(q, 0, 64u * sizeof(*q));
        dc += (int32_t)(next_random(&state) % 5u) - 2;
        q[0] = dc;
        for (uint32_t scan_pos = 1u; scan_pos < 64u; ++scan_pos) {
            uint64_t random = next_random(&state);
            if (should_emit_ac(pattern, scan_pos, random) == 0) { continue; }
            int32_t magnitude = (int32_t)pattern_magnitude(pattern, random >> 8);
            q[scan_pos] = (random & 1u) != 0u ? magnitude : -magnitude;
        }
    }
}

static int32_t make_v2_header(uint16_t width, uint16_t height,
                              topos_frame_header* fh)
{
    memset(fh, 0, sizeof(*fh));
    fh->version_major = 2u;
    fh->profile = 3u;
    fh->pixel_format = 0u;
    fh->bit_depth = 10u;
    fh->coded_width = (uint16_t)(((uint32_t)width + 7u) / 8u * 8u);
    fh->coded_height = (uint16_t)(((uint32_t)height + 7u) / 8u * 8u);
    fh->visible_width = width;
    fh->visible_height = height;
    fh->plane_count = V7_PLANE_COUNT;
    fh->qmatrix_id = 0u;
    fh->slice_count = V7_PLANE_COUNT;
    fh->entropy_mode = 1u;
    fh->codebook_version = 1u;
    fh->coding_mode = 0u;
    return tc_frame_derive_geometry(fh);
}

static uint64_t vlc_hist_bits(const uint32_t* hist, uint32_t count,
                              const tc_vlc_book* book, int is_category)
{
    uint64_t bits = 0u;
    for (uint32_t symbol = 0u; symbol < count; ++symbol) {
        if (hist[symbol] == 0u) { continue; }
        uint32_t symbol_bits = (uint32_t)book->len[symbol];
        if (is_category != 0 && symbol > 1u) { symbol_bits += symbol - 1u; }
        bits += (uint64_t)hist[symbol] * symbol_bits;
    }
    return bits;
}

static int32_t choose_v2_plane(const topos_frame_header* fh, uint32_t plane,
                               const int32_t* q_zig, uint32_t cols, uint32_t rows,
                               plane_result* result)
{
    uint32_t dc_hist[TC_VLC_DC_SYMS] = { 0u };
    uint32_t level_hist[TC_VLC_LVL_SYMS] = { 0u };
    uint32_t run_hist[TC_VLC_RUN_SYMS] = { 0u };
    int32_t* prev_row = (int32_t*)calloc(cols, sizeof(*prev_row));
    int32_t* row = (int32_t*)calloc(cols, sizeof(*row));
    if (prev_row == NULL || row == NULL) {
        free(prev_row);
        free(row);
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "v7 size gate v2 dc rows");
        return TC_ERR_OUT_OF_MEMORY;
    }

    const size_t blocks = (size_t)cols * (size_t)rows;
    for (size_t block = 0u; block < blocks; ++block) {
        uint32_t by = (uint32_t)(block / cols);
        uint32_t bx = (uint32_t)(block % cols);
        int has_left = bx > 0u ? 1 : 0;
        int has_top = by > 0u ? 1 : 0;
        int32_t pred = tc_dc_predict(has_left, has_left ? row[bx - 1u] : 0,
                                     has_top, has_top ? prev_row[bx] : 0);
        const int32_t* q = q_zig + block * 64u;
        uint32_t dc_m = tc_rice_map_signed(q[0] - pred);
        if (tc_vlc_bitlen32(dc_m) >= TC_VLC_DC_SYMS) {
            free(prev_row); free(row);
            return TC_ERR_LIMIT_EXCEEDED;
        }
        dc_hist[tc_vlc_bitlen32(dc_m)]++;
        row[bx] = q[0];

        uint32_t run = 0u;
        for (uint32_t scan_pos = 1u; scan_pos < 64u; ++scan_pos) {
            if (q[scan_pos] == 0) {
                ++run;
                continue;
            }
            uint32_t level_m = tc_rice_map_signed(q[scan_pos]);
            uint32_t level_cat = tc_vlc_bitlen32(level_m);
            if (level_cat >= TC_VLC_LVL_SYMS || run >= TC_VLC_RUN_SYMS) {
                free(prev_row); free(row);
                return TC_ERR_LIMIT_EXCEEDED;
            }
            run_hist[run]++;
            level_hist[level_cat]++;
            run = 0u;
        }
        run_hist[63u]++;
        if (bx + 1u == cols) {
            int32_t* tmp = prev_row; prev_row = row; row = tmp;
        }
    }
    free(prev_row);
    free(row);

    uint64_t best_bits = UINT64_MAX;
    uint8_t best_dc = 0u, best_level = 0u, best_run = 0u;
    for (uint32_t dc_book = 0u; dc_book < 4u; ++dc_book) {
        int32_t rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_DC, dc_book);
        if (rc != TC_OK) { return rc; }
        for (uint32_t level_book = 0u; level_book < 4u; ++level_book) {
            rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_LVL, level_book);
            if (rc != TC_OK) { return rc; }
            for (uint32_t run_book = 0u; run_book < 4u; ++run_book) {
                rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_RUN, run_book);
                if (rc != TC_OK) { return rc; }
                uint64_t bits = vlc_hist_bits(dc_hist, TC_VLC_DC_SYMS,
                                              tc_vlc_book_get(TC_VLC_FAMILY_DC, dc_book), 1);
                bits += vlc_hist_bits(level_hist, TC_VLC_LVL_SYMS,
                                      tc_vlc_book_get(TC_VLC_FAMILY_LVL, level_book), 1);
                bits += vlc_hist_bits(run_hist, TC_VLC_RUN_SYMS,
                                      tc_vlc_book_get(TC_VLC_FAMILY_RUN, run_book), 0);
                if (bits < best_bits) {
                    best_bits = bits;
                    best_dc = (uint8_t)dc_book;
                    best_level = (uint8_t)level_book;
                    best_run = (uint8_t)run_book;
                }
            }
        }
    }

    topos_slice_header sh;
    memset(&sh, 0, sizeof(sh));
    sh.plane = (uint8_t)plane;
    sh.block_h = (uint16_t)rows;
    sh.qp_delta_biased = 64u;
    sh.k1 = best_dc;
    sh.k2 = best_level;
    sh.k3 = best_run;
    tc_bitwriter bw;
    int32_t rc = tc_bitwriter_init(&bw);
    if (rc != TC_OK) { return rc; }
    rc = tc_color_slice_encode(fh, &sh, q_zig, &bw);
    if (rc == TC_OK && tc_bitwriter_bits_written(&bw) != best_bits) {
        tc_set_error(TC_ERR_STATE, "v7 size gate V2 histogram mismatch");
        rc = TC_ERR_STATE;
    }
    tc_bitwriter_free(&bw);
    if (rc != TC_OK) { return rc; }

    result->v2_bits = best_bits;
    result->v2_bytes = (uint32_t)((best_bits + 7u) / 8u);
    result->v2_eob_bits = (uint64_t)tc_vlc_book_get(TC_VLC_FAMILY_RUN, best_run)->len[63u] * blocks;
    return TC_OK;
}

static int32_t build_band_input(const int32_t* q_zig, uint32_t cols, uint32_t rows,
                                uint32_t band, tc_v7_band_input* input,
                                uint32_t* active_blocks_out)
{
    static const uint32_t starts[V7_BAND_COUNT] = { 1u, 5u, 11u, 17u, 25u };
    static const uint32_t ends[V7_BAND_COUNT] = { 5u, 11u, 17u, 25u, 64u };
    const uint32_t block_count = cols * rows;
    uint16_t* pair_count = (uint16_t*)calloc(block_count, sizeof(*pair_count));
    if (pair_count == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "v7 size gate pair counts");
        return TC_ERR_OUT_OF_MEMORY;
    }
    uint32_t active_blocks = 0u;
    uint64_t token_count64 = 0u;
    for (uint32_t block = 0u; block < block_count; ++block) {
        uint32_t count = 0u;
        const int32_t* q = q_zig + (size_t)block * 64u;
        for (uint32_t pos = starts[band]; pos < ends[band]; ++pos) {
            if (q[pos] != 0) { ++count; }
        }
        pair_count[block] = (uint16_t)count;
        if (count != 0u) { ++active_blocks; }
        token_count64 += count;
    }
    if (token_count64 > UINT32_MAX) {
        free(pair_count);
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7 size gate token count");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    uint32_t token_count = (uint32_t)token_count64;
    uint8_t* local_runs = token_count == 0u ? NULL : (uint8_t*)malloc(token_count);
    uint32_t* level_m = token_count == 0u ? NULL :
                        (uint32_t*)malloc((size_t)token_count * sizeof(*level_m));
    uint32_t* dc_m = band == 0u ? (uint32_t*)malloc((size_t)block_count * sizeof(*dc_m)) : NULL;
    if ((token_count != 0u && (local_runs == NULL || level_m == NULL)) ||
        (band == 0u && dc_m == NULL)) {
        free(pair_count); free(local_runs); free(level_m); free(dc_m);
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "v7 size gate band tokens");
        return TC_ERR_OUT_OF_MEMORY;
    }

    size_t token_index = 0u;
    for (uint32_t block = 0u; block < block_count; ++block) {
        const int32_t* q = q_zig + (size_t)block * 64u;
        if (band == 0u) {
            uint32_t by = block / cols;
            uint32_t bx = block % cols;
            int has_left = bx > 0u ? 1 : 0;
            int has_top = by > 0u ? 1 : 0;
            int32_t pred = tc_dc_predict(has_left,
                                         has_left ? q_zig[(size_t)(block - 1u) * 64u] : 0,
                                         has_top,
                                         has_top ? q_zig[(size_t)(block - cols) * 64u] : 0);
            dc_m[block] = tc_rice_map_signed(q[0] - pred);
        }
        uint32_t next_pos = 0u;
        for (uint32_t pos = starts[band]; pos < ends[band]; ++pos) {
            if (q[pos] == 0) { continue; }
            uint32_t local_pos = pos - starts[band];
            local_runs[token_index] = (uint8_t)(local_pos - next_pos);
            level_m[token_index] = tc_rice_map_signed(q[pos]);
            next_pos = local_pos + 1u;
            ++token_index;
        }
    }

    memset(input, 0, sizeof(*input));
    input->block_count = block_count;
    input->dc_m = dc_m;
    input->pair_count = pair_count;
    input->local_runs = local_runs;
    input->level_m = level_m;
    input->token_count = token_count;
    *active_blocks_out = active_blocks;
    return TC_OK;
}

static void free_band_input(tc_v7_band_input* input)
{
    free((void*)input->dc_m);
    free((void*)input->pair_count);
    free((void*)input->local_runs);
    free((void*)input->level_m);
    memset(input, 0, sizeof(*input));
}

static uint64_t rice_hist_bits(const uint64_t* hist, uint32_t hist_count, uint32_t k)
{
    uint64_t bits = 0u;
    for (uint32_t m = 0u; m < hist_count; ++m) {
        if (hist[m] == 0u) { continue; }
        uint64_t value = 0u;
        uint32_t code_bits = tc_rice_code_word(k, m, &value);
        bits += hist[m] * (uint64_t)code_bits;
    }
    return bits;
}

static int32_t choose_v7_band(const int32_t* q_zig, uint32_t cols, uint32_t rows,
                              uint32_t band, plane_result* result)
{
    tc_v7_band_input input;
    uint32_t active_blocks = 0u;
    int32_t rc = build_band_input(q_zig, cols, rows, band, &input, &active_blocks);
    if (rc != TC_OK) { return rc; }
    uint64_t level_hist[HIST_MAX_M] = { 0u };
    uint64_t run_hist[HIST_MAX_M] = { 0u };
    uint64_t dc_hist[HIST_MAX_M] = { 0u };
    for (uint32_t i = 0u; i < input.token_count; ++i) {
        if (input.level_m[i] >= HIST_MAX_M) {
            free_band_input(&input);
            return TC_ERR_LIMIT_EXCEEDED;
        }
        ++level_hist[input.level_m[i]];
        ++run_hist[input.local_runs[i]];
    }
    if (band == 0u) {
        for (uint32_t block = 0u; block < input.block_count; ++block) {
            if (input.dc_m[block] >= HIST_MAX_M) {
                free_band_input(&input);
                return TC_ERR_LIMIT_EXCEEDED;
            }
            ++dc_hist[input.dc_m[block]];
        }
    }

    uint64_t best_dc_bits = 0u;
    uint32_t best_dc_k = 0u;
    if (band == 0u) {
        best_dc_bits = UINT64_MAX;
        for (uint32_t k = 0u; k <= TC_RICE_K_MAX; ++k) {
            uint64_t bits = rice_hist_bits(dc_hist, HIST_MAX_M, k);
            if (bits < best_dc_bits) {
                best_dc_bits = bits;
                best_dc_k = k;
            }
        }
    }
    uint64_t best_run_bits = 0u;
    uint32_t best_run_k = 0u;
    if (band != 0u) {
        best_run_bits = UINT64_MAX;
        for (uint32_t k = 0u; k <= TC_RICE_K_MAX; ++k) {
            uint64_t bits = rice_hist_bits(run_hist, HIST_MAX_M, k);
            if (bits < best_run_bits) {
                best_run_bits = bits;
                best_run_k = k;
            }
        }
    }
    uint64_t best_level_bits = UINT64_MAX;
    uint32_t best_level_k = 0u;
    for (uint32_t k = 0u; k <= TC_RICE_K_MAX; ++k) {
        uint64_t bits = rice_hist_bits(level_hist, HIST_MAX_M, k);
        if (bits < best_level_bits) {
            best_level_bits = bits;
            best_level_k = k;
        }
    }
    uint64_t active_map = band == 0u ? 0u :
        32u + (uint64_t)active_blocks *
            ((uint64_t)ceil_log2_u32(input.block_count) + 6u);
    /* B3 对计数位宽 4（b41346b2：3 位截断合法值 8 致直连图失步）；
     * B4 6 位，其余 3 位 —— 与 band_codec.c::pair_count_bits 同步 */
    uint64_t pair_map = band == 0u ? 0u :
        (uint64_t)input.block_count *
        (band == 4u ? 6u : (band == 3u ? 4u : 3u));
    uint64_t selected_map = band == 0u ? 0u :
        pair_map < active_map ? pair_map : active_map;
    uint64_t best_bits = 32u + best_level_bits +
                         (band == 0u ? best_dc_bits + (uint64_t)input.block_count * 3u +
                                           (uint64_t)input.token_count * 2u
                                     : selected_map) +
                         (band == 0u ? 0u : best_run_bits);
    uint64_t api_bits = 0u;
    rc = tc_v7_band_bits(band, band == 0u ? best_dc_k : best_run_k,
                         best_level_k, &input, &api_bits);
    if (rc == TC_OK && api_bits != best_bits) {
        tc_set_error(TC_ERR_STATE, "v7 size gate band histogram mismatch");
        rc = TC_ERR_STATE;
    }
    if (rc == TC_OK) {
        result->v7_bits += best_bits;
        result->v7_bytes += (uint32_t)((best_bits + 7u) / 8u);
        result->v7_padding_bits += (uint64_t)((best_bits + 7u) / 8u) * 8u - best_bits;
        result->tokens[band] += input.token_count;
        result->active_blocks[band] += active_blocks;
        if (band != 0u) {
            result->v7_active_map_bits += active_map;
            result->v7_pair_map_bits += pair_map;
            result->v7_selected_map_bits += selected_map;
        }
    }
    free_band_input(&input);
    return rc;
}

static int32_t run_case(const size_case* test, plane_result* result,
                        uint64_t* block_count_out)
{
    topos_frame_header fh;
    int32_t rc = make_v2_header(test->width, test->height, &fh);
    if (rc != TC_OK) { return rc; }
    memset(result, 0, sizeof(*result));
    uint64_t all_blocks = 0u;
    for (uint32_t plane = 0u; plane < V7_PLANE_COUNT; ++plane) {
        uint32_t cols = fh.plane_block_cols[plane];
        uint32_t rows = fh.plane_block_rows[plane];
        size_t block_count = (size_t)cols * (size_t)rows;
        if (block_count > SIZE_MAX / (64u * sizeof(int32_t)) ||
            block_count > UINT32_MAX) {
            return TC_ERR_LIMIT_EXCEEDED;
        }
        int32_t* q_zig = (int32_t*)malloc(block_count * 64u * sizeof(*q_zig));
        if (q_zig == NULL) {
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "v7 size gate q blocks");
            return TC_ERR_OUT_OF_MEMORY;
        }
        fill_q_zig(q_zig, block_count, test->pattern,
                   test->seed ^ ((uint64_t)plane * UINT64_C(0x9E3779B97F4A7C15)));
        plane_result one;
        memset(&one, 0, sizeof(one));
        rc = choose_v2_plane(&fh, plane, q_zig, cols, rows, &one);
        if (rc == TC_OK) {
            for (uint32_t band = 0u; band < V7_BAND_COUNT; ++band) {
                rc = choose_v7_band(q_zig, cols, rows, band, &one);
                if (rc != TC_OK) { break; }
            }
        }
        free(q_zig);
        if (rc != TC_OK) { return rc; }
        result->v2_bits += one.v2_bits;
        result->v7_bits += one.v7_bits;
        result->v2_eob_bits += one.v2_eob_bits;
        result->v7_active_map_bits += one.v7_active_map_bits;
        result->v7_pair_map_bits += one.v7_pair_map_bits;
        result->v7_selected_map_bits += one.v7_selected_map_bits;
        result->v7_padding_bits += one.v7_padding_bits;
        result->v2_bytes += one.v2_bytes;
        result->v7_bytes += one.v7_bytes;
        for (uint32_t band = 0u; band < V7_BAND_COUNT; ++band) {
            result->tokens[band] += one.tokens[band];
            result->active_blocks[band] += one.active_blocks[band];
        }
        all_blocks += block_count;
    }
    *block_count_out = all_blocks;
    return TC_OK;
}

int main(void)
{
    static const size_case corpus[] = {
        { 1920u, 1080u, TEST_FLAT,    "flat",    UINT64_C(0x10001) },
        { 1920u, 1080u, TEST_LOW,     "low",     UINT64_C(0x10002) },
        { 1920u, 1080u, TEST_TEXTURE, "texture", UINT64_C(0x10003) },
        { 1920u, 1080u, TEST_NOISE,   "noise",   UINT64_C(0x10004) },
        { 3840u, 2160u, TEST_FLAT,    "flat",    UINT64_C(0x20001) },
        { 3840u, 2160u, TEST_LOW,     "low",     UINT64_C(0x20002) },
        { 3840u, 2160u, TEST_TEXTURE, "texture", UINT64_C(0x20003) },
        { 3840u, 2160u, TEST_NOISE,   "noise",   UINT64_C(0x20004) },
        { 6144u, 3456u, TEST_FLAT,    "flat",    UINT64_C(0x60001) },
        { 6144u, 3456u, TEST_LOW,     "low",     UINT64_C(0x60002) },
        { 6144u, 3456u, TEST_TEXTURE, "texture", UINT64_C(0x60003) },
        { 6144u, 3456u, TEST_NOISE,   "noise",   UINT64_C(0x60004) },
        { 7680u, 4320u, TEST_FLAT,    "flat",    UINT64_C(0x80001) },
        { 7680u, 4320u, TEST_LOW,     "low",     UINT64_C(0x80002) },
        { 7680u, 4320u, TEST_TEXTURE, "texture", UINT64_C(0x80003) },
        { 7680u, 4320u, TEST_NOISE,   "noise",   UINT64_C(0x80004) }
    };
    puts("width,height,pattern,v2_bytes,v7_bytes,v2_payload_bytes,v7_payload_bytes,"
         "directory_bytes,blocks,tokens,eob_bits,active_map_bits,pair_map_bits,"
         "selected_map_bits,padding_bits,"
         "b0_tokens,b1_tokens,b2_tokens,b3_tokens,b4_tokens");
    for (size_t i = 0u; i < sizeof(corpus) / sizeof(corpus[0]); ++i) {
        plane_result result;
        uint64_t blocks = 0u;
        int32_t rc = run_case(&corpus[i], &result, &blocks);
        if (rc != TC_OK) {
            fprintf(stderr, "%s: size gate case failed rc=%" PRId32 "\n",
                    corpus[i].name, rc);
            return 1;
        }
        uint64_t tokens = 0u;
        for (uint32_t band = 0u; band < V7_BAND_COUNT; ++band) {
            tokens += result.tokens[band];
        }
        uint64_t v2_bytes = 53u + (uint64_t)V7_PLANE_COUNT * TC_SLICE_HEADER_SIZE + result.v2_bytes;
        uint64_t v7_bytes = 53u + V7_DIRECTORY_BYTES + result.v7_bytes;
        printf("%u,%u,%s,%" PRIu64 ",%" PRIu64 ",%u,%u,%u,%" PRIu64 ","
               "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
               "%" PRIu64 ",%u,%u,%u,%u,%u\n",
               (unsigned)corpus[i].width, (unsigned)corpus[i].height, corpus[i].name,
               v2_bytes, v7_bytes, result.v2_bytes, result.v7_bytes,
               V7_DIRECTORY_BYTES, blocks, tokens, result.v2_eob_bits,
               result.v7_active_map_bits, result.v7_pair_map_bits,
               result.v7_selected_map_bits, result.v7_padding_bits,
               result.tokens[0], result.tokens[1], result.tokens[2],
               result.tokens[3], result.tokens[4]);
    }
    return 0;
}
