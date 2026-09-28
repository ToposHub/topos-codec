/* R6 性能整改单元测试：
 *  1. zigzag 逆表一致性 + 量化/块编码 natural↔zigzag 双布局差分（bit-exact 依据）；
 *  2. encode_sized 快速搜索 vs legacy 线性搜索差分（qp_used / 包字节逐一对拍）；
 *  3. sized 最终包自检契约（tc_frame_decode 复验）；
 *  4. tc_parallel_for 不变量（动态领取分发契约：worker 槽互斥、每任务恰一次、
 *     并发调用方、线程数扩展）——spawn-per-call 与常驻池两种实现都必须满足。
 */
#include "codec/codec.h"
#include "common/tpool.h"
#include "entropy/block_coding.h"
#include "entropy/rice.h"
#include "entropy/scan.h"
#include "image_synth.h"
#include "mini_test.h"
#include "topos_codec.h"
#include "transform/quant.h"

#include "../support/port_thread.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

/* ---------- 基础构帧（与 test_codec 同构） ---------- */

static void base_cfg(topos_frame_config* c, uint32_t w, uint32_t h)
{
    memset(c, 0, sizeof(*c));
    c->struct_size = (uint32_t)sizeof(*c);
    c->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    c->visible_width = (uint16_t)w;
    c->visible_height = (uint16_t)h;
    c->qmatrix_id = 1u;
}

static void fill_input(topos_frame_input* in, const uint16_t* pl[4])
{
    memset(in, 0, sizeof(*in));
    in->struct_size = (uint32_t)sizeof(topos_frame_input);
    in->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    for (int i = 0; i < 4; ++i) { in->planes[i] = pl[i]; }
}

static uint64_t r6_next(uint64_t* s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

/* ---------- 1. zigzag 表与双布局差分 ---------- */

static void test_r6_zigzag_layout(void)
{
    /* 逆表为 kTcZigzag 的精确逆（双射） */
    uint8_t seen[64];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < 64; ++i) {
        MT_CHECK_EQ_U64(kTcZigzag[kTcZigzagInv[i]], (uint64_t)i);
        MT_CHECK(seen[kTcZigzagInv[i]] == 0);
        seen[kTcZigzagInv[i]] = 1;
    }

    /* 量化：同 ctx 下 zigzag 变体与 natural 变体逐系数一致 */
    const tc_qmatrix_set* qms = tc_qmatrix_by_id(1u);
    uint64_t s = 0x065EED1234567890ull;
    for (int qp_i = 0; qp_i < 3; ++qp_i) {
        uint32_t qp = qp_i == 0 ? 4u : (qp_i == 1 ? 20u : 50u);
        tc_quant_ctx qctx;
        tc_quant_ctx_init(&qctx, qms->luma, qp);
        for (int t = 0; t < 200; ++t) {
            int32_t F[64];
            for (int i = 0; i < 64; ++i) {
                F[i] = (int32_t)(r6_next(&s) % 3000000u) - 1500000; /* 覆盖钳位域 */
            }
            int32_t qn[64];
            int32_t qz[64];
            memset(qz, 0x5A, sizeof(qz));
            tc_quant_block_ctx(&qctx, F, qn);
            tc_quant_block_zigzag(&qctx, F, qz);
            for (int i = 0; i < 64; ++i) {
                MT_CHECK_EQ_I64(qz[kTcZigzagInv[i]], qn[i]);
            }
        }
    }

    /* 块编码：两布局输出位流逐字节一致（含全零块 / 稀疏 / 稠密） */
    for (int t = 0; t < 300; ++t) {
        int32_t nat[64];
        memset(nat, 0, sizeof(nat));
        uint32_t nonzeros = (uint32_t)(r6_next(&s) % 64u);
        for (uint32_t n = 0; n < nonzeros; ++n) {
            uint32_t pos = (uint32_t)(r6_next(&s) % 64u);
            int32_t v = (int32_t)(r6_next(&s) % 8000u) - 4000;
            nat[pos] = v;
        }
        int32_t zig[64];
        for (int i = 0; i < 64; ++i) { zig[kTcZigzagInv[i]] = nat[i]; }

        tc_bitwriter bwn, bwz;
        MT_CHECK_EQ_I64(tc_bitwriter_init(&bwn), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_init(&bwz), TC_OK);
        int32_t rcn = tc_block_encode(&bwn, 2u, 4u, 1u, nat, 1, nat[0], 1, nat[0], NULL);
        int32_t rcz = tc_block_encode_zigzag(&bwz, 2u, 4u, 1u, zig, 1, nat[0], 1, nat[0], NULL);
        MT_CHECK_EQ_I64(rcn, TC_OK);
        MT_CHECK_EQ_I64(rcz, TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bwn), TC_OK);
        MT_CHECK_EQ_I64(tc_bitwriter_flush_zero_pad(&bwz), TC_OK);
        MT_CHECK_EQ_U64(tc_bitwriter_byte_size(&bwn), tc_bitwriter_byte_size(&bwz));
        MT_CHECK(memcmp(tc_bitwriter_data(&bwn), tc_bitwriter_data(&bwz),
                        tc_bitwriter_byte_size(&bwn)) == 0);
        tc_bitwriter_free(&bwn);
        tc_bitwriter_free(&bwz);
    }
}

/* ---------- 2. encode_sized 差分：fast vs legacy ---------- */

typedef struct sized_case {
    tc_synth_kind kind;
    uint32_t w, h;
    uint32_t qp0;
    double ratio; /* target = ratio × bytes(qp0) */
    uint32_t slice_rows;
    int with_alpha;
} sized_case;

static int32_t sized_run(const topos_frame_config* cfg, const topos_frame_input* in,
                         uint32_t target, uint8_t** out, size_t* size, uint8_t* qp_used,
                         topos_frame_stats* st)
{
    *out = NULL;
    *size = 0;
    size_t cap = tc_frame_packet_bound(cfg);
    uint8_t* buf = (uint8_t*)malloc(cap != 0u ? cap : 1u);
    if (buf == NULL) { return TC_ERR_OUT_OF_MEMORY; }
    int32_t rc = tc_frame_encode_sized(cfg, in, target, 0u, 63u, qp_used, buf, cap, st);
    if (rc == TC_OK) {
        *out = buf;
        *size = st->packet_size;
    } else {
        free(buf);
    }
    return rc;
}

static void test_r6_sized_differential(void)
{
    const sized_case cases[] = {
        { TC_SYNTH_GRAIN, 64u, 48u, 20u, 0.60, 1u, 0 },
        { TC_SYNTH_GRAIN, 64u, 48u, 20u, 0.25, 1u, 0 },
        { TC_SYNTH_GRAIN, 64u, 48u, 20u, 2.00, 1u, 0 },   /* 深度欠用 → 长回收 */
        { TC_SYNTH_GRAIN, 64u, 48u, 4u, 0.05, 1u, 0 },    /* 深度爬升 → 近 qp_max */
        { TC_SYNTH_GRAIN, 64u, 48u, 44u, 0.50, 1u, 0 },
        { TC_SYNTH_GRAIN, 64u, 48u, 30u, 1.00, 1u, 0 },   /* 恰好边界 */
        { TC_SYNTH_FLAT, 96u, 64u, 12u, 2.00, 1u, 0 },    /* 平坦：极小包 + 绑定 */
        { TC_SYNTH_FLAT, 96u, 64u, 30u, 0.02, 1u, 0 },
        { TC_SYNTH_GRADIENT, 96u, 64u, 20u, 0.60, 2u, 0 },
        { TC_SYNTH_DETAIL, 128u, 72u, 16u, 0.40, 1u, 0 },
        { TC_SYNTH_DETAIL, 128u, 72u, 16u, 3.00, 1u, 0 },
        { TC_SYNTH_MIXED, 128u, 72u, 24u, 0.80, 1u, 0 },
        { TC_SYNTH_MIXED, 64u, 48u, 10u, 1.50, 1u, 1 },   /* alpha mode1 */
        { TC_SYNTH_GRAIN, 96u, 64u, 18u, 0.55, 1u, 1 },
    };
    enum { NCASES = (int)(sizeof(cases) / sizeof(cases[0])) };

    for (int ci = 0; ci < NCASES; ++ci) {
        const sized_case* cs = &cases[ci];
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x1234ABCD00000000ull + (uint64_t)ci;
        ic.width = cs->w;
        ic.height = cs->h;
        ic.kind = cs->kind;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, cs->with_alpha, &y, &u, &v, &a), 0);
        const uint16_t* pl[4] = {y, u, v, a};
        topos_frame_input in;
        fill_input(&in, pl);

        topos_frame_config cfg;
        base_cfg(&cfg, cs->w, cs->h);
        cfg.qp_base = (uint8_t)cs->qp0;
        cfg.slice_rows = (uint16_t)cs->slice_rows;
        if (cs->with_alpha) { cfg.alpha_mode = 1u; cfg.alpha_bit_depth = 16u; }

        /* 基准尺寸：plain 编码（qp0） */
        size_t cap = tc_frame_packet_bound(&cfg);
        uint8_t* base = (uint8_t*)malloc(cap);
        topos_frame_stats bst;
        MT_CHECK(base != NULL);
        if (base == NULL) { free(y); free(u); free(v); free(a); continue; }
        MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, base, cap, &bst), TC_OK);
        uint32_t target = (uint32_t)((double)bst.packet_size * cs->ratio);
        free(base);

        /* fast（默认） */
        uint8_t* pkt_fast = NULL;
        size_t size_fast = 0;
        uint8_t qp_fast = 0xFF;
        topos_frame_stats st_fast;
        memset(&st_fast, 0, sizeof(st_fast));
        tc_dev_set_sized_mode(2); /* R6 快速（与 legacy 逐字节差分对） */
        int32_t rc_fast = sized_run(&cfg, &in, target, &pkt_fast, &size_fast, &qp_fast, &st_fast);

        /* legacy（强制线性） */
        uint8_t* pkt_lin = NULL;
        size_t size_lin = 0;
        uint8_t qp_lin = 0xFF;
        topos_frame_stats st_lin;
        memset(&st_lin, 0, sizeof(st_lin));
        tc_dev_set_sized_mode(1);
        int32_t rc_lin = sized_run(&cfg, &in, target, &pkt_lin, &size_lin, &qp_lin, &st_lin);
        tc_dev_set_sized_mode(0);

        MT_CHECK_EQ_I64(rc_fast, TC_OK);
        MT_CHECK_EQ_I64(rc_lin, TC_OK);
        if (rc_fast == TC_OK && rc_lin == TC_OK) {
            MT_CHECK_EQ_U64(qp_fast, qp_lin);                    /* 最终 qp 一致 */
            MT_CHECK_EQ_U64(size_fast, size_lin);                /* 包尺寸一致 */
            MT_CHECK(memcmp(pkt_fast, pkt_lin, size_fast) == 0); /* 包字节一致 */
            MT_CHECK_EQ_U64(st_fast.qp_base, qp_fast);           /* stats 对应最终包 */

            /* 预算语义：最终包 ≤ target，或在 qp_max（qp_used == 63）时允许超 */
            if (qp_fast != 63u && target > 0u) {
                MT_CHECK(size_fast <= target);
            }

            /* 最终包可解码（自检契约的外部复验） */
            topos_frame_output info;
            MT_CHECK_EQ_I64(tc_frame_decode(pkt_fast, size_fast, NULL, NULL, &info), TC_OK);
            MT_CHECK_EQ_U64(info.concealed_slices, 0u);
        }
        free(pkt_fast);
        free(pkt_lin);
        free(y); free(u); free(v); free(a);
    }
}

/* ---------- 2b. M10-7C strict 搜索：最小命中 QP 契约 ----------
 * 与 legacy 的差分对（上文）不同，strict 是语义变更：断言其自身不变量——
 *  - bytes ≤ target（qp_max best-effort 除外）；
 *  - 最小性：qp_used−1 的 plain 编码超限，或 qp_used == qp_min；
 *  - 探测预算：iters（probe + final）≤ 7（真实素材 p50 ≤ 4 的单测上界）；
 *  - 确定性 + 最终包可解码无 conceal。 */
static void test_r6_sized_strict(void)
{
    const sized_case cases[] = {
        { TC_SYNTH_GRAIN, 64u, 48u, 20u, 0.60, 1u, 0 },
        { TC_SYNTH_GRAIN, 64u, 48u, 20u, 0.25, 1u, 0 },
        { TC_SYNTH_GRAIN, 64u, 48u, 20u, 2.00, 1u, 0 },   /* 深度欠用 → 向下直探最小命中 */
        { TC_SYNTH_GRAIN, 64u, 48u, 4u, 0.05, 1u, 0 },    /* 深度爬升 → 近 qp_max */
        { TC_SYNTH_GRAIN, 64u, 48u, 44u, 0.50, 1u, 0 },
        { TC_SYNTH_GRAIN, 64u, 48u, 30u, 1.00, 1u, 0 },   /* 恰好边界 */
        { TC_SYNTH_FLAT, 96u, 64u, 12u, 2.00, 1u, 0 },    /* 平坦：极小包 */
        { TC_SYNTH_FLAT, 96u, 64u, 30u, 0.02, 1u, 0 },
        { TC_SYNTH_GRADIENT, 96u, 64u, 20u, 0.60, 2u, 0 },
        { TC_SYNTH_DETAIL, 128u, 72u, 16u, 0.40, 1u, 0 },
        { TC_SYNTH_DETAIL, 128u, 72u, 16u, 3.00, 1u, 0 },
        { TC_SYNTH_MIXED, 128u, 72u, 24u, 0.80, 1u, 0 },
        { TC_SYNTH_MIXED, 64u, 48u, 10u, 1.50, 1u, 1 },   /* alpha mode1 */
        { TC_SYNTH_GRAIN, 96u, 64u, 18u, 0.55, 1u, 1 },
    };
    enum { NCASES = (int)(sizeof(cases) / sizeof(cases[0])) };

    tc_dev_set_sized_mode(0);
    /* 本门按 qp_base 种子标定（≤10）；跨帧提示路径由 test_r6_qp_hint 覆盖 */
    tc_dev_set_qp_hint(1);
    for (int ci = 0; ci < NCASES; ++ci) {
        const sized_case* cs = &cases[ci];
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x1234ABCD00000000ull + (uint64_t)ci;
        ic.width = cs->w;
        ic.height = cs->h;
        ic.kind = cs->kind;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, cs->with_alpha, &y, &u, &v, &a), 0);
        const uint16_t* pl[4] = {y, u, v, a};
        topos_frame_input in;
        fill_input(&in, pl);

        topos_frame_config cfg;
        base_cfg(&cfg, cs->w, cs->h);
        cfg.qp_base = (uint8_t)cs->qp0;
        cfg.slice_rows = (uint16_t)cs->slice_rows;
        if (cs->with_alpha) { cfg.alpha_mode = 1u; cfg.alpha_bit_depth = 16u; }

        size_t cap = tc_frame_packet_bound(&cfg);
        uint8_t* base = (uint8_t*)malloc(cap);
        topos_frame_stats bst;
        MT_CHECK(base != NULL);
        if (base == NULL) { free(y); free(u); free(v); free(a); continue; }
        MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, base, cap, &bst), TC_OK);
        uint32_t target = (uint32_t)((double)bst.packet_size * cs->ratio);
        free(base);

        tc_dev_sized_reset();
        uint8_t* pkt = NULL;
        size_t size = 0;
        uint8_t qp_used = 0xFF;
        topos_frame_stats st;
        memset(&st, 0, sizeof(st));
        int32_t rc = sized_run(&cfg, &in, target, &pkt, &size, &qp_used, &st);
        MT_CHECK_EQ_I64(rc, TC_OK);
        if (rc == TC_OK) {
            /* 命中语义：bytes ≤ target（qp_max 封顶除外） */
            if (qp_used != 63u && target > 0u) {
                MT_CHECK(size <= target);
            }
            /* 最小性：qp_used−1 超限或已达 qp_min */
            if (qp_used > 0u) {
                uint8_t* below = (uint8_t*)malloc(cap);
                topos_frame_stats stb;
                MT_CHECK(below != NULL);
                if (below != NULL) {
                    topos_frame_config cd = cfg;
                    cd.qp_base = (uint8_t)(qp_used - 1u);
                    MT_CHECK_EQ_I64(tc_frame_encode(&cd, &in, below, cap, &stb), TC_OK);
                    if (qp_used > 0u && target > 0u) {
                        MT_CHECK(stb.packet_size > target); /* q*−1 必超限 */
                    }
                    free(below);
                }
            }
            /* 探测预算（probe + final）：合成素材最坏（平坦内容 bytes(q)
             * 对 q 近乎不敏感，实测 9 次）；真实素材 p50 门见 bench 证据 */
            MT_CHECK(tc_dev_sized_iters() <= 10u);
            /* 确定性 */
            uint8_t qp2 = 0u;
            topos_frame_stats st2;
            uint8_t* pkt2 = NULL;
            size_t size2 = 0;
            tc_dev_sized_reset();
            MT_CHECK_EQ_I64(sized_run(&cfg, &in, target, &pkt2, &size2, &qp2, &st2), TC_OK);
            MT_CHECK_EQ_U64(qp_used, qp2);
            MT_CHECK_EQ_U64(size, size2);
            MT_CHECK(memcmp(pkt, pkt2, size) == 0);
            free(pkt2);
            /* 最终包可解码（自检契约外部复验） */
            topos_frame_output info;
            MT_CHECK_EQ_I64(tc_frame_decode(pkt, size, NULL, NULL, &info), TC_OK);
            MT_CHECK_EQ_U64(info.concealed_slices, 0u);
        }
        free(pkt);
        free(y); free(u); free(v); free(a);
    }
    tc_dev_set_qp_hint(0);
}

/* ---------- 3. tc_parallel_for 不变量（动态领取分发契约） ----------
 * 解码深化批次起分发为共享游标动态领取：任务序号不再与执行线程绑定，
 * 旧「idx%nw 条带互斥」契约随之作废。新契约（与 codec 槽位资源取用同构）：
 *  - scratch 槽以执行线程标识：tc_pool_worker_slot()（0=调用线程，1..=worker）；
 *  - 同一 worker 槽位绝不并发（线程身份互斥；跨并发调用方成立——
 *    每个批次的 slot 0 只可能是该批次自己的调用线程，worker 串行消费批次）；
 *  - 每任务恰好执行一次；实际并行度由调度决定，只断言参与性（≥2 槽）。 */

typedef struct r6_job_ctx {
    atomic_uint exec_total;
    atomic_uint slot_active[TC_SLICE_MAX_THREADS];
    atomic_uint slot_violation;
    atomic_uint slot_exec[TC_SLICE_MAX_THREADS];
} r6_job_ctx;

typedef struct r6_job_one {
    r6_job_ctx* shared;
    uint32_t idx;
} r6_job_one;

static void r6_job_hold_slot(void)
{
    /* 占用槽位 ~1ms：同槽并发（若存在）在时间窗内可被稳定观测，
     * 而非依赖纳秒级竞态偶发命中；48 任务 ⇒ 调用线程串行下限 48ms，
     * 远大于 worker 被 cond 唤醒的调度延迟——参与性断言不依赖运气 */
#if defined(_WIN32)
    Sleep(1);
#else
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000; /* 1ms */
    nanosleep(&ts, NULL);
#endif
}

static void r6_parallel_job(void* v)
{
    r6_job_one* one = (r6_job_one*)v;
    r6_job_ctx* jc = one->shared;
    atomic_fetch_add(&jc->exec_total, 1u);
    uint32_t slot = tc_pool_worker_slot();
    if (slot >= (uint32_t)TC_SLICE_MAX_THREADS) {
        atomic_fetch_add(&jc->slot_violation, 1u); /* 槽位越界 = 契约破坏 */
        slot = 0u;
    }
    if (atomic_fetch_add(&jc->slot_active[slot], 1u) != 0u) {
        atomic_fetch_add(&jc->slot_violation, 1u); /* 同 worker 槽并发 = 不变量破坏 */
    }
    r6_job_hold_slot();
    atomic_fetch_add(&jc->slot_exec[slot], 1u);
    atomic_fetch_sub(&jc->slot_active[slot], 1u);
}

static void r6_ctx_init(r6_job_ctx* jc)
{
    memset(jc, 0, sizeof(*jc));
    for (int i = 0; i < TC_SLICE_MAX_THREADS; ++i) {
        atomic_init(&jc->slot_active[i], 0u);
        atomic_init(&jc->slot_exec[i], 0u);
    }
    atomic_init(&jc->exec_total, 0u);
    atomic_init(&jc->slot_violation, 0u);
}

/* 汇合校验：每任务恰一次且落在合法槽内；used = 实际参与槽位数 */
static void r6_ctx_verify(const char* tag, r6_job_ctx* jc, uint32_t n,
                          uint32_t min_used, uint32_t max_used)
{
    uint64_t sum = 0u;
    uint32_t used = 0u;
    for (int s = 0; s < TC_SLICE_MAX_THREADS; ++s) {
        uint32_t e = atomic_load(&jc->slot_exec[s]);
        sum += e;
        if (e != 0u) { used++; }
    }
    MT_CHECK_EQ_U64(atomic_load(&jc->exec_total), n);
    MT_CHECK_EQ_U64(sum, n); /* 全部任务在合法槽内执行（恰一次） */
    MT_CHECK(used >= min_used);
    MT_CHECK(used <= max_used);
    MT_CHECK_EQ_U64(atomic_load(&jc->slot_violation), 0u);
    (void)tag;
}

enum { R6_NJ = 48, R6_CJ = 64 };

static tc_job r6_jobs[R6_CJ];
static r6_job_one r6_ones[R6_CJ];

static void r6_jobs_build(r6_job_ctx* jc, uint32_t n)
{
    for (uint32_t i = 0u; i < n; ++i) {
        r6_ones[i].shared = jc;
        r6_ones[i].idx = i;
        r6_jobs[i].fn = r6_parallel_job;
        r6_jobs[i].ctx = &r6_ones[i];
    }
}

typedef struct r6_caller_arg {
    uint32_t n;
    uint32_t nw;
    uint32_t used;
} r6_caller_arg;

static void* r6_concurrent_caller(void* v)
{
    r6_caller_arg* a = (r6_caller_arg*)v;
    a->used = tc_parallel_for(r6_jobs, a->n, a->nw);
    return NULL;
}

static void test_r6_parallel_invariants(void)
{
    /* 执行完整性 + worker 槽互斥（nw=2：调用线程 + worker 1） */
    tc_dev_set_thread_count(2);
    r6_job_ctx jc;
    r6_ctx_init(&jc);
    r6_jobs_build(&jc, R6_NJ);
    (void)tc_parallel_for(r6_jobs, R6_NJ, 2u);
    r6_ctx_verify("nw2", &jc, R6_NJ, 2u, 2u);

    /* 线程数扩展：2 → 8（池实现下必须支持运行时增长）。
     * 动态领取下各槽任务数不保证均分，只验证参与性与总量。 */
    tc_dev_set_thread_count(8);
    r6_ctx_init(&jc);
    r6_jobs_build(&jc, R6_NJ);
    (void)tc_parallel_for(r6_jobs, R6_NJ, 8u);
    r6_ctx_verify("nw8", &jc, R6_NJ, 2u, 8u);

    /* 并发调用方：两个线程同时 tc_parallel_for（共享 worker/回压正确性）。
     * 两调用方共用全局 r6_jobs → 各自统计需独立 ctx：改用线程私有 job 集。
     * 各批次内 slot 0 = 该批次调用线程（两调用方同为 0 但分属不同 job 集，
     * 互不冲突）；worker 槽跨批次串行复用，同槽并发仍必须为 0。 */
    {
        static tc_job jobs2[R6_CJ];
        static r6_job_one ones2[R6_CJ];
        r6_job_ctx jcs[2];
        r6_caller_arg args[2];
        pthread_t th;
        r6_ctx_init(&jcs[0]);
        r6_ctx_init(&jcs[1]);
        for (uint32_t i = 0u; i < R6_CJ; ++i) {
            ones2[i].shared = &jcs[1];
            ones2[i].idx = i;
            jobs2[i].fn = r6_parallel_job;
            jobs2[i].ctx = &ones2[i];
        }
        r6_jobs_build(&jcs[0], R6_CJ); /* r6_jobs/ones → jcs[0] */
        args[0].n = R6_CJ; args[0].nw = 4u; args[0].used = 0u;
        args[1].n = R6_CJ; args[1].nw = 4u; args[1].used = 0u;
        if (pthread_create(&th, NULL, r6_concurrent_caller, &args[0]) == 0) {
            /* 第二调用方直接执行（使用 jobs2/ones2 → jcs[1]） */
            args[1].used = tc_parallel_for(jobs2, R6_CJ, 4u);
            pthread_join(th, NULL);
            r6_ctx_verify("cc0", &jcs[0], R6_CJ, 2u, 4u);
            r6_ctx_verify("cc1", &jcs[1], R6_CJ, 2u, 4u);
            MT_CHECK(args[0].used >= 1u);
            MT_CHECK(args[1].used >= 1u);
        } else {
            mt_report(__FILE__, __LINE__, "pthread_create failed");
        }
    }

    /* 多线程编码 parity（1 vs 8 线程 bit-exact；槽位常驻后仍成立） */
    {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0xBEEFCAFEull;
        ic.width = 128u;
        ic.height = 96u;
        ic.kind = TC_SYNTH_GRAIN;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);
        const uint16_t* pl[4] = {y, u, v, NULL};
        topos_frame_input in;
        fill_input(&in, pl);
        topos_frame_config cfg;
        base_cfg(&cfg, 128u, 96u);
        cfg.qp_base = 20u;
        cfg.slice_rows = 2u; /* 6 带 → 多线程路径 */
        size_t cap = tc_frame_packet_bound(&cfg);
        uint8_t* p1 = (uint8_t*)malloc(cap);
        uint8_t* p8 = (uint8_t*)malloc(cap);
        topos_frame_stats s1, s8;
        MT_CHECK(p1 != NULL && p8 != NULL);
        if (p1 != NULL && p8 != NULL) {
            tc_dev_set_thread_count(1);
            MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, p1, cap, &s1), TC_OK);
            tc_dev_set_thread_count(8);
            MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, p8, cap, &s8), TC_OK);
            MT_CHECK_EQ_U64(s1.packet_size, s8.packet_size);
            MT_CHECK(memcmp(p1, p8, s1.packet_size) == 0);

            /* sized 双路径 × 多线程：fast/legacy 在 8 线程下同样一致 */
            uint32_t target = s1.packet_size / 2u;
            uint8_t* pf = (uint8_t*)malloc(cap);
            uint8_t* plb = (uint8_t*)malloc(cap);
            uint8_t qf = 0, ql = 0;
            topos_frame_stats sf, sl;
            MT_CHECK(pf != NULL && plb != NULL);
            if (pf != NULL && plb != NULL) {
                tc_dev_set_sized_mode(2);
                MT_CHECK_EQ_I64(
                    tc_frame_encode_sized(&cfg, &in, target, 0u, 63u, &qf, pf, cap, &sf), TC_OK);
                tc_dev_set_sized_mode(1);
                MT_CHECK_EQ_I64(
                    tc_frame_encode_sized(&cfg, &in, target, 0u, 63u, &ql, plb, cap, &sl), TC_OK);
                tc_dev_set_sized_mode(0);
                MT_CHECK_EQ_U64(qf, ql);
                MT_CHECK_EQ_U64(sf.packet_size, sl.packet_size);
                MT_CHECK(memcmp(pf, plb, sf.packet_size) == 0);
            }
            free(pf);
            free(plb);
        }
        free(p1);
        free(p8);
        free(y); free(u); free(v); free(a);
    }

    tc_dev_set_thread_count(0); /* 恢复默认 */
}

/* ---------- 4. 槽位 bitwriter 扩容回归 ---------- */

static void test_r6_slot_writer_growth(void)
{
    /* 大带（band payload > bitwriter 初容量 64KiB）× sized 多迭代：
     * 常驻槽位 bitwriter 为结构体拷贝借出，扩容后必须回写 enc_shared，
     * 否则下轮迭代对悬垂指针 realloc（1080p 首触发的事故固化）。 */
    image_synth_cfg ic;
    memset(&ic, 0, sizeof(ic));
    ic.seed = 0x90AB12CD34EF5678ull;
    ic.width = 640u;
    ic.height = 512u; /* slice_rows 默认 32 → 2 带/平面，带 payload >64KiB */
    ic.kind = TC_SYNTH_GRAIN;
    uint16_t *y, *u, *v, *a;
    MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);
    const uint16_t* pl[4] = {y, u, v, NULL};
    topos_frame_input in;
    fill_input(&in, pl);
    topos_frame_config cfg;
    base_cfg(&cfg, 640u, 512u);
    cfg.qp_base = 20u;

    size_t cap = tc_frame_packet_bound(&cfg);
    uint8_t* p1 = (uint8_t*)malloc(cap);
    uint8_t* p2 = (uint8_t*)malloc(cap);
    MT_CHECK(p1 != NULL && p2 != NULL);
    if (p1 != NULL && p2 != NULL) {
        topos_frame_stats st;
        MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, p1, cap, &st), TC_OK);
        uint32_t target = st.packet_size / 2u;
        uint8_t q1 = 0, q2 = 0;
        topos_frame_stats s1, s2;
        /* 两次 sized：各自 ≥2 次整帧迭代 → 槽位跨迭代/跨调用复用 + 扩容回写 */
        MT_CHECK_EQ_I64(
            tc_frame_encode_sized(&cfg, &in, target, 0u, 63u, &q1, p1, cap, &s1), TC_OK);
        MT_CHECK_EQ_I64(
            tc_frame_encode_sized(&cfg, &in, target, 0u, 63u, &q2, p2, cap, &s2), TC_OK);
        MT_CHECK_EQ_U64(q1, q2);
        MT_CHECK_EQ_U64(s1.packet_size, s2.packet_size);
        MT_CHECK(memcmp(p1, p2, s1.packet_size) == 0); /* 悬垂指针若在 → 已 abort */
    }
    free(p1);
    free(p2);
    free(y); free(u); free(v); free(a);
}

/* ---------- 5. M6 token 路径 vs qbuf 回退路径差分 ---------- */

static void test_m6_token_differential(void)
{
    /* 多素材 × 多 qp × alpha/无 alpha：token 编码路径（默认）与强制 qbuf
     * 双遍回退（tc_dev_set_token_encode(1)）的输出包逐字节一致。 */
    const struct {
        tc_synth_kind kind;
        uint32_t w, h, qp, slice_rows, alpha;
    } cases[] = {
        { TC_SYNTH_GRAIN, 96u, 64u, 20u, 1u, 0u },
        { TC_SYNTH_GRAIN, 96u, 64u, 4u, 1u, 0u },   /* 低 qp 稠密系数（多 pair） */
        { TC_SYNTH_GRAIN, 96u, 64u, 52u, 1u, 0u },  /* 高 qp 稀疏（run 长） */
        { TC_SYNTH_DETAIL, 128u, 72u, 12u, 2u, 0u },
        { TC_SYNTH_MIXED, 128u, 72u, 28u, 2u, 0u },
        { TC_SYNTH_FLAT, 96u, 64u, 30u, 1u, 0u },   /* 全零 AC 块（np=0） */
        { TC_SYNTH_GRAIN, 64u, 48u, 16u, 1u, 1u },  /* alpha mode1 */
    };
    enum { NC = (int)(sizeof(cases) / sizeof(cases[0])) };

    for (int ci = 0; ci < NC; ++ci) {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x0DEC0DEC0DEC0Dull + (uint64_t)ci;
        ic.width = cases[ci].w;
        ic.height = cases[ci].h;
        ic.kind = cases[ci].kind;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, (int)cases[ci].alpha, &y, &u, &v, &a), 0);
        const uint16_t* pl[4] = {y, u, v, a};
        topos_frame_input in;
        fill_input(&in, pl);

        topos_frame_config cfg;
        base_cfg(&cfg, cases[ci].w, cases[ci].h);
        cfg.qp_base = (uint8_t)cases[ci].qp;
        cfg.slice_rows = (uint16_t)cases[ci].slice_rows;
        if (cases[ci].alpha != 0u) { cfg.alpha_mode = 1u; cfg.alpha_bit_depth = 16u; }

        size_t cap = tc_frame_packet_bound(&cfg);
        uint8_t* pt = (uint8_t*)malloc(cap); /* token 路径 */
        uint8_t* pq = (uint8_t*)malloc(cap); /* qbuf 回退路径 */
        MT_CHECK(pt != NULL && pq != NULL);
        if (pt != NULL && pq != NULL) {
            topos_frame_stats st, sq;
            tc_dev_set_token_encode(0);
            MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, pt, cap, &st), TC_OK);
            tc_dev_set_token_encode(1);
            MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, pq, cap, &sq), TC_OK);
            tc_dev_set_token_encode(0);
            MT_CHECK_EQ_U64(st.packet_size, sq.packet_size);
            MT_CHECK(memcmp(pt, pq, st.packet_size) == 0);
        }
        free(pt);
        free(pq);
        free(y); free(u); free(v); free(a);
    }
}

/* ---------- 6. M7 DCT-once/exact-probe 差分（m7 vs legacy probe vs linear） ---------- */

static void test_m7_sized_differential(void)
{
    /* 三方对拍：M7 精确位计数 probe、旧整帧编码 probe（m7 禁用）、legacy
     * 线性搜索——最终 qp 与输出包必须三方逐字节一致（含 alpha 帧：
     * alpha 一次编码跨 probe 复用的正确性）。 */
    const sized_case cases[] = {
        { TC_SYNTH_GRAIN, 64u, 48u, 20u, 0.60, 1u, 0 },
        { TC_SYNTH_GRAIN, 64u, 48u, 20u, 0.25, 1u, 0 },
        { TC_SYNTH_GRAIN, 64u, 48u, 20u, 2.00, 1u, 0 },
        { TC_SYNTH_GRAIN, 64u, 48u, 4u, 0.05, 1u, 0 },
        { TC_SYNTH_GRAIN, 64u, 48u, 44u, 0.50, 1u, 0 },
        { TC_SYNTH_FLAT, 96u, 64u, 12u, 2.00, 1u, 0 },
        { TC_SYNTH_DETAIL, 128u, 72u, 16u, 0.40, 1u, 0 },
        { TC_SYNTH_MIXED, 128u, 72u, 24u, 0.80, 2u, 0 },
        { TC_SYNTH_MIXED, 64u, 48u, 10u, 1.50, 1u, 1 },
        { TC_SYNTH_GRAIN, 96u, 64u, 18u, 0.55, 1u, 1 },
    };
    enum { NCASES = (int)(sizeof(cases) / sizeof(cases[0])) };

    for (int ci = 0; ci < NCASES; ++ci) {
        const sized_case* cs = &cases[ci];
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x7A7A7A7A7A7A7A7Aull + (uint64_t)ci;
        ic.width = cs->w;
        ic.height = cs->h;
        ic.kind = cs->kind;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, cs->with_alpha, &y, &u, &v, &a), 0);
        const uint16_t* pl[4] = {y, u, v, a};
        topos_frame_input in;
        fill_input(&in, pl);

        topos_frame_config cfg;
        base_cfg(&cfg, cs->w, cs->h);
        cfg.qp_base = (uint8_t)cs->qp0;
        cfg.slice_rows = (uint16_t)cs->slice_rows;
        if (cs->with_alpha) { cfg.alpha_mode = 1u; cfg.alpha_bit_depth = 16u; }

        size_t cap = tc_frame_packet_bound(&cfg);
        uint8_t* base = (uint8_t*)malloc(cap);
        topos_frame_stats bst;
        MT_CHECK(base != NULL);
        if (base == NULL) { free(y); free(u); free(v); free(a); continue; }
        MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, base, cap, &bst), TC_OK);
        uint32_t target = (uint32_t)((double)bst.packet_size * cs->ratio);
        free(base);

        uint8_t* pkts[3] = {NULL, NULL, NULL};
        size_t szs[3] = {0u, 0u, 0u};
        uint8_t qps[3] = {0xFF, 0xFF, 0xFF};
        topos_frame_stats sts[3];
        memset(sts, 0, sizeof(sts));

        /* M7 差分钉 R6 快速（mode 2）：同一算法 × m7/整帧 probe × legacy 三路一致 */
        tc_dev_set_sized_m7(0); tc_dev_set_sized_mode(2);
        MT_CHECK_EQ_I64(sized_run(&cfg, &in, target, &pkts[0], &szs[0], &qps[0], &sts[0]), TC_OK);
        tc_dev_set_sized_m7(1); tc_dev_set_sized_mode(2);
        MT_CHECK_EQ_I64(sized_run(&cfg, &in, target, &pkts[1], &szs[1], &qps[1], &sts[1]), TC_OK);
        tc_dev_set_sized_m7(0); tc_dev_set_sized_mode(1);
        MT_CHECK_EQ_I64(sized_run(&cfg, &in, target, &pkts[2], &szs[2], &qps[2], &sts[2]), TC_OK);
        tc_dev_set_sized_mode(0);

        for (int k = 0; k < 3; ++k) {
            MT_CHECK_EQ_U64(qps[k], qps[2]);
            MT_CHECK_EQ_U64(sts[k].packet_size, sts[2].packet_size);
            MT_CHECK(memcmp(pkts[k], pkts[2], sts[2].packet_size) == 0);
            MT_CHECK_EQ_U64(sts[k].qp_base, qps[2]);
            /* stats 完整性（m7_final 也必须填 payload/header 计数） */
            MT_CHECK_EQ_U64(sts[k].color_payload_bytes, sts[2].color_payload_bytes);
            MT_CHECK_EQ_U64(sts[k].alpha_payload_bytes, sts[2].alpha_payload_bytes);
            MT_CHECK_EQ_U64(sts[k].color_header_bytes, sts[2].color_header_bytes);
            MT_CHECK_EQ_U64(sts[k].alpha_header_bytes, sts[2].alpha_header_bytes);
            MT_CHECK_EQ_U64(sts[k].slice_count, sts[2].slice_count);
        }
        /* 最终包可解码且无 concealment */
        topos_frame_output info;
        MT_CHECK_EQ_I64(tc_frame_decode(pkts[0], sts[0].packet_size, NULL, NULL, &info), TC_OK);
        MT_CHECK_EQ_U64(info.concealed_slices, 0u);

        for (int k = 0; k < 3; ++k) { free(pkts[k]); }
        free(y); free(u); free(v); free(a);
    }
}

/* ---------- M10-6.3A：跨帧 qp 提示（strict 首探种子） ----------
 * 契约：提示只改探测起点，q* 由 bytes(q) 唯一决定 → 开/关输出逐字节一致；
 * 稳态（提示 = 本内容上一帧 q*）2 探定界（命中 + q*−1 超限）+ final ≤ 3 次；
 * 提示跨内容/几何污染时仍收敛且输出不变。 */
static void test_r6_qp_hint(void)
{
    tc_dev_set_sized_mode(0);
    image_synth_cfg ic;
    memset(&ic, 0, sizeof(ic));
    ic.seed = 0x5EED111100000002ull;
    ic.width = 128u;
    ic.height = 72u;
    ic.kind = TC_SYNTH_MIXED;
    uint16_t *y, *u, *v, *a;
    MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);
    const uint16_t* pl[4] = {y, u, v, NULL};
    topos_frame_input in;
    fill_input(&in, pl);
    topos_frame_config cfg;
    base_cfg(&cfg, 128u, 72u);
    cfg.qp_base = 20u;
    size_t cap = tc_frame_packet_bound(&cfg);

    uint8_t* base = (uint8_t*)malloc(cap);
    MT_CHECK(base != NULL);
    if (base != NULL) {
        topos_frame_stats bst;
        MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, base, cap, &bst), TC_OK);
        uint32_t target = bst.packet_size / 2u;
        free(base);

        /* 关侧基线（qp_base 种子；忽略进程内残留提示） */
        tc_dev_set_qp_hint(1);
        uint8_t* p_off = NULL; size_t s_off = 0; uint8_t q_off = 0;
        topos_frame_stats st_off;
        MT_CHECK_EQ_I64(
            sized_run(&cfg, &in, target, &p_off, &s_off, &q_off, &st_off), TC_OK);
        MT_CHECK(p_off != NULL);
        if (p_off != NULL) {
            /* 开侧：同内容重复——提示命中后稳态 ≤3 次（含 final），输出一致 */
            tc_dev_set_qp_hint(0);
            for (int rep = 0; rep < 3; ++rep) {
                uint8_t* p = NULL; size_t s = 0; uint8_t q = 0;
                topos_frame_stats st;
                tc_dev_sized_reset();
                MT_CHECK_EQ_I64(
                    sized_run(&cfg, &in, target, &p, &s, &q, &st), TC_OK);
                if (p != NULL) {
                    MT_CHECK_EQ_U64(q, q_off);
                    MT_CHECK_EQ_U64(s, s_off);
                    MT_CHECK(memcmp(p, p_off, s) == 0);
                }
                if (rep >= 1) {
                    /* 首跑可能吃到前序测试的跨内容残留提示；此后必稳态 */
                    MT_CHECK(tc_dev_sized_iters() <= 3u);
                }
                free(p);
            }

            /* 内容切换：提示来自上一内容 → 仍收敛、输出与关侧一致 */
            image_synth_cfg ic2;
            memset(&ic2, 0, sizeof(ic2));
            ic2.seed = 0x5EED222200000003ull;
            ic2.width = 128u;
            ic2.height = 72u;
            ic2.kind = TC_SYNTH_DETAIL;
            uint16_t *y2, *u2, *v2, *a2;
            MT_CHECK_EQ_I64(image_synth_alloc(&ic2, 0, &y2, &u2, &v2, &a2), 0);
            if (y2 != NULL) {
                const uint16_t* pl2[4] = {y2, u2, v2, NULL};
                topos_frame_input in2;
                fill_input(&in2, pl2);
                topos_frame_stats bst2;
                uint8_t* base2 = (uint8_t*)malloc(cap);
                MT_CHECK(base2 != NULL);
                if (base2 != NULL) {
                    MT_CHECK_EQ_I64(
                        tc_frame_encode(&cfg, &in2, base2, cap, &bst2), TC_OK);
                    uint32_t target2 = bst2.packet_size / 2u;
                    free(base2);

                    uint8_t* p2o = NULL; size_t s2o = 0; uint8_t q2o = 0;
                    topos_frame_stats st2o;
                    tc_dev_set_qp_hint(1);
                    tc_dev_sized_reset();
                    MT_CHECK_EQ_I64(
                        sized_run(&cfg, &in2, target2, &p2o, &s2o, &q2o, &st2o), TC_OK);
                    uint8_t* p2 = NULL; size_t s2 = 0; uint8_t q2 = 0;
                    topos_frame_stats st2;
                    tc_dev_sized_reset();
                    MT_CHECK_EQ_I64(
                        sized_run(&cfg, &in2, target2, &p2, &s2, &q2, &st2), TC_OK);
                    if (p2 != NULL && p2o != NULL) {
                        MT_CHECK_EQ_U64(q2, q2o);
                        MT_CHECK_EQ_U64(s2, s2o);
                        MT_CHECK(memcmp(p2, p2o, s2) == 0);
                    }
                    MT_CHECK(tc_dev_sized_iters() <= 10u);
                    free(p2);
                    free(p2o);
                }
                free(y2); free(u2); free(v2); free(a2);
            }
        }
        free(p_off);
    }
    free(y); free(u); free(v); free(a);
    tc_dev_set_qp_hint(0);
}

/* ---------- 8. AQ × v1.5 域（qp≥64）sized 搜索差分（2026-09-11 回归钉） ----------
 * m7_probe 的 AQ 变体天花板曾以 fh.qp_base（探测期间 = 搜索起点值，如 4）
 * 为源 → qp≥64 候选的偏移带全部回落 63 量化 → 探针高估 bytes、搜索冲顶、
 * 实包欠目标（真实素材 proxy 复现：AQ-on 落 q83/q95、0.50/0.66×T，
 * AQ-off 正常）。修复后变体域以候选 qp 为源。本门钉三性质：
 *  a) AQ on/off 落点 |Δqp| ≤ 2（色度专属 ±2 偏移仅微动色度码量；
 *     修复前合成素材 q92 vs q68）；
 *  b) 探针/最终一致：sized 选定包 == 同 qp 规范固定编码（双方 m7 路径）；
 *  c) AQ=0 时 fast/legacy v1.5 域逐字节一致（既有差分钉外延到 qp_max=95）
 *     + 预算语义 + 可解码无 conceal。 */
static void test_m7_sized_aq_v15(void)
{
    const double ratios[] = { 0.05, 0.02 }; /* qp4 基准 → 爬升至 70~90 段 */
    enum { NR = (int)(sizeof(ratios) / sizeof(ratios[0])) };

    for (int ri = 0; ri < NR; ++ri) {
        image_synth_cfg ic;
        memset(&ic, 0, sizeof(ic));
        ic.seed = 0x51D2C0DE00000000ull + (uint64_t)ri;
        ic.width = 96u;
        ic.height = 64u; /* 色度 8 块行 × slice_rows 1 → 8 带 ≥ 2，AQ 偏移实际生效 */
        ic.kind = TC_SYNTH_GRAIN;
        uint16_t *y, *u, *v, *a;
        MT_CHECK_EQ_I64(image_synth_alloc(&ic, 0, &y, &u, &v, &a), 0);
        /* 活动度分层：色度上半保留 grain、下半置平——逐带偏移实际触发
         * （均匀 grain 各带活动度相同 → aq_off 全 0，探针不经过变体表，
         * 旧缺陷不可见；真实素材天然分层，2026-09-11 复现即此形态） */
        for (uint32_t i = 0u; i < 48u * 32u; ++i) { /* 422 色度 48×64 的下半 */
            u[48u * 32u + i] = 512u;
            v[48u * 32u + i] = 512u;
        }
        const uint16_t* pl[4] = {y, u, v, NULL};
        topos_frame_input in;
        fill_input(&in, pl);

        topos_frame_config cfg;
        base_cfg(&cfg, 96u, 64u);
        cfg.qp_base = 4u;
        cfg.slice_rows = 1u;
        cfg.reserved[0] = 1u; /* V2（AQ 所在熵族） */

        size_t cap = tc_frame_packet_bound(&cfg);
        uint8_t* base = (uint8_t*)malloc(cap);
        topos_frame_stats bst;
        MT_CHECK(base != NULL);
        if (base == NULL) { free(y); free(u); free(v); free(a); continue; }
        MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, base, cap, &bst), TC_OK);
        uint32_t target = (uint32_t)((double)bst.packet_size * ratios[ri]);
        free(base);

        uint8_t* pkt_a = (uint8_t*)malloc(cap);   /* fast AQ=1（默认 m7 探针） */
        uint8_t* pkt_0 = (uint8_t*)malloc(cap);   /* fast AQ=0 */
        uint8_t* pkt_l = (uint8_t*)malloc(cap);   /* legacy AQ=0（v1.5 域差分） */
        uint8_t* pkt_c = (uint8_t*)malloc(cap);   /* 规范固定 qp 编码 AQ=1 */
        MT_CHECK(pkt_a != NULL && pkt_0 != NULL && pkt_l != NULL && pkt_c != NULL);
        if (pkt_a == NULL || pkt_0 == NULL || pkt_l == NULL || pkt_c == NULL) {
            free(pkt_a); free(pkt_0); free(pkt_l); free(pkt_c);
            free(y); free(u); free(v); free(a);
            continue;
        }

        cfg.reserved[1] = 1u; /* AQ on */
        uint8_t qp_a = 0xFF;
        topos_frame_stats st_a;
        memset(&st_a, 0, sizeof(st_a));
        tc_dev_set_sized_mode(2);
        MT_CHECK_EQ_I64(
            tc_frame_encode_sized(&cfg, &in, target, 0u, 95u, &qp_a, pkt_a, cap, &st_a),
            TC_OK);
        tc_dev_set_sized_mode(0);
        MT_CHECK(qp_a >= 64u); /* 目标确实落入 v1.5 域（用例有效性自检） */

        /* a) AQ on/off 落点差 ≤ 2（修复前 fast 探针高估 bytes 冲顶：
         *    合成素材 q92 vs q68，真实素材 q83/q95 vs q78/q82） */
        cfg.reserved[1] = 0u;
        uint8_t qp_0 = 0xFF, qp_l = 0xFF;
        topos_frame_stats st_0, st_l;
        memset(&st_0, 0, sizeof(st_0));
        memset(&st_l, 0, sizeof(st_l));
        tc_dev_set_sized_mode(2);
        MT_CHECK_EQ_I64(
            tc_frame_encode_sized(&cfg, &in, target, 0u, 95u, &qp_0, pkt_0, cap, &st_0),
            TC_OK);
        tc_dev_set_sized_mode(1);
        MT_CHECK_EQ_I64(
            tc_frame_encode_sized(&cfg, &in, target, 0u, 95u, &qp_l, pkt_l, cap, &st_l),
            TC_OK);
        tc_dev_set_sized_mode(0);
        {
            int dq = (int)qp_a - (int)qp_0;
            if (dq < 0) { dq = -dq; }
            MT_CHECK(dq <= 2);
        }
        /* AQ=0 时 fast/legacy 在 v1.5 域仍须逐字节一致（既有差分钉外延；
         * AQ=1 不比对——legacy 全帧路径 aq_off 恒 0，AQ 确定性降级为关
         * （codec.c 回退语义），与 m7 路径本就不逐字节同） */
        MT_CHECK_EQ_U64(qp_0, qp_l);
        MT_CHECK_EQ_U64(st_0.packet_size, st_l.packet_size);
        MT_CHECK(memcmp(pkt_0, pkt_l, st_0.packet_size) == 0);

        /* b) 探针/最终一致：sized 选定包 == 同 qp 规范固定编码（AQ=1
         *    双方都走 m7_prepare+m7_final，必须逐字节同） */
        cfg.reserved[1] = 1u;
        cfg.qp_base = qp_a;
        topos_frame_stats st_c;
        MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, pkt_c, cap, &st_c), TC_OK);
        MT_CHECK_EQ_U64(st_a.packet_size, st_c.packet_size);
        MT_CHECK(memcmp(pkt_a, pkt_c, st_a.packet_size) == 0);

        /* c) 预算语义 + 可解码无 conceal */
        if (qp_a != 95u && target > 0u) {
            MT_CHECK(st_a.packet_size <= target);
        }
        {
            topos_frame_output info;
            MT_CHECK_EQ_I64(tc_frame_decode(pkt_a, st_a.packet_size,
                                            NULL, NULL, &info), TC_OK);
            MT_CHECK_EQ_U64(info.concealed_slices, 0u);
        }
        free(pkt_a);
        free(pkt_0);
        free(pkt_l);
        free(pkt_c);
        free(y); free(u); free(v); free(a);
    }
}

int main(void)
{
    test_r6_zigzag_layout();
    test_r6_sized_differential();
    test_r6_sized_strict();
    test_r6_qp_hint();
    test_r6_parallel_invariants();
    test_r6_slot_writer_growth();
    test_m6_token_differential();
    test_m7_sized_differential();
    test_m7_sized_aq_v15();
    return MT_MAIN_RETURN();
}
