/* endian helpers —— 内部：位流规范规定多字节字段 big-endian（spec §2）。
 *
 * 全部逐字节组装：天然支持非对齐地址、无严格别名/对齐 UB，
 * 不依赖编译器 builtin（三平台可移植，阶段 10 构建矩阵的承诺基础）。
 */
#ifndef TOPOS_INTERNAL_ENDIAN_H
#define TOPOS_INTERNAL_ENDIAN_H

#include <stdint.h>

static inline uint16_t tc_load_be16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t tc_load_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline uint64_t tc_load_be64(const uint8_t* p)
{
    return ((uint64_t)tc_load_be32(p) << 32) | (uint64_t)tc_load_be32(p + 4);
}

static inline void tc_store_be16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void tc_store_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void tc_store_be64(uint8_t* p, uint64_t v)
{
    tc_store_be32(p, (uint32_t)(v >> 32));
    tc_store_be32(p + 4, (uint32_t)v);
}

/* 主机字节序标记（诊断/文档用；规范性路径不依赖主机序） */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define TOPOS_HOST_LITTLE_ENDIAN 1
#elif defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define TOPOS_HOST_BIG_ENDIAN 1
#endif

#endif /* TOPOS_INTERNAL_ENDIAN_H */
