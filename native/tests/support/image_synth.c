#include "image_synth.h"

#include <stdlib.h>
#include <string.h>

/* xorshift64*：合成器唯一随机源（平台无关确定性） */
static uint64_t synth_next(uint64_t* s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static uint32_t clamp10(int32_t v)
{
    if (v < 0) { return 0u; }
    if (v > 1023) { return 1023u; }
    return (uint32_t)v;
}

/* R4.1：bit_depth=12 时图案 ×4 进 12-bit 值域（clamp 同步缩放；alpha 恒
 * 16-bit 域不受影响）。10-bit 路径与历史输出逐字节一致。 */
static uint32_t clamp_bd(int32_t v, uint32_t scale)
{
    if (scale == 1u) { return clamp10(v); }
    int64_t s = (int64_t)v * (int64_t)scale;
    if (s < 0) { return 0u; }
    int64_t m = (1ll << 12) - 1; /* 目前仅 10/12 两档 */
    if (s > m) { s = m; }
    return (uint32_t)s;
}

static uint32_t sub_kind(tc_synth_kind kind, uint32_t quadrant)
{
    if (kind != TC_SYNTH_MIXED) { return (uint32_t)kind; }
    switch (quadrant) {
        case 0u: return (uint32_t)TC_SYNTH_FLAT;
        case 1u: return (uint32_t)TC_SYNTH_GRADIENT;
        case 2u: return (uint32_t)TC_SYNTH_GRAIN;
        default: return (uint32_t)TC_SYNTH_DETAIL;
    }
}

static int32_t luma_at(tc_synth_kind kind, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint64_t* s)
{
    uint32_t k = (uint32_t)kind;
    if (kind == TC_SYNTH_MIXED) {
        uint32_t qx = x >= w / 2u ? 1u : 0u;
        uint32_t qy = y >= h / 2u ? 1u : 0u;
        k = sub_kind(kind, qy * 2u + qx);
    }
    switch (k) {
        case TC_SYNTH_FLAT: {
            /* 平场 + 极缓慢行漂移（DC 为主，极低码率形态） */
            uint32_t drift = ((x / 32u) + (y / 32u)) % 3u;
            return (int32_t)(320u + drift * 6u);
        }
        case TC_SYNTH_GRADIENT: {
            /* 双线性梯度（低频结构，块间 DC 连续） */
            int64_t gx = (int64_t)x * 700ll / (int64_t)(w > 1u ? w - 1u : 1u);
            int64_t gy = (int64_t)y * 300ll / (int64_t)(h > 1u ? h - 1u : 1u);
            return (int32_t)(64 + gx + gy);
        }
        case TC_SYNTH_GRAIN: {
            /* 胶片颗粒：低频底 + ±6 噪声（10-bit 真实素材颗粒量级 σ≈3.5；
             * 更极端的爆码压力由 encode_sized 与 fuzz 对抗输入覆盖） */
            uint64_t r = synth_next(s);
            int32_t noise = (int32_t)(r % 13ull) - 6;
            int32_t base = 350 + (int32_t)(((x / 48u) + (y / 48u)) % 4u) * 40;
            return base + noise;
        }
        case TC_SYNTH_DETAIL: {
            /* 高频：8×8 棋盘 + 对角线（AC 能量集中形态） */
            int32_t checker = ((x / 8u) + (y / 8u)) % 2u == 0u ? 180 : 760;
            int32_t diag = ((x + y) % 16u < 2u) ? 200 : 0;
            return checker + diag;
        }
        default:
            return 512;
    }
}

int image_synth_build(const image_synth_cfg* cfg,
                      uint16_t* y, uint16_t* u, uint16_t* v, uint16_t* a)
{
    if (cfg == NULL || cfg->width == 0u || cfg->height == 0u ||
        cfg->kind >= TC_SYNTH_KIND_COUNT) {
        return -1;
    }
    uint32_t w = cfg->width;
    uint32_t h = cfg->height;
    /* R4.2/R4.3：chroma_format≠0（4:4:4/GBR）U/V 全宽；0 保持历史 4:2:2 半宽 */
    uint32_t cw = cfg->chroma_format != 0u ? w : (w + 1u) / 2u;
    uint32_t scale = cfg->bit_depth >= 12u ? 4u : 1u;

    uint64_t sy = cfg->seed ^ 0x4D4F565549564531ull; /* "MOVE1" 异化常量 */
    uint64_t su = cfg->seed ^ 0x5543425732534544ull;
    uint64_t sv = cfg->seed ^ 0x5643425732534544ull;
    uint64_t sa = cfg->seed ^ 0x414C504841323131ull;
    for (uint32_t i = 0u; i < 8u; ++i) {
        (void)synth_next(&sy);
        (void)synth_next(&su);
        (void)synth_next(&sv);
        (void)synth_next(&sa);
    }

    for (uint32_t yy = 0u; yy < h; ++yy) {
        for (uint32_t x = 0u; x < w; ++x) {
            /* luma_at 仅在 GRAIN 象限消耗噪声流；栅格序下仍是 (seed,位置) 的纯函数 */
            int32_t ly = luma_at(cfg->kind, x, yy, w, h, &sy);
            if (y != NULL) { y[(size_t)yy * w + x] = (uint16_t)clamp_bd(ly, scale); }
        }
    }

    for (uint32_t yy = 0u; yy < h; ++yy) {
        for (uint32_t x = 0u; x < cw; ++x) {
            /* chroma：平滑低频 ±缓慢相位差 + 轻噪声（GRAIN 类才加噪） */
            int32_t bu = 512 + (int32_t)((x / 24u) % 5u) * 24 - 48;
            int32_t bv = 512 + (int32_t)((yy / 24u) % 5u) * 20 - 40;
            if (cfg->kind == TC_SYNTH_GRAIN || cfg->kind == TC_SYNTH_MIXED ||
                cfg->kind == TC_SYNTH_DETAIL) {
                bu += (int32_t)(synth_next(&su) % 9ull) - 4;
                bv += (int32_t)(synth_next(&sv) % 9ull) - 4;
            }
            if (u != NULL) { u[(size_t)yy * cw + x] = (uint16_t)clamp_bd(bu, scale); }
            if (v != NULL) { v[(size_t)yy * cw + x] = (uint16_t)clamp_bd(bv, scale); }
        }
    }

    if (a != NULL) {
        for (uint32_t yy = 0u; yy < h; ++yy) {
            for (uint32_t x = 0u; x < w; ++x) {
                /* alpha：大块不透明 + 渐变带 + 硬边缘（锻炼 MED 三分支与游程） */
                uint32_t v;
                if (x < w / 4u) {
                    v = 65535u;
                } else if (x < w / 2u) {
                    v = (uint32_t)((uint64_t)65535ull * (uint64_t)(x - w / 4u) /
                                   (uint64_t)(w / 4u));
                } else if (((x / 8u) + (yy / 8u)) % 2u == 0u) {
                    v = 49152u;
                } else {
                    v = 0u;
                }
                if (cfg->kind == TC_SYNTH_GRAIN || cfg->kind == TC_SYNTH_MIXED) {
                    uint64_t r = synth_next(&sa);
                    uint32_t delta = (uint32_t)(r % 512ull);
                    v = delta > v ? 0u : v - delta;
                }
                a[(size_t)yy * w + x] = (uint16_t)v;
            }
        }
    }
    return 0;
}

int image_synth_alloc(const image_synth_cfg* cfg, int with_alpha,
                      uint16_t** y_out, uint16_t** u_out, uint16_t** v_out,
                      uint16_t** a_out)
{
    *y_out = NULL; *u_out = NULL; *v_out = NULL; *a_out = NULL;
    if (cfg == NULL || cfg->width == 0u || cfg->height == 0u) { return -1; }
    uint32_t cw = cfg->chroma_format != 0u ? cfg->width : (cfg->width + 1u) / 2u;
    *y_out = (uint16_t*)malloc((size_t)cfg->width * cfg->height * sizeof(uint16_t));
    *u_out = (uint16_t*)malloc((size_t)cw * cfg->height * sizeof(uint16_t));
    *v_out = (uint16_t*)malloc((size_t)cw * cfg->height * sizeof(uint16_t));
    uint16_t* a = NULL;
    if (with_alpha) {
        a = (uint16_t*)malloc((size_t)cfg->width * cfg->height * sizeof(uint16_t));
    }
    if (*y_out == NULL || *u_out == NULL || *v_out == NULL ||
        (with_alpha && a == NULL)) {
        free(*y_out); free(*u_out); free(*v_out); free(a);
        *y_out = NULL; *u_out = NULL; *v_out = NULL;
        return -2;
    }
    int rc = image_synth_build(cfg, *y_out, *u_out, *v_out, a);
    if (rc != 0) {
        free(*y_out); free(*u_out); free(*v_out); free(a);
        *y_out = NULL; *u_out = NULL; *v_out = NULL;
        return rc;
    }
    *a_out = a;
    return 0;
}
