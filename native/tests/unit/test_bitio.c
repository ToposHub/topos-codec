/* checked bit reader/writer：MSB-first、粘滞错误、不推进的越界、增长上限 */
#include "bitstream/bitio.h"

#include <string.h>

#include "mini_test.h"

int main(void)
{
    /* ---- reader ---- */
    {
        tc_bitreader br;
        tc_bitreader_init(&br, NULL, 0);
        uint32_t bit = 9u;
        MT_CHECK_EQ_I64(tc_bitreader_read_bit(&br, &bit), TC_ERR_TRUNCATED);
        MT_CHECK_EQ_I64(tc_bitreader_read_bits(&br, 1u, &bit), TC_ERR_TRUNCATED); /* 粘滞 */
        MT_CHECK_EQ_U64(tc_bitreader_bits_consumed(&br), 0ull);                   /* 未推进 */
    }
    {
        static const uint8_t bytes[2] = { 0xA5u, 0x0F }; /* 1010 0101 0000 1111 */
        tc_bitreader br;
        tc_bitreader_init(&br, bytes, 2);
        uint32_t v = 0;
        MT_CHECK_EQ_I64(tc_bitreader_read_bits(&br, 4u, &v), TC_OK);
        MT_CHECK_EQ_U64(v, 0xAull);
        MT_CHECK_EQ_I64(tc_bitreader_read_bits(&br, 4u, &v), TC_OK);
        MT_CHECK_EQ_U64(v, 0x5ull);
        MT_CHECK_EQ_I64(tc_bitreader_read_bits(&br, 8u, &v), TC_OK);
        MT_CHECK_EQ_U64(v, 0x0Full);
        MT_CHECK_EQ_I64(tc_bitreader_read_bit(&br, &v), TC_ERR_TRUNCATED); /* 恰好用尽 */
        MT_CHECK_EQ_U64(tc_bitreader_bits_consumed(&br), 16ull);
    }
    { /* 部分越界不推进：已读 5 位、剩 3 位时读 4 位 → TRUNCATED（粘滞，不可恢复）；
         重初始化后可完整读 8 位 */
        static const uint8_t b1[1] = { 0xFF };
        tc_bitreader br;
        tc_bitreader_init(&br, b1, 1);
        uint32_t v = 0;
        MT_CHECK_EQ_I64(tc_bitreader_read_bits(&br, 5u, &v), TC_OK);
        MT_CHECK_EQ_U64(v, 0x1Full);
        MT_CHECK_EQ_I64(tc_bitreader_read_bits(&br, 4u, &v), TC_ERR_TRUNCATED);
        MT_CHECK_EQ_U64(tc_bitreader_bits_consumed(&br), 5ull); /* 未推进 */
        MT_CHECK_EQ_I64(tc_bitreader_read_bits(&br, 1u, &v), TC_ERR_TRUNCATED); /* 粘滞 */
        tc_bitreader_init(&br, b1, 1);
        MT_CHECK_EQ_I64(tc_bitreader_read_bits(&br, 8u, &v), TC_OK);
        MT_CHECK_EQ_U64(v, 0xFFull);
    }
    { /* 32 位全宽 + align */
        static const uint8_t b4[4] = { 0x12, 0x34, 0x56, 0x78 };
        tc_bitreader br;
        tc_bitreader_init(&br, b4, 4);
        uint32_t v = 0;
        MT_CHECK_EQ_I64(tc_bitreader_read_bits(&br, 32u, &v), TC_OK);
        MT_CHECK_EQ_U64(v, 0x12345678ull);
        MT_CHECK_EQ_I64(tc_bitreader_read_bits(&br, 0u, &v), TC_OK);
        MT_CHECK_EQ_U64(v, 0ull);
        MT_CHECK_EQ_I64(tc_bitreader_align_byte(&br), TC_OK);
        MT_CHECK_EQ_I64(tc_bitreader_read_bits(&br, 33u, &v), TC_ERR_INVALID_ARGUMENT);
    }

    /* ---- writer ---- */
    {
        tc_bitwriter bw;
        MT_CHECK_EQ_I64(tc_bitwriter_init(&bw), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_put_bit(&bw, 1u), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_put_bit(&bw, 0u), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_put_bits(&bw, 3u, 0x5ull), TC_OK); /* 101 */
        MT_CHECK_EQ_I64(tc_bitwriter_put_bits(&bw, 2u, 0x3ull), TC_OK); /* 11 */
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        /* 字节 0：1,0,'101','11' → 1 0 1 0 1 1 1 + 0 填充 = 0xAE */
        MT_CHECK_EQ_U64(tc_bitwriter_byte_size(&bw), 1ull);
        MT_CHECK_EQ_U64(bw.buf[0], 0xAEull);
        MT_CHECK_EQ_U64(tc_bitwriter_bits_written(&bw), 8ull);
        /* 高位掩除：只写低 n 位 */
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_bitwriter_put_bits(&bw, 4u, 0xFFull), TC_OK); /* 低 4 位 1111 */
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        MT_CHECK_EQ_U64(bw.buf[0], 0xF0ull);
        MT_CHECK_EQ_I64(tc_bitwriter_put_bits(&bw, 33u, 0u), TC_ERR_INVALID_ARGUMENT);
        tc_bitwriter_free(&bw);
    }
    { /* 跨 64KiB 增长路径 + 随机回读一致 */
        tc_bitwriter bw;
        MT_CHECK_EQ_I64(tc_bitwriter_init(&bw), TC_OK);
        enum { N = 700000 }; /* = 87500 字节整，增长跨过初始 64KiB */
        static uint8_t expect[N / 8];
        memset(expect, 0, sizeof(expect));
        for (int i = 0; i < N; ++i) {
            uint32_t bit = (uint32_t)(mt_rand_u64() & 1ull);
            MT_CHECK_EQ_I64(tc_bitwriter_put_bit(&bw, bit), TC_OK);
            if (bit) { expect[i / 8] |= (uint8_t)(1u << (7u - (i % 8))); }
        }
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        MT_CHECK_EQ_U64(tc_bitwriter_byte_size(&bw), (uint64_t)(N / 8));
        MT_CHECK(memcmp(tc_bitwriter_data(&bw), expect, sizeof(expect)) == 0);
        /* 回读 */
        tc_bitreader br;
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        for (int i = 0; i < N; ++i) {
            uint32_t bit = 0;
            MT_CHECK_EQ_I64(tc_bitreader_read_bit(&br, &bit), TC_OK);
            if (bit != (uint32_t)((expect[i / 8] >> (7u - (i % 8))) & 1u)) {
                mt_report(__FILE__, __LINE__, "roundtrip bit mismatch");
                break;
            }
        }
        tc_bitwriter_free(&bw);
    }
    { /* 多符号 put_bits/read_bits 随机回读 */
        tc_bitwriter bw;
        MT_CHECK_EQ_I64(tc_bitwriter_init(&bw), TC_OK);
        static uint32_t widths[512];
        static uint32_t vals[512];
        int n = 0;
        uint64_t bits = 0;
        while (bits + 32ull < 4096ull) {
            uint32_t w = (uint32_t)(mt_rand_u64() % 33ull);
            uint32_t v = w == 32u ? (uint32_t)mt_rand_u64()
                                  : (uint32_t)(mt_rand_u64() & ((1ull << w) - 1ull));
            widths[n] = w; vals[n] = v; n++;
            bits += w;
        }
        for (int i = 0; i < n; ++i) {
            MT_CHECK_EQ_I64(tc_bitwriter_put_bits(&bw, widths[i], vals[i]), TC_OK);
        }
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        tc_bitreader br;
        tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
        for (int i = 0; i < n; ++i) {
            uint32_t v = 0;
            MT_CHECK_EQ_I64(tc_bitreader_read_bits(&br, widths[i], &v), TC_OK);
            if (v != vals[i]) {
                mt_report(__FILE__, __LINE__, "put_bits/read_bits mismatch");
                break;
            }
        }
        tc_bitwriter_free(&bw);
    }

    return MT_MAIN_RETURN();
}
