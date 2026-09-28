/* 32-bit rANS（range Asymmetric Numeral Systems）——V2-R 实验熵核原语。
 *
 * 设计（ADR-C034）：per-slice 传输的精确计数模型 + 单状态后向编码/
 * 前向解码。符号语义与 V2 canonical VLC 完全同源（DC_CAT/RUN/LEVEL_CAT
 * + 原始位），仅把"整数码长的规范码字"换成"精确频率的 rANS 码"——
 * 收益 = 码长取整损耗 + 书拟合损耗，真实素材上限实测 8.0%（qp24）/
 * 4.6%（qp48），suffix 位（cat−1 幅度位 + 符号位）经均匀二值模型
 * 逐位恰 1 bit，语义与裸写逐位一致。
 *
 * 数值域（test_rans 差分钉死；ryg rANS byte 变体参数）：
 *  - 状态 x ∈ [TC_RANS_L, 2^32)，L = 2^23，逐字节重整化；
 *  - 概率精度 SCALE_BITS=12（CDF 总 = 4096）：x_max = (L>>12)<<8 ×freq
 *    = 2^19×freq ≤ 2^31（freq ≤ 4096）——32 位无溢出；
 *  - 不变量：x ≥ L 且 f ≤ T ⇒ 编码后 x' = (x/f)·T + r + cum ≥
 *    (L/T)·T = L——状态永不跌破 L（解码侧对称，杜绝失步）；
 *  - 全整数、无浮点、无未定义移位；模型构建确定性（同计数 → 同 CDF）。
 */
#ifndef TOPOS_INTERNAL_RANS_H
#define TOPOS_INTERNAL_RANS_H

#include "../../include/topos_codec.h"

#include "../transform/fastdiv.h"

#include <stddef.h>
#include <stdint.h>

#define TC_RANS_L (1u << 23)      /* 重整化下界（ryg byte 变体） */
#define TC_RANS_SCALE_BITS 12
#define TC_RANS_SCALE_TOTAL (1u << TC_RANS_SCALE_BITS)  /* 4096 */
#define TC_RANS_MAX_SYMS 64      /* 最大字母表（RUN 族） */
#define TC_RANS_STATE_BYTES 4u   /* 流尾终态 */

/* ---- 模型：计数 → 确定性归一化 CDF ---- */

typedef struct tc_rans_model {
    uint32_t syms;               /* 字母表大小（1..64） */
    uint16_t freq[TC_RANS_MAX_SYMS];
    uint16_t cum[TC_RANS_MAX_SYMS + 1];
} tc_rans_model;

/* 从原始计数构建模型（确定性 largest-remainder 归一化到 4096）。
 * count[s] = 0 → freq 0（该符号不可编码；解码命中 = MALFORMED 调用方拒）。
 * 全零/超界返回 TC_ERR_INVALID_ARGUMENT。syms ∈ [1,64]，单符号计数和
 * 上限 2^32-1 由调用域保证（u8 计数 capped 场景恒满足）。 */
int32_t tc_rans_model_build(tc_rans_model* m, const uint32_t* count, uint32_t syms);

/* 均匀二值模型（原始位：freq 各 2048，逐位恰 1 bit）。 */
void tc_rans_model_uniform2(tc_rans_model* m);

/* ---- 编码（后向；符号逆序喂入） ---- */

typedef struct tc_rans_enc {
    uint32_t x;                  /* 状态，init = TC_RANS_L */
    uint8_t* buf;                /* 输出缓冲（倒序写；见 flush 布局） */
    size_t cap;                  /* 缓冲容量（字节） */
    size_t pos;                  /* 已写字节数（编码期递增） */
    int overflow;
} tc_rans_enc;

void tc_rans_enc_init(tc_rans_enc* e, uint8_t* buf, size_t cap);
/* 单符号编码（模型可逐符号切换——三族 + RAW 位共用一个状态流）。
 * 域外符号（≥syms）或 freq 0：返回 TC_ERR_INVALID_ARGUMENT。溢出置
 * e->overflow（flush 时报 TC_ERR_BUFFER_TOO_SMALL），符号仍被吸收以
 * 保持调用方可统一收尾。 */
int32_t tc_rans_put(tc_rans_enc* e, const tc_rans_model* m, uint32_t sym);
/* S4（速度计划 P1）：免 idiv 的 put——与 tc_rans_put 逐位等价（同域
 * 校验/同重整化/同状态转移），x/f 与 x%f 改经预计算 round-up magic
 * （transform/fastdiv.h）。域证明：出重整化循环时 x < x_max = 2^19·f
 * ≤ 2^31、f ≤ 2^12 → 误差项上界 x·e < 2^19·f² ≤ 2^43 ≪ 2^51，
 * fastdiv 精确性条件（n·e < 2^51）恒成立——量化调用方（n ≤ 2^25）
 * 之外的域扩展，fastdiv.h 同式推导。fd 须与 m 同源构建
 * （fd[sym] 的除数 = m->freq[sym]）。 */
int32_t tc_rans_put_fast(tc_rans_enc* e, const tc_rans_model* m,
                         const tc_fastdiv* fd, uint32_t sym);
/* 收尾：写出终态。流布局 = [终态 4B（大端）][数据字节（正序）]——
 * 编码期字节按倒序产生，flush 反转拷贝到输出区。返回总字节数。 */
int32_t tc_rans_enc_flush(tc_rans_enc* e);

/* ---- 解码（前向） ---- */

typedef struct tc_rans_dec {
    uint32_t x;
    const uint8_t* buf;
    size_t size;                 /* flush 输出总长 */
    size_t pos;                  /* 已消费数据字节（终态之后） */
} tc_rans_dec;

/* 从 flush 布局初始化（校验最小长度）。 */
int32_t tc_rans_dec_init(tc_rans_dec* d, const uint8_t* buf, size_t size);
/* 单符号解码；流尽时域内确定性收尾（消费字节后状态恒 ≥L——
 * 截断流在余下符号解码中表现为 x 停在 L 不动、查表越界 →
 * 返回 TC_ERR_TRUNCATED）。 */
int32_t tc_rans_get(tc_rans_dec* d, const tc_rans_model* m, uint32_t* sym_out);

/* ---- 热路径特化（与通用运算逐字节等价；位流不变）----
 * LUT 解码 = 通用 get 的 slot→sym 线性扫替身（Σfreq=4096 无空洞，
 * 构建即逐段填充）。语义/字节与通用版严格一致（test_rans 差分钉死）。 */

/* 原始位旁路（ryg 变体）：b 位值 v 不经频率模型，直接折入状态流
 * （编码 x = (x<<b)|v，解码 v = x&mask 后右移 + 补位）。恰 b bit 成本。
 *
 * 逐块不变量（w ≤ 23 的块，L=2^23）：解码 refill 按 x0>>(8·(k−i)) ≥ L
 * 的首个 i 停止——与编码发射数 k 相等当且仅当运算入口状态
 * x0 ∈ [L, 2^31)。符号 put 输出域恒在该区间（⌊x_r/f⌋ ≤ 2^19−1 且
 * r+cum ≤ 4095），故块运算只须保证自身输出同域：
 *  - 上界：重整化阈值 2^(31−w) ⟹ 输出 < 2^31；
 *  - 下界：发射 ⟹ x_r ≥ 2^(23−w) ⟹ 输出 ≥ L（w = 23 时阈值 2^8，
 *    x ≥ 256 ⟹ x>>8 ≥ 1，同成立）；零发射 ⟹ 输出 ≥ x0·2 ≥ 2L。
 * b > 23 时单块无法同时满足（阈值 < 2^8 会把状态清零）——拆为
 * [高 b−16 位块][低 16 位块]两次发射（解码反向：先读低块再读高块，
 * (hi<<16)|lo 重组）。b ≤ 27（cat ≤ 28 域）⟹ 高块 ≤ 11 位。 */
static inline int32_t tc_rans_put_chunk(tc_rans_enc* e, uint32_t w, uint32_t c)
{
    const uint32_t thr = 1u << (31u - w);
    while (e->x >= thr) {
        if (e->pos >= e->cap) { e->overflow = 1; return TC_ERR_BUFFER_TOO_SMALL; }
        e->buf[e->pos++] = (uint8_t)(e->x & 0xFFu);
        e->x >>= 8;
    }
    e->x = (e->x << w) | (c & ((1u << w) - 1u));
    return TC_OK;
}

static inline int32_t tc_rans_get_chunk(tc_rans_dec* d, uint32_t w, uint32_t* c_out)
{
    const uint32_t mask = (1u << w) - 1u;
    const uint32_t c = d->x & mask;
    d->x >>= w;
    while (d->x < TC_RANS_L) {
        if (d->pos >= d->size) { return TC_ERR_TRUNCATED; }
        d->x = (d->x << 8) | d->buf[d->pos++];
    }
    *c_out = c;
    return TC_OK;
}

static inline int32_t tc_rans_put_rawbits(tc_rans_enc* e, uint32_t b, uint32_t v)
{
    if (b == 0u || b > 27u) { return TC_OK; }  /* b ≤ 27：cat ≤ 28 域 */
    if (b > 23u) {
        int32_t rc = tc_rans_put_chunk(e, b - 16u, v >> 16u);
        if (rc != TC_OK) { return rc; }
        b = 16u;
    }
    return tc_rans_put_chunk(e, b, v);
}

static inline int32_t tc_rans_get_rawbits(tc_rans_dec* d, uint32_t b, uint32_t* v_out)
{
    uint32_t v = 0u;
    if (b > 23u) {
        uint32_t lo = 0u;
        int32_t rc = tc_rans_get_chunk(d, 16u, &lo);
        if (rc != TC_OK) { return rc; }
        uint32_t hi = 0u;
        rc = tc_rans_get_chunk(d, b - 16u, &hi);
        if (rc != TC_OK) { return rc; }
        v = (hi << 16u) | lo;
    } else if (b != 0u) {
        int32_t rc = tc_rans_get_chunk(d, b, &v);
        if (rc != TC_OK) { return rc; }
    }
    *v_out = v;
    return TC_OK;
}

typedef struct tc_rans_lut { uint8_t sym[TC_RANS_SCALE_TOTAL]; } tc_rans_lut;

void tc_rans_lut_build(tc_rans_lut* l, const tc_rans_model* m);

/* slot→sym 查 LUT（免线性扫）；状态更新用模型 freq/cum（同通用 get） */
static inline int32_t tc_rans_get_lut(tc_rans_dec* d, const tc_rans_lut* l,
                                      const tc_rans_model* m, uint32_t* sym_out)
{
    const uint32_t slot = d->x & (TC_RANS_SCALE_TOTAL - 1u);
    const uint32_t s = l->sym[slot];
    const uint32_t f = m->freq[s];
    d->x = f * (d->x >> TC_RANS_SCALE_BITS) + slot - m->cum[s];
    while (d->x < TC_RANS_L) {
        if (d->pos >= d->size) { return TC_ERR_TRUNCATED; }
        d->x = (d->x << 8) | d->buf[d->pos++];
    }
    *sym_out = s;
    return TC_OK;
}

/* ---- V2-R slice 表传输（ADR-C034；三族 = DC_CAT/RUN/LEVEL_CAT）----
 *
 * 表字节语义（实现期对 ADR 原文的修正，理由见 bitstream_spec_v7_rans §3）：
 * 逐符号比例量化 byte[s] = count[s]==0 ? 0 :
 *     clamp(1, 255, round(count[s]·255 / family_total))。
 * 不用 min(count,255) 裸截断——大 slice 中主导符号计数上千，裸截断会把
 * 其概率从 ~75% 压到 ~5%，编码端按同表建模型 → 体积爆炸；比例量化把每
 * 符号概率量化到 1/255 粒度（KL 损耗 <0.01 bit/符号），且 count>0 →
 * byte≥1 保住"可编码"不变量。两侧从同一 121B 表建模型（确定性归一化），
 * 绝对尺度无关——表只承载比例。 */
#define TC_RANS_DC_SYMS 29u    /* DC_CAT：bitlen(map(dc-pred)) ∈ 0..28 */
#define TC_RANS_RUN_SYMS 64u   /* RUN：0..62 + EOB=63 */
#define TC_RANS_LVL_SYMS 28u   /* LEVEL_CAT：bitlen(map(level)) ∈ 0..27 */
#define TC_RANS_TABLE_BYTES \
    (TC_RANS_DC_SYMS + TC_RANS_RUN_SYMS + TC_RANS_LVL_SYMS)  /* 121 */

/* 精确计数 → 121B 表（比例量化）。count 各族总和非零。 */
void tc_rans_table_encode(uint8_t out[TC_RANS_TABLE_BYTES],
                          const uint32_t dc_count[TC_RANS_DC_SYMS],
                          const uint32_t run_count[TC_RANS_RUN_SYMS],
                          const uint32_t lvl_count[TC_RANS_LVL_SYMS]);

/* 121B 表 → 三模型（与编码侧同源：tc_rans_model_build(byte 值)）。
 * 全零族构建失败返回 TC_ERR_INVALID_ARGUMENT（slice 解码路径统一映射
 * 为 MALFORMED——颜色 slice 每族至少有符号：DC/EOB 恒在）。 */
int32_t tc_rans_table_decode(const uint8_t in[TC_RANS_TABLE_BYTES],
                             tc_rans_model* dc, tc_rans_model* run,
                             tc_rans_model* lvl);

/* 类别后缀（cat−1 个幅度位，符号已折进 map 的 LSB）：一次 rawbits 旁路
 * （恰 sb bit；编码 LSB 侧值 = m 低 sb 位，解码 MSB→LSB 语义与 V2 VLC
 * 的字面后缀一致）。 */
static inline int32_t tc_rans_put_suffix(tc_rans_enc* e, uint32_t m, uint32_t cat)
{
    return tc_rans_put_rawbits(e, cat > 1u ? cat - 1u : 0u, m);
}

static inline int32_t tc_rans_get_suffix(tc_rans_dec* d, uint32_t cat, uint32_t* m_out)
{
    uint32_t v = 0u;
    if (cat > 1u) {
        int32_t rc = tc_rans_get_rawbits(d, cat - 1u, &v);
        if (rc != TC_OK) { return rc; }
    }
    *m_out = cat == 0u ? 0u : (1u << (cat - 1u)) | v;
    return TC_OK;
}

/* ---- V7-R2（ADR-C036）：order-1 上下文扩展（major=7，entropy_mode=7）----
 *
 * payload = [flags 1B][dc 表][run 表 64B][lvl 表][rANS 流]。
 * flags 位域：[1:0] lvl 模型（0=order-0，1=位置桶 L4，2=前 lvl 桶 L2；
 * 3 保留→MALFORMED）；[2] dc 模型（0=order-0，1=前块 DC 类桶 D1）；
 * [7:3] 保留恒 0（非零→MALFORMED）。
 * 条件表 = 每 ctx 一行，行内 8-bit 比例量化（全零行占位 {1,0,...}，
 * 该行无符号永不查询）；run 族恒 order-0（测量：≤1.5% 且高 qp 负收益）。
 * 上下文全部为解码序因果状态（解码端零成本；编码端 per-slice 按
 * 量化表精确代价 argmin 选择，结果入流信令）。
 * 依据：docs/codec/topos_ctx_ceiling_2026-09-10.md（lvl|位置 qp20-44
 * 净 +24~29% payload；高 qp 前 lvl / 前 DC 类接棒）。 */
#define TC_RANS2_LVL_NONE 0u
#define TC_RANS2_LVL_POS 1u   /* lvl | 本系数扫描位置桶，4 ctx */
#define TC_RANS2_LVL_PREV 2u  /* lvl | 前一对 lvl 类桶（含块首），5 ctx */
#define TC_RANS2_DC_NONE 0u
#define TC_RANS2_DC_PREV 1u   /* dc | 前块 DC 类桶（含首块），5 ctx */
#define TC_RANS2_POS_CTX 4u
#define TC_RANS2_PREV_CTX 5u
#define TC_RANS2_DC_CTX 5u
#define TC_RANS2_PREFIX_MAX                                                  \
    (1u + TC_RANS2_DC_CTX * TC_RANS_DC_SYMS + TC_RANS_RUN_SYMS +             \
     TC_RANS2_PREV_CTX * TC_RANS_LVL_SYMS)  /* 350：flags+全条件表上界 */

/* lvl|位置桶：{1-7, 8-15, 16-31, 32-63}（pos 为 zigzag 序 1..63） */
static inline uint32_t tc_rans2_pos_ctx(uint32_t pos)
{
    return pos < 8u ? 0u : pos < 16u ? 1u : pos < 32u ? 2u : 3u;
}

/* lvl|前 lvl 桶：{cat0, cat1, cat2-3, cat≥4, 块首}——has_prev=0（DC 后
 * 首对）为桶 4；cat = bitlen(map(level)) */
static inline uint32_t tc_rans2_prevlvl_ctx(uint32_t cat, int has_prev)
{
    if (has_prev == 0) { return 4u; }
    return cat == 0u ? 0u : cat == 1u ? 1u : cat <= 3u ? 2u : 3u;
}

/* dc|前块桶：{首块, cat0, cat1, cat2-3, cat≥4}——has_prev=0（slice 首块）
 * 为桶 0；cat = bitlen(map(dc-pred)) */
static inline uint32_t tc_rans2_dc_ctx(uint32_t cat, int has_prev)
{
    if (has_prev == 0) { return 0u; }
    return 1u + (cat == 0u ? 0u : cat == 1u ? 1u : cat <= 3u ? 2u : 3u);
}

/* 单行表编解码（行内 8-bit 比例量化；与 121B 表同语义，供条件行复用） */
void tc_rans2_row_encode(uint8_t* out, uint32_t nsym, const uint32_t* count);
int32_t tc_rans2_row_decode(const uint8_t* in, uint32_t nsym, tc_rans_model* m);

#endif /* TOPOS_INTERNAL_RANS_H */
