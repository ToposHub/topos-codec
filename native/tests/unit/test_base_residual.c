/* RD4-03：固定 base 预测与 signed residual 闭环。 */
#include "codec/base_residual.h"
#include "mini_test.h"
#include "transform/base_scale.h"

#include <string.h>

int main(void)
{
    const uint16_t base[2][2] = {{0u, 100u}, {200u, 300u}};
    uint16_t prediction[4][5];
    memset(prediction, 0, sizeof(prediction));
    MT_CHECK(tc_base_upsample_u16(&base[0][0], 2u, 2u, 2u,
                                  &prediction[0][0], 4u, 4u, 5u, 16u));

    static const int32_t delta[4][4] = {
        {0, 3, 0, -5},
        {7, 0, -4, 2},
        {0, -6, 8, 0},
        {5, 0, -3, 9}
    };
    uint16_t source[4][6];
    int32_t residual[4][6];
    uint16_t restored[4][6];
    memset(source, 0, sizeof(source));
    memset(residual, 0xA5, sizeof(residual));
    memset(restored, 0xA5, sizeof(restored));
    for (uint32_t y = 0u; y < 4u; ++y) {
        for (uint32_t x = 0u; x < 4u; ++x) {
            source[y][x] = (uint16_t)((int32_t)prediction[y][x] + delta[y][x]);
        }
    }

    MT_CHECK_EQ_I64(tc_base_residual_build_u16(&source[0][0], 4u, 4u, 6u,
                                               &base[0][0], 2u, 2u, 2u,
                                               &residual[0][0], 6u, 16u), TC_OK);
    for (uint32_t y = 0u; y < 4u; ++y) {
        for (uint32_t x = 0u; x < 4u; ++x) {
            MT_CHECK_EQ_I64(residual[y][x], delta[y][x]);
        }
        MT_CHECK_EQ_U64((uint32_t)residual[y][4], 0xA5A5A5A5u);
        MT_CHECK_EQ_U64((uint32_t)residual[y][5], 0xA5A5A5A5u);
    }

    /* Exercise non-even geometry and padded strides against the predictor
     * reference.  The residual hot path caches axis coordinates, so this
     * catches drift in edge extension or half-pixel rounding. */
    uint16_t base_odd[2][4] = {{17u, 203u, 409u, 0u}, {701u, 911u, 1203u, 0u}};
    uint16_t source_odd[5][9];
    int32_t residual_odd[5][10];
    memset(source_odd, 0, sizeof(source_odd));
    memset(residual_odd, 0xA5, sizeof(residual_odd));
    for (uint32_t y = 0u; y < 5u; ++y) {
        for (uint32_t x = 0u; x < 7u; ++x) {
            const uint16_t prediction = tc_base_upsample_pixel_unchecked_u16(
                &base_odd[0][0], 3u, 2u, 4u, 7u, 5u, x, y, 4095u);
            const int32_t delta_odd = (int32_t)((x * 3u + y * 5u) % 7u) - 3;
            source_odd[y][x] = (uint16_t)((int32_t)prediction + delta_odd);
        }
    }
    MT_CHECK_EQ_I64(tc_base_residual_build_u16(
        &source_odd[0][0], 7u, 5u, 9u, &base_odd[0][0], 3u, 2u, 4u,
        &residual_odd[0][0], 10u, 12u), TC_OK);
    for (uint32_t y = 0u; y < 5u; ++y) {
        for (uint32_t x = 0u; x < 7u; ++x) {
            const int32_t expected = (int32_t)((x * 3u + y * 5u) % 7u) - 3;
            MT_CHECK_EQ_I64(residual_odd[y][x], expected);
        }
    }
    MT_CHECK_EQ_I64(tc_base_residual_reconstruct_u16(&base[0][0], 2u, 2u, 2u,
                                                       &residual[0][0], 4u, 4u, 6u,
                                                       &restored[0][0], 6u, 16u), TC_OK);
    for (uint32_t y = 0u; y < 4u; ++y) {
        for (uint32_t x = 0u; x < 4u; ++x) {
            MT_CHECK_EQ_U64(restored[y][x], source[y][x]);
        }
        MT_CHECK_EQ_U64(restored[y][4], 0xA5A5u);
        MT_CHECK_EQ_U64(restored[y][5], 0xA5A5u);
    }

    uint16_t full[4][6];
    memset(full, 0xA5, sizeof(full));
    MT_CHECK_EQ_I64(tc_base_full_reconstruct_u16(&base[0][0], 2u, 2u, 2u,
                                                  &residual[0][0], 4u, 4u, 6u,
                                                  &full[0][0], 6u, 1u, 1u,
                                                  0u, 16u), TC_OK);
    for (uint32_t y = 0u; y < 4u; ++y) {
        for (uint32_t x = 0u; x < 4u; ++x) {
            MT_CHECK_EQ_U64(full[y][x], source[y][x]);
        }
    }
    MT_CHECK_EQ_U64(full[0][4], 0xA5A5u);
    MT_CHECK_EQ_U64(full[0][5], 0xA5A5u);

    memset(full, 0xA5, sizeof(full));
    MT_CHECK_EQ_I64(tc_base_full_reconstruct_band_u16(
        &base[0][0], 2u, 2u, 2u, &residual[0][0], 4u, 4u, 6u,
        &full[0][0], 6u, 1u, 2u, 1u, 0u, 32768u, 16u), TC_OK);
    for (uint32_t y = 0u; y < 1u; ++y) {
        for (uint32_t x = 0u; x < 4u; ++x) { MT_CHECK_EQ_U64(full[y][x], 0xA5A5u); }
    }
    for (uint32_t y = 1u; y < 3u; ++y) {
        for (uint32_t x = 0u; x < 4u; ++x) { MT_CHECK_EQ_U64(full[y][x], prediction[y][x]); }
    }
    for (uint32_t y = 3u; y < 4u; ++y) {
        for (uint32_t x = 0u; x < 4u; ++x) { MT_CHECK_EQ_U64(full[y][x], 0xA5A5u); }
    }

    const uint16_t zero_base[1][1] = {{0u}};
    int32_t positive[2][2] = {{100, 100}, {100, 100}};
    uint16_t clipped[2][2];
    MT_CHECK_EQ_I64(tc_base_full_reconstruct_u16(
        &zero_base[0][0], 1u, 1u, 1u, &positive[0][0], 2u, 2u, 2u,
        &clipped[0][0], 2u, 1u, 1u, 1u, 16u), TC_OK);
    MT_CHECK_EQ_U64(clipped[0][0], 100u);
    MT_CHECK_EQ_U64(clipped[1][1], 100u);

    memset(clipped, 0, sizeof(clipped));
    MT_CHECK_EQ_I64(tc_base_full_reconstruct_u16(
        &zero_base[0][0], 1u, 1u, 1u, NULL, 2u, 2u, 0u,
        &clipped[0][0], 2u, 1u, 0u, 777u, 10u), TC_OK);
    MT_CHECK_EQ_U64(clipped[0][0], 0u);
    MT_CHECK_EQ_U64(clipped[1][1], 0u);

    memset(clipped, 0, sizeof(clipped));
    MT_CHECK_EQ_I64(tc_base_full_reconstruct_u16(
        NULL, 1u, 1u, 1u, NULL, 2u, 2u, 0u,
        &clipped[0][0], 2u, 0u, 0u, 512u, 10u), TC_OK);
    MT_CHECK_EQ_U64(clipped[0][0], 512u);
    MT_CHECK_EQ_U64(clipped[1][1], 512u);

    MT_CHECK_EQ_I64(tc_base_full_reconstruct_u16(
        NULL, 1u, 1u, 1u, NULL, 2u, 2u, 0u,
        &clipped[0][0], 2u, 1u, 0u, 512u, 10u), TC_ERR_INVALID_ARGUMENT);

    tc_bitwriter bw;
    MT_CHECK_EQ_I64(tc_bitwriter_init(&bw), TC_OK);
    uint32_t k_level = 0u, k_run = 0u;
    MT_CHECK_EQ_I64(tc_base_residual_encode(&residual[0][0], 4u, 4u, 6u,
                                            &bw, &k_level, &k_run), TC_OK);
    MT_CHECK(k_level <= 14u && k_run <= 14u);
    MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
    int32_t decoded[4][6];
    memset(decoded, 0x5A, sizeof(decoded));
    tc_bitreader br;
    tc_bitreader_init(&br, tc_bitwriter_data(&bw), tc_bitwriter_byte_size(&bw));
    MT_CHECK_EQ_I64(tc_base_residual_decode(&br, &decoded[0][0], 4u, 4u, 6u,
                                            k_level, k_run), TC_OK);
    MT_CHECK_EQ_I64(tc_bitreader_align_byte(&br), TC_OK);
    MT_CHECK_EQ_U64(tc_bitreader_bits_consumed(&br), tc_bitwriter_bits_written(&bw));
    for (uint32_t y = 0u; y < 4u; ++y) {
        for (uint32_t x = 0u; x < 4u; ++x) {
            MT_CHECK_EQ_I64(decoded[y][x], delta[y][x]);
        }
    }
    tc_bitwriter_free(&bw);

    /* The scalable writer collects Rice parameters during residual build and
     * must emit the same byte stream as the compatibility two-pass API. */
    int32_t residual_fused[4][6];
    uint32_t fused_k_level = 0u, fused_k_run = 0u;
    MT_CHECK_EQ_I64(tc_base_residual_build_u16_with_params(
        &source[0][0], 4u, 4u, 6u, &base[0][0], 2u, 2u, 2u,
        &residual_fused[0][0], 6u, 16u, &fused_k_level, &fused_k_run), TC_OK);
    tc_bitwriter reference_bw;
    tc_bitwriter fused_bw;
    MT_CHECK_EQ_I64(tc_bitwriter_init(&reference_bw), TC_OK);
    MT_CHECK_EQ_I64(tc_bitwriter_init(&fused_bw), TC_OK);
    uint32_t reference_k_level = 0u, reference_k_run = 0u;
    MT_CHECK_EQ_I64(tc_base_residual_encode(
        &residual_fused[0][0], 4u, 4u, 6u, &reference_bw,
        &reference_k_level, &reference_k_run), TC_OK);
    MT_CHECK_EQ_I64(tc_base_residual_encode_with_params(
        &residual_fused[0][0], 4u, 4u, 6u, &fused_bw,
        fused_k_level, fused_k_run), TC_OK);
    MT_CHECK_EQ_U64(fused_k_level, reference_k_level);
    MT_CHECK_EQ_U64(fused_k_run, reference_k_run);
    MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&reference_bw), TC_OK);
    MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&fused_bw), TC_OK);
    MT_CHECK_EQ_U64(tc_bitwriter_byte_size(&fused_bw),
                    tc_bitwriter_byte_size(&reference_bw));
    MT_CHECK(memcmp(tc_bitwriter_data(&fused_bw), tc_bitwriter_data(&reference_bw),
                    tc_bitwriter_byte_size(&reference_bw)) == 0);
    tc_bitwriter_free(&reference_bw);
    tc_bitwriter_free(&fused_bw);

    /* Quantized scalable residuals use symmetric rounding before Rice coding;
     * decode multiplies the stored values by the packet's reconstruction step. */
    int32_t residual_quantized[4][6];
    MT_CHECK_EQ_I64(tc_base_residual_build_quantized_u16_with_params(
        &source[0][0], 4u, 4u, 6u, &base[0][0], 2u, 2u, 2u,
        &residual_quantized[0][0], 6u, 16u, 2u, NULL, NULL), TC_OK);
    MT_CHECK_EQ_I64(residual_quantized[0][1], 2);  /* +3 -> +2 * 2 */
    MT_CHECK_EQ_I64(residual_quantized[0][3], -3); /* -5 -> -3 * 2 */
    MT_CHECK_EQ_I64(tc_base_residual_scale_in_place(&residual_quantized[0][0],
                                                     4u, 4u, 6u, 2u), TC_OK);
    MT_CHECK_EQ_I64(residual_quantized[0][1], 4);
    MT_CHECK_EQ_I64(residual_quantized[0][3], -6);
    int32_t overflow_residual[1][1] = {{INT32_MAX}};
    MT_CHECK_EQ_I64(tc_base_residual_scale_in_place(&overflow_residual[0][0],
                                                     1u, 1u, 1u, 2u),
                    TC_ERR_MALFORMED);

    int32_t bad[2][2] = {{65536, 0}, {0, 0}};
    MT_CHECK_EQ_I64(tc_base_residual_encode(&bad[0][0], 2u, 2u, 2u,
                                            &bw, &k_level, &k_run),
                    TC_ERR_INVALID_ARGUMENT);
    MT_MAIN_RETURN();
}
