/* 阶段 10：编码器 fuzz —— 对抗性但"合法域内"的输入打编码器（此前 fuzz 全在解码侧）。
 *
 * 输入字节驱动：几何（8..320，含非 8 倍数）/qp/qmatrix/alpha 模式与位深/像素值。
 * 不变量：
 *  1. encode rc ∈ 定义集（合法域配置必须 TC_OK）；
 *  2. 编码确定性：同输入重复编码逐字节相同（spec §11.1）；
 *  3. 产物可被自家的 decode 接受（rc ∈ {TC_OK, TC_WARN_CONCEALED}）；
 *  4. sized 变体：rc ∈ 定义集，qp_used ≤ qp_max，有硬迭代上界（必然返回）。
 * 堆不放大、不越界由 ASan 承担。
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "topos_codec.h"

static uint64_t s_rng;
static uint32_t rnd(uint32_t n) /* n ≥ 1 */
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng % n);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size <= 8u) { return 0; }
    s_rng = (uint64_t)data[0] | ((uint64_t)data[1] << 8) |
            ((uint64_t)data[2] << 16) | ((uint64_t)data[3] << 24) |
            ((uint64_t)data[4] << 32) | ((uint64_t)data[5] << 40);
    uint8_t geo_lo = data[6];
    uint8_t geo_hi = data[7];
    const uint8_t* px = data + 8;
    size_t px_size = size - 8u;

    uint32_t w = 8u + (uint32_t)(geo_lo % 313u);   /* 8..320，含非 8 倍数 */
    uint32_t h = 8u + (uint32_t)(geo_hi % 173u);

    topos_frame_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.visible_width = (uint16_t)w;
    cfg.visible_height = (uint16_t)h;
    cfg.qp_base = (uint8_t)rnd(64u);
    cfg.qmatrix_id = (uint8_t)rnd(4u);
    /* V 代际收纳（2026-09-13）：写域 {0=V1, 1=V2-VLC, 8=V7-R2}（em9/V8
     * sized 未落地，plain 编码走 fuzz_v8）；退役 em 2..7 由单元层拒绝
     * 测试钉死，不再进入随机编码面。 */
    {
        static const uint32_t ems[3] = {0u, 1u, 8u};
        cfg.reserved[0] = ems[rnd(3u)];
    }
    cfg.pixel_format = (uint8_t)rnd(3u);
    cfg.bit_depth = rnd(2u) != 0u ? 12u : 10u;
    cfg.profile = 3u;
    /* Keep the generated domain aligned with frame_header cross-rules: compact
     * matrices are format/profile-specific, not independent fuzz knobs. */
    if (cfg.qmatrix_id == 2u) {
        cfg.pixel_format = 2u;
        cfg.bit_depth = 12u;
        cfg.profile = 5u;
    } else if (cfg.qmatrix_id == 3u) {
        cfg.pixel_format = 0u;
        cfg.bit_depth = 10u;
    }
    cfg.color_matrix = cfg.pixel_format == 2u ? 0u : 1u;
    cfg.slice_rows = (uint8_t)(1u + rnd(8u));
    uint32_t cw = cfg.pixel_format == 0u ? (w + 1u) / 2u : w;
    uint32_t alpha_pick = rnd(6u);
    if (alpha_pick < 3u) {
        cfg.alpha_mode = 0u;
    } else if (alpha_pick == 3u) {
        cfg.alpha_mode = 1u;
        cfg.alpha_bit_depth = 16u;
    } else {
        cfg.alpha_mode = 2u;
        cfg.alpha_bit_depth = (uint8_t)(8u + 2u * rnd(3u)); /* 8/10/12 */
    }

    /* 像素平面：fuzz 字节流循环铺开（确定性，无未初始化读） */
    uint32_t n_color = w * h + 2u * cw * h;
    uint32_t n_alpha = cfg.alpha_mode != 0u ? w * h : 0u;
    uint32_t n_total = n_color + n_alpha;
    uint16_t* planes_mem = (uint16_t*)malloc((size_t)n_total * sizeof(uint16_t));
    if (planes_mem == NULL) { return 0; }
    for (uint32_t i = 0u; i < n_total; ++i) {
        uint8_t b0 = px[(2u * i) % px_size];
        uint8_t b1 = px[(2u * i + 1u) % px_size];
        planes_mem[i] = (uint16_t)((uint16_t)b0 << 8 | (uint16_t)b1);
        if (i < n_color) { planes_mem[i] &= (uint16_t)((1u << cfg.bit_depth) - 1u); }
    }
    topos_frame_input in;
    memset(&in, 0, sizeof in);
    in.struct_size = (uint32_t)sizeof(in);
    in.planes[0] = planes_mem;
    in.planes[1] = planes_mem + (size_t)w * h;
    in.planes[2] = planes_mem + (size_t)(w * h + cw * h);
    in.planes[3] = planes_mem + n_color;

    size_t cap = tc_frame_packet_bound(&cfg);
    if (cap == 0u) { free(planes_mem); return 0; }
    uint8_t* pkt1 = (uint8_t*)malloc(cap);
    uint8_t* pkt2 = (uint8_t*)malloc(cap);
    if (pkt1 == NULL || pkt2 == NULL) { free(pkt1); free(pkt2); free(planes_mem); return 0; }

    topos_frame_stats st1;
    int32_t rc = tc_frame_encode(&cfg, &in, pkt1, cap, &st1);
    if (rc != TC_OK) {
        goto invariant;
    } /* 合法域配置必须成功 */

    /* 确定性 */
    {
        topos_frame_stats st2;
        if (tc_frame_encode(&cfg, &in, pkt2, cap, &st2) != TC_OK) {
            goto invariant;
        }
        if (st1.packet_size != st2.packet_size ||
            memcmp(pkt1, pkt2, st1.packet_size) != 0) {
            goto invariant;
        }
    }

    /* Full reconstruction round-trip. */
    {
        topos_frame_output info;
        uint16_t* decoded = (uint16_t*)malloc((size_t)n_total * sizeof(uint16_t));
        if (decoded == NULL) { free(pkt1); free(pkt2); free(planes_mem); return 0; }
        uint16_t* out[4] = {decoded, decoded + (size_t)w * h,
                            decoded + (size_t)(w * h + cw * h), decoded + n_color};
        int32_t drc = tc_frame_decode(pkt1, st1.packet_size, out, NULL, &info);
        free(decoded);
        if (drc != TC_OK) {
            goto invariant;
        }
    }

    /* sized 变体（有界迭代；目标尺寸取随机比例制造多轮搜索） */
    {
        topos_frame_stats ss;
        uint8_t qp_used = 0;
        uint32_t target = st1.packet_size / 4u + (uint32_t)rnd(st1.packet_size / 2u + 1u);
        if (target == 0u) { target = 1u; }
        uint8_t qmin = 2u;
        uint8_t qmax = 63u;
        int32_t src_rc = tc_frame_encode_sized(&cfg, &in, target, qmin, qmax,
                                               &qp_used, pkt2, cap, &ss);
        if (src_rc != TC_OK) {
            goto invariant;
        }
        if (qp_used < qmin || qp_used > qmax) {
            goto invariant;
        }
    }

    free(pkt1);
    free(pkt2);
    free(planes_mem);
    return 0;

invariant:
    free(pkt1);
    free(pkt2);
    free(planes_mem);
    abort(); /* libFuzzer ignores callback return values; invariants must crash. */
}
