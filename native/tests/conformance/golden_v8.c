/* golden_v8 —— V8 位流 conformance（批 5）。
 *
 * 记录集（3 条，确定性 builder 两侧重建）：真实 tc_frame_encode（reserved[0]=9）
 * 产出的完整 V8 packet，覆盖 粒度档 × {422/444, 10/12bit, qp}：
 *   A 96×96  422 10bit qp28  sb=16 tile=32（默认档）
 *   B 96×96  422 10bit qp20  sb=8  tile=16（细段/细瓦片）
 *   C 80×72  444 12bit qp35  sb=32 tile=64（粗档 + 444/12bit）
 *
 * 文件格式：magic "TV81"(4) | version u32be=1 | count u32be | fold u64be |
 *           记录{ u32be len | u64be pixel_fold | bytes }。fold = 逐字节
 *           splitmix 指纹（截断/篡改可检出）；pixel_fold = 解码像素折叠
 *           （解码器行为冻结：committed 流 → 逐位像素约定）。
 *
 * check = 重编码逐字节比对 + 结构扫描全瓦片 CRC + 解码 conceal=0 +
 *         像素 fold 比对。编码器/解码器任何无声漂移都会在此门禁失败。
 *
 * 用法：golden_v8 gen <file> | check <file>
 * 有意变更 V8 位流/解码语义时：走 ADR + 提升 V8 minor，重新生成。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/bitstream/packet.h"
#include "../../src/common/endian.h"
#include "topos_codec.h"

#define GOLDEN_VERSION 1u
#define RECORD_COUNT 3u

static uint64_t mix64(uint64_t h, uint64_t v)
{
    h += 0x9E3779B97F4A7C15ull;
    uint64_t z = v;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return h ^ (z ^ (z >> 31));
}

static void fold_bytes(uint64_t* h, const uint8_t* data, size_t size)
{
    for (size_t i = 0; i < size; ++i) { *h = mix64(*h, data[i]); }
}

static uint64_t g_rng;

static uint64_t rng(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}

/* ---- 记录配置与确定性 builder ---- */

typedef struct v8_record_cfg {
    uint16_t w, h;
    uint8_t pf;        /* 0=422, 1=444 */
    uint8_t bd;        /* 10/12 */
    uint8_t qp;
    uint8_t sb_log2;   /* 3/4/5 */
    uint8_t tr_log2;   /* 4/5/6 */
    uint64_t seed;
} v8_record_cfg;

static const v8_record_cfg kCfg[RECORD_COUNT] = {
    { 96u, 96u, 0u, 10u, 28u, 4u, 5u, 0xA811111111111111ull },
    { 96u, 96u, 0u, 10u, 20u, 3u, 4u, 0xB822222222222222ull },
    { 80u, 72u, 1u, 12u, 35u, 5u, 6u, 0xC833333333333333ull },
};

static uint32_t plane_cols(uint32_t p, uint32_t w, uint32_t pf)
{
    if (p == 0u) { return (w + 7u) / 8u; }
    return pf == 0u ? (w + 15u) / 16u : (w + 7u) / 8u;
}

/* 确定性帧内容（LCG；值域 < 2^bd）→ planes 紧凑 u16 */
static void build_planes(const v8_record_cfg* c, uint16_t* planes[3])
{
    const uint32_t lim = 1u << c->bd;
    g_rng = c->seed;
    for (uint32_t p = 0u; p < 3u; ++p) {
        const uint32_t w = plane_cols(p, c->w, c->pf) * 8u;
        const uint32_t h = ((c->h + 7u) / 8u) * 8u;
        uint16_t* buf = planes[p];
        for (uint32_t i = 0; i < w * h; ++i) {
            buf[i] = (uint16_t)(rng() % lim);
        }
    }
}

static int32_t build_record(uint32_t idx, uint8_t** out, size_t* out_size,
                            uint64_t* pixel_fold)
{
    const v8_record_cfg* c = &kCfg[idx];
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = c->w;
    cfg.visible_height = c->h;
    cfg.profile = c->pf == 1u ? 5u : 3u;
    cfg.pixel_format = c->pf;
    cfg.bit_depth = c->bd;
    cfg.qmatrix_id = 1u;
    cfg.qp_base = c->qp;
    cfg.slice_rows = 16u;
    cfg.color_range = 1u;
    cfg.color_primaries = 1u;
    cfg.color_transfer = 1u;
    cfg.color_matrix = 1u;
    cfg.reserved[0] = 9u;              /* V8 */
    cfg.reserved[3] = c->sb_log2;
    cfg.reserved[4] = c->tr_log2;

    uint16_t* planes[3];
    for (uint32_t p = 0u; p < 3u; ++p) {
        planes[p] = (uint16_t*)malloc((size_t)plane_cols(p, c->w, c->pf) * 8u
                                      * ((c->h + 7u) / 8u) * 8u * 2u);
        if (planes[p] == NULL) { return TC_ERR_OUT_OF_MEMORY; }
    }
    build_planes(c, planes);

    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    for (uint32_t p = 0u; p < 3u; ++p) {
        in.planes[p] = planes[p];
        in.strides[p] = plane_cols(p, c->w, c->pf) * 8u;
    }

    size_t cap = tc_frame_packet_bound(&cfg);
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (buf == NULL) {
        for (uint32_t p = 0u; p < 3u; ++p) { free(planes[p]); }
        return TC_ERR_OUT_OF_MEMORY;
    }
    topos_frame_stats st;
    int32_t rc = tc_frame_encode(&cfg, &in, buf, cap, &st);
    if (rc != TC_OK) {
        fprintf(stderr, "record %u encode failed: %s\n", (unsigned)idx,
                tc_status_message(rc));
        goto done;
    }
    if (pixel_fold != NULL) {
        /* 解码冻结面：committed 流 → conceal=0 + 像素 fold */
        topos_frame_output info;
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t)sizeof(info);
        info.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        uint16_t* dplanes[3] = { 0, 0, 0 };
        size_t dstr[3] = { 0, 0, 0 };
        for (uint32_t p = 0u; p < 3u; ++p) {
            const uint32_t cols = plane_cols(p, c->w, c->pf);
            const uint32_t rows = (c->h + 7u) / 8u;
            dplanes[p] = (uint16_t*)malloc((size_t)cols * 8u * rows * 8u * 2u);
            dstr[p] = (size_t)cols * 8u;
            if (dplanes[p] == NULL) { rc = TC_ERR_OUT_OF_MEMORY; goto done; }
        }
        rc = tc_frame_decode(buf, st.packet_size, dplanes, dstr, &info);
        if (rc != TC_OK) {
            fprintf(stderr, "record %u decode failed: %s\n", (unsigned)idx,
                    tc_status_message(rc));
            goto done;
        }
        if (info.concealed_slices != 0u) {
            fprintf(stderr, "record %u unexpected conceal %u\n", (unsigned)idx,
                    (unsigned)info.concealed_slices);
            rc = TC_ERR_MALFORMED;
            goto done;
        }
        uint64_t fold = 0x51A5A5A5A5A5A5A5ull;
        for (uint32_t p = 0u; p < 3u; ++p) {
            const uint32_t cols = plane_cols(p, c->w, c->pf);
            const uint32_t rows = (c->h + 7u) / 8u;
            for (uint32_t y = 0; y < rows * 8u; ++y) {
                fold_bytes(&fold, (const uint8_t*)(dplanes[p] + (size_t)y * cols * 8u),
                           (size_t)cols * 8u * 2u);
            }
        }
        *pixel_fold = fold;
        for (uint32_t p = 0u; p < 3u; ++p) { free(dplanes[p]); }
    }
    *out_size = st.packet_size;
    *out = buf;
    buf = NULL;
done:
    for (uint32_t p = 0u; p < 3u; ++p) { free(planes[p]); }
    free(buf);
    return rc;
}

/* ---- 文件 IO ---- */

static int write_file(const char* path, const uint8_t* data, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (f == NULL) { return -1; }
    size_t w = fwrite(data, 1, n, f);
    fclose(f);
    return w == n ? 0 : -1;
}

static uint8_t* read_file(const char* path, size_t* n_out)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) { return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    rewind(f);
    if (n < 24) { fclose(f); return NULL; }
    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (buf == NULL) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if ((long)r != n) { free(buf); return NULL; }
    *n_out = (size_t)n;
    return buf;
}

static int gen_mode(const char* path)
{
    const size_t hdr = 4u + 4u + 4u + 8u;
    size_t cap = hdr;
    uint8_t* rec[RECORD_COUNT] = { 0 };
    size_t rec_n[RECORD_COUNT] = { 0 };
    uint64_t pfold[RECORD_COUNT] = { 0 };
    int ret = 1;

    for (uint32_t i = 0; i < RECORD_COUNT; ++i) {
        const int32_t rc = build_record(i, &rec[i], &rec_n[i], &pfold[i]);
        if (rc != TC_OK) { goto out; }
        cap += 4u + 8u + rec_n[i];
    }
    uint8_t* file = (uint8_t*)malloc(cap);
    if (file == NULL) { goto out; }
    memcpy(file, "TV81", 4u);
    tc_store_be32(file + 4, GOLDEN_VERSION);
    tc_store_be32(file + 8, RECORD_COUNT);
    uint64_t fold = 0x5A5A5A5A5A5A5A5Aull;
    size_t off = 20u;
    for (uint32_t i = 0; i < RECORD_COUNT; ++i) {
        tc_store_be32(file + off, (uint32_t)rec_n[i]);
        tc_store_be64(file + off + 4, pfold[i]);
        memcpy(file + off + 12, rec[i], rec_n[i]);
        off += 12u + rec_n[i];
        fold = mix64(fold, rec_n[i]);
        fold_bytes(&fold, rec[i], rec_n[i]);
    }
    tc_store_be64(file + 12, fold);
    if (write_file(path, file, cap) != 0) {
        fprintf(stderr, "cannot write %s\n", path);
    } else {
        printf("golden v8: %u records written (%zu bytes)\n",
               (unsigned)RECORD_COUNT, cap);
        ret = 0;
    }
    free(file);
out:
    for (uint32_t i = 0; i < RECORD_COUNT; ++i) { free(rec[i]); }
    return ret;
}

static int check_mode(const char* path)
{
    size_t total = 0u;
    uint8_t* all = read_file(path, &total);
    if (all == NULL) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    if (memcmp(all, "TV81", 4u) != 0) { fprintf(stderr, "bad magic\n"); free(all); return 1; }
    const uint32_t version = tc_load_be32(all + 4);
    const uint32_t count = tc_load_be32(all + 8);
    const uint64_t fold = tc_load_be64(all + 12);
    if (version != GOLDEN_VERSION || count != RECORD_COUNT) {
        fprintf(stderr, "version/count mismatch: %u/%u\n",
                (unsigned)version, (unsigned)count);
        free(all);
        return 1;
    }

    uint64_t g_fold = 0x5A5A5A5A5A5A5A5Aull;
    size_t off = 20u;
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 12u > total) { fprintf(stderr, "record %u truncated\n", (unsigned)i); free(all); return 1; }
        const uint32_t n = tc_load_be32(all + off);
        const uint64_t pfold = tc_load_be64(all + off + 4);
        off += 12u;
        if (off + n > total) { fprintf(stderr, "record %u truncated\n", (unsigned)i); free(all); return 1; }
        const uint8_t* stored = all + off;
        off += n;

        uint8_t* rebuilt = NULL;
        size_t rebuilt_n = 0;
        uint64_t ignore = 0;
        if (build_record(i, &rebuilt, &rebuilt_n, &ignore) != TC_OK) {
            fprintf(stderr, "record %u builder failed\n", (unsigned)i);
            free(all);
            return 1;
        }
        if (rebuilt_n != (size_t)n || memcmp(rebuilt, stored, rebuilt_n) != 0) {
            fprintf(stderr, "record %u MISMATCH: stored %u bytes, rebuilt %zu\n",
                    (unsigned)i, (unsigned)n, rebuilt_n);
            free(rebuilt);
            free(all);
            return 1;
        }
        free(rebuilt);

        /* 结构扫描：全瓦片 CRC 必须 OK（committed 流结构冻结） */
        topos_v8_packet_view view;
        if (tc_packet_scan_v8(stored, n, &view) != TC_OK) {
            fprintf(stderr, "record %u scan_v8 failed: %s\n", (unsigned)i,
                    tc_last_error());
            free(all);
            return 1;
        }
        for (uint32_t t = 0; t < view.tile_count; ++t) {
            if (view.tiles[t].crc_ok == 0u) {
                fprintf(stderr, "record %u tile %u CRC bad\n", (unsigned)i, t);
                free(all);
                return 1;
            }
        }
        /* 解码像素冻结：pixel_fold 比对（conceal=0 已在 builder 断言） */
        uint8_t* rd = NULL;
        size_t rd_n = 0;
        uint64_t got_fold = 0;
        if (build_record(i, &rd, &rd_n, &got_fold) != TC_OK || rd == NULL) {
            fprintf(stderr, "record %u decode rebuild failed\n", (unsigned)i);
            free(all);
            return 1;
        }
        free(rd);
        if (got_fold != pfold) {
            fprintf(stderr, "record %u pixel fold MISMATCH: %016llx != %016llx\n",
                    (unsigned)i, (unsigned long long)got_fold,
                    (unsigned long long)pfold);
            free(all);
            return 1;
        }
        g_fold = mix64(g_fold, n);
        fold_bytes(&g_fold, stored, n);
    }
    if (off != total) {
        fprintf(stderr, "trailing bytes after last record\n");
        free(all);
        return 1;
    }
    if (g_fold != fold) {
        fprintf(stderr, "fold mismatch: %016llx != %016llx\n",
                (unsigned long long)g_fold, (unsigned long long)fold);
        free(all);
        return 1;
    }
    free(all);
    printf("golden v8: %u records byte-exact + decode pixel-exact, fold %016llx OK\n",
           (unsigned)count, (unsigned long long)fold);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[1], "gen") == 0) { return gen_mode(argv[2]); }
    if (argc == 3 && strcmp(argv[1], "check") == 0) { return check_mode(argv[2]); }
    fprintf(stderr, "usage: golden_v8 gen|check <file>\n");
    return 2;
}
