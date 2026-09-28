/* test_ctx_ceiling —— order-1 上下文天花板捕获（dev 钩子）一致性测试。
 *
 * 1. tc_ctx_log2 精度：自实现 atanh 级数 vs 硬编码已知值（|err| ≤ 2e-6，
 *    系列截断误差 < 1.5e-6）；
 * 2. 捕获开启（rANS 编码）：slice 计数与 stats 一致；符号计数与 sym_hist
 *    钩子（同一事件源）逐族相等；
 * 3. 条件熵不等式：全部 a1(模型) ≤ a0(同族)（+1e-6 浮点容差）；
 *    自适应 ≤ order-0；
 * 4. pooled 联合总数 = 对应族符号计数（run 族含 EOB）；
 * 5. 确定性：单线程下 reset-重编码，19 累加器逐位一致（折叠顺序固定）；
 * 6. 默认关闭零副作用：不开捕获编码后累加器恒零。 */
#include "codec/codec.h"
#include "codec/ctx_ceiling.h"
#include "common/tpool.h"
#include "image_synth.h"
#include "mini_test.h"
#include "topos_codec.h"

#include <stdlib.h>
#include <string.h>

static void base_cfg(topos_frame_config* c, uint32_t w, uint32_t h, uint32_t qp)
{
    memset(c, 0, sizeof(*c));
    c->struct_size = (uint32_t)sizeof(topos_frame_config);
    c->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    c->visible_width = (uint16_t)w;
    c->visible_height = (uint16_t)h;
    c->qp_base = (uint8_t)qp;
    c->qmatrix_id = 0u;
    c->slice_rows = 4u; /* 192/8=24 块行 → 每 plane 6 band → 多 slice 折叠 */
    c->reserved[0] = 8u; /* rANS2（V7-R2 产品默认；V7-R 随收纳退役后迁移） */
}

static int32_t encode_synth(const topos_frame_config* cfg, tc_synth_kind kind,
                            uint32_t seed, topos_frame_stats* st)
{
    image_synth_cfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.seed = seed;
    sc.width = cfg->visible_width;
    sc.height = cfg->visible_height;
    sc.kind = kind;
    sc.bit_depth = 10u;
    sc.chroma_format = 0u;
    uint16_t *y = NULL, *u = NULL, *v = NULL, *a = NULL;
    if (image_synth_alloc(&sc, 0, &y, &u, &v, &a) != 0) { return TC_ERR_OUT_OF_MEMORY; }
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(topos_frame_input);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    in.planes[0] = y;
    in.planes[1] = u;
    in.planes[2] = v;
    in.planes[3] = a;
    size_t cap = tc_frame_packet_bound(cfg);
    uint8_t* buf = (uint8_t*)malloc(cap != 0u ? cap : 1u);
    int32_t rc = buf == NULL ? TC_ERR_OUT_OF_MEMORY
                             : tc_frame_encode(cfg, &in, buf, cap, st);
    free(buf);
    free(y);
    free(u);
    free(v);
    free(a);
    return rc;
}

static void test_log2_accuracy(void)
{
    static const struct { double x, want; } cases[] = {
        {1.0, 0.0},           {2.0, 1.0},
        {3.0, 1.5849625007211562}, {4.0, 2.0},
        {10.0, 3.321928094887362}, {1024.0, 10.0},
        {1000000.0, 19.931568569324174}, {4294967296.0, 32.0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        double d = tc_ctx_log2(cases[i].x) - cases[i].want;
        if (d < 0.0) { d = -d; }
        MT_CHECK(d <= 2e-6);
    }
}

static void test_capture_consistency(void)
{
    topos_frame_config cfg;
    base_cfg(&cfg, 320u, 192u, 33u);

    tc_dev_ctx_reset();
    tc_dev_symbol_hist_reset();
    tc_dev_ctx_enable(1);
    tc_dev_symbol_hist_enable(1);
    topos_frame_stats st;
    memset(&st, 0, sizeof(st));
    const int32_t rc = encode_synth(&cfg, TC_SYNTH_DETAIL, 0xC036u, &st);
    tc_dev_ctx_enable(0);
    tc_dev_symbol_hist_enable(0);
    MT_CHECK(rc == TC_OK);

    double acc[TC_CTX_ACC_COUNT];
    tc_dev_ctx_get(acc);
    MT_CHECK(acc[TC_CTX_ACC_SLICES] > 0.0);
    MT_CHECK_EQ_U64((uint64_t)acc[TC_CTX_ACC_SLICES], st.slice_count);
    MT_CHECK(acc[TC_CTX_ACC_A0_DC] > 0.0);
    MT_CHECK(acc[TC_CTX_ACC_A0_RUN] > 0.0);
    MT_CHECK(acc[TC_CTX_ACC_A0_LVL] > 0.0);
    MT_CHECK(acc[TC_CTX_ACC_SYM_DC] > 0.0);
    MT_CHECK(acc[TC_CTX_ACC_SYM_RUN] > 0.0);
    MT_CHECK(acc[TC_CTX_ACC_EOB] > 0.0);

    /* 符号计数 vs sym_hist 钩子（同一事件流的两个观测） */
    uint64_t dc[29], run[64], lvl[28];
    tc_dev_symbol_hist_get(dc, run, lvl);
    uint64_t sym_dc = 0u, sym_run = 0u, sym_lvl = 0u;
    for (int i = 0; i < 29; ++i) { sym_dc += dc[i]; }
    for (int i = 0; i < 64; ++i) { sym_run += run[i]; }
    for (int i = 0; i < 28; ++i) { sym_lvl += lvl[i]; }
    MT_CHECK_EQ_U64((uint64_t)acc[TC_CTX_ACC_SYM_DC], sym_dc);
    MT_CHECK_EQ_U64((uint64_t)acc[TC_CTX_ACC_SYM_RUN], sym_run);
    MT_CHECK_EQ_U64((uint64_t)acc[TC_CTX_ACC_SYM_LVL], sym_lvl);
    MT_CHECK_EQ_U64((uint64_t)acc[TC_CTX_ACC_EOB], run[63]);

    /* 条件熵不等式：a1(模型) ≤ a0(同族)；自适应 ≤ order-0 */
    MT_CHECK(acc[TC_CTX_ACC_A1_R1] <= acc[TC_CTX_ACC_A0_RUN] + 1e-6);
    MT_CHECK(acc[TC_CTX_ACC_A1_R2] <= acc[TC_CTX_ACC_A0_RUN] + 1e-6);
    MT_CHECK(acc[TC_CTX_ACC_A1_R3] <= acc[TC_CTX_ACC_A0_RUN] + 1e-6);
    MT_CHECK(acc[TC_CTX_ACC_A1_L1] <= acc[TC_CTX_ACC_A0_LVL] + 1e-6);
    MT_CHECK(acc[TC_CTX_ACC_A1_L2] <= acc[TC_CTX_ACC_A0_LVL] + 1e-6);
    MT_CHECK(acc[TC_CTX_ACC_A1_L3] <= acc[TC_CTX_ACC_A0_LVL] + 1e-6);
    MT_CHECK(acc[TC_CTX_ACC_A1_L4] <= acc[TC_CTX_ACC_A0_LVL] + 1e-6);
    MT_CHECK(acc[TC_CTX_ACC_A1_D1] <= acc[TC_CTX_ACC_A0_DC] + 1e-6);
    MT_CHECK(acc[TC_CTX_ACC_ADAPT_RUN] <= acc[TC_CTX_ACC_A0_RUN] + 1e-6);
    MT_CHECK(acc[TC_CTX_ACC_ADAPT_LVL] <= acc[TC_CTX_ACC_A0_LVL] + 1e-6);
    MT_CHECK(acc[TC_CTX_ACC_ADAPT_DC] <= acc[TC_CTX_ACC_A0_DC] + 1e-6);

    /* pooled 联合总数 = 族符号计数 */
    uint64_t pool[30 * 64];
    uint64_t sum = 0u;
    tc_dev_ctx_pooled_r1(pool);
    for (int i = 0; i < 8 * 64; ++i) { sum += pool[i]; }
    MT_CHECK_EQ_U64(sum, sym_run);
    sum = 0u;
    tc_dev_ctx_pooled_l1(pool);
    for (int i = 0; i < 6 * 28; ++i) { sum += pool[i]; }
    MT_CHECK_EQ_U64(sum, sym_lvl);
    sum = 0u;
    tc_dev_ctx_pooled_l3(pool);
    for (int i = 0; i < 30 * 28; ++i) { sum += pool[i]; }
    MT_CHECK_EQ_U64(sum, sym_lvl);
    sum = 0u;
    tc_dev_ctx_pooled_d1(pool);
    for (int i = 0; i < 5 * 29; ++i) { sum += pool[i]; }
    MT_CHECK_EQ_U64(sum, sym_dc);
}

static void test_determinism_single_thread(void)
{
    tc_dev_set_thread_count(1);
    topos_frame_config cfg;
    base_cfg(&cfg, 256u, 128u, 40u);
    double a[TC_CTX_ACC_COUNT], b[TC_CTX_ACC_COUNT];

    tc_dev_ctx_reset();
    tc_dev_ctx_enable(1);
    MT_CHECK(encode_synth(&cfg, TC_SYNTH_MIXED, 0xC037u, NULL) == TC_OK);
    tc_dev_ctx_enable(0);
    tc_dev_ctx_get(a);

    tc_dev_ctx_reset();
    tc_dev_ctx_enable(1);
    MT_CHECK(encode_synth(&cfg, TC_SYNTH_MIXED, 0xC037u, NULL) == TC_OK);
    tc_dev_ctx_enable(0);
    tc_dev_ctx_get(b);

    MT_CHECK(memcmp(a, b, sizeof(a)) == 0);
}

static void test_disabled_zero_side_effect(void)
{
    topos_frame_config cfg;
    base_cfg(&cfg, 256u, 128u, 36u);
    tc_dev_ctx_reset();
    MT_CHECK(encode_synth(&cfg, TC_SYNTH_GRAIN, 0xC038u, NULL) == TC_OK);
    double acc[TC_CTX_ACC_COUNT];
    tc_dev_ctx_get(acc);
    for (uint32_t i = 0u; i < TC_CTX_ACC_COUNT; ++i) {
        MT_CHECK(acc[i] == 0.0);
    }
}

int main(void)
{
    test_log2_accuracy();
    test_capture_consistency();
    test_determinism_single_thread();
    test_disabled_zero_side_effect();
    return MT_MAIN_RETURN();
}
