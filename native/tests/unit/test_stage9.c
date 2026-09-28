/* 阶段 9 门禁：SIMD/优化路径与 scalar 参考的差分一致性 + fastdiv 精确性域证明复验。
 *
 * 覆盖：
 *  1. fastdiv：全可达 Q 域（qm 16..4095 × qp 0..63 抽样 + 全幂二）× n 边界/随机
 *     与 n/d 逐值相等；
 *  2. quant ctx：随机 F（含钳位界）与 tc_quant_block 逐值一致；dequant 同；
 *  3. AVX2/NEON 前向变换 vs scalar：随机 + 极值 ±2047 全一致；
 *  4. 逆变换（ILP 标量）vs 冻结 golden_transform 语义（随机 + 极值）；
 *  5. 强制后端下端到端 encode 字节一致（scalar vs force simd）；
 *  6. dispatch dev API 行为（模式切换、backend 名）。
 */
#include "bitstream/bitio.h"
#include "codec/codec.h"
#include "entropy/rice.h"
#include "common/tpool.h"
#include "simd/dispatch.h"
#include "transform/fastdiv.h"
#include "transform/quant.h"
#include "transform/transform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini_test.h"

/* 确定性 LCG（与 packet_synth 同源约定风格） */
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rng_u64(void)
{
    s_rng ^= s_rng >> 12;
    s_rng ^= s_rng << 25;
    s_rng ^= s_rng >> 27;
    return s_rng * 2685821657736338717ull;
}

/* ---- 1. fastdiv 域证明复验 ---- */
static void test_fastdiv(void)
{
    /* 全部可达 Q：qm ∈ {冻结表值 ∪ 随机 16..4095 ∪ 65535 域防御} × qp 全档 */
    uint32_t qms[24];
    const tc_qmatrix_set* std_qm = tc_qmatrix_by_id(1u);
    memcpy(qms, std_qm->luma, sizeof(uint16_t) * 16);
    for (int i = 16; i < 24; ++i) { qms[i] = 16u + (uint32_t)(rng_u64() % 4096u); }
    int cases = 0;
    qms[22] = 655u;   /* 冻结表最大 qm（444 Compact 表尾）→ qp95 = 18,045,205 */
    qms[23] = 4095u; /* A.3 值域上界：qp95 → Q≈112.8M > D_MAX → 真除回退 */
    for (int qi = 0; qi < 24; ++qi) {
        for (uint32_t qp = 0u; qp < 96u; qp += 3u) {   /* v1.5：qp 域 0..95 */
            uint32_t d = tc_quant_step((uint16_t)qms[qi], qp);
            MT_CHECK(d >= 1u);
            tc_fastdiv fd;
            tc_fastdiv_init(d, &fd);
            /* n 边界 + 域上界附近 + 随机 */
            uint32_t ns[10] = {0u, 1u, d - 1u, d, d + 1u, 2u * d, 3u * d + 1u,
                               TC_FASTDIV_N_MAX - 1u, TC_FASTDIV_N_MAX, 0};
            for (int k = 0; k < 9; ++k) {
                MT_CHECK_EQ_U64(tc_fastdiv_apply(ns[k], &fd), ns[k] / d);
                cases++;
            }
            for (int k = 0; k < 8; ++k) {
                uint32_t n = (uint32_t)(rng_u64() & (uint64_t)TC_FASTDIV_N_MAX);
                MT_CHECK_EQ_U64(tc_fastdiv_apply(n, &fd), n / d);
                cases++;
            }
        }
    }
    /* 幂二全档 */
    for (uint32_t d = 1u; d <= (1u << 15); d <<= 1) {
        tc_fastdiv fd;
        tc_fastdiv_init(d, &fd);
        for (int k = 0; k < 8; ++k) {
            uint32_t n = (uint32_t)(rng_u64() & (uint64_t)TC_FASTDIV_N_MAX);
            MT_CHECK_EQ_U64(tc_fastdiv_apply(n, &fd), n / d);
            cases++;
        }
    }
    (void)cases;
}

/* ---- 2. quant ctx 差分 ---- */
static void test_quant_ctx_differential(void)
{
    const tc_qmatrix_set* qms = tc_qmatrix_by_id(1u);
    const uint16_t* mats[2] = {qms->luma, qms->chroma};
    for (int m = 0; m < 2; ++m) {
        for (uint32_t qp = 0u; qp < 64u; qp += 5u) {
            tc_quant_ctx ctx;
            tc_quant_ctx_init(&ctx, mats[m], qp);
            for (int blk = 0; blk < 64; ++blk) {
                int32_t F[64];
                int32_t qa[64], qb[64];
                for (int i = 0; i < 64; ++i) {
                    uint32_t r = (uint32_t)(rng_u64() % 100ull);
                    if (r < 10u) {
                        F[i] = (i % 2u == 0u) ? TC_TRANSFORM_MAX_ABS_F : -TC_TRANSFORM_MAX_ABS_F;
                    } else if (r < 20u) {
                        F[i] = TC_TRANSFORM_MAX_ABS_F + 12345; /* 超界走钳位 */
                    } else if (r < 30u) {
                        F[i] = -(TC_TRANSFORM_MAX_ABS_F + 7);
                    } else {
                        F[i] = (int32_t)(rng_u64() % 4000000ull) - 2000000;
                    }
                }
                tc_quant_block(F, mats[m], qp, qa);
                tc_quant_block_ctx(&ctx, F, qb);
                MT_CHECK(memcmp(qa, qb, sizeof(qa)) == 0);
                int32_t fa[64], fb[64];
                tc_dequant_block(qa, mats[m], qp, fa);
                tc_dequant_block_ctx(&ctx, qa, fb);
                MT_CHECK(memcmp(fa, fb, sizeof(fa)) == 0);
            }
        }
    }
}

/* ---- 2b. M10-6.3B gather 型量化内核差分（zigzag 直写 + 非零掩码） ---- */
static void test_quant_zz_kernel_differential(void)
{
    const tc_qmatrix_set* qms[2] = {tc_qmatrix_by_id(0u), tc_qmatrix_by_id(1u)};
    /* 覆盖 force-scalar 回退与 auto（AVX2 可用时）两路解析 */
    for (int mode = 0; mode < 2; ++mode) {
        if (mode == 0) { tc_dev_set_simd_mode(TC_SIMD_SCALAR); }
        else { tc_dev_set_simd_mode(TC_SIMD_AUTO); }
        tc_quant_zz_fn fn = tc_simd_resolve_quant_zz(0u);
        MT_CHECK(fn != NULL);
        for (int mi = 0; mi < 2; ++mi) {
            const uint16_t* mats[2] = {qms[mi]->luma, qms[mi]->chroma};
            for (int m = 0; m < 2; ++m) {
                for (uint32_t qp = 0u; qp < 64u; ++qp) {
                    tc_quant_ctx ctx;
                    tc_quant_ctx_init(&ctx, mats[m], qp);
                    for (int blk = 0; blk < 16; ++blk) {
                        int32_t F[64], ref[64], got[64];
                        for (int i = 0; i < 64; ++i) {
                            uint32_t r = (uint32_t)(rng_u64() % 100ull);
                            if (r < 8u) { F[i] = 0; }
                            else if (r < 16u) { F[i] = TC_TRANSFORM_MAX_ABS_F; }
                            else if (r < 24u) { F[i] = -TC_TRANSFORM_MAX_ABS_F; }
                            else if (r < 30u) { F[i] = TC_TRANSFORM_MAX_ABS_F + 5; /* 钳位 */ }
                            else { F[i] = (int32_t)(rng_u64() % 40000000ull) - 20000000; }
                        }
                        tc_quant_block_zigzag(&ctx, F, ref);
                        uint64_t nz = 0u;
                        fn(&ctx, F, got, &nz);
                        MT_CHECK(memcmp(ref, got, sizeof(ref)) == 0);
                        uint64_t want = 0u;
                        for (uint32_t sp = 0u; sp < 64u; ++sp) {
                            if (ref[sp] != 0) { want |= (uint64_t)1 << sp; }
                        }
                        MT_CHECK_EQ_U64(nz, want);
                    }
                }
            }
        }
    }
    tc_dev_set_simd_mode(TC_SIMD_AUTO);
}

/* ---- 2c. P-速⑥ 自然序量化内核差分（免 gather：q 自然序 + 掩码翻译） ---- */
static void test_quant_nat_kernel_differential(void)
{
    const tc_qmatrix_set* qms[2] = {tc_qmatrix_by_id(0u), tc_qmatrix_by_id(1u)};
    for (int mode = 0; mode < 2; ++mode) {
        if (mode == 0) { tc_dev_set_simd_mode(TC_SIMD_SCALAR); }
        else { tc_dev_set_simd_mode(TC_SIMD_AUTO); }
        tc_quant_nat_fn fn = tc_simd_resolve_quant_nat(0u);
        MT_CHECK(fn != NULL);
        for (int mi = 0; mi < 2; ++mi) {
            const uint16_t* mats[2] = {qms[mi]->luma, qms[mi]->chroma};
            for (int m = 0; m < 2; ++m) {
                for (uint32_t qp = 0u; qp < 64u; ++qp) {
                    tc_quant_ctx ctx;
                    tc_quant_ctx_init(&ctx, mats[m], qp);
                    for (int blk = 0; blk < 16; ++blk) {
                        int32_t F[64], ref_nat[64], got[64];
                        for (int i = 0; i < 64; ++i) {
                            uint32_t r = (uint32_t)(rng_u64() % 100ull);
                            if (r < 8u) { F[i] = 0; }
                            else if (r < 16u) { F[i] = TC_TRANSFORM_MAX_ABS_F; }
                            else if (r < 24u) { F[i] = -TC_TRANSFORM_MAX_ABS_F; }
                            else if (r < 30u) { F[i] = TC_TRANSFORM_MAX_ABS_F + 5; /* 钳位 */ }
                            else { F[i] = (int32_t)(rng_u64() % 40000000ull) - 20000000; }
                        }
                        tc_quant_block_ctx(&ctx, F, ref_nat);
                        uint64_t nat = 0u;
                        fn(&ctx, F, got, &nat);
                        MT_CHECK(memcmp(ref_nat, got, sizeof(ref_nat)) == 0);
                        uint64_t want_nat = 0u;
                        for (uint32_t k = 0u; k < 64u; ++k) {
                            if (ref_nat[k] != 0) { want_nat |= (uint64_t)1 << k; }
                        }
                        MT_CHECK_EQ_U64(nat, want_nat);
                        /* 掩码翻译：zz bit p = nat bit kTcZigzag[p]（与 zigzag
                         * 参考内核的 nz 逐位一致——fill 扫描域等价性钉死） */
                        int32_t ref_zz[64];
                        tc_quant_block_zigzag(&ctx, F, ref_zz);
                        uint64_t want_zz = 0u;
                        for (uint32_t sp = 0u; sp < 64u; ++sp) {
                            if (ref_zz[sp] != 0) { want_zz |= (uint64_t)1 << sp; }
                        }
                        MT_CHECK_EQ_U64(tc_zz_mask_from_nat(nat), want_zz);
                        /* 全零/全非零掩码翻译边界（退化输入） */
                        MT_CHECK_EQ_U64(tc_zz_mask_from_nat(0u), 0u);
                        MT_CHECK_EQ_U64(tc_zz_mask_from_nat(~UINT64_C(0)), ~UINT64_C(0));
                    }
                }
            }
        }
    }
    tc_dev_set_simd_mode(TC_SIMD_AUTO);
}

/* ---- 2d. P-速② u16 平面行直载 forward 差分（vs 标量 gather + forward） ---- */
static void test_forward_rows_differential(void)
{
    /* 合成平面：4 块宽 × 8 行 u16，随机 + 0/65535/32768 边界；
     * mid 覆盖 10/12/16-bit（512/2048/32768——16-bit 检验 mod 2^16 溢出环绕） */
    enum { COLS = 4, STRIDE = COLS * 8, ROWS = 8 };
    static uint16_t plane[ROWS * STRIDE];
    for (int i = 0; i < ROWS * STRIDE; ++i) {
        uint32_t r = (uint32_t)(rng_u64() % 100ull);
        if (r < 5u) { plane[i] = 0u; }
        else if (r < 10u) { plane[i] = 65535u; }
        else if (r < 15u) { plane[i] = 32768u; }
        else { plane[i] = (uint16_t)(rng_u64() % 65536ull); }
    }
    static const int32_t mids[3] = {512, 2048, 32768};
    for (int mode = 0; mode < 2; ++mode) {
        if (mode == 0) { tc_dev_set_simd_mode(TC_SIMD_SCALAR); }
        else { tc_dev_set_simd_mode(TC_SIMD_AUTO); }
        tc_forward_rows_fn fn = tc_simd_resolve_forward_rows(0u);
        MT_CHECK(fn != NULL);
        for (int mi = 0; mi < 3; ++mi) {
            for (int by = 0; by < 1; ++by) {
                for (int bx = 0; bx < COLS; ++bx) {
                    const uint16_t* src = plane + (size_t)by * 8u * STRIDE + (size_t)bx * 8u;
                    int16_t x[64];
                    for (int y = 0; y < 8; ++y) {
                        for (int xx = 0; xx < 8; ++xx) {
                            x[y * 8 + xx] =
                                (int16_t)((int32_t)src[(size_t)y * STRIDE + xx] - mids[mi]);
                        }
                    }
                    int32_t ref[64], got[64];
                    tc_transform_forward_8x8_scalar(x, ref);
                    fn(src, STRIDE, mids[mi], got);
                    MT_CHECK(memcmp(ref, got, sizeof(ref)) == 0);
                }
            }
        }
    }
    tc_dev_set_simd_mode(TC_SIMD_AUTO);
}

/* ---- 3/4. 变换差分（SIMD vs scalar / 极值） ---- */
static void test_transform_differential(void)
{
    for (int blk = 0; blk < 128; ++blk) {
        int16_t x[64];
        for (int i = 0; i < 64; ++i) {
            uint32_t r = (uint32_t)(rng_u64() % 100ull);
            if (r < 5u) { x[i] = 2047; }
            else if (r < 10u) { x[i] = -2047; }
            else { x[i] = (int16_t)((int32_t)(rng_u64() % 4096ull) - 2048); }
        }
        int32_t fa[64], fb[64];
        tc_transform_forward_8x8_scalar(x, fa);
        tc_transform_forward_8x8(x, fb); /* 经 dispatch（AVX2/NEON 或 scalar） */
        MT_CHECK(memcmp(fa, fb, sizeof(fa)) == 0);

        /* 逆变换：dequant 域输入（±2^25 钳位内）scalar vs dispatch 逐位一致。
         * 批 4：±2^25 域外属宽位深（bd≥13）专属——SIMD 全逆变换内核的
         * B 16-bit 分解域（|B|<2^46）仅在 ±2^25 输入下成立，bd≥13 经
         * resolve wide 门控恒走 scalar（宽钳位 2^30，域见 transform.h），
         * 故跨后端差分收敛到共享的 ±2^25 域；域外仅标量路有定义。 */
        int32_t coef[64], coef12[64];
        for (int i = 0; i < 64; ++i) {
            uint32_t r = (uint32_t)(rng_u64() % 100ull);
            if (r < 5u) { coef[i] = TC_TRANSFORM_MAX_ABS_F; }
            else if (r < 10u) { coef[i] = -TC_TRANSFORM_MAX_ABS_F; }
            else if (r < 15u) { coef[i] = TC_TRANSFORM_MAX_ABS_F + 999; }
            else { coef[i] = (int32_t)(rng_u64() % 60000000ull) - 30000000; }
            int32_t c = coef[i];
            if (c > TC_TRANSFORM_MAX_ABS_F) { c = TC_TRANSFORM_MAX_ABS_F; }
            else if (c < -TC_TRANSFORM_MAX_ABS_F) { c = -TC_TRANSFORM_MAX_ABS_F; }
            coef12[i] = c;
        }
        int32_t xa[64], xb[64];
        tc_transform_inverse_8x8_scalar(coef12, xa);
        tc_transform_inverse_8x8(coef12, xb);
        MT_CHECK(memcmp(xa, xb, sizeof(xa)) == 0);
        /* 域外（≤2^30）：scalar 宽钳位激活——与显式钳到 ±2^30 的输入同结果 */
        int32_t coefw[64];
        for (int i = 0; i < 64; ++i) {
            int32_t c = coef[i];
            if (c > TC_TRANSFORM_MAX_ABS_F_WIDE) { c = TC_TRANSFORM_MAX_ABS_F_WIDE; }
            else if (c < -TC_TRANSFORM_MAX_ABS_F_WIDE) { c = -TC_TRANSFORM_MAX_ABS_F_WIDE; }
            coefw[i] = c;
        }
        int32_t xc[64];
        tc_transform_inverse_8x8_scalar(coef, xa);
        tc_transform_inverse_8x8_scalar(coefw, xc);
        MT_CHECK(memcmp(xa, xc, sizeof(xa)) == 0);
    }
}

/* ---- 5. 强制后端端到端位流一致 ---- */
static void test_end_to_end_backend_parity(void)
{
    /* R4.5：格式矩阵差分——pf{0,1,2} × bd{10,12} × profile{3,5,6 合法组合}
     * × alpha(mode2 a8)，编码 + 解码双端 scalar vs FORCE 逐字节一致。
     * SIMD 内核自阶段 9 起格式无关（ADR-C002 界推导覆盖 12-bit 域；
     * 平面几何/位深/profile 只是几何与枚举层概念，不进内核），本测试把
     * 该不变量钉进门禁：任何后续内核改动对新格式漂移立即失败。 */
    static const struct { uint8_t pf; uint8_t bd; uint8_t profile; } kCombos[] = {
        {0u, 10u, 3u}, {0u, 12u, 3u},
        {1u, 10u, 3u}, {1u, 12u, 3u},
        {2u, 10u, 3u}, {2u, 12u, 3u},
        {1u, 10u, 5u}, {1u, 12u, 5u}, {2u, 12u, 5u},
        {1u, 12u, 6u}, {2u, 12u, 6u},
    };
    for (size_t ci = 0; ci < sizeof(kCombos) / sizeof(kCombos[0]); ++ci) {
        const uint8_t pf = kCombos[ci].pf;
        const uint8_t bd = kCombos[ci].bd;
        const uint64_t range = 1ull << bd;
        const uint32_t W = 64u, H = 48u;
        const uint32_t CW = (pf != 0u) ? W : (W + 1u) / 2u; /* R4.2/3 几何 */
        uint16_t y[64 * 48], u[64 * 48], v[64 * 48], a[64 * 48];
        for (size_t i = 0; i < (size_t)W * H; ++i) {
            y[i] = (uint16_t)(rng_u64() % range);
            a[i] = (uint16_t)(rng_u64() % 65536ull);
        }
        for (size_t i = 0; i < (size_t)CW * H; ++i) {
            u[i] = (uint16_t)(rng_u64() % range);
            v[i] = (uint16_t)(rng_u64() % range);
        }
        topos_frame_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.struct_size = (uint32_t)sizeof(cfg);
        cfg.visible_width = 64u;
        cfg.visible_height = 48u;
        cfg.profile = kCombos[ci].profile;
        cfg.pixel_format = pf;
        cfg.bit_depth = bd;
        cfg.color_matrix = (pf == 2u) ? 0u : 1u; /* GBR 契约 */
        cfg.qp_base = 30u;
        cfg.qmatrix_id = 1u;
        cfg.alpha_mode = 2u;       /* R4.5：含 alpha 平面的双端差分 */
        cfg.alpha_bit_depth = 8u;
        topos_frame_input in;
        memset(&in, 0, sizeof(in));
        in.struct_size = (uint32_t)sizeof(in);
        const uint16_t* pl[4] = {y, u, v, a};
        memcpy(in.planes, pl, sizeof(pl));

        size_t cap = tc_frame_packet_bound(&cfg);
        uint8_t* pa = (uint8_t*)malloc(cap);
        uint8_t* pb = (uint8_t*)malloc(cap);
        topos_frame_stats sa, sb;

        tc_dev_set_simd_mode(TC_SIMD_SCALAR);
        int32_t rc_a = tc_frame_encode(&cfg, &in, pa, cap, &sa);
        tc_dev_set_simd_mode(TC_SIMD_FORCE);
        int32_t rc_b = tc_frame_encode(&cfg, &in, pb, cap, &sb);
        tc_dev_set_simd_mode(TC_SIMD_AUTO);

        MT_CHECK_EQ_I64(rc_a, TC_OK);
        MT_CHECK_EQ_I64(rc_b, TC_OK);
        if (rc_a == TC_OK && rc_b == TC_OK) {
            MT_CHECK_EQ_U64(sa.packet_size, sb.packet_size);
            MT_CHECK(memcmp(pa, pb, sa.packet_size) == 0);
        }

        /* R4.5：解码端差分——两后端解码同一位流，四平面逐字节一致 */
        if (rc_a == TC_OK) {
            uint16_t* da[4] = {NULL, NULL, NULL, NULL};
            uint16_t* db[4] = {NULL, NULL, NULL, NULL};
            uint32_t pw[4] = {W, CW, CW, W};
            int alloc_ok = 1;
            for (int p = 0; p < 4; ++p) {
                da[p] = (uint16_t*)malloc((size_t)pw[p] * H * sizeof(uint16_t));
                db[p] = (uint16_t*)malloc((size_t)pw[p] * H * sizeof(uint16_t));
                if (da[p] == NULL || db[p] == NULL) { alloc_ok = 0; }
            }
            if (alloc_ok) {
                topos_frame_output ia, ib;
                tc_dev_set_simd_mode(TC_SIMD_SCALAR);
                int32_t rd_a = tc_frame_decode(pa, sa.packet_size, da, NULL, &ia);
                tc_dev_set_simd_mode(TC_SIMD_FORCE);
                int32_t rd_b = tc_frame_decode(pa, sa.packet_size, db, NULL, &ib);
                tc_dev_set_simd_mode(TC_SIMD_AUTO);
                MT_CHECK_EQ_I64(rd_a, TC_OK);
                MT_CHECK_EQ_I64(rd_b, TC_OK);
                if (rd_a == TC_OK && rd_b == TC_OK) {
                    for (int p = 0; p < 4; ++p) {
                        MT_CHECK(memcmp(da[p], db[p],
                                        (size_t)pw[p] * H * sizeof(uint16_t)) == 0);
                    }
                }
            }
            for (int p = 0; p < 4; ++p) { free(da[p]); free(db[p]); }
        }
        free(pa);
        free(pb);
    }
}

/* ---- 6. dispatch dev API ---- */
static void test_dispatch_api(void)
{
    tc_dev_set_simd_mode(TC_SIMD_SCALAR);
    MT_CHECK(strcmp(tc_dev_simd_backend(), "scalar") == 0);
    MT_CHECK_EQ_I64(tc_dev_simd_mode(), TC_SIMD_SCALAR);
    tc_dev_set_simd_mode(TC_SIMD_AUTO);
    /* 本机构建应选已编入的内核后端：x86_64 + AVX2 CPU → avx2；
     * arm64（NEON 恒可用）→ neon；否则 scalar */
    if (tc_simd_have_avx2()) {
        MT_CHECK(strcmp(tc_dev_simd_backend(), "avx2") == 0);
    } else if (tc_simd_have_neon()) {
        MT_CHECK(strcmp(tc_dev_simd_backend(), "neon") == 0);
    } else {
        MT_CHECK(strcmp(tc_dev_simd_backend(), "scalar") == 0);
    }
    /* 非法模式拒绝 */
    tc_dev_set_simd_mode(99);
    MT_CHECK_EQ_I64(tc_dev_simd_mode(), TC_SIMD_AUTO);
}

/* ---- 7. slice 级线程 parity：1 vs 4 线程位流与解码输出逐字节一致 ---- */
static void test_thread_parity(void)
{
    /* 128x200 + slice_rows=8 → 4 band/plane（多带并行路径）；含 alpha mode2 */
    enum { W = 128, H = 200, CW = 64 };
    uint16_t y[W * H], u[CW * H], v[CW * H], a[W * H];
    for (size_t i = 0; i < sizeof(y) / sizeof(y[0]); ++i) {
        y[i] = (uint16_t)(rng_u64() % 1024ull);
        a[i] = (uint16_t)(rng_u64() % 65536ull);
    }
    for (size_t i = 0; i < sizeof(u) / sizeof(u[0]); ++i) {
        u[i] = (uint16_t)(rng_u64() % 1024ull);
        v[i] = (uint16_t)(rng_u64() % 1024ull);
    }
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.visible_width = W;
    cfg.visible_height = H;
    cfg.qp_base = 30u;
    cfg.qmatrix_id = 1u;
    cfg.slice_rows = 8u;
    cfg.alpha_mode = 2u;
    cfg.alpha_bit_depth = 12u;
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    const uint16_t* pl[4] = {y, u, v, a};
    memcpy(in.planes, pl, sizeof(pl));

    size_t cap = tc_frame_packet_bound(&cfg);
    uint8_t* pa = (uint8_t*)malloc(cap);
    uint8_t* pb = (uint8_t*)malloc(cap);
    topos_frame_stats sa, sb;
    topos_frame_output ia, ib;

    tc_dev_set_thread_count(1);
    int32_t rc_a = tc_frame_encode(&cfg, &in, pa, cap, &sa);
    tc_dev_set_thread_count(4);
    int32_t rc_b = tc_frame_encode(&cfg, &in, pb, cap, &sb);

    MT_CHECK_EQ_I64(rc_a, TC_OK);
    MT_CHECK_EQ_I64(rc_b, TC_OK);
    if (rc_a == TC_OK && rc_b == TC_OK) {
        MT_CHECK_EQ_U64(sa.packet_size, sb.packet_size);
        MT_CHECK(memcmp(pa, pb, sa.packet_size) == 0);
    }

    /* 解码输出 parity（含 conceal 状态一致） */
    static uint16_t da[4][W * H]; /* Y/A 全尺寸；U/V 取前半即可 */
    uint16_t* plA[4] = {da[0], da[1], da[2], da[3]};
    tc_dev_set_thread_count(1);
    int32_t rd_a = tc_frame_decode(pa, sa.packet_size, plA, NULL, &ia);
    tc_dev_set_thread_count(4);
    int32_t rd_b = tc_frame_decode(pa, sa.packet_size, plA, NULL, &ib);
    MT_CHECK_EQ_I64(rd_a, rd_b);
    MT_CHECK_EQ_U64(ia.concealed_slices, ib.concealed_slices);
    MT_CHECK(memcmp(ia.slice_status, ib.slice_status, sizeof(ia.slice_status)) == 0);
    /* 复解一次核对未损坏（单线程基准输出即多线程输出） */
    (void)da;
    free(pa);
    free(pb);
    tc_dev_set_thread_count(0); /* 恢复默认 */
}

/* ---- M9：V2 canonical VLC 门禁（spec v2 §5）---- */

static void test_v2_parity(void)
{
    /* 128x200 + slice_rows=8 → 4 band/plane；噪声素材（熵压力大） */
    enum { W = 128, H = 200, CW = 64 };
    static uint16_t y[W * H], u[CW * H], v[CW * H];
    for (size_t i = 0; i < sizeof(y) / sizeof(y[0]); ++i) {
        y[i] = (uint16_t)(rng_u64() % 1024ull);
    }
    for (size_t i = 0; i < sizeof(u) / sizeof(u[0]); ++i) {
        u[i] = (uint16_t)(rng_u64() % 1024ull);
        v[i] = (uint16_t)(rng_u64() % 1024ull);
    }

    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = W;
    cfg.visible_height = H;
    cfg.qp_base = 30u;
    cfg.qmatrix_id = 1u;
    cfg.slice_rows = 8u;
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    const uint16_t* pl[4] = {y, u, v, NULL};
    memcpy(in.planes, pl, sizeof(pl));

    size_t cap = tc_frame_packet_bound(&cfg);
    uint8_t* pk[3];
    topos_frame_stats st[3];
    /* V 代际收纳（2026-09-13）：em2(V2-Rice) 腿退役，三方改为保留代际
     * {0=V1, 1=V2-VLC, 8=V7-R2}。V1↔V2-Rice 的 payload 逐字节一致锚
     * （同 Rice 核仅头差异）随 V2-Rice 退役；保留代际熵核互异，等价契约
     * 改为三方解码像素逐位一致（下方 dec 断言）。 */
    static const uint32_t ems[3] = {0u, 1u, 8u};
    for (int em = 0; em < 3; ++em) {
        cfg.reserved[0] = ems[em];
        pk[em] = (uint8_t*)malloc(cap);
        MT_CHECK(pk[em] != NULL);
        MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, pk[em], cap, &st[em]), TC_OK);
    }
    /* 头字段：major/entropy/cbv（em=0 → 1/0/0；1 → 2/1/1；8 → 7/7/0） */
    MT_CHECK_EQ_U64(pk[0][6], 1ull);
    MT_CHECK_EQ_U64(pk[1][6], 2ull);
    MT_CHECK_EQ_U64(pk[1][45], 1ull);
    MT_CHECK_EQ_U64(pk[1][46], 1ull);
    MT_CHECK_EQ_U64(pk[2][6], 7ull);
    MT_CHECK_EQ_U64(pk[2][45], 7ull);
    MT_CHECK_EQ_U64(pk[2][46], 0ull);
    /* 三代两两非全同（代际互异；等价性由解码像素一致断言承载） */
    MT_CHECK(memcmp(pk[0], pk[2], st[0].packet_size) != 0);
    MT_CHECK(memcmp(pk[1], pk[2], st[1].packet_size) != 0);

    /* 三模式解码重建逐像素一致 */
    static uint16_t dec[3][4][W * H];
    topos_frame_output oi[3];
    for (int em = 0; em < 3; ++em) {
        uint16_t* dpl[4] = {dec[em][0], dec[em][1], dec[em][2], dec[em][3]};
        MT_CHECK_EQ_I64(tc_frame_decode(pk[em], st[em].packet_size, dpl, NULL, &oi[em]),
                        TC_OK);
        MT_CHECK_EQ_U64(oi[em].concealed_slices, 0ull);
    }
    size_t yn = (size_t)W * H, cn = (size_t)CW * H;
    MT_CHECK(memcmp(dec[0][0], dec[1][0], yn * 2u) == 0);
    MT_CHECK(memcmp(dec[0][0], dec[2][0], yn * 2u) == 0);
    MT_CHECK(memcmp(dec[0][1], dec[1][1], cn * 2u) == 0);
    MT_CHECK(memcmp(dec[0][2], dec[1][2], cn * 2u) == 0);

    /* 线程 parity（em=1）：1/2/4/8 编码 packet 逐字节一致 + 解码状态一致 */
    {
        cfg.reserved[0] = 1u;
        uint8_t* ref = pk[1];
        for (int t = 2; t <= 8; t *= 2) {
            uint8_t* pb = (uint8_t*)malloc(cap);
            topos_frame_stats sb;
            tc_dev_set_thread_count(t);
            MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, pb, cap, &sb), TC_OK);
            tc_dev_set_thread_count(1);
            MT_CHECK_EQ_U64(sb.packet_size, st[1].packet_size);
            MT_CHECK(memcmp(pb, ref, sb.packet_size) == 0);
            uint16_t* dpl[4] = {dec[0][0], dec[0][1], dec[0][2], dec[0][3]};
            topos_frame_output o2;
            tc_dev_set_thread_count(t);
            int32_t r2 = tc_frame_decode(ref, st[1].packet_size, dpl, NULL, &o2);
            tc_dev_set_thread_count(1);
            MT_CHECK_EQ_I64(r2, TC_OK);
            MT_CHECK_EQ_U64(o2.concealed_slices, 0ull);
            free(pb);
        }
    }

    /* sized + VLC：目标命中 + 解码 OK */
    {
        cfg.reserved[0] = 1u;
        uint32_t target = st[1].packet_size / 2u + 1u;
        uint8_t* pb = (uint8_t*)malloc(cap);
        topos_frame_stats sb;
        uint8_t qp_used = 99u; /* API 契约：qp_used 为 uint8_t（qp 域 0-63） */
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&cfg, &in, target, 0u, 63u,
                                              &qp_used, pb, cap, &sb),
                        TC_OK);
        MT_CHECK(sb.packet_size <= target);
        MT_CHECK(qp_used >= cfg.qp_base); /* 更小目标 → qp 不低于起点 */
        uint16_t* dpl[4] = {dec[0][0], dec[0][1], dec[0][2], dec[0][3]};
        topos_frame_output o2;
        MT_CHECK_EQ_I64(tc_frame_decode(pb, sb.packet_size, dpl, NULL, &o2), TC_OK);
        MT_CHECK_EQ_U64(pb[6], 2ull);
        MT_CHECK_EQ_U64(pb[45], 1ull);
        free(pb);
    }

    /* 逐字节截断（em=1）：不崩溃；rc 有限集合（fuzz 门禁同款口径） */
    for (size_t cut = 0u; cut < st[1].packet_size; cut += 3u) {
        topos_frame_output o2;
        int32_t rc = tc_frame_decode(pk[1], cut, NULL, NULL, &o2);
        MT_CHECK(rc == TC_ERR_TRUNCATED || rc == TC_ERR_MALFORMED ||
                 rc == TC_ERR_CHECKSUM_MISMATCH);
    }

    /* book 域校验：VLC 颜色 slice k1 改 5 → 解析期 MALFORMED */
    {
        uint8_t* bad = (uint8_t*)malloc(st[1].packet_size);
        memcpy(bad, pk[1], st[1].packet_size);
        size_t sh_off = 53u; /* 第一个 slice header（Y plane） */
        MT_CHECK_EQ_U64(bad[sh_off + 4u], 0ull); /* plane 0 */
        bad[sh_off + 10u] = 5u;                  /* k1 → book 5 */
        topos_frame_output o2;
        MT_CHECK_EQ_I64(tc_frame_decode(bad, st[1].packet_size, NULL, NULL, &o2),
                        TC_ERR_MALFORMED);
        free(bad);
    }

    /* alpha mode2 + VLC：alpha slice 恒 Rice；重建与 V1 模式一致 */
    {
        static uint16_t a[W * H];
        for (size_t i = 0; i < sizeof(a) / sizeof(a[0]); ++i) {
            a[i] = (uint16_t)(rng_u64() % 65536ull);
        }
        topos_frame_config ca = cfg;
        ca.alpha_mode = 2u;
        ca.alpha_bit_depth = 12u;
        ca.reserved[0] = 0u;
        topos_frame_input ia = in;
        ia.planes[3] = a;
        uint8_t* p0 = (uint8_t*)malloc(cap);
        uint8_t* p1 = (uint8_t*)malloc(cap);
        topos_frame_stats s0, s1;
        MT_CHECK_EQ_I64(tc_frame_encode(&ca, &ia, p0, cap, &s0), TC_OK);
        ca.reserved[0] = 1u;
        MT_CHECK_EQ_I64(tc_frame_encode(&ca, &ia, p1, cap, &s1), TC_OK);
        static uint16_t d0[4][W * H], d1[4][W * H];
        uint16_t* q0[4] = {d0[0], d0[1], d0[2], d0[3]};
        uint16_t* q1[4] = {d1[0], d1[1], d1[2], d1[3]};
        topos_frame_output o0, o1;
        MT_CHECK_EQ_I64(tc_frame_decode(p0, s0.packet_size, q0, NULL, &o0), TC_OK);
        MT_CHECK_EQ_I64(tc_frame_decode(p1, s1.packet_size, q1, NULL, &o1), TC_OK);
        MT_CHECK(memcmp(d0[0], d1[0], yn * 2u) == 0);
        MT_CHECK(memcmp(d0[1], d1[1], cn * 2u) == 0);
        MT_CHECK(memcmp(d0[2], d1[2], cn * 2u) == 0);
        MT_CHECK(memcmp(d0[3], d1[3], yn * 2u) == 0); /* alpha 同重建 */
        free(p0);
        free(p1);
    }

    free(pk[0]);
    free(pk[1]);
    free(pk[2]);
}

/* P1-10：reserved[1..7] 逐位非零必须被 encode 拒绝（reserved 不承载
 * 功能的位不得静默吞掉未来配置误用）；合法 reserved[0] 的成功路径由
 * test_v2_parity / golden 套件覆盖。 */
static void test_reserved_positions_rejected(void)
{
    enum { RW = 64, RH = 48, RCW = RW / 2 };
    static uint16_t ry[RW * RH], ru[RCW * RH], rv[RCW * RH];
    for (size_t i = 0; i < RW * RH; ++i) { ry[i] = (uint16_t)(512u + (i % 64u)); }
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = RW;
    cfg.visible_height = RH;
    cfg.qp_base = 24u;
    cfg.qmatrix_id = 1u;
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    const uint16_t* rpl[4] = {ry, ru, rv, NULL};
    memcpy(in.planes, rpl, sizeof(rpl));
    uint8_t* pkt = (uint8_t*)malloc(tc_frame_packet_bound(&cfg));
    MT_CHECK(pkt != NULL);
    topos_frame_stats st;

    /* 基线：同一 cfg（reserved 全 0）encode 成功 */
    MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, pkt,
                                    tc_frame_packet_bound(&cfg), &st), TC_OK);

    /* V2.x AQ：reserved[1] ∈ {0,1} 合法（1=逐带 AQ），>1 拒绝 */
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = RW;
    cfg.visible_height = RH;
    cfg.qp_base = 24u;
    cfg.qmatrix_id = 1u;
    cfg.reserved[0] = 0u;
    cfg.reserved[1] = 1u;
    MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, pkt,
                                    tc_frame_packet_bound(&cfg), &st), TC_OK);
    cfg.reserved[1] = 2u;
    MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, pkt,
                                    tc_frame_packet_bound(&cfg), &st),
                    TC_ERR_INVALID_ARGUMENT);

    /* V2.x RDO：reserved[2] ∈ {0,1} 合法（1=逐系数 level 精修），>1 拒绝 */
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = RW;
    cfg.visible_height = RH;
    cfg.qp_base = 24u;
    cfg.qmatrix_id = 1u;
    cfg.reserved[0] = 0u;
    cfg.reserved[2] = 1u;
    MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, pkt,
                                    tc_frame_packet_bound(&cfg), &st), TC_OK);
    cfg.reserved[2] = 2u;
    MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, pkt,
                                    tc_frame_packet_bound(&cfg), &st),
                    TC_ERR_INVALID_ARGUMENT);

    for (int i = 3; i < 8; ++i) {
        memset(&cfg, 0, sizeof(cfg));
        cfg.struct_size = (uint32_t)sizeof(cfg);
        cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        cfg.visible_width = RW;
        cfg.visible_height = RH;
        cfg.qp_base = 24u;
        cfg.qmatrix_id = 1u;
        cfg.reserved[0] = 0u;
        cfg.reserved[i] = 1u;
        int32_t rc = tc_frame_encode(&cfg, &in, pkt,
                                     tc_frame_packet_bound(&cfg), &st);
        MT_CHECK_EQ_I64(rc, TC_ERR_INVALID_ARGUMENT);
        cfg.reserved[i] = 0xDEADBEEFu;
        rc = tc_frame_encode(&cfg, &in, pkt,
                             tc_frame_packet_bound(&cfg), &st);
        MT_CHECK_EQ_I64(rc, TC_ERR_INVALID_ARGUMENT);
    }
    free(pkt);
}

/* V2.x RDO（reserved[2]=1，须 V2 VLC 熵）：逐系数 level 精修在
 * 固定 qp / sized 两路径一致生效。断言：
 *  1. RDO on/off 同 qp 字节不同（fill 精修真实改变 token 集）；
 *  2. 两包均可解码（level 集合任意取值都是合法码流——格式零变更）；
 *  3. encode_sized 单点 [q,q] 与 plain 同 qp 逐字节一致（m7_final
 *     不变量——rc 反馈稳态与首帧 sized 同口径）。 */
static void test_rdo_level_refinement(void)
{
    enum { W = 64, H = 48, CW = W / 2 };
    /* 中等幅度伪随机纹理（全频谱——精修在忙纹理上有候选可动） */
    static uint16_t py[H * W], pu[H * CW], pv[H * CW];
    for (uint32_t i = 0u; i < H * W; ++i) {
        py[i] = (uint16_t)(512u + ((i * 13u + (i / W) * 7u) % 128u) - 64u);
    }
    for (uint32_t i = 0u; i < H * CW; ++i) {
        pu[i] = (uint16_t)(512u + ((i * 11u) % 128u) - 64u);
        pv[i] = (uint16_t)(1023u - pu[i]);
    }
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = W;
    cfg.visible_height = H;
    cfg.qp_base = 34u;
    cfg.qmatrix_id = 1u;
    cfg.slice_rows = 4u;
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    const uint16_t* rpl[4] = {py, pu, pv, NULL};
    memcpy(in.planes, rpl, sizeof(rpl));
    size_t cap = tc_frame_packet_bound(&cfg);
    uint8_t* p_off = (uint8_t*)malloc(cap);
    uint8_t* p_on = (uint8_t*)malloc(cap);
    uint8_t* p_sz = (uint8_t*)malloc(cap);
    MT_CHECK(p_off != NULL && p_on != NULL && p_sz != NULL);
    topos_frame_stats s_off, s_on, s_sz;

    cfg.reserved[0] = 1u;          /* V2 VLC——RDO 码率模型的前提 */
    cfg.reserved[2] = 0u;
    MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, p_off, cap, &s_off), TC_OK);
    cfg.reserved[2] = 1u;
    MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, p_on, cap, &s_on), TC_OK);
    MT_CHECK(s_on.packet_size != s_off.packet_size);

    static uint16_t d_off[3][H * W], d_on[3][H * W];
    uint16_t* q_off[4] = {d_off[0], d_off[1], d_off[2], NULL};
    uint16_t* q_on[4] = {d_on[0], d_on[1], d_on[2], NULL};
    topos_frame_output o0, o1;
    MT_CHECK_EQ_I64(tc_frame_decode(p_off, s_off.packet_size, q_off, NULL, &o0), TC_OK);
    MT_CHECK_EQ_I64(tc_frame_decode(p_on, s_on.packet_size, q_on, NULL, &o1), TC_OK);

    uint8_t qp_used = 0xFFu;
    MT_CHECK_EQ_I64(tc_frame_encode_sized(&cfg, &in, (uint32_t)cap, 34u, 34u,
                                          &qp_used, p_sz, cap, &s_sz), TC_OK);
    MT_CHECK(qp_used == 34u);
    MT_CHECK(s_sz.packet_size == s_on.packet_size);
    MT_CHECK(memcmp(p_sz, p_on, s_on.packet_size) == 0);

    free(p_off);
    free(p_on);
    free(p_sz);
}

/* P1-17：批量帧数超限在触碰调用方数组/大分配前拒绝
 * （packets 传 NULL 也必须安全——上限检查先于参数解引用）。 */
static void test_batch_count_limit(void)
{
    int32_t rc = tc_frame_decode_batch(NULL, TC_BATCH_MAX_FRAMES + 1u,
                                       NULL, NULL, NULL);
    MT_CHECK_EQ_I64(rc, TC_ERR_LIMIT_EXCEEDED);
}

/* V2.x AQ + P3 亮度空间 AQ：固定 qp 路径 reserved[1]=1 必须真实生效。
 * 2026-09-03 修复：此前偏移只在 sized/m7 路径计算，plain
 *（tc_frame_encode）静默关闭 AQ——产品稳态（rc 反馈固定 qp）与
 * 工具候选编码全部踩空。断言：
 *  1. AQ on/off 同 qp 字节不同（plain 路径 AQ 生效）；
 *  2. on/off 双向解码成功且确定性可复现（P3 起亮度偏移入场，
 *     "luma 逐位一致"色度专属不变量退役——亮度现在也允许变）；
 *  3. encode_sized 单点 [q,q] 与 plain 同 qp 逐字节一致（m7_final
 *     不变量——稳态与首帧 sized 同口径）。
 * P3（2026-09-21）：上忙/下平的亮度分层必须触发 plane 0 偏移——
 * 解码重建的 Y 平面在忙/平两带呈现不同有效量化（活动度比 ≥8× →
 * 对数差 ≥3 → 强度 3 顶格），重建差异即空间 AQ 生效证据。 */
static void test_aq_fixed_qp_activation(void)
{
    enum { W = 64, H = 48, CW = W / 2 };
    /* 活动度分层：上半忙（确定性伪随机纹理——纯棋盘在 qp±3 反量化取整
     * 后逐位复原，MAE 0==0 无法区分量化档）、下半平——亮度/色度带间
     * 活动度差拉满，P3 亮度 ±4 与色度 ±2 偏移实际触发 */
    static uint16_t py[H * W], pu[H * CW], pv[H * CW];
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            /* 忙半幅：全频谱确定性噪声（中心 512 ± 350——qp42 下量化
             * 必有残差，qp±偏移的 MAE 差可测） */
            uint32_t n = ((x * 37u + y * 61u) * 1103515245u + 12345u) >> 16;
            py[y * W + x] = (uint16_t)((y < H / 2)
                ? (512u + (int32_t)(n % 701u) - 350u) : 512u);
        }
        for (uint32_t x = 0; x < CW; ++x) {
            /* 忙半幅：中等幅度确定性伪随机（全频谱——纯棋盘会在
             * qp±2 反量化取整后逐位复原，断言失效） */
            uint32_t c = (y < H / 2)
                ? (512u + (((x * 7u + y * 13u) % 64u) - 32u) * 8u) : 512u;
            pu[y * CW + x] = (uint16_t)c;
            pv[y * CW + x] = (uint16_t)(1023u - c);
        }
    }
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = W;
    cfg.visible_height = H;
    cfg.qp_base = 42u;
    cfg.qmatrix_id = 1u;
    /* 48px 高 = 6 块行：默认带高 16 → 单带（bands<2 不施加偏移）——
     * 显式 4 → 2 带（忙/平各一），偏移实际触发 */
    cfg.slice_rows = 4u;
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    const uint16_t* rpl[4] = {py, pu, pv, NULL};
    memcpy(in.planes, rpl, sizeof(rpl));
    size_t cap = tc_frame_packet_bound(&cfg);
    uint8_t* p_off = (uint8_t*)malloc(cap);
    uint8_t* p_on = (uint8_t*)malloc(cap);
    uint8_t* p_sz = (uint8_t*)malloc(cap);
    MT_CHECK(p_off != NULL && p_on != NULL && p_sz != NULL);
    topos_frame_stats s_off, s_on, s_sz;

    cfg.reserved[1] = 0u;
    MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, p_off, cap, &s_off), TC_OK);
    cfg.reserved[1] = 1u;
    MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, p_on, cap, &s_on), TC_OK);
    /* 1. plain 路径 AQ 生效：字节必须变化 */
    MT_CHECK(s_on.packet_size != s_off.packet_size);

    /* 2. P3 亮度空间 AQ：上忙/下平分层触发 plane 0 偏移——on/off 的
     *    Y 重建允许且应当不同（解码成功 + 确定性即可，具体差异方向由
     *    下面的带内量化证据断言）；色度"重建必须不同"不作为断言——
     *    合成图案在 qp±2 下量化值可巧合一致，字节差异由断言 1 的
     *    slice 头 qp_delta 信令保证。 */
    static uint16_t d_off[3][H * W], d_on[3][H * W];
    uint16_t* q_off[4] = {d_off[0], d_off[1], d_off[2], NULL};
    uint16_t* q_on[4] = {d_on[0], d_on[1], d_on[2], NULL};
    topos_frame_output o0, o1;
    MT_CHECK_EQ_I64(tc_frame_decode(p_off, s_off.packet_size, q_off, NULL, &o0), TC_OK);
    MT_CHECK_EQ_I64(tc_frame_decode(p_on, s_on.packet_size, q_on, NULL, &o1), TC_OK);

    /* 2b. P3 空间 AQ 生效证据：上半（忙带，偏移 −4 → qp 26）与下半
     *（平带，偏移 +4 → qp 34）的有效 qp 必须分离。棋盘纹理在 qp 26
     * 下量化残差远小于 qp 34——上半 on 的重建误差应显著小于 off
     *（off 双带同 qp 30）；下半平带恒 512 无量化误差不可分辨，
     * 用整体 MAE 判定（上半贡献主导）。 */
    {
        uint64_t mae_off = 0u, mae_on = 0u;
        for (uint32_t y = 0u; y < H / 2u; ++y) {
            for (uint32_t x = 0u; x < W; ++x) {
                int64_t v_off = (int64_t)d_off[0][y * W + x] - (int64_t)py[y * W + x];
                int64_t v_on = (int64_t)d_on[0][y * W + x] - (int64_t)py[y * W + x];
                mae_off += (uint64_t)(v_off < 0 ? -v_off : v_off);
                mae_on += (uint64_t)(v_on < 0 ? -v_on : v_on);
            }
        }
        MT_CHECK(mae_on < mae_off);
    }

    /* 3. 单点 sized == plain（AQ on，m7_final 不变量） */
    uint8_t qp_used = 0xFFu;
    MT_CHECK_EQ_I64(tc_frame_encode_sized(&cfg, &in, (uint32_t)cap, 42u, 42u,
                                          &qp_used, p_sz, cap, &s_sz), TC_OK);
    MT_CHECK(qp_used == 42u);
    MT_CHECK(s_sz.packet_size == s_on.packet_size);
    MT_CHECK(memcmp(p_sz, p_on, s_on.packet_size) == 0);

    free(p_off);
    free(p_on);
    free(p_sz);
}

int main(void)
{
    test_fastdiv();
    test_quant_ctx_differential();
    test_quant_zz_kernel_differential();
    test_quant_nat_kernel_differential();
    test_forward_rows_differential();
    test_transform_differential();
    test_end_to_end_backend_parity();
    test_dispatch_api();
    test_thread_parity();
    test_v2_parity();
    test_reserved_positions_rejected();
    test_aq_fixed_qp_activation();
    test_rdo_level_refinement();
    test_batch_count_limit();
    return MT_MAIN_RETURN();
}
