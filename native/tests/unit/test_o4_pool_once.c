/* O4（复验 2026-08-31）P1-15：线程池首次并发初始化竞争。
 *
 * 独立可执行文件：main 中前两个动作 = 两个 pthread 同时首次调用
 * tc_parallel_for（此前任何路径都不得预热池）。旧实现的普通
 * static int registered / 无锁 pool_init 在此场景下可并发重复
 * init 同一 mutex/cond 并重复注册 atexit/atfork（TSan 可见）；
 * pthread_once / InitOnceExecuteOnce 修复后必须：
 *   1. 两调用方 job 全部执行恰好一次；
 *   2. 退出码干净（atexit join 正常）。
 * 在 tsan 配置下运行即构成数据竞争回归门。 */
#include "common/tpool.h"
#include "mini_test.h"

#include "../support/port_thread.h"
#include <stdatomic.h>
#include <stdlib.h>

typedef struct {
    atomic_uint* counter;
    uint32_t expect;
} o4_job_ctx;

static void o4_job(void* p)
{
    o4_job_ctx* c = (o4_job_ctx*)p;
    atomic_fetch_add(c->counter, 1u);
}

typedef struct {
    tc_job* jobs;
    uint32_t n;
    uint32_t nw;
} o4_caller;

static void* o4_caller_main(void* arg)
{
    o4_caller* c = (o4_caller*)arg;
    (void)tc_parallel_for(c->jobs, c->n, c->nw);
    return NULL;
}

#define O4_N 64u

int main(void)
{
    static tc_job jobs_a[O4_N], jobs_b[O4_N];
    static o4_job_ctx ctx_a[O4_N], ctx_b[O4_N];
    static atomic_uint cnt_a, cnt_b;
    atomic_store(&cnt_a, 0u);
    atomic_store(&cnt_b, 0u);
    for (uint32_t i = 0u; i < O4_N; ++i) {
        ctx_a[i].counter = &cnt_a; ctx_a[i].expect = 1u;
        ctx_b[i].counter = &cnt_b; ctx_b[i].expect = 1u;
        jobs_a[i].fn = o4_job; jobs_a[i].ctx = &ctx_a[i];
        jobs_b[i].fn = o4_job; jobs_b[i].ctx = &ctx_b[i];
    }
    tc_dev_set_thread_count(4);

    o4_caller ca = { jobs_a, O4_N, 4u };
    o4_caller cb = { jobs_b, O4_N, 4u };
    pthread_t ta, tb;
    if (pthread_create(&ta, NULL, o4_caller_main, &ca) != 0 ||
        pthread_create(&tb, NULL, o4_caller_main, &cb) != 0) {
        mt_report(__FILE__, __LINE__, "pthread_create failed");
        return MT_MAIN_RETURN();
    }
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    MT_CHECK_EQ_U64(atomic_load(&cnt_a), O4_N);
    MT_CHECK_EQ_U64(atomic_load(&cnt_b), O4_N);
    return MT_MAIN_RETURN();
}
