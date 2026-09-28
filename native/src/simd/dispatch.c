#include "simd/dispatch.h"

#include <stdatomic.h>
#include <string.h>

#include "../common/cpudetect.h"
#include "../entropy/scan.h"
#include "../transform/quant.h"
#include "topos_codec.h"
#include "transform/transform.h"

/* ---- 后端注册（transform_avx2.c / transform_neon.c 编译期按架构提供） ---- */

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__AVX2__)
/* target-attribute（gcc/clang）或整 TU /arch:AVX2（MSVC）编译的 AVX2 内核；
 * 运行时由 cpuid 判定是否调用（无 AVX2 CPU 不受影响） */
void tc_transform_forward_8x8_avx2(const int16_t x[64], int32_t F[64]);
void tc_transform_inverse_8x8_avx2(const int32_t coef[64], int32_t xhat[64]);
void tc_transform_inverse_8x8_samples_avx2(const int32_t coef[64], const uint8_t* xs,
                                           const uint8_t* ys, uint32_t count, int32_t* out);
void tc_transform_inverse_8x8_samples_avx2_limited(const int32_t coef[64],
                                                   const uint8_t* xs, const uint8_t* ys,
                                                   uint32_t count, uint8_t max_scan_pos,
                                                   int32_t* out);
void tc_transform_inverse_8x8_samples_half_avx2_limited(const int32_t coef[64],
                                                        uint32_t count, uint8_t max_scan_pos,
                                                        int32_t* out);
void tc_dequant_inverse_samples_half_avx2_limited(const struct tc_quant_ctx* ctx,
                                                  const int32_t q[64], uint32_t count,
                                                  uint8_t max_scan_pos, int32_t* out);
void tc_dequant_inverse_8x8_avx2(const struct tc_quant_ctx* ctx, const int32_t q[64],
                                 int32_t xhat[64], uint32_t ac_rowmask);
/* M10-2C：四块 SoA 批量内核（qsoa[64][4] 为 int32 值符号扩展入 i64 lane） */
void tc_dequant_inverse_8x8x4_avx2(const struct tc_quant_ctx* ctx, const int64_t* qsoa,
                                   int32_t* xh4, uint32_t rm_union);
/* M10-6.3B：gather 型量化内核（zigzag 直写 + 非零掩码；位级语义同 scalar） */
void tc_quant_block_zigzag_avx2(const struct tc_quant_ctx* ctx, const int32_t* F,
                                int32_t* q_out, uint64_t* nz_zz);
/* P-速②：u16 平面行直载 forward 内核 */
void tc_transform_forward_8x8_rows_avx2(const uint16_t* src, size_t stride,
                                        int32_t mid, int32_t F[64]);
/* P-速⑥：自然序量化内核（免 gather；掩码为自然序） */
void tc_quant_block_nat_avx2(const struct tc_quant_ctx* ctx, const int32_t* F,
                             int32_t* q_out, uint64_t* nz_nat);
/* 批 4 阶段 2：i32 输入前向内核（bd≤16 全域与 scalar bit-exact） */
void tc_transform_forward_8x8_i32_avx2(const int32_t x[64], int32_t F[64]);
/* M10-1d：AVX2 内核表（kM32/kMT32/kWMb）懒生成 ensure 的公开入口——
 * 调用方在 frame/slice 入口一次完成，块循环不再触碰 ready atomic */
void tc_transform_avx2_tables_ensure_public(void);
#define TOPOS_HAVE_AVX2_KERNEL 1
#else
#define TOPOS_HAVE_AVX2_KERNEL 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON__) || \
    (defined(__ARM_NEON) && defined(__ARM_NEON_FP))
void tc_transform_forward_8x8_neon(const int16_t x[64], int32_t F[64]);
void tc_transform_inverse_8x8_samples_neon(const int32_t coef[64], const uint8_t* xs,
                                           const uint8_t* ys, uint32_t count, int32_t* out);
void tc_transform_inverse_8x8_samples_neon_limited(const int32_t coef[64],
                                                   const uint8_t* xs, const uint8_t* ys,
                                                   uint32_t count, uint8_t max_scan_pos,
                                                   int32_t* out);
/* 批 4 阶段 2：i32 输入前向内核（bd≤16 全域与 scalar bit-exact） */
void tc_transform_forward_8x8_i32_neon(const int32_t x[64], int32_t F[64]);
/* 解码深化：融合反量化+逆变换（含行稀疏跳过；ARM NEON 对齐，M10-8） */
void tc_dequant_inverse_8x8_neon(const struct tc_quant_ctx* ctx, const int32_t q[64],
                                 int32_t xhat[64], uint32_t ac_rowmask);
/* M10-6.3B：gather 型量化内核（zigzag 直写 + 非零掩码；位级语义同 scalar） */
void tc_quant_block_zigzag_neon(const struct tc_quant_ctx* ctx, const int32_t* F,
                                int32_t* q_out, uint64_t* nz_zz);
/* P-速⑥：自然序量化内核（免 gather；掩码为自然序） */
void tc_quant_block_nat_neon(const struct tc_quant_ctx* ctx, const int32_t* F,
                             int32_t* q_out, uint64_t* nz_nat);
/* M10-1d：NEON 内核表懒生成 ensure 的公开入口——resolve 时一次完成 */
void tc_transform_neon_tables_ensure_public(void);
#define TOPOS_HAVE_NEON_KERNEL 1
#else
#define TOPOS_HAVE_NEON_KERNEL 0
#endif

/* ---- 分发状态：一次初始化的函数指针（force 模式可重发布） ----
 * init 状态：0 未初始化 / 1 初始化中 / 2 就绪（发布先序由 seq_cst 保证） */

static void (* volatile s_forward)(const int16_t*, int32_t*) = NULL;
static void (* volatile s_forward_i32)(const int32_t*, int32_t*) = NULL;
static void (* volatile s_inverse)(const int32_t*, int32_t*) = NULL;
static void (* volatile s_inverse_samples)(const int32_t*, const uint8_t*, const uint8_t*,
                                           uint32_t, int32_t*) = NULL;
static void (* volatile s_inverse_samples_limited)(const int32_t*, const uint8_t*,
                                                   const uint8_t*, uint32_t, uint8_t,
                                                   int32_t*) = NULL;
static void (* volatile s_inverse_samples_half_limited)(const int32_t*, uint32_t, uint8_t,
                                                        int32_t*) = NULL;
static void (* volatile s_dequant_samples_half_limited)(const struct tc_quant_ctx*,
                                                        const int32_t*, uint32_t, uint8_t,
                                                        int32_t*) = NULL;
static void (* volatile s_dequant_inverse)(const struct tc_quant_ctx*, const int32_t*,
                                           int32_t*, uint32_t) = NULL;
static void (* volatile s_quant_zz)(const struct tc_quant_ctx*, const int32_t*,
                                    int32_t*, uint64_t*) = NULL;
static atomic_int s_mode = TC_SIMD_AUTO; /* dev 强制模式 */
static atomic_int s_init_state = 0;
static const char* s_backend = "scalar";

int tc_simd_have_avx2(void)
{
#if TOPOS_HAVE_AVX2_KERNEL
    return 1;
#else
    return 0;
#endif
}

int tc_simd_have_neon(void)
{
#if TOPOS_HAVE_NEON_KERNEL
    return 1;
#else
    return 0;
#endif
}

static void simd_select(int mode)
{
    void (*fwd)(const int16_t*, int32_t*) = NULL;
    void (*fwd_i32)(const int32_t*, int32_t*) = NULL;
    void (*inv)(const int32_t*, int32_t*) = NULL;
    void (*inv_samples)(const int32_t*, const uint8_t*, const uint8_t*, uint32_t, int32_t*) = NULL;
    void (*inv_samples_limited)(const int32_t*, const uint8_t*, const uint8_t*,
                                uint32_t, uint8_t, int32_t*) = NULL;
    void (*inv_samples_half_limited)(const int32_t*, uint32_t, uint8_t, int32_t*) = NULL;
    void (*dequant_samples_half_limited)(const struct tc_quant_ctx*, const int32_t*,
                                         uint32_t, uint8_t, int32_t*) = NULL;
    void (*dinv)(const struct tc_quant_ctx*, const int32_t*, int32_t*, uint32_t) = NULL;
    void (*qzz)(const struct tc_quant_ctx*, const int32_t*, int32_t*, uint64_t*) = NULL;
    const char* name = "scalar";

    uint32_t flags = tc_internal_cpu_flags();
#if TOPOS_HAVE_AVX2_KERNEL
    if (mode != TC_SIMD_SCALAR && (flags & TOPOS_CPU_X86_AVX2) != 0u) {
        fwd = &tc_transform_forward_8x8_avx2;
        fwd_i32 = &tc_transform_forward_8x8_i32_avx2;
        inv = &tc_transform_inverse_8x8_avx2; /* M4：逆变换 AVX2（bit-exact） */
        inv_samples = &tc_transform_inverse_8x8_samples_avx2;
        inv_samples_limited = &tc_transform_inverse_8x8_samples_avx2_limited;
        inv_samples_half_limited = &tc_transform_inverse_8x8_samples_half_avx2_limited;
        dequant_samples_half_limited = &tc_dequant_inverse_samples_half_avx2_limited;
        dinv = &tc_dequant_inverse_8x8_avx2;  /* 解码深化：融合反量化+逆变换 */
        qzz = &tc_quant_block_zigzag_avx2;    /* M10-6.3B：gather 型量化 */
        name = "avx2";
    }
#endif
#if TOPOS_HAVE_NEON_KERNEL
    if (mode != TC_SIMD_SCALAR && (flags & TOPOS_CPU_ARM_NEON) != 0u) {
        fwd = &tc_transform_forward_8x8_neon;
        fwd_i32 = &tc_transform_forward_8x8_i32_neon;
        /* 全块逆变换保持标量（ADR-C009 决策）；解码热路径走融合
         * dinv（反量化+逆变换一体，含行稀疏跳过——M10-8 ARM 对齐） */
        inv_samples = &tc_transform_inverse_8x8_samples_neon;
        inv_samples_limited = &tc_transform_inverse_8x8_samples_neon_limited;
        dinv = &tc_dequant_inverse_8x8_neon;
        qzz = &tc_quant_block_zigzag_neon;  /* M10-6.3B：gather 型量化 */
        name = "neon";
    }
#endif
    (void)flags;
    if (fwd == NULL) { fwd = &tc_transform_forward_8x8_scalar; }
    if (fwd_i32 == NULL) { fwd_i32 = &tc_transform_forward_8x8_i32_scalar; }
    if (inv == NULL) { inv = &tc_transform_inverse_8x8_scalar; }
    if (inv_samples == NULL) { inv_samples = &tc_transform_inverse_8x8_samples_scalar; }
    if (inv_samples_limited == NULL) {
        inv_samples_limited = &tc_transform_inverse_8x8_samples_scalar_limited;
    }

    /* 发布：先写函数指针，再以 seq_cst store 建立 happens-before */
    s_forward = fwd;
    s_forward_i32 = fwd_i32;
    s_inverse = inv;
    s_inverse_samples = inv_samples;
    s_inverse_samples_limited = inv_samples_limited;
    s_inverse_samples_half_limited = inv_samples_half_limited;
    s_dequant_samples_half_limited = dequant_samples_half_limited;
    s_dequant_inverse = dinv;
    s_quant_zz = qzz;
    s_backend = name;
    atomic_store(&s_init_state, 2);
}

static void simd_ensure(void)
{
    if (atomic_load(&s_init_state) == 2) { return; }
    int expected = 0;
    if (atomic_compare_exchange_strong(&s_init_state, &expected, 1)) {
        simd_select(atomic_load(&s_mode));
        return;
    }
    while (atomic_load(&s_init_state) != 2) { /* 等待首初始化发布 */ }
}

void tc_dev_set_simd_mode(int32_t mode)
{
    if (mode != TC_SIMD_AUTO && mode != TC_SIMD_SCALAR && mode != TC_SIMD_FORCE) { return; }
    atomic_store(&s_mode, mode);
    simd_select(mode); /* 立即重选并发布 */
}

int32_t tc_dev_simd_mode(void)
{
    return atomic_load(&s_mode);
}

const char* tc_dev_simd_backend(void)
{
    simd_ensure();
    return s_backend;
}

void tc_simd_forward_8x8(const int16_t x[64], int32_t F[64])
{
    simd_ensure();
    s_forward(x, F);
}

/* 批 4 阶段 2：i32 前向分发（bd≥13；AVX2/NEON 后端与 scalar 全域一致） */
void tc_simd_forward_8x8_i32(const int32_t x[64], int32_t F[64])
{
    simd_ensure();
    s_forward_i32(x, F);
}

void tc_simd_inverse_8x8(const int32_t coef[64], int32_t xhat[64])
{
    simd_ensure();
    s_inverse(coef, xhat);
}

void tc_simd_inverse_8x8_samples(const int32_t coef[64], const uint8_t* xs,
                                  const uint8_t* ys, uint32_t count, int32_t* out)
{
    simd_ensure();
    s_inverse_samples(coef, xs, ys, count, out);
}

void tc_simd_inverse_8x8_samples_limited(const int32_t coef[64],
                                         const uint8_t* xs, const uint8_t* ys,
                                         uint32_t count, uint8_t max_scan_pos,
                                         int32_t* out)
{
    simd_ensure();
    s_inverse_samples_limited(coef, xs, ys, count, max_scan_pos, out);
}

void tc_simd_inverse_8x8_samples_half_limited(const int32_t coef[64],
                                              uint32_t count, uint8_t max_scan_pos,
                                              int32_t* out)
{
    /* Exact-half callers only pass a 4×4 block, with a row-major prefix on
     * clipped edges.  Keep the fallback arrays local to the dispatch layer so
     * scalar and SIMD backends share exactly the same coordinate contract. */
    static const uint8_t kHalfXs[16] = {
        0u, 2u, 4u, 6u, 0u, 2u, 4u, 6u,
        0u, 2u, 4u, 6u, 0u, 2u, 4u, 6u
    };
    static const uint8_t kHalfYs[16] = {
        0u, 0u, 0u, 0u, 2u, 2u, 2u, 2u,
        4u, 4u, 4u, 4u, 6u, 6u, 6u, 6u
    };
    if (count == 0u || coef == NULL || out == NULL) { return; }
    if (count > 16u) { count = 16u; }
    simd_ensure();
    if (s_inverse_samples_half_limited != NULL) {
        s_inverse_samples_half_limited(coef, count, max_scan_pos, out);
        return;
    }
    s_inverse_samples_limited(coef, kHalfXs, kHalfYs, count, max_scan_pos, out);
}

void tc_simd_dequant_inverse_samples_half_limited(const struct tc_quant_ctx* ctx,
                                                 const int32_t q[64], uint32_t count,
                                                 uint8_t max_scan_pos, int32_t* out)
{
    static const uint8_t kHalfXs[16] = {
        0u, 2u, 4u, 6u, 0u, 2u, 4u, 6u,
        0u, 2u, 4u, 6u, 0u, 2u, 4u, 6u
    };
    static const uint8_t kHalfYs[16] = {
        0u, 0u, 0u, 0u, 2u, 2u, 2u, 2u,
        4u, 4u, 4u, 4u, 6u, 6u, 6u, 6u
    };
    if (ctx == NULL || q == NULL || out == NULL || count == 0u) { return; }
    if (count > 16u) { count = 16u; }
    simd_ensure();
    if (s_dequant_samples_half_limited != NULL) {
        s_dequant_samples_half_limited(ctx, q, count, max_scan_pos, out);
        return;
    }

    int32_t Fp[64];
    memset(Fp, 0, sizeof(Fp));
    const int64_t fclamp = ctx->f_clamp > 0 ? ctx->f_clamp : (int64_t)TC_TRANSFORM_MAX_ABS_F;
    const uint32_t limit = max_scan_pos > 63u ? 63u : (uint32_t)max_scan_pos;
    for (uint32_t scan_pos = 0u; scan_pos <= limit; ++scan_pos) {
        const uint32_t natural = kTcZigzag[scan_pos];
        int64_t v = (int64_t)q[natural] * (int64_t)ctx->Q[natural];
        if (v > fclamp) { v = fclamp; }
        else if (v < -fclamp) { v = -fclamp; }
        Fp[natural] = (int32_t)v;
    }
    s_inverse_samples_limited(Fp, kHalfXs, kHalfYs, count, max_scan_pos, out);
}

void tc_simd_dequant_inverse_8x8(const struct tc_quant_ctx* ctx, const int32_t q[64],
                                 int32_t xhat[64], uint32_t ac_rowmask)
{
    simd_ensure();
    if (s_dequant_inverse != NULL) {
        s_dequant_inverse(ctx, q, xhat, ac_rowmask);
        return;
    }
    /* 无融合后端（标量强制/NEON）：compose 回退，语义一致（掩码仅是
     * 跳零行的优化提示，全行处理与跳零行数学等价） */
    int32_t Fp[64];
    tc_dequant_block_ctx(ctx, q, Fp);
    s_inverse(Fp, xhat);
}

/* M10-1d：compose 回退的稳定包装（经解析指针调用时无融合后端的目标） */
static void dequant_inverse_compose(const struct tc_quant_ctx* ctx, const int32_t q[64],
                                    int32_t xhat[64], uint32_t ac_rowmask)
{
    (void)ac_rowmask; /* 掩码为跳零优化提示，compose 全行处理等价 */
    int32_t Fp[64];
    tc_dequant_block_ctx(ctx, q, Fp);
    s_inverse(Fp, xhat);
}

/* 批 4：宽位深稳定 compose（bd≥13 专用）——s_inverse 可能是 AVX2 内核
 * （B 16-bit 分解域 |B|<2^46 仅 ±2^25 输入成立），此处恒走 scalar 逆变换
 * （宽钳位 2^30，i64 域 |acc| ≈ 1.2e17 < 2^63）。ac_rowmask 语义同上。 */
void tc_simd_dequant_inverse_wide(const struct tc_quant_ctx* ctx, const int32_t q[64],
                                  int32_t xhat[64], uint32_t ac_rowmask)
{
    (void)ac_rowmask;
    int32_t Fp[64];
    tc_dequant_block_ctx(ctx, q, Fp);
    tc_transform_inverse_8x8_scalar(Fp, xhat);
}

tc_dequant_inverse_fn tc_simd_resolve_dequant_inverse(uint8_t wide)
{
    if (wide != 0u) { return &tc_simd_dequant_inverse_wide; }
    simd_ensure();
#if TOPOS_HAVE_AVX2_KERNEL
    if (s_dequant_inverse != NULL) {
        /* 内核表一次生成（内核内部 per-call ensure 从此恒命中快路） */
        tc_transform_avx2_tables_ensure_public();
        return s_dequant_inverse;
    }
#endif
#if TOPOS_HAVE_NEON_KERNEL
    if (s_dequant_inverse != NULL) {
        /* 内核表一次生成（M10-1d 同 AVX2 模式；per-call ensure 恒快路） */
        tc_transform_neon_tables_ensure_public();
        return s_dequant_inverse;
    }
#endif
    return &dequant_inverse_compose;
}

/* M10-6.3B：scalar 回退——量化同源（tc_quant_block_zigzag），掩码顺序推导 */
static void quant_zz_scalar(const struct tc_quant_ctx* ctx, const int32_t F[64],
                            int32_t q_out[64], uint64_t* nz_zz)
{
    tc_quant_block_zigzag(ctx, F, q_out);
    uint64_t nz = 0u;
    for (uint32_t sp = 0u; sp < 64u; ++sp) {
        if (q_out[sp] != 0) { nz |= (uint64_t)1 << sp; }
    }
    *nz_zz = nz;
}

tc_quant_zz_fn tc_simd_resolve_quant_zz(uint8_t wide)
{
    if (wide != 0u) { return &quant_zz_scalar; } /* exact_div 真除法路，SIMD 域外 */
    simd_ensure();
#if TOPOS_HAVE_AVX2_KERNEL
    if (s_quant_zz != NULL) {
        tc_transform_avx2_tables_ensure_public(); /* kZzIdx32 懒生成 */
        return s_quant_zz;
    }
#endif
#if TOPOS_HAVE_NEON_KERNEL
    if (s_quant_zz != NULL) {
        return s_quant_zz; /* 扫描序经 kTcZigzag（scan.h 常量表，无需懒生成） */
    }
#endif
    return &quant_zz_scalar;
}

/* P-速②：scalar/NEON 回退——调用方侧 gather 与 forward 分两步（语义同源） */
static void forward_rows_compose(const uint16_t* src, size_t stride,
                                 int32_t mid, int32_t F[64])
{
    int16_t x[64];
    for (int y = 0; y < 8; ++y) {
        const uint16_t* r = src + (size_t)y * stride;
        for (int xx = 0; xx < 8; ++xx) {
            x[y * 8 + xx] = (int16_t)((int32_t)r[xx] - mid);
        }
    }
    s_forward(x, F);
}

/* 批 4：宽位深行直载（bd≥13）——残差 i32 域（i16 截断不复存在），经
 * i32 前向分发（AVX2/NEON 后端可用时仍走 SIMD，全域 bit-exact）；
 * 量化前做 W 舍入偏差预失真（16-bit 无损数学保证，见 transform.h）。 */
static void forward_rows_wide_compose(const uint16_t* src, size_t stride,
                                      int32_t mid, int32_t F[64])
{
    int32_t x[64];
    for (int y = 0; y < 8; ++y) {
        const uint16_t* r = src + (size_t)y * stride;
        for (int xx = 0; xx < 8; ++xx) {
            x[y * 8 + xx] = (int32_t)r[xx] - mid;
        }
    }
    tc_simd_forward_8x8_i32(x, F);
    tc_transform_forward_predistort_i32(F);
}

tc_forward_rows_fn tc_simd_resolve_forward_rows(uint8_t wide)
{
    if (wide != 0u) { return &forward_rows_wide_compose; }
    simd_ensure();
#if TOPOS_HAVE_AVX2_KERNEL
    if (s_quant_zz != NULL) { /* AVX2 后端可用性同量化内核（同一选择分支） */
        tc_transform_avx2_tables_ensure_public();
        return &tc_transform_forward_8x8_rows_avx2;
    }
#endif
    return &forward_rows_compose;
}

/* P-速⑥：scalar 回退——tc_quant_block_ctx 本就是自然序输出，补掩码即可 */
static void quant_nat_scalar(const struct tc_quant_ctx* ctx, const int32_t F[64],
                             int32_t q_out[64], uint64_t* nz_nat)
{
    tc_quant_block_ctx(ctx, F, q_out);
    uint64_t nz = 0u;
    for (uint32_t k = 0u; k < 64u; ++k) {
        if (q_out[k] != 0) { nz |= (uint64_t)1 << k; }
    }
    *nz_nat = nz;
}

tc_quant_nat_fn tc_simd_resolve_quant_nat(uint8_t wide)
{
    if (wide != 0u) { return &quant_nat_scalar; } /* exact_div 真除法路，SIMD 域外 */
    simd_ensure();
#if TOPOS_HAVE_AVX2_KERNEL
    if (s_quant_zz != NULL) {
        return &tc_quant_block_nat_avx2;
    }
#endif
#if TOPOS_HAVE_NEON_KERNEL
    if (s_quant_zz != NULL) { /* NEON 后端可用性同量化内核（同一选择分支） */
        return &tc_quant_block_nat_neon;
    }
#endif
    return &quant_nat_scalar;
}

/* P-速⑥：自然序→扫描序掩码翻译。kScatter[b][v]：natmask 第 b 字节取值 v
 * 时贡献的 zz 位组合（natural k → bit kTcZigzagInv[k]）。懒建表 +
 * seq_cst 发布（同 vlc books 模式；确定性纯函数）。 */
static uint64_t s_zz_scatter[8][256];
static atomic_int s_zz_scatter_state = 0;

static void zz_scatter_fill(void)
{
    for (uint32_t b = 0u; b < 8u; ++b) {
        for (uint32_t v = 0u; v < 256u; ++v) {
            uint64_t m = 0u;
            for (uint32_t bit = 0u; bit < 8u; ++bit) {
                if ((v & (1u << bit)) != 0u) {
                    m |= UINT64_C(1) << kTcZigzagInv[b * 8u + bit];
                }
            }
            s_zz_scatter[b][v] = m;
        }
    }
    atomic_store(&s_zz_scatter_state, 2);
}

uint64_t tc_zz_mask_from_nat(uint64_t nat_mask)
{
    if (atomic_load_explicit(&s_zz_scatter_state, memory_order_acquire) != 2) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&s_zz_scatter_state, &expected, 1)) {
            zz_scatter_fill();
            return tc_zz_mask_from_nat(nat_mask);
        }
        while (atomic_load_explicit(&s_zz_scatter_state, memory_order_acquire) != 2) {
        }
    }
    return s_zz_scatter[0][nat_mask & 0xFFu]
         | s_zz_scatter[1][(nat_mask >> 8) & 0xFFu]
         | s_zz_scatter[2][(nat_mask >> 16) & 0xFFu]
         | s_zz_scatter[3][(nat_mask >> 24) & 0xFFu]
         | s_zz_scatter[4][(nat_mask >> 32) & 0xFFu]
         | s_zz_scatter[5][(nat_mask >> 40) & 0xFFu]
         | s_zz_scatter[6][(nat_mask >> 48) & 0xFFu]
         | s_zz_scatter[7][(nat_mask >> 56) & 0xFFu];
}

/* M10-2C：四块 SoA 批量反量化+逆变换入口。
 * qsoa[64][4]（int32 值符号扩展入 int64 lane）；xh4 为 [4][64] 连续输出；
 * rm_union = 4 块行掩码并集（并集外行对全部块恒零贡献）。无 AVX2 后端时
 * 逐块 compose 回退（语义一致）。 */
void tc_simd_dequant_inverse_8x8x4(const struct tc_quant_ctx* ctx, const int64_t* qsoa,
                                   int32_t* xh4, uint32_t rm_union)
{
    simd_ensure();
#if TOPOS_HAVE_AVX2_KERNEL
    if (s_dequant_inverse != NULL) {
        tc_transform_avx2_tables_ensure_public();
        tc_dequant_inverse_8x8x4_avx2(ctx, qsoa, xh4, rm_union);
        return;
    }
#endif
    for (int k = 0; k < 4; ++k) {
        int32_t q[64];
        for (int i = 0; i < 64; ++i) { q[i] = (int32_t)qsoa[i * 4 + k]; }
        uint32_t rm = 1u;
        for (int r = 0; r < 8; ++r) {
            for (int c = 0; c < 8; ++c) {
                if (q[r * 8 + c] != 0) { rm |= 1u << r; break; }
            }
        }
        dequant_inverse_compose(ctx, q, xh4 + (size_t)k * 64u, rm);
    }
    (void)rm_union;
}
