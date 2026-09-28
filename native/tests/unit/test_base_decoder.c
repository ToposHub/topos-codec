/* RD4-04：base-only packet 在几何门禁后完整解码。 */
#include "codec/base_decoder.h"
#include "codec/base_encoder.h"
#include "mini_test.h"

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
    topos_frame_config cfg;
    base_config(&cfg, 64u, 32u);
    uint16_t y[64u * 32u] = {0u};
    uint16_t u[32u * 32u] = {512u};
    uint16_t v[32u * 32u] = {512u};
    topos_frame_input input;
    memset(&input, 0, sizeof(input));
    input.struct_size = (uint32_t)sizeof(input);
    input.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    input.planes[0] = y;
    input.planes[1] = u;
    input.planes[2] = v;

    const size_t cap = tc_frame_packet_bound(&cfg);
    uint8_t* packet = (uint8_t*)malloc(cap);
    MT_CHECK(packet != NULL);
    if (packet == NULL) { return MT_MAIN_RETURN(); }
    topos_frame_stats stats;
    memset(&stats, 0, sizeof(stats));
    MT_CHECK_EQ_I64(tc_base_frame_encode(&cfg, &input, 16u,
                                         packet, cap, &stats), TC_OK);

    uint16_t out_y[16u * 8u];
    uint16_t out_u[8u * 8u];
    uint16_t out_v[8u * 8u];
    uint16_t* out[TC_FRAME_MAX_PLANES] = {out_y, out_u, out_v, NULL};
    size_t strides[TC_FRAME_MAX_PLANES] = {16u, 8u, 8u, 0u};
    topos_frame_output info;
    MT_CHECK_EQ_I64(tc_base_frame_decode(packet, stats.packet_size, 16u,
                                         out, strides, &info), TC_OK);
    MT_CHECK_EQ_U64(info.visible_width, 16u);
    MT_CHECK_EQ_U64(info.visible_height, 8u);
    MT_CHECK_EQ_U64(info.plane_count, 3u);

    /* The same packet is rejected before output when it is not a permitted
     * base geometry, while the underlying normal decoder remains available. */
    MT_CHECK_EQ_I64(tc_base_frame_decode(packet, stats.packet_size, 8u,
                                         out, strides, &info),
                    TC_ERR_INVALID_ARGUMENT);
    free(packet);
    MT_MAIN_RETURN();
}
