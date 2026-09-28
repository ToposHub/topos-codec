#include "common/alloc.h"
#include "bitio.h"

#include <stdlib.h>
#include <string.h>

#include "../common/checked.h"
#include "../common/endian.h"

/* ---- 读端（M5：64-bit reservoir 实现；对外语义与旧逐窗口版完全一致） ---- */

void tc_bitreader_init(tc_bitreader* br, const void* data, size_t byte_size)
{
    br->data = (const uint8_t*)data;
    br->bit_size = (uint64_t)byte_size * 8u;
    br->bit_pos = 0;
    br->error = 0;
    br->cache = 0;
    br->bits = 0;
    br->next = br->data;
}

int32_t tc_bitreader_read_bit(tc_bitreader* br, uint32_t* out_bit)
{
    if (br->error != 0) { return br->error; }
    tc_bitreader_refill(br);
    if (br->bits < 1u) {
        br->error = TC_ERR_TRUNCATED;
        return TC_ERR_TRUNCATED;
    }
    *out_bit = (uint32_t)(br->cache >> 63);
    tc_bitreader_skip(br, 1u);
    return TC_OK;
}

int32_t tc_bitreader_read_bits(tc_bitreader* br, uint32_t nbits, uint32_t* out)
{
    if (br->error != 0) { return br->error; }
    if (nbits > 32u) { return TC_ERR_INVALID_ARGUMENT; }
    if (nbits == 0u) { *out = 0u; return TC_OK; }
    tc_bitreader_refill(br);
    if (br->bits < nbits) {
        br->error = TC_ERR_TRUNCATED;
        return TC_ERR_TRUNCATED;
    }
    *out = (uint32_t)(br->cache >> (64u - nbits));
    tc_bitreader_skip(br, nbits);
    return TC_OK;
}

uint64_t tc_bitreader_bits_consumed(const tc_bitreader* br)
{
    return br->bit_pos;
}

void tc_bitreader_fail(tc_bitreader* br, int32_t code)
{
    if (br->error == 0 && code != TC_OK) { br->error = code; }
}

int32_t tc_bitreader_align_byte(tc_bitreader* br)
{
    if (br->error != 0) { return br->error; }
    uint32_t r = (uint32_t)(br->bit_pos & 7u);
    if (r != 0u) {
        /* 填充位仍须位于 payload 内（末字节内的填充允许） */
        tc_bitreader_refill(br);
        if (br->bits < 8u - r) {
            br->error = TC_ERR_TRUNCATED;
            return TC_ERR_TRUNCATED;
        }
        tc_bitreader_skip(br, 8u - r);
    }
    return TC_OK;
}

/* ---- 写端（M6：128-bit reservoir 实现；对外语义与旧逐字节版完全一致） ---- */

static int32_t bw_ensure_bytes(tc_bitwriter* bw, size_t need_bytes)
{
    if (bw->error != 0) { return bw->error; }
    if (need_bytes <= bw->cap) { return TC_OK; }
    if ((uint64_t)need_bytes > (uint64_t)TC_BITWRITER_MAX_BYTES) {
        bw->error = TC_ERR_LIMIT_EXCEEDED;
        return TC_ERR_LIMIT_EXCEEDED;
    }
    size_t new_cap = bw->cap > 0 ? bw->cap : 4096u;
    while (new_cap < need_bytes) {
        if (!tc_umul_size(new_cap, 2u, &new_cap)) {
            bw->error = TC_ERR_LIMIT_EXCEEDED;
            return TC_ERR_LIMIT_EXCEEDED;
        }
    }
    uint8_t* grown = (uint8_t*)tc_realloc(bw->buf, new_cap);
    if (grown == NULL) {
        bw->error = TC_ERR_OUT_OF_MEMORY;
        return TC_ERR_OUT_OF_MEMORY;
    }
    bw->buf = grown;
    bw->cap = new_cap;
    return TC_OK;
}

/* reservoir 追加（nbits ∈ [1,32]；调用方保证容量已就绪或本次不触发提交） */
static void bw_append(tc_bitwriter* bw, uint32_t nbits, uint64_t v)
{
    bw->hold_hi = (bw->hold_hi << nbits) | (bw->hold_lo >> (64u - nbits));
    bw->hold_lo = (bw->hold_lo << nbits) | v;
    bw->hold_bits += nbits;
}

/* 提交最老的 64 个有效位为一个 64-bit 大端字（hold_bits ≥ 64 时调用；
 * s = 提交后剩余位数 ∈ [0,63]（M10-6.3C 起 64 位 append 可使 s > 31），
 * 剩余位全部位于 hold_lo 低位） */
static int32_t bw_commit_word(tc_bitwriter* bw)
{
    uint32_t s = bw->hold_bits - 64u;
    uint64_t word = s == 0u ? bw->hold_lo
                            : ((bw->hold_hi << (64u - s)) | (bw->hold_lo >> s));
    size_t need = bw->bytes + 8u;
    int32_t rc = bw_ensure_bytes(bw, need);
    if (rc != TC_OK) { return rc; }
    tc_store_be64(bw->buf + bw->bytes, word);
    bw->bytes += 8u;
    bw->hold_hi = 0u;
    bw->hold_lo = s == 0u ? 0u : (bw->hold_lo & (((uint64_t)1 << s) - 1u));
    bw->hold_bits = s;
    return TC_OK;
}

/* 将 reservoir 中已凑齐的完整字节全部提交（终态 hold_bits < 8；提交整字后
 * hi == 0，尾部整字节直接从 hold_lo 高端提取） */
static int32_t bw_flush(tc_bitwriter* bw)
{
    int32_t rc = TC_OK;
    while (bw->hold_bits >= 64u) {
        rc = bw_commit_word(bw);
        if (rc != TC_OK) { return rc; }
    }
    while (bw->hold_bits >= 8u) { /* 尾部整字节（≤7 个） */
        rc = bw_ensure_bytes(bw, bw->bytes + 1u);
        if (rc != TC_OK) { return rc; }
        /* 只读高位、只递减计数：寄存器内容保留（与旧版语义一致，flush 后
         * 理论上不再追加，残留位也永不进入提取窗口） */
        bw->buf[bw->bytes++] = (uint8_t)(bw->hold_lo >> (bw->hold_bits - 8u));
        bw->hold_bits -= 8u;
    }
    return rc;
}

int32_t tc_bitwriter_init(tc_bitwriter* bw)
{
    bw->buf = NULL;
    bw->cap = 0;
    bw->bytes = 0;
    bw->hold_hi = 0;
    bw->hold_lo = 0;
    bw->hold_bits = 0;
    bw->error = 0;
    /* 初始容量 64 KiB（与逐位版历史行为一致） */
    return bw_ensure_bytes(bw, 64u * 1024u);
}

void tc_bitwriter_free(tc_bitwriter* bw)
{
    tc_free(bw->buf);
    bw->buf = NULL;
    bw->cap = 0;
    bw->bytes = 0;
    bw->hold_hi = 0;
    bw->hold_lo = 0;
    bw->hold_bits = 0;
    bw->error = 0;
}

void tc_bitwriter_reset(tc_bitwriter* bw)
{
    bw->bytes = 0;
    bw->hold_hi = 0;
    bw->hold_lo = 0;
    bw->hold_bits = 0;
    bw->error = 0;
}

/* 外部写入统一路径：容量先保证（append 至多触发一次 8 字节提交），再进 reservoir */
static int32_t bw_put(tc_bitwriter* bw, uint32_t nbits, uint32_t value)
{
    int32_t rc = bw_ensure_bytes(bw, bw->bytes + 8u);
    if (rc != TC_OK) { return rc; }
    uint64_t v = (uint64_t)(value & ((nbits == 32u)
                       ? 0xFFFFFFFFu : ((1u << nbits) - 1u)));
    bw_append(bw, nbits, v);
    if (bw->hold_bits >= 64u) { return bw_commit_word(bw); }
    return TC_OK;
}

int32_t tc_bitwriter_put_bit(tc_bitwriter* bw, uint32_t bit)
{
    if (bw->error != 0) { return bw->error; }
    return bw_put(bw, 1u, bit & 1u);
}

int32_t tc_bitwriter_put_bits(tc_bitwriter* bw, uint32_t nbits, uint32_t value)
{
    if (nbits > 32u) { return TC_ERR_INVALID_ARGUMENT; }
    if (nbits == 0u) { return TC_OK; }
    if (bw->error != 0) { return bw->error; }
    return bw_put(bw, nbits, value);
}

/* M10-6.3C：64 位通路（外部统一路径；nbits ∈ [1,64]，至多一次提交） */
int32_t tc_bitwriter_put_bits64(tc_bitwriter* bw, uint32_t nbits, uint64_t value)
{
    if (nbits > 64u) { return TC_ERR_INVALID_ARGUMENT; }
    if (nbits == 0u) { return TC_OK; }
    if (bw->error != 0) { return bw->error; }
    int32_t rc = bw_ensure_bytes(bw, bw->bytes + 8u);
    if (rc != TC_OK) { return rc; }
    if (nbits == 64u) {
        bw->hold_hi = bw->hold_lo;
        bw->hold_lo = value;
    } else {
        const uint64_t v = value & (((uint64_t)1 << nbits) - 1u);
        bw->hold_hi = (bw->hold_hi << nbits) | (bw->hold_lo >> (64u - nbits));
        bw->hold_lo = (bw->hold_lo << nbits) | v;
    }
    bw->hold_bits += nbits;
    if (bw->hold_bits >= 64u) { return bw_commit_word(bw); }
    return TC_OK;
}

int32_t tc_bitwriter_put_bytes(tc_bitwriter* bw, const void* src, size_t n)
{
    if (n == 0u) { return TC_OK; }
    if (bw->error != 0) { return bw->error; }
    if (bw->hold_bits != 0u) {
        const uint8_t* p = (const uint8_t*)src;
        for (size_t i = 0u; i < n; ++i) {
            const int32_t rc = bw_put(bw, 8u, p[i]);
            if (rc != TC_OK) { return rc; }
        }
        return TC_OK;
    }
    const int32_t rc = bw_ensure_bytes(bw, bw->bytes + n);
    if (rc != TC_OK) { return rc; }
    memcpy(bw->buf + bw->bytes, src, n);
    bw->bytes += n;
    return TC_OK;
}

int32_t tc_bitwriter_flush_zero_pad(tc_bitwriter* bw)
{
    if (bw->error != 0) { return bw->error; }
    uint32_t rem = bw->hold_bits & 7u;
    if (rem != 0u) {
        /* 低位补零凑整字节（值不变：低有效位之外的位不参与提取） */
        uint32_t z = 8u - rem;
        bw_append(bw, z, 0u);
    }
    return bw_flush(bw);
}

const uint8_t* tc_bitwriter_data(const tc_bitwriter* bw) /* 失败/未写为 NULL 安全 */
{
    return bw->buf;
}

size_t tc_bitwriter_byte_size(const tc_bitwriter* bw) /* 语义要求先 flush */
{
    return bw->bytes; /* flush 不变量下与 floor(总位/8) 一致 */
}

uint64_t tc_bitwriter_bits_written(const tc_bitwriter* bw)
{
    return (uint64_t)bw->bytes * 8u + (uint64_t)bw->hold_bits;
}
