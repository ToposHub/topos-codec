/* ADR-C030 scalar 参考实现。所 有 公 式 与 ADR-C030 §2 一一对应；
 * 改动这里必须先改 ADR 并升 minor。 */
#include "pyramid_transform.h"

#include <string.h>

#include "../common/alloc.h"
#include "../common/checked.h"
#include "../common/error.h"

/* —— 可移植 floor 运算（C 预先 C23 负数 >> 实现定义，禁止直接移位） —— */

static int32_t pt_floor_div2(int32_t v)
{
    return v >= 0 ? v >> 1 : -((-v + 1) >> 1);
}

static int32_t pt_floor_div4(int32_t v)
{
    return v >= 0 ? v >> 2 : -((-v + 3) >> 2);
}

/* rdiv3(v) = floor((v + 1) / 3)：v/3 四舍五入到 +∞（ADR §2.2 冻结）。 */
static int32_t pt_rdiv3(int32_t v)
{
    const int32_t a = v + 1;
    return a >= 0 ? a / 3 : -((-a + 2) / 3);
}

/* —— 1D 级原语（行/列共用；n 为输入采样数） —— */

static void pt_analyze_f2(const int32_t* x, uint32_t n, int32_t* low, int32_t* d)
{
    const uint32_t dn = n / 2u;
    const uint32_t ln = (n + 1u) / 2u;
    for (uint32_t j = 0u; j < dn; ++j) {
        const int32_t left = x[2u * j];
        const int32_t right = (2u * j + 2u < n) ? x[2u * j + 2u] : left;
        d[j] = x[2u * j + 1u] - pt_floor_div2(left + right);
    }
    for (uint32_t j = 0u; j < ln; ++j) {
        const int32_t dm = (j > 0u) ? d[j - 1u] : 0;
        const int32_t dp = (j < dn) ? d[j] : 0;
        low[j] = x[2u * j] + pt_floor_div4(dm + dp + 2);
    }
}

static void pt_synthesize_f2(const int32_t* low, const int32_t* d, uint32_t n,
                             int32_t* x)
{
    const uint32_t dn = n / 2u;
    const uint32_t ln = (n + 1u) / 2u;
    for (uint32_t j = 0u; j < ln; ++j) {
        const int32_t dm = (j > 0u) ? d[j - 1u] : 0;
        const int32_t dp = (j < dn) ? d[j] : 0;
        x[2u * j] = low[j] - pt_floor_div4(dm + dp + 2);
    }
    /* 全部偶位先重建，奇位预测只依赖偶位 —— 无前向依赖。 */
    for (uint32_t j = 0u; j < dn; ++j) {
        const int32_t left = x[2u * j];
        const int32_t right = (2u * j + 2u < n) ? x[2u * j + 2u] : left;
        x[2u * j + 1u] = d[j] + pt_floor_div2(left + right);
    }
}

static void pt_analyze_f3(const int32_t* x, uint32_t n, int32_t* low,
                          int32_t* d0, int32_t* d1)
{
    const uint32_t ln = (n + 2u) / 3u;
    for (uint32_t i = 0u; i < ln; ++i) {
        /* 尾组：缺失位复制组内最后有效采样（ADR §2.2）。 */
        const int32_t a = x[3u * i];
        const int32_t b = (3u * i + 1u < n) ? x[3u * i + 1u] : a;
        const int32_t c = (3u * i + 2u < n) ? x[3u * i + 2u] : b;
        d0[i] = a - b;
        d1[i] = c - b;
        low[i] = b + pt_rdiv3(d0[i] + d1[i]);
    }
}

static void pt_synthesize_f3(const int32_t* low, const int32_t* d0,
                             const int32_t* d1, uint32_t n, int32_t* x)
{
    const uint32_t ln = (n + 2u) / 3u;
    for (uint32_t i = 0u; i < ln; ++i) {
        const int32_t b = low[i] - pt_rdiv3(d0[i] + d1[i]);
        x[3u * i] = b + d0[i];
        if (3u * i + 1u < n) { x[3u * i + 1u] = b; }
        if (3u * i + 2u < n) { x[3u * i + 2u] = b + d1[i]; }
    }
}

/* —— 级几何 —— */

typedef struct pt_level_geom {
    uint32_t lw;      /* 水平 low 宽 = ceil(w/N) */
    uint32_t lh;      /* 垂直 low 高 = ceil(h/N) */
    uint32_t sdw;     /* 水平 detail 暂存总宽（f2: floor(w/2)；f3: 2*lw） */
    uint32_t band_w[TC_PYRAMID_MAX_BANDS_PER_LEVEL];
    uint32_t band_h[TC_PYRAMID_MAX_BANDS_PER_LEVEL];
    uint32_t band_count;
} pt_level_geom;

static uint32_t pt_ceil_div(uint32_t v, uint32_t d)
{
    return (v + d - 1u) / d;
}

static void pt_level_geometry(uint32_t w, uint32_t h, uint32_t factor,
                              pt_level_geom* g)
{
    if (factor == 2u) {
        const uint32_t dw = w / 2u;
        const uint32_t dh = h / 2u;
        g->lw = pt_ceil_div(w, 2u);
        g->lh = pt_ceil_div(h, 2u);
        g->sdw = dw;
        g->band_count = 3u;
        /* 字典序 (h,v)：LH, HL, HH */
        g->band_w[0] = g->lw; g->band_h[0] = dh;
        g->band_w[1] = dw;    g->band_h[1] = g->lh;
        g->band_w[2] = dw;    g->band_h[2] = dh;
    } else {
        const uint32_t tw = pt_ceil_div(w, 3u);
        const uint32_t th = pt_ceil_div(h, 3u);
        g->lw = tw;
        g->lh = th;
        g->sdw = 2u * tw;
        g->band_count = 8u;
        for (uint32_t b = 0u; b < 8u; ++b) {
            g->band_w[b] = tw;
            g->band_h[b] = th;
        }
    }
}

int32_t tc_pyramid_geometry(tc_pyramid_ratio ratio,
                            uint32_t source_width, uint32_t source_height,
                            uint32_t* base_width, uint32_t* base_height,
                            uint32_t* level_count)
{
    if (base_width == NULL || base_height == NULL || level_count == NULL ||
        source_width == 0u || source_height == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "pyramid geometry arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    uint32_t factors[TC_PYRAMID_MAX_LEVELS];
    uint32_t levels;
    switch (ratio) {
    case TC_PYRAMID_RATIO_2: factors[0] = 2u; levels = 1u; break;
    case TC_PYRAMID_RATIO_3: factors[0] = 3u; levels = 1u; break;
    case TC_PYRAMID_RATIO_4: factors[0] = 2u; factors[1] = 2u; levels = 2u; break;
    case TC_PYRAMID_RATIO_6: factors[0] = 3u; factors[1] = 2u; levels = 2u; break;
    default:
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "pyramid ratio %d", (int)ratio);
        return TC_ERR_INVALID_ARGUMENT;
    }
    uint32_t w = source_width;
    uint32_t h = source_height;
    for (uint32_t k = 0u; k < levels; ++k) {
        const uint32_t f = factors[k];
        w = f == 2u ? pt_ceil_div(w, 2u) : pt_ceil_div(w, 3u);
        h = f == 2u ? pt_ceil_div(h, 2u) : pt_ceil_div(h, 3u);
        if (w == 0u || h == 0u) {
            tc_set_error(TC_ERR_LIMIT_EXCEEDED, "pyramid base geometry");
            return TC_ERR_LIMIT_EXCEEDED;
        }
    }
    *base_width = w;
    *base_height = h;
    *level_count = levels;
    return TC_OK;
}

static void pt_level_factors(tc_pyramid_ratio ratio,
                             uint32_t factors[TC_PYRAMID_MAX_LEVELS])
{
    switch (ratio) {
    case TC_PYRAMID_RATIO_2: factors[0] = 2u; break;
    case TC_PYRAMID_RATIO_3: factors[0] = 3u; break;
    case TC_PYRAMID_RATIO_4: factors[0] = 2u; factors[1] = 2u; break;
    case TC_PYRAMID_RATIO_6: factors[0] = 3u; factors[1] = 2u; break;
    default: factors[0] = 0u; break;
    }
}

/* —— 内存 —— */

static int32_t pt_alloc_i32(uint32_t width, uint32_t height, int32_t** out)
{
    *out = NULL;
    if (width == 0u || height == 0u) { return TC_OK; }
    size_t count = 0u;
    if (!tc_umul_size((size_t)width, (size_t)height, &count) ||
        count > SIZE_MAX / sizeof(int32_t)) {
        return TC_ERR_LIMIT_EXCEEDED;
    }
    *out = (int32_t*)tc_alloc(count * sizeof(int32_t));
    return *out != NULL ? TC_OK : TC_ERR_OUT_OF_MEMORY;
}

static int32_t pt_alloc_bands(const pt_level_geom* g, tc_pyramid_level* level)
{
    for (uint32_t b = 0u; b < g->band_count; ++b) {
        level->bands[b].coeffs = NULL;
        level->bands[b].width = g->band_w[b];
        level->bands[b].height = g->band_h[b];
        const int32_t rc = pt_alloc_i32(g->band_w[b], g->band_h[b],
                                        &level->bands[b].coeffs);
        if (rc != TC_OK) { return rc; }
    }
    level->band_count = g->band_count;
    return TC_OK;
}

/* —— 单级 2D 分析 / 合成 ——

cur(w×h) ──水平──> SL(lw×h) ⊕ SD(sdw×h) ──垂直──> next(lw×lh) ⊕ bands
f2 的 SD 只有 1 列段（d）；f3 的 SD 两个列段（[0,lw)=d0, [lw,2lw)=d1）。 */

static int32_t pt_analyze_level(const int32_t* cur, uint32_t w, uint32_t h,
                                uint32_t factor, tc_pyramid_level* level,
                                int32_t** next_out)
{
    pt_level_geom g;
    pt_level_geometry(w, h, factor, &g);
    int32_t* sl = NULL;
    int32_t* sd = NULL;
    int32_t* next = NULL;
    int32_t rc = pt_alloc_i32(g.lw, h, &sl);
    if (rc == TC_OK) { rc = pt_alloc_i32(g.sdw, h, &sd); }
    if (rc == TC_OK) { rc = pt_alloc_i32(g.lw, g.lh, &next); }
    if (rc == TC_OK) { rc = pt_alloc_bands(&g, level); }
    if (rc != TC_OK) {
        tc_free(sl);
        tc_free(sd);
        tc_free(next);
        return rc;
    }
    level->input_width = w;
    level->input_height = h;
    level->factor = factor;
    level->ll_width = g.lw;
    level->ll_height = g.lh;

    for (uint32_t y = 0u; y < h; ++y) {
        const int32_t* row = cur + (size_t)y * w;
        int32_t* sl_row = sl + (size_t)y * g.lw;
        /* sdw==0（f2 且 w==1）时 pt_alloc_i32 给 NULL——NULL+0 亦 UB
         * （ubsan 修复，2026-09-11）；该几何下无 SD 元素，f2/f3 均不触 */
        int32_t* sd_row = sd != NULL ? sd + (size_t)y * g.sdw : NULL;
        if (factor == 2u) {
            pt_analyze_f2(row, w, sl_row, sd_row);
        } else {
            pt_analyze_f3(row, w, sl_row, sd_row, sd_row + g.lw);
        }
    }

    int32_t* col = (int32_t*)tc_alloc((size_t)h * sizeof(int32_t));
    int32_t* vlow = (int32_t*)tc_alloc((size_t)g.lh * sizeof(int32_t));
    int32_t* vd0 = (int32_t*)tc_alloc((size_t)g.lh * sizeof(int32_t));
    int32_t* vd1 = (int32_t*)tc_alloc((size_t)g.lh * sizeof(int32_t));
    if (col == NULL || vlow == NULL || vd0 == NULL || vd1 == NULL) {
        tc_free(col); tc_free(vlow); tc_free(vd0); tc_free(vd1);
        tc_free(sl); tc_free(sd); tc_free(next);
        return TC_ERR_OUT_OF_MEMORY;
    }
    for (uint32_t c = 0u; c < g.lw && rc == TC_OK; ++c) {
        for (uint32_t y = 0u; y < h; ++y) {
            col[y] = sl[(size_t)y * g.lw + c];
        }
        if (factor == 2u) {
            pt_analyze_f2(col, h, vlow, vd0);
            for (uint32_t y = 0u; y < g.lh; ++y) {
                next[(size_t)y * g.lw + c] = vlow[y];
            }
            for (uint32_t y = 0u; y < h / 2u; ++y) {
                level->bands[0].coeffs[(size_t)y * g.band_w[0] + c] = vd0[y];
            }
        } else {
            pt_analyze_f3(col, h, vlow, vd0, vd1);
            for (uint32_t y = 0u; y < g.lh; ++y) {
                next[(size_t)y * g.lw + c] = vlow[y];
            }
            for (uint32_t y = 0u; y < g.lh; ++y) {
                level->bands[0].coeffs[(size_t)y * g.band_w[0] + c] = vd0[y];
                level->bands[1].coeffs[(size_t)y * g.band_w[1] + c] = vd1[y];
            }
        }
    }
    for (uint32_t c = 0u; c < g.sdw && rc == TC_OK; ++c) {
        for (uint32_t y = 0u; y < h; ++y) {
            col[y] = sd[(size_t)y * g.sdw + c];
        }
        if (factor == 2u) {
            /* SD 单段：垂直 low → band1(HL)，垂直 detail → band2(HH)。 */
            pt_analyze_f2(col, h, vlow, vd0);
            for (uint32_t y = 0u; y < g.lh; ++y) {
                level->bands[1].coeffs[(size_t)y * g.band_w[1] + c] = vlow[y];
            }
            for (uint32_t y = 0u; y < h / 2u; ++y) {
                level->bands[2].coeffs[(size_t)y * g.band_w[2] + c] = vd0[y];
            }
        } else {
            /* SD 两段：列 c 属于水平相位 hp = c / lw + 1。字典序跳过 (L,L)
             * 后，(hp, v) 的子带序 = 3*(hp-1) + 2 + v_idx。 */
            const uint32_t hp = c / g.lw + 1u;
            const uint32_t band_col = c % g.lw; /* 段内列号 */
            pt_analyze_f3(col, h, vlow, vd0, vd1);
            const uint32_t b0 = 3u * (hp - 1u) + 2u; /* (hp, L) */
            const uint32_t b1 = 3u * (hp - 1u) + 3u; /* (hp, e0) */
            const uint32_t b2 = 3u * (hp - 1u) + 4u; /* (hp, e1) */
            for (uint32_t y = 0u; y < g.lh; ++y) {
                level->bands[b0].coeffs[(size_t)y * g.band_w[b0] + band_col] = vlow[y];
                level->bands[b1].coeffs[(size_t)y * g.band_w[b1] + band_col] = vd0[y];
                level->bands[b2].coeffs[(size_t)y * g.band_w[b2] + band_col] = vd1[y];
            }
        }
    }
    tc_free(col);
    tc_free(vlow);
    tc_free(vd0);
    tc_free(vd1);
    tc_free(sl);
    tc_free(sd);
    if (rc != TC_OK) { tc_free(next); return rc; }
    *next_out = next;
    return TC_OK;
}

static void pt_synthesize_level(const tc_pyramid_level* level,
                                const int32_t* next, uint32_t factor,
                                int32_t* cur)
{
    const uint32_t w = level->input_width;
    const uint32_t h = level->input_height;
    pt_level_geom g;
    pt_level_geometry(w, h, factor, &g);
    int32_t* sl = (int32_t*)tc_alloc((size_t)g.lw * h * sizeof(int32_t));
    int32_t* sd = (int32_t*)tc_alloc((size_t)g.sdw * h * sizeof(int32_t));
    int32_t* col = (int32_t*)tc_alloc((size_t)h * sizeof(int32_t));
    int32_t* vlow = (int32_t*)tc_alloc((size_t)g.lh * sizeof(int32_t));
    int32_t* vd0 = (int32_t*)tc_alloc((size_t)g.lh * sizeof(int32_t));
    int32_t* vd1 = (int32_t*)tc_alloc((size_t)g.lh * sizeof(int32_t));
    if (sl == NULL || sd == NULL || col == NULL || vlow == NULL ||
        vd0 == NULL || vd1 == NULL) {
        tc_free(sl); tc_free(sd); tc_free(col);
        tc_free(vlow); tc_free(vd0); tc_free(vd1);
        return; /* 合成侧 OOM 由调用方上层报错路径兜底；参考路径不触及 */
    }
    for (uint32_t c = 0u; c < g.lw; ++c) {
        for (uint32_t y = 0u; y < g.lh; ++y) {
            vlow[y] = next[(size_t)y * g.lw + c];
        }
        if (factor == 2u) {
            for (uint32_t y = 0u; y < h / 2u; ++y) {
                vd0[y] = level->bands[0].coeffs[(size_t)y * g.band_w[0] + c];
            }
            pt_synthesize_f2(vlow, vd0, h, col);
        } else {
            for (uint32_t y = 0u; y < g.lh; ++y) {
                vd0[y] = level->bands[0].coeffs[(size_t)y * g.band_w[0] + c];
                vd1[y] = level->bands[1].coeffs[(size_t)y * g.band_w[1] + c];
            }
            pt_synthesize_f3(vlow, vd0, vd1, h, col);
        }
        for (uint32_t y = 0u; y < h; ++y) {
            sl[(size_t)y * g.lw + c] = col[y];
        }
    }
    for (uint32_t c = 0u; c < g.sdw; ++c) {
        if (factor == 2u) {
            for (uint32_t y = 0u; y < g.lh; ++y) {
                vlow[y] = level->bands[1].coeffs[(size_t)y * g.band_w[1] + c];
            }
            for (uint32_t y = 0u; y < h / 2u; ++y) {
                vd0[y] = level->bands[2].coeffs[(size_t)y * g.band_w[2] + c];
            }
            pt_synthesize_f2(vlow, vd0, h, col);
        } else {
            const uint32_t hp = c / g.lw + 1u;
            const uint32_t band_col = c % g.lw; /* 段内列号 */
            const uint32_t b0 = 3u * (hp - 1u) + 2u;
            const uint32_t b1 = 3u * (hp - 1u) + 3u;
            const uint32_t b2 = 3u * (hp - 1u) + 4u;
            for (uint32_t y = 0u; y < g.lh; ++y) {
                vlow[y] = level->bands[b0].coeffs[(size_t)y * g.band_w[b0] + band_col];
                vd0[y] = level->bands[b1].coeffs[(size_t)y * g.band_w[b1] + band_col];
                vd1[y] = level->bands[b2].coeffs[(size_t)y * g.band_w[b2] + band_col];
            }
            pt_synthesize_f3(vlow, vd0, vd1, h, col);
        }
        for (uint32_t y = 0u; y < h; ++y) {
            sd[(size_t)y * g.sdw + c] = col[y];
        }
    }
    for (uint32_t y = 0u; y < h; ++y) {
        const int32_t* sl_row = sl + (size_t)y * g.lw;
        const int32_t* sd_row = sd != NULL ? sd + (size_t)y * g.sdw : NULL;
        int32_t* row = cur + (size_t)y * w;
        if (factor == 2u) {
            pt_synthesize_f2(sl_row, sd_row, w, row);
        } else {
            pt_synthesize_f3(sl_row, sd_row, sd_row + g.lw, w, row);
        }
    }
    tc_free(sl);
    tc_free(sd);
    tc_free(col);
    tc_free(vlow);
    tc_free(vd0);
    tc_free(vd1);
}

/* —— 公共入口 —— */

int32_t tc_pyramid_analyze_plane(const uint16_t* src, size_t source_stride,
                                 uint32_t source_width, uint32_t source_height,
                                 uint8_t bit_depth, tc_pyramid_ratio ratio,
                                 tc_pyramid_plane* out)
{
    if (src == NULL || out == NULL || source_width == 0u ||
        source_height == 0u || source_stride < (size_t)source_width ||
        bit_depth == 0u || bit_depth > 16u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "pyramid analyze arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    uint32_t base_w = 0u;
    uint32_t base_h = 0u;
    uint32_t level_count = 0u;
    int32_t rc = tc_pyramid_geometry(ratio, source_width, source_height,
                                     &base_w, &base_h, &level_count);
    if (rc != TC_OK) { return rc; }
    uint32_t factors[TC_PYRAMID_MAX_LEVELS];
    pt_level_factors(ratio, factors);

    int32_t* cur = NULL;
    rc = pt_alloc_i32(source_width, source_height, &cur);
    if (rc != TC_OK) { return rc; }
    for (uint32_t y = 0u; y < source_height; ++y) {
        const uint16_t* row = src + (size_t)y * source_stride;
        int32_t* dst_row = cur + (size_t)y * source_width;
        for (uint32_t x = 0u; x < source_width; ++x) {
            dst_row[x] = (int32_t)row[x];
        }
    }
    uint32_t w = source_width;
    uint32_t h = source_height;
    for (uint32_t k = 0u; k < level_count; ++k) {
        int32_t* next = NULL;
        rc = pt_analyze_level(cur, w, h, factors[k], &out->levels[k], &next);
        tc_free(cur);
        if (rc != TC_OK) {
            cur = NULL;
            goto analyze_fail;
        }
        cur = next;
        w = out->levels[k].ll_width;
        h = out->levels[k].ll_height;
    }
    /* 唯一钳位点：最终 base 平面（ADR §2.4）。中间级以 int32 承载不钳位。 */
    out->base = (uint16_t*)tc_alloc((size_t)base_w * base_h * sizeof(uint16_t));
    out->escape = (int32_t*)tc_alloc((size_t)base_w * base_h * sizeof(int32_t));
    if (out->base == NULL || out->escape == NULL) {
        rc = TC_ERR_OUT_OF_MEMORY;
        goto analyze_fail;
    }
    {
        const int32_t max_value = (int32_t)((1u << bit_depth) - 1u);
        for (uint32_t y = 0u; y < base_h; ++y) {
            for (uint32_t x = 0u; x < base_w; ++x) {
                const size_t i = (size_t)y * base_w + x;
                const int32_t raw = cur[i];
                const int32_t clamped = raw < 0 ? 0 : (raw > max_value ? max_value : raw);
                out->base[i] = (uint16_t)clamped;
                out->escape[i] = raw - clamped;
            }
        }
    }
    tc_free(cur);
    out->source_width = source_width;
    out->source_height = source_height;
    out->bit_depth = bit_depth;
    out->base_width = base_w;
    out->base_height = base_h;
    out->level_count = level_count;
    return TC_OK;

analyze_fail:
    tc_free(cur);
    tc_pyramid_plane_release(out);
    return rc;
}

int32_t tc_pyramid_synthesize_plane(const tc_pyramid_plane* pyr,
                                    uint16_t* dst, size_t dst_stride)
{
    if (pyr == NULL || dst == NULL || pyr->base == NULL || pyr->escape == NULL ||
        pyr->level_count == 0u || pyr->level_count > TC_PYRAMID_MAX_LEVELS ||
        pyr->source_width == 0u || pyr->source_height == 0u ||
        dst_stride < (size_t)pyr->source_width) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "pyramid synthesize arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* 每级因子在分析时冻结于 level->factor；极小几何下无法由尺寸反推。 */
    for (uint32_t k = 0u; k < pyr->level_count; ++k) {
        const tc_pyramid_level* lv = &pyr->levels[k];
        if (lv->input_width == 0u || lv->input_height == 0u ||
            lv->ll_width == 0u || lv->ll_height == 0u ||
            (lv->factor != 2u && lv->factor != 3u)) {
            tc_set_error(TC_ERR_MALFORMED, "pyramid level geometry");
            return TC_ERR_MALFORMED;
        }
    }

    int32_t* cur = NULL;
    int32_t rc = pt_alloc_i32(pyr->base_width, pyr->base_height, &cur);
    if (rc != TC_OK) { return rc; }
    for (uint32_t y = 0u; y < pyr->base_height; ++y) {
        for (uint32_t x = 0u; x < pyr->base_width; ++x) {
            const size_t i = (size_t)y * pyr->base_width + x;
            cur[i] = (int32_t)pyr->base[i] + pyr->escape[i];
        }
    }
    for (uint32_t k = pyr->level_count; k-- > 0u; ) {
        const tc_pyramid_level* lv = &pyr->levels[k];
        int32_t* prev = NULL;
        rc = pt_alloc_i32(lv->input_width, lv->input_height, &prev);
        if (rc != TC_OK) { tc_free(cur); return rc; }
        pt_synthesize_level(lv, cur, lv->factor, prev);
        tc_free(cur);
        cur = prev;
    }
    const int32_t max_value = (int32_t)((1u << pyr->bit_depth) - 1u);
    for (uint32_t y = 0u; y < pyr->source_height; ++y) {
        uint16_t* row = dst + (size_t)y * dst_stride;
        const int32_t* src_row = cur + (size_t)y * pyr->source_width;
        for (uint32_t x = 0u; x < pyr->source_width; ++x) {
            const int32_t v = src_row[x];
            row[x] = (uint16_t)(v < 0 ? 0 : (v > max_value ? max_value : v));
        }
    }
    tc_free(cur);
    return TC_OK;
}

void tc_pyramid_plane_release(tc_pyramid_plane* pyr)
{
    if (pyr == NULL) { return; }
    for (uint32_t k = 0u; k < TC_PYRAMID_MAX_LEVELS; ++k) {
        for (uint32_t b = 0u; b < TC_PYRAMID_MAX_BANDS_PER_LEVEL; ++b) {
            tc_free(pyr->levels[k].bands[b].coeffs);
            pyr->levels[k].bands[b].coeffs = NULL;
        }
    }
    tc_free(pyr->base);
    tc_free(pyr->escape);
    pyr->base = NULL;
    pyr->escape = NULL;
}
