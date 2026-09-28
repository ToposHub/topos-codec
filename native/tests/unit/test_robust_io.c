/* IO 故障注入（阶段 10）：权限拒绝 / 磁盘满 / 用户取消 / 截断 / 并发读。
 *
 * 契约：IO 层一切失败必须映射为定义过的错误码（TC_ERR_IO / TRUNCATED /
 * MALFORMED 等），不崩溃；句柄（mux/movie）始终可释放；读取路径无共享可变
 * 状态（并发只读共享安全，close 由单一线程在 join 之后执行 —— 头文件契约）。
 * 泄漏由同二进制 ASan 配置承担；数据竞争由 TSan 配置承担。
 */
#include "../support/port_thread.h"

#include <stdio.h>

#include <stdlib.h>
#include <string.h>

#include "fault_io.h"
#include "packet_synth.h"
#include "topos_codec.h"

#include "mini_test.h"

#define N_FRAMES 8u

static uint8_t* s_pkt;
static size_t s_pkt_size;
static uint8_t* s_mov;    /* 标准布局 ftyp+mdat+moov */
static size_t s_mov_len;
static uint8_t* s_fs;     /* FastStart 布局 ftyp+moov+mdat */
static size_t s_fs_len;

static void fill_movie_cfg(topos_movie_config* mc)
{
    memset(mc, 0, sizeof *mc);
    mc->struct_size = (uint32_t)sizeof(*mc);
    mc->abi_version = TOPOS_CODEC_ABI_VERSION;
    mc->visible_width = 48u;   /* 与 PACKET_SYNTH_CFG_REMAINDER 一致（tpcC 规则） */
    mc->visible_height = 48u;
    mc->profile = 3u;
    mc->pixel_format = 0u;
    mc->bit_depth = 10u;
    mc->alpha_mode = 1u;
    mc->alpha_bit_depth = 16u;
    mc->qp_base = 12u;
    mc->qmatrix_id = 0u;
    mc->color_range = 1u;
    mc->color_primaries = 1u;
    mc->color_transfer = 1u;
    mc->color_matrix = 1u;
    mc->timescale = 24000u;
}

static void build_corpus(void)
{
    const packet_synth_cfg* pc = packet_synth_cfg_at(PACKET_SYNTH_CFG_REMAINDER);
    if (packet_synth_build(pc, &s_pkt, &s_pkt_size) != TC_OK) { exit(2); }

    tio_fault f;
    tio_fault_reset(&f);
    tio_sink sink;
    topos_io io;
    tio_sink_init(&io, &sink, &f);
    topos_movie_config mc;
    fill_movie_cfg(&mc);
    topos_mux* mux = NULL;
    if (tc_mux_create(&mc, &io, &mux) != TC_OK) { exit(2); }
    for (uint32_t i = 0u; i < N_FRAMES; ++i) {
        if (tc_mux_add_packet(mux, s_pkt, s_pkt_size,
                              (uint64_t)i * 1000u, 1000u) != TC_OK) { exit(2); }
    }
    if (tc_mux_finish(mux) != TC_OK) { exit(2); }
    tc_mux_free(mux);
    s_mov = sink.data;
    s_mov_len = sink.len;

    tio_source src;
    topos_io sio;
    tio_source_init(&sio, &src, s_mov, s_mov_len, &f);
    tio_sink dst;
    topos_io dio;
    tio_sink_init(&dio, &dst, &f);
    if (tc_movie_faststart(&sio, &dio) != TC_OK) { exit(2); }
    s_fs = dst.data;
    s_fs_len = dst.len;
}

/* ---- 并发只读共享（同一 topos_movie，多线程随机 sample 读取） ---- */

typedef struct {
    topos_movie* movie;
    uint32_t iters;
    uint32_t seed;
    int fail;
} reader_ctx;

static void* reader_thread(void* v)
{
    reader_ctx* c = (reader_ctx*)v;
    uint64_t x = c->seed;
    uint8_t buf[1 << 16];
    for (uint32_t i = 0u; i < c->iters; ++i) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t idx = (uint32_t)((x >> 33) % N_FRAMES);
        size_t need = 0;
        int32_t rc = tc_movie_packet(c->movie, idx, NULL, 0u, &need);
        if (rc != TC_ERR_BUFFER_TOO_SMALL || need != s_pkt_size) { c->fail = 1; return NULL; }
        rc = tc_movie_packet(c->movie, idx, buf, sizeof buf, NULL);
        if (rc != TC_OK || memcmp(buf, s_pkt, s_pkt_size) != 0) { c->fail = 1; return NULL; }
        uint64_t pts = 0;
        uint32_t dur = 0;
        if (tc_movie_packet_pts(c->movie, idx, &pts, &dur) != TC_OK || dur != 1000u) {
            c->fail = 1;
            return NULL;
        }
    }
    return NULL;
}

/* ---- 并发独立 movie（每线程各自 open/read/close） ---- */

typedef struct {
    const uint8_t* data;
    size_t len;
    int fail;
} solo_ctx;

static void* solo_thread(void* v)
{
    solo_ctx* c = (solo_ctx*)v;
    tio_fault f;
    tio_fault_reset(&f);
    tio_source src;
    topos_io io;
    tio_source_init(&io, &src, c->data, c->len, &f);
    topos_movie* mov = NULL;
    if (tc_movie_open(&io, &mov) != TC_OK) { c->fail = 1; return NULL; }
    uint8_t* buf = (uint8_t*)malloc(s_pkt_size);
    for (uint32_t i = 0u; i < N_FRAMES; ++i) {
        if (tc_movie_packet(mov, i, buf, s_pkt_size, NULL) != TC_OK ||
            memcmp(buf, s_pkt, s_pkt_size) != 0) {
            c->fail = 1;
            break;
        }
    }
    free(buf);
    tc_movie_close(mov);
    return NULL;
}

int main(void)
{
    build_corpus();

    /* ---- 1. 权限拒绝：第 1 次读即失败 → open TC_ERR_IO ---- */
    {
        tio_fault f;
        tio_fault_reset(&f);
        f.fail_read_from = 1;
        tio_source src;
        topos_io io;
        tio_source_init(&io, &src, s_mov, s_mov_len, &f);
        topos_movie* mov = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&io, &mov), TC_ERR_IO);
        MT_CHECK(mov == NULL);
        tc_movie_close(mov);
    }

    /* ---- 2. 磁盘满 sweep：写/seek_write 从第 k 次调用起失败 ---- */
    {
        static const int32_t ok_io[] = { TC_OK, TC_ERR_IO, TC_ERR_STATE };
        int completed = 0;
        for (int k = 1; k <= 64 && !completed; ++k) {
            tio_fault f;
            tio_fault_reset(&f);
            f.fail_write_from = k;
            tio_sink sink;
            topos_io io;
            tio_sink_init(&io, &sink, &f);
            topos_movie_config mc;
            fill_movie_cfg(&mc);
            topos_mux* mux = NULL;
            int32_t rc = tc_mux_create(&mc, &io, &mux);
            int ok = 0;
            for (int i = 0; i < 3; ++i) { if (ok_io[i] == rc) { ok = 1; } }
            MT_CHECK(ok);
            if (rc == TC_OK) {
                rc = tc_mux_add_packet(mux, s_pkt, s_pkt_size, 0u, 1000u);
                ok = 0;
                for (int i = 0; i < 3; ++i) { if (ok_io[i] == rc) { ok = 1; } }
                MT_CHECK(ok);
            }
            if (rc == TC_OK) {
                rc = tc_mux_finish(mux);
                MT_CHECK(rc == TC_OK || rc == TC_ERR_IO || rc == TC_ERR_STATE);
                if (rc == TC_OK) {
                    completed = 1; /* 全序列成功 → 已覆盖更早 k 的全部失败点 */
                }
            }
            tc_mux_free(mux); /* 中途失败也必须可释放（ASan 验证无泄漏） */
            tio_sink_free(&sink);
        }
        MT_CHECK_EQ_I64(completed, 1);
        /* seek_write 失败（mdat 长度回填）：finish 阶段暴露 */
        {
            tio_fault f;
            tio_fault_reset(&f);
            f.fail_seek_write_from = 1;
            tio_sink sink;
            topos_io io;
            tio_sink_init(&io, &sink, &f);
            topos_movie_config mc;
            fill_movie_cfg(&mc);
            topos_mux* mux = NULL;
            MT_CHECK_EQ_I64(tc_mux_create(&mc, &io, &mux), TC_OK);
            MT_CHECK_EQ_I64(tc_mux_add_packet(mux, s_pkt, s_pkt_size, 0u, 1000u), TC_OK);
            int32_t frc = tc_mux_finish(mux);
            MT_CHECK(frc == TC_OK || frc == TC_ERR_IO || frc == TC_ERR_STATE);
            tc_mux_free(mux);
            tio_sink_free(&sink);
        }
    }

    /* ---- 3. 取消：读取中途 IO 开始失败 → 立即 TC_ERR_IO（不挂起） ---- */
    {
        tio_fault f;
        tio_fault_reset(&f);
        tio_source src;
        topos_io io;
        tio_source_init(&io, &src, s_mov, s_mov_len, &f);
        topos_movie* mov = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&io, &mov), TC_OK);
        uint8_t* buf = (uint8_t*)malloc(s_pkt_size);
        for (uint32_t i = 0u; i < N_FRAMES; ++i) {
            if (i == 3u) { f.fail_read_from = f.read_calls + 1; } /* 模拟用户取消 */
            int32_t rc = tc_movie_packet(mov, i, buf, s_pkt_size, NULL);
            if (i < 3u) {
                MT_CHECK_EQ_I64(rc, TC_OK);
            } else {
                MT_CHECK_EQ_I64(rc, TC_ERR_IO);
                break;
            }
        }
        free(buf);
        tc_movie_close(mov);
    }

    /* ---- 4. 截断 sweep：两种布局每个前缀都不得崩溃 ---- */
    {
        struct { const uint8_t* data; size_t len; } cases[2] = {
            { s_mov, s_mov_len }, { s_fs, s_fs_len },
        };
        for (int c = 0; c < 2; ++c) {
            for (size_t prefix = 0u; prefix <= cases[c].len; ++prefix) {
                tio_fault f;
                tio_fault_reset(&f);
                tio_source src;
                topos_io io;
                tio_source_init(&io, &src, cases[c].data, prefix, &f);
                topos_movie* mov = NULL;
                int32_t rc = tc_movie_open(&io, &mov);
                if (rc == TC_OK) {
                    topos_movie_info mi;
                    memset(&mi, 0, sizeof mi);
                    mi.struct_size = (uint32_t)sizeof(mi);
                    mi.abi_version = TOPOS_CODEC_ABI_VERSION;
                    MT_CHECK_EQ_I64(tc_movie_info(mov, &mi), TC_OK);
                    MT_CHECK_EQ_U64(mi.sample_count, N_FRAMES);
                    uint8_t* buf = (uint8_t*)malloc(s_pkt_size);
                    /* 截断处 sample 读取：OK（数据在前缀内）或 IO（EOF） */
                    for (uint32_t i = 0u; i < N_FRAMES; ++i) {
                        int32_t prc = tc_movie_packet(mov, i, buf, s_pkt_size, NULL);
                        MT_CHECK(prc == TC_OK || prc == TC_ERR_IO || prc == TC_ERR_TRUNCATED
                                 || prc == TC_ERR_MALFORMED);
                        if (prc == TC_OK) { MT_CHECK(memcmp(buf, s_pkt, s_pkt_size) == 0); }
                    }
                    free(buf);
                } else {
                    MT_CHECK(rc == TC_ERR_TRUNCATED || rc == TC_ERR_IO ||
                             rc == TC_ERR_MALFORMED || rc == TC_ERR_INVALID_ARGUMENT);
                    MT_CHECK(mov == NULL);
                }
                tc_movie_close(mov);
            }
        }
    }

    /* ---- 5. 并发只读共享同一 movie（读侧无共享可变状态） ---- */
    {
        tio_fault f;
        tio_fault_reset(&f);
        tio_source src;
        topos_io io;
        tio_source_init(&io, &src, s_mov, s_mov_len, &f);
        topos_movie* mov = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&io, &mov), TC_OK);
        pthread_t th[4];
        reader_ctx ctx[4];
        for (int t = 0; t < 4; ++t) {
            ctx[t].movie = mov;
            ctx[t].iters = 200u;
            ctx[t].seed = 0x9E3779B97F4A7C15ull * (uint64_t)(t + 1);
            ctx[t].fail = 0;
            MT_CHECK_EQ_I64(pthread_create(&th[t], NULL, reader_thread, &ctx[t]), 0);
        }
        for (int t = 0; t < 4; ++t) {
            pthread_join(th[t], NULL);
            MT_CHECK_EQ_I64(ctx[t].fail, 0);
        }
        tc_movie_close(mov); /* close 契约：join 之后单线程执行 */
    }

    /* ---- 6. 并发独立 movie ---- */
    {
        pthread_t th[4];
        solo_ctx ctx[4];
        for (int t = 0; t < 4; ++t) {
            ctx[t].data = s_mov;
            ctx[t].len = s_mov_len;
            ctx[t].fail = 0;
            MT_CHECK_EQ_I64(pthread_create(&th[t], NULL, solo_thread, &ctx[t]), 0);
        }
        for (int t = 0; t < 4; ++t) {
            pthread_join(th[t], NULL);
            MT_CHECK_EQ_I64(ctx[t].fail, 0);
        }
    }

    free(s_pkt);
    free(s_mov);
    free(s_fs);
    return MT_MAIN_RETURN();
}
