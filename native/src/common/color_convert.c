/* P1-12：packed RGB(A) → YUV/GBR 平面（host 侧辅助输入转换）。
 *
 * 逐位一致契约（与 ToposVideoEncoder 的 numpy 参考实现）：
 *   - 全程 float32；标量常数由 double 表达式一次舍入为 float（NEP 50 弱标量）；
 *   - rint 半到偶（= numpy rint = C rintf，默认舍入，本 TU 不改 fenv）；
 *   - 逐算子顺序与 numpy 对应，**禁止 FMA 收缩**——本 TU 以
 *     -ffp-contract=off 编译（CMakeLists set_source_files_properties），
 *     改动运算顺序须同步重跑两侧逐位一致测试；
 *   - 4:2:2 色度对「已量化整型」box（先量化成平面、再子采样的两段式同序）。
 * 本 TU 不分配堆内存（分片任务与参数栈上）。 */

#include "topos_codec.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "color_convert_simd.h"
#include "cpudetect.h"
#include "error.h"
#include "tpool.h"

/* 精度/分支裁剪：mask 上限 32768（产品 4K=3840×2160 远低于此；上限仅为
 * 行索引 size_t 算术与调用方缓冲可界定）。 */
#define TC_CVT_MAX_DIM 32768u
#define TC_CVT_MAX_CHUNKS 128u

typedef struct cvt_ctx {
    const topos_cvt_params* p;
    uint32_t src_stride;  /* 字节/行（0 → w*nch*smp，入口处解析） */
    uint32_t smp;         /* 字节/样本：1（u8）或 2（u16LE） */
    uint32_t nch;         /* 通道数：3 或 4 */
    uint32_t ro, go, bo, ao; /* 通道偏移（样本单位） */
    uint32_t cw;          /* 422 色度宽 = (w+1)/2 */
    uint32_t mode;        /* out_mode */
    uint32_t y_st, u_st, v_st, a_st; /* 输出行距（元素） */
    float    scale;       /* (1<<bd)-1 */
    float    kr, kb;      /* float32 系数（numpy 循环内同值：逐像素乘法） */
    double   krd, kbd;    /* double 原值（numpy 派生常数以 float64 全精度求值） */
    float    kf;          /* (float)(1.0 - kr - kb)（double 求值一次舍入） */
    float    du, dv;      /* (float)(2*(1-kb)) / (float)(2*(1-kr)) */
    float    y_mult, y_off, c_mult, c_off;
    float    a_nmax;      /* (float)(0xFFFF >> alpha_shift) */
    float    norm_div;    /* 255.0f / 65535.0f（除法归一化——与 numpy 同为除法，
                           * 不得改成乘倒数：非精确倒数会丢 bit-exact） */
    uint32_t alpha_shift;
    int      opaque;      /* alpha_opaque */
} cvt_ctx;

static inline float cvt_load(const uint8_t* row, uint32_t byte_off, uint32_t smp)
{
    if (smp == 1u) {
        return (float)row[byte_off];
    }
    /* u16 小端（跨端可移植读取） */
    return (float)((uint32_t)row[byte_off] |
                   ((uint32_t)row[byte_off + 1u] << 8));
}

/* clip(rint(v), 0, scale) → u16（numpy: rint → clip → astype） */
static inline uint16_t cvt_q(float v, float scale)
{
    float r = rintf(v);
    if (r < 0.0f) { r = 0.0f; }
    if (r > scale) { r = scale; }
    return (uint16_t)r;
}

/* 已量化整型的水平 box（numpy: u2 平面 mean(axis)→float 精确 / rint 半到偶）：
 * a+b ≤ 2·4095 在 float32 精确，×0.5f 精确，值只可能是整数或 .5 → 与
 * float64 参考逐位一致。 */
static inline uint16_t cvt_box2(uint16_t a, uint16_t b)
{
    return (uint16_t)rintf(((float)a + (float)b) * 0.5f);
}

static inline float cvt_sample_px(const cvt_ctx* c, const uint8_t* row,
                                  uint32_t ch, uint32_t x)
{
    /* 像素 x 的通道 ch（字节偏移 = (x*nch + ch) * smp） */
    return cvt_load(row, ((uint32_t)x * c->nch + ch) * c->smp, c->smp);
}

static inline uint16_t cvt_alpha(const cvt_ctx* c, const uint8_t* row,
                                 uint32_t x)
{
    if (c->opaque || c->nch == 3u) {
        return (uint16_t)(0xFFFFu << c->alpha_shift); /* a_nmax<<shift */
    }
    const float af = cvt_sample_px(c, row, c->ao, x) / c->norm_div;
    float v = rintf(af * c->a_nmax);
    if (v < 0.0f) { v = 0.0f; }
    if (v > c->a_nmax) { v = c->a_nmax; }
    return (uint16_t)((uint32_t)v << c->alpha_shift);
}

/* 单像素 YUV（u = (b−y)/(2(1−kb))，v = (r−y)/(2(1−kr))，运算序同 numpy） */
static inline void cvt_yuv_pixel(const cvt_ctx* c, const uint8_t* row,
                                 uint32_t x,
                                 uint16_t* y_out, uint16_t* u_out, uint16_t* v_out)
{
    const float rf = cvt_sample_px(c, row, c->ro, x) / c->norm_div;
    const float gf = cvt_sample_px(c, row, c->go, x) / c->norm_div;
    const float bf = cvt_sample_px(c, row, c->bo, x) / c->norm_div;

    /* numpy: y = kr*r + (1-kr-kb)*g + kb*b —— 从左到右两步加法 */
    float y = c->kr * rf + c->kf * gf;
    y = y + c->kb * bf;
    const float u = (bf - y) / c->du;
    const float v = (rf - y) / c->dv;

    *y_out = cvt_q(y * c->y_mult + c->y_off, c->scale);
    *u_out = cvt_q((u + 0.5f) * c->c_mult + c->c_off, c->scale);
    *v_out = cvt_q((v + 0.5f) * c->c_mult + c->c_off, c->scale);
}

static void cvt_rows(const cvt_ctx* c, uint32_t y0, uint32_t y1);

/* ---- P-速③b：u8 族 AVX2 分发（位精确契约见 color_convert_simd.h） ---- */
#if defined(__x86_64__) || defined(_M_X64)
#define TC_CVT_HAVE_SIMD 1
#else
#define TC_CVT_HAVE_SIMD 0
#endif

/* dev 覆盖：-1=自动（CPU 标志）/ 0=强制标量 / 1=强制 AVX2（单测奇偶校验用；
 * 强制 1 在无 AVX2 的机器上未定义——测试侧自行检查 tc_cvt_have_avx2） */
static atomic_int g_cvt_simd_mode = -1;

void tc_dev_cvt_simd_set(int32_t mode)
{
    atomic_store_explicit(&g_cvt_simd_mode, (int)mode, memory_order_relaxed);
}

int tc_cvt_have_avx2(void)
{
#if TC_CVT_HAVE_SIMD
    return (tc_internal_cpu_flags() & TOPOS_CPU_X86_AVX2) != 0;
#else
    return 0;
#endif
}

#if TC_CVT_HAVE_SIMD

/* 行尾标量回退：处理行 y 中 x ≥ x0 的像素（x0 恒为偶数——422 配对安全） */
static void cvt_scalar_tail(void* user, uint32_t y, uint32_t x0)
{
    const cvt_ctx* c = (const cvt_ctx*)user;
    const topos_cvt_params* p = c->p;
    const uint8_t* row = (const uint8_t*)p->src + (size_t)y * c->src_stride;
    uint16_t* yout = p->y_out + (size_t)y * c->y_st;
    uint16_t* uout = p->u_out + (size_t)y * c->u_st;
    uint16_t* vout = p->v_out + (size_t)y * c->v_st;
    const int has_a = (p->a_out != NULL);
    uint16_t* aout = has_a ? (p->a_out + (size_t)y * c->a_st) : NULL;
    const uint32_t w = p->width;

    if (c->mode == TOPOS_CVT_OUT_GBR) {
        for (uint32_t x = x0; x < w; ++x) {
            const float gf = cvt_sample_px(c, row, c->go, x) / c->norm_div;
            const float bf = cvt_sample_px(c, row, c->bo, x) / c->norm_div;
            const float rf = cvt_sample_px(c, row, c->ro, x) / c->norm_div;
            yout[x] = cvt_q(gf * c->scale, c->scale);
            uout[x] = cvt_q(bf * c->scale, c->scale);
            vout[x] = cvt_q(rf * c->scale, c->scale);
            if (has_a) { aout[x] = cvt_alpha(c, row, x); }
        }
    } else if (c->mode == TOPOS_CVT_OUT_YUV444) {
        for (uint32_t x = x0; x < w; ++x) {
            cvt_yuv_pixel(c, row, x, &yout[x], &uout[x], &vout[x]);
            if (has_a) { aout[x] = cvt_alpha(c, row, x); }
        }
    } else {
        /* 422：x0 偶 → 配对边界对齐 */
        for (uint32_t cx = x0 / 2u; cx < c->cw; ++cx) {
            const uint32_t px = cx * 2u;
            uint16_t u0v, v0v, u1v, v1v;
            cvt_yuv_pixel(c, row, px, &yout[px], &u0v, &v0v);
            if (px + 1u < w) {
                cvt_yuv_pixel(c, row, px + 1u, &yout[px + 1u], &u1v, &v1v);
                uout[cx] = cvt_box2(u0v, u1v);
                vout[cx] = cvt_box2(v0v, v1v);
            } else {
                uout[cx] = u0v;
                vout[cx] = v0v;
            }
            if (has_a) { aout[px] = cvt_alpha(c, row, px); }
            if (px + 1u < w && has_a) { aout[px + 1u] = cvt_alpha(c, row, px + 1u); }
        }
    }
}

/* cvt_ctx → SIMD 上下文（字段镜像；float 值原样） */
static void cvt_simd_ctx_fill(const cvt_ctx* c, tc_cvt_simd_ctx* s)
{
    memset(s, 0, sizeof(*s));
    s->src = (const uint8_t*)c->p->src;
    s->src_stride = c->src_stride;
    s->y_out = c->p->y_out;
    s->u_out = c->p->u_out;
    s->v_out = c->p->v_out;
    s->a_out = c->p->a_out;
    s->y_st = c->y_st;
    s->u_st = c->u_st;
    s->v_st = c->v_st;
    s->a_st = c->a_st;
    s->width = c->p->width;
    s->mode = c->mode;
    s->nch = c->nch;
    s->ro = c->ro;
    s->go = c->go;
    s->bo = c->bo;
    s->norm_div = c->norm_div;
    s->kr = c->kr;
    s->kb = c->kb;
    s->kf = c->kf;
    s->du = c->du;
    s->dv = c->dv;
    s->y_mult = c->y_mult;
    s->y_off = c->y_off;
    s->c_mult = c->c_mult;
    s->c_off = c->c_off;
    s->scale = c->scale;
    s->a_nmax = c->a_nmax;
    s->alpha_shift = c->alpha_shift;
    s->opaque = c->opaque;
}

#endif /* TC_CVT_HAVE_SIMD */

static void cvt_rows(const cvt_ctx* c, uint32_t y0, uint32_t y1)
{
#if TC_CVT_HAVE_SIMD
    int mode = atomic_load_explicit(&g_cvt_simd_mode, memory_order_relaxed);
    if (mode < 0) { mode = tc_cvt_have_avx2() ? 1 : 0; }
    if (mode != 0 && c->smp == 1u) {
        tc_cvt_simd_ctx s;
        cvt_simd_ctx_fill(c, &s);
        tc_cvt_rows_u8_avx2(&s, y0, y1, cvt_scalar_tail, (void*)c);
        return;
    }
#endif
    const topos_cvt_params* p = c->p;
    const uint32_t w = p->width;
    const uint32_t src_stride = c->src_stride;
    const uint8_t* base = (const uint8_t*)p->src;

    for (uint32_t y = y0; y < y1; ++y) {
        const uint8_t* row = base + (size_t)y * src_stride;
        uint16_t* yout = p->y_out + (size_t)y * c->y_st;
        uint16_t* uout = p->u_out + (size_t)y * c->u_st;
        uint16_t* vout = p->v_out + (size_t)y * c->v_st;
        const int has_a = (p->a_out != NULL);
        uint16_t* aout = has_a ? (p->a_out + (size_t)y * c->a_st) : NULL;

        if (c->mode == TOPOS_CVT_OUT_GBR) {
            /* G,B,R 直通量化（通道 1,2,0） */
            for (uint32_t x = 0; x < w; ++x) {
                const float gf = cvt_sample_px(c, row, c->go, x) / c->norm_div;
                const float bf = cvt_sample_px(c, row, c->bo, x) / c->norm_div;
                const float rf = cvt_sample_px(c, row, c->ro, x) / c->norm_div;
                yout[x] = cvt_q(gf * c->scale, c->scale);
                uout[x] = cvt_q(bf * c->scale, c->scale);
                vout[x] = cvt_q(rf * c->scale, c->scale);
                if (has_a) { aout[x] = cvt_alpha(c, row, x); }
            }
        } else if (c->mode == TOPOS_CVT_OUT_YUV444) {
            for (uint32_t x = 0; x < w; ++x) {
                cvt_yuv_pixel(c, row, x, &yout[x], &uout[x], &vout[x]);
                if (has_a) { aout[x] = cvt_alpha(c, row, x); }
            }
        } else {
            /* 422：pair 主导——先量化成整型，再对整型 box（同参考两段式） */
            const uint32_t cw = c->cw;
            for (uint32_t cx = 0; cx < cw; ++cx) {
                const uint32_t x0 = cx * 2u;
                uint16_t y0v, u0v, v0v, y1v, u1v, v1v;
                cvt_yuv_pixel(c, row, x0, &y0v, &u0v, &v0v);
                yout[x0] = y0v;
                if (x0 + 1u < w) {
                    cvt_yuv_pixel(c, row, x0 + 1u, &y1v, &u1v, &v1v);
                    yout[x0 + 1u] = y1v;
                    uout[cx] = cvt_box2(u0v, u1v);
                    vout[cx] = cvt_box2(v0v, v1v);
                } else {
                    /* 奇宽边缘补列：mean(a,a) → a（参考实现 pad(edge) 同结果） */
                    uout[cx] = u0v;
                    vout[cx] = v0v;
                }
                if (has_a) { aout[x0] = cvt_alpha(c, row, x0); }
                if (x0 + 1u < w && has_a) { aout[x0 + 1u] = cvt_alpha(c, row, x0 + 1u); }
            }
        }
    }
}

typedef struct cvt_job_arg {
    const cvt_ctx* c;
    uint32_t y0, y1;
} cvt_job_arg;

static void cvt_job_fn(void* arg)
{
    const cvt_job_arg* a = (const cvt_job_arg*)arg;
    cvt_rows(a->c, a->y0, a->y1);
}

static uint32_t cvt_bytes_per_sample(uint32_t format)
{
    return (format >= TOPOS_CVT_BGR48) ? 2u : 1u;
}

static uint32_t cvt_channels(uint32_t format)
{
    switch (format) {
    case TOPOS_CVT_BGRA32:
    case TOPOS_CVT_RGBA32:
    case TOPOS_CVT_BGRA64:
    case TOPOS_CVT_RGBA64:
        return 4u;
    default:
        return 3u;
    }
}

int32_t tc_convert_packed_rgb(const topos_cvt_params* params)
{
    if (params == NULL || params->struct_size != (uint32_t)sizeof(topos_cvt_params)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "cvt: params/struct_size 非法");
        return TC_ERR_INVALID_ARGUMENT;
    }
    const uint32_t fmt = params->format;
    if (fmt > (uint32_t)TOPOS_CVT_RGBA64) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "cvt: format 非法");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (params->out_mode > TOPOS_CVT_OUT_GBR ||
        (params->bit_depth != 10u && params->bit_depth != 12u) ||
        params->width == 0u || params->height == 0u ||
        params->width > TC_CVT_MAX_DIM || params->height > TC_CVT_MAX_DIM ||
        params->src == NULL || params->y_out == NULL ||
        params->u_out == NULL || params->v_out == NULL ||
        params->full_range > 1u || params->alpha_opaque > 1u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "cvt: 参数越界/空指针");
        return TC_ERR_INVALID_ARGUMENT;
    }

    cvt_ctx c;
    memset(&c, 0, sizeof(c));
    c.p = params;
    c.mode = params->out_mode;
    c.smp = cvt_bytes_per_sample(fmt);
    c.nch = cvt_channels(fmt);
    c.cw = (params->width + 1u) / 2u;
    c.opaque = (int)params->alpha_opaque;
    c.alpha_shift = params->alpha_shift > 15u ? 15u : params->alpha_shift;
    c.a_nmax = (float)(0xFFFFu >> c.alpha_shift);
    c.norm_div = (c.smp == 2u) ? 65535.0f : 255.0f;

    /* 通道偏移（样本单位）：BGRx 与 RGBx 两族 + alpha 尾随 */
    switch (fmt) {
    case TOPOS_CVT_BGR24:
    case TOPOS_CVT_BGRA32:
    case TOPOS_CVT_BGR48:
    case TOPOS_CVT_BGRA64:
        c.ro = 2u; c.go = 1u; c.bo = 0u; c.ao = 3u;
        break;
    default:
        c.ro = 0u; c.go = 1u; c.bo = 2u; c.ao = 3u;
        break;
    }

    c.y_st = params->y_stride ? params->y_stride : params->width;
    if (c.mode == TOPOS_CVT_OUT_YUV422) {
        c.u_st = params->u_stride ? params->u_stride : c.cw;
        c.v_st = params->v_stride ? params->v_stride : c.cw;
    } else {
        c.u_st = params->u_stride ? params->u_stride : params->width;
        c.v_st = params->v_stride ? params->v_stride : params->width;
    }
    c.a_st = params->a_stride ? params->a_stride : params->width;

    /* src_stride = 0 → 紧密 w*nch*smp（≤ 32768*4*2 = 256KiB，无溢出） */
    c.src_stride = params->src_stride;
    if (c.src_stride == 0u) {
        c.src_stride = params->width * c.nch * c.smp;
    } else if (c.src_stride < params->width * c.nch * c.smp) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "cvt: src_stride 小于行宽");
        return TC_ERR_INVALID_ARGUMENT;
    }

    /* 系数：double 求值一次舍入（同 numpy 弱标量进入 float32 循环） */
    c.scale = (float)((1u << params->bit_depth) - 1u);
    c.kr = (float)params->kr;
    c.kb = (float)params->kb;
    c.krd = params->kr;
    c.kbd = params->kb;
    if (c.mode != TOPOS_CVT_OUT_GBR) {
        const double krd = params->kr;
        const double kbd = params->kb;
        c.kf = (float)(1.0 - krd - kbd);
        c.du = (float)(2.0 * (1.0 - kbd));
        c.dv = (float)(2.0 * (1.0 - krd));
        if (params->full_range) {
            c.y_mult = c.scale;   c.y_off = 0.0f;
            c.c_mult = c.scale;   c.c_off = 0.0f;
        } else {
            const uint32_t sh = params->bit_depth - 8u;
            c.y_mult = (float)(219u << sh); c.y_off = (float)(16u << sh);
            c.c_mult = (float)(224u << sh); c.c_off = (float)(16u << sh);
        }
    }

    uint32_t nw = params->max_workers;
    if (nw == 0u) { nw = 1u; }
    if (nw > 64u) { nw = 64u; }
    if (nw <= 1u || params->height == 1u) {
        cvt_rows(&c, 0u, params->height);
        return TC_OK;
    }

    uint32_t want = nw * 2u;
    if (want > TC_CVT_MAX_CHUNKS) { want = TC_CVT_MAX_CHUNKS; }
    if (want > params->height) { want = params->height; }
    const uint32_t chunk = (params->height + want - 1u) / want;

    cvt_job_arg args[TC_CVT_MAX_CHUNKS];
    tc_job jobs[TC_CVT_MAX_CHUNKS];
    uint32_t y = 0u;
    for (uint32_t i = 0; i < want; ++i) {
        uint32_t y1 = y + chunk;
        if (y1 > params->height) { y1 = params->height; }
        args[i].c = &c;
        args[i].y0 = y;
        args[i].y1 = y1;
        jobs[i].fn = cvt_job_fn;
        jobs[i].ctx = &args[i];
        y = y1;
    }
    {
        const uint32_t used = tc_parallel_for(jobs, want, nw);
        if (used == 0u) {
            /* 池不可用（理论不可达：parallel_for 有内联回退）；诚实回退单线程 */
            cvt_rows(&c, 0u, params->height);
        }
    }
    return TC_OK;
}
