/* ADR-C030 P1-02：pyramid scalar 参考——无量化 round-trip bit-exact、
 * 几何/级联、钳位逃逸路径与参数校验。 */
#include "transform/pyramid_transform.h"

#include "mini_test.h"

#include <stdlib.h>
#include <string.h>

static uint16_t clamp_u16(long v)
{
    if (v < 0) { return 0u; }
    if (v > 65535) { return 65535u; }
    return (uint16_t)v;
}

/* 内容模式：0 flat、1 水平渐变、2 成对极值锯齿（触发钳位逃逸；
 * 纯棋盘的相邻 d 异号相消，update 不会越界）、3 确定性随机 */
static void fill_plane(uint16_t* p, uint32_t w, uint32_t h, int pattern,
                       uint32_t max_value)
{
    for (uint32_t y = 0u; y < h; ++y) {
        for (uint32_t x = 0u; x < w; ++x) {
            uint16_t v = 0u;
            switch (pattern) {
            case 0: v = (uint16_t)(max_value / 2u); break;
            case 1: v = (uint16_t)((x * (uint32_t)max_value) / (w == 1u ? 1u : (w - 1u))); break;
            case 2: v = ((x >> 1) & 1u) ? 0u : (uint16_t)max_value; break;
            default: {
                uint64_t r = mt_rand_u64();
                v = clamp_u16((long)(r % ((uint64_t)max_value + 1u)));
                break;
            }
            }
            p[(size_t)y * w + x] = v;
        }
    }
}

static int roundtrip_case(uint32_t w, uint32_t h, uint8_t bit_depth,
                          tc_pyramid_ratio ratio, int pattern)
{
    const uint32_t max_value = (1u << bit_depth) - 1u;
    uint16_t* src = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
    uint16_t* dst = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
    if (src == NULL || dst == NULL) {
        free(src);
        free(dst);
        return 0;
    }
    fill_plane(src, w, h, pattern, max_value);
    memset(dst, 0, (size_t)w * h * sizeof(uint16_t));
    tc_pyramid_plane pyr;
    memset(&pyr, 0, sizeof(pyr));
    int ok = tc_pyramid_analyze_plane(src, w, w, h, bit_depth, ratio, &pyr) == TC_OK &&
             tc_pyramid_synthesize_plane(&pyr, dst, w) == TC_OK;
    if (ok) {
        for (uint32_t y = 0u; y < h && ok; ++y) {
            for (uint32_t x = 0u; x < w; ++x) {
                if (src[(size_t)y * w + x] != dst[(size_t)y * w + x]) {
                    ok = 0;
                    break;
                }
            }
        }
    }
    tc_pyramid_plane_release(&pyr);
    free(src);
    free(dst);
    return ok;
}

/* --mirror：输出确定性图案的分析产物，供 tests/media 的 Python 镜像
 * （ADR-C030 验收：双实现逐样本一致）逐值比对。图案为纯公式，无 PRNG。 */
static int mirror_case(tc_pyramid_ratio ratio, uint32_t w, uint32_t h,
                       uint8_t bit_depth)
{
    const uint32_t max_value = (1u << bit_depth) - 1u;
    uint16_t* src = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
    if (src == NULL) { return 1; }
    for (uint32_t y = 0u; y < h; ++y) {
        for (uint32_t x = 0u; x < w; ++x) {
            const uint32_t v = (x * 7u + y * 13u + (x * x + y) % 5u) % (max_value + 1u);
            src[(size_t)y * w + x] = (uint16_t)v;
        }
    }
    tc_pyramid_plane pyr;
    memset(&pyr, 0, sizeof(pyr));
    if (tc_pyramid_analyze_plane(src, w, w, h, bit_depth, ratio, &pyr) != TC_OK) {
        free(src);
        return 1;
    }
    printf("CASE %u %u %u %u\n", (unsigned)ratio, (unsigned)w, (unsigned)h,
           (unsigned)bit_depth);
    printf("BASE %u %u\n", (unsigned)pyr.base_width, (unsigned)pyr.base_height);
    for (uint32_t y = 0u; y < pyr.base_height; ++y) {
        for (uint32_t x = 0u; x < pyr.base_width; ++x) {
            printf("B %u %u %u\n", (unsigned)x, (unsigned)y,
                   (unsigned)pyr.base[(size_t)y * pyr.base_width + x]);
        }
    }
    for (uint32_t y = 0u; y < pyr.base_height; ++y) {
        for (uint32_t x = 0u; x < pyr.base_width; ++x) {
            printf("E %u %u %d\n", (unsigned)x, (unsigned)y,
                   pyr.escape[(size_t)y * pyr.base_width + x]);
        }
    }
    for (uint32_t k = 0u; k < pyr.level_count; ++k) {
        for (uint32_t b = 0u; b < pyr.levels[k].band_count; ++b) {
            const tc_pyramid_band* band = &pyr.levels[k].bands[b];
            printf("L %u %u %u %u\n", (unsigned)k, (unsigned)b,
                   (unsigned)band->width, (unsigned)band->height);
            for (uint32_t y = 0u; y < band->height; ++y) {
                for (uint32_t x = 0u; x < band->width; ++x) {
                    printf("C %u %u %u %u %d\n", (unsigned)k, (unsigned)b,
                           (unsigned)x, (unsigned)y,
                           band->coeffs[(size_t)y * band->width + x]);
                }
            }
        }
    }
    printf("END\n");
    tc_pyramid_plane_release(&pyr);
    free(src);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--mirror") == 0) {
        if (mirror_case(TC_PYRAMID_RATIO_6, 25u, 14u, 12u) != 0) { return 1; }
        if (mirror_case(TC_PYRAMID_RATIO_4, 37u, 21u, 10u) != 0) { return 1; }
        if (mirror_case(TC_PYRAMID_RATIO_3, 11u, 8u, 16u) != 0) { return 1; }
        return 0;
    }
    (void)argc;
    (void)argv;
    /* —— 几何/级联表（冻结）—— */
    struct { uint32_t w, h, bw, bh, levels; tc_pyramid_ratio r; } geo[] = {
        {1920u, 1080u, 960u, 540u, 1u, TC_PYRAMID_RATIO_2},
        {3840u, 2160u, 1920u, 1080u, 1u, TC_PYRAMID_RATIO_2},
        {5760u, 3240u, 1920u, 1080u, 1u, TC_PYRAMID_RATIO_3},
        {7680u, 4320u, 1920u, 1080u, 2u, TC_PYRAMID_RATIO_4},
        {11520u, 6480u, 1920u, 1080u, 2u, TC_PYRAMID_RATIO_6},
        /* ragged：允许，按 ceil 规则报告实际几何 */
        {33u, 17u, 17u, 9u, 1u, TC_PYRAMID_RATIO_2},
        {35u, 19u, 12u, 7u, 1u, TC_PYRAMID_RATIO_3},
        {37u, 21u, 10u, 6u, 2u, TC_PYRAMID_RATIO_4},
        {39u, 23u, 7u, 4u, 2u, TC_PYRAMID_RATIO_6},
    };
    for (size_t i = 0u; i < sizeof(geo) / sizeof(geo[0]); ++i) {
        uint32_t bw = 0u, bh = 0u, lv = 0u;
        MT_CHECK_EQ_I64(tc_pyramid_geometry(geo[i].r, geo[i].w, geo[i].h,
                                            &bw, &bh, &lv), TC_OK);
        MT_CHECK_EQ_U64(bw, geo[i].bw);
        MT_CHECK_EQ_U64(bh, geo[i].bh);
        MT_CHECK_EQ_U64(lv, geo[i].levels);
    }
    {
        uint32_t bw, bh, lv;
        MT_CHECK_EQ_I64(tc_pyramid_geometry((tc_pyramid_ratio)5, 64u, 64u,
                                            &bw, &bh, &lv),
                        TC_ERR_INVALID_ARGUMENT);
        MT_CHECK_EQ_I64(tc_pyramid_geometry(TC_PYRAMID_RATIO_2, 0u, 8u,
                                            &bw, &bh, &lv),
                        TC_ERR_INVALID_ARGUMENT);
    }

    /* —— 无量化 bit-exact round-trip ——
     * 尺寸覆盖整除、odd、非 8 对齐、1×1；位深覆盖 10/12/16（Alpha）。 */
    static const struct { uint32_t w, h; } sizes[] = {
        {64u, 64u}, {240u, 136u}, {33u, 17u}, {65u, 47u}, {12u, 12u},
        {5u, 5u}, {3u, 2u}, {1u, 1u}, {2u, 3u},
    };
    static const tc_pyramid_ratio ratios[] = {
        TC_PYRAMID_RATIO_2, TC_PYRAMID_RATIO_3, TC_PYRAMID_RATIO_4,
        TC_PYRAMID_RATIO_6,
    };
    static const uint8_t depths[] = { 10u, 12u, 16u };
    for (size_t si = 0u; si < sizeof(sizes) / sizeof(sizes[0]); ++si) {
        for (size_t ri = 0u; ri < sizeof(ratios) / sizeof(ratios[0]); ++ri) {
            for (size_t di = 0u; di < sizeof(depths) / sizeof(depths[0]); ++di) {
                for (int pattern = 0; pattern < 4; ++pattern) {
                    if (!roundtrip_case(sizes[si].w, sizes[si].h, depths[di],
                                        ratios[ri], pattern)) {
                        MT_CHECK(0);
                        printf("FAIL case w=%u h=%u bd=%u ratio=%d pattern=%d\n",
                               (unsigned)sizes[si].w, (unsigned)sizes[si].h,
                               (unsigned)depths[di], (int)ratios[ri], pattern);
                    }
                }
            }
        }
    }

    /* —— 钳位逃逸路径确实被触发并逐位还原 —— */
    {
        const uint32_t w = 64u;
        const uint32_t h = 16u;
        uint16_t* src = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
        uint16_t* dst = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
        MT_CHECK(src != NULL && dst != NULL);
        fill_plane(src, w, h, 2 /* 成对极值锯齿 */, 1023u);
        tc_pyramid_plane pyr;
        memset(&pyr, 0, sizeof(pyr));
        MT_CHECK_EQ_I64(tc_pyramid_analyze_plane(src, w, w, h, 10u,
                                                 TC_PYRAMID_RATIO_2, &pyr),
                        TC_OK);
        uint32_t escapes = 0u;
        for (uint32_t i = 0u; i < pyr.base_width * pyr.base_height; ++i) {
            if (pyr.escape[i] != 0) { ++escapes; }
        }
        MT_CHECK(escapes > 0u); /* 极值棋盘必然推出采样域 */
        MT_CHECK_EQ_I64(tc_pyramid_synthesize_plane(&pyr, dst, w), TC_OK);
        MT_CHECK(memcmp(src, dst, (size_t)w * h * sizeof(uint16_t)) == 0);
        tc_pyramid_plane_release(&pyr);
        free(src);
        free(dst);
    }

    /* —— 每采样恰好一次的不变量：base + 全部子带 + 逃逸样本数 = 源样本数 —— */
    {
        tc_pyramid_plane pyr;
        memset(&pyr, 0, sizeof(pyr));
        const uint32_t w = 96u;
        const uint32_t h = 66u; /* 3 整除：样本数不变量要求精确分配 */
        uint16_t* src = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
        MT_CHECK(src != NULL);
        fill_plane(src, w, h, 3, 4095u);
        MT_CHECK_EQ_I64(tc_pyramid_analyze_plane(src, w, w, h, 12u,
                                                 TC_PYRAMID_RATIO_6, &pyr),
                        TC_OK);
        uint64_t samples = (uint64_t)pyr.base_width * pyr.base_height;
        for (uint32_t k = 0u; k < pyr.level_count; ++k) {
            for (uint32_t b = 0u; b < pyr.levels[k].band_count; ++b) {
                samples += (uint64_t)pyr.levels[k].bands[b].width *
                           pyr.levels[k].bands[b].height;
            }
        }
        MT_CHECK_EQ_U64(samples, (uint64_t)w * h);
        tc_pyramid_plane_release(&pyr);
        free(src);
    }

    /* —— 参数校验 —— */
    {
        tc_pyramid_plane pyr;
        memset(&pyr, 0, sizeof(pyr));
        uint16_t one = 0u;
        MT_CHECK_EQ_I64(tc_pyramid_analyze_plane(NULL, 1u, 1u, 1u, 10u,
                                                 TC_PYRAMID_RATIO_2, &pyr),
                        TC_ERR_INVALID_ARGUMENT);
        MT_CHECK_EQ_I64(tc_pyramid_analyze_plane(&one, 1u, 1u, 1u, 0u,
                                                 TC_PYRAMID_RATIO_2, &pyr),
                        TC_ERR_INVALID_ARGUMENT);
        MT_CHECK_EQ_I64(tc_pyramid_analyze_plane(&one, 1u, 1u, 1u, 17u,
                                                 TC_PYRAMID_RATIO_2, &pyr),
                        TC_ERR_INVALID_ARGUMENT);
        MT_CHECK_EQ_I64(tc_pyramid_synthesize_plane(NULL, NULL, 0u),
                        TC_ERR_INVALID_ARGUMENT);
    }

    return MT_MAIN_RETURN();
}
