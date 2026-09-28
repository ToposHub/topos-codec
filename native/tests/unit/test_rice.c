/* 有界 Rice：映射、金标位型、escape 边界、域校验、k 上限、随机往返、截断 */
#include "entropy/rice.h"

#include <string.h>

#include "mini_test.h"

/* M10-6.3C：码字构造 + 64 位写通道差分——(code_word, put64) 与逐符号
 * tc_rice_encode_inline 逐位一致；put64 与两次 32 位写一致（含 escape/
 * 长 unary 拼接位型与随机符号流）。 */
static void test_cword_put64_differential(void)
{
    tc_bitwriter bwa, bwb;
    MT_CHECK_EQ_I64(tc_bitwriter_init(&bwa), TC_OK);
    MT_CHECK_EQ_I64(tc_bitwriter_init(&bwb), TC_OK);
    for (uint32_t k = 0u; k <= (uint32_t)TC_RICE_K_MAX; ++k) {
        /* 边界 m：q=0/1/29/30/31/32 与随机（覆盖 escape 与长 unary 拼接） */
        const uint32_t edges[] = {
            0u, 1u, 2u, (1u << k), (29u << k), (30u << k), (31u << k),
            (32u << k), (33u << k), 0x7FFFFFFFu, 0xFFFFFFFFu
        };
        for (size_t e = 0u; e < sizeof(edges) / sizeof(edges[0]); ++e) {
            const uint32_t m = edges[e];
            tc_bitwriter_reset(&bwa);
            tc_bitwriter_reset(&bwb);
            MT_CHECK_EQ_I64(tc_rice_encode_inline(&bwa, k, m), TC_OK);
            uint64_t w = 0u;
            const uint32_t n = tc_rice_code_word(k, m, &w);
            MT_CHECK(n >= 1u && n <= 63u);
            MT_CHECK_EQ_I64(tc_bitwriter_put_bits64_inline(&bwb, n, w), TC_OK);
            MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bwa), TC_OK);
            MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bwb), TC_OK);
            const size_t sa = tc_bitwriter_byte_size(&bwa);
            const size_t sb = tc_bitwriter_byte_size(&bwb);
            MT_CHECK_EQ_U64((uint64_t)sa, (uint64_t)sb);
            if (sa == sb && memcmp(tc_bitwriter_data(&bwa), tc_bitwriter_data(&bwb), sa) != 0) {
                mt_report(__FILE__, __LINE__, "code word bit mismatch");
                break;
            }
        }
    }
    /* 随机符号流：put64 与（≤32 位时）put_bits 两次拼接一致 */
    for (int trial = 0; trial < 64; ++trial) {
        tc_bitwriter_reset(&bwa);
        tc_bitwriter_reset(&bwb);
        for (int i = 0; i < 512; ++i) {
            const uint32_t k = (uint32_t)(mt_rand_u64() % 15ull);
            const uint32_t m = (uint32_t)(mt_rand_u64() >> (mt_rand_u64() % 32ull));
            uint64_t w = 0u;
            const uint32_t n = tc_rice_code_word(k, m, &w);
            MT_CHECK_EQ_I64(tc_bitwriter_put_bits64_inline(&bwa, n, w), TC_OK);
            if (n <= 32u) {
                MT_CHECK_EQ_I64(tc_bitwriter_put_bits_inline(&bwb, n, (uint32_t)w), TC_OK);
            } else {
                MT_CHECK_EQ_I64(
                    tc_bitwriter_put_bits_inline(&bwb, n - 32u, (uint32_t)(w >> 32)), TC_OK);
                MT_CHECK_EQ_I64(
                    tc_bitwriter_put_bits_inline(&bwb, 32u, (uint32_t)w), TC_OK);
            }
        }
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bwa), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bwb), TC_OK);
        const size_t sa = tc_bitwriter_byte_size(&bwa);
        MT_CHECK_EQ_U64((uint64_t)sa, (uint64_t)tc_bitwriter_byte_size(&bwb));
        if (memcmp(tc_bitwriter_data(&bwa), tc_bitwriter_data(&bwb), sa) != 0) {
            mt_report(__FILE__, __LINE__, "random stream bit mismatch");
            break;
        }
    }
    /* 长流（>1 MB）：跨多次容量增长边界（64KB 初始 + inline 回退增长路径）——
     * 准入条件错误（未保证容量即提交）在此显现为越界/错位 */
    {
        tc_bitwriter_reset(&bwa);
        tc_bitwriter_reset(&bwb);
        for (int i = 0; i < 200000; ++i) {
            const uint32_t k = (uint32_t)(mt_rand_u64() % 15ull);
            const uint32_t m = (uint32_t)(mt_rand_u64() >> (mt_rand_u64() % 32ull));
            uint64_t w = 0u;
            const uint32_t n = tc_rice_code_word(k, m, &w);
            MT_CHECK_EQ_I64(tc_bitwriter_put_bits64_inline(&bwa, n, w), TC_OK);
            if (n <= 32u) {
                MT_CHECK_EQ_I64(tc_bitwriter_put_bits_inline(&bwb, n, (uint32_t)w), TC_OK);
            } else {
                MT_CHECK_EQ_I64(
                    tc_bitwriter_put_bits_inline(&bwb, n - 32u, (uint32_t)(w >> 32)), TC_OK);
                MT_CHECK_EQ_I64(
                    tc_bitwriter_put_bits_inline(&bwb, 32u, (uint32_t)w), TC_OK);
            }
        }
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bwa), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bwb), TC_OK);
        const size_t sa = tc_bitwriter_byte_size(&bwa);
        MT_CHECK(sa > 1024u * 1024u); /* 确认确实跨过容量增长边界 */
        MT_CHECK_EQ_U64((uint64_t)sa, (uint64_t)tc_bitwriter_byte_size(&bwb));
        MT_CHECK(memcmp(tc_bitwriter_data(&bwa), tc_bitwriter_data(&bwb), sa) == 0);
    }
    tc_bitwriter_free(&bwa);
    tc_bitwriter_free(&bwb);
}

/* P0-07：tc_dc_reconstruct_checked —— 域界 ±2^25（spec §7.6）、int64 重建、
 * 恶意累积路径在越界瞬间被拒。符号残差界 ±2^26 与域界的关系：合法编码器
 * 恒有 |q| ≤ 2^25，故 |residual| ≤ 2^26 的任意符号只有落在 ±2^25 内才接受。 */
static void test_dc_reconstruct_checked(void)
{
    int32_t out = 0;

    /* 域内：0、边界值本身、小残差 */
    MT_CHECK_EQ_I64(tc_dc_reconstruct_checked(0, tc_rice_map_signed(0), &out), TC_OK);
    MT_CHECK_EQ_I64(out, 0);
    MT_CHECK_EQ_I64(tc_dc_reconstruct_checked(0, tc_rice_map_signed(1), &out), TC_OK);
    MT_CHECK_EQ_I64(out, 1);
    MT_CHECK_EQ_I64(tc_dc_reconstruct_checked(TC_DC_DOMAIN_MAX, tc_rice_map_signed(0), &out),
                    TC_OK);
    MT_CHECK_EQ_I64(out, TC_DC_DOMAIN_MAX);
    MT_CHECK_EQ_I64(tc_dc_reconstruct_checked(-TC_DC_DOMAIN_MAX, tc_rice_map_signed(0), &out),
                    TC_OK);
    MT_CHECK_EQ_I64(out, -TC_DC_DOMAIN_MAX);

    /* 最大符号残差 ±2^26：仅当落在域内才接受（合法最坏情形 = −MAX 走到 +MAX） */
    MT_CHECK_EQ_I64(tc_dc_reconstruct_checked(-TC_DC_DOMAIN_MAX,
                                              tc_rice_map_signed(1 << 26), &out), TC_OK);
    MT_CHECK_EQ_I64(out, TC_DC_DOMAIN_MAX);
    MT_CHECK_EQ_I64(tc_dc_reconstruct_checked(TC_DC_DOMAIN_MAX,
                                              tc_rice_map_signed(-(1 << 26)), &out), TC_OK);
    MT_CHECK_EQ_I64(out, -TC_DC_DOMAIN_MAX);

    /* 越界：域外任一方向立即拒 TC_ERR_MALFORMED（含从 0 的单步 2^26 走出） */
    MT_CHECK_EQ_I64(tc_dc_reconstruct_checked(TC_DC_DOMAIN_MAX,
                                              tc_rice_map_signed(1), &out),
                    TC_ERR_MALFORMED);
    MT_CHECK_EQ_I64(tc_dc_reconstruct_checked(-TC_DC_DOMAIN_MAX,
                                              tc_rice_map_signed(-1), &out),
                    TC_ERR_MALFORMED);
    MT_CHECK_EQ_I64(tc_dc_reconstruct_checked(0, tc_rice_map_signed(1 << 26), &out),
                    TC_ERR_MALFORMED);

    /* 恶意累积演化：保持域内的前提下每步最多走到 ±MAX，越界瞬间截断 */
    int32_t pred = 0;
    for (int blk = 0; blk < 64; ++blk) {
        int32_t rc = tc_dc_reconstruct_checked(pred, tc_rice_map_signed(1 << 26), &out);
        if (rc != TC_OK) {
            break; /* 首次越界即拒：累积 UB 不可达 */
        }
        pred = out;
        MT_CHECK(out >= -TC_DC_DOMAIN_MAX && out <= TC_DC_DOMAIN_MAX);
    }
    MT_CHECK(pred != 1 << 26); /* 任何已接受的 dc 都不可能走出域界 */
}

int main(void)
{
    test_cword_put64_differential();
    test_dc_reconstruct_checked();

    /* ---- 有符号映射（A.5 示例 + 对合 + 边界）---- */
    MT_CHECK_EQ_U64(tc_rice_map_signed(0), 0ull);
    MT_CHECK_EQ_U64(tc_rice_map_signed(1), 2ull);
    MT_CHECK_EQ_U64(tc_rice_map_signed(-1), 1ull);
    MT_CHECK_EQ_U64(tc_rice_map_signed(2), 4ull);
    MT_CHECK_EQ_U64(tc_rice_map_signed(-2), 3ull);
    MT_CHECK_EQ_I64(tc_rice_unmap_signed(0), 0);
    MT_CHECK_EQ_I64(tc_rice_unmap_signed(1), -1);
    MT_CHECK_EQ_I64(tc_rice_unmap_signed(2), 1);
    MT_CHECK_EQ_I64(tc_rice_unmap_signed(3), -2);
    MT_CHECK_EQ_I64(tc_rice_unmap_signed(tc_rice_map_signed(-(1 << 26))), -(1 << 26));
    MT_CHECK_EQ_I64(tc_rice_unmap_signed(tc_rice_map_signed(1 << 26)), 1 << 26);
    for (int i = 0; i < 20000; ++i) {
        int64_t wide = (int64_t)(mt_rand_u64() % (1ull << 31)) - (1ll << 30);
        int32_t v = (int32_t)wide; /* |v| ≤ 2^30 前置域内 */
        if (tc_rice_unmap_signed(tc_rice_map_signed(v)) != v) {
            mt_report(__FILE__, __LINE__, "map/unmap involution");
            break;
        }
    }

    tc_bitwriter bw;
    MT_CHECK_EQ_I64(tc_bitwriter_init(&bw), TC_OK);
    tc_bitreader br;

    /* ---- 金标位型（手工推导，MSB-first）---- */
    {
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0u, 0u), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        MT_CHECK_EQ_U64(bw.buf[0], 0x00ull); /* "0" + 填充 → 0x00 */

        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0u, 1u), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        MT_CHECK_EQ_U64(bw.buf[0], 0x80ull); /* "10" → 0x80 */

        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0u, 2u), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        MT_CHECK_EQ_U64(bw.buf[0], 0xC0ull); /* "110" → 0xC0 */

        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 1u, 2u), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        MT_CHECK_EQ_U64(bw.buf[0], 0x80ull); /* "10"+"0" → 100 → 0x80 */

        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 2u, 13u), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        MT_CHECK_EQ_U64(bw.buf[0], 0xE4ull); /* "1110"+"01" → 111001xx → 0xE4 */
    }

    /* ---- escape 边界：q=30 正常 / q=31 escape ---- */
    for (uint32_t k = 0; k <= 14u; ++k) {
        uint32_t m30 = 30u << k;      /* q=30：unary 30 一 + 0 */
        uint32_t m31 = 31u << k;      /* q=31：escape */
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, k, m30), TC_OK);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, k, m31), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        uint32_t m = 0;
        MT_CHECK_EQ_I64(tc_rice_decode(&br, k, 0xFFFFFFFFu, &m), TC_OK);
        MT_CHECK_EQ_U64(m, m30);
        MT_CHECK_EQ_I64(tc_rice_decode(&br, k, 0xFFFFFFFFu, &m), TC_OK);
        MT_CHECK_EQ_U64(m, m31);
    }
    { /* escape 字面值含高位 */
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0u, 0x80000001u), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        uint32_t m = 0;
        MT_CHECK_EQ_I64(tc_rice_decode(&br, 0u, 0xFFFFFFFFu, &m), TC_OK);
        MT_CHECK_EQ_U64(m, 0x80000001ull);
    }

    /* ---- 域校验与 k 上限 ---- */
    {
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 15u, 0u), TC_ERR_INVALID_ARGUMENT);
        tc_bitreader_init(&br, NULL, 0);
        uint32_t m = 0;
        MT_CHECK_EQ_I64(tc_rice_decode(&br, 15u, 63u, &m), TC_ERR_INVALID_ARGUMENT);

        static const uint8_t raw[16] = { 0 }; /* 全零位 → m=0 */
        tc_bitreader_init(&br, raw, sizeof(raw));
        MT_CHECK_EQ_I64(tc_rice_decode(&br, 0u, 63u, &m), TC_OK);
        MT_CHECK_EQ_U64(m, 0ull);
        /* m=0 合法；再用全 1 位造超界 m：32 个 1 之后 escape 字面值 0xFFFFFFFF */
        static uint8_t ones[64];
        memset(ones, 0xFF, sizeof(ones));
        tc_bitreader_init(&br, ones, sizeof(ones));
        MT_CHECK_EQ_I64(tc_rice_decode(&br, 0u, 63u, &m), TC_ERR_MALFORMED);
        MT_CHECK_EQ_I64(br.error, TC_ERR_MALFORMED); /* 粘滞 */
    }

    /* ---- 随机多符号流往返（含 k 全域、escape、32 位字面值）---- */
    for (int iter = 0; iter < 300; ++iter) {
        uint32_t k = (uint32_t)(mt_rand_u64() % 15ull);
        enum { NS = 256 };
        static uint32_t syms[NS];
        int ns = 40 + (int)(mt_rand_u64() % (NS - 40ull));
        tc_bitwriter_reset(&bw);
        for (int i = 0; i < ns; ++i) {
            uint64_t r = mt_rand_u64();
            uint32_t m;
            if ((r & 3ull) == 0ull) {
                m = (uint32_t)mt_rand_u64(); /* 大值 → escape 路径 */
            } else if ((r & 7ull) == 1ull) {
                m = (uint32_t)(mt_rand_u64() % 40ull); /* 小值 → unary */
            } else {
                m = (uint32_t)((mt_rand_u64() >> 16) & 0x000FFFFFull);
            }
            syms[i] = m;
            if (tc_rice_encode(&bw, k, m) != TC_OK) { mt_report(__FILE__, __LINE__, "enc"); }
        }
        if (tc_bitwriter_flush_zero_pad(&bw) != TC_OK) { mt_report(__FILE__, __LINE__, "flush"); }
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        int bad = 0;
        for (int i = 0; i < ns; ++i) {
            uint32_t m = 0;
            if (tc_rice_decode(&br, k, 0xFFFFFFFFu, &m) != TC_OK || m != syms[i]) { bad = 1; break; }
        }
        if (bad) { mt_report(__FILE__, __LINE__, "random stream roundtrip"); }
    }

    /* ---- 截断：每个截断点都不越界、不崩溃，且报 TRUNCATED ---- */
    {
        tc_bitwriter_reset(&bw);
        for (int i = 0; i < 64; ++i) {
            if (tc_rice_encode(&bw, 3u, 100000u + (uint32_t)i) != TC_OK) {
                mt_report(__FILE__, __LINE__, "enc truncation prep");
            }
        }
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        size_t full = tc_bitwriter_byte_size(&bw);
        for (size_t cut = 0; cut < full; ++cut) {
            tc_bitreader_init(&br, tc_bitwriter_data(&bw), cut);
            uint32_t m = 0;
            int32_t rc;
            int consumed = 0;
            while ((rc = tc_rice_decode(&br, 3u, 0xFFFFFFFFu, &m)) == TC_OK) { consumed++; }
            if (rc != TC_ERR_TRUNCATED) {
                mt_report(__FILE__, __LINE__, "truncation must end with TRUNCATED");
                break;
            }
            (void)consumed;
        }
    }

    tc_bitwriter_free(&bw);
    return MT_MAIN_RETURN();
}
