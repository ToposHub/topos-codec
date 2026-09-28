#include "fault_io.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* 调用计数原子：同一 movie 可被多线程并发读（topos_io ctx 由调用方保证安全；
 * 本测试实现用原子计数满足该契约，TSan 下干净） */
static int32_t tio_fault_check(int64_t fail_from, atomic_int_fast64_t* calls)
{
    int64_t n = atomic_fetch_add_explicit(calls, 1, memory_order_relaxed) + 1;
    if (fail_from >= 0 && n >= fail_from) { return TC_ERR_IO; }
    return TC_OK;
}

static int32_t tio_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
    tio_source* s = (tio_source*)ctx;
    int32_t rc = tio_fault_check(s->fault->fail_read_from, &s->fault->read_calls);
    if (rc != TC_OK) { return rc; }
    if (len == 0u) { return TC_OK; }
    if (offset > (uint64_t)s->len || (size_t)((uint64_t)s->len - offset) < len) {
        return TC_ERR_IO; /* EOF 越界 → 等价短读 */
    }
    memcpy(buf, s->data + (size_t)offset, len);
    return TC_OK;
}

static int32_t sink_grow(tio_sink* s, size_t need)
{
    if (need <= s->cap) { return TC_OK; }
    size_t cap = s->cap != 0u ? s->cap : 4096u;
    while (cap < need) { cap *= 2u; }
    uint8_t* p = (uint8_t*)realloc(s->data, cap);
    if (p == NULL) { return TC_ERR_OUT_OF_MEMORY; }
    s->data = p;
    s->cap = cap;
    return TC_OK;
}

static int32_t tio_write(void* ctx, const void* data, size_t len)
{
    tio_sink* s = (tio_sink*)ctx;
    int32_t rc = tio_fault_check(s->fault->fail_write_from, &s->fault->write_calls);
    if (rc != TC_OK) { return rc; }
    if (len == 0u) { return TC_OK; }
    if (sink_grow(s, s->len + len) != TC_OK) { return TC_ERR_OUT_OF_MEMORY; }
    memcpy(s->data + s->len, data, len);
    s->len += len;
    return TC_OK;
}

static int32_t tio_seek_write(void* ctx, uint64_t offset, const void* data, size_t len)
{
    tio_sink* s = (tio_sink*)ctx;
    int32_t rc = tio_fault_check(s->fault->fail_seek_write_from,
                                 &s->fault->seek_write_calls);
    if (rc != TC_OK) { return rc; }
    if (len == 0u) { return TC_OK; }
    if (offset + (uint64_t)len > (uint64_t)s->len) { return TC_ERR_IO; /* 只允许覆写已写区域 */ }
    memcpy(s->data + (size_t)offset, data, len);
    return TC_OK;
}

void tio_source_init(topos_io* io, tio_source* s, const uint8_t* data, size_t len,
                     tio_fault* f)
{
    memset(io, 0, sizeof(*io));
    io->struct_size = (uint32_t)sizeof(*io);
    io->abi_version = TOPOS_CODEC_ABI_VERSION;
    io->ctx = s;
    io->read = tio_read;
    s->data = data;
    s->len = len;
    s->fault = f;
    io->length = (uint64_t)len;
}

void tio_sink_init(topos_io* io, tio_sink* s, tio_fault* f)
{
    memset(io, 0, sizeof(*io));
    io->struct_size = (uint32_t)sizeof(*io);
    io->abi_version = TOPOS_CODEC_ABI_VERSION;
    io->ctx = s;
    io->write = tio_write;
    io->seek_write = tio_seek_write;
    memset(s, 0, sizeof(*s));
    s->fault = f;
}

void tio_sink_reset(tio_sink* s)
{
    s->len = 0u;
}

void tio_sink_free(tio_sink* s)
{
    free(s->data);
    s->data = NULL;
    s->len = 0u;
    s->cap = 0u;
}
