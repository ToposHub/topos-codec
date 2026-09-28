/* topos_scalable_corpus —— P0-03 冻结的 scalable 基准 corpus 生成器。
 *
 * 冻结表（kCorpusItems / kCorpusTableVersion）定义每一类内容的确定性
 * image_synth 参数；本工具按条目生成 raw 平面（Y|U|V[|A]，原生字节序
 * u16，布局与 topos_rawgen 一致），并用冻结的 profile/qp/qmatrix 编码
 * V2 质量参考包、回解测 PSNR-Y。SHA256 与 manifest 装订由
 * scripts/freeze_topos_scalable_corpus.py 完成——工具本身只产出位精确
 * 工件与参数，保证任何平台同一 commit 重建结果逐字节一致。
 *
 * 用法：
 *   topos_scalable_corpus --list
 *   topos_scalable_corpus --item <id> --outdir <dir>
 *   topos_scalable_corpus --all --outdir <dir>
 */
#include "topos_codec.h"

#include "image_synth.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CORPUS_TABLE_VERSION 1u

/* 色彩标签（bitstream_spec_v1 §A.7）：primaries 9=bt2020、transfer
 * 16=smpte2084(PQ)、matrix 1=bt709 / 9=bt2020nc、0=identity(GBR)。 */
typedef struct corpus_item {
    const char* id;
    const char* content_class; /* flat/gradient/detail_text/texture/grain/hdr/alpha */
    tc_synth_kind kind;
    uint64_t seed;
    uint32_t width;
    uint32_t height;
    uint32_t bit_depth;      /* 10 / 12 */
    uint32_t chroma_format;  /* 0=4:2:2, 1=4:4:4（仅合成平面几何） */
    uint32_t pixel_format;   /* 0=YUV422, 1=YUV444, 2=GBR */
    uint32_t profile;        /* 3=Standard, 5=Pro444 */
    uint32_t alpha_mode;     /* 0 / 1(straight, 16-bit) */
    uint32_t color_range;
    uint32_t color_primaries;
    uint32_t color_transfer;
    uint32_t color_matrix;
} corpus_item;

/* 冻结表 v1：内容类覆盖 plan P0-03 全部类目。seed 为冻结字面量，改表必须
 * 递增 CORPUS_TABLE_VERSION 并在 manifest 中记录原因。 */
static const corpus_item kCorpusItems[] = {
    {"flat_1080p_10_422", "flat", TC_SYNTH_FLAT,
     UINT64_C(0x53433130386C6661), 1920u, 1080u, 10u, 0u, 0u, 3u, 0u,
     0u, 1u, 1u, 1u},
    {"gradient_1080p_10_422", "gradient", TC_SYNTH_GRADIENT,
     UINT64_C(0x5343313038677264), 1920u, 1080u, 10u, 0u, 0u, 3u, 0u,
     0u, 1u, 1u, 1u},
    {"detail_text_1080p_10_422", "detail_text", TC_SYNTH_DETAIL,
     UINT64_C(0x534331303864746C), 1920u, 1080u, 10u, 0u, 0u, 3u, 0u,
     0u, 1u, 1u, 1u},
    {"texture_1080p_10_422", "texture", TC_SYNTH_MIXED,
     UINT64_C(0x5343313038747874), 1920u, 1080u, 10u, 0u, 0u, 3u, 0u,
     0u, 1u, 1u, 1u},
    {"grain_1080p_10_422", "grain", TC_SYNTH_GRAIN,
     UINT64_C(0x534331303867726E), 1920u, 1080u, 10u, 0u, 0u, 3u, 0u,
     0u, 1u, 1u, 1u},
    {"gradient_1080p_12_422", "gradient", TC_SYNTH_GRADIENT,
     UINT64_C(0x5343313238677264), 1920u, 1080u, 12u, 0u, 0u, 3u, 0u,
     0u, 1u, 1u, 1u},
    {"grain_1080p_12_422", "grain", TC_SYNTH_GRAIN,
     UINT64_C(0x534331323867726E), 1920u, 1080u, 12u, 0u, 0u, 3u, 0u,
     0u, 1u, 1u, 1u},
    {"gradient_1080p_10_444", "gradient", TC_SYNTH_GRADIENT,
     UINT64_C(0x5343343438677264), 1920u, 1080u, 10u, 1u, 1u, 5u, 0u,
     0u, 1u, 1u, 1u},
    {"grain_1080p_10_444", "grain", TC_SYNTH_GRAIN,
     UINT64_C(0x534334343867726E), 1920u, 1080u, 10u, 1u, 1u, 5u, 0u,
     0u, 1u, 1u, 1u},
    {"hdr_grain_1080p_12_444_pq", "hdr", TC_SYNTH_GRAIN,
     UINT64_C(0x534348445267726E), 1920u, 1080u, 12u, 1u, 1u, 5u, 0u,
     1u, 9u, 16u, 9u},
    {"gbr_texture_1080p_10", "texture", TC_SYNTH_MIXED,
     UINT64_C(0x5343474252747874), 1920u, 1080u, 10u, 1u, 2u, 5u, 0u,
     0u, 1u, 1u, 0u},
    {"alpha_detail_1080p_10_422", "alpha", TC_SYNTH_DETAIL,
     UINT64_C(0x53434C503864746C), 1920u, 1080u, 10u, 0u, 0u, 3u, 1u,
     0u, 1u, 1u, 1u},
    {"gradient_2160p_10_422", "gradient", TC_SYNTH_GRADIENT,
     UINT64_C(0x5343323138677264), 3840u, 2160u, 10u, 0u, 0u, 3u, 0u,
     0u, 1u, 1u, 1u},
    {"detail_text_2160p_10_422", "detail_text", TC_SYNTH_DETAIL,
     UINT64_C(0x534332313864746C), 3840u, 2160u, 10u, 0u, 0u, 3u, 0u,
     0u, 1u, 1u, 1u},
    {"grain_2160p_10_422", "grain", TC_SYNTH_GRAIN,
     UINT64_C(0x534332313867726E), 3840u, 2160u, 10u, 0u, 0u, 3u, 0u,
     0u, 1u, 1u, 1u},
};

#define CORPUS_ITEM_COUNT (sizeof(kCorpusItems) / sizeof(kCorpusItems[0]))

static const corpus_item* find_item(const char* id)
{
    for (size_t i = 0u; i < CORPUS_ITEM_COUNT; ++i) {
        if (strcmp(kCorpusItems[i].id, id) == 0) { return &kCorpusItems[i]; }
    }
    return NULL;
}

static void print_item_json(const corpus_item* item)
{
    printf("{\"id\":\"%s\",\"class\":\"%s\",\"kind\":%d,"
           "\"seed\":%" PRIu64 ",\"width\":%u,\"height\":%u,"
           "\"bit_depth\":%u,\"chroma_format\":%u,\"pixel_format\":%u,"
           "\"profile\":%u,\"alpha_mode\":%u,\"color_range\":%u,"
           "\"color_primaries\":%u,\"color_transfer\":%u,\"color_matrix\":%u}",
           item->id, item->content_class, (int)item->kind,
           item->seed, item->width, item->height, item->bit_depth,
           item->chroma_format, item->pixel_format, item->profile,
           item->alpha_mode, item->color_range, item->color_primaries,
           item->color_transfer, item->color_matrix);
}

static int write_file(const char* path, const void* data, size_t bytes)
{
    FILE* f = fopen(path, "wb");
    if (f == NULL) { return -1; }
    const size_t written = fwrite(data, 1u, bytes, f);
    if (fclose(f) != 0 || written != bytes) { return -1; }
    return 0;
}

static double measure_psnr_y(const uint16_t* source, const uint16_t* decoded,
                             size_t count)
{
    long double sse = 0.0L;
    for (size_t i = 0u; i < count; ++i) {
        const int64_t delta = (int64_t)source[i] - (int64_t)decoded[i];
        sse += (long double)(delta * delta);
    }
    if (sse == 0.0L) { return 99.0; }
    const long double mse = sse / (long double)count;
    return 10.0 * log10((1023.0 * 1023.0) / (double)mse);
}

static int generate_item(const corpus_item* item, const char* outdir)
{
    image_synth_cfg synth;
    memset(&synth, 0, sizeof(synth));
    synth.seed = item->seed;
    synth.width = item->width;
    synth.height = item->height;
    synth.kind = item->kind;
    synth.bit_depth = item->bit_depth;
    synth.chroma_format = item->chroma_format;
    uint16_t* y = NULL;
    uint16_t* u = NULL;
    uint16_t* v = NULL;
    uint16_t* a = NULL;
    const int with_alpha = item->alpha_mode != 0u;
    if (image_synth_alloc(&synth, with_alpha, &y, &u, &v, &a) != 0) {
        fprintf(stderr, "%s: synth alloc failed\n", item->id);
        return 1;
    }
    const uint32_t chroma_w = item->chroma_format == 0u
        ? (item->width + 1u) / 2u : item->width;
    const size_t y_count = (size_t)item->width * item->height;
    const size_t uv_count = (size_t)chroma_w * item->height;

    /* planes.raw：Y|U|V[|A] 连续布局，与 topos_rawgen/encoder_cli 输入一致。 */
    const size_t plane_bytes = (y_count + 2u * uv_count +
                                (with_alpha ? y_count : 0u)) * 2u;
    uint16_t* packed = (uint16_t*)malloc(plane_bytes);
    if (packed == NULL) {
        fprintf(stderr, "%s: OOM\n", item->id);
        free(y); free(u); free(v); free(a);
        return 1;
    }
    memcpy(packed, y, y_count * 2u);
    memcpy(packed + y_count, u, uv_count * 2u);
    memcpy(packed + y_count + uv_count, v, uv_count * 2u);
    if (with_alpha) {
        memcpy(packed + y_count + 2u * uv_count, a, y_count * 2u);
    }

    topos_frame_config config;
    memset(&config, 0, sizeof(config));
    config.struct_size = (uint32_t)sizeof(config);
    config.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    config.visible_width = (uint16_t)item->width;
    config.visible_height = (uint16_t)item->height;
    config.profile = (uint8_t)item->profile;
    config.pixel_format = (uint8_t)item->pixel_format;
    config.bit_depth = (uint8_t)item->bit_depth;
    config.qmatrix_id = 1u;  /* 冻结的参考锚点：与 scalable gate 相同 */
    config.qp_base = 24u;
    config.alpha_mode = (uint8_t)item->alpha_mode;
    config.alpha_bit_depth = item->alpha_mode != 0u ? 16u : 0u;
    config.color_range = (uint8_t)item->color_range;
    config.color_primaries = (uint8_t)item->color_primaries;
    config.color_transfer = (uint8_t)item->color_transfer;
    config.color_matrix = (uint8_t)item->color_matrix;
    topos_frame_input input;
    memset(&input, 0, sizeof(input));
    input.struct_size = (uint32_t)sizeof(input);
    input.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    input.planes[0] = y;
    input.planes[1] = u;
    input.planes[2] = v;
    input.planes[3] = a;
    input.strides[0] = item->width;
    input.strides[1] = chroma_w;
    input.strides[2] = chroma_w;
    input.strides[3] = item->width;

    const size_t packet_cap = tc_frame_packet_bound(&config);
    uint8_t* packet = packet_cap != 0u ? (uint8_t*)malloc(packet_cap) : NULL;
    uint16_t* dy = (uint16_t*)malloc(y_count * 2u);
    uint16_t* du = (uint16_t*)malloc(uv_count * 2u);
    uint16_t* dv = (uint16_t*)malloc(uv_count * 2u);
    uint16_t* da = with_alpha ? (uint16_t*)malloc(y_count * 2u) : NULL;
    int failed = packet == NULL || dy == NULL || du == NULL || dv == NULL ||
                 (with_alpha && da == NULL);
    double psnr_y = 0.0;
    uint32_t packet_size = 0u;
    if (!failed) {
        topos_frame_stats stats;
        memset(&stats, 0, sizeof(stats));
        if (tc_frame_encode(&config, &input, packet, packet_cap, &stats) != TC_OK) {
            fprintf(stderr, "%s: V2 reference encode failed: %s\n",
                    item->id, tc_last_error());
            failed = 1;
        } else {
            packet_size = stats.packet_size;
            uint16_t* planes[TC_FRAME_MAX_PLANES] = { dy, du, dv, da };
            size_t strides[TC_FRAME_MAX_PLANES] = {
                item->width, chroma_w, chroma_w, item->width
            };
            topos_frame_output out;
            const int32_t rc = tc_frame_decode(packet, packet_size, planes,
                                               strides, &out);
            if (rc != TC_OK && rc != TC_WARN_CONCEALED) {
                fprintf(stderr, "%s: reference decode failed: %s\n",
                        item->id, tc_last_error());
                failed = 1;
            } else {
                psnr_y = measure_psnr_y(y, dy, y_count);
            }
        }
    }

    if (!failed) {
        char path[512];
        if (snprintf(path, sizeof(path), "%s/planes.raw", outdir) < 0 ||
            write_file(path, packed, plane_bytes) != 0) {
            fprintf(stderr, "%s: planes.raw write failed\n", item->id);
            failed = 1;
        }
        if (!failed) {
            snprintf(path, sizeof(path), "%s/v2_reference.tpc", outdir);
            if (write_file(path, packet, packet_size) != 0) {
                fprintf(stderr, "%s: v2_reference.tpc write failed\n", item->id);
                failed = 1;
            }
        }
        if (!failed) {
            FILE* f = NULL;
            snprintf(path, sizeof(path), "%s/reference.json", outdir);
            f = fopen(path, "w");
            if (f == NULL) {
                fprintf(stderr, "%s: reference.json open failed\n", item->id);
                failed = 1;
            } else {
                fprintf(f, "{\"corpus_table_version\":%u,"
                        "\"generator\":\"topos_scalable_corpus\","
                        "\"git_commit\":\"%s\","
                        "\"id\":\"%s\",\"plane_bytes\":%zu,"
                        "\"v2_packet_bytes\":%u,\"v2_psnr_y\":%.4f}\n",
                        CORPUS_TABLE_VERSION, TOPOS_GIT_COMMIT, item->id,
                        plane_bytes, packet_size, psnr_y);
                fclose(f);
            }
        }
    }
    free(packet);
    free(dy);
    free(du);
    free(dv);
    free(da);
    free(packed);
    free(y);
    free(u);
    free(v);
    free(a);
    return failed ? 1 : 0;
}

int main(int argc, char** argv)
{
    const char* item_id = NULL;
    const char* outdir = NULL;
    int want_all = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--list") == 0) {
            printf("{\"corpus_table_version\":%u,\"generator\":"
                   "\"topos_scalable_corpus\",\"git_commit\":\"%s\","
                   "\"items\":[", CORPUS_TABLE_VERSION, TOPOS_GIT_COMMIT);
            for (size_t k = 0u; k < CORPUS_ITEM_COUNT; ++k) {
                if (k != 0u) { putchar(','); }
                print_item_json(&kCorpusItems[k]);
            }
            puts("]}");
            return 0;
        } else if (strcmp(argv[i], "--item") == 0 && i + 1 < argc) {
            item_id = argv[++i];
        } else if (strcmp(argv[i], "--all") == 0) {
            want_all = 1;
        } else if (strcmp(argv[i], "--outdir") == 0 && i + 1 < argc) {
            outdir = argv[++i];
        } else {
            fprintf(stderr, "usage: topos_scalable_corpus --list | "
                    "(--item <id> | --all) --outdir <dir>\n");
            return 2;
        }
    }
    if (outdir == NULL || (item_id == NULL && !want_all)) {
        fprintf(stderr, "usage: topos_scalable_corpus --list | "
                "(--item <id> | --all) --outdir <dir>\n");
        return 2;
    }
    if (item_id != NULL) {
        const corpus_item* item = find_item(item_id);
        if (item == NULL) {
            fprintf(stderr, "unknown corpus item: %s\n", item_id);
            return 2;
        }
        return generate_item(item, outdir);
    }
    for (size_t k = 0u; k < CORPUS_ITEM_COUNT; ++k) {
        char dir[512];
        if (snprintf(dir, sizeof(dir), "%s/%s", outdir, kCorpusItems[k].id) < 0 ||
            generate_item(&kCorpusItems[k], dir) != 0) {
            return 1;
        }
    }
    return 0;
}
