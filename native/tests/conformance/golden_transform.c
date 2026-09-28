/* golden_transform —— 阶段 2 conformance 向量生成/校验。
 *
 * 用法：
 *   topos_golden_transform gen   <file>    生成 golden（需在受控变更时手动执行）
 *   topos_golden_transform check <file>    重算全部记录并与文件逐字节比对
 *
 * 任何实现变更导致 check 失败时：要么修实现，要么（有意变更时）走
 * spec §13 版本规则 + 重生成 + ADR 记录。
 */
#include "golden_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transform/quant.h"
#include "transform/transform.h"

/* —— 确定性 PRNG（与 mini_test 独立，golden 专用种子） —— */
static uint64_t golden_rng(uint64_t* state)
{
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static void compute_record(golden_record* r, uint8_t depth, uint8_t qp,
                           const uint16_t px[64])
{
    const int32_t mid = 1 << (depth - 1);
    const int32_t maxv = (1 << depth) - 1;
    int16_t x[64];
    r->depth = depth;
    r->qp = qp;
    memcpy(r->px, px, sizeof r->px);
    for (int i = 0; i < 64; ++i) { x[i] = (int16_t)((int32_t)px[i] - mid); }
    const tc_qmatrix_set* qm = tc_qmatrix_by_id(0);
    tc_transform_forward_8x8(x, r->F);
    tc_quant_block(r->F, qm->luma, qp, r->q);
    tc_dequant_block(r->q, qm->luma, qp, r->Fp);
    int32_t xh[64];
    tc_transform_inverse_8x8(r->Fp, xh);
    for (int i = 0; i < 64; ++i) {
        int32_t v = xh[i] + mid;
        if (v < 0) { v = 0; }
        if (v > maxv) { v = maxv; }
        r->rec[i] = (uint16_t)v;
    }
}

size_t golden_enumerate(golden_emit_fn emit, void* ud)
{
    static const uint8_t depths[2] = { 10, 12 };
    static const uint8_t qps[5] = { 0, 4, 20, 40, 63 };
    size_t count = 0;
    golden_record rec;

    for (int d = 0; d < 2; ++d) {
        const uint8_t depth = depths[d];
        const uint32_t maxv = (1u << depth) - 1u;
        const uint32_t mid = 1u << (depth - 1);
        uint16_t px[64];

        /* 4 常量块 */
        const uint32_t cvals[4] = { 0u, mid, maxv, 333u };
        for (int c = 0; c < 4; ++c) {
            for (int i = 0; i < 64; ++i) { px[i] = (uint16_t)cvals[c]; }
            for (int qi = 0; qi < 5; ++qi) {
                compute_record(&rec, depth, qps[qi], px);
                emit(&rec, ud); ++count;
            }
        }
        /* 64 单脉冲（每位置，max 幅度） */
        for (int pos = 0; pos < 64; ++pos) {
            for (int i = 0; i < 64; ++i) { px[i] = 0; }
            px[pos] = (uint16_t)maxv;
            for (int qi = 0; qi < 5; ++qi) {
                compute_record(&rec, depth, qps[qi], px);
                emit(&rec, ud); ++count;
            }
        }
        /* 3 渐变 */
        for (int g = 0; g < 3; ++g) {
            for (int i = 0; i < 64; ++i) {
                const uint32_t xx = (uint32_t)(i % 8), yy = (uint32_t)(i / 8);
                uint32_t v;
                if (g == 0) { v = (maxv * xx) / 7u; }
                else if (g == 1) { v = (maxv * yy) / 7u; }
                else { v = (maxv * (xx + yy)) / 14u; }
                px[i] = (uint16_t)v;
            }
            for (int qi = 0; qi < 5; ++qi) {
                compute_record(&rec, depth, qps[qi], px);
                emit(&rec, ud); ++count;
            }
        }
        /* 3 棋盘（1/2/4 px） */
        for (int k = 1; k <= 3; ++k) {
            for (int i = 0; i < 64; ++i) {
                const uint32_t xx = (uint32_t)(i % 8), yy = (uint32_t)(i / 8);
                px[i] = (uint16_t)((((xx / (uint32_t)k) + (yy / (uint32_t)k)) & 1u)
                                       ? maxv : 0u);
            }
            for (int qi = 0; qi < 5; ++qi) {
                compute_record(&rec, depth, qps[qi], px);
                emit(&rec, ud); ++count;
            }
        }
        /* 64 随机块（固定种子族） */
        for (int b = 0; b < 64; ++b) {
            uint64_t st = 0xC0FFEE00ull + 0x1000ull * (uint64_t)b + (uint64_t)depth;
            for (int i = 0; i < 64; ++i) {
                px[i] = (uint16_t)(golden_rng(&st) % ((uint64_t)maxv + 1ull));
            }
            for (int qi = 0; qi < 5; ++qi) {
                compute_record(&rec, depth, qps[qi], px);
                emit(&rec, ud); ++count;
            }
        }
    }
    return count;
}

void golden_serialize(const golden_record* r, uint8_t out[GOLDEN_RECORD_SIZE])
{
    size_t o = 0;
    out[o++] = r->depth;
    out[o++] = r->qp;
    for (int i = 0; i < 64; ++i) {
        out[o++] = (uint8_t)(r->px[i] & 0xFF);
        out[o++] = (uint8_t)(r->px[i] >> 8);
    }
    for (int i = 0; i < 64; ++i) {
        uint32_t v = (uint32_t)r->F[i];
        out[o++] = (uint8_t)(v & 0xFF); out[o++] = (uint8_t)((v >> 8) & 0xFF);
        out[o++] = (uint8_t)((v >> 16) & 0xFF); out[o++] = (uint8_t)((v >> 24) & 0xFF);
    }
    for (int i = 0; i < 64; ++i) {
        uint32_t v = (uint32_t)r->q[i];
        out[o++] = (uint8_t)(v & 0xFF); out[o++] = (uint8_t)((v >> 8) & 0xFF);
        out[o++] = (uint8_t)((v >> 16) & 0xFF); out[o++] = (uint8_t)((v >> 24) & 0xFF);
    }
    for (int i = 0; i < 64; ++i) {
        uint32_t v = (uint32_t)r->Fp[i];
        out[o++] = (uint8_t)(v & 0xFF); out[o++] = (uint8_t)((v >> 8) & 0xFF);
        out[o++] = (uint8_t)((v >> 16) & 0xFF); out[o++] = (uint8_t)((v >> 24) & 0xFF);
    }
    for (int i = 0; i < 64; ++i) {
        out[o++] = (uint8_t)(r->rec[i] & 0xFF);
        out[o++] = (uint8_t)(r->rec[i] >> 8);
    }
}

uint64_t golden_checksum(const uint8_t* blob, size_t n)
{
    uint64_t h = 0x1234567890ABCDEFull;
    for (size_t i = 0; i < n; ++i) {
        h ^= blob[i];
        h = (h << 13) | (h >> 51);
        h *= 0x9E3779B97F4A7C15ull;
    }
    return h;
}

/* —— gen/check —— */
static void emit_to_mem(const golden_record* r, void* ud)
{
    struct { uint8_t* p; size_t n; }* sink = (void*)ud;
    uint8_t buf[GOLDEN_RECORD_SIZE];
    golden_serialize(r, buf);
    memcpy(sink->p + sink->n, buf, GOLDEN_RECORD_SIZE);
    sink->n += GOLDEN_RECORD_SIZE;
}

int main(int argc, char** argv)
{
    if (argc != 3 || (strcmp(argv[1], "gen") != 0 && strcmp(argv[1], "check") != 0)) {
        fprintf(stderr, "usage: %s gen|check <golden-file>\n", argv[0]);
        return 2;
    }

    /* 先在内存中枚举全部记录 */
    const size_t count = (2u * 5u) * (4u + 64u + 3u + 3u + 64u); /* 1380 */
    uint8_t* blob = (uint8_t*)malloc(count * GOLDEN_RECORD_SIZE);
    if (blob == NULL) { return 2; }
    struct { uint8_t* p; size_t n; } sink = { blob, 0 };
    const size_t got = golden_enumerate(emit_to_mem, &sink);
    if (got != count || sink.n != count * GOLDEN_RECORD_SIZE) {
        fprintf(stderr, "enumeration mismatch: %zu vs %zu\n", got, count);
        free(blob);
        return 2;
    }
    const uint64_t csum = golden_checksum(blob, sink.n);

    if (strcmp(argv[1], "gen") == 0) {
        FILE* f = fopen(argv[2], "wb");
        if (f == NULL) { perror("fopen"); free(blob); return 2; }
        /* 布局：magic(4) ver(4) count(4) recsize(4) checksum(8) = 24 字节头 */
        uint8_t hdr[24];
        memcpy(hdr, GOLDEN_MAGIC, 4);
        const uint32_t ver = GOLDEN_VERSION;
        const uint32_t cnt = (uint32_t)count;
        const uint32_t rs = GOLDEN_RECORD_SIZE;
        for (int i = 0; i < 4; ++i) { hdr[4 + i] = (uint8_t)(ver >> (8 * i)); }
        for (int i = 0; i < 4; ++i) { hdr[8 + i] = (uint8_t)(cnt >> (8 * i)); }
        for (int i = 0; i < 4; ++i) { hdr[12 + i] = (uint8_t)(rs >> (8 * i)); }
        for (int i = 0; i < 8; ++i) { hdr[16 + i] = (uint8_t)(csum >> (8 * i)); }
        fwrite(hdr, 1, 24, f);
        fwrite(blob, 1, sink.n, f);
        fclose(f);
        printf("generated %zu records (%zu bytes) -> %s\n", count, sink.n, argv[2]);
    } else {
        FILE* f = fopen(argv[2], "rb");
        if (f == NULL) { perror("fopen"); free(blob); return 1; }
        uint8_t hdr[24];
        if (fread(hdr, 1, 24, f) != 24 || memcmp(hdr, GOLDEN_MAGIC, 4) != 0) {
            fprintf(stderr, "golden header invalid\n"); fclose(f); free(blob); return 1;
        }
        uint32_t cnt = 0, rs = 0; uint64_t cs = 0;
        for (int i = 0; i < 4; ++i) { cnt |= (uint32_t)hdr[8 + i] << (8 * i); }
        for (int i = 0; i < 4; ++i) { rs |= (uint32_t)hdr[12 + i] << (8 * i); }
        for (int i = 0; i < 8; ++i) { cs |= (uint64_t)hdr[16 + i] << (8 * i); }
        if (cnt != count || rs != GOLDEN_RECORD_SIZE) {
            fprintf(stderr, "golden shape mismatch: count=%u recsize=%u\n", cnt, rs);
            fclose(f); free(blob); return 1;
        }
        uint8_t* file_blob = (uint8_t*)malloc(sink.n);
        if (file_blob == NULL || fread(file_blob, 1, sink.n, f) != sink.n) {
            fprintf(stderr, "golden body short\n"); fclose(f); free(blob);
            free(file_blob); return 1;
        }
        fclose(f);
        if (memcmp(file_blob, blob, sink.n) != 0) {
            for (size_t i = 0; i < sink.n; ++i) {
                if (file_blob[i] != blob[i]) {
                    fprintf(stderr,
                            "golden mismatch at byte %zu (record %zu, field offset %zu)\n",
                            i, i / GOLDEN_RECORD_SIZE, i % GOLDEN_RECORD_SIZE);
                    break;
                }
            }
            free(blob); free(file_blob); return 1;
        }
        if (golden_checksum(file_blob, sink.n) != cs) {
            fprintf(stderr, "golden checksum mismatch\n");
            free(blob); free(file_blob); return 1;
        }
        printf("golden OK: %zu records byte-exact, checksum match\n", count);
        free(file_blob);
    }
    free(blob);
    return 0;
}
