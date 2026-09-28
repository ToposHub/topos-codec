/* test_rans —— rANS 核心原语差分测试（ADR-C034 配套）。
 *
 * 1. 模型构建：确定性归一化（Σfreq = 4096）、零计数零 freq、
 *    非零计数 freq ≥ 1、同输入同输出、单符号退化合法；
 * 2. 随机往返（多种子 × 多字母表 × 多分布 × 多模型混排）：逐符号一致；
 * 3. 流布局：[终态 4B][数据]、终态 ≥ L、截断流有限步内 TRUNCATED；
 * 4. 畸形域：域外符号/零 freq → INVALID_ARGUMENT；溢出 → BUFFER_TOO_SMALL；
 * 5. 均匀二值模型逐位恰 1 bit。 */
#include "entropy/rans.h"
#include "topos_codec.h"

#include "mini_test.h"

#include <string.h>

static uint32_t rng_state = 1u;
static uint32_t rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static void test_model_build(void)
{
    uint32_t cnt[8] = { 100, 50, 25, 12, 6, 3, 2, 1 };
    tc_rans_model m;
    MT_CHECK(tc_rans_model_build(&m, cnt, 8) == TC_OK);
    uint32_t sum = 0u;
    for (uint32_t s = 0u; s < 8u; ++s) {
        MT_CHECK(m.freq[s] > 0u);          /* 非零计数 → freq ≥ 1 */
        sum += m.freq[s];
    }
    MT_CHECK_EQ_U64(sum, TC_RANS_SCALE_TOTAL);
    MT_CHECK_EQ_U64(m.cum[8], TC_RANS_SCALE_TOTAL);

    tc_rans_model m2;
    MT_CHECK(tc_rans_model_build(&m2, cnt, 8) == TC_OK);
    MT_CHECK(memcmp(&m, &m2, sizeof(m)) == 0);

    /* 稀疏：零计数零 freq，两个活跃符号 */
    uint32_t sp[16] = { 0 };
    sp[3] = 999u;
    sp[11] = 1u;
    MT_CHECK(tc_rans_model_build(&m, sp, 16) == TC_OK);
    for (uint32_t s = 0u; s < 16u; ++s) {
        if (s != 3u && s != 11u) { MT_CHECK_EQ_U64(m.freq[s], 0u); }
        else { MT_CHECK(m.freq[s] > 0u); }
    }

    /* 单符号退化：freq = 4096（合法；流零数据字节） */
    uint32_t one[1] = { 7u };
    MT_CHECK(tc_rans_model_build(&m, one, 1) == TC_OK);
    MT_CHECK_EQ_U64(m.freq[0], TC_RANS_SCALE_TOTAL);

    uint32_t z[4] = { 0 };
    MT_CHECK(tc_rans_model_build(&m, z, 4) == TC_ERR_INVALID_ARGUMENT);
    MT_CHECK(tc_rans_model_build(&m, cnt, 0) == TC_ERR_INVALID_ARGUMENT);
    MT_CHECK(tc_rans_model_build(&m, cnt, 65) == TC_ERR_INVALID_ARGUMENT);
    MT_CHECK(tc_rans_model_build(&m, NULL, 4) == TC_ERR_INVALID_ARGUMENT);
}

static void test_roundtrip(void)
{
    for (uint32_t seed = 1u; seed <= 8u; ++seed) {
        rng_state = seed * 7919u + 13u;
        for (uint32_t trial = 0u; trial < 32u; ++trial) {
            const uint32_t syms = 2u + (rng() % 63u);
            uint32_t cnt[TC_RANS_MAX_SYMS] = { 0 };
            uint32_t active = 0u;
            for (uint32_t s = 0u; s < syms; ++s) {
                if ((rng() % 3u) != 0u) {
                    cnt[s] = 1u + (rng() % (1u << (rng() % 12u)));
                    ++active;
                }
            }
            if (active == 0u) { cnt[rng() % syms] = 1u; ++active; }
            tc_rans_model m;
            MT_CHECK(tc_rans_model_build(&m, cnt, syms) == TC_OK);

            tc_rans_model mu;
            tc_rans_model_uniform2(&mu);
            enum { N = 512 };
            uint32_t stream[N];
            uint32_t which[N];
            const tc_rans_model* models[2] = { &m, &mu };
            uint32_t valid_syms[TC_RANS_MAX_SYMS];
            uint32_t n_valid = 0u;
            for (uint32_t s = 0u; s < syms; ++s) {
                if (m.freq[s] > 0u) { valid_syms[n_valid++] = s; }
            }
            MT_CHECK_EQ_U64(n_valid, active);
            for (uint32_t i = 0u; i < N; ++i) {
                which[i] = (rng() % 4u == 0u) ? 1u : 0u;
                if (which[i] == 0u) { stream[i] = valid_syms[rng() % n_valid]; }
                else { stream[i] = rng() % 2u; }
            }

            uint8_t buf[8192];
            tc_rans_enc e;
            tc_rans_enc_init(&e, buf, sizeof(buf));
            for (int i = (int)N - 1; i >= 0; --i) {
                MT_CHECK(tc_rans_put(&e, models[which[i]], stream[i]) == TC_OK);
            }
            const int32_t len = tc_rans_enc_flush(&e);
            MT_CHECK(len >= (int32_t)TC_RANS_STATE_BYTES);
            MT_CHECK((size_t)len <= sizeof(buf));

            tc_rans_dec d;
            MT_CHECK(tc_rans_dec_init(&d, buf, (size_t)len) == TC_OK);
            for (uint32_t i = 0u; i < N; ++i) {
                uint32_t s = 0u;
                MT_CHECK(tc_rans_get(&d, models[which[i]], &s) == TC_OK);
                MT_CHECK_EQ_U64(s, stream[i]);
            }
        }
    }
}

static void test_layout_and_errors(void)
{
    uint32_t cnt[4] = { 8, 4, 2, 1 };
    tc_rans_model m;
    MT_CHECK(tc_rans_model_build(&m, cnt, 4) == TC_OK);

    uint8_t buf[64];
    tc_rans_enc e;
    tc_rans_enc_init(&e, buf, sizeof(buf));
    MT_CHECK(tc_rans_put(&e, &m, 2u) == TC_OK);
    MT_CHECK(tc_rans_put(&e, &m, 0u) == TC_OK);
    const int32_t len = tc_rans_enc_flush(&e);
    MT_CHECK(len >= 4);

    uint32_t x0 = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
                | ((uint32_t)buf[2] << 8) | buf[3];
    MT_CHECK(x0 >= TC_RANS_L);

    tc_rans_dec d;
    MT_CHECK(tc_rans_dec_init(&d, buf, (size_t)len) == TC_OK);
    uint32_t s = 0u;
    MT_CHECK(tc_rans_get(&d, &m, &s) == TC_OK);   /* 后向编码 → 前向解码 */
    MT_CHECK_EQ_U64(s, 0u);
    MT_CHECK(tc_rans_get(&d, &m, &s) == TC_OK);
    MT_CHECK_EQ_U64(s, 2u);
    int hit_trunc = 0;
    for (int i = 0; i < 64; ++i) {
        if (tc_rans_get(&d, &m, &s) == TC_ERR_TRUNCATED) { hit_trunc = 1; break; }
    }
    MT_CHECK(hit_trunc);

    tc_rans_dec d2;
    MT_CHECK(tc_rans_dec_init(&d2, buf, 3u) == TC_ERR_TRUNCATED);

    tc_rans_enc e2;
    tc_rans_enc_init(&e2, buf, sizeof(buf));
    MT_CHECK(tc_rans_put(&e2, &m, 4u) == TC_ERR_INVALID_ARGUMENT);
    uint32_t zcnt[4] = { 1, 0, 1, 1 };
    tc_rans_model mz;
    MT_CHECK(tc_rans_model_build(&mz, zcnt, 4) == TC_OK);
    MT_CHECK(tc_rans_put(&e2, &mz, 1u) == TC_ERR_INVALID_ARGUMENT);

    uint8_t tiny[5];
    tc_rans_enc e3;
    tc_rans_enc_init(&e3, tiny, sizeof(tiny));
    int overflowed = 0;
    for (int i = 0; i < 32; ++i) {
        if (tc_rans_put(&e3, &m, (uint32_t)(i & 3)) == TC_ERR_BUFFER_TOO_SMALL) {
            overflowed = 1;
            break;
        }
    }
    MT_CHECK(overflowed);
    MT_CHECK(tc_rans_enc_flush(&e3) == TC_ERR_BUFFER_TOO_SMALL);
}

static void test_uniform_bit_cost(void)
{
    tc_rans_model mu;
    tc_rans_model_uniform2(&mu);
    uint8_t buf[1024];
    tc_rans_enc e;
    tc_rans_enc_init(&e, buf, sizeof(buf));
    const uint32_t bits = 2048u;
    for (uint32_t i = 0u; i < bits; ++i) {
        MT_CHECK(tc_rans_put(&e, &mu, (i >> 3u) & 1u) == TC_OK);
    }
    const int32_t len = tc_rans_enc_flush(&e);
    MT_CHECK(len >= (int32_t)(bits / 8u + 4u) - 2);
    MT_CHECK(len <= (int32_t)(bits / 8u + 4u) + 2);
}

/* 混排往返（符号 + rawbits 旁路交替）：V7-R slice 的真实运算形态。
 * 配对不变量的回归钉子——b > 23 的分块（阈值下限）曾把状态清零致
 * 输出跌破 L（见 rans.h 推导注释）。 */
static void test_mixed_rawbits_roundtrip(void)
{
    for (int trial = 0; trial < 200; ++trial) {
        uint32_t cnt[8] = { 0 };
        int act = 0;
        for (int i = 0; i < 8; ++i) {
            cnt[i] = rng() % 100u;
            if (cnt[i] != 0u) { act++; }
        }
        if (act < 2) { cnt[0] = 50u; cnt[1] = 30u; }
        tc_rans_model m;
        if (tc_rans_model_build(&m, cnt, 8) != TC_OK) { continue; }
        enum { N = 64 };
        uint32_t syms[N], bs[N], vs[N];
        for (int i = 0; i < N; ++i) {
            uint32_t s;
            do { s = rng() % 8u; } while (m.freq[s] == 0u);
            syms[i] = s;
            bs[i] = rng() % 28u;
            vs[i] = bs[i] != 0u ? rng() & ((1u << bs[i]) - 1u) : 0u;
        }
        uint8_t buf[1 << 16];
        tc_rans_enc e;
        tc_rans_enc_init(&e, buf, sizeof(buf));
        int ok = 1;
        for (int i = N; i-- > 0;) {   /* 后向：bits 先、符号后 */
            if (tc_rans_put_rawbits(&e, bs[i], vs[i]) != TC_OK ||
                tc_rans_put(&e, &m, syms[i]) != TC_OK) {
                ok = 0;
                break;
            }
        }
        int32_t len = ok ? tc_rans_enc_flush(&e) : -1;
        MT_CHECK(ok != 0 && len > 0);
        if (!ok || len <= 0) { return; }
        tc_rans_dec d;
        MT_CHECK(tc_rans_dec_init(&d, buf, (size_t)len) == TC_OK);
        for (int i = 0; i < N; ++i) {
            uint32_t s = 0, v = 0;
            MT_CHECK(tc_rans_get(&d, &m, &s) == TC_OK);
            MT_CHECK_EQ_U64(s, syms[i]);
            MT_CHECK(tc_rans_get_rawbits(&d, bs[i], &v) == TC_OK);
            MT_CHECK_EQ_U64(v, vs[i]);
        }
        MT_CHECK_EQ_U64((uint64_t)d.pos, (uint64_t)d.size);
    }
}

/* LUT 解码与通用 get 逐符号一致（同模型同流）。 */
static void test_lut_equivalence(void)
{
    for (int trial = 0; trial < 50; ++trial) {
        uint32_t cnt[16] = { 0 };
        for (int i = 0; i < 16; ++i) { cnt[i] = rng() % 37u; }
        cnt[rng() % 16u] += 3u;
        tc_rans_model m;
        if (tc_rans_model_build(&m, cnt, 16) != TC_OK) { continue; }
        tc_rans_lut lut;
        tc_rans_lut_build(&lut, &m);
        /* Σfreq = T ⟹ LUT 无空洞：未覆盖槽必为填充错误 */
        for (uint32_t s = 0; s < m.syms; ++s) {
            MT_CHECK(m.freq[s] == 0u || lut.sym[m.cum[s]] == s);
        }
        enum { N = 256 };
        uint32_t syms[N];
        uint8_t buf[4096];
        tc_rans_enc e;
        tc_rans_enc_init(&e, buf, sizeof(buf));
        int ok = 1;
        for (int i = N; i-- > 0;) {
            uint32_t s;
            do { s = rng() % 16u; } while (m.freq[s] == 0u);
            syms[i] = s;
            if (tc_rans_put(&e, &m, s) != TC_OK) { ok = 0; break; }
        }
        int32_t len = ok ? tc_rans_enc_flush(&e) : -1;
        MT_CHECK(ok != 0 && len > 0);
        if (!ok || len <= 0) { return; }
        tc_rans_dec d1, d2;
        MT_CHECK(tc_rans_dec_init(&d1, buf, (size_t)len) == TC_OK);
        MT_CHECK(tc_rans_dec_init(&d2, buf, (size_t)len) == TC_OK);
        for (int i = 0; i < N; ++i) {
            uint32_t s1 = 0, s2 = 0;
            MT_CHECK(tc_rans_get(&d1, &m, &s1) == TC_OK);
            MT_CHECK(tc_rans_get_lut(&d2, &lut, &m, &s2) == TC_OK);
            MT_CHECK_EQ_U64(s1, s2);
            MT_CHECK_EQ_U64(s1, syms[i]);
        }
    }
}

int main(void)
{
    test_model_build();
    test_roundtrip();
    test_layout_and_errors();
    test_uniform_bit_cost();
    test_mixed_rawbits_roundtrip();
    test_lut_equivalence();
    return MT_MAIN_RETURN();
}
