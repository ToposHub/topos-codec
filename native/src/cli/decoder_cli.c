/* topos_decoder_cli —— Topos MOV → raw planar 帧 + 摘要。
 *
 * raw 布局：按帧顺序写出 plane 0..N 的 tight uint16 little-endian 数据。
 * 解码统一经 tc_frame_decode_request：--scale full 保持旧整帧语义，固定比例
 * 使用低频 reduced 路径，auto2k 让 native 选择不超过 2K 的 legacy fallback。
 *
 * 用法：
 *   topos_decoder_cli --input file.mov [--output out.raw] [--frames N]
 *   [--start N] [--scale full|1/2|1/3|1/4|1/8|auto2k] [--report]
 * 退出码：0 成功（含 conceal 警告）；1 IO/结构错；2 参数错。
 */
#include "../common/alloc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_file.h"

static void usage(void)
{
    fprintf(stderr,
        "usage: topos_decoder_cli --input file.mov [--output out.raw]\n"
        "  [--start N] [--frames N]\n"
        "  [--scale full|1/2|1/3|1/4|1/8|auto2k] [--report]\n");
}

static int parse_u32(const char* s, uint32_t* out)
{
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v > 0x7FFFFFFFL) { return -1; }
    *out = (uint32_t)v;
    return 0;
}

static int parse_scale(const char* text, uint32_t* mode, uint32_t* scale)
{
    if (strcmp(text, "full") == 0) {
        *mode = TC_DECODE_MODE_FULL;
        *scale = TC_DECODE_SCALE_FULL;
        return 0;
    }
    if (strcmp(text, "auto2k") == 0) {
        *mode = TC_DECODE_MODE_AUTO_2K;
        *scale = TC_DECODE_SCALE_FULL;
        return 0;
    }
    *mode = TC_DECODE_MODE_REDUCED;
    if (strcmp(text, "1/2") == 0) { *scale = TC_DECODE_SCALE_HALF; return 0; }
    if (strcmp(text, "1/3") == 0) { *scale = TC_DECODE_SCALE_THIRD; return 0; }
    if (strcmp(text, "1/4") == 0) { *scale = TC_DECODE_SCALE_QUARTER; return 0; }
    if (strcmp(text, "1/8") == 0) { *scale = TC_DECODE_SCALE_EIGHTH; return 0; }
    return -1;
}

static const char* requested_label(const topos_decode_request* request)
{
    if (request->mode == TC_DECODE_MODE_FULL) { return "full"; }
    if (request->mode == TC_DECODE_MODE_AUTO_2K) { return "auto2k"; }
    switch ((tc_decode_scale)request->scale) {
    case TC_DECODE_SCALE_HALF: return "1/2";
    case TC_DECODE_SCALE_THIRD: return "1/3";
    case TC_DECODE_SCALE_QUARTER: return "1/4";
    case TC_DECODE_SCALE_EIGHTH: return "1/8";
    default: return "invalid";
    }
}

static const char* actual_label(const topos_decode_request* request,
                                uint32_t source_w, uint32_t source_h,
                                const topos_frame_output* info)
{
    if (request->mode != TC_DECODE_MODE_AUTO_2K) {
        return request->mode == TC_DECODE_MODE_FULL ? "full" : "reduced";
    }
    if (info->visible_width == source_w && info->visible_height == source_h) {
        return "auto2k/full";
    }
    const tc_decode_scale scales[] = {
        TC_DECODE_SCALE_HALF, TC_DECODE_SCALE_THIRD,
        TC_DECODE_SCALE_QUARTER, TC_DECODE_SCALE_EIGHTH
    };
    const char* labels[] = { "auto2k/reduced-1/2", "auto2k/reduced-1/3",
                             "auto2k/reduced-1/4", "auto2k/reduced-1/8" };
    for (size_t i = 0u; i < sizeof(scales) / sizeof(scales[0]); ++i) {
        uint32_t w = 0u, h = 0u;
        if (tc_decode_scale_dimensions(source_w, source_h, scales[i], &w, &h) == TC_OK &&
            info->visible_width == w && info->visible_height == h) {
            return labels[i];
        }
    }
    return "auto2k/scaled";
}

static int32_t read_packet(topos_movie* movie, uint32_t index,
                           uint8_t** packet, size_t* capacity, size_t* size)
{
    size_t need = 0u;
    int32_t rc = tc_movie_packet(movie, index, NULL, 0u, &need);
    if (rc != TC_ERR_BUFFER_TOO_SMALL) { return rc; }
    if (need > *capacity) {
        uint8_t* next = (uint8_t*)tc_alloc(need);
        if (next == NULL) { return TC_ERR_OUT_OF_MEMORY; }
        tc_free(*packet);
        *packet = next;
        *capacity = need;
    }
    rc = tc_movie_packet(movie, index, *packet, *capacity, NULL);
    if (rc == TC_OK) { *size = need; }
    return rc;
}

static void free_planes(uint16_t* planes[TC_FRAME_MAX_PLANES])
{
    for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
        tc_free(planes[p]);
        planes[p] = NULL;
    }
}

/* V9（topos_v9_micro_gop_plan 批 3）：GOP 感知解码——prev_sync 定位 +
 * context 顺序解（§4.4）。坏参考（REFERENCE_INVALID）跳下一 sync，
 * 禁止展示旧帧冒充新帧。非 full 缩放在 V9 窗口未定义 → 拒绝。 */
static int decode_v9_movie(topos_movie* mv, const topos_movie_info* info,
                           uint32_t start, uint32_t end, const char* out_path,
                           int report, uint32_t* decoded_out)
{
    uint32_t sync_idx = 0u;
    int32_t rc = tc_movie_prev_sync(mv, start, &sync_idx);
    if (rc != TC_OK) {
        fprintf(stderr, "prev_sync: %s (%s)\n", tc_status_message(rc), tc_last_error());
        return 1;
    }
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = info->visible_width;
    cfg.visible_height = info->visible_height;
    cfg.profile = info->profile;
    cfg.pixel_format = info->pixel_format;
    cfg.bit_depth = info->bit_depth;
    cfg.qmatrix_id = info->qmatrix_id;
    cfg.qp_base = info->qp_base;
    cfg.color_range = info->color_range;
    cfg.color_primaries = info->color_primaries;
    cfg.color_transfer = info->color_transfer;
    cfg.color_matrix = info->color_matrix;
    cfg.reserved[0] = 10u;
    topos_gop_context* ctx = NULL;
    rc = tc_gop_context_create(&cfg, &ctx);
    if (rc != TC_OK) {
        fprintf(stderr, "gop context: %s (%s)\n", tc_status_message(rc), tc_last_error());
        return 1;
    }
    if (sync_idx != start) {
        printf("v9: start %u not sync -> decoding from sync %u\n", start, sync_idx);
    }
    FILE* out = NULL;
    if (out_path != NULL) {
        out = fopen(out_path, "wb");
        if (out == NULL) { perror("cannot open output"); tc_gop_context_close(ctx); return 1; }
    }
    uint16_t* planes[TC_FRAME_MAX_PLANES] = { NULL, NULL, NULL, NULL };
    uint32_t decoded = 0u;
    uint32_t i = sync_idx;
    while (i < end) {
        uint8_t* pkt = NULL;
        size_t pkt_cap = 0u, pkt_size = 0u;
        if (read_packet(mv, i, &pkt, &pkt_cap, &pkt_size) != TC_OK) {
            fprintf(stderr, "sample %u: %s (%s)\n", i, tc_status_message(rc), tc_last_error());
            tc_free(pkt);
            break;
        }
        topos_gop_frame_info gi;
        memset(&gi, 0, sizeof(gi));
        const int want_output = (i >= start);
        if (want_output && planes[0] == NULL) {
            /* 首个输出帧前按查询几何分配 */
            topos_frame_output probe;
            memset(&probe, 0, sizeof(probe));
            probe.struct_size = (uint32_t)sizeof(probe);
            probe.abi_version = TOPOS_CODEC_ABI_VERSION;
            if (tc_frame_decode(pkt, pkt_size, NULL, NULL, &probe) != TC_OK) {
                fprintf(stderr, "sample %u query: %s\n", i, tc_last_error());
                tc_free(pkt);
                break;
            }
            for (uint32_t p = 0u; p < probe.plane_count; ++p) {
                uint32_t pw = 0u, ph = 0u;
                tc_frame_plane_geometry(&probe, p, &pw, &ph);
                planes[p] = (uint16_t*)tc_alloc((size_t)pw * ph * sizeof(uint16_t));
            }
        }
        topos_plane_view views[TC_FRAME_MAX_PLANES];
        memset(views, 0, sizeof(views));
        if (want_output && planes[0] != NULL) {
            for (int p = 0; p < TC_FRAME_MAX_PLANES; ++p) {
                views[p].struct_size = (uint32_t)sizeof(views[p]);
                views[p].abi_version = TOPOS_CODEC_ABI_VERSION;
                views[p].pixels = planes[p];
            }
        }
        rc = tc_gop_context_feed(ctx, pkt, pkt_size, want_output ? views : NULL, &gi);
        tc_free(pkt);
        if (rc == TC_ERR_REFERENCE_INVALID || rc == TC_ERR_MALFORMED ||
            rc == TC_ERR_TRUNCATED || rc == TC_ERR_CHECKSUM_MISMATCH) {
            /* §3.8：跳下一 sync；REFERENCE_INVALID 不算硬失败 */
            fprintf(stderr, "sample %u: %s (%s) -> skip to next sync\n",
                    i, tc_status_message(rc), tc_last_error());
            uint32_t k = i + 1u;
            while (k < end) {
                uint8_t sync = 0u;
                tc_movie_packet_sync(mv, k, &sync);
                if (sync != 0u) { break; }
                k++;
            }
            tc_gop_context_abort(ctx, 1);
            i = k;
            rc = TC_OK;
            continue;
        }
        if (rc != TC_OK) {
            fprintf(stderr, "sample %u feed: %s (%s)\n", i, tc_status_message(rc),
                    tc_last_error());
            break;
        }
        if (want_output) {
            decoded++;
            if (out != NULL && planes[0] != NULL) {
                /* V9 P0 = 颜色三平面（no-alpha capability）；422 半宽色度 */
                const uint32_t pc = 3u;
                for (uint32_t p = 0u; p < pc; ++p) {
                    uint32_t pw = info->visible_width, ph = info->visible_height;
                    if (p != 0u && info->pixel_format == 0u) { pw = (pw + 1u) / 2u; }
                    fwrite(planes[p], sizeof(uint16_t), (size_t)pw * ph, out);
                }
            }
            if (report) {
                uint64_t pts = 0u;
                uint32_t dur = 0u;
                tc_movie_packet_pts(mv, i, &pts, &dur);
                printf("frame %u: type=%c gop=%u pts=%llu dur=%u\n", i,
                       gi.frame_type == 0u ? 'I' : 'P', (unsigned)gi.gop_id,
                       (unsigned long long)pts, dur);
            }
        }
        i++;
    }
    printf("decoded=%u range=[%u,%u) v9_gop_path=1\n", decoded, start, end);
    if (out != NULL) { fclose(out); }
    for (int p = 0; p < TC_FRAME_MAX_PLANES; ++p) { tc_free(planes[p]); }
    tc_gop_context_close(ctx);
    *decoded_out = decoded;
    return 0;
}

int main(int argc, char** argv)
{
    const char* in_path = NULL;
    const char* out_path = NULL;
    uint32_t start = 0u, frames = 0u;
    uint32_t mode = TC_DECODE_MODE_FULL;
    uint32_t scale = TC_DECODE_SCALE_FULL;
    int report = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) { in_path = argv[++i]; }
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) { out_path = argv[++i]; }
        else if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &start) != 0) { usage(); return 2; }
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &frames) != 0) { usage(); return 2; }
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            if (parse_scale(argv[++i], &mode, &scale) != 0) { usage(); return 2; }
        } else if (strcmp(argv[i], "--report") == 0) { report = 1; }
        else { usage(); return 2; }
    }
    if (in_path == NULL) { usage(); return 2; }

    FILE* in = fopen(in_path, "rb");
    if (in == NULL) { fprintf(stderr, "cannot open input\n"); return 1; }
    topos_io io;
    if (tc_io_src_file(in, &io) != 0) {
        fprintf(stderr, "cannot size input\n");
        fclose(in);
        return 1;
    }
    topos_movie* mv = NULL;
    int32_t rc = tc_movie_open(&io, &mv);
    if (rc != TC_OK) {
        fprintf(stderr, "tc_movie_open: %s (%s)\n", tc_status_message(rc), tc_last_error());
        fclose(in);
        return 1;
    }
    topos_movie_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    rc = tc_movie_info(mv, &info);
    if (rc != TC_OK) {
        fprintf(stderr, "tc_movie_info: %s\n", tc_status_message(rc));
        tc_movie_close(mv);
        fclose(in);
        return 1;
    }
    printf("movie: %ux%u frames=%u timescale=%u faststart=%u index=%u B "
           "bitstream_major=%u profile=%u bit_depth=%u qm=%u qp=%u alpha=%u/%u premult=%u "
           "colors=(%u,%u,%u,%u) siting=%u sar=%u:%u\n",
           info.visible_width, info.visible_height, info.sample_count,
           info.timescale, info.faststart, info.index_bytes, info.reserved[0],
           info.profile, info.bit_depth, info.qmatrix_id, info.qp_base,
           info.alpha_mode, info.alpha_bit_depth, info.alpha_premultiplied,
           info.color_range, info.color_primaries, info.color_transfer,
           info.color_matrix, info.chroma_siting, info.sar_num, info.sar_den);

    if (start >= info.sample_count) {
        fprintf(stderr, "--start %u beyond frames %u\n", start, info.sample_count);
        tc_movie_close(mv);
        fclose(in);
        return 2;
    }
    uint32_t end = info.sample_count;
    if (frames != 0u) {
        uint64_t requested_end = (uint64_t)start + frames;
        if (requested_end < (uint64_t)end) { end = (uint32_t)requested_end; }
    }

    topos_decode_request request;
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.abi_version = TOPOS_CODEC_ABI_VERSION;
    request.mode = mode;
    request.scale = scale;
    request.memory_type = TC_DECODE_MEMORY_CPU;

    uint8_t* pkt = NULL;
    size_t pkt_cap = 0u, pkt_size = 0u;
    rc = read_packet(mv, start, &pkt, &pkt_cap, &pkt_size);
    if (rc != TC_OK) {
        fprintf(stderr, "sample %u: %s (%s)\n", start,
                tc_status_message(rc), tc_last_error());
        tc_free(pkt);
        tc_movie_close(mv);
        fclose(in);
        return 1;
    }

    /* V9（micro-gop 计划批 3）：GOP 感知路径——prev_sync 定位 + context
     * 顺序解；P 直启自动回退到前一 I（输出仍从 start 起）。 */
    if (pkt_size >= 19u && memcmp(pkt, "TPIC", 4u) == 0 && pkt[6] == 9u) {
        if (request.mode != TC_DECODE_MODE_FULL ||
            request.scale != TC_DECODE_SCALE_FULL) {
            fprintf(stderr, "v9: --scale reduced/auto2k not supported (full only)\n");
            tc_free(pkt);
            tc_movie_close(mv);
            fclose(in);
            return 2;
        }
        uint32_t v9_decoded = 0u;
        const int vrc = decode_v9_movie(mv, &info, start, end, out_path, report,
                                        &v9_decoded);
        tc_free(pkt);
        tc_movie_close(mv);
        fclose(in);
        return vrc;
    }

    /* 先查询 request 解析后的实际目标几何，再按 plane_geometry 分配；这同时
     * 保证 4:2:2、4:4:4、GBR 和 Alpha 都不会按错误的半宽色度分配。 */
    topos_frame_output probe;
    memset(&probe, 0, sizeof(probe));
    probe.struct_size = (uint32_t)sizeof(probe);
    probe.abi_version = TOPOS_CODEC_ABI_VERSION;
    rc = tc_frame_decode_request(pkt, pkt_size, &request, NULL, NULL, &probe);
    if (rc != TC_OK) {
        fprintf(stderr, "sample %u query: %s (%s)\n", start,
                tc_status_message(rc), tc_last_error());
        tc_free(pkt);
        tc_movie_close(mv);
        fclose(in);
        return 1;
    }
    printf("decode: request=%s path=%s output=%ux%u planes=%u\n",
           requested_label(&request), actual_label(&request, info.visible_width,
                                                    info.visible_height, &probe),
           probe.visible_width, probe.visible_height, probe.plane_count);

    uint16_t* planes[TC_FRAME_MAX_PLANES] = { NULL, NULL, NULL, NULL };
    for (uint32_t p = 0u; p < probe.plane_count; ++p) {
        uint32_t pw = 0u, ph = 0u;
        rc = tc_frame_plane_geometry(&probe, p, &pw, &ph);
        if (rc != TC_OK || pw == 0u || ph == 0u ||
            (size_t)pw > SIZE_MAX / (size_t)ph ||
            (size_t)pw * ph > SIZE_MAX / sizeof(uint16_t)) {
            fprintf(stderr, "sample %u plane %u geometry invalid\n", start, p);
            free_planes(planes);
            tc_free(pkt);
            tc_movie_close(mv);
            fclose(in);
            return 1;
        }
        planes[p] = (uint16_t*)tc_alloc((size_t)pw * ph * sizeof(uint16_t));
        if (planes[p] == NULL) {
            fprintf(stderr, "plane allocation failed\n");
            free_planes(planes);
            tc_free(pkt);
            tc_movie_close(mv);
            fclose(in);
            return 1;
        }
    }

    FILE* out = NULL;
    if (out_path != NULL) {
        out = fopen(out_path, "wb");
        if (out == NULL) {
            fprintf(stderr, "cannot open output\n");
            free_planes(planes);
            tc_free(pkt);
            tc_movie_close(mv);
            fclose(in);
            return 1;
        }
    }

    uint32_t concealed_frames = 0u;
    uint32_t decoded = 0u;
    for (uint32_t i = start; i < end && rc == TC_OK; ++i) {
        if (i != start) {
            rc = read_packet(mv, i, &pkt, &pkt_cap, &pkt_size);
            if (rc != TC_OK) {
                fprintf(stderr, "sample %u: %s (%s)\n", i,
                        tc_status_message(rc), tc_last_error());
                break;
            }
        }
        topos_frame_output finfo;
        memset(&finfo, 0, sizeof(finfo));
        finfo.struct_size = (uint32_t)sizeof(finfo);
        finfo.abi_version = TOPOS_CODEC_ABI_VERSION;
        rc = tc_frame_decode_request(pkt, pkt_size, &request, planes, NULL, &finfo);
        if (rc == TC_WARN_CONCEALED) {
            concealed_frames++;
            rc = TC_OK; /* 帧仍交付，继续 */
        }
        if (rc != TC_OK) {
            fprintf(stderr, "sample %u decode: %s (%s)\n", i,
                    tc_status_message(rc), tc_last_error());
            break;
        }
        if (finfo.visible_width != probe.visible_width ||
            finfo.visible_height != probe.visible_height ||
            finfo.plane_count != probe.plane_count) {
            fprintf(stderr, "sample %u geometry differs from first frame\n", i);
            rc = TC_ERR_MALFORMED;
            break;
        }
        decoded++;
        if (out != NULL) {
            for (uint32_t p = 0u; p < finfo.plane_count; ++p) {
                uint32_t pw = 0u, ph = 0u;
                if (tc_frame_plane_geometry(&finfo, p, &pw, &ph) != TC_OK ||
                    fwrite(planes[p], sizeof(uint16_t), (size_t)pw * ph, out) !=
                        (size_t)pw * ph) {
                    fprintf(stderr, "output write failed\n");
                    rc = TC_ERR_IO;
                    break;
                }
            }
        }
        if (report) {
            uint64_t pts = 0;
            uint32_t dur = 0;
            tc_movie_packet_pts(mv, i, &pts, &dur);
            printf("frame %u: %zu B pts=%llu dur=%u planes=%u output=%ux%u concealed=%u\n",
                   i, pkt_size, (unsigned long long)pts, dur, finfo.plane_count,
                   finfo.visible_width, finfo.visible_height, finfo.concealed_slices);
        }
    }

    printf("decoded=%u concealed_frames=%u range=[%u,%u)\n",
           decoded, concealed_frames, start, end);
    if (out != NULL) { fclose(out); }
    free_planes(planes);
    tc_free(pkt);
    tc_movie_close(mv);
    fclose(in);
    return rc == TC_OK ? 0 : 1;
}
