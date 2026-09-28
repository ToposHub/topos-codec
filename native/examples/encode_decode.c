/* Topos Codec SDK 独立示例（阶段 10）：唯一依赖 include/ + lib/。
 *
 * 演示完整最小闭环：
 *   合成帧（Y/2U/2V + 受限近似 alpha）→ tc_frame_encode → tc_mux_* 写 .mov
 *   （真实文件 IO 回调）→ tc_movie_open/packet → tc_frame_decode → 校验。
 *
 * 构建：
 *   cc examples/encode_decode.c -Iinclude -Llib -ltopos_codec -o encode_decode
 * 运行：
 *   ./encode_decode [输出.mov 路径]
 *
 * 预期输出末行：sdk-example: PASS（任何失败路径非零退出并打印错误码）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "topos_codec.h"

#define W 320u
#define H 180u
#define CW 160u
#define N_FRAMES 24u

/* ---- 文件 IO 回调（SDK 集成方照抄即可） ---- */

static int32_t file_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
    FILE* f = (FILE*)ctx;
    if (len == 0u) { return TC_OK; }
    if (fseek(f, (long)offset, SEEK_SET) != 0) { return TC_ERR_IO; }
    return fread(buf, 1u, len, f) == len ? TC_OK : TC_ERR_IO;
}

static int32_t file_write(void* ctx, const void* data, size_t len)
{
    FILE* f = (FILE*)ctx;
    return fwrite(data, 1u, len, f) == len ? TC_OK : TC_ERR_IO;
}

static int32_t file_seek_write(void* ctx, uint64_t offset, const void* data, size_t len)
{
    FILE* f = (FILE*)ctx;
    if (fseek(f, (long)offset, SEEK_SET) != 0) { return TC_ERR_IO; }
    return fwrite(data, 1u, len, f) == len ? TC_OK : TC_ERR_IO;
}

static uint64_t file_length(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) { return 0u; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n > 0 ? (uint64_t)n : 0u;
}

static void fill_frame(uint32_t frame_i,
                       uint16_t* y, uint16_t* u, uint16_t* v, uint16_t* a)
{
    for (uint32_t r = 0u; r < H; ++r) {
        for (uint32_t c = 0u; c < W; ++c) {
            y[r * W + c] = (uint16_t)((r * 4u + c + frame_i * 7u) % 1024u);
            a[r * W + c] = (uint16_t)((c * 65535u) / (W - 1u));
        }
        for (uint32_t c = 0u; c < CW; ++c) {
            u[r * CW + c] = (uint16_t)((c * 1023u) / (CW - 1u));
            v[r * CW + c] = (uint16_t)((r * 1023u) / (H - 1u));
        }
    }
}

static void die(const char* what, int32_t rc)
{
    fprintf(stderr, "sdk-example: FAIL %s rc=%d (%s)\n", what, rc, tc_last_error());
    exit(1);
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "topos_sdk_demo.mov";
    remove(path);

    /* 能力协商（可选但推荐：集成方先问再编） */
    if (tc_query_support(3u, 0u, 10u, 2u) != TC_OK) { die("query_support", -1); }

    /* ---- 编码配置（frame 级与 movie 级字段必须一致：tpcC 规则） ---- */
    topos_frame_config fc;
    memset(&fc, 0, sizeof fc);
    fc.struct_size = (uint32_t)sizeof(fc);
    fc.visible_width = (uint16_t)W;
    fc.visible_height = (uint16_t)H;
    fc.qp_base = 28u;
    fc.qmatrix_id = 1u;
    fc.alpha_mode = 2u;        /* 受限近似 alpha */
    fc.alpha_bit_depth = 12u;
    fc.color_range = 1u;       /* full / bt709 显式给出 */
    fc.color_primaries = 1u;
    fc.color_transfer = 1u;
    fc.color_matrix = 1u;

    uint16_t* y = malloc((size_t)W * H * 2u);
    uint16_t* u = malloc((size_t)CW * H * 2u);
    uint16_t* v = malloc((size_t)CW * H * 2u);
    uint16_t* a = malloc((size_t)W * H * 2u);
    if (!y || !u || !v || !a) { return 1; }
    topos_frame_input in;
    memset(&in, 0, sizeof in);
    in.struct_size = (uint32_t)sizeof(in);
    in.planes[0] = y;
    in.planes[1] = u;
    in.planes[2] = v;
    in.planes[3] = a;

    size_t cap = tc_frame_packet_bound(&fc);
    uint8_t* pkt = malloc(cap);
    if (!pkt || cap == 0u) { return 1; }

    /* ---- mux：文件 sink ---- */
    topos_movie_config mc;
    memset(&mc, 0, sizeof mc);
    mc.struct_size = (uint32_t)sizeof(mc);
    mc.abi_version = TOPOS_CODEC_ABI_VERSION;
    mc.visible_width = (uint16_t)W;
    mc.visible_height = (uint16_t)H;
    mc.profile = 3u;
    mc.pixel_format = 0u;
    mc.bit_depth = 10u;
    mc.qp_base = 28u;
    mc.qmatrix_id = 1u;
    mc.alpha_mode = 2u;
    mc.alpha_bit_depth = 12u;
    mc.color_range = 1u;
    mc.color_primaries = 1u;
    mc.color_transfer = 1u;
    mc.color_matrix = 1u;
    mc.timescale = 24000u;

    FILE* sink = fopen(path, "wb");
    if (sink == NULL) { perror("fopen"); return 1; }
    topos_io sink_io;
    memset(&sink_io, 0, sizeof sink_io);
    sink_io.struct_size = (uint32_t)sizeof(sink_io);
    sink_io.abi_version = TOPOS_CODEC_ABI_VERSION;
    sink_io.ctx = sink;
    sink_io.write = file_write;
    sink_io.seek_write = file_seek_write;

    topos_mux* mux = NULL;
    int32_t rc = tc_mux_create(&mc, &sink_io, &mux);
    if (rc != TC_OK) { die("mux_create", rc); }

    uint64_t total_payload = 0u;
    for (uint32_t i = 0u; i < N_FRAMES; ++i) {
        fill_frame(i, y, u, v, a);
        topos_frame_stats st;
        rc = tc_frame_encode(&fc, &in, pkt, cap, &st);
        if (rc != TC_OK) { die("frame_encode", rc); }
        rc = tc_mux_add_packet(mux, pkt, st.packet_size, (uint64_t)i * 1000u, 1000u);
        if (rc != TC_OK) { die("mux_add_packet", rc); }
        total_payload += st.packet_size;
        if (i == 0u && st.alpha_max_abs_error > 16u) {
            fprintf(stderr, "sdk-example: FAIL alpha 误差界 %u > 16\n",
                    (unsigned)st.alpha_max_abs_error);
            return 1;
        }
    }
    rc = tc_mux_finish(mux);
    if (rc != TC_OK) { die("mux_finish", rc); }
    tc_mux_free(mux);
    fclose(sink);

    /* ---- demux + 解码首帧 ---- */
    FILE* srcf = fopen(path, "rb");
    if (srcf == NULL) { perror("fopen rb"); return 1; }
    topos_io src_io;
    memset(&src_io, 0, sizeof src_io);
    src_io.struct_size = (uint32_t)sizeof(src_io);
    src_io.abi_version = TOPOS_CODEC_ABI_VERSION;
    src_io.ctx = srcf;
    src_io.read = file_read;
    src_io.length = file_length(path);

    topos_movie* mov = NULL;
    rc = tc_movie_open(&src_io, &mov);
    if (rc != TC_OK) { die("movie_open", rc); }
    topos_movie_info mi;
    memset(&mi, 0, sizeof mi);
    mi.struct_size = (uint32_t)sizeof(mi);
    mi.abi_version = TOPOS_CODEC_ABI_VERSION;
    rc = tc_movie_info(mov, &mi);
    if (rc != TC_OK) { die("movie_info", rc); }
    if (mi.sample_count != N_FRAMES) { die("sample_count", -1); }

    size_t need = 0;
    rc = tc_movie_packet(mov, 0u, NULL, 0u, &need);
    if (rc != TC_ERR_BUFFER_TOO_SMALL) { die("packet probe", rc); }
    rc = tc_movie_packet(mov, 0u, pkt, cap, NULL);
    if (rc != TC_OK) { die("packet read", rc); }

    topos_frame_output info;
    rc = tc_frame_decode(pkt, need, NULL, NULL, &info);
    if (rc != TC_OK) { die("decode probe", rc); }
    uint16_t* out[4] = { NULL, NULL, NULL, NULL };
    for (uint32_t p = 0u; p < info.plane_count; ++p) {
        uint32_t w = 0u, h = 0u;
        if (tc_frame_plane_geometry(&info, p, &w, &h) != TC_OK) { die("geometry", -1); }
        out[p] = malloc((size_t)w * h * 2u);
        if (out[p] == NULL) { return 1; }
    }
    rc = tc_frame_decode(pkt, need, out, NULL, &info);
    if (rc != TC_OK || info.concealed_slices != 0u) { die("decode", rc); }

    /* alpha 受限近似（bd12）：误差必须落在 << 2^(16-12) 的界内（体面 sanity） */
    uint32_t alpha_max_diff = 0u;
    for (uint32_t i = 0u; i < W * H; ++i) {
        uint32_t d = a[i] > out[3][i] ? a[i] - out[3][i] : out[3][i] - a[i];
        if (d > alpha_max_diff) { alpha_max_diff = d; }
    }
    if (alpha_max_diff > 64u) {
        fprintf(stderr, "sdk-example: FAIL alpha 解码误差 %u\n", alpha_max_diff);
        return 1;
    }

    tc_movie_close(mov);
    fclose(srcf);

    printf("sdk-example: %u 帧 -> %s（%.1f KiB，均 %.1f KiB/帧，alpha max diff %u）\n",
           (unsigned)N_FRAMES, path, (double)file_length(path) / 1024.0,
           (double)total_payload / 1024.0 / N_FRAMES, alpha_max_diff);
    printf("sdk-example: PASS\n");
    return 0;
}
