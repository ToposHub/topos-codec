/* test_rans2_slice —— V7-R2（major=7/entropy=7，rANS order-1 上下文扩展，
 * ADR-C036）位流集成测试。
 *
 * 1. 选择域：reserved[0]=8 合法 / 9 拒绝；入流帧头 (major,entropy) = (7,7)；
 * 2. 差分 vs V2-VLC 与 V7-R：同输入同 qp，三种流的解码输出逐位一致
 *    （熵层无损替换的核心证据——上下文只改符号模型不改符号序列）；
 *    覆盖 5 图案 × qp 档 × 12-bit × alpha mode2 × 444/GBR；
 * 3. 确定性：同输入两次编码逐字节一致；
 * 4. 体积：纹理素材上 rans2 < rans（模型选择必须赢）；平坦/qp88 退化为
 *    order-0 + flags/表开销界内（≤ rans + 每 slice 230B 前缀增量）；
 * 5. 畸形域：flags 保留位非零 / lvl 模型 3 → MALFORMED（经 concealment）；
 *    表行清零 → 模型不可建 → concealment；截断包 → 非 OK。 */
#include "bitstream/packet.h"
#include "codec/codec.h"
#include "common/crc32.h"
#include "common/endian.h"
#include "image_synth.h"
#include "mini_test.h"
#include "topos_codec.h"

#include <stdlib.h>
#include <string.h>

/* ---------- 辅助（结构对齐 test_rans_slice.c 同名版） ---------- */

static void base_cfg(topos_frame_config* c, uint32_t w, uint32_t h, uint32_t sel)
{
    memset(c, 0, sizeof(*c));
    c->struct_size = (uint32_t)sizeof(topos_frame_config);
    c->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    c->visible_width = (uint16_t)w;
    c->visible_height = (uint16_t)h;
    c->qp_base = 24u;
    c->qmatrix_id = 0u;
    c->slice_rows = 4u;
    c->reserved[0] = sel;
}

static int32_t encode_to_buf(const topos_frame_config* cfg, const topos_frame_input* in,
                             uint8_t** out, size_t* size)
{
    *out = NULL;
    *size = 0u;
    topos_frame_stats st;
    memset(&st, 0, sizeof(st));
    size_t cap = tc_frame_packet_bound(cfg);
    uint8_t* buf = (uint8_t*)malloc(cap != 0u ? cap : 1u);
    if (buf == NULL) { return TC_ERR_OUT_OF_MEMORY; }
    int32_t rc = tc_frame_encode(cfg, in, buf, cap, &st);
    if (rc == TC_OK) {
        *out = buf;
        *size = st.packet_size;
    } else {
        free(buf);
    }
    return rc;
}

static int32_t decode_to_planes(const uint8_t* data, size_t size,
                                topos_frame_output* info, uint16_t* planes[4])
{
    for (int i = 0; i < 4; ++i) { planes[i] = NULL; }
    int32_t rc = tc_frame_decode(data, size, NULL, NULL, info);
    if (rc != TC_OK) { return rc; }
    for (uint32_t p = 0u; p < info->plane_count; ++p) {
        uint32_t w = 0u, h = 0u;
        if (tc_frame_plane_geometry(info, p, &w, &h) != TC_OK) { return TC_ERR_MALFORMED; }
        planes[p] = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
        if (planes[p] == NULL) {
            for (int i = 0; i < 4; ++i) { free(planes[i]); planes[i] = NULL; }
            return TC_ERR_OUT_OF_MEMORY;
        }
    }
    return tc_frame_decode(data, size, planes, NULL, info);
}

static void free_planes(uint16_t* planes[4])
{
    for (int i = 0; i < 4; ++i) { free(planes[i]); planes[i] = NULL; }
}

/* 差分主路径：sel_a 与 sel_b 两流解码逐位一致 */
static int differential_bitexact(uint32_t w, uint32_t h, tc_synth_kind kind,
                                 uint32_t qp, uint32_t bit_depth, int with_alpha,
                                 uint32_t sel_a, uint32_t sel_b,
                                 int64_t* size_a_out, int64_t* size_b_out,
                                 uint32_t chroma_format)
{
    image_synth_cfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.seed = 0xC036u + (uint64_t)kind * 131u + (uint64_t)qp
            + (uint64_t)chroma_format * 7919u;
    sc.width = w;
    sc.height = h;
    sc.kind = kind;
    sc.bit_depth = bit_depth;
    sc.chroma_format = chroma_format;
    uint16_t *y = NULL, *u = NULL, *v = NULL, *a = NULL;
    if (image_synth_alloc(&sc, with_alpha, &y, &u, &v, &a) != 0) { return 0; }

    topos_frame_config ca, cb;
    base_cfg(&ca, w, h, sel_a);
    base_cfg(&cb, w, h, sel_b);
    ca.pixel_format = (uint8_t)chroma_format;
    cb.pixel_format = (uint8_t)chroma_format;
    ca.qp_base = (uint8_t)qp;
    cb.qp_base = (uint8_t)qp;
    ca.bit_depth = (uint16_t)bit_depth;
    cb.bit_depth = (uint16_t)bit_depth;
    if (with_alpha) {
        ca.alpha_mode = 2u;
        ca.alpha_bit_depth = 10u;
        cb.alpha_mode = 2u;
        cb.alpha_bit_depth = 10u;
    }

    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(topos_frame_input);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    in.planes[0] = y;
    in.planes[1] = u;
    in.planes[2] = v;
    in.planes[3] = a;

    uint8_t *pa = NULL, *pb = NULL;
    size_t sa = 0u, sb = 0u;
    int ok = 1;
    if (encode_to_buf(&ca, &in, &pa, &sa) != TC_OK ||
        encode_to_buf(&cb, &in, &pb, &sb) != TC_OK) {
        ok = 0;
        goto done;
    }
    if (size_a_out != NULL) { *size_a_out = (int64_t)sa; }
    if (size_b_out != NULL) { *size_b_out = (int64_t)sb; }

    {
        topos_frame_output ia, ib;
        uint16_t* pla[4];
        uint16_t* plb[4];
        if (decode_to_planes(pa, sa, &ia, pla) != TC_OK ||
            decode_to_planes(pb, sb, &ib, plb) != TC_OK) {
            ok = 0;
            goto done2;
        }
        if (ia.plane_count != ib.plane_count || ia.visible_width != ib.visible_width ||
            ia.visible_height != ib.visible_height) {
            ok = 0;
            goto done2;
        }
        for (uint32_t p = 0u; p < ia.plane_count; ++p) {
            uint32_t pw = 0u, ph = 0u;
            tc_frame_plane_geometry(&ia, p, &pw, &ph);
            if (memcmp(pla[p], plb[p], (size_t)pw * ph * sizeof(uint16_t)) != 0) {
                ok = 0;
            }
        }
done2:
        free_planes(pla);
        free_planes(plb);
    }
done:
    free(pa);
    free(pb);
    free(y);
    free(u);
    free(v);
    free(a);
    return ok;
}

/* ---------- 1. 选择域 ---------- */

static void test_selection_domain(void)
{
    image_synth_cfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.seed = 0xC036u;
    sc.width = 64u;
    sc.height = 48u;
    sc.kind = TC_SYNTH_DETAIL;
    sc.bit_depth = 10u;
    uint16_t *y, *u, *v, *a;
    MT_CHECK(image_synth_alloc(&sc, 0, &y, &u, &v, &a) == 0);
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(topos_frame_input);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    in.planes[0] = y;
    in.planes[1] = u;
    in.planes[2] = v;

    topos_frame_config c;
    base_cfg(&c, 64u, 48u, 8u);
    uint8_t* pkt = NULL;
    size_t sz = 0u;
    MT_CHECK(encode_to_buf(&c, &in, &pkt, &sz) == TC_OK);
    /* 帧头 [6]=major=7；[45]=entropy_mode=7（V7-A=5、V7-R=6 之外） */
    MT_CHECK_EQ_U64(pkt[6], 7u);
    MT_CHECK_EQ_U64(pkt[45], 7u);
    topos_frame_output info;
    uint16_t* pl[4];
    MT_CHECK(decode_to_planes(pkt, sz, &info, pl) == TC_OK);
    MT_CHECK_EQ_U64(info.concealed_slices, 0u);
    free_planes(pl);
    free(pkt);

    /* 9 = V8（批 2 编码器落地）：配置合法，plain 编码产 V8 包
     * （major=8 / entropy_mode=8）；域上限 10 */
    base_cfg(&c, 64u, 48u, 9u);
    MT_CHECK(tc_frame_config_validate(&c) == TC_OK);
    MT_CHECK(encode_to_buf(&c, &in, &pkt, &sz) == TC_OK);
    MT_CHECK_EQ_U64(pkt[6], 8u);
    MT_CHECK_EQ_U64(pkt[45], 8u);
    free(pkt);
    /* V9 批 1（topos_v9_micro_gop_plan）：em=10 → V9（major=9 三元组
     * (9,0,0)）；cfg 域 {0..10}。编码像素路径批 2 接线（NOT_IMPLEMENTED）。 */
    base_cfg(&c, 64u, 48u, 10u);
    MT_CHECK(tc_frame_config_validate(&c) == TC_OK);

    /* V 代际收纳（2026-09-13）：退役 em 写路径拒绝钉死（承接原
     * test_rans_slice 选择域覆盖，语义反转——em7 V7-R 写端退役，
     * em 编号永久封存；2..7 一律 INVALID_ARGUMENT 不静默回落）。 */
    for (uint32_t em = 2u; em <= 7u; ++em) {
        base_cfg(&c, 64u, 48u, em);
        MT_CHECK(tc_frame_config_validate(&c) == TC_ERR_INVALID_ARGUMENT);
    }

    free(y);
    free(u);
    free(v);
    free(a);
}

/* ---------- 2. 差分 ---------- */

static void test_differential(void)
{
    static const tc_synth_kind kinds[5] = {
        TC_SYNTH_FLAT, TC_SYNTH_GRADIENT, TC_SYNTH_GRAIN,
        TC_SYNTH_DETAIL, TC_SYNTH_MIXED,
    };
    for (uint32_t ki = 0u; ki < 5u; ++ki) {
        for (uint32_t qi = 0u; qi < 2u; ++qi) {
            const uint32_t qp = qi == 0u ? 20u : 48u;
            /* vs V2-VLC(sel 1) 与 vs V1(sel 0)——V 代际收纳（2026-09-13）：
             * 原 V7-R(sel 7) 基线随其写端退役改为 V1（共享量化核，解码
             * 像素逐位一致的契约不变，保留代际间交叉差分更强）。 */
            MT_CHECK(differential_bitexact(96u, 64u, kinds[ki], qp, 10u, 0,
                                           1u, 8u, NULL, NULL, 0u));
            MT_CHECK(differential_bitexact(96u, 64u, kinds[ki], qp, 10u, 0,
                                           0u, 8u, NULL, NULL, 0u));
        }
    }
    /* 12-bit + alpha mode2（alpha 恒 Rice 不参与上下文） */
    MT_CHECK(differential_bitexact(96u, 64u, TC_SYNTH_DETAIL, 33u, 12u, 1,
                                   0u, 8u, NULL, NULL, 0u));
    /* 444 与 GBR（pf=1/2） */
    MT_CHECK(differential_bitexact(96u, 64u, TC_SYNTH_MIXED, 40u, 10u, 0,
                                   0u, 8u, NULL, NULL, 1u));
    MT_CHECK(differential_bitexact(96u, 64u, TC_SYNTH_MIXED, 40u, 10u, 0,
                                   0u, 8u, NULL, NULL, 2u));
    /* 极端 qp（选择退化为 order-0 + 占位表） */
    MT_CHECK(differential_bitexact(96u, 64u, TC_SYNTH_FLAT, 88u, 10u, 0,
                                   0u, 8u, NULL, NULL, 0u));
}

/* ---------- 3. 确定性 ---------- */

static void test_determinism(void)
{
    image_synth_cfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.seed = 0xC036u;
    sc.width = 128u;
    sc.height = 80u;
    sc.kind = TC_SYNTH_MIXED;
    sc.bit_depth = 10u;
    uint16_t *y, *u, *v, *a;
    MT_CHECK(image_synth_alloc(&sc, 0, &y, &u, &v, &a) == 0);
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(topos_frame_input);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    in.planes[0] = y;
    in.planes[1] = u;
    in.planes[2] = v;
    topos_frame_config c;
    base_cfg(&c, 128u, 80u, 8u);
    uint8_t *p1 = NULL, *p2 = NULL;
    size_t s1 = 0u, s2 = 0u;
    MT_CHECK(encode_to_buf(&c, &in, &p1, &s1) == TC_OK);
    MT_CHECK(encode_to_buf(&c, &in, &p2, &s2) == TC_OK);
    MT_CHECK(s1 == s2 && memcmp(p1, p2, s1) == 0);
    free(p1);
    free(p2);
    free(y);
    free(u);
    free(v);
    free(a);
}

/* ---------- 4. 体积 ---------- */

static void test_size_properties(void)
{
    /* 纹理素材：条件模型必须净赢（V 代际收纳 2026-09-13：基线由已退役的
     * V7-R(sel7) 改为 V2-VLC(sel1)——2026-09-12 实测同画质码率较 vlc
     * −7~12%（tools/toos_entropy_2026-09-12.json），只断方向与余量） */
    int64_t s7 = 0, s8 = 0;
    MT_CHECK(differential_bitexact(128u, 80u, TC_SYNTH_DETAIL, 33u, 10u, 0,
                                   1u, 8u, &s7, &s8, 0u));
    MT_CHECK(s8 < s7);

    /* 平坦 + 极高 qp：选择退化为 order-0，相对 VLC 的增量界 = 条件表
     * 最多 (350−121)B/slice——按 slice 数放大 + 终态余量
     * （实测 ≈100B/slice，界留 2 倍以上余量防爆炸回归） */
    MT_CHECK(differential_bitexact(128u, 80u, TC_SYNTH_FLAT, 88u, 10u, 0,
                                   1u, 8u, &s7, &s8, 0u));
    /* 128x80 / slice_rows 4 → 每 plane 3 band → 9 色 slice */
    MT_CHECK(s8 <= s7 + 9 * 231 + 64);
}

/* ---------- 5. 畸形域 ---------- */

static void patch_slice_crc(const topos_packet_view* view, uint32_t si)
{
    uint8_t* hdr = (uint8_t*)view->payloads[si] - TC_SLICE_HEADER_SIZE;
    const uint32_t crc = tc_crc32(view->payloads[si],
                                  view->slices[si].slice_payload_size);
    tc_store_be32(hdr + 13, crc);
}

static void test_malformed_domain(void)
{
    image_synth_cfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.seed = 5u;
    sc.width = 64u;
    sc.height = 48u;
    sc.kind = TC_SYNTH_MIXED;
    sc.bit_depth = 10u;
    uint16_t *y, *u, *v, *a;
    MT_CHECK(image_synth_alloc(&sc, 0, &y, &u, &v, &a) == 0);
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(topos_frame_input);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    in.planes[0] = y;
    in.planes[1] = u;
    in.planes[2] = v;
    topos_frame_config c;
    base_cfg(&c, 64u, 48u, 8u);

    uint8_t* pkt = NULL;
    size_t sz = 0u;
    MT_CHECK(encode_to_buf(&c, &in, &pkt, &sz) == TC_OK);

    topos_packet_view view;
    MT_CHECK(tc_packet_parse_structure(pkt, sz, &view) == TC_OK);
    uint32_t color_si = 0u;
    for (uint32_t si = 0u; si < view.slice_count; ++si) {
        if (view.slices[si].plane < 3u) { color_si = si; break; }
    }

    /* a) flags 保留位非零（bit3）→ MALFORMED（经 concealment 观测） */
    {
        uint8_t* bad = (uint8_t*)malloc(sz);
        memcpy(bad, pkt, sz);
        topos_packet_view bv;
        MT_CHECK(tc_packet_parse_structure(bad, sz, &bv) == TC_OK);
        *(uint8_t*)bv.payloads[color_si] |= 0x08u;
        patch_slice_crc(&bv, color_si);
        topos_frame_output info;
        uint16_t* pl[4];
        MT_CHECK(decode_to_planes(bad, sz, &info, pl) == TC_WARN_CONCEALED);
        free_planes(pl);
        free(bad);
    }

    /* b) flags lvl 模型 = 3（保留）→ MALFORMED */
    {
        uint8_t* bad = (uint8_t*)malloc(sz);
        memcpy(bad, pkt, sz);
        topos_packet_view bv;
        MT_CHECK(tc_packet_parse_structure(bad, sz, &bv) == TC_OK);
        *(uint8_t*)bv.payloads[color_si] = 0x03u;
        patch_slice_crc(&bv, color_si);
        topos_frame_output info;
        uint16_t* pl[4];
        MT_CHECK(decode_to_planes(bad, sz, &info, pl) == TC_WARN_CONCEALED);
        free_planes(pl);
        free(bad);
    }

    /* c) 条件表行全清零（含占位）→ 模型不可建 → concealment */
    {
        uint8_t* bad = (uint8_t*)malloc(sz);
        memcpy(bad, pkt, sz);
        topos_packet_view bv;
        MT_CHECK(tc_packet_parse_structure(bad, sz, &bv) == TC_OK);
        const uint32_t psz = bv.slices[color_si].slice_payload_size;
        const uint32_t wipe = psz > 32u ? 32u : psz;
        memset((void*)bv.payloads[color_si], 0, wipe);
        patch_slice_crc(&bv, color_si);
        topos_frame_output info;
        uint16_t* pl[4];
        MT_CHECK(decode_to_planes(bad, sz, &info, pl) == TC_WARN_CONCEALED);
        free_planes(pl);
        free(bad);
    }

    /* d) 截断包：任意非 OK（不得成功/崩溃） */
    {
        topos_frame_output info;
        uint16_t* pl[4];
        MT_CHECK(decode_to_planes(pkt, sz / 2u, &info, pl) != TC_OK);
        free_planes(pl);
    }

    free(pkt);
    free(y);
    free(u);
    free(v);
    free(a);
}

int main(void)
{
    test_selection_domain();
    test_differential();
    test_determinism();
    test_size_properties();
    test_malformed_domain();
    return MT_MAIN_RETURN();
}
