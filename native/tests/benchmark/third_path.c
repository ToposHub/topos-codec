/* RD3-03：固定 1/3 相位表与通用 sampled 坐标/写回的微基准。 */
#include "codec/color_store.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    kSourceWidth = 21,
    kSourceHeight = 15,
    kTargetWidth = 7,
    kTargetHeight = 5,
    kStride = 11,
    kBlockCols = 3,
    kBlockRows = 2,
    kDefaultIterations = 2000000
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    (void)timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* RD3-03 前的通用参考：每个目标样本都做比例除法，并通过散点数组写回。 */
static void generic_sample_store(const tc_color_store_ctx* c, uint32_t bx, uint32_t by,
                                 const int32_t* xh)
{
    const uint32_t px0 = bx * 8u;
    const uint32_t py0 = by * 8u;
    if (px0 >= c->vis_w || py0 >= c->vis_h) { return; }
    const uint32_t cw = c->vis_w - px0 < 8u ? c->vis_w - px0 : 8u;
    const uint32_t ch = c->vis_h - py0 < 8u ? c->vis_h - py0 : 8u;
    uint32_t tx0 = (uint32_t)(((uint64_t)px0 * c->dst_w + c->vis_w - 1u) / c->vis_w);
    uint32_t tx1 = (uint32_t)(((uint64_t)(px0 + cw) * c->dst_w + c->vis_w - 1u) /
                              c->vis_w);
    uint32_t ty0 = (uint32_t)(((uint64_t)py0 * c->dst_h + c->vis_h - 1u) / c->vis_h);
    uint32_t ty1 = (uint32_t)(((uint64_t)(py0 + ch) * c->dst_h + c->vis_h - 1u) /
                              c->vis_h);
    if (tx1 > c->dst_w) { tx1 = c->dst_w; }
    if (ty1 > c->dst_h) { ty1 = c->dst_h; }
    uint8_t xs[64], ys[64];
    uint32_t txs[64], tys[64];
    int32_t values[64];
    uint32_t n = 0u;
    for (uint32_t ty = ty0; ty < ty1; ++ty) {
        const uint32_t sy = (uint32_t)(((uint64_t)ty * c->vis_h) / c->dst_h);
        for (uint32_t tx = tx0; tx < tx1; ++tx) {
            const uint32_t sx = (uint32_t)(((uint64_t)tx * c->vis_w) / c->dst_w);
            if (n >= 64u) { return; }
            xs[n] = (uint8_t)(sx - px0);
            ys[n] = (uint8_t)(sy - py0);
            txs[n] = tx;
            tys[n] = ty;
            values[n] = xh[(uint32_t)ys[n] * 8u + xs[n]];
            ++n;
        }
    }
    tc_color_store_scaled_i32(c, xs, ys, txs, tys, values, n);
}

static void fill_block(int32_t xh[64], uint32_t bx, uint32_t by)
{
    for (uint32_t y = 0u; y < 8u; ++y) {
        for (uint32_t x = 0u; x < 8u; ++x) {
            xh[y * 8u + x] = (int32_t)((by * 8u + y) * 100u + bx * 8u + x);
        }
    }
}

static uint64_t checksum(const uint16_t* dst)
{
    uint64_t sum = 0u;
    for (uint32_t y = 0u; y < kTargetHeight; ++y) {
        for (uint32_t x = 0u; x < kTargetWidth; ++x) {
            sum = sum * 131u + dst[y * kStride + x];
        }
    }
    return sum;
}

static void run_fixed(const tc_color_store_ctx* c, uint16_t* dst, int iterations,
                      int32_t blocks[kBlockRows][kBlockCols][64])
{
    tc_color_store_ctx local = *c;
    local.dst = dst;
    for (int rep = 0; rep < iterations; ++rep) {
        for (uint32_t by = 0u; by < kBlockRows; ++by) {
            for (uint32_t bx = 0u; bx < kBlockCols; ++bx) {
                tc_color_store_scaled_from_xh(&local, bx, by, blocks[by][bx]);
            }
        }
    }
}

static void run_generic(const tc_color_store_ctx* c, uint16_t* dst, int iterations,
                        int32_t blocks[kBlockRows][kBlockCols][64])
{
    tc_color_store_ctx local = *c;
    local.dst = dst;
    for (int rep = 0; rep < iterations; ++rep) {
        for (uint32_t by = 0u; by < kBlockRows; ++by) {
            for (uint32_t bx = 0u; bx < kBlockCols; ++bx) {
                generic_sample_store(&local, bx, by, blocks[by][bx]);
            }
        }
    }
}

int main(int argc, char** argv)
{
    int iterations = kDefaultIterations;
    if (argc > 1) {
        iterations = atoi(argv[1]);
        if (iterations <= 0) { return 2; }
    }
    int32_t blocks[kBlockRows][kBlockCols][64];
    for (uint32_t by = 0u; by < kBlockRows; ++by) {
        for (uint32_t bx = 0u; bx < kBlockCols; ++bx) {
            fill_block(blocks[by][bx], bx, by);
        }
    }
    uint16_t fixed_dst[kTargetHeight * kStride];
    uint16_t generic_dst[kTargetHeight * kStride];
    tc_color_store_ctx c;
    memset(&c, 0, sizeof(c));
    c.stride = kStride;
    c.vis_w = kSourceWidth;
    c.vis_h = kSourceHeight;
    c.dst_w = kTargetWidth;
    c.dst_h = kTargetHeight;
    c.scaled = 1u;
    c.max = 65535u;
    if (!tc_color_store_exact_third(&c)) { return 3; }

    memset(fixed_dst, 0, sizeof(fixed_dst));
    memset(generic_dst, 0, sizeof(generic_dst));
    run_fixed(&c, fixed_dst, 1, blocks);
    run_generic(&c, generic_dst, 1, blocks);
    if (memcmp(fixed_dst, generic_dst, sizeof(fixed_dst)) != 0) {
        fprintf(stderr, "fixed/generic output mismatch\n");
        return 4;
    }

    run_fixed(&c, fixed_dst, 1000, blocks);
    run_generic(&c, generic_dst, 1000, blocks);
    const uint64_t fixed_start = now_ns();
    run_fixed(&c, fixed_dst, iterations, blocks);
    const uint64_t fixed_ns = now_ns() - fixed_start;
    const uint64_t generic_start = now_ns();
    run_generic(&c, generic_dst, iterations, blocks);
    const uint64_t generic_ns = now_ns() - generic_start;
    volatile uint64_t sink = checksum(fixed_dst) ^ checksum(generic_dst);
    (void)sink;
    printf("{\"iterations\":%d,\"blocks_per_iteration\":%d,"
           "\"fixed_ns\":%llu,\"generic_ns\":%llu,\"fixed_checksum\":%llu,"
           "\"generic_checksum\":%llu}\n",
           iterations, kBlockRows * kBlockCols,
           (unsigned long long)fixed_ns, (unsigned long long)generic_ns,
           (unsigned long long)checksum(fixed_dst),
           (unsigned long long)checksum(generic_dst));
    return 0;
}
