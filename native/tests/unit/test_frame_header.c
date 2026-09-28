/* frame header：字段级校验矩阵（patch+重算 CRC）、CRC 门、几何推导、往返 */
#include "bitstream/frame_header.h"

#include <string.h>

#include "common/crc32.h"
#include "common/endian.h"
#include "mini_test.h"

static topos_frame_header base_fh(void)
{
    topos_frame_header fh;
    memset(&fh, 0, sizeof(fh));
    fh.version_major = 1u;
    fh.profile = 3u;
    fh.pixel_format = 0u;
    fh.bit_depth = 10u;
    fh.alpha_mode = 0u;
    fh.alpha_bit_depth = 0u;
    fh.coded_width = 40u;
    fh.coded_height = 24u;
    fh.visible_width = 36u;
    fh.visible_height = 20u;
    fh.plane_count = 3u;
    fh.qmatrix_id = 0u;
    fh.qp_base = 20u;
    fh.slice_count = 3u;
    fh.color_range = 1u;
    fh.color_primaries = 1u;
    fh.color_transfer = 1u;
    fh.color_matrix = 1u;
    fh.chroma_siting = 0u;
    fh.frame_packet_size = 12345u;
    return fh;
}

/* 对合法编码结果 patch 指定偏移（BE 半字/字节/字由调用者给原始字节值）并重算 CRC */
static int32_t patched_decode(const uint8_t* valid, size_t off, const uint8_t* patch, size_t plen,
                              topos_frame_header* out)
{
    uint8_t buf[TC_FRAME_HEADER_SIZE];
    memcpy(buf, valid, sizeof(buf));
    memcpy(buf + off, patch, plen);
    tc_store_be32(buf + 49, tc_crc32(buf, 49u));
    return tc_frame_header_decode(buf, sizeof(buf), out);
}

#define CHECK_PATCH_RC(expect_rc, off, arr)                                    \
    do {                                                                       \
        topos_frame_header tmp;                                                \
        int32_t got = patched_decode(valid, (off), (arr), sizeof(arr), &tmp);  \
        if (got != (expect_rc)) {                                              \
            fprintf(stderr, "FAIL %s:%d: patch@%u expect %d got %d (%s)\n",    \
                    __FILE__, __LINE__, (unsigned)(off), (expect_rc), got,     \
                    tc_last_error());                                          \
            mt_failures++;                                                     \
        }                                                                      \
    } while (0)

int main(void)
{
    /* ---- 往返 + 几何 ---- */
    {
        topos_frame_header fh = base_fh();
        uint8_t enc[TC_FRAME_HEADER_SIZE];
        MT_CHECK_EQ_I64(tc_frame_header_encode(&fh, enc), TC_OK);
        topos_frame_header back;
        MT_CHECK_EQ_I64(tc_frame_header_decode(enc, sizeof(enc), &back), TC_OK);
        MT_CHECK_EQ_U64(back.coded_width, 40ull);
        MT_CHECK_EQ_U64(back.visible_width, 36ull);
        MT_CHECK_EQ_U64(back.slice_count, 3ull);
        MT_CHECK_EQ_U64(back.frame_packet_size, 12345ull);
        /* 几何：Y 40x24（块 5x3）；chroma vis 18 → coded 24（块 3x3） */
        MT_CHECK_EQ_U64(back.plane_coded_w[0], 40ull);
        MT_CHECK_EQ_U64(back.plane_coded_h[0], 24ull);
        MT_CHECK_EQ_U64(back.plane_coded_w[1], 24ull);
        MT_CHECK_EQ_U64(back.plane_coded_w[2], 24ull);
        MT_CHECK_EQ_U64(back.plane_visible_w[1], 18ull);
        MT_CHECK_EQ_U64(back.plane_block_cols[0], 5ull);
        MT_CHECK_EQ_U64(back.plane_block_rows[0], 3ull);
        MT_CHECK_EQ_U64(back.plane_block_cols[1], 3ull);
    }
    { /* qmatrix_id=3：422 Low Compact 合法交叉组合 */
        topos_frame_header fh = base_fh();
        fh.qmatrix_id = 3u;
        uint8_t enc[TC_FRAME_HEADER_SIZE];
        MT_CHECK_EQ_I64(tc_frame_header_encode(&fh, enc), TC_OK);
        topos_frame_header back;
        MT_CHECK_EQ_I64(tc_frame_header_decode(enc, sizeof(enc), &back), TC_OK);
        MT_CHECK_EQ_U64(back.qmatrix_id, 3ull);

        fh.profile = 5u;
        fh.pixel_format = 2u;
        fh.bit_depth = 12u;
        fh.version_minor = 3u;
        fh.color_matrix = 0u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&fh, enc), TC_ERR_UNSUPPORTED_MATRIX);
    }
    { /* alpha 几何 = luma */
        topos_frame_header fh = base_fh();
        fh.alpha_mode = 1u;
        fh.alpha_bit_depth = 16u;
        fh.plane_count = 4u;
        fh.slice_count = 4u;
        uint8_t enc[TC_FRAME_HEADER_SIZE];
        MT_CHECK_EQ_I64(tc_frame_header_encode(&fh, enc), TC_OK);
        topos_frame_header back;
        MT_CHECK_EQ_I64(tc_frame_header_decode(enc, sizeof(enc), &back), TC_OK);
        MT_CHECK_EQ_U64(back.plane_coded_w[3], 40ull);
        MT_CHECK_EQ_U64(back.plane_visible_h[3], 20ull);
    }

    topos_frame_header fh = base_fh();
    uint8_t valid[TC_FRAME_HEADER_SIZE];
    MT_CHECK_EQ_I64(tc_frame_header_encode(&fh, valid), TC_OK);

    /* ---- CRC / 长度 / magic（不修复 CRC 的破坏）---- */
    {
        topos_frame_header out;
        uint8_t buf[TC_FRAME_HEADER_SIZE];
        memcpy(buf, valid, sizeof(buf));
        buf[20] ^= 0x01u; /* coded_width 低位 */
        MT_CHECK_EQ_I64(tc_frame_header_decode(buf, sizeof(buf), &out), TC_ERR_CHECKSUM_MISMATCH);
        buf[20] ^= 0x01u;
        buf[0] = 'X';
        MT_CHECK_EQ_I64(tc_frame_header_decode(buf, sizeof(buf), &out), TC_ERR_MALFORMED);
        MT_CHECK_EQ_I64(tc_frame_header_decode(valid, 52u, &out), TC_ERR_TRUNCATED);
        MT_CHECK_EQ_I64(tc_frame_header_decode(NULL, 0u, &out), TC_ERR_TRUNCATED);
    }

    /* ---- patch 矩阵（重算 CRC 后命中字段规则）---- */
    {
        uint8_t w16_1[2] = { 0, 1 };
        uint8_t w16_54[2] = { 0, 54 };
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 4u, w16_54);          /* header_size=54 */
        uint8_t b2[1] = { 2 };
        uint8_t b3[1] = { 3 };
        uint8_t b4[1] = { 4 };
        uint8_t b5[1] = { 5 };
        uint8_t b1[1] = { 1 };
        uint8_t b7[1] = { 7 };
        uint8_t b0[1] = { 0 };
        uint8_t b12[1] = { 12 };
        uint8_t b64[1] = { 64 };
        uint8_t b513[2] = { 0x02, 0x01 };
        /* M9（ADR-C027）→ V 代际收纳（2026-09-13）：major=2 + 熵字段全 0 =
         * V2-Rice 已退役（UNSUPPORTED_VERSION，信息含 retired 指引）；
         * major=4 同为干净拒绝 */
        CHECK_PATCH_RC(TC_ERR_UNSUPPORTED_VERSION, 6u, b2);   /* major=2 (Rice) retired */
        CHECK_PATCH_RC(TC_ERR_UNSUPPORTED_VERSION, 6u, b4);   /* major=4 */
        /* v1.2（R4.1）：minor=1 = bd=12 扩展代；v1.3（R4.2）：minor=2 = pf=1
         * 扩展代。v1.0 枚举 + 已知 minor = 前向兼容 writer，按 §13.1 完全一致
         * 解码（正例见下方各段）；minor>2 仍拒绝 */
        CHECK_PATCH_RC(TC_OK, 7u, b2);                         /* minor=2 + v1.0 枚举 */
        CHECK_PATCH_RC(TC_OK, 7u, b3);                         /* minor=3 + v1.0 枚举 */
        CHECK_PATCH_RC(TC_OK, 7u, b4);                         /* minor=4 + v1.0 枚举（v1.5 qp 域，前向兼容 writer） */
        /* v1.6（TRAW 批 1）：minor=5 已知（pf=3/profile=7 扩展代）——v1.0
         * 枚举 + minor=5 = 前向兼容 writer 放行；v1.7（批 4）：minor=6
         * 已知（bd=16 扩展代）同规则。v1.8（H1 2026-09-21）：minor=7
         * 已知（sRGB transfer=13 扩展代）同规则（transfer 语义由 tag
         * 校验承接）；minor=8 未知拒绝 */
        CHECK_PATCH_RC(TC_OK, 7u, b5);
        uint8_t b6[1] = { 6 };
        CHECK_PATCH_RC(TC_OK, 7u, b6);                         /* minor=6 + v1.0 枚举 */
        uint8_t bminor7[1] = { 7 };
        CHECK_PATCH_RC(TC_OK, 7u, bminor7);                    /* minor=7 + v1.0 枚举 */
        uint8_t bminor8[1] = { 8 };
        CHECK_PATCH_RC(TC_ERR_UNSUPPORTED_VERSION, 7u, bminor8); /* minor=8 未知 */
        uint8_t flags2[2] = { 0, 2 };
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 8u, flags2);          /* flags bit1 */
        uint8_t prof0[1] = { 0 };
        uint8_t prof2[1] = { 2 };
        CHECK_PATCH_RC(TC_ERR_UNSUPPORTED_PROFILE, 10u, prof0);
        CHECK_PATCH_RC(TC_ERR_UNSUPPORTED_PROFILE, 10u, prof2); /* 非 {3,5,6,7} 未验收 */
        /* v1.6（TRAW 批 1）：profile 7 枚举已知——本包 pf=0 命中 TRAW 交叉
         * 规则（profile 7 ⇔ CFA pf=3）→ MALFORMED；正例见下方 TRAW 段 */
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 10u, b7);
        /* R4.4：Pro444/Extreme 激活——格式交叉规则（本包 pf=0/bd=10） */
        uint8_t prof5[1] = { 5 };
        uint8_t prof6[1] = { 6 };
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 10u, prof5); /* Pro444 须 4:4:4 */
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 10u, prof6); /* Extreme 须 4:4:4+12 */
        /* v1.3（R4.2）：pf=1 枚举已知——minor=0 流出现 = 版本声明不一致；
         * 正例（minor=2 + pf=1）在下方 v1.3 段；pf=2（GBR）保留 → R4.3 */
        CHECK_PATCH_RC(TC_ERR_UNSUPPORTED_VERSION, 11u, b1);   /* pf=1 w/o minor=2 */
        /* v1.4（R4.3）：pf=2 枚举已知——minor=0 流出现 = 版本声明不一致；
         * 正例（minor=3 + pf=2）在下方 v1.4 段 */
        CHECK_PATCH_RC(TC_ERR_UNSUPPORTED_VERSION, 11u, b2);   /* pf=2 w/o minor=3 */
        /* v1.6（TRAW 批 1）：pf=3 枚举已知——本包 profile 3 命中交叉规则
         * （CFA ⇔ TRAW profile 7）→ MALFORMED；正例见下方 TRAW 段 */
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 11u, b3);
        /* v1.2：bd=12 枚举已知——minor=0 流出现 = 版本声明不一致（R4.1）；
         * 正例（minor=1 + bd=12）在下方 v1.2 段 */
        CHECK_PATCH_RC(TC_ERR_UNSUPPORTED_VERSION, 12u, b12);  /* depth 12 w/o minor */
        uint8_t am2[1] = { 2 };
        uint8_t am3[1] = { 3 };
        /* v1.1（阶段 4）：mode 2 激活 —— 本包 abd=0 → MALFORMED（须 ∈ {8,10,12}） */
        uint8_t am2_abd16[2] = { 2, 16 };
        uint8_t am2_abd12[2] = { 2, 12 };
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 13u, am2);
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 13u, am2_abd16);  /* mode2 且 abd=16 → 拒绝 */
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 13u, am2_abd12);  /* plane_count(3) 不匹配 */
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 13u, am3);
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 14u, b12);   /* alpha_bit_depth=12 且 mode=0 */
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 13u, b1);    /* alpha_mode=1 但 abd 未跟上（仍 0） */
        CHECK_PATCH_RC(TC_ERR_UNSUPPORTED_VERSION, 15u, b1);   /* frame_type=P */
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 16u, w16_1); /* gop_id */
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 18u, b1);    /* ref_distance */
        CHECK_PATCH_RC(TC_ERR_LIMIT_EXCEEDED, 19u, ((uint8_t[2]){ 0x40, 0x01 })); /* coded_w=16385 */
        CHECK_PATCH_RC(TC_ERR_LIMIT_EXCEEDED, 19u, ((uint8_t[2]){ 0, 41 })); /* 非 8 倍 */
        uint8_t vw0[2] = { 0, 0 };
        uint8_t vw100[2] = { 0, 100 };
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 23u, vw0);   /* visible_w=0 */
        CHECK_PATCH_RC(TC_ERR_LIMIT_EXCEEDED, 23u, vw100); /* visible > coded */
        uint8_t pc2[1] = { 2 };
        uint8_t pc4[1] = { 4 };
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 27u, pc2);
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 27u, pc4);
        uint8_t qm5[1] = { 5 };
        /* id=4（边缘均衡，2026-09-21）合法但仅限 4:2:2/10-bit/pf3——
         * base_fh 即该域，故越界样本改用 5（永拒绝域） */
        CHECK_PATCH_RC(TC_ERR_UNSUPPORTED_MATRIX, 28u, qm5);
        CHECK_PATCH_RC(TC_ERR_UNSUPPORTED_VERSION, 29u, b64);   /* v1.5 qp≥64 w/o minor=4 */
        uint8_t b96[1] = { 96 };
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 29u, b96);  /* qp_base>95 恒拒绝 */
        uint8_t sc0[2] = { 0, 0 };
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 30u, sc0);
        CHECK_PATCH_RC(TC_ERR_LIMIT_EXCEEDED, 30u, b513);
        uint8_t sc2[2] = { 0, 2 };
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 30u, sc2);   /* < plane_count */
        uint8_t cr2[1] = { 2 };
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 32u, cr2);
        uint8_t tag2[1] = { 2 };
        uint8_t tag4[1] = { 4 };
        uint8_t tag17[1] = { 17 };
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 33u, tag2);  /* primaries 2 未知 */
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 34u, tag4);  /* transfer 4 未知 */
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 35u, b0);    /* matrix 0 = GBR 保留 */
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 35u, tag17);
        uint8_t siting3[1] = { 3 };
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 36u, siting3);
        uint8_t sarbad[4] = { 0, 1, 0, 0 };
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 37u, sarbad); /* num=1 den=0 */
        uint8_t huge[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        CHECK_PATCH_RC(TC_ERR_LIMIT_EXCEEDED, 41u, huge); /* packet_size 32 位最大 */
        uint8_t zero4[4] = { 0, 0, 0, 0 };
        CHECK_PATCH_RC(TC_ERR_LIMIT_EXCEEDED, 41u, zero4);
        uint8_t r0[4] = { 0, 0, 0, 1 };
        CHECK_PATCH_RC(TC_ERR_MALFORMED, 45u, r0);    /* reserved0 */
    }

    /* ---- v1.2（R4.1）：bd=12 扩展枚举 + minor 标记 ---- */
    {
        topos_frame_header fh = base_fh();
        fh.bit_depth = 12u;
        fh.version_minor = 1u;
        uint8_t enc[TC_FRAME_HEADER_SIZE];
        MT_CHECK_EQ_I64(tc_frame_header_encode(&fh, enc), TC_OK);
        MT_CHECK_EQ_U64(enc[7], 1ull);                 /* minor 随扩展枚举起 */
        topos_frame_header back;
        MT_CHECK_EQ_I64(tc_frame_header_decode(enc, sizeof(enc), &back), TC_OK);
        MT_CHECK_EQ_U64(back.bit_depth, 12ull);
        MT_CHECK_EQ_U64(back.version_minor, 1ull);

        /* v1.0 枚举 + minor=1：按 §13.1 完全一致解码（前向兼容 writer） */
        uint8_t v10[TC_FRAME_HEADER_SIZE];
        topos_frame_header f10 = base_fh();
        MT_CHECK_EQ_I64(tc_frame_header_encode(&f10, v10), TC_OK);
        MT_CHECK_EQ_U64(v10[7], 0ull);                 /* v1.0 语义流恒 0 */
        v10[7] = 1u;
        tc_store_be32(v10 + 49, tc_crc32(v10, 49u));
        MT_CHECK_EQ_I64(tc_frame_header_decode(v10, sizeof(v10), &back), TC_OK);
        MT_CHECK_EQ_U64(back.coded_width, 40ull);

        /* bd=12 但忘了 minor=1（直接构造）→ 编码侧同规则拒绝 */
        topos_frame_header bad = base_fh();
        bad.bit_depth = 12u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc),
                        TC_ERR_UNSUPPORTED_VERSION);
    }

    /* ---- v1.3（R4.2）：YUV 4:4:4（pf=1）扩展枚举 + minor=2 标记 ---- */
    {
        topos_frame_header fh = base_fh();
        fh.pixel_format = 1u;
        fh.version_minor = 2u;
        uint8_t enc[TC_FRAME_HEADER_SIZE];
        MT_CHECK_EQ_I64(tc_frame_header_encode(&fh, enc), TC_OK);
        MT_CHECK_EQ_U64(enc[7], 2ull);                 /* minor 随 pf=1 扩展代 */
        MT_CHECK_EQ_U64(enc[11], 1ull);
        topos_frame_header back;
        MT_CHECK_EQ_I64(tc_frame_header_decode(enc, sizeof(enc), &back), TC_OK);
        MT_CHECK_EQ_U64(back.pixel_format, 1ull);
        MT_CHECK_EQ_U64(back.version_minor, 2ull);
        /* 4:4:4 几何：chroma 全宽（coded/visible/块列 = luma 同值） */
        MT_CHECK_EQ_U64(back.plane_coded_w[1], 40ull);
        MT_CHECK_EQ_U64(back.plane_coded_w[2], 40ull);
        MT_CHECK_EQ_U64(back.plane_visible_w[1], 36ull);
        MT_CHECK_EQ_U64(back.plane_block_cols[1], 5ull);

        /* minor=1 + pf=1：v1.2 包络不含 pf=1 → 版本声明不一致（非枚举域） */
        topos_frame_header m1 = fh;
        m1.version_minor = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&m1, enc), TC_ERR_UNSUPPORTED_VERSION);

        /* pf=1 但 minor=0（v1.0 语义流）→ 编码侧同规则拒绝 */
        topos_frame_header bad = fh;
        bad.version_minor = 0u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_UNSUPPORTED_VERSION);

        /* R4.3 语义更新：pf=2（GBR）属 v1.4 扩展代——v1.3 包络（minor=2）
         * 出现 = 版本声明不一致；pf=3 已知（TRAW 批 1）→ 交叉规则拒绝 */
        topos_frame_header gbr = fh;
        gbr.pixel_format = 2u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&gbr, enc), TC_ERR_UNSUPPORTED_VERSION);
        gbr.pixel_format = 3u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&gbr, enc), TC_ERR_MALFORMED);
    }

    /* ---- v1.4（R4.3）：GBR 4:4:4（pf=2）扩展枚举 + minor=3 + matrix 交叉规则 ---- */
    {
        topos_frame_header fh = base_fh();
        fh.pixel_format = 2u;
        fh.version_minor = 3u;
        fh.color_matrix = 0u;      /* identity（GBR 契约） */
        fh.chroma_siting = 0u;     /* 无色度采样可定位 */
        uint8_t enc[TC_FRAME_HEADER_SIZE];
        MT_CHECK_EQ_I64(tc_frame_header_encode(&fh, enc), TC_OK);
        MT_CHECK_EQ_U64(enc[7], 3ull);                 /* minor 随 pf=2 扩展代 */
        MT_CHECK_EQ_U64(enc[11], 2ull);
        MT_CHECK_EQ_U64(enc[35], 0ull);                /* matrix=0 identity */
        topos_frame_header back;
        MT_CHECK_EQ_I64(tc_frame_header_decode(enc, sizeof(enc), &back), TC_OK);
        MT_CHECK_EQ_U64(back.pixel_format, 2ull);
        MT_CHECK_EQ_U64(back.version_minor, 3ull);
        /* GBR 几何：三平面全宽全高（与 4:4:4 同构） */
        MT_CHECK_EQ_U64(back.plane_coded_w[1], 40ull);
        MT_CHECK_EQ_U64(back.plane_visible_w[2], 36ull);
        MT_CHECK_EQ_U64(back.plane_block_cols[1], 5ull);

        /* 交叉规则：GBR 不得携带 YUV 矩阵；YUV 不得携带 identity */
        topos_frame_header mx = fh;
        mx.color_matrix = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&mx, enc), TC_ERR_MALFORMED);

        /* pf=2 + minor=2（v1.3 包络不含 pf=2）→ 版本声明不一致 */
        topos_frame_header m2 = fh;
        m2.version_minor = 2u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&m2, enc), TC_ERR_UNSUPPORTED_VERSION);

        /* pf=2 + minor=0 → 编码侧同规则拒绝 */
        topos_frame_header bad = fh;
        bad.version_minor = 0u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_UNSUPPORTED_VERSION);

        /* pf=2 + chroma_siting≠0（无采样位置可言）→ MALFORMED */
        topos_frame_header st = fh;
        st.chroma_siting = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&st, enc), TC_ERR_MALFORMED);
    }

    /* ---- R4.4：Pro444（profile=5）/ Extreme（profile=6）激活 ---- */
    {
        /* Pro444：4:4:4（pf=1/2）10/12-bit */
        topos_frame_header p5 = base_fh();
        p5.profile = 5u;
        p5.pixel_format = 1u;
        p5.version_minor = 2u;
        uint8_t enc[TC_FRAME_HEADER_SIZE];
        MT_CHECK_EQ_I64(tc_frame_header_encode(&p5, enc), TC_OK);
        MT_CHECK_EQ_U64(enc[10], 5ull);
        topos_frame_header back;
        MT_CHECK_EQ_I64(tc_frame_header_decode(enc, sizeof(enc), &back), TC_OK);
        MT_CHECK_EQ_U64(back.profile, 5ull);

        /* Extreme：4:4:4 12-bit only（GBR 组合 + minor=3） */
        topos_frame_header p6 = base_fh();
        p6.profile = 6u;
        p6.pixel_format = 2u;
        p6.bit_depth = 12u;
        p6.version_minor = 3u;
        p6.color_matrix = 0u;   /* GBR 契约 */
        MT_CHECK_EQ_I64(tc_frame_header_encode(&p6, enc), TC_OK);
        MT_CHECK_EQ_U64(enc[10], 6ull);
        MT_CHECK_EQ_I64(tc_frame_header_decode(enc, sizeof(enc), &back), TC_OK);
        MT_CHECK_EQ_U64(back.profile, 6ull);

        /* 交叉规则：Extreme + 10-bit → MALFORMED；Pro444 + 4:2:2 → MALFORMED */
        topos_frame_header e10 = p6;
        e10.bit_depth = 10u;
        e10.version_minor = 3u; /* pf=2 仍需 minor=3（先撞格式交叉） */
        MT_CHECK_EQ_I64(tc_frame_header_encode(&e10, enc), TC_ERR_MALFORMED);
        topos_frame_header p422 = p5;
        p422.pixel_format = 0u;
        p422.version_minor = 0u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&p422, enc), TC_ERR_MALFORMED);
    }

    /* ---- v1.6（TRAW 批 1）：pf=3 CFA / profile 7 / minor=5 / transfer=20 ---- */
    {
        topos_frame_header tr = base_fh();
        tr.profile = 7u;
        tr.pixel_format = 3u;
        tr.bit_depth = 12u;
        tr.version_minor = 5u;
        tr.plane_count = 4u;
        tr.slice_count = 4u;
        tr.color_transfer = TC_TRANSFER_TRAW_LOG0;
        tr.color_matrix = 0u;   /* CFA identity 契约 */
        tr.chroma_siting = 0u;
        uint8_t enc[TC_FRAME_HEADER_SIZE];
        MT_CHECK_EQ_I64(tc_frame_header_encode(&tr, enc), TC_OK);
        MT_CHECK_EQ_U64(enc[7], 5ull);                 /* minor=5 扩展代 */
        MT_CHECK_EQ_U64(enc[10], 7ull);
        MT_CHECK_EQ_U64(enc[11], 3ull);
        MT_CHECK_EQ_U64(enc[34], (unsigned long long)TC_TRANSFER_TRAW_LOG0);
        topos_frame_header back;
        MT_CHECK_EQ_I64(tc_frame_header_decode(enc, sizeof(enc), &back), TC_OK);
        /* CFA 几何：4 平面各 (36+1)/2=18 可见宽、(20+1)/2=10 可见高；
         * coded 18→24？18 pad8 = 24 错——18 pad8 = 24-6 = 24?（pad8(18)=24×0=24? 18/8=2.25→3×8=24）正确=24 */
        MT_CHECK_EQ_U64(back.plane_count, 4ull);
        MT_CHECK_EQ_U64(back.plane_visible_w[0], 18ull);
        MT_CHECK_EQ_U64(back.plane_visible_h[0], 10ull);
        MT_CHECK_EQ_U64(back.plane_visible_w[3], 18ull);
        MT_CHECK_EQ_U64(back.plane_visible_h[3], 10ull);
        MT_CHECK_EQ_U64(back.plane_coded_w[0], 24ull);  /* pad8(18) */
        MT_CHECK_EQ_U64(back.plane_coded_h[0], 16ull);  /* pad8(10) */
        MT_CHECK_EQ_U64(back.plane_block_cols[0], 3ull);
        MT_CHECK_EQ_U64(back.plane_block_rows[0], 2ull);

        /* 交叉规则反例：matrix≠0 / siting≠0 / transfer≠LOG0 / qm≠0 /
         * alpha≠0 / bd≠12 / pf≠3 / minor<5 包络 */
        topos_frame_header bad = tr;
        bad.color_matrix = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_MALFORMED);
        bad = tr; bad.chroma_siting = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_MALFORMED);
        bad = tr; bad.color_transfer = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_MALFORMED);
        bad = tr; bad.qmatrix_id = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_MALFORMED);
        bad = tr; bad.alpha_mode = 1u; bad.alpha_bit_depth = 16u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_MALFORMED);
        bad = tr; bad.bit_depth = 10u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_MALFORMED);
        bad = tr; bad.profile = 3u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_MALFORMED);
        bad = tr; bad.version_minor = 4u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_UNSUPPORTED_VERSION);
        bad = tr; bad.plane_count = 3u;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&bad, enc), TC_ERR_MALFORMED);

        /* TRAW_LOG0 私有值反向规则：非 TRAW 流携带即拒 */
        topos_frame_header yuv = base_fh();
        yuv.color_transfer = TC_TRANSFER_TRAW_LOG0;
        MT_CHECK_EQ_I64(tc_frame_header_encode(&yuv, enc), TC_ERR_MALFORMED);
    }

    /* ---- M9：V2 字段规则（spec v2 §2；V2-VLC 基底逐字段 patch）---- */
    {
        topos_frame_header fh2 = base_fh();
        fh2.version_major = 2u;
        fh2.entropy_mode = 1u;
        fh2.codebook_version = 1u;
        fh2.pixel_format = 1u; /* V2 无 minor 枚举代规则：pf=1 合法 */
        uint8_t v2[TC_FRAME_HEADER_SIZE];
        MT_CHECK_EQ_I64(tc_frame_header_encode(&fh2, v2), TC_OK);
        topos_frame_header back;
        MT_CHECK_EQ_I64(tc_frame_header_decode(v2, sizeof(v2), &back), TC_OK);
        MT_CHECK_EQ_U64(back.version_major, 2ull);
        MT_CHECK_EQ_U64(back.entropy_mode, 1ull);
        MT_CHECK_EQ_U64(back.codebook_version, 1ull);

        uint8_t b1_[1] = { 1 };
        uint8_t b2_[1] = { 2 };
        uint8_t b0_[1] = { 0 };
        topos_frame_header tmp2;
        /* V2 基底逐字段 patch（patched_decode 以 v2 为基底重算 CRC）：
         * entropy_mode=2 未知 / codebook_version 与 mode 不配套 /
         * coding_mode≠0（V2.1 占位）/ reserved_v2_0≠0 */
        MT_CHECK_EQ_I64(patched_decode(v2, 45u, b2_, 1u, &tmp2),
                        TC_ERR_UNSUPPORTED_VERSION);
        MT_CHECK_EQ_I64(patched_decode(v2, 46u, b0_, 1u, &tmp2),
                        TC_ERR_UNSUPPORTED_VERSION);
        MT_CHECK_EQ_I64(patched_decode(v2, 47u, b1_, 1u, &tmp2),
                        TC_ERR_UNSUPPORTED_VERSION);
        MT_CHECK_EQ_I64(patched_decode(v2, 48u, b1_, 1u, &tmp2),
                        TC_ERR_MALFORMED);
        /* validate 层等价构造校验（见下方 bad 系列） */
        topos_frame_header bad = fh2;
        bad.entropy_mode = 0u; /* cbv 仍 1 */
        MT_CHECK_EQ_I64(tc_frame_header_validate(&bad), TC_ERR_UNSUPPORTED_VERSION);
        bad = fh2;
        bad.coding_mode = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_validate(&bad), TC_ERR_UNSUPPORTED_VERSION);
        bad = fh2;
        bad.version_minor = 1u; /* V2.0 minor 恒 0 */
        MT_CHECK_EQ_I64(tc_frame_header_validate(&bad), TC_ERR_UNSUPPORTED_VERSION);
        bad = fh2;
        bad.flags = 0x0002u; /* tile_layout 占位 */
        MT_CHECK_EQ_I64(tc_frame_header_validate(&bad), TC_ERR_UNSUPPORTED_VERSION);
        bad = fh2;
        bad.frame_type = 1u; /* P 帧占位（V2.1） */
        MT_CHECK_EQ_I64(tc_frame_header_validate(&bad), TC_ERR_UNSUPPORTED_VERSION);
        /* V1 + 任何 V2 字段非 0 → MALFORMED（等价旧 reserved0!=0） */
        topos_frame_header v1b = base_fh();
        v1b.entropy_mode = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_validate(&v1b), TC_ERR_MALFORMED);
        v1b = base_fh();
        v1b.coding_mode = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_validate(&v1b), TC_ERR_MALFORMED);
    }

    /* 命名辅助（编译期冒烟：未知值不越界） */
    MT_CHECK(strcmp(tc_profile_name(3), "Standard") == 0);
    MT_CHECK(strcmp(tc_color_primaries_name(9), "bt2020") == 0);
    MT_CHECK(strcmp(tc_color_matrix_name(0), "identity(GBR)") == 0);
    MT_CHECK(strcmp(tc_color_transfer_name(99), "unknown") == 0);

    return MT_MAIN_RETURN();
}
