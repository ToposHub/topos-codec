/* M10-1 验收测试：稳态零分配 + 池几何增长 + 扩容故障回滚 + 池化/临时双路径差分。
 *
 *  1. 稳定几何下 tc_decoder 连续解码 3000 帧：池预热后分配计数必须为 0
 *     （颜色 DC 行 + alpha 行 + 线程池批次全部 grow-only/内嵌）；
 *  2. 小几何 → 大几何（alpha + 颜色）→ 小几何：扩容后输出仍与无状态
 *     参考逐位一致；回退小几何沿用大池不重新分配；
 *  3. 扩容路径 OOM 注入扫描：任一分配点失败 → 旧池保留、slice 临时分配
 *     兜底（像素仍正确）或整帧 OOM（slice 临时分配也失败时）；故障解除
 *     后扩容成功、稳态回到零分配（不再永久退化——M10-1e 回归的根）；
 *  4. decoder 销毁（重负载后）无崩溃无泄漏（ASan 配置复跑承担泄漏门）。
 */
#include "codec/codec.h"
#include "common/alloc.h"
#include "common/tpool.h"
#include "image_synth.h"
#include "mini_test.h"
#include "topos_codec.h"

#include "../support/port_thread.h"
#include <stdlib.h>
#include <string.h>

static void base_cfg(topos_frame_config* c, uint32_t w, uint32_t h, int with_alpha)
{
    memset(c, 0, sizeof(*c));
    c->struct_size = (uint32_t)sizeof(*c);
    c->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    c->visible_width = (uint16_t)w;
    c->visible_height = (uint16_t)h;
    c->qmatrix_id = 1u;
    if (with_alpha) { c->alpha_mode = 1u; c->alpha_bit_depth = 16u; }
}

/* 合成 → 编码 → 无状态参考平面 + context 输出平面/视图 */
typedef struct m10_blob {
    uint8_t* pkt;
    size_t size;
    uint16_t* ref[4];    /* 无状态参考 */
    uint16_t* out[4];    /* context 输出 */
    topos_plane_view views[TC_FRAME_MAX_PLANES];
    uint32_t plane_w[4];
    uint32_t plane_h[4];
    uint32_t plane_count;
} m10_blob;

static void blob_free(m10_blob* b)
{
    free(b->pkt);
    for (int p = 0; p < 4; ++p) { free(b->ref[p]); free(b->out[p]); }
    free(b);
}

static m10_blob* blob_make(uint32_t w, uint32_t h, int with_alpha, uint64_t seed, uint8_t qp)
{
    m10_blob* b = (m10_blob*)calloc(1, sizeof(*b));
    if (b == NULL) { return NULL; }
    image_synth_cfg ic;
    memset(&ic, 0, sizeof(ic));
    ic.seed = seed;
    ic.width = w;
    ic.height = h;
    ic.kind = TC_SYNTH_MIXED;
    uint16_t *y, *u, *v, *a;
    if (image_synth_alloc(&ic, with_alpha, &y, &u, &v, &a) != 0) { free(b); return NULL; }
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(topos_frame_input);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    in.planes[0] = y;
    in.planes[1] = u;
    in.planes[2] = v;
    in.planes[3] = a;

    topos_frame_config cfg;
    base_cfg(&cfg, w, h, with_alpha);
    cfg.qp_base = qp;
    size_t cap = tc_frame_packet_bound(&cfg);
    b->pkt = (uint8_t*)malloc(cap);
    topos_frame_stats st;
    int32_t rc = TC_ERR_OUT_OF_MEMORY;
    if (b->pkt != NULL) {
        rc = tc_frame_encode(&cfg, &in, b->pkt, cap, &st);
    }
    free(y); free(u); free(v); free(a);
    if (rc != TC_OK) { blob_free(b); return NULL; }
    b->size = st.packet_size;

    topos_frame_output info;
    rc = tc_frame_decode(b->pkt, b->size, NULL, NULL, &info);
    if (rc != TC_OK) { blob_free(b); return NULL; }
    b->plane_count = info.plane_count;
    for (uint32_t p = 0u; p < info.plane_count; ++p) {
        uint32_t pw = 0u, ph = 0u;
        if (tc_frame_plane_geometry(&info, p, &pw, &ph) != TC_OK) { blob_free(b); return NULL; }
        b->plane_w[p] = pw;
        b->plane_h[p] = ph;
        size_t n = (size_t)pw * (size_t)ph;
        b->ref[p] = (uint16_t*)malloc(n * sizeof(uint16_t));
        b->out[p] = (uint16_t*)malloc(n * sizeof(uint16_t));
        if (b->ref[p] == NULL || b->out[p] == NULL) { blob_free(b); return NULL; }
        b->views[p].struct_size = (uint32_t)sizeof(topos_plane_view);
        b->views[p].abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        b->views[p].pixels = b->out[p];
        b->views[p].stride = 0u;
    }
    rc = tc_frame_decode(b->pkt, b->size, b->ref, NULL, &info);
    if (rc != TC_OK) { blob_free(b); return NULL; }
    return b;
}

static int blob_check(const m10_blob* b, const topos_frame_output* info)
{
    if (info->concealed_slices != 0u) { return 0; }
    for (uint32_t p = 0u; p < b->plane_count; ++p) {
        if (memcmp(b->out[p], b->ref[p],
                   (size_t)b->plane_w[p] * b->plane_h[p] * sizeof(uint16_t)) != 0) {
            return 0;
        }
    }
    return 1;
}

/* ---------- 1. 稳定几何 3000 帧零分配（1 线程 + 8 线程） ---------- */

static void zero_alloc_steady(int threads)
{
    tc_dev_set_thread_count(threads);
    m10_blob* b = blob_make(128u, 96u, 1, 0x51CE6001ull, 24u);
    MT_CHECK(b != NULL);
    if (b == NULL) { tc_dev_set_thread_count(0); return; }

    tc_decoder* dec = NULL;
    MT_CHECK_EQ_I64(tc_decoder_create(NULL, &dec), TC_OK);
    topos_frame_output info;

    /* 预热两帧：池按首帧几何扩容到位（8 线程档同时长满池槽/线程池） */
    for (int i = 0; i < 2; ++i) {
        MT_CHECK_EQ_I64(tc_decoder_decode(dec, b->pkt, b->size, b->views, &info), TC_OK);
        MT_CHECK(blob_check(b, &info));
    }

    /* 验收门：稳定几何 3000 帧分配计数 == 0（含 alpha 帧，覆盖 DC 行池 +
     * alpha 行池 + 线程池批次内嵌——8 线程档同时验证 ring entry 复用） */
    tc_dev_set_alloc_fault(-1); /* 关故障并清零计数 */
    for (int i = 0; i < 3000; ++i) {
        int32_t rc = tc_decoder_decode(dec, b->pkt, b->size, b->views, &info);
        if (rc != TC_OK || !blob_check(b, &info)) {
            mt_report(__FILE__, __LINE__, "steady decode failed");
            break;
        }
    }
    MT_CHECK_EQ_I64(tc_dev_alloc_count(), 0);

    /* RD3-05：context 批量入口的 view/request 描述也必须进入 grow-only
     * arena；预热后连续批量解码不能因为每次临时数组而分配。 */
    topos_batch_packet batch_packet = {b->pkt, b->size};
    for (int i = 0; i < 2; ++i) {
        MT_CHECK_EQ_I64(tc_decoder_decode_batch(dec, &batch_packet, 1u,
                                                b->views, &info), TC_OK);
        MT_CHECK(blob_check(b, &info));
    }
    tc_dev_set_alloc_fault(-1);
    for (int i = 0; i < 1000; ++i) {
        MT_CHECK_EQ_I64(tc_decoder_decode_batch(dec, &batch_packet, 1u,
                                                b->views, &info), TC_OK);
    }
    MT_CHECK_EQ_I64(tc_dev_alloc_count(), 0);

    tc_decoder_destroy(dec);
    blob_free(b);
    tc_dev_set_thread_count(0);
}

static void test_m10_zero_alloc_steady(void)
{
    zero_alloc_steady(1);
    zero_alloc_steady(8);
}

/* ---------- 2/3. 几何增长 + OOM 扩容扫描 + 回滚 ---------- */

static void test_m10_geometry_growth_and_oom(void)
{
    tc_dev_set_thread_count(1);
    m10_blob* small_b = blob_make(64u, 48u, 1, 0x51CE6002ull, 20u);
    m10_blob* big_b = blob_make(192u, 128u, 1, 0x51CE6003ull, 20u);
    MT_CHECK(small_b != NULL && big_b != NULL);
    if (small_b == NULL || big_b == NULL) {
        blob_free(small_b); blob_free(big_b);
        tc_dev_set_thread_count(0);
        return;
    }

    tc_decoder* dec = NULL;
    MT_CHECK_EQ_I64(tc_decoder_create(NULL, &dec), TC_OK);
    topos_frame_output info;

    /* 小几何建立池 → 大几何扩容（alpha coded 宽 64→192、DC cols 增） */
    MT_CHECK_EQ_I64(tc_decoder_decode(dec, small_b->pkt, small_b->size, small_b->views, &info), TC_OK);
    MT_CHECK(blob_check(small_b, &info));
    MT_CHECK_EQ_I64(tc_decoder_decode(dec, big_b->pkt, big_b->size, big_b->views, &info), TC_OK);
    MT_CHECK(blob_check(big_b, &info));
    /* 回退小几何：沿用大池，输出仍逐位一致 */
    MT_CHECK_EQ_I64(tc_decoder_decode(dec, small_b->pkt, small_b->size, small_b->views, &info), TC_OK);
    MT_CHECK(blob_check(small_b, &info));

    /* OOM 扫描：全新 decoder，先建小池，再对「扩容解码」逐点注入。
     * 任一分配点失败：旧池保留 + slice 临时分配兜底 → 成功且像素正确；
     * 或 slice 临时分配也失败 → TC_ERR_OUT_OF_MEMORY（不允许其他码）。
     * 注：故障恰好命中一次——被扩容消耗后临时分配必然成功，故本场景
     * 只覆盖「兜底成功」类；「临时分配也失败 → 整帧 OOM」由 test_oom 的
     * 无状态全路径扫描覆盖（无状态路径恒走临时分配）。 */
    tc_decoder_destroy(dec);
    dec = NULL;
    MT_CHECK_EQ_I64(tc_decoder_create(NULL, &dec), TC_OK);
    MT_CHECK_EQ_I64(tc_decoder_decode(dec, small_b->pkt, small_b->size, small_b->views, &info), TC_OK);

    for (int64_t n = 1; n <= 16; ++n) {
        tc_dev_set_alloc_fault(n);
        int32_t rc = tc_decoder_decode(dec, big_b->pkt, big_b->size, big_b->views, &info);
        tc_dev_set_alloc_fault(-1);
        if (rc == TC_OK) {
            /* 池扩容失败但 slice 临时分配兜底成功：像素必须正确（回滚语义） */
            MT_CHECK(blob_check(big_b, &info));
        } else {
            MT_CHECK_EQ_I64(rc, TC_ERR_OUT_OF_MEMORY);
        }
    }

    /* 故障解除后：扩容完成 → 稳态零分配恢复（旧实现在此永久退化） */
    MT_CHECK_EQ_I64(tc_decoder_decode(dec, big_b->pkt, big_b->size, big_b->views, &info), TC_OK);
    tc_dev_set_alloc_fault(-1);
    for (int i = 0; i < 500; ++i) {
        MT_CHECK_EQ_I64(tc_decoder_decode(dec, big_b->pkt, big_b->size, big_b->views, &info), TC_OK);
    }
    MT_CHECK_EQ_I64(tc_dev_alloc_count(), 0);

    /* 混合序列：大小交替 × 200（增长后不再分配 + 输出始终正确） */
    tc_dev_set_alloc_fault(-1);
    for (int i = 0; i < 200; ++i) {
        m10_blob* cur = (i & 1) != 0 ? small_b : big_b;
        MT_CHECK_EQ_I64(tc_decoder_decode(dec, cur->pkt, cur->size, cur->views, &info), TC_OK);
        MT_CHECK(blob_check(cur, &info));
    }
    MT_CHECK_EQ_I64(tc_dev_alloc_count(), 0);

    tc_decoder_destroy(dec);
    blob_free(small_b);
    blob_free(big_b);
    tc_dev_set_thread_count(0);
}

/* ---------- 4. 多线程池化路径差分（worker 槽位绑定正确性） ---------- */

static void test_m10_multithread_parity(void)
{
    m10_blob* b = blob_make(256u, 160u, 1, 0x51CE6004ull, 18u);
    MT_CHECK(b != NULL);
    if (b == NULL) { return; }

    /* 8 线程下池槽位跨 slice 复用：输出必须与无状态参考逐位一致
     * （worker 槽位绑定错误会在 DC 行残留处产生确定性偏差） */
    tc_dev_set_thread_count(8);
    for (int round = 0; round < 50; ++round) {
        tc_decoder* dec = NULL;
        MT_CHECK_EQ_I64(tc_decoder_create(NULL, &dec), TC_OK);
        topos_frame_output info;
        for (int rep = 0; rep < 4; ++rep) {
            MT_CHECK_EQ_I64(tc_decoder_decode(dec, b->pkt, b->size, b->views, &info), TC_OK);
            MT_CHECK(blob_check(b, &info));
        }
        tc_decoder_destroy(dec);
    }
    blob_free(b);
    tc_dev_set_thread_count(0);
}

/* ---------- 5. M10-2A：整帧 direct（默认）vs generic（dev 强制）差分 ---------- */

static void direct_parity_case(uint32_t w, uint32_t h, int with_alpha, uint8_t qp)
{
    m10_blob* b = blob_make(w, h, with_alpha, 0x51CE6005ull + w + h, qp);
    MT_CHECK(b != NULL);
    if (b == NULL) { return; }

    topos_frame_output info;
    /* direct（生产默认） */
    tc_dev_set_direct_scan(0);
    MT_CHECK_EQ_I64(tc_frame_decode(b->pkt, b->size, b->out, NULL, &info), TC_OK);
    MT_CHECK(blob_check(b, &info));
    /* 强制 generic sink 路径 → 与 direct 输出必须逐位一致（out 已是 direct
     * 结果，重解进 ref-比较：先存 direct 快照） */
    static uint16_t* snap[4];
    static int snap_inited = 0;
    if (!snap_inited) {
        for (int p = 0; p < 4; ++p) { snap[p] = (uint16_t*)malloc(3840u * 2160u * 2u + 4096u); }
        snap_inited = 1;
    }
    for (uint32_t p = 0u; p < b->plane_count; ++p) {
        memcpy(snap[p], b->out[p],
               (size_t)b->plane_w[p] * b->plane_h[p] * sizeof(uint16_t));
    }
    tc_dev_set_direct_scan(1);
    MT_CHECK_EQ_I64(tc_frame_decode(b->pkt, b->size, b->out, NULL, &info), TC_OK);
    tc_dev_set_direct_scan(0);
    int same = 1;
    for (uint32_t p = 0u; p < b->plane_count; ++p) {
        if (memcmp(snap[p], b->out[p],
                   (size_t)b->plane_w[p] * b->plane_h[p] * sizeof(uint16_t)) != 0) {
            same = 0;
        }
    }
    MT_CHECK(same != 0);

    /* M10-2B：稀疏重建路径（阈值强制开）与稠密输出必须逐位一致 */
    tc_dev_set_sparse_threshold(16);
    MT_CHECK_EQ_I64(tc_frame_decode(b->pkt, b->size, b->out, NULL, &info), TC_OK);
    tc_dev_set_sparse_threshold(0);
    same = 1;
    for (uint32_t p = 0u; p < b->plane_count; ++p) {
        if (memcmp(snap[p], b->out[p],
                   (size_t)b->plane_w[p] * b->plane_h[p] * sizeof(uint16_t)) != 0) {
            same = 0;
        }
    }
    MT_CHECK(same != 0);

    /* M10-2C：四块批量 IDCT 禁用路径与默认（批量开）逐位一致 */
    tc_dev_set_batch_idct(1);
    MT_CHECK_EQ_I64(tc_frame_decode(b->pkt, b->size, b->out, NULL, &info), TC_OK);
    tc_dev_set_batch_idct(0);
    same = 1;
    for (uint32_t p = 0u; p < b->plane_count; ++p) {
        if (memcmp(snap[p], b->out[p],
                   (size_t)b->plane_w[p] * b->plane_h[p] * sizeof(uint16_t)) != 0) {
            same = 0;
        }
    }
    MT_CHECK(same != 0);
    blob_free(b);
}

static void test_m10_direct_parity(void)
{
    /* 非 8 对齐边缘（63×45/127×93）+ alpha + 多 slice 行 */
    direct_parity_case(64u, 48u, 1, 22u);
    direct_parity_case(63u, 45u, 0, 30u);
    direct_parity_case(128u, 96u, 0, 12u);
    direct_parity_case(127u, 93u, 1, 18u);
}

/* ---------------- M10-6.1：编码持久 enc_shared 缓存 ---------------- */

typedef struct m10_enc_frame {
    topos_frame_input in;
    uint16_t* pl[4];
    topos_frame_config cfg;
    size_t cap;
} m10_enc_frame;

static void enc_frame_free(m10_enc_frame* f)
{
    for (int p = 0; p < 4; ++p) { free(f->pl[p]); }
}

static int enc_frame_make(m10_enc_frame* f, uint32_t w, uint32_t h, int with_alpha,
                          uint8_t qp, uint64_t seed)
{
    memset(f, 0, sizeof(*f));
    image_synth_cfg ic;
    memset(&ic, 0, sizeof(ic));
    ic.seed = seed;
    ic.width = w;
    ic.height = h;
    ic.kind = TC_SYNTH_MIXED;
    if (image_synth_alloc(&ic, with_alpha, &f->pl[0], &f->pl[1], &f->pl[2], &f->pl[3]) != 0) {
        return -1;
    }
    f->in.struct_size = (uint32_t)sizeof(topos_frame_input);
    f->in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    for (int p = 0; p < 4; ++p) { f->in.planes[p] = f->pl[p]; }
    base_cfg(&f->cfg, w, h, with_alpha);
    f->cfg.qp_base = qp;
    f->cap = tc_frame_packet_bound(&f->cfg);
    return 0;
}

/* 序列编码（plain + sized 交替）到 out；返回包大小（失败 0） */
static size_t enc_seq_one(const m10_enc_frame* f, int sized, uint32_t target,
                          uint8_t* out, uint8_t* qp_out)
{
    topos_frame_stats st;
    int32_t rc;
    if (sized != 0) {
        rc = tc_frame_encode_sized(&f->cfg, &f->in, target, 0u, 63u, qp_out,
                                   out, f->cap, &st);
    } else {
        rc = tc_frame_encode(&f->cfg, &f->in, out, f->cap, &st);
    }
    return rc == TC_OK ? st.packet_size : 0u;
}

/* 并发烟囱：两线程同帧编码——其一走缓存，另一走栈回退，均须与参考一致 */
typedef struct m10_enc_conc_job {
    const m10_enc_frame* f;
    const uint8_t* ref;
    size_t ref_size;
    int sized;
    uint32_t target;
    int ok;
} m10_enc_conc_job;

static void* m10_enc_conc_run(void* vp)
{
    m10_enc_conc_job* j = (m10_enc_conc_job*)vp;
    for (int i = 0; i < 50; ++i) {
        uint8_t* buf = (uint8_t*)malloc(j->f->cap);
        uint8_t qp = 0;
        size_t sz = enc_seq_one(j->f, j->sized, j->target, buf, &qp);
        if (sz != j->ref_size || memcmp(buf, j->ref, sz) != 0) { j->ok = 0; }
        free(buf);
    }
    return NULL;
}

static void test_m10_enc_cache(void)
{
    /* 1) 缓存开 vs 关：交替几何/模式序列输出逐字节一致 */
    m10_enc_frame fa, fb;
    MT_CHECK_EQ_I64(enc_frame_make(&fa, 96u, 64u, 0, 24u, 0xAA), 0);
    MT_CHECK_EQ_I64(enc_frame_make(&fb, 128u, 96u, 1, 30u, 0xBB), 0);
    {
        uint8_t* on[6] = {0};
        uint8_t* off[6] = {0};
        size_t szon[6], szoff[6];
        const m10_enc_frame* seq[6] = {&fa, &fb, &fa, &fb, &fa, &fb};
        const int smode[6] = {0, 1, 1, 0, 1, 1};
        int ok = 1;
        for (int pass = 0; pass < 2 && ok; ++pass) {
            tc_dev_set_enc_cache(pass); /* pass0: 关（栈实例）；pass1: 开 */
            for (int i = 0; i < 6 && ok; ++i) {
                uint8_t* buf = (uint8_t*)malloc(seq[i]->cap);
                uint8_t qp = 0;
                size_t sz = enc_seq_one(seq[i], smode[i],
                                        (uint32_t)(seq[i]->cap / 3u), buf, &qp);
                if (sz == 0u) { free(buf); ok = 0; break; }
                if (pass == 0) { off[i] = buf; szoff[i] = sz; }
                else { on[i] = buf; szon[i] = sz; }
            }
        }
        tc_dev_set_enc_cache(0);
        for (int i = 0; i < 6 && ok; ++i) {
            if (szon[i] != szoff[i] ||
                memcmp(on[i], off[i], szon[i]) != 0) { ok = 0; }
        }
        MT_CHECK(ok);
        for (int i = 0; i < 6; ++i) { free(on[i]); free(off[i]); }
    }

    /* 2) 稳态零分配：同几何 plain 连续编码，预热后分配计数为 0 */
    {
        tc_dev_alloc_count(); /* 读取器存在性 */
        for (int i = 0; i < 20; ++i) { /* 预热：缓存/槽位/arena 全部到位 */
            uint8_t* buf = (uint8_t*)malloc(fa.cap);
            uint8_t qp = 0;
            MT_CHECK(enc_seq_one(&fa, 0, 0u, buf, &qp) != 0u);
            free(buf);
        }
        uint64_t a0 = tc_dev_alloc_count();
        for (int i = 0; i < 100; ++i) {
            uint8_t* buf = (uint8_t*)malloc(fa.cap);
            uint8_t qp = 0;
            MT_CHECK(enc_seq_one(&fa, 0, 0u, buf, &qp) != 0u);
            free(buf);
        }
        MT_CHECK_EQ_I64(tc_dev_alloc_count() - a0, 0);

        /* sized 同口径（strict：m7 F-cache/arena/位写器稳态后零分配） */
        for (int i = 0; i < 20; ++i) {
            uint8_t* buf = (uint8_t*)malloc(fa.cap);
            uint8_t qp = 0;
            MT_CHECK(enc_seq_one(&fa, 1, (uint32_t)(fa.cap / 3u), buf, &qp) != 0u);
            free(buf);
        }
        a0 = tc_dev_alloc_count();
        for (int i = 0; i < 100; ++i) {
            uint8_t* buf = (uint8_t*)malloc(fa.cap);
            uint8_t qp = 0;
            MT_CHECK(enc_seq_one(&fa, 1, (uint32_t)(fa.cap / 3u), buf, &qp) != 0u);
            free(buf);
        }
        MT_CHECK_EQ_I64(tc_dev_alloc_count() - a0, 0);
    }

    /* 3) 并发：竞争失败方走栈回退，输出与参考一致 */
    {
        uint8_t* ref = (uint8_t*)malloc(fa.cap);
        uint8_t rqp = 0;
        size_t ref_size = enc_seq_one(&fa, 0, 0u, ref, &rqp);
        MT_CHECK(ref_size != 0u);
        m10_enc_conc_job jobs[2];
        for (int k = 0; k < 2; ++k) {
            jobs[k].f = &fa; jobs[k].ref = ref; jobs[k].ref_size = ref_size;
            jobs[k].sized = 0; jobs[k].target = 0u; jobs[k].ok = 1;
        }
        pthread_t th[2];
        MT_CHECK_EQ_I64(pthread_create(&th[0], NULL, m10_enc_conc_run, &jobs[0]), 0);
        MT_CHECK_EQ_I64(pthread_create(&th[1], NULL, m10_enc_conc_run, &jobs[1]), 0);
        pthread_join(th[0], NULL);
        pthread_join(th[1], NULL);
        MT_CHECK(jobs[0].ok && jobs[1].ok);
        free(ref);
    }

    enc_frame_free(&fa);
    enc_frame_free(&fb);
}

int main(void)
{
    test_m10_zero_alloc_steady();
    test_m10_geometry_growth_and_oom();
    test_m10_multithread_parity();
    test_m10_direct_parity();
    test_m10_enc_cache();
    return MT_MAIN_RETURN();
}
