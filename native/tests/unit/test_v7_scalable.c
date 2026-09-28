/* RD4：V7-B base + signed residual mux round-trip and base-only skip contract. */
#include "codec/v7_scalable.h"
#include "common/endian.h"
#include "common/tpool.h"
#include "mini_test.h"

#include <stdlib.h>
#include <string.h>

static void init_config(topos_frame_config* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->struct_size = (uint32_t)sizeof(*cfg);
    cfg->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg->visible_width = 48u;
    cfg->visible_height = 24u;
    cfg->profile = 3u;
    cfg->pixel_format = 0u;
    cfg->bit_depth = 10u;
    cfg->qmatrix_id = 0u;
    cfg->qp_base = 12u;
    cfg->color_range = 1u;
    cfg->color_primaries = 1u;
    cfg->color_transfer = 1u;
    cfg->color_matrix = 1u;
}

static void fill_plane(uint16_t* pixels, uint32_t width, uint32_t height, uint32_t seed)
{
    for (uint32_t y = 0u; y < height; ++y) {
        for (uint32_t x = 0u; x < width; ++x) {
            pixels[(size_t)y * width + x] = (uint16_t)((x * 19u + y * 31u +
                                                        seed * 47u + (x ^ y) * 3u) & 1023u);
        }
    }
}

typedef struct counting_ctx {
    const uint8_t* packet;
    uint64_t total_bytes;
    uint32_t calls;
} counting_ctx;

static int32_t counting_read_fn(void* ctx, uint64_t offset, void* buf, size_t len)
{
    counting_ctx* cctx = (counting_ctx*)ctx;
    ++cctx->calls;
    cctx->total_bytes += (uint64_t)len;
    memcpy(buf, cctx->packet + offset, len);
    return TC_OK;
}

static uint32_t v7b_test_directory_size(const uint8_t* packet)
{
    /* TPLD 目录：帧头后 fixed36 内 directory_size 位于 +24 */
    const uint8_t* directory = packet + TC_FRAME_HEADER_SIZE;
    return tc_load_be32(directory + 24u);
}

int main(void)
{
    const uint32_t width = 48u, height = 24u, chroma_width = 24u;
    /* 定长数组（MSVC 无 VLA）：几何为常量，enum 提供编译期尺寸 */
    enum { Y_ELEMS = 48u * 24u, C_ELEMS = 24u * 24u };
    uint16_t y[Y_ELEMS];
    uint16_t u[C_ELEMS];
    uint16_t v[C_ELEMS];
    fill_plane(y, width, height, 1u);
    fill_plane(u, chroma_width, height, 2u);
    fill_plane(v, chroma_width, height, 3u);
    topos_frame_config cfg;
    init_config(&cfg);
    topos_frame_input input;
    memset(&input, 0, sizeof(input));
    input.struct_size = (uint32_t)sizeof(input);
    input.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    input.planes[0] = y;
    input.planes[1] = u;
    input.planes[2] = v;
    input.strides[0] = 0u;
    input.strides[1] = 0u;
    input.strides[2] = 0u;

    const size_t cap = 8u * 1024u * 1024u;
    uint8_t* packet = (uint8_t*)malloc(cap);
    MT_CHECK(packet != NULL);
    if (packet == NULL) { return MT_MAIN_RETURN(); }
    topos_frame_stats enc_stats;
    memset(&enc_stats, 0, sizeof(enc_stats));
    topos_frame_stats probe_stats;
    memset(&probe_stats, 0, sizeof(probe_stats));
    MT_CHECK_EQ_I64(tc_frame_encode_scalable(&cfg, &input, 16u, NULL, 0u,
                                              &probe_stats), TC_ERR_BUFFER_TOO_SMALL);
    MT_CHECK(probe_stats.packet_size > 53u);
    MT_CHECK_EQ_I64(tc_frame_encode_scalable(&cfg, &input, 2049u, packet, cap,
                                              &enc_stats), TC_ERR_LIMIT_EXCEEDED);
    const int32_t encode_rc = tc_frame_encode_scalable(&cfg, &input, 16u,
                                                        packet, cap, &enc_stats);
    MT_CHECK_EQ_I64(encode_rc, TC_OK);
    MT_CHECK(enc_stats.packet_size > 53u);

    uint16_t base_y[16u * 8u];
    uint16_t base_u[8u * 8u];
    uint16_t base_v[8u * 8u];
    uint16_t* base_planes[TC_FRAME_MAX_PLANES] = { base_y, base_u, base_v, NULL };
    size_t base_strides[TC_FRAME_MAX_PLANES] = { 16u, 8u, 8u, 0u };
    topos_frame_output base_info;
    tc_v7b_decode_stats base_stats;
    MT_CHECK_EQ_I64(tc_v7b_frame_decode(packet, enc_stats.packet_size, 16u,
                                         TC_V7B_DECODE_BASE_ONLY, base_planes,
                                         base_strides, &base_info, &base_stats), TC_OK);
    MT_CHECK_EQ_U64(base_info.visible_width, 16u);
    MT_CHECK_EQ_U64(base_info.visible_height, 8u);
    MT_CHECK_EQ_U64(base_stats.segments_requested, 1u);
    MT_CHECK_EQ_U64(base_stats.segments_read, 1u);
    MT_CHECK_EQ_U64(base_stats.segments_skipped, 3u);
    MT_CHECK(base_stats.bytes_skipped > 0u);

    topos_frame_output query_info;
    tc_v7b_decode_stats query_stats;
    MT_CHECK_EQ_I64(tc_v7b_frame_decode(packet, enc_stats.packet_size, 16u,
                                         TC_V7B_DECODE_FULL, NULL, NULL,
                                         &query_info, &query_stats), TC_OK);
    MT_CHECK_EQ_U64(query_info.visible_width, width);
    MT_CHECK_EQ_U64(query_info.visible_height, height);
    MT_CHECK_EQ_U64(query_stats.segments_requested, 1u);
    MT_CHECK_EQ_U64(query_stats.segments_read, 1u);

    MT_CHECK_EQ_I64(tc_v7b_frame_decode(packet, enc_stats.packet_size - 1u, 16u,
                                         TC_V7B_DECODE_BASE_ONLY, base_planes,
                                         base_strides, &base_info, NULL), TC_ERR_TRUNCATED);

    uint16_t out_y[Y_ELEMS];
    uint16_t out_u[C_ELEMS];
    uint16_t out_v[C_ELEMS];
    uint16_t* out_planes[TC_FRAME_MAX_PLANES] = { out_y, out_u, out_v, NULL };
    size_t out_strides[TC_FRAME_MAX_PLANES] = { width, chroma_width, chroma_width, 0u };
    topos_frame_output full_info;
    tc_v7b_decode_stats full_stats;
    MT_CHECK_EQ_I64(tc_v7b_frame_decode(packet, enc_stats.packet_size, 16u,
                                         TC_V7B_DECODE_FULL, out_planes,
                                         out_strides, &full_info, &full_stats), TC_OK);
    MT_CHECK_EQ_U64(full_info.visible_width, width);
    MT_CHECK_EQ_U64(full_info.visible_height, height);
    MT_CHECK_EQ_U64(full_stats.segments_requested, 4u);
    MT_CHECK_EQ_U64(full_stats.segments_read, 4u);
    MT_CHECK(memcmp(out_y, y, sizeof(y)) == 0);
    MT_CHECK(memcmp(out_u, u, sizeof(u)) == 0);
    MT_CHECK(memcmp(out_v, v, sizeof(v)) == 0);

    uint16_t reduced_y[16u * 8u];
    uint16_t reduced_u[8u * 8u];
    uint16_t reduced_v[8u * 8u];
    uint16_t* reduced_planes[TC_FRAME_MAX_PLANES] = {
        reduced_y, reduced_u, reduced_v, NULL
    };
    size_t reduced_strides[TC_FRAME_MAX_PLANES] = { 16u, 8u, 8u, 0u };
    topos_frame_output reduced_info;
    MT_CHECK_EQ_I64(tc_frame_decode_reduced(packet, enc_stats.packet_size,
                                             TC_DECODE_SCALE_THIRD, reduced_planes,
                                             reduced_strides, &reduced_info), TC_OK);
    MT_CHECK_EQ_U64(reduced_info.visible_width, 16u);
    MT_CHECK_EQ_U64(reduced_info.visible_height, 8u);
    MT_CHECK(memcmp(reduced_y, base_y, sizeof(base_y)) == 0);
    MT_CHECK(memcmp(reduced_u, base_u, sizeof(base_u)) == 0);
    MT_CHECK(memcmp(reduced_v, base_v, sizeof(base_v)) == 0);

    topos_frame_output reduced_query;
    MT_CHECK_EQ_I64(tc_frame_decode_reduced(packet, enc_stats.packet_size,
                                             TC_DECODE_SCALE_THIRD, NULL, NULL,
                                             &reduced_query), TC_OK);
    MT_CHECK_EQ_U64(reduced_query.visible_width, 16u);
    MT_CHECK_EQ_U64(reduced_query.visible_height, 8u);

    topos_decode_request request;
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    request.mode = TC_DECODE_MODE_REDUCED;
    request.scale = TC_DECODE_SCALE_THIRD;
    request.memory_type = TC_DECODE_MEMORY_CPU;
    MT_CHECK_EQ_I64(tc_frame_decode_request(packet, enc_stats.packet_size, &request,
                                             reduced_planes, reduced_strides,
                                             &reduced_info), TC_OK);
    MT_CHECK_EQ_U64(reduced_info.visible_width, 16u);
    MT_CHECK_EQ_U64(reduced_info.visible_height, 8u);
    MT_CHECK(memcmp(reduced_y, base_y, sizeof(base_y)) == 0);
    MT_CHECK(memcmp(reduced_u, base_u, sizeof(base_u)) == 0);
    MT_CHECK(memcmp(reduced_v, base_v, sizeof(base_v)) == 0);

    topos_frame_output public_info;
    MT_CHECK_EQ_I64(tc_frame_decode(packet, enc_stats.packet_size, out_planes,
                                    out_strides, &public_info), TC_OK);
    MT_CHECK_EQ_U64(public_info.visible_width, width);
    MT_CHECK_EQ_U64(public_info.visible_height, height);
    MT_CHECK(memcmp(out_y, y, sizeof(y)) == 0);
    MT_CHECK(memcmp(out_u, u, sizeof(u)) == 0);
    MT_CHECK(memcmp(out_v, v, sizeof(v)) == 0);

    tc_decoder* decoder = NULL;
    MT_CHECK_EQ_I64(tc_decoder_create(NULL, &decoder), TC_OK);
    topos_plane_view full_views[TC_FRAME_MAX_PLANES];
    memset(full_views, 0, sizeof(full_views));
    for (uint32_t p = 0u; p < 3u; ++p) {
        full_views[p].struct_size = (uint32_t)sizeof(topos_plane_view);
        full_views[p].abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        full_views[p].pixels = out_planes[p];
        full_views[p].stride = out_strides[p];
    }
    topos_frame_output context_info;
    MT_CHECK_EQ_I64(tc_decoder_prepare(decoder, packet, enc_stats.packet_size,
                                       &context_info), TC_OK);
    MT_CHECK_EQ_U64(context_info.visible_width, width);
    MT_CHECK_EQ_I64(tc_decoder_decode(decoder, packet, enc_stats.packet_size,
                                      full_views, &context_info), TC_OK);
    MT_CHECK(memcmp(out_y, y, sizeof(y)) == 0);
    MT_CHECK(memcmp(out_u, u, sizeof(u)) == 0);
    MT_CHECK(memcmp(out_v, v, sizeof(v)) == 0);

    topos_plane_view reduced_views[TC_FRAME_MAX_PLANES];
    memset(reduced_views, 0, sizeof(reduced_views));
    for (uint32_t p = 0u; p < 3u; ++p) {
        reduced_views[p].struct_size = (uint32_t)sizeof(topos_plane_view);
        reduced_views[p].abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        reduced_views[p].pixels = reduced_planes[p];
        reduced_views[p].stride = reduced_strides[p];
    }
    MT_CHECK_EQ_I64(tc_decoder_prepare_request(decoder, packet, enc_stats.packet_size,
                                               &request, &context_info), TC_OK);
    MT_CHECK_EQ_U64(context_info.visible_width, 16u);
    MT_CHECK_EQ_I64(tc_decoder_decode_request(decoder, packet, enc_stats.packet_size,
                                              &request, reduced_views, &context_info), TC_OK);
    MT_CHECK(memcmp(reduced_y, base_y, sizeof(base_y)) == 0);
    MT_CHECK(memcmp(reduced_u, base_u, sizeof(base_u)) == 0);
    MT_CHECK(memcmp(reduced_v, base_v, sizeof(base_v)) == 0);

    uint16_t batch_y[2][16u * 8u];
    uint16_t batch_u[2][8u * 8u];
    uint16_t batch_v[2][8u * 8u];
    topos_plane_view batch_views[2u * TC_FRAME_MAX_PLANES];
    memset(batch_views, 0, sizeof(batch_views));
    for (uint32_t f = 0u; f < 2u; ++f) {
        uint16_t* batch_planes[TC_FRAME_MAX_PLANES] = {
            batch_y[f], batch_u[f], batch_v[f], NULL
        };
        for (uint32_t p = 0u; p < 3u; ++p) {
            topos_plane_view* v = &batch_views[f * TC_FRAME_MAX_PLANES + p];
            v->struct_size = (uint32_t)sizeof(topos_plane_view);
            v->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
            v->pixels = batch_planes[p];
            v->stride = reduced_strides[p];
        }
    }
    topos_batch_packet batch_packets[2] = {
        { packet, enc_stats.packet_size }, { packet, enc_stats.packet_size }
    };
    topos_frame_output batch_info[2];
    tc_dev_set_thread_count(2);
    tc_dev_tpool_stats_reset();
    MT_CHECK_EQ_I64(tc_decoder_decode_batch_request(decoder, batch_packets, 2u,
                                                    &request, batch_views,
                                                    batch_info), TC_OK);
    MT_CHECK_EQ_U64(batch_info[0].visible_width, 16u);
    MT_CHECK_EQ_U64(batch_info[1].visible_width, 16u);
    MT_CHECK(memcmp(batch_y[0], base_y, sizeof(base_y)) == 0);
    MT_CHECK(memcmp(batch_y[1], base_y, sizeof(base_y)) == 0);
    tc_tpool_stats batch_pool_stats;
    tc_dev_tpool_stats_get(&batch_pool_stats);
    MT_CHECK(batch_pool_stats.pool_batches >= 1u);
    tc_dev_set_thread_count(1);
    tc_decoder_destroy(decoder);

    /* Corrupt only the final residual byte: base-only must not touch it, while
     * full reconstruction must reject its segment CRC. */
    packet[enc_stats.packet_size - 1u] ^= 0x01u;
    MT_CHECK_EQ_I64(tc_v7b_frame_decode(packet, enc_stats.packet_size, 16u,
                                         TC_V7B_DECODE_BASE_ONLY, base_planes,
                                         base_strides, &base_info, NULL), TC_OK);
    MT_CHECK_EQ_I64(tc_v7b_frame_decode(packet, enc_stats.packet_size, 16u,
                                         TC_V7B_DECODE_FULL, out_planes,
                                         out_strides, &full_info, NULL), TC_ERR_CHECKSUM_MISMATCH);
    free(packet);

    /* —— ADR-C030 minor-4 shared pyramid：encode/decode 闭环 —— */
    {
        /* 源 96×54 > max_dim 50 → 最小可行比例 2（base 48×27，
         * 每平面 3 带：slice = 1 + 3 + 9 = 13）。 */
        const uint32_t test_max_dim = 50u;
        const uint32_t pw = 96u, ph = 54u, pcw = 48u;
        const size_t pcount = (size_t)pw * ph;
        const size_t ccount = (size_t)pcw * ph;
        uint16_t* py = (uint16_t*)malloc(pcount * sizeof(uint16_t));
        uint16_t* pu = (uint16_t*)malloc(ccount * sizeof(uint16_t));
        uint16_t* pv = (uint16_t*)malloc(ccount * sizeof(uint16_t));
        uint16_t* ry = (uint16_t*)malloc(pcount * sizeof(uint16_t));
        uint16_t* ru = (uint16_t*)malloc(ccount * sizeof(uint16_t));
        uint16_t* rv = (uint16_t*)malloc(ccount * sizeof(uint16_t));
        uint16_t* by = (uint16_t*)malloc(pcount * sizeof(uint16_t));
        uint16_t* bu = (uint16_t*)malloc(ccount * sizeof(uint16_t));
        uint16_t* bv = (uint16_t*)malloc(ccount * sizeof(uint16_t));
        uint8_t* ppacket = (uint8_t*)malloc(cap);
        MT_CHECK(py && pu && pv && ry && ru && rv && by && bu && bv && ppacket);
        fill_plane(py, pw, ph, 7u);
        fill_plane(pu, pcw, ph, 8u);
        fill_plane(pv, pcw, ph, 9u);
        topos_frame_config pcfg;
        init_config(&pcfg);
        pcfg.visible_width = (uint16_t)pw;
        pcfg.visible_height = (uint16_t)ph;
        pcfg.qp_base = 0u; /* 近无损：qp0 下 V2 base 应逐位还原 */
        topos_frame_input pinput;
        memset(&pinput, 0, sizeof(pinput));
        pinput.struct_size = (uint32_t)sizeof(pinput);
        pinput.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        pinput.planes[0] = py;
        pinput.planes[1] = pu;
        pinput.planes[2] = pv;
        pinput.strides[0] = pw;
        pinput.strides[1] = pcw;
        pinput.strides[2] = pcw;
        topos_frame_stats pstats;
        memset(&pstats, 0, sizeof(pstats));
        MT_CHECK_EQ_I64(tc_v7b_frame_encode_pyramid(&pcfg, &pinput, test_max_dim,
                                                    ppacket, cap, &pstats),
                        TC_OK);
        MT_CHECK(pstats.packet_size > 0u);
        MT_CHECK_EQ_I64(pstats.slice_count, 1u + 3u + 3u * 3u); /* ratio2 每平面 3 带 */
        MT_CHECK(tc_v7b_packet_is(ppacket, pstats.packet_size));

        /* base-only：不读任何 detail/escape 段 */
        tc_v7b_decode_stats pstats_view;
        uint16_t* base_view_planes[TC_FRAME_MAX_PLANES] = { by, bu, bv, NULL };
        size_t base_view_strides[TC_FRAME_MAX_PLANES] = { 48u, 24u, 24u, 0u };
        topos_frame_output pbase_info;
        memset(&pbase_info, 0, sizeof(pbase_info));
        MT_CHECK_EQ_I64(tc_v7b_frame_decode(ppacket, pstats.packet_size, test_max_dim,
                                            TC_V7B_DECODE_BASE_ONLY,
                                            base_view_planes, base_view_strides,
                                            &pbase_info, &pstats_view), TC_OK);
        MT_CHECK_EQ_U64(pbase_info.visible_width, 48u);
        MT_CHECK_EQ_U64(pbase_info.visible_height, 27u);
        /* base-only 只读 base 段 payload；skipped 只含其余段 payload
         * （帧头与目录字节不计入两者）。 */
        MT_CHECK(pstats_view.bytes_read > 0u);
        MT_CHECK(pstats_view.bytes_skipped > 0u);
        MT_CHECK(pstats_view.bytes_read + pstats_view.bytes_skipped <
                 pstats.packet_size);

        /* full：近无损（qp0 base + 量化 detail），逐平面误差必须受限 */
        uint16_t* full_planes[TC_FRAME_MAX_PLANES] = { ry, ru, rv, NULL };
        size_t full_strides[TC_FRAME_MAX_PLANES] = { pw, pcw, pcw, 0u };
        topos_frame_output pfull_info;
        memset(&pfull_info, 0, sizeof(pfull_info));
        MT_CHECK_EQ_I64(tc_v7b_frame_decode(ppacket, pstats.packet_size, test_max_dim,
                                            TC_V7B_DECODE_FULL, full_planes,
                                            full_strides, &pfull_info, NULL), TC_OK);
        MT_CHECK_EQ_U64(pfull_info.visible_width, pw);
        long max_err = 0;
        for (size_t i = 0u; i < pcount; ++i) {
            long d = (long)py[i] - (long)ry[i];
            if (d < 0) { d = -d; }
            if (d > max_err) { max_err = d; }
        }
        MT_CHECK(max_err <= 2);

        /* P3-01：range reader 只读 头+目录+base 段，enhancement 字节为 0 */
        {
            counting_ctx cctx;
            cctx.packet = ppacket;
            cctx.total_bytes = 0u;
            cctx.calls = 0u;
            uint8_t rbase[64u * 1024u];
            size_t rbase_size = 0u;
            topos_frame_header router_fh;
            tc_v7b_decode_stats rstats;
            MT_CHECK_EQ_I64(tc_v7b_read_base_packet(counting_read_fn, &cctx,
                                                   pstats.packet_size, rbase,
                                                   sizeof(rbase), &rbase_size,
                                                   &router_fh, &rstats), TC_OK);
            (void)test_max_dim; /* range 读取不传 max_dim，几何由包内目录决定 */
            MT_CHECK(rbase_size > 0u);
            MT_CHECK_EQ_U64(rstats.segments_read, 1u);
            MT_CHECK_EQ_U64(rstats.bytes_read, rbase_size);
            /* 实际 I/O：帧头 + 目录 + base 段；任何 enhancement 字节都不读 */
            const uint32_t rdir = v7b_test_directory_size(ppacket);
            MT_CHECK_EQ_U64(cctx.total_bytes,
                            (uint64_t)TC_FRAME_HEADER_SIZE + rdir + rbase_size);
            MT_CHECK_EQ_U64(rstats.bytes_skipped,
                            pstats.packet_size - (uint64_t)TC_FRAME_HEADER_SIZE -
                            rdir - rbase_size);
        }

        /* 互操作几何（99×55 不被任何冻结比例整除）回落 minor-2 rice */
        {
            const uint32_t iw = 99u, ih = 55u, icw = (iw + 1u) / 2u;
            uint16_t* iy = (uint16_t*)malloc((size_t)iw * ih * sizeof(uint16_t));
            uint16_t* iu2 = (uint16_t*)malloc((size_t)icw * ih * sizeof(uint16_t));
            uint16_t* iv2 = (uint16_t*)malloc((size_t)icw * ih * sizeof(uint16_t));
            uint8_t* ipacket = (uint8_t*)malloc(cap);
            MT_CHECK(iy && iu2 && iv2 && ipacket);
            fill_plane(iy, iw, ih, 4u);
            fill_plane(iu2, icw, ih, 5u);
            fill_plane(iv2, icw, ih, 6u);
            topos_frame_config icfg;
            init_config(&icfg);
            icfg.visible_width = (uint16_t)iw;
            icfg.visible_height = (uint16_t)ih;
            topos_frame_input iinput;
            memset(&iinput, 0, sizeof(iinput));
            iinput.struct_size = (uint32_t)sizeof(iinput);
            iinput.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
            iinput.planes[0] = iy;
            iinput.planes[1] = iu2;
            iinput.planes[2] = iv2;
            iinput.strides[0] = iw;
            iinput.strides[1] = icw;
            iinput.strides[2] = icw;
            topos_frame_stats istats;
            memset(&istats, 0, sizeof(istats));
            MT_CHECK_EQ_I64(tc_v7b_frame_encode_pyramid(&icfg, &iinput, test_max_dim,
                                                        ipacket, cap, &istats), TC_OK);
            MT_CHECK_EQ_I64(istats.slice_count, (int64_t)(1u + 3u)); /* 99 非 2 整除 → rice 目录 */
            free(iy); free(iu2); free(iv2); free(ipacket);
        }

        free(py); free(pu); free(pv);
        free(ry); free(ru); free(rv);
        free(by); free(bu); free(bv);
        free(ppacket);
    }

    return MT_MAIN_RETURN();
}
