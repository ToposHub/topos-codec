/* P1-08：enc_shared 缓存命中/回退量化——计数与路径一一对应。
 *
 * 门：禁用 → fallback_disabled；启用单线程 → 恰好 hits；
 * bit-exact 与否由既有 golden/codec 套件承担，本测只验遥测语义。 */
#include "codec/codec.h"
#include "common/tpool.h"

#include <stdlib.h>
#include <string.h>

#include "image_synth.h"
#include "topos_codec.h"

#include "mini_test.h"

int main(void)
{
    image_synth_cfg ic = { 0x5EED00000000C900ull, 64u, 48u, TC_SYNTH_GRAIN };
    uint16_t *y, *u, *v, *a;
    if (image_synth_alloc(&ic, 0, &y, &u, &v, &a) != 0) { return 2; }
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = (uint32_t)sizeof cfg;
    cfg.visible_width = 64u;
    cfg.visible_height = 48u;
    cfg.qp_base = 24u;
    cfg.qmatrix_id = 1u;
    topos_frame_input in;
    memset(&in, 0, sizeof in);
    in.struct_size = (uint32_t)sizeof in;
    const uint16_t* pl[4] = { y, u, v, a };
    memcpy(in.planes, pl, sizeof(pl));
    size_t cap = tc_frame_packet_bound(&cfg);
    uint8_t* pkt = (uint8_t*)malloc(cap);
    if (pkt == NULL) { return 2; }

    topos_frame_stats st;
    tc_enc_cache_stats cs;

    /* 1) 禁用 → 栈实例，fallback_disabled 计数 */
    tc_dev_enc_cache_stats_reset();
    tc_dev_set_enc_cache(1);
    MT_CHECK(tc_frame_encode(&cfg, &in, pkt, cap, &st) == TC_OK);
    tc_dev_enc_cache_stats_get(&cs);
    MT_CHECK_EQ_U64(cs.fallback_disabled, 1u);
    MT_CHECK_EQ_U64(cs.hits, 0u);

    /* 2) 启用 → 单线程恰好命中缓存实例 */
    tc_dev_set_enc_cache(0);
    tc_dev_enc_cache_stats_reset();
    MT_CHECK(tc_frame_encode(&cfg, &in, pkt, cap, &st) == TC_OK);
    MT_CHECK(tc_frame_encode(&cfg, &in, pkt, cap, &st) == TC_OK);
    tc_dev_enc_cache_stats_get(&cs);
    MT_CHECK_EQ_U64(cs.hits, 2u);
    MT_CHECK_EQ_U64(cs.fallback_disabled, 0u);
    MT_CHECK_EQ_U64(cs.fallback_busy, 0u);
    MT_CHECK_EQ_U64(cs.fallback_alloc, 0u);

    /* 3) 两次编码 bit-exact（缓存复用不污染输出） */
    {
        uint8_t* pkt2 = (uint8_t*)malloc(cap);
        if (pkt2 == NULL) { return 2; }
        MT_CHECK(tc_frame_encode(&cfg, &in, pkt2, cap, &st) == TC_OK);
        MT_CHECK(memcmp(pkt, pkt2, st.packet_size) == 0);
        free(pkt2);
    }

    tc_dev_set_enc_cache(0);
    free(pkt);
    free(y);
    free(u);
    free(v);
    free(a);
    MT_MAIN_RETURN();
}
