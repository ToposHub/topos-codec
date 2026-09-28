/* C036 前置测量：order-1 上下文天花板捕获（dev-only；见 ctx_ceiling.h）。
 *
 * 数值约定：计数以 double 参与位计算（u64 计数 < 2^53 在 double 中精确）；
 * tc_ctx_log2 为自包含实现（atanh 级数），避免给 topos_codec 引入 libm
 * 链接面变化。折叠在 worker 线程发生，经本模块私有互斥锁串行化。 */
#include "ctx_ceiling.h"

#include "../common/alloc.h"
#include "../common/port_mutex.h"
#include "../entropy/vlc.h"

#include <stdatomic.h>
#include <string.h>

/* ---- 上下文域常量（与头文件文档一致） ---- */
#define R1_CTX 8u
#define R2_CTX 7u
#define R3_CTX 4u
#define L1_CTX 6u
#define L2_CTX 5u
#define L3_CTX (L1_CTX * L2_CTX) /* 30 */
#define L4_CTX 4u
#define D1_CTX 5u
#define DC_SYMS 29u
#define RUN_SYMS 64u
#define LVL_SYMS 28u

struct tc_ctx_cap {
    uint64_t r1[R1_CTX * RUN_SYMS];
    uint64_t r2[R2_CTX * RUN_SYMS];
    uint64_t r3[R3_CTX * RUN_SYMS];
    uint64_t l1[L1_CTX * LVL_SYMS];
    uint64_t l2[L2_CTX * LVL_SYMS];
    uint64_t l3[L3_CTX * LVL_SYMS];
    uint64_t l4[L4_CTX * LVL_SYMS];
    uint64_t d1[D1_CTX * DC_SYMS];
    uint32_t cur_dcb;     /* 本块 DC 类桶（0..3） */
    uint32_t prev_dcb;    /* 前块 DC 类桶（0..3）；首块无前块 */
    uint32_t prev_runb;   /* 前一对 run 桶；6 = 块首 */
    uint32_t prev_lvlb;   /* 前一对 lvl 类桶；4 = 块首 */
    uint32_t blocks_seen; /* slice 内已见块数（D1 首块判别） */
};

static inline uint32_t prev_pos_b(uint32_t p)
{
    if (p == 0u) { return 0u; }
    const uint32_t b = 1u + ((p - 1u) >> 3);
    return b > 7u ? 7u : b; /* prev 56..63 → 7 */
}

static inline uint32_t run_b(uint32_t r)
{
    if (r <= 1u) { return r; }
    if (r < 4u) { return 2u; }
    if (r < 8u) { return 3u; }
    if (r < 16u) { return 4u; }
    return 5u;
}

static inline uint32_t lvlcat_b(uint32_t cat)
{
    if (cat <= 1u) { return cat; }
    return cat <= 3u ? 2u : 3u;
}

static inline uint32_t pos_b(uint32_t pos)
{
    if (pos < 8u) { return 0u; }
    if (pos < 16u) { return 1u; }
    if (pos < 32u) { return 2u; }
    return 3u;
}

/* ---- 自包含 log2：指数位提取 + atanh 级数（z ≤ 1/3，截至 z¹³） ---- */
double tc_ctx_log2(double x)
{
    uint64_t bits = 0u;
    memcpy(&bits, &x, sizeof(bits));
    const int e = (int)((bits >> 52) & UINT64_C(0x7ff)) - 1023;
    bits = (bits & ~(UINT64_C(0x7ff) << 52)) | (UINT64_C(1023) << 52);
    double m = 0.0;
    memcpy(&m, &bits, sizeof(m)); /* m ∈ [1,2)，x ≥ 1 保证规格化 */
    const double z = (m - 1.0) / (m + 1.0);
    const double z2 = z * z;
    const double at = z * (1.0 + z2 * (1.0 / 3.0 + z2 * (1.0 / 5.0
        + z2 * (1.0 / 7.0 + z2 * (1.0 / 9.0 + z2 * (1.0 / 11.0
        + z2 * (1.0 / 13.0)))))));
    return (double)e + 2.885390081777926814 * at; /* 2/ln2 */
}

/* N·log2 N − Σ c·log2 c（族直方图；全零返回 0） */
static double fam_bits(const uint32_t* h, uint32_t n)
{
    uint64_t total = 0u;
    for (uint32_t i = 0u; i < n; ++i) { total += h[i]; }
    if (total == 0u) { return 0.0; }
    double b = (double)total * tc_ctx_log2((double)total);
    for (uint32_t i = 0u; i < n; ++i) {
        if (h[i] != 0u) { b -= (double)h[i] * tc_ctx_log2((double)h[i]); }
    }
    return b;
}

/* 联合版：Σ_ctx [N_c·log2 N_c − Σ c·log2 c] */
static double joint_bits(const uint64_t* j, uint32_t ctx_n, uint32_t sym_n)
{
    double b = 0.0;
    for (uint32_t ci = 0u; ci < ctx_n; ++ci) {
        const uint64_t* row = j + (size_t)ci * sym_n;
        uint64_t nc = 0u;
        for (uint32_t s = 0u; s < sym_n; ++s) { nc += row[s]; }
        if (nc == 0u) { continue; }
        b += (double)nc * tc_ctx_log2((double)nc);
        for (uint32_t s = 0u; s < sym_n; ++s) {
            if (row[s] != 0u) { b -= (double)row[s] * tc_ctx_log2((double)row[s]); }
        }
    }
    return b;
}

static double mind(double a, double b)
{
    return b < a ? b : a;
}

/* ---- 全局累加（私有互斥锁保护；折叠频率 = 每 slice 一次） ---- */
static atomic_int s_ctx_on = 0;
static topos_mutex s_ctx_mu = TOPOS_MUTEX_INIT;
static double g_acc[TC_CTX_ACC_COUNT];
static uint64_t g_r1[R1_CTX * RUN_SYMS];
static uint64_t g_l1[L1_CTX * LVL_SYMS];
static uint64_t g_l3[L3_CTX * LVL_SYMS];
static uint64_t g_d1[D1_CTX * DC_SYMS];

tc_ctx_cap* tc_ctx_cap_alloc(void)
{
    tc_ctx_cap* c = (tc_ctx_cap*)tc_alloc(sizeof(tc_ctx_cap));
    if (c != NULL) {
        memset(c, 0, sizeof(*c));
        c->prev_runb = 6u;
        c->prev_lvlb = 4u;
    }
    return c;
}

void tc_ctx_cap_free(tc_ctx_cap* c)
{
    tc_free(c);
}

void tc_ctx_dc(tc_ctx_cap* c, uint32_t dcm)
{
    /* D1 上下文 = 前块 DC 类（因果；2026-09-10 二次勘误：初版用本块自身
     * 类——非因果自条件把高 qp 段 D1 天花板虚标了一个量级）。 */
    const uint32_t db = lvlcat_b(tc_vlc_bitlen32(dcm));
    const uint32_t dctx = c->blocks_seen == 0u ? 0u : 1u + c->prev_dcb;
    c->d1[(size_t)dctx * DC_SYMS + tc_vlc_bitlen32(dcm)]++;
    c->prev_dcb = db;
    c->cur_dcb = db;
    c->prev_runb = 6u; /* 块首（无前一对） */
    c->prev_lvlb = 4u;
    c->blocks_seen++;
}

void tc_ctx_pair(tc_ctx_cap* c, uint32_t prev, uint32_t pos,
                 uint32_t runv, uint32_t lvl_m)
{
    /* 符号轴 = 完整类别 cat ∈ 0..27（2026-09-10 勘误：初版误用 4 桶粗类
     * lvlcat_b(cat)——测得的是粗类条件熵（上限 2 bit），把 lvl 族天花板
     * 高估了一个量级；上下文轴分桶不受影响）。 */
    const uint32_t rb = run_b(runv);
    const uint32_t cat = tc_vlc_bitlen32(lvl_m);
    c->r1[(size_t)prev_pos_b(prev) * RUN_SYMS + runv]++;
    c->r2[(size_t)c->prev_runb * RUN_SYMS + runv]++;
    c->r3[(size_t)c->cur_dcb * RUN_SYMS + runv]++;
    c->l1[(size_t)rb * LVL_SYMS + cat]++;
    c->l2[(size_t)c->prev_lvlb * LVL_SYMS + cat]++;
    c->l3[(size_t)(rb * L2_CTX + c->prev_lvlb) * LVL_SYMS + cat]++;
    c->l4[(size_t)pos_b(pos) * LVL_SYMS + cat]++;
    c->prev_runb = rb;
    c->prev_lvlb = lvlcat_b(cat);
}

void tc_ctx_eob(tc_ctx_cap* c, uint32_t prev)
{
    c->r1[(size_t)prev_pos_b(prev) * RUN_SYMS + 63u]++;
    c->r2[(size_t)c->prev_runb * RUN_SYMS + 63u]++;
    c->r3[(size_t)c->cur_dcb * RUN_SYMS + 63u]++;
}

void tc_ctx_fold(tc_ctx_cap* c, const uint32_t* dc_hist,
                 const uint32_t* run_hist, const uint32_t* lvl_hist)
{
    const double a0_dc = fam_bits(dc_hist, DC_SYMS);
    const double a0_run = fam_bits(run_hist, RUN_SYMS);
    const double a0_lvl = fam_bits(lvl_hist, LVL_SYMS);
    const double a1_r1 = joint_bits(c->r1, R1_CTX, RUN_SYMS);
    const double a1_r2 = joint_bits(c->r2, R2_CTX, RUN_SYMS);
    const double a1_r3 = joint_bits(c->r3, R3_CTX, RUN_SYMS);
    const double a1_l1 = joint_bits(c->l1, L1_CTX, LVL_SYMS);
    const double a1_l2 = joint_bits(c->l2, L2_CTX, LVL_SYMS);
    const double a1_l3 = joint_bits(c->l3, L3_CTX, LVL_SYMS);
    const double a1_l4 = joint_bits(c->l4, L4_CTX, LVL_SYMS);
    const double a1_d1 = joint_bits(c->d1, D1_CTX, DC_SYMS);
    /* 逐 slice 自适应：ctx 胜出才付 2 bit 信令（≤4 模型可选） */
    const double adapt_run = mind(mind(a0_run, a1_r1 - 2.0),
                                  mind(a1_r2 - 2.0, a1_r3 - 2.0));
    const double adapt_lvl = mind(mind(mind(a0_lvl, a1_l1 - 2.0),
                                       mind(a1_l2 - 2.0, a1_l3 - 2.0)),
                                  a1_l4 - 2.0);

    topos_mutex_lock(&s_ctx_mu);
    g_acc[TC_CTX_ACC_SLICES] += 1.0;
    g_acc[TC_CTX_ACC_A0_DC] += a0_dc;
    g_acc[TC_CTX_ACC_A0_RUN] += a0_run;
    g_acc[TC_CTX_ACC_A0_LVL] += a0_lvl;
    g_acc[TC_CTX_ACC_A1_R1] += a1_r1;
    g_acc[TC_CTX_ACC_A1_R2] += a1_r2;
    g_acc[TC_CTX_ACC_A1_R3] += a1_r3;
    g_acc[TC_CTX_ACC_A1_L1] += a1_l1;
    g_acc[TC_CTX_ACC_A1_L2] += a1_l2;
    g_acc[TC_CTX_ACC_A1_L3] += a1_l3;
    g_acc[TC_CTX_ACC_A1_L4] += a1_l4;
    g_acc[TC_CTX_ACC_A1_D1] += a1_d1;
    g_acc[TC_CTX_ACC_ADAPT_RUN] += adapt_run;
    g_acc[TC_CTX_ACC_ADAPT_LVL] += adapt_lvl;
    g_acc[TC_CTX_ACC_ADAPT_DC] += (a1_d1 - 2.0 < a0_dc) ? a1_d1 - 2.0 : a0_dc;
    for (uint32_t i = 0u; i < DC_SYMS; ++i) {
        g_acc[TC_CTX_ACC_SYM_DC] += (double)dc_hist[i];
    }
    for (uint32_t i = 0u; i < RUN_SYMS; ++i) {
        g_acc[TC_CTX_ACC_SYM_RUN] += (double)run_hist[i];
    }
    g_acc[TC_CTX_ACC_EOB] += (double)run_hist[63u];
    for (uint32_t i = 0u; i < LVL_SYMS; ++i) {
        g_acc[TC_CTX_ACC_SYM_LVL] += (double)lvl_hist[i];
    }
    for (uint32_t i = 0u; i < R1_CTX * RUN_SYMS; ++i) { g_r1[i] += c->r1[i]; }
    for (uint32_t i = 0u; i < L1_CTX * LVL_SYMS; ++i) { g_l1[i] += c->l1[i]; }
    for (uint32_t i = 0u; i < L3_CTX * LVL_SYMS; ++i) { g_l3[i] += c->l3[i]; }
    for (uint32_t i = 0u; i < D1_CTX * DC_SYMS; ++i) { g_d1[i] += c->d1[i]; }
    topos_mutex_unlock(&s_ctx_mu);
}

int tc_dev_ctx_active(void)
{
    return atomic_load_explicit(&s_ctx_on, memory_order_relaxed) != 0;
}

void tc_dev_ctx_enable(int enable)
{
    atomic_store_explicit(&s_ctx_on, enable ? 1 : 0, memory_order_relaxed);
}

void tc_dev_ctx_reset(void)
{
    topos_mutex_lock(&s_ctx_mu);
    memset(g_acc, 0, sizeof(g_acc));
    memset(g_r1, 0, sizeof(g_r1));
    memset(g_l1, 0, sizeof(g_l1));
    memset(g_l3, 0, sizeof(g_l3));
    memset(g_d1, 0, sizeof(g_d1));
    topos_mutex_unlock(&s_ctx_mu);
}

void tc_dev_ctx_get(double* acc)
{
    topos_mutex_lock(&s_ctx_mu);
    memcpy(acc, g_acc, sizeof(double) * TC_CTX_ACC_COUNT);
    topos_mutex_unlock(&s_ctx_mu);
}

static void pooled_copy(uint64_t* dst, const uint64_t* src, uint32_t n)
{
    topos_mutex_lock(&s_ctx_mu);
    memcpy(dst, src, (size_t)n * sizeof(uint64_t));
    topos_mutex_unlock(&s_ctx_mu);
}

void tc_dev_ctx_pooled_r1(uint64_t* out)
{
    pooled_copy(out, g_r1, R1_CTX * RUN_SYMS);
}

void tc_dev_ctx_pooled_l1(uint64_t* out)
{
    pooled_copy(out, g_l1, L1_CTX * LVL_SYMS);
}

void tc_dev_ctx_pooled_l3(uint64_t* out)
{
    pooled_copy(out, g_l3, L3_CTX * LVL_SYMS);
}

void tc_dev_ctx_pooled_d1(uint64_t* out)
{
    pooled_copy(out, g_d1, D1_CTX * DC_SYMS);
}
