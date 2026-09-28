#include "rice.h"

uint32_t tc_rice_map_signed(int32_t v)
{
    /* int64 中转：杜绝 INT32_MIN 取负等 UB（域内 |v| ≤ 2^30，2·|v| < 2^32） */
    int64_t s = (int64_t)v;
    if (s >= 0) { return (uint32_t)((uint64_t)s << 1); }
    uint64_t mag = (uint64_t)(-s);
    return (uint32_t)((mag << 1) - 1u);
}

int32_t tc_rice_unmap_signed(uint32_t m)
{
    uint64_t u = (uint64_t)m;
    if ((u & 1u) == 0u) { return (int32_t)(u / 2u); }
    return -(int32_t)((u + 1u) / 2u);
}

int32_t tc_rice_encode(tc_bitwriter* bw, uint32_t k, uint32_t m)
{
    if (k > (uint32_t)TC_RICE_K_MAX) { return TC_ERR_INVALID_ARGUMENT; }
    return tc_rice_encode_inline(bw, k, m);
}

/* ---- M5 §9.3：12-bit 前缀 LUT 构建与发布 ---- */

uint16_t tc_rice_short_lut[15][256];
uint16_t tc_rice_lut[15][4096];
atomic_int tc_rice_lut_ready[15];

#include "../common/port_mutex.h"

static topos_mutex g_lut_mu = TOPOS_MUTEX_INIT;

static void rice_lut_build_row(uint32_t k)
{
    for (uint32_t p = 0u; p < 256u; ++p) {
        uint32_t ones;
        if (p == 0xFFu) {
            ones = 8u;
        } else {
            uint32_t x = ~(p << 24);
#if defined(__GNUC__) || defined(__clang__)
            ones = (uint32_t)__builtin_clz(x);
#else
            ones = 0u;
            while ((x & 0x80000000u) == 0u) { x <<= 1; ones++; }
#endif
        }
        uint32_t total = ones + 1u + k;
        if (total > 8u) {
            tc_rice_short_lut[k][p] = (uint16_t)TC_RICE_LUT_SENTINEL;
            continue;
        }
        uint32_t rem = k == 0u ? 0u : (p >> (8u - total)) & ((1u << k) - 1u);
        uint32_t m = (ones << k) | rem;
        tc_rice_short_lut[k][p] = (uint16_t)((m << 4) | total);
    }
    for (uint32_t p = 0u; p < 4096u; ++p) {
        uint32_t ones;
        if (p == 0xFFFu) {
            ones = 12u; /* 窗口全 1：长 unary/escape 归慢路 */
        } else {
            uint32_t x = ~(p << 20);
#if defined(__GNUC__) || defined(__clang__)
            ones = (uint32_t)__builtin_clz(x); /* clz(0) 已被上式排除 */
#else
            ones = 0u;
            while ((x & 0x80000000u) == 0u) { x <<= 1; ones++; }
#endif
        }
        uint32_t total = ones + 1u + k;
        if (total > 12u) {
            tc_rice_lut[k][p] = (uint16_t)TC_RICE_LUT_SENTINEL;
            continue;
        }
        uint32_t rem = k == 0u ? 0u : (p >> (12u - total)) & ((1u << k) - 1u);
        uint32_t m = (ones << k) | rem;
        /* total ≤ 12 ⇒ m ≤ (12−k)·2^k−1 ≤ 2047 < 2^12：16 位 entry 无损 */
        tc_rice_lut[k][p] = (uint16_t)((m << 4) | total);
    }
}

int32_t tc_rice_lut_ensure(uint32_t k)
{
    if (k > (uint32_t)TC_RICE_K_MAX) { return TC_ERR_INVALID_ARGUMENT; }
    /* M10-1b：acquire 快路（与 tc_vlc_tables_ensure 同模式）——稳态（表已
     * 就绪）零锁返回。此前每个 Rice slice 三次无条件进全局 mutex，1080p/4K
     * 每帧 ~45/81 次获取且多 slice 启动时短暂串行。 */
    if (atomic_load_explicit(&tc_rice_lut_ready[k], memory_order_acquire) != 0) {
        return TC_OK;
    }
    topos_mutex_lock(&g_lut_mu);
    if (atomic_load_explicit(&tc_rice_lut_ready[k], memory_order_relaxed) == 0) {
        rice_lut_build_row(k);
        atomic_store_explicit(&tc_rice_lut_ready[k], 1, memory_order_release);
    }
    topos_mutex_unlock(&g_lut_mu);
    return TC_OK;
}

/* M5：实现移入 rice.h 内联版；外部函数保留为薄包装（非热路径/测试用） */
int32_t tc_rice_decode(tc_bitreader* br, uint32_t k, uint32_t m_max, uint32_t* m_out)
{
    return tc_rice_decode_inline(br, k, m_max, m_out);
}
