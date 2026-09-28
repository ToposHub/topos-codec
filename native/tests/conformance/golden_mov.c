/* golden_mov —— 阶段 5 conformance（v1 冻结）。
 *
 * 记录集（每用例 2 条：偶数 = MOV 文件字节，奇数 = 索引摘要）：
 *   摘要 = n u32be + index_bytes u32be + faststart u32be
 *          + 每 sample {offset u64, size u32, pts u64, dur u32, sync u8}
 *
 * 用例（确定性，image_synth 内容）：
 *   0/1   64×40 grain qp28 qm1，3 帧，标准布局
 *   2/3   同 0 但 FastStart 布局
 *   4/5   36×20（奇尺寸）alpha mode1 无损，5 帧
 *   6/7   64×40 alpha mode2(s=2) + sar 5/4（pasp 存在），4 帧
 *   8/9   变时长 stts（dur 1/2/3 交替 → 多 run），6 帧
 *
 * 文件格式：magic "TPM1" | version u32be=1 | count u32be | fold u64be |
 *           记录{ u32be len | bytes }。fold = splitmix 逐字节指纹。
 *
 * check = 重建与入库逐字节比对 —— mux 输出任何无声漂移（字段顺序、
 * 时间戳、表布局）在此失败（container_spec §8 确定性）。
 *
 * 用法：golden_mov gen <file> | check <file>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../support/image_synth.h"
#include "topos_codec.h"

static uint64_t g_fold;

static uint64_t mix64(uint64_t h, uint64_t v)
{
    h += 0x9E3779B97F4A7C15ull;
    uint64_t z = v;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return h ^ (z ^ (z >> 31));
}

/* —— 内存 io —— */
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
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void store_be64(uint8_t* p, uint64_t v)
{
    store_be32(p, (uint32_t)(v >> 32));
    store_be32(p + 4, (uint32_t)v);
}

/* —— 用例定义 —— */
typedef struct {
    uint32_t w, h, frames;
    uint8_t qp, qm;
    uint8_t alpha_mode;      /* 0=无 alpha */
    uint8_t alpha_bit_depth;
    uint16_t sar_num, sar_den;
    int faststart;
    int var_dur;             /* dur ticks 1/2/3 循环（多 run stts） */
    tc_synth_kind kind;
    uint64_t seed;
} mov_case;

static const mov_case CASES[] = {
    { 64u, 40u, 3u, 28u, 1u, 0u, 16u, 1u, 1u, 0, 0, TC_SYNTH_GRAIN, 0xA1 },
    { 64u, 40u, 3u, 28u, 1u, 0u, 16u, 1u, 1u, 1, 0, TC_SYNTH_GRAIN, 0xA1 },
    { 36u, 20u, 5u, 24u, 0u, 1u, 16u, 1u, 1u, 0, 0, TC_SYNTH_DETAIL, 0xB2 },
    { 64u, 40u, 4u, 32u, 1u, 2u, 12u, 5u, 4u, 0, 0, TC_SYNTH_MIXED, 0xC3 },
    { 64u, 40u, 6u, 30u, 1u, 0u, 16u, 1u, 1u, 0, 1, TC_SYNTH_GRADIENT, 0xD4 },
};

#define CASE_COUNT ((uint32_t)(sizeof(CASES) / sizeof(CASES[0])))

/* mux 单用例 → 文件字节 */
static int32_t build_movie(const mov_case* c, mem_file* out)
{
    topos_movie_config mc;
    memset(&mc, 0, sizeof(mc));
    mc.struct_size = (uint32_t)sizeof(mc);
    mc.abi_version = TOPOS_CODEC_ABI_VERSION;
    mc.visible_width = (uint16_t)c->w;
    mc.visible_height = (uint16_t)c->h;
    mc.profile = 3u;
    mc.bit_depth = 10u;
    mc.qmatrix_id = c->qm;
    mc.qp_base = c->qp;
    mc.alpha_mode = c->alpha_mode;
    mc.alpha_bit_depth = c->alpha_mode != 0u ? c->alpha_bit_depth : 0u;
    mc.color_range = 1u;
    mc.color_primaries = 1u;
    mc.color_transfer = 1u;
    mc.color_matrix = 1u;
    mc.sar_num = c->sar_num;
    mc.sar_den = c->sar_den;
    mc.timescale = 24u;

    topos_frame_config fc;
    memset(&fc, 0, sizeof(fc));
    fc.struct_size = (uint32_t)sizeof(fc);
    fc.abi_version = TOPOS_CODEC_ABI_VERSION;
    fc.visible_width = (uint16_t)c->w;
    fc.visible_height = (uint16_t)c->h;
    fc.profile = 3u;
    fc.bit_depth = 10u;
    fc.qmatrix_id = c->qm;
    fc.qp_base = c->qp;
    fc.alpha_mode = c->alpha_mode;
    fc.alpha_bit_depth = mc.alpha_bit_depth;
    fc.color_range = 1u;
    fc.color_primaries = 1u;
    fc.color_transfer = 1u;
    fc.color_matrix = 1u;
    fc.sar_num = c->sar_num;
    fc.sar_den = c->sar_den;

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

    image_synth_cfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.width = c->w;
    sc.height = c->h;
    sc.kind = c->kind;
    sc.seed = c->seed;

    uint16_t* pl[4] = { NULL, NULL, NULL, NULL };
    image_synth_alloc(&sc, c->alpha_mode != 0u, &pl[0], &pl[1], &pl[2], &pl[3]);

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
    uint64_t pts = 0u;
    if (pkt == NULL) { rc = TC_ERR_OUT_OF_MEMORY; }
    for (uint32_t i = 0; i < c->frames && rc == TC_OK; ++i) {
        sc.seed = c->seed + (uint64_t)i;
        image_synth_build(&sc, pl[0], pl[1], pl[2], pl[3]);
        fc.qp_base = (uint8_t)(c->qp + (i % 3u)); /* 逐帧 qp 变化（tpcC 豁免项） */
        rc = tc_frame_encode(&fc, &in, pkt, bound, &st);
        if (rc == TC_OK) {
            uint32_t dur = c->var_dur ? (1u + (i % 3u)) : 1u;
            rc = tc_mux_add_packet(m, pkt, st.packet_size, pts, dur);
            pts += dur;
        }
    }
    free(pkt);
    free(pl[0]);
    free(pl[1]);
    free(pl[2]);
    free(pl[3]);
    if (rc == TC_OK) { rc = tc_mux_finish(m); }
    tc_mux_free(m);
    if (rc != TC_OK) { return rc; }

    if (c->faststart) {
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
    }
    return rc;
}

/* 索引摘要（offset/size/pts/dur/sync + 元信息） */
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
    if (rc == TC_OK) {
        uint8_t b4[4];
        uint8_t b8[8];
        store_be32(b4, info.sample_count);
        mf_write(sum, b4, 4u);
        store_be32(b4, info.index_bytes);
        mf_write(sum, b4, 4u);
        store_be32(b4, info.faststart);
        mf_write(sum, b4, 4u);
        store_be32(b4, info.timescale);
        mf_write(sum, b4, 4u);
        for (uint32_t i = 0; i < info.sample_count && rc == TC_OK; ++i) {
            size_t need = 0;
            rc = tc_movie_packet(mv, i, NULL, 0, &need);
            if (rc != TC_ERR_BUFFER_TOO_SMALL) { rc = TC_ERR_MALFORMED; break; }
            rc = TC_OK;
            uint64_t pts = 0;
            uint32_t dur = 0;
            uint8_t sync = 0;
            tc_movie_packet_pts(mv, i, &pts, &dur);
            tc_movie_packet_sync(mv, i, &sync);
            store_be64(b8, pts);
            mf_write(sum, b8, 8u);
            store_be32(b4, dur);
            mf_write(sum, b4, 4u);
            store_be32(b4, (uint32_t)need);
            mf_write(sum, b4, 4u);
            mf_write(sum, &sync, 1u);
        }
    }
    tc_movie_close(mv);
    return rc;
}

#define GOLDEN_MAGIC "TPM1"
#define GOLDEN_VERSION 1u
#define RECORD_COUNT (CASE_COUNT * 2u)

static int32_t build_record(uint32_t index, uint8_t** out, size_t* out_size)
{
    mem_file f;
    memset(&f, 0, sizeof(f));
    int32_t rc = build_movie(&CASES[index / 2u], &f);
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
    FILE* f = fopen(path, "wb");
    if (f == NULL) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    uint8_t hdr[24];
    memcpy(hdr, GOLDEN_MAGIC, 4u);
    store_be32(hdr + 4, GOLDEN_VERSION);
    store_be32(hdr + 8, RECORD_COUNT);
    store_be64(hdr + 16, 0u);
    fwrite(hdr, 1, sizeof(hdr), f);
    g_fold = 0x5A5A5A5A5A5A5A5Aull;
    for (uint32_t i = 0; i < RECORD_COUNT; ++i) {
        uint8_t* data = NULL;
        size_t n = 0;
        int32_t rc = build_record(i, &data, &n);
        if (rc != TC_OK) { fclose(f); return 1; }
        uint8_t lenb[4];
        store_be32(lenb, (uint32_t)n);
        fwrite(lenb, 1, 4u, f);
        fwrite(data, 1, n, f);
        g_fold = mix64(g_fold, (uint64_t)n);
        for (size_t k = 0; k < n; ++k) { g_fold = mix64(g_fold, data[k]); }
        free(data);
    }
    fseek(f, 16, SEEK_SET);
    uint8_t foldb[8];
    store_be64(foldb, g_fold);
    fwrite(foldb, 1, 8u, f);
    fclose(f);
    printf("generated %u records, fold %016llx\n", (unsigned)RECORD_COUNT,
           (unsigned long long)g_fold);
    return 0;
}

static int check_mode(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    uint8_t* all = (uint8_t*)malloc(64u * 1024u * 1024u);
    if (all == NULL) { fclose(f); return 1; }
    size_t total = fread(all, 1, 64u * 1024u * 1024u, f);
    fclose(f);
    if (total < 24u || memcmp(all, GOLDEN_MAGIC, 4u) != 0) {
        fprintf(stderr, "bad magic\n");
        free(all);
        return 1;
    }
    uint32_t version = (uint32_t)((all[4] << 24) | (all[5] << 16) | (all[6] << 8) | all[7]);
    uint32_t count = (uint32_t)((all[8] << 24) | (all[9] << 16) | (all[10] << 8) | all[11]);
    uint64_t fold = 0u;
    for (int i = 0; i < 8; ++i) { fold = (fold << 8) | all[16 + i]; }
    if (version != GOLDEN_VERSION || count != RECORD_COUNT) {
        fprintf(stderr, "version/count mismatch\n");
        free(all);
        return 1;
    }
    size_t off = 24u;
    g_fold = 0x5A5A5A5A5A5A5A5Aull;
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 4u > total) { break; }
        uint32_t n = (uint32_t)((all[off] << 24) | (all[off + 1] << 16) |
                                (all[off + 2] << 8) | all[off + 3]);
        off += 4u;
        if (off + n > total) { break; }
        uint8_t* expect = NULL;
        size_t expect_n = 0;
        int32_t rc = build_record(i, &expect, &expect_n);
        if (rc != TC_OK) { free(all); return 1; }
        if (expect_n != (size_t)n || memcmp(expect, all + off, expect_n) != 0) {
            fprintf(stderr, "record %u MISMATCH: stored %u, rebuilt %zu\n",
                    (unsigned)i, (unsigned)n, expect_n);
            free(expect);
            free(all);
            return 1;
        }
        free(expect);
        g_fold = mix64(g_fold, (uint64_t)n);
        for (uint32_t k = 0; k < n; ++k) { g_fold = mix64(g_fold, all[off + k]); }
        off += n;
    }
    if (g_fold != fold) {
        fprintf(stderr, "fold mismatch\n");
        free(all);
        return 1;
    }
    free(all);
    printf("golden mov: %u records byte-exact, fold %016llx OK\n",
           (unsigned)count, (unsigned long long)fold);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[1], "gen") == 0) { return gen_mode(argv[2]); }
    if (argc == 3 && strcmp(argv[1], "check") == 0) { return check_mode(argv[2]); }
    fprintf(stderr, "usage: golden_mov gen|check <file>\n");
    return 2;
}
