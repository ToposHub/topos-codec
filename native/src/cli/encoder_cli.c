/* topos_encoder_cli —— 阶段 5 交付物：raw planar 帧 → Topos MOV。
 *
 * raw 布局（每帧连续；帧序列直接拼接，原生字节序 uint16）：
 *   Y: w×h | U: ceil(w/2)×h | V: ceil(w/2)×h [| A: w×h (--alpha 1/2)]
 *
 * 用法示例：
 *   topos_encoder_cli --width 1920 --height 1080 --fps 24 --qp 24 --qm 1 \
 *       --input in.raw --output out.mov [--alpha 1] [--faststart] [--frames N]
 *   [--sar 5:4] [--target-mb N Mbps  帧级码率搜索（tc_frame_encode_sized）]
 *
 * 退出码：0 成功；2 参数错；1 编码/IO 错（stderr 带 tc_last_error 详情）。
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
        "usage: topos_encoder_cli --width W --height H [--fps 24] [--qp 24] [--qm 1]\n"
        "  [--alpha 0|1|2] [--alpha-depth 16|12|10|8] [--sar N:D] [--slice-rows N]\n"
        "  [--target-mb MBPS] [--frames N] [--faststart]\n"
        "  [--entropy v1|vlc|rans2|v8]  (intra/intra-range/rans 已退役, 见 ADR-C0xx)\n"
        "  [--seg-blocks 8|16|32] [--tile-rows 16|32|64]  (仅 --entropy v8)\n"
        "  --input file.raw --output file.mov\n");
}

static int parse_u32(const char* s, uint32_t* out)
{
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v > 0x7FFFFFFFL) { return -1; }
    *out = (uint32_t)v;
    return 0;
}

int main(int argc, char** argv)
{
    const char* in_path = NULL;
    const char* out_path = NULL;
    uint32_t width = 0, height = 0, fps = 24u, qp = 24u, qm = 1u;
    uint32_t alpha = 0u, alpha_depth = 16u, slice_rows = 0u, frames = 0u; /* P1-09：0=ABI 默认 16 */
    uint32_t profile_id = 3u, pf_id = 0u, bd_id = 10u; /* M4-R6：默认 legacy 组合 */
    uint32_t sar_num = 1u, sar_den = 1u, target_mb = 0u;
    uint32_t seg_blocks = 0u, tile_rows = 0u; /* V8 粒度：0 = 编码器默认档 */
    int faststart = 0;
    uint32_t coding_selection = 0u;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "--input") == 0 && i + 1 < argc) { in_path = argv[++i]; }
        else if (strcmp(a, "--output") == 0 && i + 1 < argc) { out_path = argv[++i]; }
        else if (strcmp(a, "--width") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &width) != 0) { usage(); return 2; }
        } else if (strcmp(a, "--height") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &height) != 0) { usage(); return 2; }
        } else if (strcmp(a, "--fps") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &fps) != 0 || fps == 0u) { usage(); return 2; }
        } else if (strcmp(a, "--qp") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &qp) != 0) { usage(); return 2; }
        } else if (strcmp(a, "--qm") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &qm) != 0) { usage(); return 2; }
        } else if (strcmp(a, "--profile") == 0 && i + 1 < argc) {
            /* M4-R6：profile 7（TRAW）+ pf 3 + bd 12/16 视频域解锁——
             * 输入 = 4 相位平面 R/Gr/Gb/B 依次拼接 uint16 LE（与图片
             * CLI 同输入契约） */
            if (parse_u32(argv[++i], &profile_id) != 0) { usage(); return 2; }
        } else if (strcmp(a, "--pf") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &pf_id) != 0) { usage(); return 2; }
        } else if (strcmp(a, "--bd") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &bd_id) != 0) { usage(); return 2; }
        } else if (strcmp(a, "--alpha") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &alpha) != 0 || alpha > 2u) { usage(); return 2; }
        } else if (strcmp(a, "--alpha-depth") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &alpha_depth) != 0) { usage(); return 2; }
        } else if (strcmp(a, "--slice-rows") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &slice_rows) != 0) { usage(); return 2; }
        } else if (strcmp(a, "--frames") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &frames) != 0) { usage(); return 2; }
        } else if (strcmp(a, "--sar") == 0 && i + 2 < argc) {
            if (parse_u32(argv[++i], &sar_num) != 0 ||
                parse_u32(argv[++i], &sar_den) != 0) { usage(); return 2; }
        } else if (strcmp(a, "--target-mb") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &target_mb) != 0) { usage(); return 2; }
        } else if (strcmp(a, "--entropy") == 0 && i + 1 < argc) {
            const char* mode = argv[++i];
            /* V 代际收纳（2026-09-13）：写面收缩 v1|vlc|rans2|v8；em 2..7
             * （V2-Rice/V3/V4/V5/V6/V7-R）已退役——CLI 层拒绝，退役代际
             * 仅保留解码（UNSUPPORTED_VERSION），em 编号永久封存。 */
            if (strcmp(mode, "v1") == 0) { coding_selection = 0u; }
            else if (strcmp(mode, "vlc") == 0) { coding_selection = 1u; }
            else if (strcmp(mode, "rans2") == 0) { coding_selection = 8u; }
            else if (strcmp(mode, "v8") == 0) { coding_selection = 9u; }
            else if (strcmp(mode, "intra") == 0 || strcmp(mode, "intra-range") == 0 ||
                     strcmp(mode, "rans") == 0) {
                fprintf(stderr, "error: --entropy %s: generation retired "
                                "(V consolidation 2026-09-13, see ADR-C0xx); "
                                "use v1|vlc|rans2|v8\n", mode);
                return 2;
            }
            else { usage(); return 2; }
        } else if (strcmp(a, "--seg-blocks") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &seg_blocks) != 0) { usage(); return 2; }
        } else if (strcmp(a, "--tile-rows") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &tile_rows) != 0) { usage(); return 2; }
        } else if (strcmp(a, "--faststart") == 0) { faststart = 1; }
        else { usage(); return 2; }
    }
    if (in_path == NULL || out_path == NULL || width == 0 || height == 0 ||
        width > 16384u || height > 16384u) {
        usage();
        return 2;
    }
    if (alpha != 0u) {
        /* alpha_mode=1 需 16-bit；mode2 需 8/10/12（spec §8.6） */
        if (alpha == 1u && alpha_depth != 16u) { usage(); return 2; }
        if (alpha == 2u && alpha_depth != 8u && alpha_depth != 10u && alpha_depth != 12u) {
            usage();
            return 2;
        }
    }
    if (pf_id == 3u) {
        /* M4-R6：TRAW 域约束（native 交叉规则前置；qm0 冻结 + no-alpha） */
        if (profile_id != 7u) { fprintf(stderr, "error: --pf 3 requires --profile 7\n"); return 2; }
        if (bd_id != 12u && bd_id != 16u) { fprintf(stderr, "error: --pf 3 requires --bd 12|16\n"); return 2; }
        if (alpha != 0u) { fprintf(stderr, "error: TRAW 与 alpha 互斥\n"); return 2; }
        if (qm != 0u) { fprintf(stderr, "error: TRAW qm0 冻结（--qm 0）\n"); return 2; }
    }

    /* raw 帧几何（TRAW：4 相位平面 R/Gr/Gb/B 各 ceil(W/2)×ceil(H/2)，
     * 拼接序 = 图片 CLI 同契约；帧字节数 = 4 × pw*ph × 2） */
    uint32_t cw = (width + 1u) / 2u;
    size_t y_elems = (size_t)width * height;
    size_t uv_elems = (size_t)cw * height;
    size_t frame_elems = y_elems + 2u * uv_elems + (alpha ? y_elems : 0u);
    if (pf_id == 3u) {
        /* TRAW（M4-R6）：4 相位平面各 ceil(W/2)×ceil(H/2)，无 alpha */
        const uint32_t phh = (height + 1u) / 2u;
        frame_elems = 4u * (size_t)phh * ((width + 1u) / 2u);
    }
    size_t frame_bytes = frame_elems * 2u;

    FILE* in = fopen(in_path, "rb");
    if (in == NULL) { fprintf(stderr, "cannot open input\n"); return 1; }
    long long in_size;
#if defined(_MSC_VER)
    if (_fseeki64(in, 0, SEEK_END) != 0) { fclose(in); return 1; }
    in_size = _ftelli64(in);
#else
    if (fseeko(in, 0, SEEK_END) != 0) { fclose(in); return 1; }
    in_size = (long long)ftello(in);
#endif
    rewind(in);
    uint64_t total_frames = (uint64_t)in_size / frame_bytes;
    if (total_frames == 0ull) {
        fprintf(stderr, "input smaller than one frame (%lld < %zu)\n",
                (long long)in_size, frame_bytes);
        fclose(in);
        return 1;
    }
    if (frames != 0u) {
        if ((uint64_t)frames > total_frames) {
            fprintf(stderr, "--frames %u exceeds input frames %llu\n",
                    frames, (unsigned long long)total_frames);
            fclose(in);
            return 1;
        }
        total_frames = frames;
    }

    topos_movie_config mc;
    memset(&mc, 0, sizeof(mc));
    mc.struct_size = (uint32_t)sizeof(mc);
    mc.abi_version = TOPOS_CODEC_ABI_VERSION;
    mc.visible_width = (uint16_t)width;
    mc.visible_height = (uint16_t)height;
    mc.profile = (uint8_t)profile_id;
    mc.pixel_format = (uint8_t)pf_id;
    mc.bit_depth = (uint8_t)bd_id;
    mc.qmatrix_id = (uint8_t)qm;
    mc.qp_base = (uint8_t)qp;
    mc.alpha_mode = (uint8_t)alpha;
    mc.alpha_bit_depth = alpha ? (uint8_t)alpha_depth : 0u;
    mc.color_range = 1u;
    mc.color_primaries = 1u;
    mc.color_transfer = (pf_id == 3u) ? (bd_id == 16u ? 8u : 20u) : 1u;
    mc.color_matrix = (pf_id == 2u || pf_id == 3u) ? 0u : 1u;
    mc.sar_num = (uint16_t)sar_num;
    mc.sar_den = (uint16_t)sar_den;
    mc.timescale = 24000u;

    topos_frame_config fc;
    memset(&fc, 0, sizeof(fc));
    fc.struct_size = (uint32_t)sizeof(fc);
    fc.abi_version = TOPOS_CODEC_ABI_VERSION;
    fc.visible_width = (uint16_t)width;
    fc.visible_height = (uint16_t)height;
    fc.profile = (uint8_t)profile_id;
    fc.pixel_format = (uint8_t)pf_id;
    fc.bit_depth = (uint8_t)bd_id;
    fc.qmatrix_id = (uint8_t)qm;
    fc.qp_base = (uint8_t)qp;
    fc.reserved[0] = coding_selection;
    fc.alpha_mode = (uint8_t)alpha;
    fc.alpha_bit_depth = mc.alpha_bit_depth;
    fc.color_range = 1u;
    fc.color_primaries = 1u;
    fc.color_transfer = (pf_id == 3u) ? (bd_id == 16u ? 8u : 20u) : 1u;
    fc.color_matrix = (pf_id == 2u || pf_id == 3u) ? 0u : 1u;
    fc.sar_num = (uint16_t)sar_num;
    fc.sar_den = (uint16_t)sar_den;
    /* P1-09：0 = ABI 默认（TC_FRAME_DEFAULT_SLICE_ROWS=16），与 Python/API 同义 */
    fc.slice_rows = (uint8_t)(slice_rows ? slice_rows : 16u);
    if (seg_blocks != 0u || tile_rows != 0u) {
        /* V8 粒度信令（reserved[3]=sb_log2 / [4]=tile_log2）；非 V8 熵下
         * 显式给出是参数错误（reserved 在 V7 路径必须为 0）。 */
        if (coding_selection != 9u) { usage(); return 2; }
    }
    if (seg_blocks != 0u) {
        switch (seg_blocks) {
        case 8u: fc.reserved[3] = 3u; break;
        case 16u: fc.reserved[3] = 4u; break;
        case 32u: fc.reserved[3] = 5u; break;
        default: fprintf(stderr, "--seg-blocks must be 8|16|32\n"); return 2;
        }
    }
    if (tile_rows != 0u) {
        switch (tile_rows) {
        case 16u: fc.reserved[4] = 4u; break;
        case 32u: fc.reserved[4] = 5u; break;
        case 64u: fc.reserved[4] = 6u; break;
        default: fprintf(stderr, "--tile-rows must be 16|32|64\n"); return 2;
        }
    }

    FILE* out = fopen(out_path, "wb");
    if (out == NULL) {
        fprintf(stderr, "cannot open output\n");
        fclose(in);
        return 1;
    }
    topos_io sink;
    tc_io_sink_file(out, &sink);
    topos_mux* mux = NULL;
    int32_t rc = tc_mux_create(&mc, &sink, &mux);
    if (rc != TC_OK) {
        fprintf(stderr, "tc_mux_create: %s\n", tc_status_message(rc));
        fclose(in);
        fclose(out);
        return 1;
    }

    /* M4-R6：TRAW = 4 相位平面 R/Gr/Gb/B 各 ceil(W/2)×ceil(H/2)；
     * 输入帧 = 4 平面依次拼接，帧字节数与平面指针/stride 全部改走
     * CFA 几何（pf!=3 保持 YUV/GBR 既有布局）。 */
    const int is_traw = (pf_id == 3u);
    const uint32_t ph_h = (height + 1u) / 2u;
    const size_t ph_elems = (size_t)ph_h * ((width + 1u) / 2u);
    uint16_t* planes[4] = { NULL, NULL, NULL, NULL };
    if (is_traw) {
        for (int p = 0; p < 4; ++p) { planes[p] = (uint16_t*)tc_alloc(ph_elems * 2u); }
    } else {
        planes[0] = (uint16_t*)tc_alloc(y_elems * 2u);
        planes[1] = (uint16_t*)tc_alloc(uv_elems * 2u);
        planes[2] = (uint16_t*)tc_alloc(uv_elems * 2u);
        if (alpha) { planes[3] = (uint16_t*)tc_alloc(y_elems * 2u); }
    }
    topos_frame_input input;
    memset(&input, 0, sizeof(input));
    input.struct_size = (uint32_t)sizeof(input);
    input.abi_version = TOPOS_CODEC_ABI_VERSION;
    for (int p = 0; p < 4; ++p) { input.planes[p] = planes[p]; }
    /* strides = uint16 元素行距（ABI 契约，非字节） */
    input.strides[0] = is_traw ? (width + 1u) / 2u : width;
    input.strides[1] = is_traw ? (width + 1u) / 2u : cw;
    input.strides[2] = is_traw ? (width + 1u) / 2u : cw;
    input.strides[3] = is_traw ? (width + 1u) / 2u : width;

    size_t bound = tc_frame_packet_bound(&fc);
    uint8_t* pkt = (uint8_t*)tc_alloc(bound);
    if (pkt == NULL || planes[0] == NULL || planes[1] == NULL || planes[2] == NULL ||
        (alpha && planes[3] == NULL)) {
        fprintf(stderr, "out of memory\n");
        rc = TC_ERR_OUT_OF_MEMORY;
    }

    uint64_t pts = 0u;
    uint64_t payload_bytes = 0u;
    uint64_t dur_tick = 24000ull / fps; /* 帧时长（timescale 24000） */
    for (uint64_t i = 0; i < total_frames && rc == TC_OK; ++i) {
        if (is_traw) {
            /* 4 相位平面依次读取（R/Gr/Gb/B） */
            int bad = 0;
            for (int p = 0; p < 4 && !bad; ++p) {
                bad = (fread(planes[p], 2u, ph_elems, in) != ph_elems);
            }
            if (bad) {
                fprintf(stderr, "input read failed at frame %llu\n",
                        (unsigned long long)i);
                rc = TC_ERR_IO;
                break;
            }
        } else if (fread(planes[0], 2u, y_elems, in) != y_elems ||
            fread(planes[1], 2u, uv_elems, in) != uv_elems ||
            fread(planes[2], 2u, uv_elems, in) != uv_elems ||
            (alpha && fread(planes[3], 2u, y_elems, in) != y_elems)) {
            fprintf(stderr, "input read failed at frame %llu\n", (unsigned long long)i);
            rc = TC_ERR_IO;
            break;
        }
        topos_frame_stats st;
        memset(&st, 0, sizeof(st));
        if (target_mb != 0u) {
            uint8_t qp_used = 0u;
            /* --target-mb 语义 = Mbps：bits/s ÷8 ÷fps → B/frame
             * （原式 ×fps 为量纲错误——目标随帧率平方漂移，24/25fps 下
             * 差 576/625 倍，sized 搜索实际永远跑 qp 下界） */
            uint32_t target = target_mb * 1000u * 1000u / 8u / fps;
            rc = tc_frame_encode_sized(&fc, &input, target, 0u, 95u, &qp_used,
                                       pkt, bound, &st);
            if (rc == TC_OK) {
                fprintf(stderr, "frame %llu: qp_used=%u size=%u\n",
                        (unsigned long long)i, qp_used, st.packet_size);
            }
        } else {
            rc = tc_frame_encode(&fc, &input, pkt, bound, &st);
        }
        if (rc == TC_OK) {
            rc = tc_mux_add_packet(mux, pkt, st.packet_size, pts, (uint32_t)dur_tick);
            pts += dur_tick;
            payload_bytes += st.packet_size;
        }
    }
    if (rc == TC_OK) { rc = tc_mux_finish(mux); }
    if (rc != TC_OK) {
        fprintf(stderr, "encode/mux: %s (%s)\n", tc_status_message(rc), tc_last_error());
    }
    tc_mux_free(mux);
    fclose(in);
    if (fclose(out) != 0 && rc == TC_OK) { rc = TC_ERR_IO; }

    if (rc == TC_OK && faststart) {
        /* FastStart 后处理：读刚写出的文件 → ftyp+moov+mdat 布局写临时文件 → 替换 */
        FILE* src = fopen(out_path, "rb");
        if (src == NULL) { rc = TC_ERR_IO; }
        size_t tmp_len = strlen(out_path) + 8u;
        char* tmp_path = (char*)tc_alloc(tmp_len);
        if (tmp_path == NULL) { rc = TC_ERR_OUT_OF_MEMORY; }
        if (rc == TC_OK) {
            snprintf(tmp_path, tmp_len, "%s.fs.tmp", out_path);
            FILE* dst = fopen(tmp_path, "wb");
            if (dst == NULL) { rc = TC_ERR_IO; }
            if (rc == TC_OK) {
                topos_io sio, dio;
                if (tc_io_src_file(src, &sio) != 0) { rc = TC_ERR_IO; }
                tc_io_sink_file(dst, &dio);
                if (rc == TC_OK) { rc = tc_movie_faststart(&sio, &dio); }
                if (fclose(dst) != 0 && rc == TC_OK) { rc = TC_ERR_IO; }
            }
            fclose(src);
            if (rc == TC_OK) {
                if (rename(tmp_path, out_path) != 0) {
                    fprintf(stderr, "faststart rename failed: %s\n", tmp_path);
                    remove(tmp_path);
                    rc = TC_ERR_IO;
                }
            } else {
                remove(tmp_path);
            }
            tc_free(tmp_path);
        }
    }

    if (rc == TC_OK) {
        long long file_size = 0;
        FILE* chk = fopen(out_path, "rb");
        if (chk != NULL) {
#if defined(_MSC_VER)
            _fseeki64(chk, 0, SEEK_END);
            file_size = (long long)_ftelli64(chk);
#else
            fseeko(chk, 0, SEEK_END);
            file_size = (long long)ftello(chk);
#endif
            fclose(chk);
        }
        printf("frames=%llu payload=%llu B file=%lld B fps=%u layout=%s\n",
               (unsigned long long)total_frames, (unsigned long long)payload_bytes,
               file_size, fps, faststart ? "faststart" : "standard");
    }

    tc_free(pkt);
    tc_free(planes[0]);
    tc_free(planes[1]);
    tc_free(planes[2]);
    tc_free(planes[3]);
    return rc == TC_OK ? 0 : 1;
}
