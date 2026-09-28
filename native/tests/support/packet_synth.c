#include "packet_synth.h"

#include <stdlib.h>
#include <string.h>

#include "../../src/bitstream/frame_header.h"
#include "../../src/bitstream/packet.h"
#include "../../src/bitstream/slice_codec.h"
#include "../../src/bitstream/slice_map.h"
#include "../../src/common/crc32.h"
#include "../../src/common/endian.h"
#include "../../src/common/error.h"
#include "../../src/entropy/scan.h"

static const packet_synth_cfg kCfgs[PACKET_SYNTH_CFG_COUNT] = {
    /* seed, vw, vh, alpha, bands, qp_base, qmatrix, bit_depth, pixel_format, profile */
    { 0xA1B2C3D4E5F60718ull, 8u, 8u, 0u, 1u, 4u, 0u, 0u, 0u, 0u },
    { 0x1122334455667788ull, 16u, 16u, 0u, 1u, 20u, 0u, 0u, 0u, 0u },
    { 0x2468ACE02468ACE0ull, 36u, 20u, 0u, 2u, 40u, 0u, 0u, 0u, 0u },
    { 0xFEDCBA9876543210ull, 64u, 32u, 1u, 3u, 0u, 0u, 0u, 0u, 0u },
    { 0x0F0F0F0F0F0F0F0Full, 128u, 72u, 1u, 4u, 63u, 0u, 0u, 0u, 0u },
    { 0x13579BDF2468ACE0ull, 48u, 48u, 1u, 5u, 12u, 0u, 0u, 0u, 0u }
};

const packet_synth_cfg* packet_synth_cfg_at(unsigned index)
{
    if (index >= PACKET_SYNTH_CFG_COUNT) { return NULL; }
    return &kCfgs[index];
}

/* xorshift64*：合成器唯一随机源（平台无关确定性） */
static uint64_t synth_next(uint64_t* s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

/* 量化块生成：多数为零、稀疏非零（含大值与负值），DC 连续缓慢变化。
 * R6：tc_color_slice_encode 输入契约为 zigzag 扫描序——按 kTcZigzagInv
 * 散射存储，符号序列与 natural 序旧路径逐位一致（golden 字节不变）。 */
static void synth_blocks(uint64_t* s, int32_t* blocks, size_t block_count)
{
    int32_t dc = (int32_t)(synth_next(s) % 500u);
    for (size_t b = 0; b < block_count; ++b) {
        int32_t* q = blocks + b * 64u;
        for (uint32_t i = 0; i < 64u; ++i) { q[i] = 0; }
        q[0] = dc;
        dc += (int32_t)(synth_next(s) % 7u) - 3; /* 行进式 DC（差分小、非零） */
        uint32_t nonzeros = (uint32_t)(synth_next(s) % 6u); /* 0..5 个 AC */
        for (uint32_t n = 0; n < nonzeros; ++n) {
            uint32_t pos = (uint32_t)(synth_next(s) % 64u);
            uint32_t mag = (uint32_t)(synth_next(s) % 4000u) + 1u;
            q[kTcZigzagInv[pos]] = (synth_next(s) & 1ull) != 0ull ? (int32_t)mag : -(int32_t)mag;
        }
    }
}

static void synth_residuals(uint64_t* s, int32_t* residuals, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        uint64_t r = synth_next(s);
        if ((r % 5ull) == 0ull) {
            residuals[i] = (int32_t)((int64_t)(synth_next(s) % 131071ull) - 65535);
        } else {
            residuals[i] = 0; /* 长零游程：吃 run 路径 */
        }
    }
}

int32_t packet_synth_build(const packet_synth_cfg* cfg, uint8_t** out_data, size_t* out_size)
{
    *out_data = NULL;
    *out_size = 0;
    uint64_t s = cfg->seed;

    topos_frame_header fh;
    memset(&fh, 0, sizeof(fh));
    fh.version_major = cfg->entropy_mode == 1u ? 2u : 1u; /* M9：V2+VLC */
    fh.flags = 0u;
    fh.profile = (cfg->profile == 5u || cfg->profile == 6u || cfg->profile == 7u)
                     ? cfg->profile : 3u; /* R4.4 / TRAW 批 1 */
    fh.pixel_format = cfg->pixel_format <= 3u ? cfg->pixel_format : 0u; /* R4.2/R4.3/TRAW */
    fh.bit_depth = cfg->bit_depth == 12u ? 12u : 10u; /* R4.1 */
    /* R4.1/R4.2/R4.3/v1.6：扩展枚举流写所属扩展代 minor（pf=2 → 3、pf=1 → 2、
     * bd=12 → 1、pf=3 → 5）；都不用时为 v1.0 语义流（minor=0，历史输出不变） */
    fh.version_minor = (fh.pixel_format == 3u) ? 5u
                     : (fh.pixel_format == 2u) ? 3u
                     : (fh.pixel_format == 1u) ? 2u
                     : (fh.bit_depth != 10u) ? 1u : 0u;
    if (fh.version_major == 2u) {
        /* M9（ADR-C027 D-6）：V2 minor 恒 0，全枚举开放；熵字段激活 */
        fh.version_minor = 0u;
        fh.entropy_mode = 1u;
        fh.codebook_version = 1u;
    }

    fh.alpha_mode = cfg->with_alpha ? 1u : 0u;
    fh.alpha_bit_depth = cfg->with_alpha ? 16u : 0u;
    fh.frame_type = 0u;
    fh.coded_width = (uint16_t)(((uint32_t)cfg->visible_w + 7u) / 8u * 8u);
    fh.coded_height = (uint16_t)(((uint32_t)cfg->visible_h + 7u) / 8u * 8u);
    fh.visible_width = cfg->visible_w;
    fh.visible_height = cfg->visible_h;
    /* TRAW CFA：恒 4 相位平面无 alpha；其余 3 平面 + 可选 alpha */
    fh.plane_count = (uint8_t)(cfg->pixel_format == 3u
                                   ? 4u
                                   : 3u + (cfg->with_alpha ? 1u : 0u));
    fh.qmatrix_id = (cfg->pixel_format == 3u) ? 0u : cfg->qmatrix_id; /* TRAW 冻结 qm0 */
    fh.qp_base = cfg->qp_base;
    fh.color_range = 1u;
    fh.color_primaries = 1u;
    fh.color_transfer = (cfg->pixel_format == 3u) ? 20u : 1u; /* TRAW_LOG0 */
    fh.color_matrix = (fh.pixel_format == 2u || fh.pixel_format == 3u) ? 0u : 1u; /* GBR/CFA=identity */
    fh.chroma_siting = 0u;
    fh.sar_num = 0u;
    fh.sar_den = 0u;

    int32_t rc = tc_frame_derive_geometry(&fh);
    if (rc != TC_OK) { return rc; }

    if (cfg->bands == 0u || cfg->bands > fh.plane_block_rows[0]) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "synth bands %u invalid", (unsigned)cfg->bands);
        return TC_ERR_INVALID_ARGUMENT;
    }
    uint16_t slice_count = (uint16_t)((uint32_t)fh.plane_count * (uint32_t)cfg->bands);
    if (slice_count > TC_MAX_SLICE_COUNT) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "synth slice count overflow");
        return TC_ERR_INVALID_ARGUMENT;
    }
    fh.slice_count = slice_count;

    /* 先产出全部 slice（payload + header），再回填 frame_packet_size/总装配 */
    uint32_t total_slices = slice_count;
    size_t slice_cap = 64u * 1024u * 1024u; /* 合成包上限（synth 输入本身可控） */
    uint8_t* assembled = (uint8_t*)malloc(slice_cap);
    if (assembled == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "synth assembly buffer");
        return TC_ERR_OUT_OF_MEMORY;
    }
    size_t off = TC_FRAME_HEADER_SIZE;

    tc_bitwriter bw;
    rc = tc_bitwriter_init(&bw);
    if (rc != TC_OK) {
        free(assembled);
        return rc;
    }

    uint32_t si = 0u;
    for (uint32_t plane = 0u; plane < fh.plane_count && rc == TC_OK; ++plane) {
        uint32_t rows = fh.plane_block_rows[plane];
        uint32_t bands = cfg->bands;
        uint32_t base = rows / bands;
        uint32_t rem = rows % bands;
        uint32_t y0 = 0u;
        for (uint32_t b = 0u; b < bands && rc == TC_OK; ++b) {
            uint32_t h = base + (b < rem ? 1u : 0u);
            tc_bitwriter_reset(&bw);

            topos_slice_header sh;
            memset(&sh, 0, sizeof(sh));
            sh.plane = (uint8_t)plane;
            sh.block_y0 = (uint16_t)y0;
            sh.block_h = (uint16_t)h;
            sh.qp_delta_biased = 64u; /* qp_eff == qp_base */
            if (fh.version_major == 2u && !(plane == 3u && cfg->with_alpha)) {
                /* M9：VLC 颜色 slice 的 k 字段 = book id（0..3）；
                 * alpha 恒 Rice（k1/k2 ≤14） */
                sh.k1 = (uint8_t)(synth_next(&s) % 4u);
                sh.k2 = (uint8_t)(synth_next(&s) % 4u);
                sh.k3 = (uint8_t)(synth_next(&s) % 4u);
            } else {
                sh.k1 = (uint8_t)(synth_next(&s) % 15u);
                sh.k2 = (uint8_t)(synth_next(&s) % 15u);
                sh.k3 = (plane == 3u && cfg->with_alpha)
                            ? 0u
                            : (uint8_t)(synth_next(&s) % 15u); /* pf=3 plane3=颜色平面 */
            }

            if (plane == 3u && cfg->with_alpha) { /* pf=3 的 plane 3 = B 相位（颜色路径） */
                size_t pixels = (size_t)fh.plane_coded_w[3] * ((size_t)h * 8u);
                int32_t* residuals = (int32_t*)malloc(pixels * sizeof(int32_t));
                if (residuals == NULL) {
                    rc = TC_ERR_OUT_OF_MEMORY;
                    break;
                }
                synth_residuals(&s, residuals, pixels);
                rc = tc_alpha_slice_encode(&fh, &sh, residuals, pixels, &bw);
                free(residuals);
            } else {
                size_t blocks = (size_t)fh.plane_block_cols[plane] * (size_t)h;
                int32_t* blocks_mem = (int32_t*)malloc(blocks * 64u * sizeof(int32_t));
                if (blocks_mem == NULL) {
                    rc = TC_ERR_OUT_OF_MEMORY;
                    break;
                }
                synth_blocks(&s, blocks_mem, blocks);
                rc = tc_color_slice_encode(&fh, &sh, blocks_mem, &bw);
                free(blocks_mem);
            }
            if (rc != TC_OK) { break; }

            rc = tc_bitwriter_flush_zero_pad(&bw);
            if (rc != TC_OK) { break; }
            size_t payload_size = tc_bitwriter_byte_size(&bw);
            sh.slice_payload_size = (uint32_t)payload_size;
            sh.slice_crc32 = tc_crc32(tc_bitwriter_data(&bw), payload_size);

            rc = tc_slice_header_encode(&sh, assembled + off);
            if (rc != TC_OK) { break; }
            off += TC_SLICE_HEADER_SIZE;
            memcpy(assembled + off, tc_bitwriter_data(&bw), payload_size);
            off += payload_size;
            y0 += h;
            si++;
        }
    }
    tc_bitwriter_free(&bw);
    if (rc != TC_OK) {
        free(assembled);
        return rc;
    }
    if (si != total_slices) {
        free(assembled);
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "synth produced %u slices, expected %u",
                     (unsigned)si, (unsigned)total_slices);
        return TC_ERR_INVALID_ARGUMENT;
    }

    fh.frame_packet_size = (uint32_t)off;
    rc = tc_frame_header_encode(&fh, assembled);
    if (rc != TC_OK) {
        free(assembled);
        return rc;
    }

    /* 自证：合成包必须能通过完整扫描（synth bug = 立即失败，不产出无效样本） */
    topos_packet_view view;
    rc = tc_packet_scan(assembled, off, &view);
    if (rc != TC_OK) {
        free(assembled);
        return rc;
    }

    *out_data = assembled;
    *out_size = off;
    return TC_OK;
}
