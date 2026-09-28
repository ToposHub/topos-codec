/* buffer view —— 内部：对一段不可变字节的受检只读视图。
 *
 * 语义：所有读取先经 tc_offset_in_bounds；data == NULL 的视图一律读取失败
 * （即使 size == 0），杜绝把 NULL+0 合法化的歧义。
 */
#ifndef TOPOS_INTERNAL_BUFVIEW_H
#define TOPOS_INTERNAL_BUFVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "checked.h"

typedef struct tc_bufview {
    const uint8_t* data;
    size_t         size;
} tc_bufview;

static inline tc_bufview tc_bufview_make(const void* data, size_t size)
{
    tc_bufview v;
    v.data = (const uint8_t*)data;
    v.size = size;
    return v;
}

static inline bool tc_bufview_read(const tc_bufview* v, size_t off, size_t len,
                                   const void** out)
{
    if (v == NULL || v->data == NULL) { return false; }
    if (!tc_offset_in_bounds(v->size, off, len)) { return false; }
    *out = (const void*)(v->data + off);
    return true;
}

static inline bool tc_bufview_slice(const tc_bufview* v, size_t off, size_t len,
                                    tc_bufview* out)
{
    if (v == NULL || v->data == NULL || out == NULL) { return false; }
    if (!tc_offset_in_bounds(v->size, off, len)) { return false; }
    out->data = v->data + off;
    out->size = len;
    return true;
}

/* 从 off 到末尾的剩余视图 */
static inline bool tc_bufview_remaining(const tc_bufview* v, size_t off,
                                        tc_bufview* out)
{
    if (v == NULL || v->data == NULL || out == NULL) { return false; }
    if (off > v->size) { return false; }
    out->data = v->data + off;
    out->size = v->size - off;
    return true;
}

#endif /* TOPOS_INTERNAL_BUFVIEW_H */
