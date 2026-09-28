/* V2 canonical VLC 熵编码（M9，spec v2 §4；ADR-C027）。
 *
 * 与 Rice 的关系：V1（major=1）永久走 rice.h；major=2 且 entropy_mode=1
 * 的颜色 slice 走本模块。符号语义（DC 差分 / (run,level)+EOB）不变，
 * 仅码字层替换为冻结 canonical VLC（码长表 vlc_tables.h，码字由
 * tc_vlc_book_build 确定性重建）。
 *
 * 码表结构（spec v2 §4.2/§4.3）：
 *  - 三族（DC_CAT 29 符号 / RUN 64 符号 / LEVEL_CAT 27 符号，cat=0 无码）
 *    × 4 本（qp 档位）冻结码长；
 *  - 编码端逐族选最小 total_bits（tc_vlc_*_bits 精确位计数，M7 口径），
 *    同分取最小 book id；
 *  - 解码端 12-bit 一级表 + 长码（13..20 bit）8-bit 二级表；窗口不足或
 *    长码近流尾时回退逐位 canonical 慢路（任意截断位置语义正确）。
 *
 * entry 打包（u16，P1 提速瘦身边改；纯内存格式，位流不受影响）：
 *   短码（≤12 bit，bit15=0）：
 *     bits  0..5  symbol（≤63）
 *     bits  6..10 nbits（1..12）
 *   长码前缀（bit15=1）：
 *     bits  0..5  secondary 行号（≤63）
 *   二级表 entry（u16，bit15=0）：
 *     bits  0..5  symbol
 *     bits  6..10 nbits（13..20，完整码长含 12-bit 前缀）
 *   INVALID = 0x0000（合法短/二级 entry 恒有 nbits ≥ 1，0 值天然保留）
 */
#ifndef TOPOS_INTERNAL_VLC_H
#define TOPOS_INTERNAL_VLC_H

#include <stdatomic.h>
#include <stdint.h>

#include "../bitstream/bitio.h"
#include "vlc_tables.h"

#define TC_VLC_MAX_SYMS 64u          /* RUN 族 64；DC 29 / LVL 28 */
#define TC_VLC_PRIMARY_BITS 12u /* P1 实验：可降 11 —— 主表 4KB/本 更贴 L1 */
#define TC_VLC_PRIMARY_SIZE (1u << TC_VLC_PRIMARY_BITS)
#define TC_VLC_PRIMARY_PEEK (64u - TC_VLC_PRIMARY_BITS) /* 主表索引移位量 */
#define TC_VLC_SECONDARY_IDX_BITS 8u /* ≥ MAX_CODE_BITS−PRIMARY_BITS（11 位主表时 tail 可达 9） */
#define TC_VLC_SEC_IDX_MASK ((1u << TC_VLC_SECONDARY_IDX_BITS) - 1u)
#define TC_VLC_MAX_SEC_ROWS 64u      /* 长码 ≤ 字母表大小 ≤ 64 → 12-bit 前缀行数 ≤ 64 */

#define TC_VLC_E_INVALID UINT16_C(0)

#if defined(__GNUC__) || defined(__clang__)
#define TC_VLC_PREFETCH(p) __builtin_prefetch((p))
#else
#define TC_VLC_PREFETCH(p) ((void)(p))
#endif

/* P2 解码提速：快路符号解码强制内联进扫描循环。两函数体小（快路 ~20
 * 指令）而调用频率 = 每符号 1 次（粒噪 ~18 符号/块）——-O3 下 clang 仍按
 * 代码体积启发式保留真实调用（实测 sample：call/ret + struct 存储转发
 * 挂在符号间依赖链上，熵解码占整帧 ~48%）。内联后跨符号重叠 refill。 */
#if defined(__GNUC__) || defined(__clang__)
#define TC_VLC_ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#define TC_VLC_ALWAYS_INLINE static inline
#endif

enum {
    TC_VLC_KIND_DC = 0,
    TC_VLC_KIND_RUN = 1,
    TC_VLC_KIND_LVL = 2,
    TC_VLC_KIND_INVALID = 3
};
enum { TC_VLC_FAMILY_DC = 0, TC_VLC_FAMILY_RUN = 1, TC_VLC_FAMILY_LVL = 2 };

typedef struct tc_vlc_book {
    uint32_t nsym;      /* 字母表大小（len[] 有效长度；符号值 < nsym） */
    uint32_t kind;      /* TC_VLC_KIND_*（审计/单测一致性校验用） */
    uint16_t order[TC_VLC_MAX_SYMS];             /* (len, sym) 升序 */
    uint32_t code[TC_VLC_MAX_SYMS];              /* canonical 码字 */
    uint8_t len[TC_VLC_MAX_SYMS];                /* 0 = 不在字母表 */
    uint32_t first_code[TC_VLC_MAX_CODE_BITS + 1u]; /* 慢路步进 */
    uint32_t first_idx[TC_VLC_MAX_CODE_BITS + 1u];
    uint32_t count[TC_VLC_MAX_CODE_BITS + 1u];
    /* P1 瘦身：u16 entry（见文件头打包说明）。primary 8 KB/本，
     * secondary ≤32 KB/本（热行远小于此）——三本工作集压进 L1/L2。 */
    uint16_t primary[TC_VLC_PRIMARY_SIZE];
    uint16_t secondary[TC_VLC_MAX_SEC_ROWS][1u << TC_VLC_SECONDARY_IDX_BITS];
    uint32_t sec_rows;
} tc_vlc_book;

/* 由码长表构建（确定性）：canonical 指派码字 + 一级/二级解码表 + 慢路
 * 步进数据。校验：len ≤ TC_VLC_MAX_CODE_BITS、nsym ≤ 64、Kraft 取等、
 * 长码 12-bit 前缀行数 ≤ 64。供 tables_ensure 与单测合成表使用。 */
int32_t tc_vlc_book_build(tc_vlc_book* b, const uint8_t* len, uint32_t nsym,
                          uint32_t kind);

/* 冻结表（进程级缓存，模式同 tc_rice_lut_ensure）：family ∈ {DC,RUN,LVL}
 * （0..2），book ∈ 0..3。首次调用构建（互斥下完成），此后只读。 */
int32_t tc_vlc_tables_ensure(uint32_t family, uint32_t book);
const tc_vlc_book* tc_vlc_book_get(uint32_t family, uint32_t book);

/* ---- 便携位宽（cat = m 的有效位数；m=0 → 0） ---- */
static inline uint32_t tc_vlc_bitlen32(uint32_t m)
{
#if defined(__GNUC__) || defined(__clang__)
    return m == 0u ? 0u : (uint32_t)(32 - __builtin_clz(m));
#else
    uint32_t n = 0u;
    while ((m >> n) != 0u) { n++; }
    return n;
#endif
}

/* ---- 编码（内联；前置：符号在字母表内 len>0 —— 符号层域保证） ---- */

static inline int32_t tc_vlc_put_sym(tc_bitwriter* bw, const tc_vlc_book* b,
                                     uint32_t sym)
{
    return tc_bitwriter_put_bits_inline(bw, b->len[sym], b->code[sym]);
}

/* 类别符号（DC/LEVEL 的映射幅度 m）：码字 + (cat−1) 位字面后缀。
 * P1：码字与后缀融合为单次 64 位发射（同一位序列，少一次 writer 调用）。
 * cat=0 → m=0 无后缀；cat=1 → 后缀宽 0。 */
static inline int32_t tc_vlc_put_cat(tc_bitwriter* bw, const tc_vlc_book* b,
                                     uint32_t m)
{
    uint32_t cat = tc_vlc_bitlen32(m);
    uint32_t cl = b->len[cat];
    if (cat > 1u) {
        uint32_t sb = cat - 1u;
        uint64_t v = ((uint64_t)b->code[cat] << sb)
                     | (uint64_t)(m & ((1u << sb) - 1u));
        return tc_bitwriter_put_bits64_inline(bw, cl + sb, v);
    }
    return tc_bitwriter_put_bits_inline(bw, cl, b->code[cat]);
}

/* ---- 精确位计数（编码端选表；M7 rice_bits_exact 口径） ---- */

static inline uint32_t tc_vlc_sym_bits(const tc_vlc_book* b, uint32_t sym)
{
    return b->len[sym];
}

static inline uint32_t tc_vlc_cat_bits(const tc_vlc_book* b, uint32_t m)
{
    uint32_t cat = tc_vlc_bitlen32(m);
    return b->len[cat] + (cat > 1u ? cat - 1u : 0u);
}

/* ---- 解码 ---- */

/* 慢路（外部函数）：逐位 canonical 步进，任意窗口/截断语义正确。 */
int32_t tc_vlc_decode_sym_slow(tc_bitreader* br, const tc_vlc_book* b,
                               uint32_t* sym_out);
/* 类别融合慢路：码字逐位 + 后缀位 + 域校验（m > m_max → MALFORMED）。 */
int32_t tc_vlc_decode_cat_slow(tc_bitreader* br, const tc_vlc_book* b,
                               uint32_t m_max, uint32_t* m_out);

/* 解码一个符号（码字，不含后缀）。
 * 快路：窗口 ≥12 位查一级表（u16 entry：短码直出；长码 bit15=1 → 行号，
 * 窗口 ≥20 位时从当前窗口一次定位二级 entry 并按完整码长 skip——无中途
 * refill）。INVALID=0x0000；窗口不足 → 慢路（未消费任何位，语义一致）。 */
TC_VLC_ALWAYS_INLINE int32_t tc_vlc_decode_sym(tc_bitreader* br, const tc_vlc_book* b,
                                        uint32_t* sym_out)
{
    if (br->error != 0) { return br->error; }
    tc_bitreader_refill(br);
    if (br->bits >= TC_VLC_PRIMARY_BITS) {
        uint32_t e = b->primary[(uint32_t)(br->cache >> TC_VLC_PRIMARY_PEEK)];
        if (e == TC_VLC_E_INVALID) {
            tc_bitreader_fail(br, TC_ERR_MALFORMED);
            return TC_ERR_MALFORMED;
        }
        if ((e & 0x8000u) == 0u) { /* 短码直出 */
            tc_bitreader_skip(br, (e >> 6) & 0x1Fu);
            *sym_out = e & 0x3Fu;
            return TC_OK;
        }
        if (br->bits >= TC_VLC_PRIMARY_BITS + TC_VLC_SECONDARY_IDX_BITS) {
            uint32_t e2 = b->secondary[e & 0x3Fu]
                                       [(uint32_t)(br->cache >> (TC_VLC_PRIMARY_PEEK - TC_VLC_SECONDARY_IDX_BITS)) & TC_VLC_SEC_IDX_MASK];
            if (e2 == TC_VLC_E_INVALID) {
                tc_bitreader_fail(br, TC_ERR_MALFORMED);
                return TC_ERR_MALFORMED;
            }
            tc_bitreader_skip(br, (e2 >> 6) & 0x1Fu); /* 完整码长（含前缀） */
            *sym_out = e2 & 0x3Fu;
            return TC_OK;
        }
        /* 长码近流尾（窗口 <20）：未消费任何位，走慢路保证精确截断语义 */
    }
    return tc_vlc_decode_sym_slow(br, b, sym_out);
}

/* ---- 类别融合解码（M9 热路径）：码字 + (cat−1) 位后缀 + 域校验一次完成 ----
 * 快路：12-bit 一级表出 cat 后，码字与后缀在当前窗口内一次拼装、一次 skip
 * （总位宽 ≤ 20+25 < 64）；长码从当前窗口偷看前缀后 8 位定位二级 entry
 * （不做部分消费——窗口不足时须以未消费状态回退慢路）。窗口不足 → 慢路。
 * 语义与「decode_sym + read_bits(cat−1) + 域校验」逐位一致。 */
TC_VLC_ALWAYS_INLINE int32_t tc_vlc_decode_cat(tc_bitreader* br, const tc_vlc_book* b,
                                        uint32_t m_max, uint32_t* m_out)
{
    if (br->error != 0) { return br->error; }
    tc_bitreader_refill(br);
    if (br->bits >= TC_VLC_PRIMARY_BITS) {
        uint32_t e = b->primary[(uint32_t)(br->cache >> TC_VLC_PRIMARY_PEEK)];
        if (e == TC_VLC_E_INVALID) {
            tc_bitreader_fail(br, TC_ERR_MALFORMED);
            return TC_ERR_MALFORMED;
        }
        if ((e & 0x8000u) == 0u) {
            uint32_t cat = e & 0x3Fu;
            uint32_t nbits = (e >> 6) & 0x1Fu;
            uint32_t suffix_bits = cat > 1u ? cat - 1u : 0u;
            uint32_t total = nbits + suffix_bits;
            if (br->bits >= total) {
                uint32_t m;
                if (cat == 0u) {
                    m = 0u;
                } else {
                    m = (uint32_t)(br->cache >> (64u - total))
                        & ((1u << suffix_bits) - 1u);
                    m |= 1u << (cat - 1u);
                }
                if (m > m_max) {
                    tc_bitreader_fail(br, TC_ERR_MALFORMED);
                    return TC_ERR_MALFORMED;
                }
                tc_bitreader_skip(br, total);
                *m_out = m;
                return TC_OK;
            }
        } else if (br->bits >= TC_VLC_PRIMARY_BITS
                                  + TC_VLC_SECONDARY_IDX_BITS) {
            /* 长码：当前窗口已含前缀后 8 位（无需中途 refill）；窗口不足时
             * 不得窥视无效位——落慢路保证任意截断语义 */
            uint32_t e2 = b->secondary[e & 0x3Fu]
                                       [(uint32_t)(br->cache >> (TC_VLC_PRIMARY_PEEK - TC_VLC_SECONDARY_IDX_BITS)) & TC_VLC_SEC_IDX_MASK];
            if (e2 == TC_VLC_E_INVALID) {
                tc_bitreader_fail(br, TC_ERR_MALFORMED);
                return TC_ERR_MALFORMED;
            }
            uint32_t cat = e2 & 0x3Fu;
            uint32_t nbits = (e2 >> 6) & 0x1Fu; /* 总码长（含 12 前缀） */
            uint32_t suffix_bits = cat > 1u ? cat - 1u : 0u;
            uint32_t total = nbits + suffix_bits;
            if (br->bits >= total) {
                uint32_t m;
                if (cat == 0u) {
                    m = 0u;
                } else {
                    m = (uint32_t)(br->cache >> (64u - total))
                        & ((1u << suffix_bits) - 1u);
                    m |= 1u << (cat - 1u);
                }
                if (m > m_max) {
                    tc_bitreader_fail(br, TC_ERR_MALFORMED);
                    return TC_ERR_MALFORMED;
                }
                tc_bitreader_skip(br, total);
                *m_out = m;
                return TC_OK;
            }
        }
        /* 窗口不足：未消费任何位，走慢路 */
    }
    return tc_vlc_decode_cat_slow(br, b, m_max, m_out);
}

/* ---- (run,level) 融合对解码（P1 热路径）：一次错误检查 + 尽量单次 refill
 * 覆盖 RUN 码字与 LVL cat；EOB 经 *is_eob 返回（run==63 时不再消费 LVL）。
 * 慢路逐位回退与「decode_sym + decode_cat」逐位一致；窗口不足时以未消费
 * 状态回退（截断语义精确）。lvl 域校验保留（防御性，冻结表恒可过）。 */
static inline int32_t tc_vlc_decode_run_lvl(tc_bitreader* br,
                                            const tc_vlc_book* runb,
                                            const tc_vlc_book* lvlb,
                                            uint32_t m_max, uint32_t* run_out,
                                            uint32_t* lvl_m_out, uint32_t* is_eob)
{
    if (br->error != 0) { return br->error; }
    tc_bitreader_refill(br);
    if (br->bits >= TC_VLC_PRIMARY_BITS) {
        uint32_t e = runb->primary[(uint32_t)(br->cache >> TC_VLC_PRIMARY_PEEK)];
        if (e != TC_VLC_E_INVALID) {
            uint32_t run;
            if ((e & 0x8000u) == 0u) {
                run = e & 0x3Fu;
                tc_bitreader_skip(br, (e >> 6) & 0x1Fu);
            } else if (br->bits >= TC_VLC_PRIMARY_BITS
                                       + TC_VLC_SECONDARY_IDX_BITS) {
                uint32_t e2 = runb->secondary[e & 0x3Fu]
                            [(uint32_t)(br->cache >> (TC_VLC_PRIMARY_PEEK - TC_VLC_SECONDARY_IDX_BITS)) & TC_VLC_SEC_IDX_MASK];
                if (e2 == TC_VLC_E_INVALID) {
                    tc_bitreader_fail(br, TC_ERR_MALFORMED);
                    return TC_ERR_MALFORMED;
                }
                run = e2 & 0x3Fu;
                tc_bitreader_skip(br, (e2 >> 6) & 0x1Fu);
            } else {
                goto slow;
            }
            TC_VLC_PREFETCH(lvlb->primary + (uint32_t)(br->cache >> TC_VLC_PRIMARY_PEEK));
            *run_out = run;
            if (run == 63u) { *is_eob = 1u; return TC_OK; }
            *is_eob = 0u;
            return tc_vlc_decode_cat(br, lvlb, m_max, lvl_m_out);
        }
        tc_bitreader_fail(br, TC_ERR_MALFORMED);
        return TC_ERR_MALFORMED;
    }
slow:
    {
        uint32_t run = 0u;
        int32_t rc = tc_vlc_decode_sym(br, runb, &run);
        if (rc != TC_OK) { return rc; }
        *run_out = run;
        if (run == 63u) { *is_eob = 1u; return TC_OK; }
        *is_eob = 0u;
        return tc_vlc_decode_cat(br, lvlb, m_max, lvl_m_out);
    }
}

#endif /* TOPOS_INTERNAL_VLC_H */
