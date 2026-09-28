/* 长时压力（阶段 10）：编码 → mux → 顺序播放 → 拖拽随机 seek → 随机损坏。
 *
 * 模拟应用真实负载：多内容类型轮换、qp/alpha 混合、sized 码控、FastStart 布局、
 * 多线程并行（tc_dev_set_thread_count(4)）。不变量：
 *  - 完好码流播放/seek：rc == TC_OK 且 concealed == 0（中间片不允许无声劣化）；
 *  - 解码确定性：同 packet 重复解码逐字节一致；
 *  - 损坏码流：rc 落在定义集（concealment 交付或整帧拒绝），绝不崩溃；
 *    concealed_slices 与 slice_status[] 逐项一致。
 * 规模可用环境变量放大做浸泡：TOPOS_STRESS_FRAMES / _DRAG / _CORRUPT。
 * 泄漏由 ASan 配置承担；数据竞争由 TSan 配置承担。
 */
#include "common/tpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fault_io.h"
#include "image_synth.h"
#include "topos_codec.h"

#include "mini_test.h"

#define W 316u
#define H 180u
#define CW 158u /* ceil(316/2) */

static uint64_t s_rng = 0x243F6A8885A308D3ull;
static uint64_t rnd64(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return s_rng;
}

static long env_or(const char* name, long def)
{
    const char* v = getenv(name);
    return (v != NULL && v[0] != '\0') ? atol(v) : def;
}

static uint32_t plane_elems(uint32_t plane)
{
    return plane == 1u || plane == 2u ? CW * H : W * H;
}

int main(void)
{
    const long n_frames = env_or("TOPOS_STRESS_FRAMES", 160);
    const long n_drag = env_or("TOPOS_STRESS_DRAG", 300);
    const long n_corrupt = env_or("TOPOS_STRESS_CORRUPT", 150);
    const long n_threads = env_or("TOPOS_STRESS_THREADS", 4);
    if (n_frames < 4) { return 0; }

    /* W04：线程扫描旋钮（1/2/4/8/逻辑核），Win32 池在 tc_dev 层同一入口 */
    tc_dev_set_thread_count(n_threads);

    static const tc_synth_kind kinds[5] = {
        TC_SYNTH_FLAT, TC_SYNTH_GRADIENT, TC_SYNTH_GRAIN, TC_SYNTH_DETAIL, TC_SYNTH_MIXED
    };
    static const uint8_t qps[4] = { 16u, 28u, 40u, 52u };

    uint16_t* y = (uint16_t*)malloc((size_t)W * H * 2u);
    uint16_t* u = (uint16_t*)malloc((size_t)CW * H * 2u);
    uint16_t* v = (uint16_t*)malloc((size_t)CW * H * 2u);
    uint16_t* a = (uint16_t*)malloc((size_t)W * H * 2u);
    MT_CHECK(y != NULL && u != NULL && v != NULL && a != NULL);

    topos_frame_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.visible_width = (uint16_t)W;
    cfg.visible_height = (uint16_t)H;
    cfg.qmatrix_id = 1u;
    cfg.alpha_mode = 2u;
    cfg.alpha_bit_depth = 12u;
    /* H1（2026-09-21）：钉 legacy transfer(1)——cfg 默认已改派 sRGB(13)，
     * 保持与 tpcC（mc 0→1 默认）一致；H1 双轨策略见 golden_codec.c */
    cfg.color_transfer = 1u;
    topos_frame_input in;
    memset(&in, 0, sizeof in);
    in.struct_size = (uint32_t)sizeof(in);
    in.planes[0] = y;
    in.planes[1] = u;
    in.planes[2] = v;
    in.planes[3] = a;

    size_t cap = tc_frame_packet_bound(&cfg);
    uint8_t* pkt = (uint8_t*)malloc(cap);
    uint8_t* pkt2 = (uint8_t*)malloc(cap);
    MT_CHECK(pkt != NULL && pkt2 != NULL);
    /* 解码输出平面（全帧同几何，一次分配） */
    uint16_t* out[4] = { 0 };
    for (uint32_t p = 0u; p < 4u; ++p) {
        out[p] = (uint16_t*)malloc((size_t)plane_elems(p) * 2u);
        MT_CHECK(out[p] != NULL);
    }
    uint16_t* redec[4] = { 0 };
    for (uint32_t p = 0u; p < 4u; ++p) {
        redec[p] = (uint16_t*)malloc((size_t)plane_elems(p) * 2u);
        MT_CHECK(redec[p] != NULL);
    }

    tio_fault f;
    tio_fault_reset(&f);
    tio_sink sink;
    topos_io mio;
    tio_sink_init(&mio, &sink, &f);
    topos_movie_config mc;
    memset(&mc, 0, sizeof mc);
    mc.struct_size = (uint32_t)sizeof(mc);
    mc.abi_version = TOPOS_CODEC_ABI_VERSION;
    mc.visible_width = (uint16_t)W;
    mc.visible_height = (uint16_t)H;
    mc.profile = 3u;
    mc.pixel_format = 0u;
    mc.bit_depth = 10u;
    mc.alpha_mode = 2u;
    mc.alpha_bit_depth = 12u;
    mc.color_primaries = 1u;   /* bt709：与编码器默认一致（tpcC 规则） */
    mc.color_transfer = 1u;
    mc.color_matrix = 1u;
    mc.qp_base = 28u;
    mc.qmatrix_id = 1u;
    mc.timescale = 24000u;
    topos_mux* mux = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&mc, &mio, &mux), TC_OK);

    /* ---- 编码 + mux（混合内容/qp，sized 码控穿插） ---- */
    size_t last_plain_size = 0;
    for (long i = 0; i < n_frames; ++i) {
        image_synth_cfg ic = { 0x5EED000000000000ull + (uint64_t)i, W, H,
                               kinds[i % 5] };
        MT_CHECK_EQ_I64(image_synth_build(&ic, y, u, v, a), 0);
        cfg.qp_base = qps[i % 4];
        topos_frame_stats st;
        int32_t rc;
        if (i % 8 == 0 && last_plain_size > 0u) {
            uint8_t qp_used = 0;
            rc = tc_frame_encode_sized(&cfg, &in, (uint32_t)(last_plain_size / 2u),
                                       4u, 60u, &qp_used, pkt, cap, &st);
        } else {
            rc = tc_frame_encode(&cfg, &in, pkt, cap, &st);
            last_plain_size = st.packet_size;
        }
        MT_CHECK_EQ_I64(rc, TC_OK);
        MT_CHECK_EQ_I64(tc_mux_add_packet(mux, pkt, st.packet_size,
                                          (uint64_t)i * 1000u, 1000u), TC_OK);
    }
    MT_CHECK_EQ_I64(tc_mux_finish(mux), TC_OK);
    tc_mux_free(mux);

    /* FastStart 布局一遍（拖拽 seek 模拟用它） */
    tio_source src;
    topos_io sio;
    tio_source_init(&sio, &src, sink.data, sink.len, &f);
    tio_sink fs_dst;
    topos_io fio;
    tio_sink_init(&fio, &fs_dst, &f);
    MT_CHECK_EQ_I64(tc_movie_faststart(&sio, &fio), TC_OK);

    topos_movie_info mi;
    memset(&mi, 0, sizeof mi);
    mi.struct_size = (uint32_t)sizeof(mi);
    mi.abi_version = TOPOS_CODEC_ABI_VERSION;

    /* ---- 顺序播放（标准布局） ---- */
    topos_movie* mov = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&sio, &mov), TC_OK);
    MT_CHECK_EQ_I64(tc_movie_info(mov, &mi), TC_OK);
    MT_CHECK_EQ_U64(mi.sample_count, (uint64_t)n_frames);
    uint8_t* sbuf = (uint8_t*)malloc(cap);
    MT_CHECK(sbuf != NULL);
    for (long i = 0; i < n_frames; ++i) {
        size_t need = 0;
        MT_CHECK_EQ_I64(tc_movie_packet(mov, (uint32_t)i, NULL, 0u, &need), TC_ERR_BUFFER_TOO_SMALL);
        MT_CHECK_EQ_I64(tc_movie_packet(mov, (uint32_t)i, sbuf, cap, NULL), TC_OK);
        topos_frame_output info;
        int32_t rc = tc_frame_decode(sbuf, need, out, NULL, &info);
        if (rc != TC_OK || info.concealed_slices != 0u) {
            mt_report(__FILE__, __LINE__, "stress: 顺序播放帧失败/concealed");
            break;
        }
        if (i % 16 == 0) { /* 确定性：重复解码逐平面一致 */
            topos_frame_output info2;
            rc = tc_frame_decode(sbuf, need, redec, NULL, &info2);
            MT_CHECK_EQ_I64(rc, TC_OK);
            int same = 1;
            for (uint32_t p = 0u; p < info.plane_count; ++p) {
                if (memcmp(out[p], redec[p], (size_t)plane_elems(p) * 2u) != 0) { same = 0; }
            }
            if (!same) {
                mt_report(__FILE__, __LINE__, "stress: 重复解码不确定");
                break;
            }
        }
    }

    /* ---- 拖拽模拟：FastStart 布局随机 seek + 解码 ---- */
    tio_source fsrc;
    topos_io fsio;
    tio_source_init(&fsio, &fsrc, fs_dst.data, fs_dst.len, &f);
    topos_movie* fmov = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&fsio, &fmov), TC_OK);
    for (long d = 0; d < n_drag; ++d) {
        uint32_t idx = (uint32_t)(rnd64() % (uint64_t)n_frames);
        size_t need = 0;
        MT_CHECK_EQ_I64(tc_movie_packet(fmov, idx, NULL, 0u, &need), TC_ERR_BUFFER_TOO_SMALL);
        MT_CHECK_EQ_I64(tc_movie_packet(fmov, idx, sbuf, cap, NULL), TC_OK);
        topos_frame_output info;
        int32_t rc = tc_frame_decode(sbuf, need, out, NULL, &info);
        if (rc != TC_OK || info.concealed_slices != 0u) {
            mt_report(__FILE__, __LINE__, "stress: 拖拽 seek 帧失败/concealed");
            break;
        }
        uint64_t pts = 0;
        uint32_t dur = 0;
        MT_CHECK_EQ_I64(tc_movie_packet_pts(fmov, idx, &pts, &dur), TC_OK);
        if (pts != (uint64_t)idx * 1000u || dur != 1000u) {
            mt_report(__FILE__, __LINE__, "stress: pts/dur 不符");
            break;
        }
    }
    tc_movie_close(fmov);
    tc_movie_close(mov);

    /* ---- 随机损坏：绝不崩溃，错误码落在定义集，状态自洽 ---- */
    tio_source csrc;
    topos_io csio;
    tio_source_init(&csio, &csrc, sink.data, sink.len, &f);
    topos_movie* cmov = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&csio, &cmov), TC_OK);
    for (long c = 0; c < n_corrupt; ++c) {
        uint32_t idx = (uint32_t)(rnd64() % (uint64_t)n_frames);
        size_t need = 0;
        if (tc_movie_packet(cmov, idx, NULL, 0u, &need) != TC_ERR_BUFFER_TOO_SMALL) {
            mt_report(__FILE__, __LINE__, "stress: 探测读取失败");
            break;
        }
        MT_CHECK(tc_movie_packet(cmov, idx, sbuf, cap, NULL) == TC_OK);
        for (int b = 0; b < 2; ++b) { /* 1-2 个随机位翻转 */
            size_t off = (size_t)(rnd64() % (uint64_t)need);
            sbuf[off] ^= (uint8_t)(1u << (rnd64() % 8u));
        }
        topos_frame_output info;
        int32_t rc = tc_frame_decode(sbuf, need, out, NULL, &info);
        if (rc == TC_OK || rc == TC_WARN_CONCEALED) {
            uint32_t concealed = 0u;
            for (uint32_t s = 0u; s < info.slice_count; ++s) {
                if (info.slice_status[s] == TC_FRAME_SLICE_CONCEALED) { concealed++; }
            }
            if (concealed != info.concealed_slices) {
                mt_report(__FILE__, __LINE__, "stress: concealed_slices 与逐 slice 状态不符");
                break;
            }
        } else if (rc != TC_ERR_MALFORMED && rc != TC_ERR_TRUNCATED &&
                   rc != TC_ERR_CHECKSUM_MISMATCH && rc != TC_ERR_UNSUPPORTED_VERSION &&
                   rc != TC_ERR_LIMIT_EXCEEDED) {
            mt_report(__FILE__, __LINE__, "stress: 损坏路径未定义错误码");
            break;
        }
    }
    tc_movie_close(cmov);

    free(sbuf);
    for (uint32_t p = 0u; p < 4u; ++p) { free(out[p]); free(redec[p]); }
    free(y); free(u); free(v); free(a);
    free(pkt); free(pkt2);
    tio_sink_free(&fs_dst);
    tio_sink_free(&sink);
    return MT_MAIN_RETURN();
}
