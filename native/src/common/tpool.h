/* 有界并行原语（R6 常驻池化；复验 P3 文档纠偏）—— slice 级线程并行唯一支撑。
 *
 * 设计（ADR-C009 spawn-per-call → ADR-C022 常驻池）：
 *  - 常驻有界 worker 池（grow-only，上限 min(n, max_workers) ≤
 *    TC_SLICE_MAX_THREADS）：一次性初始化经 pthread_once /
 *    InitOnceExecuteOnce（复验 P1-15——并发首调不得重复 init 同步原语），
 *    atexit join + pthread_atfork 三段式（fork 后惰性重建）；
 *  - 任务分发（解码深化批次）：共享原子游标动态领取（每任务恰好执行一次，
 *    执行线程不定；结果由调用方按索引汇合——对 slice 大小悬殊的批次消除
 *    静态条带的尾部失衡）；任务私有槽位资源以 tc_pool_worker_slot() 取槽；
 *  - 批次环（64 槽）满 / 初始化失败 / OOM / worker 内嵌套调用 → 调用线程
 *    顺序内联执行（正确性优先；环满即调用方回压——不排队、不阻塞他人）；
 *  - 批次内存双条件释放（全部 worker 越过 ∧ 调用方已离开），无 UAF/泄漏；
 *  - 上界 TC_SLICE_MAX_THREADS=16（M10-6：旧上界 8 在 16~24 逻辑核机器上
 *    白锁一半算力——编码 t8→t16 实测零增益的根因）；默认
 *    min(ncpu, TC_SLICE_MAX_THREADS)（16 逻辑核机 = 16；2026-09-02 静机交错
 *    对复验追认，详见 tpool.c thread_default 注释）；TOPOS_SLICE_THREADS
 *    可覆盖（1..16）；应用层自有线程池时可用
 *    tc_dev_set_thread_count(1) 关闭，避免重复并行（计划 §12 阶段 9 任务）。
 *    Win32 镜像（SRWLOCK + CONDITION_VARIABLE + _beginthreadex）。
 */
#ifndef TOPOS_INTERNAL_TPOOL_H
#define TOPOS_INTERNAL_TPOOL_H

#include <stdint.h>

#define TC_SLICE_MAX_THREADS 16

typedef struct tc_job {
    void (*fn)(void* ctx);
    void* ctx;
} tc_job;

/* 并行执行 jobs[0..n)（动态领取分发）。
 * 返回实际参与线程数（含调用线程）；n ≤ 1 或 workers ≤ 1 时顺序内联。 */
uint32_t tc_parallel_for(const tc_job* jobs, uint32_t n, uint32_t max_workers);

/* 当前线程的池内槽位：0 = 调用线程（或无线程平台）；1..nthreads = worker。
 * 供按 worker 私有的槽位资源（解码 alpha 行缓冲）取槽——动态领取分发后
 * 任务序号不再与 worker 绑定，槽位须以执行线程标识。 */
uint32_t tc_pool_worker_slot(void);

/* 线程上限配置（dev/内部 API；1 = 关闭并行） */
void tc_dev_set_thread_count(int32_t n);
int32_t tc_dev_thread_count(void);

/* P1-07：池退化 telemetry（dev；全 relaxed 原子计数，每批次一次加法）。
 * 退化路径本身语义不变（正确性优先的顺序内联），但必须可观测——否则
 * 并发压力下的 p95 断崖无从归因。健康判据：ring_full/nested/init_fail
 * 在产品路径上应恒为 0 或仅在已知压力窗口出现。 */
typedef struct tc_tpool_stats {
    uint64_t pool_batches;       /* 经池并行执行的批次数 */
    uint64_t inline_batches;     /* 顺序内联批次数（任务数/线程数 ≤1——正常） */
    uint64_t nested_fallbacks;   /* worker 内嵌套调用 → 串行（死锁防护触发） */
    uint64_t ring_full_fallbacks;/* 批次环满（≥64 outstanding）→ 串行 */
    uint64_t init_fail_fallbacks;/* 池初始化失败 → 串行 */
    uint64_t grow_fail_batches;  /* worker 增长失败、以退化档入池的批次数 */
    uint64_t workers_max;        /* 池内 worker 峰值（观测用快照） */
} tc_tpool_stats;

void tc_dev_tpool_stats_get(tc_tpool_stats* out);
void tc_dev_tpool_stats_reset(void);

#endif /* TOPOS_INTERNAL_TPOOL_H */
