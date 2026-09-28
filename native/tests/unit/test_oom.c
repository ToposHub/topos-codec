/* OOM 穷举（阶段 10）：tc_dev_set_alloc_fault 逐点注入 —— 每条 API 路径在
 * "第 n 次分配失败"（n = 1..N 全枚举）下必须返回 TC_ERR_OUT_OF_MEMORY，
 * 不崩溃、状态可恢复；泄漏由同二进制的 ASan 配置复跑承担。
 * 终止条件：故障点超过该操作实际分配数 → 操作返回 TC_OK。 */
#include "common/alloc.h"
#include "common/tpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fault_io.h"
#include "image_synth.h"
#include "packet_synth.h"
#include "topos_codec.h"

#include "mini_test.h"

/* ---- 通用 sweep：每次注入一个故障点，rc 必须落在 allowed 集 ---- */

static int64_t sweep_run(int64_t max_n, int (*fn)(void*, int32_t* rc_out), void* ctx,
                         const int32_t* allowed, int n_allowed)
{
    for (int64_t n = 1; n <= max_n; ++n) {
        tc_dev_set_alloc_fault(n);
        int32_t rc = 0;
        fn(ctx, &rc);
        tc_dev_set_alloc_fault(-1);
        if (rc == TC_OK) { return n - 1; } /* 该操作仅用 n-1 次分配 → 全部点已覆盖 */
        int ok = 0;
        for (int i = 0; i < n_allowed; ++i) { if (allowed[i] == rc) { ok = 1; } }
        if (!ok) {
            fprintf(stderr, "[oom-sweep] n=%lld rc=%d last_error=%s\n",
                    (long long)n, (int)rc, tc_last_error());
            mt_report(__FILE__, __LINE__, "OOM sweep: unexpected rc (非 OOM/OK)");
            return -1;
        }
    }
    mt_report(__FILE__, __LINE__, "OOM sweep: 超过 max_n 仍未干净成功");
    return -1;
}

/* ---- 帧路径：encode / encode_sized / decode ---- */

static uint16_t *s_y, *s_u, *s_v, *s_a;
static topos_frame_config s_cfg;
static topos_frame_input s_in;
static uint8_t* s_pkt;
static size_t s_pkt_cap, s_pkt_size;
static uint16_t* s_out[4];

static void frame_setup(void)
{
    image_synth_cfg ic = { 0x5EED00000000A000ull, 64u, 48u, TC_SYNTH_GRAIN };
    if (image_synth_alloc(&ic, 1, &s_y, &s_u, &s_v, &s_a) != 0) { exit(2); }
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.struct_size = (uint32_t)sizeof(s_cfg);
    s_cfg.visible_width = 64u;
    s_cfg.visible_height = 48u;
    s_cfg.qp_base = 24u;
    s_cfg.qmatrix_id = 1u;
    s_cfg.alpha_mode = 2u;
    s_cfg.alpha_bit_depth = 12u;
    memset(&s_in, 0, sizeof s_in);
    s_in.struct_size = (uint32_t)sizeof(s_in);
    const uint16_t* pl[4] = { s_y, s_u, s_v, s_a };
    memcpy(s_in.planes, pl, sizeof(pl));
    s_pkt_cap = tc_frame_packet_bound(&s_cfg);
    s_pkt = (uint8_t*)malloc(s_pkt_cap);
    if (s_pkt == NULL) { exit(2); }
    topos_frame_stats st;
    if (tc_frame_encode(&s_cfg, &s_in, s_pkt, s_pkt_cap, &st) != TC_OK) { exit(2); }
    s_pkt_size = st.packet_size;
    topos_frame_output info;
    if (tc_frame_decode(s_pkt, s_pkt_size, NULL, NULL, &info) != TC_OK) { exit(2); }
    for (uint32_t p = 0; p < info.plane_count; ++p) {
        uint32_t w = 0, h = 0;
        if (tc_frame_plane_geometry(&info, p, &w, &h) != TC_OK) { exit(2); }
        s_out[p] = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
        if (s_out[p] == NULL) { exit(2); }
    }
}

static int op_encode(void* ctx, int32_t* rc_out)
{
    (void)ctx;
    topos_frame_stats st;
    *rc_out = tc_frame_encode(&s_cfg, &s_in, s_pkt, s_pkt_cap, &st);
    return 0;
}

static int op_encode_sized(void* ctx, int32_t* rc_out)
{
    (void)ctx;
    topos_frame_stats st;
    uint8_t qp_used = 0;
    *rc_out = tc_frame_encode_sized(&s_cfg, &s_in, (uint32_t)(s_pkt_size / 2u),
                                    4u, 60u, &qp_used, s_pkt, s_pkt_cap, &st);
    return 0;
}

static int op_decode(void* ctx, int32_t* rc_out)
{
    (void)ctx;
    topos_frame_output info;
    *rc_out = tc_frame_decode(s_pkt, s_pkt_size, s_out, NULL, &info);
    return 0;
}

/* ---- 容器路径：mux / open / faststart ---- */

typedef struct mux_env {
    topos_io io;
    tio_sink sink;
    tio_fault fault;
    const uint8_t *p1, *p2;
    size_t p1_size, p2_size;
} mux_env;

static uint8_t *s_p1, *s_p2;
static size_t s_p1_size, s_p2_size;

static int op_mux(void* ctx, int32_t* rc_out)
{
    mux_env* e = (mux_env*)ctx;
    topos_movie_config mc;
    memset(&mc, 0, sizeof mc);
    mc.struct_size = (uint32_t)sizeof(mc);
    mc.abi_version = TOPOS_CODEC_ABI_VERSION;
    mc.visible_width = 16u;
    mc.visible_height = 16u;
    mc.profile = 3u;        /* v1 仅 Standard */
    mc.pixel_format = 0u;   /* YUV 4:2:2 */
    mc.bit_depth = 10u;
    mc.qp_base = 20u; /* 与 PACKET_SYNTH_CFG_TINY 一致（tpcC 规则） */
    mc.qmatrix_id = 0u;
    mc.color_range = 1u;      /* full */
    mc.color_primaries = 1u;  /* bt709 */
    mc.color_transfer = 1u;
    mc.color_matrix = 1u;
    mc.timescale = 24000u;
    tio_sink_reset(&e->sink);
    topos_mux* mux = NULL;
    int32_t rc = tc_mux_create(&mc, &e->io, &mux);
    if (rc == TC_OK) { rc = tc_mux_add_packet(mux, e->p1, e->p1_size, 0u, 1000u); }
    if (rc == TC_OK) { rc = tc_mux_add_packet(mux, e->p2, e->p2_size, 1000u, 1000u); }
    if (rc == TC_OK) { rc = tc_mux_finish(mux); }
    tc_mux_free(mux); /* NULL 安全 */
    *rc_out = rc;
    return 0;
}

typedef struct open_env {
    topos_io io;
    tio_source src;
} open_env;

static int op_open(void* ctx, int32_t* rc_out)
{
    open_env* e = (open_env*)ctx;
    topos_movie* mov = NULL;
    int32_t rc = tc_movie_open(&e->io, &mov);
    tc_movie_close(mov); /* NULL 安全性同 mux_free */
    *rc_out = rc;
    return 0;
}

typedef struct fs_env {
    topos_io src_io, dst_io;
    tio_source src;
    tio_sink dst;
} fs_env;

static int op_faststart(void* ctx, int32_t* rc_out)
{
    fs_env* e = (fs_env*)ctx;
    tio_sink_reset(&e->dst);
    *rc_out = tc_movie_faststart(&e->src_io, &e->dst_io);
    return 0;
}

/* ---- O4（复验 2026-08-31）：P1-16 槽位首次扩容 OOM sweep ---- */

static uint16_t *s_gy, *s_gu, *s_gv, *s_ga;
static topos_frame_config s_gcfg;
static topos_frame_input s_gin;
static uint8_t* s_gpkt;
static size_t s_gcap;

static void o4_grow_setup(void)
{
    /* 更大几何（320×240）：4 线程下 enc_slots_reserve 首次扩容触发
     * qbuf/dca/dcb/rbuf ×3 槽的完整分配序列——此前 sweep 只用 64×48
     * 且池在注入前已被同几何预热（复验 P1-16 证据） */
    image_synth_cfg ic = { 0x5EED00000000B000ull, 320u, 240u, TC_SYNTH_GRAIN };
    if (image_synth_alloc(&ic, 1, &s_gy, &s_gu, &s_gv, &s_ga) != 0) { exit(2); }
    memset(&s_gcfg, 0, sizeof s_gcfg);
    s_gcfg.struct_size = (uint32_t)sizeof(s_gcfg);
    s_gcfg.abi_version = TOPOS_CODEC_ABI_VERSION;
    s_gcfg.visible_width = 320u;
    s_gcfg.visible_height = 240u;
    s_gcfg.profile = 3u;
    s_gcfg.bit_depth = 10u;
    s_gcfg.qmatrix_id = 1u;
    s_gcfg.qp_base = 24u;
    s_gcfg.color_range = 1u;
    s_gcfg.color_primaries = 1u;
    s_gcfg.color_transfer = 1u;
    s_gcfg.color_matrix = 1u;
    memset(&s_gin, 0, sizeof s_gin);
    s_gin.struct_size = (uint32_t)sizeof(s_gin);
    s_gin.abi_version = TOPOS_CODEC_ABI_VERSION;
    s_gin.planes[0] = s_gy;
    s_gin.planes[1] = s_gu;
    s_gin.planes[2] = s_gv;
    s_gcap = tc_frame_packet_bound(&s_gcfg);
    s_gpkt = (uint8_t*)malloc(s_gcap);
}

static int op_encode_grow(void* ctx, int32_t* rc_out)
{
    (void)ctx;
    topos_frame_stats st;
    *rc_out = tc_frame_encode(&s_gcfg, &s_gin, s_gpkt, s_gcap, &st);
    return 0;
}

static void test_o4_slot_growth_oom_sweep(void)
{
    o4_grow_setup();
    tc_dev_set_thread_count(4);
    static const int32_t oom_only[] = { TC_ERR_OUT_OF_MEMORY };
    /* 每个故障点：rc ∈ {OOM}（或干净成功终止）；泄漏由 ASan 配置的
     * 同 sweep 复跑 + LSan 承担（临时指针逐项提交后无丢失分配） */
    int64_t used = sweep_run(512, op_encode_grow, NULL, oom_only, 1);
    MT_CHECK(used > 0);
    fprintf(stderr, "[o4] slot-growth OOM sweep covered %lld alloc points\n",
            (long long)used);
    tc_dev_set_thread_count(4);
}

int main(void)
{
    static const int32_t oom_only[] = { TC_ERR_OUT_OF_MEMORY };

    frame_setup();
    if (packet_synth_build(packet_synth_cfg_at(PACKET_SYNTH_CFG_TINY), &s_p1, &s_p1_size) != TC_OK) { exit(2); }
    if (packet_synth_build(packet_synth_cfg_at(PACKET_SYNTH_CFG_TINY), &s_p2, &s_p2_size) != TC_OK) { exit(2); }

    /* dev API 自身：默认故障点 0 = 关闭 */
    MT_CHECK_EQ_I64(tc_dev_set_alloc_fault(-1), 0);
    MT_CHECK_EQ_I64(tc_dev_alloc_count(), 0);
    MT_CHECK_EQ_I64(tc_dev_set_alloc_fault(-1), -1);

    /* encode（含首跑共享缓冲增长路径）/ sized / decode */
    MT_CHECK(sweep_run(256, op_encode, NULL, oom_only, 1) >= 0);
    {
        topos_frame_stats st;
        MT_CHECK_EQ_I64(tc_frame_encode(&s_cfg, &s_in, s_pkt, s_pkt_cap, &st), TC_OK); /* 可恢复 */
    }
    MT_CHECK(sweep_run(256, op_encode_sized, NULL, oom_only, 1) >= 0);
    /* sized sweep 覆写了 s_pkt —— 恢复基线包再扫 decode */
    {
        topos_frame_stats st;
        MT_CHECK_EQ_I64(tc_frame_encode(&s_cfg, &s_in, s_pkt, s_pkt_cap, &st), TC_OK);
        s_pkt_size = st.packet_size;
    }
    MT_CHECK(sweep_run(256, op_decode, NULL, oom_only, 1) >= 0);

    /* mux 全序列 → 干净产物 → open sweep → faststart sweep */
    {
        mux_env menv;
        memset(&menv, 0, sizeof menv);
        tio_fault_reset(&menv.fault);
        tio_sink_init(&menv.io, &menv.sink, &menv.fault);
        menv.p1 = s_p1; menv.p1_size = s_p1_size;
        menv.p2 = s_p2; menv.p2_size = s_p2_size;
        MT_CHECK(sweep_run(256, op_mux, &menv, oom_only, 1) >= 0);
        int32_t rc = 0;
        op_mux(&menv, &rc);
        MT_CHECK_EQ_I64(rc, TC_OK);

        open_env oenv;
        tio_source_init(&oenv.io, &oenv.src, menv.sink.data, menv.sink.len, &menv.fault);
        topos_movie* mov = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&oenv.io, &mov), TC_OK);
        topos_movie_info mi;
        memset(&mi, 0, sizeof mi);
        mi.struct_size = (uint32_t)sizeof(mi);
        mi.abi_version = TOPOS_CODEC_ABI_VERSION;
        MT_CHECK_EQ_I64(tc_movie_info(mov, &mi), TC_OK);
        MT_CHECK_EQ_U64(mi.sample_count, 2ull);
        tc_movie_close(mov);
        MT_CHECK(sweep_run(256, op_open, &oenv, oom_only, 1) >= 0);

        fs_env fenv;
        memset(&fenv, 0, sizeof fenv);
        tio_fault_reset(&menv.fault);
        fenv.src = oenv.src;
        fenv.src_io = oenv.io;
        tio_sink_init(&fenv.dst_io, &fenv.dst, &menv.fault);
        MT_CHECK(sweep_run(256, op_faststart, &fenv, oom_only, 1) >= 0);
        int32_t rc2 = 0;
        op_faststart(&fenv, &rc2);
        MT_CHECK_EQ_I64(rc2, TC_OK);
        open_env denv;
        tio_source_init(&denv.io, &denv.src, fenv.dst.data, fenv.dst.len, &menv.fault);
        topos_movie* mov2 = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&denv.io, &mov2), TC_OK);
        tc_movie_close(mov2);
        tio_sink_free(&fenv.dst);
        tio_sink_free(&menv.sink);
    }

    /* 全部 sweep 后干净端到端一遍（共享状态健康） */
    {
        topos_frame_stats st;
        MT_CHECK_EQ_I64(tc_frame_encode(&s_cfg, &s_in, s_pkt, s_pkt_cap, &st), TC_OK);
        topos_frame_output info;
        MT_CHECK_EQ_I64(tc_frame_decode(s_pkt, st.packet_size, s_out, NULL, &info), TC_OK);
    }

    free(s_y); free(s_u); free(s_v); free(s_a);
    free(s_pkt);
    for (int p = 0; p < 4; ++p) { free(s_out[p]); }
    free(s_p1); free(s_p2);
    test_o4_slot_growth_oom_sweep();
    return MT_MAIN_RETURN();
}
