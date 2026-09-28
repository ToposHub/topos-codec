/* fault_io —— 阶段 10 健壮性测试的 IO 故障注入（topos_io 回调实现，测试侧）。
 *
 * 模拟：磁盘满/权限拒绝（write/read 从第 k 次调用起 TC_ERR_IO）、用户取消
 * （读中途开始失败）、EOF 越界读（TC_ERR_IO）。短读按库契约同样归为 IO 错误。
 *
 * 故意使用测试侧原生 malloc/free（不经 lib 的 tc_alloc 路由）：OOM 穷举
 * 只注入库内分配点，IO 缓冲增长不得混入计数。
 */
#ifndef TOPOS_TEST_FAULT_IO_H
#define TOPOS_TEST_FAULT_IO_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "topos_codec.h"

typedef struct tio_fault {
    int64_t fail_read_from;       /* 读调用计数 ≥ 此值 → TC_ERR_IO；<0 关闭 */
    int64_t fail_write_from;
    int64_t fail_seek_write_from;
    atomic_int_fast64_t read_calls, write_calls, seek_write_calls;
} tio_fault;

static inline void tio_fault_reset(tio_fault* f)
{
    f->fail_read_from = -1;
    f->fail_write_from = -1;
    f->fail_seek_write_from = -1;
    f->read_calls = 0;
    f->write_calls = 0;
    f->seek_write_calls = 0;
}

/* ---- 源：固定缓冲 ---- */
typedef struct tio_source {
    const uint8_t* data;
    size_t len;
    tio_fault* fault;
} tio_source;

/* io 与 s 均由调用方持有；io 回调引用 s（生命周期覆盖使用期） */
void tio_source_init(topos_io* io, tio_source* s, const uint8_t* data, size_t len,
                     tio_fault* f);

/* ---- 汇：可增长缓冲 ---- */
typedef struct tio_sink {
    uint8_t* data;
    size_t len, cap;
    tio_fault* fault;
} tio_sink;

void tio_sink_init(topos_io* io, tio_sink* s, tio_fault* f);
void tio_sink_reset(tio_sink* s); /* len=0，容量复用（迭代 sweep 用） */
void tio_sink_free(tio_sink* s);  /* 释放缓冲并清零 */

#endif /* TOPOS_TEST_FAULT_IO_H */
