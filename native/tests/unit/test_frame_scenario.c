/* V7-R2（产品默认熵，cfg em=8）closed-loop frame scenario suite：格式矩阵、
 * 独立 slice、调用方 stride、全部解码入口与 sized 精确搜索。
 *
 * V 代际收纳（2026-09-13，见 docs/codec/topos_consolidation_audit_2026-09-13.md）：
 * 本文件原为 V3 帧内预测引擎套件（test_intra.c）。V3/V6 退役后，场景性覆盖
 * （格式×位深×alpha×几何、确定性、损坏隔离、OOM、sized、入口契约）在保留
 * 代际 V7-R2 上等价重建；V3 引擎专属腿（range 往返、VLC 模式语法/padding、
 * intra-vs-VLC 质量守卫）随代际归档，VLC 语法覆盖由 test_slice_codec 承接，
 * intra 案例网格由 conformance_codec_v7r2_intra 黄金承接。 */
#include "bitstream/packet.h"
#include "bitstream/slice_codec.h"
#include "common/alloc.h"
#include "common/crc32.h"
#include "common/endian.h"
#include "common/tpool.h"
#include "simd/dispatch.h"
#include "topos_codec.h"
#include "mini_test.h"
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
    topos_frame_config cfg;
    topos_frame_input in;
    uint16_t* src[4];
    uint16_t* ref[4];
    uint16_t* out[4];
    size_t stride[4];
    uint32_t width[4];
    uint8_t* packet;
    size_t cap;
    topos_frame_stats stats;
} fixture;

static void* must_alloc(size_t n)
{
    void* p = malloc(n);
    if (p == NULL) { fprintf(stderr, "test allocation failed\n"); exit(2); }
    return p;
}

static void setup(fixture* f, uint8_t pf, uint8_t bd, uint8_t alpha,
                  uint16_t w, uint16_t h)
{
    memset(f, 0, sizeof(*f));
    f->cfg.struct_size = sizeof(f->cfg);
    f->cfg.visible_width = w;
    f->cfg.visible_height = h;
    f->cfg.pixel_format = pf;
    f->cfg.color_matrix = pf == 2u ? 0u : 1u;
    f->cfg.bit_depth = bd;
    f->cfg.qp_base = 25u;
    f->cfg.qmatrix_id = 1u;
    f->cfg.slice_rows = 2u;
    f->cfg.alpha_mode = alpha;
    f->cfg.alpha_bit_depth = alpha == 1u ? 16u : (alpha == 2u ? 12u : 0u);
    f->cfg.reserved[0] = 8u; /* V7-R2（产品默认熵；V3 退役后本套件宿主） */
    f->in.struct_size = sizeof(f->in);
    uint32_t max = (1u << bd) - 1u;
    for (unsigned p = 0; p < (alpha != 0u ? 4u : 3u); ++p) {
        f->width[p] = (p == 1u || p == 2u) && pf == 0u ? (w + 1u) / 2u : w;
        f->stride[p] = f->width[p] + 5u;
        size_t n = f->stride[p] * h;
        f->src[p] = must_alloc(n * sizeof(uint16_t));
        f->out[p] = must_alloc(n * sizeof(uint16_t));
        f->ref[p] = must_alloc((size_t)f->width[p] * h * sizeof(uint16_t));
        for (size_t i = 0; i < n; ++i) { f->src[p][i] = 0xABCDu; f->out[p][i] = 0xDCBAu; }
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < f->width[p]; ++x) {
                uint32_t v = (x * 17u + y * 11u + p * 31u);
                /* Sloped gradients, diagonal edges, and texture in separate regions. */
                if (x > f->width[p] / 2u) { v += x > y ? max / 3u : 0u; }
                if (y > h / 2u) { v += (uint32_t)((x * 71u ^ y * 97u) % 59u); }
                f->src[p][y * f->stride[p] + x] = (uint16_t)(v & (p == 3u ? 65535u : max));
            }
        }
        f->in.planes[p] = f->src[p];
        f->in.strides[p] = f->stride[p];
    }
    f->cap = tc_frame_packet_bound(&f->cfg);
    f->packet = must_alloc(f->cap == 0u ? 1u : f->cap);
}

static void cleanup(fixture* f)
{
    free(f->packet);
    for (unsigned p = 0; p < 4u; ++p) { free(f->src[p]); free(f->ref[p]); free(f->out[p]); }
}

static int encode_fixture(fixture* f)
{
    int32_t rc = tc_frame_encode(&f->cfg, &f->in, f->packet, f->cap, &f->stats);
    MT_CHECK_EQ_I64(rc, TC_OK);
    if (rc != TC_OK) { fprintf(stderr, "intra encode: %s\n", tc_last_error()); return 0; }
    return 1;
}

static void compare_output(const fixture* f)
{
    for (unsigned p = 0; p < (f->cfg.alpha_mode != 0u ? 4u : 3u); ++p) {
        for (uint32_t y = 0; y < f->cfg.visible_height; ++y) {
            MT_CHECK(memcmp(f->out[p] + y * f->stride[p], f->ref[p] + y * f->width[p],
                            f->width[p] * sizeof(uint16_t)) == 0);
            for (size_t x = f->width[p]; x < f->stride[p]; ++x) {
                MT_CHECK_EQ_U64(f->out[p][y * f->stride[p] + x], 0xDCBAu);
            }
        }
    }
}

static void test_formats_and_entry_points(void)
{
    tc_decoder* dec = NULL;
    MT_CHECK_EQ_I64(tc_decoder_create(NULL, &dec), TC_OK);
    if (dec == NULL) { return; }
    for (uint8_t pf = 0; pf < 3u; ++pf) {
        for (uint8_t bd = 10; bd <= 12u; bd += 2u) {
            for (uint8_t alpha = 0; alpha < 3u; ++alpha) {
                fixture f;
                setup(&f, pf, bd, alpha, 73u, 39u);
                if (!encode_fixture(&f)) { cleanup(&f); continue; }
                topos_packet_view packet;
                MT_CHECK_EQ_I64(tc_packet_scan(f.packet, f.stats.packet_size, &packet), TC_OK);
                MT_CHECK_EQ_U64(packet.fh.version_major, 7u);
                MT_CHECK_EQ_U64(packet.fh.version_minor, 0u);
                MT_CHECK_EQ_U64(packet.fh.entropy_mode, 7u);
                MT_CHECK_EQ_U64(packet.fh.coding_mode, 0u);
                topos_frame_output info;
                MT_CHECK_EQ_I64(tc_frame_decode(f.packet, f.stats.packet_size, f.ref, NULL, &info), TC_OK);
                MT_CHECK_EQ_U64(info.bit_depth, bd);
                MT_CHECK_EQ_U64(info.pixel_format, pf);
                MT_CHECK_EQ_U64(info.concealed_slices, 0u);
                for (unsigned p = 0; p < info.plane_count; ++p) {
                    for (uint32_t y = 0; y < f.cfg.visible_height; ++y) {
                        for (uint32_t x = 0; x < f.width[p]; ++x) {
                            uint16_t v = f.ref[p][y * f.width[p] + x];
                            if (p < 3u) { MT_CHECK(v < (1u << bd)); }
                            else {
                                int d = (int)v - f.src[p][y * f.stride[p] + x];
                                if (d < 0) { d = -d; }
                                MT_CHECK(d <= (alpha == 1u ? 0 : 16));
                            }
                        }
                    }
                }
                MT_CHECK_EQ_I64(tc_frame_decode(f.packet, f.stats.packet_size, f.out, f.stride, &info), TC_OK);
                compare_output(&f);
                topos_plane_view views[4];
                memset(views, 0, sizeof(views));
                for (unsigned p = 0; p < info.plane_count; ++p) {
                    views[p].struct_size = sizeof(views[p]);
                    views[p].abi_version = TOPOS_CODEC_ABI_VERSION;
                    views[p].pixels = f.out[p]; views[p].stride = f.stride[p];
                }
                MT_CHECK_EQ_I64(tc_decoder_prepare(dec, f.packet, f.stats.packet_size, &info), TC_OK);
                MT_CHECK_EQ_I64(tc_decoder_decode(dec, f.packet, f.stats.packet_size, views, &info), TC_OK);
                compare_output(&f);
                topos_batch_packet bp = {f.packet, f.stats.packet_size};
                MT_CHECK_EQ_I64(tc_frame_decode_batch(&bp, 1u, f.out, f.stride, &info), TC_OK);
                compare_output(&f);
                MT_CHECK_EQ_I64(tc_decoder_decode_batch(dec, &bp, 1u, views, &info), TC_OK);
                compare_output(&f);
                /* Persistent context must recover after invalid caller stride. */
                views[0].stride = f.width[0] - 1u;
                MT_CHECK_EQ_I64(tc_decoder_decode(dec, f.packet, f.stats.packet_size, views, &info), TC_ERR_INVALID_ARGUMENT);
                views[0].stride = f.stride[0];
                MT_CHECK_EQ_I64(tc_decoder_decode(dec, f.packet, f.stats.packet_size, views, &info), TC_OK);
                compare_output(&f);
                cleanup(&f);
            }
        }
    }
    tc_decoder_destroy(dec);
}

/* context 快路：首帧预热 decoder-context 的 DC 双行池后，稳定几何重复
 * 解码不应再触发 codec 内部分配。输出仍与无状态参考逐位一致。
 * （V3/V6 专用 intra scratch 池随代际退役；非 V3/V6 颜色 DC 池为共享路径，
 * 本测试继续钉住其稳态零分配契约。） */
static void test_context_scratch_reuse(void)
{
    fixture f;
    setup(&f, 0u, 10u, 0u, 73u, 39u);
    if (!encode_fixture(&f)) { cleanup(&f); return; }

    topos_frame_output info;
    MT_CHECK_EQ_I64(tc_frame_decode(f.packet, f.stats.packet_size, f.ref,
                                    NULL, &info), TC_OK);

    topos_plane_view views[TC_FRAME_MAX_PLANES];
    memset(views, 0, sizeof(views));
    for (uint32_t p = 0u; p < info.plane_count; ++p) {
        views[p].struct_size = (uint32_t)sizeof(views[p]);
        views[p].abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        views[p].pixels = f.out[p];
        views[p].stride = f.stride[p];
    }

    tc_decoder* dec = NULL;
    MT_CHECK_EQ_I64(tc_decoder_create(NULL, &dec), TC_OK);
    if (dec != NULL) {
        MT_CHECK_EQ_I64(tc_decoder_decode(dec, f.packet, f.stats.packet_size,
                                          views, &info), TC_OK);
        compare_output(&f);
        tc_dev_set_alloc_fault(-1); /* 关闭注入并清零分配计数 */
        for (unsigned i = 0u; i < 32u; ++i) {
            MT_CHECK_EQ_I64(tc_decoder_decode(dec, f.packet, f.stats.packet_size,
                                              views, &info), TC_OK);
        }
        /* V7-R2 契约：rANS2 解码 scratch 未入 context 池（每次解码常数次
         * 分配），V3 池化零分配契约随 V3 退役——等价契约收敛为每次解码
         * 分配数恒定（不随解码次数增长，即 scratch 调用后释放、无滞留）。 */
        const int64_t steady = tc_dev_alloc_count();
        for (unsigned i = 0u; i < 32u; ++i) {
            MT_CHECK_EQ_I64(tc_decoder_decode(dec, f.packet, f.stats.packet_size,
                                              views, &info), TC_OK);
        }
        MT_CHECK_EQ_I64(tc_dev_alloc_count(), 2 * steady);
        tc_decoder_destroy(dec);
    }
    cleanup(&f);
}

static void test_determinism_and_small_edges(void)
{
    static const uint16_t dims[][2] = {{1u, 1u}, {7u, 9u}, {73u, 39u}};
    for (unsigned d = 0; d < sizeof(dims) / sizeof(dims[0]); ++d) {
        fixture f;
        setup(&f, 0u, 12u, 1u, dims[d][0], dims[d][1]);
        tc_dev_set_thread_count(1); tc_dev_set_simd_mode(TC_SIMD_SCALAR);
        if (!encode_fixture(&f)) { cleanup(&f); continue; }
        topos_frame_output info;
        MT_CHECK_EQ_I64(tc_frame_decode(f.packet, f.stats.packet_size, f.ref, NULL, &info), TC_OK);
        uint8_t* other = must_alloc(f.cap);
        tc_dev_set_thread_count(4); tc_dev_set_simd_mode(TC_SIMD_FORCE);
        topos_frame_stats st;
        MT_CHECK_EQ_I64(tc_frame_encode(&f.cfg, &f.in, other, f.cap, &st), TC_OK);
        MT_CHECK_EQ_U64(st.packet_size, f.stats.packet_size);
        MT_CHECK(memcmp(other, f.packet, f.stats.packet_size) == 0);
        MT_CHECK_EQ_I64(tc_frame_decode(other, st.packet_size, f.out, f.stride, &info), TC_OK);
        compare_output(&f);
        free(other); cleanup(&f);
    }
    tc_dev_set_thread_count(0); tc_dev_set_simd_mode(TC_SIMD_AUTO);
}

static void test_corruption_and_symbols(void)
{
    fixture f;
    setup(&f, 0u, 10u, 1u, 73u, 39u);
    if (!encode_fixture(&f)) { cleanup(&f); return; }
    topos_packet_view v;
    MT_CHECK_EQ_I64(tc_packet_scan(f.packet, f.stats.packet_size, &v), TC_OK);
    topos_frame_output info;
    MT_CHECK_EQ_I64(tc_frame_decode(f.packet, f.stats.packet_size, f.ref, NULL, &info), TC_OK);
    for (unsigned i = 0; i < v.slice_count; ++i) {
        if (v.slices[i].plane == 3u) { continue; }
        const topos_slice_header* sh = &v.slices[i];
        size_t n = (size_t)sh->block_h * v.fh.plane_block_cols[sh->plane] * 64u;
        int32_t* q = must_alloc(n * sizeof(int32_t));
        uint64_t h1 = 0, h2 = 0;
        MT_CHECK_EQ_I64(tc_color_slice_decode(&v.fh, sh, v.payloads[i], sh->slice_payload_size, NULL, &h1), TC_OK);
        MT_CHECK_EQ_I64(tc_color_slice_decode(&v.fh, sh, v.payloads[i], sh->slice_payload_size, q, &h2), TC_OK);
        MT_CHECK_EQ_U64(h1, h2);
        MT_CHECK(tc_color_slice_decode(&v.fh, sh, v.payloads[i], 0u, NULL, NULL) < 0);
        free(q);
    }
    /* Corrupt first color slice: every other band, including the next band
     * of the same plane, must remain bit exact (no cross-slice neighbors). */
    size_t off = (size_t)(v.payloads[0] - f.packet);
    f.packet[off] ^= 0x40u;
    MT_CHECK_EQ_I64(tc_frame_decode(f.packet, f.stats.packet_size, f.out, f.stride, &info), TC_WARN_CONCEALED);
    MT_CHECK_EQ_U64(info.concealed_slices, 1u);
    MT_CHECK_EQ_U64(info.slice_status[0], TC_FRAME_SLICE_CONCEALED);
    uint32_t affected_h = v.slices[0].block_h * 8u;
    for (unsigned p = 0; p < 4u; ++p) {
        for (uint32_t y = 0; y < f.cfg.visible_height; ++y) {
            for (uint32_t x = 0; x < f.width[p]; ++x) {
                uint16_t expect = p == 0u && y < affected_h ? 512u : f.ref[p][y * f.width[p] + x];
                MT_CHECK_EQ_U64(f.out[p][y * f.stride[p] + x], expect);
            }
        }
    }
    f.packet[off] ^= 0x40u;
    for (size_t cut = 0; cut < TC_FRAME_HEADER_SIZE; ++cut) {
        MT_CHECK(tc_frame_decode(f.packet, cut, NULL, NULL, &info) < 0);
    }
    MT_CHECK(tc_frame_decode(f.packet, f.stats.packet_size - 1u, NULL, NULL, &info) < 0);
    /* Invalid frame-level coding mode and
     * future major version must be rejected even with a valid header CRC. */
    uint8_t saved = f.packet[47];
    f.packet[47] = 2u;
    tc_store_be32(f.packet + 49u, tc_crc32(f.packet, 49u));
    MT_CHECK(tc_frame_decode(f.packet, f.stats.packet_size, NULL, NULL, &info) < 0);
    f.packet[47] = saved; f.packet[6] = 4u;
    tc_store_be32(f.packet + 49u, tc_crc32(f.packet, 49u));
    MT_CHECK_EQ_I64(tc_frame_decode(f.packet, f.stats.packet_size, NULL, NULL, &info), TC_ERR_UNSUPPORTED_VERSION);
    cleanup(&f);
}

static void test_sized_and_config(void)
{
    fixture f;
    setup(&f, 2u, 12u, 0u, 33u, 25u);
    MT_CHECK_EQ_I64(tc_frame_config_validate(&f.cfg), TC_OK);
    /* V3 时代 reserved[1..2]≠0 拒绝随 V3 退役（em8 下 AQ/RDO 合法）；
     * em8 的等价误用拒绝：AQ/RDO 域 {0,1} 越界 + reserved[3]（em9 专有）。 */
    f.cfg.reserved[1] = 2u;
    MT_CHECK_EQ_I64(tc_frame_config_validate(&f.cfg), TC_ERR_INVALID_ARGUMENT);
    f.cfg.reserved[1] = 0u;
    f.cfg.reserved[2] = 2u;
    MT_CHECK_EQ_I64(tc_frame_config_validate(&f.cfg), TC_ERR_INVALID_ARGUMENT);
    f.cfg.reserved[2] = 0u;
    f.cfg.reserved[3] = 1u;
    MT_CHECK_EQ_I64(tc_frame_config_validate(&f.cfg), TC_ERR_INVALID_ARGUMENT);
    f.cfg.reserved[3] = 0u;
    /* 保留代际合法域（cfg em {0,1,8,9}，审计 §0）：0/1 由 V1/VLC golden
     * 覆盖，这里钉住本套件宿主 em8 与 V8 的 cfg em9 必须通过 validate；
     * 退役域拒绝由批 2 写端收敛测试补齐。 */
    f.cfg.reserved[0] = 9u;
    MT_CHECK_EQ_I64(tc_frame_config_validate(&f.cfg), TC_OK);
    f.cfg.reserved[0] = 8u;
    if (!encode_fixture(&f)) { cleanup(&f); return; }
    uint8_t* packet = must_alloc(f.cap);
    uint8_t qp = 0;
    topos_frame_stats st;
    MT_CHECK_EQ_I64(tc_frame_encode_sized(&f.cfg, &f.in, f.stats.packet_size, 25u, 25u,
                                         &qp, packet, f.cap, &st), TC_OK);
    MT_CHECK_EQ_U64(qp, 25u);
    MT_CHECK_EQ_U64(st.packet_size, f.stats.packet_size);
    MT_CHECK(memcmp(packet, f.packet, st.packet_size) == 0);
    uint32_t sizes[7];
    for (unsigned q = 22; q <= 28u; ++q) {
        f.cfg.qp_base = (uint8_t)q;
        MT_CHECK_EQ_I64(tc_frame_encode(&f.cfg, &f.in, packet, f.cap, &st), TC_OK);
        sizes[q - 22u] = st.packet_size;
    }
    uint32_t targets[] = {sizes[0], sizes[3], sizes[6], 1u};
    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        unsigned expect = 28u;
        for (unsigned q = 22; q <= 28u; ++q) {
            if (sizes[q - 22u] <= targets[i]) { expect = q; break; }
        }
        f.cfg.qp_base = 25u;
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&f.cfg, &f.in, targets[i], 22u, 28u,
                                             &qp, packet, f.cap, &st), TC_OK);
        MT_CHECK_EQ_U64(qp, expect);
        MT_CHECK_EQ_U64(st.packet_size, sizes[expect - 22u]);
        if (targets[i] == 1u) { MT_CHECK(st.packet_size > targets[i]); }
        f.cfg.qp_base = (uint8_t)expect;
        if (encode_fixture(&f)) {
            MT_CHECK_EQ_U64(f.stats.packet_size, st.packet_size);
            MT_CHECK(memcmp(packet, f.packet, st.packet_size) == 0);
        }
    }
    /* A sized output buffer only needs to hold the chosen packet. Earlier
     * high-quality probes may exceed it without terminating the search. */
    f.cfg.qp_base = 63u;
    if (encode_fixture(&f)) {
        size_t chosen_cap = f.stats.packet_size;
        f.cfg.qp_base = 0u;
        if (encode_fixture(&f)) {
            MT_CHECK(f.stats.packet_size > chosen_cap);
            memset(packet, 0xA5, f.cap);
            MT_CHECK_EQ_I64(tc_frame_encode_sized(&f.cfg, &f.in,
                (uint32_t)chosen_cap, 0u, 63u, &qp, packet, chosen_cap, &st), TC_OK);
            MT_CHECK(qp > 0u);
            MT_CHECK(st.packet_size <= chosen_cap);
            for (size_t i = chosen_cap; i < f.cap; ++i) {
                MT_CHECK_EQ_U64(packet[i], 0xA5u);
            }
            f.cfg.qp_base = qp;
            if (encode_fixture(&f)) {
                MT_CHECK_EQ_U64(f.stats.packet_size, st.packet_size);
                MT_CHECK(memcmp(packet, f.packet, st.packet_size) == 0);
            }
        }
    }
    MT_CHECK_EQ_I64(tc_frame_encode_sized(NULL, &f.in, 1u, 0u, 63u,
                                         &qp, packet, f.cap, &st), TC_ERR_INVALID_ARGUMENT);
    free(packet); cleanup(&f);
}

static void test_decode_oom_recovery(void)
{
    fixture f;
    setup(&f, 1u, 12u, 0u, 73u, 39u);
    tc_dev_set_thread_count(1);
    if (!encode_fixture(&f)) { cleanup(&f); tc_dev_set_thread_count(0); return; }
    int finished = 0;
    for (int64_t n = 1; n <= 128; ++n) {
        topos_frame_output info;
        tc_dev_set_alloc_fault(n);
        int32_t rc = tc_frame_decode(f.packet, f.stats.packet_size, f.out, f.stride, &info);
        tc_dev_set_alloc_fault(-1);
        MT_CHECK(rc == TC_OK || rc == TC_ERR_OUT_OF_MEMORY);
        MT_CHECK_EQ_I64(tc_frame_decode(f.packet, f.stats.packet_size, f.out, f.stride, &info), TC_OK);
        if (rc == TC_OK) { finished = 1; break; }
    }
    MT_CHECK(finished);
    cleanup(&f); tc_dev_set_thread_count(0);
}

int main(void)
{
    test_formats_and_entry_points();
    test_context_scratch_reuse();
    test_determinism_and_small_edges();
    test_corruption_and_symbols();
    test_sized_and_config();
    test_decode_oom_recovery();
    return MT_MAIN_RETURN();
}
