/* RD2-03/04: exercise the internal V7-A writer bridge from real DCT/quant
 * tokens, then decode the resulting packet through the V7-A frame reader. */
#include "codec/codec.h"
#include "bitstream/frame_header.h"
#include "bitstream/layer_directory.h"
#include "bitstream/v7_frame_codec.h"

#include <stdlib.h>
#include <string.h>

#include "mini_test.h"

static void fill_plane(uint16_t* plane, uint32_t width, uint32_t height,
                       uint32_t seed)
{
    for (uint32_t y = 0u; y < height; ++y) {
        for (uint32_t x = 0u; x < width; ++x) {
            plane[(size_t)y * width + x] = (uint16_t)((x * 37u + y * 53u +
                                                       seed * 71u +
                                                       ((x ^ y) * 11u)) & 1023u);
        }
    }
}

static void init_config(topos_frame_config* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->struct_size = (uint32_t)sizeof(*cfg);
    cfg->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg->visible_width = 16u;
    cfg->visible_height = 8u;
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

int main(void)
{
    /* V 代际收纳：回放构建专属测试——运行期放行退役代际（frame_header.c 运行期门） */
#if defined(_WIN32)
    _putenv("TOPOS_DEV=1");
#elif defined(__APPLE__)
    setenv("TOPOS_DEV", "1", 1);
#else
    if (setenv("TOPOS_DEV", "1", 1) != 0) { return 2; }
#endif
    const uint32_t y_width = 16u, uv_width = 8u, height = 8u;
    uint16_t y[y_width * height];
    uint16_t u[uv_width * height];
    uint16_t v[uv_width * height];
    fill_plane(y, y_width, height, 1u);
    fill_plane(u, uv_width, height, 2u);
    fill_plane(v, uv_width, height, 3u);

    topos_frame_config cfg;
    init_config(&cfg);
    topos_frame_input input;
    memset(&input, 0, sizeof(input));
    input.struct_size = (uint32_t)sizeof(input);
    input.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    input.planes[0] = y;
    input.planes[1] = u;
    input.planes[2] = v;
    input.strides[0] = y_width;
    input.strides[1] = uv_width;
    input.strides[2] = uv_width;

    const size_t cap = 2u * 1024u * 1024u;
    uint8_t* v7_packet = (uint8_t*)malloc(cap);
    uint8_t* v2_packet = (uint8_t*)malloc(cap);
    if (v7_packet == NULL || v2_packet == NULL) {
        free(v7_packet);
        free(v2_packet);
        return 2;
    }
    topos_frame_stats v7_stats;
    memset(&v7_stats, 0, sizeof(v7_stats));
    MT_CHECK_EQ_I64(tc_v7_frame_encode(&cfg, &input, v7_packet, cap, &v7_stats), TC_OK);
    MT_CHECK(v7_stats.packet_size > TC_FRAME_HEADER_SIZE);

    topos_frame_header v7_header;
    MT_CHECK_EQ_I64(tc_v7_packet_header_decode(v7_packet, v7_stats.packet_size,
                                               &v7_header), TC_OK);
    MT_CHECK_EQ_U64(v7_header.version_major, 7u);
    tc_v7_directory_view directory;
    MT_CHECK_EQ_I64(tc_v7_directory_parse(v7_packet, v7_stats.packet_size,
                                           &v7_header, &directory), TC_OK);
    MT_CHECK_EQ_U64(directory.slice_count, 3u);
    MT_CHECK_EQ_U64(directory.segment_count, 3u * TC_V7_BAND_COUNT);

    uint16_t v7_y[y_width * height];
    uint16_t v7_u[uv_width * height];
    uint16_t v7_v[uv_width * height];
    uint16_t* v7_planes[TC_FRAME_MAX_PLANES] = { v7_y, v7_u, v7_v, NULL };
    size_t v7_strides[TC_FRAME_MAX_PLANES] = { y_width, uv_width, uv_width, 0u };
    topos_frame_output v7_output;
    tc_v7_decode_stats decode_stats;
    memset(&decode_stats, 0, sizeof(decode_stats));
    MT_CHECK_EQ_I64(tc_v7_frame_decode(v7_packet, v7_stats.packet_size, 4u,
                                       v7_planes, v7_strides, &v7_output,
                                       &decode_stats), TC_OK);
    MT_CHECK_EQ_U64(decode_stats.segments_read, 3u * TC_V7_BAND_COUNT);
    MT_CHECK_EQ_U64(decode_stats.segments_skipped, 0u);

    /* Public V1/V2 entry points now dispatch V7-A packets without changing
     * their legacy behavior. */
    uint16_t public_y[y_width * height];
    uint16_t public_u[uv_width * height];
    uint16_t public_v[uv_width * height];
    uint16_t* public_planes[TC_FRAME_MAX_PLANES] = {
        public_y, public_u, public_v, NULL
    };
    MT_CHECK_EQ_I64(tc_frame_decode(v7_packet, v7_stats.packet_size,
                                    public_planes, v7_strides, &v7_output), TC_OK);
    MT_CHECK(memcmp(public_y, v7_y, sizeof(public_y)) == 0);
    MT_CHECK(memcmp(public_u, v7_u, sizeof(public_u)) == 0);
    MT_CHECK(memcmp(public_v, v7_v, sizeof(public_v)) == 0);

    uint16_t reduced_y[8u * 4u];
    uint16_t reduced_u[4u * 4u];
    uint16_t reduced_v[4u * 4u];
    uint16_t* reduced_planes[TC_FRAME_MAX_PLANES] = {
        reduced_y, reduced_u, reduced_v, NULL
    };
    size_t reduced_strides[TC_FRAME_MAX_PLANES] = { 8u, 4u, 4u, 0u };
    topos_frame_output reduced_output;
    MT_CHECK_EQ_I64(tc_frame_decode_reduced(v7_packet, v7_stats.packet_size,
                                            TC_DECODE_SCALE_HALF,
                                            reduced_planes, reduced_strides,
                                            &reduced_output), TC_OK);
    MT_CHECK_EQ_U64(reduced_output.visible_width, 8u);
    MT_CHECK_EQ_U64(reduced_output.visible_height, 4u);

    topos_decode_request request;
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    request.mode = TC_DECODE_MODE_AUTO_2K;
    request.quality = TC_DECODE_QUALITY_FAST;
    topos_frame_output request_output;
    MT_CHECK_EQ_I64(tc_frame_decode_request(v7_packet, v7_stats.packet_size,
                                             &request, NULL, NULL,
                                             &request_output), TC_OK);
    MT_CHECK_EQ_U64(request_output.visible_width, 16u);
    MT_CHECK_EQ_U64(request_output.visible_height, 8u);

    /* Compare full V7 reconstruction with the plain encoder.
     * （V 代际收纳 2026-09-13：V2-Rice 参考腿 em=2 写域退役，改挂 V1
     * em=0——两代编码路径逐字节一致（stage9 parity 锚），参考等价。） */
    topos_frame_config v2_cfg = cfg;
    v2_cfg.reserved[0] = 0u;
    topos_frame_stats v2_stats;
    memset(&v2_stats, 0, sizeof(v2_stats));
    MT_CHECK_EQ_I64(tc_frame_encode(&v2_cfg, &input, v2_packet, cap, &v2_stats), TC_OK);
    uint16_t ref_y[y_width * height];
    uint16_t ref_u[uv_width * height];
    uint16_t ref_v[uv_width * height];
    uint16_t* ref_planes[TC_FRAME_MAX_PLANES] = { ref_y, ref_u, ref_v, NULL };
    size_t ref_strides[TC_FRAME_MAX_PLANES] = { y_width, uv_width, uv_width, 0u };
    topos_frame_output ref_output;
    MT_CHECK_EQ_I64(tc_frame_decode(v2_packet, v2_stats.packet_size,
                                    ref_planes, ref_strides, &ref_output), TC_OK);
    MT_CHECK(memcmp(v7_y, ref_y, sizeof(ref_y)) == 0);
    MT_CHECK(memcmp(v7_u, ref_u, sizeof(ref_u)) == 0);
    MT_CHECK(memcmp(v7_v, ref_v, sizeof(ref_v)) == 0);

    free(v7_packet);
    free(v2_packet);
    return MT_MAIN_RETURN();
}
