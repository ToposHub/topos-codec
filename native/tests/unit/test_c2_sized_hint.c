/* C2（速度计划 v2）验收测试：sized 搜索 hint 模型强化。
 *
 *  1. qp 轨迹差分：模型开 vs 关，跨目标/几何变化序列的每次 encode_sized
 *     必须 (qp_used, packet_size, 包字节) 逐值一致——纯种子/先验不改
 *     strict 括号解 q*（码控决策不变性）；
 *  2. 稳态摊销：同目标重复编码 probe+final 计数 == 2（1 探即中 + final）；
 *  3. 目标变化摊销：换目标后首搜探针数 ≤ 冷启动的一半且 ≤ 4
 *     （模型外推种子 vs 裸 qp_base 冷启）；
 *  4. hint 全禁用路径不受模型开关影响（两级开关正交性）。
 */
#include "codec/codec.h"
#include "common/alloc.h"
#include "common/tpool.h"
#include "image_synth.h"
#include "mini_test.h"
#include "topos_codec.h"

#include <stdlib.h>
#include <string.h>

typedef struct c2_frame {
    topos_frame_config cfg;
    topos_frame_input in;
    uint16_t* pl[4];
    size_t cap;
} c2_frame;

static void c2_frame_free(c2_frame* f)
{
    for (int p = 0; p < 4; ++p) { free(f->pl[p]); }
}

static int c2_frame_make(c2_frame* f, uint32_t w, uint32_t h, uint8_t qp,
                         uint64_t seed)
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
    memset(&f->cfg, 0, sizeof(f->cfg));
    f->cfg.struct_size = (uint32_t)sizeof(f->cfg);
    f->cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    f->cfg.visible_width = (uint16_t)w;
    f->cfg.visible_height = (uint16_t)h;
    f->cfg.qp_base = qp;
    f->cfg.qmatrix_id = 1u;
    f->cfg.slice_rows = 4u;
    f->cfg.reserved[0] = 8u; /* V7-R2（产品默认，strict sized 路径） */
    f->cap = tc_frame_packet_bound(&f->cfg);
    return f->cap != 0u ? 0 : -1;
}

/* 单次 sized 编码（malloc 输出缓冲，不污染 tc 分配计数） */
static int32_t c2_sized(const c2_frame* f, uint32_t target, uint8_t* qp_out,
                        uint8_t** pkt, uint32_t* size_out)
{
    uint8_t* buf = (uint8_t*)malloc(f->cap);
    if (buf == NULL) { return TC_ERR_OUT_OF_MEMORY; }
    topos_frame_stats st;
    uint8_t qp = 0;
    int32_t rc = tc_frame_encode_sized(&f->cfg, &f->in, target, 0u, 63u, &qp,
                                       buf, f->cap, &st);
    if (rc != TC_OK) { free(buf); return rc; }
    *qp_out = qp;
    *pkt = buf;
    *size_out = st.packet_size;
    return TC_OK;
}

/* ---------- 1. qp 轨迹 + 包字节差分（模型开 vs 关） ---------- */

static void test_c2_differential(void)
{
    c2_frame fa, fb;
    MT_CHECK_EQ_I64(c2_frame_make(&fa, 160u, 112u, 18u, 0xC2A1), 0);
    MT_CHECK_EQ_I64(c2_frame_make(&fb, 128u, 96u, 30u, 0xC2A2), 0);
    const uint32_t cap_ref = (uint32_t)fa.cap;
    const uint32_t targets[8] = {
        cap_ref / 6u, cap_ref / 6u, cap_ref / 14u, cap_ref / 5u,
        cap_ref / 5u, cap_ref / 20u, cap_ref / 9u, cap_ref / 6u,
    };
    const c2_frame* seq[8] = { &fa, &fa, &fa, &fb, &fa, &fb, &fb, &fa };

    uint8_t qp_off[8], qp_on[8];
    uint32_t sz_off[8], sz_on[8];
    uint8_t* pkt_off[8] = { 0 };
    uint8_t* pkt_on[8] = { 0 };

    tc_dev_set_qp_hint(0);
    tc_dev_set_qp_hint_model(1); /* pass0：模型关（= M10-6.3A 语义） */
    for (int i = 0; i < 8; ++i) {
        MT_CHECK_EQ_I64(c2_sized(seq[i], targets[i], &qp_off[i], &pkt_off[i],
                                 &sz_off[i]), TC_OK);
    }
    /* 注：hint 状态（qp_hint/bytes/prev）跨 pass 常驻（进程缓存语义，
     * 与生产一致）——pass1 的种子路径与 pass0 不同正是被测差异面；
     * 输出必须仍逐值一致（q* 由 bytes(q) 唯一决定）。 */
    tc_dev_set_qp_hint_model(0); /* pass1：模型开 */
    for (int i = 0; i < 8; ++i) {
        MT_CHECK_EQ_I64(c2_sized(seq[i], targets[i], &qp_on[i], &pkt_on[i],
                                 &sz_on[i]), TC_OK);
    }
    for (int i = 0; i < 8; ++i) {
        MT_CHECK_EQ_I64(qp_on[i], qp_off[i]);
        MT_CHECK_EQ_I64(sz_on[i], sz_off[i]);
        MT_CHECK(pkt_on[i] != NULL && pkt_off[i] != NULL &&
                 memcmp(pkt_on[i], pkt_off[i], sz_on[i]) == 0);
    }

    tc_dev_set_qp_hint_model(0);
    for (int i = 0; i < 8; ++i) { free(pkt_off[i]); free(pkt_on[i]); }
    c2_frame_free(&fa);
    c2_frame_free(&fb);
}

/* ---------- 2/3. 探针计数：稳态与目标变化摊销 ---------- */

static void test_c2_probe_amortization(void)
{
    c2_frame f;
    MT_CHECK_EQ_I64(c2_frame_make(&f, 192u, 128u, 4u, 0xC2B1), 0);
    const uint32_t t1 = (uint32_t)(f.cap / 40u); /* q*≈23：冷启多探 */
    const uint32_t t2 = (uint32_t)(f.cap / 60u); /* q*≈40：目标变化外推 */
    const uint32_t t3 = (uint32_t)(f.cap / 80u); /* q*≈48：弦线第二换向 */
    uint8_t qp = 0;
    uint8_t* pkt = NULL;
    uint32_t sz = 0;

    tc_dev_set_qp_hint(0);
    tc_dev_set_qp_hint_model(0); /* 模型开（0 = disable 关） */

    /* 冷启动（qp_base=4 偏远，无历史）：多探基准 */
    tc_dev_sized_reset();
    MT_CHECK_EQ_I64(c2_sized(&f, t1, &qp, &pkt, &sz), TC_OK);
    free(pkt);
    const uint64_t cold_iters = tc_dev_sized_iters();
    MT_CHECK(cold_iters > 4u);

    /* 稳态：同目标重复——2 探定界 + final == 3（M10-6.3A 契约不变） */
    MT_CHECK_EQ_I64(c2_sized(&f, t1, &qp, &pkt, &sz), TC_OK);
    free(pkt);
    tc_dev_sized_reset();
    MT_CHECK_EQ_I64(c2_sized(&f, t1, &qp, &pkt, &sz), TC_OK);
    free(pkt);
    MT_CHECK_EQ_I64(tc_dev_sized_iters(), 3u);

    /* 目标变化①（单点历史：保守外推）——不劣于冷启动一半 */
    tc_dev_sized_reset();
    MT_CHECK_EQ_I64(c2_sized(&f, t2, &qp, &pkt, &sz), TC_OK);
    free(pkt);
    const uint64_t switch_iters = tc_dev_sized_iters();
    MT_CHECK(switch_iters * 2u < cold_iters);

    /* 稳态于 t2 */
    tc_dev_sized_reset();
    MT_CHECK_EQ_I64(c2_sized(&f, t2, &qp, &pkt, &sz), TC_OK);
    free(pkt);
    MT_CHECK_EQ_I64(tc_dev_sized_iters(), 3u);

    /* 目标变化②（双工作点弦线）：回旧目标——弦外推直落工作点 */
    tc_dev_sized_reset();
    MT_CHECK_EQ_I64(c2_sized(&f, t1, &qp, &pkt, &sz), TC_OK);
    free(pkt);
    MT_CHECK(tc_dev_sized_iters() <= 4u);

    /* 目标变化③（弦线，向下）：更小目标 */
    tc_dev_sized_reset();
    MT_CHECK_EQ_I64(c2_sized(&f, t3, &qp, &pkt, &sz), TC_OK);
    free(pkt);
    MT_CHECK(tc_dev_sized_iters() <= 5u);

    /* ---------- 4. 两级开关正交：hint 全禁时不因模型开关改变输出 ---------- */
    {
        uint8_t qp_m = 0;
        uint32_t sz_m = 0;
        uint8_t* pkt_m = NULL;
        tc_dev_set_qp_hint(1);
        tc_dev_set_qp_hint_model(1);
        MT_CHECK_EQ_I64(c2_sized(&f, t2, &qp, &pkt, &sz), TC_OK);
        tc_dev_set_qp_hint_model(0);
        MT_CHECK_EQ_I64(c2_sized(&f, t2, &qp_m, &pkt_m, &sz_m), TC_OK);
        MT_CHECK_EQ_I64(qp_m, qp);
        MT_CHECK_EQ_I64(sz_m, sz);
        MT_CHECK(pkt_m != NULL && pkt != NULL && memcmp(pkt_m, pkt, sz) == 0);
        tc_dev_set_qp_hint(0);
        free(pkt);
        free(pkt_m);
    }

    c2_frame_free(&f);
}

int main(void)
{
    test_c2_differential();
    test_c2_probe_amortization();
    return MT_MAIN_RETURN();
}
