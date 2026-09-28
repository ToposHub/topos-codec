#include "deblock.h"

#include "../../include/topos_codec.h"
#include "../common/tpool.h"
#include "../transform/quant.h"

#include <stdlib.h>

/* ---- 强度参数（spec v2 冻结值；调优走 A/B 战役并入档） ---- */
#define DB_ALPHA_MIN 3u
#define DB_ALPHA_MAX 48u
/* step_px = max(1, scale·3/173056)：DC 步 (scale>>4)/10816 × AC 增益 3。 */
#define DB_STEP_NUM 3u
#define DB_STEP_DEN 173056u
/* 弱滤波权重 d = (W0·(q0−p0) + W1·(p1−q1) + 8) >> 4，令 Δ = q0−p0。
 * 两个必须同时成立的端点约束（否则滤波器自己造伪影）：
 *   纯 DC 台阶（p1=p0=A, q0=q1=A+Δ，即 p1−q1 = −Δ）：要求 d = Δ/2
 *     ⇒ (W0 − W1)/16 = 1/2 ⇒ W0 − W1 = 8；
 *   均匀斜坡（斜率 s，p1−q1 = −3s，Δ = s）是**真实梯度**，滤波须恒等
 *     ⇒ (W0 − 3·W1)/16 = 0 ⇒ W0 = 3·W1。
 * 联立 ⇒ W1 = 4、W0 = 12：d = (12Δ + 4(p1−q1) + 8) >> 4 —— 台阶整段抹平、
 * 斜坡严格零响应。v1 的 (4,1) 同构（斜坡零响应）但台阶响应仅 3Δ/16 ≈ 19%，
 * 实测块界台阶只降 44%（与 ProRes 同输入对照仍差 4.3×）。 */
#define DB_W0 12
#define DB_W1 4
#define DB_ROUND 8u
#define DB_SHIFT 4u
/* 扩散：主校正 d 同时以 d>>DB_SPREAD_SHIFT 加到 p1/q1，把台阶从"边界一步"
 * 摊成 4 像素单调过渡。只劈开边界台阶而不扩散，等于把台阶搬进块内
 * （p1→p0 冒出新台阶，块效应指标转负 = 新的周期性条纹）；铺开后边界与
 * 块内差分同时趋零。0 = 关。 */
#define DB_SPREAD_SHIFT 1

#ifdef TC_DB_TUNE
#include <stdio.h>
#endif

/* 调优旋钮：默认即冻结值（DB_* 宏）。TC_DB_TUNE 构建可经环境变量覆盖，
 * 仅用于 A/B 扫参（不随发布构建发布；冻结后以宏值为唯一权威）。 */
static int32_t g_w0 = DB_W0;
static int32_t g_w1 = DB_W1;
static int32_t g_shift = (int32_t)DB_SHIFT;
static uint32_t g_alpha_max = DB_ALPHA_MAX;
static uint32_t g_step_num = DB_STEP_NUM;
static uint32_t g_step_den = DB_STEP_DEN;
static uint32_t g_spread = (uint32_t)DB_SPREAD_SHIFT;

#ifdef TC_DB_TUNE
static int g_tune_loaded = 0;
static void db_tune_load(void)
{
    if (g_tune_loaded) { return; }
    g_tune_loaded = 1;
    const char* e;
    if ((e = getenv("DB_W0")) != NULL) { g_w0 = atoi(e); }
    if ((e = getenv("DB_W1")) != NULL) { g_w1 = atoi(e); }
    if ((e = getenv("DB_SHIFT")) != NULL) { g_shift = atoi(e); }
    if ((e = getenv("DB_ALPHA_MAX")) != NULL) { g_alpha_max = (uint32_t)atoi(e); }
    if ((e = getenv("DB_STEP_NUM")) != NULL) { g_step_num = (uint32_t)atoi(e); }
    if ((e = getenv("DB_STEP_DEN")) != NULL) { g_step_den = (uint32_t)atoi(e); }
    if ((e = getenv("DB_SPREAD_SHIFT")) != NULL) { g_spread = (uint32_t)atoi(e); }
    fprintf(stderr, "[deblock tune] W0=%d W1=%d shift=%d alpha_max=%u "
            "step=%u/%u spread=%u\n", g_w0, g_w1, g_shift,
            g_alpha_max, g_step_num, g_step_den, g_spread);
}
#define DB_TUNE_INIT() db_tune_load()
#else
#define DB_TUNE_INIT() ((void)0)
#endif

static inline uint32_t deblock_alpha(uint32_t table_sel, uint32_t qp)
{
    const uint32_t scale = tc_qp_scale_tbl(table_sel, qp);
    uint32_t step = (scale * g_step_num + g_step_den / 2u) / g_step_den;
    if (step < 1u) { step = 1u; }
    uint32_t a = step * 2u;
    if (a < DB_ALPHA_MIN) { a = DB_ALPHA_MIN; }
    if (a > g_alpha_max) { a = g_alpha_max; }
    return a;
}

static inline int32_t db_clip3(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int32_t db_clamp(int32_t v, int32_t maxv)
{
    return v < 0 ? 0 : (v > maxv ? maxv : v);
}

/* 门控：m0 = |q0−p0|（跨边界台阶）、m1 = |p1−p0|、m2 = |q1−q0|（两侧局部
 * 起伏）。三者同时低于 alpha/beta 才判为量化台阶而非真实边缘/纹理。
 *
 * v2 曾以 (uint32_t)v < t 代替 |v| < t"省三条取绝对指令"——该等价式只在
 * v ≥ 0 时成立：负差转无符号是巨值、比较必然失败，门控因此退化成"只滤
 * q0 > p0 一侧"。反向台阶（q0 < p0，实测占 4K proxy 边界 45.2%、占帧内
 * 台阶能量 53.4%）全部漏滤，块效应残留近半。此处按幅值判，两侧同权。 */
static inline uint32_t db_absu(int32_t v)
{
    return (uint32_t)(v < 0 ? -v : v);
}

static inline int db_gate(int32_t p1, int32_t p0, int32_t q0, int32_t q1,
                          int32_t alpha, int32_t beta)
{
    return (db_absu(q0 - p0) < (uint32_t)alpha)
           && (db_absu(p1 - p0) < (uint32_t)beta)
           && (db_absu(q1 - q0) < (uint32_t)beta);
}

/* 校正量 d = (W0·Δ + W1·(p1−q1) + 8) >> 4（Δ = q0−p0），双重上界：
 *  1) |d| ≤ clip（= alpha/2，量化步长域）；
 *  2) |d| ≤ |Δ|/2 且 sign(d) = sign(Δ)——**不得越过 p0/q0 的中点**。
 * 第 2 条是必需的：纯台阶邻域下 (12,4) 恰好给 d = Δ/2（台阶整段抹平），
 * 但在**振铃/过冲**邻域（p1−q1 与 Δ 同号——线条边缘经 DCT 量化后极常见）
 * 权重和可让 d 超过 Δ，p0 越过 q0 把台阶**翻到另一侧**：幅度不减、位置
 * 互换，实测在高细节帧上块效应指标反而恶化（逐帧 block_v 4.31→7.04）。
 * 中点上界保证滤波是"单调收敛"而非振荡，同时保留纯台阶的满响应。 */
static inline int32_t db_delta(int32_t p1, int32_t p0, int32_t q0, int32_t q1,
                               int32_t clip)
{
    int32_t d = (g_w0 * (q0 - p0) + g_w1 * (p1 - q1)
                 + (int32_t)DB_ROUND) >> g_shift;
    const int32_t delta = q0 - p0;
    const int32_t half = (delta >= 0) ? (delta >> 1) : -((-delta) >> 1);
    if (half >= 0) {
        if (d > half) { d = half; }
        if (d < 0) { d = 0; }
    } else {
        if (d < half) { d = half; }
        if (d > 0) { d = 0; }
    }
    return db_clip3(d, -clip, clip);
}

/* 就地施加一次校正：p0 += d、q0 −= d、p1 += d>>s、q1 −= d>>s（d 已钳位）。
 * 相邻边界的依赖窗口（{x0−2..x0+1} 与 {x0+6..x0+9}）不相交，故同趟内各
 * 边界读取的都是原始像素——"逐边界就地更新"与"先算后统一写回"逐位等价。
 *
 * 保持"门控不过就一个像素都不碰"的分支写法。曾试过无分支 select 版（把
 * gate ? clamp(...) : 原值 写成 select，好让 clang 向量化 H 趟——H 趟确实
 * 向量化了，输出也逐位一致），但**实测无收益**：同一码流的重复测量在本机
 * 有 ±12% 噪声（同二进制 2K proxy 16 线程 296–338 fps），两版落在这个带内，
 * 分不出差别；而无条件计算要在门控稀疏处白算 apply。故保留标量分支版，
 * 若要真做 SIMD，必须走"保留门控跳过的掩码/早退路径"（见 §5 SIMD 计划）。 */
static inline void db_apply(uint16_t* p1, uint16_t* p0, uint16_t* q0,
                            uint16_t* q1, int32_t d, int32_t maxv)
{
    *p0 = (uint16_t)db_clamp((int32_t)*p0 + d, maxv);
    *q0 = (uint16_t)db_clamp((int32_t)*q0 - d, maxv);
    if (g_spread != 0u) {
        const int32_t ds = d >> g_spread;
        *p1 = (uint16_t)db_clamp((int32_t)*p1 + ds, maxv);
        *q1 = (uint16_t)db_clamp((int32_t)*q1 - ds, maxv);
    }
}

/* 垂直边界：**逐行**遍历（行主序连续访问），每行只走 8 对齐的边界列。
 * 首版按"块行带 × 8 行跨 stride"遍历（8 行跨 stride 的零散访问 + 每点一个
 * 数据相关分支），是二倍以上解码开销的来源；逐行扫描把内层工作集压到一行。
 * 每行边界列间隔 8、依赖窗口互不重叠，故可就地更新。 */
static void deblock_v_pass(uint16_t* pix, int32_t stride, uint32_t w,
                           uint32_t h, uint32_t unit,
                           int32_t alpha, int32_t beta,
                           int32_t clip, int32_t maxv)
{
    for (uint32_t y = 0u; y < h; ++y) {
        uint16_t* row = pix + (size_t)y * (size_t)stride;
        for (uint32_t x0 = unit; x0 + 1u < w; x0 += unit) {
            const int32_t p1 = row[x0 - 2], p0 = row[x0 - 1];
            const int32_t q0 = row[x0], q1 = row[x0 + 1];
            if (db_gate(p1, p0, q0, q1, alpha, beta)) {
                db_apply(row + x0 - 2, row + x0 - 1, row + x0, row + x0 + 1,
                         db_delta(p1, p0, q0, q1, clip), maxv);
            }
        }
    }
}

/* 水平边界：整条边界上**每个 x 都是边界位置**，故一趟平坦扫描覆盖全部列
 * （无 8 对齐掩码、无无效位置），循环体内无跨行分支，便于向量化。
 * 跨水平边界的样本序列沿**垂直**方向：p1 在 p0 上一行、q1 在 q0 下一行。
 * v1 误取 pu[x−1]/qu[x+1]（同一行内相邻列）——校正方向虽对，但"台阶 vs
 * 斜坡"判别用了与边界无关的横向抽头，水平边界台阶因此降不下去（A/B：
 * block_h 已归零而 block_v 仍有 1.2–1.5）。y0 是 8 的倍数且 ≥ 8，故
 * y0−2 ≥ 6、y0+1 ≤ h−7 恒在界内。
 * r_begin/r_end 限定处理的边界行区间（并行切片用）：各边界行的读写窗口
 * {y0−2..y0+1} 相隔 unit、互不相交（unit ≥ 4 时），故任意区间划分都与串行
 * 逐位一致。 */
static void deblock_h_range(uint16_t* pix, int32_t stride, uint32_t w,
                            uint32_t unit, uint32_t r_begin, uint32_t r_end,
                            int32_t alpha, int32_t beta, int32_t clip,
                            int32_t maxv)
{
    for (uint32_t r = r_begin; r < r_end; ++r) {
        const uint32_t y0 = r * unit;
        uint16_t* pu2 = pix + (size_t)(y0 - 2u) * (size_t)stride;
        uint16_t* pu = pix + (size_t)(y0 - 1u) * (size_t)stride;
        uint16_t* qu = pix + (size_t)y0 * (size_t)stride;
        uint16_t* qu2 = pix + (size_t)(y0 + 1u) * (size_t)stride;
        /* 整行稠密 stencil：每个 x 都跨同一条边界，逐 x 互不相干 */
        for (uint32_t x = 0u; x < w; ++x) {
            const int32_t p1 = pu2[x], p0 = pu[x];
            const int32_t q0 = qu[x], q1 = qu2[x];
            if (db_gate(p1, p0, q0, q1, alpha, beta)) {
                db_apply(pu2 + x, pu + x, qu + x, qu2 + x,
                         db_delta(p1, p0, q0, q1, clip), maxv);
            }
        }
    }
}

/* ---- 两趟的并行切分 ----
 * 可并行性来自"访问窗口互不相交"，不是近似：
 *  - 垂直趟：第 y 行只读写本行的 4 抽头窗口 → 按行带切分，任一行带独立；
 *  - 水平趟：边界行 y0=8r 的读写窗口是 {y0−2..y0+1}，相邻边界行相隔 8，
 *    窗口两两不相交 → 按 r 区间切分，任一区间独立。
 * 唯一的跨趟约束是"垂直全部完成后才水平"（水平趟读垂直趟改过的像素），
 * 两次 tc_parallel_for 调用本身即为屏障。故并行输出与串行**逐位一致**
 * （单测 test_deblock 的并行等价用例 + 解码端"任意线程数解码结果逐位
 * 一致"复验）。
 * 线程 ≤1、单元数 ≤1、平面过小或 tpool 退化（worker 内嵌套/环满/OOM）
 * 时自动顺序内联——tpool 契约保证，正确性不受影响。 */

/* 小于该样本数的平面不做并行分发：批次开销高于滤波本身（缩略图/局部
 * 平面常见）。64K 采样 ≈ 128 KB u16，仍远低于任何一档产品平面。 */
#define DB_PAR_MIN_SAMPLES (64u * 1024u)
/* 任务上限：片数取 4×线程数以摊平大小边界行的尾部失衡（动态领取），
 * 上限即上限（栈上数组，避免每次分配）。 */
#define DB_JOB_MAX 64u

typedef struct db_job {
    uint16_t* pix;
    int32_t stride;
    uint32_t w;
    uint32_t unit;    /* 目标域块界周期（全尺寸 8；1/2 抽样 4） */
    uint32_t first;   /* 垂直：起始行；水平：起始边界行号 r */
    uint32_t units;   /* 本片单元数 */
    int32_t alpha;
    int32_t beta;
    int32_t clip;
    int32_t maxv;
    int vertical;     /* 1 = 垂直趟，0 = 水平趟 */
} db_job;

static void db_job_run(void* ctx)
{
    const db_job* j = (const db_job*)ctx;
    if (j->vertical != 0) {
        deblock_v_pass(j->pix + (size_t)j->first * (size_t)j->stride,
                       j->stride, j->w, j->units, j->unit,
                       j->alpha, j->beta, j->clip, j->maxv);
    } else {
        deblock_h_range(j->pix, j->stride, j->w, j->unit, j->first,
                        j->first + j->units,
                        j->alpha, j->beta, j->clip, j->maxv);
    }
}

/* 把一趟切成 n 片交给常驻池；first_unit 为该趟的单元起始编号
 * （垂直趟 = 0 行，水平趟 = 1 号边界行）。 */
static void db_run_pass(uint16_t* pix, int32_t stride, uint32_t w,
                        uint32_t unit, uint32_t units, uint32_t first_unit,
                        int vertical, int32_t alpha, int32_t beta, int32_t clip,
                        int32_t maxv, uint32_t threads)
{
    if (units == 0u) { return; }
    uint32_t n = threads * 4u;
    if (n > units) { n = units; }
    if (n > DB_JOB_MAX) { n = DB_JOB_MAX; }
    if (n <= 1u) {
        db_job one;
        one.pix = pix; one.stride = stride; one.w = w; one.unit = unit;
        one.first = first_unit; one.units = units;
        one.alpha = alpha; one.beta = beta; one.clip = clip; one.maxv = maxv;
        one.vertical = vertical;
        db_job_run(&one);
        return;
    }
    tc_job jobs[DB_JOB_MAX];
    db_job ctx[DB_JOB_MAX];
    for (uint32_t i = 0u; i < n; ++i) {
        const uint32_t b = (uint32_t)(((uint64_t)units * i) / n);
        const uint32_t e = (uint32_t)(((uint64_t)units * (i + 1u)) / n);
        ctx[i].pix = pix; ctx[i].stride = stride; ctx[i].w = w;
        ctx[i].unit = unit;
        ctx[i].first = first_unit + b; ctx[i].units = e - b;
        ctx[i].alpha = alpha; ctx[i].beta = beta;
        ctx[i].clip = clip; ctx[i].maxv = maxv;
        ctx[i].vertical = vertical;
        jobs[i].fn = db_job_run;
        jobs[i].ctx = &ctx[i];
    }
    (void)tc_parallel_for(jobs, n, threads);
}

/* 内核：unit = 目标域块界周期。全尺寸交付 unit=8（源 8×8 变换网格）；
 * 1/2 抽样交付 unit=4（源块界在目标域落在 4 的倍数上，见 codec.c 的
 * dec_scaled_deblock_unit）。
 * unit ≥ 4 是并行等价性的前提：水平趟各边界行的读写窗口宽 4（{y0−2..y0+1}），
 * unit < 4 时窗口重叠、切片不再独立。1/4 及更小的抽样（窗口重叠，且块界
 * 周期 2 已近 Nyquist）归后续批次，此处直接跳过。 */
static void deblock_plane_unit(uint16_t* pix, int32_t stride_pix, uint32_t w,
                               uint32_t h, uint32_t qp, uint32_t table_sel,
                               uint8_t bit_depth, uint32_t unit)
{
    if (pix == NULL || qp > TC_QP_MAX || unit < 4u || unit > 8u
            || w < 2u * unit || h < 2u * unit
            || (w % unit) != 0u || (h % unit) != 0u) {
        return; /* 非 unit 对齐/非法 qp：不去块（防御；产品平面恒 unit 对齐） */
    }
    if (bit_depth < 8u || bit_depth > 16u) { return; }
    DB_TUNE_INIT();
    const int32_t maxv = (int32_t)((1u << bit_depth) - 1u);
    /* 强度只与 (qp, table_sel) 有关、全平面恒定——在每条边界上重算（含
     * 一次整数除法）是滤波耗时的可观部分，提到循环外。 */
    const int32_t alpha = (int32_t)deblock_alpha(table_sel, qp);
    const int32_t beta = alpha / 2 + 2;
    const int32_t clip = alpha >> 1;
    /* 先垂直后水平，与 H.264 环内滤波同序：遍历序固定 → 输出确定。 */
    const uint32_t threads = ((uint64_t)w * (uint64_t)h < DB_PAR_MIN_SAMPLES)
        ? 1u : (uint32_t)tc_dev_thread_count();
    db_run_pass(pix, stride_pix, w, unit, h, 0u, 1,
                alpha, beta, clip, maxv, threads);
    db_run_pass(pix, stride_pix, w, unit, (h / unit) - 1u, 1u, 0,
                alpha, beta, clip, maxv, threads);
}

void tc_deblock_plane(uint16_t* pix, int32_t stride_pix, uint32_t w, uint32_t h,
                      uint32_t qp, uint32_t table_sel, uint8_t bit_depth)
{
    deblock_plane_unit(pix, stride_pix, w, h, qp, table_sel, bit_depth, 8u);
}

void tc_deblock_plane_scaled(uint16_t* pix, int32_t stride_pix, uint32_t w,
                             uint32_t h, uint32_t qp, uint32_t table_sel,
                             uint8_t bit_depth, uint32_t unit)
{
    deblock_plane_unit(pix, stride_pix, w, h, qp, table_sel, bit_depth, unit);
}
