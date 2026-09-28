#include "common/tpool.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <unistd.h>
#define TOPOS_TPOOL_PTHREAD 1
#endif

/* ---- 线程上限配置（env 一次解析；调用方可覆盖） ---- */

static atomic_int s_thread_max = -1; /* -1 = 待解析 */

/* ---- P1-07：退化 telemetry（全 relaxed——计数无顺序依赖，只求无竞争） ---- */
static _Atomic uint64_t s_tp_pool_batches;
static _Atomic uint64_t s_tp_inline_batches;
static _Atomic uint64_t s_tp_nested;
static _Atomic uint64_t s_tp_ring_full;
static _Atomic uint64_t s_tp_init_fail;
static _Atomic uint64_t s_tp_grow_fail;
static _Atomic uint64_t s_tp_workers_max;

void tc_dev_tpool_stats_get(tc_tpool_stats* out)
{
    if (out == NULL) { return; }
    out->pool_batches = atomic_load_explicit(&s_tp_pool_batches, memory_order_relaxed);
    out->inline_batches = atomic_load_explicit(&s_tp_inline_batches, memory_order_relaxed);
    out->nested_fallbacks = atomic_load_explicit(&s_tp_nested, memory_order_relaxed);
    out->ring_full_fallbacks = atomic_load_explicit(&s_tp_ring_full, memory_order_relaxed);
    out->init_fail_fallbacks = atomic_load_explicit(&s_tp_init_fail, memory_order_relaxed);
    out->grow_fail_batches = atomic_load_explicit(&s_tp_grow_fail, memory_order_relaxed);
    out->workers_max = atomic_load_explicit(&s_tp_workers_max, memory_order_relaxed);
}

void tc_dev_tpool_stats_reset(void)
{
    atomic_store_explicit(&s_tp_pool_batches, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_tp_inline_batches, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_tp_nested, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_tp_ring_full, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_tp_init_fail, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_tp_grow_fail, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_tp_workers_max, 0u, memory_order_relaxed);
}

#define TP_BUMP(field) \
    atomic_fetch_add_explicit(&s_tp_##field, 1u, memory_order_relaxed)

static int32_t thread_default(void)
{
#if defined(TOPOS_TPOOL_PTHREAD)
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int32_t def = ncpu > 0 ? (int32_t)ncpu : 1;
    /* 默认 = min(ncpu, TC_SLICE_MAX_THREADS)（M10-6 上限 8→16 后 16 逻辑核
     * 机默认即为 16——M3 时同一表达式等价 min(8,ncpu)，故注释旧记 min(8)）。
     * 2026-09-02 静机交错对复验（2K/4K 各 12 对，tools/bench_t8_t16_paired）
     * 追认 16 为解码默认：2K 打平（−0.2%）、4K +2.0%（12/12 正对）且方差
     * 显著更小；负载下 +14%（5/5 对）。编码在 8P/16L 机 t16 −8%（M10-6
     * 实测）——产品编码路径显式 min(8,cpu)，不受默认影响。 */
    if (def > TC_SLICE_MAX_THREADS) { def = TC_SLICE_MAX_THREADS; }
#elif defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int32_t def = si.dwNumberOfProcessors > 0 ? (int32_t)si.dwNumberOfProcessors : 1;
    if (def > TC_SLICE_MAX_THREADS) { def = TC_SLICE_MAX_THREADS; }
#else
    int32_t def = 1;
#endif
    const char* env = getenv("TOPOS_SLICE_THREADS");
    if (env != NULL && env[0] != '\0') {
        int v = atoi(env);
        if (v >= 1 && v <= TC_SLICE_MAX_THREADS) { return v; }
    }
    return def;
}

void tc_dev_set_thread_count(int32_t n)
{
    if (n < 1) { n = 1; }
    if (n > TC_SLICE_MAX_THREADS) { n = TC_SLICE_MAX_THREADS; }
    atomic_store(&s_thread_max, (int)n);
}

int32_t tc_dev_thread_count(void)
{
    int v = atomic_load(&s_thread_max);
    if (v < 0) {
        int32_t def = thread_default();
        atomic_store(&s_thread_max, (int)def);
        return def;
    }
    return (int32_t)v;
}

/* ---- R6：常驻有界线程池 ----
 *
 * 取代阶段 10 的 spawn-per-call（encode_sized 最坏 ~96 波 pthread_create/join
 * 每帧）。语义与旧实现一致：
 *  - 静态条带划分 i % nw（worker w 固定执行条带 w；同槽任务绝不并发）；
 *  - 调用线程执行条带 0 后等待其余条带；
 *  - 初始化/spawn/OOM/环满失败路径一律顺序执行（正确性优先）。
 *
 * 池化带来的结构性回压：并发调用方共享同一组常驻 worker，总执行线程数
 * = 池内 worker + 活跃调用方（旧实现 = 各调用方各自 spawn 相乘）。
 *
 * 批次生命周期（无 UAF/无泄漏）：tc_pool_batch 按调用堆分配；ring 槽记录
 * worker 越过数与调用方离开标志，两侧都在互斥锁内检查——「全部 worker 越
 * 过 ∧ 调用方已取得结果」成立时由后到者释放。worker 内嵌套 tc_parallel_for
 * （codec 当前无此路径）退化为顺序执行，杜绝 worker 互相等待的死锁。 */

#if defined(TOPOS_TPOOL_PTHREAD) || defined(_WIN32)

typedef struct tc_pool_batch {
    const tc_job* jobs;
    uint32_t count;
    uint32_t nw;   /* 本批条带数（调用方占 1 条；worker id ∈ (id_base, id_base+nw)） */
    uint32_t id_base; /* M10-4：本批 worker id 起点（并发批次占不相交区间） */
    uint32_t done; /* 已完成 worker 条带数（锁保护；达 nw−1 唤醒调用方） */
    atomic_uint next; /* 动态领取游标（fetch_add；初始化后只增） */
} tc_pool_batch;

#define TC_POOL_RING 64u /* 批次环形槽（并发 outstanding 上限；溢出退化串行） */

typedef struct tc_pool_ring_ent {
    tc_pool_batch b;     /* 内嵌（M10-1c）：免每批 heap calloc/free——解码
                          * 每帧 ≥1 批、encoder 每 plane/probe 多批，稳态
                          * 分配归零。槽位仅在双侧释放条件满足后复用，
                          * jobs 指针生命周期与旧 heap 版完全一致 */
    uint32_t passed;      /* 已越过该批次的 worker 数 */
    uint32_t need;        /* 需越过的 worker 数（入队时的池内 worker 数） */
    int caller_left;      /* 调用方已取得结果（其后不再触碰 b） */
    int in_use;           /* b 有效（入队后未双条件释放） */
} tc_pool_ring_ent;

typedef struct tc_pool_state {
    int init;
    int shutdown;
    int nthreads;                                /* 常驻 worker 数（id 1..nthreads） */
    tc_pool_ring_ent ring[TC_POOL_RING];
    uint64_t seq;                                /* 已入队批次总数（含已释放） */
    uint64_t freed;                              /* 已释放批次数（outstanding = seq − freed） */
    void* threads[TC_SLICE_MAX_THREADS - 1];
#if defined(TOPOS_TPOOL_PTHREAD)
    pthread_mutex_t mu;
    pthread_cond_t work_cv;
    pthread_cond_t done_cv;
#elif defined(_WIN32)
    SRWLOCK mu;
    CONDITION_VARIABLE work_cv;
    CONDITION_VARIABLE done_cv;
#endif
} tc_pool_state;

static tc_pool_state g_pool;

#if defined(TOPOS_TPOOL_PTHREAD)
static __thread int t_pool_worker;
static __thread int t_pool_worker_rel;
#elif defined(__MINGW32__)
static __thread int t_pool_worker;
static __thread int t_pool_worker_rel;
#elif defined(_WIN32)
static __declspec(thread) int t_pool_worker;
static __declspec(thread) int t_pool_worker_rel;
#else
#define t_pool_worker 0
#define t_pool_worker_rel 0
#endif

static void pool_lock(tc_pool_state* p)
{
#if defined(TOPOS_TPOOL_PTHREAD)
    pthread_mutex_lock(&p->mu);
#elif defined(_WIN32)
    AcquireSRWLockExclusive(&p->mu);
#endif
}

static void pool_unlock(tc_pool_state* p)
{
#if defined(TOPOS_TPOOL_PTHREAD)
    pthread_mutex_unlock(&p->mu);
#elif defined(_WIN32)
    ReleaseSRWLockExclusive(&p->mu);
#endif
}

static void pool_wait(tc_pool_state* p, int done_cv)
{
#if defined(TOPOS_TPOOL_PTHREAD)
    if (done_cv != 0) {
        pthread_cond_wait(&p->done_cv, &p->mu);
    } else {
        pthread_cond_wait(&p->work_cv, &p->mu);
    }
#elif defined(_WIN32)
    if (done_cv != 0) {
        SleepConditionVariableSRW(&p->done_cv, &p->mu, INFINITE, 0);
    } else {
        SleepConditionVariableSRW(&p->work_cv, &p->mu, INFINITE, 0);
    }
#endif
}

static void pool_wake(tc_pool_state* p, int done_cv)
{
#if defined(TOPOS_TPOOL_PTHREAD)
    if (done_cv != 0) {
        pthread_cond_broadcast(&p->done_cv);
    } else {
        pthread_cond_broadcast(&p->work_cv);
    }
#elif defined(_WIN32)
    if (done_cv != 0) {
        WakeAllConditionVariable(&p->done_cv);
    } else {
        WakeAllConditionVariable(&p->work_cv);
    }
#endif
}

/* 环槽双侧释放检查（调用方与 worker 各执一侧；锁内调用）。
 * M10-1c：释放 = 槽位可复用标记（不再 free）；任何一方在释放后均不再
 * 触碰 b —— caller 在 done==nw−1 后只读一次 b->nw 即标记离开，worker 在
 * done++/passed++ 的同一锁段后不再访问 b。 */
static void pool_ent_release(tc_pool_ring_ent* ent)
{
    if (ent->in_use != 0 && ent->passed == ent->need && ent->caller_left != 0) {
        ent->in_use = 0;
        g_pool.freed++;
    }
}

/* worker 主循环：按自身序号消费批次环（id 固定 → 静态条带语义） */
typedef struct tc_pool_worker_arg {
    tc_pool_state* p;
    uint64_t seen; /* 已越过批次数（新 worker 从入队时刻的 seq 起步） */
    uint32_t id;   /* worker id（1..nthreads） */
} tc_pool_worker_arg;

/* 动态领取（解码深化批次）：共享原子游标 fetch_add 逐任务领取。
 * 语义与静态条带等价（每任务恰好执行一次；执行线程不定）；对任务大小
 * 悬殊的批次（解码 slice：Y 重 U/V 轻）消除静态条带的尾部失衡
 * （27 slice / 8 worker 实测有效核数 5.9 → 7.5+）。任务间无顺序依赖、
 * 结果按索引汇合（codec 侧 join 后遍历），执行顺序不影响确定性输出。 */
static void batch_run_dynamic(tc_pool_batch* b)
{
    for (;;) {
        uint32_t i = atomic_fetch_add_explicit(&b->next, 1u, memory_order_relaxed);
        if (i >= b->count) { break; }
        b->jobs[i].fn(b->jobs[i].ctx);
    }
}

static void pool_worker_loop(tc_pool_worker_arg* wa)
{
    tc_pool_state* p = wa->p;
    t_pool_worker = (int)wa->id; /* TLS 载 id（1..nthreads；0 = 调用线程）：
                                  * 按 worker 槽位私有的资源（alpha 行缓冲等）
                                  * 据此取槽——动态领取后 si%nw 不再保证互斥 */
    pool_lock(p);
    for (;;) {
        if (wa->seen < p->seq) {
            tc_pool_ring_ent* ent = &p->ring[wa->seen % TC_POOL_RING];
            tc_pool_batch* b = &ent->b;
            pool_unlock(p);

            /* M10-4：id_base 错开并发批次——worker 只参与
             * id ∈ (id_base, id_base+nw) 的批次；单批（base=0）同旧版语义。
             * 相对槽位 = id − id_base ∈ [1, nw−1]，保证槽位资源（DC/alpha
             * 行缓冲等，容量按 nw 分配）的正确私有性与互斥性。 */
            if (wa->id > b->id_base && wa->id - b->id_base < b->nw) {
                t_pool_worker_rel = (int)(wa->id - b->id_base);
                batch_run_dynamic(b);
                t_pool_worker_rel = 0;
            }

            pool_lock(p);
            if (wa->id > b->id_base && wa->id - b->id_base < b->nw) {
                b->done++;
                if (b->done == b->nw - 1u) { pool_wake(p, 1); }
            }
            wa->seen++;
            ent->passed++;
            pool_ent_release(ent);
            continue;
        }
        if (p->shutdown != 0) { break; }
        pool_wait(p, 0);
    }
    pool_unlock(p);
}

#if defined(TOPOS_TPOOL_PTHREAD)

static void* pool_worker_main(void* arg)
{
    pool_worker_loop((tc_pool_worker_arg*)arg);
    free(arg);
    return NULL;
}

static pthread_once_t g_pool_once = PTHREAD_ONCE_INIT;
static void pool_shutdown(void);
static void pool_atfork_prepare(void);
static void pool_atfork_parent(void);
static void pool_atfork_child(void);

/* 复验 P1-15：一次性初始化必须经 pthread_once——两个首次调用方并发进入
 * 时，旧实现可同时 init 同一 mutex/cond（其中一个正在被使用）并重复
 * 注册 atexit/atfork。once 之内完成：同步原语 init + 生命周期钩子注册。 */
static void pool_init_once_impl(void)
{
    tc_pool_state* p = &g_pool;
    if (pthread_mutex_init(&p->mu, NULL) != 0) { return; }
    if (pthread_cond_init(&p->work_cv, NULL) != 0) {
        pthread_mutex_destroy(&p->mu);
        return;
    }
    if (pthread_cond_init(&p->done_cv, NULL) != 0) {
        pthread_cond_destroy(&p->work_cv);
        pthread_mutex_destroy(&p->mu);
        return;
    }
    p->nthreads = 0;
    p->seq = 0;
    p->freed = 0;
    p->shutdown = 0;
    p->init = 1;
    atexit(pool_shutdown);
    pthread_atfork(pool_atfork_prepare, pool_atfork_parent, pool_atfork_child);
}

static int pool_init(tc_pool_state* p)
{
    (void)p;
    if (pthread_once(&g_pool_once, pool_init_once_impl) != 0) { return -1; }
    return g_pool.init != 0 ? 0 : -1;
}

static void pool_atfork_child(void)
{
    /* fork 后仅调用线程存活：worker 与锁状态不复存在 → 惰性重建。
     * once 状态须回退为未执行（g_pool.init 归零后允许重新走 once）。 */
    memset(&g_pool, 0, sizeof(g_pool));
    /* PTHREAD_ONCE_INIT 是花括号初始化器，不可直接赋值——经局部临时 +
     * 字节拷贝回退 once 状态（child 单线程，安全）。 */
    {
        pthread_once_t fresh = PTHREAD_ONCE_INIT;
        memcpy(&g_pool_once, &fresh, sizeof(fresh));
    }
    t_pool_worker = 0;
}

static void pool_atfork_prepare(void)
{
    if (g_pool.init != 0) { pool_lock(&g_pool); }
}

static void pool_atfork_parent(void)
{
    if (g_pool.init != 0) { pool_unlock(&g_pool); }
}

static void pool_shutdown(void)
{
    tc_pool_state* p = &g_pool;
    if (p->init == 0) { return; }
    pool_lock(p);
    p->shutdown = 1;
    pool_wake(p, 0);
    pool_unlock(p);
    for (int i = 0; i < p->nthreads; ++i) { pthread_join((pthread_t)p->threads[i], NULL); }
    p->nthreads = 0;
    p->init = 0;
}

/* 锁内调用：增长常驻 worker 到 want（失败返回 −1，已起线程保留） */
static int pool_grow(tc_pool_state* p, int want)
{
    while (p->nthreads < want) {
        int slot = p->nthreads;
        tc_pool_worker_arg* wa = (tc_pool_worker_arg*)calloc(1, sizeof(*wa));
        if (wa == NULL) { return -1; }
        wa->p = p;
        wa->seen = p->seq; /* 新 worker 不回扫历史批次 */
        wa->id = (uint32_t)(slot + 1);
        pthread_t th;
        if (pthread_create(&th, NULL, pool_worker_main, wa) != 0) {
            free(wa);
            return -1;
        }
        p->threads[slot] = (void*)th;
        p->nthreads++;
        atomic_store_explicit(&s_tp_workers_max, (uint64_t)p->nthreads,
                              memory_order_relaxed);
    }
    return 0;
}

#elif defined(_WIN32)

static void pool_shutdown(void);

static unsigned int __stdcall pool_worker_main(void* arg)
{
    pool_worker_loop((tc_pool_worker_arg*)arg);
    free(arg);
    return 0;
}

/* 复验 P1-15（Win32 镜像）：InitOnceExecuteOnce 保证并发首调只初始化一次 */
static BOOL CALLBACK pool_init_once_win(PINIT_ONCE once, PVOID param, PVOID* ctx)
{
    tc_pool_state* p = &g_pool;
    (void)once; (void)param; (void)ctx;
    InitializeSRWLock(&p->mu);
    InitializeConditionVariable(&p->work_cv);
    InitializeConditionVariable(&p->done_cv);
    p->nthreads = 0;
    p->seq = 0;
    p->freed = 0;
    p->shutdown = 0;
    p->init = 1;
    atexit(pool_shutdown);
    return TRUE;
}

static int pool_init(tc_pool_state* p)
{
    (void)p;
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, pool_init_once_win, NULL, NULL);
    return g_pool.init != 0 ? 0 : -1;
}

static void pool_shutdown(void)
{
    tc_pool_state* p = &g_pool;
    if (p->init == 0) { return; }
    pool_lock(p);
    p->shutdown = 1;
    pool_wake(p, 0);
    pool_unlock(p);
    for (int i = 0; i < p->nthreads; ++i) {
        WaitForSingleObject((HANDLE)p->threads[i], INFINITE);
        CloseHandle((HANDLE)p->threads[i]);
    }
    p->nthreads = 0;
    p->init = 0;
}

/* 锁内调用：增长常驻 worker 到 want（失败返回 −1，已起线程保留） */
static int pool_grow(tc_pool_state* p, int want)
{
    while (p->nthreads < want) {
        int slot = p->nthreads;
        tc_pool_worker_arg* wa = (tc_pool_worker_arg*)calloc(1, sizeof(*wa));
        if (wa == NULL) { return -1; }
        wa->p = p;
        wa->seen = p->seq; /* 新 worker 不回扫历史批次 */
        wa->id = (uint32_t)(slot + 1);
        uintptr_t th = _beginthreadex(NULL, 0u, pool_worker_main, wa, 0u, NULL);
        if (th == 0) {
            free(wa);
            return -1;
        }
        p->threads[slot] = (void*)th;
        p->nthreads++;
        atomic_store_explicit(&s_tp_workers_max, (uint64_t)p->nthreads,
                              memory_order_relaxed);
    }
    return 0;
}

#endif /* 平台 init/shutdown/grow */

/* 复验 P1-15：atexit/atfork 注册已并入 pthread_once / InitOnce——本函数
 * 保留为空（调用点不改动，语义 = 无操作）。 */
static void pool_atexit_once(void)
{
}

/* ---- 并行入口（语义与阶段 10 spawn-per-call 一致；动态领取分发） ---- */

uint32_t tc_pool_worker_slot(void)
{
    /* 0 = 调用线程（含无线程平台）；1..nw−1 = 本批次内相对 worker 槽位。
     * M10-4 起 id_base 会错开并发批次的绝对 worker id，槽位资源（容量按
     * nw 分配）必须按批内相对位置取——绝对 id 会越界回退槽 0 造成竞争。 */
    return (uint32_t)(t_pool_worker_rel > 0 ? t_pool_worker_rel : 0);
}

static uint32_t stripe_nw(uint32_t n, uint32_t max_workers)
{
    uint32_t nw = max_workers < n ? max_workers : n;
    if (nw > (uint32_t)TC_SLICE_MAX_THREADS) { nw = (uint32_t)TC_SLICE_MAX_THREADS; }
    if (nw < 1u) { nw = 1u; }
    return nw;
}

static uint32_t tc_parallel_for_impl(const tc_job* jobs, uint32_t n,
                                     uint32_t max_workers)
{
    t_pool_worker_rel = 0; /* 调用方恒槽 0（清残留；worker 侧进批时再写） */
    if (n <= 1u || max_workers <= 1u) {
        for (uint32_t i = 0u; i < n; ++i) { jobs[i].fn(jobs[i].ctx); }
        TP_BUMP(inline_batches);
        return 1u;
    }
    if (t_pool_worker != 0) {
        /* worker 内嵌套调用：调用线程顺序执行（防 worker 互等死锁） */
        for (uint32_t i = 0u; i < n; ++i) { jobs[i].fn(jobs[i].ctx); }
        TP_BUMP(nested);
        return 1u;
    }
    uint32_t nw = stripe_nw(n, max_workers);
    if (nw <= 1u) {
        for (uint32_t i = 0u; i < n; ++i) { jobs[i].fn(jobs[i].ctx); }
        TP_BUMP(inline_batches);
        return 1u;
    }

    tc_pool_state* p = &g_pool;
    pool_atexit_once();
    if (pool_init(p) != 0) {
        for (uint32_t i = 0u; i < n; ++i) { jobs[i].fn(jobs[i].ctx); }
        TP_BUMP(init_fail);
        return 1u;
    }

    pool_lock(p);
    if (p->seq - p->freed >= TC_POOL_RING) {
        /* 环满（≥64 个 outstanding 批次）→ 顺序执行，不阻塞其他调用方 */
        pool_unlock(p);
        for (uint32_t i = 0u; i < n; ++i) { jobs[i].fn(jobs[i].ctx); }
        TP_BUMP(ring_full);
        return 1u;
    }
    if (p->nthreads < (int)(nw - 1u)) {
        if (pool_grow(p, (int)(nw - 1u)) != 0 && p->nthreads < 1) {
            pool_unlock(p);
            for (uint32_t i = 0u; i < n; ++i) { jobs[i].fn(jobs[i].ctx); }
            return 1u; /* 一个 worker 都起不来 → 顺序 */
        }
    }
    uint32_t eff_nw = (uint32_t)p->nthreads + 1u; /* 部分增长失败的退化档 */
    if (eff_nw < nw) { atomic_fetch_add_explicit(&s_tp_grow_fail, 1u, memory_order_relaxed); }
    if (eff_nw > nw) { eff_nw = nw; }

    tc_pool_ring_ent* ent = &p->ring[p->seq % TC_POOL_RING];
    tc_pool_batch* b = &ent->b; /* M10-1c：环槽内嵌（无 heap 分配） */
    /* M10-4：有在飞批次时新批错开到其占用区间之后（无空位回退 0 排队，
     * 语义同旧版串行）。锁内读 prev：入队/释放均在锁内，字段一致。 */
    uint32_t id_base = 0u;
    if (p->seq > p->freed) {
        const tc_pool_ring_ent* prev = &p->ring[(p->seq - 1u) % TC_POOL_RING];
        if (prev->in_use) {
            uint32_t next_base = prev->b.id_base + prev->b.nw;
            if (next_base <= (uint32_t)p->nthreads) { id_base = next_base; }
        }
    }
    /* 区间钳位：id_base + eff_nw 不得超过调用方+worker 总 id 数（否则
     * done 计数等待不存在的 worker → 死锁）；钳到 1 时仅调用方串行领取 */
    if (id_base + eff_nw > (uint32_t)p->nthreads + 1u) {
        eff_nw = (uint32_t)p->nthreads + 1u - id_base;
    }
    if (eff_nw < 1u) { eff_nw = 1u; }
    b->jobs = jobs;
    b->count = n;
    b->nw = eff_nw;
    b->id_base = id_base;
    b->done = 0u;
    atomic_store_explicit(&b->next, 0u, memory_order_relaxed);
    ent->passed = 0u;
    ent->need = (uint32_t)p->nthreads;
    ent->caller_left = 0;
    ent->in_use = 1;
    p->seq++;
    pool_wake(p, 0);
    pool_unlock(p);

    /* 调用线程作为 worker 0 参与动态领取，随后等待 worker 条带完成 */
    batch_run_dynamic(b);
    pool_lock(p);
    while (b->done < b->nw - 1u) { pool_wait(p, 1); }
    const uint32_t nw_used = b->nw;
    ent->caller_left = 1;
    pool_ent_release(ent); /* 调用方后到 → 调用方释放（此后不再触碰 b） */
    pool_unlock(p);
    TP_BUMP(pool_batches);
    return nw_used;
}

/* P1-19：worker 内嵌套调用入口统一保存/恢复相对槽位 TLS——嵌套调用
 * 入口清零（调用方恒槽 0 语义）不得泄漏到外层任务（否则外层在嵌套
 * 返回后再取槽位会错误落回槽 0，与同槽任务互斥约定冲突）。 */
uint32_t tc_parallel_for(const tc_job* jobs, uint32_t n, uint32_t max_workers)
{
    const int rel_saved = t_pool_worker_rel;
    t_pool_worker_rel = 0;
    const uint32_t nw = tc_parallel_for_impl(jobs, n, max_workers);
    t_pool_worker_rel = rel_saved;
    return nw;
}

#else /* 无线程平台：顺序执行（正确性优先） */

uint32_t tc_parallel_for(const tc_job* jobs, uint32_t n, uint32_t max_workers)
{
    (void)max_workers;
    for (uint32_t i = 0u; i < n; ++i) { jobs[i].fn(jobs[i].ctx); }
    return 1u;
}

/* P1-07：stats get/reset 已在文件头无条件定义（无线程平台计数恒 0） */

#endif /* 平台池 / 串行回退 */
