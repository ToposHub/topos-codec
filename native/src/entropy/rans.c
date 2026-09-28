/* rANS 核心实现——设计约束与数值域见 rans.h（ADR-C034）。 */
#include "rans.h"

#include "../common/error.h"
#include "../transform/fastdiv.h"
#include "../../include/topos_codec.h"

#include <string.h>

int32_t tc_rans_model_build(tc_rans_model* m, const uint32_t* count, uint32_t syms)
{
    if (m == NULL || count == NULL) { return TC_ERR_INVALID_ARGUMENT; }
    if (syms == 0u || syms > TC_RANS_MAX_SYMS) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "rans model syms %u out of 1..64",
                     (unsigned)syms);
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(m, 0, sizeof(*m));   /* 尾部未用槽清零：结构体级确定性比较 */
    uint64_t total = 0u;
    for (uint32_t s = 0u; s < syms; ++s) {
        if (count[s] > UINT32_MAX / 4u) {  /* 计数域防御（u8 capped 场景恒过） */
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "rans count[%u] too large", (unsigned)s);
            return TC_ERR_INVALID_ARGUMENT;
        }
        total += count[s];
    }
    if (total == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "rans model all-zero counts");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* 确定性归一化（不变量优先）：
     *  1) count>0 → freq ≥ 1 保底（floor(count·T/total) 上取 1）——
     *     饿死符号 = 不可编码，先钉死；
     *  2) Σ > T：从最大 freq（同值取最小序号）逐个 −1（不破 1 下限）；
     *  3) Σ < T：给余数最大者 +1（同值取最小序号）。
     * 全程 O(T) 上界步数、无浮点、同输入同输出。 */
    uint32_t freq[TC_RANS_MAX_SYMS];
    uint32_t rem[TC_RANS_MAX_SYMS];
    uint32_t sum = 0u;
    for (uint32_t s = 0u; s < syms; ++s) {
        const uint64_t scaled = (uint64_t)count[s] * TC_RANS_SCALE_TOTAL;
        freq[s] = (count[s] == 0u) ? 0u : (uint32_t)(scaled / total);
        rem[s] = (uint32_t)(scaled % total);
        if (count[s] != 0u && freq[s] == 0u) { freq[s] = 1u; }
        sum += freq[s];
    }
    while (sum > TC_RANS_SCALE_TOTAL) {
        uint32_t big = UINT32_MAX;
        for (uint32_t s = 0u; s < syms; ++s) {
            if (freq[s] > 1u && (big == UINT32_MAX || freq[s] > freq[big])) {
                big = s;
            }
        }
        if (big == UINT32_MAX) { break; }
        freq[big] -= 1u;
        rem[big] = 0u;
        sum -= 1u;
    }
    while (sum < TC_RANS_SCALE_TOTAL) {
        uint32_t best = UINT32_MAX;
        for (uint32_t s = 0u; s < syms; ++s) {
            if (count[s] == 0u) { continue; }
            if (best == UINT32_MAX || rem[s] > rem[best]) { best = s; }
        }
        if (best == UINT32_MAX) { break; }
        freq[best] += 1u;
        rem[best] = 0u;
        sum += 1u;
    }
    if (sum != TC_RANS_SCALE_TOTAL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "rans normalization sum %u != %u",
                     (unsigned)sum, (unsigned)TC_RANS_SCALE_TOTAL);
        return TC_ERR_INVALID_ARGUMENT;
    }
    m->syms = syms;
    uint32_t acc = 0u;
    for (uint32_t s = 0u; s < syms; ++s) {
        m->freq[s] = (uint16_t)freq[s];
        m->cum[s] = (uint16_t)acc;
        acc += freq[s];
    }
    m->cum[syms] = (uint16_t)acc;                         /* == SCALE_TOTAL */
    return TC_OK;
}

void tc_rans_model_uniform2(tc_rans_model* m)
{
    m->syms = 2u;
    m->freq[0] = TC_RANS_SCALE_TOTAL / 2u;
    m->freq[1] = TC_RANS_SCALE_TOTAL / 2u;
    m->cum[0] = 0u;
    m->cum[1] = m->freq[0];
    m->cum[2] = TC_RANS_SCALE_TOTAL;
}

void tc_rans_enc_init(tc_rans_enc* e, uint8_t* buf, size_t cap)
{
    e->x = TC_RANS_L;
    e->buf = buf;
    e->cap = cap;
    e->pos = 0u;
    e->overflow = 0;
}

int32_t tc_rans_put(tc_rans_enc* e, const tc_rans_model* m, uint32_t sym)
{
    if (m == NULL || sym >= m->syms) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "rans put sym %u >= %u",
                     (unsigned)sym, (unsigned)(m ? m->syms : 0u));
        return TC_ERR_INVALID_ARGUMENT;
    }
    const uint32_t f = m->freq[sym];
    if (f == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "rans put zero-freq sym %u", (unsigned)sym);
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* x_max = (L >> SCALE_BITS) << 8 × f = 2^19 × f ≤ 2^31（uint32 安全；
     * f = 4096 单符号退化时恒不重整化——零数据字节，语义正确）。 */
    const uint32_t x_max = ((TC_RANS_L >> TC_RANS_SCALE_BITS) << 8) * f;
    while (e->x >= x_max) {
        if (e->pos >= e->cap) {
            e->overflow = 1;
            tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "rans encode overflow (%zu/%zu)",
                         e->pos, e->cap);
            return TC_ERR_BUFFER_TOO_SMALL;
        }
        e->buf[e->pos++] = (uint8_t)(e->x & 0xFFu);
        e->x >>= 8;
    }
    e->x = ((e->x / f) << TC_RANS_SCALE_BITS) + (e->x % f) + m->cum[sym];
    return TC_OK;
}

int32_t tc_rans_put_fast(tc_rans_enc* e, const tc_rans_model* m,
                         const tc_fastdiv* fd, uint32_t sym)
{
    /* 与 tc_rans_put 同域同码（差分钉死）；唯 x/f 与 x%f 改经 magic
     * ——q 精确 ⇔ 余数精确 ⇔ 状态转移逐位一致（域证明见 rans.h）。 */
    if (m == NULL || sym >= m->syms) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "rans put sym %u >= %u",
                     (unsigned)sym, (unsigned)(m ? m->syms : 0u));
        return TC_ERR_INVALID_ARGUMENT;
    }
    const uint32_t f = m->freq[sym];
    if (f == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "rans put zero-freq sym %u", (unsigned)sym);
        return TC_ERR_INVALID_ARGUMENT;
    }
    const uint32_t x_max = ((TC_RANS_L >> TC_RANS_SCALE_BITS) << 8) * f;
    while (e->x >= x_max) {
        if (e->pos >= e->cap) {
            e->overflow = 1;
            tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "rans encode overflow (%zu/%zu)",
                         e->pos, e->cap);
            return TC_ERR_BUFFER_TOO_SMALL;
        }
        e->buf[e->pos++] = (uint8_t)(e->x & 0xFFu);
        e->x >>= 8;
    }
    const uint32_t q = tc_fastdiv_apply(e->x, fd);
    e->x = (q << TC_RANS_SCALE_BITS) + (e->x - q * f) + m->cum[sym];
    return TC_OK;
}

int32_t tc_rans_enc_flush(tc_rans_enc* e)
{
    if (e->overflow != 0) {
        tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "rans encode overflow (%zu/%zu)",
                     e->pos, e->cap);
        return TC_ERR_BUFFER_TOO_SMALL;
    }
    const size_t need = e->pos + TC_RANS_STATE_BYTES;
    if (need > e->cap) {
        tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "rans flush %zu > %zu", need, e->cap);
        return TC_ERR_BUFFER_TOO_SMALL;
    }
    /* 布局：[终态 4B 大端][数据正序]。编码期字节按发射序写在 [0,pos)
     * （= 消费序的倒序）。三步原地变换（无重叠风险）：
     *  1) 反转 [0,pos) → 数据变为消费正序；
     *  2) 终态写 [pos,pos+4)；
     *  3) [0,pos+4) 右旋 4 字节（整体反转 + 两段分别反转）→ 终态在前。 */
    {
        size_t i = 0u;
        size_t j = e->pos;
        while (i + 1u < j) {
            const uint8_t t = e->buf[i];
            e->buf[i] = e->buf[j - 1u];
            e->buf[j - 1u] = t;
            ++i;
            --j;
        }
    }
    e->buf[e->pos] = (uint8_t)(e->x >> 24);
    e->buf[e->pos + 1u] = (uint8_t)(e->x >> 16);
    e->buf[e->pos + 2u] = (uint8_t)(e->x >> 8);
    e->buf[e->pos + 3u] = (uint8_t)(e->x);
    {
        const size_t n = need;
        size_t i = 0u;
        size_t j = n;
        while (i + 1u < j) {
            const uint8_t t = e->buf[i];
            e->buf[i] = e->buf[j - 1u];
            e->buf[j - 1u] = t;
            ++i;
            --j;
        }
        i = 0u;
        j = TC_RANS_STATE_BYTES;
        while (i + 1u < j) {
            const uint8_t t = e->buf[i];
            e->buf[i] = e->buf[j - 1u];
            e->buf[j - 1u] = t;
            ++i;
            --j;
        }
        i = TC_RANS_STATE_BYTES;
        j = n;
        while (i + 1u < j) {
            const uint8_t t = e->buf[i];
            e->buf[i] = e->buf[j - 1u];
            e->buf[j - 1u] = t;
            ++i;
            --j;
        }
    }
    return (int32_t)need;
}

int32_t tc_rans_dec_init(tc_rans_dec* d, const uint8_t* buf, size_t size)
{
    if (d == NULL || buf == NULL) { return TC_ERR_INVALID_ARGUMENT; }
    if (size < TC_RANS_STATE_BYTES) {
        tc_set_error(TC_ERR_TRUNCATED, "rans stream %zu < 4", size);
        return TC_ERR_TRUNCATED;
    }
    d->x = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
         | ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
    d->buf = buf;
    d->size = size;
    d->pos = TC_RANS_STATE_BYTES;
    if (d->x < TC_RANS_L) {
        tc_set_error(TC_ERR_MALFORMED, "rans initial state %u < L", (unsigned)d->x);
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

int32_t tc_rans_get(tc_rans_dec* d, const tc_rans_model* m, uint32_t* sym_out)
{
    if (m == NULL || sym_out == NULL) { return TC_ERR_INVALID_ARGUMENT; }
    const uint32_t slot = d->x & (TC_RANS_SCALE_TOTAL - 1u);
    /* CDF 定位：cum[s] ≤ slot < cum[s]+freq[s]。线性扫（≤64；热路径后续
     * 可 LUT 化，核心先钉正确性）。freq 0 符号天然不可命中。 */
    uint32_t s = UINT32_MAX;
    for (uint32_t i = 0u; i < m->syms; ++i) {
        if (slot >= m->cum[i] && slot < m->cum[i] + m->freq[i]) {
            s = i;
            break;
        }
    }
    if (s == UINT32_MAX) {
        /* 模型空洞（构造保证不出现）或状态漂移 → 截断/畸形 */
        tc_set_error(TC_ERR_TRUNCATED, "rans cdf hole at slot %u", (unsigned)slot);
        return TC_ERR_TRUNCATED;
    }
    const uint32_t f = m->freq[s];
    d->x = f * (d->x >> TC_RANS_SCALE_BITS) + (d->x & (TC_RANS_SCALE_TOTAL - 1u))
         - m->cum[s];
    while (d->x < TC_RANS_L) {
        if (d->pos >= d->size) {
            tc_set_error(TC_ERR_TRUNCATED, "rans stream exhausted");
            return TC_ERR_TRUNCATED;
        }
        d->x = (d->x << 8) | d->buf[d->pos++];
    }
    *sym_out = s;
    return TC_OK;
}

/* ---- V2-R slice 表（语义见 rans.h "表字节语义" 注释）---- */

static void family_table_bytes(uint8_t* out, uint32_t nsym,
                               const uint32_t* count)
{
    uint64_t total = 0u;
    for (uint32_t s = 0u; s < nsym; ++s) { total += count[s]; }
    if (total == 0u) {
        /* 合法空族（如极低码率 slice 无 AC 对的 LEVEL_CAT）：占位单符号，
         * 保证表可建模型；该族无符号被编码/解码，占位值无语义 */
        memset(out, 0, nsym);
        out[0] = 1u;
        return;
    }
    for (uint32_t s = 0u; s < nsym; ++s) {
        if (count[s] == 0u) {
            out[s] = 0u;
            continue;
        }
        /* round-half-up（负数无涉）；used 符号下限 1（freq≥1 编码契约） */
        uint64_t v = ((uint64_t)count[s] * 255u + total / 2u) / total;
        if (v == 0u) { v = 1u; }
        if (v > 255u) { v = 255u; }
        out[s] = (uint8_t)v;
    }
}

void tc_rans_table_encode(uint8_t out[TC_RANS_TABLE_BYTES],
                          const uint32_t dc_count[TC_RANS_DC_SYMS],
                          const uint32_t run_count[TC_RANS_RUN_SYMS],
                          const uint32_t lvl_count[TC_RANS_LVL_SYMS])
{
    family_table_bytes(out, TC_RANS_DC_SYMS, dc_count);
    family_table_bytes(out + TC_RANS_DC_SYMS, TC_RANS_RUN_SYMS, run_count);
    family_table_bytes(out + TC_RANS_DC_SYMS + TC_RANS_RUN_SYMS,
                       TC_RANS_LVL_SYMS, lvl_count);
}

int32_t tc_rans_table_decode(const uint8_t in[TC_RANS_TABLE_BYTES],
                             tc_rans_model* dc, tc_rans_model* run,
                             tc_rans_model* lvl)
{
    uint32_t count[TC_RANS_RUN_SYMS];  /* 三族最大 64，逐族复用 */

    for (uint32_t s = 0u; s < TC_RANS_DC_SYMS; ++s) { count[s] = in[s]; }
    int32_t rc = tc_rans_model_build(dc, count, TC_RANS_DC_SYMS);
    if (rc != TC_OK) { return rc; }
    const uint8_t* run_in = in + TC_RANS_DC_SYMS;
    for (uint32_t s = 0u; s < TC_RANS_RUN_SYMS; ++s) { count[s] = run_in[s]; }
    rc = tc_rans_model_build(run, count, TC_RANS_RUN_SYMS);
    if (rc != TC_OK) { return rc; }
    const uint8_t* lvl_in = in + TC_RANS_DC_SYMS + TC_RANS_RUN_SYMS;
    for (uint32_t s = 0u; s < TC_RANS_LVL_SYMS; ++s) { count[s] = lvl_in[s]; }
    return tc_rans_model_build(lvl, count, TC_RANS_LVL_SYMS);
}

void tc_rans_lut_build(tc_rans_lut* l, const tc_rans_model* m)
{
    if (l == NULL || m == NULL) { return; }
    for (uint32_t s = 0u; s < m->syms; ++s) {
        const uint32_t lo = m->cum[s];
        const uint32_t hi = lo + m->freq[s];
        memset(l->sym + lo, (int)s, hi - lo);
    }
}

/* ---- V7-R2（ADR-C036）：条件行表（语义见 rans.h）----
 * family_table_bytes 的导出版本（行内比例量化，全零行占位）+ bytes→model。 */
void tc_rans2_row_encode(uint8_t* out, uint32_t nsym, const uint32_t* count)
{
    family_table_bytes(out, nsym, count);
}

int32_t tc_rans2_row_decode(const uint8_t* in, uint32_t nsym, tc_rans_model* m)
{
    if (in == NULL || m == NULL || nsym == 0u || nsym > TC_RANS_MAX_SYMS) {
        return TC_ERR_INVALID_ARGUMENT;
    }
    uint32_t count[TC_RANS_MAX_SYMS];
    for (uint32_t s = 0u; s < nsym; ++s) { count[s] = in[s]; }
    return tc_rans_model_build(m, count, nsym);
}
