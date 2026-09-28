/* RD2-04：V7-A scalar band segment 的 exact bit count、active map、CRC 和确定性。 */
#include "bitstream/band_codec.h"

#include <stdlib.h>
#include <string.h>

#include "common/crc32.h"
#include "mini_test.h"

static void check_encode(uint32_t band, const tc_v7_band_input* input,
                         uint32_t k_dc, uint32_t k_level)
{
    uint64_t exact_bits = 0u;
    MT_CHECK_EQ_I64(tc_v7_band_bits(band, k_dc, k_level, input, &exact_bits), TC_OK);
    MT_CHECK(exact_bits >= 32u);

    tc_bitwriter a;
    tc_bitwriter b;
    MT_CHECK_EQ_I64(tc_bitwriter_init(&a), TC_OK);
    MT_CHECK_EQ_I64(tc_bitwriter_init(&b), TC_OK);
    uint32_t asz = 0u, ac = 0u, bsz = 0u, bc = 0u;
    MT_CHECK_EQ_I64(tc_v7_band_encode(band, k_dc, k_level, input, &a, &asz, &ac), TC_OK);
    MT_CHECK_EQ_I64(tc_v7_band_encode(band, k_dc, k_level, input, &b, &bsz, &bc), TC_OK);
    MT_CHECK_EQ_U64(asz, (exact_bits + 7u) / 8u);
    MT_CHECK_EQ_U64(asz, bsz);
    MT_CHECK_EQ_U64(ac, tc_crc32(tc_bitwriter_data(&a), asz));
    MT_CHECK_EQ_U64(ac, bc);
    MT_CHECK(memcmp(tc_bitwriter_data(&a), tc_bitwriter_data(&b), asz) == 0);
    MT_CHECK(asz >= 4u);
    MT_CHECK_EQ_U64(tc_bitwriter_data(&a)[0], 1u);
    MT_CHECK_EQ_U64(tc_bitwriter_data(&a)[1], k_dc);
    MT_CHECK_EQ_U64(tc_bitwriter_data(&a)[2], k_level);
    MT_CHECK(tc_bitwriter_data(&a)[3] == 0u || tc_bitwriter_data(&a)[3] == 1u);
    int32_t q_zig[3u * 64u] = { 0 };
    MT_CHECK_EQ_I64(tc_v7_band_decode(tc_bitwriter_data(&a), asz, band,
                                      input->block_count, 3u, q_zig), TC_OK);
    tc_bitwriter_free(&a);
    tc_bitwriter_free(&b);
}

int main(void)
{
    /* V 代际收纳：回放构建专属测试——运行期放行退役代际（frame_header.c 运行期门） */
#if defined(_WIN32)
    _putenv("TOPOS_DEV=1");
#elif defined(__APPLE__)
    setenv("TOPOS_DEV", "1", 1);
#else
    if (setenv("TOPOS_DEV", "1", 1) != 0) { return 2; }
#endif
    const uint32_t dc_m[3] = { 0u, 2u, 1u };
    const uint16_t b0_counts[3] = { 2u, 0u, 1u };
    const uint8_t b0_runs[3] = { 0u, 2u, 3u };
    const uint32_t b0_levels[3] = { 2u, 4u, 6u };
    const tc_v7_band_input b0 = {
        3u, dc_m, b0_counts, b0_runs, b0_levels, 3u
    };
    check_encode(0u, &b0, 2u, 3u);

    const uint16_t b1_counts[3] = { 0u, 2u, 0u };
    const uint8_t b1_runs[2] = { 0u, 4u };
    const uint32_t b1_levels[2] = { 8u, 10u };
    const tc_v7_band_input b1 = {
        3u, NULL, b1_counts, b1_runs, b1_levels, 2u
    };
    check_encode(1u, &b1, 2u, 2u);

    const uint16_t b2_counts[3] = { 0u, 1u, 0u };
    const uint8_t b2_runs[1] = { 5u };
    const uint32_t b2_levels[1] = { 2u };
    const tc_v7_band_input b2 = {
        3u, NULL, b2_counts, b2_runs, b2_levels, 1u
    };
    check_encode(2u, &b2, 0u, 1u);

    /* B3 spans eight scan positions.  Its direct pair-count map must encode
     * a legal count of 8 without truncating it to 0 (the historical 3-bit
     * field desynchronised a real 4K V7-A stream at this point). */
    const uint16_t b3_counts[3] = { 8u, 0u, 0u };
    const uint8_t b3_runs[8] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    const uint32_t b3_levels[8] = { 2u, 4u, 6u, 8u, 10u, 12u, 14u, 16u };
    const tc_v7_band_input b3 = {
        3u, NULL, b3_counts, b3_runs, b3_levels, 8u
    };
    check_encode(3u, &b3, 0u, 1u);

    const uint16_t b4_counts[3] = { 1u, 0u, 1u };
    const uint8_t b4_runs[2] = { 0u, 37u };
    const uint32_t b4_levels[2] = { 12u, 14u };
    const tc_v7_band_input b4 = {
        3u, NULL, b4_counts, b4_runs, b4_levels, 2u
    };
    check_encode(4u, &b4, 0u, 4u);

    /* 输入在任何 bitwriter 写入前被拒绝，避免产生半个非法 segment。 */
    {
        const uint8_t bad_run[2] = { 3u, 3u };
        const uint32_t bad_level[2] = { 1u, 1u };
        const uint16_t count[1] = { 2u };
        const tc_v7_band_input bad = { 1u, dc_m, count, bad_run, bad_level, 2u };
        uint64_t bits = 0u;
        MT_CHECK_EQ_I64(tc_v7_band_bits(0u, 0u, 0u, &bad, &bits), TC_ERR_MALFORMED);
        MT_CHECK_EQ_I64(tc_v7_band_bits(5u, 0u, 0u, &b0, &bits), TC_ERR_INVALID_ARGUMENT);
        MT_CHECK_EQ_I64(tc_v7_band_bits(0u, 15u, 0u, &b0, &bits), TC_ERR_INVALID_ARGUMENT);
    }
    {
        tc_bitwriter bw;
        MT_CHECK_EQ_I64(tc_bitwriter_init(&bw), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_put_bit(&bw, 1u), TC_OK);
        uint32_t size = 0u, crc = 0u;
        MT_CHECK_EQ_I64(tc_v7_band_encode(0u, 0u, 0u, &b0, &bw, &size, &crc), TC_ERR_STATE);
        tc_bitwriter_free(&bw);
    }

    /* active map、syntax header、zero padding 和任意截断均 fail closed。 */
    {
        tc_bitwriter bw;
        MT_CHECK_EQ_I64(tc_bitwriter_init(&bw), TC_OK);
        uint32_t size = 0u, crc = 0u;
        MT_CHECK_EQ_I64(tc_v7_band_encode(1u, 2u, 2u, &b1, &bw, &size, &crc), TC_OK);
        uint8_t copy[256];
        MT_CHECK(size <= sizeof(copy));
        if (size <= sizeof(copy)) {
            memcpy(copy, tc_bitwriter_data(&bw), size);
            copy[0] = 2u;
            int32_t q_zig[3u * 64u] = { 0 };
            MT_CHECK_EQ_I64(tc_v7_band_decode(copy, size, 1u, 3u, 3u, q_zig),
                            TC_ERR_UNSUPPORTED_VERSION);
            memcpy(copy, tc_bitwriter_data(&bw), size);
            copy[3] = 0x80u;
            MT_CHECK_EQ_I64(tc_v7_band_decode(copy, size, 1u, 3u, 3u, q_zig),
                            TC_ERR_UNSUPPORTED_VERSION);
            memcpy(copy, tc_bitwriter_data(&bw), size);
            copy[4] |= 0x38u; /* B1 direct map: block 1 pair_count becomes 7 */
            MT_CHECK_EQ_I64(tc_v7_band_decode(copy, size, 1u, 3u, 3u, q_zig),
                            TC_ERR_MALFORMED);
            memcpy(copy, tc_bitwriter_data(&bw), size);
            for (uint32_t cut = 0u; cut < size; ++cut) {
                MT_CHECK(tc_v7_band_decode(copy, cut, 1u, 3u, 3u, q_zig) < 0);
            }
        }
        tc_bitwriter_free(&bw);
    }

    return MT_MAIN_RETURN();
}
