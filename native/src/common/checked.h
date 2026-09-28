/* checked arithmetic —— 内部（src/common），全部 static inline、零分配。
 *
 * 阶段 1 完成门槛：核心库内不得出现未检查的外部 size/offset 算术；
 * 所有处理码流来源整数的加/乘/区间判断必须经由本头（fuzz + 单测覆盖）。
 *
 * 约定：返回 true = 成功且 *out 已写入；false = 溢出/越界且 *out 不被写入。
 */
#ifndef TOPOS_INTERNAL_CHECKED_H
#define TOPOS_INTERNAL_CHECKED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool tc_uadd_u32(uint32_t a, uint32_t b, uint32_t* out)
{
    uint32_t r = a + b;
    if (r < a) { return false; }
    *out = r;
    return true;
}

static inline bool tc_uadd_u64(uint64_t a, uint64_t b, uint64_t* out)
{
    uint64_t r = a + b;
    if (r < a) { return false; }
    *out = r;
    return true;
}

static inline bool tc_uadd_size(size_t a, size_t b, size_t* out)
{
    if (b > SIZE_MAX - a) { return false; }
    *out = a + b;
    return true;
}

static inline bool tc_usub_size(size_t a, size_t b, size_t* out)
{
    if (b > a) { return false; }
    *out = a - b;
    return true;
}

static inline bool tc_umul_u32(uint32_t a, uint32_t b, uint32_t* out)
{
    uint32_t r = a * b;
    if (a != 0u && r / a != b) { return false; }
    *out = r;
    return true;
}

static inline bool tc_umul_u64(uint64_t a, uint64_t b, uint64_t* out)
{
    uint64_t r = a * b;
    if (a != 0u && r / a != b) { return false; }
    *out = r;
    return true;
}

static inline bool tc_umul_size(size_t a, size_t b, size_t* out)
{
    size_t r = a * b;
    if (a != (size_t)0 && r / a != b) { return false; }
    *out = r;
    return true;
}

/* [off, off+len) 是否落在 [0, limit) 内 —— 无溢出构造（阶段 1 门禁的判据本体） */
static inline bool tc_offset_in_bounds(size_t limit, size_t off, size_t len)
{
    return off <= limit && len <= limit - off;
}

/* 最低置位位序（前置 x ≠ 0）。M10-6.3B：编码零掩码跳扫用。 */
static inline uint32_t tc_ctz64(uint64_t x)
{
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_ctzll(x);
#elif defined(_M_X64)
    unsigned long i = 0u;
    if (_BitScanForward64(&i, x)) { return (uint32_t)i; }
    return 64u;
#else
    uint32_t i = 0u;
    while ((x & 1u) == 0u) { x >>= 1; i++; }
    return i;
#endif
}

#endif /* TOPOS_INTERNAL_CHECKED_H */
