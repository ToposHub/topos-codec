/* test_retired_read —— V 代际收纳（2026-09-13）读端退役钉死测试。
 *
 * 退役代际（V2-Rice/V3/V4/V5/V6/V7-R）文件读取必须：
 *   1. 返回 TC_ERR_UNSUPPORTED_VERSION（干净、可诊断）；
 *   2. 错误信息含 "retired" + ADR 指引（诊断工具能解释为什么打不开）；
 *   3. 绝不 crash / 误解码 / 越界（含真实归档流 corpus_retired）；
 *   4. 保留代际（V1/V2-VLC/V7-R2/V8）读取不受闸门影响（对照腿）。
 *
 * fixture 方式：退役代际写端已拒绝，无法再编码产包——用保留代际合法包
 * patch 代际/熵字段 + 重算帧头 CRC 构造退役包（字节级 fixture，方法本身
 * 在腿 4 里做还原自检）。
 */
#include "bitstream/packet.h"
#include "codec/codec.h"
#include "common/crc32.h"
#include "common/endian.h"
#include "common/error.h"
#include "image_synth.h"
#include "mini_test.h"
#include "topos_codec.h"

#include <stdlib.h>
#include <string.h>

#ifndef TOPOS_CORPUS_RETIRED_DIR
#define TOPOS_CORPUS_RETIRED_DIR "tests/fuzz/corpus_retired"
#endif

static void* must_alloc(size_t n)
{
    void* p = malloc(n);
    if (p == NULL) { fprintf(stderr, "allocation failed\n"); exit(2); }
    return p;
}

/* 编码一帧保留代际包（em = sel，1x1 够过闸门即可） */
static uint8_t* encode_retained(uint32_t sel, size_t* out_size)
{
    image_synth_cfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.seed = 0x9E713EDull + sel;
    sc.width = 32u;
    sc.height = 24u;
    sc.kind = TC_SYNTH_MIXED;
    sc.bit_depth = 10u;
    uint16_t *y, *u, *v, *a;
    if (image_synth_alloc(&sc, 0, &y, &u, &v, &a) != 0) { return NULL; }
    topos_frame_config c;
    memset(&c, 0, sizeof(c));
    c.struct_size = (uint32_t)sizeof(c);
    c.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    c.visible_width = 32u;
    c.visible_height = 24u;
    c.qp_base = 24u;
    c.slice_rows = 4u;
    c.bit_depth = 10u;
    c.reserved[0] = sel;
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    in.planes[0] = y;
    in.planes[1] = u;
    in.planes[2] = v;
    size_t cap = tc_frame_packet_bound(&c);
    uint8_t* buf = must_alloc(cap != 0u ? cap : 1u);
    topos_frame_stats st;
    int32_t rc = tc_frame_encode(&c, &in, buf, cap, &st);
    free(y); free(u); free(v); free(a);
    if (rc != TC_OK) { free(buf); return NULL; }
    *out_size = st.packet_size;
    return buf;
}

/* patch + 重算帧头 CRC（帧头 49B，CRC 在 49..52） */
static void patch_byte(uint8_t* pkt, size_t off, uint8_t v)
{
    pkt[off] = v;
    tc_store_be32(pkt + 49u, tc_crc32(pkt, 49u));
}

static void test_retired_major_rejected(void)
{
    static const unsigned retired_majors[4] = {3u, 4u, 5u, 6u};
    for (unsigned i = 0; i < 4u; ++i) {
        size_t n = 0;
        uint8_t* pkt = encode_retained(0u, &n); /* V1 载体 */
        MT_CHECK(pkt != NULL);
        if (pkt == NULL) { return; }
        patch_byte(pkt, 6u, (uint8_t)retired_majors[i]);
        topos_frame_output info;
        MT_CHECK_EQ_I64(tc_frame_decode(pkt, n, NULL, NULL, &info),
                        TC_ERR_UNSUPPORTED_VERSION);
        MT_CHECK(strstr(tc_last_error(), "retired") != NULL);
        MT_CHECK(strstr(tc_last_error(), "ADR-C0xx") != NULL);
        topos_packet_view view;
        MT_CHECK_EQ_I64(tc_packet_scan(pkt, n, &view), TC_ERR_UNSUPPORTED_VERSION);
        /* 还原自检：fixture 方法本身不破坏合法读取 */
        patch_byte(pkt, 6u, 1u);
        MT_CHECK_EQ_I64(tc_frame_decode(pkt, n, NULL, NULL, &info), TC_OK);
        free(pkt);
    }
}

static void test_retired_entropy_combo_rejected(void)
{
    /* V2-Rice = (major 2, entropy 0)；V7-R = (major 7, entropy 6)——
     * 保留 major 内的退役熵组合同样被闸门拦截 */
    static const unsigned base_sel[2] = {1u, 8u};
    static const unsigned off[2] = {45u, 45u};
    static const uint8_t retired_em[2] = {0u, 6u};
    for (unsigned i = 0; i < 2u; ++i) {
        size_t n = 0;
        uint8_t* pkt = encode_retained(base_sel[i], &n);
        MT_CHECK(pkt != NULL);
        if (pkt == NULL) { return; }
        patch_byte(pkt, off[i], retired_em[i]);
        topos_frame_output info;
        MT_CHECK_EQ_I64(tc_frame_decode(pkt, n, NULL, NULL, &info),
                        TC_ERR_UNSUPPORTED_VERSION);
        MT_CHECK(strstr(tc_last_error(), "retired") != NULL);
        /* 还原自检 */
        patch_byte(pkt, off[i], base_sel[i] == 1u ? 1u : 7u);
        MT_CHECK_EQ_I64(tc_frame_decode(pkt, n, NULL, NULL, &info), TC_OK);
        free(pkt);
    }
}

static void test_retained_generations_still_readable(void)
{
    static const unsigned retained[3] = {0u, 1u, 8u};
    for (unsigned i = 0; i < 3u; ++i) {
        size_t n = 0;
        uint8_t* pkt = encode_retained(retained[i], &n);
        MT_CHECK(pkt != NULL);
        if (pkt == NULL) { return; }
        topos_frame_output info;
        MT_CHECK_EQ_I64(tc_frame_decode(pkt, n, NULL, NULL, &info), TC_OK);
        free(pkt);
    }
}

/* V7-A（major7 + entropy5 专用语法）随批 4 归档——探测链在非回放构建
 * 干净拒绝；V7-R2（同 major，entropy7）不受影响（对照即本文件其余腿）。 */
static void test_v7a_probe_rejected(void)
{
    size_t n = 0;
    uint8_t* pkt = encode_retained(8u, &n); /* V7-R2 载体（major 7） */
    MT_CHECK(pkt != NULL);
    if (pkt == NULL) { return; }
    patch_byte(pkt, 45u, 5u); /* em7 → em5 = V7-A 判据 */
    topos_frame_output info;
    MT_CHECK_EQ_I64(tc_frame_decode(pkt, n, NULL, NULL, &info),
                    TC_ERR_UNSUPPORTED_VERSION);
    MT_CHECK(strstr(tc_last_error(), "V7-A band decode") != NULL);
    /* 还原自检 */
    patch_byte(pkt, 45u, 7u);
    MT_CHECK_EQ_I64(tc_frame_decode(pkt, n, NULL, NULL, &info), TC_OK);
    free(pkt);
}

static void test_validate_rejects_retired_headers(void)
{
    /* 基底 = 真实 V7-R2 包的合法帧头（交叉规则字段齐全），逐腿 patch */
    size_t n = 0;
    uint8_t* pkt = encode_retained(8u, &n);
    MT_CHECK(pkt != NULL);
    if (pkt == NULL) { return; }
    topos_packet_view view;
    MT_CHECK_EQ_I64(tc_packet_scan(pkt, n, &view), TC_OK);
    topos_frame_header fh = view.fh;
    free(pkt);

    MT_CHECK_EQ_I64(tc_frame_header_validate(&fh), TC_OK); /* 基底自检 */
    static const unsigned retired_majors[4] = {3u, 4u, 5u, 6u};
    for (unsigned i = 0; i < 4u; ++i) {
        fh.version_major = (uint8_t)retired_majors[i];
        MT_CHECK_EQ_I64(tc_frame_header_validate(&fh), TC_ERR_UNSUPPORTED_VERSION);
    }
    fh.version_major = 2u; /* V2-Rice：patch 熵三元组 (0,0,0) */
    fh.entropy_mode = 0u;
    fh.codebook_version = 0u;
    fh.coding_mode = 0u;
    MT_CHECK_EQ_I64(tc_frame_header_validate(&fh), TC_ERR_UNSUPPORTED_VERSION);
    fh.version_major = 7u; /* V7-R：(6,0,0) */
    fh.entropy_mode = 6u;
    MT_CHECK_EQ_I64(tc_frame_header_validate(&fh), TC_ERR_UNSUPPORTED_VERSION);
    fh.entropy_mode = 7u; /* V7-R2 保留 */
    MT_CHECK_EQ_I64(tc_frame_header_validate(&fh), TC_OK);
}

/* 真实归档流：corpus_retired 的 V3 golden（16 记录，偶数=packet 字节）
 * 必须全部干净 UNSUPPORTED_VERSION，不 crash / 不越界（TOPOS_DEV 未设、
 * 默认构建无回放面）。 */
static void test_archived_v3_golden_rejected(void)
{
    static const char* name = "/golden_codec_v3_intra.bin";
    char path[512];
    snprintf(path, sizeof(path), "%s%s", TOPOS_CORPUS_RETIRED_DIR, name);
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "skip: %s not found\n", path);
        return;
    }
    uint8_t hdr[24];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "TPC1", 4) != 0) { fclose(f); MT_CHECK(0); return; }
    for (unsigned record = 0; record < 16u; ++record) {
        uint8_t lenb[4];
        if (fread(lenb, 1, 4, f) != 4) { break; }
        uint32_t n = tc_load_be32(lenb);
        if (n == 0u || n > 1024u * 1024u) { break; }
        uint8_t* packet = must_alloc(n);
        if (fread(packet, 1, n, f) != n) { free(packet); break; }
        if ((record & 1u) == 0u) {
            topos_frame_output info;
            MT_CHECK_EQ_I64(tc_frame_decode(packet, n, NULL, NULL, &info),
                            TC_ERR_UNSUPPORTED_VERSION);
            MT_CHECK(strstr(tc_last_error(), "retired") != NULL);
        }
        free(packet);
    }
    fclose(f);
}

int main(void)
{
    test_retired_major_rejected();
    test_retired_entropy_combo_rejected();
    test_retained_generations_still_readable();
    test_validate_rejects_retired_headers();
    test_v7a_probe_rejected();
    test_archived_v3_golden_rejected();
    return MT_MAIN_RETURN();
}
