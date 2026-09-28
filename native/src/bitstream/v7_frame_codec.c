#include "v7_frame_codec.h"

#include <string.h>

#include "../codec/codec.h"
#include "../codec/color_store.h"
#include "../common/alloc.h"
#include "../common/checked.h"
#include "../common/error.h"
#include "../simd/dispatch.h"
#include "../transform/quant.h"
#include "../transform/transform.h"
#include "band_tokens.h"
#include "frame_header.h"
#include "layer_directory.h"
#include "../entropy/scan.h"

static uint32_t v7_bd_mid(uint8_t bit_depth)
{
    return UINT32_C(1) << (bit_depth - 1u);
}

static uint32_t v7_bd_max(uint8_t bit_depth)
{
    return (UINT32_C(1) << bit_depth) - 1u;
}

static uint32_t v7_qp_effective(const topos_frame_header* fh)
{
    /* 批 4 复查：与 codec.c bd_qp_offset（2·(bd−10)）同式对齐——bd10→0、
     * bd12→4 行为不变（12-bit golden 零变化），bd16 → 12 */
    uint32_t qp = (uint32_t)fh->qp_base
        + (fh->bit_depth >= 12u ? 2u * (fh->bit_depth - 10u) : 0u);
    /* v1.5：qp_base≤63 顶格 63 不变；≥64 放开到 95 */
    return qp > tc_qp_eff_ceiling(fh->qp_base) ? tc_qp_eff_ceiling(fh->qp_base) : qp;
}

static uint32_t v7_output_plane_width(const topos_frame_header* fh,
                                      uint32_t plane, uint32_t target_width)
{
    if (fh->pixel_format == 0u && (plane == 1u || plane == 2u)) {
        return (target_width + 1u) / 2u;
    }
    return target_width;
}

static uint8_t v7_max_scan_pos(uint32_t max_band)
{
    static const uint8_t limits[TC_V7_BAND_COUNT] = { 4u, 10u, 16u, 24u, 63u };
    return limits[max_band];
}

static int32_t v7_validate_output(const topos_frame_header* fh,
                                  uint32_t target_width, uint32_t target_height,
                                  uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                  const size_t strides[TC_FRAME_MAX_PLANES])
{
    if (planes_out == NULL) { return TC_OK; }
    for (uint32_t plane = 0u; plane < (uint32_t)fh->plane_count; ++plane) {
        if (planes_out[plane] == NULL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a output plane %u is NULL", plane);
            return TC_ERR_INVALID_ARGUMENT;
        }
        const uint32_t plane_width = v7_output_plane_width(fh, plane, target_width);
        size_t stride = strides != NULL && strides[plane] != 0u
                      ? strides[plane] : (size_t)plane_width;
        if (stride < (size_t)plane_width) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT,
                         "v7a output stride[%u]=%zu < visible width %u",
                         plane, stride, (unsigned)plane_width);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    (void)target_height;
    return TC_OK;
}

static void v7_store_q_scan(const tc_color_store_ctx* store,
                            uint32_t block, const int32_t* q_scan)
{
    int32_t q_nat[64];
    uint32_t rowmask = 1u;
    for (uint32_t scan_pos = 0u; scan_pos < 64u; ++scan_pos) {
        const int32_t value = q_scan[(size_t)block * 64u + scan_pos];
        const uint32_t natural = kTcZigzag[scan_pos];
        q_nat[natural] = value;
        if (scan_pos != 0u && value != 0) {
            rowmask |= UINT32_C(1) << (natural >> 3u);
        }
    }
    (void)tc_color_store_block((tc_color_store_ctx*)store, block, q_nat, rowmask);
}

int32_t tc_v7_frame_decode_scaled(const uint8_t* packet, size_t packet_size,
                                  uint32_t max_band,
                                  uint32_t target_width, uint32_t target_height,
                                  uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                  const size_t strides[TC_FRAME_MAX_PLANES],
                                  topos_frame_output* out_info,
                                  tc_v7_decode_stats* stats)
{
    if (packet == NULL || out_info == NULL || max_band >= TC_V7_BAND_COUNT) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a frame decode arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = (uint32_t)sizeof(*out_info);
    out_info->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    if (stats != NULL) { memset(stats, 0, sizeof(*stats)); }

    topos_frame_header fh;
    int32_t rc = tc_v7_packet_header_decode(packet, packet_size, &fh);
    if (rc != TC_OK) { return rc; }
    tc_v7_directory_view directory;
    rc = tc_v7_directory_parse(packet, packet_size, &fh, &directory);
    if (rc != TC_OK) { return rc; }
    if (fh.alpha_mode != 0u) {
        tc_set_error(TC_ERR_NOT_IMPLEMENTED,
                     "v7a band frame alpha decode is not implemented");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    /* TRAW（pf=3）走 rANS2 链；V7-A band 解码不承载 CFA（编码侧同拒） */
    if (fh.pixel_format == 3u) {
        tc_set_error(TC_ERR_NOT_IMPLEMENTED, "v7a band decode does not carry CFA");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    if (target_width == 0u && target_height == 0u) {
        target_width = fh.visible_width;
        target_height = fh.visible_height;
    }
    if (target_width == 0u || target_height == 0u ||
        target_width > fh.visible_width || target_height > fh.visible_height ||
        target_width > UINT16_MAX || target_height > UINT16_MAX) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "v7a target %ux%u exceeds source %ux%u",
                     (unsigned)target_width, (unsigned)target_height,
                     (unsigned)fh.visible_width, (unsigned)fh.visible_height);
        return TC_ERR_INVALID_ARGUMENT;
    }
    tc_fill_output_info(&fh, out_info);
    out_info->visible_width = (uint16_t)target_width;
    out_info->visible_height = (uint16_t)target_height;
    out_info->coded_width = (uint16_t)target_width;
    out_info->coded_height = (uint16_t)target_height;
    rc = v7_validate_output(&fh, target_width, target_height, planes_out, strides);
    if (rc != TC_OK || planes_out == NULL) { return rc; }

    const tc_qmatrix_set* qms = tc_qmatrix_by_id(fh.qmatrix_id);
    if (qms == NULL) {
        tc_set_error(TC_ERR_UNSUPPORTED_MATRIX, "v7a qmatrix_id %u", fh.qmatrix_id);
        return TC_ERR_UNSUPPORTED_MATRIX;
    }
    const tc_dequant_inverse_fn dinv =
        (fh.bit_depth > 12u) ? &tc_simd_dequant_inverse_wide
                             : tc_simd_resolve_dequant_inverse(0u);
    uint32_t max_blocks = 0u;
    for (uint32_t si = 0u; si < (uint32_t)directory.slice_count; ++si) {
        tc_v7_slice_desc slice;
        rc = tc_v7_directory_get_slice(&directory, si, &slice);
        if (rc != TC_OK) { return rc; }
        uint32_t cols = fh.plane_block_cols[slice.plane_index];
        uint32_t blocks = 0u;
        if (!tc_umul_u32((uint32_t)slice.block_h, cols, &blocks) || blocks > max_blocks) {
            max_blocks = blocks;
        }
    }
    size_t q_bytes = 0u;
    if (!tc_umul_size((size_t)max_blocks, 64u, &q_bytes) ||
        q_bytes > SIZE_MAX / sizeof(int32_t)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a q scratch size");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    int32_t* q_scan = (int32_t*)tc_alloc(q_bytes * sizeof(int32_t));
    if (q_scan == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "v7a q scratch allocation");
        return TC_ERR_OUT_OF_MEMORY;
    }

    for (uint32_t si = 0u; si < (uint32_t)directory.slice_count; ++si) {
        tc_v7_slice_desc slice;
        rc = tc_v7_directory_get_slice(&directory, si, &slice);
        if (rc != TC_OK) { break; }
        const uint32_t plane = slice.plane_index;
        const uint32_t cols = fh.plane_block_cols[plane];
        const uint32_t block_count = (uint32_t)slice.block_h * cols;
        tc_v7_decode_stats local_stats;
        memset(&local_stats, 0, sizeof(local_stats));
        rc = tc_v7_directory_decode_slice(&directory, si, max_band, cols,
                                           q_scan, &local_stats);
        if (stats != NULL) {
            stats->segments_requested += local_stats.segments_requested;
            stats->segments_read += local_stats.segments_read;
            stats->segments_skipped += local_stats.segments_skipped;
            stats->unchecked_segment_count += local_stats.unchecked_segment_count;
            stats->bytes_read += local_stats.bytes_read;
            stats->bytes_skipped += local_stats.bytes_skipped;
        }
        if (rc != TC_OK) { break; }

        const uint16_t* qm = plane == 0u ? qms->luma : qms->chroma;
        tc_quant_ctx qctx;
        tc_quant_ctx_init_bd(&qctx, qm, v7_qp_effective(&fh), fh.bit_depth);
        tc_color_store_ctx store;
        memset(&store, 0, sizeof(store));
        store.dst = planes_out[plane];
        const uint32_t source_width = fh.plane_visible_w[plane];
        const uint32_t source_height = fh.plane_visible_h[plane];
        const uint32_t target_plane_width = v7_output_plane_width(&fh, plane,
                                                                   target_width);
        store.stride = strides != NULL && strides[plane] != 0u
                     ? strides[plane] : (size_t)target_plane_width;
        store.qctx = &qctx;
        store.cols = cols;
        store.block_y0 = slice.block_y0;
        store.vis_w = source_width;
        store.vis_h = source_height;
        store.dst_w = target_plane_width;
        store.dst_h = target_height;
        store.scaled = (target_plane_width != source_width ||
                        target_height != source_height) ? 1u : 0u;
        store.coefficient_limit = v7_max_scan_pos(max_band);
        store.mid = v7_bd_mid(fh.bit_depth);
        store.max = v7_bd_max(fh.bit_depth);
        store.w0 = tc_transform_weights()[0];
        store.dinv = dinv;
        for (uint32_t block = 0u; block < block_count; ++block) {
            v7_store_q_scan(&store, block, q_scan);
        }
        out_info->slice_status[si] = TC_FRAME_SLICE_OK;
    }
    tc_free(q_scan);
    if (rc != TC_OK) {
        return rc;
    }
    return TC_OK;
}

int32_t tc_v7_frame_decode(const uint8_t* packet, size_t packet_size,
                           uint32_t max_band,
                           uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                           const size_t strides[TC_FRAME_MAX_PLANES],
                           topos_frame_output* out_info,
                           tc_v7_decode_stats* stats)
{
    return tc_v7_frame_decode_scaled(packet, packet_size, max_band, 0u, 0u,
                                     planes_out, strides, out_info, stats);
}
