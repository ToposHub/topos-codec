/* C1（速度计划 v2）验收测试：编码侧 per-slice scratch 池化。
 *
 *  1. V7-R2（sel=8，产品默认）稳态零分配：plain + sized，预热后
 *     分配计数必须为 0（rANS2 renorm scratch 与 sized 探针全部
 *     走 enc_shared 槽位池；1 线程 + 4 线程）；
 *     （V 代际收纳 2026-09-13：V7-R sel=7 腿随 V7-R 退役移除，
 *     等价覆盖由 sel=8 腿承接——同族 per-slice 熵，同池化路径。）
 *  2. 位一致性：冷池（enc_cache 关闭，栈实例每调用新建）与热池（缓存
 *     常驻、池水位已长满）输出逐字节一致——池复用不改位流；
 *     1 线程 vs 4 线程输出逐字节一致（槽位绑定确定性）；
 *  3. 池增长 OOM 传播：小几何预热后对大几何逐点注入分配故障——
 *     每个故障点必须返回 TC_ERR_OUT_OF_MEMORY 或干净成功（成功时
 *     包与无故障参考逐字节一致；realloc 失败不动旧指针 → 池保留）；
 *  4. （V7-A 分桶 storage 腿随 V 代际收纳批 4 归档移除——band 语法测试
 *     仅存于 TOPOS_DEV_REPLAY 构建的 unit_v7_frame_encoder 等。） */
#include "codec/codec.h"
#include "common/alloc.h"
#include "common/tpool.h"
#include "image_synth.h"
#include "mini_test.h"
#include "topos_codec.h"

#include <stdlib.h>
#include <string.h>

static void sel_cfg(topos_frame_config* c, uint32_t w, uint32_t h, uint32_t sel)
{
    memset(c, 0, sizeof(*c));
    c->struct_size = (uint32_t)sizeof(*c);
    c->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    c->visible_width = (uint16_t)w;
    c->visible_height = (uint16_t)h;
    c->qp_base = 22u;
    c->qmatrix_id = 1u;
    c->slice_rows = 4u;
    c->reserved[0] = sel;
}

typedef struct c1_frame {
    topos_frame_config cfg;
    topos_frame_input in;
    uint16_t* pl[4];
    size_t cap;
} c1_frame;

static void c1_frame_free(c1_frame* f)
{
    for (int p = 0; p < 4; ++p) { free(f->pl[p]); }
}

static int c1_frame_make(c1_frame* f, uint32_t w, uint32_t h, uint32_t sel,
                         uint8_t qp, uint64_t seed)
{
    memset(f, 0, sizeof(*f));
    image_synth_cfg ic;
    memset(&ic, 0, sizeof(ic));
    ic.seed = seed;
    ic.width = w;
    ic.height = h;
    ic.kind = TC_SYNTH_MIXED;
    if (image_synth_alloc(&ic, 0, &f->pl[0], &f->pl[1], &f->pl[2], &f->pl[3]) != 0) {
        return -1;
    }
    f->in.struct_size = (uint32_t)sizeof(topos_frame_input);
    f->in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    for (int p = 0; p < 3; ++p) { f->in.planes[p] = f->pl[p]; }
    sel_cfg(&f->cfg, w, h, sel);
    f->cfg.qp_base = qp;
    f->cap = tc_frame_packet_bound(&f->cfg);
    return f->cap != 0u ? 0 : -1;
}

/* 单次编码到新 malloc 缓冲（malloc 不经 tc_alloc → 不污染分配计数） */
static size_t c1_encode(const c1_frame* f, int sized, uint32_t target,
                        uint8_t** out)
{
    uint8_t* buf = (uint8_t*)malloc(f->cap);
    if (buf == NULL) { *out = NULL; return 0u; }
    topos_frame_stats st;
    int32_t rc;
    if (sized != 0) {
        uint8_t qp = 0;
        rc = tc_frame_encode_sized(&f->cfg, &f->in, target, 0u, 63u, &qp,
                                   buf, f->cap, &st);
    } else {
        rc = tc_frame_encode(&f->cfg, &f->in, buf, f->cap, &st);
    }
    if (rc != TC_OK) { free(buf); *out = NULL; return 0u; }
    *out = buf;
    return st.packet_size;
}

/* ---------- 1. 稳态零分配（plain + sized × 1/4 线程 × V7-R/R2） ---------- */

static void zero_alloc_case(uint32_t sel, int threads)
{
    tc_dev_set_thread_count(threads);
    c1_frame f;
    MT_CHECK_EQ_I64(c1_frame_make(&f, 128u, 96u, sel, 22u, 0xC1A11 + sel), 0);
    const uint32_t target = (uint32_t)(f.cap / 3u);

    /* 预热：池/槽位/arena/F-cache/各 worker 槽的惰性 chunk 全部到位
     * （多线程下 arena chunk 的首建随调度落在不同迭代——深预热收敛） */
    for (int i = 0; i < 16; ++i) {
        uint8_t* p = NULL;
        MT_CHECK(c1_encode(&f, i & 1, target, &p) != 0u);
        free(p);
    }
    tc_dev_set_alloc_fault(-1); /* 清零计数 */
    for (int i = 0; i < 60; ++i) {
        uint8_t* p = NULL;
        size_t sz = c1_encode(&f, i & 1, target, &p);
        if (sz == 0u) {
            mt_report(__FILE__, __LINE__, "steady encode failed");
            free(p);
            break;
        }
        free(p);
    }
    MT_CHECK_EQ_I64(tc_dev_alloc_count(), 0);

    c1_frame_free(&f);
    tc_dev_set_thread_count(0);
}

static void test_c1_zero_alloc_steady(void)
{
    zero_alloc_case(8u, 1);  /* V7-R2（产品默认） */
    zero_alloc_case(8u, 4);
}

/* ---------- 2. 冷/热池与跨线程位一致性 ---------- */

static void test_c1_bit_identity(void)
{
    c1_frame f;
    MT_CHECK_EQ_I64(c1_frame_make(&f, 128u, 96u, 8u, 24u, 0xC1B17), 0);
    const uint32_t target = (uint32_t)(f.cap / 3u);

    /* 冷池：缓存关闭 → 栈实例每次全新（首建即冷） */
    tc_dev_set_enc_cache(1);
    uint8_t* cold = NULL;
    size_t cold_sz = c1_encode(&f, 0, 0u, &cold);
    uint8_t* cold_sized = NULL;
    size_t cold_sized_sz = c1_encode(&f, 1, target, &cold_sized);
    tc_dev_set_enc_cache(0);

    /* 热池：缓存常驻，多次编码后池水位长满 */
    for (int i = 0; i < 30; ++i) {
        uint8_t* p = NULL;
        MT_CHECK(c1_encode(&f, i & 1, target, &p) != 0u);
        free(p);
    }
    {
        uint8_t* warm = NULL;
        size_t warm_sz = c1_encode(&f, 0, 0u, &warm);
        MT_CHECK_EQ_I64(warm_sz, cold_sz);
        MT_CHECK(warm != NULL && memcmp(warm, cold, warm_sz) == 0);
        free(warm);

        uint8_t* warm_sized = NULL;
        size_t warm_sized_sz = c1_encode(&f, 1, target, &warm_sized);
        MT_CHECK_EQ_I64(warm_sized_sz, cold_sized_sz);
        MT_CHECK(warm_sized != NULL &&
                 memcmp(warm_sized, cold_sized, warm_sized_sz) == 0);
        free(warm_sized);
    }

    /* 跨线程：4 线程槽位池与 1 线程输出逐字节一致 */
    tc_dev_set_thread_count(4);
    for (int i = 0; i < 4; ++i) {
        uint8_t* p = NULL;
        MT_CHECK(c1_encode(&f, 0, 0u, &p) != 0u);
        free(p);
    }
    {
        uint8_t* mt = NULL;
        size_t mt_sz = c1_encode(&f, 0, 0u, &mt);
        MT_CHECK_EQ_I64(mt_sz, cold_sz);
        MT_CHECK(mt != NULL && memcmp(mt, cold, mt_sz) == 0);
        free(mt);
    }
    tc_dev_set_thread_count(0);

    free(cold);
    free(cold_sized);
    c1_frame_free(&f);
}

/* ---------- 3. 池增长 OOM 传播（V7-R2 plain + sized） ---------- */

static size_t c1_encode_rc(const c1_frame* f, int sized, uint32_t target,
                           uint8_t** out, int32_t* rc_out)
{
    uint8_t* buf = (uint8_t*)malloc(f->cap);
    if (buf == NULL) { *out = NULL; *rc_out = TC_ERR_OUT_OF_MEMORY; return 0u; }
    topos_frame_stats st;
    int32_t rc;
    if (sized != 0) {
        uint8_t qp = 0;
        rc = tc_frame_encode_sized(&f->cfg, &f->in, target, 0u, 63u, &qp,
                                   buf, f->cap, &st);
    } else {
        rc = tc_frame_encode(&f->cfg, &f->in, buf, f->cap, &st);
    }
    if (rc != TC_OK) { free(buf); *out = NULL; *rc_out = rc; return 0u; }
    *out = buf;
    *rc_out = TC_OK;
    return st.packet_size;
}

static void growth_sweep_case(int sized)
{
    tc_dev_set_thread_count(4);
    c1_frame big;
    MT_CHECK_EQ_I64(c1_frame_make(&big, 256u, 160u, 8u, 20u, 0xC1C2), 0);
    const uint32_t target = (uint32_t)(big.cap / 3u);

    /* 缓存关闭 → 每次编码栈实例全新（scratch 池从零增长）：干净参考
     * 与 sweep 同口径，故障点必经池增长路径（首建 realloc） */
    tc_dev_set_enc_cache(1);
    uint8_t* ref = NULL;
    int32_t rc = 0;
    size_t ref_sz = c1_encode_rc(&big, sized, target, &ref, &rc);
    MT_CHECK(ref != NULL);
    MT_CHECK_EQ_I64(rc, TC_OK);

    for (int64_t n = 1; n <= 48; ++n) {
        tc_dev_set_alloc_fault(n);
        uint8_t* p = NULL;
        int32_t r = 0;
        size_t sz = c1_encode_rc(&big, sized, target, &p, &r);
        tc_dev_set_alloc_fault(-1);
        if (sz != 0u) {
            MT_CHECK_EQ_I64(r, TC_OK);
            MT_CHECK_EQ_I64(sz, ref_sz);
            MT_CHECK(memcmp(p, ref, ref_sz) == 0);
            free(p);
        } else {
            MT_CHECK_EQ_I64(r, TC_ERR_OUT_OF_MEMORY);
        }
    }
    /* 故障解除后干净成功且一致 */
    {
        uint8_t* p = NULL;
        int32_t r = 0;
        size_t sz = c1_encode_rc(&big, sized, target, &p, &r);
        MT_CHECK_EQ_I64(r, TC_OK);
        MT_CHECK_EQ_I64(sz, ref_sz);
        MT_CHECK(p != NULL && memcmp(p, ref, ref_sz) == 0);
        free(p);
    }
    tc_dev_set_enc_cache(0);

    free(ref);
    c1_frame_free(&big);
    tc_dev_set_thread_count(0);
}

static void test_c1_growth_oom_sweep(void)
{
    growth_sweep_case(0);
    growth_sweep_case(1);
}

int main(void)
{
    test_c1_zero_alloc_steady();
    test_c1_bit_identity();
    test_c1_growth_oom_sweep();
    return MT_MAIN_RETURN();
}
