/* checked 位读写 —— 熵编码的唯一位流载体（spec §2.2/§6.2）。
 *
 * 读端：越界读即置粘滞 TC_ERR_TRUNCATED，此后一切读操作直接返回该错误，
 * 不推进 bit_pos（「任意截断位置均不越界」的门禁在结构上成立，而非靠调用方小心）。
 * 外部（如 Rice 域校验）可经 tc_bitreader_fail 注入 TC_ERR_MALFORMED。
 *
 * 写端：可增长缓冲，上限 TC_BITWRITER_MAX_BYTES（spec §10 包上限 256 MiB）；
 * 超限/分配失败为粘滞错误（LIMIT_EXCEEDED / OUT_OF_MEMORY），绝不静默丢弃。
 */
#ifndef TOPOS_INTERNAL_BITIO_H
#define TOPOS_INTERNAL_BITIO_H

#include <stddef.h>
#include <stdint.h>

#include "topos_codec.h"

#include "../common/endian.h"

#define TC_BITWRITER_MAX_BYTES (268435456u) /* == TC_MAX_PACKET_SIZE（spec §10） */

typedef struct tc_bitreader {
    const uint8_t* data;
    uint64_t bit_size; /* 可读总位 = payload 字节数 × 8 */
    uint64_t bit_pos;  /* 已消费位（绝对位偏移；与 cache 推进同步） */
    int32_t error;     /* 粘滞：0 或首个错误码 */
    /* M5 reservoir：MSB 对齐位窗口——替代每符号重拼 8 字节窗口。
     * 不变量：bits ∈ [0,64]；cache 高 bits 位有效；next − data 为已装载数；
     * bit_pos == (next − data)×8 − bits（消费计数，与装载解耦）。 */
    uint64_t cache;
    uint32_t bits;
    const uint8_t* next;
} tc_bitreader;

typedef struct tc_bitwriter {
    uint8_t* buf;
    size_t cap;          /* buf 容量（字节） */
    size_t bytes;        /* 已提交进 buf 的完整字节数 */
    /* M6 128-bit reservoir：hi:lo 为移位追加寄存器，有效位 = 低 hold_bits 位
     * （最老的有效位在最高处；MSB-first 位序由此保持）。不变量：每次 put 返回
     * 时 hold_bits ≤ 63 且 hi == 0（≥64 即提交一个 64-bit 大端字，至多一次）；
     * flush_zero_pad 后 < 8。单次 append（≤32 位）在寄存器内绝不丢失位，
     * escape 双段（31+32）也只触发一次提交。 */
    uint64_t hold_hi;
    uint64_t hold_lo;
    uint32_t hold_bits;
    int32_t error;       /* 粘滞：0 或首个错误码 */
} tc_bitwriter;

/* ---- 读端 ---- */

void tc_bitreader_init(tc_bitreader* br, const void* data, size_t byte_size);

/* 读 1 位到 *out_bit（0/1）。失败返回 br->error（粘滞），不推进。 */
int32_t tc_bitreader_read_bit(tc_bitreader* br, uint32_t* out_bit);

/* 读 nbits（0..32）位，MSB-first 拼装到 *out。nbits==0 → *out=0、TC_OK。
 * 越界（含部分越界）→ TC_ERR_TRUNCATED，不推进。 */
int32_t tc_bitreader_read_bits(tc_bitreader* br, uint32_t nbits, uint32_t* out);

uint64_t tc_bitreader_bits_consumed(const tc_bitreader* br);

/* 外部注入粘滞错误（仅当当前无错误时生效）；用于符号域校验失败 */
void tc_bitreader_fail(tc_bitreader* br, int32_t code);

/* 跳过至下一字节边界（仅丢弃 <8 个填充位；已处边界为 no-op）。不产生 TRUNCATED。 */
int32_t tc_bitreader_align_byte(tc_bitreader* br);

/* ---- 写端 ---- */

/* 初始容量 64 KiB。成功 TC_OK；分配失败 TC_ERR_OUT_OF_MEMORY（粘滞）。 */
int32_t tc_bitwriter_init(tc_bitwriter* bw);

/* 释放缓冲并把实例归零；可对 init 失败的实例安全调用 */
void tc_bitwriter_free(tc_bitwriter* bw);

/* 复位（保留已分配缓冲，字节内容按需清零），错误清零 */
void tc_bitwriter_reset(tc_bitwriter* bw);

int32_t tc_bitwriter_put_bit(tc_bitwriter* bw, uint32_t bit /*0/1*/);

/* 写 value 的低 nbits（0..64）位，MSB-first；高于 nbits 的位被掩除。
 * M10-6.3C：熵符号融合发射（run+level 单写）的 64 位通路。 */
int32_t tc_bitwriter_put_bits64(tc_bitwriter* bw, uint32_t nbits, uint64_t value);

/* 写 value 的低 nbits（0..32）位，MSB-first；value 高于 nbits 的位被掩除 */
int32_t tc_bitwriter_put_bits(tc_bitwriter* bw, uint32_t nbits, uint32_t value);

/* 用 0 位填充到字节边界（spec §2.3）。已对齐为 no-op。 */
int32_t tc_bitwriter_flush_zero_pad(tc_bitwriter* bw);

/* C5：批量字节追加。hold_bits == 0（字节对齐且 reservoir 空）时 memcpy
 * 直提交——与 N 次 put_bits(8) 在 flush 后输出逐位一致（提交路径不同、
 * 最终 buf/byte_size 等同；reservoir 不变量不受扰）。hold_bits != 0 回落
 * 逐字节 put（任意位对齐下位精确；调用方字节边界场景不会走到）。 */
int32_t tc_bitwriter_put_bytes(tc_bitwriter* bw, const void* src, size_t n);

const uint8_t* tc_bitwriter_data(const tc_bitwriter* bw); /* 失败/未写为 NULL 安全 */
size_t tc_bitwriter_byte_size(const tc_bitwriter* bw);    /* 语义要求先 flush */
uint64_t tc_bitwriter_bits_written(const tc_bitwriter* bw);

/* ---- 内联快速路径（阶段 9：熵热路径消除跨 TU 调用开销；语义与上述函数一致） ----
 * 前置：nbits ∈ [1,32]（调用方构造保证）。需要增长缓冲时整体委托外部函数。 */

static inline int32_t tc_bitwriter_put_bits_inline(tc_bitwriter* bw, uint32_t nbits,
                                                   uint32_t value)
{
    if (bw->error != 0) { return bw->error; }
    /* 快路准入：要么本次 append 不触发提交（hold_bits+nbits < 64），要么为
     * 至多一次的 8 字节提交预留好容量；否则委托外部函数走增长路径。
     * 提交后 hold_bits ≤ 31 < 64 → 后续 append 溢出位先暂存 hi 再随提交消费。 */
    if (bw->hold_bits + nbits < 64u || bw->bytes + 8u <= bw->cap) {
        uint64_t v = (uint64_t)(value & ((nbits == 32u)
                          ? 0xFFFFFFFFu : ((1u << nbits) - 1u)));
        bw->hold_hi = (bw->hold_hi << nbits) | (bw->hold_lo >> (64u - nbits));
        bw->hold_lo = (bw->hold_lo << nbits) | v;
        bw->hold_bits += nbits;
        if (bw->hold_bits >= 64u) {
            uint32_t s = bw->hold_bits - 64u; /* 提交后剩余（≤31） */
            uint64_t word = s == 0u ? bw->hold_lo
                                    : ((bw->hold_hi << (64u - s)) | (bw->hold_lo >> s));
            tc_store_be64(bw->buf + bw->bytes, word);
            bw->bytes += 8u;
            bw->hold_hi = 0u;
            bw->hold_lo = s == 0u ? 0u : (bw->hold_lo & (((uint64_t)1 << s) - 1u));
            bw->hold_bits = s;
        }
        return TC_OK;
    }
    return tc_bitwriter_put_bits(bw, nbits, value); /* 外部函数：ensure+增长+写入 */
}

/* ---- M10-6.3C：64 位内联快速路径（熵符号/符号对融合发射） ----
 * 前置：nbits ∈ [1,64]；128 位蓄水池不变量同上（单次 append ≤64 位至多
 * 触发一次 8 字节提交，提交后 hold_bits ≤ 63 且 hi == 0）。 */

static inline int32_t tc_bitwriter_put_bits64_inline(tc_bitwriter* bw, uint32_t nbits,
                                                     uint64_t value)
{
    if (bw->error != 0) { return bw->error; }
    /* 快路准入与 32 位版同构：要么本次 append 后仍 < 64 位（绝不提交），
     * 要么为至多一次的 8 字节提交预留好容量；否则委托外部函数增长。
     * （hold+nbits < 128 只保证"至多一次提交"，不保证免容量检查——
     *  sum ∈ [64,128) 时仍会提交，实测越界写钉死该教训。） */
    if (bw->hold_bits + nbits < 64u || bw->bytes + 8u <= bw->cap) {
        if (nbits == 64u) {
            bw->hold_hi = bw->hold_lo; /* 整体左移 64：旧 hi 已空（不变量） */
            bw->hold_lo = value;
        } else {
            const uint64_t v =
                value & (((uint64_t)1 << nbits) - 1u); /* 掩除高位（nbits<64） */
            bw->hold_hi = (bw->hold_hi << nbits) | (bw->hold_lo >> (64u - nbits));
            bw->hold_lo = (bw->hold_lo << nbits) | v;
        }
        bw->hold_bits += nbits;
        if (bw->hold_bits >= 64u) {
            uint32_t s = bw->hold_bits - 64u; /* 提交后剩余（≤63） */
            uint64_t word = s == 0u ? bw->hold_lo
                                    : ((bw->hold_hi << (64u - s)) | (bw->hold_lo >> s));
            tc_store_be64(bw->buf + bw->bytes, word);
            bw->bytes += 8u;
            bw->hold_hi = 0u;
            bw->hold_lo = s == 0u ? 0u : (bw->hold_lo & (((uint64_t)1 << s) - 1u));
            bw->hold_bits = s;
        }
        return TC_OK;
    }
    return tc_bitwriter_put_bits64(bw, nbits, value);
}

/* ---- M5 reservoir 内联原语（熵热路径；语义与外部函数一致） ----
 * 前置：n ∈ [1,64]；skip/peek 前调用方须 refill 且校验 bits ≥ n。 */

static inline void tc_bitreader_refill(tc_bitreader* br)
{
    /* data==NULL（空 payload）时 end 保持 NULL：避免对空指针加 0 的 UB
     * （语义不变——循环体本就不会执行） */
    const uint8_t* end = br->data != NULL
        ? br->data + (size_t)(br->bit_size >> 3)
        : (const uint8_t*)NULL;
    if (br->bits <= 56u) {
        /* 字级快路（解码深化批次）：剩余 ≥8 字节时一次装载 n=(64−bits)/8 字节。
     * chunk 先掩掉低 n 字节之外的部分 → 有效位以下保持全 0（|=/or 的前置），
     * 掩码移位量 64−8n ∈ [0,56]（n=8 时为 0，合法）——不变量与逐字节版一致：
     * bits+8n ≤ 64，next 只前移 n，绝不越界。 */
        if (end != NULL && (size_t)(end - br->next) >= 8u) {
            uint32_t n = (64u - br->bits) >> 3;
            uint64_t chunk = tc_load_be64(br->next) & (~UINT64_C(0) << (64u - 8u * n));
            br->cache |= chunk >> br->bits;
            br->bits += 8u * n;
            br->next += n;
            return;
        }
        /* 流尾（<8 字节可读）：逐字节推进，绝不越界。
         * 有效位 MSB 对齐占据 [63..64−bits]，新字节必须紧贴其下：
         * cache |= byte << (56−bits)（bits ≤ 56 由循环条件保证）。 */
        while (br->bits <= 56u && br->next < end) {
            br->cache |= (uint64_t)*br->next++ << (56u - br->bits);
            br->bits += 8u;
        }
    }
}

static inline void tc_bitreader_skip(tc_bitreader* br, uint32_t n)
{
    if (n == 0u) { return; }
    br->cache = (n == 64u) ? UINT64_C(0) : (br->cache << n); /* 移 64 为 UB，单独清零 */
    br->bits -= n;
    br->bit_pos += n;
}

static inline int32_t tc_bitreader_read_bits_inline(tc_bitreader* br, uint32_t nbits,
                                                    uint32_t* out)
{
    if (br->error != 0) { return br->error; }
    if (nbits == 0u) { *out = 0u; return TC_OK; }
    tc_bitreader_refill(br);
    if (br->bits < nbits) {
        br->error = TC_ERR_TRUNCATED;
        return TC_ERR_TRUNCATED;
    }
    *out = (uint32_t)(br->cache >> (64u - nbits));
    tc_bitreader_skip(br, nbits);
    return TC_OK;
}

#endif /* TOPOS_INTERNAL_BITIO_H */
