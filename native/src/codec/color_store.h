/* 颜色块重建存储（M10-2A）：反量化+IDCT+钳位+直写 plane 的单一定义点。
 *
 * 被 two 端共享，保证逐位一致：
 *  - codec.c 的 generic sink（tc_color_block_sink：函数指针路径，作 oracle/
 *    inspect/fuzz/差分参考）；
 *  - color_scan.h 的专用 scan-to-plane 热路径（生产解码：sink 调用在扫描
 *    循环内展开，消除每块间接调用/reader 状态换出）。
 *
 * 两路径的正确性契约由 test_slice_codec（直写 vs 标量参考重建）与
 * test_m10（整帧 direct on/off 差分）钉死。
 */
#ifndef TOPOS_INTERNAL_COLOR_STORE_H
#define TOPOS_INTERNAL_COLOR_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../simd/dispatch.h"
#include "../entropy/scan.h"
#include "../transform/sparse_inverse.h"
#include "../transform/transform.h"
#include "codec.h"

/* M4：重建存储 SSE2 内核（x86_64 恒可用；ARM 走标量回退） */
#if defined(__x86_64__) || defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#define TOPOS_SINK_SSE2 1
#endif

/* M4 对齐（aarch64）：重建存储 NEON 内核。vqmovun_s32 饱和域
 * [0,65535] ⊇ clamp 域 [0,max]（bd ≤ 16 全域）——无 SSE2 packs 的
 * 4095 门禁，批 4 宽位深同样精确。 */
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON__) || \
    (defined(__ARM_NEON) && defined(__ARM_NEON_FP))
#include <arm_neon.h>
#define TOPOS_SINK_NEON 1
#endif

/* M4：与 transform.c tc_round_shift32 相同的符号拆分四舍五入（闭式快路用；
 * 精确整数语义，无 UB 移位） */
static inline int32_t tc_store_round_shift32(int64_t v)
{
    if (v >= 0) {
        return (int32_t)((v + ((int64_t)1 << 31)) >> 32);
    }
    return -(int32_t)((-v + ((int64_t)1 << 31)) >> 32);
}

/* 批 4：位深感知系数钳位（12-bit 冻结值等比外推；|x'| ≤ mid−1） */
static inline int64_t tc_color_store_f_clamp(uint32_t mid, uint32_t max)
{
    if (max <= 4095u) { return (int64_t)TC_TRANSFORM_MAX_ABS_F; }
    const int64_t x12 = 2047;   /* 12-bit level shift 后最大幅度 */
    const int64_t x = (int64_t)max - (int64_t)mid; /* == 2^(bd-1)-1 */
    return ((int64_t)TC_TRANSFORM_MAX_ABS_F * x) / x12;
}

typedef struct tc_color_store_ctx {
    uint16_t* dst;
    size_t stride;      /* 调用方 plane 步长（元素计；>= vis_w） */
    const tc_quant_ctx* qctx; /* 每 slice 一次建表（阶段 9：块循环零 idiv） */
    uint32_t cols;
    uint32_t block_y0; /* 带首块行，块写入基偏移 */
    uint32_t vis_w;    /* 可见宽度（像素）——列 clip 上界 */
    uint32_t vis_h;    /* 可见高度（像素）——行 clip 上界 */
    uint32_t dst_w;    /* 目标输出宽；scaled=0 时等于 vis_w */
    uint32_t dst_h;    /* 目标输出高；scaled=0 时等于 vis_h */
    uint8_t scaled;    /* 目标尺寸解码：只求目标像素，不构造完整源平面 */
    uint8_t coefficient_limit; /* 扫描序保留上限；63 = 完整系数 */
    uint8_t reserved_scaled[2];
    uint32_t mid;      /* R4.1：重建 level shift 中值/上限按位深 */
    uint32_t max;
    /* 批 4：逆变换系数防御钳位按位深推导（|F|max ∝ max|x'|——
     * 12-bit 域 = TC_TRANSFORM_MAX_ABS_F；bd=16 → ≈2^28.4）。
     * 初始化方未设置时按 mid 推导（0 = 未设置哨兵）。 */
    int64_t f_clamp;
    uint32_t w0;       /* M4：DC-only 快路的 W[0] 权重（每 slice 一次取出） */
    tc_dequant_inverse_fn dinv; /* M10-1d：帧入口解析的融合内核 */
    tc_decode_stage_stats* stats; /* M0 观测（NULL = 关闭） */
    /* M10-0.2：反量化/IDCT 计时按块序号 1/64 确定性抽样（slice 级归一） */
    uint64_t sampled_ns;
    uint32_t sampled_blocks;
    uint64_t sampled_store_ns;
    uint32_t sampled_store_blocks;
    /* V3 intra 计时：与旧 scan 墙钟分开，确定性按块抽样。 */
    uint64_t sampled_entropy_ns;
    uint64_t sampled_recon_ns;
    uint32_t sampled_entropy_blocks;
    uint32_t sampled_recon_blocks;
} tc_color_store_ctx;

static inline void tc_color_store_profile_store_done(tc_color_store_ctx* c,
                                                      int sample,
                                                      uint64_t start_ns,
                                                      uint32_t blocks)
{
    if (sample != 0 && blocks != 0u) {
        c->sampled_store_ns += tc_profile_now_ns() - start_ns;
        c->sampled_store_blocks += blocks;
    }
}

static inline uint32_t tc_store_ceil_div_u64(uint64_t n, uint64_t d)
{
    return (uint32_t)((n + d - 1u) / d);
}

/* RD3-03：精确 1/3 档的三个源块相位。目标采样点采用与通用路径相同的
 * nearest 反向映射 sx=3*tx、sy=3*ty；按 8 像素块切开后，局部坐标只可能
 * 是下面三种形态。把相位固定下来后，6K→2K 热路径不再逐样本做除法。 */
typedef struct tc_color_store_third_phase {
    uint8_t count;
    uint8_t offsets[3];
} tc_color_store_third_phase;

static const tc_color_store_third_phase kTcColorStoreThirdPhases[3] = {
    {3u, {0u, 3u, 6u}},
    {2u, {2u, 5u, 0u}},
    {3u, {1u, 4u, 7u}}
};

static inline int tc_color_store_exact_third(const tc_color_store_ctx* c)
{
    return c->scaled != 0u && c->dst_w != 0u && c->dst_h != 0u &&
           (uint64_t)c->dst_w * 3ull == (uint64_t)c->vis_w &&
           (uint64_t)c->dst_h * 3ull == (uint64_t)c->vis_h;
}

/* 4K→2K 是编辑器代理/低分辨率时间线的主路径。源、目标均为偶数且
 * 严格 2:1 时，每个源 8×8 块固定对应目标 4×4 网格，避免通用缩放路径
 * 每块反复执行 64-bit 除法。奇数或非精确 2:1 几何继续走通用参考路径。 */
static inline int tc_color_store_exact_half(const tc_color_store_ctx* c)
{
    return c->scaled != 0u && c->dst_w != 0u && c->dst_h != 0u &&
           (uint64_t)c->dst_w * 2ull == (uint64_t)c->vis_w &&
           (uint64_t)c->dst_h * 2ull == (uint64_t)c->vis_h;
}

/* Exact-half geometry in its native 4×4 target layout.  Since vis_w/vis_h
 * are exactly twice dst_w/dst_h, the clipped edge extent remains even and the
 * target footprint is always a row-major prefix of a 4×4 block. */
static inline uint32_t tc_color_store_half_shape(const tc_color_store_ctx* c,
                                                 uint32_t bx, uint32_t by,
                                                 uint32_t* tx0, uint32_t* ty0,
                                                 uint32_t* nx, uint32_t* ny)
{
    if (c == NULL || tx0 == NULL || ty0 == NULL || nx == NULL || ny == NULL ||
        !tc_color_store_exact_half(c)) {
        return 0u;
    }
    const uint32_t px0 = bx * 8u;
    const uint32_t py0 = by * 8u;
    if (px0 >= c->vis_w || py0 >= c->vis_h) { return 0u; }
    const uint32_t cw = c->vis_w - px0 < 8u ? c->vis_w - px0 : 8u;
    const uint32_t ch = c->vis_h - py0 < 8u ? c->vis_h - py0 : 8u;
    *tx0 = px0 / 2u;
    *ty0 = py0 / 2u;
    *nx = cw / 2u;
    *ny = ch / 2u;
    if (*tx0 >= c->dst_w || *ty0 >= c->dst_h) { return 0u; }
    if (*nx > c->dst_w - *tx0) { *nx = c->dst_w - *tx0; }
    if (*ny > c->dst_h - *ty0) { *ny = c->dst_h - *ty0; }
    return (*nx) * (*ny);
}

/* 返回一个轴在 8 像素源块中的固定相位坐标，及对应的连续目标起点。
 * extent 用于裁掉右/下边缘的 coded padding；合法 offset 总是前缀。 */
static inline uint32_t tc_color_store_third_axis(uint32_t src0, uint32_t extent,
                                                 uint32_t vis, uint32_t dst_limit,
                                                 uint8_t local[3], uint32_t* dst0)
{
    if (src0 >= vis || dst0 == NULL) { return 0u; }
    const tc_color_store_third_phase* phase =
        &kTcColorStoreThirdPhases[src0 % 3u];
    const uint32_t base = (src0 + 2u) / 3u;
    if (base >= dst_limit) { return 0u; }
    uint32_t count = phase->count;
    if (count > dst_limit - base) { count = dst_limit - base; }
    while (count != 0u && phase->offsets[count - 1u] >= extent) { --count; }
    if (local != NULL) {
        for (uint32_t i = 0u; i < count; ++i) { local[i] = phase->offsets[i]; }
    }
    *dst0 = base;
    return count;
}

/* 精确 1/3 档的固定相位坐标生成。返回值为 nx*ny，坐标按目标行优先排列。 */
static inline uint32_t tc_color_store_third_coords(const tc_color_store_ctx* c,
                                                   uint32_t bx, uint32_t by,
                                                   uint8_t* xs, uint8_t* ys,
                                                   uint32_t* tx0, uint32_t* ty0,
                                                   uint32_t* nx_out, uint32_t* ny_out)
{
    if (c == NULL || xs == NULL || ys == NULL || tx0 == NULL || ty0 == NULL ||
        nx_out == NULL || ny_out == NULL || !tc_color_store_exact_third(c)) {
        return 0u;
    }
    const uint32_t px0 = bx * 8u;
    const uint32_t py0 = by * 8u;
    if (px0 >= c->vis_w || py0 >= c->vis_h) { return 0u; }
    const uint32_t cw = c->vis_w - px0 < 8u ? c->vis_w - px0 : 8u;
    const uint32_t ch = c->vis_h - py0 < 8u ? c->vis_h - py0 : 8u;
    uint8_t xlocal[3], ylocal[3];
    const uint32_t nx = tc_color_store_third_axis(px0, cw, c->vis_w, c->dst_w,
                                                  xlocal, tx0);
    const uint32_t ny = tc_color_store_third_axis(py0, ch, c->vis_h, c->dst_h,
                                                  ylocal, ty0);
    *nx_out = nx;
    *ny_out = ny;
    for (uint32_t iy = 0u; iy < ny; ++iy) {
        for (uint32_t ix = 0u; ix < nx; ++ix) {
            const uint32_t pos = iy * nx + ix;
            xs[pos] = xlocal[ix];
            ys[pos] = ylocal[iy];
        }
    }
    return nx * ny;
}

/* 返回一个源块在目标平面覆盖的目标坐标范围。preview 只在目标不大于
 * 源平面时启用，因此一个 8×8 源块最多对应 64 个目标样本。 */
static inline uint32_t tc_color_store_scaled_samples(const tc_color_store_ctx* c,
                                                     uint32_t bx, uint32_t by,
                                                     uint8_t* xs, uint8_t* ys,
                                                     uint32_t* txs, uint32_t* tys)
{
    if (c->scaled == 0u || c->dst_w == 0u || c->dst_h == 0u) { return 0u; }
    const uint32_t px0 = bx * 8u;
    const uint32_t py0 = by * 8u;
    if (px0 >= c->vis_w || py0 >= c->vis_h) { return 0u; }
    const uint32_t cw = c->vis_w - px0 < 8u ? c->vis_w - px0 : 8u;
    const uint32_t ch = c->vis_h - py0 < 8u ? c->vis_h - py0 : 8u;
    if (tc_color_store_exact_half(c)) {
        uint32_t tx0 = 0u, ty0 = 0u, nx = 0u, ny = 0u;
        const uint32_t n = tc_color_store_half_shape(c, bx, by,
                                                      &tx0, &ty0, &nx, &ny);
        for (uint32_t iy = 0u; iy < ny; ++iy) {
            for (uint32_t ix = 0u; ix < nx; ++ix) {
                const uint32_t i = iy * nx + ix;
                xs[i] = (uint8_t)(ix * 2u);
                ys[i] = (uint8_t)(iy * 2u);
                txs[i] = tx0 + ix;
                tys[i] = ty0 + iy;
            }
        }
        return n;
    }
    /* 6K→2K 的精确 1/3 档：源/目标几何可整除时，固定相位表生成连续
     * 目标坐标；这里仍填充通用接口的 tx/ty 数组，供非直写调用方复用。 */
    if (tc_color_store_exact_third(c)) {
        uint32_t tx0 = 0u, ty0 = 0u, nx = 0u, ny = 0u;
        const uint32_t n = tc_color_store_third_coords(c, bx, by, xs, ys,
                                                       &tx0, &ty0, &nx, &ny);
        uint32_t pos = 0u;
        for (uint32_t iy = 0u; iy < ny; ++iy) {
            for (uint32_t ix = 0u; ix < nx; ++ix) {
                if (pos >= n || pos >= 64u) { return pos; }
                txs[pos] = tx0 + ix;
                tys[pos] = ty0 + iy;
                ++pos;
            }
        }
        return pos;
    }
    uint32_t tx0 = tc_store_ceil_div_u64((uint64_t)px0 * c->dst_w, c->vis_w);
    uint32_t tx1 = tc_store_ceil_div_u64((uint64_t)(px0 + cw) * c->dst_w, c->vis_w);
    uint32_t ty0 = tc_store_ceil_div_u64((uint64_t)py0 * c->dst_h, c->vis_h);
    uint32_t ty1 = tc_store_ceil_div_u64((uint64_t)(py0 + ch) * c->dst_h, c->vis_h);
    if (tx1 > c->dst_w) { tx1 = c->dst_w; }
    if (ty1 > c->dst_h) { ty1 = c->dst_h; }
    uint32_t n = 0u;
    for (uint32_t ty = ty0; ty < ty1; ++ty) {
        const uint32_t sy = (uint32_t)(((uint64_t)ty * c->vis_h) / c->dst_h);
        for (uint32_t tx = tx0; tx < tx1; ++tx) {
            const uint32_t sx = (uint32_t)(((uint64_t)tx * c->vis_w) / c->dst_w);
            if (n >= 64u) { return n; }
            xs[n] = (uint8_t)(sx - px0);
            ys[n] = (uint8_t)(sy - py0);
            txs[n] = tx;
            tys[n] = ty;
            ++n;
        }
    }
    return n;
}

static inline void tc_color_store_scaled_u16(const tc_color_store_ctx* c,
                                             const uint8_t* xs, const uint8_t* ys,
                                             const uint32_t* txs, const uint32_t* tys,
                                             const uint16_t* values, uint32_t n)
{
    for (uint32_t i = 0u; i < n; ++i) {
        c->dst[(size_t)tys[i] * c->stride + txs[i]] = values[i];
        (void)xs;
        (void)ys;
    }
}

static inline void tc_color_store_scaled_i32(const tc_color_store_ctx* c,
                                            const uint8_t* xs, const uint8_t* ys,
                                            const uint32_t* txs, const uint32_t* tys,
                                            const int32_t* values, uint32_t n)
{
    for (uint32_t i = 0u; i < n; ++i) {
        int32_t v = values[i] + (int32_t)c->mid;
        if (v < 0) { v = 0; }
        else if (v > (int32_t)c->max) { v = (int32_t)c->max; }
        c->dst[(size_t)tys[i] * c->stride + txs[i]] = (uint16_t)v;
        (void)xs;
        (void)ys;
    }
}

static inline void tc_color_store_half_i32(const tc_color_store_ctx* c,
                                           uint32_t bx, uint32_t by,
                                           const int32_t* values)
{
    uint32_t tx0 = 0u, ty0 = 0u, nx = 0u, ny = 0u;
    const uint32_t n = tc_color_store_half_shape(c, bx, by, &tx0, &ty0, &nx, &ny);
    if (n == 0u || values == NULL) { return; }
    for (uint32_t iy = 0u; iy < ny; ++iy) {
        uint16_t* row = c->dst + (size_t)(ty0 + iy) * c->stride + tx0;
        for (uint32_t ix = 0u; ix < nx; ++ix) {
            int32_t v = values[iy * 4u + ix] + (int32_t)c->mid;
            if (v < 0) { v = 0; }
            else if (v > (int32_t)c->max) { v = (int32_t)c->max; }
            row[ix] = (uint16_t)v;
        }
    }
}

/* 精确 1/3 档连续写回：phase 表已经把每个源块对应的目标区域压缩成
 * [tx0, tx0+nx) × [ty0, ty0+ny)，所以这里只做连续行写和一次钳位。 */
static inline void tc_color_store_third_i32(const tc_color_store_ctx* c,
                                            uint32_t tx0, uint32_t ty0,
                                            uint32_t nx, uint32_t ny,
                                            const int32_t* values)
{
    if (c == NULL || values == NULL || nx == 0u || ny == 0u) { return; }
    for (uint32_t iy = 0u; iy < ny; ++iy) {
        uint16_t* row = c->dst + (size_t)(ty0 + iy) * c->stride + tx0;
        for (uint32_t ix = 0u; ix < nx; ++ix) {
            int32_t v = values[iy * nx + ix] + (int32_t)c->mid;
            if (v < 0) { v = 0; }
            else if (v > (int32_t)c->max) { v = (int32_t)c->max; }
            row[ix] = (uint16_t)v;
        }
    }
}

static inline void tc_color_store_third_u16(const tc_color_store_ctx* c,
                                            uint32_t tx0, uint32_t ty0,
                                            uint32_t nx, uint32_t ny,
                                            const uint16_t* values)
{
    if (c == NULL || values == NULL || nx == 0u || ny == 0u) { return; }
    for (uint32_t iy = 0u; iy < ny; ++iy) {
        uint16_t* row = c->dst + (size_t)(ty0 + iy) * c->stride + tx0;
        memcpy(row, values + iy * nx, (size_t)nx * sizeof(uint16_t));
    }
}

/* 1/3 热路径：从完整的 8×8 重建块按两个固定相位表直接取样并连续写行，
 * 不再构造 xs/ys、txs/tys 或 values 临时数组。 */
static inline void tc_color_store_third_from_xh(const tc_color_store_ctx* c,
                                                uint32_t bx, uint32_t by,
                                                const int32_t* xh)
{
    if (c == NULL || xh == NULL || !tc_color_store_exact_third(c)) { return; }
    const uint32_t px0 = bx * 8u;
    const uint32_t py0 = by * 8u;
    if (px0 >= c->vis_w || py0 >= c->vis_h) { return; }
    const uint32_t cw = c->vis_w - px0 < 8u ? c->vis_w - px0 : 8u;
    const uint32_t ch = c->vis_h - py0 < 8u ? c->vis_h - py0 : 8u;
    uint8_t xlocal[3], ylocal[3];
    uint32_t tx0 = 0u, ty0 = 0u;
    const uint32_t nx = tc_color_store_third_axis(px0, cw, c->vis_w, c->dst_w,
                                                  xlocal, &tx0);
    const uint32_t ny = tc_color_store_third_axis(py0, ch, c->vis_h, c->dst_h,
                                                  ylocal, &ty0);
    for (uint32_t iy = 0u; iy < ny; ++iy) {
        uint16_t* row = c->dst + (size_t)(ty0 + iy) * c->stride + tx0;
        for (uint32_t ix = 0u; ix < nx; ++ix) {
            int32_t v = xh[(uint32_t)ylocal[iy] * 8u + xlocal[ix]] +
                        (int32_t)c->mid;
            if (v < 0) { v = 0; }
            else if (v > (int32_t)c->max) { v = (int32_t)c->max; }
            row[ix] = (uint16_t)v;
        }
    }
}

static inline void tc_color_store_third_from_u16(const tc_color_store_ctx* c,
                                                 uint32_t bx, uint32_t by,
                                                 const uint16_t* pixels)
{
    if (c == NULL || pixels == NULL || !tc_color_store_exact_third(c)) { return; }
    const uint32_t px0 = bx * 8u;
    const uint32_t py0 = by * 8u;
    if (px0 >= c->vis_w || py0 >= c->vis_h) { return; }
    const uint32_t cw = c->vis_w - px0 < 8u ? c->vis_w - px0 : 8u;
    const uint32_t ch = c->vis_h - py0 < 8u ? c->vis_h - py0 : 8u;
    uint8_t xlocal[3], ylocal[3];
    uint32_t tx0 = 0u, ty0 = 0u;
    const uint32_t nx = tc_color_store_third_axis(px0, cw, c->vis_w, c->dst_w,
                                                  xlocal, &tx0);
    const uint32_t ny = tc_color_store_third_axis(py0, ch, c->vis_h, c->dst_h,
                                                  ylocal, &ty0);
    for (uint32_t iy = 0u; iy < ny; ++iy) {
        uint16_t* row = c->dst + (size_t)(ty0 + iy) * c->stride + tx0;
        for (uint32_t ix = 0u; ix < nx; ++ix) {
            row[ix] = pixels[(uint32_t)ylocal[iy] * 8u + xlocal[ix]];
        }
    }
}

/* scaled 存储防御路径：对已重建的完整 8×8 int32 块（未加 mid/未钳位），
 * 按目标采样网格抽取目标点后写目标平面。sparse/batch 存储入口共用——
 * 这两条路径天然面向完整平面直写；scaled 模式必须走采样写，否则按源
 * 坐标写目标尺寸平面会越界（ASan 差分复现钉死）。 */
static inline void tc_color_store_scaled_from_xh(const tc_color_store_ctx* c,
                                                 uint32_t bx, uint32_t by,
                                                 const int32_t* xh)
{
    if (tc_color_store_exact_half(c)) {
        uint32_t tx0 = 0u, ty0 = 0u, nx = 0u, ny = 0u;
        const uint32_t n = tc_color_store_half_shape(c, bx, by, &tx0, &ty0, &nx, &ny);
        int32_t values[16];
        for (uint32_t iy = 0u; iy < ny; ++iy) {
            for (uint32_t ix = 0u; ix < nx; ++ix) {
                values[iy * 4u + ix] = xh[(iy * 2u) * 8u + ix * 2u];
            }
        }
        if (n != 0u) { tc_color_store_half_i32(c, bx, by, values); }
        return;
    }
    if (tc_color_store_exact_third(c)) {
        tc_color_store_third_from_xh(c, bx, by, xh);
        return;
    }
    uint8_t xs[64], ys[64];
    uint32_t txs[64], tys[64];
    const uint32_t n = tc_color_store_scaled_samples(c, bx, by, xs, ys, txs, tys);
    int32_t values[64];
    for (uint32_t i = 0u; i < n; ++i) {
        values[i] = xh[(uint32_t)ys[i] * 8u + xs[i]];
    }
    tc_color_store_scaled_i32(c, xs, ys, txs, tys, values, n);
}

/* 重建像素 clip 存储（full/sparse 两路共享；xh 为逆变换输出 64 值）。
 * 语义与 M3~M10-1 的存储路径完全一致（可见区 clip + §7.7 钳位）。 */
static inline void tc_color_store_pixels_xy(const tc_color_store_ctx* c, uint32_t idx,
                                            uint32_t bx, uint32_t by,
                                            const int32_t* xh)
{
    (void)idx;
    uint32_t px0 = bx * 8u;
    uint32_t py0 = by * 8u;
    uint32_t cw = (c->vis_w - px0 < 8u) ? c->vis_w - px0 : 8u;
    uint32_t ch = (c->vis_h - py0 < 8u) ? c->vis_h - py0 : 8u;
    uint16_t* blk = c->dst + (size_t)py0 * c->stride + px0;
#if defined(TOPOS_SINK_SSE2)
    /* M4：整行 8 像素一次存储。bit-exact 论证：mid ≥ 0、max ≤ 4095 < 2^15；
     * packs_epi32 把 xh+mid 饱和到 [−32768, 32767] 后 clamp [0, max]——
     * 超上界的输入两侧都得 max、超下界两侧都得 0，域内逐位相同。
     * 批 4 门禁：bd≥13（max > 4095）时 packs 的 i16 饱和产出错值
     * （65535 → 32767）——必须回退标量（计划 §4 批 4 硬阻塞 2）。 */
    if (cw == 8u && c->max <= 4095u) {
        const __m128i vmid = _mm_set1_epi32((int32_t)c->mid);
        const __m128i vzero = _mm_setzero_si128();
        const __m128i vmax = _mm_set1_epi16((int16_t)c->max);
        for (uint32_t y = 0u; y < ch; ++y) {
            const int32_t* xr = xh + y * 8u;
            __m128i a = _mm_add_epi32(_mm_loadu_si128((const __m128i*)(xr)), vmid);
            __m128i b = _mm_add_epi32(_mm_loadu_si128((const __m128i*)(xr + 4)), vmid);
            __m128i p = _mm_packs_epi32(a, b);
            p = _mm_min_epi16(_mm_max_epi16(p, vzero), vmax);
            _mm_storeu_si128((__m128i*)(blk + (size_t)y * c->stride), p);
        }
        return;
    }
#endif
#if defined(TOPOS_SINK_NEON)
    /* M4 对齐：整行 8 像素一次存储。bit-exact：vqmovun_s32 饱和域
     * [0,65535] ⊇ clamp 域 [0,max]（bd ≤ 16 全域，无 SSE2 的 4095 门禁）
     * ——负值 → 0、超界经 vmin → max，与标量逐位相同。 */
    if (cw == 8u) {
        const int32x4_t vmid = vdupq_n_s32((int32_t)c->mid);
        const uint16x8_t vmax = vdupq_n_u16((uint16_t)c->max);
        for (uint32_t y = 0u; y < ch; ++y) {
            const int32_t* xr = xh + y * 8u;
            const int32x4_t a = vaddq_s32(vld1q_s32(xr), vmid);
            const int32x4_t b = vaddq_s32(vld1q_s32(xr + 4), vmid);
            uint16x8_t p = vcombine_u16(vqmovun_s32(a), vqmovun_s32(b));
            p = vminq_u16(p, vmax);
            vst1q_u16(blk + (size_t)y * c->stride, p);
        }
        return;
    }
#endif
    {
        for (uint32_t y = 0u; y < ch; ++y) {
            uint16_t* r = blk + (size_t)y * c->stride;
            const int32_t* xr = xh + y * 8u;
            for (uint32_t x = 0u; x < cw; ++x) {
                int32_t v = xr[x] + (int32_t)c->mid; /* §7.7 重建钳位 */
                if (v < 0) { v = 0; }
                else if (v > (int32_t)c->max) { v = (int32_t)c->max; }
                r[x] = (uint16_t)v;
            }
        }
    }
}

static inline void tc_color_store_pixels(const tc_color_store_ctx* c, uint32_t idx,
                                         const int32_t* xh)
{
    const uint32_t bx = idx % c->cols;
    const uint32_t by = c->block_y0 + idx / c->cols;
    tc_color_store_pixels_xy(c, idx, bx, by, xh);
}

/* 块重建直存（natural 序 q + 自然序非零行掩码）。
 * 语义与 M3~M10-1 的 color_block_sink 完全一致：
 *  - ac_rowmask==0（EOB 紧随 DC）→ 闭式常数填充；
 *  - 否则融合反量化+逆变换 + 可见区 clip 存储。 */
static inline int32_t tc_color_store_block_xy(tc_color_store_ctx* c, uint32_t idx,
                                              uint32_t bx, uint32_t by,
                                              const int32_t* q, uint32_t ac_rowmask)
{
    const int sample = c->stats != NULL && (idx & 63u) == 0u; /* M10-0.2：1/64 抽样计时 */
    uint64_t t0 = sample ? tc_profile_now_ns() : 0u;

    if (c->scaled != 0u) {
        uint32_t n = 0u;
        if (tc_color_store_exact_half(c)) {
            uint32_t tx0 = 0u, ty0 = 0u, nx = 0u, ny = 0u;
            n = tc_color_store_half_shape(c, bx, by, &tx0, &ty0, &nx, &ny);
            int32_t values[16];
            if (ac_rowmask == 0u) {
                int64_t f0 = (int64_t)q[0] * (int64_t)c->qctx->Q[0];
                {
                    const int64_t fclamp = c->f_clamp > 0
                        ? c->f_clamp : tc_color_store_f_clamp(c->mid, c->max);
                    if (f0 > fclamp) { f0 = fclamp; }
                    if (f0 < -fclamp) { f0 = -fclamp; }
                }
                int64_t acc = (int64_t)13 * (f0 * (int64_t)c->w0) * (int64_t)13;
                const int32_t v0 = tc_store_round_shift32(acc);
                for (uint32_t i = 0u; i < n; ++i) { values[i] = v0; }
            } else {
                const uint8_t limit = c->coefficient_limit == 0u ? 63u : c->coefficient_limit;
                tc_dequant_inverse_samples_half_limited(c->qctx, q, n, limit, values);
                if (c->stats != NULL) { c->stats->idct_samples += n; }
            }
            const uint64_t t_store = sample ? tc_profile_now_ns() : 0u;
            tc_color_store_half_i32(c, bx, by, values);
            tc_color_store_profile_store_done(c, sample, t_store, 1u);
        } else if (tc_color_store_exact_third(c)) {
            uint8_t xs[9], ys[9];
            uint32_t tx0 = 0u, ty0 = 0u, nx = 0u, ny = 0u;
            n = tc_color_store_third_coords(c, bx, by, xs, ys,
                                            &tx0, &ty0, &nx, &ny);
            int32_t values[9];
            if (ac_rowmask == 0u) {
                int64_t f0 = (int64_t)q[0] * (int64_t)c->qctx->Q[0];
                {
                    const int64_t fclamp = c->f_clamp > 0
                        ? c->f_clamp : tc_color_store_f_clamp(c->mid, c->max);
                    if (f0 > fclamp) { f0 = fclamp; }
                    if (f0 < -fclamp) { f0 = -fclamp; }
                }
                int64_t acc = (int64_t)13 * (f0 * (int64_t)c->w0) * (int64_t)13;
                const int32_t v0 = tc_store_round_shift32(acc);
                for (uint32_t i = 0u; i < n; ++i) { values[i] = v0; }
            } else {
                const uint8_t limit = c->coefficient_limit == 0u ? 63u : c->coefficient_limit;
                tc_dequant_inverse_samples_limited(c->qctx, q, xs, ys, n,
                                                   limit, values);
                if (c->stats != NULL) { c->stats->idct_samples += n; }
            }
            const uint64_t t_store = sample ? tc_profile_now_ns() : 0u;
            tc_color_store_third_i32(c, tx0, ty0, nx, ny, values);
            tc_color_store_profile_store_done(c, sample, t_store, 1u);
        } else {
            uint8_t xs[64], ys[64];
            uint32_t txs[64], tys[64];
            n = tc_color_store_scaled_samples(c, bx, by, xs, ys, txs, tys);
            const uint64_t t_store = sample ? tc_profile_now_ns() : 0u;
            if (ac_rowmask == 0u) {
                int64_t f0 = (int64_t)q[0] * (int64_t)c->qctx->Q[0];
                {
                    const int64_t fclamp = c->f_clamp > 0
                        ? c->f_clamp : tc_color_store_f_clamp(c->mid, c->max);
                    if (f0 > fclamp) { f0 = fclamp; }
                    if (f0 < -fclamp) { f0 = -fclamp; }
                }
                int64_t acc = (int64_t)13 * (f0 * (int64_t)c->w0) * (int64_t)13;
                const int32_t v0 = tc_store_round_shift32(acc);
                int32_t values[64];
                for (uint32_t i = 0u; i < n; ++i) { values[i] = v0; }
                tc_color_store_scaled_i32(c, xs, ys, txs, tys, values, n);
            } else {
                int32_t values[64];
                const uint8_t limit = c->coefficient_limit == 0u ? 63u : c->coefficient_limit;
                tc_dequant_inverse_samples_limited(c->qctx, q, xs, ys, n,
                                                   limit, values);
                tc_color_store_scaled_i32(c, xs, ys, txs, tys, values, n);
                if (c->stats != NULL) { c->stats->idct_samples += n; }
            }
            tc_color_store_profile_store_done(c, sample, t_store, 1u);
        }
        if (c->stats != NULL) {
            if (sample) {
                c->sampled_ns += tc_profile_now_ns() - t0;
                c->sampled_blocks++;
            }
            c->stats->blocks++;
            if (ac_rowmask == 0u) { c->stats->dc_only_blocks++; }
            else {
                for (uint32_t i = 1u; i < 64u; ++i) {
                    if (q[i] != 0) { c->stats->nonzero_ac++; }
                }
            }
        }
        return TC_OK;
    }

    /* M4 DC-only 快路：q[1..63] 全零 → 逆变换闭式为常数
     * x̂[y][x] = round(13·(c0·W0)·13 / 2^32)（仅 u=v=0 项存活；bit-exact
     * 等价 full IDCT 的 round(M[0][y]·B[0][x])）。真实 4K HQ 命中 ~16%。
     * 解码深化批次：ac_rowmask 由解码器随块给出（解出过 (run,level) 对即
     * 非 0），免 63 次非零扫描；域内 level m=0（系数 0）的病态流经 full
     * 路径，与闭式路径对该输入 bit-exact 等价。 */
    if (ac_rowmask == 0u) {
        int64_t f0 = (int64_t)q[0] * (int64_t)c->qctx->Q[0];
        {
            const int64_t fclamp = c->f_clamp > 0
                ? c->f_clamp : tc_color_store_f_clamp(c->mid, c->max);
            if (f0 > fclamp) { f0 = fclamp; }
            if (f0 < -fclamp) { f0 = -fclamp; }
        }
        int64_t acc = (int64_t)13 * (f0 * (int64_t)c->w0) * (int64_t)13;
        int32_t v0 = tc_store_round_shift32(acc) + (int32_t)c->mid;
        if (v0 < 0) { v0 = 0; }
        else if (v0 > (int32_t)c->max) { v0 = (int32_t)c->max; }
        uint16_t fill = (uint16_t)v0;
        uint32_t px0 = bx * 8u;
        uint32_t py0 = by * 8u;
        uint32_t cw = (c->vis_w - px0 < 8u) ? c->vis_w - px0 : 8u;
        uint32_t ch = (c->vis_h - py0 < 8u) ? c->vis_h - py0 : 8u;
        uint16_t* blk = c->dst + (size_t)py0 * c->stride + px0;
#if defined(TOPOS_SINK_NEON)
        if (cw == 8u) {
            const uint16x8_t vfill = vdupq_n_u16(fill);
            const uint64_t t_store = sample ? tc_profile_now_ns() : 0u;
            for (uint32_t y = 0u; y < ch; ++y) {
                vst1q_u16(blk + (size_t)y * c->stride, vfill);
            }
            tc_color_store_profile_store_done(c, sample, t_store, 1u);
            if (c->stats != NULL) {
                if (sample) {
                    c->sampled_ns += tc_profile_now_ns() - t0;
                    c->sampled_blocks++;
                }
                c->stats->blocks++;
                c->stats->dc_only_blocks++;
            }
            return TC_OK;
        }
#endif
        const uint64_t t_store = sample ? tc_profile_now_ns() : 0u;
        for (uint32_t y = 0u; y < ch; ++y) {
            uint16_t* r = blk + (size_t)y * c->stride;
            for (uint32_t x = 0u; x < cw; ++x) { r[x] = fill; }
        }
        tc_color_store_profile_store_done(c, sample, t_store, 1u);
        if (c->stats != NULL) {
            if (sample) {
                c->sampled_ns += tc_profile_now_ns() - t0;
                c->sampled_blocks++;
            }
            c->stats->blocks++;
            c->stats->dc_only_blocks++;
        }
        return TC_OK;
    }

    int32_t xh[64];
    c->dinv(c->qctx, q, xh, ac_rowmask);
    const uint64_t t_store = sample ? tc_profile_now_ns() : 0u;
    tc_color_store_pixels_xy(c, idx, bx, by, xh);
    tc_color_store_profile_store_done(c, sample, t_store, 1u);
    if (c->stats != NULL) {
        c->stats->idct_samples += 64u;
        if (sample) {
            c->sampled_ns += tc_profile_now_ns() - t0;
            c->sampled_blocks++;
        }
        c->stats->blocks++;
        uint32_t nz = 0u;
        for (uint32_t i = 1u; i < 64u; ++i) {
            if (q[i] != 0) { nz++; }
        }
        c->stats->nonzero_ac += nz;
    }
    return TC_OK;
}

static inline int32_t tc_color_store_block(tc_color_store_ctx* c, uint32_t idx,
                                           const int32_t* q, uint32_t ac_rowmask)
{
    const uint32_t bx = idx % c->cols;
    const uint32_t by = c->block_y0 + idx / c->cols;
    return tc_color_store_block_xy(c, idx, bx, by, q, ac_rowmask);
}

/* Generic/C1/V3 sink 也复用同一低频裁剪契约。专用 V1/V2 scan 路径在熵读取
 * 时就不写入高频项；这里作为其它实验语法的安全边界，避免 reduced API
 * 因码流版本不同而悄悄退化为完整逆变换。 */
static inline int32_t tc_color_store_block_reduced(tc_color_store_ctx* c, uint32_t idx,
                                                   const int32_t* q, uint32_t ac_rowmask)
{
    /* Generic/C1 scanner arrives here after consuming one DC symbol, one
     * initial run/EOB symbol, and two symbols for every decoded AC pair.  The
     * direct V1/V2 scanner accounts at the exact read sites instead. */
    if (c->stats != NULL) {
        c->stats->entropy_symbols += 2u;
        for (uint32_t nat = 1u; nat < 64u; ++nat) {
            if (q[nat] != 0) {
                c->stats->entropy_symbols += 2u;
                if (c->coefficient_limit != 0u &&
                    kTcZigzagInv[nat] > (uint32_t)c->coefficient_limit) {
                    c->stats->coefficients_skipped++;
                }
            }
        }
    }
    if (c->coefficient_limit == 0u || c->coefficient_limit >= 63u) {
        return tc_color_store_block(c, idx, q, ac_rowmask);
    }
    int32_t low[64];
    uint32_t rm = 1u;
    for (uint32_t nat = 0u; nat < 64u; ++nat) {
        const uint32_t scan_pos = kTcZigzagInv[nat];
        low[nat] = scan_pos <= (uint32_t)c->coefficient_limit ? q[nat] : 0;
        if (low[nat] != 0) { rm |= 1u << (nat >> 3); }
    }
    (void)ac_rowmask;
    return tc_color_store_block(c, idx, low, rm);
}

/* M10-2B：稀疏重建直存——扫描侧直接给出 (natural 序号, level) 对（免
 * q[64] 清零/散写/再扫描）。反量化按现行规则（int64 精确 + f_clamp
 * 位深钳位，与 tc_dequant_block_ctx / 融合内核同式），重建经 tc_sparse_inverse
 * （与稠密两遍逆变换 bit-exact，test_transform 差分钉死）。np ≥ 1。 */
static inline int32_t tc_color_store_block_sparse_xy(tc_color_store_ctx* c, uint32_t idx,
                                                     uint32_t bx, uint32_t by,
                                                     int32_t dc_val,
                                                     const int32_t* lvls, const uint8_t* nats,
                                                     uint32_t np)
{
    const int sample = c->stats != NULL && (idx & 63u) == 0u;
    uint64_t t0 = sample ? tc_profile_now_ns() : 0u;

    const uint32_t* Q = c->qctx->Q;
    int32_t p[TC_SPARSE_MAX_AC + 1];
    uint8_t nat[TC_SPARSE_MAX_AC + 1];
    {
        int64_t v0 = (int64_t)dc_val * (int64_t)Q[0];
        {
            const int64_t fclamp = c->f_clamp > 0
                ? c->f_clamp : tc_color_store_f_clamp(c->mid, c->max);
            if (v0 > fclamp) { v0 = fclamp; }
            else if (v0 < -fclamp) { v0 = -fclamp; }
        }
        p[0] = (int32_t)v0;
    }
    nat[0] = 0u;
    for (uint32_t i = 0u; i < np; ++i) {
        int64_t v = (int64_t)lvls[i] * (int64_t)Q[nats[i]];
        {
            const int64_t fclamp = c->f_clamp > 0
                ? c->f_clamp : tc_color_store_f_clamp(c->mid, c->max);
            if (v > fclamp) { v = fclamp; }
            else if (v < -fclamp) { v = -fclamp; }
        }
        p[i + 1u] = (int32_t)v;
        nat[i + 1u] = nats[i];
    }
    int32_t xh[64];
    (void)tc_sparse_inverse(p, nat, np + 1u, xh);
    if (c->stats != NULL) { c->stats->idct_samples += 64u; }
    const uint64_t t_store = sample ? tc_profile_now_ns() : 0u;
    if (c->scaled != 0u) {
        tc_color_store_scaled_from_xh(c, bx, by, xh);
    } else {
        tc_color_store_pixels_xy(c, idx, bx, by, xh);
    }
    tc_color_store_profile_store_done(c, sample, t_store, 1u);
    if (c->stats != NULL) {
        if (sample) {
            c->sampled_ns += tc_profile_now_ns() - t0;
            c->sampled_blocks++;
        }
        c->stats->blocks++;
        c->stats->nonzero_ac += np;
    }
    return TC_OK;
}

static inline int32_t tc_color_store_block_sparse(tc_color_store_ctx* c, uint32_t idx,
                                                  int32_t dc_val,
                                                  const int32_t* lvls, const uint8_t* nats,
                                                  uint32_t np)
{
    const uint32_t bx = idx % c->cols;
    const uint32_t by = c->block_y0 + idx / c->cols;
    return tc_color_store_block_sparse_xy(c, idx, bx, by, dc_val, lvls, nats, np);
}

/* M10-2C：四块批量提交——qsoa[64][4] 已按 pending 块填充（int32 值符号
 * 扩展入 i64 lane），n ∈ [1,4]；n<4 时尾块（lane n..3 为上一批残留值，
 * 运算有界且结果被丢弃）。变换一次完成，逐块像素存储与单块路径同源。 */
static inline int32_t tc_color_batch_commit(tc_color_store_ctx* c,
                                            const int64_t* qsoa, int32_t* xh4,
                                            const uint32_t* batch_idx,
                                            const uint32_t* batch_rm, int n)
{
    const int sample = c->stats != NULL && (batch_idx[0] & 63u) == 0u;
    uint64_t t0 = sample ? tc_profile_now_ns() : 0u;
    uint32_t rm_union = 0u;
    for (int k = 0; k < n; ++k) { rm_union |= batch_rm[k]; }
    tc_simd_dequant_inverse_8x8x4(c->qctx, qsoa, xh4, rm_union);
    if (c->stats != NULL) { c->stats->idct_samples += (uint64_t)n * 64u; }
    const uint64_t t_store = sample ? tc_profile_now_ns() : 0u;
    for (int k = 0; k < n; ++k) {
        if (c->scaled != 0u) {
            const uint32_t bidx = batch_idx[k];
            tc_color_store_scaled_from_xh(c, bidx % c->cols,
                                          c->block_y0 + bidx / c->cols,
                                          xh4 + (size_t)k * 64u);
        } else {
            tc_color_store_pixels(c, batch_idx[k], xh4 + (size_t)k * 64u);
        }
    }
    tc_color_store_profile_store_done(c, sample, t_store, (uint32_t)n);
    if (c->stats != NULL) {
        if (sample) {
            c->sampled_ns += tc_profile_now_ns() - t0;
            c->sampled_blocks += (uint32_t)n; /* 样本时长覆盖 n 块 */
        }
        for (int k = 0; k < n; ++k) {
            c->stats->blocks++;
            uint32_t nz = 0u;
            for (uint32_t i = 1u; i < 64u; ++i) {
                if (qsoa[i * 4u + (uint32_t)k] != 0) { nz++; }
            }
            c->stats->nonzero_ac += nz;
        }
    }
    return TC_OK;
}

/* 批量缓冲冲刷（行尾/切片结束/错误路径）：不足 4 块逐块回退单块内核 */
static inline int32_t tc_color_batch_flush(tc_color_store_ctx* c,
                                           const int64_t* qsoa, int32_t* xh4,
                                           const uint32_t* batch_idx,
                                           const uint32_t* batch_rm, int* nb)
{
    int rc = TC_OK;
    for (int k = 0; k < *nb; ++k) {
        int32_t q[64];
        for (uint32_t i = 0u; i < 64u; ++i) { q[i] = (int32_t)qsoa[i * 4u + (uint32_t)k]; }
        rc = tc_color_store_block(c, batch_idx[k], q, batch_rm[k]);
        if (rc != TC_OK) { break; }
    }
    *nb = 0;
    (void)xh4;
    return rc;
}

#endif /* TOPOS_INTERNAL_COLOR_STORE_H */
