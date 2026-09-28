/* V9 批 1：格式骨架——头部版本机（major=9 三元组 (9,0,0) + GOP 字段强校验）、
 * cfg 写域 {0..10}（em=10）、包结构扫描（V8 同构）、解码/编码入口
 * NOT_IMPLEMENTED 占位、GOP context 序列状态机（零像素重建）、截断扫描。
 *
 * 合成几何：coded 64×320，422 10bit（布局与 test_v8_format 同构——V9 包
 * 布局 = V8 同构继承，仅 major/熵三元组/GOP 字段不同）。 */
#include "bitstream/frame_header.h"
#include "bitstream/packet.h"

#include <stdlib.h>
#include <string.h>

#include "common/crc32.h"
#include "common/endian.h"
#include "common/error.h"
#include "mini_test.h"
#include "topos_codec.h"

#define PLANE_COUNT 3u
#define SB 16u
#define TILE_ROWS 16u
#define COLS0 8u
#define COLSC 4u
#define ROWS 40u
#define T_P 3u
#define TILE_COUNT (T_P * PLANE_COUNT)
#define SEG_LEN 4u

static uint32_t v9_cols(uint32_t p) { return p == 0u ? COLS0 : COLSC; }

static topos_frame_header v9_base_fh(uint32_t packet_size)
{
    topos_frame_header fh;
    memset(&fh, 0, sizeof(fh));
    fh.version_major = 9u;
    fh.version_minor = 0u;
    fh.profile = 3u;
    fh.pixel_format = 0u;
    fh.bit_depth = 10u;
    fh.alpha_mode = 0u;
    fh.coded_width = 64u;
    fh.coded_height = 320u;
    fh.visible_width = 60u;
    fh.visible_height = 316u;
    fh.plane_count = PLANE_COUNT;
    fh.qmatrix_id = 0u;
    fh.qp_base = 30u;
    fh.slice_count = (uint16_t)TILE_COUNT;
    fh.color_range = 1u;
    fh.color_primaries = 1u;
    fh.color_transfer = 1u;
    fh.color_matrix = 1u;
    fh.entropy_mode = 9u;
    /* frame_type=0(I)/ref_distance=0/gop_id=0 缺省 */
    fh.frame_packet_size = packet_size;
    return fh;
}

static uint8_t* build_v9_packet(size_t* size_out, uint8_t sb_log2, uint8_t tr_log2,
                                uint16_t slice_count, uint32_t size_override,
                                uint8_t frame_type, uint16_t gop_id, uint8_t ref_distance)
{
    const uint32_t sb = 1u << sb_log2;
    const uint32_t tile_rows = tr_log2 == 0u ? 0u : (1u << tr_log2);
    size_t total = TC_FRAME_HEADER_SIZE + TC_V8_EXT_HEADER_SIZE;
    uint32_t segs[4] = {0u, 0u, 0u, 0u};
    uint32_t tiles_p = 0u;
    for (uint32_t p = 0u; p < PLANE_COUNT; ++p) {
        segs[p] = ((ROWS * v9_cols(p)) + sb - 1u) / sb;
        tiles_p = (tile_rows == 0u) ? 1u : (ROWS + tile_rows - 1u) / tile_rows;
        total += (size_t)segs[p] * TC_V8_DIR_ENTRY_BYTES
               + (size_t)tiles_p * TC_V8_TABLE_BYTES
               + (size_t)segs[p] * SEG_LEN
               + (size_t)tiles_p * 4u;
    }
    const size_t buf_cap = total + 1u;
    uint8_t* buf = (uint8_t*)calloc(1u, buf_cap);
    if (buf == NULL) { return NULL; }

    topos_frame_header fh = v9_base_fh((uint32_t)(size_override != 0u ? size_override : total));
    fh.slice_count = slice_count;
    fh.frame_type = frame_type;
    fh.gop_id = gop_id;
    fh.ref_distance = ref_distance;
    if (tc_frame_header_encode(&fh, buf) != TC_OK) { free(buf); return NULL; }

    buf[TC_FRAME_HEADER_SIZE] = sb_log2;
    buf[TC_FRAME_HEADER_SIZE + 1u] = tr_log2;

    size_t off = TC_FRAME_HEADER_SIZE + TC_V8_EXT_HEADER_SIZE;
    for (uint32_t p = 0u; p < PLANE_COUNT; ++p) {
        const uint32_t cols = v9_cols(p);
        uint32_t cum = 0u;
        for (uint32_t s = 0u; s < segs[p]; ++s) {
            uint8_t* e = buf + off + (size_t)s * TC_V8_DIR_ENTRY_BYTES;
            tc_store_be32(e, cum);
            tc_store_be32(e + 4u, SEG_LEN);
            e[8] = 64u; /* qp_delta_biased = +0 */
            cum += SEG_LEN;
        }
        off += (size_t)segs[p] * TC_V8_DIR_ENTRY_BYTES;
        for (uint32_t t = 0u; t < tiles_p; ++t) {
            uint8_t* tb = buf + off + (size_t)t * TC_V8_TABLE_BYTES;
            tb[0] = 0u;
            for (uint32_t i = 1u; i < TC_V8_TABLE_BYTES; ++i) {
                tb[i] = (uint8_t)(i * 7u + t);
            }
        }
        off += (size_t)tiles_p * TC_V8_TABLE_BYTES;
        const uint32_t stream_base = (uint32_t)off;
        off += (size_t)segs[p] * SEG_LEN;
        const uint32_t blocks = ROWS * cols;
        for (uint32_t t = 0u; t < tiles_p; ++t) {
            const uint64_t fb = tile_rows == 0u ? 0ull : (uint64_t)t * tile_rows * cols;
            const uint64_t fn = tile_rows == 0u ? (uint64_t)blocks
                                                : (uint64_t)(t + 1u) * tile_rows * cols;
            uint32_t sf = (uint32_t)(fb / sb);
            uint32_t sn = (uint32_t)(fn / sb);
            if (sf > segs[p]) { sf = segs[p]; }
            if (sn > segs[p]) { sn = segs[p]; }
            if (sn < sf) { sn = sf; }
            uint32_t crc = tc_crc32_update(0u,
                                           buf + stream_base
                                               - (size_t)tiles_p * TC_V8_TABLE_BYTES
                                               + (size_t)t * TC_V8_TABLE_BYTES,
                                           TC_V8_TABLE_BYTES);
            crc = tc_crc32_update(crc, buf + stream_base + (size_t)sf * SEG_LEN,
                                  (size_t)(sn - sf) * SEG_LEN);
            tc_store_be32(buf + off + (size_t)t * 4u, crc);
        }
        off += (size_t)tiles_p * 4u;
    }
    *size_out = total;
    return buf;
}

static topos_frame_config v9_cfg(void)
{
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = 60u;
    cfg.visible_height = 316u;
    cfg.profile = 3u;
    cfg.pixel_format = 0u;
    cfg.bit_depth = 10u;
    cfg.qmatrix_id = 0u;
    cfg.qp_base = 30u;
    cfg.color_range = 1u;
    cfg.color_primaries = 1u;
    cfg.color_transfer = 1u;
    cfg.color_matrix = 1u;
    cfg.reserved[0] = 10u;
    return cfg;
}

static int status_is_defined(int32_t rc)
{
    return rc == TC_OK || rc == TC_WARN_CONCEALED || rc == TC_ERR_INVALID_ARGUMENT ||
           rc == TC_ERR_OUT_OF_MEMORY || rc == TC_ERR_UNSUPPORTED_VERSION ||
           rc == TC_ERR_UNSUPPORTED_PROFILE || rc == TC_ERR_UNSUPPORTED_PIXEL_FORMAT ||
           rc == TC_ERR_UNSUPPORTED_MATRIX || rc == TC_ERR_UNSUPPORTED_ALPHA_MODE ||
           rc == TC_ERR_LIMIT_EXCEEDED || rc == TC_ERR_MALFORMED ||
           rc == TC_ERR_TRUNCATED || rc == TC_ERR_CHECKSUM_MISMATCH ||
           rc == TC_ERR_NOT_IMPLEMENTED || rc == TC_ERR_REFERENCE_INVALID;
}

int main(void)
{
    /* ---- 头部版本机：V9 三元组 (9,0,0)、minor 恒 0 ---- */
    {
        topos_frame_header fh = v9_base_fh(12345u);
        uint8_t enc[TC_FRAME_HEADER_SIZE];
        MT_CHECK_EQ_I64(tc_frame_header_encode(&fh, enc), TC_OK);
        topos_frame_header back;
        MT_CHECK_EQ_I64(tc_frame_header_decode(enc, sizeof(enc), &back), TC_OK);
        MT_CHECK_EQ_U64(back.version_major, 9ull);
        MT_CHECK_EQ_U64(back.entropy_mode, 9ull);
        MT_CHECK_EQ_U64(back.frame_type, 0ull);

        topos_frame_header bad = fh;
        bad.entropy_mode = 8u; /* V9 只认 (9,0,0) */
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_UNSUPPORTED_VERSION);
        bad = fh;
        bad.codebook_version = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_UNSUPPORTED_VERSION);
        bad = fh;
        bad.coding_mode = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_UNSUPPORTED_VERSION);
        bad = fh;
        bad.version_minor = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_UNSUPPORTED_VERSION);

        /* ---- GOP 字段规则组（major=9 专属；V8 强制零不变） ---- */
        bad = fh;
        bad.frame_type = 1u;
        bad.ref_distance = 1u;
        bad.gop_id = 7u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_OK);
        MT_CHECK_EQ_I64(tc_frame_header_decode(enc, sizeof(enc), &back), TC_OK);
        MT_CHECK_EQ_U64(back.frame_type, 1ull);
        MT_CHECK_EQ_U64(back.gop_id, 7ull);
        MT_CHECK_EQ_U64(back.ref_distance, 1ull);

        bad = fh;
        bad.frame_type = 0u;
        bad.ref_distance = 1u; /* I 帧不得带 ref */
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_MALFORMED);
        bad = fh;
        bad.frame_type = 1u;
        bad.ref_distance = 0u; /* P 帧 ref 恒 1 */
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_MALFORMED);
        bad = fh;
        bad.frame_type = 2u; /* P0 无更高帧型（ADR-C047） */
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_UNSUPPORTED_VERSION);
        bad = fh;
        bad.frame_type = 1u;
        bad.ref_distance = 1u;
        bad.version_major = 8u;
        bad.entropy_mode = 8u; /* V8：GOP 字段仍强制零 */
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_UNSUPPORTED_VERSION);
    }

    /* ---- cfg 写域 {0..10}：em=10 合法；em=11 越界；退役 2..7 不变 ---- */
    {
        topos_frame_config cfg = v9_cfg();
        MT_CHECK_EQ_I64(tc_frame_config_validate(&cfg), TC_OK);

        topos_frame_config bad = cfg;
        bad.reserved[0] = 11u; /* V7-R3（ADR-C048）：cfg 域扩至 11，合法 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_OK);
        bad = cfg;
        bad.reserved[0] = 12u; /* 越界探针随之上移 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = cfg;
        bad.reserved[0] = 3u; /* V3 退役（写端显式拒绝） */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = cfg;
        bad.reserved[0] = 10u;
        bad.reserved[3] = 2u; /* frame_type 只有 {0,1} */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = cfg;
        bad.reserved[0] = 10u;
        bad.reserved[3] = 1u;
        bad.reserved[5] = 0u; /* P 帧 ref_distance 须 1 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = cfg;
        bad.reserved[0] = 10u;
        bad.reserved[6] = 1u; /* rsv_end=6：reserved[6..7] 恒 0 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);

        /* em=8（V7-R2 产品 intra）：reserved[5] = 输出特性（V7-R4/R5，
         * 锯齿战役 P6 2026-09-21）——位域 {0..3} 且产品域合法；
         * em=9（V8 段化试验）reserved[5] 仍须 0（槽位边界不变） */
        bad = cfg;
        bad.reserved[0] = 8u;
        bad.reserved[5] = 3u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_OK);
        bad.reserved[5] = 4u; /* 未定义特性位 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = cfg;
        bad.reserved[0] = 8u;
        bad.reserved[5] = 1u;
        bad.bit_depth = 12u; /* 特性位域外：非 10-bit */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = cfg;
        bad.reserved[0] = 9u;
        bad.reserved[5] = 1u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
    }

    /* ---- 结构扫描（V8 同构）+ 探测/拒绝矩阵 ---- */
    size_t total = 0u;
    uint8_t* pkt = build_v9_packet(&total, 4u, 4u, (uint16_t)TILE_COUNT, 0u,
                                   0u, 0u, 0u);
    MT_CHECK(pkt != NULL);
    if (pkt == NULL) { return MT_MAIN_RETURN(); }

    MT_CHECK_EQ_U64(tc_packet_is_v9(pkt, total), 1ull);
    MT_CHECK_EQ_U64(tc_packet_is_v8(pkt, total), 0ull);
    {
        topos_v8_packet_view view;
        MT_CHECK_EQ_I64(tc_packet_scan_v8(pkt, total, &view), TC_OK);
        MT_CHECK_EQ_U64(view.segment_blocks, 16ull);
        MT_CHECK_EQ_U64(view.tile_count, TILE_COUNT);
        MT_CHECK_EQ_U64(view.fh.version_major, 9ull);
        /* V7 扫描入口拒绝 major 9（与 major 8 同位） */
        topos_packet_view legacy;
        MT_CHECK_EQ_I64(tc_packet_parse_structure(pkt, total, &legacy),
                        TC_ERR_UNSUPPORTED_VERSION);
    }

    /* ---- 解码入口：查询 OK；P 帧无参考 → TC_ERR_STATE（批 2 起
     *      I 帧直连 V8 像素机，真编解码语义归 test_v9_codec / golden_v9）---- */
    {
        topos_frame_output info;
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t)sizeof(info);
        info.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        MT_CHECK_EQ_I64(tc_frame_decode(pkt, total, NULL, NULL, &info), TC_OK);
        MT_CHECK_EQ_U64(info.visible_width, 60ull);
        MT_CHECK_EQ_U64(info.visible_height, 316ull);
        MT_CHECK_EQ_U64(info.plane_count, PLANE_COUNT);

        size_t total_p = 0u;
        uint8_t* pkt_p = build_v9_packet(&total_p, 4u, 4u, (uint16_t)TILE_COUNT, 0u,
                                         1u, 3u, 1u);
        static uint16_t buf_y[64 * 320];
        static uint16_t buf_c[32 * 320];
        uint16_t* planes[TC_FRAME_MAX_PLANES] = { buf_y, buf_c, buf_c, NULL };
        MT_CHECK(pkt_p != NULL);
        if (pkt_p != NULL) {
            MT_CHECK_EQ_I64(tc_frame_decode(pkt_p, total_p, NULL, NULL, &info), TC_OK);
            MT_CHECK_EQ_U64(info.visible_width, 60ull);
            MT_CHECK_EQ_I64(tc_frame_decode(pkt_p, total_p, planes, NULL, &info),
                            TC_ERR_STATE);
            /* 查询模式对 P 包仍开放（GOP 元数据可查） */
            MT_CHECK_EQ_I64(tc_frame_decode(pkt_p, total_p, NULL, NULL, &info), TC_OK);
            free(pkt_p);
        }
        MT_CHECK_EQ_I64(tc_frame_decode_scaled(pkt, total, 30u, 158u, planes, NULL, &info),
                        TC_ERR_NOT_IMPLEMENTED);
    }

    /* ---- 编码入口（批 2）：I/P 都可直连编码（残差语义归 GOP context；
     *      真编解码 roundtrip 归 test_v9_codec / golden_v9）---- */
    {
        topos_frame_config cfg = v9_cfg();
        cfg.reserved[3] = 0u; /* I */
        topos_frame_input in;
        memset(&in, 0, sizeof(in));
        in.struct_size = (uint32_t)sizeof(in);
        in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        static uint16_t py[64 * 320];
        static uint16_t pc[32 * 320];
        for (size_t i = 0; i < 64u * 320u; ++i) { py[i] = 512u; }
        for (size_t i = 0; i < 32u * 320u; ++i) { pc[i] = 512u; }
        in.planes[0] = py;
        in.planes[1] = pc;
        in.planes[2] = pc;
        in.strides[0] = 64u;
        in.strides[1] = 32u;
        in.strides[2] = 32u;
        uint8_t out[1 << 16];
        topos_frame_stats st;
        memset(&st, 0, sizeof(st));
        st.struct_size = (uint32_t)sizeof(st);
        st.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, out, sizeof(out), &st), TC_OK);
        MT_CHECK(st.packet_size > 0u);
        cfg.reserved[3] = 1u;
        cfg.reserved[5] = 1u;
        MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, out, sizeof(out), &st), TC_OK);
    }

    /* ---- 写口径回归（V9 复审 2026-09-14）---- */
    {
        topos_frame_config cfg = v9_cfg();
        topos_frame_input in;
        memset(&in, 0, sizeof(in));
        in.struct_size = (uint32_t)sizeof(in);
        in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        static uint16_t py[64 * 320];
        static uint16_t pc[32 * 320];
        for (size_t i = 0; i < 64u * 320u; ++i) { py[i] = 400u + (uint16_t)(i % 97u); }
        for (size_t i = 0; i < 32u * 320u; ++i) { pc[i] = 512u; }
        in.planes[0] = py;
        in.planes[1] = pc;
        in.planes[2] = pc;
        in.strides[0] = 64u;
        in.strides[1] = 32u;
        in.strides[2] = 32u;
        uint8_t out[1 << 16];
        topos_frame_stats st;
        memset(&st, 0, sizeof(st));
        st.struct_size = (uint32_t)sizeof(st);
        st.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;

        /* sized 码控不是 V9 写口（固定锚 qp P0；qp 搜索绕过 I/P 决策） */
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&cfg, &in, 4096u, 20u, 60u,
                                              NULL, out, sizeof(out), &st),
                        TC_ERR_NOT_IMPLEMENTED);

        /* AQ/RDO + V9：确定性降级为关（m7 F-cache 写端不是 V8/V9 机器——
         * 旧实现会产出 major9 头 + legacy Rice payload 的坏包）。降级 =
         * 输出与 AQ/RDO 全关逐字节一致。 */
        static uint8_t plain_pkt[1 << 16];
        size_t plain_sz = 0u;
        MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, plain_pkt, sizeof(plain_pkt), &st), TC_OK);
        plain_sz = st.packet_size;
        topos_frame_config aq = cfg;
        aq.reserved[1] = 1u; /* AQ on */
        MT_CHECK_EQ_I64(tc_frame_encode(&aq, &in, out, sizeof(out), &st), TC_OK);
        MT_CHECK_EQ_U64(st.packet_size, plain_sz);
        MT_CHECK(memcmp(out, plain_pkt, plain_sz) == 0);
        topos_frame_config rdo = cfg;
        rdo.reserved[2] = 1u; /* RDO on */
        MT_CHECK_EQ_I64(tc_frame_encode(&rdo, &in, out, sizeof(out), &st), TC_OK);
        MT_CHECK_EQ_U64(st.packet_size, plain_sz);
        MT_CHECK(memcmp(out, plain_pkt, plain_sz) == 0);
    }

    /* ---- GOP context：序列状态机（零像素重建） ---- */
    {
        topos_frame_config cfg = v9_cfg();
        topos_gop_context* ctx = NULL;
        MT_CHECK_EQ_I64(tc_gop_context_create(&cfg, &ctx), TC_OK);
        topos_gop_frame_info gi;
        size_t sz_i = 0u, sz_p1 = 0u, sz_p2 = 0u, sz_p3 = 0u;
        uint8_t* p_i = build_v9_packet(&sz_i, 4u, 4u, (uint16_t)TILE_COUNT, 0u, 0u, 5u, 0u);
        uint8_t* p_p1 = build_v9_packet(&sz_p1, 4u, 4u, (uint16_t)TILE_COUNT, 0u, 1u, 5u, 1u);
        uint8_t* p_p2 = build_v9_packet(&sz_p2, 4u, 4u, (uint16_t)TILE_COUNT, 0u, 1u, 6u, 1u);
        uint8_t* p_i2 = build_v9_packet(&sz_p3, 4u, 4u, (uint16_t)TILE_COUNT, 0u, 0u, 6u, 0u);
        MT_CHECK(p_i && p_p1 && p_p2 && p_i2);
        uint32_t state = 0u;
        uint16_t gop = 0u;

        /* 首帧 P → MALFORMED（状态不提交，仍 NO_REF） */
        MT_CHECK_EQ_I64(tc_gop_context_observe(ctx, p_p1, sz_p1, &gi),
                        TC_ERR_MALFORMED);
        MT_CHECK_EQ_I64(tc_gop_context_state(ctx, &state, &gop), TC_OK);
        MT_CHECK_EQ_U64(state, (uint32_t)TOPOS_GOP_NO_REF);

        /* I → REF_READY，gop_id=5 */
        MT_CHECK_EQ_I64(tc_gop_context_observe(ctx, p_i, sz_i, &gi), TC_OK);
        MT_CHECK_EQ_U64(gi.frame_type, 0ull);
        MT_CHECK_EQ_U64(gi.sample_index, 0ull);
        MT_CHECK_EQ_U64(gi.ref_state, (uint32_t)TOPOS_GOP_REF_READY);
        MT_CHECK_EQ_I64(tc_gop_context_state(ctx, &state, &gop), TC_OK);
        MT_CHECK_EQ_U64(state, (uint32_t)TOPOS_GOP_REF_READY);
        MT_CHECK_EQ_U64(gop, 5ull);

        /* 同 GOP P → OK */
        MT_CHECK_EQ_I64(tc_gop_context_observe(ctx, p_p1, sz_p1, &gi), TC_OK);
        MT_CHECK_EQ_U64(gi.sample_index, 1ull);

        /* 跨 GOP P（gop_id=6）→ MALFORMED；参考不被污染 */
        MT_CHECK_EQ_I64(tc_gop_context_observe(ctx, p_p2, sz_p2, NULL),
                        TC_ERR_MALFORMED);
        MT_CHECK_EQ_I64(tc_gop_context_state(ctx, &state, &gop), TC_OK);
        MT_CHECK_EQ_U64(state, (uint32_t)TOPOS_GOP_REF_READY);
        MT_CHECK_EQ_U64(gop, 5ull);

        /* V8 major 包 → UNSUPPORTED_VERSION，且参考失效（保守闸） */
        {
            uint8_t* v8p = build_v9_packet(&sz_p3, 4u, 4u, (uint16_t)TILE_COUNT, 0u, 0u, 0u, 0u);
            MT_CHECK(v8p != NULL);
            if (v8p != NULL) {
                v8p[6] = 8u;
                v8p[45] = 8u;
                tc_store_be32(v8p + 49, tc_crc32(v8p, 49u));
                MT_CHECK_EQ_I64(tc_gop_context_observe(ctx, v8p, sz_p3, &gi),
                                TC_ERR_UNSUPPORTED_VERSION);
                MT_CHECK_EQ_I64(tc_gop_context_state(ctx, &state, NULL), TC_OK);
                MT_CHECK_EQ_U64(state, (uint32_t)TOPOS_GOP_REF_INVALID);
                /* REF_INVALID 下 P → REFERENCE_INVALID */
                MT_CHECK_EQ_I64(tc_gop_context_observe(ctx, p_p1, sz_p1, &gi),
                                TC_ERR_REFERENCE_INVALID);
                /* 新 I 恢复 */
                MT_CHECK_EQ_I64(tc_gop_context_observe(ctx, p_i2, sz_p3, &gi), TC_OK);
                MT_CHECK_EQ_U64(gi.ref_state, (uint32_t)TOPOS_GOP_REF_READY);
                free(v8p);
            }
        }

        /* abort(reset_to_i) → NO_REF；P 再被拒（首帧必须 I） */
        tc_gop_context_abort(ctx, 1);
        MT_CHECK_EQ_I64(tc_gop_context_state(ctx, &state, NULL), TC_OK);
        MT_CHECK_EQ_U64(state, (uint32_t)TOPOS_GOP_NO_REF);
        MT_CHECK_EQ_I64(tc_gop_context_observe(ctx, p_p1, sz_p1, &gi),
                        TC_ERR_MALFORMED);

        /* 非 V9 cfg 拒绝 */
        {
            topos_gop_context* bad_ctx = (void*)1;
            topos_frame_config bad = cfg;
            bad.reserved[0] = 9u;
            MT_CHECK_EQ_I64(tc_gop_context_create(&bad, &bad_ctx),
                            TC_ERR_INVALID_ARGUMENT);
            MT_CHECK(bad_ctx == NULL);
        }

        free(p_i);
        free(p_p1);
        free(p_p2);
        free(p_i2);
        tc_gop_context_close(ctx);
    }

    /* ---- 截断扫描：任意前缀不越界、返回码有定义 ---- */
    {
        for (size_t len = 0u; len <= total; ++len) {
            topos_v8_packet_view view;
            int32_t rc = tc_packet_scan_v8(pkt, len, &view);
            MT_CHECK(status_is_defined(rc));
        }
        /* 拖尾字节 → MALFORMED（结构精确耗尽） */
        pkt[total] = 0u;
        topos_v8_packet_view view;
        MT_CHECK_EQ_I64(tc_packet_scan_v8(pkt, total + 1u, &view), TC_ERR_MALFORMED);
    }

    free(pkt);
    return MT_MAIN_RETURN();
}
