/* slice 符号层：颜色/Alpha 带编解码往返、O(1) 指纹路径、尾随字节与截断 */
#include "bitstream/slice_codec.h"

#include <stdlib.h>
#include <string.h>

#include "codec/codec.h"
#include "codec/color_store.h"
#include "entropy/block_coding.h"
#include "entropy/scan.h"
#include "entropy/vlc.h"
#include "mini_test.h"
#include "simd/dispatch.h"
#include "transform/quant.h"
#include "transform/transform.h"

static void make_fh(topos_frame_header* fh, int alpha)
{
    memset(fh, 0, sizeof(*fh));
    fh->version_major = 1u;
    fh->profile = 3u;
    fh->pixel_format = 0u;
    fh->bit_depth = 10u;
    fh->alpha_mode = alpha ? 1u : 0u;
    fh->alpha_bit_depth = alpha ? 16u : 0u;
    fh->coded_width = 64u;
    fh->coded_height = 48u;
    fh->visible_width = 63u;
    fh->visible_height = 45u;
    fh->plane_count = alpha ? 4u : 3u;
    fh->qmatrix_id = 0u;
    fh->qp_base = 10u;
    fh->slice_count = fh->plane_count;
    fh->color_primaries = 1u;
    fh->color_transfer = 1u;
    fh->color_matrix = 1u;
    fh->color_range = 1u;
    MT_CHECK_EQ_I64(tc_frame_derive_geometry(fh), TC_OK);
    MT_CHECK_EQ_U64(fh->plane_block_cols[0], 8ull);
    MT_CHECK_EQ_U64(fh->plane_block_rows[0], 6ull);
}

/* ---- 解码深化批次差分：融合流式 vs 离散逐块（权威参考实现） ----
 * 参考侧复刻 M9 及之前的离散循环（tc_block_decode / tc_block_decode_vlc 每
 * 块一次调用 + sink 前逐系数指纹），与产品路径 tc_color_slice_decode_stream
 * （color_scan.h 融合版）在全 payload 与每一字节截断位对拍：
 *   返回码、逐块 natural 序系数、has_ac 标志、symbol_hash 全部一致。 */

typedef struct rec_sink_ctx {
    int32_t* q;        /* [blocks][64] */
    uint8_t* has_ac;   /* [blocks] */
    uint32_t count;
    uint32_t limit;    /* 注入失败：第 limit 块起返回错误码 */
    int32_t fail_rc;
} rec_sink_ctx;

static int32_t rec_sink(void* vctx, uint32_t idx, const int32_t q_natural[64],
                        uint32_t ac_rowmask)
{
    rec_sink_ctx* c = (rec_sink_ctx*)vctx;
    if (c->limit != 0u && idx >= c->limit) { return c->fail_rc; }
    int32_t* dst = c->q + (size_t)idx * 64u;
    if (ac_rowmask != 0u) {
        memcpy(dst, q_natural, 64u * sizeof(int32_t));
    } else {
        /* 契约：ac_rowmask=0 仅 q[0] 有意义——归一后拷贝，差分只比有意义位 */
        dst[0] = q_natural[0];
        for (uint32_t i = 1u; i < 64u; ++i) { dst[i] = 0; }
    }
    c->has_ac[idx] = (uint8_t)(ac_rowmask != 0u ? 1u : 0u);
    c->count = idx + 1u;
    return TC_OK;
}

static int32_t ref_decode(const topos_frame_header* fh, const topos_slice_header* sh,
                          const uint8_t* payload, size_t payload_size,
                          int32_t* q_out, uint8_t* has_ac_out, uint64_t* hash_out)
{
    tc_bitreader br;
    tc_bitreader_init(&br, payload, payload_size);
    uint32_t cols = fh->plane_block_cols[sh->plane];
    uint32_t band = sh->block_h;
    const int use_vlc = fh->version_major == 2u && fh->entropy_mode == 1u;
    const tc_vlc_book* dc_b = NULL;
    const tc_vlc_book* lvl_b = NULL;
    const tc_vlc_book* run_b = NULL;
    if (use_vlc != 0) {
        int32_t rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_DC, sh->k1);
        if (rc != TC_OK) { return rc; }
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_LVL, sh->k2);
        if (rc != TC_OK) { return rc; }
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_RUN, sh->k3);
        if (rc != TC_OK) { return rc; }
        dc_b = tc_vlc_book_get(TC_VLC_FAMILY_DC, sh->k1);
        lvl_b = tc_vlc_book_get(TC_VLC_FAMILY_LVL, sh->k2);
        run_b = tc_vlc_book_get(TC_VLC_FAMILY_RUN, sh->k3);
        if (dc_b == NULL || lvl_b == NULL || run_b == NULL) { return TC_ERR_STATE; }
    }
    size_t elems = cols > 0u ? (size_t)cols : 1u;
    int32_t* prev = (int32_t*)calloc(elems, sizeof(int32_t));
    int32_t* row = (int32_t*)calloc(elems, sizeof(int32_t));
    if (prev == NULL || row == NULL) { free(prev); free(row); return TC_ERR_OUT_OF_MEMORY; }

    uint64_t hash = 0;
    int32_t blk[64];
    int32_t rc = TC_OK;
    for (uint32_t by = 0u; by < band && rc == TC_OK; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            int has_left = bx > 0u ? 1 : 0;
            int has_top = by > 0u ? 1 : 0;
            if (use_vlc != 0) {
                rc = tc_block_decode_vlc(&br, dc_b, lvl_b, run_b, has_left,
                                         row[bx > 0u ? bx - 1u : 0u], has_top, prev[bx], blk);
            } else {
                rc = tc_block_decode(&br, sh->k1, sh->k2, sh->k3, has_left,
                                     row[bx > 0u ? bx - 1u : 0u], has_top, prev[bx], blk);
            }
            if (rc != TC_OK) { break; }
            row[bx] = blk[0];
            for (uint32_t i = 0u; i < 64u; ++i) {
                hash = tc_symbol_hash_mix(hash, (uint64_t)(int64_t)blk[i]);
            }
            memcpy(q_out + ((size_t)by * (size_t)cols + (size_t)bx) * 64u, blk,
                   64u * sizeof(int32_t));
            uint8_t ac = 0u;
            for (uint32_t i = 1u; i < 64u; ++i) {
                if (q_out[((size_t)by * (size_t)cols + (size_t)bx) * 64u + i] != 0) {
                    ac = 1u;
                    break;
                }
            }
            has_ac_out[by * cols + bx] = ac;
        }
        int32_t* tmp = prev; prev = row; row = tmp;
    }
    free(prev);
    free(row);
    if (rc != TC_OK) { return rc; }
    int32_t arc = tc_bitreader_align_byte(&br);
    if (arc != TC_OK) { return arc; }
    if (tc_bitreader_bits_consumed(&br) != (uint64_t)payload_size * 8u) {
        return TC_ERR_MALFORMED;
    }
    *hash_out = hash;
    return TC_OK;
}

static void diff_case(const topos_frame_header* fh, const topos_slice_header* sh,
                      const uint8_t* payload, size_t payload_size, uint32_t blocks)
{
    static int32_t q_fused[512 * 64];
    static int32_t q_ref[512 * 64];
    static uint8_t ac_fused[512];
    static uint8_t ac_ref[512];
    uint64_t h_fused = 0, h_ref = 0;

    rec_sink_ctx c;
    memset(&c, 0, sizeof(c));
    c.q = q_fused;
    c.has_ac = ac_fused;
    MT_CHECK_EQ_I64(tc_color_slice_decode_stream(fh, sh, payload, payload_size, rec_sink,
                                                 &c, &h_fused),
                    TC_OK);
    MT_CHECK_EQ_I64(ref_decode(fh, sh, payload, payload_size, q_ref, ac_ref, &h_ref), TC_OK);
    MT_CHECK(memcmp(q_fused, q_ref, (size_t)blocks * 64u * sizeof(int32_t)) == 0);
    MT_CHECK(memcmp(ac_fused, ac_ref, blocks) == 0);
    MT_CHECK_EQ_U64(h_fused, h_ref);

    /* 逐字节截断：返回码一致（两侧同码同终止点；块内容不比——失败即弃） */
    for (size_t cut = 0u; cut < payload_size; ++cut) {
        rec_sink_ctx c2;
        memset(&c2, 0, sizeof(c2));
        c2.q = q_fused;
        c2.has_ac = ac_fused;
        int32_t r1 = tc_color_slice_decode_stream(fh, sh, payload, cut, rec_sink, &c2, NULL);
        int32_t r2 = ref_decode(fh, sh, payload, cut, q_ref, ac_ref, &h_ref);
        if (r1 != r2) {
            mt_report(__FILE__, __LINE__, "fused/discrete rc diverge at cut");
            MT_CHECK_EQ_I64((int64_t)r1, (int64_t)r2);
            break;
        }
        if (r1 >= 0) {
            mt_report(__FILE__, __LINE__, "truncated decode must fail");
            break;
        }
    }

    /* sink 注入失败：第 limit 块起透传错误码，已交付块与参考前缀一致 */
    rec_sink_ctx c3;
    memset(&c3, 0, sizeof(c3));
    c3.q = q_fused;
    c3.has_ac = ac_fused;
    c3.limit = blocks / 2u;
    c3.fail_rc = TC_ERR_OUT_OF_MEMORY;
    MT_CHECK_EQ_I64(tc_color_slice_decode_stream(fh, sh, payload, payload_size, rec_sink,
                                                 &c3, NULL),
                    TC_ERR_OUT_OF_MEMORY);
    MT_CHECK_EQ_U64(c3.count, c3.limit);
    MT_CHECK(memcmp(q_fused, q_ref, (size_t)c3.limit * 64u * sizeof(int32_t)) == 0);
}

/* ---- M10-2A 差分：专用 scan-to-plane vs 独立标量参考重建 ----
 * 参考侧：generic 流式解码记录 q 块（上方已钉死 == 离散参考）+ 测试内
 * 标量 dequant→inverse→clip 重建（恒走全 IDCT——DC-only 闭式与 full 的
 * bit-exact 等价由本差分顺带复验）。对拍维度：全 payload 像素、逐字节
 * 截断返回码、逐字节翻转损坏（成功时像素 + 失败时错误码）。 */
static void diff_direct_case(const topos_frame_header* fh, const topos_slice_header* sh,
                             const uint8_t* payload, size_t payload_size, uint32_t blocks)
{
    static int32_t q_ref[512 * 64];
    static uint8_t ac_ref[512];
    static uint16_t plane_ref[64 * 64];
    static uint16_t plane_dir[64 * 64];

    const uint32_t cols = fh->plane_block_cols[sh->plane];
    const uint32_t vis_w = fh->plane_visible_w[sh->plane];
    const uint32_t vis_h = fh->plane_visible_h[sh->plane];
    const uint32_t mid = 1u << (fh->bit_depth - 1u);
    const uint32_t max = (1u << fh->bit_depth) - 1u;

    /* 参考系数（generic 流式） */
    rec_sink_ctx c;
    memset(&c, 0, sizeof(c));
    c.q = q_ref;
    c.has_ac = ac_ref;
    MT_CHECK_EQ_I64(tc_color_slice_decode_stream(fh, sh, payload, payload_size, rec_sink,
                                                 &c, NULL),
                    TC_OK);

    /* 独立标量重建（全 IDCT + 标量 clip store） */
    {
        const tc_qmatrix_set* qms = tc_qmatrix_by_id(fh->qmatrix_id);
        MT_CHECK(qms != NULL);
        tc_quant_ctx qctx;
        tc_quant_ctx_init(&qctx, (sh->plane == 0u) ? qms->luma : qms->chroma,
                          (uint32_t)tc_slice_effective_qp(fh->qp_base, sh->qp_delta_biased));
        memset(plane_ref, 0xAA, sizeof(plane_ref));
        for (uint32_t b = 0u; b < blocks; ++b) {
            int32_t F[64];
            int32_t xh[64];
            tc_dequant_block_ctx(&qctx, q_ref + (size_t)b * 64u, F);
            tc_transform_inverse_8x8_scalar(F, xh);
            uint32_t bx = b % cols;
            uint32_t by = sh->block_y0 + b / cols;
            uint32_t px0 = bx * 8u;
            uint32_t py0 = by * 8u;
            uint32_t cw = (vis_w - px0 < 8u) ? vis_w - px0 : 8u;
            uint32_t ch = (vis_h - py0 < 8u) ? vis_h - py0 : 8u;
            for (uint32_t y = 0u; y < ch; ++y) {
                for (uint32_t x = 0u; x < cw; ++x) {
                    int32_t v = xh[y * 8u + x] + (int32_t)mid;
                    if (v < 0) { v = 0; }
                    else if (v > (int32_t)max) { v = (int32_t)max; }
                    plane_ref[(size_t)(py0 + y) * vis_w + px0 + x] = (uint16_t)v;
                }
            }
        }
    }

    /* 构造 store 上下文（quant ctx 堆分配，函数末统一释放） */
    tc_color_store_ctx sc;
    memset(&sc, 0, sizeof(sc));
    const tc_qmatrix_set* qms = tc_qmatrix_by_id(fh->qmatrix_id);
    MT_CHECK(qms != NULL);
    if (qms == NULL) { return; }
    tc_quant_ctx* qctx = (tc_quant_ctx*)malloc(sizeof(tc_quant_ctx));
    MT_CHECK(qctx != NULL);
    if (qctx == NULL) { return; }
    tc_quant_ctx_init(qctx, (sh->plane == 0u) ? qms->luma : qms->chroma,
                      (uint32_t)tc_slice_effective_qp(fh->qp_base, sh->qp_delta_biased));
    sc.qctx = qctx;
    sc.cols = cols;
    sc.block_y0 = sh->block_y0;
    sc.vis_w = vis_w;
    sc.vis_h = vis_h;
    sc.mid = mid;
    sc.max = max;
    sc.w0 = tc_transform_weights()[0];
    sc.dinv = tc_simd_resolve_dequant_inverse(0u);

    /* 直写路径落平面，与独立标量参考逐位对拍（band 只覆盖
     * [block_y0·8, block_y0·8 + band·8) 行——比较范围与写入范围一致） */
    memset(plane_dir, 0x55, sizeof(plane_dir));
    sc.dst = plane_dir;
    sc.stride = vis_w;
    {
        int32_t* prev = (int32_t*)calloc(cols, sizeof(int32_t));
        int32_t* row = (int32_t*)calloc(cols, sizeof(int32_t));
        MT_CHECK(prev != NULL && row != NULL);
        tc_scan_dc_ctx dc;
        dc.prev_row = prev;
        dc.row = row;
        dc.elems = cols;
        MT_CHECK_EQ_I64(
            tc_color_slice_decode_to_plane(fh, sh, payload, payload_size, &dc, &sc), TC_OK);
        free(prev);
        free(row);
    }
    {
        uint32_t y0 = sh->block_y0 * 8u;
        uint32_t rows = sh->block_h * 8u;
        if (rows > vis_h - y0) { rows = vis_h - y0; }
        MT_CHECK(memcmp(plane_dir + (size_t)y0 * vis_w, plane_ref + (size_t)y0 * vis_w,
                        (size_t)vis_w * rows * sizeof(uint16_t)) == 0);
    }

    /* 逐字节截断：直写与 generic 流式返回码一致，且必须为负 */
    for (size_t cut = 0u; cut < payload_size; ++cut) {
        int32_t* prev = (int32_t*)calloc(cols, sizeof(int32_t));
        int32_t* row = (int32_t*)calloc(cols, sizeof(int32_t));
        tc_scan_dc_ctx dc;
        dc.prev_row = prev;
        dc.row = row;
        dc.elems = cols;
        int32_t r_dir = tc_color_slice_decode_to_plane(fh, sh, payload, cut, &dc, &sc);
        free(prev);
        free(row);
        rec_sink_ctx c2;
        memset(&c2, 0, sizeof(c2));
        c2.q = q_ref;
        c2.has_ac = ac_ref;
        int32_t r_gen = tc_color_slice_decode_stream(fh, sh, payload, cut, rec_sink, &c2, NULL);
        if (r_dir != r_gen || r_dir >= 0) {
            mt_report(__FILE__, __LINE__, "direct/generic truncated rc diverge");
            MT_CHECK_EQ_I64((int64_t)r_dir, (int64_t)r_gen);
            break;
        }
    }

    /* 逐字节翻转损坏：返回码一致；两侧都成功时像素一致（写入范围可能
     * 不完整——只比对两侧一致，不比对参考） */
    {
        uint8_t* mut = (uint8_t*)malloc(payload_size);
        MT_CHECK(mut != NULL);
        if (mut != NULL) {
            for (size_t pos = 0u; pos < payload_size; ++pos) {
                memcpy(mut, payload, payload_size);
                mut[pos] ^= 0xFFu;

                int32_t* prev = (int32_t*)calloc(cols, sizeof(int32_t));
                int32_t* row = (int32_t*)calloc(cols, sizeof(int32_t));
                tc_scan_dc_ctx dc;
                dc.prev_row = prev;
                dc.row = row;
                dc.elems = cols;
                memset(plane_dir, 0, sizeof(plane_dir));
                sc.dst = plane_dir;
                int32_t r_dir = tc_color_slice_decode_to_plane(fh, sh, mut, payload_size,
                                                               &dc, &sc);
                free(prev);
                free(row);

                rec_sink_ctx c2;
                memset(&c2, 0, sizeof(c2));
                c2.q = q_ref;
                c2.has_ac = ac_ref;
                int32_t r_gen = tc_color_slice_decode_stream(fh, sh, mut, payload_size,
                                                             rec_sink, &c2, NULL);
                if (r_dir != r_gen) {
                    mt_report(__FILE__, __LINE__, "direct/generic corrupt rc diverge");
                    MT_CHECK_EQ_I64((int64_t)r_dir, (int64_t)r_gen);
                    break;
                }
            }
            free(mut);
        }
    }
    free(qctx);
}

int main(void)
{
    topos_frame_header fh;
    make_fh(&fh, 1);

    tc_bitwriter bw;
    MT_CHECK_EQ_I64(tc_bitwriter_init(&bw), TC_OK);

    /* ---- 颜色带：2 块行 × 8 块列 ---- */
    {
        enum { COLS = 8, BAND = 2, BLOCKS = COLS * BAND };
        /* R6：tc_color_slice_encode 输入为 zigzag 扫描序——以 natural 序定义
         * 期望块，散射后送编码；解码输出 natural 序与期望对拍。 */
        static int32_t nat[BLOCKS * 64];
        static int32_t q[BLOCKS * 64];
        static int32_t back[BLOCKS * 64];
        for (size_t b = 0; b < BLOCKS; ++b) {
            for (uint32_t i = 0; i < 64u; ++i) { nat[b * 64u + i] = 0; }
            nat[b * 64u] = (int32_t)(b * 37u % 900u); /* 行进 DC */
            if ((b % 3u) == 0u) { nat[b * 64u + 9u] = -1200; }
            if ((b % 5u) == 0u) { nat[b * 64u + 63u] = 77; } /* natural 63 回归点 */
            for (uint32_t i = 0; i < 64u; ++i) {
                q[b * 64u + kTcZigzagInv[i]] = nat[b * 64u + i];
            }
        }
        topos_slice_header sh;
        memset(&sh, 0, sizeof(sh));
        sh.plane = 0u;
        sh.block_y0 = 1u;
        sh.block_h = BAND;
        sh.qp_delta_biased = 64u;
        sh.k1 = 3u; sh.k2 = 5u; sh.k3 = 1u;

        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_color_slice_encode(&fh, &sh, q, &bw), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        const uint8_t* payload = tc_bitwriter_data(&bw);
        size_t payload_size = tc_bitwriter_byte_size(&bw);

        uint64_t hash_arr = 1, hash_null = 2;
        memset(back, 0xAA, sizeof(back));
        MT_CHECK_EQ_I64(tc_color_slice_decode(&fh, &sh, payload, payload_size, back, &hash_arr),
                        TC_OK);
        MT_CHECK(memcmp(back, nat, sizeof(nat)) == 0);
        MT_CHECK_EQ_I64(tc_color_slice_decode(&fh, &sh, payload, payload_size, NULL, &hash_null),
                        TC_OK);
        MT_CHECK_EQ_U64(hash_arr, hash_null); /* 数组路径与 O(1) 指纹路径一致 */

        /* 截断：任一截断点 → 负值返回，不崩溃 */
        for (size_t cut = 0; cut < payload_size; ++cut) {
            int32_t rc = tc_color_slice_decode(&fh, &sh, payload, cut, NULL, NULL);
            if (rc >= 0) { mt_report(__FILE__, __LINE__, "truncated decode must fail"); break; }
        }
        /* 多声明 1 字节（缓冲内确有该字节）→ 熵流未消费完 → MALFORMED */
        MT_CHECK_EQ_I64(tc_color_slice_decode(&fh, &sh, payload, payload_size + 1u, NULL, NULL),
                        TC_ERR_MALFORMED);

        /* 解码深化：融合流式 vs 离散参考（Rice，全量 + 逐字节截断 + sink 注入） */
        diff_case(&fh, &sh, payload, payload_size, BLOCKS);

        /* M10-2A：专用 scan-to-plane vs 独立标量参考（Rice/10-bit/422/63×45
         * 非 8 对齐边缘） */
        diff_direct_case(&fh, &sh, payload, payload_size, BLOCKS);
    }

    /* ---- 解码深化：VLC（major=2/entropy=1）差分 ---- */
    {
        enum { COLS = 8, BAND = 2, BLOCKS = COLS * BAND };
        static int32_t q[BLOCKS * 64];
        for (size_t b = 0; b < BLOCKS; ++b) {
            for (uint32_t i = 0u; i < 64u; ++i) { q[b * 64u + i] = 0; }
            uint64_t x = (uint64_t)b * 6364136223846793005ull + 1442695040888963407ull;
            x = (x ^ (x >> 30)) * 2654435761ull;
            q[b * 64u] = (int32_t)(x % 4096u);
            if ((b % 4u) == 1u) { continue; } /* DC-only 块（EOB 早退路径） */
            for (uint32_t i = 1u; i < 64u; ++i) {
                x = (x ^ (x >> 27)) * 2246822519ull;
                q[b * 64u + i] = ((x >> 33) % 7ull) == 0ull
                    ? (int32_t)((int64_t)(x % 5001ull) - 2500) : 0;
            }
        }
        topos_frame_header fh2 = fh;
        fh2.version_major = 2u;
        fh2.entropy_mode = 1u;
        topos_slice_header sh;
        memset(&sh, 0, sizeof(sh));
        sh.plane = 0u;
        sh.block_y0 = 0u;
        sh.block_h = BAND;
        sh.qp_delta_biased = 64u;
        sh.k1 = 1u; sh.k2 = 2u; sh.k3 = 0u; /* book id 0..3 */

        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_color_slice_encode(&fh2, &sh, q, &bw), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        const uint8_t* payload = tc_bitwriter_data(&bw);
        size_t payload_size = tc_bitwriter_byte_size(&bw);
        diff_case(&fh2, &sh, payload, payload_size, BLOCKS);
        diff_direct_case(&fh2, &sh, payload, payload_size, BLOCKS);
    }

    /* ---- M10-2A：12-bit / 4:4:4 直写差分（位深定标 + 全宽色度平面） ---- */
    {
        enum { COLS = 8, BAND = 2, BLOCKS = COLS * BAND };
        topos_frame_header fh3;
        make_fh(&fh3, 0);
        fh3.bit_depth = 12u;
        fh3.pixel_format = 1u; /* 4:4:4：U/V 全宽 */
        MT_CHECK_EQ_I64(tc_frame_derive_geometry(&fh3), TC_OK);
        MT_CHECK_EQ_U64(fh3.plane_block_cols[1], 8ull); /* chroma 全宽同 luma */

        static int32_t q[BLOCKS * 64];
        for (size_t b = 0; b < BLOCKS; ++b) {
            for (uint32_t i = 0u; i < 64u; ++i) { q[b * 64u + i] = 0; }
            uint64_t x = (uint64_t)b * 2862933555777941757ull + 3037000493ull;
            x = (x ^ (x >> 31)) * 48657493ull;
            q[b * 64u] = (int32_t)(x % 16000u);
            if ((b % 3u) != 0u) {
                for (uint32_t i = 1u; i < 64u; ++i) {
                    x = (x ^ (x >> 29)) * 2654435761ull;
                    q[b * 64u + i] = ((x >> 35) % 5ull) == 0ull
                        ? (int32_t)((int64_t)(x % 8001ull) - 4000) : 0;
                }
            }
        }
        topos_slice_header sh;
        memset(&sh, 0, sizeof(sh));
        sh.plane = 1u; /* chroma 平面（4:4:4 全宽） */
        sh.block_y0 = 1u;
        sh.block_h = BAND;
        sh.qp_delta_biased = 64u;
        sh.k1 = 4u; sh.k2 = 6u; sh.k3 = 2u;

        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_color_slice_encode(&fh3, &sh, q, &bw), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        const uint8_t* payload = tc_bitwriter_data(&bw);
        size_t payload_size = tc_bitwriter_byte_size(&bw);
        diff_direct_case(&fh3, &sh, payload, payload_size, BLOCKS);
    }

    /* ---- Alpha 带：64 × 16 像素 ---- */
    {
        enum { W = 64, ROWS = 16, N = W * ROWS };
        static int32_t r[N];
        static int32_t back[N];
        for (size_t i = 0; i < N; ++i) {
            uint64_t x = (uint64_t)i * 2654435761ull;
            r[i] = (x % 7ull) == 0ull ? (int32_t)((int64_t)(x % 131071ull) - 65535) : 0;
        }
        topos_slice_header sh;
        memset(&sh, 0, sizeof(sh));
        sh.plane = 3u;
        sh.block_y0 = 0u;
        sh.block_h = 2u; /* 16 行 */
        sh.qp_delta_biased = 64u;
        sh.k1 = 8u; sh.k2 = 6u; sh.k3 = 0u;

        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_alpha_slice_encode(&fh, &sh, r, N, &bw), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        const uint8_t* payload = tc_bitwriter_data(&bw);
        size_t payload_size = tc_bitwriter_byte_size(&bw);

        uint64_t h1 = 0, h2 = 0;
        memset(back, 0xAA, sizeof(back));
        MT_CHECK_EQ_I64(tc_alpha_slice_decode(&fh, &sh, payload, payload_size, back, N, &h1),
                        TC_OK);
        MT_CHECK(memcmp(back, r, sizeof(r)) == 0);
        MT_CHECK_EQ_I64(tc_alpha_slice_decode(&fh, &sh, payload, payload_size, NULL, 0u, &h2),
                        TC_OK);
        MT_CHECK_EQ_U64(h1, h2);
        for (size_t cut = 0; cut < payload_size; ++cut) {
            int32_t rc = tc_alpha_slice_decode(&fh, &sh, payload, cut, NULL, 0u, NULL);
            if (rc >= 0) { mt_report(__FILE__, __LINE__, "alpha truncated must fail"); break; }
        }
    }

    tc_bitwriter_free(&bw);
    return MT_MAIN_RETURN();
}
