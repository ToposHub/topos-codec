/* 块符号层：zigzag 表、DC 预测、块编解码往返与对抗输入、Alpha 残差流 */
#include "entropy/block_coding.h"
#include "entropy/rice.h"
#include "entropy/scan.h"
#include "entropy/vlc.h"

#include <string.h>

#include "mini_test.h"

/* 对角遍历生成参考 zigzag（scan 位置 → natural 索引）：
 * 第 d 条对角线（row+col==d），偶数 d 从左下端向右上、奇数 d 从右上端向左下 */
static void zigzag_reference(uint8_t* out)
{
    int i = 0;
    for (int d = 0; d < 15; ++d) {
        if ((d % 2) == 0) {
            int r = d < 7 ? d : 7;
            int c = d - r;
            while (r >= 0 && c <= 7) {
                out[i++] = (uint8_t)(r * 8 + c);
                r--; c++;
            }
        } else {
            int c = d < 7 ? d : 7;
            int r = d - c;
            while (c >= 0 && r <= 7) {
                out[i++] = (uint8_t)(r * 8 + c);
                r++; c--;
            }
        }
    }
}

static void zero_block(int32_t* q) { for (int i = 0; i < 64; ++i) { q[i] = 0; } }

/* 编码→解码 往返并比对；返回 0 一致 */
static int roundtrip_block(const int32_t* q, uint32_t k1, uint32_t k2, uint32_t k3,
                           int has_left, int32_t left, int has_top, int32_t top,
                           tc_bitwriter* bw)
{
    tc_bitwriter_reset(bw);
    if (tc_block_encode(bw, k1, k2, k3, q, has_left, left, has_top, top, NULL) != TC_OK) {
        return -1;
    }
    if (tc_bitwriter_flush_zero_pad(bw) != TC_OK) { return -1; }
    tc_bitreader br;
    tc_bitreader_init(&br, tc_bitwriter_data(bw), tc_bitwriter_byte_size(bw));
    int32_t out[64];
    if (tc_block_decode(&br, k1, k2, k3, has_left, left, has_top, top, out) != TC_OK) {
        return -2;
    }
    if (tc_bitreader_align_byte(&br) != TC_OK) { return -3; }
    if (tc_bitreader_bits_consumed(&br) != tc_bitwriter_bits_written(bw)) { return -4; }
    for (int i = 0; i < 64; ++i) {
        if (out[i] != q[i]) { return -5; }
    }
    return 0;
}

int main(void)
{
    /* ---- zigzag：对角遍历参考 + 置换性 ---- */
    {
        uint8_t ref[64];
        zigzag_reference(ref);
        int seen[64] = { 0 };
        for (int i = 0; i < 64; ++i) {
            if (kTcZigzag[i] != ref[i]) {
                mt_report(__FILE__, __LINE__, "zigzag table != diagonal reference");
                break;
            }
        }
        for (int i = 0; i < 64; ++i) { seen[kTcZigzag[i]]++; }
        for (int i = 0; i < 64; ++i) {
            if (seen[i] != 1) { mt_report(__FILE__, __LINE__, "zigzag not a permutation"); break; }
        }
        MT_CHECK_EQ_U64(kTcZigzag[0], 0ull);
        MT_CHECK_EQ_U64(kTcZigzag[63], 63ull); /* 末扫描位 = natural 63（回归点） */
    }

    /* ---- DC 预测公式（round-half-up，负数不右移）---- */
    MT_CHECK_EQ_I64(tc_dc_predict(0, 0, 0, 0), 0);
    MT_CHECK_EQ_I64(tc_dc_predict(1, 7, 0, 0), 7);
    MT_CHECK_EQ_I64(tc_dc_predict(0, 0, 1, -9), -9);
    MT_CHECK_EQ_I64(tc_dc_predict(1, 3, 1, 4), 4);   /* (3+4+1)>>1 = 4 */
    MT_CHECK_EQ_I64(tc_dc_predict(1, -3, 1, 0), -1); /* floor(-2/2)=-1：half-up(−1.5)=−1 */
    MT_CHECK_EQ_I64(tc_dc_predict(1, -4, 1, 1), -1); /* half-up(−1.5)=−1 */
    MT_CHECK_EQ_I64(tc_dc_predict(1, -5, 1, 0), -2); /* half-up(−2.5)=−2 */
    MT_CHECK_EQ_I64(tc_dc_predict(1, 4, 1, -3), 1);  /* half-up(0.5)=1 */

    tc_bitwriter bw;
    MT_CHECK_EQ_I64(tc_bitwriter_init(&bw), TC_OK);

    /* ---- 构造块 ---- */
    {
        int32_t q[64];
        zero_block(q);
        MT_CHECK_EQ_I64(roundtrip_block(q, 0, 0, 0, 0, 0, 0, 0, &bw), 0); /* 全零：DC+EOB */

        zero_block(q); q[0] = 1234;
        MT_CHECK_EQ_I64(roundtrip_block(q, 2, 3, 1, 1, 1000, 1, 1100, &bw), 0);

        zero_block(q); q[63] = -77; /* 仅 natural63 非零：run=62 回归点（v1 不变量） */
        MT_CHECK_EQ_I64(roundtrip_block(q, 1, 1, 0, 0, 0, 0, 0, &bw), 0);

        zero_block(q); q[1] = 1 << 25; q[8] = -(1 << 25); /* AC 域界 ±2^25 → m ≤ 2^26 */
        MT_CHECK_EQ_I64(roundtrip_block(q, 0, 14, 14, 0, 0, 0, 0, &bw), 0);

        /* P0-07（spec §7.6）：量化 DC 系数域 ±2^25（编码器钳位 |q| ≤ 2^25）。
         * 最大符号残差 ±2^26（m ≤ 2^27）仅在重建后 |dc| ≤ 2^25 时合法——
         * pred=∓2^25 的上下文把重建顶到域边界，同时覆盖残差域与系数域。 */
        zero_block(q); q[0] = (1 << 25);
        MT_CHECK_EQ_I64(roundtrip_block(q, 14, 0, 0, 0, 0, 0, 0, &bw), 0);
        zero_block(q); q[0] = -(1 << 25);
        MT_CHECK_EQ_I64(roundtrip_block(q, 14, 0, 0, 0, 0, 0, 0, &bw), 0);
        zero_block(q); q[0] = (1 << 25); /* 残差 +2^26：pred=−2^25 → dc=+2^25 */
        MT_CHECK_EQ_I64(roundtrip_block(q, 14, 0, 0, 1, -(1 << 25), 0, 0, &bw), 0);

        zero_block(q); /* P0-07：显式清 q[0]——上一用例残留会让稠密块 dc 越域 */
        for (int i = 1; i < 64; ++i) { q[i] = (i % 2) ? i : -i; } /* 稠密交替符号 */
        MT_CHECK_EQ_I64(roundtrip_block(q, 4, 5, 2, 1, 999, 1, -999, &bw), 0);
    }

    /* ---- 随机块 × 随机上下文 × k 全域 ---- */
    {
        int bad = 0;
        for (int iter = 0; iter < 5000 && bad == 0; ++iter) {
            int32_t q[64];
            zero_block(q);
            q[0] = (int32_t)(mt_rand_u64() % 4001ull) - 2000;
            int nonzeros = (int)(mt_rand_u64() % 64ull);
            for (int n = 0; n < nonzeros; ++n) {
                int pos = (int)(mt_rand_u64() % 64ull);
                int32_t mag = (int32_t)(mt_rand_u64() % 30000ull) + 1;
                q[pos] = (mt_rand_u64() & 1ull) != 0ull ? mag : -mag;
            }
            uint32_t k1 = (uint32_t)(mt_rand_u64() % 15ull);
            uint32_t k2 = (uint32_t)(mt_rand_u64() % 15ull);
            uint32_t k3 = (uint32_t)(mt_rand_u64() % 15ull);
            int hl = (int)(mt_rand_u64() & 1ull);
            int ht = (int)((mt_rand_u64() >> 1) & 1ull);
            int32_t left = (int32_t)(mt_rand_u64() % 5000ull) - 2500;
            int32_t top = (int32_t)(mt_rand_u64() % 5000ull) - 2500;
            int rc = roundtrip_block(q, k1, k2, k3, hl, left, ht, top, &bw);
            if (rc != 0) { bad = rc; }
        }
        MT_CHECK_EQ_I64(bad, 0);
    }

    /* ---- 对抗解码：不变量违反 → MALFORMED ---- */
    {
        /* P0-07：DC 重建越域（|dc| > 2^25，spec §7.6）→ MALFORMED。
         * 合法编码器钳位 |q| ≤ 2^25，此类块只能来自恶意/损坏码流。 */
        {
            int32_t q[64];
            zero_block(q);
            q[0] = 1 << 26;
            tc_bitwriter_reset(&bw);
            MT_CHECK_EQ_I64(tc_block_encode(&bw, 14, 0, 0, q, 0, 0, 0, 0, NULL), TC_OK);
            MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
            tc_bitreader br;
            tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
            int32_t out[64];
            MT_CHECK_EQ_I64(tc_block_decode(&br, 14, 0, 0, 0, 0, 0, 0, out),
                            TC_ERR_MALFORMED);
        }

        /* DC 后 run=62 + level + 再一对（pos 已 64）→ MALFORMED */
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, 0), TC_OK);                       /* DC=0 */
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, 62), TC_OK);                      /* run=62 */
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, tc_rice_map_signed(5)), TC_OK);   /* level */
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, 1), TC_OK);                       /* run=1 → 64+1>63 */
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, 63), TC_OK);                      /* EOB */
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        tc_bitreader br;
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        int32_t out[64];
        MT_CHECK_EQ_I64(tc_block_decode(&br, 0, 0, 0, 0, 0, 0, 0, out), TC_ERR_MALFORMED);

        /* AC level 域超界（m > 2^26 → |level| > 2^25） */
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, 0), TC_OK);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, 0), TC_OK);      /* run=0 */
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, (1u << 26) + 1u), TC_OK); /* 超界 level m */
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        MT_CHECK_EQ_I64(tc_block_decode(&br, 0, 0, 0, 0, 0, 0, 0, out), TC_ERR_MALFORMED);

        /* 缺 EOB：截断 */
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, 0), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        MT_CHECK_EQ_I64(tc_block_decode(&br, 0, 0, 0, 0, 0, 0, 0, out), TC_ERR_TRUNCATED);

        /* run=64 不可能（域校验 63），run=63 即 EOB：全零 AC 合法返回 */
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, 0), TC_OK);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, 63), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        MT_CHECK_EQ_I64(tc_block_decode(&br, 0, 0, 0, 0, 0, 0, 0, out), TC_OK);
        MT_CHECK_EQ_I64(out[0], 0);
    }

    /* ---- C1 joint AC experimental syntax：common pair / escape / truncation ---- */
    {
        MT_CHECK_EQ_I64(tc_vlc_tables_ensure(TC_VLC_FAMILY_DC, 0u), TC_OK);
        MT_CHECK_EQ_I64(tc_vlc_tables_ensure(TC_VLC_FAMILY_LVL, 0u), TC_OK);
        MT_CHECK_EQ_I64(tc_vlc_tables_ensure(TC_VLC_FAMILY_RUN, 0u), TC_OK);
        const tc_vlc_book* dc = tc_vlc_book_get(TC_VLC_FAMILY_DC, 0u);
        const tc_vlc_book* lvl = tc_vlc_book_get(TC_VLC_FAMILY_LVL, 0u);
        const tc_vlc_book* pair = tc_vlc_book_get(TC_VLC_FAMILY_RUN, 0u);
        int32_t q[64], qz[64], out[64];
        zero_block(q); q[0] = 7; q[1] = -3; q[2] = 4; q[63] = 123;
        for (int i = 0; i < 64; ++i) { qz[i] = q[kTcZigzag[i]]; }
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_block_encode_zigzag_c1(&bw, dc, lvl, pair, qz,
                                                   1, 2, 1, -4, NULL), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        tc_bitreader br;
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        MT_CHECK_EQ_I64(tc_block_decode_c1(&br, dc, lvl, pair, 1, 2, 1, -4, out), TC_OK);
        for (int i = 0; i < 64; ++i) { MT_CHECK_EQ_I64(out[i], q[i]); }

        /* All 63 AC positions nonzero force the escape path for large runs/categories. */
        zero_block(q);
        for (int i = 1; i < 64; ++i) { q[i] = (i & 1) ? (1 << 20) : -(1 << 20); }
        for (int i = 0; i < 64; ++i) { qz[i] = q[kTcZigzag[i]]; }
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_block_encode_zigzag_c1(&bw, dc, lvl, pair, qz,
                                                   0, 0, 0, 0, NULL), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        MT_CHECK_EQ_I64(tc_block_decode_c1(&br, dc, lvl, pair, 0, 0, 0, 0, out), TC_OK);
        for (int i = 0; i < 64; ++i) { MT_CHECK_EQ_I64(out[i], q[i]); }

        size_t n = tc_bitwriter_byte_size(&bw);
        if (n > 1u) {
            tc_bitreader_init(&br, tc_bitwriter_data(&bw), n - 1u);
            int32_t trc = tc_block_decode_c1(&br, dc, lvl, pair, 0, 0, 0, 0, out);
            MT_CHECK_EQ_I64(trc, TC_ERR_TRUNCATED);
        }

        /* C2 builds a deterministic per-slice canonical table and keeps the
         * same exact coefficient reconstruction contract. */
        tc_vlc_book c2;
        uint8_t lengths[64];
        MT_CHECK_EQ_I64(tc_c2_pair_book_build(qz, 1u, &c2, lengths), TC_OK);
        tc_bitwriter_reset(&bw);
        for (int i = 0; i < 64; ++i) {
            MT_CHECK_EQ_I64(tc_bitwriter_put_bits(&bw, 8u, lengths[i]), TC_OK);
        }
        MT_CHECK_EQ_I64(tc_block_encode_zigzag_c2(&bw, dc, lvl, &c2, qz,
                                                   0, 0, 0, 0, NULL), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        uint32_t v = 0u;
        for (int i = 0; i < 64; ++i) {
            MT_CHECK_EQ_I64(tc_bitreader_read_bits(&br, 8u, &v), TC_OK);
        }
        MT_CHECK_EQ_I64(tc_block_decode_c2(&br, dc, lvl, &c2, 0, 0, 0, 0, out), TC_OK);
        for (int i = 0; i < 64; ++i) { MT_CHECK_EQ_I64(out[i], q[i]); }
    }

    /* ---- Alpha 残差流 ---- */
    {
        enum { N = 4096 };
        static int32_t r[N];
        static int32_t back[N];

        /* 全零 */
        memset(r, 0, sizeof(r));
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_alpha_residuals_encode(&bw, 4, 4, r, N), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        tc_bitreader br;
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        uint64_t hash1 = 0, hash2 = 0;
        memset(back, 0x55, sizeof(back));
        MT_CHECK_EQ_I64(tc_alpha_residuals_decode(&br, 4, 4, back, N, &hash1), TC_OK);
        for (int i = 0; i < N; ++i) { MT_CHECK_EQ_I64(back[i], 0); }
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        MT_CHECK_EQ_I64(tc_alpha_residuals_decode(&br, 4, 4, NULL, N, &hash2), TC_OK);
        MT_CHECK_EQ_U64(hash1, hash2); /* O(1) 指纹路径一致 */

        /* 尾部带零 + 域内随机（±65535） */
        for (int iter = 0; iter < 500; ++iter) {
            size_t n = 1 + (size_t)(mt_rand_u64() % N);
            size_t tail = (size_t)(mt_rand_u64() % n);
            for (size_t i = 0; i < n - tail; ++i) {
                uint64_t pick = mt_rand_u64() % 5ull;
                r[i] = pick == 0ull ? 0
                                    : (int32_t)((int64_t)(mt_rand_u64() % 131071ull) - 65535);
            }
            for (size_t i = n - tail; i < n; ++i) { r[i] = 0; }
            uint32_t kl = (uint32_t)(mt_rand_u64() % 15ull);
            uint32_t kr = (uint32_t)(mt_rand_u64() % 15ull);
            tc_bitwriter_reset(&bw);
            MT_CHECK_EQ_I64(tc_alpha_residuals_encode(&bw, kl, kr, r, n), TC_OK);
            MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
            tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
            memset(back, 0x55, sizeof(back));
            int32_t rc = tc_alpha_residuals_decode(&br, kl, kr, back, n, NULL);
            if (rc != TC_OK) { mt_report(__FILE__, __LINE__, "alpha roundtrip rc"); break; }
            if (memcmp(back, r, n * sizeof(int32_t)) != 0) {
                mt_report(__FILE__, __LINE__, "alpha roundtrip bytes");
                break;
            }
        }

        /* 对抗：终结对 run != remaining → MALFORMED */
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, 5), TC_OK); /* run=5 */
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, 0), TC_OK); /* level=0 终结 */
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        MT_CHECK_EQ_I64(tc_alpha_residuals_decode(&br, 0, 0, NULL, 8, NULL), TC_ERR_MALFORMED);

        /* 对抗：数据对 run == remaining（level 无处落位）→ MALFORMED */
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, 7), TC_OK);            /* run=7 == remaining */
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, tc_rice_map_signed(3)), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        MT_CHECK_EQ_I64(tc_alpha_residuals_decode(&br, 0, 0, NULL, 7, NULL), TC_ERR_MALFORMED);

        /* 对抗：level 域超界（m > 2^17） */
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, 0), TC_OK);
        MT_CHECK_EQ_I64(tc_rice_encode(&bw, 0, (1u << 17) + 1u), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        MT_CHECK_EQ_I64(tc_alpha_residuals_decode(&br, 0, 0, NULL, 4, NULL), TC_ERR_MALFORMED);
    }

    tc_bitwriter_free(&bw);
    return MT_MAIN_RETURN();
}
