/* C04（2026-09-27 检查计划）：编码容量契约哨兵测试——
 *
 * 头文件契约（topos_codec.h §285）：out_cap 不足 → TC_ERR_BUFFER_TOO_SMALL
 * 且**不写 out**。历史实现边装配边拷贝（asm_buf 直写 out），容量不足返回
 * 时 out 已被部分改写。修复后装配全程写内部 grow-only 暂存，成功才一次性
 * 提交调用方 out。
 *
 * 本测用哨兵字节验证错误返回路径的原子性：
 *   - out_cap = 0（out == NULL dry-run）：报告精确所需尺寸；
 *   - out_cap = 所需-1：全部 out 字节不变；
 *   - out_cap = 所需：成功且与参考包逐字节一致；
 *   - stats.packet_size 在错误/成功两种返回下均正确；
 *   - 覆盖多 slice、alpha、sized（m7 final 组装）公开入口。 */
#include "codec/codec.h"

#include <stdlib.h>
#include <string.h>

#include "image_synth.h"
#include "topos_codec.h"

#include "mini_test.h"

#define SENTINEL 0xA5u

static void fill_input(topos_frame_input* in, const uint16_t* const pl[4])
{
    memset(in, 0, sizeof *in);
    in->struct_size = (uint32_t)sizeof *in;
    memcpy(in->planes, pl, sizeof(in->planes));
}

/* 哨兵断言：buf 全 len 字节仍为 SENTINEL */
static int all_sentinel(const uint8_t* buf, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] != SENTINEL) { return 0; }
    }
    return 1;
}

static void probe_case(const topos_frame_config* cfg, const topos_frame_input* in)
{
    size_t cap = tc_frame_packet_bound(cfg);
    uint8_t* ref = (uint8_t*)malloc(cap);
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (ref == NULL || buf == NULL) { exit(2); }

    /* 参考包（完整容量） */
    topos_frame_stats st;
    memset(&st, 0, sizeof st);
    MT_CHECK_EQ_I64(tc_frame_encode(cfg, in, ref, cap, &st), TC_OK);
    const size_t needed = st.packet_size;
    MT_CHECK(needed > 0u && needed <= cap);

    /* 1) dry-run：out == NULL / cap == 0 → 报告精确所需，不崩 */
    memset(&st, 0, sizeof st);
    MT_CHECK_EQ_I64(tc_frame_encode(cfg, in, NULL, 0u, &st),
                    TC_ERR_BUFFER_TOO_SMALL);
    MT_CHECK_EQ_U64(st.packet_size, (uint32_t)needed);

    /* 2) 所需-1：错误返回，out 全字节不变，packet_size 正确 */
    memset(buf, SENTINEL, cap);
    memset(&st, 0, sizeof st);
    MT_CHECK_EQ_I64(tc_frame_encode(cfg, in, buf, needed - 1u, &st),
                    TC_ERR_BUFFER_TOO_SMALL);
    MT_CHECK_EQ_U64(st.packet_size, (uint32_t)needed);
    MT_CHECK(all_sentinel(buf, cap));

    /* 3) 所需：成功，且与参考包逐字节一致 */
    memset(buf, SENTINEL, cap);
    memset(&st, 0, sizeof st);
    MT_CHECK_EQ_I64(tc_frame_encode(cfg, in, buf, needed, &st), TC_OK);
    MT_CHECK_EQ_U64(st.packet_size, (uint32_t)needed);
    MT_CHECK(memcmp(buf, ref, needed) == 0);

    free(ref);
    free(buf);
}

int main(void)
{
    /* —— 多 slice（64×48，rows16 → 3 slice）无 alpha —— */
    {
        image_synth_cfg ic = { 0x5EED00000000C404ull, 64u, 48u, TC_SYNTH_GRAIN };
        uint16_t *y, *u, *v, *a;
        if (image_synth_alloc(&ic, 0, &y, &u, &v, &a) != 0) { return 2; }
        const uint16_t* pl[4] = { y, u, v, a };
        topos_frame_input in;
        fill_input(&in, pl);
        topos_frame_config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.struct_size = (uint32_t)sizeof cfg;
        cfg.visible_width = 64u;
        cfg.visible_height = 48u;
        cfg.qp_base = 24u;
        cfg.qmatrix_id = 1u;
        probe_case(&cfg, &in);
        free(y); free(u); free(v); free(a);
    }

    /* —— alpha（mode1 a16）—— */
    {
        image_synth_cfg ic = { 0x5EED00000000C405ull, 64u, 48u, TC_SYNTH_GRAIN };
        uint16_t *y, *u, *v, *a;
        if (image_synth_alloc(&ic, 1, &y, &u, &v, &a) != 0) { return 2; }
        const uint16_t* pl[4] = { y, u, v, a };
        topos_frame_input in;
        fill_input(&in, pl);
        topos_frame_config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.struct_size = (uint32_t)sizeof cfg;
        cfg.visible_width = 64u;
        cfg.visible_height = 48u;
        cfg.qp_base = 24u;
        cfg.qmatrix_id = 1u;
        cfg.alpha_mode = 1u;
        cfg.alpha_bit_depth = 16u;
        probe_case(&cfg, &in);
        free(y); free(u); free(v); free(a);
    }

    /* —— sized（m7 final 组装路径）—— */
    {
        image_synth_cfg ic = { 0x5EED00000000C406ull, 64u, 48u, TC_SYNTH_GRAIN };
        uint16_t *y, *u, *v, *a;
        if (image_synth_alloc(&ic, 0, &y, &u, &v, &a) != 0) { return 2; }
        const uint16_t* pl[4] = { y, u, v, a };
        topos_frame_input in;
        fill_input(&in, pl);
        topos_frame_config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.struct_size = (uint32_t)sizeof cfg;
        cfg.visible_width = 64u;
        cfg.visible_height = 48u;
        cfg.qp_base = 24u;
        cfg.qmatrix_id = 1u;

        size_t cap = tc_frame_packet_bound(&cfg);
        uint8_t* ref = (uint8_t*)malloc(cap);
        uint8_t* buf = (uint8_t*)malloc(cap);
        if (ref == NULL || buf == NULL) { return 2; }
        uint8_t qp_used = 0;
        topos_frame_stats st;

        /* 参考 sized 包（宽松预算 → qp_min） */
        memset(&st, 0, sizeof st);
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&cfg, &in, (uint32_t)cap,
                                              12u, 40u, &qp_used,
                                              ref, cap, &st), TC_OK);
        const size_t needed = st.packet_size;
        MT_CHECK(needed > 0u);

        /* dry-run */
        memset(&st, 0, sizeof st);
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&cfg, &in, (uint32_t)cap,
                                              12u, 40u, &qp_used,
                                              NULL, 0u, &st),
                        TC_ERR_BUFFER_TOO_SMALL);
        MT_CHECK_EQ_U64(st.packet_size, (uint32_t)needed);

        /* 所需-1：out 原子不变 */
        memset(buf, SENTINEL, cap);
        memset(&st, 0, sizeof st);
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&cfg, &in, (uint32_t)cap,
                                              12u, 40u, &qp_used,
                                              buf, needed - 1u, &st),
                        TC_ERR_BUFFER_TOO_SMALL);
        MT_CHECK_EQ_U64(st.packet_size, (uint32_t)needed);
        MT_CHECK(all_sentinel(buf, cap));

        /* 所需：成功 bit-exact */
        memset(buf, SENTINEL, cap);
        memset(&st, 0, sizeof st);
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&cfg, &in, (uint32_t)cap,
                                              12u, 40u, &qp_used,
                                              buf, needed, &st), TC_OK);
        MT_CHECK_EQ_U64(st.packet_size, (uint32_t)needed);
        MT_CHECK(memcmp(buf, ref, needed) == 0);

        free(ref);
        free(buf);
        free(y); free(u); free(v); free(a);
    }

    /* —— C05：sized 回退搜索不被中间 QP 容量错误终止 ——
     * 复现（2026-09-27 计划）：64×48 输入，目标/容量 = qp_max 的包大小；
     * 历史缺陷在 M7 禁用（回退线性搜索）时首探 QP20 包 24160 B 超
     * 容量 829 B → 直接 -15/QP20，QP95=829 B 可命中却不尝试。 */
    {
        image_synth_cfg ic = { 0x5EED00000000C407ull, 64u, 48u, TC_SYNTH_GRAIN };
        uint16_t *y, *u, *v, *a;
        if (image_synth_alloc(&ic, 0, &y, &u, &v, &a) != 0) { return 2; }
        const uint16_t* pl[4] = { y, u, v, a };
        topos_frame_input in;
        fill_input(&in, pl);
        topos_frame_config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.struct_size = (uint32_t)sizeof cfg;
        cfg.visible_width = 64u;
        cfg.visible_height = 48u;
        cfg.qp_base = 20u; /* 搜索种子 = 最粗画质 → 首探包最大（超容量） */
        cfg.qmatrix_id = 1u;

        size_t cap = tc_frame_packet_bound(&cfg);
        uint8_t* ref_hi = (uint8_t*)malloc(cap);
        uint8_t* buf = (uint8_t*)malloc(cap);
        if (ref_hi == NULL || buf == NULL) { return 2; }
        topos_frame_stats st;
        uint8_t qp_used = 0;

        /* qp_max 的最小包尺寸（回收/命中下界） */
        cfg.qp_base = 95u;
        MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, ref_hi, cap, &st), TC_OK);
        const uint32_t bytes_hi = st.packet_size;
        cfg.qp_base = 20u;

        /* M7 禁用（strict 不可用 → 回退线性搜索）：目标/容量 = bytes_hi。
         * 首探 QP20 超容量 → 不得终止；应命中 qp_max。 */
        tc_dev_set_sized_m7(1);
        memset(&st, 0, sizeof st);
        memset(buf, SENTINEL, cap);
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&cfg, &in, bytes_hi,
                                              20u, 95u, &qp_used,
                                              buf, bytes_hi, &st), TC_OK);
        MT_CHECK_EQ_U64(st.packet_size, bytes_hi);
        MT_CHECK_EQ_U64(qp_used, 95u);
        MT_CHECK(memcmp(buf, ref_hi, bytes_hi) == 0);

        /* 最终包超容量才 -15：容量 = bytes_hi - 1 */
        memset(&st, 0, sizeof st);
        memset(buf, SENTINEL, cap);
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&cfg, &in, bytes_hi,
                                              20u, 95u, &qp_used,
                                              buf, bytes_hi - 1u, &st),
                        TC_ERR_BUFFER_TOO_SMALL);
        MT_CHECK(all_sentinel(buf, cap));

        /* M7 启用：同输入同目标，命中包与回退语义一致（尺寸 ≤ 目标） */
        tc_dev_set_sized_m7(0);
        memset(&st, 0, sizeof st);
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&cfg, &in, bytes_hi,
                                              20u, 95u, &qp_used,
                                              buf, cap, &st), TC_OK);
        MT_CHECK(st.packet_size <= bytes_hi);

        tc_dev_set_sized_m7(0);
        free(ref_hi);
        free(buf);
        free(y); free(u); free(v); free(a);
    }

    MT_MAIN_RETURN();
}
