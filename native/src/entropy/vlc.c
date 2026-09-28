/* V2 canonical VLC —— 码表构建与慢路解码（实现；接口说明见 vlc.h）。
 *
 * 确定性：canonical 指派/表填充均为纯函数；冻结表一经构建进程内只读
 * （release 发布保证无数据竞争，模式同 tc_rice_lut）。
 */
#include "vlc.h"

#include "../common/port_mutex.h"
#include <string.h>

#include "../common/error.h"

/* P1 瘦身后 entry 打包内联于 vlc.h 热路径（sym/nbits/行号直取），
 * 独立打包函数不再需要。 */

int32_t tc_vlc_book_build(tc_vlc_book* b, const uint8_t* len, uint32_t nsym,
                          uint32_t kind)
{
    if (b == NULL || len == NULL || nsym == 0u || nsym > TC_VLC_MAX_SYMS ||
        kind > (uint32_t)TC_VLC_KIND_LVL) {
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(b, 0, sizeof(*b));
    b->nsym = nsym;
    b->kind = kind;

    /* Kraft 取等校验（Σ 2^(L−l) == 2^L；len=0 不在字母表） */
    uint64_t kraft = 0u;
    for (uint32_t s = 0u; s < nsym; ++s) {
        if (len[s] > TC_VLC_MAX_CODE_BITS) { return TC_ERR_INVALID_ARGUMENT; }
        if (len[s] != 0u) { kraft += UINT64_C(1) << (TC_VLC_MAX_CODE_BITS - len[s]); }
    }
    if (kraft != (UINT64_C(1) << TC_VLC_MAX_CODE_BITS)) {
        return TC_ERR_INVALID_ARGUMENT;
    }

    /* canonical 指派：(len, sym) 升序连续分配；同时建 order/count/first_* */
    uint32_t code = 0u;
    uint32_t norder = 0u;
    for (uint32_t l = 1u; l <= TC_VLC_MAX_CODE_BITS; ++l) {
        b->first_code[l] = code;
        b->first_idx[l] = norder;
        b->count[l] = 0u;
        for (uint32_t s = 0u; s < nsym; ++s) {
            if (len[s] != l) { continue; }
            b->len[s] = (uint8_t)l;
            b->code[s] = code;
            b->order[norder++] = (uint16_t)s;
            b->count[l]++;
            code++;
        }
        code <<= 1;
    }
    b->first_code[0] = 0u; /* 未用 */
    b->first_idx[0] = 0u;
    b->count[0] = 0u;

    /* 一级/二级表：INVALID = 0x0000（合法 entry 恒有 nbits ≥ 1）。
     * 短码（≤12 bit）：bits0..5=sym、6..10=nbits、bit15=0；
     * 长码前缀槽：bit15=1、bits0..5=二级行号。 */
    memset(b->primary, 0, sizeof(b->primary));
    memset(b->secondary, 0, sizeof(b->secondary));
    b->sec_rows = 0u;

    for (uint32_t s = 0u; s < nsym; ++s) {
        uint32_t l = b->len[s];
        if (l == 0u) { continue; }
        uint32_t c = b->code[s];
        if (l <= TC_VLC_PRIMARY_BITS) {
            uint32_t base = c << (TC_VLC_PRIMARY_BITS - l);
            uint32_t cnt = 1u << (TC_VLC_PRIMARY_BITS - l);
            uint32_t e = s | (l << 6);
            for (uint32_t i = 0u; i < cnt; ++i) { b->primary[base + i] = (uint16_t)e; }
        } else {
            /* 长码：12-bit 前缀 → 二级行（同前缀多码共用行） */
            uint32_t pfx = c >> (l - TC_VLC_PRIMARY_BITS);
            uint32_t row;
            if ((b->primary[pfx] & 0x8000u) != 0u) {
                row = b->primary[pfx] & 0x3Fu;
            } else {
                if (b->primary[pfx] != 0u) {
                    return TC_ERR_INVALID_ARGUMENT; /* 前缀冲突（非前缀无关） */
                }
                if (b->sec_rows >= TC_VLC_MAX_SEC_ROWS) {
                    return TC_ERR_INVALID_ARGUMENT;
                }
                row = b->sec_rows++;
                b->primary[pfx] = (uint16_t)(0x8000u | row);
            }
            /* 后 (l−12) 位定位二级索引高段；其余低段复制填充。
             * nbits 存完整码长（含 12-bit 前缀）——快路单次查表一次 skip。 */
            uint32_t tail = l - TC_VLC_PRIMARY_BITS;          /* 1..8 */
            uint32_t suffix = c & ((1u << tail) - 1u);
            uint32_t lo_bits = TC_VLC_SECONDARY_IDX_BITS - tail;
            uint32_t base = suffix << lo_bits;
            uint32_t cnt = 1u << lo_bits;
            uint32_t e = s | (l << 6);
            for (uint32_t i = 0u; i < cnt; ++i) {
                b->secondary[row][base + i] = (uint16_t)e;
            }
        }
    }
    return TC_OK;
}

/* ---- 慢路：逐位 canonical 步进（流尾窗口不足；语义与快路一致） ---- */

int32_t tc_vlc_decode_sym_slow(tc_bitreader* br, const tc_vlc_book* b,
                               uint32_t* sym_out)
{
    if (br->error != 0) { return br->error; }
    uint32_t code = 0u;
    for (uint32_t l = 1u; l <= TC_VLC_MAX_CODE_BITS; ++l) {
        uint32_t bit = 0u;
        int32_t rc = tc_bitreader_read_bit(br, &bit);
        if (rc != TC_OK) { return rc; }
        code = (code << 1) | bit;
        if (b->count[l] != 0u && code >= b->first_code[l] &&
            code - b->first_code[l] < b->count[l]) {
            *sym_out = b->order[b->first_idx[l] + (code - b->first_code[l])];
            return TC_OK;
        }
    }
    tc_bitreader_fail(br, TC_ERR_MALFORMED);
    return TC_ERR_MALFORMED;
}

/* 类别融合慢路：码字（逐位）+ (cat−1) 位后缀 + 域校验。
 * 与 tc_vlc_decode_cat 快路逐位一致（流尾窗口不足时的精确回退）。 */
int32_t tc_vlc_decode_cat_slow(tc_bitreader* br, const tc_vlc_book* b,
                               uint32_t m_max, uint32_t* m_out)
{
    uint32_t cat = 0u;
    int32_t rc = tc_vlc_decode_sym_slow(br, b, &cat);
    if (rc != TC_OK) { return rc; }
    uint32_t m = 0u;
    if (cat > 1u) {
        uint32_t suffix = 0u;
        rc = tc_bitreader_read_bits(br, cat - 1u, &suffix);
        if (rc != TC_OK) { return rc; }
        m = (1u << (cat - 1u)) | suffix;
    } else if (cat == 1u) {
        m = 1u;
    }
    if (m > m_max) {
        tc_bitreader_fail(br, TC_ERR_MALFORMED);
        return TC_ERR_MALFORMED;
    }
    *m_out = m;
    return TC_OK;
}

/* ---- 冻结表（进程级缓存；结构同 tc_rice_lut 的 ensure/ready 模式） ---- */

static tc_vlc_book g_books[3][TC_VLC_BOOKS];
static atomic_int g_ready[3][TC_VLC_BOOKS];
static topos_mutex g_mux = TOPOS_MUTEX_INIT;

static const uint8_t* table_row(uint32_t family, uint32_t book)
{
    switch (family) {
    case TC_VLC_FAMILY_DC: return tc_vlc_dc_len[book];
    case TC_VLC_FAMILY_RUN: return tc_vlc_run_len[book];
    default: return tc_vlc_lvl_len[book];
    }
}

static const uint32_t g_nsyms[3] = {
    TC_VLC_DC_SYMS, TC_VLC_RUN_SYMS, TC_VLC_LVL_SYMS
};

int32_t tc_vlc_tables_ensure(uint32_t family, uint32_t book)
{
    if (family > 2u || book >= TC_VLC_BOOKS) { return TC_ERR_INVALID_ARGUMENT; }
    if (atomic_load_explicit(&g_ready[family][book], memory_order_acquire) != 0) {
        return TC_OK;
    }
    topos_mutex_lock(&g_mux);
    int32_t rc = TC_OK;
    if (atomic_load_explicit(&g_ready[family][book], memory_order_relaxed) == 0) {
        rc = tc_vlc_book_build(&g_books[family][book], table_row(family, book),
                               g_nsyms[family], family);
        if (rc == TC_OK) {
            atomic_store_explicit(&g_ready[family][book], 1, memory_order_release);
        }
    }
    topos_mutex_unlock(&g_mux);
    return rc;
}

const tc_vlc_book* tc_vlc_book_get(uint32_t family, uint32_t book)
{
    if (family > 2u || book >= TC_VLC_BOOKS) { return NULL; }
    if (atomic_load_explicit(&g_ready[family][book], memory_order_acquire) == 0) {
        return NULL;
    }
    return &g_books[family][book];
}
