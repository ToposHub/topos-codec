/* mov_audio（v1.1 音频轨，container_spec_v1.1 附录）：mux 音频轨往返、
 * lpcm/mp4a 声样描述、音频声明校验、视频解码等价回归、性能基准（--bench）。
 *
 * ffprobe oracle：argv[2] 给出输出路径时把带音轨样本落盘，供外部
 * ffprobe/ffmpeg 交叉校验（声道/采样率/位深/双轨时长）。
 */
#include "common/endian.h"
#include "image_synth.h"
#include "mini_test.h"
#include "topos_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

/* ---------- 内存 io（与 test_mov 同构） ---------- */

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
} mem_file;

static int32_t mem_read(void* ctx, uint64_t off, void* buf, size_t len)
{
    mem_file* f = (mem_file*)ctx;
    if (off > (uint64_t)f->len || len > (uint64_t)f->len - off) { return TC_ERR_IO; }
    if (len != 0u) { memcpy(buf, f->data + off, len); }
    return TC_OK;
}

static int32_t mem_write(void* ctx, const void* data, size_t len)
{
    mem_file* f = (mem_file*)ctx;
    if (f->len + len > f->cap) {
        size_t cap = f->cap ? f->cap : 256u;
        while (cap < f->len + len) { cap *= 2u; }
        uint8_t* p = (uint8_t*)realloc(f->data, cap);
        if (p == NULL) { return TC_ERR_OUT_OF_MEMORY; }
        f->data = p;
        f->cap = cap;
    }
    if (len != 0u) { memcpy(f->data + f->len, data, len); }
    f->len += len;
    return TC_OK;
}

static int32_t mem_seek_write(void* ctx, uint64_t off, const void* data, size_t len)
{
    mem_file* f = (mem_file*)ctx;
    if (off > (uint64_t)f->len || len > (uint64_t)f->len - off) { return TC_ERR_IO; }
    if (len != 0u) { memcpy(f->data + off, data, len); }
    return TC_OK;
}

static void mem_io_sink(mem_file* f, topos_io* io)
{
    memset(io, 0, sizeof(*io));
    io->struct_size = (uint32_t)sizeof(topos_io);
    io->abi_version = TOPOS_CODEC_ABI_VERSION;
    io->ctx = f;
    io->read = mem_read;
    io->write = mem_write;
    io->seek_write = mem_seek_write;
    io->length = 0;
}

static void mem_io_src(mem_file* f, topos_io* io)
{
    memset(io, 0, sizeof(*io));
    io->struct_size = (uint32_t)sizeof(topos_io);
    io->abi_version = TOPOS_CODEC_ABI_VERSION;
    io->ctx = f;
    io->read = mem_read;
    io->length = (uint64_t)f->len;
}

static void mem_free(mem_file* f)
{
    free(f->data);
    memset(f, 0, sizeof(*f));
}

/* ---------- 帧 packet 生成（确定性） ---------- */

#define TEST_FRAMES 96
#define AUDIO_CHUNK_FRAMES 1024u
#define AUDIO_RATE 48000u
#define AUDIO_CHANNELS 2u
#define AUDIO_BITS 24u
#define AUDIO_FRAME_BYTES (AUDIO_CHANNELS * (AUDIO_BITS / 8u))

static uint8_t* g_pkts[TEST_FRAMES];
static size_t g_pkt_sizes[TEST_FRAMES];

static void build_test_packets(void)
{
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
        topos_frame_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.struct_size = (uint32_t)sizeof(cfg);
        cfg.abi_version = TOPOS_CODEC_ABI_VERSION;
        cfg.visible_width = 64u;
        cfg.visible_height = 40u;
        cfg.profile = 3u;
        cfg.bit_depth = 10u;
        cfg.qmatrix_id = 1u;
        cfg.qp_base = 28u;
        cfg.color_range = 1u;
        cfg.color_primaries = 1u;
        cfg.color_transfer = 1u;
        cfg.color_matrix = 1u;
        cfg.sar_num = 1u;
        cfg.sar_den = 1u;

        image_synth_cfg sc;
        memset(&sc, 0, sizeof(sc));
        sc.width = 64u;
        sc.height = 40u;
        sc.kind = (tc_synth_kind)(i % TC_SYNTH_KIND_COUNT);
        sc.seed = 900u + (uint64_t)i;
        uint16_t* pl[4] = { NULL, NULL, NULL, NULL };
        image_synth_alloc(&sc, 0, &pl[0], &pl[1], &pl[2], &pl[3]);

        topos_frame_input in;
        memset(&in, 0, sizeof(in));
        in.struct_size = (uint32_t)sizeof(in);
        in.abi_version = TOPOS_CODEC_ABI_VERSION;
        in.planes[0] = pl[0];
        in.planes[1] = pl[1];
        in.planes[2] = pl[2];

        size_t bound = tc_frame_packet_bound(&cfg);
        uint8_t* buf = (uint8_t*)malloc(bound);
        topos_frame_stats st;
        int32_t rc = tc_frame_encode(&cfg, &in, buf, bound, &st);
        MT_CHECK_EQ_I64(rc, TC_OK);
        g_pkts[i] = buf;
        g_pkt_sizes[i] = st.packet_size;
        free(pl[0]);
        free(pl[1]);
        free(pl[2]);
    }
}

static void free_test_packets(void)
{
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) { free(g_pkts[i]); }
}

static void base_movie_cfg(topos_movie_config* c)
{
    memset(c, 0, sizeof(*c));
    c->struct_size = (uint32_t)sizeof(*c);
    c->abi_version = TOPOS_CODEC_ABI_VERSION;
    c->visible_width = 64u;
    c->visible_height = 40u;
    c->profile = 3u;
    c->pixel_format = 0u;
    c->bit_depth = 10u;
    c->qmatrix_id = 1u;
    c->qp_base = 28u;
    c->color_range = 1u;
    c->color_primaries = 1u;
    c->color_transfer = 1u;
    c->color_matrix = 1u;
    c->sar_num = 1u;
    c->sar_den = 1u;
    c->timescale = 24u;
}

static void cfg_set_audio_fmt(topos_movie_config* c, uint32_t codec,
                              uint32_t rate, uint32_t channels,
                              uint32_t layout, uint32_t bits, uint32_t fmt);

static void cfg_set_audio(topos_movie_config* c, uint32_t codec, uint32_t rate,
                          uint32_t channels, uint32_t layout, uint32_t bits)
{
    cfg_set_audio_fmt(c, codec, rate, channels, layout, bits, TC_AUDIO_FMT_INT);
}

static void cfg_set_audio_fmt(topos_movie_config* c, uint32_t codec,
                              uint32_t rate, uint32_t channels,
                              uint32_t layout, uint32_t bits, uint32_t fmt)
{
    c->reserved[TC_AUDIO_SLOT_CODEC] = codec;
    c->reserved[TC_AUDIO_SLOT_RATE] = rate;
    c->reserved[TC_AUDIO_SLOT_CHANNELS] = channels;
    c->reserved[TC_AUDIO_SLOT_LAYOUT] = layout;
    c->reserved[TC_AUDIO_SLOT_BITS] = bits;
    c->reserved[TC_AUDIO_SLOT_FORMAT] = fmt;
}

/* 确定性 PCM 块（大端 'twos' 语义；内容不影响容器行为，仅往返 MD5 用） */
static void fill_pcm(uint8_t* p, uint32_t frames, uint32_t salt)
{
    const size_t bytes = (size_t)frames * AUDIO_FRAME_BYTES;
    for (size_t i = 0; i < bytes; ++i) {
        p[i] = (uint8_t)((i * 7u + salt * 31u) & 0xFFu);
    }
}

/* mux：96 帧 + 逐帧 1024 采样帧 PCM chunk 交错注入（with_audio=0 时纯视频） */
static void mux_with_audio(mem_file* out, const topos_movie_config* cfg, int with_audio)
{
    memset(out, 0, sizeof(*out));
    topos_io sink;
    mem_io_sink(out, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(cfg, &sink, &m), TC_OK);
    /* 帧字节数随 cfg 位深推导（M-B4 float32/32bit 复用本 helper） */
    const uint32_t bits = cfg->reserved[TC_AUDIO_SLOT_BITS];
    const size_t fb = (size_t)AUDIO_CHANNELS * (bits ? bits : 16u) / 8u;
    uint8_t pcm[AUDIO_CHUNK_FRAMES * 8u]; /* 上限：8 字节/帧（32bit×双声道） */
    int32_t rc = TC_OK;
    for (uint32_t i = 0; i < TEST_FRAMES && rc == TC_OK; ++i) {
        rc = tc_mux_add_packet(m, g_pkts[i], g_pkt_sizes[i], (uint64_t)i, 1u);
        if (rc != TC_OK || !with_audio) { continue; }
        for (size_t k = 0; k < (size_t)AUDIO_CHUNK_FRAMES * fb; ++k) {
            pcm[k] = (uint8_t)((k * 7u + i * 31u) & 0xFFu);
        }
        rc = tc_mux_add_audio(m, pcm, (size_t)AUDIO_CHUNK_FRAMES * fb,
                              AUDIO_CHUNK_FRAMES);
    }
    MT_CHECK_EQ_I64(rc, TC_OK);
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    tc_mux_free(m);
}

/* ---------- 测试 ---------- */

static void test_mux_lpcm_roundtrip_video_unchanged(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg_set_audio(&cfg, TC_AUDIO_CODEC_LPCM, AUDIO_RATE, AUDIO_CHANNELS,
                  TC_AUDIO_LAYOUT_STEREO, AUDIO_BITS);
    mem_file f;
    mux_with_audio(&f, &cfg, 1);

    topos_io src;
    mem_io_src(&f, &src);
    topos_movie* mv = NULL;
    const int32_t orc = tc_movie_open(&src, &mv);
    if (orc != TC_OK) { fprintf(stderr, "DEBUG open: %d %s\n", orc, tc_last_error()); }
    MT_CHECK_EQ_I64(orc, TC_OK);

    /* 视频轨解码等价：逐帧字节 + pts/dur/sync 与注入一致 */
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
        size_t need = 0u;
        uint8_t buf[1 << 20];
        MT_CHECK_EQ_I64(tc_movie_packet(mv, i, buf, sizeof(buf), &need), TC_OK);
        MT_CHECK_EQ_U64(need, g_pkt_sizes[i]);
        MT_CHECK(memcmp(buf, g_pkts[i], g_pkt_sizes[i]) == 0);
        uint64_t pts = 0u;
        uint32_t dur = 0u;
        MT_CHECK_EQ_I64(tc_movie_packet_pts(mv, i, &pts, &dur), TC_OK);
        MT_CHECK_EQ_U64(pts, i);
        MT_CHECK_EQ_U64(dur, 1u);
    }
    tc_movie_close(mv);
    mem_free(&f);
}

static void test_audio_validation_errors(void)
{
    /* 未声明音频 → add_audio 拒绝（STATE） */
    {
        topos_movie_config cfg;
        base_movie_cfg(&cfg);
        mem_file f;
        memset(&f, 0, sizeof(f));
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
        uint8_t pcm[64];
        MT_CHECK_EQ_I64(tc_mux_add_audio(m, pcm, sizeof(pcm), 8u), TC_ERR_STATE);
        MT_CHECK_EQ_I64(tc_mux_set_audio_asc(m, pcm, 4u), TC_ERR_INVALID_ARGUMENT);
        MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[0], g_pkt_sizes[0], 0u, 1u), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
        tc_mux_free(m);
        mem_free(&f);
    }
    /* lpcm 字节数不符 → INVALID_ARGUMENT */
    {
        topos_movie_config cfg;
        base_movie_cfg(&cfg);
        cfg_set_audio(&cfg, TC_AUDIO_CODEC_LPCM, AUDIO_RATE, AUDIO_CHANNELS,
                      TC_AUDIO_LAYOUT_STEREO, AUDIO_BITS);
        mem_file f;
        memset(&f, 0, sizeof(f));
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
        uint8_t pcm[100];
        MT_CHECK_EQ_I64(tc_mux_add_audio(m, pcm, sizeof(pcm), 16u),
                        TC_ERR_INVALID_ARGUMENT);
        tc_mux_free(m);
        mem_free(&f);
    }
    /* 声明音频但零样本 → finish 拒绝 */
    {
        topos_movie_config cfg;
        base_movie_cfg(&cfg);
        cfg_set_audio(&cfg, TC_AUDIO_CODEC_LPCM, AUDIO_RATE, AUDIO_CHANNELS,
                      TC_AUDIO_LAYOUT_STEREO, AUDIO_BITS);
        mem_file f;
        memset(&f, 0, sizeof(f));
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[0], g_pkt_sizes[0], 0u, 1u), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_finish(m), TC_ERR_INVALID_ARGUMENT);
        tc_mux_free(m);
        mem_free(&f);
    }
    /* 半声明（codec=NONE 但 rate ≠ 0）→ create 拒绝 */
    {
        topos_movie_config cfg;
        base_movie_cfg(&cfg);
        cfg.reserved[TC_AUDIO_SLOT_RATE] = 48000u;
        mem_file f;
        memset(&f, 0, sizeof(f));
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_ERR_INVALID_ARGUMENT);
        mem_free(&f);
    }
    /* 采样率/声道布局/位深白名单 */
    {
        const struct { uint32_t rate, ch, layout, bits; } bad[] = {
            { 32000u, 2u, TC_AUDIO_LAYOUT_STEREO, 16u },
            { 48000u, 4u, TC_AUDIO_LAYOUT_STEREO, 16u },
            { 48000u, 6u, TC_AUDIO_LAYOUT_STEREO, 16u },
            { 48000u, 2u, TC_AUDIO_LAYOUT_STEREO, 12u },
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            topos_movie_config cfg;
            base_movie_cfg(&cfg);
            cfg_set_audio(&cfg, TC_AUDIO_CODEC_LPCM, bad[i].rate, bad[i].ch,
                          bad[i].layout, bad[i].bits);
            mem_file f;
            memset(&f, 0, sizeof(f));
            topos_io sink;
            mem_io_sink(&f, &sink);
            topos_mux* m = NULL;
            MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_ERR_INVALID_ARGUMENT);
            mem_free(&f);
        }
    }
    /* mp4a 未声明 ASC → add_audio 拒绝（STATE） */
    {
        topos_movie_config cfg;
        base_movie_cfg(&cfg);
        cfg_set_audio(&cfg, TC_AUDIO_CODEC_MP4A, AUDIO_RATE, AUDIO_CHANNELS,
                      TC_AUDIO_LAYOUT_STEREO, 0u);
        mem_file f;
        memset(&f, 0, sizeof(f));
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
        uint8_t pkt[16];
        MT_CHECK_EQ_I64(tc_mux_add_audio(m, pkt, sizeof(pkt), 1024u), TC_ERR_STATE);
        tc_mux_free(m);
        mem_free(&f);
    }
}

static void test_ffprobe_dump(const char* path)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg_set_audio(&cfg, TC_AUDIO_CODEC_LPCM, AUDIO_RATE, AUDIO_CHANNELS,
                  TC_AUDIO_LAYOUT_STEREO, AUDIO_BITS);
    mem_file f;
    mux_with_audio(&f, &cfg, 1);
    FILE* fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "无法写入 ffprobe 样本 %s\n", path);
        mt_failures++;
        mem_free(&f);
        return;
    }
    fwrite(f.data, 1, f.len, fp);
    fclose(fp);
    fprintf(stderr, "ffprobe 样本已写出: %s (%zu bytes)\n", path, f.len);
    mem_free(&f);
}

/* 性能基准（--bench 手动运行；M-A1 验收门：PCM 5.1/48k 注入使 mux 总耗时
 * 增量 ≤ 基线的 5%）。用 640×360 大帧建立真实量级基线（64×40 小帧基线
 * 微秒级、无统计意义）；音频注入开销应为 PCM 字节量的 memcpy 级。 */
#if !defined(_WIN32)
#include <time.h>
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
#endif

static uint8_t* g_bench_pkts[TEST_FRAMES];
static size_t g_bench_sizes[TEST_FRAMES];

static void build_bench_packets(void)
{
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
        topos_frame_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.struct_size = (uint32_t)sizeof(cfg);
        cfg.abi_version = TOPOS_CODEC_ABI_VERSION;
        cfg.visible_width = 640u;
        cfg.visible_height = 360u;
        cfg.profile = 3u;
        cfg.bit_depth = 10u;
        cfg.qmatrix_id = 1u;
        cfg.qp_base = 20u;
        cfg.color_range = 1u;
        cfg.color_primaries = 1u;
        cfg.color_transfer = 1u;
        cfg.color_matrix = 1u;
        cfg.sar_num = 1u;
        cfg.sar_den = 1u;

        image_synth_cfg sc;
        memset(&sc, 0, sizeof(sc));
        sc.width = 640u;
        sc.height = 360u;
        sc.kind = TC_SYNTH_GRADIENT;
        sc.seed = 7000u + (uint64_t)i;
        uint16_t* pl[4] = { NULL, NULL, NULL, NULL };
        image_synth_alloc(&sc, 0, &pl[0], &pl[1], &pl[2], &pl[3]);
        topos_frame_input in;
        memset(&in, 0, sizeof(in));
        in.struct_size = (uint32_t)sizeof(in);
        in.abi_version = TOPOS_CODEC_ABI_VERSION;
        in.planes[0] = pl[0];
        in.planes[1] = pl[1];
        in.planes[2] = pl[2];
        size_t bound = tc_frame_packet_bound(&cfg);
        uint8_t* buf = (uint8_t*)malloc(bound);
        topos_frame_stats st;
        if (tc_frame_encode(&cfg, &in, buf, bound, &st) != TC_OK) { mt_failures++; }
        g_bench_pkts[i] = buf;
        g_bench_sizes[i] = st.packet_size;
        free(pl[0]);
        free(pl[1]);
        free(pl[2]);
    }
}

static void free_bench_packets(void)
{
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) { free(g_bench_pkts[i]); }
}

/* bench 专用 sink：缓冲跨轮复用（cap 只增不减），计时区不含 realloc 抖动 */
static mem_file g_bench_file;

static void bench_once(const topos_movie_config* cfg, int with_audio)
{
    mem_file* f = &g_bench_file;
    f->len = 0;  /* cap 复用 */
    topos_io sink;
    mem_io_sink(f, &sink);
    topos_mux* m = NULL;
    if (tc_mux_create(cfg, &sink, &m) != TC_OK) {
        fprintf(stderr, "bench: create failed: %s\n", tc_last_error());
        mt_failures++; return;
    }
    uint8_t pcm[AUDIO_CHUNK_FRAMES * 6u * 2u];
    int32_t rc = TC_OK;
    for (uint32_t i = 0; i < TEST_FRAMES && rc == TC_OK; ++i) {
        rc = tc_mux_add_packet(m, g_bench_pkts[i], g_bench_sizes[i], (uint64_t)i, 1u);
        if (rc != TC_OK || !with_audio) { continue; }
        fill_pcm(pcm, AUDIO_CHUNK_FRAMES, i);
        rc = tc_mux_add_audio(m, pcm, sizeof(pcm), AUDIO_CHUNK_FRAMES);
    }
    if (rc == TC_OK) { rc = tc_mux_finish(m); }
    if (rc != TC_OK) { fprintf(stderr, "bench: rc=%d at add: %s\n", rc, tc_last_error()); mt_failures++; }
    tc_mux_free(m);
}

static void test_bench(void)
{
#if !defined(_WIN32)
    const int ROUNDS = 8;
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    /* 与 bench packet 的几何/qp 一致（tpcC 一致性规则） */
    cfg.visible_width = 640u;
    cfg.visible_height = 360u;
    cfg.qp_base = 20u;
    topos_movie_config cfg_a = cfg;
    cfg_set_audio(&cfg_a, TC_AUDIO_CODEC_LPCM, AUDIO_RATE, 6u,
                  TC_AUDIO_LAYOUT_5_1, 16u);
    /* 预热（内存/分支缓存），消除先后冷启动偏差 */
    bench_once(&cfg, 0);
    bench_once(&cfg_a, 1);
    double t0 = now_ms();
    for (int r = 0; r < ROUNDS; ++r) { bench_once(&cfg, 0); }
    double t1 = now_ms();
    /* PCM 5.1/48k/16bit（M-A1 验收口径） */
    for (int r = 0; r < ROUNDS; ++r) { bench_once(&cfg_a, 1); }
    double t2 = now_ms();
    const double base = (t1 - t0) / ROUNDS;
    const double with_audio = (t2 - t1) / ROUNDS;
    const double incr = with_audio - base;
    const double pct = base > 0.0 ? incr / base * 100.0 : 0.0;
    /* 注入的 PCM 字节量：96 chunk × 1024 帧 × 6 声道 × 2 字节 = 1.125 MiB */
    const double audio_mib = (double)TEST_FRAMES * AUDIO_CHUNK_FRAMES * 6u * 2u / 1048576.0;
    fprintf(stderr, "bench: mux 96×640×360 基线=%.2fms 带音频(5.1/48k/16b)=%.2fms "
                    "增量=%.2fms (%.2f%%) 注入=%.2fMiB → 吞吐≥%.1fGiB/s\n",
            base, with_audio, incr, pct, audio_mib,
            incr > 0.0 ? audio_mib / 1024.0 / (incr / 1000.0) : 0.0);
    /* 单测断言「memcpy 量级」：注入吞吐劣于 1 GiB/s 视为算法级回归（实现
     * 实测 ~数 GiB/s，10 倍余量防 CI 抖动）。5% 总耗时门在 M-A6 e2e 以
     * 导出 fps 复核（微基准 ±0.2ms 噪声无法稳定承载百分比断言）。 */
    MT_CHECK(incr <= audio_mib);  /* 1.125 MiB → ≤1.125ms 等价 ≥1 GiB/s */
#endif
}



/* ---------- M-A3：AAC 包存储（mp4a + esds） ---------- */

static void mux_with_aac_ex(mem_file* out, const topos_movie_config* cfg,
                            uint32_t priming);

static void mux_with_aac(mem_file* out, const topos_movie_config* cfg)
{
    mux_with_aac_ex(out, cfg, 0u);
}

/* priming != 0 时声明 elst（v1.4；全部包注入后调用——C 校验 < 总采样数） */
static void mux_with_aac_ex(mem_file* out, const topos_movie_config* cfg,
                            uint32_t priming)
{
    memset(out, 0, sizeof(*out));
    topos_io sink;
    mem_io_sink(out, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(cfg, &sink, &m), TC_OK);
    /* AAC-LC 48kHz stereo 的 AudioSpecificConfig（2 字节：AOT=2, freqIdx=3, chanCfg=2） */
    static const uint8_t ASC[2] = { 0x12, 0x10 };
    MT_CHECK_EQ_I64(tc_mux_set_audio_asc(m, ASC, sizeof(ASC)), TC_OK);
    uint8_t pkt[512];
    int32_t rc = TC_OK;
    for (uint32_t i = 0; i < TEST_FRAMES && rc == TC_OK; ++i) {
        rc = tc_mux_add_packet(m, g_pkts[i], g_pkt_sizes[i], (uint64_t)i, 1u);
        if (rc != TC_OK) { break; }
        /* 逐包变长（1024 采样帧/包，AAC 天然帧结构） */
        const size_t pkt_size = 64u + (i % 37u);
        for (size_t k = 0; k < pkt_size; ++k) { pkt[k] = (uint8_t)((k * 13u + i) & 0xFFu); }
        rc = tc_mux_add_audio(m, pkt, pkt_size, 1024u);
    }
    MT_CHECK_EQ_I64(rc, TC_OK);
    if (priming != 0u) {
        MT_CHECK_EQ_I64(tc_mux_set_audio_priming(m, priming), TC_OK);
    }
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    tc_mux_free(m);
}

static void test_aac_roundtrip(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg_set_audio(&cfg, TC_AUDIO_CODEC_MP4A, AUDIO_RATE, AUDIO_CHANNELS,
                  TC_AUDIO_LAYOUT_STEREO, 0u);
    mem_file f;
    mux_with_aac(&f, &cfg);
    topos_io src;
    mem_io_src(&f, &src);
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
    topos_audio_track_info ai;
    memset(&ai, 0, sizeof(ai));
    ai.struct_size = (uint32_t)sizeof(ai);
    ai.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_audio_info(mv, &ai), TC_OK);
    MT_CHECK_EQ_U64(ai.codec, TC_AUDIO_CODEC_MP4A);
    MT_CHECK_EQ_U64(ai.sample_rate, AUDIO_RATE);
    MT_CHECK_EQ_U64(ai.channel_count, AUDIO_CHANNELS);
    MT_CHECK_EQ_U64(ai.bits_per_sample, 0u);
    MT_CHECK_EQ_U64(ai.chunk_count, TEST_FRAMES);
    MT_CHECK_EQ_U64(ai.sample_count, (uint64_t)1024u * TEST_FRAMES);

    /* 逐包字节回读（变长 stsz） */
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
        const size_t pkt_size = 64u + (i % 37u);
        uint8_t expect[512];
        for (size_t k = 0; k < pkt_size; ++k) { expect[k] = (uint8_t)((k * 13u + i) & 0xFFu); }
        uint8_t got[512];
        size_t need = 0u;
        MT_CHECK_EQ_I64(tc_movie_read_audio(mv, i, 1u, got, sizeof(got), &need), TC_OK);
        MT_CHECK_EQ_U64(need, pkt_size);
        MT_CHECK(memcmp(got, expect, pkt_size) == 0);
    }
    tc_movie_close(mv);
    mem_free(&f);
}

static void test_aac_validation(void)
{
    /* ASC 重复声明 → STATE */
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg_set_audio(&cfg, TC_AUDIO_CODEC_MP4A, AUDIO_RATE, AUDIO_CHANNELS,
                  TC_AUDIO_LAYOUT_STEREO, 0u);
    mem_file f;
    memset(&f, 0, sizeof(f));
    topos_io sink;
    mem_io_sink(&f, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
    static const uint8_t ASC[2] = { 0x12, 0x10 };
    MT_CHECK_EQ_I64(tc_mux_set_audio_asc(m, ASC, sizeof(ASC)), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_set_audio_asc(m, ASC, sizeof(ASC)), TC_ERR_STATE);
    /* lpcm 档拒绝 ASC */
    tc_mux_free(m);
    mem_free(&f);
    {
        topos_movie_config lc;
        base_movie_cfg(&lc);
        cfg_set_audio(&lc, TC_AUDIO_CODEC_LPCM, AUDIO_RATE, AUDIO_CHANNELS,
                      TC_AUDIO_LAYOUT_STEREO, 16u);
        mem_file g;
        memset(&g, 0, sizeof(g));
        topos_io s2;
        mem_io_sink(&g, &s2);
        topos_mux* m2 = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&lc, &s2, &m2), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_set_audio_asc(m2, ASC, sizeof(ASC)),
                        TC_ERR_INVALID_ARGUMENT);
        tc_mux_free(m2);
        mem_free(&g);
    }
}

static void test_aac_dump(const char* path)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg_set_audio(&cfg, TC_AUDIO_CODEC_MP4A, AUDIO_RATE, AUDIO_CHANNELS,
                  TC_AUDIO_LAYOUT_STEREO, 0u);
    mem_file f;
    mux_with_aac(&f, &cfg);
    FILE* fp = fopen(path, "wb");
    if (fp == NULL) { mt_failures++; mem_free(&f); return; }
    fwrite(f.data, 1, f.len, fp);
    fclose(fp);
    fprintf(stderr, "AAC ffprobe 样本已写出: %s (%zu bytes)\n", path, f.len);
    mem_free(&f);
}

/* ---------- M-B9：tmcd 时间码轨（v1.5） ---------- */

static void mux_with_tc(mem_file* out, uint32_t fps, uint32_t df,
                        uint32_t hh, uint32_t mm, uint32_t ss, uint32_t ff)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    memset(out, 0, sizeof(*out));
    topos_io sink;
    mem_io_sink(out, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
    for (uint32_t i = 0; i < 10u && i < TEST_FRAMES; ++i) {
        MT_CHECK_EQ_I64(
            tc_mux_add_packet(m, g_pkts[i], g_pkt_sizes[i], (uint64_t)i, 1u),
            TC_OK);
    }
    MT_CHECK_EQ_I64(tc_mux_set_timecode(m, hh, mm, ss, ff, fps, df), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    tc_mux_free(m);
}

static long find_fourcc_last(const uint8_t* data, size_t len, const char* typ);

/* 祖先容器 size 修补：对包含偏移 at 的已知容器原子逐层 +delta */
static void patch_ancestor_sizes(uint8_t* buf, size_t len, long at,
                                 uint32_t delta)
{
    static const char* containers[] = { "moov", "trak", "mdia", "minf", "stbl" };
    long pos = 0;
    while (pos + 8 <= (long)len) {
        uint32_t sz = (uint32_t)((buf[pos] << 24) | (buf[pos + 1] << 16) |
                                 (buf[pos + 2] << 8) | buf[pos + 3]);
        /* size==1 → 64 位扩展长度（如 mdat）：按 be64 实长跳过 */
        if (sz == 1u) {
            if (pos + 16 > (long)len) { return; }
            const uint64_t ext =
                ((uint64_t)buf[pos + 8] << 56) | ((uint64_t)buf[pos + 9] << 48) |
                ((uint64_t)buf[pos + 10] << 40) | ((uint64_t)buf[pos + 11] << 32) |
                ((uint64_t)buf[pos + 12] << 24) | ((uint64_t)buf[pos + 13] << 16) |
                ((uint64_t)buf[pos + 14] << 8) | (uint64_t)buf[pos + 15];
            if (ext < 16ull) { return; }
            pos += (long)ext;
            continue;
        }
        if (sz < 8u) { return; }
        int is_container = 0;
        for (size_t k = 0; k < sizeof(containers) / sizeof(containers[0]); ++k) {
            if (memcmp(buf + pos + 4, containers[k], 4) == 0) { is_container = 1; }
        }
        /* 只深入包含插入点且自身不是被替换原子的容器 */
        if (is_container && (long)sz + pos > at) {
            uint32_t nsz = sz + delta;
            buf[pos] = (uint8_t)(nsz >> 24);
            buf[pos + 1] = (uint8_t)(nsz >> 16);
            buf[pos + 2] = (uint8_t)(nsz >> 8);
            buf[pos + 3] = (uint8_t)nsz;
            patch_ancestor_sizes(buf + pos + 8, (size_t)(sz - 8u), at - pos - 8,
                                 delta);
            return;
        }
        pos += (long)sz;
    }
}

/* tmcd 表级畸形：缺 stco 拒绝 + co64 变体读取（>4GB 外部文件合法形态） */
static void test_tmcd_malformed_tables(void)
{
    mem_file f;
    mux_with_tc(&f, 30u, 1u, 1u, 2u, 3u, 12u);
    /* tmcd 恒末轨 → 文件中最后一个 stco 即 tmcd 的 */
    const long stco = find_fourcc_last(f.data, f.len, "stco");
    MT_CHECK(stco > 0);
    /* ① stco fourcc 改为 stsc（tmcd 中合法忽略）→ 缺表 → MALFORMED
     *    （此前静默 start_count=0 + TC_OK，违背规范 A.6/D4 fail-fast） */
    {
        mem_file g = f;
        g.data = (uint8_t*)malloc(g.len);
        memcpy(g.data, f.data, f.len);
        memcpy(g.data + stco, "stsc", 4u);
        topos_io src;
        mem_io_src(&g, &src);
        topos_movie* mv = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_ERR_MALFORMED);
        mem_free(&g);
    }
    /* ② stco（20B）替换为 co64（24B，同偏移升 64 位）→ 读取成功且
     *    时码换算结果与 stco 形态逐字段一致（读侧 co64 容忍验证） */
    {
        const uint32_t off32 = (uint32_t)((f.data[stco + 12] << 24) |
                                          (f.data[stco + 13] << 16) |
                                          (f.data[stco + 14] << 8) |
                                          f.data[stco + 15]);
        /* stco 是 fourcc 位置；原子区间 [stco-4, stco+16)，co64 为
         * [stco-4, stco+20)：size 24 + 'co64' + ver/flags 0 + count 1 +
         * offset u64（高 4B 零，同 32 位值） */
        const size_t nlen = f.len + 4u;
        uint8_t* b = (uint8_t*)malloc(nlen);
        memcpy(b, f.data, (size_t)(stco + 16));
        b[stco - 4] = 0u; b[stco - 3] = 0u; b[stco - 2] = 0u; b[stco - 1] = 24u;
        memcpy(b + stco, "co64", 4u);
        memset(b + stco + 4, 0, 8u);             /* ver/flags + count 高位 */
        b[stco + 11] = 1u;                       /* entry_count = 1 */
        memset(b + stco + 12, 0, 8u);
        b[stco + 16] = (uint8_t)(off32 >> 24);   /* offset u64 低 4B */
        b[stco + 17] = (uint8_t)(off32 >> 16);
        b[stco + 18] = (uint8_t)(off32 >> 8);
        b[stco + 19] = (uint8_t)off32;
        memcpy(b + stco + 20, f.data + stco + 16, f.len - (size_t)stco - 16u);
        patch_ancestor_sizes(b, nlen, stco, 4u);
        mem_file g;
        g.data = b;
        g.len = nlen;
        topos_io src;
        mem_io_src(&g, &src);
        topos_movie* mv = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
        topos_timecode_info ti;
        memset(&ti, 0, sizeof(ti));
        ti.struct_size = (uint32_t)sizeof(ti);
        ti.abi_version = TOPOS_CODEC_ABI_VERSION;
        MT_CHECK_EQ_I64(tc_movie_timecode(mv, &ti), TC_OK);
        MT_CHECK_EQ_U64(ti.hh, 1u);
        MT_CHECK_EQ_U64(ti.mm, 2u);
        MT_CHECK_EQ_U64(ti.ss, 3u);
        MT_CHECK_EQ_U64(ti.ff, 12u);
        tc_movie_close(mv);
        free(b);
    }
    mem_free(&f);
}

/* ---------- M-B8：多音轨 / stems（v1.6） ---------- */

/* 声明一条音轨的便捷封装 */
static void track_cfg_init(topos_audio_track_config* tc, uint32_t codec,
                           uint32_t rate, uint32_t channels, uint32_t layout,
                           uint32_t bits, uint32_t fmt, const char* name)
{
    memset(tc, 0, sizeof(*tc));
    tc->struct_size = (uint32_t)sizeof(*tc);
    tc->abi_version = TOPOS_CODEC_ABI_VERSION;
    tc->codec = codec;
    tc->sample_rate = rate;
    tc->channel_count = channels;
    tc->channel_layout = layout;
    tc->bits_per_sample = bits;
    tc->sample_format = fmt;
    tc->name = name;
}

/* 双轨 stems：轨 0 = lpcm float32 立体声（Music）、轨 1 = lpcm s16 mono
 *（Dialogue）；每轨独立数据。 */
static void mux_with_stems(mem_file* out, uint32_t frames)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);   /* reserved 槽位全 0：音频全走 add_audio_track */
    memset(out, 0, sizeof(*out));
    topos_io sink;
    mem_io_sink(out, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
    for (uint32_t i = 0; i < 4u && i < TEST_FRAMES; ++i) {
        MT_CHECK_EQ_I64(
            tc_mux_add_packet(m, g_pkts[i], g_pkt_sizes[i], (uint64_t)i, 1u),
            TC_OK);
    }
    topos_audio_track_config tc;
    track_cfg_init(&tc, TC_AUDIO_CODEC_LPCM, 48000u, 2u,
                   TC_AUDIO_LAYOUT_STEREO, 32u, TC_AUDIO_FMT_FLOAT32, "Music");
    MT_CHECK_EQ_I64(tc_mux_add_audio_track(m, &tc), TC_OK);
    track_cfg_init(&tc, TC_AUDIO_CODEC_LPCM, 48000u, 1u,
                   TC_AUDIO_LAYOUT_MONO, 16u, TC_AUDIO_FMT_INT, "Dialogue");
    MT_CHECK_EQ_I64(tc_mux_add_audio_track(m, &tc), TC_OK);
    for (uint32_t i = 0; i < frames; ++i) {
        for (uint32_t ch = 0; ch < 8u; ++ch) { /* f32 数据填充在下方 */
        }
        uint8_t f32[2 * 4u];
        for (uint32_t k = 0; k < sizeof(f32); ++k) {
            f32[k] = (uint8_t)((i * 7u + k * 13u + 1u) & 0xFFu);
        }
        MT_CHECK_EQ_I64(tc_mux_add_audio_to(m, 0u, f32, sizeof(f32), 1u),
                        TC_OK);
        uint8_t s16[1 * 2u];
        for (uint32_t k = 0; k < sizeof(s16); ++k) {
            s16[k] = (uint8_t)((i * 11u + k * 5u + 2u) & 0xFFu);
        }
        MT_CHECK_EQ_I64(tc_mux_add_audio_to(m, 1u, s16, sizeof(s16), 1u),
                        TC_OK);
    }
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    tc_mux_free(m);
}

static void test_multitrack_roundtrip(void)
{
    mem_file f;
    mux_with_stems(&f, 8u);
    topos_io src;
    mem_io_src(&f, &src);
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);

    /* 轨数 + 逐轨信息（含轨名） */
    uint32_t n_tracks = 0u;
    MT_CHECK_EQ_I64(tc_movie_audio_track_count(mv, &n_tracks), TC_OK);
    MT_CHECK_EQ_U64(n_tracks, 2u);
    topos_audio_track_info ai;
    memset(&ai, 0, sizeof(ai));
    ai.struct_size = (uint32_t)sizeof(ai);
    ai.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_audio_info_at(mv, 0u, &ai), TC_OK);
    MT_CHECK_EQ_U64(ai.codec, TC_AUDIO_CODEC_LPCM);
    MT_CHECK_EQ_U64(ai.channel_count, 2u);
    MT_CHECK_EQ_U64(ai.channel_layout, TC_AUDIO_LAYOUT_STEREO);
    MT_CHECK_EQ_U64(ai.bits_per_sample, 32u);
    MT_CHECK_EQ_U64(ai.reserved[1], TC_AUDIO_FMT_FLOAT32);
    MT_CHECK_EQ_U64(ai.sample_count, 8u);
    MT_CHECK(strcmp(ai.name, "Music") == 0);
    memset(&ai, 0, sizeof(ai));
    ai.struct_size = (uint32_t)sizeof(ai);
    ai.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_audio_info_at(mv, 1u, &ai), TC_OK);
    MT_CHECK_EQ_U64(ai.channel_count, 1u);
    MT_CHECK_EQ_U64(ai.channel_layout, TC_AUDIO_LAYOUT_MONO);
    MT_CHECK_EQ_U64(ai.bits_per_sample, 16u);
    MT_CHECK(strcmp(ai.name, "Dialogue") == 0);

    /* 逐轨字节回读一致 */
    uint8_t got[64];
    for (uint32_t i = 0; i < 8u; ++i) {
        MT_CHECK_EQ_I64(tc_movie_read_audio_at(mv, 0u, i, 1u, got, sizeof(got), NULL),
                        TC_OK);
        for (uint32_t k = 0; k < 8u; ++k) {
            MT_CHECK_EQ_U64(got[k], (uint8_t)((i * 7u + k * 13u + 1u) & 0xFFu));
        }
        MT_CHECK_EQ_I64(tc_movie_read_audio_at(mv, 1u, i, 1u, got, sizeof(got), NULL),
                        TC_OK);
        for (uint32_t k = 0; k < 2u; ++k) {
            MT_CHECK_EQ_U64(got[k], (uint8_t)((i * 11u + k * 5u + 2u) & 0xFFu));
        }
    }
    /* 旧 API = 轨 0 视图 */
    MT_CHECK_EQ_I64(tc_movie_read_audio(mv, 0u, 1u, got, sizeof(got), NULL), TC_OK);
    MT_CHECK_EQ_U64(got[0], (uint8_t)1u);
    /* 越界轨索引拒绝 */
    MT_CHECK_EQ_I64(tc_movie_audio_info_at(mv, 2u, &ai), TC_ERR_INVALID_ARGUMENT);
    MT_CHECK_EQ_I64(tc_movie_read_audio_at(mv, 2u, 0u, 1u, got, sizeof(got), NULL),
                    TC_ERR_INVALID_ARGUMENT);
    tc_movie_close(mv);
    mem_free(&f);

    /* faststart 保留多轨 */
    mem_file f2;
    mux_with_stems(&f2, 8u);
    topos_io s2;
    mem_io_src(&f2, &s2);
    mem_file dst;
    memset(&dst, 0, sizeof(dst));
    topos_io dio;
    mem_io_sink(&dst, &dio);
    MT_CHECK_EQ_I64(tc_movie_faststart(&s2, &dio), TC_OK);
    topos_io r2;
    mem_io_src(&dst, &r2);
    topos_movie* mv2 = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&r2, &mv2), TC_OK);
    uint32_t n2 = 0u;
    MT_CHECK_EQ_I64(tc_movie_audio_track_count(mv2, &n2), TC_OK);
    MT_CHECK_EQ_U64(n2, 2u);
    memset(&ai, 0, sizeof(ai));
    ai.struct_size = (uint32_t)sizeof(ai);
    ai.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_audio_info_at(mv2, 1u, &ai), TC_OK);
    MT_CHECK(strcmp(ai.name, "Dialogue") == 0);
    MT_CHECK_EQ_I64(tc_movie_read_audio_at(mv2, 1u, 0u, 1u, got, sizeof(got), NULL),
                    TC_OK);
    MT_CHECK_EQ_U64(got[0], (uint8_t)2u);
    tc_movie_close(mv2);
    mem_free(&dst);
    mem_free(&f2);

    /* 校验：轨数超限（17 条）→ LIMIT_EXCEEDED */
    {
        topos_movie_config nc;
        base_movie_cfg(&nc);
        mem_file g;
        memset(&g, 0, sizeof(g));
        topos_io s3;
        mem_io_sink(&g, &s3);
        topos_mux* m3 = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&nc, &s3, &m3), TC_OK);
        topos_audio_track_config tc;
        for (uint32_t i = 0; i < 17u; ++i) {
            track_cfg_init(&tc, TC_AUDIO_CODEC_LPCM, 48000u, 1u,
                           TC_AUDIO_LAYOUT_MONO, 16u, TC_AUDIO_FMT_INT, NULL);
            int32_t rc = tc_mux_add_audio_track(m3, &tc);
            if (i < 16u) {
                MT_CHECK_EQ_I64(rc, TC_OK);
            } else {
                MT_CHECK_EQ_I64(rc, TC_ERR_LIMIT_EXCEEDED);
            }
        }
        tc_mux_free(m3);
        mem_free(&g);
    }
    /* 校验：非法格式（float32 配 mp4a / 声道布局不配对 / NONE）→ INVALID */
    {
        topos_movie_config nc;
        base_movie_cfg(&nc);
        mem_file g;
        memset(&g, 0, sizeof(g));
        topos_io s3;
        mem_io_sink(&g, &s3);
        topos_mux* m3 = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&nc, &s3, &m3), TC_OK);
        topos_audio_track_config tc;
        track_cfg_init(&tc, TC_AUDIO_CODEC_NONE, 48000u, 2u,
                       TC_AUDIO_LAYOUT_STEREO, 16u, TC_AUDIO_FMT_INT, NULL);
        MT_CHECK_EQ_I64(tc_mux_add_audio_track(m3, &tc), TC_ERR_INVALID_ARGUMENT);
        track_cfg_init(&tc, TC_AUDIO_CODEC_MP4A, 48000u, 2u,
                       TC_AUDIO_LAYOUT_STEREO, 0u, TC_AUDIO_FMT_FLOAT32, NULL);
        MT_CHECK_EQ_I64(tc_mux_add_audio_track(m3, &tc), TC_ERR_INVALID_ARGUMENT);
        track_cfg_init(&tc, TC_AUDIO_CODEC_LPCM, 48000u, 1u,
                       TC_AUDIO_LAYOUT_STEREO, 16u, TC_AUDIO_FMT_INT, NULL);
        MT_CHECK_EQ_I64(tc_mux_add_audio_track(m3, &tc), TC_ERR_INVALID_ARGUMENT);
        track_cfg_init(&tc, TC_AUDIO_CODEC_LPCM, 48000u, 2u,
                       TC_AUDIO_LAYOUT_STEREO, 32u, TC_AUDIO_FMT_INT, NULL);
        MT_CHECK_EQ_I64(tc_mux_add_audio_track(m3, &tc), TC_OK);
        /* 未声明 ASC 直接 add mp4a 由 _to 路径拒；此处 add_audio_to 未声明轨 */
        MT_CHECK_EQ_I64(tc_mux_add_audio_to(m3, 1u, g.data, 4u, 1u),
                        TC_ERR_INVALID_ARGUMENT);
        /* mp4a 轨缺 ASC → STATE */
        track_cfg_init(&tc, TC_AUDIO_CODEC_MP4A, 48000u, 2u,
                       TC_AUDIO_LAYOUT_STEREO, 0u, TC_AUDIO_FMT_INT, NULL);
        MT_CHECK_EQ_I64(tc_mux_add_audio_track(m3, &tc), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_add_audio_to(m3, 1u, g.data, 4u, 1u), TC_ERR_STATE);
        /* finish：声明轨无样本 → INVALID（半声明） */
        MT_CHECK_EQ_I64(tc_mux_finish(m3), TC_ERR_INVALID_ARGUMENT);
        tc_mux_free(m3);
        mem_free(&g);
    }
    /* v1.1 单轨文件经新 API 读出 = 旧 API 视图（轨 0） */
    {
        topos_movie_config cfg;
        base_movie_cfg(&cfg);
        cfg_set_audio(&cfg, TC_AUDIO_CODEC_LPCM, AUDIO_RATE, AUDIO_CHANNELS,
                      TC_AUDIO_LAYOUT_STEREO, AUDIO_BITS);
        mem_file g;
        mux_with_audio(&g, &cfg, 1);
        topos_io s4;
        mem_io_src(&g, &s4);
        topos_movie* mv4 = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&s4, &mv4), TC_OK);
        uint32_t n4 = 0u;
        MT_CHECK_EQ_I64(tc_movie_audio_track_count(mv4, &n4), TC_OK);
        MT_CHECK_EQ_U64(n4, 1u);
        memset(&ai, 0, sizeof(ai));
        ai.struct_size = (uint32_t)sizeof(ai);
        ai.abi_version = TOPOS_CODEC_ABI_VERSION;
        MT_CHECK_EQ_I64(tc_movie_audio_info_at(mv4, 0u, &ai), TC_OK);
        MT_CHECK_EQ_U64(ai.sample_count,
                        (uint64_t)AUDIO_CHUNK_FRAMES * TEST_FRAMES);
        size_t need = 0u;
        uint8_t* buf = (uint8_t*)malloc(1u << 20);
        MT_CHECK_EQ_I64(tc_movie_read_audio_at(mv4, 0u, 0u, TEST_FRAMES, buf,
                                               1u << 20, &need), TC_OK);
        MT_CHECK_EQ_I64(tc_movie_read_audio(mv4, 0u, TEST_FRAMES, buf,
                                            1u << 20, &need), TC_OK);
        free(buf);
        tc_movie_close(mv4);
        mem_free(&g);
    }
}

static void test_timecode_roundtrip_matrix(void)
{
    static const struct {
        uint32_t fps, df, hh, mm, ss, ff;
        uint32_t want_count; /* DF 计数 = wall - 2*(分钟 - 分钟/10) */
    } CASES[] = {
        { 24u, 0u, 1u, 2u, 3u, 12u, 89364u },
        { 25u, 0u, 0u, 0u, 10u, 20u, 270u },
        { 30u, 0u, 0u, 0u, 10u, 25u, 325u },
        { 30u, 1u, 0u, 0u, 59u, 29u, 1799u },
        /* 60DF：分钟首帧 ff≥4（SMPTE 12M 跳 4；oracle '00:01:00;04'→3600） */
        { 60u, 1u, 0u, 1u, 0u, 4u, 3600u },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); ++i) {
        mem_file f;
        mux_with_tc(&f, CASES[i].fps, CASES[i].df, CASES[i].hh, CASES[i].mm,
                    CASES[i].ss, CASES[i].ff);
        topos_io src;
        mem_io_src(&f, &src);
        topos_movie* mv = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
        topos_timecode_info ti;
        memset(&ti, 0, sizeof(ti));
        ti.struct_size = (uint32_t)sizeof(ti);
        ti.abi_version = TOPOS_CODEC_ABI_VERSION;
        MT_CHECK_EQ_I64(tc_movie_timecode(mv, &ti), TC_OK);
        MT_CHECK_EQ_U64(ti.start_frame_count, CASES[i].want_count);
        MT_CHECK_EQ_U64((uint32_t)ti.hh, CASES[i].hh);
        MT_CHECK_EQ_U64((uint32_t)ti.mm, CASES[i].mm);
        MT_CHECK_EQ_U64((uint32_t)ti.ss, CASES[i].ss);
        MT_CHECK_EQ_U64((uint32_t)ti.ff, CASES[i].ff);
        MT_CHECK_EQ_U64(ti.fps, CASES[i].fps);
        MT_CHECK_EQ_U64(ti.drop_frame, CASES[i].df);
        tc_movie_close(mv);
        /* faststart 保留时码 */
        mem_file fs;
        memset(&fs, 0, sizeof(fs));
        topos_io dst;
        mem_io_sink(&fs, &dst);
        MT_CHECK_EQ_I64(tc_movie_faststart(&src, &dst), TC_OK);
        topos_io src2;
        mem_io_src(&fs, &src2);
        topos_movie* mv2 = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&src2, &mv2), TC_OK);
        topos_timecode_info ti2;
        memset(&ti2, 0, sizeof(ti2));
        ti2.struct_size = (uint32_t)sizeof(ti2);
        ti2.abi_version = TOPOS_CODEC_ABI_VERSION;
        MT_CHECK_EQ_I64(tc_movie_timecode(mv2, &ti2), TC_OK);
        MT_CHECK_EQ_U64(ti2.start_frame_count, CASES[i].want_count);
        MT_CHECK_EQ_U64(ti2.drop_frame, CASES[i].df);
        tc_movie_close(mv2);
        mem_free(&fs);
        mem_free(&f);
    }
}

static void test_timecode_validation(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file f;
    memset(&f, 0, sizeof(f));
    topos_io sink;
    mem_io_sink(&f, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[0], g_pkt_sizes[0], 0u, 1u),
                    TC_OK);
    /* fps 白名单外 */
    MT_CHECK_EQ_I64(tc_mux_set_timecode(m, 0, 0, 0, 0, 23u, 0u),
                    TC_ERR_INVALID_ARGUMENT);
    /* DF 配非 30/60 */
    MT_CHECK_EQ_I64(tc_mux_set_timecode(m, 0, 0, 0, 0, 24u, 1u),
                    TC_ERR_INVALID_ARGUMENT);
    /* 分量越界 */
    MT_CHECK_EQ_I64(tc_mux_set_timecode(m, 24u, 0, 0, 0, 24u, 0u),
                    TC_ERR_INVALID_ARGUMENT);
    MT_CHECK_EQ_I64(tc_mux_set_timecode(m, 0, 60u, 0, 0, 24u, 0u),
                    TC_ERR_INVALID_ARGUMENT);
    MT_CHECK_EQ_I64(tc_mux_set_timecode(m, 0, 0, 0, 24u, 24u, 0u),
                    TC_ERR_INVALID_ARGUMENT);
    /* DF 分钟首帧标签无效：非 10 分钟整的分钟 ss==0 时 30DF 拒 ff<2、
     * 60DF 拒 ff<4（与 media 层 Timecode 同规） */
    MT_CHECK_EQ_I64(tc_mux_set_timecode(m, 0, 1u, 0, 1u, 30u, 1u),
                    TC_ERR_INVALID_ARGUMENT);
    MT_CHECK_EQ_I64(tc_mux_set_timecode(m, 0, 1u, 0, 3u, 60u, 1u),
                    TC_ERR_INVALID_ARGUMENT);
    /* 合法声明 + 至多一次 */
    MT_CHECK_EQ_I64(tc_mux_set_timecode(m, 1u, 2u, 3u, 12u, 24u, 0u), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_set_timecode(m, 1u, 2u, 3u, 12u, 24u, 0u),
                    TC_ERR_STATE);
    /* 无 TC 文件：tc_movie_timecode → STATE */
    tc_mux_free(m);
    mem_free(&f);
    {
        mem_file g;
        mux_with_tc(&g, 24u, 0u, 1u, 2u, 3u, 12u);
        /* 有 TC 文件正常；构造无 TC 对照 */
        mem_file h;
        base_movie_cfg(&cfg);
        memset(&h, 0, sizeof(h));
        topos_io s2;
        mem_io_sink(&h, &s2);
        topos_mux* m2 = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &s2, &m2), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_add_packet(m2, g_pkts[0], g_pkt_sizes[0], 0u, 1u),
                        TC_OK);
        MT_CHECK_EQ_I64(tc_mux_finish(m2), TC_OK);
        tc_mux_free(m2);
        topos_io src;
        mem_io_src(&h, &src);
        topos_movie* mv = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
        topos_timecode_info ti;
        memset(&ti, 0, sizeof(ti));
        ti.struct_size = (uint32_t)sizeof(ti);
        ti.abi_version = TOPOS_CODEC_ABI_VERSION;
        MT_CHECK_EQ_I64(tc_movie_timecode(mv, &ti), TC_ERR_STATE);
        tc_movie_close(mv);
        mem_free(&h);
        mem_free(&g);
    }
}

/* ---------- M-B4：全家族采样率 + float32（v1.4） ---------- */

/* 采样率×位深 roundtrip（复用 mux_with_audio；rate 变化不改 chunk 结构） */
static void test_rate_family_roundtrip(void)
{
    static const uint32_t RATES[] = { 44100u, 48000u, 88200u, 96000u,
                                      176400u, 192000u };
    for (size_t i = 0; i < sizeof(RATES) / sizeof(RATES[0]); ++i) {
        topos_movie_config cfg;
        base_movie_cfg(&cfg);
        cfg_set_audio(&cfg, TC_AUDIO_CODEC_LPCM, RATES[i], AUDIO_CHANNELS,
                      TC_AUDIO_LAYOUT_STEREO, 16u);
        mem_file f;
        mux_with_audio(&f, &cfg, 1);
        topos_io src;
        mem_io_src(&f, &src);
        topos_movie* mv = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
        topos_audio_track_info ai;
        memset(&ai, 0, sizeof(ai));
        ai.struct_size = (uint32_t)sizeof(ai);
        ai.abi_version = TOPOS_CODEC_ABI_VERSION;
        MT_CHECK_EQ_I64(tc_movie_audio_info(mv, &ai), TC_OK);
        MT_CHECK_EQ_U64(ai.sample_rate, RATES[i]);
        MT_CHECK_EQ_U64(ai.reserved[1], TC_AUDIO_FMT_INT);
        tc_movie_close(mv);
        mem_free(&f);
    }
    /* 非法采样率 → INVALID_ARGUMENT（create 期 fail-fast） */
    {
        topos_movie_config cfg;
        base_movie_cfg(&cfg);
        cfg_set_audio(&cfg, TC_AUDIO_CODEC_LPCM, 22050u, AUDIO_CHANNELS,
                      TC_AUDIO_LAYOUT_STEREO, 16u);
        mem_file f;
        memset(&f, 0, sizeof(f));
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_ERR_INVALID_ARGUMENT);
        mem_free(&f);
    }
}

/* float32：formatFlags 0xB 写读 + sample_format 读回 + 校验拒绝 */
static void test_float32_roundtrip_and_validation(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg_set_audio_fmt(&cfg, TC_AUDIO_CODEC_LPCM, AUDIO_RATE, AUDIO_CHANNELS,
                      TC_AUDIO_LAYOUT_STEREO, 32u, TC_AUDIO_FMT_FLOAT32);
    mem_file f;
    mux_with_audio(&f, &cfg, 1);
    topos_io src;
    mem_io_src(&f, &src);
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
    topos_audio_track_info ai;
    memset(&ai, 0, sizeof(ai));
    ai.struct_size = (uint32_t)sizeof(ai);
    ai.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_audio_info(mv, &ai), TC_OK);
    MT_CHECK_EQ_U64(ai.bits_per_sample, 32u);
    MT_CHECK_EQ_U64(ai.reserved[1], TC_AUDIO_FMT_FLOAT32);
    /* chunk 字节逐位读回（'twos' 容器语义：原样存储不解释）；
     * 期望字节与 mux_with_audio 的填充序列一致（k*7+i*31，chunk=5） */
    const size_t fb32 = (size_t)AUDIO_CHANNELS * 4u;
    uint8_t expect[AUDIO_CHUNK_FRAMES * 8u];
    for (size_t k = 0; k < (size_t)AUDIO_CHUNK_FRAMES * fb32; ++k) {
        expect[k] = (uint8_t)((k * 7u + 5u * 31u) & 0xFFu);
    }
    uint8_t got[AUDIO_CHUNK_FRAMES * 8u];
    size_t need = 0u;
    MT_CHECK_EQ_I64(tc_movie_read_audio(mv, 5u, 1u, got, sizeof(got), &need),
                    TC_OK);
    MT_CHECK_EQ_U64(need, (size_t)AUDIO_CHUNK_FRAMES * fb32);
    MT_CHECK(memcmp(got, expect, (size_t)AUDIO_CHUNK_FRAMES * fb32) == 0);
    tc_movie_close(mv);
    mem_free(&f);

    /* 校验拒绝：float 配 mp4a / float 配 24bit / 非法标志位 */
    {
        topos_movie_config bad;
        base_movie_cfg(&bad);
        cfg_set_audio_fmt(&bad, TC_AUDIO_CODEC_MP4A, AUDIO_RATE,
                          AUDIO_CHANNELS, TC_AUDIO_LAYOUT_STEREO, 0u,
                          TC_AUDIO_FMT_FLOAT32);
        mem_file g;
        memset(&g, 0, sizeof(g));
        topos_io s2;
        mem_io_sink(&g, &s2);
        topos_mux* m2 = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&bad, &s2, &m2), TC_ERR_INVALID_ARGUMENT);
        mem_free(&g);
    }
    {
        topos_movie_config bad;
        base_movie_cfg(&bad);
        cfg_set_audio_fmt(&bad, TC_AUDIO_CODEC_LPCM, AUDIO_RATE,
                          AUDIO_CHANNELS, TC_AUDIO_LAYOUT_STEREO, 24u,
                          TC_AUDIO_FMT_FLOAT32);
        mem_file g;
        memset(&g, 0, sizeof(g));
        topos_io s2;
        mem_io_sink(&g, &s2);
        topos_mux* m2 = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&bad, &s2, &m2), TC_ERR_INVALID_ARGUMENT);
        mem_free(&g);
    }
    {
        topos_movie_config bad;
        base_movie_cfg(&bad);
        cfg_set_audio_fmt(&bad, TC_AUDIO_CODEC_LPCM, AUDIO_RATE,
                          AUDIO_CHANNELS, TC_AUDIO_LAYOUT_STEREO, 16u, 0x2u);
        mem_file g;
        memset(&g, 0, sizeof(g));
        topos_io s2;
        mem_io_sink(&g, &s2);
        topos_mux* m2 = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&bad, &s2, &m2), TC_ERR_INVALID_ARGUMENT);
        mem_free(&g);
    }
}

/* ---------- M-B1：elst/priming（v1.4，ADR-C052） ---------- */

/* 四字节类型在缓冲中的偏移。mdat 内合成包数据可能碰巧含同名字节，
 * 原子四字搜索必须取最后一次出现（moov 恒为末尾顶层原子）。 */
static long find_fourcc_last(const uint8_t* data, size_t len, const char* typ)
{
    for (long i = (long)len - 4; i >= 0; --i) {
        if (memcmp(data + i, typ, 4) == 0) { return i; }
    }
    return -1;
}

static void test_priming_roundtrip(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg_set_audio(&cfg, TC_AUDIO_CODEC_MP4A, AUDIO_RATE, AUDIO_CHANNELS,
                  TC_AUDIO_LAYOUT_STEREO, 0u);
    /* priming=2048 → 读回相等 */
    mem_file f;
    mux_with_aac_ex(&f, &cfg, 2048u);
    topos_io src;
    mem_io_src(&f, &src);
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
    topos_audio_track_info ai;
    memset(&ai, 0, sizeof(ai));
    ai.struct_size = (uint32_t)sizeof(ai);
    ai.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_audio_info(mv, &ai), TC_OK);
    MT_CHECK_EQ_U64(ai.reserved[0], 2048u);
    MT_CHECK_EQ_U64(ai.sample_count, (uint64_t)1024u * TEST_FRAMES);
    tc_movie_close(mv);
    /* faststart 重写后 elst 保留 */
    mem_file fs;
    memset(&fs, 0, sizeof(fs));
    topos_io dst;
    mem_io_sink(&fs, &dst);
    MT_CHECK_EQ_I64(tc_movie_faststart(&src, &dst), TC_OK);
    topos_io src2;
    mem_io_src(&fs, &src2);
    topos_movie* mv2 = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&src2, &mv2), TC_OK);
    topos_audio_track_info ai2;
    memset(&ai2, 0, sizeof(ai2));
    ai2.struct_size = (uint32_t)sizeof(ai2);
    ai2.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_audio_info(mv2, &ai2), TC_OK);
    MT_CHECK_EQ_U64(ai2.reserved[0], 2048u);
    tc_movie_close(mv2);
    mem_free(&fs);
    mem_free(&f);

    /* 未声明 priming → reserved[0] = 0（旧文件兼容语义） */
    mem_file g;
    mux_with_aac_ex(&g, &cfg, 0u);
    topos_io src3;
    mem_io_src(&g, &src3);
    topos_movie* mv3 = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&src3, &mv3), TC_OK);
    topos_audio_track_info ai3;
    memset(&ai3, 0, sizeof(ai3));
    ai3.struct_size = (uint32_t)sizeof(ai3);
    ai3.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_audio_info(mv3, &ai3), TC_OK);
    MT_CHECK_EQ_U64(ai3.reserved[0], 0u);
    tc_movie_close(mv3);
    mem_free(&g);
}

static void test_priming_validation(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg_set_audio(&cfg, TC_AUDIO_CODEC_MP4A, AUDIO_RATE, AUDIO_CHANNELS,
                  TC_AUDIO_LAYOUT_STEREO, 0u);
    static const uint8_t ASC[2] = { 0x12, 0x10 };
    mem_file f;
    memset(&f, 0, sizeof(f));
    topos_io sink;
    mem_io_sink(&f, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_set_audio_asc(m, ASC, sizeof(ASC)), TC_OK);
    /* 未写任何音频样本（total=0）→ 越界拒绝 */
    MT_CHECK_EQ_I64(tc_mux_set_audio_priming(m, 1024u), TC_ERR_INVALID_ARGUMENT);
    uint8_t pkt[80];
    memset(pkt, 0, sizeof(pkt));
    MT_CHECK_EQ_I64(tc_mux_add_audio(m, pkt, sizeof(pkt), 1024u), TC_OK);
    /* 0 → 拒绝（0 = 不写 elst，无需声明） */
    MT_CHECK_EQ_I64(tc_mux_set_audio_priming(m, 0u), TC_ERR_INVALID_ARGUMENT);
    /* ≥ 已写总采样数（1024）→ 拒绝 */
    MT_CHECK_EQ_I64(tc_mux_set_audio_priming(m, 1024u), TC_ERR_INVALID_ARGUMENT);
    MT_CHECK_EQ_I64(tc_mux_set_audio_priming(m, 1023u), TC_OK);
    /* 至多一次 */
    MT_CHECK_EQ_I64(tc_mux_set_audio_priming(m, 512u), TC_ERR_STATE);
    tc_mux_free(m);
    mem_free(&f);
    /* lpcm 档拒绝 */
    {
        topos_movie_config lc;
        base_movie_cfg(&lc);
        cfg_set_audio(&lc, TC_AUDIO_CODEC_LPCM, AUDIO_RATE, AUDIO_CHANNELS,
                      TC_AUDIO_LAYOUT_STEREO, 16u);
        mem_file g;
        memset(&g, 0, sizeof(g));
        topos_io s2;
        mem_io_sink(&g, &s2);
        topos_mux* m2 = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&lc, &s2, &m2), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_set_audio_priming(m2, 1024u),
                        TC_ERR_INVALID_ARGUMENT);
        tc_mux_free(m2);
        mem_free(&g);
    }
    /* 无音频声明（NONE）拒绝 */
    {
        topos_movie_config nc;
        base_movie_cfg(&nc);
        mem_file g;
        memset(&g, 0, sizeof(g));
        topos_io s2;
        mem_io_sink(&g, &s2);
        topos_mux* m2 = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&nc, &s2, &m2), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_set_audio_priming(m2, 1024u),
                        TC_ERR_INVALID_ARGUMENT);
        tc_mux_free(m2);
        mem_free(&g);
    }
}

/* 畸形 elst（字节手术）→ TC_ERR_MALFORMED，不静默成功 */
static void test_priming_malformed(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg_set_audio(&cfg, TC_AUDIO_CODEC_MP4A, AUDIO_RATE, AUDIO_CHANNELS,
                  TC_AUDIO_LAYOUT_STEREO, 0u);
    mem_file f;
    mux_with_aac_ex(&f, &cfg, 2048u);

    const long elst = find_fourcc_last(f.data, f.len, "elst");
    MT_CHECK(elst > 0);
    /* elst body：ver/flags(4) count(4) seg(4) media_time(4) rate(4) */
    uint8_t* elst_body = f.data + elst + 4;

    /* ① entry_count 1→2 */
    {
        mem_file g = f;
        g.data = (uint8_t*)malloc(g.len);
        memcpy(g.data, f.data, g.len);
        uint8_t* b = g.data + elst + 4;
        b[4] = 0u; b[5] = 0u; b[6] = 0u; b[7] = 2u;
        topos_io src;
        mem_io_src(&g, &src);
        topos_movie* mv = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_ERR_MALFORMED);
        mem_free(&g);
    }
    /* ② media_time → 0 */
    {
        mem_file g = f;
        g.data = (uint8_t*)malloc(g.len);
        memcpy(g.data, f.data, g.len);
        uint8_t* b = g.data + elst + 4;
        b[12] = 0u; b[13] = 0u; b[14] = 0u; b[15] = 0u;
        topos_io src;
        mem_io_src(&g, &src);
        topos_movie* mv = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_ERR_MALFORMED);
        mem_free(&g);
    }
    /* ③ media_time → 0x7FFFFFFF（≥ 轨总采样数 1024×96） */
    {
        mem_file g = f;
        g.data = (uint8_t*)malloc(g.len);
        memcpy(g.data, f.data, g.len);
        uint8_t* b = g.data + elst + 4;
        b[12] = 0x7Fu; b[13] = 0xFFu; b[14] = 0xFFu; b[15] = 0xFFu;
        topos_io src;
        mem_io_src(&g, &src);
        topos_movie* mv = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_ERR_MALFORMED);
        mem_free(&g);
    }
    /* ④ media_rate 1.0 → 2.0 */
    {
        mem_file g = f;
        g.data = (uint8_t*)malloc(g.len);
        memcpy(g.data, f.data, g.len);
        uint8_t* b = g.data + elst + 4;
        b[16] = 0x00u; b[17] = 0x02u; b[18] = 0x00u; b[19] = 0x00u; /* 2.0 */
        topos_io src;
        mem_io_src(&g, &src);
        topos_movie* mv = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_ERR_MALFORMED);
        mem_free(&g);
    }
    /* ⑤ version ≠ 0（v1 elst 64 位字段不支持） */
    {
        mem_file g = f;
        g.data = (uint8_t*)malloc(g.len);
        memcpy(g.data, f.data, g.len);
        uint8_t* b = g.data + elst + 4;
        b[0] = 1u;
        topos_io src;
        mem_io_src(&g, &src);
        topos_movie* mv = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_ERR_MALFORMED);
        mem_free(&g);
    }
    mem_free(&f);

    /* ⑥ 视频 trak 插入 edts → 拒绝（输入域不变：视频 pts 无空洞约束） */
    {
        mem_file g;
        mux_with_aac_ex(&g, &cfg, 0u); /* 无音频 elst 的基线文件 */
        const long moov0 = find_fourcc_last(g.data, g.len, "moov");
        MT_CHECK(moov0 > 0);
        long trak = -1;
        for (long i = moov0; i + 4 <= (long)g.len; ++i) {
            if (memcmp(g.data + i, "trak", 4) == 0) { trak = i; break; }
        }
        MT_CHECK(trak > 0);
        /* trak 为 fourcc 偏移：其 size 在 trak-4，body 自 trak+4 起；
         * 首子原子 tkhd 的 size 字段在 trak+4（trak+8 是 'tkhd' 字节本身） */
        const uint32_t tkhd_size =
            (uint32_t)((g.data[trak + 4] << 24) | (g.data[trak + 5] << 16) |
                       (g.data[trak + 6] << 8) | g.data[trak + 7]);
        MT_CHECK(tkhd_size >= 8u && tkhd_size < 256u);
        const long insert_at = trak + 4 + (long)tkhd_size; /* tkhd 原子之后 */
        static const uint8_t EDTS[36] = {
            0x00, 0x00, 0x00, 0x24, 'e', 'd', 't', 's',
            0x00, 0x00, 0x00, 0x1C, 'e', 'l', 's', 't',
            0x00, 0x00, 0x00, 0x00,  /* ver/flags */
            0x00, 0x00, 0x00, 0x01,  /* entry_count */
            0x00, 0x00, 0x00, 0x64,  /* segment_duration */
            0x00, 0x00, 0x00, 0x0A,  /* media_time */
            0x00, 0x01, 0x00, 0x00,  /* media_rate 1.0 */
        };
        uint8_t* bigger = (uint8_t*)malloc(g.len + sizeof(EDTS));
        memcpy(bigger, g.data, (size_t)insert_at);
        memcpy(bigger + insert_at, EDTS, sizeof(EDTS));
        memcpy(bigger + insert_at + sizeof(EDTS), g.data + insert_at,
               g.len - (size_t)insert_at);
        /* 祖先 size 修补：trak（首个 'trak' 前 4B）与 moov */
        const long moov = find_fourcc_last(bigger, g.len + sizeof(EDTS), "moov");
        MT_CHECK(moov > 0);
        uint32_t trak_size = (uint32_t)((bigger[trak - 4] << 24) |
                                        (bigger[trak - 3] << 16) |
                                        (bigger[trak - 2] << 8) | bigger[trak - 1]);
        trak_size += sizeof(EDTS);
        bigger[trak - 4] = (uint8_t)(trak_size >> 24);
        bigger[trak - 3] = (uint8_t)(trak_size >> 16);
        bigger[trak - 2] = (uint8_t)(trak_size >> 8);
        bigger[trak - 1] = (uint8_t)trak_size;
        uint32_t moov_size = (uint32_t)((bigger[moov - 4] << 24) |
                                        (bigger[moov - 3] << 16) |
                                        (bigger[moov - 2] << 8) | bigger[moov - 1]);
        moov_size += sizeof(EDTS);
        bigger[moov - 4] = (uint8_t)(moov_size >> 24);
        bigger[moov - 3] = (uint8_t)(moov_size >> 16);
        bigger[moov - 2] = (uint8_t)(moov_size >> 8);
        bigger[moov - 1] = (uint8_t)moov_size;
        mem_file h;
        h.data = bigger;
        h.len = g.len + sizeof(EDTS);
        topos_io src;
        mem_io_src(&h, &src);
        topos_movie* mv = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_ERR_MALFORMED);
        free(bigger);
        mem_free(&g);
    }
}

/* ---------- M-A2：demux 音频轨 ---------- */

/* 注入的 PCM 全量重建（与 mux_with_audio 的 fill_pcm 序列一致） */
static uint8_t* build_expected_pcm(size_t* out_len)
{
    const size_t chunk_bytes = (size_t)AUDIO_CHUNK_FRAMES * AUDIO_FRAME_BYTES;
    uint8_t* all = (uint8_t*)malloc(chunk_bytes * TEST_FRAMES);
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
        fill_pcm(all + (size_t)i * chunk_bytes, AUDIO_CHUNK_FRAMES, i);
    }
    *out_len = chunk_bytes * TEST_FRAMES;
    return all;
}

static void test_demux_audio_info_and_read(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg_set_audio(&cfg, TC_AUDIO_CODEC_LPCM, AUDIO_RATE, AUDIO_CHANNELS,
                  TC_AUDIO_LAYOUT_STEREO, AUDIO_BITS);
    mem_file f;
    mux_with_audio(&f, &cfg, 1);
    topos_io src;
    mem_io_src(&f, &src);
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);

    /* audio_info 字段 */
    topos_audio_track_info ai;
    memset(&ai, 0, sizeof(ai));
    ai.struct_size = (uint32_t)sizeof(ai);
    ai.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_audio_info(mv, &ai), TC_OK);
    MT_CHECK_EQ_U64(ai.codec, TC_AUDIO_CODEC_LPCM);
    MT_CHECK_EQ_U64(ai.sample_rate, AUDIO_RATE);
    MT_CHECK_EQ_U64(ai.channel_count, AUDIO_CHANNELS);
    MT_CHECK_EQ_U64(ai.channel_layout, TC_AUDIO_LAYOUT_STEREO);
    MT_CHECK_EQ_U64(ai.bits_per_sample, AUDIO_BITS);
    MT_CHECK_EQ_U64(ai.sample_count, (uint64_t)AUDIO_CHUNK_FRAMES * TEST_FRAMES);
    MT_CHECK_EQ_U64(ai.chunk_count, TEST_FRAMES);

    /* 全量回读逐字节相等（先探测后读取） */
    size_t expect_len = 0u;
    uint8_t* expected = build_expected_pcm(&expect_len);
    size_t need = 0u;
    uint8_t* buf = (uint8_t*)malloc(expect_len + 16u);
    MT_CHECK_EQ_I64(tc_movie_read_audio(mv, 0u, TEST_FRAMES, NULL, 0u, &need),
                    TC_ERR_BUFFER_TOO_SMALL);
    MT_CHECK_EQ_U64(need, expect_len);
    MT_CHECK_EQ_I64(tc_movie_read_audio(mv, 0u, TEST_FRAMES, buf, expect_len - 1u, &need),
                    TC_ERR_BUFFER_TOO_SMALL);
    MT_CHECK_EQ_I64(tc_movie_read_audio(mv, 0u, TEST_FRAMES, buf, expect_len, NULL), TC_OK);
    MT_CHECK(memcmp(buf, expected, expect_len) == 0);

    /* 局部区间回读（含末 chunk） */
    MT_CHECK_EQ_I64(tc_movie_read_audio(mv, TEST_FRAMES - 1u, 1u, buf, expect_len, NULL), TC_OK);
    MT_CHECK(memcmp(buf, expected + (size_t)(TEST_FRAMES - 1u) * ((size_t)AUDIO_CHUNK_FRAMES * AUDIO_FRAME_BYTES),
                    (size_t)AUDIO_CHUNK_FRAMES * AUDIO_FRAME_BYTES) == 0);
    /* 越界区间 */
    MT_CHECK_EQ_I64(tc_movie_read_audio(mv, TEST_FRAMES, 1u, buf, expect_len, NULL),
                    TC_ERR_INVALID_ARGUMENT);

    free(buf);
    free(expected);
    tc_movie_close(mv);
    mem_free(&f);
}

static void test_demux_no_audio_file(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file f;
    mux_with_audio(&f, &cfg, 0);  /* cfg 无音频声明且不注入 → 纯视频文件 */
    topos_io src;
    mem_io_src(&f, &src);
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
    topos_audio_track_info ai;
    memset(&ai, 0, sizeof(ai));
    ai.struct_size = (uint32_t)sizeof(ai);
    ai.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_audio_info(mv, &ai), TC_OK);  /* 不报错 */
    MT_CHECK_EQ_U64(ai.codec, TC_AUDIO_CODEC_NONE);
    MT_CHECK_EQ_U64(ai.sample_count, 0u);
    size_t need = 0u;
    uint8_t buf[64];
    MT_CHECK_EQ_I64(tc_movie_read_audio(mv, 0u, 1u, buf, sizeof(buf), &need),
                    TC_ERR_STATE);
    tc_movie_close(mv);
    mem_free(&f);
}

static void test_faststart_with_audio(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg_set_audio(&cfg, TC_AUDIO_CODEC_LPCM, AUDIO_RATE, AUDIO_CHANNELS,
                  TC_AUDIO_LAYOUT_STEREO, AUDIO_BITS);
    mem_file src_f;
    mux_with_audio(&src_f, &cfg, 1);
    /* faststart 布局转换（moov 前置） */
    mem_file dst_f;
    memset(&dst_f, 0, sizeof(dst_f));
    topos_io sio;
    mem_io_src(&src_f, &sio);
    topos_io dio;
    mem_io_sink(&dst_f, &dio);
    MT_CHECK_EQ_I64(tc_movie_faststart(&sio, &dio), TC_OK);
    /* 转换后文件：moov 在前 */
    topos_io rio;
    mem_io_src(&dst_f, &rio);
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&rio, &mv), TC_OK);
    topos_movie_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_info(mv, &info), TC_OK);
    MT_CHECK_EQ_U64(info.faststart, 1u);
    MT_CHECK_EQ_U64(info.sample_count, TEST_FRAMES);
    /* 音频回读逐字节不变 */
    size_t expect_len = 0u;
    uint8_t* expected = build_expected_pcm(&expect_len);
    size_t need = 0u;
    uint8_t* buf = (uint8_t*)malloc(expect_len);
    MT_CHECK_EQ_I64(tc_movie_read_audio(mv, 0u, TEST_FRAMES, buf, expect_len, &need), TC_OK);
    MT_CHECK(memcmp(buf, expected, expect_len) == 0);
    free(buf);
    free(expected);
    tc_movie_close(mv);
    mem_free(&src_f);
    mem_free(&dst_f);
}

int main(int argc, char** argv)
{
    if (argc >= 3 && strcmp(argv[1], "--dump") == 0) {
        build_test_packets();
        test_ffprobe_dump(argv[2]);
        free_test_packets();
        return MT_MAIN_RETURN();
    }
    if (argc >= 3 && strcmp(argv[1], "--dump-aac") == 0) {
        build_test_packets();
        test_aac_dump(argv[2]);
        free_test_packets();
        return MT_MAIN_RETURN();
    }
    if (argc >= 2 && strcmp(argv[1], "--bench") == 0) {
        build_test_packets();
        build_bench_packets();
        test_bench();
        free_test_packets();
        free_bench_packets();
        return MT_MAIN_RETURN();
    }
    build_test_packets();
    test_mux_lpcm_roundtrip_video_unchanged();
    test_audio_validation_errors();
    test_aac_roundtrip();
    test_aac_validation();
    test_demux_no_audio_file();
    test_demux_audio_info_and_read();
    test_faststart_with_audio();
    test_priming_roundtrip();
    test_priming_validation();
    test_priming_malformed();
    test_rate_family_roundtrip();
    test_float32_roundtrip_and_validation();
    test_multitrack_roundtrip();
    test_timecode_roundtrip_matrix();
    test_timecode_validation();
    test_tmcd_malformed_tables();
    free_test_packets();
    if (mt_failures == 0) { fprintf(stderr, "mov_audio: all passed\n"); }
    return MT_MAIN_RETURN();
}
