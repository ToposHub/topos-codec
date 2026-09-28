/* P1-07：tpool 退化 telemetry——计数可见、路径可归因。
 *
 * 门：池内执行与内联执行分别计数；worker 内嵌套调用触发串行回退时
 * nested_fallbacks 递增（死锁防护触发可观测）。ring_full / init_fail
 * 需要特殊压力/故障环境，不在此覆盖（fuzz/压测另行观测）。 */
#include "common/tpool.h"
#include "mini_test.h"

#include <stdatomic.h>
#include <stdlib.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

static void bump_job(void* p)
{
    atomic_uint* counter = (atomic_uint*)p;
    atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
}

typedef struct {
    const tc_job* jobs;
    uint32_t n;
} nested_arg;

static void nested_job(void* p)
{
    /* 在（可能由 worker 执行的）任务内再发一次并行调用 → 触发嵌套串行回退 */
    nested_arg* na = (nested_arg*)p;
    (void)tc_parallel_for(na->jobs, na->n, 4u);
}

#if !defined(_WIN32)
static void test_pool_fork_and_nested_slot(void);
#endif

int main(void)
{
    tc_dev_set_thread_count(4);
    tc_dev_tpool_stats_reset();
#if !defined(_WIN32)
    test_pool_fork_and_nested_slot();
#endif

    tc_tpool_stats st;
    tc_dev_tpool_stats_get(&st);
    MT_CHECK_EQ_U64(st.pool_batches, 0u);
    MT_CHECK_EQ_U64(st.nested_fallbacks, 0u);

    /* 1) 池内并行批次：计数 + 全部任务恰好一次 */
    enum { N = 32u };
    static atomic_uint cnt;
    atomic_store_explicit(&cnt, 0u, memory_order_relaxed);
    tc_job jobs[N];
    for (uint32_t i = 0; i < N; ++i) {
        jobs[i].fn = bump_job;
        jobs[i].ctx = &cnt;
    }
    (void)tc_parallel_for(jobs, N, 4u);
    MT_CHECK_EQ_U64(atomic_load(&cnt), N);
    tc_dev_tpool_stats_get(&st);
    MT_CHECK_EQ_U64(st.pool_batches, 1u);
    MT_CHECK(st.workers_max >= 3u); /* t4：至少 3 个常驻 worker */

    /* 2) 任务数 ≤1 → 内联计数，不进池 */
    uint64_t inline_before = st.inline_batches;
    (void)tc_parallel_for(jobs, 1u, 4u);
    tc_dev_tpool_stats_get(&st);
    MT_CHECK_EQ_U64(st.inline_batches, inline_before + 1u);

    /* 3) 池内任务再发并行调用 → nested_fallbacks 递增且任务全部恰好一次 */
    static atomic_uint nested_cnt;
    atomic_store_explicit(&nested_cnt, 0u, memory_order_relaxed);
    tc_job inner[4];
    for (uint32_t i = 0; i < 4u; ++i) {
        inner[i].fn = bump_job;
        inner[i].ctx = &nested_cnt;
    }
    nested_arg nas[8];
    for (uint32_t i = 0; i < 8u; ++i) {
        nas[i].jobs = inner;
        nas[i].n = 4u;
    }
    tc_job outer_pool[8];
    for (uint32_t i = 0; i < 8u; ++i) {
        outer_pool[i].fn = nested_job;
        outer_pool[i].ctx = &nas[i];
    }
    uint64_t nested_before = st.nested_fallbacks;
    (void)tc_parallel_for(outer_pool, 8u, 4u);
    MT_CHECK_EQ_U64(atomic_load(&nested_cnt), 8u * 4u);
    tc_dev_tpool_stats_get(&st);
    MT_CHECK(st.nested_fallbacks > nested_before);

    MT_MAIN_RETURN();
}

/* P1-19：worker 内嵌套调用返回后，外层任务的相对槽位不变；
 * fork 后（已用池）子进程池惰性重建、任务恰好一次、槽位契约不破。 */
#if !defined(_WIN32)

typedef struct {
    atomic_uint* counter;
    atomic_uint* slot_before;
    atomic_uint* slot_after;
} slot_ctx;

static void nested_slot_job(void* p)
{
    slot_ctx* c = (slot_ctx*)p;
    atomic_store_explicit(c->slot_before, tc_pool_worker_slot(),
                          memory_order_relaxed);
    tc_job inner[2];
    for (uint32_t i = 0; i < 2u; ++i) {
        inner[i].fn = bump_job;
        inner[i].ctx = c->counter;
    }
    (void)tc_parallel_for(inner, 2u, 4u); /* worker 内嵌套 → 串行回退 */
    atomic_store_explicit(c->slot_after, tc_pool_worker_slot(),
                          memory_order_relaxed);
}

static void run_nested_slot_probe(void)
{
    static atomic_uint cnt, sb, sa;
    atomic_store_explicit(&cnt, 0u, memory_order_relaxed);
    atomic_store_explicit(&sb, 0u, memory_order_relaxed);
    atomic_store_explicit(&sa, 0u, memory_order_relaxed);
    slot_ctx c;
    c.counter = &cnt;
    c.slot_before = &sb;
    c.slot_after = &sa;
    tc_job jobs[8];
    for (uint32_t i = 0; i < 8u; ++i) {
        jobs[i].fn = nested_slot_job;
        jobs[i].ctx = &c;
    }
    (void)tc_parallel_for(jobs, 8u, 4u);
    _exit(atomic_load_explicit(&cnt, memory_order_relaxed) == 16u &&
          atomic_load_explicit(&sb, memory_order_relaxed) ==
          atomic_load_explicit(&sa, memory_order_relaxed) &&
          atomic_load_explicit(&sb, memory_order_relaxed) != 0u ? 0 : 1);
}

static void test_pool_fork_and_nested_slot(void)
{
    tc_dev_set_thread_count(4);
    tc_dev_tpool_stats_reset();

    /* 嵌套槽位不变（父进程直接验证；失败时 MT_CHECK 报告非 _exit） */
    static atomic_uint cnt, sb, sa;
    atomic_store_explicit(&cnt, 0u, memory_order_relaxed);
    atomic_store_explicit(&sb, 0u, memory_order_relaxed);
    atomic_store_explicit(&sa, 0u, memory_order_relaxed);
    slot_ctx c;
    c.counter = &cnt;
    c.slot_before = &sb;
    c.slot_after = &sa;
    tc_job jobs[8];
    for (uint32_t i = 0; i < 8u; ++i) {
        jobs[i].fn = nested_slot_job;
        jobs[i].ctx = &c;
    }
    (void)tc_parallel_for(jobs, 8u, 4u);
    MT_CHECK_EQ_U64(atomic_load(&cnt), 16u);
    MT_CHECK(atomic_load(&sb) != 0u); /* 确实有任务在 worker 上执行 */
    MT_CHECK(atomic_load(&sb) == atomic_load(&sa));

    /* fork（worker 存活状态下）→ 子进程惰性重建；重复 fork 同样干净 */
    static atomic_uint warm;
    atomic_store_explicit(&warm, 0u, memory_order_relaxed);
    tc_job w[8];
    for (uint32_t i = 0; i < 8u; ++i) {
        w[i].fn = bump_job;
        w[i].ctx = &warm;
    }
    (void)tc_parallel_for(w, 8u, 4u);
    MT_CHECK_EQ_U64(atomic_load(&warm), 8u);
    for (int round = 0; round < 2; ++round) {
        const pid_t pid = fork();
        MT_CHECK(pid >= 0);
        if (pid == 0) { run_nested_slot_probe(); } /* 子进程 _exit 探针 */
        int status = -1;
        waitpid(pid, &status, 0);
        MT_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

#endif /* !_WIN32 */

