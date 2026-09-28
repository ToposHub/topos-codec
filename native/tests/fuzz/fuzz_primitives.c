/* fuzz 阶段 1 target：对安全基础设施（checked/bufview/endian/status）做
 * 对抗性输入验证。真实位流 fuzz target 在阶段 3 接入。
 *
 * 返回 -1 表示发现不变量破坏（libFuzzer 视为 crash）。
 */
#include <stddef.h>
#include <stdint.h>

#include "common/bufview.h"
#include "common/checked.h"
#include "common/endian.h"
#include "topos_codec.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 16) { return 0; }

    const uint64_t seed = tc_load_be64(data);

    /* 从输入派生视图与偏移——全部经由 checked 原语消费 */
    const size_t view_size = (size_t)(seed % (uint64_t)(size - 8));
    tc_bufview v = tc_bufview_make(data + 8, view_size);

    for (int i = 0; i < 8; ++i) {
        /* data 内安全取 8 字节（索引取模保持在 [0, size-8]） */
        const size_t idx8 = (size_t)(((uint64_t)(unsigned)i * 8ull) % (uint64_t)(size - 7));
        const uint64_t r = tc_load_be64(data + idx8);
        const size_t off = (size_t)(r >> 11);
        const size_t len = (size_t)(r >> 33);

        const void* ptr = NULL;
        if (tc_bufview_read(&v, off, len, &ptr) && ptr != NULL) {
            const uint8_t* p = (const uint8_t*)ptr;
            if (p < v.data) { return -1; }
            if ((size_t)(p - v.data) != off) { return -1; }
            if (!tc_offset_in_bounds(v.size, off, len)) { return -1; }
        }

        tc_bufview sub;
        if (tc_bufview_slice(&v, off, len, &sub)) {
            if (sub.size != len) { return -1; }
            if (v.data != NULL && sub.data != v.data + off) { return -1; }
        }

        tc_bufview rest;
        if (tc_bufview_remaining(&v, off, &rest)) {
            if (rest.size != v.size - off) { return -1; }
        }

        uint32_t m32 = 0;
        uint64_t s64 = 0;
        size_t msz = 0;
        (void)tc_umul_u32((uint32_t)r, (uint32_t)(r >> 32), &m32);
        (void)tc_uadd_u64(r, r, &s64);
        (void)tc_umul_size((size_t)r, (size_t)(r >> 40), &msz);
    }

    /* 视图内安全偏移上的 endian 读取 */
    if (v.size >= 4) {
        const size_t o4 = (size_t)(seed % (uint64_t)(v.size - 3));
        (void)tc_load_be32(v.data + o4);
    }
    if (v.size >= 2) {
        const size_t o2 = (size_t)(seed % (uint64_t)(v.size - 1));
        (void)tc_load_be16(v.data + o2);
    }

    /* 随机状态码必须映射到有效静态串 */
    const char* msg = tc_status_message((int32_t)(uint32_t)(seed >> 32));
    if (msg == NULL || msg[0] == '\0') { return -1; }

    return 0;
}
