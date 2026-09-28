/* C16（2026-09-27 检查计划）：公开模式域文档一致性测试。
 *
 * topos_codec.h 的 reserved[0] 注释是 SDK 使用者的唯一模式表；本测把它
 * 钉到实现：写域 {0,1,8,9,10,11} 合法、退役域 {2..7} 显式拒绝
 * （INVALID_ARGUMENT，不静默回落）、>11 拒绝、默认值 0 可编码。 */
#include "codec/codec.h"

#include <stdlib.h>
#include <string.h>

#include "image_synth.h"
#include "topos_codec.h"

#include "mini_test.h"

static void base_cfg(topos_frame_config* cfg, uint8_t em)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->struct_size = (uint32_t)sizeof *cfg;
    cfg->visible_width = 64u;
    cfg->visible_height = 48u;
    cfg->qp_base = 24u;
    cfg->qmatrix_id = 1u;
    cfg->reserved[0] = em;
}

int main(void)
{
    topos_frame_config cfg;

    /* 退役域 {2..7}：显式拒绝（文档 = V 代际收纳，仅解码） */
    for (uint8_t em = 2u; em <= 7u; ++em) {
        base_cfg(&cfg, em);
        if (tc_frame_config_validate(&cfg) != TC_ERR_INVALID_ARGUMENT) {
            mt_report(__FILE__, __LINE__, "retired mode accepted");
            return 1;
        }
    }

    /* >11：域外拒绝 */
    base_cfg(&cfg, 12u);
    MT_CHECK_EQ_I64(tc_frame_config_validate(&cfg), TC_ERR_INVALID_ARGUMENT);

    /* 写域 {0,1,8,9,10,11}：配置校验通过 */
    const uint8_t writable[] = { 0u, 1u, 8u, 9u, 10u, 11u };
    for (size_t i = 0; i < sizeof(writable) / sizeof(writable[0]); ++i) {
        base_cfg(&cfg, writable[i]);
        if (tc_frame_config_validate(&cfg) != TC_OK) {
            mt_report(__FILE__, __LINE__, "writable mode rejected");
            return 1;
        }
    }

    /* 默认值（0 = V1 流）示例配置可编码 */
    {
        image_synth_cfg ic = { 0x5EED00000000C416ull, 64u, 48u, TC_SYNTH_GRAIN };
        uint16_t *y, *u, *v, *a;
        if (image_synth_alloc(&ic, 0, &y, &u, &v, &a) != 0) { return 2; }
        const uint16_t* pl[4] = { y, u, v, a };
        topos_frame_input in;
        memset(&in, 0, sizeof in);
        in.struct_size = (uint32_t)sizeof in;
        memcpy(in.planes, pl, sizeof(pl));
        base_cfg(&cfg, 0u);
        size_t cap = tc_frame_packet_bound(&cfg);
        uint8_t* pkt = (uint8_t*)malloc(cap);
        topos_frame_stats st;
        if (pkt == NULL) { return 2; }
        MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, pkt, cap, &st), TC_OK);
        MT_CHECK(st.packet_size > 0u);
        free(pkt);
        free(y); free(u); free(v); free(a);
    }

    MT_MAIN_RETURN();
}
