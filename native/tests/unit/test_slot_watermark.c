/* N03：线程数先高后低再恢复时，enc_slots_reserve 的水位不得为未重建的
 * 槽位虚报容量。旧实现两处同源缺陷：
 *   - tok 组：重建循环只覆盖 w < want，成功后却无条件抬
 *     tok_dc_built/tok_pair_built；
 *   - qbuf/dc 组：need_grow 时同样只重建 w < want，水位更新却为
 *     slot_n 内全部槽背书新容量。
 * 进程级线程数被 R6 策略临时调低的编码只在低 want 下扩容；恢复高线程
 * 数的消费方（m7 探针/条带任务按当前线程数取槽）拿到旧尺寸小数组 →
 * 越界写堆（组合回归 SIGSEGV/同序列摘要不一致的根因）。
 *
 * 门：16 线程小几何（全槽小容量）→ 2 线程大几何（增容）→ 恢复 16 线程，
 * tc_dev_enc_slots_validate（malloc_size 实测，不信水位）全程必须 0，
 * 且高线程大几何编码正常返回。 */
#include "codec/codec.h"
#include "common/tpool.h"

#include <stdlib.h>
#include <string.h>

#include "image_synth.h"
#include "topos_codec.h"

#include "mini_test.h"

static int32_t encode_one(uint32_t w, uint32_t h, uint64_t seed)
{
    image_synth_cfg ic = { seed, w, h, TC_SYNTH_GRAIN, 0u, 0u };
    uint16_t *y, *u, *v, *a;
    if (image_synth_alloc(&ic, 0, &y, &u, &v, &a) != 0) { return -1; }
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = (uint32_t)sizeof cfg;
    cfg.visible_width = w;
    cfg.visible_height = h;
    cfg.qp_base = 24u;
    cfg.qmatrix_id = 1u;
    topos_frame_input in;
    memset(&in, 0, sizeof in);
    in.struct_size = (uint32_t)sizeof in;
    const uint16_t* pl[4] = { y, u, v, a };
    memcpy(in.planes, pl, sizeof(pl));
    size_t cap = tc_frame_packet_bound(&cfg);
    uint8_t* pkt = (uint8_t*)malloc(cap);
    if (pkt == NULL) {
        free(y); free(u); free(v); free(a);
        return -1;
    }
    topos_frame_stats st;
    int32_t rc = tc_frame_encode(&cfg, &in, pkt, cap, &st);
    free(pkt);
    free(y); free(u); free(v); free(a);
    return rc;
}

int main(void)
{
    tc_dev_set_enc_cache(0);   /* 进程缓存实例（常驻槽位的载体，默认启用） */

    /* 1) 高线程 + 小几何：全部常驻槽按小容量建立 */
    tc_dev_set_thread_count(TC_SLICE_MAX_THREADS);
    MT_CHECK(encode_one(64u, 48u, 0x5EED00000000C901ull) == TC_OK);
    MT_CHECK_EQ_I64(tc_dev_enc_slots_validate(), 0);

    /* 2) 低线程 + 大几何：容量增长须惠及全部已建槽（水位不得虚报） */
    tc_dev_set_thread_count(2);
    MT_CHECK(encode_one(640u, 336u, 0x5EED00000000C902ull) == TC_OK);
    MT_CHECK_EQ_I64(tc_dev_enc_slots_validate(), 0);

    /* 3) 恢复高线程：need_grow 判定不被虚报水位骗过；大几何高线程
     * 编码（消费方取全部槽位）正常返回。 */
    tc_dev_set_thread_count(TC_SLICE_MAX_THREADS);
    MT_CHECK_EQ_I64(tc_dev_enc_slots_validate(), 0);
    MT_CHECK(encode_one(640u, 336u, 0x5EED00000000C903ull) == TC_OK);
    MT_CHECK_EQ_I64(tc_dev_enc_slots_validate(), 0);

    /* 4) 中线程再增容（tok 尺寸轴另一档）后回到高线程 */
    tc_dev_set_thread_count(4);
    MT_CHECK(encode_one(1152u, 656u, 0x5EED00000000C904ull) == TC_OK);
    MT_CHECK_EQ_I64(tc_dev_enc_slots_validate(), 0);
    tc_dev_set_thread_count(TC_SLICE_MAX_THREADS);
    MT_CHECK(encode_one(1152u, 656u, 0x5EED00000000C905ull) == TC_OK);
    MT_CHECK_EQ_I64(tc_dev_enc_slots_validate(), 0);

    MT_MAIN_RETURN();
}
