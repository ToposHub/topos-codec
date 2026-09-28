#include "slice_map.h"

#include <string.h>

#include "../common/endian.h"
#include "../common/error.h"
#include "../transform/quant.h"

int32_t tc_slice_header_encode(const topos_slice_header* sh, uint8_t out[TC_SLICE_HEADER_SIZE])
{
    tc_store_be32(out + 0, sh->slice_payload_size);
    out[4] = sh->plane;
    tc_store_be16(out + 5, sh->block_y0);
    tc_store_be16(out + 7, sh->block_h);
    out[9] = sh->qp_delta_biased;
    out[10] = sh->k1;
    out[11] = sh->k2;
    out[12] = sh->k3;
    tc_store_be32(out + 13, sh->slice_crc32);
    return TC_OK;
}

int32_t tc_slice_header_decode(const uint8_t* data, size_t size, topos_slice_header* sh)
{
    if (size < TC_SLICE_HEADER_SIZE) {
        tc_set_error(TC_ERR_TRUNCATED, "slice header needs %u bytes, got %zu",
                     (unsigned)TC_SLICE_HEADER_SIZE, size);
        return TC_ERR_TRUNCATED;
    }
    sh->slice_payload_size = tc_load_be32(data + 0);
    sh->plane = data[4];
    sh->block_y0 = tc_load_be16(data + 5);
    sh->block_h = tc_load_be16(data + 7);
    sh->qp_delta_biased = data[9];
    sh->k1 = data[10];
    sh->k2 = data[11];
    sh->k3 = data[12];
    sh->slice_crc32 = tc_load_be32(data + 13);
    if (sh->k1 > 14u || sh->k2 > 14u || sh->k3 > 14u) {
        tc_set_error(TC_ERR_MALFORMED, "slice k1/k2/k3 > 14: %u/%u/%u",
                     (unsigned)sh->k1, (unsigned)sh->k2, (unsigned)sh->k3);
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

int32_t tc_slice_map_validate(const topos_frame_header* fh,
                              const topos_slice_header* slices, uint16_t slice_count)
{
    if (slice_count != fh->slice_count) {
        tc_set_error(TC_ERR_MALFORMED, "slice_count mismatch %u != header %u",
                     (unsigned)slice_count, (unsigned)fh->slice_count);
        return TC_ERR_MALFORMED;
    }
    uint32_t idx = 0u;
    for (uint32_t plane = 0u; plane < fh->plane_count; ++plane) {
        uint32_t covered = 0u; /* 已覆盖的块行数 */
        uint32_t rows = fh->plane_block_rows[plane];
        while (covered < rows) {
            if (idx >= slice_count) {
                tc_set_error(TC_ERR_MALFORMED,
                             "plane %u tiling incomplete at block row %u (slices exhausted)",
                             (unsigned)plane, (unsigned)covered);
                return TC_ERR_MALFORMED;
            }
            const topos_slice_header* sh = &slices[idx];
            if (sh->plane != (uint8_t)plane) {
                tc_set_error(TC_ERR_MALFORMED, "slice %u plane %u breaks Y->U->V->[A] order",
                             (unsigned)idx, (unsigned)sh->plane);
                return TC_ERR_MALFORMED;
            }
            if (sh->k1 > 14u || sh->k2 > 14u || sh->k3 > 14u) {
                tc_set_error(TC_ERR_MALFORMED, "slice %u k1/k2/k3 > 14: %u/%u/%u",
                             (unsigned)idx, (unsigned)sh->k1, (unsigned)sh->k2,
                             (unsigned)sh->k3);
                return TC_ERR_MALFORMED;
            }
            /* V2 entropy_mode=1：颜色 slice 的 k 字段是 book id（0..3）；
             * alpha（plane 3，仅带 alpha 帧）恒 Rice（k1/k2 ≤14、k3==0，
             * 上方已覆盖 ≤14）。pf=3（TRAW）无 alpha，4 平面全是颜色平面 */
            if (fh->version_major >= 2u &&
                (fh->entropy_mode == 1u ||
                 (fh->version_major == 4u && fh->entropy_mode == 2u) ||
                 (fh->version_major == 5u && fh->entropy_mode == 3u)) &&
                (plane < 3u || fh->alpha_mode == 0u) &&
                (sh->k1 > 3u || sh->k2 > 3u || sh->k3 > 3u)) {
                tc_set_error(TC_ERR_MALFORMED,
                             "slice %u vlc book id > 3: %u/%u/%u",
                             (unsigned)idx, (unsigned)sh->k1, (unsigned)sh->k2,
                             (unsigned)sh->k3);
                return TC_ERR_MALFORMED;
            }
            /* V7-R/R2/R3（ADR-C034/C036/C048）：颜色 slice 模型/计数表在
             * payload 前缀，k1/k2/k3 无语义——恒 0 入流（alpha 恒 Rice，
             * 下方独立规则；pf=3 无 alpha，4 平面全部适用本规则） */
            if (fh->version_major == 7u &&
                (fh->entropy_mode == 6u || fh->entropy_mode == 7u ||
                 fh->entropy_mode == 8u) &&
                (plane < 3u || fh->alpha_mode == 0u) &&
                (sh->k1 != 0u || sh->k2 != 0u || sh->k3 != 0u)) {
                tc_set_error(TC_ERR_MALFORMED,
                             "slice %u rans k1/k2/k3 != 0: %u/%u/%u",
                             (unsigned)idx, (unsigned)sh->k1, (unsigned)sh->k2,
                             (unsigned)sh->k3);
                return TC_ERR_MALFORMED;
            }
            if (sh->block_y0 != (uint16_t)covered) {
                tc_set_error(TC_ERR_MALFORMED,
                             "slice %u block_y0 %u != expected %u (gap/overlap/out of order)",
                             (unsigned)idx, (unsigned)sh->block_y0, (unsigned)covered);
                return TC_ERR_MALFORMED;
            }
            if (sh->block_h == 0u) {
                tc_set_error(TC_ERR_MALFORMED, "slice %u block_h == 0", (unsigned)idx);
                return TC_ERR_MALFORMED;
            }
            if ((uint32_t)sh->block_y0 + (uint32_t)sh->block_h > rows) {
                tc_set_error(TC_ERR_MALFORMED, "slice %u band [%u,+%u) exceeds plane rows %u",
                             (unsigned)idx, (unsigned)sh->block_y0, (unsigned)sh->block_h,
                             (unsigned)rows);
                return TC_ERR_MALFORMED;
            }
            int32_t qp = tc_slice_effective_qp(fh->qp_base, sh->qp_delta_biased);
            if (qp < 0 || qp > (int32_t)TC_QP_MAX) {
                tc_set_error(TC_ERR_MALFORMED, "slice %u effective qp %d outside 0..%u",
                             (unsigned)idx, qp, (unsigned)TC_QP_MAX);
                return TC_ERR_MALFORMED;
            }
            if (fh->alpha_mode != 0u && plane == 3u && sh->k3 != 0u) {
                tc_set_error(TC_ERR_MALFORMED, "alpha slice %u k3 != 0", (unsigned)idx);
                return TC_ERR_MALFORMED;
            }
            covered += sh->block_h;
            idx++;
        }
    }
    if (idx != slice_count) {
        tc_set_error(TC_ERR_MALFORMED, "%u trailing slices after full tiling (used %u)",
                     (unsigned)(slice_count - (uint16_t)idx), (unsigned)idx);
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}
