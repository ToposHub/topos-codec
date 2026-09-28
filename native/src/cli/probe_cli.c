/* topos_probe_cli —— 阶段 5 交付物：Topos MOV → JSON 探测报告。
 *
 * 输出：布局（faststart）、tpcC 配置、采样表统计（stts runs/co64/
 * index_bytes）、帧列表（offset/size/pts/dur/sync，--samples 控制上限）、
 * --verify 时全帧 packet_scan 级校验（经 tc_frame_decode：magic、帧头
 * CRC 与结构自检，非仅比对魔数）。
 *
 * 用法：topos_probe_cli file.mov [--json] [--samples 16] [--verify]
 * 退出码：0 = 合法 Topos MOV；1 = 结构错；2 参数错。
 */
#include "../common/alloc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_file.h"

static void usage(void)
{
    fprintf(stderr, "usage: topos_probe_cli file.mov [--json] [--samples N] [--verify]\n");
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
    const char* path = NULL;
    uint32_t samples = 16u;
    int json = 0, verify = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) { json = 1; }
        else if (strcmp(argv[i], "--verify") == 0) { verify = 1; }
        else if (strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &samples) != 0) { usage(); return 2; }
        } else if (argv[i][0] != '-' && path == NULL) { path = argv[i]; }
        else { usage(); return 2; }
    }
    if (path == NULL) { usage(); return 2; }

    FILE* f = fopen(path, "rb");
    if (f == NULL) { fprintf(stderr, "cannot open\n"); return 1; }
    topos_io io;
    if (tc_io_src_file(f, &io) != 0) { fclose(f); return 1; }
    topos_movie* mv = NULL;
    int32_t rc = tc_movie_open(&io, &mv);
    if (rc != TC_OK) {
        fprintf(stderr, "open: %s (%s)\n", tc_status_message(rc), tc_last_error());
        fclose(f);
        return 1;
    }
    topos_movie_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    rc = tc_movie_info(mv, &info);
    if (rc != TC_OK) {
        fprintf(stderr, "info: %s\n", tc_status_message(rc));
        tc_movie_close(mv);
        fclose(f);
        return 1;
    }

    uint64_t duration_s_num = 0;
    {
        /* 时长 = 末帧 pts+dur（秒换算留给调用方） */
        if (info.sample_count != 0u) {
            uint64_t pts = 0;
            uint32_t dur = 0;
            tc_movie_packet_pts(mv, info.sample_count - 1u, &pts, &dur);
            duration_s_num = pts + dur;
        }
    }

    if (!json) {
        printf("file: %s (%llu B)\n", path, (unsigned long long)io.length);
        printf("layout: %s\n", info.faststart ? "faststart (ftyp+moov+mdat)" : "standard (ftyp+mdat+moov)");
        topos_scalable_status scalable = TC_SCALABLE_STATUS_EXPERIMENTAL;
        if (tc_query_scalable_status(&scalable) == TC_OK) {
            /* P0-04：诊断输出必须区分 reference 与已发布能力 */
            printf("v7b_scalable: status=%s%s\n",
                   tc_scalable_status_name(scalable),
                   scalable == TC_SCALABLE_STATUS_DEFAULT
                       ? " (default writer)"
                       : scalable == TC_SCALABLE_STATUS_ELIGIBLE
                             ? " (gated rollout ready)"
                             : " (NOT a released capability)");
        }
        printf("video: %ux%u bitstream_major=%u profile=%u pixel_format=%u bit_depth=%u\n",
               info.visible_width, info.visible_height, info.reserved[0],
               info.profile, info.pixel_format, info.bit_depth);
        printf("qp: base=%u deltas=(%d,%d) qmatrix=%u\n",
               info.qp_base, (int)info.qp_delta_luma, (int)info.qp_delta_chroma,
               info.qmatrix_id);
        printf("alpha: mode=%u depth=%u premultiplied=%u\n",
               info.alpha_mode, info.alpha_bit_depth, info.alpha_premultiplied);
        printf("color: range=%u primaries=%u transfer=%u matrix=%u siting=%u sar=%u:%u\n",
               info.color_range, info.color_primaries, info.color_transfer,
               info.color_matrix, info.chroma_siting, info.sar_num, info.sar_den);
        printf("samples: %u timescale=%u duration_ticks=%llu index_bytes=%u (%.2f B/frame)\n",
               info.sample_count, info.timescale, (unsigned long long)duration_s_num,
               info.index_bytes,
               info.sample_count ? (double)info.index_bytes / (double)info.sample_count : 0.0);
    } else {
        topos_scalable_status scalable = TC_SCALABLE_STATUS_EXPERIMENTAL;
        (void)tc_query_scalable_status(&scalable);
        printf("{\n");
        printf("  \"format\": \"topos-mov\",\n");
        printf("  \"layout\": \"%s\",\n", info.faststart ? "faststart" : "standard");
        printf("  \"v7b_scalable_status\": \"%s\",\n",
               tc_scalable_status_name(scalable));
        printf("  \"file_size\": %llu,\n", (unsigned long long)io.length);
        printf("  \"width\": %u, \"height\": %u,\n", info.visible_width, info.visible_height);
        printf("  \"bitstream_major\": %u, \"profile\": %u, \"pixel_format\": %u, \"bit_depth\": %u,\n",
               info.reserved[0], info.profile, info.pixel_format, info.bit_depth);
        printf("  \"qp_base\": %u, \"qp_delta_luma\": %d, \"qp_delta_chroma\": %d, \"qmatrix\": %u,\n",
               info.qp_base, (int)info.qp_delta_luma, (int)info.qp_delta_chroma, info.qmatrix_id);
        printf("  \"alpha_mode\": %u, \"alpha_bit_depth\": %u, \"alpha_premultiplied\": %u,\n",
               info.alpha_mode, info.alpha_bit_depth, info.alpha_premultiplied);
        printf("  \"color\": {\"range\": %u, \"primaries\": %u, \"transfer\": %u, \"matrix\": %u, \"siting\": %u},\n",
               info.color_range, info.color_primaries, info.color_transfer,
               info.color_matrix, info.chroma_siting);
        printf("  \"sar\": \"%u:%u\",\n", info.sar_num, info.sar_den);
        printf("  \"timescale\": %u, \"duration_ticks\": %llu,\n",
               info.timescale, (unsigned long long)duration_s_num);
        printf("  \"sample_count\": %u, \"index_bytes\": %u,\n",
               info.sample_count, info.index_bytes);
    }

    /* 采样表摘要 + 校验 */
    uint32_t shown = samples < info.sample_count ? samples : info.sample_count;
    if (!json) { printf("samples_table (first %u):\n", shown); }
    else { printf("  \"samples\": [\n"); }
    uint32_t crc_bad = 0u;
    uint32_t read_err = 0u;   /* 索引越界等读不出的 sample（JSON 显式计数，不静默跳过） */
    uint8_t* pkt = NULL;
    size_t pkt_cap = 0u;

    /* V9（micro-gop 计划批 3）：轨级 GOP 摘要——首包判 major；I 数 =
     * sync 位计数（V9 mux 恒 stss 只列 I） */
    if (info.sample_count != 0u) {
        size_t need0 = 0u;
        if (tc_movie_packet(mv, 0, NULL, 0, &need0) == TC_ERR_BUFFER_TOO_SMALL &&
            need0 >= 19u) {
            if (pkt_cap < need0) {
                tc_free(pkt);
                pkt = (uint8_t*)tc_alloc(need0);
                pkt_cap = pkt ? need0 : 0u;
            }
            if (pkt != NULL &&
                tc_movie_packet(mv, 0, pkt, pkt_cap, NULL) == TC_OK &&
                memcmp(pkt, "TPIC", 4u) == 0 && pkt[6] == 9u) {
                uint32_t i_count = 0u;
                for (uint32_t k = 0; k < info.sample_count; ++k) {
                    uint8_t sy = 0u;
                    tc_movie_packet_sync(mv, k, &sy);
                    i_count += sy;
                }
                if (!json) {
                    printf("  v9 gop: frames=%u I=%u P=%u first_gop_id=%u\n",
                           info.sample_count, i_count, info.sample_count - i_count,
                           (unsigned)((pkt[16] << 8) | pkt[17]));
                }
            }
        }
    }
    for (uint32_t i = 0; i < shown; ++i) {
        size_t need = 0;
        int32_t prc = tc_movie_packet(mv, i, NULL, 0, &need);
        if (prc != TC_ERR_BUFFER_TOO_SMALL) {
            read_err++;
            if (!json) { printf("  %u: READ-ERROR %s\n", i, tc_status_message(prc)); }
            continue;
        }
        uint64_t pts = 0;
        uint32_t dur = 0;
        uint8_t sync = 0;
        tc_movie_packet_pts(mv, i, &pts, &dur);
        tc_movie_packet_sync(mv, i, &sync);
        if (!json) {
            printf("  %u: size=%zu pts=%llu dur=%u sync=%u\n",
                   i, need, (unsigned long long)pts, dur, sync);
        } else {
            printf("    {\"index\": %u, \"size\": %zu, \"pts\": %llu, \"dur\": %u, \"sync\": %u}%s\n",
                   i, need, (unsigned long long)pts, dur, sync,
                   i + 1u < shown ? "," : "");
        }
    }
    if (json) { printf("  ],\n  \"read_errors\": %u%s\n", read_err, verify ? "," : ""); }

    if (verify) {
        /* R5：全帧 packet_scan 级校验——tc_frame_decode 内部先做
         * tc_packet_scan（magic/帧头 CRC/结构自检）再解出帧信息；
         * 任何一步失败即计坏帧。 */
        for (uint32_t i = 0; i < info.sample_count; ++i) {
            size_t need = 0;
            if (tc_movie_packet(mv, i, NULL, 0, &need) != TC_ERR_BUFFER_TOO_SMALL) {
                crc_bad++;
                continue;
            }
            if (need > pkt_cap) {
                tc_free(pkt);
                pkt = (uint8_t*)tc_alloc(need);
                pkt_cap = pkt ? need : 0u;
            }
            /* 复验 P2-05：OOM 中断 = 未验证帧，计坏并停止（此前静默 break，
             * 少验证的帧不给 CI 失败信号） */
            if (pkt == NULL) {
                crc_bad += info.sample_count - i;
                fprintf(stderr, "verify: OOM at frame %u — 剩余帧未验证\n", i);
                break;
            }
            if (tc_movie_packet(mv, i, pkt, need, NULL) != TC_OK) { crc_bad++; continue; }
            topos_frame_output finfo;
            memset(&finfo, 0, sizeof(finfo));
            finfo.struct_size = (uint32_t)sizeof(finfo);
            finfo.abi_version = TOPOS_CODEC_ABI_VERSION;
            if (tc_frame_decode(pkt, need, NULL, NULL, &finfo) != TC_OK) { crc_bad++; }
        }
        if (!json) {
            printf("verify: %u/%u frames OK%s\n",
                   info.sample_count - crc_bad, info.sample_count,
                   crc_bad ? "" : " (magic+CRC+结构)");
        } else {
            printf("  \"verify_ok\": %u, \"verify_bad\": %u\n",
                   info.sample_count - crc_bad, crc_bad);
        }
    }
    if (json) { printf("}\n"); }
    tc_free(pkt);
    tc_movie_close(mv);
    fclose(f);
    /* 复验 P2-05：--verify 检出坏帧（或存在未验证帧）必须以非零退出——
     * 此前 verify: 3/4 frames OK 仍返回 0，CI 无法据此失败 */
    if (verify && crc_bad != 0u) { return 1; }
    return 0;
}
