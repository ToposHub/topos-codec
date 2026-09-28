/* 去块滤波（V7-R4 / spec v2）与细化 qp 表（V7-R5）单元测试。
 * 覆盖：表变体一致性/单调性、块台阶平滑（含扩散展开）、**均匀斜坡恒等**
 * （真实梯度的零响应不变式）、真边缘保护、平坦不变、水平/垂直两个方向的
 * 抽头几何、位深钳位、强度随 qp 自适应、非紧致行距、非 8 对齐防御。
 *
 * v2 相对 v1 的两处修正由本测试钉死：
 *  - 水平边界曾误用"同一行内相邻列"作 p1/q1（应为同一列相邻行），台阶
 *    因此降不下去 —— test_horizontal_step_geometry；
 *  - 弱滤波权重由 (4,1) 改为 (12,4)：台阶响应 Δ/2（原 3Δ/16），同时保持
 *    斜坡零响应 —— test_step_vs_ramp。 */
#include "mini_test.h"

#include "codec/deblock.h"
#include "common/tpool.h"
#include "transform/quant.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define W 64u
#define H 64u
#define STRIDE (W + 16u) /* 非紧致行距：捕获行距处理错误 */
#define BD 10u

static void fill_plane(uint16_t* p, uint16_t v)
{
    /* 行距 padding 一并填充：哨兵语义 = 「滤波不得触碰」，需先有确定值 */
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < STRIDE; ++x) {
            p[y * STRIDE + x] = v;
        }
    }
}

static uint16_t at(const uint16_t* p, uint32_t y, uint32_t x)
{
    return p[y * STRIDE + x];
}

/* 左低右高、边界落在 x0（须为 8 的倍数）的垂直台阶。 */
static void make_v_step_at(uint16_t* p, uint16_t lo, uint16_t hi, uint32_t x0)
{
    fill_plane(p, lo);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = x0; x < W; ++x) { p[y * STRIDE + x] = hi; }
    }
}

static void make_v_step(uint16_t* p, uint16_t lo, uint16_t hi)
{
    make_v_step_at(p, lo, hi, 32u);
}

/* 上低下高、边界落在 y=32 的水平台阶。 */
static void make_h_step(uint16_t* p, uint16_t lo, uint16_t hi)
{
    fill_plane(p, lo);
    for (uint32_t y = 32u; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) { p[y * STRIDE + x] = hi; }
    }
}

/* 斜率为 s 的均匀斜坡（真实梯度）——滤波必须逐位恒等。 */
static void make_ramp(uint16_t* p, int32_t base, int32_t s)
{
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < STRIDE; ++x) {
            p[y * STRIDE + x] = (uint16_t)(base + s * (int32_t)x);
        }
    }
}

static void run(uint16_t* p, uint32_t qp)
{
    tc_deblock_plane(p, (int32_t)STRIDE, W, H, qp, TC_QPTBL_CLASSIC, BD);
}

/* ---- 均匀斜坡：零响应（v1/v2 共有不变式，防止滤波器自造伪影） ---- */
static void test_ramp_invariance(void)
{
    uint16_t* plane = (uint16_t*)malloc(sizeof(uint16_t) * STRIDE * H);
    MT_CHECK(plane != NULL);
    if (plane == NULL) { return; }
    for (int32_t s = 1; s <= 4; ++s) {
        make_ramp(plane, 300, s);
        uint16_t before[H * 8];
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < 8u; ++x) {
                before[y * 8u + x] = at(plane, y, 26u + x);
            }
        }
        run(plane, 80u);
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < 8u; ++x) {
                if (at(plane, y, 26u + x) != before[y * 8u + x]) {
                    MT_CHECK(!"ramp must be invariant");
                    fprintf(stderr, "FAIL ramp s=%d y=%u x=%u: %u -> %u\n",
                            (int)s, (unsigned)y, (unsigned)(26u + x),
                            (unsigned)before[y * 8u + x],
                            (unsigned)at(plane, y, 26u + x));
                    free(plane);
                    return;
                }
            }
        }
    }
    free(plane);
}

/* ---- 垂直台阶：跨块界台阶被抹平，且过渡扩散到 p1/q1 ---- */
static void test_vertical_step(void)
{
    uint16_t* plane = (uint16_t*)malloc(sizeof(uint16_t) * STRIDE * H);
    MT_CHECK(plane != NULL);
    if (plane == NULL) { return; }
    make_v_step(plane, 512u, 520u);      /* +8 @ qp80：alpha=18 > 8 */
    run(plane, 80u);
    const uint32_t y0 = 4u;
    /* 边界两侧同为 516（台阶消失）；两侧 p1/q1 被拉开 → 4 样本单调过渡 */
    MT_CHECK_EQ_U64(at(plane, y0, 31u), 516u);
    MT_CHECK_EQ_U64(at(plane, y0, 32u), 516u);
    MT_CHECK_EQ_U64(at(plane, y0, 30u), 514u);
    MT_CHECK_EQ_U64(at(plane, y0, 33u), 518u);
    /* 单调不减（无反向台阶/振铃） */
    for (uint32_t x = 30u; x < 34u; ++x) {
        MT_CHECK(at(plane, y0, x) <= at(plane, y0, x + 1u));
    }
    /* 行距哨兵不被触碰 */
    MT_CHECK_EQ_U64(at(plane, y0, W + 3u), 512u);
    /* 全行覆盖：每个块行都被处理（无遗漏行） */
    for (uint32_t y = 0; y < H; ++y) {
        if (at(plane, y, 31u) == 512u && at(plane, y, 32u) == 520u) {
            MT_CHECK(!"row not filtered");
            fprintf(stderr, "FAIL unfiltered row %u\n", (unsigned)y);
            break;
        }
    }
    free(plane);
}

/* ---- 水平台阶：v1 因抽头几何错误（取同列相邻行才对）此例不收敛 ---- */
static void test_horizontal_step(void)
{
    uint16_t* plane = (uint16_t*)malloc(sizeof(uint16_t) * STRIDE * H);
    MT_CHECK(plane != NULL);
    if (plane == NULL) { return; }
    make_h_step(plane, 512u, 520u);
    run(plane, 80u);
    const uint32_t x0 = 4u;
    MT_CHECK_EQ_U64(at(plane, 31u, x0), 516u);
    MT_CHECK_EQ_U64(at(plane, 32u, x0), 516u);
    MT_CHECK_EQ_U64(at(plane, 30u, x0), 514u);
    MT_CHECK_EQ_U64(at(plane, 33u, x0), 518u);
    for (uint32_t y = 30u; y < 34u; ++y) {
        MT_CHECK(at(plane, y, x0) <= at(plane, y + 1u, x0));
    }
    /* 水平边界之外的行不受影响 */
    MT_CHECK_EQ_U64(at(plane, 20u, x0), 512u);
    MT_CHECK_EQ_U64(at(plane, 40u, x0), 520u);
    free(plane);
}

/* ---- 真边缘（+200 >> alpha）原样保留；平坦不变 ---- */
static void test_edge_and_flat(void)
{
    uint16_t* plane = (uint16_t*)malloc(sizeof(uint16_t) * STRIDE * H);
    MT_CHECK(plane != NULL);
    if (plane == NULL) { return; }
    make_v_step(plane, 512u, 712u);
    run(plane, 80u);
    MT_CHECK_EQ_U64(at(plane, 4u, 31u), 512u);
    MT_CHECK_EQ_U64(at(plane, 4u, 32u), 712u);

    fill_plane(plane, 512u);
    run(plane, 80u);
    for (uint32_t i = 0; i < STRIDE * H; ++i) {
        if (plane[i] != 512u) { MT_CHECK_EQ_U64(plane[i], 512u); break; }
    }
    free(plane);
}

/* ---- 强度随 qp 自适应：+8 台阶在 qp60（小 alpha）不被滤 ---- */
static void test_qp_adaptivity(void)
{
    uint16_t* plane = (uint16_t*)malloc(sizeof(uint16_t) * STRIDE * H);
    MT_CHECK(plane != NULL);
    if (plane == NULL) { return; }
    make_v_step(plane, 512u, 520u);
    run(plane, 60u);
    MT_CHECK_EQ_U64(at(plane, 4u, 31u), 512u);
    MT_CHECK_EQ_U64(at(plane, 4u, 32u), 520u);
    free(plane);
}

/* ---- 位深钳位：接近满幅的台阶不得写出 bit_depth 以上样本 ---- */
static void test_bit_depth_clamp(void)
{
    uint16_t* plane = (uint16_t*)malloc(sizeof(uint16_t) * STRIDE * H);
    MT_CHECK(plane != NULL);
    if (plane == NULL) { return; }
    make_v_step(plane, 1015u, 1023u);
    run(plane, 92u);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            if (at(plane, y, x) > 1023u) {
                MT_CHECK(!"sample exceeds bit depth");
                fprintf(stderr, "FAIL clamp y=%u x=%u v=%u\n", (unsigned)y,
                        (unsigned)x, (unsigned)at(plane, y, x));
                free(plane);
                return;
            }
        }
    }
    /* 暗端同理 */
    make_v_step(plane, 0u, 8u);
    run(plane, 92u);
    for (uint32_t i = 0; i < STRIDE * H; ++i) {
        if (plane[i] > 1023u) { MT_CHECK(!"dark-side clamp"); break; }
    }
    free(plane);
}

/* ---- 非紧致行距 + 非 8 对齐几何：无操作防御 ---- */
static void test_geometry_guard(void)
{
    uint16_t* plane = (uint16_t*)malloc(sizeof(uint16_t) * STRIDE * H);
    MT_CHECK(plane != NULL);
    if (plane == NULL) { return; }
    fill_plane(plane, 512u);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 31u; x < W; ++x) { plane[y * STRIDE + x] = 600u; }
    }
    tc_deblock_plane(plane, (int32_t)STRIDE, W - 1u, H, 80u,
                     TC_QPTBL_CLASSIC, BD);
    tc_deblock_plane(plane, (int32_t)STRIDE, W, H - 7u, 80u,
                     TC_QPTBL_CLASSIC, BD);
    MT_CHECK_EQ_U64(at(plane, 4u, 30u), 512u);
    MT_CHECK_EQ_U64(at(plane, 4u, 31u), 600u);
    /* 非法位深：整段跳过 */
    tc_deblock_plane(plane, (int32_t)STRIDE, W, H, 80u, TC_QPTBL_CLASSIC, 4u);
    MT_CHECK_EQ_U64(at(plane, 4u, 31u), 600u);
    free(plane);
}

/* ---- 门控的符号对称性（v2 回归钉）----
 * v2 门控以 (uint32_t)v < t 代替 |v| < t，而该等价式只在 v ≥ 0 时成立，
 * 于是反向台阶（q0 < p0）整片漏滤：同一幅度、同一 qp，左高右低的台阶被
 * 原样保留。用例取 ±4 小台阶（qp80 下 alpha=18 > 4，必过门控），两侧输出
 * 必须互为镜像（同一整数算式取相反符号），故可逐位钉死。 */
static void test_gate_sign_symmetry(void)
{
    uint16_t* plane = (uint16_t*)malloc(sizeof(uint16_t) * STRIDE * H);
    MT_CHECK(plane != NULL);
    if (plane == NULL) { return; }
    const uint32_t y0 = 4u;

    /* 左低右高：Δ = +4 → p0/q0 合流到 402、p1/q1 铺开成 4 样本过渡 */
    make_v_step(plane, 400u, 404u);
    run(plane, 80u);
    MT_CHECK_EQ_U64(at(plane, y0, 30u), 401u);
    MT_CHECK_EQ_U64(at(plane, y0, 31u), 402u);
    MT_CHECK_EQ_U64(at(plane, y0, 32u), 402u);
    MT_CHECK_EQ_U64(at(plane, y0, 33u), 403u);

    /* 左高右低：Δ = −4 → 逐位镜像（buggy v2 下这四个采样点全数不变） */
    make_v_step(plane, 404u, 400u);
    run(plane, 80u);
    MT_CHECK_EQ_U64(at(plane, y0, 30u), 403u);
    MT_CHECK_EQ_U64(at(plane, y0, 31u), 402u);
    MT_CHECK_EQ_U64(at(plane, y0, 32u), 402u);
    MT_CHECK_EQ_U64(at(plane, y0, 33u), 401u);

    /* 水平边界走同一门控：上高下低同理 */
    make_h_step(plane, 404u, 400u);
    run(plane, 80u);
    MT_CHECK_EQ_U64(at(plane, 30u, 4u), 403u);
    MT_CHECK_EQ_U64(at(plane, 31u, 4u), 402u);
    MT_CHECK_EQ_U64(at(plane, 32u, 4u), 402u);
    MT_CHECK_EQ_U64(at(plane, 33u, 4u), 401u);

    free(plane);
}

/* ---- 缩放交付（1/2 抽样）：目标域周期 4 的滤波 ----
 * 源 8×8 块界在目标域落在 4 的倍数上，故滤波器换成 unit=4 后必须：
 *  (a) 抓住周期 4 的台阶（x0=36 既非 8 的倍数，全尺寸入口完全不动它）；
 *  (b) 均匀斜坡仍零响应（强度公式与网格周期无关）；
 *  (c) 非 unit 对齐/过小抽样比防御性跳过。 */
static void run_scaled(uint16_t* p, uint32_t qp, uint32_t unit)
{
    tc_deblock_plane_scaled(p, (int32_t)STRIDE, W, H, qp, TC_QPTBL_CLASSIC, BD,
                            unit);
}

static void test_scaled_domain(void)
{
    uint16_t* plane = (uint16_t*)malloc(sizeof(uint16_t) * STRIDE * H);
    MT_CHECK(plane != NULL);
    if (plane == NULL) { return; }
    const uint32_t y0 = 4u;

    /* (a) 周期 4 台阶：x0=36，+8 @qp80 → 514/516/516/518 */
    make_v_step_at(plane, 512u, 520u, 36u);
    run_scaled(plane, 80u, 4u);
    MT_CHECK_EQ_U64(at(plane, y0, 34u), 514u);
    MT_CHECK_EQ_U64(at(plane, y0, 35u), 516u);
    MT_CHECK_EQ_U64(at(plane, y0, 36u), 516u);
    MT_CHECK_EQ_U64(at(plane, y0, 37u), 518u);

    /* 全尺寸入口（unit=8）对同一图不动 x=36（周期参数确实生效） */
    make_v_step_at(plane, 512u, 520u, 36u);
    run(plane, 80u);
    MT_CHECK_EQ_U64(at(plane, y0, 35u), 512u);
    MT_CHECK_EQ_U64(at(plane, y0, 36u), 520u);

    /* (b) 均匀斜坡（斜率 s）在目标网格上同样恒等 */
    for (int32_t s = 1; s <= 4; ++s) {
        make_ramp(plane, 300, s);
        uint16_t before[H * 8];
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < 8u; ++x) {
                before[y * 8u + x] = at(plane, y, 30u + x);
            }
        }
        run_scaled(plane, 80u, 4u);
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < 8u; ++x) {
                if (at(plane, y, 30u + x) != before[y * 8u + x]) {
                    MT_CHECK(!"scaled ramp must be invariant");
                    fprintf(stderr, "FAIL scaled ramp s=%d y=%u x=%u\n",
                            (int)s, (unsigned)y, (unsigned)(30u + x));
                    free(plane);
                    return;
                }
            }
        }
    }

    /* (c) 防御：unit<4（窗口重叠）、unit>8、非 unit 对齐几何一律不动 */
    make_v_step_at(plane, 512u, 520u, 36u);
    run_scaled(plane, 80u, 2u);
    MT_CHECK_EQ_U64(at(plane, y0, 36u), 520u);
    run_scaled(plane, 80u, 16u);
    MT_CHECK_EQ_U64(at(plane, y0, 36u), 520u);
    tc_deblock_plane_scaled(plane, (int32_t)STRIDE, W - 1u, H, 80u,
                            TC_QPTBL_CLASSIC, BD, 4u);
    MT_CHECK_EQ_U64(at(plane, y0, 36u), 520u);

    free(plane);
}

/* ---- 并行切片与串行逐位一致 ----
 * 两趟的切片依据是"访问窗口互不相交"（垂直趟按行、水平趟按边界行），因此
 * 任意线程数都必须给出同一结果。平面取 512×512（越过并行分发下限），内容
 * 造得让门控大量触发：8×8 块棋盘 + 块内斜坡，使垂直/水平两个方向都有校正
 * 落地——若切片存在重叠写入，这里就会逐位暴露。 */
static void par_eq_serial(uint32_t unit)
{
    const uint32_t PW = 512u, PH = 512u, PSTR = 512u + 8u;
    const size_t bytes = sizeof(uint16_t) * (size_t)PSTR * PH;
    uint16_t* a = (uint16_t*)malloc(bytes);
    uint16_t* b = (uint16_t*)malloc(bytes);
    uint16_t* c = (uint16_t*)malloc(bytes);
    MT_CHECK(a != NULL && b != NULL && c != NULL);
    if (a == NULL || b == NULL || c == NULL) { free(a); free(b); free(c); return; }
    for (uint32_t y = 0; y < PH; ++y) {
        for (uint32_t x = 0; x < PSTR; ++x) {
            const uint32_t bx = x >> 3, by = y >> 3;
            /* 块间 ±10（必须落在 alpha 内，否则门控全灭 = 用例退化成恒等：
             * 首版用 ±128 棋盘 + ×3 斜坡，实测一个像素都没滤——正是新加的
             * "确有改动"断言抓出来的）。x 向 4 周期 +8 让 unit=4 网格在
             * 非 8 倍数处也有命中。 */
            const int32_t base = ((bx + by) & 1u) ? 512 : 522;
            int32_t v = base + (int32_t)((x & 7u) + (y & 7u))
                        + (int32_t)(((x >> 2) & 1u) * 8);
            if (v > 1023) { v = 1023; }
            a[y * PSTR + x] = (uint16_t)v;
        }
    }
    memcpy(b, a, bytes);
    memcpy(c, a, bytes);

    const int32_t saved = tc_dev_thread_count();
    tc_dev_set_thread_count(1);
    if (unit == 8u) {
        tc_deblock_plane(a, (int32_t)PSTR, PW, PH, 78u, TC_QPTBL_CLASSIC, BD);
    } else {
        tc_deblock_plane_scaled(a, (int32_t)PSTR, PW, PH, 78u, TC_QPTBL_CLASSIC,
                                BD, unit);
    }
    tc_dev_set_thread_count(TC_SLICE_MAX_THREADS);
    if (unit == 8u) {
        tc_deblock_plane(b, (int32_t)PSTR, PW, PH, 78u, TC_QPTBL_CLASSIC, BD);
    } else {
        tc_deblock_plane_scaled(b, (int32_t)PSTR, PW, PH, 78u, TC_QPTBL_CLASSIC,
                                BD, unit);
    }
    tc_dev_set_thread_count(saved);

    /* 滤波确实落了像素（否则用例退化成恒等，掩盖"没跑滤波"） */
    if (memcmp(a, c, bytes) == 0) {
        fprintf(stderr, "FAIL unit=%u: 滤波未落像素（内容门控全灭）\n",
                (unsigned)unit);
    }
    MT_CHECK(memcmp(a, c, bytes) != 0);
    MT_CHECK(memcmp(a, b, bytes) == 0);
    if (memcmp(a, b, bytes) != 0) {
        for (uint32_t y = 0; y < PH; ++y) {
            for (uint32_t x = 0; x < PSTR; ++x) {
                if (a[y * PSTR + x] != b[y * PSTR + x]) {
                    fprintf(stderr, "FAIL parallel!=serial y=%u x=%u %u vs %u\n",
                            (unsigned)y, (unsigned)x,
                            (unsigned)a[y * PSTR + x], (unsigned)b[y * PSTR + x]);
                    free(a); free(b); free(c);
                    return;
                }
            }
        }
    }
    free(a);
    free(b);
    free(c);
}

void test_deblock_main(void)
{
    /* ---- 表变体：经典表 = 旧访问器；细化表 qp≤63 一致、64..95 更细、
     * 单调不减、端点钉死 ---- */
    for (uint32_t q = 0; q <= 95u; ++q) {
        MT_CHECK_EQ_U64(tc_qp_scale_tbl(TC_QPTBL_CLASSIC, q), tc_qp_scale(q));
        if (q <= 63u) {
            MT_CHECK_EQ_U64(tc_qp_scale_tbl(TC_QPTBL_REFINED, q),
                            tc_qp_scale(q));
        } else if (q == 64u) {
            /* 两表同基点（32768） */
            MT_CHECK_EQ_U64(tc_qp_scale_tbl(TC_QPTBL_REFINED, q),
                            tc_qp_scale(q));
        } else {
            MT_CHECK(tc_qp_scale_tbl(TC_QPTBL_REFINED, q)
                     < tc_qp_scale(q));
        }
        if (q > 0u) {
            MT_CHECK(tc_qp_scale_tbl(TC_QPTBL_REFINED, q)
                     >= tc_qp_scale_tbl(TC_QPTBL_REFINED, q - 1u));
        }
    }
    MT_CHECK_EQ_U64(tc_qp_scale_tbl(TC_QPTBL_REFINED, 95u), 1176987u);
    MT_CHECK_EQ_U64(tc_qp_scale(95u), 7053950u);
    MT_CHECK_EQ_U64(tc_qp_scale_tbl(9u, 80u), 0u); /* 未知表选择 → 0（无效） */

    /* step 一致性：refined ctx 与 step_tbl 同源 */
    {
        const uint16_t qm = 16u;
        MT_CHECK_EQ_U64(tc_quant_step_tbl(TC_QPTBL_REFINED, qm, 80u),
                        (uint32_t)((((uint64_t)qm
                                     * tc_qp_scale_tbl(TC_QPTBL_REFINED, 80u))
                                    + 128u) >> 8));
    }

    test_ramp_invariance();
    test_vertical_step();
    test_horizontal_step();
    test_edge_and_flat();
    test_qp_adaptivity();
    test_bit_depth_clamp();
    test_geometry_guard();
    test_gate_sign_symmetry();
    test_scaled_domain();
    par_eq_serial(8u);
    par_eq_serial(4u);
}

int main(void)
{
    test_deblock_main();
    return MT_MAIN_RETURN();
}
