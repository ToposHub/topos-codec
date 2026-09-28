#include "common/alloc.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

/* 分配计数与故障点（0 = 未启用故障注入） */
static atomic_int_fast64_t s_alloc_count = 0;
static atomic_int_fast64_t s_alloc_fault_at = 0;

/* 故障判定与计数在同一原子操作内完成（fetch_add 先取旧值再 +1，
 * 第 fail_at 次分配的旧值恰为 fail_at-1）。 */
static int fault_hit(void)
{
    /* 无论是否武装都计数：tc_dev_alloc_count() 才能测出干净运行的总分配数 */
    int64_t prev = atomic_fetch_add_explicit(&s_alloc_count, 1, memory_order_relaxed);
    int64_t fail_at = atomic_load_explicit(&s_alloc_fault_at, memory_order_relaxed);
    return (fail_at > 0 && prev == fail_at - 1) ? 1 : 0;
}

void* tc_alloc(size_t n)
{
    if (n == 0u) { n = 1u; } /* 与测试 NULL 解引用隔离：0 字节也给出可 free 的指针 */
    if (fault_hit()) { return NULL; }
    return malloc(n);
}

void* tc_calloc(size_t n, size_t size)
{
    if (n == 0u) { n = 1u; }
    if (size == 0u) { size = 1u; }
    if (n > SIZE_MAX / size) { return NULL; }
    if (fault_hit()) { return NULL; }
    return calloc(n, size);
}

void* tc_realloc(void* p, size_t n)
{
    if (n == 0u) { n = 1u; }
    if (fault_hit()) { return NULL; }
    return realloc(p, n);
}

void tc_free(void* p)
{
    free(p);
}

void* tc_alloc_aligned(size_t n, size_t align)
{
    if (n == 0u) { n = 1u; }
    if (align < sizeof(void*)) { align = sizeof(void*); }
    if ((align & (align - 1u)) != 0u) { return NULL; } /* 须为 2 的幂 */
    if (fault_hit()) { return NULL; }
#if defined(_WIN32)
    return _aligned_malloc(n, align);
#else
    void* p = NULL;
    if (posix_memalign(&p, align, n) != 0) { return NULL; }
    return p;
#endif
}

void tc_free_aligned(void* p)
{
#if defined(_WIN32)
    _aligned_free(p);
#else
    free(p);
#endif
}

int64_t tc_dev_set_alloc_fault(int64_t fail_at)
{
    int64_t prev = atomic_exchange(&s_alloc_fault_at, fail_at);
    /* 每次设置都清零计数：武装后的下一次操作从第 1 次分配数起（sweep 对齐） */
    atomic_store(&s_alloc_count, 0);
    return prev;
}

int64_t tc_dev_alloc_count(void)
{
    return atomic_load(&s_alloc_count);
}
