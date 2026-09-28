/* RD4-08：V2 全帧与 V7-B scalable packet 的体积/编码速度测量。
 * --writer rice（公开 minor-2）| pyramid（ADR-C030 minor-4 实验路径）。 */
#include "topos_codec.h"
#include "codec/v7_scalable.h"
#include "transform/base_scale.h"

#include "image_synth.h"

#include <inttypes.h>
#include <math.h>
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
    /* Product-preview target: 1920×1080 (not the cinema-DCI 2048 edge). */
    kBaseMaxDim = 1920u,
    kDefaultIterations = 3u,
};

typedef struct gate_case {
    uint32_t width;
    uint32_t height;
} gate_case;

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
    config->qp_base = 24u;
}

static void make_input(topos_frame_input* input, const uint16_t* y,
                       const uint16_t* u, const uint16_t* v,
                       uint32_t width)
{
    memset(input, 0, sizeof(*input));
    input->struct_size = (uint32_t)sizeof(*input);
    input->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    input->planes[0] = y;
    input->planes[1] = u;
    input->planes[2] = v;
    input->strides[0] = (size_t)width;
    input->strides[1] = ((size_t)width + 1u) / 2u;
    input->strides[2] = ((size_t)width + 1u) / 2u;
}

static void free_planes(uint16_t* y, uint16_t* u, uint16_t* v)
{
    free(y);
    free(u);
    free(v);
}

static int measure_luma_psnr(const uint8_t* packet, size_t packet_size,
                             const uint16_t* source_y,
                             uint32_t width, uint32_t height,
                             double* out_psnr)
{
    if (packet == NULL || source_y == NULL || out_psnr == NULL) { return 0; }
    const uint32_t chroma_width = (width + 1u) / 2u;
    const size_t y_count = (size_t)width * height;
    const size_t chroma_count = (size_t)chroma_width * height;
    uint16_t* y = (uint16_t*)malloc(y_count * sizeof(*y));
    uint16_t* u = (uint16_t*)malloc(chroma_count * sizeof(*u));
    uint16_t* v = (uint16_t*)malloc(chroma_count * sizeof(*v));
    if (y == NULL || u == NULL || v == NULL) {
        free_planes(y, u, v);
        return 0;
    }
    uint16_t* planes[TC_FRAME_MAX_PLANES] = { y, u, v, NULL };
    size_t strides[TC_FRAME_MAX_PLANES] = { width, chroma_width, chroma_width, 0u };
    topos_frame_output output;
    const int32_t rc = tc_v7b_packet_is(packet, packet_size)
        ? tc_v7b_frame_decode(packet, packet_size, kBaseMaxDim,
                              TC_V7B_DECODE_FULL, planes, strides, &output, NULL)
        : tc_frame_decode(packet, packet_size, planes, strides, &output);
    if (rc != TC_OK && rc != TC_WARN_CONCEALED) {
        free_planes(y, u, v);
        return 0;
    }
    long double sum_squared_error = 0.0L;
    for (size_t i = 0u; i < y_count; ++i) {
        const int64_t delta = (int64_t)source_y[i] - (int64_t)y[i];
        sum_squared_error += (long double)(delta * delta);
    }
    free_planes(y, u, v);
    if (sum_squared_error == 0.0L) {
        *out_psnr = 99.0;
    } else {
        const long double mse = sum_squared_error / (long double)y_count;
        *out_psnr = 10.0 * log10((1023.0 * 1023.0) / (double)mse);
    }
    return 1;
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

static uint32_t gate_gcd(uint32_t a, uint32_t b)
{
    while (b != 0u) {
        const uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

int main(int argc, char** argv)
{
    uint32_t iterations = kDefaultIterations;
    int quick = 0;
    int interop = 0;
    int pyramid = 0;
    tc_synth_kind kind = TC_SYNTH_GRADIENT;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--quick") == 0) {
            quick = 1;
        } else if (strcmp(argv[i], "--interop") == 0) {
            interop = 1;
        } else if (strcmp(argv[i], "--kind") == 0 && i + 1 < argc) {
            const char* name = argv[++i];
            if (strcmp(name, "flat") == 0) { kind = TC_SYNTH_FLAT; }
            else if (strcmp(name, "gradient") == 0) { kind = TC_SYNTH_GRADIENT; }
            else if (strcmp(name, "grain") == 0) { kind = TC_SYNTH_GRAIN; }
            else if (strcmp(name, "detail") == 0) { kind = TC_SYNTH_DETAIL; }
            else if (strcmp(name, "mixed") == 0) { kind = TC_SYNTH_MIXED; }
            else {
                fprintf(stderr, "unknown --kind (flat|gradient|grain|detail|mixed)\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--writer") == 0 && i + 1 < argc) {
            if (strcmp(argv[++i], "pyramid") == 0) { pyramid = 1; }
            else if (strcmp(argv[i], "rice") != 0) {
                fprintf(stderr, "unknown --writer (rice|pyramid)\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            if (!parse_positive(argv[++i], &iterations)) {
                fprintf(stderr, "invalid --iters value\n");
                return 2;
            }
        } else {
            fprintf(stderr, "usage: %s [--quick] [--interop] "
                    "[--writer rice|pyramid] [--kind flat|gradient|grain|detail|mixed] "
                    "[--iters N]\n", argv[0]);
            return 2;
        }
    }

    /* Product main table (plan §1.1): 16:9 sources whose ideal 2K preview
     * ratio is exactly 1, 1/2, 1/3, 1/4, and 1/6.  6K is 5760×3240 here --
     * the DCI 6144×3456 edge belongs to the interop appendix below. */
    static const gate_case main_corpus[] = {
        {1920u, 1080u},
        {3840u, 2160u},
        {5760u, 3240u},
        {7680u, 4320u},
        {11520u, 6480u},
    };
    /* Interoperability appendix: cinema ratios whose 1/3 base is NOT
     * 1920×1080 (6144×3456 -> 2048×1152); reported separately so it can
     * never be quoted as a product 2K number. */
    static const gate_case interop_corpus[] = {
        {6144u, 3456u},
        {12288u, 6480u},
    };
    static const gate_case quick_corpus[] = {
        {1920u, 1080u},
        {3840u, 2160u},
        {5760u, 3240u},
    };
    const gate_case* corpus;
    size_t corpus_count;
    if (interop) {
        corpus = interop_corpus;
        corpus_count = sizeof(interop_corpus) / sizeof(interop_corpus[0]);
    } else if (quick) {
        corpus = quick_corpus;
        corpus_count = sizeof(quick_corpus) / sizeof(quick_corpus[0]);
    } else {
        corpus = main_corpus;
        corpus_count = sizeof(main_corpus) / sizeof(main_corpus[0]);
    }

    puts("width,height,iteration,v2_bytes,v7b_bytes,v2_encode_ms,v7b_encode_ms,"
         "v2_psnr_y,v7b_psnr_y,base_width,base_height,"
         "target_width,target_height,ratio_num,ratio_den,table,writer,kind");
    for (size_t cidx = 0u; cidx < corpus_count; ++cidx) {
        const uint32_t width = corpus[cidx].width;
        const uint32_t height = corpus[cidx].height;
        image_synth_cfg synth;
        memset(&synth, 0, sizeof(synth));
        synth.seed = UINT64_C(0x5244343038) ^ ((uint64_t)width << 16) ^ height;
        synth.width = width;
        synth.height = height;
        synth.kind = kind;
        synth.bit_depth = 10u;
        synth.chroma_format = 0u;
        uint16_t* y = NULL;
        uint16_t* u = NULL;
        uint16_t* v = NULL;
        uint16_t* alpha = NULL;
        if (image_synth_alloc(&synth, 0, &y, &u, &v, &alpha) != 0) {
            fprintf(stderr, "source allocation failed for %ux%u\n",
                    (unsigned)width, (unsigned)height);
            free_planes(y, u, v);
            return 1;
        }
        (void)alpha;

        topos_frame_config config;
        make_config(&config, width, height);
        topos_frame_input input;
        make_input(&input, y, u, v, width);
        const size_t packet_cap = tc_frame_packet_bound(&config);
        uint8_t* v2_packet = (uint8_t*)malloc(packet_cap == 0u ? 1u : packet_cap);
        if (v2_packet == NULL) {
            fprintf(stderr, "packet allocation failed for %ux%u (%zu bytes)\n",
                    (unsigned)width, (unsigned)height, packet_cap);
            free_planes(y, u, v);
            return 1;
        }
        uint32_t base_width = 0u;
        uint32_t base_height = 0u;
        if (!tc_base_scale_dimensions(width, height, kBaseMaxDim,
                                      &base_width, &base_height)) {
            fprintf(stderr, "base geometry failed for %ux%u\n",
                    (unsigned)width, (unsigned)height);
            free(v2_packet);
            free_planes(y, u, v);
            return 1;
        }

        /* The V7-B residual is content-dependent; obtain the exact packet
         * capacity through the public writer's dry-run before measuring the
         * real encode call. */
        topos_frame_stats v7b_probe;
        memset(&v7b_probe, 0, sizeof(v7b_probe));
        int32_t probe_rc = pyramid
            ? tc_v7b_frame_encode_pyramid(&config, &input, kBaseMaxDim,
                                          NULL, 0u, &v7b_probe)
            : tc_frame_encode_scalable(&config, &input, kBaseMaxDim, NULL, 0u,
                                       &v7b_probe);
        if (probe_rc != TC_ERR_BUFFER_TOO_SMALL || v7b_probe.packet_size == 0u) {
            fprintf(stderr, "V7-B size probe failed for %ux%u: %s\n",
                    (unsigned)width, (unsigned)height, tc_last_error());
            free(v2_packet);
            free_planes(y, u, v);
            return 1;
        }
        uint8_t* v7b_packet = (uint8_t*)malloc(v7b_probe.packet_size);
        if (v7b_packet == NULL) {
            fprintf(stderr, "V7-B packet allocation failed for %ux%u (%u bytes)\n",
                    (unsigned)width, (unsigned)height,
                    (unsigned)v7b_probe.packet_size);
            free(v2_packet);
            free_planes(y, u, v);
            return 1;
        }

        double v2_psnr_y = 0.0;
        double v7b_psnr_y = 0.0;

        for (uint32_t iter = 0u; iter < iterations; ++iter) {
            topos_frame_stats v2_stats;
            memset(&v2_stats, 0, sizeof(v2_stats));
            uint64_t start = now_ns();
            int32_t rc = tc_frame_encode(&config, &input, v2_packet, packet_cap,
                                          &v2_stats);
            uint64_t end = now_ns();
            if (rc != TC_OK) {
                fprintf(stderr, "V2 encode failed for %ux%u iter %u: %s\n",
                        (unsigned)width, (unsigned)height, (unsigned)iter,
                        tc_last_error());
                free(v2_packet);
                free(v7b_packet);
                free_planes(y, u, v);
                return 1;
            }
            const double v2_ms = (double)(end - start) / 1000000.0;

            topos_frame_stats v7b_stats;
            memset(&v7b_stats, 0, sizeof(v7b_stats));
            start = now_ns();
            rc = pyramid
                ? tc_v7b_frame_encode_pyramid(&config, &input, kBaseMaxDim,
                                              v7b_packet, v7b_probe.packet_size,
                                              &v7b_stats)
                : tc_frame_encode_scalable(&config, &input, kBaseMaxDim,
                                           v7b_packet, v7b_probe.packet_size,
                                           &v7b_stats);
            end = now_ns();
            if (rc != TC_OK) {
                fprintf(stderr, "V7-B encode failed for %ux%u iter %u: %s\n",
                        (unsigned)width, (unsigned)height, (unsigned)iter,
                        tc_last_error());
                free(v2_packet);
                free(v7b_packet);
                free_planes(y, u, v);
                return 1;
            }
            const double v7b_ms = (double)(end - start) / 1000000.0;
            if (iter == 0u &&
                (!measure_luma_psnr(v2_packet, v2_stats.packet_size, y, width, height,
                                    &v2_psnr_y) ||
                 !measure_luma_psnr(v7b_packet, v7b_stats.packet_size, y, width, height,
                                    &v7b_psnr_y))) {
                fprintf(stderr, "quality decode failed for %ux%u\n",
                        (unsigned)width, (unsigned)height);
                free(v2_packet);
                free(v7b_packet);
                free_planes(y, u, v);
                return 1;
            }
            const uint32_t ratio_div = gate_gcd(width, base_width);
            const uint32_t ratio_num = width / ratio_div;
            const uint32_t ratio_den = base_width / ratio_div;
            printf("%u,%u,%u,%u,%u,%.6f,%.6f,%.6f,%.6f,%u,%u,%u,%u,%u,%u,%s,%s,%s\n",
                   (unsigned)width, (unsigned)height, (unsigned)iter,
                   (unsigned)v2_stats.packet_size,
                   (unsigned)v7b_stats.packet_size,
                   v2_ms, v7b_ms, v2_psnr_y, v7b_psnr_y, (unsigned)base_width,
                   (unsigned)base_height, (unsigned)base_width, (unsigned)base_height,
                   (unsigned)ratio_num, (unsigned)ratio_den,
                   interop ? "interop" : "main", pyramid ? "pyramid" : "rice",
                   kind == TC_SYNTH_FLAT ? "flat"
                       : kind == TC_SYNTH_GRAIN ? "grain"
                       : kind == TC_SYNTH_DETAIL ? "detail"
                       : kind == TC_SYNTH_MIXED ? "mixed" : "gradient");
            fflush(stdout);
        }
        free(v2_packet);
        free(v7b_packet);
        free_planes(y, u, v);
    }
    return 0;
}
