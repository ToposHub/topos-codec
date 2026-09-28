/* P1-12：tc_convert_packed_rgb 单元测试。
 *
 * 门：
 *  - 语义常量：白/黑/纯红在 BT.709 limited 10-bit 落在标准 YUV 钉值上；
 *  - GBR 直通 = 通道量化（G,B,R 序）；
 *  - max_workers=1 与 >1 输出逐位一致（行分片不改变结果）；
 *  - 奇宽（edge 补列）、自定义 src_stride、alpha opaque/直通；
 *  - 参数校验：NULL / struct_size / format / bit_depth / out_mode / stride。
 * 逐位一致的真正 oracle 是 Python 侧 numpy 参考
 * （tests/media/test_topos_color_convert.py），此处只测不变量。 */
#include "topos_codec.h"
#include "mini_test.h"

#include "../../src/common/color_convert_simd.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W 34
#define H 9
#define W_ODD 33

static topos_cvt_params base_params(uint8_t* src, uint16_t* y, uint16_t* u,
                                    uint16_t* v, uint16_t* a)
{
    topos_cvt_params p;
    memset(&p, 0, sizeof(p));
    p.struct_size = (uint32_t)sizeof(topos_cvt_params);
    p.format = TOPOS_CVT_BGRA32;
    p.width = W;
    p.height = H;
    p.out_mode = TOPOS_CVT_OUT_YUV422;
    p.bit_depth = 10u;
    p.kr = 0.2126f;
    p.kb = 0.0722f;
    p.full_range = 0u;
    p.src = src;
    p.y_out = y;
    p.u_out = u;
    p.v_out = v;
    p.a_out = a;
    return p;
}

static void test_known_values(void)
{
    /* 白 = (255,255,255,255)（BGRA → B,G,R 字节序；值相同不看序） */
    uint8_t src[W * H * 4];
    uint16_t y[W * H], u[W * ((W + 1) / 2) * 2], v[W * ((W + 1) / 2) * 2],
        a[W * H];
    memset(src, 255, sizeof(src));
    topos_cvt_params p = base_params(src, y, u, v, a);
    MT_CHECK_EQ_I64(tc_convert_packed_rgb(&p), TC_OK);
    /* limited 10-bit：白 Y=940，色度中性 512 */
    MT_CHECK_EQ_U64(y[0], 940u);
    MT_CHECK_EQ_U64(u[0], 512u);
    MT_CHECK_EQ_U64(v[0], 512u);
    /* alpha_shift=0 → 16-bit alpha 语义：a_nmax=65535，白 alpha=65535 */
    MT_CHECK_EQ_U64(a[0], 65535u);

    memset(src, 0, sizeof(src));
    MT_CHECK_EQ_I64(tc_convert_packed_rgb(&p), TC_OK);
    MT_CHECK_EQ_U64(y[0], 64u); /* 黑 = 16<<2 */
    MT_CHECK_EQ_U64(u[0], 512u);
    MT_CHECK_EQ_U64(v[0], 512u);

    /* 纯红（BGRA 字节序 B=0,G=0,R=255,A=255）：BT.709 limited 10-bit——
     * Y=rint(0.2126*876+64)=250；U=rint((−0.11457+0.5)*896+64)=409；
     * V：v=(1−kr)/(2(1−kr))=0.5 → rint((0.5+0.5)*896+64)=960。
     * 整帧同色——4:2:2 色度是 2 像素 box，单像素异色会被邻居平均。 */
    for (uint32_t i = 0; i < sizeof(src); i += 4u) {
        src[i + 0] = 0;
        src[i + 1] = 0;
        src[i + 2] = 255;
        src[i + 3] = 255;
    }
    MT_CHECK_EQ_I64(tc_convert_packed_rgb(&p), TC_OK);
    MT_CHECK_EQ_U64(a[0], 65535u);
    int dy = abs((int)y[0] - 250);
    int du = abs((int)u[0] - 409);
    MT_CHECK(dy <= 1);
    MT_CHECK(du <= 1);
    MT_CHECK_EQ_U64(v[0], 960u);
}

/* 与实现同一浮点求值：clip(rint((x/255)*1023), 0, 1023) */
static uint32_t cvt_expect(uint8_t x)
{
    float v = rintf(((float)x / 255.0f) * 1023.0f);
    if (v < 0.0f) { v = 0.0f; }
    if (v > 1023.0f) { v = 1023.0f; }
    return (uint32_t)v;
}

static void test_gbr_passthrough(void)
{
    uint8_t src[W * H * 3];
    uint16_t y[W * H], u[W * H], v[W * H];
    for (uint32_t i = 0; i < W * H; ++i) {
        src[i * 3 + 0] = (uint8_t)(i * 7 + 1);   /* B */
        src[i * 3 + 1] = (uint8_t)(i * 13 + 2);  /* G */
        src[i * 3 + 2] = (uint8_t)(i * 29 + 3);  /* R */
    }
    topos_cvt_params p = base_params(src, y, u, v, NULL);
    p.format = TOPOS_CVT_BGR24;
    p.out_mode = TOPOS_CVT_OUT_GBR;
    p.bit_depth = 10u;
    MT_CHECK_EQ_I64(tc_convert_packed_rgb(&p), TC_OK);
    for (uint32_t i = 0; i < 128u; ++i) {
        /* GBR：满刻度直通量化，平面序 G,B,R（= y,u,v 槽位）；期望与实现
         * 同一浮点运算（÷255 → ×1023 → rint 半到偶 → clip） */
        MT_CHECK_EQ_U64(y[i], (uint64_t)cvt_expect(src[i * 3 + 1]));
        MT_CHECK_EQ_U64(u[i], (uint64_t)cvt_expect(src[i * 3 + 0]));
        MT_CHECK_EQ_U64(v[i], (uint64_t)cvt_expect(src[i * 3 + 2]));
    }
}

static void run_ref(uint8_t* src, uint16_t* y, uint16_t* u, uint16_t* v,
                    uint16_t* a, uint32_t workers)
{
    topos_cvt_params p = base_params(src, y, u, v, a);
    p.max_workers = workers;
    MT_CHECK_EQ_I64(tc_convert_packed_rgb(&p), TC_OK);
}

static void test_worker_parity_and_odd(void)
{
    uint8_t src[W * H * 4];
    for (uint32_t i = 0; i < sizeof(src); ++i) {
        src[i] = (uint8_t)(i * 31 + i / 7);
    }
    static uint16_t y1[W * H], u1[W * ((W + 1) / 2) * 2],
        v1[W * ((W + 1) / 2) * 2], a1[W * H];
    static uint16_t y2[W * H], u2[W * ((W + 1) / 2) * 2],
        v2[W * ((W + 1) / 2) * 2], a2[W * H];
    run_ref(src, y1, u1, v1, a1, 1u);
    run_ref(src, y2, u2, v2, a2, 4u);
    MT_CHECK(memcmp(y1, y2, sizeof(y1)) == 0);
    MT_CHECK(memcmp(u1, u2, sizeof(u1)) == 0);
    MT_CHECK(memcmp(v1, v2, sizeof(v1)) == 0);
    MT_CHECK(memcmp(a1, a2, sizeof(a1)) == 0);
}

static void test_odd_width_and_stride(void)
{
    enum { OW = W_ODD, OH = 7 };
    const uint32_t cw = (OW + 1) / 2;
    uint8_t* src = (uint8_t*)calloc(OH * (OW + 3) * 4u, 1u);
    uint16_t* y = (uint16_t*)calloc(OH * OW, 2u);
    uint16_t* u = (uint16_t*)calloc(OH * cw, 2u);
    uint16_t* v = (uint16_t*)calloc(OH * cw, 2u);
    MT_CHECK(src && y && u && v);
    for (uint32_t r = 0; r < OH; ++r) {
        for (uint32_t x = 0; x < OW; ++x) {
            uint8_t* px = src + (r * (OW + 3) + x) * 4u;
            px[0] = (uint8_t)(x * 5 + r);
            px[1] = (uint8_t)(x * 3 + r * 11);
            px[2] = (uint8_t)(x * 9 + r * 2 + 1);
            px[3] = 200;
        }
    }
    topos_cvt_params p;
    memset(&p, 0, sizeof(p));
    p.struct_size = (uint32_t)sizeof(topos_cvt_params);
    p.format = TOPOS_CVT_BGRA32;
    p.width = OW;
    p.height = OH;
    p.out_mode = TOPOS_CVT_OUT_YUV422;
    p.bit_depth = 10u;
    p.kr = 0.2126f;
    p.kb = 0.0722f;
    p.src = src;
    p.src_stride = (OW + 3) * 4u;
    p.y_out = y;
    p.u_out = u;
    p.v_out = v;
    p.a_out = NULL;
    MT_CHECK_EQ_I64(tc_convert_packed_rgb(&p), TC_OK);
    /* 奇宽末列 edge 补列 → box(a,a)=a：末列色度必须等于末像素的 444 直通值。
     * 用 1×1 同像素 444 转换独立求出该像素的 u/v 钉值后对拍。 */
    {
        uint8_t px1[4];
        uint16_t y1[1], u1[1], v1[1];
        const uint8_t* last = src + ((OW - 1) * 4u); /* 首行末像素（B,G,R,A） */
        px1[0] = last[0];
        px1[1] = last[1];
        px1[2] = last[2];
        px1[3] = last[3];
        topos_cvt_params q;
        memset(&q, 0, sizeof(q));
        q.struct_size = (uint32_t)sizeof(topos_cvt_params);
        q.format = TOPOS_CVT_BGRA32;
        q.width = 1u;
        q.height = 1u;
        q.out_mode = TOPOS_CVT_OUT_YUV444;
        q.bit_depth = 10u;
        q.kr = 0.2126f;
        q.kb = 0.0722f;
        q.src = px1;
        q.y_out = y1;
        q.u_out = u1;
        q.v_out = v1;
        MT_CHECK_EQ_I64(tc_convert_packed_rgb(&q), TC_OK);
        MT_CHECK_EQ_U64(u[cw - 1], u1[0]);
        MT_CHECK_EQ_U64(v[cw - 1], v1[0]);
    }
    free(src);
    free(y);
    free(u);
    free(v);
}

static void test_param_validation(void)
{
    uint8_t src[W * 4];
    uint16_t y[W], u[W], v[W];
    topos_cvt_params p = base_params(src, y, u, v, NULL);
    p.height = 1;
    p.width = W / 2;
    MT_CHECK_EQ_I64(tc_convert_packed_rgb(NULL), TC_ERR_INVALID_ARGUMENT);
    p.struct_size = 0;
    MT_CHECK_EQ_I64(tc_convert_packed_rgb(&p), TC_ERR_INVALID_ARGUMENT);
    p = base_params(src, y, u, v, NULL);
    p.format = 99u;
    MT_CHECK_EQ_I64(tc_convert_packed_rgb(&p), TC_ERR_INVALID_ARGUMENT);
    p = base_params(src, y, u, v, NULL);
    p.bit_depth = 8u;
    MT_CHECK_EQ_I64(tc_convert_packed_rgb(&p), TC_ERR_INVALID_ARGUMENT);
    p = base_params(src, y, u, v, NULL);
    p.out_mode = 7u;
    MT_CHECK_EQ_I64(tc_convert_packed_rgb(&p), TC_ERR_INVALID_ARGUMENT);
    p = base_params(src, y, u, v, NULL);
    p.src = NULL;
    MT_CHECK_EQ_I64(tc_convert_packed_rgb(&p), TC_ERR_INVALID_ARGUMENT);
    p = base_params(src, y, u, v, NULL);
    p.src_stride = 1u; /* 小于行宽 */
    MT_CHECK_EQ_I64(tc_convert_packed_rgb(&p), TC_ERR_INVALID_ARGUMENT);
}


/* P-速③b：AVX2 内核 vs 标量逐位奇偶校验（u8 输入族全格式 × 全模式 ×
 * 奇偶宽 × 位深/范围/alpha 变体）。强制切换经 tc_dev_cvt_simd_set；
 * 结束恢复自动。真正 oracle 仍是 Python numpy 参考测试（本机 AVX2 上
 * 自动走新内核）。 */
static void test_avx2_scalar_parity(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    if (!tc_cvt_have_avx2()) { return; }

    static const uint32_t kFormats[] = {
        TOPOS_CVT_BGR24, TOPOS_CVT_RGB24, TOPOS_CVT_BGRA32, TOPOS_CVT_RGBA32
    };
    static const uint32_t kWidths[] = { 34u, 33u, 40u, 8u, 1u };
    static const uint32_t kModes[] = {
        TOPOS_CVT_OUT_YUV422, TOPOS_CVT_OUT_YUV444, TOPOS_CVT_OUT_GBR
    };
    static const double kMatrices[][2] = {
        { 0.2126, 0.0722 }, { 0.299, 0.114 }, { 0.2627, 0.0593 },
    };

    /* 确定性数据：全值域扫描 + LCG 混合 + rint 边界值（x.5 触发器） */
    uint8_t src[40 * 9 * 4];
    {
        uint32_t st = 12345u;
        for (uint32_t i = 0; i < sizeof(src); ++i) {
            st = st * 1664525u + 1013904223u;
            uint32_t r = (st >> 16) & 0xFFu;
            switch (i % 5u) {
            case 0: r = (uint8_t)(i * 37u); break;      /* 全值域扫描 */
            case 1: r = 0u; break;                      /* 边界 0 */
            case 2: r = 255u; break;                    /* 边界 255 */
            case 3: r = 127u + (i % 2u); break;         /* 127/128 中点 */
            default: break;                             /* LCG */
            }
            src[i] = (uint8_t)r;
        }
    }

    uint32_t cases = 0, diffs = 0;
    for (uint32_t fi = 0; fi < 4u; ++fi) {
        const uint32_t fmt = kFormats[fi];
        for (uint32_t wi = 0; wi < 5u; ++wi) {
            const uint32_t w = kWidths[wi];
            const uint32_t cw = (w + 1u) / 2u;
            for (uint32_t mi = 0; mi < 3u; ++mi) {
                for (uint32_t full = 0; full < 2u; ++full) {
                    for (uint32_t bd = 10u; bd <= 12u; bd += 2u) {
                        for (uint32_t a_off = 0; a_off < 2u; ++a_off) {
                            uint16_t y0[40 * 9], u0[40 * 9], v0[40 * 9],
                                a0[40 * 9];
                            uint16_t y1[40 * 9], u1[40 * 9], v1[40 * 9],
                                a1[40 * 9];
                            topos_cvt_params p;
                            memset(&p, 0, sizeof(p));
                            p.struct_size = (uint32_t)sizeof(p);
                            p.format = fmt;
                            p.width = w;
                            p.height = 9u;
                            p.out_mode = kModes[mi];
                            p.bit_depth = bd;
                            p.kr = kMatrices[(fi + wi) % 3u][0];
                            p.kb = kMatrices[(fi + wi) % 3u][1];
                            p.full_range = full;
                            p.src = src;
                            p.y_out = y0; p.u_out = u0; p.v_out = v0;
                            p.a_out = a_off ? a0 : NULL;
                            p.alpha_shift = 4u;
                            p.max_workers = 1u;

                            tc_dev_cvt_simd_set(0);
                            MT_CHECK_EQ_I64(tc_convert_packed_rgb(&p), TC_OK);
                            p.y_out = y1; p.u_out = u1; p.v_out = v1;
                            p.a_out = a_off ? a1 : NULL;
                            tc_dev_cvt_simd_set(1);
                            MT_CHECK_EQ_I64(tc_convert_packed_rgb(&p), TC_OK);
                            tc_dev_cvt_simd_set(-1);
                            ++cases;
                            if (memcmp(y0, y1, sizeof(y0)) != 0 ||
                                memcmp(u0, u1, sizeof(u0)) != 0 ||
                                memcmp(v0, v1, sizeof(v0)) != 0) { ++diffs; }
                            if (a_off && memcmp(a0, a1, sizeof(a0)) != 0) {
                                ++diffs;
                            }
                        }
                    }
                }
            }
                    }
    }
    MT_CHECK_EQ_U64(diffs, 0u);
    (void)cases;
#endif
}

int main(void)
{
    test_known_values();
    test_avx2_scalar_parity();
    test_gbr_passthrough();
    test_worker_parity_and_odd();
    test_odd_width_and_stride();
    test_param_validation();
    MT_MAIN_RETURN();
}
