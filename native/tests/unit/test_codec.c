/* codec（阶段 4）：配置校验、往返、确定性、concealment、失败路径、多代、统计、码率调节 */
#include "bitstream/packet.h"
#include "bitstream/slice_codec.h"
#include "bitstream/slice_map.h"
#include "codec/codec.h"
#include "codec/color_store.h"
#include "common/crc32.h"
#include "common/endian.h"
#include "image_synth.h"
#include "mini_test.h"
#include "packet_synth.h"
#include "simd/dispatch.h"
#include "topos_codec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 辅助 ---------- */

static void base_cfg(topos_frame_config* c, uint32_t w, uint32_t h)
{
    memset(c, 0, sizeof(*c));
    c->struct_size = (uint32_t)sizeof(topos_frame_config);
    c->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    c->visible_width = (uint16_t)w;
    c->visible_height = (uint16_t)h;
    /* H1（2026-09-21）：cfg 默认 transfer 已改派 sRGB(13)（minor≥7 扩展
     * 代）——本用例钉住 legacy bt709(1)，保持 minor 枚举语义与 tpcC
     * （mc.color_transfer=1）逐项一致；新默认由 golden/pytest 双轨覆盖 */
    c->color_transfer = 1u;
}

static void fill_input(topos_frame_input* in, const uint16_t* pl[4])
{
    memset(in, 0, sizeof(*in));
    in->struct_size = (uint32_t)sizeof(topos_frame_input);
    in->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    for (int i = 0; i < 4; ++i) { in->planes[i] = pl[i]; }
}

static int32_t encode_to_buf(const topos_frame_config* cfg, const topos_frame_input* in,
                             uint8_t** out, size_t* size, topos_frame_stats* stats)
{
    *out = NULL;
    *size = 0;
    topos_frame_stats st;
    memset(&st, 0, sizeof(st));
    size_t cap = tc_frame_packet_bound(cfg);
    uint8_t* buf = (uint8_t*)malloc(cap != 0u ? cap : 1u);
    if (buf == NULL) { return TC_ERR_OUT_OF_MEMORY; }
    int32_t rc = tc_frame_encode(cfg, in, buf, cap, &st);
    if (rc == TC_OK) {
        *out = buf;
        *size = st.packet_size;
        if (stats != NULL) { *stats = st; }
    } else {
        free(buf);
    }
    return rc;
}

/* M1 回归测试用：只做符号层校验、不重建的空 sink */
static int32_t noop_block_sink(void* ctx, uint32_t idx, const int32_t q_natural[64],
                               uint32_t ac_rowmask)
{
    (void)ctx; (void)idx; (void)q_natural; (void)ac_rowmask;
    return TC_OK;
}

static int32_t decode_to_planes(const uint8_t* data, size_t size, topos_frame_output* info,
                                uint16_t* planes[4])
{    for (int i = 0; i < 4; ++i) { planes[i] = NULL; }
    int32_t rc = tc_frame_decode(data, size, NULL, NULL, info);
    if (rc != TC_OK) { return rc; }
    for (uint32_t p = 0u; p < info->plane_count; ++p) {
        uint32_t w = 0, h = 0;
        if (tc_frame_plane_geometry(info, p, &w, &h) != TC_OK) { return TC_ERR_MALFORMED; }
        planes[p] = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
        if (planes[p] == NULL) {
            for (int i = 0; i < 4; ++i) { free(planes[i]); planes[i] = NULL; }
            return TC_ERR_OUT_OF_MEMORY;
        }
    }
    rc = tc_frame_decode(data, size, planes, NULL, info);
    if (rc != TC_OK && rc != TC_WARN_CONCEALED) {
        for (int i = 0; i < 4; ++i) { free(planes[i]); planes[i] = NULL; }
    }
    return rc;
}

static void free_planes(uint16_t* planes[4])
{
    for (int i = 0; i < 4; ++i) { free(planes[i]); planes[i] = NULL; }
}

static int max_abs_diff(const uint16_t* a, const uint16_t* b, size_t n)
{
    int m = 0;
    for (size_t i = 0; i < n; ++i) {
        int d = (int)a[i] - (int)b[i];
        if (d < 0) { d = -d; }
        if (d > m) { m = d; }
    }
    return m;
}

static double mean_abs_diff(const uint16_t* a, const uint16_t* b, size_t n)
{
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double d = (double)a[i] - (double)b[i];
        acc += d < 0.0 ? -d : d;
    }
    return n != 0u ? acc / (double)n : 0.0;
}

/* RD0-02：reduced/scaled 输出的越界哨兵。stride 刻意比可见宽多 3 个
 * uint16，既覆盖 tight 之外的行距契约，也能抓住按源坐标直写目标面的回归。 */
typedef struct reduced_guard_plane {
    uint16_t* storage;
    uint16_t* pixels;
    uint32_t width;
    uint32_t height;
    size_t stride;
    size_t rows_bytes;
} reduced_guard_plane;

static int reduced_guard_alloc(const topos_frame_output* info, uint32_t plane,
                               reduced_guard_plane* out)
{
    memset(out, 0, sizeof(*out));
    if (tc_frame_plane_geometry(info, plane, &out->width, &out->height) != TC_OK) {
        return 0;
    }
    out->stride = (size_t)out->width + 3u;
    out->rows_bytes = out->stride * (size_t)out->height * sizeof(uint16_t);
    const size_t guard = 8u;
    out->storage = (uint16_t*)malloc(out->rows_bytes + guard * 2u * sizeof(uint16_t));
    if (out->storage == NULL) { return 0; }
    for (size_t i = 0u; i < out->rows_bytes / sizeof(uint16_t) + guard * 2u; ++i) {
        out->storage[i] = 0xA55Au;
    }
    out->pixels = out->storage + guard;
    return 1;
}

static void reduced_guard_free(reduced_guard_plane* plane)
{
    free(plane->storage);
    memset(plane, 0, sizeof(*plane));
}

static void reduced_guard_check(const reduced_guard_plane* plane)
{
    const size_t guard = 8u;
    for (size_t i = 0u; i < guard; ++i) {
        MT_CHECK_EQ_U64(plane->storage[i], 0xA55Au);
        MT_CHECK_EQ_U64(plane->storage[guard + plane->rows_bytes / sizeof(uint16_t) + i],
                        0xA55Au);
    }
    for (uint32_t y = 0u; y < plane->height; ++y) {
        for (size_t x = plane->width; x < plane->stride; ++x) {
            MT_CHECK_EQ_U64(plane->pixels[(size_t)y * plane->stride + x], 0xA55Au);
        }
    }
}

static void reduced_guard_compare(const reduced_guard_plane* a,
                                  const reduced_guard_plane* b)
{
    MT_CHECK_EQ_U64(a->width, b->width);
    MT_CHECK_EQ_U64(a->height, b->height);
    for (uint32_t y = 0u; y < a->height && y < b->height; ++y) {
        for (uint32_t x = 0u; x < a->width && x < b->width; ++x) {
            MT_CHECK_EQ_U64(a->pixels[(size_t)y * a->stride + x],
                            b->pixels[(size_t)y * b->stride + x]);
        }
    }
}

/* RD3-03：固定 1/3 相位表的 golden。21×15 → 7×5 同时覆盖 exact-divisible
 * 几何、奇数源尺寸、8×8 右/下边缘和连续目标行写回；两种存储类型都必须
 * 与 sx=3*tx、sy=3*ty 的参考映射逐位一致。 */
static void test_third_store_golden(void)
{
    enum { SRC_W = 21, SRC_H = 15, DST_W = 7, DST_H = 5, STRIDE = 10 };
    uint16_t i32_storage[DST_H * STRIDE];
    uint16_t u16_storage[DST_H * STRIDE];
    for (size_t i = 0u; i < sizeof(i32_storage) / sizeof(i32_storage[0]); ++i) {
        i32_storage[i] = 0xA55Au;
        u16_storage[i] = 0xA55Au;
    }
    tc_color_store_ctx c;
    memset(&c, 0, sizeof(c));
    c.dst = i32_storage;
    c.stride = STRIDE;
    c.vis_w = SRC_W;
    c.vis_h = SRC_H;
    c.dst_w = DST_W;
    c.dst_h = DST_H;
    c.scaled = 1u;
    c.max = 65535u;
    MT_CHECK(tc_color_store_exact_third(&c) != 0);

    for (uint32_t by = 0u; by < 3u; ++by) {
        for (uint32_t bx = 0u; bx < 3u; ++bx) {
            int32_t xh[64];
            uint16_t xh_u16[64];
            for (uint32_t y = 0u; y < 8u; ++y) {
                for (uint32_t x = 0u; x < 8u; ++x) {
                    const uint32_t value = (by * 8u + y) * 100u + bx * 8u + x;
                    xh[y * 8u + x] = (int32_t)value;
                    xh_u16[y * 8u + x] = (uint16_t)value;
                }
            }
            tc_color_store_scaled_from_xh(&c, bx, by, xh);

            c.dst = u16_storage;
            uint8_t xs[9], ys[9];
            uint32_t tx0 = 0u, ty0 = 0u, nx = 0u, ny = 0u;
            const uint32_t n = tc_color_store_third_coords(&c, bx, by, xs, ys,
                                                           &tx0, &ty0, &nx, &ny);
            uint16_t values[9];
            for (uint32_t i = 0u; i < n; ++i) {
                values[i] = xh_u16[(uint32_t)ys[i] * 8u + xs[i]];
            }
            tc_color_store_third_u16(&c, tx0, ty0, nx, ny, values);
            c.dst = i32_storage;
        }
    }
    for (uint32_t ty = 0u; ty < DST_H; ++ty) {
        for (uint32_t tx = 0u; tx < DST_W; ++tx) {
            const uint32_t sx = tx * 3u;
            const uint32_t sy = ty * 3u;
            const uint32_t expected = sy * 100u + sx;
            MT_CHECK_EQ_U64(i32_storage[ty * STRIDE + tx], expected);
            MT_CHECK_EQ_U64(u16_storage[ty * STRIDE + tx], expected);
        }
        for (uint32_t tx = DST_W; tx < STRIDE; ++tx) {
            MT_CHECK_EQ_U64(i32_storage[ty * STRIDE + tx], 0xA55Au);
            MT_CHECK_EQ_U64(u16_storage[ty * STRIDE + tx], 0xA55Au);
        }
    }
}

/* 4K→2K 固定 1/2 相位表的 golden。18×14 → 9×7 覆盖非 8 对齐的右/下边缘，
 * 同时验证通用 scaled_from_xh 的快路径与 sx=2*tx、sy=2*ty 参考映射一致。 */
static void test_half_store_golden(void)
{
    enum { SRC_W = 18, SRC_H = 14, DST_W = 9, DST_H = 7, STRIDE = 12 };
    uint16_t storage[DST_H * STRIDE];
    for (size_t i = 0u; i < sizeof(storage) / sizeof(storage[0]); ++i) {
        storage[i] = 0xA55Au;
    }
    tc_color_store_ctx c;
    memset(&c, 0, sizeof(c));
    c.dst = storage;
    c.stride = STRIDE;
    c.vis_w = SRC_W;
    c.vis_h = SRC_H;
    c.dst_w = DST_W;
    c.dst_h = DST_H;
    c.scaled = 1u;
    c.max = 65535u;
    MT_CHECK(tc_color_store_exact_half(&c) != 0);

    for (uint32_t by = 0u; by < 2u; ++by) {
        for (uint32_t bx = 0u; bx < 3u; ++bx) {
            int32_t xh[64];
            for (uint32_t y = 0u; y < 8u; ++y) {
                for (uint32_t x = 0u; x < 8u; ++x) {
                    xh[y * 8u + x] = (int32_t)(
                        (by * 8u + y) * 100u + bx * 8u + x);
                }
            }
            tc_color_store_scaled_from_xh(&c, bx, by, xh);
        }
    }
    for (uint32_t ty = 0u; ty < DST_H; ++ty) {
        for (uint32_t tx = 0u; tx < DST_W; ++tx) {
            const uint32_t sx = tx * 2u;
            const uint32_t sy = ty * 2u;
            const uint32_t expected = sy * 100u + sx;
            MT_CHECK_EQ_U64(storage[ty * STRIDE + tx], expected);
        }
        for (uint32_t tx = DST_W; tx < STRIDE; ++tx) {
            MT_CHECK_EQ_U64(storage[ty * STRIDE + tx], 0xA55Au);
        }
    }
}

static const packet_synth_cfg* reduced_matrix_cfgs(size_t* count)
{
    static const packet_synth_cfg cfgs[] = {
        /* 4:2:2 v1.0，非 8 倍数 visible 尺寸 */
        { 0xD001000000000001ull, 37u, 23u, 0u, 2u, 18u, 0u, 10u, 0u, 3u, 0u },
        /* 4:4:4 v1.3，奇数 visible 尺寸 */
        { 0xD001000000000002ull, 35u, 21u, 0u, 2u, 24u, 0u, 10u, 1u, 5u, 0u },
        /* GBR 4:4:4 12-bit Extreme v1.4，覆盖 qmatrix=2/plane geometry */
        { 0xD001000000000003ull, 35u, 21u, 0u, 2u, 24u, 2u, 12u, 2u, 6u, 0u },
        /* 4:4:4 + lossless alpha */
        { 0xD001000000000004ull, 35u, 21u, 1u, 2u, 12u, 0u, 10u, 1u, 5u, 0u },
        /* V2+VLC 4:2:2 与 V2+VLC 4:4:4 各覆盖一次 */
        { 0xD001000000000005ull, 37u, 23u, 0u, 2u, 18u, 0u, 10u, 0u, 3u, 1u },
        { 0xD001000000000006ull, 35u, 21u, 0u, 2u, 24u, 0u, 10u, 1u, 5u, 1u }
    };
    *count = sizeof(cfgs) / sizeof(cfgs[0]);
    return cfgs;
}

int main(void)
{
    test_third_store_golden();
    test_half_store_golden();

    /* ---------- 配置校验错误路径 ---------- */
    {
        topos_frame_config c;
        base_cfg(&c, 64u, 48u);

        MT_CHECK_EQ_I64(tc_frame_config_validate(NULL), TC_ERR_INVALID_ARGUMENT);
        topos_frame_config bad = c;
        bad.struct_size = 4u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = c; bad.abi_version = 99u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = c; bad.visible_width = 0u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = c; bad.visible_width = 16385u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = c; bad.visible_width = 16384u; bad.visible_height = 16u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_OK);
        bad = c; bad.qp_base = 64u;   /* v1.5（ADR-C031）：64..95 合法粗量化域 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_OK);
        bad = c; bad.qp_base = 96u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = c; bad.alpha_mode = 3u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = c; bad.alpha_mode = 1u; bad.alpha_bit_depth = 10u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = c; bad.alpha_mode = 2u; bad.alpha_bit_depth = 16u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = c; bad.profile = 4u; /* v1 仅 Standard */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_UNSUPPORTED_PROFILE);
        bad = c; bad.slice_rows = 1u; bad.visible_height = 4096u; /* 512 带 × 3 平面 > 512 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_LIMIT_EXCEEDED);
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK); /* 全 0 字段 = 合法最小配置 */
    }

    /* ---------- M2：持久 decoder context 与 tc_frame_decode 等价 ----------
     * 96×66（高非 8 对齐 → clip 路径）+ alpha → alpha 行缓冲池跨帧复用。 */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x0DEC0DEC0DEC0Dull;
        ic.width = 96u;
        ic.height = 66u;
        ic.kind = TC_SYNTH_MIXED;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 1, &y, &u, &v, &a), 0);
        const uint16_t* pl[4] = {y, u, v, a};
        topos_frame_input in;
        fill_input(&in, pl);
        topos_frame_config c;
        base_cfg(&c, 96u, 66u);
        c.alpha_mode = 1u;
        c.alpha_bit_depth = 16u;
        c.qp_base = 24u;

        uint8_t* pkt = NULL;
        size_t size = 0;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);

        topos_frame_output clean;
        uint16_t* ref[4];
        MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &clean, ref), TC_OK);

        tc_decoder* dec = NULL;
        MT_CHECK_EQ_I64(tc_decoder_create(NULL, &dec), TC_OK);
        MT_CHECK(dec != NULL);

        topos_frame_output pinfo;
        MT_CHECK_EQ_I64(tc_decoder_prepare(dec, pkt, size, &pinfo), TC_OK);
        MT_CHECK_EQ_U64(pinfo.plane_count, clean.plane_count);
        MT_CHECK_EQ_U64(pinfo.slice_count, clean.slice_count);

        topos_plane_view views[TC_FRAME_MAX_PLANES];
        memset(views, 0, sizeof(views));
        uint16_t* ctx_planes[4] = {NULL, NULL, NULL, NULL};
        uint32_t plane_w[4] = {0u, 0u, 0u, 0u};
        uint32_t plane_h[4] = {0u, 0u, 0u, 0u};
        for (uint32_t p = 0u; p < pinfo.plane_count; ++p) {
            uint32_t w = 0u, h = 0u;
            MT_CHECK_EQ_I64(tc_frame_plane_geometry(&pinfo, p, &w, &h), TC_OK);
            plane_w[p] = w;
            plane_h[p] = h;
            ctx_planes[p] = (uint16_t*)malloc((size_t)w * (size_t)h * sizeof(uint16_t));
            views[p].struct_size = (uint32_t)sizeof(topos_plane_view);
            views[p].abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
            views[p].pixels = ctx_planes[p];
            views[p].stride = 0u;
        }

        /* 三次重复解码：context 池跨帧复用，输出与 legacy bit-exact 一致 */
        for (int rep = 0; rep < 3; ++rep) {
            topos_frame_output dinfo;
            MT_CHECK_EQ_I64(tc_decoder_decode(dec, pkt, size, views, &dinfo), TC_OK);
            MT_CHECK_EQ_U64(dinfo.concealed_slices, 0u);
            for (uint32_t p = 0u; p < dinfo.plane_count; ++p) {
                MT_CHECK(memcmp(ctx_planes[p], ref[p],
                                (size_t)plane_w[p] * plane_h[p] * sizeof(uint16_t)) == 0);
            }
        }

        /* RD1-02：context request 直接分配/写入 reduced 目标面，不经过完整源面。 */
        topos_decode_request reduced_request;
        memset(&reduced_request, 0, sizeof(reduced_request));
        reduced_request.struct_size = (uint32_t)sizeof(reduced_request);
        reduced_request.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        reduced_request.mode = TC_DECODE_MODE_REDUCED;
        reduced_request.scale = TC_DECODE_SCALE_HALF;
        reduced_request.memory_type = TC_DECODE_MEMORY_CPU;
        topos_frame_output reduced_info;
        MT_CHECK_EQ_I64(tc_decoder_prepare_request(dec, pkt, size, &reduced_request,
                                                   &reduced_info), TC_OK);
        MT_CHECK_EQ_U64(reduced_info.visible_width, 48u);
        MT_CHECK_EQ_U64(reduced_info.visible_height, 33u);
        topos_plane_view reduced_views[TC_FRAME_MAX_PLANES];
        uint16_t* reduced_planes[TC_FRAME_MAX_PLANES] = {NULL, NULL, NULL, NULL};
        memset(reduced_views, 0, sizeof(reduced_views));
        for (uint32_t p = 0u; p < reduced_info.plane_count; ++p) {
            uint32_t w = 0u, h = 0u;
            MT_CHECK_EQ_I64(tc_frame_plane_geometry(&reduced_info, p, &w, &h), TC_OK);
            reduced_planes[p] = (uint16_t*)calloc((size_t)w * h, sizeof(uint16_t));
            MT_CHECK(reduced_planes[p] != NULL);
            reduced_views[p].struct_size = (uint32_t)sizeof(topos_plane_view);
            reduced_views[p].abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
            reduced_views[p].pixels = reduced_planes[p];
        }
        topos_frame_output reduced_decoded;
        MT_CHECK_EQ_I64(tc_decoder_decode_request(dec, pkt, size, &reduced_request,
                                                  reduced_views, &reduced_decoded), TC_OK);
        MT_CHECK_EQ_U64(reduced_decoded.visible_width, reduced_info.visible_width);
        MT_CHECK_EQ_U64(reduced_decoded.visible_height, reduced_info.visible_height);
        topos_batch_packet request_batch[1] = {{pkt, size}};
        topos_frame_output request_batch_info[1];
        MT_CHECK_EQ_I64(tc_decoder_decode_batch_request(dec, request_batch, 1u,
                                                        &reduced_request, NULL,
                                                        request_batch_info), TC_OK);
        MT_CHECK_EQ_U64(request_batch_info[0].visible_width, reduced_info.visible_width);
        MT_CHECK_EQ_I64(tc_decoder_decode_batch_request(dec, request_batch, 1u,
                                                        &reduced_request, reduced_views,
                                                        request_batch_info), TC_OK);
        for (uint32_t p = 0u; p < reduced_info.plane_count; ++p) {
            uint32_t w = 0u, h = 0u;
            MT_CHECK_EQ_I64(tc_frame_plane_geometry(&reduced_info, p, &w, &h), TC_OK);
            MT_CHECK(w > 0u && h > 0u && reduced_planes[p] != NULL);
            free(reduced_planes[p]);
        }

        /* 坏 handle / 坏 view 元数据 → INVALID_ARGUMENT，不写输出 */
        MT_CHECK_EQ_I64(tc_decoder_decode(NULL, pkt, size, views, &pinfo),
                        TC_ERR_INVALID_ARGUMENT);
        {
            topos_plane_view bad = views[0];
            bad.struct_size = 4u;
            topos_plane_view vs[TC_FRAME_MAX_PLANES];
            memcpy(vs, views, sizeof(views));
            vs[0] = bad;
            topos_frame_output dinfo;
            MT_CHECK_EQ_I64(tc_decoder_decode(dec, pkt, size, vs, &dinfo),
                            TC_ERR_INVALID_ARGUMENT);
        }
        /* truncated packet → 整帧拒绝（截断/结构错误码） */
        {
            topos_frame_output dinfo;
            MT_CHECK(tc_decoder_decode(dec, pkt, size - 1u, views, &dinfo) < 0);
        }

        tc_decoder_destroy(dec);
        for (int p = 0; p < 4; ++p) { free(ctx_planes[p]); }
        free_planes(ref);
        free(pkt);
        free(y); free(u); free(v); free(a);
    }

    /* ---------- 往返 + 确定性 + qp/qmatrix 扫描 ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0xC0DEC0DEC0DEC0DEull;
        ic.width = 64u;
        ic.height = 48u;
        ic.kind = TC_SYNTH_GRAIN;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);

        const uint16_t* pl[4] = {y, u, v, NULL};
        topos_frame_input in;
        fill_input(&in, pl);

        for (int qm = 0; qm <= 1; ++qm) {
            for (int qp = 0; qp <= 63; qp += 21) {
                topos_frame_config c;
                base_cfg(&c, 64u, 48u);
                c.qmatrix_id = (uint8_t)qm;
                c.qp_base = (uint8_t)qp;
                uint8_t* pkt = NULL;
                size_t size = 0;
                MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);
                /* 确定性：同输入同配置 → bit-exact */
                uint8_t* pkt2 = NULL;
                size_t size2 = 0;
                MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt2, &size2, NULL), TC_OK);
                MT_CHECK_EQ_U64(size, size2);
                MT_CHECK(memcmp(pkt, pkt2, size) == 0);
                free(pkt2);

                topos_frame_output info;
                uint16_t* dec[4];
                MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, dec), TC_OK);
                MT_CHECK_EQ_U64(info.concealed_slices, 0u);
                MT_CHECK_EQ_U64(info.plane_count, 3u);
                MT_CHECK_EQ_U64(info.visible_width, 64u);
                if (qp == 0) { /* Q=1：量化无损（仅逆变换舍入 ±1） */
                    MT_CHECK(max_abs_diff(y, dec[0], 64u * 48u) <= 1);
                    MT_CHECK(max_abs_diff(u, dec[1], 32u * 48u) <= 1);
                }
                free_planes(dec);
                free(pkt);
            }
        }
        free(y); free(u); free(v); free(a);
    }

    /* ---------- 奇数尺寸 + 无损 Alpha（mode 1，bit-exact 门禁） ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0xABCD12345678EF90ull;
        ic.width = 36u;
        ic.height = 20u;
        ic.kind = TC_SYNTH_MIXED;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 1, &y, &u, &v, &a), 0);

        topos_frame_config c;
        base_cfg(&c, 36u, 20u);
        c.alpha_mode = 1u;
        c.alpha_bit_depth = 16u;
        c.qp_base = 26u;
        const uint16_t* pl[4] = {y, u, v, a};
        topos_frame_input in;
        fill_input(&in, pl);

        uint8_t* pkt = NULL;
        size_t size = 0;
        topos_frame_stats st;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, &st), TC_OK);
        MT_CHECK_EQ_U64(st.alpha_max_abs_error, 0u);
        MT_CHECK_EQ_U64((uint64_t)st.color_payload_bytes + st.alpha_payload_bytes +
                        st.color_header_bytes + st.alpha_header_bytes, (uint64_t)st.packet_size);
        MT_CHECK_EQ_U64((uint64_t)st.packet_size, (uint64_t)size);

        topos_frame_output info;
        uint16_t* dec[4];
        MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, dec), TC_OK);
        MT_CHECK_EQ_U64(info.plane_count, 4u);
        MT_CHECK(max_abs_diff(a, dec[3], 36u * 20u) == 0); /* 无损 Alpha bit-exact */
        uint32_t aw = 0, ah = 0;
        MT_CHECK_EQ_I64(tc_frame_plane_geometry(&info, 0u, &aw, &ah), TC_OK);
        MT_CHECK_EQ_U64(aw, 36u); MT_CHECK_EQ_U64(ah, 20u);
        MT_CHECK_EQ_I64(tc_frame_plane_geometry(&info, 1u, &aw, &ah), TC_OK);
        MT_CHECK_EQ_U64(aw, 18u);
        MT_CHECK_EQ_I64(tc_frame_plane_geometry(&info, 3u, &aw, &ah), TC_OK);
        MT_CHECK_EQ_U64(aw, 36u);
        MT_CHECK_EQ_I64(tc_frame_plane_geometry(&info, 4u, &aw, &ah), TC_ERR_INVALID_ARGUMENT);
        free_planes(dec);

        /* 逐字节截断：任何截断点都是已定义负值错误码（不崩溃、无泄漏） */
        int32_t worst = TC_OK;
        for (size_t cut = 0; cut < size; ++cut) {
            topos_frame_output info2;
            int32_t rc = tc_frame_decode(pkt, cut, NULL, NULL, &info2);
            if (rc >= TC_OK) { worst = TC_OK + 1; break; }
            worst = rc;
        }
        MT_CHECK(worst < TC_OK);
        free(pkt);
        free(y); free(u); free(v); free(a);
    }

    /* ---------- 受限近似 Alpha（mode 2）：误差界 + 2^s 倍数不变量 ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x5EED5EED5EED5EEDull;
        ic.width = 48u;
        ic.height = 32u;
        ic.kind = TC_SYNTH_GRAIN;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 1, &y, &u, &v, &a), 0);

        topos_frame_config c;
        base_cfg(&c, 48u, 32u);
        c.alpha_mode = 2u;
        c.alpha_bit_depth = 12u; /* s = 4 */
        c.qp_base = 30u;
        const uint16_t* pl[4] = {y, u, v, a};
        topos_frame_input in;
        fill_input(&in, pl);

        uint8_t* pkt = NULL;
        size_t size = 0;
        topos_frame_stats st;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, &st), TC_OK);
        MT_CHECK(st.alpha_max_abs_error > 0u);
        MT_CHECK(st.alpha_max_abs_error <= 15u); /* ≤ 2^4 − 1 */

        topos_frame_output info;
        uint16_t* dec[4];
        MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, dec), TC_OK);
        size_t n = (size_t)48u * 32u;
        MT_CHECK(max_abs_diff(a, dec[3], n) <= 15);
        int all_multiple = 1;
        uint32_t top = 0xFFF0u;
        for (size_t i = 0; i < n; ++i) {
            if ((dec[3][i] & 0xFu) != 0u || dec[3][i] > top) { all_multiple = 0; }
        }
        MT_CHECK(all_multiple);
        /* 与规范量化公式逐值一致（round-half-up + 顶钳位） */
        int exact = 1;
        for (size_t i = 0; i < n; ++i) {
            uint32_t q = ((a[i] + 8u) >> 4) << 4;
            if (q > top) { q = top; }
            if (dec[3][i] != (uint16_t)q) { exact = 0; }
        }
        MT_CHECK(exact);
        free_planes(dec);
        free(pkt);
        free(y); free(u); free(v); free(a);
    }

    /* ---------- 输入 stride（非 tight）不影响输出 ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x1122334455667788ull;
        ic.width = 40u;
        ic.height = 24u;
        ic.kind = TC_SYNTH_GRADIENT;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);

        /* 重建带 stride 的输入（stride = w + 13，填充区写垃圾） */
        uint32_t cw = 20u;
        size_t sy = 40u + 13u, sc = cw + 13u;
        uint16_t* py = (uint16_t*)malloc(sy * 24u * 2u);
        uint16_t* pu = (uint16_t*)malloc(sc * 24u * 2u);
        uint16_t* pv = (uint16_t*)malloc(sc * 24u * 2u);
        for (uint32_t r = 0u; r < 24u; ++r) {
            for (uint32_t x = 0u; x < sy; ++x) {
                py[r * sy + x] = x < 40u ? y[r * 40u + x] : (uint16_t)0x5A5Au;
            }
            for (uint32_t x = 0u; x < sc; ++x) {
                pu[r * sc + x] = x < cw ? u[r * cw + x] : (uint16_t)0xA5A5u;
                pv[r * sc + x] = x < cw ? v[r * cw + x] : (uint16_t)0x1357u;
            }
        }
        topos_frame_config c;
        base_cfg(&c, 40u, 24u);
        c.qp_base = 18u;

        topos_frame_input tight, strided;
        const uint16_t* plt[4] = {y, u, v, NULL};
        fill_input(&tight, plt);
        const uint16_t* pls[4] = {py, pu, pv, NULL};
        fill_input(&strided, pls);
        strided.strides[0] = sy;
        strided.strides[1] = sc;
        strided.strides[2] = sc;

        uint8_t *p1 = NULL, *p2 = NULL;
        size_t s1 = 0, s2 = 0;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &tight, &p1, &s1, NULL), TC_OK);
        MT_CHECK_EQ_I64(encode_to_buf(&c, &strided, &p2, &s2, NULL), TC_OK);
        MT_CHECK_EQ_U64(s1, s2);
        MT_CHECK(memcmp(p1, p2, s1) == 0);
        free(p1); free(p2); free(py); free(pu); free(pv);
        free(y); free(u); free(v); free(a);
    }

    /* ---------- slice_rows 变体 + qp delta 生效 ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0xDEADBEEFCAFEBABEull;
        ic.width = 64u;
        ic.height = 48u;
        ic.kind = TC_SYNTH_DETAIL;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);
        const uint16_t* pl[4] = {y, u, v, NULL};
        topos_frame_input in;
        fill_input(&in, pl);

        for (uint32_t sr = 1u; sr <= 6u; sr += 5u) { /* 1（每带 1 块行）与 6（整平面单带） */
            topos_frame_config c;
            base_cfg(&c, 64u, 48u);
            c.slice_rows = (uint8_t)sr;
            c.qp_base = 10u;
            c.qp_delta_chroma = 8; /* U/V qp_eff = 18 */
            uint8_t* pkt = NULL;
            size_t size = 0;
            MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);

            topos_packet_view view;
            MT_CHECK_EQ_I64(tc_packet_scan(pkt, size, &view), TC_OK);
            for (uint32_t si = 0u; si < view.slice_count; ++si) {
                int32_t qe = tc_slice_effective_qp(view.fh.qp_base,
                                                   view.slices[si].qp_delta_biased);
                if (view.slices[si].plane == 0u) { MT_CHECK_EQ_I64(qe, 10); }
                else { MT_CHECK_EQ_I64(qe, 18); }
            }
            /* sr=6：48/8=6 块行 → 每平面 1 带 → 共 3 slice */
            if (sr == 6u) { MT_CHECK_EQ_U64(view.slice_count, 3u); }

            topos_frame_output info;
            uint16_t* dec[4];
            MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, dec), TC_OK);
            MT_CHECK(max_abs_diff(y, dec[0], 64u * 48u) <= 1); /* qp10 仍近乎无损 */
            free_planes(dec);
            free(pkt);
        }
        free(y); free(u); free(v); free(a);
    }

    /* ---------- M10-6.4 回归：非 8 对齐高 + alpha 多带末带 pad 行不得写穿平面 ----------
     * 316×180（coded 320×184）+ alpha mode2 + slice_rows=8：末带局部 pad 行
     * （全局 180..183）曾被「y < vis_h」误放行，直写越界平面末端（ASan
     * heap-buffer-overflow；紧 stride 下任何构建均可被哨兵捕获）。 */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0xC0FFEE1234567890ull;
        ic.width = 316u;
        ic.height = 180u;
        ic.kind = TC_SYNTH_DETAIL;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 1, &y, &u, &v, &a), 0);
        /* alpha 用 12-bit 对齐图案（mode2 bd12 域 = 16 的倍数）→ 无损往返可 memcmp */
        for (uint32_t i = 0u; i < 316u * 180u; ++i) {
            a[i] = (uint16_t)(((i * 11u) % 4096u) * 16u);
        }
        const uint16_t* pl[4] = {y, u, v, a};
        topos_frame_input in;
        fill_input(&in, pl);

        topos_frame_config c;
        base_cfg(&c, 316u, 180u);
        c.slice_rows = 8u;
        c.qp_base = 20u;
        c.alpha_mode = 2u;
        c.alpha_bit_depth = 12u;
        uint8_t* pkt = NULL;
        size_t size = 0;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);

        topos_packet_view view;
        MT_CHECK_EQ_I64(tc_packet_scan(pkt, size, &view), TC_OK);
        MT_CHECK(view.slice_count > 6u); /* 每平面 3 带 × 4 平面——确保多带 */

        topos_frame_output info;
        MT_CHECK_EQ_I64(tc_frame_decode(pkt, size, NULL, NULL, &info), TC_OK);
        uint16_t* dec[4] = {0};
        size_t plane_elems[4] = {0};
        static const uint16_t kGuard = 0x5AA5u;
        for (uint32_t p = 0u; p < info.plane_count; ++p) {
            uint32_t w = 0, h = 0;
            MT_CHECK_EQ_I64(tc_frame_plane_geometry(&info, p, &w, &h), TC_OK);
            plane_elems[p] = (size_t)w * h;
            dec[p] = (uint16_t*)malloc((plane_elems[p] + 8u) * sizeof(uint16_t));
            MT_CHECK(dec[p] != NULL);
            for (int g = 0; g < 8; ++g) { dec[p][plane_elems[p] + (size_t)g] = kGuard; }
        }
        MT_CHECK_EQ_I64(tc_frame_decode(pkt, size, dec, NULL, &info), TC_OK);
        MT_CHECK_EQ_U64(info.concealed_slices, 0u);
        for (uint32_t p = 0u; p < info.plane_count; ++p) {
            for (int g = 0; g < 8; ++g) {
                MT_CHECK_EQ_U64(dec[p][plane_elems[p] + (size_t)g], kGuard);
            }
        }
        /* alpha（mode2 无损）整平面与源一致——末可见行（179）正确落在平面内 */
        MT_CHECK(memcmp(dec[3], a, plane_elems[3] * sizeof(uint16_t)) == 0);
        for (uint32_t p = 0u; p < info.plane_count; ++p) { free(dec[p]); }
        free(y); free(u); free(v); free(a);
        free(pkt);
    }

    /* ---------- concealment：slice CRC 坏 / 熵流坏 → 仅该带中灰，帧交付 ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x1234567890ABCDEFull;
        ic.width = 64u;
        ic.height = 48u;
        ic.kind = TC_SYNTH_GRADIENT;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);
        const uint16_t* pl[4] = {y, u, v, NULL};
        topos_frame_input in;
        fill_input(&in, pl);

        topos_frame_config c;
        base_cfg(&c, 64u, 48u);
        c.slice_rows = 1u; /* 每块行一带：Y 6 带 */
        c.qp_base = 22u;
        uint8_t* pkt = NULL;
        size_t size = 0;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);

        topos_frame_output clean_info;
        uint16_t* clean[4];
        MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &clean_info, clean), TC_OK);

        topos_packet_view view;
        MT_CHECK_EQ_I64(tc_packet_scan(pkt, size, &view), TC_OK);
        MT_CHECK_EQ_U64(view.slice_count, 18u);

        /* 定位 Y 第 2 个 slice 的 payload 并翻转一个字节（CRC 必坏） */
        size_t off = TC_FRAME_HEADER_SIZE;
        for (uint32_t si = 0u; si < 2u; ++si) {
            off += TC_SLICE_HEADER_SIZE + view.slices[si].slice_payload_size;
        }
        off += TC_SLICE_HEADER_SIZE;
        uint8_t* corrupt = (uint8_t*)malloc(size);
        memcpy(corrupt, pkt, size);
        corrupt[off] ^= 0x40u;

        topos_frame_output info;
        uint16_t* dec[4];
        MT_CHECK_EQ_I64(decode_to_planes(corrupt, size, &info, dec), TC_WARN_CONCEALED);
        MT_CHECK_EQ_U64(info.concealed_slices, 1u);
        MT_CHECK_EQ_U64(info.slice_count, 18u);
        int status_ok = 1;
        for (uint32_t si = 0u; si < info.slice_count; ++si) {
            if (info.slice_status[si] != (si == 2u ? TC_FRAME_SLICE_CONCEALED
                                                  : TC_FRAME_SLICE_OK)) {
                status_ok = 0;
            }
        }
        MT_CHECK(status_ok);
        /* 带 2 = Y 像素行 [16,24)：中灰 512；带外与干净解码一致 */
        int band_mid = 1, outside_ok = 1;
        for (uint32_t r = 0u; r < 48u; ++r) {
            for (uint32_t x = 0u; x < 64u; ++x) {
                uint16_t got = dec[0][(size_t)r * 64u + x];
                if (r >= 16u && r < 24u) {
                    if (got != 512u) { band_mid = 0; }
                } else if (got != clean[0][(size_t)r * 64u + x]) {
                    outside_ok = 0;
                }
            }
        }
        MT_CHECK(band_mid);
        MT_CHECK(outside_ok);
        MT_CHECK(max_abs_diff(u, dec[1], 32u * 48u) <= 4); /* U 未受影响 */
        free_planes(dec);

        /* 熵流坏但 CRC 自洽：payload 清零 + 重算 CRC → MALFORMED → conceal */
        memset(corrupt + off, 0, (size_t)view.slices[2u].slice_payload_size);
        uint32_t crc = tc_crc32(corrupt + off, view.slices[2u].slice_payload_size);
        tc_store_be32(corrupt + off - (size_t)TC_SLICE_HEADER_SIZE + 13u, crc);
        MT_CHECK_EQ_I64(decode_to_planes(corrupt, size, &info, dec), TC_WARN_CONCEALED);
        MT_CHECK_EQ_U64(info.concealed_slices, 1u);
        free_planes(dec);
        free(corrupt);

        /* M1 回归：CRC 坏但 Rice 语法仍可完整消费 → 必须 conceal。
         * 旧实现 dec_slice_task 不检查 slice CRC，此类损坏曾被当作正常
         * slice 解码交付；构造方式：翻转某颜色 slice payload 末字节的
         * pad 位（未被熵流消费），符号层校验仍通过而 CRC 必坏。 */
        {
            uint8_t* c2 = (uint8_t*)malloc(size);
            int hit_slice = -1;
            for (uint32_t s2 = 0u; s2 < view.slice_count && hit_slice < 0; ++s2) {
                if (view.slices[s2].plane == 3u) { continue; } /* alpha 流另测 */
                size_t poff = TC_FRAME_HEADER_SIZE;
                for (uint32_t j = 0u; j < s2; ++j) {
                    poff += TC_SLICE_HEADER_SIZE + view.slices[j].slice_payload_size;
                }
                poff += TC_SLICE_HEADER_SIZE;
                size_t plen = view.slices[s2].slice_payload_size;
                if (plen == 0u) { continue; }
                for (uint32_t b = 0u; b < 8u && hit_slice < 0; ++b) {
                    memcpy(c2, pkt, size);
                    c2[poff + plen - 1u] ^= (uint8_t)(1u << b);
                    if (tc_color_slice_decode_stream(&view.fh, &view.slices[s2], c2 + poff,
                                                     plen, noop_block_sink, NULL,
                                                     NULL) == TC_OK) {
                        hit_slice = (int)s2;
                    }
                }
            }
            MT_CHECK(hit_slice >= 0); /* 18 slice 中应存在带 pad 位的颜色 slice */
            topos_frame_output info2;
            uint16_t* dec2[4];
            MT_CHECK_EQ_I64(decode_to_planes(c2, size, &info2, dec2), TC_WARN_CONCEALED);
            MT_CHECK_EQ_U64(info2.concealed_slices, 1u);
            MT_CHECK_EQ_U64(info2.slice_status[hit_slice], TC_FRAME_SLICE_CONCEALED);
            free_planes(dec2);
            free(c2);
        }

        /* 帧间隔离：损坏帧解码后再解干净帧 → 完全正常 */
        MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, dec), TC_OK);
        MT_CHECK(max_abs_diff(clean[0], dec[0], 64u * 48u) == 0);
        free_planes(dec);
        free_planes(clean);

        /* header CRC 坏 → 整帧拒绝 */
        uint8_t* hbad = (uint8_t*)malloc(size);
        memcpy(hbad, pkt, size);
        hbad[30] ^= 0x01u;
        MT_CHECK_EQ_I64(tc_frame_decode(hbad, size, NULL, NULL, &info),
                        TC_ERR_CHECKSUM_MISMATCH);
        free(hbad);
        free(pkt);
        free(y); free(u); free(v); free(a);
    }

    /* ---------- Alpha 带 concealment：填 65535 ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x0F0F0F0F0F0F0F0Full;
        ic.width = 32u;
        ic.height = 16u;
        ic.kind = TC_SYNTH_MIXED;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 1, &y, &u, &v, &a), 0);
        const uint16_t* pl[4] = {y, u, v, a};
        topos_frame_input in;
        fill_input(&in, pl);

        topos_frame_config c;
        base_cfg(&c, 32u, 16u);
        c.alpha_mode = 1u;
        c.alpha_bit_depth = 16u;
        c.slice_rows = 1u;
        c.qp_base = 20u;
        uint8_t* pkt = NULL;
        size_t size = 0;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);

        topos_packet_view view;
        MT_CHECK_EQ_I64(tc_packet_scan(pkt, size, &view), TC_OK);
        /* 找 alpha slice（plane 3）并破坏其 payload */
        size_t off = TC_FRAME_HEADER_SIZE;
        int target = -1;
        for (uint32_t si = 0u; si < view.slice_count; ++si) {
            if ((int)view.slices[si].plane == 3 && target < 0) { target = (int)si; }
            if (target < 0 || si < (uint32_t)target) {
                off += TC_SLICE_HEADER_SIZE + view.slices[si].slice_payload_size;
            }
        }
        off += TC_SLICE_HEADER_SIZE;
        pkt[off + 1u] ^= 0x11u;

        topos_frame_output info;
        uint16_t* dec[4];
        MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, dec), TC_WARN_CONCEALED);
        int fill_ok = 1;
        for (uint32_t x = 0u; x < 32u; ++x) {
            if (dec[3][x] != 65535u) { fill_ok = 0; } /* 第一带 = 前 8 行 */
        }
        MT_CHECK(fill_ok);
        free_planes(dec);
        free(pkt);
        free(y); free(u); free(v); free(a);
    }

    /* ---------- v1.5（ADR-C031）：qp 值域 64..95 粗量化档 ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x5131363551503031ull;
        ic.width = 64u;
        ic.height = 48u;
        ic.kind = TC_SYNTH_GRAIN;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);
        const uint16_t* pl[4] = {y, u, v, NULL};
        topos_frame_input in;
        fill_input(&in, pl);
        topos_frame_config c;
        base_cfg(&c, 64u, 48u);

        /* qp≤63：minor 仍 0，字节行为与 v1.0 完全一致（前向兼容锚点） */
        c.qp_base = 58u;
        uint8_t* pkt_legacy = NULL; size_t size_legacy = 0;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt_legacy, &size_legacy, NULL), TC_OK);
        topos_packet_view view_legacy;
        MT_CHECK_EQ_I64(tc_packet_scan(pkt_legacy, size_legacy, &view_legacy), TC_OK);
        MT_CHECK_EQ_U64(view_legacy.fh.version_minor, 0u);

        /* qp80/95：minor=4 写入、码率单调下降、解码往返成功 */
        uint64_t size_at_63 = 0u, size_at_80 = 0u, size_at_95 = 0u;
        const uint8_t v15_qps[3] = {63u, 80u, 95u};
        uint64_t* sizes[3] = {&size_at_63, &size_at_80, &size_at_95};
        for (int i = 0; i < 3; ++i) {
            c.qp_base = v15_qps[i];
            uint8_t* pkt = NULL; size_t sz = 0;
            MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &sz, NULL), TC_OK);
            topos_packet_view view;
            MT_CHECK_EQ_I64(tc_packet_scan(pkt, sz, &view), TC_OK);
            MT_CHECK_EQ_U64(view.fh.qp_base, v15_qps[i]);
            if (v15_qps[i] >= 64u) {
                MT_CHECK_EQ_U64(view.fh.version_minor, 4u);   /* v1.5 扩展代 */
            }
            topos_frame_output info;
            uint16_t* dec[4];
            MT_CHECK_EQ_I64(decode_to_planes(pkt, sz, &info, dec), TC_OK);
            free_planes(dec);
            *sizes[i] = (uint64_t)sz;
            free(pkt);
        }
        MT_CHECK(size_at_80 < size_at_63);   /* 粗量化单调缩码率 */
        MT_CHECK(size_at_95 < size_at_80);
        /* sized 搜索开域：Proxy 级目标在 qp≤63 不可达、64..95 可达 */
        size_t cap = tc_frame_packet_bound(&c);
        uint8_t* sized = (uint8_t*)malloc(cap);
        topos_frame_stats sst;
        uint8_t qp_used = 0u;
        c.qp_base = 24u;
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&c, &in, (uint32_t)(size_at_95 * 3u / 2u),
                                              64u, 95u, &qp_used, sized, cap, &sst), TC_OK);
        MT_CHECK(qp_used >= 64u);
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&c, &in, 1u, 0u, 63u, &qp_used,
                                              sized, cap, &sst), TC_OK); /* 旧域界仍合法 */
        free(sized);
        free(pkt_legacy);
        free(y); free(u); free(v); free(a);
    }

    /* ---------- 失败路径：cap 不足 dry-run、NULL 参数、bound ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x9999AAAABBBBCCCCull;
        ic.width = 64u;
        ic.height = 48u;
        ic.kind = TC_SYNTH_GRAIN;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);
        const uint16_t* pl[4] = {y, u, v, NULL};
        topos_frame_input in;
        fill_input(&in, pl);

        topos_frame_config c;
        base_cfg(&c, 64u, 48u);
        c.qp_base = 24u;
        MT_CHECK(tc_frame_packet_bound(&c) > 0u);
        MT_CHECK_EQ_U64(tc_frame_packet_bound(NULL), 0u);

        uint8_t* pkt = NULL;
        size_t size = 0;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);
        MT_CHECK(tc_frame_packet_bound(&c) >= size);

        /* cap = size−1 → BUFFER_TOO_SMALL，dry-run 给出精确所需尺寸 */
        uint8_t* small = (uint8_t*)malloc(size - 1u);
        topos_frame_stats st;
        MT_CHECK_EQ_I64(tc_frame_encode(&c, &in, small, size - 1u, &st),
                        TC_ERR_BUFFER_TOO_SMALL);
        MT_CHECK_EQ_U64(st.packet_size, (uint64_t)size);
        free(small);

        /* out=NULL cap=0 → 同样 dry-run */
        MT_CHECK_EQ_I64(tc_frame_encode(&c, &in, NULL, 0u, &st), TC_ERR_BUFFER_TOO_SMALL);
        MT_CHECK_EQ_U64(st.packet_size, (uint64_t)size);

        /* NULL 参数族 */
        MT_CHECK_EQ_I64(tc_frame_encode(NULL, &in, pkt, size, NULL), TC_ERR_INVALID_ARGUMENT);
        MT_CHECK_EQ_I64(tc_frame_encode(&c, NULL, pkt, size, NULL), TC_ERR_INVALID_ARGUMENT);
        topos_frame_input badin = in;
        badin.planes[1] = NULL;
        MT_CHECK_EQ_I64(tc_frame_encode(&c, &badin, pkt, size, NULL), TC_ERR_INVALID_ARGUMENT);
        MT_CHECK_EQ_I64(tc_frame_encode(&c, &in, NULL, size, NULL), TC_ERR_INVALID_ARGUMENT);
        topos_frame_output info;
        MT_CHECK_EQ_I64(tc_frame_decode(NULL, size, NULL, NULL, &info), TC_ERR_INVALID_ARGUMENT);
        MT_CHECK_EQ_I64(tc_frame_decode(pkt, size, NULL, NULL, NULL), TC_ERR_INVALID_ARGUMENT);

        /* 输出缓冲 NULL 校验 */
        uint16_t* outs[4] = {NULL, NULL, NULL, NULL};
        MT_CHECK_EQ_I64(tc_frame_decode(pkt, size, outs, NULL, &info), TC_ERR_INVALID_ARGUMENT);

        /* NULL stats 合法 */
        uint8_t* again = (uint8_t*)malloc(size);
        MT_CHECK_EQ_I64(tc_frame_encode(&c, &in, again, size, NULL), TC_OK);
        free(again);
        free(pkt);
        free(y); free(u); free(v); free(a);
    }

    /* ---------- 目标尺寸编码（确定性 qp 搜索） ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x7172737475767778ull;
        ic.width = 96u;
        ic.height = 64u;
        ic.kind = TC_SYNTH_GRAIN;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);
        const uint16_t* pl[4] = {y, u, v, NULL};
        topos_frame_input in;
        fill_input(&in, pl);

        topos_frame_config c;
        base_cfg(&c, 96u, 64u);
        c.qp_base = 20u;

        uint8_t* pkt = NULL;
        size_t size = 0;
        topos_frame_stats st;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, &st), TC_OK);
        uint32_t target = st.packet_size / 4u * 3u;

        size_t cap = tc_frame_packet_bound(&c);
        uint8_t* out = (uint8_t*)malloc(cap);
        uint8_t qp_used = 0u;
        topos_frame_stats st2;
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&c, &in, target, 0u, 63u, &qp_used,
                                              out, cap, &st2), TC_OK);
        MT_CHECK(qp_used >= 20u);            /* 超预算 → qp 只会升 */
        MT_CHECK(st2.packet_size <= target); /* 目标达成（qp63 前） */
        MT_CHECK_EQ_U64(st2.qp_base, (uint64_t)qp_used);
        /* 确定性 */
        uint8_t qp2 = 0u;
        topos_frame_stats st3;
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&c, &in, target, 0u, 63u, &qp2,
                                              out, cap, &st3), TC_OK);
        MT_CHECK_EQ_U64(qp_used, qp2);
        MT_CHECK_EQ_U64(st2.packet_size, st3.packet_size);
        /* 等价性：以 qp_used 直接编码 → bit-exact 同输出 */
        c.qp_base = qp_used;
        uint8_t* direct = (uint8_t*)malloc(cap);
        topos_frame_stats st4;
        MT_CHECK_EQ_I64(tc_frame_encode(&c, &in, direct, cap, &st4), TC_OK);
        MT_CHECK(memcmp(out, direct, st2.packet_size) == 0);
        free(direct);
        /* 非法 qp 区间 */
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&c, &in, target, 30u, 20u, NULL,
                                              out, cap, NULL), TC_ERR_INVALID_ARGUMENT);
        free(out);
        free(pkt);

        /* 不可达小目标：qp_max 封顶仍尽力返回（best-effort 语义） */
        uint8_t qp_max_used = 0u;
        uint8_t* out2 = (uint8_t*)malloc(cap);
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&c, &in, 64u, 0u, 40u, &qp_max_used,
                                              out2, cap, NULL), TC_OK);
        MT_CHECK_EQ_U64(qp_max_used, 40u);
        free(out2);
        free(y); free(u); free(v); free(a);
    }

    /* ---------- 多代编解码：稳定化 + 无色偏 ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x3333444455556666ull;
        ic.width = 96u;
        ic.height = 64u;
        ic.kind = TC_SYNTH_GRAIN;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);

        topos_frame_config c;
        base_cfg(&c, 96u, 64u);
        c.qp_base = 24u;

        uint16_t* cur[4] = {y, u, v, NULL};
        uint16_t* gens[5][4];
        memset(gens, 0, sizeof(gens));
        gens[0][0] = y; gens[0][1] = u; gens[0][2] = v;

        for (int gen = 1; gen <= 4; ++gen) {
            topos_frame_input in;
            fill_input(&in, (const uint16_t**)cur);
            uint8_t* pkt = NULL;
            size_t size = 0;
            MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);
            topos_frame_output info;
            MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, gens[gen]), TC_OK);
            free(pkt);
            cur[0] = gens[gen][0]; cur[1] = gens[gen][1]; cur[2] = gens[gen][2];
        }
        /* 第 3 代起逐像素稳定（量格吸收后编码幂等） */
        size_t n = (size_t)96u * 64u;
        MT_CHECK(memcmp(gens[3][0], gens[4][0], n * 2u) == 0);
        MT_CHECK(memcmp(gens[3][1], gens[4][1], 48u * 64u * 2u) == 0);
        MT_CHECK(memcmp(gens[3][2], gens[4][2], 48u * 64u * 2u) == 0);

        /* 退化有界且无色偏：对 gen0 的平均误差逐代不增，chroma 均值漂移 ≤ 1 */
        double m0 = mean_abs_diff(y, gens[1][0], n);
        double m3 = mean_abs_diff(y, gens[4][0], n);
        MT_CHECK(m3 <= m0 + 1e-9);
        double su = 0.0, su0 = 0.0;
        for (size_t i = 0; i < 48u * 64u; ++i) {
            su += (double)gens[4][1][i];
            su0 += (double)u[i];
        }
        double drift = su / (48.0 * 64.0) - su0 / (48.0 * 64.0);
        MT_CHECK(drift < 1.0 && drift > -1.0);
        for (int g = 1; g <= 4; ++g) { free_planes(gens[g]); }
        free(y); free(u); free(v); free(a);
    }

    /* ---------- 多代（有损工作点 qp48）：单调收敛 + 有界漂移 ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x9A9B9C9D9E9F0000ull;
        ic.width = 96u;
        ic.height = 64u;
        ic.kind = TC_SYNTH_GRAIN;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);

        topos_frame_config c;
        base_cfg(&c, 96u, 64u);
        c.qp_base = 48u;

        uint16_t* cur[4] = {y, u, v, NULL};
        uint16_t* gens[7][4];
        memset(gens, 0, sizeof(gens));
        gens[0][0] = y; gens[0][1] = u; gens[0][2] = v;
        double psnr_y[7];
        memset(psnr_y, 0, sizeof(psnr_y));
        for (int gen = 1; gen <= 6; ++gen) {
            topos_frame_input in;
            fill_input(&in, (const uint16_t**)cur);
            uint8_t* pkt = NULL;
            size_t size = 0;
            MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);
            topos_frame_output info;
            MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, gens[gen]), TC_OK);
            free(pkt);
            cur[0] = gens[gen][0]; cur[1] = gens[gen][1]; cur[2] = gens[gen][2];
            double mse = 0.0;
            for (size_t i = 0; i < (size_t)96u * 64u; ++i) {
                double d = (double)y[i] - (double)gens[gen][0][i];
                mse += d * d;
            }
            mse /= (double)(96u * 64u);
            psnr_y[gen] = mse > 0.0 ? 10.0 * log10(1023.0 * 1023.0 / mse) : 99.0;
        }
        /* PSNR 单调不升（不反弹），且逐代变化收敛 */
        for (int g = 2; g <= 6; ++g) {
            MT_CHECK(psnr_y[g] <= psnr_y[g - 1] + 1e-9);
        }
        MT_CHECK(fabs(psnr_y[6] - psnr_y[5]) <= fabs(psnr_y[2] - psnr_y[1]));
        /* 无明显色偏：亮度均值漂移 |Δ| < 0.1 LSB */
        double sy = 0.0;
        for (size_t i = 0; i < (size_t)96u * 64u; ++i) { sy += (double)gens[6][0][i]; }
        double ymean0 = 0.0;
        for (size_t i = 0; i < (size_t)96u * 64u; ++i) { ymean0 += (double)y[i]; }
        double drift = sy / (96.0 * 64.0) - ymean0 / (96.0 * 64.0);
        MT_CHECK(drift < 0.1 && drift > -0.1);
        for (int g = 1; g <= 6; ++g) { free_planes(gens[g]); }
        free(y); free(u); free(v); free(a);
    }

    /* ---------- R4.1：12-bit YUV 4:2:2（v1.2 枚举扩展） ---------- */
    {
        /* 枚举域：12 合法；11/13 拒绝 */
        topos_frame_config c;
        base_cfg(&c, 64u, 48u);
        c.bit_depth = 12u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);
        c.bit_depth = 11u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_ERR_UNSUPPORTED_PIXEL_FORMAT);
        c.bit_depth = 13u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_ERR_UNSUPPORTED_PIXEL_FORMAT);

        /* 往返 + 确定性 + 帧头 v1.2 标记（byte7=1） */
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x1212121234343434ull;
        ic.width = 96u;
        ic.height = 64u;
        ic.kind = TC_SYNTH_MIXED;
        ic.bit_depth = 12u;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);
        MT_CHECK((y[0] % 4u) == 0u && (u[0] % 4u) == 0u); /* 12-bit 缩放合成 */

        base_cfg(&c, 96u, 64u);
        c.bit_depth = 12u;
        c.qp_base = 24u;
        const uint16_t* pl[4] = {y, u, v, NULL};
        topos_frame_input in;
        fill_input(&in, pl);
        uint8_t *pkt = NULL, *pkt2 = NULL;
        size_t size = 0, size2 = 0;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt2, &size2, NULL), TC_OK);
        MT_CHECK(size == size2 && memcmp(pkt, pkt2, size) == 0); /* 确定性 */
        MT_CHECK_EQ_U64(pkt[6], 1ull);   /* version_major */
        MT_CHECK_EQ_U64(pkt[7], 1ull);   /* v1.2 minor：扩展枚举流标记 */
        MT_CHECK_EQ_U64(pkt[12], 12ull); /* bit_depth 入头 */

        topos_frame_output info;
        uint16_t* dec[4] = {NULL, NULL, NULL, NULL};
        MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, dec), TC_OK);
        MT_CHECK_EQ_U64(info.bit_depth, 12ull);
        MT_CHECK_EQ_U64(info.concealed_slices, 0u);
        /* 质量界：qp24 下 12-bit 域最大误差有界（精确值由 golden 冻结） */
        {
            uint32_t maxerr = 0u;
            for (size_t i = 0; i < (size_t)96u * 64u; ++i) {
                uint32_t d = dec[0][i] > y[i] ? dec[0][i] - y[i] : y[i] - dec[0][i];
                if (d > maxerr) { maxerr = d; }
            }
            MT_CHECK(maxerr <= 96u);
        }

        /* 同 qp 码率与 10-bit 可比（等效量化偏移 +4 生效；无偏移时 ≈×2） */
        {
            image_synth_cfg ic10 = ic;
            ic10.bit_depth = 10u;
            uint16_t *y0, *u0, *v0, *a0;
            MT_CHECK_EQ_I64(image_synth_alloc(&ic10, 0, &y0, &u0, &v0, &a0), 0);
            topos_frame_config c10;
            base_cfg(&c10, 96u, 64u);
            c10.qp_base = 24u;
            const uint16_t* pl0[4] = {y0, u0, v0, NULL};
            topos_frame_input in0;
            fill_input(&in0, pl0);
            uint8_t* pkt0 = NULL;
            size_t size0 = 0;
            MT_CHECK_EQ_I64(encode_to_buf(&c10, &in0, &pkt0, &size0, NULL), TC_OK);
            MT_CHECK((double)size <= (double)size0 * 1.6);
            free(pkt0);
            free(y0); free(u0); free(v0); free(a0);
        }

        /* concealment 中性值 = 2^(12−1) = 2048：坏 Y slice → 整带填充 */
        {
            uint8_t* bad = (uint8_t*)malloc(size);
            MT_CHECK(bad != NULL);
            memcpy(bad, pkt, size);
            bad[53 + 17 + 2] ^= 0x40u; /* 首 slice payload（Y）CRC 坏 */
            topos_frame_output binfo;
            uint16_t* bdec[4] = {NULL, NULL, NULL, NULL};
            int32_t rc = decode_to_planes(bad, size, &binfo, bdec);
            MT_CHECK(rc == TC_OK || rc == TC_WARN_CONCEALED);
            MT_CHECK_EQ_U64(binfo.concealed_slices, 1u);
            int all_mid = 1;
            for (size_t i = 0; i < (size_t)96u * 64u; ++i) {
                if (bdec[0][i] != 2048u) { all_mid = 0; break; }
            }
            MT_CHECK(all_mid); /* h=64 → 单带：整平面中性值 */
            free_planes(bdec);
            free(bad);
        }
        free_planes(dec);
        free(pkt); free(pkt2);
        free(y); free(u); free(v); free(a);
    }

    /* ---------- R4.1：12-bit 多代（稳定化 + 无色偏，与 10-bit 同构） ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x5A5A5A5AC3C3C3C3ull;
        ic.width = 96u;
        ic.height = 64u;
        ic.kind = TC_SYNTH_GRAIN;
        ic.bit_depth = 12u;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);

        topos_frame_config c;
        base_cfg(&c, 96u, 64u);
        c.bit_depth = 12u;
        c.qp_base = 24u;

        uint16_t* cur[4] = {y, u, v, NULL};
        uint16_t* gens[5][4];
        memset(gens, 0, sizeof(gens));
        gens[0][0] = y; gens[0][1] = u; gens[0][2] = v;
        for (int gen = 1; gen <= 4; ++gen) {
            topos_frame_input in;
            fill_input(&in, (const uint16_t**)cur);
            uint8_t* pkt = NULL;
            size_t gsz = 0;
            MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &gsz, NULL), TC_OK);
            topos_frame_output info;
            MT_CHECK_EQ_I64(decode_to_planes(pkt, gsz, &info, gens[gen]), TC_OK);
            free(pkt);
            cur[0] = gens[gen][0]; cur[1] = gens[gen][1]; cur[2] = gens[gen][2];
        }
        size_t n = (size_t)96u * 64u;
        MT_CHECK(memcmp(gens[3][0], gens[4][0], n * 2u) == 0);
        double m0 = mean_abs_diff(y, gens[1][0], n);
        double m3 = mean_abs_diff(y, gens[4][0], n);
        MT_CHECK(m3 <= m0 + 1e-9);
        double sy = 0.0, sy0 = 0.0;
        for (size_t i = 0; i < n; ++i) {
            sy += (double)gens[4][0][i];
            sy0 += (double)y[i];
        }
        double drift = sy / (double)n - sy0 / (double)n;
        MT_CHECK(drift < 4.0 && drift > -4.0); /* 12-bit LSB 尺度的 10-bit 1 LSB 等价 */
        for (int g = 1; g <= 4; ++g) { free_planes(gens[g]); }
        free(y); free(u); free(v); free(a);
    }

    /* ---------- R4.2：YUV 4:4:4 10/12-bit（v1.3 枚举扩展，pf=1） ---------- */
    {
        /* 枚举域：pf=1 合法（bd 10/12）；pf=2（GBR，R4.3 起合法，默认
         * matrix=identity）；pf=3 合法（TRAW 批 1，默认 profile=7/qm0/
         * LOG0 transfer——完整正例见下方 TRAW 段） */
        topos_frame_config c;
        base_cfg(&c, 64u, 48u);
        c.pixel_format = 1u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);
        c.bit_depth = 12u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);
        c.pixel_format = 2u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);
        c.pixel_format = 3u;
        c.color_transfer = 0u;   /* H1：pf=3 走原生冻结对默认（base_cfg 钉 1 还原） */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);
        c.alpha_mode = 1u; c.alpha_bit_depth = 16u; /* TRAW 与 alpha 互斥 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_ERR_INVALID_ARGUMENT);
        base_cfg(&c, 64u, 48u);
        c.pixel_format = 3u;
        c.color_transfer = 0u;   /* H1：pf=3 走原生冻结对默认（base_cfg 钉 1 还原） */
        c.qp_delta_chroma = 4;   /* 无色度语义 → 显式拒绝 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_ERR_INVALID_ARGUMENT);

        /* 往返 + 确定性 + 帧头 v1.3 标记（byte7=2 / byte11=1）+ 全宽几何 */
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x4444333322221111ull;
        ic.width = 96u;
        ic.height = 64u;
        ic.kind = TC_SYNTH_MIXED;
        ic.chroma_format = 1u;   /* 4:4:4 合成：U/V 全宽 */
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 1, &y, &u, &v, &a), 0);

        base_cfg(&c, 96u, 64u);
        c.pixel_format = 1u;
        c.qp_base = 24u;
        c.alpha_mode = 2u;
        c.alpha_bit_depth = 8u;
        const uint16_t* pl[4] = {y, u, v, a};
        topos_frame_input in;
        fill_input(&in, pl);
        uint8_t *pkt = NULL, *pkt2 = NULL;
        size_t size = 0, size2 = 0;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt2, &size2, NULL), TC_OK);
        MT_CHECK(size == size2 && memcmp(pkt, pkt2, size) == 0); /* 确定性 */
        MT_CHECK_EQ_U64(pkt[7], 2ull);    /* v1.3 minor：pf=1 扩展代 */
        MT_CHECK_EQ_U64(pkt[11], 1ull);   /* pixel_format 入头 */

        topos_frame_output info;
        uint16_t* dec[4] = {NULL, NULL, NULL, NULL};
        MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, dec), TC_OK);
        MT_CHECK_EQ_U64(info.pixel_format, 1ull);
        MT_CHECK_EQ_U64(info.concealed_slices, 0u);
        /* 4:4:4 几何：U/V 平面 = 全宽全高（96×64），plane_geometry 出口一致 */
        {
            uint32_t w = 0, h = 0;
            MT_CHECK_EQ_I64(tc_frame_plane_geometry(&info, 1u, &w, &h), TC_OK);
            MT_CHECK_EQ_U64(w, 96ull);
            MT_CHECK_EQ_U64(h, 64ull);
            MT_CHECK_EQ_I64(tc_frame_plane_geometry(&info, 3u, &w, &h), TC_OK);
            MT_CHECK_EQ_U64(w, 96ull);
        }
        /* 质量界：qp24 下 Y 最大误差有界（精确值由 golden 冻结） */
        {
            uint32_t maxerr = 0u;
            for (size_t i = 0; i < (size_t)96u * 64u; ++i) {
                uint32_t d = dec[0][i] > y[i] ? dec[0][i] - y[i] : y[i] - dec[0][i];
                if (d > maxerr) { maxerr = d; }
            }
            MT_CHECK(maxerr <= 96u);
        }
        /* packet_bound 上界必须覆盖 4:4:4（chroma 全宽 → 三平面同块数） */
        MT_CHECK(tc_frame_packet_bound(&c) >= size);

        /* concealment：坏 Y slice → 整带中性值（10-bit 中点 512；h=64 单带） */
        {
            uint8_t* bad = (uint8_t*)malloc(size);
            MT_CHECK(bad != NULL);
            memcpy(bad, pkt, size);
            bad[53 + 17 + 2] ^= 0x40u; /* 首 slice payload（Y）CRC 坏 */
            topos_frame_output binfo;
            uint16_t* bdec[4] = {NULL, NULL, NULL, NULL};
            int32_t rc = decode_to_planes(bad, size, &binfo, bdec);
            MT_CHECK(rc == TC_OK || rc == TC_WARN_CONCEALED);
            MT_CHECK_EQ_U64(binfo.concealed_slices, 1u);
            int all_mid = 1;
            for (size_t i = 0; i < (size_t)96u * 64u; ++i) {
                if (bdec[0][i] != 512u) { all_mid = 0; break; }
            }
            MT_CHECK(all_mid);
            free_planes(bdec);
            free(bad);
        }
        free_planes(dec);
        free(pkt); free(pkt2);
        free(y); free(u); free(v); free(a);
    }

    /* ---------- R4.2：4:4:4 多代（稳定化 + 无漂移，与 4:2:2 同构） ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x4C4C4C4C3D3D3D3Dull;
        ic.width = 96u;
        ic.height = 64u;
        ic.kind = TC_SYNTH_GRAIN;
        ic.chroma_format = 1u;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);

        topos_frame_config c;
        base_cfg(&c, 96u, 64u);
        c.pixel_format = 1u;
        c.qp_base = 24u;

        uint16_t* cur[4] = {y, u, v, NULL};
        uint16_t* gens[5][4];
        memset(gens, 0, sizeof(gens));
        gens[0][0] = y; gens[0][1] = u; gens[0][2] = v;
        for (int gen = 1; gen <= 4; ++gen) {
            topos_frame_input in;
            fill_input(&in, (const uint16_t**)cur);
            uint8_t* pkt = NULL;
            size_t gsz = 0;
            MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &gsz, NULL), TC_OK);
            topos_frame_output info;
            MT_CHECK_EQ_I64(decode_to_planes(pkt, gsz, &info, gens[gen]), TC_OK);
            free(pkt);
            cur[0] = gens[gen][0]; cur[1] = gens[gen][1]; cur[2] = gens[gen][2];
        }
        size_t n = (size_t)96u * 64u;
        MT_CHECK(memcmp(gens[3][0], gens[4][0], n * 2u) == 0); /* 收敛后逐位稳定 */
        double m0 = mean_abs_diff(y, gens[1][0], n);
        double m3 = mean_abs_diff(y, gens[4][0], n);
        MT_CHECK(m3 <= m0 + 1e-9);
        double sy = 0.0, sy0 = 0.0;
        for (size_t i = 0; i < n; ++i) {
            sy += (double)gens[4][0][i];
            sy0 += (double)y[i];
        }
        double drift = sy / (double)n - sy0 / (double)n;
        MT_CHECK(drift < 1.0 && drift > -1.0); /* 10-bit 1 LSB 尺度 */
        for (int g = 1; g <= 4; ++g) { free_planes(gens[g]); }
        free(y); free(u); free(v); free(a);
    }

    /* ---------- R4.3：GBR 4:4:4 10/12-bit（v1.4 枚举扩展，pf=2/matrix=0） ---------- */
    {
        /* 枚举域：pf=2 合法（bd 10/12）；pf=3 合法（TRAW 批 1）；matrix 交叉规则 */
        topos_frame_config c;
        base_cfg(&c, 64u, 48u);
        c.pixel_format = 2u;
        c.color_matrix = 0u;   /* GBR 契约：identity */
        c.chroma_siting = 0u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);
        c.bit_depth = 12u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);
        c.pixel_format = 3u;
        c.color_transfer = 0u;   /* H1：pf=3 走原生冻结对默认（base_cfg 钉 1 还原） */
        c.color_matrix = 0u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);
        /* 注：cfg.matrix=0 = "默认"（cfg_to_frame_header 按 pf 取默认矩阵，
         * pf=2 → 0/identity、pf∈{0,1} → 1/bt709）——config 层无法给 YUV 显式
         * 声明 identity，交叉规则的帧头层正反例在 test_frame_header 覆盖 */
        base_cfg(&c, 64u, 48u);
        c.pixel_format = 2u;
        c.color_matrix = 1u;    /* GBR 带 YUV 矩阵 → 拒绝 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_ERR_MALFORMED);
        base_cfg(&c, 64u, 48u);
        c.pixel_format = 2u;
        c.color_matrix = 0u;
        c.chroma_siting = 1u;   /* GBR 无色度采样位置 → 拒绝 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_ERR_MALFORMED);

        /* 往返 + 确定性 + 帧头 v1.4 标记（byte7=3 / byte11=2 / byte35=0）
         * 平面契约 = G,B,R（FFmpeg gbrp 约定），全幅值域 0..2^bd−1 */
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x6B626B6272727272ull;
        ic.width = 96u;
        ic.height = 64u;
        ic.kind = TC_SYNTH_MIXED;
        ic.chroma_format = 1u;   /* GBR：三平面全宽合成 */
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 1, &y, &u, &v, &a), 0);

        base_cfg(&c, 96u, 64u);
        c.pixel_format = 2u;
        c.color_matrix = 0u;
        c.qp_base = 24u;
        c.alpha_mode = 2u;
        c.alpha_bit_depth = 8u;
        const uint16_t* pl[4] = {y, u, v, a};
        topos_frame_input in;
        fill_input(&in, pl);
        uint8_t *pkt = NULL, *pkt2 = NULL;
        size_t size = 0, size2 = 0;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt2, &size2, NULL), TC_OK);
        MT_CHECK(size == size2 && memcmp(pkt, pkt2, size) == 0); /* 确定性 */
        MT_CHECK_EQ_U64(pkt[7], 3ull);    /* v1.4 minor：pf=2 扩展代 */
        MT_CHECK_EQ_U64(pkt[11], 2ull);   /* pixel_format=2 入头 */
        MT_CHECK_EQ_U64(pkt[35], 0ull);   /* color_matrix=0 identity */

        topos_frame_output info;
        uint16_t* dec[4] = {NULL, NULL, NULL, NULL};
        MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, dec), TC_OK);
        MT_CHECK_EQ_U64(info.pixel_format, 2ull);
        MT_CHECK_EQ_U64(info.concealed_slices, 0u);
        {
            uint32_t w = 0, h = 0;
            MT_CHECK_EQ_I64(tc_frame_plane_geometry(&info, 1u, &w, &h), TC_OK);
            MT_CHECK_EQ_U64(w, 96ull);   /* GBR：B 平面全宽 */
            MT_CHECK_EQ_U64(h, 64ull);
        }
        /* 质量界：qp24 下 G 平面最大误差有界（精确值由 golden 冻结） */
        {
            uint32_t maxerr = 0u;
            for (size_t i = 0; i < (size_t)96u * 64u; ++i) {
                uint32_t d = dec[0][i] > y[i] ? dec[0][i] - y[i] : y[i] - dec[0][i];
                if (d > maxerr) { maxerr = d; }
            }
            MT_CHECK(maxerr <= 96u);
        }
        /* packet_bound 上界覆盖（R4.2 遗留镜像修复：pf!=0 全宽块数） */
        MT_CHECK(tc_frame_packet_bound(&c) >= size);

        /* concealment：坏 G slice → 整带中性值（mid=512） */
        {
            uint8_t* bad = (uint8_t*)malloc(size);
            MT_CHECK(bad != NULL);
            memcpy(bad, pkt, size);
            bad[53 + 17 + 2] ^= 0x40u;
            topos_frame_output binfo;
            uint16_t* bdec[4] = {NULL, NULL, NULL, NULL};
            int32_t rc = decode_to_planes(bad, size, &binfo, bdec);
            MT_CHECK(rc == TC_OK || rc == TC_WARN_CONCEALED);
            MT_CHECK_EQ_U64(binfo.concealed_slices, 1u);
            int all_mid = 1;
            for (size_t i = 0; i < (size_t)96u * 64u; ++i) {
                if (bdec[0][i] != 512u) { all_mid = 0; break; }
            }
            MT_CHECK(all_mid);
            free_planes(bdec);
            free(bad);
        }
        free_planes(dec);
        free(pkt); free(pkt2);
        free(y); free(u); free(v); free(a);
    }

    /* ---------- R4.3：GBR 多代（稳定化 + 无漂移） ---------- */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x5B5B5B5B4A4A4A4Aull;
        ic.width = 96u;
        ic.height = 64u;
        ic.kind = TC_SYNTH_GRAIN;
        ic.chroma_format = 1u;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);

        topos_frame_config c;
        base_cfg(&c, 96u, 64u);
        c.pixel_format = 2u;
        c.color_matrix = 0u;
        c.qp_base = 24u;

        uint16_t* cur[4] = {y, u, v, NULL};
        uint16_t* gens[5][4];
        memset(gens, 0, sizeof(gens));
        gens[0][0] = y; gens[0][1] = u; gens[0][2] = v;
        for (int gen = 1; gen <= 4; ++gen) {
            topos_frame_input in;
            fill_input(&in, (const uint16_t**)cur);
            uint8_t* pkt = NULL;
            size_t gsz = 0;
            MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &gsz, NULL), TC_OK);
            topos_frame_output info;
            MT_CHECK_EQ_I64(decode_to_planes(pkt, gsz, &info, gens[gen]), TC_OK);
            free(pkt);
            cur[0] = gens[gen][0]; cur[1] = gens[gen][1]; cur[2] = gens[gen][2];
        }
        size_t n = (size_t)96u * 64u;
        MT_CHECK(memcmp(gens[3][0], gens[4][0], n * 2u) == 0);
        double m0 = mean_abs_diff(y, gens[1][0], n);
        double m3 = mean_abs_diff(y, gens[4][0], n);
        MT_CHECK(m3 <= m0 + 1e-9);
        double sy = 0.0, sy0 = 0.0;
        for (size_t i = 0; i < n; ++i) {
            sy += (double)gens[4][0][i];
            sy0 += (double)y[i];
        }
        double drift = sy / (double)n - sy0 / (double)n;
        MT_CHECK(drift < 1.0 && drift > -1.0);
        for (int g = 1; g <= 4; ++g) { free_planes(gens[g]); }
        free(y); free(u); free(v); free(a);
    }

    /* ---------- R4.4：Pro444/Extreme profile 激活 ---------- */
    {
        /* 交叉规则：Pro444 须 4:4:4（10/12）；Extreme 须 4:4:4 + 12-bit */
        topos_frame_config c;
        base_cfg(&c, 64u, 48u);
        c.profile = 5u;
        c.pixel_format = 1u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);
        c.bit_depth = 12u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);
        c.pixel_format = 2u;
        c.color_matrix = 0u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);
        base_cfg(&c, 64u, 48u);
        c.profile = 5u;          /* Pro444 + 4:2:2 → 拒绝 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_ERR_MALFORMED);
        base_cfg(&c, 64u, 48u);
        c.profile = 6u;
        c.pixel_format = 1u;
        c.bit_depth = 12u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);
        c.bit_depth = 10u;       /* Extreme + 10-bit → 拒绝 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_ERR_MALFORMED);
        base_cfg(&c, 64u, 48u);
        c.profile = 4u;          /* HQ 仍未激活（应用层以 profile3+预设表达） */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_ERR_UNSUPPORTED_PROFILE);

        /* Pro444 往返 + 确定性 + 帧头 profile=5（byte10） */
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x50524F3434342121ull;
        ic.width = 96u;
        ic.height = 64u;
        ic.kind = TC_SYNTH_MIXED;
        ic.chroma_format = 1u;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);
        base_cfg(&c, 96u, 64u);
        c.profile = 5u;
        c.pixel_format = 1u;
        c.bit_depth = 12u;
        c.qp_base = 24u;
        const uint16_t* pl[4] = {y, u, v, NULL};
        topos_frame_input in;
        fill_input(&in, pl);
        uint8_t *pkt = NULL, *pkt2 = NULL;
        size_t size = 0, size2 = 0;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt2, &size2, NULL), TC_OK);
        MT_CHECK(size == size2 && memcmp(pkt, pkt2, size) == 0);
        MT_CHECK_EQ_U64(pkt[10], 5ull);  /* profile=5 入头 */
        MT_CHECK_EQ_U64(pkt[7], 2ull);   /* pf=1 → minor=2 */
        MT_CHECK_EQ_U64(pkt[12], 12ull);
        topos_frame_output info;
        uint16_t* dec[4] = {NULL, NULL, NULL, NULL};
        MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, dec), TC_OK);
        MT_CHECK_EQ_U64(info.profile, 5ull);
        MT_CHECK_EQ_U64(info.concealed_slices, 0u);
        free_planes(dec);
        free(pkt); free(pkt2);
        free(y); free(u); free(v); free(a);

        /* Extreme（GBR 12-bit + alpha）往返 + 帧头 profile=6 */
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x585452454D453131ull;
        ic.width = 48u;
        ic.height = 32u;
        ic.kind = TC_SYNTH_GRAIN;
        ic.chroma_format = 1u;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 1, &y, &u, &v, &a), 0);
        base_cfg(&c, 48u, 32u);
        c.profile = 6u;
        c.pixel_format = 2u;
        c.color_matrix = 0u;
        c.bit_depth = 12u;
        c.qp_base = 20u;
        c.alpha_mode = 2u;
        c.alpha_bit_depth = 8u;
        const uint16_t* plx[4] = {y, u, v, a};
        fill_input(&in, plx);
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);
        MT_CHECK_EQ_U64(pkt[10], 6ull);  /* profile=6 */
        MT_CHECK_EQ_U64(pkt[11], 2ull);  /* GBR */
        MT_CHECK_EQ_U64(pkt[7], 3ull);   /* minor=3 */
        MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, dec), TC_OK);
        MT_CHECK_EQ_U64(info.profile, 6ull);
        MT_CHECK_EQ_U64(info.plane_count, 4u);
        free_planes(dec);
        free(pkt);
        free(y); free(u); free(v); free(a);
    }

    /* ---------- v1.6：TRAW（pf=3 CFA / profile 7 / 12-bit log / minor=5） ---------- */
    {
        /* 合成 4 相位平面 R/Gr/Gb/B，各 (W/2)×(H/2)——复用 luma 合成器
         * （u/v/a 槽 NULL 只写 y），4 个异化种子产独立相位纹理 */
        const uint32_t W = 96u, H = 64u;
        const uint32_t PW = W / 2u, PH = H / 2u;
        uint16_t* ph_planes[4] = {NULL, NULL, NULL, NULL};
        for (int i = 0; i < 4; ++i) {
            image_synth_cfg pc;
            memset(&pc, 0, sizeof(pc));
            pc.seed = 0x54524157CFA0000Full + (uint64_t)i * 0x100000000ull;
            pc.width = PW;
            pc.height = PH;
            pc.kind = (i % 2 == 0) ? TC_SYNTH_GRAIN : TC_SYNTH_GRADIENT;
            pc.bit_depth = 12u;
            ph_planes[i] = (uint16_t*)malloc((size_t)PW * PH * sizeof(uint16_t));
            MT_CHECK(ph_planes[i] != NULL);
            MT_CHECK_EQ_I64(image_synth_build(&pc, ph_planes[i], NULL, NULL, NULL), 0);
        }

        topos_frame_config c;
        base_cfg(&c, W, H);
        c.pixel_format = 3u;     /* TRAW：默认 profile=7 / qm0 / LOG0 transfer */
        c.color_transfer = 0u;   /* H1：pf=3 走原生冻结对默认（base_cfg 钉 1 还原） */
        c.bit_depth = 12u;
        c.qp_base = 24u;
        c.color_transfer = 0u;   /* pf=3 走原生冻结对默认（LOG0），还原 base_cfg 钉扎 */
        const uint16_t* plr[4] = {ph_planes[0], ph_planes[1], ph_planes[2], ph_planes[3]};
        topos_frame_input in;
        fill_input(&in, plr);
        uint8_t *pkt = NULL, *pkt2 = NULL;
        size_t size = 0, size2 = 0;
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt, &size, NULL), TC_OK);
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &pkt2, &size2, NULL), TC_OK);
        MT_CHECK(size == size2 && memcmp(pkt, pkt2, size) == 0); /* 确定性 */
        MT_CHECK_EQ_U64(pkt[7], 5ull);    /* v1.6 minor：pf=3 扩展代 */
        MT_CHECK_EQ_U64(pkt[10], 7ull);   /* profile=7 入头 */
        MT_CHECK_EQ_U64(pkt[11], 3ull);   /* pixel_format=3 入头 */
        MT_CHECK_EQ_U64(pkt[34], 20ull);  /* transfer=TRAW_LOG0 入头 */
        MT_CHECK_EQ_U64(pkt[35], 0ull);   /* matrix=0（identity 契约） */
        MT_CHECK_EQ_U64(pkt[27], 4ull);   /* plane_count=4 */

        topos_frame_output info;
        uint16_t* dec[4] = {NULL, NULL, NULL, NULL};
        MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, dec), TC_OK);
        MT_CHECK_EQ_U64(info.profile, 7ull);
        MT_CHECK_EQ_U64(info.pixel_format, 3ull);
        MT_CHECK_EQ_U64(info.plane_count, 4ull);
        MT_CHECK_EQ_U64(info.concealed_slices, 0u);
        /* CFA 几何：全平面 (W/2)×(H/2)，plane_geometry 出口一致 */
        for (uint32_t p = 0u; p < 4u; ++p) {
            uint32_t w = 0, h = 0;
            MT_CHECK_EQ_I64(tc_frame_plane_geometry(&info, p, &w, &h), TC_OK);
            MT_CHECK_EQ_U64(w, PW);
            MT_CHECK_EQ_U64(h, PH);
        }
        /* 质量界：qp24 透明域（误差 << 噪声量级；精确字节由 golden 冻结） */
        for (int i = 0; i < 4; ++i) {
            uint32_t maxerr = 0u;
            for (size_t j = 0; j < (size_t)PW * PH; ++j) {
                uint32_t d = dec[i][j] > ph_planes[i][j]
                                 ? dec[i][j] - ph_planes[i][j]
                                 : ph_planes[i][j] - dec[i][j];
                if (d > maxerr) { maxerr = d; }
            }
            MT_CHECK(maxerr <= 24u);
        }
        MT_CHECK(tc_frame_packet_bound(&c) >= size);

        /* TRAW 不承载 sized 码控（批 1 CQ 固定锚点 qp；计划 §7 后补选项） */
        {
            uint8_t qp_used = 0u;
            MT_CHECK_EQ_I64(tc_frame_encode_sized(&c, &in, 1024u, 40u, 70u,
                                                  &qp_used, NULL, 0u, NULL),
                            TC_ERR_NOT_IMPLEMENTED);
        }
        for (int i = 0; i < 4; ++i) { free(ph_planes[i]); }
        free_planes(dec);
        free(pkt); free(pkt2);
    }

    /* ---------- v1.7：TRAW 16-bit linear（pf=3 / profile 7 / minor=6 /
     * 无损域验收：qp_base=36 码域像素精确 maxerr=0——批 0 试点同域锚，
     * 变换归一化 EuEv 与位深无关，批 4 阶段 2 内核解锁后实测复证） ---------- */
    {
        const uint32_t W = 96u, H = 64u;
        const uint32_t PW = W / 2u, PH = H / 2u;
        /* 16-bit 全域合成（计划测试域规则：禁止 <<8/<<4 上采，直接生成
         * 全域值）——渐变基底 + 异化 LCG 噪声 + 满刻度探针 */
        uint16_t* ph_planes[4] = {NULL, NULL, NULL, NULL};
        for (int i = 0; i < 4; ++i) {
            uint16_t* pl = (uint16_t*)malloc((size_t)PW * PH * sizeof(uint16_t));
            MT_CHECK(pl != NULL);
            uint64_t st = 0x5452415731360000ull + (uint64_t)i * 0x9E3779B97F4A7C15ull;
            for (size_t j = 0; j < (size_t)PW * PH; ++j) {
                st = st * 6364136223846793005ull + 1442695040888963407ull;
                const uint32_t x = (uint32_t)(j % PW);
                const int32_t grad = (int32_t)((x * 65535u) / (PW - 1u));
                const int32_t noise = (int32_t)((st >> 33) % 96u) - 48;
                int32_t v = grad + noise * (int32_t)(i + 1);
                if (j == 0) { v = 65535; }   /* uint16 满刻度探针 */
                if (j == 1) { v = 0; }       /* 零刻度探针 */
                pl[j] = (uint16_t)(v < 0 ? 0 : (v > 65535 ? 65535 : v));
            }
            ph_planes[i] = pl;
        }

        topos_frame_config c;
        base_cfg(&c, W, H);
        c.pixel_format = 3u;     /* TRAW：默认 profile=7 / qm0 / linear transfer */
        c.color_transfer = 0u;   /* H1：pf=3 走原生冻结对默认（base_cfg 钉 1 还原） */
        c.bit_depth = 16u;
        c.qp_base = 36u;
        c.color_transfer = 0u;   /* pf=3 走原生冻结对默认（bd16→linear 8） */
        c.reserved[0] = 8u;      /* rans2（V7-R2 产品熵；V1/V2 为 12-bit 冻结域；em7 随 V7-R 退役） */
        const uint16_t* plr16[4] = {ph_planes[0], ph_planes[1], ph_planes[2], ph_planes[3]};
        topos_frame_input in16;
        fill_input(&in16, plr16);
        uint8_t *pkt = NULL, *pkt2 = NULL;
        size_t size = 0, size2 = 0;
        /* 批 4 阶段 2：bd16 编码面解锁（原 NOT_IMPLEMENTED 门禁移除） */
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in16, &pkt, &size, NULL), TC_OK);
        MT_CHECK_EQ_I64(encode_to_buf(&c, &in16, &pkt2, &size2, NULL), TC_OK);
        MT_CHECK(size == size2 && memcmp(pkt, pkt2, size) == 0); /* 确定性 */
        MT_CHECK_EQ_U64(pkt[6], 7ull);    /* V7-R2 容器（rans2；V1 minor=6 代际
                                           * 字节覆盖在 test_frame_header patch matrix） */
        MT_CHECK_EQ_U64(pkt[10], 7ull);   /* profile=7 入头 */
        MT_CHECK_EQ_U64(pkt[11], 3ull);   /* pixel_format=3 入头 */
        MT_CHECK_EQ_U64(pkt[34], 8ull);   /* transfer=linear（bd16 冻结对） */
        MT_CHECK_EQ_U64(pkt[35], 0ull);   /* matrix=0（identity 契约） */
        MT_CHECK_EQ_U64(pkt[27], 4ull);   /* plane_count=4 */

        topos_frame_output info;
        uint16_t* dec[4] = {NULL, NULL, NULL, NULL};
        MT_CHECK_EQ_I64(decode_to_planes(pkt, size, &info, dec), TC_OK);
        MT_CHECK_EQ_U64(info.profile, 7ull);
        MT_CHECK_EQ_U64(info.pixel_format, 3ull);
        MT_CHECK_EQ_U64(info.plane_count, 4ull);
        MT_CHECK_EQ_U64(info.concealed_slices, 0u);
        for (uint32_t p = 0u; p < 4u; ++p) {
            uint32_t w = 0, h = 0;
            MT_CHECK_EQ_I64(tc_frame_plane_geometry(&info, p, &w, &h), TC_OK);
            MT_CHECK_EQ_U64(w, PW);
            MT_CHECK_EQ_U64(h, PH);
        }
        /* 无损验收（计划 §批 4）：16-bit linear Q=1 域像素精确 maxerr=0 */
        for (int i = 0; i < 4; ++i) {
            uint32_t maxerr = 0u;
            for (size_t j = 0; j < (size_t)PW * PH; ++j) {
                uint32_t d = dec[i][j] > ph_planes[i][j]
                                 ? dec[i][j] - ph_planes[i][j]
                                 : ph_planes[i][j] - dec[i][j];
                if (d > maxerr) { maxerr = d; }
            }
            MT_CHECK_EQ_U64(maxerr, 0ull);
        }
        MT_CHECK(tc_frame_packet_bound(&c) >= size);
        /* 能力查询：bd16 产品面开放 */
        MT_CHECK_EQ_I64(tc_query_support(7u, 3u, 16u, 0u), TC_OK);
        for (int i = 0; i < 4; ++i) { free(ph_planes[i]); }
        free_planes(dec);
        free(pkt); free(pkt2);
    }

    /* ---------- 批 4 复查（2026-09-13）：bd≥13 熵域门扩展 + token 域
     * 前置硬校验（P4 遗留②升级内存安全必修） ---------- */
    {
        topos_frame_config c;
        base_cfg(&c, 64u, 48u);
        c.pixel_format = 3u;
        c.color_transfer = 0u;   /* H1：pf=3 走原生冻结对默认（base_cfg 钉 1 还原） */
        c.bit_depth = 16u;
        /* VLC/C1/C2/intra/range 族的解码核心是 12-bit 冻结域——bd16 必须
         * 显式拒绝（此前只拒 V1/V2-Rice，em∈{1,3,4,5,6} 可走到 VLC 表
         * 越界读 / batch-commit ±2^25 逆变换） */
        for (uint32_t em = 0u; em <= 6u; ++em) {
            c.reserved[0] = em;
            MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_ERR_INVALID_ARGUMENT);
        }
        c.reserved[0] = 7u;   /* V7-R 随 V 代际收纳退役——写域拒绝 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_ERR_INVALID_ARGUMENT);
        c.reserved[0] = 8u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);
        c.reserved[0] = 9u;   /* V8 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);

        /* V 代际收纳（2026-09-13）：退役 em 写路径拒绝钉死——常规 10-bit
         * 配置下 em∈{2..7} 一律 INVALID_ARGUMENT（信息含 ADR 指引），
         * 静默回落 V1 即事故。 */
        {
            topos_frame_config r;
            base_cfg(&r, 64u, 48u);
            for (uint32_t em = 2u; em <= 7u; ++em) {
                r.reserved[0] = em;
                MT_CHECK_EQ_I64(tc_frame_config_validate(&r), TC_ERR_INVALID_ARGUMENT);
            }
            r.reserved[0] = 11u; /* V7-R3（ADR-C048）：cfg 域扩至 11 */
            MT_CHECK_EQ_I64(tc_frame_config_validate(&r), TC_OK);
            for (uint32_t em = 12u; em <= 13u; ++em) { /* V7-R3 批 1：域上界 11 */
                r.reserved[0] = em;
                MT_CHECK_EQ_I64(tc_frame_config_validate(&r), TC_ERR_INVALID_ARGUMENT);
            }
            r.reserved[0] = 0u;
            MT_CHECK_EQ_I64(tc_frame_config_validate(&r), TC_OK);
            r.reserved[0] = 1u;
            MT_CHECK_EQ_I64(tc_frame_config_validate(&r), TC_OK);
            r.reserved[0] = 8u;
            MT_CHECK_EQ_I64(tc_frame_config_validate(&r), TC_OK);
        }

        /* token 域前检：满幅棋盘 + 低 qp（Q=1）的 |level| 越出 rans 字母表
         * 容量（bitlen>27）——饱和防越界写 + 整帧 INVALID_ARGUMENT（此前
         * 为 hist/freq 栈数组越界写）。qp≥26（Q≥16）同内容合法。 */
        const uint32_t W2 = 96u, H2 = 64u, PW2 = 48u, PH2 = 32u;
        uint16_t* cb[4];
        for (int i = 0; i < 4; ++i) {
            cb[i] = (uint16_t*)malloc((size_t)PW2 * PH2 * sizeof(uint16_t));
            MT_CHECK(cb[i] != NULL);
            for (uint32_t y = 0u; y < PH2; ++y) {
                for (uint32_t x = 0u; x < PW2; ++x) {
                    cb[i][y * PW2 + x] =
                        (uint16_t)(((x + y + (uint32_t)i) & 1u) * 65535u);
                }
            }
        }
        topos_frame_config cc;
        base_cfg(&cc, W2, H2);
        cc.pixel_format = 3u;
        cc.color_transfer = 0u;   /* H1：pf=3 走原生冻结对默认（base_cfg 钉 1 还原） */
        cc.bit_depth = 16u;
        cc.reserved[0] = 8u; /* rans2（token 域硬校验为共享路径，em7 退役后迁宿主） */
        cc.qp_base = 0u;
        const uint16_t* plc[4] = {cb[0], cb[1], cb[2], cb[3]};
        topos_frame_input inc;
        fill_input(&inc, plc);
        uint8_t* bad_pkt = NULL;
        size_t bad_size = 0;
        MT_CHECK_EQ_I64(encode_to_buf(&cc, &inc, &bad_pkt, &bad_size, NULL),
                        TC_ERR_INVALID_ARGUMENT);
        MT_CHECK(bad_pkt == NULL);
        cc.qp_base = 26u;
        uint8_t* ok_pkt = NULL;
        size_t ok_size = 0;
        MT_CHECK_EQ_I64(encode_to_buf(&cc, &inc, &ok_pkt, &ok_size, NULL), TC_OK);
        for (int i = 0; i < 4; ++i) { free(cb[i]); }
        free(ok_pkt);
    }

    /* ---------- 阶段4：跨帧批量解码差分（单批 vs 逐帧；输出位不变契约） ---------- */
    {
        /* 三帧不同几何/内容/alpha：64×48 梯度 sr1、96×64 噪声+alpha、32×32 噪声 */
        uint8_t *p0 = NULL, *p1 = NULL, *p2 = NULL;
        size_t z0 = 0, z1 = 0, z2 = 0;
        {
            image_synth_cfg ic;
            memset(&ic, 0, sizeof(ic));
            ic.seed = 0x1111222233334444ull;
            ic.width = 64u;
            ic.height = 48u;
            ic.kind = TC_SYNTH_GRADIENT;
            uint16_t *y, *u, *v, *a;
            MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);
            const uint16_t* pl[4] = {y, u, v, NULL};
            topos_frame_input in;
            fill_input(&in, pl);
            topos_frame_config c;
            base_cfg(&c, 64u, 48u);
            c.slice_rows = 1u;
            c.qp_base = 22u;
            MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &p0, &z0, NULL), TC_OK);
            free(y); free(u); free(v); free(a);
        }
        {
            image_synth_cfg ic;
            memset(&ic, 0, sizeof(ic));
            ic.seed = 0x5555666677778888ull;
            ic.width = 96u;
            ic.height = 64u;
            ic.kind = TC_SYNTH_GRAIN;
            uint16_t *y, *u, *v, *a;
            MT_CHECK_EQ_I64(image_synth_alloc(&ic, 1, &y, &u, &v, &a), 0);
            const uint16_t* pl[4] = {y, u, v, a};
            topos_frame_input in;
            fill_input(&in, pl);
            topos_frame_config c;
            base_cfg(&c, 96u, 64u);
            c.qp_base = 30u;
            c.alpha_mode = 2u;
            c.alpha_bit_depth = 8u;
            MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &p1, &z1, NULL), TC_OK);
            free(y); free(u); free(v); free(a);
        }
        {
            image_synth_cfg ic;
            memset(&ic, 0, sizeof(ic));
            ic.seed = 0x9999AAAABBBBCCCCull;
            ic.width = 32u;
            ic.height = 32u;
            ic.kind = TC_SYNTH_GRAIN;
            uint16_t *y, *u, *v, *a;
            MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);
            const uint16_t* pl[4] = {y, u, v, NULL};
            topos_frame_input in;
            fill_input(&in, pl);
            topos_frame_config c;
            base_cfg(&c, 32u, 32u);
            c.qp_base = 10u;
            MT_CHECK_EQ_I64(encode_to_buf(&c, &in, &p2, &z2, NULL), TC_OK);
            free(y); free(u); free(v); free(a);
        }

        /* 参考侧：逐帧单帧解码（现有公共路径） */
        topos_frame_output ri[3];
        uint16_t* rp[3][4];
        MT_CHECK_EQ_I64(decode_to_planes(p0, z0, &ri[0], rp[0]), TC_OK);
        MT_CHECK_EQ_I64(decode_to_planes(p1, z1, &ri[1], rp[1]), TC_OK);
        MT_CHECK_EQ_I64(decode_to_planes(p2, z2, &ri[2], rp[2]), TC_OK);

        /* 批量侧：无状态 tc_frame_decode_batch（tight strides = NULL） */
        topos_batch_packet pkts[3];
        pkts[0].data = p0; pkts[0].size = z0;
        pkts[1].data = p1; pkts[1].size = z1;
        pkts[2].data = p2; pkts[2].size = z2;
        uint16_t* bp[3 * TC_FRAME_MAX_PLANES];
        uint32_t geoms[3][4][2];
        for (uint32_t f = 0u; f < 3u; ++f) {
            for (uint32_t p = 0u; p < ri[f].plane_count; ++p) {
                tc_frame_plane_geometry(&ri[f], p, &geoms[f][p][0], &geoms[f][p][1]);
                bp[f * TC_FRAME_MAX_PLANES + p] = (uint16_t*)malloc(
                    (size_t)geoms[f][p][0] * geoms[f][p][1] * sizeof(uint16_t));
                MT_CHECK(bp[f * TC_FRAME_MAX_PLANES + p] != NULL);
            }
        }
        topos_frame_output bi[3];
        MT_CHECK_EQ_I64(tc_frame_decode_batch(pkts, 3u, bp, NULL, bi), TC_OK);
        for (uint32_t f = 0u; f < 3u; ++f) {
            MT_CHECK_EQ_U64(bi[f].concealed_slices, ri[f].concealed_slices);
            MT_CHECK_EQ_U64(bi[f].slice_count, ri[f].slice_count);
            MT_CHECK_EQ_U64(bi[f].plane_count, ri[f].plane_count);
            MT_CHECK_EQ_U64(bi[f].visible_width, ri[f].visible_width);
            MT_CHECK_EQ_U64(bi[f].visible_height, ri[f].visible_height);
            int st_ok = 1;
            for (uint32_t si = 0u; si < ri[f].slice_count; ++si) {
                if (bi[f].slice_status[si] != ri[f].slice_status[si]) { st_ok = 0; }
            }
            MT_CHECK(st_ok);
            for (uint32_t p = 0u; p < ri[f].plane_count; ++p) {
                MT_CHECK_EQ_I64(memcmp(bp[f * TC_FRAME_MAX_PLANES + p], rp[f][p],
                                       (size_t)geoms[f][p][0] * geoms[f][p][1]
                                       * sizeof(uint16_t)), 0);
            }
        }

        /* RD3-04：跨帧任务图的线程预算验收。3 帧全部 slice 必须进入一个
         * tc_parallel_for 批次；8/16 worker 都不得在 worker 内再嵌套派发，
         * 且调度档切换不改变任何输出。 */
        {
            const int scheduler_threads[] = {8, 16};
            for (size_t ti = 0u; ti < sizeof(scheduler_threads) /
                                      sizeof(scheduler_threads[0]); ++ti) {
                tc_dev_set_thread_count(scheduler_threads[ti]);
                tc_dev_tpool_stats_reset();
                topos_frame_output scheduled[3];
                MT_CHECK_EQ_I64(tc_frame_decode_batch(pkts, 3u, bp, NULL, scheduled), TC_OK);
                tc_tpool_stats tps;
                tc_dev_tpool_stats_get(&tps);
                MT_CHECK_EQ_U64(tps.pool_batches, 1u);
                MT_CHECK_EQ_U64(tps.nested_fallbacks, 0u);
                for (uint32_t f = 0u; f < 3u; ++f) {
                    MT_CHECK_EQ_U64(scheduled[f].slice_count, bi[f].slice_count);
                    for (uint32_t p = 0u; p < bi[f].plane_count; ++p) {
                        MT_CHECK_EQ_I64(memcmp(bp[f * TC_FRAME_MAX_PLANES + p], rp[f][p],
                                               (size_t)geoms[f][p][0] * geoms[f][p][1]
                                               * sizeof(uint16_t)), 0);
                    }
                }
            }
            tc_dev_set_thread_count(0);
        }

        /* 带 stride 的批量输出（plane0 行距 = w+8）与 tight 参考位一致 */
        {
            uint16_t* bs[TC_FRAME_MAX_PLANES];
            uint32_t w = geoms[0][0][0], h = geoms[0][0][1];
            bs[0] = (uint16_t*)malloc((size_t)(w + 8u) * h * sizeof(uint16_t));
            for (uint32_t p = 1u; p < ri[0].plane_count; ++p) {
                bs[p] = (uint16_t*)malloc(
                    (size_t)geoms[0][p][0] * geoms[0][p][1] * sizeof(uint16_t));
            }
            size_t st[TC_FRAME_MAX_PLANES];
            st[0] = w + 8u;
            for (uint32_t p = 1u; p < ri[0].plane_count; ++p) { st[p] = 0u; }
            topos_frame_output si1;
            MT_CHECK_EQ_I64(tc_frame_decode_batch(pkts, 1u, bs, st, &si1), TC_OK);
            int rows_ok = 1;
            for (uint32_t r = 0u; r < h; ++r) {
                if (memcmp(&bs[0][(size_t)r * (w + 8u)],
                           &rp[0][0][(size_t)r * w], (size_t)w * sizeof(uint16_t)) != 0) {
                    rows_ok = 0;
                }
            }
            MT_CHECK(rows_ok);
            free(bs[0]);
            for (uint32_t p = 1u; p < ri[0].plane_count; ++p) { free(bs[p]); }
        }

        /* count == 1 等价单帧；count == 0 无操作；查询模式仅填几何 */
        {
            topos_frame_output one;
            MT_CHECK_EQ_I64(tc_frame_decode_batch(pkts, 1u, bp, NULL, &one), TC_OK);
            MT_CHECK_EQ_U64(one.slice_count, ri[0].slice_count);
            MT_CHECK_EQ_I64(tc_frame_decode_batch(NULL, 0u, NULL, NULL, NULL), TC_OK);
            topos_frame_output qi[3];
            MT_CHECK_EQ_I64(tc_frame_decode_batch(pkts, 3u, NULL, NULL, qi), TC_OK);
            for (uint32_t f = 0u; f < 3u; ++f) {
                MT_CHECK_EQ_U64(qi[f].visible_width, ri[f].visible_width);
                MT_CHECK_EQ_U64(qi[f].visible_height, ri[f].visible_height);
                MT_CHECK_EQ_U64(qi[f].plane_count, ri[f].plane_count);
            }
        }

        /* 中帧 payload 坏（CRC）→ 仅该帧 conceal，邻帧位不变；返回码 = 首个
         * 非 OK 帧（帧 1 的 TC_WARN_CONCEALED） */
        {
            topos_packet_view vw;
            MT_CHECK_EQ_I64(tc_packet_scan(p1, z1, &vw), TC_OK);
            size_t off = TC_FRAME_HEADER_SIZE;
            for (uint32_t si = 0u; si < 2u; ++si) {
                off += TC_SLICE_HEADER_SIZE + vw.slices[si].slice_payload_size;
            }
            off += TC_SLICE_HEADER_SIZE;
            uint8_t* bad1 = (uint8_t*)malloc(z1);
            memcpy(bad1, p1, z1);
            bad1[off] ^= 0x40u;

            topos_frame_output cref;
            uint16_t* cdec[4];
            MT_CHECK_EQ_I64(decode_to_planes(bad1, z1, &cref, cdec), TC_WARN_CONCEALED);
            MT_CHECK_EQ_U64(cref.concealed_slices, 1u);

            topos_batch_packet bk[3];
            bk[0].data = p0; bk[0].size = z0;
            bk[1].data = bad1; bk[1].size = z1;
            bk[2].data = p2; bk[2].size = z2;
            topos_frame_output bci[3];
            MT_CHECK_EQ_I64(tc_frame_decode_batch(bk, 3u, bp, NULL, bci),
                            TC_WARN_CONCEALED);
            MT_CHECK_EQ_U64(bci[0].concealed_slices, 0u);
            MT_CHECK_EQ_U64(bci[1].concealed_slices, 1u);
            MT_CHECK_EQ_U64(bci[2].concealed_slices, 0u);
            MT_CHECK_EQ_I64(memcmp(bp[0], rp[0][0],
                                   (size_t)geoms[0][0][0] * geoms[0][0][1]
                                   * sizeof(uint16_t)), 0);
            MT_CHECK_EQ_I64(memcmp(bp[1 * TC_FRAME_MAX_PLANES], cdec[0],
                                   (size_t)geoms[1][0][0] * geoms[1][0][1]
                                   * sizeof(uint16_t)), 0);
            MT_CHECK_EQ_I64(memcmp(bp[2 * TC_FRAME_MAX_PLANES], rp[2][0],
                                   (size_t)geoms[2][0][0] * geoms[2][0][1]
                                   * sizeof(uint16_t)), 0);
            free_planes(cdec);
            free(bad1);
        }

        /* 结构解析失败（截断包）→ 整批拒绝，不产出任何帧 */
        {
            topos_batch_packet tk[2];
            tk[0].data = p0; tk[0].size = z0;
            tk[1].data = p2; tk[1].size = z2 - 4u;
            topos_frame_output ti[2];
            MT_CHECK(tc_frame_decode_batch(tk, 2u, bp, NULL, ti) < 0);
        }

        /* context 批量：tc_decoder_decode_batch 输出与无状态批量位一致；
         * 中帧 plane view pixels==NULL → 仅该帧拒，邻帧交付 */
        {
            tc_decoder* dec = NULL;
            MT_CHECK_EQ_I64(tc_decoder_create(NULL, &dec), TC_OK);
            topos_plane_view vs[3 * TC_FRAME_MAX_PLANES];
            memset(vs, 0, sizeof(vs));
            for (uint32_t f = 0u; f < 3u; ++f) {
                for (uint32_t p = 0u; p < ri[f].plane_count; ++p) {
                    topos_plane_view* v = &vs[f * TC_FRAME_MAX_PLANES + p];
                    v->struct_size = (uint32_t)sizeof(topos_plane_view);
                    v->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
                    v->pixels = bp[f * TC_FRAME_MAX_PLANES + p];
                    v->stride = 0;
                }
            }
            topos_frame_output ci[3];
            MT_CHECK_EQ_I64(tc_decoder_decode_batch(dec, pkts, 3u, vs, ci), TC_OK);
            for (uint32_t f = 0u; f < 3u; ++f) {
                MT_CHECK_EQ_U64(ci[f].concealed_slices, ri[f].concealed_slices);
                MT_CHECK_EQ_U64(ci[f].slice_count, ri[f].slice_count);
                for (uint32_t p = 0u; p < ri[f].plane_count; ++p) {
                    MT_CHECK_EQ_I64(memcmp(bp[f * TC_FRAME_MAX_PLANES + p], rp[f][p],
                                           (size_t)geoms[f][p][0] * geoms[f][p][1]
                                           * sizeof(uint16_t)), 0);
                }
            }

            topos_plane_view* bad = &vs[1 * TC_FRAME_MAX_PLANES];
            uint16_t* tmp = bad->pixels;
            bad->pixels = NULL;
            MT_CHECK_EQ_I64(tc_decoder_decode_batch(dec, pkts, 3u, vs, ci),
                            TC_ERR_INVALID_ARGUMENT);
            bad->pixels = tmp;
            /* 复跑确认 context 状态未被失败批污染 */
            MT_CHECK_EQ_I64(tc_decoder_decode_batch(dec, pkts, 3u, vs, ci), TC_OK);
            tc_decoder_destroy(dec);
        }

        /* 固定比例 reduced 解码：尺寸、单帧/跨帧结果，以及 host-visible
         * surface 直写契约。低频路径允许与完整解码有近似差异，但不允许越界
         * 或改变帧/plane 几何。 */
        {
            const tc_decode_scale scales[] = {
                TC_DECODE_SCALE_HALF, TC_DECODE_SCALE_THIRD,
                TC_DECODE_SCALE_QUARTER, TC_DECODE_SCALE_EIGHTH
            };
            for (uint32_t si = 0u; si < 4u; ++si) {
                topos_frame_output qi[3];
                MT_CHECK_EQ_I64(tc_frame_decode_batch_reduced(pkts, 3u, scales[si],
                                                               NULL, NULL, qi), TC_OK);
                uint16_t* outp[3 * TC_FRAME_MAX_PLANES];
                memset(outp, 0, sizeof(outp));
                for (uint32_t f = 0u; f < 3u; ++f) {
                    for (uint32_t p = 0u; p < qi[f].plane_count; ++p) {
                        uint32_t w = 0u, h = 0u;
                        MT_CHECK_EQ_I64(tc_frame_plane_geometry(&qi[f], p, &w, &h), TC_OK);
                        outp[f * TC_FRAME_MAX_PLANES + p] =
                            (uint16_t*)calloc((size_t)w * h, sizeof(uint16_t));
                        MT_CHECK(outp[f * TC_FRAME_MAX_PLANES + p] != NULL);
                    }
                }
                topos_frame_output bi_reduced[3];
                MT_CHECK_EQ_I64(tc_frame_decode_batch_reduced(pkts, 3u, scales[si],
                                                               outp, NULL, bi_reduced), TC_OK);
                for (uint32_t f = 0u; f < 3u; ++f) {
                    MT_CHECK_EQ_U64(bi_reduced[f].visible_width, qi[f].visible_width);
                    MT_CHECK_EQ_U64(bi_reduced[f].visible_height, qi[f].visible_height);
                    for (uint32_t p = 0u; p < qi[f].plane_count; ++p) {
                        MT_CHECK(outp[f * TC_FRAME_MAX_PLANES + p] != NULL);
                        free(outp[f * TC_FRAME_MAX_PLANES + p]);
                    }
                }
            }

        /* scaled 解码与 sparse/batch 存储路径的差分回归：scaled 输出在任何
         * dev 开关组合下必须逐位一致，且按目标几何精确分配的堆缓冲不得被
         * 越界写（回归背景：sparse_xy/batch_commit 曾按源坐标直写目标平面，
         * ASan 复现 heap-buffer-overflow；修复后 scaled 恒走逐点采样路径）。 */
        {
            topos_frame_output ref_info;
            memset(&ref_info, 0, sizeof(ref_info));
            MT_CHECK_EQ_I64(tc_frame_decode_scaled(p0, z0, 0u, 0u, NULL, NULL,
                                                   &ref_info),
                            TC_ERR_INVALID_ARGUMENT);
            const uint32_t sw = (uint32_t)ri[0].visible_width;
            const uint32_t sh = (uint32_t)ri[0].visible_height;
            const uint32_t tw = sw / 2u;
            const uint32_t th = sh / 2u;
            MT_CHECK(tw > 0u && th > 0u);
            uint16_t* ref_planes[4] = {NULL, NULL, NULL, NULL};
            uint16_t* knob_planes[4] = {NULL, NULL, NULL, NULL};
            topos_frame_output ref_out;
            memset(&ref_out, 0, sizeof(ref_out));
            MT_CHECK_EQ_I64(tc_frame_decode_scaled(p0, z0, tw, th, NULL, NULL,
                                                   &ref_out), TC_OK);
            for (uint32_t p = 0u; p < ref_out.plane_count; ++p) {
                uint32_t w = 0u, h = 0u;
                MT_CHECK_EQ_I64(tc_frame_plane_geometry(&ref_out, p, &w, &h), TC_OK);
                ref_planes[p] = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
                knob_planes[p] = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
                MT_CHECK(ref_planes[p] != NULL && knob_planes[p] != NULL);
            }
            MT_CHECK_EQ_I64(tc_frame_decode_scaled(p0, z0, tw, th, ref_planes,
                                                   NULL, &ref_out), TC_OK);
            /* sparse 开 → 逐位一致；batch 开 → 逐位一致；恢复默认 */
            tc_dev_set_sparse_threshold(16);
            topos_frame_output knob_out;
            memset(&knob_out, 0, sizeof(knob_out));
            MT_CHECK_EQ_I64(tc_frame_decode_scaled(p0, z0, tw, th, knob_planes,
                                                   NULL, &knob_out), TC_OK);
            tc_dev_set_sparse_threshold(0);
            tc_dev_set_batch_idct(0);
            MT_CHECK_EQ_I64(tc_frame_decode_scaled(p0, z0, tw, th, knob_planes,
                                                   NULL, &knob_out), TC_OK);
            tc_dev_set_batch_idct(1);
            for (uint32_t p = 0u; p < ref_out.plane_count; ++p) {
                uint32_t w = 0u, h = 0u;
                MT_CHECK_EQ_I64(tc_frame_plane_geometry(&ref_out, p, &w, &h), TC_OK);
                MT_CHECK_EQ_I64(memcmp(ref_planes[p], knob_planes[p],
                                       (size_t)w * h * sizeof(uint16_t)), 0);
            }
            for (uint32_t p = 0u; p < 4u; ++p) {
                free(ref_planes[p]);
                free(knob_planes[p]);
            }
        }

            tc_decoder* surface_dec = NULL;
            MT_CHECK_EQ_I64(tc_decoder_create(NULL, &surface_dec), TC_OK);
            topos_decode_surface surface;
            memset(&surface, 0, sizeof(surface));
            surface.struct_size = (uint32_t)sizeof(surface);
            surface.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
            surface.memory_type = TC_DECODE_MEMORY_HOST_VISIBLE_GPU;
            for (uint32_t p = 0u; p < ri[0].plane_count; ++p) {
                surface.planes[p].struct_size = (uint32_t)sizeof(topos_plane_view);
                surface.planes[p].abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
                surface.planes[p].pixels = rp[0][p];
                surface.planes[p].stride = 0u;
            }
            topos_frame_output surface_info;
            MT_CHECK_EQ_I64(tc_decoder_decode_surface(surface_dec, p0, z0,
                                                      &surface, &surface_info), TC_OK);
            tc_decoder_destroy(surface_dec);
        }

        for (uint32_t f = 0u; f < 3u; ++f) {
            for (uint32_t p = 0u; p < ri[f].plane_count; ++p) {
                free(bp[f * TC_FRAME_MAX_PLANES + p]);
            }
            free_planes(rp[f]);
        }
        free(p0); free(p1); free(p2);
    }

    /* RD0-02：当前 reduced/scaled 路径的格式矩阵回归。覆盖非 8 倍数/奇数
     * 尺寸、4:2:2/4:4:4/GBR、12-bit、alpha、V2 VLC，以及 scalar/SIMD、
     * sparse、batch IDCT 的组合。所有输出都带行尾和首尾哨兵，确保“降采样”
     * 不会把源坐标或 coded 坐标误写进目标平面。 */
    {
        uint32_t tw = 0u;
        uint32_t th = 0u;
        MT_CHECK_EQ_I64(tc_decode_scale_dimensions(1u, 1u, TC_DECODE_SCALE_EIGHTH,
                                                   &tw, &th), TC_OK);
        MT_CHECK_EQ_U64(tw, 1u);
        MT_CHECK_EQ_U64(th, 1u);
        MT_CHECK_EQ_I64(tc_decode_scale_dimensions(0u, 1u, TC_DECODE_SCALE_HALF,
                                                   &tw, &th), TC_ERR_INVALID_ARGUMENT);
        MT_CHECK_EQ_I64(tc_decode_scale_dimensions(1u, 1u, (tc_decode_scale)99,
                                                   &tw, &th), TC_ERR_INVALID_ARGUMENT);

        size_t cfg_count = 0u;
        const packet_synth_cfg* cfgs = reduced_matrix_cfgs(&cfg_count);
        const tc_decode_scale scales[] = {
            TC_DECODE_SCALE_HALF, TC_DECODE_SCALE_THIRD,
            TC_DECODE_SCALE_QUARTER, TC_DECODE_SCALE_EIGHTH
        };
        for (size_t ci = 0u; ci < cfg_count; ++ci) {
            uint8_t* packet = NULL;
            size_t packet_size = 0u;
            MT_CHECK_EQ_I64(packet_synth_build(&cfgs[ci], &packet, &packet_size), TC_OK);
            if (packet == NULL) { continue; }

            for (size_t si = 0u; si < sizeof(scales) / sizeof(scales[0]); ++si) {
                topos_frame_output query;
                memset(&query, 0, sizeof(query));
                MT_CHECK_EQ_I64(tc_frame_decode_reduced(packet, packet_size, scales[si],
                                                        NULL, NULL, &query), TC_OK);
                uint32_t expect_w = 0u;
                uint32_t expect_h = 0u;
                MT_CHECK_EQ_I64(tc_decode_scale_dimensions(cfgs[ci].visible_w,
                                                           cfgs[ci].visible_h,
                                                           scales[si], &expect_w, &expect_h),
                                TC_OK);
                MT_CHECK_EQ_U64(query.visible_width, expect_w);
                MT_CHECK_EQ_U64(query.visible_height, expect_h);
                MT_CHECK_EQ_U64(query.plane_count, (uint32_t)(3u + cfgs[ci].with_alpha));

                reduced_guard_plane reduced_auto[TC_FRAME_MAX_PLANES];
                reduced_guard_plane reduced_scalar[TC_FRAME_MAX_PLANES];
                reduced_guard_plane reduced_force[TC_FRAME_MAX_PLANES];
                reduced_guard_plane scaled_auto[TC_FRAME_MAX_PLANES];
                reduced_guard_plane scaled_knobs[TC_FRAME_MAX_PLANES];
                memset(reduced_auto, 0, sizeof(reduced_auto));
                memset(reduced_scalar, 0, sizeof(reduced_scalar));
                memset(reduced_force, 0, sizeof(reduced_force));
                memset(scaled_auto, 0, sizeof(scaled_auto));
                memset(scaled_knobs, 0, sizeof(scaled_knobs));
                uint16_t* reduced_auto_ptrs[TC_FRAME_MAX_PLANES] = {NULL};
                uint16_t* reduced_scalar_ptrs[TC_FRAME_MAX_PLANES] = {NULL};
                uint16_t* reduced_force_ptrs[TC_FRAME_MAX_PLANES] = {NULL};
                size_t reduced_strides[TC_FRAME_MAX_PLANES] = {0u};
                uint16_t* scaled_auto_ptrs[TC_FRAME_MAX_PLANES] = {NULL};
                uint16_t* scaled_knobs_ptrs[TC_FRAME_MAX_PLANES] = {NULL};
                size_t scaled_strides[TC_FRAME_MAX_PLANES] = {0u};
                int alloc_ok = 1;
                for (uint32_t p = 0u; p < query.plane_count; ++p) {
                    if (!reduced_guard_alloc(&query, p, &reduced_auto[p]) ||
                        !reduced_guard_alloc(&query, p, &reduced_scalar[p]) ||
                        !reduced_guard_alloc(&query, p, &reduced_force[p])) {
                        alloc_ok = 0;
                        break;
                    }
                    reduced_auto_ptrs[p] = reduced_auto[p].pixels;
                    reduced_scalar_ptrs[p] = reduced_scalar[p].pixels;
                    reduced_force_ptrs[p] = reduced_force[p].pixels;
                    reduced_strides[p] = reduced_auto[p].stride;
                }
                topos_frame_output scaled_query;
                memset(&scaled_query, 0, sizeof(scaled_query));
                if (alloc_ok) {
                    MT_CHECK_EQ_I64(tc_frame_decode_scaled(packet, packet_size, expect_w,
                                                           expect_h, NULL, NULL,
                                                           &scaled_query), TC_OK);
                    for (uint32_t p = 0u; p < scaled_query.plane_count; ++p) {
                        if (!reduced_guard_alloc(&scaled_query, p, &scaled_auto[p]) ||
                            !reduced_guard_alloc(&scaled_query, p, &scaled_knobs[p])) {
                            alloc_ok = 0;
                            break;
                        }
                        scaled_auto_ptrs[p] = scaled_auto[p].pixels;
                        scaled_knobs_ptrs[p] = scaled_knobs[p].pixels;
                        scaled_strides[p] = scaled_auto[p].stride;
                    }
                }

                if (alloc_ok) {
                    tc_dev_set_sparse_threshold(0);
                    tc_dev_set_batch_idct(0);
                    tc_dev_set_simd_mode(TC_SIMD_AUTO);
                    MT_CHECK_EQ_I64(tc_frame_decode_reduced(packet, packet_size, scales[si],
                                                            reduced_auto_ptrs, reduced_strides,
                                                            &query), TC_OK);

                    tc_dev_set_sparse_threshold(16);
                    tc_dev_set_batch_idct(1);
                    tc_dev_set_simd_mode(TC_SIMD_SCALAR);
                    topos_frame_output scalar_info;
                    memset(&scalar_info, 0, sizeof(scalar_info));
                    MT_CHECK_EQ_I64(tc_frame_decode_reduced(packet, packet_size, scales[si],
                                                            reduced_scalar_ptrs, reduced_strides,
                                                            &scalar_info), TC_OK);

                    tc_dev_set_sparse_threshold(0);
                    tc_dev_set_batch_idct(0);
                    tc_dev_set_simd_mode(TC_SIMD_FORCE);
                    topos_frame_output force_info;
                    memset(&force_info, 0, sizeof(force_info));
                    MT_CHECK_EQ_I64(tc_frame_decode_reduced(packet, packet_size, scales[si],
                                                            reduced_force_ptrs, reduced_strides,
                                                            &force_info), TC_OK);
                    for (uint32_t p = 0u; p < query.plane_count; ++p) {
                        reduced_guard_check(&reduced_auto[p]);
                        reduced_guard_check(&reduced_scalar[p]);
                        reduced_guard_check(&reduced_force[p]);
                        reduced_guard_compare(&reduced_auto[p], &reduced_scalar[p]);
                        reduced_guard_compare(&reduced_auto[p], &reduced_force[p]);
                    }

                    /* scaled 目标尺寸仍应与 dev 开关逐位一致；它与 reduced
                     * 的低频近似结果不要求逐位相同。 */
                    tc_dev_set_sparse_threshold(0);
                    tc_dev_set_batch_idct(0);
                    tc_dev_set_simd_mode(TC_SIMD_AUTO);
                    MT_CHECK_EQ_I64(tc_frame_decode_scaled(packet, packet_size, expect_w,
                                                           expect_h, scaled_auto_ptrs,
                                                           scaled_strides, &scaled_query), TC_OK);
                    tc_dev_set_sparse_threshold(16);
                    tc_dev_set_batch_idct(1);
                    tc_dev_set_simd_mode(TC_SIMD_SCALAR);
                    topos_frame_output scaled_knob_info;
                    memset(&scaled_knob_info, 0, sizeof(scaled_knob_info));
                    MT_CHECK_EQ_I64(tc_frame_decode_scaled(packet, packet_size, expect_w,
                                                           expect_h, scaled_knobs_ptrs,
                                                           scaled_strides, &scaled_knob_info), TC_OK);
                    for (uint32_t p = 0u; p < scaled_query.plane_count; ++p) {
                        reduced_guard_check(&scaled_auto[p]);
                        reduced_guard_check(&scaled_knobs[p]);
                        reduced_guard_compare(&scaled_auto[p], &scaled_knobs[p]);
                    }

                    /* 批量 reduced 必须与同一包的单帧路径逐位一致，且两帧
                     * 的平面内存互不相交。 */
                    topos_batch_packet packets[2] = {
                        {packet, packet_size}, {packet, packet_size}
                    };
                    topos_frame_output batch_query[2];
                    memset(batch_query, 0, sizeof(batch_query));
                    MT_CHECK_EQ_I64(tc_frame_decode_batch_reduced(packets, 2u, scales[si],
                                                                   NULL, NULL, batch_query), TC_OK);
                    reduced_guard_plane batch_planes[2][TC_FRAME_MAX_PLANES];
                    memset(batch_planes, 0, sizeof(batch_planes));
                    uint16_t* batch_ptrs[2 * TC_FRAME_MAX_PLANES] = {NULL};
                    size_t batch_strides[2 * TC_FRAME_MAX_PLANES] = {0u};
                    int batch_alloc_ok = 1;
                    for (uint32_t f = 0u; f < 2u && batch_alloc_ok; ++f) {
                        for (uint32_t p = 0u; p < batch_query[f].plane_count; ++p) {
                            if (!reduced_guard_alloc(&batch_query[f], p,
                                                     &batch_planes[f][p])) {
                                batch_alloc_ok = 0;
                                break;
                            }
                            const size_t index = (size_t)f * TC_FRAME_MAX_PLANES + p;
                            batch_ptrs[index] = batch_planes[f][p].pixels;
                            batch_strides[index] = batch_planes[f][p].stride;
                        }
                    }
                    if (batch_alloc_ok) {
                        tc_dev_set_sparse_threshold(0);
                        tc_dev_set_batch_idct(0);
                        tc_dev_set_simd_mode(TC_SIMD_AUTO);
                        topos_frame_output batch_info[2];
                        memset(batch_info, 0, sizeof(batch_info));
                        MT_CHECK_EQ_I64(tc_frame_decode_batch_reduced(packets, 2u, scales[si],
                                                                       batch_ptrs,
                                                                       batch_strides,
                                                                       batch_info), TC_OK);
                        for (uint32_t f = 0u; f < 2u; ++f) {
                            for (uint32_t p = 0u; p < batch_info[f].plane_count; ++p) {
                                reduced_guard_check(&batch_planes[f][p]);
                                reduced_guard_compare(&reduced_auto[p], &batch_planes[f][p]);
                            }
                        }
                    }
                    for (uint32_t f = 0u; f < 2u; ++f) {
                        for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
                            reduced_guard_free(&batch_planes[f][p]);
                        }
                    }
                }

                tc_dev_set_sparse_threshold(0);
                tc_dev_set_batch_idct(0);
                tc_dev_set_simd_mode(TC_SIMD_AUTO);
                for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
                    reduced_guard_free(&reduced_auto[p]);
                    reduced_guard_free(&reduced_scalar[p]);
                    reduced_guard_free(&reduced_force[p]);
                    reduced_guard_free(&scaled_auto[p]);
                    reduced_guard_free(&scaled_knobs[p]);
                }
            }
            free(packet);
        }
    }

    /* RD1-01：统一 decode request ABI 与 AUTO_2K 选择规则。此测试直接锁定
     * 16K 输入会落到 1/8 目标网格，且不会把 V1/V2 fallback 误报成跳段解码。 */
    {
        topos_decode_request request;
        memset(&request, 0, sizeof(request));
        request.struct_size = (uint32_t)sizeof(request);
        request.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        request.mode = TC_DECODE_MODE_AUTO_2K;
        request.memory_type = TC_DECODE_MEMORY_CPU;

        topos_frame_header fh;
        memset(&fh, 0, sizeof(fh));
        fh.visible_width = 16384u;
        fh.visible_height = 9000u;
        tc_decode_plan plan;
        MT_CHECK_EQ_I64(tc_decode_request_resolve(&fh, &request, &plan), TC_OK);
        MT_CHECK_EQ_U64(plan.scale, TC_DECODE_SCALE_EIGHTH);
        MT_CHECK_EQ_U64(plan.target_width, 2048u);
        MT_CHECK_EQ_U64(plan.target_height, 1125u);
        MT_CHECK_EQ_U64(plan.coefficient_limit, 4u);
        MT_CHECK(plan.scaled != 0u);

        request.mode = TC_DECODE_MODE_SCALED;
        request.target_width = 1920u;
        request.target_height = 1080u;
        MT_CHECK_EQ_I64(tc_decode_request_resolve(&fh, &request, &plan), TC_OK);
        MT_CHECK_EQ_U64(plan.target_width, 1920u);
        MT_CHECK_EQ_U64(plan.target_height, 1080u);
        MT_CHECK_EQ_U64(plan.coefficient_limit, 63u);
        MT_CHECK(plan.scaled != 0u);

        request.mode = TC_DECODE_MODE_REDUCED;
        request.target_width = 0u;
        request.target_height = 0u;
        request.scale = TC_DECODE_SCALE_THIRD;
        MT_CHECK_EQ_I64(tc_decode_request_resolve(&fh, &request, &plan), TC_OK);
        MT_CHECK_EQ_U64(plan.target_width, 5462u);
        MT_CHECK_EQ_U64(plan.target_height, 3000u);
        MT_CHECK_EQ_U64(plan.coefficient_limit, 16u);

        /* 半尺寸优化（2026-09-20）：bit0 = TC_DECODE_FLAG_DROP_ALPHA（省略
         * alpha 解码）已合法化——未定义位（bit1）才须拒绝 */
        request.flags = 2u;
        MT_CHECK_EQ_I64(tc_decode_request_validate(&request), TC_ERR_INVALID_ARGUMENT);
        request.flags = 0u;
        request.reserved[2] = 1u;
        MT_CHECK_EQ_I64(tc_decode_request_validate(&request), TC_ERR_INVALID_ARGUMENT);
    }

    /* RD0-03：profile 诊断契约（仅在 TOPOS_CODEC_PROFILE=1 的 sanitizer/诊断
     * 运行中断言；默认单测保持零统计开销）。当前 V1/V2 没有可跳过 segment，
     * 所以 segments_skipped 必须保持 0，而 entropy_symbols 仍应覆盖完整消费。 */
    if (tc_profile_enabled()) {
        const packet_synth_cfg* telemetry_cfg =
            packet_synth_cfg_at(PACKET_SYNTH_CFG_WIDE);
        uint8_t* telemetry_packet = NULL;
        size_t telemetry_size = 0u;
        MT_CHECK(telemetry_cfg != NULL);
        if (telemetry_cfg != NULL) {
            MT_CHECK_EQ_I64(packet_synth_build(telemetry_cfg, &telemetry_packet,
                                               &telemetry_size), TC_OK);
        }
        if (telemetry_packet != NULL) {
            topos_frame_output telemetry_info;
            memset(&telemetry_info, 0, sizeof(telemetry_info));
            MT_CHECK_EQ_I64(tc_frame_decode_reduced(
                                telemetry_packet, telemetry_size,
                                TC_DECODE_SCALE_QUARTER, NULL, NULL,
                                &telemetry_info), TC_OK);
            uint16_t* telemetry_planes[TC_FRAME_MAX_PLANES] = {NULL};
            for (uint32_t p = 0u; p < telemetry_info.plane_count; ++p) {
                uint32_t w = 0u, h = 0u;
                MT_CHECK_EQ_I64(tc_frame_plane_geometry(&telemetry_info, p, &w, &h), TC_OK);
                telemetry_planes[p] = (uint16_t*)calloc((size_t)w * h, sizeof(uint16_t));
                MT_CHECK(telemetry_planes[p] != NULL);
            }
            tc_dev_decode_stats_reset();
            MT_CHECK_EQ_I64(tc_frame_decode_reduced(
                                telemetry_packet, telemetry_size,
                                TC_DECODE_SCALE_QUARTER, telemetry_planes, NULL,
                                &telemetry_info), TC_OK);
            tc_decode_stage_stats telemetry_stats;
            memset(&telemetry_stats, 0, sizeof(telemetry_stats));
            tc_dev_decode_stats_get(&telemetry_stats);
            MT_CHECK_EQ_U64(telemetry_stats.frames, 1u);
            MT_CHECK_EQ_U64(telemetry_stats.packet_bytes_read, telemetry_size);
            MT_CHECK_EQ_U64(telemetry_stats.segments_parsed, telemetry_info.slice_count);
            MT_CHECK_EQ_U64(telemetry_stats.segments_skipped, 0u);
            MT_CHECK(telemetry_stats.entropy_symbols > 0u);
            MT_CHECK(telemetry_stats.idct_samples > 0u);
            MT_CHECK(telemetry_stats.upload_bytes > 0u);
            MT_CHECK(telemetry_stats.wall_ns > 0u);
            MT_CHECK(telemetry_stats.p95_ns >= telemetry_stats.p50_ns);
            MT_CHECK(telemetry_stats.p99_ns >= telemetry_stats.p95_ns);
            for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
                free(telemetry_planes[p]);
            }
            free(telemetry_packet);
        }
    }

    return MT_MAIN_RETURN();
}
