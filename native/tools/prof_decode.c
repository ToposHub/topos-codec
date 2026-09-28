/* prof_decode —— 解码热路径采样/计时基座（解码深化批次，dev 工具）。
 *
 * 读单个 .tpc 帧 packet 文件，循环 tc_frame_decode（平面预分配）。
 * 用法：prof_decode <frame.tpc> <iters>
 * 配合 sample/xctrace 或 TOPOS_CODEC_PROFILE=1 使用；非产品代码。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "topos_codec.h"

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: prof_decode <frame.tpc> <iters>\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (f == NULL) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* pkt = (uint8_t*)malloc((size_t)n);
    if (fread(pkt, 1, (size_t)n, f) != (size_t)n) { return 1; }
    fclose(f);

    topos_frame_output info;
    int32_t rc = tc_frame_decode(pkt, (size_t)n, NULL, NULL, &info);
    if (rc != TC_OK) { fprintf(stderr, "query rc=%d (%s)\n", rc, tc_last_error()); return 1; }
    uint16_t* planes[TC_FRAME_MAX_PLANES];
    for (uint32_t p = 0u; p < info.plane_count; ++p) {
        uint32_t w, h;
        (void)tc_frame_plane_geometry(&info, p, &w, &h);
        planes[p] = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
    }

    long iters = atol(argv[2]);
    for (long i = 0; i < iters; ++i) {
        rc = tc_frame_decode(pkt, (size_t)n, planes, NULL, &info);
        if (rc != TC_OK && rc != TC_WARN_CONCEALED) {
            fprintf(stderr, "decode rc=%d\n", rc);
            return 1;
        }
    }
    printf("ok %ld iters, %ux%u\n", iters, (unsigned)info.visible_width,
           (unsigned)info.visible_height);
    return 0;
}
