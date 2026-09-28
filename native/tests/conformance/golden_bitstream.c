/* golden_bitstream —— 阶段 3 conformance（v1 冻结）。
 *
 * 记录集（56 条，由同一组确定性 builder 在 gen/check 两侧重建）：
 *   A 8× frame header（53B）   B 30× Rice 符号流（k=0..14 × 2）
 *   C 12× 块符号 payload       D 6× 完整合法 packet（packet_synth 全配置）
 *
 * 文件格式：magic "TPB1"(4) | version u32be=1 | count u32be | fold u64be |
 *           记录{ u32be len | bytes }。fold = 逐字节 splitmix 指纹（截断/篡改可检出）。
 *
 * check = 重建期望字节与入库文件逐字节比对 + fold 校验 —— 编码器输出的任何
 * 无声漂移（重构、编译器差异、平台）都会在此门禁失败（spec §11.1/§13.3）。
 *
 * 用法：golden_bitstream gen <file> | check <file>
 * 有意变更语法/熵语义时：走 ADR + 提升位流 minor 版本，然后手动重新生成。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/bitstream/bitio.h"
#include "../../src/bitstream/frame_header.h"
#include "../../src/bitstream/packet.h"
#include "../../src/bitstream/slice_codec.h"
#include "../../src/bitstream/slice_map.h"
#include "../../src/common/endian.h"
#include "../../src/entropy/block_coding.h"
#include "../../src/entropy/rice.h"
#include "../support/packet_synth.h"

static uint64_t g_fold;

static void fold_reset(void) { g_fold = 0x5A5A5A5A5A5A5A5Aull; }

static uint64_t mix64(uint64_t h, uint64_t v)
{
    h += 0x9E3779B97F4A7C15ull;
    uint64_t z = v;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return h ^ (z ^ (z >> 31));
}

static void fold_bytes(const uint8_t* data, size_t size)
{
    for (size_t i = 0; i < size; ++i) { g_fold = mix64(g_fold, data[i]); }
}

static uint64_t g_rng;
static uint64_t rng(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}

/* —— record 构建器：每条返回 malloc 的字节与长度 —— */

typedef int32_t (*builder_fn)(uint8_t** out, size_t* out_size);

static void header_base(topos_frame_header* fh)
{
    memset(fh, 0, sizeof(*fh));
    fh->version_major = 1u;
    fh->profile = 3u;
    fh->bit_depth = 10u;
    fh->coded_width = 40u;
    fh->coded_height = 24u;
    fh->visible_width = 36u;
    fh->visible_height = 20u;
    fh->plane_count = 3u;
    fh->qp_base = 20u;
    fh->slice_count = 3u;
    fh->color_range = 1u;
    fh->color_primaries = 1u;
    fh->color_transfer = 1u;
    fh->color_matrix = 1u;
    fh->frame_packet_size = 12345u;
}

static int32_t build_header(uint32_t variant, uint8_t** out, size_t* out_size)
{
    topos_frame_header fh;
    header_base(&fh);
    switch (variant) {
    case 0: break;
    case 1:
        fh.alpha_mode = 1u; fh.alpha_bit_depth = 16u; fh.plane_count = 4u;
        fh.slice_count = 4u;
        break;
    case 2:
        fh.visible_width = 1920u; fh.visible_height = 1080u;
        fh.coded_width = 1920u; fh.coded_height = 1088u;
        fh.slice_count = 34u;
        break;
    case 3: fh.qp_base = 0u; fh.qmatrix_id = 1u; break;
    case 4: fh.qp_base = 63u; break;
    case 5:
        fh.color_primaries = 9u; fh.color_transfer = 16u; fh.color_matrix = 9u;
        fh.color_range = 0u; fh.chroma_siting = 2u;
        break;
    case 6: fh.sar_num = 5u; fh.sar_den = 4u; fh.slice_count = 512u; break;
    case 7:
        fh.flags = 1u; /* alpha_premultiplied */
        fh.visible_width = 8u; fh.visible_height = 8u;
        fh.coded_width = 8u; fh.coded_height = 8u;
        fh.slice_count = 3u; /* 每平面 1 带（slice_count 下限 = plane_count） */
        break;
    default: return TC_ERR_INVALID_ARGUMENT;
    }
    uint8_t* buf = (uint8_t*)malloc(TC_FRAME_HEADER_SIZE);
    if (buf == NULL) { return TC_ERR_OUT_OF_MEMORY; }
    int32_t rc = tc_frame_header_encode(&fh, buf);
    if (rc != TC_OK) { free(buf); return rc; }
    *out = buf;
    *out_size = TC_FRAME_HEADER_SIZE;
    return TC_OK;
}

static int32_t build_rice_stream(uint32_t variant, uint8_t** out, size_t* out_size)
{
    /* variant: 0..14 = k，配流 A（小值/中值）；15..29 = k，配流 B（escape 密集） */
    uint32_t k = variant % 15u;
    int heavy = variant >= 15u;
    g_rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)(variant * 0x1Fu + 7u);

    tc_bitwriter bw;
    int32_t rc = tc_bitwriter_init(&bw);
    if (rc != TC_OK) { return rc; }
    for (int i = 0; i < 96; ++i) {
        uint32_t m;
        if (heavy) {
            m = (uint32_t)(rng() >> 8) | 0x80000000u; /* 大多数走 escape 字面值 */
            if ((i % 8) == 0) { m = (uint32_t)(rng() % 33ull); }
        } else if ((i % 5) == 0) {
            m = (uint32_t)(rng() % 4096ull);
        } else {
            m = (uint32_t)(rng() % 64ull);
        }
        rc = tc_rice_encode(&bw, k, m);
        if (rc != TC_OK) { break; }
    }
    if (rc == TC_OK) { rc = tc_bitwriter_flush_zero_pad(&bw); }
    if (rc != TC_OK) { tc_bitwriter_free(&bw); return rc; }
    size_t n = tc_bitwriter_byte_size(&bw);
    uint8_t* buf = (uint8_t*)malloc(n ? n : 1u);
    if (buf == NULL) { tc_bitwriter_free(&bw); return TC_ERR_OUT_OF_MEMORY; }
    memcpy(buf, tc_bitwriter_data(&bw), n);
    tc_bitwriter_free(&bw);
    *out = buf;
    *out_size = n;
    return TC_OK;
}

static int32_t build_block_payload(uint32_t variant, uint8_t** out, size_t* out_size)
{
    g_rng = 0xC0FFEE1234567890ull ^ (uint64_t)(variant * 0x9E37ull + 11u);
    int32_t q[64];
    memset(q, 0, sizeof(q));
    switch (variant) {
    case 0: break;                                        /* 全零：DC+EOB */
    case 1: q[0] = 4096; break;                           /* DC only */
    case 2: q[0] = -4096; break;
    case 3: q[63] = -77; break;                           /* natural 63 唯一 AC */
    case 4: q[1] = 1 << 25; q[8] = -(1 << 25); break;     /* AC 域界 */
    case 5: q[0] = -(1 << 26); break;                     /* DC 残差域界 */
    case 6:
        for (int i = 1; i < 64; ++i) { q[i] = (i % 2) ? i : -i; } /* 稠密交替 */
        break;
    case 7: q[0] = 30000; q[9] = -22000; q[36] = 15; break;
    default:
        for (int i = 0; i < 8; ++i) {
            int pos = (int)(rng() % 64ull);
            int32_t mag = (int32_t)(rng() % 100000ull) + 1;
            q[pos] = (rng() & 1ull) != 0ull ? mag : -mag;
        }
        q[0] = (int32_t)(rng() % 100000ull);
        break;
    }
    uint32_t k1 = (uint32_t)(variant % 15u);
    uint32_t k2 = (uint32_t)((variant * 7u + 3u) % 15u);
    uint32_t k3 = (uint32_t)((variant * 5u + 1u) % 15u);
    int has_left = (variant & 1u) != 0u;
    int has_top = (variant & 2u) != 0u;
    int32_t left = (int32_t)(rng() % 20000ull) - 10000;
    int32_t top = (int32_t)(rng() % 20000ull) - 10000;

    tc_bitwriter bw;
    int32_t rc = tc_bitwriter_init(&bw);
    if (rc != TC_OK) { return rc; }
    rc = tc_block_encode(&bw, k1, k2, k3, q, has_left, left, has_top, top, NULL);
    if (rc == TC_OK) { rc = tc_bitwriter_flush_zero_pad(&bw); }
    if (rc != TC_OK) { tc_bitwriter_free(&bw); return rc; }
    size_t n = tc_bitwriter_byte_size(&bw);
    uint8_t* buf = (uint8_t*)malloc(n ? n : 1u);
    if (buf == NULL) { tc_bitwriter_free(&bw); return TC_ERR_OUT_OF_MEMORY; }
    memcpy(buf, tc_bitwriter_data(&bw), n);
    tc_bitwriter_free(&bw);
    *out = buf;
    *out_size = n;
    return TC_OK;
}

static int32_t build_packet(uint32_t variant, uint8_t** out, size_t* out_size)
{
    if (variant >= PACKET_SYNTH_CFG_COUNT) { return TC_ERR_INVALID_ARGUMENT; }
    return packet_synth_build(packet_synth_cfg_at(variant), out, out_size);
}

#define GOLDEN_MAGIC "TPB1"
#define GOLDEN_VERSION 1u
#define RECORD_COUNT (8u + 30u + 12u + PACKET_SYNTH_CFG_COUNT)

static int32_t build_record(uint32_t index, uint8_t** out, size_t* out_size)
{
    if (index < 8u) { return build_header(index, out, out_size); }
    index -= 8u;
    if (index < 30u) { return build_rice_stream(index, out, out_size); }
    index -= 30u;
    if (index < 12u) { return build_block_payload(index, out, out_size); }
    index -= 12u;
    return build_packet(index, out, out_size);
}

static int gen_mode(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (f == NULL) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    uint8_t hdr[24];
    memcpy(hdr, GOLDEN_MAGIC, 4u);
    tc_store_be32(hdr + 4, GOLDEN_VERSION);
    tc_store_be32(hdr + 8, RECORD_COUNT);
    tc_store_be64(hdr + 16, 0u); /* fold 占位，收尾回写 */
    fwrite(hdr, 1, sizeof(hdr), f);

    fold_reset();
    for (uint32_t i = 0; i < RECORD_COUNT; ++i) {
        uint8_t* data = NULL;
        size_t n = 0;
        int32_t rc = build_record(i, &data, &n);
        if (rc != TC_OK) {
            fprintf(stderr, "builder %u failed: %d\n", (unsigned)i, rc);
            fclose(f);
            return 1;
        }
        uint8_t lenb[4];
        tc_store_be32(lenb, (uint32_t)n);
        fwrite(lenb, 1, 4u, f);
        fwrite(data, 1, n, f);
        g_fold = mix64(g_fold, (uint64_t)n);
        fold_bytes(data, n);
        free(data);
    }
    fseek(f, 16, SEEK_SET);
    uint8_t foldb[8];
    tc_store_be64(foldb, g_fold);
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
    uint32_t version = tc_load_be32(all + 4);
    uint32_t count = tc_load_be32(all + 8);
    uint64_t fold = tc_load_be64(all + 16);
    if (version != GOLDEN_VERSION || count != RECORD_COUNT) {
        fprintf(stderr, "version/count mismatch: %u/%u\n", (unsigned)version, (unsigned)count);
        free(all);
        return 1;
    }

    size_t off = 24u;
    fold_reset();
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 4u > total) { fprintf(stderr, "record %u truncated\n", (unsigned)i); break; }
        uint32_t n = tc_load_be32(all + off);
        off += 4u;
        if (off + n > total) { fprintf(stderr, "record %u truncated\n", (unsigned)i); break; }
        const uint8_t* stored = all + off;

        uint8_t* expect = NULL;
        size_t expect_n = 0;
        int32_t rc = build_record(i, &expect, &expect_n);
        if (rc != TC_OK) {
            fprintf(stderr, "record %u builder failed: %d\n", (unsigned)i, rc);
            free(all);
            return 1;
        }
        if (expect_n != (size_t)n || memcmp(expect, stored, expect_n) != 0) {
            fprintf(stderr, "record %u MISMATCH: stored %u bytes, rebuilt %zu\n",
                    (unsigned)i, (unsigned)n, expect_n);
            free(expect);
            free(all);
            return 1;
        }
        free(expect);
        g_fold = mix64(g_fold, (uint64_t)n);
        fold_bytes(stored, n);
        off += n;
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
    printf("golden bitstream: %u records byte-exact, fold %016llx OK\n",
           (unsigned)count, (unsigned long long)fold);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[1], "gen") == 0) { return gen_mode(argv[2]); }
    if (argc == 3 && strcmp(argv[1], "check") == 0) { return check_mode(argv[2]); }
    fprintf(stderr, "usage: golden_bitstream gen|check <file>\n");
    return 2;
}
