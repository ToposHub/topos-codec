/* RD4-02：基础层只保留 ≤2K 目标平面，不创建全尺寸中间平面。 */
#include "codec/base_encoder.h"
#include "mini_test.h"
#include "transform/base_scale.h"

#include <stdlib.h>
#include <string.h>

static void base_config(topos_frame_config* cfg, uint16_t width, uint16_t height)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->struct_size = (uint32_t)sizeof(*cfg);
    cfg->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg->visible_width = width;
    cfg->visible_height = height;
    cfg->profile = 3u;
    cfg->pixel_format = 0u;
    cfg->bit_depth = 10u;
    cfg->qp_base = 24u;
}

int main(void)
{
    uint32_t bw = 0u, bh = 0u;
    static const struct {
        uint32_t source_w;
        uint32_t source_h;
    } preview_cases[] = {
        {3840u, 2160u}, {6144u, 3456u}, {7680u, 4320u}, {11520u, 6480u}
    };
    for (size_t i = 0u; i < sizeof(preview_cases) / sizeof(preview_cases[0]); ++i) {
        MT_CHECK(tc_base_scale_dimensions(preview_cases[i].source_w,
                                          preview_cases[i].source_h,
                                          1920u, &bw, &bh));
        MT_CHECK_EQ_U64(bw, 1920u);
        MT_CHECK_EQ_U64(bh, 1080u);
    }
    MT_CHECK(tc_base_scale_dimensions(7680u, 4320u, 2048u, &bw, &bh));
    MT_CHECK_EQ_U64(bw, 2048u);
    MT_CHECK_EQ_U64(bh, 1152u);
    MT_CHECK(tc_base_scale_dimensions(12288u, 6912u, 2048u, &bw, &bh));
    MT_CHECK_EQ_U64(bw, 2048u);
    MT_CHECK_EQ_U64(bh, 1152u);
    MT_CHECK(tc_base_scale_dimensions(16384u, 9216u, 2048u, &bw, &bh));
    MT_CHECK_EQ_U64(bw, 2048u);
    MT_CHECK_EQ_U64(bh, 1152u);

    /* 6K source: the input itself is caller-owned; the prepare call must only
     * allocate the reduced Y/U/V planes (2048×1152 + two 1024×1152 planes). */
    const uint32_t source_w = 6144u, source_h = 3456u;
    const uint32_t chroma_w = (source_w + 1u) / 2u;
    uint16_t* y = (uint16_t*)calloc((size_t)source_w * source_h, sizeof(*y));
    uint16_t* u = (uint16_t*)calloc((size_t)chroma_w * source_h, sizeof(*u));
    uint16_t* v = (uint16_t*)calloc((size_t)chroma_w * source_h, sizeof(*v));
    MT_CHECK(y != NULL && u != NULL && v != NULL);
    if (y == NULL || u == NULL || v == NULL) {
        free(y); free(u); free(v);
        return MT_MAIN_RETURN();
    }
    topos_frame_config cfg;
    base_config(&cfg, (uint16_t)source_w, (uint16_t)source_h);
    topos_frame_input input;
    memset(&input, 0, sizeof(input));
    input.struct_size = (uint32_t)sizeof(input);
    input.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    input.planes[0] = y;
    input.planes[1] = u;
    input.planes[2] = v;
    input.strides[0] = source_w;
    input.strides[1] = chroma_w;
    input.strides[2] = chroma_w;

    tc_base_frame base;
    MT_CHECK_EQ_I64(tc_base_frame_prepare(&cfg, &input, 2048u, &base), TC_OK);
    MT_CHECK_EQ_U64(base.config.visible_width, 2048u);
    MT_CHECK_EQ_U64(base.config.visible_height, 1152u);
    MT_CHECK_EQ_U64(base.plane_width[0], 2048u);
    MT_CHECK_EQ_U64(base.plane_height[0], 1152u);
    MT_CHECK_EQ_U64(base.plane_width[1], 1024u);
    MT_CHECK_EQ_U64(base.plane_height[1], 1152u);
    MT_CHECK(base.owns_plane[0] != 0u && base.owns_plane[1] != 0u &&
             base.owns_plane[2] != 0u);
    MT_CHECK(base.plane_bytes[0] <= (size_t)2048u * 1152u * sizeof(uint16_t));
    MT_CHECK(base.plane_bytes[1] <= (size_t)1024u * 1152u * sizeof(uint16_t));
    MT_CHECK(base.plane_bytes[2] <= (size_t)1024u * 1152u * sizeof(uint16_t));
    MT_CHECK(base.input.planes[0] != y && base.input.planes[1] != u &&
             base.input.planes[2] != v);
    tc_base_frame_release(&base);

    tc_base_budget budget;
    MT_CHECK_EQ_I64(tc_base_split_budget(1000u, 300u, 700u, &budget), TC_OK);
    MT_CHECK_EQ_U64(budget.total_bytes, 1000u);
    MT_CHECK_EQ_U64(budget.base_bytes, 300u);
    MT_CHECK_EQ_U64(budget.residual_bytes, 700u);
    MT_CHECK_EQ_I64(tc_base_split_budget(1001u, 0u, 0u, &budget), TC_OK);
    MT_CHECK_EQ_U64(budget.base_bytes + budget.residual_bytes, 1001u);
    MT_CHECK_EQ_I64(tc_base_split_budget(0u, 1u, 1u, &budget),
                    TC_ERR_INVALID_ARGUMENT);

    /* A <=2K source is borrowed, so preview preparation performs no copy. */
    uint16_t tiny_y[8u * 8u] = {0u};
    uint16_t tiny_u[4u * 8u] = {0u};
    uint16_t tiny_v[4u * 8u] = {0u};
    base_config(&cfg, 8u, 8u);
    memset(&input, 0, sizeof(input));
    input.planes[0] = tiny_y;
    input.planes[1] = tiny_u;
    input.planes[2] = tiny_v;
    MT_CHECK_EQ_I64(tc_base_frame_prepare(&cfg, &input, 2048u, &base), TC_OK);
    MT_CHECK(base.owns_plane[0] == 0u && base.owns_plane[1] == 0u &&
             base.owns_plane[2] == 0u);
    MT_CHECK(base.input.planes[0] == tiny_y && base.input.planes[1] == tiny_u &&
             base.input.planes[2] == tiny_v);
    tc_base_frame_release(&base);

    /* The base-only packet primitive exercises the same path with a small
     * downsampled frame; its output is a normal independently decodable packet. */
    uint16_t encode_y[64u * 32u] = {0u};
    uint16_t encode_u[32u * 32u] = {512u};
    uint16_t encode_v[32u * 32u] = {512u};
    base_config(&cfg, 64u, 32u);
    memset(&input, 0, sizeof(input));
    input.planes[0] = encode_y;
    input.planes[1] = encode_u;
    input.planes[2] = encode_v;
    const size_t packet_cap = tc_frame_packet_bound(&cfg);
    uint8_t* packet = (uint8_t*)malloc(packet_cap);
    MT_CHECK(packet != NULL);
    if (packet != NULL) {
        topos_frame_stats stats;
        memset(&stats, 0, sizeof(stats));
        MT_CHECK_EQ_I64(tc_base_frame_encode(&cfg, &input, 16u,
                                             packet, packet_cap, &stats), TC_OK);
        MT_CHECK(stats.packet_size > 53u && stats.packet_size <= packet_cap);
        free(packet);
    }

    packet = (uint8_t*)malloc(packet_cap);
    MT_CHECK(packet != NULL);
    if (packet != NULL) {
        uint8_t qp_used = 0u;
        topos_frame_stats sized_stats;
        memset(&sized_stats, 0, sizeof(sized_stats));
        MT_CHECK_EQ_I64(tc_base_frame_encode_sized(
            &cfg, &input, 16u, (uint32_t)(packet_cap / 2u), 0u, 63u,
            &qp_used, packet, packet_cap, &sized_stats), TC_OK);
        MT_CHECK(qp_used <= 63u && sized_stats.packet_size > 53u);
        free(packet);
    }

    free(y); free(u); free(v);
    MT_MAIN_RETURN();
}
