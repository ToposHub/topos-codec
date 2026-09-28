/* 内部分配路由（阶段 10）—— 库内全部动态分配经由本头。
 *
 * 目的：
 *  - OOM 故障注入（tc_dev_set_alloc_fault）：第 N 次分配返回 NULL，
 *    走调用方既有的 NULL 检查路径 → 全链路 TC_ERR_OUT_OF_MEMORY 传播可被
 *    穷举测试（test_oom）；
 *  - 未来分配策略挂钩（上限/池化）单点接入。
 *
 * 约定：
 *  - 语义与 malloc/calloc/realloc/free 完全一致（默认直通）；
 *  - 计数原子（线程安全）；故障注入建议仅在单线程测试路径使用；
 *  - dev/内部 API，不入公共 ABI（同 tc_dev_set_qmatrix_override 先例）。
 */
#ifndef TOPOS_INTERNAL_ALLOC_H
#define TOPOS_INTERNAL_ALLOC_H

#include <stddef.h>
#include <stdint.h>

void* tc_alloc(size_t n);
void* tc_calloc(size_t n, size_t size);
void* tc_realloc(void* p, size_t n);
void tc_free(void* p);

/* 对齐分配（M10-1：worker 槽位 scratch 池；M10-2：SIMD 批量内核缓冲）。
 * align 须为 2 的幂且 ≥ sizeof(void*)；释放必须配对 tc_free_aligned。 */
void* tc_alloc_aligned(size_t n, size_t align);
void tc_free_aligned(void* p);

/* OOM 故障注入：fail_at ≥ 1 时，设置之后的第 fail_at 次分配返回 NULL（恰好一次）；
 * 每次调用均清零计数。fail_at < 0 关闭。返回旧故障点。 */
int64_t tc_dev_set_alloc_fault(int64_t fail_at);

/* 当前分配计数（自最近一次 set_fault 起；测试据此判断已覆盖全部分配点） */
int64_t tc_dev_alloc_count(void);

#endif /* TOPOS_INTERNAL_ALLOC_H */
