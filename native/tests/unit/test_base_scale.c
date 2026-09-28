/* RD4-01：scalable base 的整数下采样/上采样参考实现与几何契约。 */
#include "transform/base_scale.h"
#include "mini_test.h"

#include <string.h>

int main(void)
{
    uint32_t w = 0u, h = 0u;
    static const struct {
        uint32_t source_w;
        uint32_t source_h;
        uint32_t max_dim;
        uint32_t base_w;
        uint32_t base_h;
    } geometry_cases[] = {
        {1u, 1u, 2048u, 1u, 1u},
        {17u, 9u, 2048u, 17u, 9u},
        {2048u, 2048u, 2048u, 2048u, 2048u},
        {2049u, 2047u, 2048u, 2048u, 2046u},
        {16383u, 8191u, 2048u, 2048u, 1023u},
        {1u, 16384u, 2048u, 1u, 2048u},
        {16384u, 1u, 2048u, 2048u, 1u},
        {16384u, 9216u, 2048u, 2048u, 1152u},
        {3840u, 2160u, 1920u, 1920u, 1080u},
        {6144u, 3456u, 1920u, 1920u, 1080u},
        {7680u, 4320u, 1920u, 1920u, 1080u},
        {11520u, 6480u, 1920u, 1920u, 1080u},
    };
    for (size_t i = 0u; i < sizeof(geometry_cases) / sizeof(geometry_cases[0]); ++i) {
        MT_CHECK(tc_base_scale_dimensions(geometry_cases[i].source_w,
                                          geometry_cases[i].source_h,
                                          geometry_cases[i].max_dim, &w, &h));
        MT_CHECK_EQ_U64(w, geometry_cases[i].base_w);
        MT_CHECK_EQ_U64(h, geometry_cases[i].base_h);
        MT_CHECK(w >= 1u && h >= 1u && w <= geometry_cases[i].max_dim &&
                 h <= geometry_cases[i].max_dim);
    }
    MT_CHECK(tc_base_scale_dimensions(1024u, 512u, 2048u, &w, &h));
    MT_CHECK_EQ_U64(w, 1024u);
    MT_CHECK_EQ_U64(h, 512u);
    MT_CHECK(tc_base_scale_dimensions(3840u, 2160u, 2048u, &w, &h));
    MT_CHECK_EQ_U64(w, 2048u);
    MT_CHECK_EQ_U64(h, 1152u);
    MT_CHECK(tc_base_scale_dimensions(6016u, 3384u, 2048u, &w, &h));
    MT_CHECK_EQ_U64(w, 2048u);
    MT_CHECK(h <= 2048u && h > 0u);
    MT_CHECK(!tc_base_scale_dimensions(0u, 1u, 2048u, &w, &h));
    MT_CHECK(!tc_base_scale_dimensions(1u, 1u, 0u, &w, &h));

    MT_CHECK(tc_base_plane_dimensions(17u, 9u, 0u, 0u,
                                      TC_BASE_CHROMA_LEFT, &w, &h));
    MT_CHECK_EQ_U64(w, 17u);
    MT_CHECK_EQ_U64(h, 9u);
    MT_CHECK(tc_base_plane_dimensions(17u, 9u, 0u, 1u,
                                      TC_BASE_CHROMA_CENTER, &w, &h));
    MT_CHECK_EQ_U64(w, 9u); /* ceil(17/2), 4:2:2 horizontal chroma */
    MT_CHECK_EQ_U64(h, 9u);
    MT_CHECK(tc_base_plane_dimensions(16383u, 1023u, 0u, 1u,
                                      TC_BASE_CHROMA_LEFT, &w, &h));
    MT_CHECK_EQ_U64(w, 8192u); /* odd-width chroma is ceil, never truncate */
    MT_CHECK_EQ_U64(h, 1023u);
    MT_CHECK(tc_base_plane_dimensions(17u, 9u, 1u, 2u,
                                      TC_BASE_CHROMA_TOPLEFT, &w, &h));
    MT_CHECK_EQ_U64(w, 17u);
    MT_CHECK(!tc_base_plane_dimensions(17u, 9u, 2u, 1u,
                                       TC_BASE_CHROMA_CENTER, &w, &h));

    /* 4×4 → 2×2：面积平均的四舍五入（half-up），含 row stride。 */
    const uint16_t source[4][5] = {
        {0u, 1u, 2u, 3u, 0xA55Au},
        {10u, 11u, 12u, 13u, 0xA55Au},
        {20u, 21u, 22u, 23u, 0xA55Au},
        {30u, 31u, 32u, 33u, 0xA55Au}
    };
    uint16_t reduced[2][4];
    for (size_t i = 0u; i < sizeof(reduced) / sizeof(reduced[0][0]); ++i) {
        ((uint16_t*)reduced)[i] = 0xA55Au;
    }
    MT_CHECK(tc_base_downsample_u16(&source[0][0], 4u, 4u, 5u,
                                    &reduced[0][0], 2u, 2u, 4u, 10u));
    const uint16_t reduced_expected[2][2] = {{6u, 8u}, {26u, 28u}};
    for (uint32_t y = 0u; y < 2u; ++y) {
        for (uint32_t x = 0u; x < 2u; ++x) {
            MT_CHECK(reduced[y][x] == reduced_expected[y][x]);
        }
    }
    MT_CHECK_EQ_U64(reduced[0][2], 0xA55Au);
    MT_CHECK_EQ_U64(reduced[0][3], 0xA55Au);
    MT_CHECK_EQ_U64(reduced[1][2], 0xA55Au);
    MT_CHECK_EQ_U64(reduced[1][3], 0xA55Au);

    uint16_t uniform_src[3][5];
    uint16_t uniform_dst[2][4];
    for (uint32_t y = 0u; y < 3u; ++y) {
        for (uint32_t x = 0u; x < 5u; ++x) { uniform_src[y][x] = 4095u; }
    }
    memset(uniform_dst, 0, sizeof(uniform_dst));
    MT_CHECK(tc_base_downsample_u16(&uniform_src[0][0], 5u, 3u, 5u,
                                    &uniform_dst[0][0], 2u, 2u, 4u, 12u));
    for (uint32_t y = 0u; y < 2u; ++y) {
        for (uint32_t x = 0u; x < 2u; ++x) {
            MT_CHECK_EQ_U64(uniform_dst[y][x], 4095u);
        }
    }

    /* The streamed target-sized accumulator remains bit-exact for both an
     * integer 3:1 reduction and a non-integer 15:4 reduction used by 8K→2K. */
    uint16_t exact_src[6][9];
    uint16_t exact_dst[2][3];
    for (uint32_t yy = 0u; yy < 6u; ++yy) {
        for (uint32_t xx = 0u; xx < 9u; ++xx) {
            exact_src[yy][xx] = (uint16_t)(yy * 100u + xx * 3u);
        }
    }
    MT_CHECK(tc_base_downsample_u16(&exact_src[0][0], 9u, 6u, 9u,
                                    &exact_dst[0][0], 3u, 2u, 3u, 10u));
    MT_CHECK_EQ_U64(exact_dst[0][0], 103u);
    MT_CHECK_EQ_U64(exact_dst[0][1], 112u);
    MT_CHECK_EQ_U64(exact_dst[0][2], 121u);
    MT_CHECK_EQ_U64(exact_dst[1][0], 403u);
    MT_CHECK_EQ_U64(exact_dst[1][1], 412u);
    MT_CHECK_EQ_U64(exact_dst[1][2], 421u);

    uint16_t fractional_src[15][15];
    uint16_t fractional_dst[4][4];
    for (uint32_t yy = 0u; yy < 15u; ++yy) {
        for (uint32_t xx = 0u; xx < 15u; ++xx) {
            fractional_src[yy][xx] = (uint16_t)((yy * 17u + xx * 11u) % 1024u);
        }
    }
    MT_CHECK(tc_base_downsample_u16(&fractional_src[0][0], 15u, 15u, 15u,
                                    &fractional_dst[0][0], 4u, 4u, 4u, 10u));
    MT_CHECK_EQ_U64(fractional_dst[0][0], 39u);
    MT_CHECK_EQ_U64(fractional_dst[0][3], 162u);
    MT_CHECK_EQ_U64(fractional_dst[3][0], 230u);
    MT_CHECK_EQ_U64(fractional_dst[3][3], 353u);

    /* 2×2 → 4×4：half-pixel bilinear + edge clamp 的固定 golden。 */
    const uint16_t small[2][2] = {{0u, 100u}, {200u, 300u}};
    uint16_t enlarged[4][5];
    memset(enlarged, 0xA5, sizeof(enlarged));
    MT_CHECK(tc_base_upsample_u16(&small[0][0], 2u, 2u, 2u,
                                  &enlarged[0][0], 4u, 4u, 5u, 16u));
    const uint16_t enlarged_expected[4][4] = {
        {0u, 25u, 75u, 100u},
        {50u, 75u, 125u, 150u},
        {150u, 175u, 225u, 250u},
        {200u, 225u, 275u, 300u}
    };
    for (uint32_t y = 0u; y < 4u; ++y) {
        MT_CHECK(memcmp(enlarged[y], enlarged_expected[y], sizeof(enlarged_expected[y])) == 0);
        MT_CHECK_EQ_U64(enlarged[y][4], 0xA5A5u);
    }
    MT_CHECK(!tc_base_downsample_u16(&source[0][0], 4u, 4u, 5u,
                                     &reduced[0][0], 5u, 2u, 4u, 10u));
    MT_CHECK(!tc_base_upsample_u16(&small[0][0], 2u, 2u, 2u,
                                   &enlarged[0][0], 1u, 4u, 5u, 10u));
    MT_CHECK(!tc_base_downsample_u16(&source[0][0], 4u, 4u, 5u,
                                     &reduced[0][0], 2u, 2u, 4u, 0u));

    MT_MAIN_RETURN();
}
