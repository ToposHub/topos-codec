/* topos_rawgen —— 确定性 raw planar 帧生成（CLI 冒烟/基准用）。
 * 用法：topos_rawgen --width W --height H --frames N --kind 0..4 [--alpha] --out f.raw
 * 布局与 topos_encoder_cli 输入一致（Y|U|V[|A]，原生字节序 u16）。
 */
#include "../common/alloc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image_synth.h"

static void usage(void)
{
    fprintf(stderr, "usage: topos_rawgen --width W --height H --frames N "
            "[--kind 0..4] [--alpha] --out file.raw\n");
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
    uint32_t w = 0, h = 0, frames = 0, kind = 2u;
    int alpha = 0;
    const char* out_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) { parse_u32(argv[++i], &w); }
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) { parse_u32(argv[++i], &h); }
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) { parse_u32(argv[++i], &frames); }
        else if (strcmp(argv[i], "--kind") == 0 && i + 1 < argc) { parse_u32(argv[++i], &kind); }
        else if (strcmp(argv[i], "--alpha") == 0) { alpha = 1; }
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) { out_path = argv[++i]; }
        else { usage(); return 2; }
    }
    if (w == 0 || h == 0 || frames == 0 || out_path == NULL || kind >= TC_SYNTH_KIND_COUNT) {
        usage();
        return 2;
    }
    uint32_t cw = (w + 1u) / 2u;
    size_t y_n = (size_t)w * h;
    size_t uv_n = (size_t)cw * h;
    uint16_t* y = (uint16_t*)tc_alloc(y_n * 2u);
    uint16_t* u = (uint16_t*)tc_alloc(uv_n * 2u);
    uint16_t* v = (uint16_t*)tc_alloc(uv_n * 2u);
    uint16_t* a = alpha ? (uint16_t*)tc_alloc(y_n * 2u) : NULL;
    if (y == NULL || u == NULL || v == NULL || (alpha && a == NULL)) { return 1; }
    FILE* f = fopen(out_path, "wb");
    if (f == NULL) { return 1; }
    image_synth_cfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.width = w;
    sc.height = h;
    sc.kind = (tc_synth_kind)kind;
    for (uint32_t i = 0; i < frames; ++i) {
        sc.seed = 0x70536F746F527261ull + (uint64_t)i;
        image_synth_build(&sc, y, u, v, a);
        fwrite(y, 2u, y_n, f);
        fwrite(u, 2u, uv_n, f);
        fwrite(v, 2u, uv_n, f);
        if (alpha) { fwrite(a, 2u, y_n, f); }
    }
    fclose(f);
    tc_free(y);
    tc_free(u);
    tc_free(v);
    tc_free(a);
    printf("raw: %ux%u frames=%u alpha=%d -> %s\n", w, h, frames, alpha, out_path);
    return 0;
}
