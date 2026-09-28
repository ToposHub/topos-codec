/* P4-02 / 计划 §6.3：codec-only 2K 预览解码对比基准。
 *
 * 对每个源尺寸（quick：1920/3840/5760/7680；全量另含 11520）：
 *   1. 合成确定性内容，用 ADR-C030 minor-4 pyramid 写入器编码一次（不计
 *      时；2K 源经回落路径 base=源本身）；
 *   2. 计时 tc_v7b_frame_decode(BASE_ONLY) —— 高分辨率→2K 预览路径，
 *      只读 base 段，输出精确 1920×1080；
 * 参照：同内容 1920×1080 的 V2 包 tc_frame_decode（"正常 2K 解码"）。
 * CSV 每行一个样本；p50/p95 与比值由 Python 侧计算。
 */
#include "topos_codec.h"
#include "codec/v7_scalable.h"
#include "common/tpool.h"
#include "transform/base_scale.h"

#include "image_synth.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

enum {
    kBaseMaxDim = 1920u,
    kDefaultIterations = 30u,
    kWarmup = 3u,
    kDefaultQp = 24u,
};

typedef struct preview_case {
    uint32_t width;
    uint32_t height;
} preview_case;

static uint64_t now_ns(void)
{
#ifdef _WIN32
    /* QPC 单调时钟（MSVC CRT 无 clock_gettime/CLOCK_MONOTONIC） */
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); }
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000000ull) / (uint64_t)freq.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0u) { return 0u; }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
#endif
}

static uint32_t g_qp_base = kDefaultQp;

static void make_config(topos_frame_config* config, uint32_t width, uint32_t height)
{
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
    config->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    config->visible_width = (uint16_t)width;
    config->visible_height = (uint16_t)height;
    config->profile = 3u;
    config->pixel_format = 0u;
    config->bit_depth = 10u;
    config->qmatrix_id = 1u;
    config->qp_base = (uint8_t)g_qp_base;
}

static int parse_positive(const char* text, uint32_t* out)
{
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (text[0] == '\0' || end == NULL || *end != '\0' || value == 0ul ||
        value > UINT32_MAX) {
        return 0;
    }
    *out = (uint32_t)value;
    return 1;
}

static int synth_source(const preview_case* cse, tc_synth_kind kind,
                        topos_frame_input* input, topos_frame_config* config)
{
    image_synth_cfg synth;
    memset(&synth, 0, sizeof(synth));
    synth.seed = UINT64_C(0x5052324B30383031) ^ ((uint64_t)cse->width << 16) ^
                 cse->height;
    synth.width = cse->width;
    synth.height = cse->height;
    synth.kind = kind;
    synth.bit_depth = 10u;
    synth.chroma_format = 0u;
    uint16_t* y = NULL;
    uint16_t* u = NULL;
    uint16_t* v = NULL;
    uint16_t* a = NULL;
    if (image_synth_alloc(&synth, 0, &y, &u, &v, &a) != 0) {
        return 0;
    }
    make_config(config, cse->width, cse->height);
    memset(input, 0, sizeof(*input));
    input->struct_size = (uint32_t)sizeof(*input);
    input->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    input->planes[0] = y;
    input->planes[1] = u;
    input->planes[2] = v;
    input->strides[0] = cse->width;
    input->strides[1] = ((size_t)cse->width + 1u) / 2u;
    input->strides[2] = ((size_t)cse->width + 1u) / 2u;
    return 1;
}

static void free_input(topos_frame_input* input)
{
    free((void*)input->planes[0]);
    free((void*)input->planes[1]);
    free((void*)input->planes[2]);
}

int main(int argc, char** argv)
{
    uint32_t iterations = kDefaultIterations;
    int quick = 0;
    tc_synth_kind kind = TC_SYNTH_GRADIENT;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--quick") == 0) {
            quick = 1;
        } else if (strcmp(argv[i], "--kind") == 0 && i + 1 < argc) {
            const char* name = argv[++i];
            if (strcmp(name, "flat") == 0) { kind = TC_SYNTH_FLAT; }
            else if (strcmp(name, "gradient") == 0) { kind = TC_SYNTH_GRADIENT; }
            else if (strcmp(name, "grain") == 0) { kind = TC_SYNTH_GRAIN; }
            else if (strcmp(name, "detail") == 0) { kind = TC_SYNTH_DETAIL; }
            else if (strcmp(name, "mixed") == 0) { kind = TC_SYNTH_MIXED; }
            else {
                fprintf(stderr, "unknown --kind\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            if (!parse_positive(argv[++i], &iterations)) {
                fprintf(stderr, "invalid --iters value\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            uint32_t threads = 0u;
            if (!parse_positive(argv[++i], &threads) || threads > 256u) {
                fprintf(stderr, "invalid --threads value\n");
                return 2;
            }
            tc_dev_set_thread_count(threads);
        } else if (strcmp(argv[i], "--qp") == 0 && i + 1 < argc) {
            uint32_t qp = 0u;
            if (!parse_positive(argv[++i], &qp) || qp > 95u) {
                fprintf(stderr, "invalid --qp value (1..95)\n");
                return 2;
            }
            g_qp_base = qp;
        } else {
            fprintf(stderr, "usage: %s [--quick] "
                    "[--kind flat|gradient|grain|detail|mixed] "
                    "[--qp 1..95] [--threads N] [--iters N]\n", argv[0]);
            return 2;
        }
    }

    static const preview_case quick_cases[] = {
        {1920u, 1080u}, {3840u, 2160u}, {5760u, 3240u}, {7680u, 4320u},
    };
    static const preview_case full_cases[] = {
        {1920u, 1080u}, {3840u, 2160u}, {5760u, 3240u}, {7680u, 4320u},
        {11520u, 6480u},
    };
    const preview_case* cases = quick ? quick_cases : full_cases;
    const size_t case_count = quick ? sizeof(quick_cases) / sizeof(quick_cases[0])
                                    : sizeof(full_cases) / sizeof(full_cases[0]);

    /* 输出平面：base-only 输出恒为 base 几何；主表源（含 2K 回落）的
     * base 均为 1920×1080，422 chroma 960×1080。 */
    const uint32_t out_w = 1920u;
    const uint32_t out_h = 1080u;
    const uint32_t out_cw = (out_w + 1u) / 2u;
    const size_t y_count = (size_t)out_w * out_h;
    const size_t c_count = (size_t)out_cw * out_h;
    uint16_t* dy = (uint16_t*)malloc(y_count * sizeof(uint16_t));
    uint16_t* du = (uint16_t*)malloc(c_count * sizeof(uint16_t));
    uint16_t* dv = (uint16_t*)malloc(c_count * sizeof(uint16_t));
    if (dy == NULL || du == NULL || dv == NULL) {
        fprintf(stderr, "output allocation failed\n");
        return 1;
    }
    uint16_t* decode_planes[TC_FRAME_MAX_PLANES] = { dy, du, dv, NULL };
    size_t decode_strides[TC_FRAME_MAX_PLANES] = { out_w, out_cw, out_cw, 0u };

    puts("source_w,source_h,mode,iteration,decode_ms,target_w,target_h,"
         "bytes_read,bytes_skipped,segments_skipped,kind,"
         "v7b_packet_bytes,v2_packet_bytes,qp");

    /* —— 参照：正常 2K 解码（V2 1920×1080 包 → 2K 平面）—— */
    {
        const preview_case ref_case = { out_w, out_h };
        topos_frame_config config;
        topos_frame_input input;
        if (!synth_source(&ref_case, kind, &input, &config)) {
            fprintf(stderr, "reference synth failed\n");
            return 1;
        }
        const size_t cap = tc_frame_packet_bound(&config);
        uint8_t* packet = (uint8_t*)malloc(cap == 0u ? 1u : cap);
        if (packet == NULL) {
            fprintf(stderr, "reference packet allocation failed\n");
            free_input(&input);
            return 1;
        }
        topos_frame_stats stats;
        memset(&stats, 0, sizeof(stats));
        if (tc_frame_encode(&config, &input, packet, cap, &stats) != TC_OK) {
            fprintf(stderr, "reference encode failed: %s\n", tc_last_error());
            free(packet);
            free_input(&input);
            return 1;
        }
        topos_frame_output out_info;
        for (uint32_t iter = 0u; iter < iterations + kWarmup; ++iter) {
            uint64_t start = now_ns();
            int32_t rc = tc_frame_decode(packet, stats.packet_size,
                                         decode_planes, decode_strides, &out_info);
            uint64_t end = now_ns();
            if (rc != TC_OK && rc != TC_WARN_CONCEALED) {
                fprintf(stderr, "reference decode failed: %s\n", tc_last_error());
                free(packet);
                free_input(&input);
                return 1;
            }
            if (iter >= kWarmup) {
                printf("%u,%u,v2_2k_ref,%u,%.6f,%u,%u,%u,0,0,%s,%u,%u,%u\n",
                       (unsigned)ref_case.width, (unsigned)ref_case.height,
                       (unsigned)(iter - kWarmup),
                       (double)(end - start) / 1000000.0,
                       (unsigned)out_w, (unsigned)out_h,
                       (unsigned)stats.packet_size,
                       kind == TC_SYNTH_FLAT ? "flat"
                           : kind == TC_SYNTH_GRAIN ? "grain"
                           : kind == TC_SYNTH_DETAIL ? "detail"
                           : kind == TC_SYNTH_MIXED ? "mixed" : "gradient",
                       (unsigned)stats.packet_size,
                       (unsigned)stats.packet_size,
                       (unsigned)g_qp_base);
                fflush(stdout);
            }
        }
        free(packet);
        free_input(&input);
    }

    /* —— 各源尺寸：V7-B minor-4 base-only → 2K —— */
    for (size_t ci = 0u; ci < case_count; ++ci) {
        const uint32_t width = cases[ci].width;
        const uint32_t height = cases[ci].height;
        topos_frame_config config;
        topos_frame_input input;
        if (!synth_source(&cases[ci], kind, &input, &config)) {
            fprintf(stderr, "synth failed for %ux%u\n",
                    (unsigned)width, (unsigned)height);
            return 1;
        }
        /* 先探测容量再正式编码（与 encode gate 相同的 probe 语义） */
        topos_frame_stats probe;
        memset(&probe, 0, sizeof(probe));
        int32_t rc = tc_v7b_frame_encode_pyramid(&config, &input, kBaseMaxDim,
                                                 NULL, 0u, &probe);
        if (rc != TC_ERR_BUFFER_TOO_SMALL || probe.packet_size == 0u) {
            fprintf(stderr, "pyramid probe failed for %ux%u: %s\n",
                    (unsigned)width, (unsigned)height, tc_last_error());
            free_input(&input);
            return 1;
        }
        uint8_t* packet = (uint8_t*)malloc(probe.packet_size);
        if (packet == NULL) {
            fprintf(stderr, "packet allocation failed for %ux%u\n",
                    (unsigned)width, (unsigned)height);
            free_input(&input);
            return 1;
        }
        topos_frame_stats enc_stats;
        memset(&enc_stats, 0, sizeof(enc_stats));
        rc = tc_v7b_frame_encode_pyramid(&config, &input, kBaseMaxDim, packet,
                                         probe.packet_size, &enc_stats);
        if (rc != TC_OK) {
            fprintf(stderr, "pyramid encode failed for %ux%u: %s\n",
                    (unsigned)width, (unsigned)height, tc_last_error());
            free(packet);
            free_input(&input);
            return 1;
        }
        uint32_t v2_source_bytes = 0u;
        {
            const size_t v2_cap = tc_frame_packet_bound(&config);
            uint8_t* v2_packet = (uint8_t*)malloc(v2_cap == 0u ? 1u : v2_cap);
            topos_frame_stats v2_stats;
            memset(&v2_stats, 0, sizeof(v2_stats));
            if (v2_packet == NULL ||
                tc_frame_encode(&config, &input, v2_packet, v2_cap, &v2_stats) != TC_OK) {
                fprintf(stderr, "V2 source encode failed for %ux%u: %s\n",
                        (unsigned)width, (unsigned)height, tc_last_error());
                free(v2_packet);
                free(packet);
                free_input(&input);
                return 1;
            }
            v2_source_bytes = v2_stats.packet_size;
            free(v2_packet);
        }
        topos_frame_output out_info;
        tc_v7b_decode_stats dstats;
        for (uint32_t iter = 0u; iter < iterations + kWarmup; ++iter) {
            memset(&dstats, 0, sizeof(dstats));
            uint64_t start = now_ns();
            rc = tc_v7b_frame_decode(packet, enc_stats.packet_size, kBaseMaxDim,
                                     TC_V7B_DECODE_BASE_ONLY, decode_planes,
                                     decode_strides, &out_info, &dstats);
            uint64_t end = now_ns();
            if (rc != TC_OK) {
                fprintf(stderr, "base-only decode failed for %ux%u: %s\n",
                        (unsigned)width, (unsigned)height, tc_last_error());
                free(packet);
                free_input(&input);
                return 1;
            }
            if (iter >= kWarmup) {
                printf("%u,%u,v7b_base_only,%u,%.6f,%u,%u,%llu,%llu,%u,%s,%u,%u,%u\n",
                       (unsigned)width, (unsigned)height,
                       (unsigned)(iter - kWarmup),
                       (double)(end - start) / 1000000.0,
                       (unsigned)out_info.visible_width,
                       (unsigned)out_info.visible_height,
                       (unsigned long long)dstats.bytes_read,
                       (unsigned long long)dstats.bytes_skipped,
                       (unsigned)dstats.segments_skipped,
                       kind == TC_SYNTH_FLAT ? "flat"
                           : kind == TC_SYNTH_GRAIN ? "grain"
                           : kind == TC_SYNTH_DETAIL ? "detail"
                           : kind == TC_SYNTH_MIXED ? "mixed" : "gradient",
                       (unsigned)enc_stats.packet_size,
                       (unsigned)v2_source_bytes,
                       (unsigned)g_qp_base);
                fflush(stdout);
            }
        }
        free(packet);
        free_input(&input);
    }
    free(dy);
    free(du);
    free(dv);
    return 0;
}
