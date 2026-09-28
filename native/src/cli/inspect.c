/* topos_inspect —— elementary frame packet 诊断工具（阶段 3 交付物）。
 *
 * 用法：topos_inspect [--structure] <file.tpkt>
 *       （<file.tpkt> = 裸 TPIC elementary frame packet；`.toos` 扩展名已保留给
 *        Topos Image 静态图容器（TPIM magic），见 docs/image/topos_image_file_spec_v0.md。
 *        ADR-I001 D10：裸 packet 一律使用 .tpkt 测试后缀或 .bin corpus。）
 * 退出码：0 = 可完整解析（CRC 与符号层均通过）；1 = 解析失败（stderr 给出码与详情）；
 *         2 = 用法错误。切片符号解码默认执行（--structure 可跳过）。
 */
#include "../common/alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bitstream/frame_header.h"
#include "../bitstream/packet.h"
#include "../bitstream/slice_codec.h"
#include "topos_codec.h"

#define INSPECT_MAX_BYTES (TC_MAX_PACKET_SIZE + 1u)

static const char* plane_name(const topos_frame_header* fh, uint8_t p)
{
    if (fh != NULL && fh->pixel_format == 3u) {
        /* TRAW CFA：4 相位平面（无 alpha） */
        switch (p) {
        case 0: return "R";
        case 1: return "Gr";
        case 2: return "Gb";
        case 3: return "B";
        default: return "?";
        }
    }
    switch (p) {
    case 0: return "Y";
    case 1: return "U";
    case 2: return "V";
    case 3: return "A";
    default: return "?";
    }
}

/* V8 包（批 1）：结构报告（段目录/瓦片表/瓦片 CRC；符号解码随批 3）。 */
static int inspect_v8(const uint8_t* data, size_t n)
{
    topos_v8_packet_view view;
    int32_t rc = tc_packet_scan_v8(data, n, &view);
    if (rc != TC_OK) {
        fprintf(stderr, "error: %d (%s) — %s\n", rc, tc_status_message(rc), tc_last_error());
        return 1;
    }
    const topos_frame_header* fh = &view.fh;
    printf("Topos V8 frame packet: %zu bytes\n", n);
    printf("version %u.%u  profile %u (%s)  pixfmt %u (%s)  depth %u\n",
           fh->version_major, fh->version_minor, fh->profile,
           tc_profile_name(fh->profile), fh->pixel_format,
           tc_pixel_format_name(fh->pixel_format), fh->bit_depth);
    printf("v8: segment_blocks %u  tile_rows %u  tiles %u  segments %u/%u/%u\n",
           view.segment_blocks, view.tile_rows, view.tile_count,
           view.segs_per_plane[0], view.segs_per_plane[1], view.segs_per_plane[2]);
    printf("frame: visible %ux%u  coded %ux%u  qp_base %u  packet_size %u\n",
           fh->visible_width, fh->visible_height, fh->coded_width, fh->coded_height,
           fh->qp_base, fh->frame_packet_size);
    for (uint32_t p = 0u; p < fh->plane_count; ++p) {
        printf("plane %s: dir@%u (%u×%uB) tables@%u (%u×%uB) stream@%u crc@%u\n",
               plane_name(fh, (uint8_t)p), view.plane_dir_off[p], view.segs_per_plane[p],
               TC_V8_DIR_ENTRY_BYTES, view.plane_table_off[p], view.tiles_per_plane[p],
               TC_V8_TABLE_BYTES, view.plane_stream_off[p], view.plane_crc_off[p]);
    }
    uint16_t crc_bad = 0u;
    for (uint32_t t = 0u; t < view.tile_count; ++t) {
        const topos_v8_tile_info* ti = &view.tiles[t];
        printf("tile %3u plane %s id %u segs [%4u..%4u) table@%u stream %6uB crc %s\n",
               t, plane_name(fh, (uint8_t)ti->plane), ti->tile_id,
               ti->seg_first, ti->seg_first + ti->seg_count,
               ti->table_off, ti->stream_bytes,
               ti->crc_ok ? "OK" : "BAD");
        if (!ti->crc_ok) { crc_bad++; }
    }
    printf("summary: tiles %u  crc_bad %u  symbols: n/a (v8 decode batch 3)\n",
           (unsigned)view.tile_count, (unsigned)crc_bad);
    return crc_bad != 0u ? 1 : 0;
}

int main(int argc, char** argv)
{
    int decode_symbols = 1;
    const char* path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--structure") == 0) { decode_symbols = 0; }
        else if (path == NULL) { path = argv[i]; }
        else { path = NULL; break; }
    }
    if (path == NULL) {
        fprintf(stderr, "usage: topos_inspect [--structure] <file.tpkt>\n");
        return 2;
    }

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "error: cannot open %s\n", path);
        return 1;
    }
    uint8_t* data = (uint8_t*)tc_alloc(INSPECT_MAX_BYTES);
    if (data == NULL) { fclose(f); fprintf(stderr, "error: OOM\n"); return 1; }
    size_t n = fread(data, 1, INSPECT_MAX_BYTES, f);
    fclose(f);
    if (n == INSPECT_MAX_BYTES) {
        fprintf(stderr, "error: file exceeds %u bytes (spec §10)\n", (unsigned)TC_MAX_PACKET_SIZE);
        tc_free(data);
        return 1;
    }

    if (tc_packet_is_v8(data, n)) {
        int v8_rc = inspect_v8(data, n);
        tc_free(data);
        return v8_rc;
    }

    topos_packet_view view;
    int32_t rc = tc_packet_scan(data, n, &view);
    if (rc != TC_OK) {
        fprintf(stderr, "error: %d (%s) — %s\n", rc, tc_status_message(rc), tc_last_error());
        tc_free(data);
        return 1;
    }

    const topos_frame_header* fh = &view.fh;
    printf("Topos frame packet: %zu bytes\n", n);
    printf("version %u.%u  profile %u (%s)  pixfmt %u (%s)  depth %u\n",
           fh->version_major, fh->version_minor, fh->profile,
           tc_profile_name(fh->profile), fh->pixel_format,
           tc_pixel_format_name(fh->pixel_format), fh->bit_depth);
    printf("alpha: %s (bit_depth %u)  flags 0x%04x\n",
           tc_alpha_mode_name(fh->alpha_mode), fh->alpha_bit_depth, fh->flags);
    printf("frame: visible %ux%u  coded %ux%u  type %u(I)  gop_id %u ref_distance %u\n",
           fh->visible_width, fh->visible_height, fh->coded_width, fh->coded_height,
           fh->frame_type, fh->gop_id, fh->ref_distance);
    printf("color: range %s primaries %s transfer %s matrix %s siting %s  sar %u:%u\n",
           tc_color_range_name(fh->color_range), tc_color_primaries_name(fh->color_primaries),
           tc_color_transfer_name(fh->color_transfer), tc_color_matrix_name(fh->color_matrix),
           tc_chroma_siting_name(fh->chroma_siting), fh->sar_num, fh->sar_den);
    printf("planes %u  qmatrix %u  qp_base %u  slices %u  packet_size %u\n",
           fh->plane_count, fh->qmatrix_id, fh->qp_base, fh->slice_count,
           fh->frame_packet_size);
    for (uint32_t p = 0u; p < fh->plane_count; ++p) {
        printf("plane %s: coded %ux%u blocks %ux%u visible %ux%u\n", plane_name(fh, (uint8_t)p),
               fh->plane_coded_w[p], fh->plane_coded_h[p], fh->plane_block_cols[p],
               fh->plane_block_rows[p], fh->plane_visible_w[p], fh->plane_visible_h[p]);
    }

    uint16_t crc_bad = 0;
    uint16_t sym_bad = 0;
    for (uint16_t i = 0; i < view.slice_count; ++i) {
        const topos_slice_header* sh = &view.slices[i];
        int32_t qp = tc_slice_effective_qp(fh->qp_base, sh->qp_delta_biased);
        printf("slice %3u plane %s y0 %4u h %3u qp %2d k %u/%u/%u payload %6u crc %s",
               (unsigned)i, plane_name(fh, sh->plane), sh->block_y0, sh->block_h, qp,
               sh->k1, sh->k2, sh->k3, sh->slice_payload_size,
               view.slice_crc_ok[i] ? "OK" : "BAD");
        if (!view.slice_crc_ok[i]) {
            crc_bad++;
            printf("\n");
            continue;
        }
        if (decode_symbols) {
            int32_t src;
            if (sh->plane == 3u) {
                src = tc_alpha_slice_decode(fh, sh, view.payloads[i],
                                            (size_t)sh->slice_payload_size, NULL, 0u, NULL);
            } else {
                src = tc_color_slice_decode(fh, sh, view.payloads[i],
                                            (size_t)sh->slice_payload_size, NULL, NULL);
            }
            if (src != TC_OK) {
                sym_bad++;
                printf("  symbols: %d (%s)", src, tc_status_message(src));
            } else {
                printf("  symbols: OK");
            }
        }
        printf("\n");
    }
    printf("summary: slices %u  crc_bad %u  symbols_bad %u\n",
           (unsigned)view.slice_count, (unsigned)crc_bad, (unsigned)sym_bad);

    tc_free(data);
    return (crc_bad != 0u || sym_bad != 0u) ? 1 : 0;
}
