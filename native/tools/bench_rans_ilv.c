/* 交错 rANS 可行性原型（2026-09-11，速度轴；ADR-C041 候选）。
 *
 * 问题：生产 rans2 解码 = 单 rANS 状态串行链（slot 查 LUT → freq/cum
 * 依赖加载 → 乘法状态更新 → 逐字节 refill），符号循环占解码 45-69%。
 * 候选：L 条状态 lane 按逻辑符号序 round-robin——符号、模型、上下文
 * 推导、概率全同（压缩率不变），只把状态链的串行深度放宽 L 倍。
 *
 * 基元：直接链接生产 src/entropy/rans.c（tc_rans_put/get_lut/put_suffix/
 * get_suffix/lut_build/dec_init/enc_flush 逐字节同源——上批教训：原型
 * 不得复刻基元，微偏差会高估）。
 *
 * 路径：
 *  A   单状态（生产形态：get_lut + get_suffix + ctx 重推导 + blk 写 +
 *      rowmask/pos 记账，逐字节 refill）；
 *  B(L) L∈{2,4,8} lane：符号 k → lane k%L；每 lane 独立后向编码/
 *      flush（[终态 4B][数据]），流 = 各 lane flush 拼接；解码按逻辑序
 *      从对应 lane 取状态。ctx 推导不变（逻辑序因果）。
 *
 * 自检：A/B 解码出的 (family, ctx, sym, m) 与源逐项一致（ctx 由解码端
 * 重推导并与 dump 对照——推导漂移即失败）。计时 = 暖机 1 + N 轮取中位。
 */
#include "entropy/rans.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DC_SYMS 29u
#define RUN_SYMS 64u
#define LVL_SYMS 28u
#define DC_CTX 5u
#define LVL_CTX 5u
#define MAX_LANES 8

typedef struct {
    uint32_t fam : 8;   /* 0=dc 1=run 2=lvl */
    uint32_t ctx : 8;
    uint32_t sym : 16;
    uint32_t m;
} sym_rec;

typedef struct {
    uint32_t n;
    sym_rec* v;
    tc_rans_model dc_m[DC_CTX];
    tc_rans_model run_m;
    tc_rans_model lvl_m[LVL_CTX];
    tc_rans_lut dc_l[DC_CTX];
    tc_rans_lut run_l;
    tc_rans_lut lvl_l[LVL_CTX];
    uint8_t* buf[MAX_LANES];        /* A 用 [0]；B(L) 用 [0..L) */
    size_t size[MAX_LANES];
    sym_rec* dec_out;
} slice_bench;

static uint32_t prevlvl_ctx(uint32_t cat, int has_prev)
{
    if (!has_prev) { return 4u; }
    return cat == 0u ? 0u : cat == 1u ? 1u : cat <= 3u ? 2u : 3u;
}

static uint32_t dc_ctx_fn(uint32_t cat, int has_prev)
{
    if (!has_prev) { return 0u; }
    return 1u + (cat == 0u ? 0u : cat == 1u ? 1u : cat <= 3u ? 2u : 3u);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static int32_t enc_one(tc_rans_enc* e, const sym_rec* r, const slice_bench* s)
{
    /* 后向编码：后缀位先于符号（解码 get_lut→get_suffix 的逆序，
     * test_rans.c:219-225 同款约定） */
    switch (r->fam) {
    case 0u:
        if (tc_rans_put_suffix(e, r->m, r->sym) != TC_OK) { return -1; }
        return tc_rans_put(e, &s->dc_m[r->ctx], r->sym);
    case 1u:
        return tc_rans_put(e, &s->run_m, r->sym);
    default:
        if (tc_rans_put_suffix(e, r->m, r->sym) != TC_OK) { return -1; }
        return tc_rans_put(e, &s->lvl_m[r->ctx], r->sym);
    }
}

static int32_t enc_slice(const slice_bench* s, uint32_t lanes,
                         uint8_t* out_buf[MAX_LANES],
                         size_t out_size[MAX_LANES])
{
    tc_rans_enc enc[MAX_LANES];
    const size_t cap = (size_t)s->n * 8u + 64u;
    for (uint32_t l = 0u; l < lanes; ++l) {
        out_buf[l] = (uint8_t*)malloc(cap);
        if (out_buf[l] == NULL) { return -1; }
        tc_rans_enc_init(&enc[l], out_buf[l], cap);
    }
    for (uint32_t i = s->n; i-- > 0u;) {
        if (enc_one(&enc[i % lanes], &s->v[i], s) != TC_OK) { return -1; }
    }
    for (uint32_t l = 0u; l < lanes; ++l) {
        const int32_t rc = tc_rans_enc_flush(&enc[l]);
        if (rc < 0) { return -1; }   /* flush 成功返回总字节数（正值） */
        out_size[l] = (size_t)rc;
    }
    return 0;
}

/* 镜像生产符号循环：每块 DC(get_lut+suffix) → [run(get_lut) →
 * lvl(get_lut+suffix)]* → EOB(63)；交错版仅 dec 指针按符号序轮转。 */
static int32_t dec_slice(slice_bench* s, uint32_t lanes)
{
    tc_rans_dec dec[MAX_LANES];
    for (uint32_t l = 0u; l < lanes; ++l) {
        if (tc_rans_dec_init(&dec[l], s->buf[l], s->size[l]) != TC_OK) {
            return -1;
        }
    }
    sym_rec* out = s->dec_out;
    uint32_t out_n = 0u;
    uint32_t k = 0u;
    int32_t blk[64];
    int32_t dc_left = 0;
    uint32_t prev_dc_cat = 0u;
    int first_block = 1;
    uint64_t rowmask_acc = 0u;
    for (;;) {
        if (out_n >= s->n) { break; }
        {   /* 块头 DC */
            const uint32_t ctx = first_block ? 0u : dc_ctx_fn(prev_dc_cat, 1);
            tc_rans_dec* d = &dec[k % lanes]; ++k;
            uint32_t cat = 0u, m = 0u;
            if (tc_rans_get_lut(d, &s->dc_l[ctx], &s->dc_m[ctx], &cat) != TC_OK) { return -2; }
            if (tc_rans_get_suffix(d, cat, &m) != TC_OK) { return -2; }
            out[out_n].fam = 0u; out[out_n].ctx = ctx;
            out[out_n].sym = cat; out[out_n].m = m; ++out_n;
            blk[0] = (m & 1u) ? -(int32_t)(m >> 1) : (int32_t)(m >> 1);
            dc_left = (dc_left + blk[0]) >> 1;   /* 生产 dc 预测的算术量级替身 */
            prev_dc_cat = cat;
            first_block = 0;
        }
        uint32_t rowmask = 1u;
        uint32_t pos = 1u;
        uint32_t prev_cat = 0u;
        int has_prev = 0;
        for (;;) {
            tc_rans_dec* d = &dec[k % lanes]; ++k;
            uint32_t run_sym = 0u;
            if (tc_rans_get_lut(d, &s->run_l, &s->run_m, &run_sym) != TC_OK) { return -3; }
            out[out_n].fam = 1u; out[out_n].ctx = 0u;
            out[out_n].sym = run_sym; out[out_n].m = 0u; ++out_n;
            if (run_sym == 63u) { break; }
            const uint64_t idx = (uint64_t)pos + (uint64_t)run_sym;
            if (idx > 63u) { return -4; }
            const uint32_t ctx = prevlvl_ctx(prev_cat, has_prev);
            d = &dec[k % lanes]; ++k;
            uint32_t cat = 0u, m = 0u;
            if (tc_rans_get_lut(d, &s->lvl_l[ctx], &s->lvl_m[ctx], &cat) != TC_OK) { return -5; }
            if (tc_rans_get_suffix(d, cat, &m) != TC_OK) { return -5; }
            out[out_n].fam = 2u; out[out_n].ctx = ctx;
            out[out_n].sym = cat; out[out_n].m = m; ++out_n;
            blk[idx] = (m & 1u) ? -(int32_t)(m >> 1) : (int32_t)(m >> 1);
            rowmask |= 1u << (uint32_t)(idx >> 3);
            prev_cat = cat;
            has_prev = 1;
            pos = (uint32_t)idx + 1u;
        }
        rowmask_acc += rowmask;
    }
    return (out_n == s->n && rowmask_acc != 0u) ? 0 : -6;
}


/* ---- B(4)-reg：4 状态寄存器驻留（宏特化 x0..x3 + force-inline 步进）----
 * 与 dec_slice(lanes=4) 的差别只在状态放寄存器（每符号 switch(k&3)
 * 分派到对应标量，编译器特化每个分支）——测量"真交错实现"的天花板。
 * 流布局与 B(4) 相同（4 个独立 flush 拼接）。 */
#define ILV_REFILL(X, P, BUF, SZ)                                            \
    do {                                                                      \
        while ((X) < TC_RANS_L) {                                             \
            if ((P) >= (SZ)) { return -9; }                                   \
            (X) = ((X) << 8) | (BUF)[(P)++];                                  \
        }                                                                     \
    } while (0)

/* 一步符号解码（镜像 tc_rans_get_lut 数值；状态为标量寄存器） */
#define ILV_STEP(X, P, BUF, SZ, LUT, MDL, SYM_OUT)                            \
    do {                                                                      \
        const uint32_t slot_ = (X) & (TC_RANS_SCALE_TOTAL - 1u);              \
        const uint32_t symv_ = (LUT)->sym[slot_];                             \
        const uint32_t f_ = (MDL)->freq[symv_];                               \
        (X) = f_ * ((X) >> TC_RANS_SCALE_BITS) + slot_ - (MDL)->cum[symv_];   \
        ILV_REFILL(X, P, BUF, SZ);                                            \
        (SYM_OUT) = symv_;                                                    \
    } while (0)

/* 一次 chunk 读取（镜像 tc_rans_get_chunk：先取低位再右移，然后 refill） */
#define ILV_CHUNK(X, P, BUF, SZ, W, VOUT)                                     \
    do {                                                                      \
        const uint32_t w_ = (W);                                              \
        (VOUT) = (X) & ((1u << w_) - 1u);                                     \
        (X) >>= w_;                                                           \
        ILV_REFILL(X, P, BUF, SZ);                                            \
    } while (0)

/* 类别后缀（镜像 tc_rans_get_suffix；b>23 拆 [16][b-16] 两块） */
#define ILV_SUFFIX(X, P, BUF, SZ, CAT, MOUT)                                  \
    do {                                                                      \
        uint32_t v_ = 0u;                                                     \
        const uint32_t b_ = (CAT) > 1u ? (CAT) - 1u : 0u;                     \
        if (b_ > 23u) {                                                       \
            uint32_t lo_ = 0u, hi_ = 0u;                                      \
            ILV_CHUNK(X, P, BUF, SZ, 16u, lo_);                               \
            ILV_CHUNK(X, P, BUF, SZ, b_ - 16u, hi_);                          \
            v_ = (hi_ << 16u) | lo_;                                          \
        } else if (b_ != 0u) {                                                \
            ILV_CHUNK(X, P, BUF, SZ, b_, v_);                                 \
        }                                                                     \
        (MOUT) = (CAT) == 0u ? 0u : ((1u << ((CAT) - 1u)) | v_);              \
    } while (0)

/* lane 分派：switch(k&3) 特化到 x0..x3 标量（编译器各生成一份直通代码） */
#define ILV_LANE3(OP, ...)                                                    \
    do {                                                                      \
        switch (k & 3u) {                                                     \
        case 0u: OP(x0, pos0, buf0, size0, __VA_ARGS__); break;               \
        case 1u: OP(x1, pos1, buf1, size1, __VA_ARGS__); break;               \
        case 2u: OP(x2, pos2, buf2, size2, __VA_ARGS__); break;               \
        default: OP(x3, pos3, buf3, size3, __VA_ARGS__); break;               \
        }                                                                     \
    } while (0)

static tc_rans_dec dec_tmp[4];

static int32_t dec_slice_reg4(slice_bench* s)
{
    for (uint32_t l = 0u; l < 4u; ++l) {
        if (tc_rans_dec_init(&dec_tmp[l], s->buf[l], s->size[l]) != TC_OK) {
            return -1;
        }
    }
    uint32_t x0 = dec_tmp[0].x, x1 = dec_tmp[1].x, x2 = dec_tmp[2].x, x3 = dec_tmp[3].x;
    size_t pos0 = dec_tmp[0].pos, pos1 = dec_tmp[1].pos, pos2 = dec_tmp[2].pos, pos3 = dec_tmp[3].pos;
    const uint8_t* buf0 = dec_tmp[0].buf; const uint8_t* buf1 = dec_tmp[1].buf;
    const uint8_t* buf2 = dec_tmp[2].buf; const uint8_t* buf3 = dec_tmp[3].buf;
    const size_t size0 = dec_tmp[0].size, size1 = dec_tmp[1].size;
    const size_t size2 = dec_tmp[2].size, size3 = dec_tmp[3].size;
    sym_rec* out = s->dec_out;
    uint32_t out_n = 0u;
    uint32_t k = 0u;
    int32_t blk[64];
    uint32_t prev_dc_cat = 0u;
    int first_block = 1;
    uint64_t rowmask_acc = 0u;
    int32_t dc_left = 0;
    while (out_n < s->n) {
        {   const uint32_t ctx = first_block ? 0u : dc_ctx_fn(prev_dc_cat, 1);
            uint32_t cat = 0u, m = 0u;
            ILV_LANE3(ILV_STEP, &s->dc_l[ctx], &s->dc_m[ctx], cat);
            ILV_LANE3(ILV_SUFFIX, cat, m); ++k;
            out[out_n].fam = 0u; out[out_n].ctx = ctx;
            out[out_n].sym = cat; out[out_n].m = m; ++out_n;
            blk[0] = (m & 1u) ? -(int32_t)(m >> 1) : (int32_t)(m >> 1);
            dc_left = (dc_left + blk[0]) >> 1;
            prev_dc_cat = cat;
            first_block = 0;
        }
        uint32_t rowmask = 1u;
        uint32_t pos = 1u;
        uint32_t prev_cat = 0u;
        int has_prev = 0;
        for (;;) {
            uint32_t run_sym = 0u;
            ILV_LANE3(ILV_STEP, &s->run_l, &s->run_m, run_sym); ++k;
            out[out_n].fam = 1u; out[out_n].ctx = 0u;
            out[out_n].sym = run_sym; out[out_n].m = 0u; ++out_n;
            if (run_sym == 63u) { break; }
            const uint64_t idx = (uint64_t)pos + (uint64_t)run_sym;
            if (idx > 63u) { return -4; }
            const uint32_t ctx = prevlvl_ctx(prev_cat, has_prev);
            uint32_t cat = 0u, m = 0u;
            ILV_LANE3(ILV_STEP, &s->lvl_l[ctx], &s->lvl_m[ctx], cat);
            ILV_LANE3(ILV_SUFFIX, cat, m); ++k;
            out[out_n].fam = 2u; out[out_n].ctx = ctx;
            out[out_n].sym = cat; out[out_n].m = m; ++out_n;
            blk[idx] = (m & 1u) ? -(int32_t)(m >> 1) : (int32_t)(m >> 1);
            rowmask |= 1u << (uint32_t)(idx >> 3);
            prev_cat = cat;
            has_prev = 1;
            pos = (uint32_t)idx + 1u;
        }
        rowmask_acc += rowmask;
    }
    (void)dc_left;
    return (out_n == s->n && rowmask_acc != 0u) ? 0 : -6;
}

/* A-reg 对照：单状态，但 x 提升为寄存器标量（无交错）——分离
 * "寄存器驻代码生成效益" 与 "交错破链效益"。流布局与 A 相同。 */
static int32_t dec_slice_reg1(slice_bench* s)
{
    if (tc_rans_dec_init(&dec_tmp[0], s->buf[0], s->size[0]) != TC_OK) { return -1; }
    uint32_t x0 = dec_tmp[0].x;
    size_t pos0 = dec_tmp[0].pos;
    const uint8_t* buf0 = dec_tmp[0].buf;
    const size_t size0 = dec_tmp[0].size;
    sym_rec* out = s->dec_out;
    uint32_t out_n = 0u;
    int32_t blk[64];
    uint32_t prev_dc_cat = 0u;
    int first_block = 1;
    uint64_t rowmask_acc = 0u;
    int32_t dc_left = 0;
    while (out_n < s->n) {
        {   const uint32_t ctx = first_block ? 0u : dc_ctx_fn(prev_dc_cat, 1);
            uint32_t cat = 0u, m = 0u;
            ILV_STEP(x0, pos0, buf0, size0, &s->dc_l[ctx], &s->dc_m[ctx], cat);
            ILV_SUFFIX(x0, pos0, buf0, size0, cat, m);
            out[out_n].fam = 0u; out[out_n].ctx = ctx;
            out[out_n].sym = cat; out[out_n].m = m; ++out_n;
            blk[0] = (m & 1u) ? -(int32_t)(m >> 1) : (int32_t)(m >> 1);
            dc_left = (dc_left + blk[0]) >> 1;
            prev_dc_cat = cat;
            first_block = 0;
        }
        uint32_t rowmask = 1u;
        uint32_t pos = 1u;
        uint32_t prev_cat = 0u;
        int has_prev = 0;
        for (;;) {
            uint32_t run_sym = 0u;
            ILV_STEP(x0, pos0, buf0, size0, &s->run_l, &s->run_m, run_sym);
            out[out_n].fam = 1u; out[out_n].ctx = 0u;
            out[out_n].sym = run_sym; out[out_n].m = 0u; ++out_n;
            if (run_sym == 63u) { break; }
            const uint64_t idx = (uint64_t)pos + (uint64_t)run_sym;
            if (idx > 63u) { return -4; }
            const uint32_t ctx = prevlvl_ctx(prev_cat, has_prev);
            uint32_t cat = 0u, m = 0u;
            ILV_STEP(x0, pos0, buf0, size0, &s->lvl_l[ctx], &s->lvl_m[ctx], cat);
            ILV_SUFFIX(x0, pos0, buf0, size0, cat, m);
            out[out_n].fam = 2u; out[out_n].ctx = ctx;
            out[out_n].sym = cat; out[out_n].m = m; ++out_n;
            blk[idx] = (m & 1u) ? -(int32_t)(m >> 1) : (int32_t)(m >> 1);
            rowmask |= 1u << (uint32_t)(idx >> 3);
            prev_cat = cat;
            has_prev = 1;
            pos = (uint32_t)idx + 1u;
        }
        rowmask_acc += rowmask;
    }
    (void)dc_left;
    return (out_n == s->n && rowmask_acc != 0u) ? 0 : -6;
}

static int verify(const slice_bench* s)
{
    for (uint32_t i = 0u; i < s->n; ++i) {
        if (s->dec_out[i].fam != s->v[i].fam || s->dec_out[i].ctx != s->v[i].ctx
            || s->dec_out[i].sym != s->v[i].sym || s->dec_out[i].m != s->v[i].m) {
            fprintf(stderr, "verify fail sym %u/%u\n", i, s->n);
            return -1;
        }
    }
    return 0;
}

static int cmp_double(const void* a, const void* b)
{
    const double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "/tmp/rans_ilv/syms.bin";
    const uint32_t rounds = argc > 2 ? (uint32_t)atoi(argv[2]) : 7u;
    FILE* f = fopen(path, "rb");
    if (f == NULL) { perror("open"); return 1; }
    uint32_t n_slices = 0u;
    if (fread(&n_slices, 4u, 1u, f) != 1u) { return 1; }
    slice_bench* sl = (slice_bench*)calloc(n_slices, sizeof(slice_bench));
    uint64_t total_syms = 0u;
    for (uint32_t i = 0u; i < n_slices; ++i) {
        uint32_t n = 0u;
        if (fread(&n, 4u, 1u, f) != 1u) { return 1; }
        sl[i].n = n;
        sl[i].v = (sym_rec*)malloc((size_t)n * sizeof(sym_rec));
        sl[i].dec_out = (sym_rec*)malloc((size_t)n * sizeof(sym_rec));
        if (sl[i].v == NULL || sl[i].dec_out == NULL) { return 1; }
        uint32_t dc_cnt[DC_CTX][DC_SYMS];
        uint32_t run_cnt[RUN_SYMS];
        uint32_t lvl_cnt[LVL_CTX][LVL_SYMS];
        memset(dc_cnt, 0, sizeof(dc_cnt));
        memset(run_cnt, 0, sizeof(run_cnt));
        memset(lvl_cnt, 0, sizeof(lvl_cnt));
        for (uint32_t j = 0u; j < n; ++j) {
            uint8_t rec[6];
            if (fread(rec, 6u, 1u, f) != 1u) { return 1; }
            sl[i].v[j].fam = rec[0];
            sl[i].v[j].ctx = rec[1];
            sl[i].v[j].sym = (uint32_t)rec[2] | ((uint32_t)rec[3] << 8);
            sl[i].v[j].m = (uint32_t)rec[4] | ((uint32_t)rec[5] << 8);
            const sym_rec* r = &sl[i].v[j];
            if (r->fam == 0u) { dc_cnt[r->ctx][r->sym] += 1u; }
            else if (r->fam == 1u) { run_cnt[r->sym] += 1u; }
            else { lvl_cnt[r->ctx][r->sym] += 1u; }
        }
        total_syms += n;
        for (uint32_t c = 0u; c < DC_CTX; ++c) {
            uint32_t tot = 0u;
            for (uint32_t q = 0u; q < DC_SYMS; ++q) { tot += dc_cnt[c][q]; }
            if (tot == 0u) { dc_cnt[c][0] = 1u; }   /* 空行占位（同生产） */
        }
        for (uint32_t c = 0u; c < LVL_CTX; ++c) {
            uint32_t tot = 0u;
            for (uint32_t q = 0u; q < LVL_SYMS; ++q) { tot += lvl_cnt[c][q]; }
            if (tot == 0u) { lvl_cnt[c][0] = 1u; }
        }
        for (uint32_t c = 0u; c < DC_CTX; ++c) {
            if (tc_rans_model_build(&sl[i].dc_m[c], dc_cnt[c], DC_SYMS) != TC_OK) { return 1; }
        }
        if (tc_rans_model_build(&sl[i].run_m, run_cnt, RUN_SYMS) != TC_OK) { return 1; }
        for (uint32_t c = 0u; c < LVL_CTX; ++c) {
            if (tc_rans_model_build(&sl[i].lvl_m[c], lvl_cnt[c], LVL_SYMS) != TC_OK) { return 1; }
        }
        for (uint32_t c = 0u; c < DC_CTX; ++c) { tc_rans_lut_build(&sl[i].dc_l[c], &sl[i].dc_m[c]); }
        tc_rans_lut_build(&sl[i].run_l, &sl[i].run_m);
        for (uint32_t c = 0u; c < LVL_CTX; ++c) { tc_rans_lut_build(&sl[i].lvl_l[c], &sl[i].lvl_m[c]); }
    }
    fclose(f);
    printf("[ilv] %u slices, %" PRIu64 " symbols\n", n_slices, total_syms);

    const uint32_t lane_cfgs[] = {1u, 2u, 4u, 8u, 4u, 1u}; /* [4]=4-reg, [5]=A-reg */
    double best_ms[6];
    double bytes_per_sym[6];
    for (int c = 0; c < 6; ++c) {
        const uint32_t lanes = lane_cfgs[c];
        uint64_t bytes = 0u;
        for (uint32_t i = 0u; i < n_slices; ++i) {
            for (uint32_t q = 0u; q < MAX_LANES; ++q) {
                free(sl[i].buf[q]);
                sl[i].buf[q] = NULL;
                sl[i].size[q] = 0u;
            }
            if (enc_slice(&sl[i], lanes, sl[i].buf, sl[i].size) != 0) {
                fprintf(stderr, "enc fail lanes=%u slice %u: %s\n", lanes, i, tc_last_error());
                return 1;
            }
            for (uint32_t q = 0u; q < lanes; ++q) { bytes += sl[i].size[q]; }
        }
        bytes_per_sym[c] = (double)bytes * 8.0 / (double)total_syms;
        for (uint32_t i = 0u; i < n_slices; ++i) {
            const int32_t drc = (c == 4) ? dec_slice_reg4(&sl[i])
                                         : (c == 5) ? dec_slice_reg1(&sl[i])
                                         : dec_slice(&sl[i], lanes);
            if (drc != 0) {
                fprintf(stderr, "decode fail cfg=%d slice %u rc=%d\n", c, i, drc);
                return 1;
            }
            if (verify(&sl[i]) != 0) {
                fprintf(stderr, "verify fail cfg=%d\n", c);
                return 1;
            }
        }
        for (uint32_t i = 0u; i < n_slices; ++i) {
            if (c == 4) { (void)dec_slice_reg4(&sl[i]); }
            else if (c == 5) { (void)dec_slice_reg1(&sl[i]); }
            else { (void)dec_slice(&sl[i], lanes); }
        }
        double ms[32];
        for (uint32_t r = 0u; r < rounds; ++r) {
            const double t0 = now_sec();
            for (uint32_t i = 0u; i < n_slices; ++i) {
                if (c == 4) {
                    if (dec_slice_reg4(&sl[i]) != 0) { return 1; }
                } else if (c == 5) {
                    if (dec_slice_reg1(&sl[i]) != 0) { return 1; }
                } else if (dec_slice(&sl[i], lanes) != 0) { return 1; }
            }
            ms[r] = (now_sec() - t0) * 1e3;
        }
        qsort(ms, rounds, sizeof(double), cmp_double);
        best_ms[c] = ms[rounds / 2u];
        printf("[ilv] lanes=%u%s: %8.2f ms | %6.1f M sym/s | %.2f ns/sym | "
               "%.3f bits/sym\n", lanes, c == 4 ? "-reg4" : (c == 5 ? "-reg1(A)" : ""), best_ms[c],
               (double)total_syms / (best_ms[c] * 1e-3) / 1e6,
               best_ms[c] * 1e6 / (double)total_syms, bytes_per_sym[c]);
    }
    printf("[ilv] 加速比：2 lane %.2f× / 4 lane %.2f× / 8 lane %.2f×；"
           "体积差 2/4/8 lane %+0.2f%%/%+0.2f%%/%+0.2f%%\n",
           best_ms[0] / best_ms[1], best_ms[0] / best_ms[2],
           best_ms[0] / best_ms[3],
           (bytes_per_sym[1] / bytes_per_sym[0] - 1.0) * 100.0,
           (bytes_per_sym[2] / bytes_per_sym[0] - 1.0) * 100.0,
           (bytes_per_sym[3] / bytes_per_sym[0] - 1.0) * 100.0);
    return 0;
}
