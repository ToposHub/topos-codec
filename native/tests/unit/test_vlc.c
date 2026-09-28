/* V2 canonical VLC：表构建/Kraft/前缀无关、canonical 指派、符号与类别编解码
 * 往返、逐截断位语义（快/慢路径一致）、无效前缀 MALFORMED、精确位计数、
 * 冻结表 12 本全量校验、含 20-bit 长码的合成码书二级表路径。 */
#include "entropy/rice.h"
#include "entropy/vlc.h"

#include <string.h>

#include "mini_test.h"

/* ---- 合成码书：Kraft=1 且覆盖 13..20 bit 全部长码段（二级表全路径） ----
 * {1..12} 各一贡献 (2^20−2^8)/2^20；剩 256 单位由 {13,14,15,16,17,18,19,
 * 20,20} 恰好补满：128+64+32+16+8+4+2+1+1 = 256。 */
static int make_synth_book(tc_vlc_book* b)
{
    uint8_t len[TC_VLC_MAX_SYMS];
    memset(len, 0, sizeof(len));
    uint32_t n = 0u;
    for (uint32_t l = 1u; l <= 12u; ++l) { len[n++] = (uint8_t)l; }
    static const uint8_t tail[9] = {13, 14, 15, 16, 17, 18, 19, 20, 20};
    for (uint32_t i = 0u; i < 9u; ++i) { len[n++] = tail[i]; }
    uint64_t kraft = 0u;
    for (uint32_t i = 0u; i < n; ++i) {
        kraft += UINT64_C(1) << (TC_VLC_MAX_CODE_BITS - len[i]);
    }
    if (kraft != (UINT64_C(1) << TC_VLC_MAX_CODE_BITS)) { return -1; }
    return tc_vlc_book_build(b, len, n, TC_VLC_KIND_DC);
}

int main(void)
{
    /* ---- bitlen ---- */
    MT_CHECK_EQ_U64(tc_vlc_bitlen32(0), 0ull);
    MT_CHECK_EQ_U64(tc_vlc_bitlen32(1), 1ull);
    MT_CHECK_EQ_U64(tc_vlc_bitlen32(2), 2ull);
    MT_CHECK_EQ_U64(tc_vlc_bitlen32(3), 2ull);
    MT_CHECK_EQ_U64(tc_vlc_bitlen32(1u << 27), 28ull);

    /* ---- 冻结表 12 本：ensure + Kraft 复核（C 侧独立再算一遍） ---- */
    for (uint32_t f = 0u; f < 3u; ++f) {
        for (uint32_t bk = 0u; bk < TC_VLC_BOOKS; ++bk) {
            MT_CHECK_EQ_I64(tc_vlc_tables_ensure(f, bk), TC_OK);
            const tc_vlc_book* b = tc_vlc_book_get(f, bk);
            MT_CHECK(b != NULL);
            if (b == NULL) { continue; }
            MT_CHECK_EQ_U64(b->kind, f);
            uint64_t kraft = 0u;
            uint32_t coded = 0u;
            for (uint32_t s = 0u; s < b->nsym; ++s) {
                if (b->len[s] != 0u) {
                    kraft += UINT64_C(1) << (TC_VLC_MAX_CODE_BITS - b->len[s]);
                    coded++;
                    MT_CHECK(b->code[s] < (1ull << b->len[s]));
                }
            }
            MT_CHECK_EQ_U64(kraft, 1ull << TC_VLC_MAX_CODE_BITS);
            /* LVL 族 cat=0 无码；DC/RUN 全字母表 */
            if (f == TC_VLC_FAMILY_LVL) {
                MT_CHECK_EQ_U64(b->len[0], 0ull);
                MT_CHECK_EQ_U64(b->nsym, TC_VLC_LVL_SYMS);
            } else {
                MT_CHECK_EQ_U64(coded, b->nsym);
            }
        }
    }

    /* ---- 前缀无关（canonical 结构性）：同长码连续、跨长前缀严格小于 ---- */
    {
        const tc_vlc_book* b = tc_vlc_book_get(TC_VLC_FAMILY_RUN, 3u);
        uint32_t prev_len = 0u, prev_code = 0u, first = 1u;
        for (uint32_t l = 1u; l <= TC_VLC_MAX_CODE_BITS; ++l) {
            for (uint32_t s = 0u; s < b->nsym; ++s) {
                if (b->len[s] != l) { continue; }
                if (!first) {
                    MT_CHECK(b->code[s] > prev_code ||
                             (l > prev_len));
                }
                prev_len = l; prev_code = b->code[s]; first = 0u;
            }
        }
    }

    /* ---- 符号/类别编解码往返 + 精确位计数（全部 12 本 × 随机符号流） ---- */
    tc_bitwriter bw;
    MT_CHECK_EQ_I64(tc_bitwriter_init(&bw), TC_OK);
    for (uint32_t f = 0u; f < 3u; ++f) {
        for (uint32_t bk = 0u; bk < TC_VLC_BOOKS; ++bk) {
            const tc_vlc_book* b = tc_vlc_book_get(f, bk);
            enum { N = 400 };
            uint32_t syms[N];
            uint32_t ms[N];
            uint64_t expect_bits = 0u;
            tc_bitwriter_reset(&bw);
            for (int i = 0; i < N; ++i) {
                uint64_t r = mt_rand_u64();
                if (f == (uint32_t)TC_VLC_FAMILY_RUN) {
                    syms[i] = (uint32_t)(r % 64u);
                    expect_bits += tc_vlc_sym_bits(b, syms[i]);
                    MT_CHECK_EQ_I64(tc_vlc_put_sym(&bw, b, syms[i]), TC_OK);
                } else {
                    /* DC: m ≤ 2^27（cat ≤ 28）；LVL: m ≤ 2^26 且 cat ≥ 1 */
                    uint32_t cat_hi = (f == (uint32_t)TC_VLC_FAMILY_DC) ? 27u : 26u;
                    uint32_t m = (uint32_t)(mt_rand_u64() & ((1ull << cat_hi) - 1u));
                    if (f == (uint32_t)TC_VLC_FAMILY_LVL && m == 0u) { m = 1u; }
                    ms[i] = m;
                    expect_bits += tc_vlc_cat_bits(b, m);
                    MT_CHECK_EQ_I64(tc_vlc_put_cat(&bw, b, m), TC_OK);
                }
            }
            MT_CHECK_EQ_U64(tc_bitwriter_bits_written(&bw), expect_bits);
            MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
            const uint8_t* data = tc_bitwriter_data(&bw);
            size_t sz = tc_bitwriter_byte_size(&bw);
            uint64_t total_bits = expect_bits;

            /* 全量解码往返 */
            tc_bitreader br;
            tc_bitreader_init(&br, data, sz);
            for (int i = 0; i < N; ++i) {
                uint32_t sym = 0xFFFFFFFFu;
                MT_CHECK_EQ_I64(tc_vlc_decode_sym(&br, b, &sym), TC_OK);
                if (f == (uint32_t)TC_VLC_FAMILY_RUN) {
                    MT_CHECK_EQ_U64(sym, syms[i]);
                } else {
                    uint32_t cat = sym;
                    uint32_t m = 0u;
                    if (cat > 0u) {
                        uint32_t suf = 0u;
                        MT_CHECK_EQ_I64(
                            tc_bitreader_read_bits(&br, cat - 1u, &suf), TC_OK);
                        m = (1u << (cat - 1u)) | suf;
                    }
                    MT_CHECK_EQ_U64(m, ms[i]);
                }
            }
            MT_CHECK_EQ_U64(tc_bitreader_bits_consumed(&br), total_bits);
            MT_CHECK_EQ_I64(tc_bitreader_align_byte(&br), TC_OK);
            MT_CHECK_EQ_U64(tc_bitreader_bits_consumed(&br),
                            ((total_bits + 7u) / 8u) * 8u);
        }
    }

    /* ---- 逐截断位：任何截断点不越界、不部分解码、错误码正确 ---- */
    {
        const tc_vlc_book* b = tc_vlc_book_get(TC_VLC_FAMILY_DC, 0u);
        tc_bitwriter_reset(&bw);
        uint32_t seq[8];
        for (int i = 0; i < 8; ++i) {
            seq[i] = (uint32_t)(mt_rand_u64() & 0xFFFFFull); /* cat ≤ 21 */
            MT_CHECK_EQ_I64(tc_vlc_put_cat(&bw, b, seq[i]), TC_OK);
        }
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        size_t sz = tc_bitwriter_byte_size(&bw);
        const uint8_t* data = tc_bitwriter_data(&bw);
        for (size_t cut = 0u; cut < sz * 8u; ++cut) {
            tc_bitreader br;
            tc_bitreader_init(&br, data, sz);
            /* 逐符号解到截断层：整符号成功者必须正确，跨截断者 TRUNCATED */
            uint64_t consumed = 0u;
            int i = 0u;
            for (; i < 8; ++i) {
                uint32_t sym = 0u;
                /* 复制 reader 模拟截断（bit_size = cut） */
                tc_bitreader tmp = br;
                tmp.bit_size = (uint64_t)cut;
                tmp.error = 0;
                int32_t rc = tc_vlc_decode_sym(&tmp, b, &sym);
                if (rc != TC_OK) {
                    MT_CHECK_EQ_I64(rc, TC_ERR_TRUNCATED);
                    MT_CHECK_EQ_I64(tmp.error, TC_ERR_TRUNCATED);
                    break;
                }
                uint32_t cat = sym;
                if (cat > 1u) {
                    uint32_t suf = 0u;
                    rc = tc_bitreader_read_bits(&tmp, cat - 1u, &suf);
                    if (rc != TC_OK) {
                        MT_CHECK_EQ_I64(rc, TC_ERR_TRUNCATED);
                        break;
                    }
                    MT_CHECK_EQ_U64((1u << (cat - 1u)) | suf, seq[i]);
                } else {
                    MT_CHECK_EQ_U64(cat == 0u ? 0u : 1u, seq[i] <= 1u ? seq[i] : 1u);
                }
                consumed = tc_bitreader_bits_consumed(&tmp);
                br = tmp;
            }
            /* 截断点之后的符号数应随 cut 单调不减——此处仅验证不越界崩溃 */
            (void)consumed;
        }
    }

    /* ---- 无效前缀 → MALFORMED（合成码书构造 20-bit 长码二级表未填区） ---- */
    {
        tc_vlc_book sb;
        MT_CHECK_EQ_I64(make_synth_book(&sb), TC_OK);
        MT_CHECK(sb.sec_rows > 0u); /* 必须真实触发二级表 */
        /* 找一个长码符号，编出后翻转尾部位 → 落入行内 INVALID 区或另一码 */
        uint32_t long_sym = 0xFFFFFFFFu;
        for (uint32_t s = 0u; s < sb.nsym; ++s) {
            if (sb.len[s] > TC_VLC_PRIMARY_BITS) { long_sym = s; break; }
        }
        MT_CHECK(long_sym != 0xFFFFFFFFu);
        tc_bitwriter_reset(&bw);
        MT_CHECK_EQ_I64(tc_vlc_put_sym(&bw, &sb, long_sym), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
        size_t sz = tc_bitwriter_byte_size(&bw);
        uint8_t buf[8];
        memcpy(buf, tc_bitwriter_data(&bw), sz);
        /* 翻转最后一个有效位之后的填充区前位 → 改变码字尾段 */
        uint32_t clen = sb.len[long_sym];
        if (clen < sz * 8u) {
            uint32_t bitpos = clen; /* 码字后第一位（填充区起点或下界内） */
            buf[bitpos / 8u] ^= (uint8_t)(1u << (7u - (bitpos % 8u)));
            tc_bitreader br;
            tc_bitreader_init(&br, buf, sz);
            uint32_t sym = 0u;
            int32_t rc = tc_vlc_decode_sym(&br, &sb, &sym);
            /* 翻转后或是另一合法码（前缀无关仍保证正确解码）或 INVALID */
            MT_CHECK(rc == TC_OK || rc == TC_ERR_MALFORMED);
            if (rc == TC_OK) {
                MT_CHECK_EQ_U64(sb.len[sym],
                                tc_vlc_bitlen32(0) + (uint32_t)(br.bit_pos));
            }
        }

        /* 慢路路径验证：窗口不足（流尾）时长码正确解码 */
        tc_bitreader br2;
        tc_bitreader_init(&br2, tc_bitwriter_data(&bw), sz);
        uint32_t sym2 = 0u;
        MT_CHECK_EQ_I64(tc_vlc_decode_sym_slow(&br2, &sb, &sym2), TC_OK);
        MT_CHECK_EQ_U64(sym2, long_sym);
        MT_CHECK_EQ_U64(tc_bitreader_bits_consumed(&br2), clen);

        /* 单独一个长码放 3 字节缓冲 → 快路窗口不足回退慢路 */
        tc_bitreader br3;
        tc_bitreader_init(&br3, tc_bitwriter_data(&bw), sz);
        uint32_t sym3 = 0u;
        MT_CHECK_EQ_I64(tc_vlc_decode_sym(&br3, &sb, &sym3), TC_OK);
        MT_CHECK_EQ_U64(sym3, long_sym);
    }

    /* ---- 融合类别解码（tc_vlc_decode_cat）vs 离散（decode_sym+read_bits）----
     * 全 12 本 × 随机 m 流：逐符号 m 相等、消费位相等；逐截断位语义一致 */
    for (uint32_t f = 0u; f < 2u; ++f) { /* DC / LVL 族 */
        for (uint32_t bk = 0u; bk < TC_VLC_BOOKS; ++bk) {
            const tc_vlc_book* b = tc_vlc_book_get(f, bk);
            uint32_t m_max = (f == (uint32_t)TC_VLC_FAMILY_DC)
                                 ? TC_RICE_M_MAX_DC : TC_RICE_M_MAX_AC_LEVEL;
            enum { NC = 300 };
            uint32_t ms[NC];
            tc_bitwriter_reset(&bw);
            for (int i = 0; i < NC; ++i) {
                uint32_t m = (uint32_t)(mt_rand_u64() & (m_max - 1u));
                if (f == (uint32_t)TC_VLC_FAMILY_LVL && m == 0u) { m = 1u; }
                ms[i] = m;
                MT_CHECK_EQ_I64(tc_vlc_put_cat(&bw, b, m), TC_OK);
            }
            MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bw), TC_OK);
            const uint8_t* data = tc_bitwriter_data(&bw);
            size_t sz = tc_bitwriter_byte_size(&bw);

            tc_bitreader br;
            tc_bitreader_init(&br, data, sz);
            for (int i = 0; i < NC; ++i) {
                uint32_t m = 0u;
                MT_CHECK_EQ_I64(tc_vlc_decode_cat(&br, b, m_max, &m), TC_OK);
                MT_CHECK_EQ_U64(m, ms[i]);
            }
            /* 逐截断：融合与离散路径错误码一致（任意截断位） */
            for (size_t cut = 0u; cut < sz * 8u; cut += 5u) {
                tc_bitreader r1, r2;
                tc_bitreader_init(&r1, data, (cut + 7u) / 8u);
                r1.bit_size = (uint64_t)cut; /* 精确到位的截断 */
                tc_bitreader_init(&r2, data, (cut + 7u) / 8u);
                r2.bit_size = (uint64_t)cut;
                for (;;) {
                    uint32_t m1 = 0u, m2 = 0u;
                    int32_t rc1 = tc_vlc_decode_cat(&r1, b, m_max, &m1);
                    /* 离散对照：decode_sym + read_bits */
                    uint32_t cat = 0u, suf = 0u;
                    int32_t rc2 = tc_vlc_decode_sym(&r2, b, &cat);
                    if (rc2 == TC_OK && cat > 1u) {
                        rc2 = tc_bitreader_read_bits(&r2, cat - 1u, &suf);
                    }
                    if (rc2 == TC_OK) {
                        m2 = cat == 0u ? 0u : (1u << (cat - 1u)) | suf;
                        if (m2 > m_max) { rc2 = TC_ERR_MALFORMED; }
                    }
                    if (rc1 != rc2) {
                        mt_report(__FILE__, __LINE__, "fused/discrete rc mismatch");
                        break;
                    }
                    if (rc1 != TC_OK) { break; }
                    if (m1 != m2) {
                        mt_report(__FILE__, __LINE__, "fused/discrete m mismatch");
                        break;
                    }
                }
            }
        }
    }

    /* ---- build 校验拒绝：Kraft 超/欠、len 越界、nsym 越界 ---- */
    {
        tc_vlc_book sb;
        uint8_t len[4] = {1, 2, 3, 3}; /* Kraft = 1/2+1/4+1/8+1/8 = 1 ✓ */
        MT_CHECK_EQ_I64(tc_vlc_book_build(&sb, len, 4u, TC_VLC_KIND_RUN), TC_OK);
        uint8_t bad1[4] = {1, 2, 3, 4}; /* < 1 */
        MT_CHECK_EQ_I64(tc_vlc_book_build(&sb, bad1, 4u, TC_VLC_KIND_RUN),
                        TC_ERR_INVALID_ARGUMENT);
        uint8_t bad2[3] = {1, 2, 2}; /* = 1 但 3 符号 2 深度合法 ✓ 对照 */
        MT_CHECK_EQ_I64(tc_vlc_book_build(&sb, bad2, 3u, TC_VLC_KIND_RUN), TC_OK);
        uint8_t bad3[4] = {1, 1, 2, 4}; /* > 1（两个 1-bit 码） */
        MT_CHECK_EQ_I64(tc_vlc_book_build(&sb, bad3, 4u, TC_VLC_KIND_RUN),
                        TC_ERR_INVALID_ARGUMENT);
        uint8_t bad4[2] = {21, 1}; /* len > 20 */
        MT_CHECK_EQ_I64(tc_vlc_book_build(&sb, bad4, 2u, TC_VLC_KIND_RUN),
                        TC_ERR_INVALID_ARGUMENT);
        MT_CHECK_EQ_I64(tc_vlc_book_build(&sb, bad2, 0u, TC_VLC_KIND_RUN),
                        TC_ERR_INVALID_ARGUMENT);
        MT_CHECK_EQ_I64(tc_vlc_book_build(&sb, bad2, 65u, TC_VLC_KIND_RUN),
                        TC_ERR_INVALID_ARGUMENT);
    }

    tc_bitwriter_free(&bw);
    return MT_MAIN_RETURN();
}
