/* golden_v73 —— V7-R3 zero-motion IP-2 conformance（ADR-C048 载体更换批 2/3）。
 *
 * 与 golden_v9 同构（I,P,P,I gop 1,1,1,2；零漂锚 d2==d1；重编码逐字节
 * 比对），载体换 cfg em=11 → (major 7, entropy 8)——V7 band 并行机器。
 *
 * 文件布局：magic 'G''V''7''3' + u32 BE 帧数 + 每帧 [u32 BE 包长 + 包]。
 * MOV（批 3）：4 包 mux 成标准布局 .mov（stss 只列 I）；check 含 prev_sync
 * 与 FastStart。
 *
 * 用法：golden_v73 gen <es-file> <mov-file> | check <es-file> <mov-file> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "topos_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "topos_codec.h"


/* 便携 bswap32：MSVC 无 __builtin_bswap32（golden 文件格式 u32 BE） */
#if defined(__GNUC__) || defined(__clang__)
#define GV_SWAP32(v) __builtin_bswap32(v)
#else
static uint32_t gv_swap32(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}
#define GV_SWAP32(v) gv_swap32(v)
#endif

#define W 96u
#define H 80u
#define CW ((W + 7u) / 8u * 8u)
#define CH ((H + 7u) / 8u * 8u)
#define CWC ((W / 2u + 7u) / 8u * 8u)
#define NFRAMES 4u
#define PKT_CAP (1u << 18u)

static uint64_t rng_state;

static uint64_t rng(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1Dull;
}

typedef struct {
    uint16_t y[CW * CH];
    uint16_t u[CWC * CH];
    uint16_t v[CWC * CH];
} gframe;

static void build_base_frames(gframe* f)
{
    rng_state = 0x9E3779B97F4A7C15ull;
    for (uint32_t yb = 0; yb < CH / 8u; ++yb) {
        for (uint32_t xb = 0; xb < CW / 8u; ++xb) {
            const uint16_t base = (uint16_t)((xb * 37u + yb * 61u) % 512u + 256u);
            for (uint32_t yy = 0; yy < 8u; ++yy) {
                for (uint32_t xx = 0; xx < 8u; ++xx) {
                    f[0].y[(yb * 8u + yy) * CW + xb * 8u + xx] =
                        (uint16_t)(base + rng() % 17u);
                }
            }
        }
    }
    for (size_t i = 0; i < CWC * CH; ++i) { f[0].u[i] = (uint16_t)(512u + rng() % 9u); }
    for (size_t i = 0; i < CWC * CH; ++i) { f[0].v[i] = (uint16_t)(512u + rng() % 9u); }

    f[1] = f[0];
    for (size_t i = 0; i < CW * CH; i += 5u) {
        int32_t v = (int32_t)f[1].y[i] + (int32_t)(rng() % 11u) - 5;
        if (v < 0) { v = 0; }
        if (v > 1023) { v = 1023; }
        f[1].y[i] = (uint16_t)v;
    }

    rng_state = 0xCAFEBABEDEADBEEFull;
    for (size_t i = 0; i < CW * CH; ++i) { f[3].y[i] = (uint16_t)(rng() % 1024u); }
    for (size_t i = 0; i < CWC * CH; ++i) { f[3].u[i] = (uint16_t)(rng() % 1024u); }
    for (size_t i = 0; i < CWC * CH; ++i) { f[3].v[i] = (uint16_t)(rng() % 1024u); }
    /* F2 由序列过程动态产出（= F1 解码输出 = 编码器参考） */
}

static void make_input(const gframe* f, topos_frame_input* in)
{
    memset(in, 0, sizeof(*in));
    in->struct_size = (uint32_t)sizeof(*in);
    in->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    in->planes[0] = f->y;
    in->planes[1] = f->u;
    in->planes[2] = f->v;
    in->strides[0] = CW;
    in->strides[1] = CWC;
    in->strides[2] = CWC;
}

static topos_frame_config gcfg(void)
{
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = (uint16_t)W;
    cfg.visible_height = (uint16_t)H;
    cfg.profile = 3u;
    cfg.pixel_format = 0u;
    cfg.bit_depth = 10u;
    cfg.qmatrix_id = 0u;
    cfg.qp_base = 44u;
    cfg.slice_rows = 16u;
    cfg.color_range = 1u;
    cfg.color_primaries = 1u;
    cfg.color_transfer = 1u;
    cfg.color_matrix = 1u;
    cfg.reserved[0] = 11u; /* V7-R3（ADR-C048） */
    return cfg;
}

/* 共享序列过程（解码链用独立 dctx 建模真实解码器，V9 复审同纪律）。 */
static int32_t run_sequence(uint8_t pkts[NFRAMES][PKT_CAP], size_t szs[NFRAMES],
                            gframe* frames_out)
{
    gframe frames[NFRAMES];
    build_base_frames(frames);
    topos_frame_config cfg = gcfg();
    topos_gop_context* ctx = NULL;
    topos_gop_context* dctx = NULL;
    int32_t rc = tc_gop_context_create(&cfg, &ctx);
    if (rc != TC_OK) { return rc; }
    rc = tc_gop_context_create(&cfg, &dctx);
    if (rc != TC_OK) { tc_gop_context_close(ctx); return rc; }
    static const int want_intra[NFRAMES] = { 1, 0, 0, 0 };
    for (uint32_t k = 0u; k < NFRAMES; ++k) {
        topos_frame_input in;
        make_input(&frames[k], &in);
        topos_frame_stats st;
        memset(&st, 0, sizeof(st));
        st.struct_size = (uint32_t)sizeof(st);
        st.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        rc = tc_gop_context_encode_frame(ctx, &in, pkts[k], PKT_CAP, &szs[k],
                                         want_intra[k], &st);
        if (rc != TC_OK) { goto out; }
        topos_plane_view out[TC_FRAME_MAX_PLANES];
        memset(out, 0, sizeof(out));
        for (int p = 0; p < TC_FRAME_MAX_PLANES; ++p) {
            out[p].struct_size = (uint32_t)sizeof(out[p]);
            out[p].abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        }
        out[0].pixels = frames_out[k].y; out[0].stride = CW;
        out[1].pixels = frames_out[k].u; out[1].stride = CWC;
        out[2].pixels = frames_out[k].v; out[2].stride = CWC;
        topos_gop_frame_info gi;
        memset(&gi, 0, sizeof(gi));
        rc = tc_gop_context_feed(dctx, pkts[k], szs[k], out, &gi);
        if (rc != TC_OK) { goto out; }
        if (k == 1) {
            frames[2] = frames_out[1]; /* F2 = F1 解码输出（= 编码器参考） */
        }
    }
out:
    tc_gop_context_close(dctx);
    tc_gop_context_close(ctx);
    return rc;
}

static int gen_mode(const char* path, const uint8_t pkts[NFRAMES][PKT_CAP],
                    const size_t szs[NFRAMES])
{
    FILE* f = fopen(path, "wb");
    if (f == NULL) { perror("gen"); return 1; }
    static const uint8_t magic[4] = { 'G', 'V', '7', '3' };
    fwrite(magic, 1, 4u, f);
    const uint32_t be_n = GV_SWAP32(NFRAMES);
    fwrite(&be_n, 4u, 1u, f);
    for (uint32_t k = 0; k < NFRAMES; ++k) {
        const uint32_t be_sz = GV_SWAP32((uint32_t)szs[k]);
        fwrite(&be_sz, 4u, 1u, f);
        fwrite(pkts[k], 1u, szs[k], f);
        fprintf(stderr, "gen: frame %u -> %zu bytes (major=%u em=%u frame_type=%u gop=%u)\n",
                (unsigned)k, szs[k], (unsigned)pkts[k][6], (unsigned)pkts[k][45],
                (unsigned)pkts[k][15],
                (unsigned)((pkts[k][16] << 8) | pkts[k][17]));
    }
    fclose(f);
    return 0;
}

static int gen_mode_mov(const char* path, const uint8_t pkts[NFRAMES][PKT_CAP],
                        const size_t szs[NFRAMES]);
static int check_mode_mov(const char* path, const uint8_t pkts[NFRAMES][PKT_CAP],
                          const size_t szs[NFRAMES]);

static int check_mode(const char* es_path, const char* mov_path)
{
    FILE* f = fopen(es_path, "rb");
    if (f == NULL) { perror("check"); return 1; }
    uint8_t magic[4];
    uint32_t be_n = 0u;
    if (fread(magic, 1, 4u, f) != 4u || memcmp(magic, "GV73", 4u) != 0 ||
        fread(&be_n, 4u, 1u, f) != 1u || GV_SWAP32(be_n) != NFRAMES) {
        fprintf(stderr, "check: bad header\n");
        fclose(f);
        return 1;
    }
    static uint8_t stored[NFRAMES][PKT_CAP];
    static size_t stored_sz[NFRAMES];
    for (uint32_t k = 0; k < NFRAMES; ++k) {
        uint32_t be_sz = 0u;
        if (fread(&be_sz, 4u, 1u, f) != 1u) { fclose(f); return 1; }
        stored_sz[k] = GV_SWAP32(be_sz);
        if (stored_sz[k] > PKT_CAP ||
            fread(stored[k], 1u, stored_sz[k], f) != stored_sz[k]) {
            fprintf(stderr, "check: truncated frame %u\n", (unsigned)k);
            fclose(f);
            return 1;
        }
    }
    fclose(f);

    /* ①② 重编码逐字节比对 + 帧型/gop 序列 + 载体字节 */
    static uint8_t pkts[NFRAMES][PKT_CAP];
    static size_t szs[NFRAMES];
    static gframe dec[NFRAMES];
    if (run_sequence(pkts, szs, dec) != TC_OK) {
        fprintf(stderr, "check: re-encode: %s\n", tc_last_error());
        return 1;
    }
    static const uint8_t want_ft[NFRAMES] = { 0u, 1u, 1u, 0u };
    static const uint16_t want_gop[NFRAMES] = { 1u, 1u, 1u, 2u };
    for (uint32_t k = 0; k < NFRAMES; ++k) {
        if (szs[k] != stored_sz[k] || memcmp(pkts[k], stored[k], szs[k]) != 0) {
            fprintf(stderr, "check: frame %u re-encode mismatch (%zu vs %zu)\n",
                    (unsigned)k, szs[k], stored_sz[k]);
            return 1;
        }
        if (pkts[k][6] != 7u || pkts[k][45] != 8u) {
            fprintf(stderr, "check: frame %u carrier bytes wrong (major=%u em=%u)\n",
                    (unsigned)k, (unsigned)pkts[k][6], (unsigned)pkts[k][45]);
            return 1;
        }
        if (pkts[k][15] != want_ft[k] ||
            (uint16_t)((pkts[k][16] << 8) | pkts[k][17]) != want_gop[k]) {
            fprintf(stderr, "check: frame %u ft=%u gop=%u (want %u,%u)\n",
                    (unsigned)k, (unsigned)pkts[k][15],
                    (unsigned)((pkts[k][16] << 8) | pkts[k][17]),
                    (unsigned)want_ft[k], (unsigned)want_gop[k]);
            return 1;
        }
    }
    /* ③ 零漂锚：F2（参考的精确重复）解码输出 == F1 */
    if (memcmp(&dec[2], &dec[1], sizeof(gframe)) != 0) {
        fprintf(stderr, "check: zero-drift anchor violated (d2 != d1)\n");
        return 1;
    }
    printf("golden_v73 check ok: %u frames (I,P,P,I gop 1,1,1,2), "
           "re-encode byte-exact, zero-drift anchor verified, carrier (7,8)\n",
           (unsigned)NFRAMES);
    return check_mode_mov(mov_path, pkts, szs);
}

/* —— MOV 段（批 3）：内存 io —— */
typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
} g9_mem;

static int32_t g9_read(void* ctx, uint64_t off, void* buf, size_t len)
{
    g9_mem* f = (g9_mem*)ctx;
    if ((uint64_t)off + len > f->len) { return TC_ERR_IO; }
    memcpy(buf, f->data + off, len);
    return TC_OK;
}

static int32_t g9_write(void* ctx, const void* buf, size_t len)
{
    g9_mem* f = (g9_mem*)ctx;
    if (f->len + len > f->cap) {
        size_t cap = f->cap ? f->cap * 2u : (1u << 20);
        while (cap < f->len + len) { cap *= 2u; }
        uint8_t* nd = (uint8_t*)realloc(f->data, cap);
        if (nd == NULL) { return TC_ERR_IO; }
        f->data = nd;
        f->cap = cap;
    }
    memcpy(f->data + f->len, buf, len);
    f->len += len;
    return TC_OK;
}

static int32_t g9_seek_write(void* ctx, uint64_t off, const void* buf, size_t len)
{
    g9_mem* f = (g9_mem*)ctx;
    if (off + len > f->len) { return TC_ERR_IO; }
    memcpy(f->data + off, buf, len);
    return TC_OK;
}

static void g9_free(g9_mem* f)
{
    free(f->data);
    f->data = NULL;
    f->len = f->cap = 0u;
}

static int32_t g9_mux_mov(const uint8_t pkts[NFRAMES][PKT_CAP],
                          const size_t szs[NFRAMES], g9_mem* out)
{
    topos_movie_config mc;
    memset(&mc, 0, sizeof(mc));
    mc.struct_size = (uint32_t)sizeof(mc);
    mc.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    mc.visible_width = (uint16_t)W;
    mc.visible_height = (uint16_t)H;
    mc.profile = 3u;
    mc.pixel_format = 0u;
    mc.bit_depth = 10u;
    mc.qmatrix_id = 0u;
    mc.qp_base = 44u;
    mc.color_range = 1u;
    mc.color_primaries = 1u;
    mc.color_transfer = 1u;
    mc.color_matrix = 1u;
    mc.timescale = 25u;
    topos_io sink;
    memset(&sink, 0, sizeof(sink));
    sink.struct_size = (uint32_t)sizeof(sink);
    sink.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    sink.ctx = out;
    sink.write = g9_write;
    sink.seek_write = g9_seek_write;
    topos_mux* mux = NULL;
    int32_t rc = tc_mux_create(&mc, &sink, &mux);
    if (rc != TC_OK) { return rc; }
    for (uint32_t k = 0; k < NFRAMES; ++k) {
        rc = tc_mux_add_packet(mux, pkts[k], szs[k], (uint64_t)k, 1u);
        if (rc != TC_OK) { tc_mux_free(mux); return rc; }
    }
    rc = tc_mux_finish(mux);
    tc_mux_free(mux);
    return rc;
}

static int gen_mode_mov(const char* path, const uint8_t pkts[NFRAMES][PKT_CAP],
                        const size_t szs[NFRAMES])
{
    g9_mem mem;
    memset(&mem, 0, sizeof(mem));
    if (g9_mux_mov(pkts, szs, &mem) != TC_OK) {
        fprintf(stderr, "gen mov: %s\n", tc_last_error());
        return 1;
    }
    FILE* f = fopen(path, "wb");
    if (f == NULL) { perror("gen mov"); return 1; }
    static const uint8_t magic[4] = { 'G', 'M', '9', '1' };
    fwrite(magic, 1, 4u, f);
    const uint32_t be_len = GV_SWAP32((uint32_t)mem.len);
    fwrite(&be_len, 4u, 1u, f);
    fwrite(mem.data, 1u, mem.len, f);
    fclose(f);
    g9_free(&mem);
    return 0;
}

static int check_mode_mov(const char* path, const uint8_t pkts[NFRAMES][PKT_CAP],
                          const size_t szs[NFRAMES])
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) { perror("check mov"); return 1; }
    uint8_t magic[4];
    uint32_t be_len = 0u;
    if (fread(magic, 1, 4u, f) != 4u || memcmp(magic, "GM91", 4u) != 0 ||
        fread(&be_len, 4u, 1u, f) != 1u) {
        fprintf(stderr, "check mov: bad header\n");
        fclose(f);
        return 1;
    }
    const uint32_t stored_len = GV_SWAP32(be_len);
    g9_mem stored;
    memset(&stored, 0, sizeof(stored));
    stored.cap = stored.len = stored_len;
    stored.data = (uint8_t*)malloc(stored_len);
    if (stored.data == NULL || fread(stored.data, 1u, stored_len, f) != stored_len) {
        fprintf(stderr, "check mov: truncated\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    /* ① 重 mux 逐字节比对（容器确定性） */
    g9_mem rebuilt;
    memset(&rebuilt, 0, sizeof(rebuilt));
    if (g9_mux_mov(pkts, szs, &rebuilt) != TC_OK) {
        fprintf(stderr, "check mov: re-mux: %s\n", tc_last_error());
        return 1;
    }
    if (rebuilt.len != stored.len || memcmp(rebuilt.data, stored.data, stored.len) != 0) {
        fprintf(stderr, "check mov: container mismatch (%zu vs %u)\n",
                rebuilt.len, stored_len);
        return 1;
    }
    g9_free(&rebuilt);

    /* ②③④⑤ 打开校验：包/sync/prev_sync/faststart/I 起解 */
    topos_io io;
    memset(&io, 0, sizeof(io));
    io.struct_size = (uint32_t)sizeof(io);
    io.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    io.ctx = &stored;
    io.read = g9_read;
    io.length = (uint64_t)stored.len;
    topos_movie* mv = NULL;
    if (tc_movie_open(&io, &mv) != TC_OK) {
        fprintf(stderr, "check mov: open: %s\n", tc_last_error());
        return 1;
    }
    topos_movie_info mi;
    memset(&mi, 0, sizeof(mi));
    mi.struct_size = (uint32_t)sizeof(mi);
    mi.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    if (tc_movie_info(mv, &mi) != TC_OK || mi.sample_count != NFRAMES) {
        fprintf(stderr, "check mov: info/sample_count\n");
        return 1;
    }
    for (uint32_t k = 0; k < NFRAMES; ++k) {
        uint8_t buf[PKT_CAP];
        size_t need = 0u;
        if (tc_movie_packet(mv, k, buf, sizeof(buf), &need) != TC_OK || need != szs[k] ||
            memcmp(buf, pkts[k], need) != 0) {
            fprintf(stderr, "check mov: sample %u mismatch\n", (unsigned)k);
            return 1;
        }
        uint8_t sync = 0u;
        uint32_t prev = 0u;
        static const uint8_t want_sync[NFRAMES] = { 1u, 0u, 0u, 1u };
        static const uint32_t want_prev[NFRAMES] = { 0u, 0u, 0u, 3u };
        if (tc_movie_packet_sync(mv, k, &sync) != TC_OK || sync != want_sync[k] ||
            tc_movie_prev_sync(mv, k, &prev) != TC_OK || prev != want_prev[k]) {
            fprintf(stderr, "check mov: sample %u sync=%u prev=%u\n",
                    (unsigned)k, (unsigned)sync, (unsigned)prev);
            return 1;
        }
    }
    /* ⑤ 从每个 I 起新 context 解码成功；P 直启 MALFORMED */
    {
        topos_frame_config cfg = gcfg();
        static const uint32_t i_idx[NFRAMES] = { 0u, 3u };
        for (int t = 0; t < 2; ++t) {
            uint8_t buf[PKT_CAP];
            size_t need = 0u;
            tc_movie_packet(mv, i_idx[t], buf, sizeof(buf), &need);
            topos_gop_context* ctx = NULL;
            if (tc_gop_context_create(&cfg, &ctx) != TC_OK) { return 1; }
            topos_gop_frame_info gi;
            memset(&gi, 0, sizeof(gi));
            if (tc_gop_context_feed(ctx, buf, need, NULL, &gi) != TC_OK) {
                fprintf(stderr, "check mov: decode from I %u failed\n", (unsigned)i_idx[t]);
                return 1;
            }
            tc_gop_context_close(ctx);
        }
        uint8_t pbuf[PKT_CAP];
        size_t pneed = 0u;
        tc_movie_packet(mv, 1u, pbuf, sizeof(pbuf), &pneed);
        topos_gop_context* pctx = NULL;
        if (tc_gop_context_create(&cfg, &pctx) != TC_OK) { return 1; }
        topos_gop_frame_info gi;
        memset(&gi, 0, sizeof(gi));
        if (tc_gop_context_feed(pctx, pbuf, pneed, NULL, &gi) != TC_ERR_MALFORMED) {
            fprintf(stderr, "check mov: P-start must be MALFORMED\n");
            return 1;
        }
        tc_gop_context_close(pctx);
    }
    tc_movie_close(mv);

    /* ④ FastStart：moov 前置 + packets/sync 不变 */
    {
        g9_mem dst;
        memset(&dst, 0, sizeof(dst));
        topos_io src_io = io;
        topos_io dst_io;
        memset(&dst_io, 0, sizeof(dst_io));
        dst_io.struct_size = (uint32_t)sizeof(dst_io);
        dst_io.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        dst_io.ctx = &dst;
        dst_io.write = g9_write;
        dst_io.seek_write = g9_seek_write;
        src_io.length = (uint64_t)stored.len;
        if (tc_movie_faststart(&src_io, &dst_io) != TC_OK) {
            fprintf(stderr, "check mov: faststart: %s\n", tc_last_error());
            return 1;
        }
        /* 顶层 atom 走查：要求 ftyp → moov → mdat 顺序（FastStart 布局） */
        uint64_t aoff = 0u;
        int seen_moov = 0, ok_layout = 0;
        while (aoff + 8u <= (uint64_t)dst.len) {
            uint32_t asize = ((uint32_t)dst.data[aoff] << 24) |
                             ((uint32_t)dst.data[aoff + 1u] << 16) |
                             ((uint32_t)dst.data[aoff + 2u] << 8) |
                             (uint32_t)dst.data[aoff + 3u];
            const uint8_t* atype = dst.data + aoff + 4u;
            if (asize == 1u) {
                /* 64 位 size（低 32 位足够本用例） */
                asize = (uint32_t)((uint64_t)dst.data[aoff + 12u] << 24 |
                                   (uint64_t)dst.data[aoff + 13u] << 16 |
                                   (uint64_t)dst.data[aoff + 14u] << 8 |
                                   (uint64_t)dst.data[aoff + 15u]);
            }
            if (asize < 8u) { break; }
            if (memcmp(atype, "mdat", 4u) == 0) { ok_layout = seen_moov; break; }
            if (memcmp(atype, "moov", 4u) == 0) { seen_moov = 1; }
            aoff += asize;
        }
        if (!ok_layout) {
            fprintf(stderr, "check mov: faststart moov not front\n");
            return 1;
        }
        dst_io.length = (uint64_t)dst.len;
        dst_io.read = g9_read;
        topos_movie* mv2 = NULL;
        if (tc_movie_open(&dst_io, &mv2) != TC_OK) {
            fprintf(stderr, "check mov: faststart open: %s\n", tc_last_error());
            return 1;
        }
        for (uint32_t k = 0; k < NFRAMES; ++k) {
            uint8_t buf[PKT_CAP];
            size_t need = 0u;
            uint8_t sync = 0u, want_sync = (k == 0u || k == 3u) ? 1u : 0u;
            uint32_t prev = 0u, want_prev = (k == 3u) ? 3u : 0u;
            if (tc_movie_packet(mv2, k, buf, sizeof(buf), &need) != TC_OK ||
                need != szs[k] || memcmp(buf, pkts[k], need) != 0 ||
                tc_movie_packet_sync(mv2, k, &sync) != TC_OK || sync != want_sync ||
                tc_movie_prev_sync(mv2, k, &prev) != TC_OK || prev != want_prev) {
                fprintf(stderr, "check mov: faststart sample %u mismatch\n", (unsigned)k);
                return 1;
            }
        }
        tc_movie_close(mv2);
        g9_free(&dst);
    }

    g9_free(&stored);
    printf("golden_v73 mov check ok: stss 只列 I (1,0,0,1), prev_sync (0,0,0,3), "
           "faststart moov 前置 + 逐字节容器确定性\n");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 4 && strcmp(argv[1], "gen") == 0) {
        static uint8_t pkts[NFRAMES][PKT_CAP];
        static size_t szs[NFRAMES];
        static gframe dec[NFRAMES];
        if (run_sequence(pkts, szs, dec) != TC_OK) {
            fprintf(stderr, "gen: %s\n", tc_last_error());
            return 1;
        }
        if (gen_mode(argv[2], pkts, szs) != 0) { return 1; }
        return gen_mode_mov(argv[3], pkts, szs);
    }
    if (argc == 4 && strcmp(argv[1], "check") == 0) {
        return check_mode(argv[2], argv[3]);
    }
    fprintf(stderr, "usage: golden_v73 gen|check <es-file> <mov-file>\n");
    return 1;
}
