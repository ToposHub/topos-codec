/* 有界 Rice 熵编码（spec §6，唯一熵方案，冻结）。
 *
 * - 有符号映射（§6.1）：m = v≥0 ? 2v : −2v−1；逆映射精确。
 * - 码字（§6.2）：q = m >> k；q ≤ 30 → q 个 '1' + '0' + k 位余数；q > 30 →
 *   31 个 '1'（escape）+ 32 位 m 字面值。unary 上限 31 为规范的一部分。
 * - 域校验：解码侧每符号立即校验 m ≤ m_max（按符号类型传入下列常量），
 *   超界 → TC_ERR_MALFORMED 并置 reader 粘滞错误（slice 级 concealment 输入）。
 */
#ifndef TOPOS_INTERNAL_RICE_H
#define TOPOS_INTERNAL_RICE_H

#include <stdatomic.h>
#include <stdint.h>

#include "../bitstream/bitio.h"

enum {
    TC_RICE_K_MIN = 0,
    TC_RICE_K_MAX = 14,
    TC_RICE_UNARY_LIMIT = 31 /* escape 标记 = 31 个连续 '1' */
};

/* 符号域上限（依据 spec §7.5/§7.6/§8.4 的中间值界推导，frozen）。
 * 映射 m=2|v|：上界取 2 的幂整（|v| ≤ 2^26 → m ≤ 2^27=0x08000000）。 */
#define TC_RICE_M_MAX_DC          0x08000000u /* 2^27：|DC 残差| ≤ 2^26 */
#define TC_RICE_M_MAX_AC_LEVEL    0x04000000u /* 2^26：|AC level| ≤ 2^25 */
#define TC_RICE_M_MAX_RUN         63u        /* 颜色 run；63 保留为 EOB */
#define TC_RICE_M_MAX_ALPHA_LEVEL 0x00020000u /* 2^17：|Alpha 残差| ≤ 65535（含终结 level=0） */

/* 有符号映射（前置：|v| ≤ 2^30，覆盖全部符号域并规避 2·|v| 溢出） */
uint32_t tc_rice_map_signed(int32_t v);
int32_t tc_rice_unmap_signed(uint32_t m);

/* ---- P0-07：DC 重建唯一入口（spec §7.6 系数域，C-24 补强） ----
 * DC 残差按符号有界（|v| ≤ 2^26），但 pred + 残差 的 int32 直接累加可被
 * 恶意码流逐块走出域界直至 UB 溢出。编码器合法输出恒满足 |q| ≤ 2^25
 * （spec §7.6「实现钳位 ±2^25」），故以该域为解码验收界：int64 重建 +
 * 域检查，越界返回 TC_ERR_MALFORMED（调用方令 bitreader 置粘滞错误，
 * slice 级 concealment）。全部解码路径（direct-scan / 参考 / VLC 变体）
 * 必须经此函数重建 DC，禁止各自修补。 */
#define TC_DC_DOMAIN_MAX (1 << 25)

/* ---- 批 4：位深感知符号域（bd≥13/16-bit 流） ----
 * rans/rans2 字母表结构容量（rans.h 冻结）：DC_CAT = bitlen(map(Δdc)) ∈
 * 0..28 → m = map < 2^28 → |Δdc| ≤ 2^27−1；LEVEL_CAT bitlen ∈ 0..27 →
 * m < 2^27 → |level| ≤ 2^26−1。12-bit 冻结值（上方）远窄于结构容量
 * （12-bit |q| ≤ 2^25 不可达，属验收域收紧）；bd16 量化系数 |q| = |F'|/Q
 * 在 Q≥8 时 ≤ f_clamp/8 ≈ 2^26，Q≥4 时贴 2^27 —— 按结构容量放行。
 * frame 头 minor=6 门禁保证旧读端 UNSUPPORTED_VERSION 干净拒绝。
 * V1/V2-Rice（block_coding 路径）保持 12-bit 冻结域，编码侧对 bd≥13
 * 显式拒绝（config validate），不自行动态扩域。 */
#define TC_RICE_M_MAX_DC_BD16        0x0FFFFFFFu /* 2^28−1：|Δdc| ≤ 2^27−1 */
#define TC_RICE_M_MAX_AC_LEVEL_BD16  0x07FFFFFFu /* 2^27−1：|AC level| ≤ 2^26−1 */
#define TC_DC_DOMAIN_MAX_BD16        (1 << 27)   /* DC q 域（pred 链 int64 校验） */

static inline uint32_t tc_rice_m_max_dc_bd(uint8_t bit_depth)
{
    return bit_depth > 12u ? TC_RICE_M_MAX_DC_BD16 : TC_RICE_M_MAX_DC;
}

static inline uint32_t tc_rice_m_max_level_bd(uint8_t bit_depth)
{
    return bit_depth > 12u ? TC_RICE_M_MAX_AC_LEVEL_BD16 : TC_RICE_M_MAX_AC_LEVEL;
}

static inline int32_t tc_dc_reconstruct_checked_bd(int32_t pred, uint32_t m,
                                                   uint8_t bit_depth, int32_t* out)
{
    const int64_t lim = (bit_depth > 12u) ? (int64_t)TC_DC_DOMAIN_MAX_BD16
                                           : (int64_t)TC_DC_DOMAIN_MAX;
    const int64_t v = (int64_t)pred + (int64_t)tc_rice_unmap_signed(m);
    if (v > lim || v < -lim) {
        return TC_ERR_MALFORMED;
    }
    *out = (int32_t)v;
    return TC_OK;
}

static inline int32_t tc_dc_reconstruct_checked(int32_t pred, uint32_t m,
                                                int32_t* out)
{
    return tc_dc_reconstruct_checked_bd(pred, m, 12u, out);
}

/* 编码非负 m（k ∈ 0..14，否则 INVALID_ARGUMENT；m 无额外上界 —— escape 字面值 32 位） */
int32_t tc_rice_encode(tc_bitwriter* bw, uint32_t k, uint32_t m);

/* ---- M6 内联编码（与外部函数逐位一致） ----
 * 供熵热循环（block_coding/alpha）内联使用，消除每符号跨 TU 调用；
 * 前置：k ≤ 14（调用方 k 来自 rice_k_estimate，域内保证）。 */

static inline int32_t tc_rice_encode_inline(tc_bitwriter* bw, uint32_t k, uint32_t m)
{
    uint32_t q = m >> k;
    if (q <= 30u) {
        /* q 个 '1' + '0' 的 unary 段 = 低 q+1 位的 (1<<(q+1))-2；q+1+k ≤ 32 时
         * 与 k 位余数融合为一次写入（阶段 9；位序与 golden_bitstream 冻结值一致） */
        uint32_t total = q + 1u + k;
        if (total <= 32u) {
            uint32_t v = (((1u << (q + 1u)) - 2u) << k) | (m & ((1u << k) - 1u));
            return tc_bitwriter_put_bits_inline(bw, total, v);
        }
        int32_t rc = tc_bitwriter_put_bits_inline(bw, q + 1u, (1u << (q + 1u)) - 2u);
        if (rc != TC_OK) { return rc; }
        if (k > 0u) {
            return tc_bitwriter_put_bits_inline(bw, k, m & ((1u << k) - 1u));
        }
        return TC_OK;
    }
    /* escape：31 个 '1' + 32 位字面值（单符号最长 63 位，spec §6.2） */
    int32_t rc = tc_bitwriter_put_bits_inline(bw, (uint32_t)TC_RICE_UNARY_LIMIT, 0x7FFFFFFFu);
    if (rc != TC_OK) { return rc; }
    return tc_bitwriter_put_bits_inline(bw, 32u, m);
}

/* ---- M10-6.3C：单符号完整码字（≤63 位，供 64 位融合发射） ----
 * 返回码长 nbits 并写 *val_out；位序列与 tc_rice_encode_inline 逐位一致
 * （q≤30：q 个 '1'+'0'+k 位余数一次拼装，长 unary 不再拆段；escape：
 * 31 个 '1' + 32 位字面值 = 63 位）。前置：k ≤ 14。 */
static inline uint32_t tc_rice_code_word(uint32_t k, uint32_t m, uint64_t* val_out)
{
    uint32_t q = m >> k;
    if (q <= 30u) {
        /* 先提升 u64 再左移：q+1+k 可达 45 位（u32 内左移会截断） */
        const uint64_t unary = ((uint64_t)((1u << (q + 1u)) - 2u)) << k;
        *val_out = unary | (m & (((uint32_t)1 << k) - 1u));
        return q + 1u + k;
    }
    *val_out = ((uint64_t)0x7FFFFFFFu << 32) | m;
    return (uint32_t)TC_RICE_UNARY_LIMIT + 32u;
}

/* ---- RD3-01：两级 Rice 前缀 LUT（k 行按需构建，进程级缓存）----
 * 一级是 8-bit short-code table，二级是 12-bit table；entry = uint16：
 * (m << 4) | nbits；sentinel 0xFFFF = 长 unary/escape/窗口不足，回退
 * reservoir+CLZ 慢路。一级命中可用更小的 512-byte 热行，二级用于
 * 9..12-bit 短码。二级命中域约束 total = q+1+k ≤ 12 ⇒
 * m ≤ (12−k)·2^k−1 ≤ 2047（k≤11 全域成立，k≥12 行恒 sentinel）、
 * nbits ≤ 12 < 16 ⇒ 16 位无损编码（解码深化批次：条目 32→16 位，
 * 每行 16KB→8KB；每 slice 仅 2~3 个 k（DC/level/run）→ 8-bit
 * 一级热工作集 1~1.5KB，二级按需 16~24KB。
 * 构建为纯函数（确定性），互斥下完成，release 发布保证无数据竞争。 */
#define TC_RICE_LUT_SENTINEL 0xFFFFu
extern uint16_t tc_rice_short_lut[15][256];
extern uint16_t tc_rice_lut[15][4096];
extern atomic_int tc_rice_lut_ready[15];
int32_t tc_rice_lut_ensure(uint32_t k);

/* ---- M5 内联解码（reservoir + CLZ；语义与外部函数逐位一致） ----
 * 供熵热循环（block_coding/alpha）内联使用，消除每符号跨 TU 调用并让
 * reader 状态跨符号驻留寄存器。 */

static inline int32_t tc_rice_unary_scan_inline(tc_bitreader* br, uint32_t* q_out)
{
    uint32_t q = 0u;
    for (;;) {
        tc_bitreader_refill(br);
        if (br->bits == 0u) {
            br->error = TC_ERR_TRUNCATED;
            return TC_ERR_TRUNCATED;
        }
        uint64_t inv = ~br->cache;
        uint32_t ones;
        if (inv == 0u) {
            ones = 64u;
        } else {
#if defined(__GNUC__) || defined(__clang__)
            ones = (uint32_t)__builtin_clzll(inv);
#else
            ones = 0u;
            while ((inv & 0x8000000000000000ull) == 0ull) { inv <<= 1; ones++; }
#endif
        }
        if (ones > br->bits) { ones = br->bits; }
        if (q + ones >= (uint32_t)TC_RICE_UNARY_LIMIT) {
            tc_bitreader_skip(br, (uint32_t)TC_RICE_UNARY_LIMIT - q);
            *q_out = (uint32_t)TC_RICE_UNARY_LIMIT;
            return TC_OK;
        }
        if (ones < br->bits) {
            tc_bitreader_skip(br, ones + 1u);
            *q_out = q + ones;
            return TC_OK;
        }
        tc_bitreader_skip(br, ones);
        q += ones;
    }
}

/* 调用方已通过 tc_rice_lut_ensure(k) 预热并持有两级表行指针时使用。
 * 这条路径不做 atomic acquire，适合一个 segment/slice 内的批量符号循环。 */
static inline int32_t tc_rice_decode_lut_inline(tc_bitreader* br,
                                                const uint16_t* short_lut_row,
                                                const uint16_t* lut_row,
                                                uint32_t k, uint32_t m_max,
                                                uint32_t* m_out)
{
    if (k > (uint32_t)TC_RICE_K_MAX) { return TC_ERR_INVALID_ARGUMENT; }
    if (br->error != 0) { return br->error; }

    /* 一级表先处理最常见的 <=8-bit code；二级表覆盖 9..12-bit code。 */
    if (br->bits < 8u) { tc_bitreader_refill(br); }
    if (br->bits >= 8u) {
        uint16_t e = short_lut_row[(uint32_t)(br->cache >> 56)];
        if (e != TC_RICE_LUT_SENTINEL) {
            uint32_t m = (uint32_t)e >> 4;
            if (m > m_max) {
                tc_bitreader_fail(br, TC_ERR_MALFORMED);
                return TC_ERR_MALFORMED;
            }
            tc_bitreader_skip(br, e & 0xFu);
            *m_out = m;
            return TC_OK;
        }
    }
    if (br->bits >= 12u) {
        uint16_t e = lut_row[(uint32_t)(br->cache >> 52)];
        if (e != TC_RICE_LUT_SENTINEL) {
            uint32_t m = (uint32_t)e >> 4;
            if (m > m_max) {
                tc_bitreader_fail(br, TC_ERR_MALFORMED);
                return TC_ERR_MALFORMED;
            }
            tc_bitreader_skip(br, e & 0xFu);
            *m_out = m;
            return TC_OK;
        }
    }

    uint32_t q = 0;
    int32_t rc = tc_rice_unary_scan_inline(br, &q);
    if (rc != TC_OK) { return rc; }

    uint64_t m;
    if (q == (uint32_t)TC_RICE_UNARY_LIMIT) {
        tc_bitreader_refill(br);
        if (br->bits < 32u) {
            br->error = TC_ERR_TRUNCATED;
            return TC_ERR_TRUNCATED;
        }
        m = br->cache >> 32;
        tc_bitreader_skip(br, 32u);
    } else {
        uint32_t rem = 0u;
        if (k > 0u) {
            tc_bitreader_refill(br);
            if (br->bits < k) {
                br->error = TC_ERR_TRUNCATED;
                return TC_ERR_TRUNCATED;
            }
            rem = (uint32_t)(br->cache >> (64u - k));
            tc_bitreader_skip(br, k);
        }
        m = ((uint64_t)q << k) | (uint64_t)rem;
    }

    if (m > (uint64_t)m_max) {
        tc_bitreader_fail(br, TC_ERR_MALFORMED);
        return TC_ERR_MALFORMED;
    }
    *m_out = (uint32_t)m;
    return TC_OK;
}

static inline int32_t tc_rice_decode_inline(tc_bitreader* br, uint32_t k, uint32_t m_max,
                                            uint32_t* m_out)
{
    if (k > (uint32_t)TC_RICE_K_MAX) { return TC_ERR_INVALID_ARGUMENT; }
    if (br->error != 0) { return br->error; }
    if (atomic_load_explicit(&tc_rice_lut_ready[k], memory_order_acquire) == 0) {
        int32_t erc = tc_rice_lut_ensure(k);
        if (erc != TC_OK) { return erc; }
    }
    return tc_rice_decode_lut_inline(br, tc_rice_short_lut[k], tc_rice_lut[k],
                                     k, m_max, m_out);
}

/* 解码单个 Rice 符号并做域校验（m > m_max → TC_ERR_MALFORMED，reader 置粘滞）。
 * 返回 TC_OK / reader 粘滞错误 / TC_ERR_MALFORMED / TC_ERR_INVALID_ARGUMENT(k>14)。 */
int32_t tc_rice_decode(tc_bitreader* br, uint32_t k, uint32_t m_max, uint32_t* m_out);

#endif /* TOPOS_INTERNAL_RICE_H */
