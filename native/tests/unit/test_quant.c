/* 量化：qp_scale 冻结值、Q 公式、死区边界、符号对称、往返界、钳位防御 */
#include "transform/quant.h"
#include "transform/transform.h"
#include "mini_test.h"

int main(void)
{
    /* qp_scale 冻结值抽验（spec 附录 A.4）与公式锚点 qp=4+4k → 2^k */
    struct { uint32_t qp, v; } anchors[] = {
        {0, 1}, {7, 2}, {10, 3}, {12, 4}, {13, 5}, {16, 8}, {20, 16}, {24, 32},
        {28, 64}, {32, 128}, {36, 256}, {40, 512}, {44, 1024}, {48, 2048},
        {52, 4096}, {56, 8192}, {60, 16384}, {61, 19484}, {63, 27555},
        /* v1.5（ADR-C031）扩展域锚点：整幂 + 表尾 */
        {64, 32768}, {68, 65536}, {72, 131072}, {76, 262144}, {80, 524288},
        {84, 1048576}, {88, 2097152}, {92, 4194304}, {95, 7053950},
    };
    for (size_t i = 0; i < sizeof(anchors) / sizeof(anchors[0]); ++i) {
        MT_CHECK_EQ_U64(tc_qp_scale(anchors[i].qp), anchors[i].v);
    }
    for (uint32_t k = 0; k <= 22; ++k) {   /* v1.5：2^(22) = qp92 表项 */
        MT_CHECK_EQ_U64(tc_qp_scale(4 + 4 * k), 1u << k);
    }
    MT_CHECK_EQ_U64(tc_qp_scale(96), 0);   /* v1.5：上界 95 */
    MT_CHECK_EQ_U64(tc_qp_scale(1000), 0);

    /* Q 公式：Q = max(1, (qm·s+128)>>8)；单调不减 */
    {
        uint32_t prev = 0;
        for (uint32_t qp = 0; qp <= 95; ++qp) {       /* v1.5：全域单调 */
            uint32_t q = tc_quant_step(1000, qp);
            MT_CHECK(q >= 1);
            MT_CHECK(q >= prev);
            prev = q;
        }
        MT_CHECK_EQ_U64(tc_quant_step(16, 4), 1);     /* flat 低 qp → 1 */
        MT_CHECK_EQ_U64(tc_quant_step(1000, 4), 4);   /* (1000+128)>>8 */
        MT_CHECK_EQ_U64(tc_quant_step(4095, 63), 440772); /* 值域上界 (4095·27555+128)>>8 */
        MT_CHECK_EQ_U64(tc_quant_step(16, 64), 2048); /* v1.5：(16·32768+128)>>8 */
        MT_CHECK_EQ_U64(tc_quant_step(16, 96), 0);    /* qp 非法 */
        /* v1.5 高域 uint64 中转：655（冻结表最大 qm）× 7053950 */
        MT_CHECK_EQ_U64(tc_quant_step(655, 95), (655ull * 7053950ull + 128ull) >> 8);
    }

    /* qmatrix 查表 */
    MT_CHECK(tc_qmatrix_by_id(0) != NULL);
    MT_CHECK(tc_qmatrix_by_id(1) != NULL);
    MT_CHECK(tc_qmatrix_by_id(2) != NULL);
    MT_CHECK(tc_qmatrix_by_id(3) != NULL);
    MT_CHECK(tc_qmatrix_by_id(255) == NULL);
    const tc_qmatrix_set* flat = tc_qmatrix_by_id(0);
    for (int i = 0; i < 64; ++i) {
        MT_CHECK_EQ_U64(flat->luma[i], 16);
        MT_CHECK_EQ_U64(flat->chroma[i], 16);
    }

    /* 死区边界：QM/qp 组合出 Q=4（qm=1000,qp=4 → Q=4）
     * DC(i=0)：q=0 ⟺ mag < Q/2=2；mag=2 → q=1
     * AC(i=1)：q=0 ⟺ mag < Q/2+Q/4=3；mag=3 → q=1 */
    {
        int32_t F[64], q[64];
        const uint16_t qm1000[64] = { 0 };
        /* 构造：用显式数组，全 1000 */
        uint16_t qmv[64];
        for (int i = 0; i < 64; ++i) { qmv[i] = 1000; }
        (void)qm1000;
        for (int i = 0; i < 64; ++i) { F[i] = 0; }

        F[0] = 1; tc_quant_block(F, qmv, 4, q); MT_CHECK_EQ_I64(q[0], 0);
        F[0] = 2; tc_quant_block(F, qmv, 4, q); MT_CHECK_EQ_I64(q[0], 1);
        F[0] = -2; tc_quant_block(F, qmv, 4, q); MT_CHECK_EQ_I64(q[0], -1);
        F[0] = 5; tc_quant_block(F, qmv, 4, q); MT_CHECK_EQ_I64(q[0], 1);  /* (5+2)/4=1 */
        F[0] = 6; tc_quant_block(F, qmv, 4, q); MT_CHECK_EQ_I64(q[0], 2);  /* (6+2)/4=2 */

        F[1] = 2; tc_quant_block(F, qmv, 4, q); MT_CHECK_EQ_I64(q[1], 0);  /* 2+1=3 <4 */
        F[1] = 3; tc_quant_block(F, qmv, 4, q); MT_CHECK_EQ_I64(q[1], 1);  /* 3+1=4 → 1 */
        F[1] = -3; tc_quant_block(F, qmv, 4, q); MT_CHECK_EQ_I64(q[1], -1);
    }

    /* 符号对称 + DC 往返界 |q·Q − |F|| ≤ Q/2（dz=0）—— 随机 2 万组 */
    {
        uint16_t qmv[64];
        for (int i = 0; i < 64; ++i) { qmv[i] = 97; }
        for (int t = 0; t < 20000; ++t) {
            int32_t F[64], q[64], Fp[64];
            for (int i = 0; i < 64; ++i) {
                F[i] = (int32_t)(mt_rand_u64() % 4000000u) - 2000000;
            }
            tc_quant_block(F, qmv, 20, q);
            tc_dequant_block(q, qmv, 20, Fp);
            const uint32_t Q0 = tc_quant_step(97, 20);
            MT_CHECK(q[0] == 0 ? F[0] < (int32_t)(Q0 / 2 + 1) : 1);
            if (q[0] != 0) {
                int32_t diff = Fp[0] - F[0];
                if (diff < 0) { diff = -diff; }
                MT_CHECK((uint32_t)diff <= Q0 / 2 + 1);
            }
            /* 符号对称 */
            int32_t Fneg[64], qneg[64];
            for (int i = 0; i < 64; ++i) { Fneg[i] = -F[i]; }
            tc_quant_block(Fneg, qmv, 20, qneg);
            for (int i = 0; i < 64; ++i) { MT_CHECK(qneg[i] == -q[i]); }
        }
    }

    /* 钳位防御：非法大 q 的 dequant / 非法大 F 的 quant 不越界 */
    {
        uint16_t qmv[64];
        for (int i = 0; i < 64; ++i) { qmv[i] = 4095; }
        int32_t q[64], Fp[64];
        for (int i = 0; i < 64; ++i) { q[i] = (i & 1) ? (1 << 30) : -(1 << 30); }
        tc_dequant_block(q, qmv, 63, Fp);
        for (int i = 0; i < 64; ++i) {
            MT_CHECK(Fp[i] <= TC_TRANSFORM_MAX_ABS_F);
            MT_CHECK(Fp[i] >= -TC_TRANSFORM_MAX_ABS_F);
        }
        int32_t F[64], qo[64];
        for (int i = 0; i < 64; ++i) { F[i] = (i & 1) ? (0x7FFFFFFF) : (-2147483647 - 1); }
        tc_quant_block(F, qmv, 63, qo); /* 不崩溃即通过（sanitizer 门禁） */
        (void)qo[0];
    }

    /* —— 批 4：位深感知 ctx（bd16 exact_div 真除法 + f_clamp 外推；
     * bd12 与 tc_quant_ctx_init 逐位一致） —— */
    {
        uint16_t qm[64];
        for (int i = 0; i < 64; ++i) { qm[i] = (uint16_t)(1u + (uint32_t)(i % 17u)); }

        /* bd12 一致性：_init_bd 与历史 _init 产出相同 ctx */
        {
            tc_quant_ctx a;
            tc_quant_ctx_init(&a, qm, 31u);
            tc_quant_ctx b;
            tc_quant_ctx_init_bd(&b, qm, 31u, 12u);
            for (int i = 0; i < 64; ++i) {
                MT_CHECK_EQ_U64(a.Q[i], b.Q[i]);
                MT_CHECK_EQ_U64(a.half[i], b.half[i]);
                MT_CHECK_EQ_U64(a.dz[i], b.dz[i]);
            }
            MT_CHECK_EQ_U64(b.exact_div, 0ull);
            MT_CHECK_EQ_I64(b.f_clamp, 0);
        }

        /* bd16：exact_div=1 + f_clamp 按 (2^(bd-1)-1)/2047 等比外推；
         * 量化逐系数 == 域钳位 + 真除法参考（fastdiv n 域外语义钉死） */
        tc_quant_ctx w;
        tc_quant_ctx_init_bd(&w, qm, 31u, 16u);
        MT_CHECK_EQ_U64(w.exact_div, 1ull);
        MT_CHECK_EQ_I64(w.f_clamp, ((int64_t)TC_TRANSFORM_MAX_ABS_F * 32767) / 2047);
        for (int t = 0; t < 2000; ++t) {
            int32_t F[64], q[64];
            for (int i = 0; i < 64; ++i) {
                const uint64_t r = mt_rand_u64();
                if (r % 8u == 0u) { F[i] = (int32_t)w.f_clamp; }
                else if (r % 8u == 1u) { F[i] = -(int32_t)w.f_clamp; }
                else { F[i] = (int32_t)(r % 700000000ull) - 350000000; }
            }
            tc_quant_block_ctx(&w, F, q);
            for (int i = 0; i < 64; ++i) {
                int64_t v = F[i];
                if (v > w.f_clamp) { v = w.f_clamp; }
                else if (v < -w.f_clamp) { v = -w.f_clamp; }
                const int64_t mag = v < 0 ? -v : v;
                const int64_t num = mag + (int64_t)w.half[i];
                const int64_t num_eff = num > (int64_t)w.dz[i]
                    ? num - (int64_t)w.dz[i] : 0;
                const int64_t ref = (v < 0 ? -1 : 1)
                    * (int64_t)(num_eff / (int64_t)w.Q[i]);
                MT_CHECK_EQ_I64(q[i], ref);
            }
        }

        /* bd16 反量化宽钳位：非法大 q 不越 f_clamp（±2^25 旧界不再误钳） */
        {
            int32_t q[64], Fp[64];
            for (int i = 0; i < 64; ++i) { q[i] = (i & 1) ? (1 << 29) : -(1 << 29); }
            tc_dequant_block_ctx(&w, q, Fp);
            for (int i = 0; i < 64; ++i) {
                MT_CHECK(Fp[i] <= w.f_clamp && Fp[i] >= -w.f_clamp);
            }
        }
    }

    return MT_MAIN_RETURN();
}
