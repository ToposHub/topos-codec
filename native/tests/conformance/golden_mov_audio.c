/* golden_mov_audio —— M-B6：v1.4 音频轨字节级 conformance 冻结。
 *
 * 记录集（每用例 2 条：偶数 = MOV 文件字节，奇数 = 索引+音频元数据摘要）：
 *   摘要 = 视频 n/index_bytes/faststart + 每 sample {size u32, pts u64,
 *   dur u32, sync u8}（offset 冻结由文件字节承担）
 *        + audio {codec,rate,channels,bits,sample_format,priming,
 *                 chunk_count,sample_count} 各 u32be
 *
 * 用例（确定性；视频包 image_synth 合成，音频 chunk 确定性字节填充）：
 *   0/1  lpcm 16bit 2.0 48k        标准布局（V0 声样 + TN2120 + chan）
 *   2/3  lpcm 32bit f32 2.0 48k    （V2 声样：float 恒走 V2）
 *   4/5  lpcm 24bit 2.0 88200      （V2 声样：rate > 65535）
 *   6/7  mp4a 2.0 48k priming=1024 （elst + wave/frma/esds/chan 冻结）
 *   8/9  mp4a 5.1 48k priming=1024 faststart（重定位后字节冻结）
 *
 * 文件格式：与 golden_mov 一致（magic "TPMA" | version 1 | count | fold）。
 * 任何无声漂移（esds/wave/elst/V2/字段顺序/表布局）在此失败。
 *
 * 用法：golden_mov_audio gen <file> | check <file>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../support/image_synth.h"
#include "topos_codec.h"

#define GOLDEN_MAGIC "TPMA"
#define GOLDEN_VERSION 1u

static uint64_t g_fold;

static uint64_t mix64(uint64_t h, uint64_t v)
{
    h += 0x9E3779B97F4A7C15ull;
    uint64_t z = v;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return h ^ (z ^ (z >> 31));
}

/* —— 内存 io（同 golden_mov）—— */
typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
} mem_file;

static int32_t mf_read(void* ctx, uint64_t off, void* buf, size_t len)
{
    mem_file* f = (mem_file*)ctx;
    if (off > (uint64_t)f->len || len > (uint64_t)f->len - off) { return TC_ERR_IO; }
    if (len != 0u) { memcpy(buf, f->data + off, len); }
    return TC_OK;
}

static int32_t mf_write(void* ctx, const void* d, size_t len)
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
    if (len != 0u) { memcpy(f->data + f->len, d, len); }
    f->len += len;
    return TC_OK;
}

static int32_t mf_seek_write(void* ctx, uint64_t off, const void* d, size_t len)
{
    mem_file* f = (mem_file*)ctx;
    if (off > (uint64_t)f->len || len > (uint64_t)f->len - off) { return TC_ERR_IO; }
    if (len != 0u) { memcpy(f->data + off, d, len); }
    return TC_OK;
}

static void store_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void store_be64(uint8_t* p, uint64_t v)
{
    store_be32(p, (uint32_t)(v >> 32));
    store_be32(p + 4, (uint32_t)v);
}

/* —— 音频用例定义 —— */
typedef struct {
    uint32_t codec;          /* TC_AUDIO_CODEC_* */
    uint32_t rate;
    uint32_t channels;
    uint32_t layout;
    uint32_t bits;           /* lpcm；mp4a = 0 */
    uint32_t sample_format;  /* TC_AUDIO_FMT_* */
    uint32_t priming;        /* mp4a elst；lpcm = 0 */
    uint32_t chunk_frames;   /* 每音频 chunk 采样帧数（mp4a 恒 1024） */
    int faststart;
} audio_case;

static const audio_case ACASES[] = {
    { TC_AUDIO_CODEC_LPCM, 48000u, 2u, 0u, 16u, 0u, 0u, 1920u, 0 },
    { TC_AUDIO_CODEC_LPCM, 48000u, 2u, 0u, 32u, 1u, 0u, 1920u, 0 },
    { TC_AUDIO_CODEC_LPCM, 88200u, 2u, 0u, 24u, 0u, 0u, 3528u, 0 },
    { TC_AUDIO_CODEC_MP4A, 48000u, 2u, 0u, 0u, 0u, 1024u, 1024u, 0 },
    { TC_AUDIO_CODEC_MP4A, 48000u, 6u, 1u, 0u, 0u, 1024u, 1024u, 1 },
};

#define ACASE_COUNT ((uint32_t)(sizeof(ACASES) / sizeof(ACASES[0])))
#define RECORD_COUNT (ACASE_COUNT * 2u)
#define A_FRAMES 8u /* 音频 chunk 数（=视频帧数） */

/* mux 单用例 → 文件字节 */
static int32_t build_movie(const audio_case* c, mem_file* out)
{
    topos_movie_config mc;
    memset(&mc, 0, sizeof(mc));
    mc.struct_size = (uint32_t)sizeof(mc);
    mc.abi_version = TOPOS_CODEC_ABI_VERSION;
    mc.visible_width = 64u;
    mc.visible_height = 40u;
    mc.profile = 3u;
    mc.bit_depth = 10u;
    mc.qmatrix_id = 1u;
    mc.qp_base = 28u;
    mc.color_range = 1u;
    mc.color_primaries = 1u;
    mc.color_transfer = 1u;
    mc.color_matrix = 1u;
    mc.timescale = 24000u;
    mc.reserved[TC_AUDIO_SLOT_CODEC] = c->codec;
    mc.reserved[TC_AUDIO_SLOT_RATE] = c->rate;
    mc.reserved[TC_AUDIO_SLOT_CHANNELS] = c->channels;
    mc.reserved[TC_AUDIO_SLOT_LAYOUT] = c->layout;
    mc.reserved[TC_AUDIO_SLOT_BITS] = c->bits;
    mc.reserved[TC_AUDIO_SLOT_FORMAT] = c->sample_format;

    topos_frame_config fc;
    memset(&fc, 0, sizeof(fc));
    fc.struct_size = (uint32_t)sizeof(fc);
    fc.abi_version = TOPOS_CODEC_ABI_VERSION;
    fc.visible_width = 64u;
    fc.visible_height = 40u;
    fc.profile = 3u;
    fc.bit_depth = 10u;
    fc.qmatrix_id = 1u;
    fc.qp_base = 28u;
    fc.color_range = 1u;
    fc.color_primaries = 1u;
    fc.color_transfer = 1u;
    fc.color_matrix = 1u;

    image_synth_cfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.width = 64u;
    sc.height = 40u;
    sc.kind = TC_SYNTH_GRAIN;
    sc.seed = 0xA1u;

    topos_io sink;
    memset(&sink, 0, sizeof(sink));
    sink.struct_size = (uint32_t)sizeof(sink);
    sink.abi_version = TOPOS_CODEC_ABI_VERSION;
    sink.ctx = out;
    sink.write = mf_write;
    sink.seek_write = mf_seek_write;

    topos_mux* m = NULL;
    int32_t rc = tc_mux_create(&mc, &sink, &m);
    if (rc != TC_OK) { return rc; }

    uint16_t* pl[4] = { NULL, NULL, NULL, NULL };
    image_synth_alloc(&sc, 0, &pl[0], &pl[1], &pl[2], &pl[3]);

    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = TOPOS_CODEC_ABI_VERSION;
    in.planes[0] = pl[0];
    in.planes[1] = pl[1];
    in.planes[2] = pl[2];
    in.planes[3] = pl[3];

    size_t bound = tc_frame_packet_bound(&fc);
    uint8_t* pkt = (uint8_t*)malloc(bound);
    topos_frame_stats st;
    if (pkt == NULL) { rc = TC_ERR_OUT_OF_MEMORY; }

    static const uint8_t ASC[2] = { 0x12, 0x10 };
    if (rc == TC_OK && c->codec == TC_AUDIO_CODEC_MP4A) {
        rc = tc_mux_set_audio_asc(m, ASC, sizeof(ASC));
    }
    const size_t frame_bytes =
        (size_t)c->channels
        * (c->codec == TC_AUDIO_CODEC_LPCM ? c->bits / 8u : 2u);
    uint8_t* chunk = (uint8_t*)malloc((size_t)c->chunk_frames * frame_bytes);
    if (chunk == NULL) { rc = TC_ERR_OUT_OF_MEMORY; }

    for (uint32_t i = 0; i < A_FRAMES && rc == TC_OK; ++i) {
        sc.seed = 0xA1u + (uint64_t)i;
        image_synth_build(&sc, pl[0], pl[1], pl[2], pl[3]);
        fc.qp_base = (uint8_t)(28u + (i % 3u)); /* 逐帧 qp 变化（tpcC 豁免项） */
        rc = tc_frame_encode(&fc, &in, pkt, bound, &st);
        if (rc == TC_OK) {
            rc = tc_mux_add_packet(m, pkt, st.packet_size, (uint64_t)i, 1u);
        }
        if (rc == TC_OK) {
            const size_t bytes = (size_t)c->chunk_frames * frame_bytes;
            for (size_t k = 0; k < bytes; ++k) {
                chunk[k] = (uint8_t)((k * 7u + i * 31u) & 0xFFu);
            }
            rc = tc_mux_add_audio(m, chunk, bytes, c->chunk_frames);
        }
    }
    if (rc == TC_OK && c->priming != 0u) {
        rc = tc_mux_set_audio_priming(m, c->priming);
    }
    if (rc == TC_OK) { rc = tc_mux_finish(m); }
    free(chunk);
    free(pkt);
    free(pl[0]);
    free(pl[1]);
    free(pl[2]);
    free(pl[3]);
    tc_mux_free(m);
    if (rc != TC_OK) { free(out->data); return rc; }

    if (!c->faststart) { return TC_OK; }
    mem_file fs;
    memset(&fs, 0, sizeof(fs));
    topos_io src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.struct_size = (uint32_t)sizeof(src);
    src.abi_version = TOPOS_CODEC_ABI_VERSION;
    src.ctx = out;
    src.read = mf_read;
    src.length = (uint64_t)out->len;
    dst.struct_size = (uint32_t)sizeof(dst);
    dst.abi_version = TOPOS_CODEC_ABI_VERSION;
    dst.ctx = &fs;
    dst.write = mf_write;
    dst.seek_write = mf_seek_write;
    rc = tc_movie_faststart(&src, &dst);
    if (rc == TC_OK) {
        free(out->data);
        *out = fs;
    } else {
        free(fs.data);
    }
    return rc;
}

/* 摘要：视频索引 + 音频元数据（priming/sample_format 冻结） */
static int32_t build_summary(const mem_file* f, mem_file* sum)
{
    topos_io src;
    memset(&src, 0, sizeof(src));
    src.struct_size = (uint32_t)sizeof(src);
    src.abi_version = TOPOS_CODEC_ABI_VERSION;
    src.ctx = (void*)f;
    src.read = mf_read;
    src.length = (uint64_t)f->len;
    topos_movie* mv = NULL;
    int32_t rc = tc_movie_open(&src, &mv);
    if (rc != TC_OK) { return rc; }
    topos_movie_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    rc = tc_movie_info(mv, &info);
    if (rc != TC_OK) { tc_movie_close(mv); return rc; }
    topos_audio_track_info ai;
    memset(&ai, 0, sizeof(ai));
    ai.struct_size = (uint32_t)sizeof(ai);
    ai.abi_version = TOPOS_CODEC_ABI_VERSION;
    rc = tc_movie_audio_info(mv, &ai);
    if (rc != TC_OK) { tc_movie_close(mv); return rc; }

    mem_file s;
    memset(&s, 0, sizeof(s));
    uint8_t b4[4];
    store_be32(b4, info.sample_count);
    rc = mf_write(&s, b4, 4u);
    store_be32(b4, info.index_bytes);
    if (rc == TC_OK) { rc = mf_write(&s, b4, 4u); }
    store_be32(b4, info.faststart);
    if (rc == TC_OK) { rc = mf_write(&s, b4, 4u); }
    for (uint32_t i = 0; i < info.sample_count && rc == TC_OK; ++i) {
        uint8_t rec[17];
        size_t size = 0;
        uint64_t pts = 0;
        uint32_t dur = 0;
        uint8_t sync = 0;
        if (tc_movie_packet(mv, i, NULL, 0, &size) != TC_ERR_BUFFER_TOO_SMALL ||
            tc_movie_packet_pts(mv, i, &pts, &dur) != TC_OK ||
            tc_movie_packet_sync(mv, i, &sync) != TC_OK) {
            rc = TC_ERR_MALFORMED;
            break;
        }
        store_be32(rec, (uint32_t)size);
        store_be64(rec + 4, pts);
        store_be32(rec + 12, dur);
        rec[16] = sync;
        rc = mf_write(&s, rec, sizeof(rec));
    }
    if (rc == TC_OK) {
        uint8_t ab[32];
        store_be32(ab, ai.codec);
        store_be32(ab + 4, ai.sample_rate);
        store_be32(ab + 8, ai.channel_count);
        store_be32(ab + 12, ai.bits_per_sample);
        store_be32(ab + 16, ai.reserved[1]); /* sample_format */
        store_be32(ab + 20, ai.reserved[0]); /* priming */
        store_be32(ab + 24, ai.chunk_count);
        store_be32(ab + 28, ai.sample_count);
        rc = mf_write(&s, ab, sizeof(ab));
    }
    tc_movie_close(mv);
    if (rc == TC_OK) { *sum = s; } else { free(s.data); }
    return rc;
}

static int32_t build_record(uint32_t index, uint8_t** out, size_t* out_size)
{
    mem_file f;
    memset(&f, 0, sizeof(f));
    int32_t rc = build_movie(&ACASES[index / 2u], &f);
    if (rc != TC_OK) {
        fprintf(stderr, "case %u mux failed: %d\n", (unsigned)(index / 2u), (int)rc);
        free(f.data);
        return rc;
    }
    if ((index % 2u) == 0u) {
        *out = f.data;
        *out_size = f.len;
        return TC_OK;
    }
    mem_file sum;
    memset(&sum, 0, sizeof(sum));
    rc = build_summary(&f, &sum);
    free(f.data);
    if (rc != TC_OK) { free(sum.data); return rc; }
    *out = sum.data;
    *out_size = sum.len;
    return TC_OK;
}

static int gen_mode(const char* path)
{
    FILE* fp = fopen(path, "wb");
    if (fp == NULL) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    uint8_t hdr[24];
    memcpy(hdr, GOLDEN_MAGIC, 4u);
    store_be32(hdr + 4, GOLDEN_VERSION);
    store_be32(hdr + 8, RECORD_COUNT);
    store_be64(hdr + 16, 0u);
    fwrite(hdr, 1, sizeof(hdr), fp);
    g_fold = 0x5A5A5A5A5A5A5A5Aull;
    for (uint32_t i = 0; i < RECORD_COUNT; ++i) {
        uint8_t* data = NULL;
        size_t n = 0;
        int32_t rc = build_record(i, &data, &n);
        if (rc != TC_OK) { fclose(fp); return 1; }
        uint8_t lenb[4];
        store_be32(lenb, (uint32_t)n);
        fwrite(lenb, 1, 4u, fp);
        fwrite(data, 1, n, fp);
        g_fold = mix64(g_fold, (uint64_t)n);
        for (size_t k = 0; k < n; ++k) { g_fold = mix64(g_fold, data[k]); }
        free(data);
    }
    fseek(fp, 16, SEEK_SET);
    uint8_t foldb[8];
    store_be64(foldb, g_fold);
    fwrite(foldb, 1, 8u, fp);
    fclose(fp);
    printf("generated %u records, fold %016llx\n", (unsigned)RECORD_COUNT,
           (unsigned long long)g_fold);
    return 0;
}

static int check_mode(const char* path)
{
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    uint8_t hdr[24];
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr) ||
        memcmp(hdr, GOLDEN_MAGIC, 4u) != 0) {
        fprintf(stderr, "bad golden header\n");
        fclose(fp);
        return 1;
    }
    const uint32_t count = ((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) |
                           ((uint32_t)hdr[10] << 8) | (uint32_t)hdr[11];
    /* fold 存于头部 offset 16（大端），记录流之后无尾随数据 */
    const uint64_t want_fold =
        ((uint64_t)hdr[16] << 56) | ((uint64_t)hdr[17] << 48) |
        ((uint64_t)hdr[18] << 40) | ((uint64_t)hdr[19] << 32) |
        ((uint64_t)hdr[20] << 24) | ((uint64_t)hdr[21] << 16) |
        ((uint64_t)hdr[22] << 8) | (uint64_t)hdr[23];
    if (count != RECORD_COUNT) {
        fprintf(stderr, "record count %u != %u\n", (unsigned)count,
                (unsigned)RECORD_COUNT);
        fclose(fp);
        return 1;
    }
    g_fold = 0x5A5A5A5A5A5A5A5Aull;
    int bad = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t lenb[4];
        if (fread(lenb, 1, 4u, fp) != 4u) { bad = 1; break; }
        const size_t n = ((size_t)lenb[0] << 24) | ((size_t)lenb[1] << 16) |
                         ((size_t)lenb[2] << 8) | (size_t)lenb[3];
        uint8_t* stored = (uint8_t*)malloc(n ? n : 1u);
        if (stored == NULL || fread(stored, 1, n, fp) != n) {
            free(stored);
            bad = 1;
            break;
        }
        g_fold = mix64(g_fold, (uint64_t)n);
        for (size_t k = 0; k < n; ++k) { g_fold = mix64(g_fold, stored[k]); }
        uint8_t* built = NULL;
        size_t bn = 0;
        int32_t rc = build_record(i, &built, &bn);
        if (rc != TC_OK || bn != n || memcmp(built, stored, n) != 0) {
            fprintf(stderr, "record %u MISMATCH (rc=%d built=%zu stored=%zu)\n",
                    (unsigned)i, (int)rc, bn, n);
            bad = 1;
        }
        free(built);
        free(stored);
        if (bad) { break; }
    }
    if (!bad && g_fold != want_fold) {
        fprintf(stderr, "fold mismatch: computed %016llx want %016llx\n",
                (unsigned long long)g_fold, (unsigned long long)want_fold);
        bad = 1;
    }
    fclose(fp);
    if (bad) { return 1; }
    printf("golden_mov_audio: all %u records match (fold %016llx)\n",
           (unsigned)count, (unsigned long long)g_fold);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[1], "gen") == 0) { return gen_mode(argv[2]); }
    if (argc == 3 && strcmp(argv[1], "check") == 0) { return check_mode(argv[2]); }
    fprintf(stderr, "usage: golden_mov_audio gen|check <file>\n");
    return 2;
}
