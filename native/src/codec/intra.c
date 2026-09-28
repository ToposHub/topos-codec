#include "intra.h"

#include <string.h>

#include "color_store.h"
#include "../common/alloc.h"
#include "../common/checked.h"
#include "../common/error.h"
#include "../common/endian.h"
#include "../entropy/block_coding.h"
#include "../entropy/rice.h"
#include "../entropy/range.h"

#include "../entropy/scan.h"
#include "../entropy/vlc.h"
#include "../transform/transform.h"

/* Mode IDs: midpoint, boundary DC, vertical,
 * horizontal, planar, diagonal down-right, top-right, left-down.
 * Missing edges use the bit-depth midpoint. Unavailable edge extensions
 * repeat the last available sample. Every reference is reconstructed and
 * strictly inside this slice, including for non-multiple-of-eight images. */
typedef struct intra_edges {
    uint16_t* allocation;
    uint16_t* top;
    uint16_t* bottom;
    uint16_t left[8];
    uint32_t width;
    uint32_t mid;
    uint8_t bit_depth; /* 批 4：残差/系数域按位深分路 */
} intra_edges;

static int32_t intra_geometry(const topos_frame_header* fh,
                               const topos_slice_header* sh)
{
    /* plane 域 = plane_count 驱动：pf=3（TRAW）4 相位平面全部可做 intra
     * （V3/V6）；带 alpha 帧的 plane 3（alpha）不经本路径（MED 专用） */
    if (fh == NULL || sh == NULL || sh->plane >= fh->plane_count ||
        (sh->plane == 3u && fh->alpha_mode != 0u) ||
        (fh->bit_depth != 10u && fh->bit_depth != 12u && fh->bit_depth != 16u)) {
        return TC_ERR_INVALID_ARGUMENT;
    }
    uint32_t cols = fh->plane_block_cols[sh->plane];
    if (cols == 0u || cols > 1024u || sh->block_h == 0u ||
        (uint32_t)sh->block_y0 + sh->block_h > fh->plane_block_rows[sh->plane] ||
        fh->plane_coded_w[sh->plane] != cols * 8u) {
        return TC_ERR_INVALID_ARGUMENT;
    }
    return TC_OK;
}

static int32_t edges_init(intra_edges* e, uint32_t width, uint32_t mid)
{
    memset(e, 0, sizeof(*e));
    e->allocation = (uint16_t*)tc_alloc((size_t)width * 2u * sizeof(uint16_t));
    if (e->allocation == NULL) { return TC_ERR_OUT_OF_MEMORY; }
    e->top = e->allocation;
    e->bottom = e->top + width;
    e->width = width;
    e->mid = mid;
    for (uint32_t x = 0u; x < width; ++x) { e->top[x] = (uint16_t)mid; }
    return TC_OK;
}

static void predict_block(const intra_edges* e, uint32_t bx, uint32_t by,
                           uint32_t mode, uint16_t pred[64])
{
    uint32_t top[16], left[8];
    uint32_t x0 = bx * 8u;
    uint32_t sum = 0u, count = 0u;
    for (uint32_t i = 0u; i < 16u; ++i) {
        uint32_t x = x0 + i;
        if (x >= e->width) { x = e->width - 1u; }
        top[i] = by != 0u ? e->top[x] : e->mid;
    }
    for (uint32_t i = 0u; i < 8u; ++i) {
        left[i] = bx != 0u ? e->left[i] : e->mid;
        if (by != 0u) { sum += top[i]; count++; }
        if (bx != 0u) { sum += left[i]; count++; }
    }
    uint32_t dc = count != 0u ? (sum + count / 2u) / count : e->mid;
    uint32_t corner = bx != 0u && by != 0u ? e->top[x0 - 1u] : e->mid;
    for (uint32_t y = 0u; y < 8u; ++y) {
        for (uint32_t x = 0u; x < 8u; ++x) {
            uint32_t v = e->mid;
            switch (mode) {
            case 1u: v = dc; break;
            case 2u: v = top[x]; break;
            case 3u: v = left[y]; break;
            case 4u:
                v = ((7u - x) * left[y] + (x + 1u) * top[7]
                     + (7u - y) * top[x] + (y + 1u) * left[7] + 8u) / 16u;
                break;
            case 5u:
                v = x > y ? top[x - y - 1u] : y > x ? left[y - x - 1u] : corner;
                break;
            case 6u: v = top[x + y + 1u]; break;
            case 7u: v = left[x + y + 1u < 8u ? x + y + 1u : 7u]; break;
            default: break;
            }
            pred[y * 8u + x] = (uint16_t)v;
        }
    }
}

static void reconstruct_block(intra_edges* e, uint32_t bx,
                               const tc_quant_ctx* qctx,
                               tc_dequant_inverse_fn dinv, uint32_t w0,
                               const int32_t q[64],
                               const uint16_t pred[64], uint32_t max,
                               uint32_t rowmask, uint16_t pixels[64])
{
    int32_t residual[64];
    if (rowmask == 0u && w0 != 0u) {
        /* 与 color_store.h 的 DC-only 闭式相同；预测仍逐像素相加。 */
        int64_t f0 = (int64_t)q[0] * (int64_t)qctx->Q[0];
        if (f0 > (int64_t)TC_TRANSFORM_MAX_ABS_F) {
            f0 = (int64_t)TC_TRANSFORM_MAX_ABS_F;
        }
        if (f0 < -(int64_t)TC_TRANSFORM_MAX_ABS_F) {
            f0 = -(int64_t)TC_TRANSFORM_MAX_ABS_F;
        }
        int64_t acc = (int64_t)13 * (f0 * (int64_t)w0) * (int64_t)13;
        int32_t v = tc_store_round_shift32(acc);
        for (uint32_t i = 0u; i < 64u; ++i) { residual[i] = v; }
    } else if (dinv != NULL) {
        /* V2/V3 共用帧入口解析的融合内核，避免每块再次走 dispatch。 */
        dinv(qctx, q, residual, rowmask);
    } else {
        int32_t f[64];
        tc_dequant_block_ctx(qctx, q, f);
        tc_transform_inverse_8x8(f, residual);
    }
    for (uint32_t y = 0u; y < 8u; ++y) {
        for (uint32_t x = 0u; x < 8u; ++x) {
            int32_t v = residual[y * 8u + x] + (int32_t)pred[y * 8u + x];
            if (v < 0) { v = 0; }
            else if ((uint32_t)v > max) { v = (int32_t)max; }
            pixels[y * 8u + x] = (uint16_t)v;
        }
        e->left[y] = pixels[y * 8u + 7u];
    }
    memcpy(e->bottom + bx * 8u, pixels + 56u, 8u * sizeof(uint16_t));
}

static void edges_next_row(intra_edges* e)
{
    uint16_t* tmp = e->top;
    e->top = e->bottom;
    e->bottom = tmp;
}

static void forward_residual(const uint16_t* src, size_t stride,
                              const uint16_t pred[64], int32_t f[64],
                              uint8_t bit_depth)
{
    /* 批 4 双路：bd ≤ 12 残差 ∈ int16 域（现行快路 + SIMD 分发，历史
     * 位流逐位不变）；bd ≥ 13 走 int32 通用路（i32 前向分发：AVX2/NEON
     * 后端在 bd≤16 全域与 scalar bit-exact，test_transform 三路差分钉死）。
     * int32 中间容器在 bd≤12 与 int16 版算术逐位一致（同矩阵同累加序）。 */
    if (bit_depth <= 12u) {
        int16_t residual[64];
        for (uint32_t y = 0u; y < 8u; ++y) {
            for (uint32_t x = 0u; x < 8u; ++x) {
                residual[y * 8u + x] = (int16_t)((int32_t)src[(size_t)y * stride + x]
                                                 - (int32_t)pred[y * 8u + x]);
            }
        }
        tc_transform_forward_8x8(residual, f);
        return;
    }
    int32_t residual32[64];
    for (uint32_t y = 0u; y < 8u; ++y) {
        for (uint32_t x = 0u; x < 8u; ++x) {
            residual32[y * 8u + x] = (int32_t)src[(size_t)y * stride + x]
                                     - (int32_t)pred[y * 8u + x];
        }
    }
    tc_transform_forward_8x8_i32(residual32, f);
    /* 批 4 阶段 2：16-bit 无损预失真（W 表项舍入偏差补偿；见 transform.h） */
    tc_transform_forward_predistort_i32(f);
}

/* V6 syntax helpers.  The range coder carries the same prediction residuals as
 * V3, but models syntax bits instead of emitting frozen DC/level/run books.
 * Context IDs are stable and intentionally small so a slice can reset all
 * probabilities without allocating another object. */
static int32_t range_put_bits(tc_range_encoder* rc, tc_range_ctx contexts[],
                              uint32_t base, uint32_t nbits, uint32_t value)
{
    for (uint32_t i = 0u; i < nbits; ++i) {
        int32_t result = tc_range_encode_bit(rc, &contexts[base + i],
                                             (value >> (nbits - 1u - i)) & 1u);
        if (result != TC_OK) { return result; }
    }
    return TC_OK;
}

static int32_t range_get_bits(tc_range_decoder* rd, tc_range_ctx contexts[],
                              uint32_t base, uint32_t nbits, uint32_t* value)
{
    uint32_t result_value = 0u;
    for (uint32_t i = 0u; i < nbits; ++i) {
        uint32_t bit = 0u;
        int32_t result = tc_range_decode_bit_inline(rd, &contexts[base + i], &bit);
        if (result != TC_OK) { return result; }
        result_value = (result_value << 1u) | bit;
    }
    *value = result_value;
    return TC_OK;
}

static uint32_t range_map_signed(int32_t value)
{
    if (value < 0) {
        return (uint32_t)((-(int64_t)value) * 2ll - 1ll);
    }
    return (uint32_t)value * 2u;
}

static int32_t range_unmap_signed(uint32_t value, int32_t* out)
{
    uint32_t magnitude = (value >> 1u) + (value & 1u);
    if (magnitude > (uint32_t)INT32_MAX) { return TC_ERR_MALFORMED; }
    int32_t v = (int32_t)magnitude;
    *out = (value & 1u) != 0u ? -v : v;
    return TC_OK;
}

static int32_t range_encode_block(tc_range_encoder* rc, tc_range_ctx contexts[],
                                  tc_bitwriter* raw, const tc_vlc_book* run_book,
                                  const tc_vlc_book* dc_book,
                                  const tc_vlc_book* lvl_book,
                                  uint32_t mode,
                                  const int32_t q[64], int32_t dc_pred)
{
    int32_t result = tc_range_encode_bit(rc, &contexts[0], mode != 0u ? 1u : 0u);
    if (result != TC_OK) { return result; }
    if (mode != 0u) {
        result = range_put_bits(rc, contexts, 1u, 3u, mode - 1u);
        if (result != TC_OK) { return result; }
    }
    result = tc_vlc_put_cat(raw, dc_book, range_map_signed(q[0] - dc_pred));
    if (result != TC_OK) { return result; }
    /* AC uses the same run/level shape as the mature VLC path.  The selected
     * canonical books and reservoir stream keep coefficient syntax on the
     * decoder's fast path; only the spatial-mode flag remains adaptive. */
    uint32_t run = 0u;
    for (uint32_t k = 1u; k < 64u; ++k) {
        /* The encoder staging buffer is already zigzag ordered. */
        int32_t level = q[k];
        if (level == 0) { ++run; continue; }
        result = tc_vlc_put_sym(raw, run_book, run);
        if (result != TC_OK) { return result; }
        uint32_t magnitude = (uint32_t)(level < 0 ? -(int64_t)level : level);
        result = tc_bitwriter_put_bits_inline(raw, 1u, level < 0 ? 1u : 0u);
        if (result != TC_OK) { return result; }
        uint32_t cat = tc_vlc_bitlen32(magnitude);
        result = tc_vlc_put_sym(raw, lvl_book, cat);
        if (result == TC_OK && cat > 1u) {
            result = tc_bitwriter_put_bits_inline(raw, cat - 1u,
                                                  magnitude - (1u << (cat - 1u)));
        }
        if (result != TC_OK) { return result; }
        run = 0u;
    }
    return tc_vlc_put_sym(raw, run_book, 63u);
}

static int32_t range_decode_block(tc_range_decoder* rd, tc_range_ctx contexts[],
                                  tc_bitreader* raw, const tc_vlc_book* run_book,
                                  const tc_vlc_book* dc_book,
                                  const tc_vlc_book* lvl_book,
                                  uint32_t* mode,
                                  int32_t q[64], int32_t dc_pred)
{
    uint32_t flag = 0u;
    int32_t result = tc_range_decode_bit_inline(rd, &contexts[0], &flag);
    if (result != TC_OK) { return result; }
    if (flag == 0u) {
        *mode = 0u;
    } else {
        uint32_t mode_minus_one = 0u;
        result = range_get_bits(rd, contexts, 1u, 3u, &mode_minus_one);
        if (result != TC_OK) { return result; }
        if (mode_minus_one >= 7u) { return TC_ERR_MALFORMED; }
        *mode = mode_minus_one + 1u;
    }
    uint32_t mapped = 0u;
    result = tc_vlc_decode_cat(raw, dc_book, TC_RICE_M_MAX_DC, &mapped);
    if (result != TC_OK) { return result; }
    int32_t delta = 0;
    result = range_unmap_signed(mapped, &delta);
    if (result != TC_OK) { return result; }
    q[0] = dc_pred + delta;
    memset(q + 1, 0, 63u * sizeof(q[0]));
    uint32_t pos = 1u;
    for (;;) {
        uint32_t run = 0u;
        result = tc_vlc_decode_sym(raw, run_book, &run);
        if (result != TC_OK) { return result; }
        if (run == 63u) { return TC_OK; }
        if (run > 63u || pos + run > 63u) { return TC_ERR_MALFORMED; }
        pos += run;
        result = tc_bitreader_read_bits_inline(raw, 1u, &flag);
        if (result != TC_OK) { return result; }
        uint32_t cat = 0u;
        result = tc_vlc_decode_sym(raw, lvl_book, &cat);
        if (result != TC_OK) { return result; }
        if (cat == 0u || cat > 31u) { return TC_ERR_MALFORMED; }
        uint32_t suffix = 0u;
        if (cat > 1u) {
            result = tc_bitreader_read_bits_inline(raw, cat - 1u, &suffix);
            if (result != TC_OK) { return result; }
        }
        uint32_t magnitude = (1u << (cat - 1u)) | suffix;
        if (magnitude > (uint32_t)INT32_MAX) { return TC_ERR_MALFORMED; }
        q[kTcZigzag[pos]] = flag != 0u ? -(int32_t)magnitude : (int32_t)magnitude;
        ++pos;
    }
    return TC_OK;
}

/* Fixed frozen-book rate estimate, including category suffixes and EOB.
 * Slice-wide book selection still runs on the final selected coefficients. */
static uint32_t block_bits(const int32_t q[64], int32_t dc_pred)
{
    uint32_t cat = tc_vlc_bitlen32(tc_rice_map_signed(q[0] - dc_pred));
    uint32_t bits = tc_vlc_dc_len[3][cat] + (cat > 1u ? cat - 1u : 0u);
    uint32_t run = 0u;
    for (uint32_t k = 1u; k < 64u; ++k) {
        int32_t level = q[kTcZigzag[k]];
        if (level == 0) { run++; continue; }
        cat = tc_vlc_bitlen32(tc_rice_map_signed(level));
        bits += tc_vlc_run_len[3][run] + tc_vlc_lvl_len[3][cat]
                + (cat > 1u ? cat - 1u : 0u);
        run = 0u;
    }
    return bits + tc_vlc_run_len[3][63];
}

static uint64_t reconstruction_error(const uint16_t* src, size_t stride,
                                     const tc_quant_ctx* qctx, const int32_t q[64],
                                     const uint16_t pred[64], uint32_t max,
                                     uint32_t cw, uint32_t ch)
{
    int32_t f[64], residual[64];
    tc_dequant_block_ctx(qctx, q, f);
    tc_transform_inverse_8x8(f, residual);
    uint64_t error = 0u;
    for (uint32_t y = 0u; y < ch; ++y) {
        for (uint32_t x = 0u; x < cw; ++x) {
            int32_t v = residual[y * 8u + x] + pred[y * 8u + x];
            if (v < 0) { v = 0; }
            else if ((uint32_t)v > max) { v = (int32_t)max; }
            int32_t d = v - src[(size_t)y * stride + x];
            error += (uint64_t)((int64_t)d * d);
        }
    }
    return error;
}

/* Quantized residual rate screens the small fixed mode set. Only the two
 * cheapest spatial candidates receive a pixel-domain distortion check.
 * Midpoint is always a candidate; spatial prediction must not increase its
 * reconstructed SSE. No coefficient refinement, partitions or transform RDO. */
static uint32_t select_mode(const intra_edges* e, uint32_t bx, uint32_t by,
                             const uint16_t* src, size_t stride,
                             const tc_quant_ctx* qctx, int32_t dc_pred,
                             uint16_t best_pred[64], int32_t best_q[64],
                             uint32_t cw, uint32_t ch)
{
    int32_t candidates[TC_INTRA_MODE_COUNT][64];
    uint32_t bits[TC_INTRA_MODE_COUNT];
    for (uint32_t mode = 0u; mode < TC_INTRA_MODE_COUNT; ++mode) {
        uint16_t pred[64];
        int32_t f[64];
        predict_block(e, bx, by, mode, pred);
        forward_residual(src, stride, pred, f, e->bit_depth);
        /* 批 4：系数域钳位按位深外推（|F|max ∝ max|x'|；12-bit 冻结值不变） */
        const int64_t fclamp = (int64_t)TC_TRANSFORM_MAX_ABS_F
            * ((int64_t)((1u << (e->bit_depth - 1u)) - 1u)) / 2047;
        int valid = 1;
        for (uint32_t k = 0u; k < 64u; ++k) {
            if (f[k] > fclamp || f[k] < -fclamp) {
                valid = 0; break;
            }
        }
        bits[mode] = UINT32_MAX;
        if (!valid) { continue; } /* 12-bit residual outside frozen domain */
        tc_quant_block_ctx(qctx, f, candidates[mode]);
        bits[mode] = block_bits(candidates[mode], dc_pred) + (mode == 0u ? 1u : 4u);
    }
    uint32_t best_mode = 0u, best_bits = bits[0];
    predict_block(e, bx, by, 0u, best_pred);
    memcpy(best_q, candidates[0], sizeof(candidates[0]));
    uint64_t best_error = reconstruction_error(src, stride, qctx, best_q,
                                               best_pred, e->mid * 2u - 1u, cw, ch);
    for (uint32_t trial = 0u; trial < 2u; ++trial) {
        uint32_t mode = 1u;
        for (uint32_t m = 2u; m < TC_INTRA_MODE_COUNT; ++m) {
            if (bits[m] < bits[mode]) { mode = m; }
        }
        uint32_t rate = bits[mode];
        bits[mode] = UINT32_MAX;
        if (rate >= best_bits) { break; }
        uint16_t pred[64];
        predict_block(e, bx, by, mode, pred);
        uint64_t error = reconstruction_error(src, stride, qctx, candidates[mode],
                                               pred, e->mid * 2u - 1u, cw, ch);
        if (error > best_error) { continue; }
        best_mode = mode; best_bits = rate; best_error = error;
        memcpy(best_pred, pred, sizeof(pred));
        memcpy(best_q, candidates[mode], sizeof(candidates[mode]));
    }
    return best_mode;
}

static uint8_t best_book(const uint8_t* lengths, uint32_t symbols,
                         const uint32_t* hist)
{
    uint64_t best = UINT64_MAX;
    uint8_t result = 0u;
    for (uint32_t book = 0u; book < TC_VLC_BOOKS; ++book) {
        uint64_t bits = 0u;
        for (uint32_t sym = 0u; sym < symbols; ++sym) {
            bits += (uint64_t)hist[sym] * lengths[(size_t)book * symbols + sym];
        }
        /* Category suffix lengths do not depend on the book. */
        if (bits < best) { best = bits; result = (uint8_t)book; }
    }
    return result;
}

static int32_t get_books(const topos_slice_header* sh,
                          const tc_vlc_book** dc, const tc_vlc_book** lvl,
                          const tc_vlc_book** run)
{
    if (sh->k1 >= TC_VLC_BOOKS || sh->k2 >= TC_VLC_BOOKS || sh->k3 >= TC_VLC_BOOKS) {
        return TC_ERR_MALFORMED;
    }
    int32_t rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_DC, sh->k1);
    if (rc != TC_OK) { return rc; }
    rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_LVL, sh->k2);
    if (rc != TC_OK) { return rc; }
    rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_RUN, sh->k3);
    if (rc != TC_OK) { return rc; }
    *dc = tc_vlc_book_get(TC_VLC_FAMILY_DC, sh->k1);
    *lvl = tc_vlc_book_get(TC_VLC_FAMILY_LVL, sh->k2);
    *run = tc_vlc_book_get(TC_VLC_FAMILY_RUN, sh->k3);
    return *dc != NULL && *lvl != NULL && *run != NULL ? TC_OK : TC_ERR_STATE;
}

int32_t tc_intra_slice_encode(const topos_frame_header* fh,
                              topos_slice_header* sh, const uint16_t* coded,
                              const tc_quant_ctx* qctx, tc_bitwriter* bw)
{
    int32_t rc = intra_geometry(fh, sh);
    if (rc != TC_OK) { return rc; }
    if (coded == NULL || qctx == NULL || bw == NULL) { return TC_ERR_INVALID_ARGUMENT; }
    uint32_t cols = fh->plane_block_cols[sh->plane];
    uint32_t max = (1u << fh->bit_depth) - 1u;
    const uint16_t* band = coded + (size_t)sh->block_y0 * 8u * cols * 8u;
    size_t samples = (size_t)cols * sh->block_h * 64u;
    for (size_t i = 0u; i < samples; ++i) {
        if (band[i] > max) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "V3 input sample exceeds bit depth");
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    size_t blocks = (size_t)cols * sh->block_h;
    size_t bytes = 0u;
    if (!tc_umul_size(blocks, 64u * sizeof(int32_t), &bytes)) {
        return TC_ERR_LIMIT_EXCEEDED;
    }
    int32_t* coeffs = (int32_t*)tc_alloc(bytes);
    uint8_t* modes = (uint8_t*)tc_alloc(blocks);
    if (coeffs == NULL || modes == NULL) {
        tc_free(coeffs); tc_free(modes);
        return TC_ERR_OUT_OF_MEMORY;
    }
    intra_edges edges;
    rc = edges_init(&edges, cols * 8u, 1u << (fh->bit_depth - 1u));
    edges.bit_depth = fh->bit_depth;
    if (rc != TC_OK) { tc_free(coeffs); tc_free(modes); return rc; }
    uint32_t dc_hist[TC_VLC_DC_SYMS] = {0};
    uint32_t lvl_hist[TC_VLC_LVL_SYMS] = {0};
    uint32_t run_hist[TC_VLC_RUN_SYMS] = {0};
    uint32_t v6_lvl_hist[TC_VLC_LVL_SYMS] = {0};
    for (uint32_t by = 0u; by < sh->block_h; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            size_t index = (size_t)by * cols + bx;
            const uint16_t* src = coded + ((size_t)sh->block_y0 + by) * 8u * edges.width + bx * 8u;
            uint16_t pred[64], pixels[64];
            int32_t q[64];
            int32_t* zig = coeffs + index * 64u;
            int32_t left_dc = bx != 0u ? zig[-64] : 0;
            int32_t top_dc = by != 0u ? coeffs[(index - cols) * 64u] : 0;
            int32_t dc_pred = tc_dc_predict(bx != 0u, left_dc, by != 0u, top_dc);
            uint32_t cw = fh->plane_visible_w[sh->plane] - bx * 8u;
            uint32_t ch = fh->plane_visible_h[sh->plane]
                          - ((uint32_t)sh->block_y0 + by) * 8u;
            if (cw > 8u) { cw = 8u; }
            if (ch > 8u) { ch = 8u; }
            modes[index] = (uint8_t)select_mode(&edges, bx, by, src, edges.width,
                                               qctx, dc_pred, pred, q, cw, ch);
            reconstruct_block(&edges, bx, qctx, NULL, tc_transform_weights()[0],
                              q, pred, max, UINT32_MAX, pixels);
            for (uint32_t k = 0u; k < 64u; ++k) { zig[k] = q[kTcZigzag[k]]; }
            dc_hist[tc_vlc_bitlen32(tc_rice_map_signed(q[0] - dc_pred))]++;
            uint32_t zeros = 0u;
            for (uint32_t k = 1u; k < 64u; ++k) {
                if (zig[k] == 0) { zeros++; continue; }
                run_hist[zeros]++;
                lvl_hist[tc_vlc_bitlen32(tc_rice_map_signed(zig[k]))]++;
                v6_lvl_hist[tc_vlc_bitlen32((uint32_t)(zig[k] < 0
                    ? -(int64_t)zig[k] : zig[k]))]++;
                zeros = 0u;
            }
            run_hist[63u]++;
        }
        edges_next_row(&edges);
    }
    if (fh->version_major == 6u) {
        /* V6 keeps the proven V3 predictor and changes only the residual
         * representation.  A private byte buffer keeps range-code emission
         * independent from the enclosing bit writer and makes the slice CRC
         * boundary explicit. */
        size_t temp_cap = 0u;
        /* A range-coded slice has no fixed bits-per-block ceiling: large
         * 12-bit DC residuals can spend more than the legacy VLC bound even
         * after adaptation.  Keep a generous per-block staging budget so the
         * arithmetic stream remains contiguous without falling back to a
         * second syntax path. */
        if (!tc_umul_size(blocks, 2048u, &temp_cap) ||
            !tc_uadd_size(temp_cap, 16u, &temp_cap) ||
            temp_cap > (size_t)TC_BITWRITER_MAX_BYTES) {
            tc_free(edges.allocation); tc_free(coeffs); tc_free(modes);
            return TC_ERR_LIMIT_EXCEEDED;
        }
        uint8_t* temp = (uint8_t*)tc_alloc(temp_cap);
        if (temp == NULL) {
            tc_free(edges.allocation); tc_free(coeffs); tc_free(modes);
            return TC_ERR_OUT_OF_MEMORY;
        }
        tc_range_encoder range;
        tc_range_ctx contexts[TC_RANGE_CONTEXTS];
        tc_bitwriter raw;
        uint8_t dc_book_id = best_book(&tc_vlc_dc_len[0][0],
                                       TC_VLC_DC_SYMS, dc_hist);
        uint8_t run_book_id = best_book(&tc_vlc_run_len[0][0],
                                        TC_VLC_RUN_SYMS, run_hist);
        uint8_t lvl_book_id = best_book(&tc_vlc_lvl_len[0][0],
                                        TC_VLC_LVL_SYMS, v6_lvl_hist);
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_DC, dc_book_id);
        if (rc == TC_OK) {
            rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_RUN, run_book_id);
        }
        if (rc == TC_OK) {
            rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_LVL, lvl_book_id);
        }
        if (rc != TC_OK) {
            tc_free(temp);
            tc_free(edges.allocation); tc_free(coeffs); tc_free(modes);
            return rc;
        }
        const tc_vlc_book* run_book = tc_vlc_book_get(TC_VLC_FAMILY_RUN,
                                                       run_book_id);
        const tc_vlc_book* dc_book = tc_vlc_book_get(TC_VLC_FAMILY_DC,
                                                      dc_book_id);
        const tc_vlc_book* lvl_book = tc_vlc_book_get(TC_VLC_FAMILY_LVL,
                                                       lvl_book_id);
        if (run_book == NULL || dc_book == NULL || lvl_book == NULL) {
            tc_free(temp);
            tc_free(edges.allocation); tc_free(coeffs); tc_free(modes);
            return TC_ERR_STATE;
        }
        sh->k1 = dc_book_id;
        sh->k2 = lvl_book_id;
        sh->k3 = run_book_id;
        tc_range_encoder_init(&range, temp, temp_cap);
        tc_range_contexts_init(contexts);
        rc = tc_bitwriter_init(&raw);
        if (rc != TC_OK) {
            tc_free(temp);
            tc_free(edges.allocation); tc_free(coeffs); tc_free(modes);
            return rc;
        }
        rc = TC_OK;
        for (uint32_t by = 0u; by < sh->block_h && rc == TC_OK; ++by) {
            for (uint32_t bx = 0u; bx < cols && rc == TC_OK; ++bx) {
                size_t index = (size_t)by * cols + bx;
                const int32_t* zig = coeffs + index * 64u;
                int32_t left_dc = bx != 0u ? zig[-64] : 0;
                int32_t top_dc = by != 0u ? coeffs[(index - cols) * 64u] : 0;
                int32_t dc_pred = tc_dc_predict(bx != 0u, left_dc, by != 0u, top_dc);
                rc = range_encode_block(&range, contexts, &raw, run_book,
                                        dc_book, lvl_book,
                                        modes[index], zig, dc_pred);
            }
        }
        if (rc == TC_OK) { rc = tc_range_encoder_finish(&range); }
        uint64_t raw_bits64 = tc_bitwriter_bits_written(&raw);
        if (rc == TC_OK) { rc = tc_bitwriter_flush_zero_pad(&raw); }
        if (rc == TC_OK) {
            size_t encoded = tc_range_encoder_size(&range);
            size_t raw_bytes = tc_bitwriter_byte_size(&raw);
            if (encoded > (size_t)UINT32_MAX || raw_bits64 > UINT32_MAX ||
                raw_bytes > (size_t)UINT32_MAX) {
                rc = TC_ERR_LIMIT_EXCEEDED;
            } else {
                uint8_t header[8];
                tc_store_be32(header, (uint32_t)encoded);
                tc_store_be32(header + 4u, (uint32_t)raw_bits64);
                for (size_t i = 0u; i < sizeof(header) && rc == TC_OK; ++i) {
                    rc = tc_bitwriter_put_bits_inline(bw, 8u, header[i]);
                }
                for (size_t i = 0u; i < encoded && rc == TC_OK; ++i) {
                    rc = tc_bitwriter_put_bits_inline(bw, 8u, temp[i]);
                }
                const uint8_t* raw_data = tc_bitwriter_data(&raw);
                for (size_t i = 0u; i < raw_bytes && rc == TC_OK; ++i) {
                    rc = tc_bitwriter_put_bits_inline(bw, 8u, raw_data[i]);
                }
            }
        }
        tc_bitwriter_free(&raw);
        tc_free(temp);
        tc_free(edges.allocation); tc_free(coeffs); tc_free(modes);
        return rc;
    }
    sh->k1 = best_book(&tc_vlc_dc_len[0][0], TC_VLC_DC_SYMS, dc_hist);
    sh->k2 = best_book(&tc_vlc_lvl_len[0][0], TC_VLC_LVL_SYMS, lvl_hist);
    sh->k3 = best_book(&tc_vlc_run_len[0][0], TC_VLC_RUN_SYMS, run_hist);
    const tc_vlc_book *dc, *lvl, *run;
    rc = get_books(sh, &dc, &lvl, &run);
    for (uint32_t by = 0u; by < sh->block_h && rc == TC_OK; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            size_t index = (size_t)by * cols + bx;
            const int32_t* zig = coeffs + index * 64u;
            rc = tc_bitwriter_put_bits(bw, 1u, modes[index] != 0u ? 1u : 0u);
            if (rc == TC_OK && modes[index] != 0u) {
                rc = tc_bitwriter_put_bits(bw, 3u, (uint32_t)modes[index] - 1u);
            }
            if (rc != TC_OK) { break; }
            rc = tc_block_encode_zigzag_vlc(bw, dc, lvl, run, zig,
                    bx != 0u, bx != 0u ? zig[-64] : 0,
                    by != 0u, by != 0u ? coeffs[(index - cols) * 64u] : 0, NULL);
            if (rc != TC_OK) { break; }
        }
    }
    tc_free(edges.allocation); tc_free(coeffs); tc_free(modes);
    return rc;
}

int32_t tc_intra_slice_decode(const topos_frame_header* fh,
                              const topos_slice_header* sh,
                              const uint8_t* payload, size_t payload_size,
                              int32_t* q_out, uint64_t* symbol_hash,
                              tc_color_block_sink_fn sink, void* ctx,
                              struct tc_color_store_ctx* store,
                              tc_scan_dc_ctx* dc_scratch,
                              uint16_t* edge_scratch, size_t edge_elems)
{
    int32_t rc = intra_geometry(fh, sh);
    if (rc != TC_OK) { return rc; }
    if (payload == NULL && payload_size != 0u) { return TC_ERR_INVALID_ARGUMENT; }
    if (store != NULL && (store->dst == NULL || store->qctx == NULL ||
        store->vis_w == 0u || store->vis_h == 0u || store->dst_w == 0u ||
        store->dst_h == 0u ||
        store->stride < (store->scaled != 0u ? store->dst_w : store->vis_w))) {
        return TC_ERR_INVALID_ARGUMENT;
    }
    const int use_range = fh->version_major == 6u;
    const tc_vlc_book *dc = NULL, *lvl = NULL, *run = NULL;
    if (use_range == 0) {
        rc = get_books(sh, &dc, &lvl, &run);
        if (rc != TC_OK) { return rc; }
    } else {
        if (sh->k1 >= TC_VLC_BOOKS || sh->k2 >= TC_VLC_BOOKS ||
            sh->k3 >= TC_VLC_BOOKS) {
            return TC_ERR_MALFORMED;
        }
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_DC, sh->k1);
        if (rc != TC_OK) { return rc; }
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_RUN, sh->k3);
        if (rc != TC_OK) { return rc; }
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_LVL, sh->k2);
        if (rc != TC_OK) { return rc; }
        dc = tc_vlc_book_get(TC_VLC_FAMILY_DC, sh->k1);
        run = tc_vlc_book_get(TC_VLC_FAMILY_RUN, sh->k3);
        lvl = tc_vlc_book_get(TC_VLC_FAMILY_LVL, sh->k2);
        if (dc == NULL || run == NULL || lvl == NULL) { return TC_ERR_STATE; }
    }
    uint32_t cols = fh->plane_block_cols[sh->plane];
    int32_t* dc_rows = NULL;
    int32_t* top_dc = NULL;
    int32_t* row_dc = NULL;
    int own_dc = 0;
    if (dc_scratch != NULL && dc_scratch->prev_row != NULL
            && dc_scratch->row != NULL && dc_scratch->elems >= (size_t)cols) {
        top_dc = dc_scratch->prev_row;
        row_dc = dc_scratch->row;
        memset(top_dc, 0, (size_t)cols * sizeof(*top_dc));
        memset(row_dc, 0, (size_t)cols * sizeof(*row_dc));
    } else {
        dc_rows = (int32_t*)tc_calloc((size_t)cols * 2u, sizeof(int32_t));
        if (dc_rows == NULL) { return TC_ERR_OUT_OF_MEMORY; }
        top_dc = dc_rows;
        row_dc = dc_rows + cols;
        own_dc = 1;
    }
    intra_edges edges;
    memset(&edges, 0, sizeof(edges));
    int own_edges = 0;
    if (store != NULL) {
        uint32_t edge_width = cols * 8u;
        size_t edge_need = (size_t)edge_width * 2u;
        if (edge_scratch != NULL && edge_elems >= edge_need) {
            edges.allocation = edge_scratch;
            edges.top = edge_scratch;
            edges.bottom = edge_scratch + edge_width;
            edges.width = edge_width;
            edges.mid = 1u << (fh->bit_depth - 1u);
            for (uint32_t x = 0u; x < edge_width; ++x) {
                edges.top[x] = (uint16_t)edges.mid;
            }
        } else {
            rc = edges_init(&edges, edge_width, 1u << (fh->bit_depth - 1u));
            if (rc != TC_OK) {
                if (own_dc != 0) { tc_free(dc_rows); }
                return rc;
            }
            own_edges = 1;
        }
    }
    tc_bitreader br;
    tc_range_decoder range;
    tc_range_ctx range_contexts[TC_RANGE_CONTEXTS];
    tc_bitreader raw;
    uint32_t raw_bits_expected = 0u;
    size_t range_size = 0u;
    size_t raw_size = 0u;
    if (use_range != 0) {
        if (payload_size < 8u) {
            if (own_edges != 0) { tc_free(edges.allocation); }
            if (own_dc != 0) { tc_free(dc_rows); }
            return TC_ERR_MALFORMED;
        }
        range_size = (size_t)tc_load_be32(payload);
        uint64_t raw_bits64 = (uint64_t)tc_load_be32(payload + 4u);
        if (raw_bits64 > (uint64_t)SIZE_MAX ||
            range_size > payload_size - 8u) {
            if (own_edges != 0) { tc_free(edges.allocation); }
            if (own_dc != 0) { tc_free(dc_rows); }
            return TC_ERR_MALFORMED;
        }
        raw_size = payload_size - 8u - range_size;
        if (raw_bits64 > (uint64_t)raw_size * 8u) {
            if (own_edges != 0) { tc_free(edges.allocation); }
            if (own_dc != 0) { tc_free(dc_rows); }
            return TC_ERR_MALFORMED;
        }
        raw_bits_expected = (uint32_t)raw_bits64;
        const uint8_t* range_payload = payload + 8u;
        const uint8_t* raw_payload = range_payload + range_size;
        tc_bitreader_init(&raw, raw_payload, raw_size);
        rc = tc_range_decoder_init(&range, range_payload, range_size);
        if (rc != TC_OK) {
            if (own_edges != 0) { tc_free(edges.allocation); }
            if (own_dc != 0) { tc_free(dc_rows); }
            return rc;
        }
        tc_range_contexts_init(range_contexts);
    } else {
        tc_bitreader_init(&br, payload, payload_size);
    }
    uint64_t hash = 0u;
    uint32_t max = (1u << fh->bit_depth) - 1u;
    for (uint32_t by = 0u; by < sh->block_h && rc == TC_OK; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            uint32_t mode = 0u;
            int32_t q[64];
            uint32_t index = by * cols + bx;
            const int sample = store != NULL && store->stats != NULL &&
                               (index & 63u) == 0u;
            uint64_t t_entropy = sample ? tc_profile_now_ns() : 0u;
            if (use_range != 0) {
                int32_t dc_pred = tc_dc_predict(bx != 0u,
                    bx != 0u ? row_dc[bx - 1u] : 0,
                    by != 0u, top_dc[bx]);
                rc = range_decode_block(&range, range_contexts, &raw, run, dc, lvl,
                                        &mode, q, dc_pred);
            } else {
                rc = tc_bitreader_read_bits(&br, 1u, &mode);
                if (rc != TC_OK) { break; }
                if (mode != 0u) {
                    rc = tc_bitreader_read_bits(&br, 3u, &mode);
                    if (rc != TC_OK) { break; }
                    if (mode == 7u) {
                        tc_set_error(TC_ERR_MALFORMED, "reserved intra mode");
                        rc = TC_ERR_MALFORMED; break;
                    }
                    mode++;
                }
                rc = tc_block_decode_vlc(&br, dc, lvl, run,
                        bx != 0u, bx != 0u ? row_dc[bx - 1u] : 0,
                        by != 0u, top_dc[bx], q);
            }
            if (rc != TC_OK) { break; }
            if (sample) {
                store->sampled_entropy_ns += tc_profile_now_ns() - t_entropy;
                store->sampled_entropy_blocks++;
            }
            row_dc[bx] = q[0];
            uint32_t rowmask = 0u, nonzero = 0u;
            for (uint32_t k = 1u; k < 64u; ++k) {
                if (q[k] != 0) { rowmask |= 1u << (k / 8u); nonzero++; }
            }
            if (rowmask != 0u) { rowmask |= 1u; }
            if (symbol_hash != NULL) {
                hash = tc_symbol_hash_mix(hash, mode);
                for (uint32_t k = 0u; k < 64u; ++k) {
                    hash = tc_symbol_hash_mix(hash, (uint64_t)(int64_t)q[k]);
                }
            }
            if (q_out != NULL) { memcpy(q_out + (size_t)index * 64u, q, sizeof(q)); }
            if (sink != NULL) {
                rc = sink(ctx, index, q, rowmask);
                if (rc != TC_OK) { break; }
            }
            if (store != NULL) {
                uint64_t t_recon = sample ? tc_profile_now_ns() : 0u;
                uint16_t pred[64], pixels[64];
                predict_block(&edges, bx, by, mode, pred);
                reconstruct_block(&edges, bx, store->qctx, store->dinv, store->w0,
                                  q, pred, max, rowmask, pixels);
                const uint32_t x0 = bx * 8u;
                const uint32_t y0 = ((uint32_t)sh->block_y0 + by) * 8u;
                if (store->scaled != 0u) {
                    if (tc_color_store_exact_third(store)) {
                        tc_color_store_third_from_u16(
                            store, bx, (uint32_t)sh->block_y0 + by, pixels);
                    } else {
                        uint8_t xs[64], ys[64];
                        uint32_t txs[64], tys[64];
                        const uint32_t n = tc_color_store_scaled_samples(
                            store, bx, (uint32_t)sh->block_y0 + by,
                            xs, ys, txs, tys);
                        uint16_t values[64];
                        for (uint32_t i = 0u; i < n; ++i) {
                            values[i] = pixels[(uint32_t)ys[i] * 8u + xs[i]];
                        }
                        tc_color_store_scaled_u16(store, xs, ys, txs, tys, values, n);
                    }
                } else {
                    for (uint32_t y = 0u; y < 8u && y0 + y < store->vis_h; ++y) {
                        for (uint32_t x = 0u; x < 8u && x0 + x < store->vis_w; ++x) {
                            store->dst[(size_t)(y0 + y) * store->stride + x0 + x] = pixels[y * 8u + x];
                        }
                    }
                }
                if (store->stats != NULL) {
                    store->stats->blocks++;
                    store->stats->nonzero_ac += nonzero;
                    if (nonzero == 0u) { store->stats->dc_only_blocks++; }
                }
                if (sample) {
                    store->sampled_recon_ns += tc_profile_now_ns() - t_recon;
                    store->sampled_recon_blocks++;
                }
            }
        }
        int32_t* tmp = top_dc; top_dc = row_dc; row_dc = tmp;
        if (store != NULL) { edges_next_row(&edges); }
    }
    if (own_edges != 0) { tc_free(edges.allocation); }
    if (own_dc != 0) { tc_free(dc_rows); }
    if (rc != TC_OK) { return rc; }
    if (use_range != 0) {
        size_t used = tc_range_decoder_bytes_used(&range);
        uint32_t raw_padding = (uint32_t)((raw_size * 8u) - raw_bits_expected);
        uint32_t raw_pad_value = 0u;
        if (raw_padding != 0u) {
            rc = tc_bitreader_read_bits_inline(&raw, raw_padding, &raw_pad_value);
        }
        /* The arithmetic decoder may leave the final partial byte (at most
         * four flush bytes) unread after the symbol count is known.  The raw
         * suffix stream may only leave its explicit zero byte padding. */
        if (used > range_size || range_size - used > 4u ||
            rc != TC_OK || raw_pad_value != 0u ||
            tc_bitreader_bits_consumed(&raw) != (uint64_t)raw_size * 8u) {
            tc_set_error(TC_ERR_MALFORMED, "range intra slice trailing bytes");
            return TC_ERR_MALFORMED;
        }
    } else {
        /* V3 defines zero alignment bits, unlike permissive historical readers. */
        uint32_t padding = (uint32_t)((8u - (tc_bitreader_bits_consumed(&br) & 7u)) & 7u);
        uint32_t pad_value = 0u;
        rc = tc_bitreader_read_bits(&br, padding, &pad_value);
        if (rc != TC_OK) { return rc; }
        if (pad_value != 0u || tc_bitreader_bits_consumed(&br) != (uint64_t)payload_size * 8u) {
            tc_set_error(TC_ERR_MALFORMED, "intra slice has nonzero padding or trailing data");
            return TC_ERR_MALFORMED;
        }
    }
    if (symbol_hash != NULL) { *symbol_hash = hash; }
    return TC_OK;
}
