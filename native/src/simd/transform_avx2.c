/* AVX2 前向变换内核（阶段 9）—— 与 scalar 逐位一致（差分测试 + golden 复验）。
 *
 * 精确性（ADR-C009 / ADR-C002 值域）：
 *  - pass1 A[y][v] = Σ_c x[y][c]·M[v][c]：|x| ≤ 2047（12-bit shift 后的规范界）、
 *    |M| ≤ 13 → |A| ≤ 8·2047·13 = 212,888 < 2^18，i32 通道精确；
 *  - pass2 F[u][v] = Σ_y M[u][y]·A[y][v] → |F| ≤ 8·212,888·13 ≈ 2.2×10^7 < 2^25。
 * 全程 epi32 乘加（无舍入无溢出）→ 与 scalar bit-exact。
 *
 * 平台（阶段 10）：
 *  - gcc/clang：函数级 target("avx2")，库本体保持基线指令集；
 *  - MSVC：无函数级 target —— 本 TU 整体以 /arch:AVX2 编译（CMake per-source），
 *    由 dispatch 的运行时 cpuid 判定是否调用。
 *  - universal 构建（macOS x86_64+arm64）时本 TU 按编译切片各自取舍：
 *    整个 TU 包在 x86_64 守卫内，arm64 切片编译为空（dispatch 同样按切片判定）。
 * M 布局表懒生成（C-77）：首用 CAS 单写者填充，替代 constructor
 * （MSVC 无 constructor 属性；也解除 dylib 卸载时的加载顺序依赖）。
 */
#if defined(__x86_64__) || defined(_M_X64)

/* MSVC 无 __attribute__：对齐用 __declspec(align)；target("avx2") 由本 TU
 * 的 /arch:AVX2 整体编译替代（CMake per-source 属性，见 CMakeLists） */
#if defined(_MSC_VER)
#define TOPOS_SIMD_TARGET
#define TOPOS_ALIGNED32 __declspec(align(32))
#else
#define TOPOS_SIMD_TARGET __attribute__((target("avx2")))
#define TOPOS_ALIGNED32 __attribute__((aligned(32)))
#endif

#include <immintrin.h>
#include <stdatomic.h>

#include "../entropy/scan.h"
#include "../transform/quant.h"
#include "../transform/transform.h"
#include "../transform/transform_tables.h"

/* kMT32[c][v] = M[v][c]（pass1 的乘数列）；kM32[u][y] = M[u][y]（pass2 乘数行） */
static int32_t kMT32[8][8];
static int32_t kM32[8][8];
/* P-速①：pass1 pmaddwd 配对常数——word0 = M[v][2p]、word1 = M[v][2p+1]
 * 打包进 i32（i16 值域 |M| ≤ 13）；与列配对向量 P(2p,2p+1) 相乘时
 * lane i = x[i][2p]·M[v][2p] + x[i][2p+1]·M[v][2p+1]。 */
static int32_t kMP32[8][4];
/* M10-6.3B：gather 型量化的 zigzag 索引（i32 lane 版 kTcZigzag） */
static TOPOS_ALIGNED32 int32_t kZzIdx32[64];
/* M4 逆变换：kWMb[u][v][i] = W[u][v]·M[v][i]（i = x0..3，64-bit lane 低 32 位
 * 承载 int32 值；|W·M| ≤ 10228·13 < 2^18）。 */
static int64_t kWMb[8][8][4];
/* Reduced preview basis: W[u][v]·M[v][x]·M[u][y]，按自然序系数和
 * 块内样本位置 y*8+x 索引，避免采样热循环重复计算固定权重。 */
static int64_t kSampleWeights[64][64];
/* Exact-half preview basis, indexed by natural coefficient then row-major
 * output position.  The output coordinates are fixed at 0,2,4,6 on both
 * axes, so the hot loop needs no coordinate packing or indirect sample lookup. */
static int64_t kHalfSampleWeights[64][16];
static atomic_int s_tables_state; /* 0 未生成 / 1 生成中 / 2 就绪（与 crc32 同模式） */

static void transform_avx2_tables_fill(void)
{
    for (int a = 0; a < 8; ++a) {
        for (int b = 0; b < 8; ++b) {
            kM32[a][b] = (int32_t)kTransformM[a * 8 + b];
            kMT32[a][b] = (int32_t)kTransformM[b * 8 + a];
        }
    }
    for (int i = 0; i < 64; ++i) { kZzIdx32[i] = (int32_t)kTcZigzag[i]; }
    for (int v = 0; v < 8; ++v) {
        for (int p = 0; p < 4; ++p) {
            const uint32_t lo = (uint16_t)kTransformM[v * 8 + 2 * p];
            const uint32_t hi = (uint16_t)kTransformM[v * 8 + 2 * p + 1];
            kMP32[v][p] = (int32_t)((hi << 16) | lo);
        }
    }
    for (int u = 0; u < 8; ++u) {
        for (int v = 0; v < 8; ++v) {
            for (int i = 0; i < 4; ++i) {
                kWMb[u][v][i] = (int64_t)kTransformW[u * 8 + v]
                                * (int64_t)kTransformM[v * 8 + i];
            }
        }
    }
    for (int natural = 0; natural < 64; ++natural) {
        const int u = natural >> 3;
        const int v = natural & 7;
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                kSampleWeights[natural][y * 8 + x] =
                    (int64_t)kTransformW[natural]
                    * (int64_t)kTransformM[v * 8 + x]
                    * (int64_t)kTransformM[u * 8 + y];
            }
        }
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                kHalfSampleWeights[natural][y * 4 + x] =
                    kSampleWeights[natural][(y * 2) * 8 + (x * 2)];
            }
        }
    }
    atomic_store(&s_tables_state, 2); /* 表写完成后再发布（seq_cst 建立先序） */
}

static void transform_avx2_tables_ensure(void)
{
    if (atomic_load_explicit(&s_tables_state, memory_order_acquire) == 2) { return; }
    int expected = 0;
    if (atomic_compare_exchange_strong(&s_tables_state, &expected, 1)) {
        transform_avx2_tables_fill(); /* 唯一写者 */
        return;
    }
    while (atomic_load_explicit(&s_tables_state, memory_order_acquire) != 2) {
        /* 等待写者发布（确定性数据，无序亦无害；自旋极短） */
    }
}

/* M10-1d：公开入口——调用方（dispatch 解析）在 frame/slice 入口一次完成
 * 表生成，块循环内内核的 per-call ensure 恒命中 acquire 快路。 */
void tc_transform_avx2_tables_ensure_public(void)
{
    transform_avx2_tables_ensure();
}

#if defined(__GNUC__) || defined(__clang__)
TOPOS_SIMD_TARGET
#endif
static void forward_core_avx2(const __m128i r[8], int32_t F[64])
{
    transform_avx2_tables_ensure();
    /* P-速① 重写（位精确：整数域任意重排，见下方推导）：
     * pass1 = x 转置 → 列配对 → 8×(4 pmaddwd + 3 add)（消 64 个慢
     * mullo_epi32；pmaddwd 单 uop 且 i16 对乘加）→ A_col[v]（lanes = y）；
     * A 转置（i32 蝶形）→ A_row[y]（lanes = v）；
     * pass2 = M 偶奇对称性（M[u][7−y] = εu·M[u][y]，εu = +1 偶行/−1 奇行）
     * 预合并 S/D（乘法数 64→32）→ F[u]（mullo 广播乘加）。
     * 精确性界（全部 i32 无溢出，与旧内核同值域）：
     *   pmaddwd lane ≤ 2·2047·13 = 53,222；A ≤ 8·2047·13 = 212,888；
     *   S/D ≤ 2·|A| = 425,776；M·S ≤ 13·425,776 ≈ 5.5×10^6；
     *   F ≤ 4·5.5×10^6 ≈ 2.2×10^7 < 2^25 ✓ */
    /* 8×8 i16 转置（蝶形 unpack）→ c[k] = 列 k（lanes = y） */
    __m128i t0 = _mm_unpacklo_epi16(r[0], r[1]);
    __m128i t1 = _mm_unpackhi_epi16(r[0], r[1]);
    __m128i t2 = _mm_unpacklo_epi16(r[2], r[3]);
    __m128i t3 = _mm_unpackhi_epi16(r[2], r[3]);
    __m128i t4 = _mm_unpacklo_epi16(r[4], r[5]);
    __m128i t5 = _mm_unpackhi_epi16(r[4], r[5]);
    __m128i t6 = _mm_unpacklo_epi16(r[6], r[7]);
    __m128i t7 = _mm_unpackhi_epi16(r[6], r[7]);
    __m128i q0 = _mm_unpacklo_epi32(t0, t2);
    __m128i q1 = _mm_unpackhi_epi32(t0, t2);
    __m128i q2 = _mm_unpacklo_epi32(t1, t3);
    __m128i q3 = _mm_unpackhi_epi32(t1, t3);
    __m128i q4 = _mm_unpacklo_epi32(t4, t6);
    __m128i q5 = _mm_unpackhi_epi32(t4, t6);
    __m128i q6 = _mm_unpacklo_epi32(t5, t7);
    __m128i q7 = _mm_unpackhi_epi32(t5, t7);
    __m128i c0 = _mm_unpacklo_epi64(q0, q4);
    __m128i c1 = _mm_unpackhi_epi64(q0, q4);
    __m128i c2 = _mm_unpacklo_epi64(q1, q5);
    __m128i c3 = _mm_unpackhi_epi64(q1, q5);
    __m128i c4 = _mm_unpacklo_epi64(q2, q6);
    __m128i c5 = _mm_unpackhi_epi64(q2, q6);
    __m128i c6 = _mm_unpacklo_epi64(q3, q7);
    __m128i c7 = _mm_unpackhi_epi64(q3, q7);

    /* 相邻列交织（word 2i = 列 2p[i]，word 2i+1 = 列 2p+1[i]） */
#define TC_SET256(lo, hi)                                                      \
    _mm256_insertf128_si256(_mm256_castsi128_si256(lo), hi, 1)
    const __m256i p01 = TC_SET256(_mm_unpacklo_epi16(c0, c1),
                                  _mm_unpackhi_epi16(c0, c1));
    const __m256i p23 = TC_SET256(_mm_unpacklo_epi16(c2, c3),
                                  _mm_unpackhi_epi16(c2, c3));
    const __m256i p45 = TC_SET256(_mm_unpacklo_epi16(c4, c5),
                                  _mm_unpackhi_epi16(c4, c5));
    const __m256i p67 = TC_SET256(_mm_unpacklo_epi16(c6, c7),
                                  _mm_unpackhi_epi16(c6, c7));
#undef TC_SET256

    /* A_col[v]（lanes = y）：A[y][v] = Σ_c x[y][c]·M[v][c] */
    __m256i ac[8];
    for (int v = 0; v < 8; ++v) {
        __m256i acc = _mm256_madd_epi16(p01, _mm256_set1_epi32(kMP32[v][0]));
        acc = _mm256_add_epi32(
            acc, _mm256_madd_epi16(p23, _mm256_set1_epi32(kMP32[v][1])));
        acc = _mm256_add_epi32(
            acc, _mm256_madd_epi16(p45, _mm256_set1_epi32(kMP32[v][2])));
        acc = _mm256_add_epi32(
            acc, _mm256_madd_epi16(p67, _mm256_set1_epi32(kMP32[v][3])));
        ac[v] = acc;
    }

    /* A 转置（8×8 i32 蝶形）→ a[y] = A_row[y]（lanes = v） */
    __m256i u0 = _mm256_unpacklo_epi32(ac[0], ac[1]);
    __m256i u1 = _mm256_unpackhi_epi32(ac[0], ac[1]);
    __m256i u2 = _mm256_unpacklo_epi32(ac[2], ac[3]);
    __m256i u3 = _mm256_unpackhi_epi32(ac[2], ac[3]);
    __m256i u4 = _mm256_unpacklo_epi32(ac[4], ac[5]);
    __m256i u5 = _mm256_unpackhi_epi32(ac[4], ac[5]);
    __m256i u6 = _mm256_unpacklo_epi32(ac[6], ac[7]);
    __m256i u7 = _mm256_unpackhi_epi32(ac[6], ac[7]);
    __m256i g0 = _mm256_unpacklo_epi64(u0, u2);
    __m256i g1 = _mm256_unpackhi_epi64(u0, u2);
    __m256i g2 = _mm256_unpacklo_epi64(u1, u3);
    __m256i g3 = _mm256_unpackhi_epi64(u1, u3);
    __m256i g4 = _mm256_unpacklo_epi64(u4, u6);
    __m256i g5 = _mm256_unpackhi_epi64(u4, u6);
    __m256i g6 = _mm256_unpacklo_epi64(u5, u7);
    __m256i g7 = _mm256_unpackhi_epi64(u5, u7);
    __m256i a[8];
    a[0] = _mm256_permute2x128_si256(g0, g4, 0x20);
    a[1] = _mm256_permute2x128_si256(g1, g5, 0x20);
    a[2] = _mm256_permute2x128_si256(g2, g6, 0x20);
    a[3] = _mm256_permute2x128_si256(g3, g7, 0x20);
    a[4] = _mm256_permute2x128_si256(g0, g4, 0x31);
    a[5] = _mm256_permute2x128_si256(g1, g5, 0x31);
    a[6] = _mm256_permute2x128_si256(g2, g6, 0x31);
    a[7] = _mm256_permute2x128_si256(g3, g7, 0x31);

    /* pass2 对称性：S/D 预合并（乘法 64→32）*/
    __m256i sd[2][4];
    for (int y = 0; y < 4; ++y) {
        sd[0][y] = _mm256_add_epi32(a[y], a[7 - y]); /* 偶 u：A[y]+εA[7−y], ε=+1 */
        sd[1][y] = _mm256_sub_epi32(a[y], a[7 - y]); /* 奇 u：ε=−1 */
    }
    for (int u = 0; u < 8; ++u) {
        const __m256i* s = sd[u & 1];
        __m256i acc = _mm256_mullo_epi32(
            _mm256_set1_epi32(kM32[u][0]), s[0]);
        acc = _mm256_add_epi32(
            acc, _mm256_mullo_epi32(_mm256_set1_epi32(kM32[u][1]), s[1]));
        acc = _mm256_add_epi32(
            acc, _mm256_mullo_epi32(_mm256_set1_epi32(kM32[u][2]), s[2]));
        acc = _mm256_add_epi32(
            acc, _mm256_mullo_epi32(_mm256_set1_epi32(kM32[u][3]), s[3]));
        _mm256_storeu_si256((__m256i*)(F + u * 8), acc);
    }
}

#if defined(__GNUC__) || defined(__clang__)
TOPOS_SIMD_TARGET
#endif
void tc_transform_forward_8x8_avx2(const int16_t x[64], int32_t F[64])
{
    __m128i r[8];
    for (int y = 0; y < 8; ++y) {
        r[y] = _mm_loadu_si128((const __m128i*)(x + y * 8));
    }
    forward_core_avx2(r, F);
}

/* P-速②：u16 平面行直载入口——省去调用方 64 次标量 gather（loadu +
 * (int16_t)(v−mid) 截断存储）。精确性：sub_epi16 lane 语义 = (v−mid)
 * mod 2^16 再按 i16 解释，与标量 (int16_t)((int32_t)v − mid) 逐位一致
 * （v ∈ [0,2^bit_depth]、mid = 2^(bit_depth−1) → v−mid ∈ ±2^(bd−1)，
 * mod 2^16 表示唯一）。每行 8×u16 恰 16 字节 = 一次 loadu。 */
#if defined(__GNUC__) || defined(__clang__)
TOPOS_SIMD_TARGET
#endif
void tc_transform_forward_8x8_rows_avx2(const uint16_t* src, size_t stride,
                                        int32_t mid, int32_t F[64])
{
    const __m128i vmid = _mm_set1_epi16((int16_t)mid);
    __m128i r[8];
    for (int y = 0; y < 8; ++y) {
        r[y] = _mm_sub_epi16(
            _mm_loadu_si128((const __m128i*)(src + (size_t)y * stride)), vmid);
    }
    forward_core_avx2(r, F);
}

/* ---- 批 4 阶段 2：i32 输入前向内核（bd≥13 通用路） ----
 * 与 tc_transform_forward_8x8_i32_scalar 逐位一致（整数加法可精确重结合，
 * 累加序差异不改变结果）。精确性（12-bit 规范界等比外推至 bd16）：
 *  - pass1 A[y][v] = Σ_c x[y][c]·M[v][c]：|x| ≤ 32767（16-bit level shift 后
 *    规范界）→ |A| ≤ 8·32767·13 = 3,407,768 < 2^22，epi32 通道精确；
 *  - pass2 F[u][v] = Σ_y M[u][y]·A[y][v] → |F| ≤ 8·13·|A|max ≈ 3.54×10^8
 *    < 2^29 < 2^31，epi32 通道精确。
 * mullo（mod 2^32）在所有中间值 ∈ i32 时与精确乘法一致 → 无舍入无溢出。 */
#if defined(__GNUC__) || defined(__clang__)
TOPOS_SIMD_TARGET
#endif
void tc_transform_forward_8x8_i32_avx2(const int32_t x[64], int32_t F[64])
{
    transform_avx2_tables_ensure();

    /* pass1：每输出 A[y][v] 整行 mullo 后水平求和（kM32[v] = M[v][0..7]；
     * 注意 kMT32[v] 是 M 的第 v 列，pass1 需要 M 的第 v 行） */
    TOPOS_ALIGNED32 int32_t A[64];
    for (int y = 0; y < 8; ++y) {
        const __m256i r = _mm256_loadu_si256((const __m256i*)(x + y * 8));
        for (int v = 0; v < 8; ++v) {
            const __m256i p = _mm256_mullo_epi32(
                r, _mm256_loadu_si256((const __m256i*)(kM32[v])));
            __m128i s = _mm_add_epi32(_mm256_castsi256_si128(p),
                                      _mm256_extracti128_si256(p, 1));
            s = _mm_hadd_epi32(s, s);
            s = _mm_hadd_epi32(s, s);
            A[y * 8 + v] = _mm_cvtsi128_si32(s);
        }
    }

    /* pass2：F[u] 行 = Σ_y M[u][y]·A[y]（broadcast 标量乘行向量，y 升序） */
    for (int u = 0; u < 8; ++u) {
        __m256i acc = _mm256_setzero_si256();
        for (int y = 0; y < 8; ++y) {
            acc = _mm256_add_epi32(acc, _mm256_mullo_epi32(
                _mm256_set1_epi32(kM32[u][y]),
                _mm256_loadu_si256((const __m256i*)(A + y * 8))));
        }
        _mm256_storeu_si256((__m256i*)(F + u * 8), acc);
    }
}

/* ---- M4：AVX2 逆变换内核（与 scalar bit-exact；对称化 + B 16-bit 分解） ----
 *
 * 精确性推导（整数域无溢出即可任意重结合）：
 *  - cc = clamp(coef) ∈ ±2^25；WM = W·M ∈ ±132964 < 2^18 → mul_epi32(32×32→64)
 *    精确，|B| = |Σ cc·WM| < 8·2^25·2^18 < 2^46（64-bit lane）；
 *  - 对称性 M[v][7−x] = εv·M[v][x]（偶 v 行 +1/奇 v 行 −1）：pe/po 为偶/奇 v
 *    部分和 → B[u][0..3] = pe+po、B[u][7..4] = pe−po（乘法数减半，逐位一致）；
 *  - B 分解 B = BHi·2^16 + BLo（|BHi| < 2^30 fits i32 lane、BLo ∈ [0,2^16)）：
 *    M·BHi/M·BLo 均 32×32→64 精确，Σ_u 后 (hi<<16)+lo = Σ M·B（64 位精确）；
 *  - pass3 对称性 M[u][7−y] = εu·M[u][y]：qe/qo → x̂[y] = round(qe+qo)、
 *    x̂[7−y] = round(qe−qo)，round 为符号拆分四舍五入（与 scalar 同式）。
 */
#if defined(__GNUC__) || defined(__clang__)
TOPOS_SIMD_TARGET
#endif
void tc_transform_inverse_8x8_avx2(const int32_t coef[64], int32_t xhat[64])
{
    transform_avx2_tables_ensure();

    /* 防御性钳位到 ±2^25（与 scalar 同值；32 位通道安全） */
    TOPOS_ALIGNED32 int32_t cc[64];
    {
        const __m256i vl = _mm256_set1_epi32((int32_t)-TC_TRANSFORM_MAX_ABS_F);
        const __m256i vh = _mm256_set1_epi32((int32_t)TC_TRANSFORM_MAX_ABS_F);
        for (int i = 0; i < 64; i += 8) {
            __m256i v = _mm256_loadu_si256((const __m256i*)(coef + i));
            _mm256_store_si256((__m256i*)(cc + i), _mm256_min_epi32(_mm256_max_epi32(v, vl), vh));
        }
    }

    /* pass B（lanes = x0..3）：pe/po → B[u][0..3] / B[u][7..4]（permute 反转） */
    __m256i b[8][2];
    for (int u = 0; u < 8; ++u) {
        __m256i pe = _mm256_setzero_si256();
        __m256i po = _mm256_setzero_si256();
        for (int v = 0; v < 8; ++v) {
            __m256i cv = _mm256_set1_epi64x((int64_t)cc[u * 8 + v]);
            __m256i pr = _mm256_mul_epi32(cv, _mm256_loadu_si256((const __m256i*)kWMb[u][v]));
            if ((v & 1) == 0) { pe = _mm256_add_epi64(pe, pr); }
            else { po = _mm256_add_epi64(po, pr); }
        }
        b[u][0] = _mm256_add_epi64(pe, po);
        b[u][1] = _mm256_permute4x64_epi64(_mm256_sub_epi64(pe, po), _MM_SHUFFLE(0, 1, 2, 3));
    }

    /* B 16-bit 分解：B = BHi·2^16 + BLo。
     * BHi 的完整 32 位值（bits 16..47）须跨 dword 漏斗拼装进 lane 低 32：
     *   dword0 = (B_d0 >>u16) | (B_d1 <<u16)   —— 仅低 dword 被 mul_epi32 读取
     * |BHi| < 2^30 < 2^31 → 该 i32 即 BHi（符号正确）。BLo = B & 0xFFFF。 */
    __m256i bhi[8][2];
    __m256i blo[8][2];
    {
        const __m256i m16 = _mm256_set1_epi64x((int64_t)0xFFFF);
        for (int u = 0; u < 8; ++u) {
            for (int g = 0; g < 2; ++g) {
                __m256i pair = _mm256_shuffle_epi32(b[u][g], _MM_SHUFFLE(3, 3, 1, 1));
                bhi[u][g] = _mm256_or_si256(_mm256_srli_epi32(b[u][g], 16),
                                            _mm256_slli_epi32(pair, 16));
                blo[u][g] = _mm256_and_si256(b[u][g], m16);
            }
        }
    }

    /* pass 3（lanes = x0..3 / x4..7）+ 对称输出 + round32 */
    const __m256i vr = _mm256_set1_epi64x((int64_t)1 << 31);
    const __m256i zero = _mm256_setzero_si256();
    for (int y = 0; y < 4; ++y) {
        for (int g = 0; g < 2; ++g) {
            __m256i qe_hi = zero, qe_lo = zero;
            __m256i qo_hi = zero, qo_lo = zero;
            for (int u = 0; u < 8; ++u) {
                __m256i my = _mm256_set1_epi64x((int64_t)kM32[u][y]);
                __m256i hi = _mm256_mul_epi32(my, bhi[u][g]);
                __m256i lo = _mm256_mul_epi32(my, blo[u][g]);
                if ((u & 1) == 0) {
                    qe_hi = _mm256_add_epi64(qe_hi, hi);
                    qe_lo = _mm256_add_epi64(qe_lo, lo);
                } else {
                    qo_hi = _mm256_add_epi64(qo_hi, hi);
                    qo_lo = _mm256_add_epi64(qo_lo, lo);
                }
            }
            __m256i te = _mm256_add_epi64(_mm256_slli_epi64(qe_hi, 16), qe_lo);
            __m256i to = _mm256_add_epi64(_mm256_slli_epi64(qo_hi, 16), qo_lo);
            __m256i tp = _mm256_add_epi64(te, to); /* x̂[y][...] 总和 */
            __m256i tm = _mm256_sub_epi64(te, to); /* x̂[7−y][...] 总和 */
            TOPOS_ALIGNED32 int64_t op[4];
            TOPOS_ALIGNED32 int64_t om[4];
            {
                __m256i sg = _mm256_cmpgt_epi64(zero, tp);
                __m256i av = _mm256_sub_epi64(_mm256_xor_si256(tp, sg), sg);
                __m256i r = _mm256_srli_epi64(_mm256_add_epi64(av, vr), 32);
                _mm256_store_si256((__m256i*)op, _mm256_sub_epi64(_mm256_xor_si256(r, sg), sg));
            }
            {
                __m256i sg = _mm256_cmpgt_epi64(zero, tm);
                __m256i av = _mm256_sub_epi64(_mm256_xor_si256(tm, sg), sg);
                __m256i r = _mm256_srli_epi64(_mm256_add_epi64(av, vr), 32);
                _mm256_store_si256((__m256i*)om, _mm256_sub_epi64(_mm256_xor_si256(r, sg), sg));
            }
            for (int i = 0; i < 4; ++i) {
                xhat[y * 8 + g * 4 + i] = (int32_t)op[i];
                xhat[(7 - y) * 8 + g * 4 + i] = (int32_t)om[i];
            }
        }
    }
}

/* 固定比例预览的采样逆变换：4 个目标点共享同一组频率项。
 * 每个 i64 lane 对应一个目标点，避免为每个像素重复完整 8×8 乘加。
 * RD3-02 limited 入口按冻结 zigzag band 直接遍历 5/11/17/25 项。 */
#if defined(__GNUC__) || defined(__clang__)
TOPOS_SIMD_TARGET
#endif
static void tc_transform_inverse_8x8_samples_avx2_impl(
    const int32_t coef[64], const uint8_t* xs, const uint8_t* ys,
    uint32_t count, uint8_t max_scan_pos, int32_t* out)
{
    transform_avx2_tables_ensure();
    if (count == 0u) { return; }
    const uint32_t limit = max_scan_pos > 63u ? 63u : (uint32_t)max_scan_pos;
    for (uint32_t base = 0u; base < count; base += 4u) {
        const uint32_t n = (count - base) < 4u ? (count - base) : 4u;
        uint32_t sample_pos[4] = { 0u, 0u, 0u, 0u };
        for (uint32_t lane = 0u; lane < n; ++lane) {
            const uint32_t x = (uint32_t)(xs[base + lane] & 7u);
            const uint32_t y = (uint32_t)(ys[base + lane] & 7u);
            sample_pos[lane] = y * 8u + x;
        }
        __m256i acc = _mm256_setzero_si256();
        for (uint32_t scan_pos = 0u; scan_pos <= limit; ++scan_pos) {
            const uint32_t natural = kTcZigzag[scan_pos];
            const int64_t* basis = kSampleWeights[natural];
            const __m256i vw = _mm256_set_epi64x(
                basis[sample_pos[3]], basis[sample_pos[2]],
                basis[sample_pos[1]], basis[sample_pos[0]]);
            int32_t coeff = coef[natural];
            /* 批 4：宽钳位（bd≥13 合法域；i64 累加 |acc| ≤ 64·2^30·2^20.7 < 2^57） */
            if (coeff > TC_TRANSFORM_MAX_ABS_F_WIDE) { coeff = TC_TRANSFORM_MAX_ABS_F_WIDE; }
            else if (coeff < -TC_TRANSFORM_MAX_ABS_F_WIDE) { coeff = -TC_TRANSFORM_MAX_ABS_F_WIDE; }
            acc = _mm256_add_epi64(
                acc, _mm256_mul_epi32(_mm256_set1_epi64x((int64_t)coeff), vw));
        }
        int64_t sums[4];
        _mm256_storeu_si256((__m256i*)sums, acc);
        for (uint32_t lane = 0u; lane < n; ++lane) {
            const int64_t av = sums[lane] >= 0 ? sums[lane] : -sums[lane];
            const int64_t rounded = (av + ((int64_t)1 << 31)) >> 32;
            out[base + lane] = sums[lane] >= 0 ? (int32_t)rounded : -(int32_t)rounded;
        }
    }
}

void tc_transform_inverse_8x8_samples_avx2(const int32_t coef[64], const uint8_t* xs,
                                           const uint8_t* ys, uint32_t count, int32_t* out)
{
    tc_transform_inverse_8x8_samples_avx2_impl(coef, xs, ys, count, 63u, out);
}

void tc_transform_inverse_8x8_samples_avx2_limited(const int32_t coef[64],
                                                   const uint8_t* xs, const uint8_t* ys,
                                                   uint32_t count, uint8_t max_scan_pos,
                                                   int32_t* out)
{
    tc_transform_inverse_8x8_samples_avx2_impl(coef, xs, ys, count,
                                               max_scan_pos, out);
}

/* Exact 1/2 preview variant.  This is intentionally the same accumulation
 * and rounding sequence as the generic AVX2 sample kernel; only the fixed
 * coordinate lookup is removed.  That keeps the scalar/SIMD differential
 * contract bit-exact while addressing the dominant per-block setup cost. */
#if defined(__GNUC__) || defined(__clang__)
TOPOS_SIMD_TARGET
#endif
void tc_transform_inverse_8x8_samples_half_avx2_limited(const int32_t coef[64],
                                                        uint32_t count,
                                                        uint8_t max_scan_pos,
                                                        int32_t* out)
{
    transform_avx2_tables_ensure();
    if (coef == NULL || out == NULL || count == 0u) { return; }
    if (count > 16u) { count = 16u; }
    const uint32_t limit = max_scan_pos > 63u ? 63u : (uint32_t)max_scan_pos;
    for (uint32_t base = 0u; base < count; base += 4u) {
        const uint32_t n = (count - base) < 4u ? (count - base) : 4u;
        __m256i acc = _mm256_setzero_si256();
        for (uint32_t scan_pos = 0u; scan_pos <= limit; ++scan_pos) {
            const uint32_t natural = kTcZigzag[scan_pos];
            const int64_t* basis = kHalfSampleWeights[natural];
            const __m256i vw = _mm256_loadu_si256(
                (const __m256i*)(basis + base));
            int32_t coeff = coef[natural];
            if (coeff > TC_TRANSFORM_MAX_ABS_F_WIDE) { coeff = TC_TRANSFORM_MAX_ABS_F_WIDE; }
            else if (coeff < -TC_TRANSFORM_MAX_ABS_F_WIDE) { coeff = -TC_TRANSFORM_MAX_ABS_F_WIDE; }
            acc = _mm256_add_epi64(
                acc, _mm256_mul_epi32(_mm256_set1_epi64x((int64_t)coeff), vw));
        }
        int64_t sums[4];
        _mm256_storeu_si256((__m256i*)sums, acc);
        for (uint32_t lane = 0u; lane < n; ++lane) {
            const int64_t av = sums[lane] >= 0 ? sums[lane] : -sums[lane];
            const int64_t rounded = (av + ((int64_t)1 << 31)) >> 32;
            out[base + lane] = sums[lane] >= 0 ? (int32_t)rounded : -(int32_t)rounded;
        }
    }
}

/* Exact-half fused dequant + inverse preview.  The arithmetic intentionally
 * matches tc_dequant_inverse_samples_limited followed by the half sample
 * kernel; the only removed work is the temporary Fp[64] materialization. */
#if defined(__GNUC__) || defined(__clang__)
TOPOS_SIMD_TARGET
#endif
void tc_dequant_inverse_samples_half_avx2_limited(const tc_quant_ctx* ctx,
                                                  const int32_t q[64],
                                                  uint32_t count,
                                                  uint8_t max_scan_pos,
                                                  int32_t* out)
{
    transform_avx2_tables_ensure();
    if (ctx == NULL || q == NULL || out == NULL || count == 0u) { return; }
    if (count > 16u) { count = 16u; }
    const int64_t fclamp = ctx->f_clamp > 0
        ? ctx->f_clamp : (int64_t)TC_TRANSFORM_MAX_ABS_F;
    const uint32_t limit = max_scan_pos > 63u ? 63u : (uint32_t)max_scan_pos;
    /* Dequantize once per scan position, not once per 4-output SIMD batch.
     * Keeping scan order here also avoids the natural-order memset/scatter of
     * the older Fp[64] compose path while preserving the same clamping. */
    int32_t dequant[64];
    for (uint32_t scan_pos = 0u; scan_pos <= limit; ++scan_pos) {
        const uint32_t natural = kTcZigzag[scan_pos];
        int64_t v = (int64_t)q[natural] * (int64_t)ctx->Q[natural];
        if (v > fclamp) { v = fclamp; }
        else if (v < -fclamp) { v = -fclamp; }
        dequant[scan_pos] = (int32_t)v;
    }
    for (uint32_t base = 0u; base < count; base += 4u) {
        const uint32_t n = (count - base) < 4u ? (count - base) : 4u;
        __m256i acc = _mm256_setzero_si256();
        for (uint32_t scan_pos = 0u; scan_pos <= limit; ++scan_pos) {
            const uint32_t natural = kTcZigzag[scan_pos];
            const int32_t coeff = dequant[scan_pos];
            if (coeff == 0) { continue; }
            const int64_t* basis = kHalfSampleWeights[natural];
            const __m256i vw = _mm256_loadu_si256(
                (const __m256i*)(basis + base));
            acc = _mm256_add_epi64(
                acc, _mm256_mul_epi32(_mm256_set1_epi64x((int64_t)coeff), vw));
        }
        int64_t sums[4];
        _mm256_storeu_si256((__m256i*)sums, acc);
        for (uint32_t lane = 0u; lane < n; ++lane) {
            const int64_t av = sums[lane] >= 0 ? sums[lane] : -sums[lane];
            const int64_t rounded = (av + ((int64_t)1 << 31)) >> 32;
            out[base + lane] = sums[lane] >= 0 ? (int32_t)rounded : -(int32_t)rounded;
        }
    }
}

/* ---- 解码深化批次：融合反量化 + 逆变换（解码 sink 热路径） ----
 * 数学恒等：pass B 中 cc==0 的 v 迭代与 pass 3 中整行零的 u 迭代贡献为
 * 加零向量——跳过不改变任何和（与「tc_dequant_block_ctx + 逆变换」逐位
 * 一致，test_transform 差分钉死）。反量化内联为标量 imul，只对非零系数
 * 执行：稀疏块（真实素材主流形态，低频聚集、高频整行零）省 64 元素
 * 反量化 pass 与大部分 pass-B 乘法；稠密块成本与分离版相当。
 * 跳转形态跨块稳定（同一 zigzag 位置在不同块间稀疏性一致）→ 分支预测
 * 命中率高。行稀疏掩码 rowmask：bit u = 第 u 频率行存在非零系数。 */
#if defined(__GNUC__) || defined(__clang__)
TOPOS_SIMD_TARGET
#endif
void tc_dequant_inverse_8x8_avx2(const tc_quant_ctx* ctx, const int32_t q[64],
                                 int32_t xhat[64], uint32_t ac_rowmask)
{
    transform_avx2_tables_ensure();
    const uint32_t* Q = ctx->Q;

    /* 行稀疏掩码由解码侧散射时积累直通（bit u = 频率行 u 存在非零系数，
     * 解码器保证为实际非零行的超集——DC 恒 bit0）。
     * 行粒度跳转在块间/邻块间形态一致 → 分支预测命中率高（系数级判断在
     * 噪声素材上零散无规律，实测退化）。 */
    const uint32_t rowmask = ac_rowmask;

    /* pass B（lanes = x0..3）：pe/po → B[u][0..3] / B[u][7..4]（permute 反转）。
     * 非零行内 8 个 v 无条件执行（与旧版「先 dequant 后 inverse」每系数
     * 同做乘加一致，仅把反量化内联为标量 imul、省 Fp 64 元素写读往返）；
     * 整行零 → B[u] = 0（加零向量恒等）。 */
    __m256i b[8][2];
    for (int u = 0; u < 8; ++u) {
        if ((rowmask & (1u << u)) != 0u) {
            __m256i pe = _mm256_setzero_si256();
            __m256i po = _mm256_setzero_si256();
            for (int v = 0; v < 8; ++v) {
                const int32_t c0 = q[u * 8 + v];
                /* 内联反量化（int64 精确 + 防御钳位，与 tc_dequant_block_ctx
                 * 同式；c0==0 → p=0，无分支） */
                int64_t p = (int64_t)c0 * (int64_t)Q[u * 8 + v];
                if (p > (int64_t)TC_TRANSFORM_MAX_ABS_F) { p = (int64_t)TC_TRANSFORM_MAX_ABS_F; }
                else if (p < -(int64_t)TC_TRANSFORM_MAX_ABS_F) { p = -(int64_t)TC_TRANSFORM_MAX_ABS_F; }
                __m256i cv = _mm256_set1_epi64x(p);
                __m256i pr = _mm256_mul_epi32(cv, _mm256_loadu_si256((const __m256i*)kWMb[u][v]));
                if ((v & 1) == 0) { pe = _mm256_add_epi64(pe, pr); }
                else { po = _mm256_add_epi64(po, pr); }
            }
            b[u][0] = _mm256_add_epi64(pe, po);
            b[u][1] = _mm256_permute4x64_epi64(_mm256_sub_epi64(pe, po), _MM_SHUFFLE(0, 1, 2, 3));
        } else {
            b[u][0] = _mm256_setzero_si256();
            b[u][1] = _mm256_setzero_si256();
        }
    }

    /* B 16-bit 分解：B = BHi·2^16 + BLo。 */
    __m256i bhi[8][2];
    __m256i blo[8][2];
    {
        const __m256i m16 = _mm256_set1_epi64x((int64_t)0xFFFF);
        for (int u = 0; u < 8; ++u) {
            for (int g = 0; g < 2; ++g) {
                __m256i pair = _mm256_shuffle_epi32(b[u][g], _MM_SHUFFLE(3, 3, 1, 1));
                bhi[u][g] = _mm256_or_si256(_mm256_srli_epi32(b[u][g], 16),
                                            _mm256_slli_epi32(pair, 16));
                blo[u][g] = _mm256_and_si256(b[u][g], m16);
            }
        }
    }

    /* pass 3（lanes = x0..3 / x4..7）+ 对称输出 + round32（u 行全零 → 跳过） */
    const __m256i vr = _mm256_set1_epi64x((int64_t)1 << 31);
    const __m256i zero = _mm256_setzero_si256();
    for (int y = 0; y < 4; ++y) {
        for (int g = 0; g < 2; ++g) {
            __m256i qe_hi = zero, qe_lo = zero;
            __m256i qo_hi = zero, qo_lo = zero;
            for (int u = 0; u < 8; ++u) {
                if ((rowmask & (1u << u)) == 0u) { continue; } /* 加零项，跳过 */
                __m256i my = _mm256_set1_epi64x((int64_t)kM32[u][y]);
                __m256i hi = _mm256_mul_epi32(my, bhi[u][g]);
                __m256i lo = _mm256_mul_epi32(my, blo[u][g]);
                if ((u & 1) == 0) {
                    qe_hi = _mm256_add_epi64(qe_hi, hi);
                    qe_lo = _mm256_add_epi64(qe_lo, lo);
                } else {
                    qo_hi = _mm256_add_epi64(qo_hi, hi);
                    qo_lo = _mm256_add_epi64(qo_lo, lo);
                }
            }
            __m256i te = _mm256_add_epi64(_mm256_slli_epi64(qe_hi, 16), qe_lo);
            __m256i to = _mm256_add_epi64(_mm256_slli_epi64(qo_hi, 16), qo_lo);
            __m256i tp = _mm256_add_epi64(te, to);
            __m256i tm = _mm256_sub_epi64(te, to);
            TOPOS_ALIGNED32 int64_t op[4];
            TOPOS_ALIGNED32 int64_t om[4];
            {
                __m256i sg = _mm256_cmpgt_epi64(zero, tp);
                __m256i av = _mm256_sub_epi64(_mm256_xor_si256(tp, sg), sg);
                __m256i r = _mm256_srli_epi64(_mm256_add_epi64(av, vr), 32);
                _mm256_store_si256((__m256i*)op, _mm256_sub_epi64(_mm256_xor_si256(r, sg), sg));
            }
            {
                __m256i sg = _mm256_cmpgt_epi64(zero, tm);
                __m256i av = _mm256_sub_epi64(_mm256_xor_si256(tm, sg), sg);
                __m256i r = _mm256_srli_epi64(_mm256_add_epi64(av, vr), 32);
                _mm256_store_si256((__m256i*)om, _mm256_sub_epi64(_mm256_xor_si256(r, sg), sg));
            }
            for (int i = 0; i < 4; ++i) {
                xhat[y * 8 + g * 4 + i] = (int32_t)op[i];
                xhat[(7 - y) * 8 + g * 4 + i] = (int32_t)om[i];
            }
        }
    }
}

#endif /* x86_64 */

/* 非 x86_64 切片（universal 构建）编译为空：ISO 要求 TU 至少一个声明 */
typedef int topos_avx2_tu_placeholder;

/* ---- M10-2C：四块 SoA 融合反量化 + 逆变换（x86_64 专属，基线指令集外壳） ----
 * 与 tc_dequant_inverse_8x8_avx2 逐块 bit-exact：
 *  - 4 个 64-bit lane 各承载一个块的同一 (u,v) 系数（qsoa[64][4]，
 *    int32 值符号扩展入 lane）；每块反量化（q·Q + ±2^25 钳位）在各自
 *    lane 独立进行，与单块内核同式；
 *  - 权重（W·M[v][x]·标量）跨块广播共享：装载/钳位/B 16-bit 分解/
 *    round32 等按块固定的成本摊薄 4×（这些在单块内核中约占块成本四成）；
 *  - 行掩码取 4 块并集：并集外行全零 → lane 加零恒等（与单块内核的
 *    零行跳过数学等价，test_transform 差分钉死）；
 *  - B 分解的 dword 漏斗（shuffle 3,3,1,1 等）均为 lane 内局部运算，
 *    对 4 块 lane 布局同样成立；对称输出 x̂[7−y] = qe−qo 逐 lane 独立。
 * 溢出域与单块一致：|p| ≤ 2^25、|WM| < 2^18 → |B| < 2^46；pass3 和
 * < 2^52（64-bit lane 精确）。 */
#if defined(__x86_64__) || defined(_M_X64)

#include <immintrin.h>
#include <stdatomic.h>
#include <stddef.h>

#include "../transform/quant.h"
#include "../transform/transform.h"
#include "../transform/transform_tables.h"

#if defined(__GNUC__) || defined(__clang__)
TOPOS_SIMD_TARGET
#endif
void tc_dequant_inverse_8x8x4_avx2(const tc_quant_ctx* ctx, const int64_t* qsoa,
                                   int32_t* xh4, uint32_t rm_union)
{
    transform_avx2_tables_ensure();
    const uint32_t* Q = ctx->Q;

    const __m256i vl = _mm256_set1_epi64x((int64_t)-TC_TRANSFORM_MAX_ABS_F);
    const __m256i vh = _mm256_set1_epi64x((int64_t)TC_TRANSFORM_MAX_ABS_F);

    /* 系数装载 + 反量化：p[u][v] = clamp(q·Q)（4 块 lane）。
     * 钳位用 cmpgt 位混合（min/max_epi64 为 AVX512VL，AVX2 无）。 */
    __m256i p4[64];
    for (int i = 0; i < 64; ++i) {
        if ((rm_union & (1u << (i >> 3))) == 0u) { continue; } /* 并集外行恒零 */
        __m256i qv = _mm256_loadu_si256((const __m256i*)(qsoa + (size_t)i * 4u));
        __m256i pv = _mm256_mul_epi32(qv, _mm256_set1_epi64x((int64_t)Q[i]));
        __m256i over = _mm256_cmpgt_epi64(pv, vh);
        __m256i under = _mm256_cmpgt_epi64(vl, pv);
        __m256i mask = _mm256_or_si256(over, under);
        pv = _mm256_or_si256(
            _mm256_and_si256(_mm256_or_si256(_mm256_and_si256(over, vh),
                                             _mm256_and_si256(under, vl)),
                             mask),
            _mm256_andnot_si256(mask, pv));
        p4[i] = pv;
    }

    /* pass B：b[u][x]（lanes = 4 块）；对称 B[u][7−x] = pe − po。
     * kWMb 不适用（其 lanes 承载 x 0..3）——此处 x 为循环变量，
     * 权重 W[u][v]·M[v][x] 为标量广播。 */
    __m256i b[8][8];
    for (int u = 0; u < 8; ++u) {
        if ((rm_union & (1u << u)) == 0u) {
            for (int x = 0; x < 8; ++x) { b[u][x] = _mm256_setzero_si256(); }
            continue;
        }
        for (int x = 0; x < 4; ++x) {
            __m256i pe = _mm256_setzero_si256();
            __m256i po = _mm256_setzero_si256();
            for (int v = 0; v < 8; ++v) {
                /* 行内 8 个 v 全处理：块间零系数以零 lane 参与（加零恒等），
                 * 与单块内核的行粒度跳过语义一致 */
                __m256i w = _mm256_set1_epi64x(
                    (int64_t)kTransformW[u * 8 + v] * (int64_t)kTransformM[v * 8 + x]);
                __m256i pr = _mm256_mul_epi32(p4[u * 8 + v], w);
                if ((v & 1) == 0) { pe = _mm256_add_epi64(pe, pr); }
                else { po = _mm256_add_epi64(po, pr); }
            }
            b[u][x] = _mm256_add_epi64(pe, po);
            b[u][7 - x] = _mm256_sub_epi64(pe, po);
        }
    }

    /* B 16-bit 分解（lane 内 dword 漏斗，与单块内核同式） */
    __m256i bhi[8][8];
    __m256i blo[8][8];
    {
        const __m256i m16 = _mm256_set1_epi64x((int64_t)0xFFFF);
        for (int u = 0; u < 8; ++u) {
            if ((rm_union & (1u << u)) == 0u) { continue; }
            for (int x = 0; x < 8; ++x) {
                __m256i pair = _mm256_shuffle_epi32(b[u][x], _MM_SHUFFLE(3, 3, 1, 1));
                bhi[u][x] = _mm256_or_si256(_mm256_srli_epi32(b[u][x], 16),
                                            _mm256_slli_epi32(pair, 16));
                blo[u][x] = _mm256_and_si256(b[u][x], m16);
            }
        }
    }

    /* pass 3（lanes = 4 块）+ 对称输出 + round32；r[y][x] = 4 块的 x̂[y][x] */
    const __m256i vr = _mm256_set1_epi64x((int64_t)1 << 31);
    const __m256i zero = _mm256_setzero_si256();
    __m256i r[8][8];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 8; ++x) {
            __m256i qe_hi = zero, qe_lo = zero, qo_hi = zero, qo_lo = zero;
            for (int u = 0; u < 8; ++u) {
                if ((rm_union & (1u << u)) == 0u) { continue; }
                __m256i my = _mm256_set1_epi64x((int64_t)kTransformM[u * 8 + y]);
                __m256i hi = _mm256_mul_epi32(my, bhi[u][x]);
                __m256i lo = _mm256_mul_epi32(my, blo[u][x]);
                if ((u & 1) == 0) {
                    qe_hi = _mm256_add_epi64(qe_hi, hi);
                    qe_lo = _mm256_add_epi64(qe_lo, lo);
                } else {
                    qo_hi = _mm256_add_epi64(qo_hi, hi);
                    qo_lo = _mm256_add_epi64(qo_lo, lo);
                }
            }
            __m256i te = _mm256_add_epi64(_mm256_slli_epi64(qe_hi, 16), qe_lo);
            __m256i to = _mm256_add_epi64(_mm256_slli_epi64(qo_hi, 16), qo_lo);
            __m256i tp = _mm256_add_epi64(te, to); /* x̂[y][x]（4 块） */
            __m256i tm = _mm256_sub_epi64(te, to); /* x̂[7−y][x]（4 块） */
            {
                __m256i sg = _mm256_cmpgt_epi64(zero, tp);
                __m256i av = _mm256_sub_epi64(_mm256_xor_si256(tp, sg), sg);
                __m256i rr = _mm256_srli_epi64(_mm256_add_epi64(av, vr), 32);
                r[y][x] = _mm256_sub_epi64(_mm256_xor_si256(rr, sg), sg);
            }
            {
                __m256i sg = _mm256_cmpgt_epi64(zero, tm);
                __m256i av = _mm256_sub_epi64(_mm256_xor_si256(tm, sg), sg);
                __m256i rr = _mm256_srli_epi64(_mm256_add_epi64(av, vr), 32);
                r[7 - y][x] = _mm256_sub_epi64(_mm256_xor_si256(rr, sg), sg);
            }
        }
    }

    /* 转置 r[8][8]（行 y 列 x，各 4 块 i64 lane）→ xh4[块][y*8+x]：
     * 1) permutevar8x32 取每 lane 低 dword（round32 输出值域 i32，符号在
     *    低 dword 完整——负数 i64 符号扩展的低 32 位即 two's complement 值）
     *    → a_x = 4 块的 x̂[y][x]（__m128i 4×i32）；
     * 2) SSE 4×8 转置（unpack_epi32/epi64 级联）→ 每块一行 8 个 i32，
     *    两个 16B 半行存储。 */
    {
        const __m256i vidx = _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6);
        for (int y = 0; y < 8; ++y) {
            __m128i a[8];
            for (int x = 0; x < 8; ++x) {
                a[x] = _mm256_castsi256_si128(
                    _mm256_permutevar8x32_epi32(r[y][x], vidx));
            }
            __m128i t0 = _mm_unpacklo_epi32(a[0], a[1]);
            __m128i t1 = _mm_unpackhi_epi32(a[0], a[1]);
            __m128i t2 = _mm_unpacklo_epi32(a[2], a[3]);
            __m128i t3 = _mm_unpackhi_epi32(a[2], a[3]);
            __m128i t4 = _mm_unpacklo_epi32(a[4], a[5]);
            __m128i t5 = _mm_unpackhi_epi32(a[4], a[5]);
            __m128i t6 = _mm_unpacklo_epi32(a[6], a[7]);
            __m128i t7 = _mm_unpackhi_epi32(a[6], a[7]);
            _mm_storeu_si128((__m128i*)(xh4 + 0u * 64u + (size_t)y * 8u),
                             _mm_unpacklo_epi64(t0, t2));
            _mm_storeu_si128((__m128i*)(xh4 + 1u * 64u + (size_t)y * 8u),
                             _mm_unpackhi_epi64(t0, t2));
            _mm_storeu_si128((__m128i*)(xh4 + 2u * 64u + (size_t)y * 8u),
                             _mm_unpacklo_epi64(t1, t3));
            _mm_storeu_si128((__m128i*)(xh4 + 3u * 64u + (size_t)y * 8u),
                             _mm_unpackhi_epi64(t1, t3));
            _mm_storeu_si128((__m128i*)(xh4 + 0u * 64u + (size_t)y * 8u + 4u),
                             _mm_unpacklo_epi64(t4, t6));
            _mm_storeu_si128((__m128i*)(xh4 + 1u * 64u + (size_t)y * 8u + 4u),
                             _mm_unpackhi_epi64(t4, t6));
            _mm_storeu_si128((__m128i*)(xh4 + 2u * 64u + (size_t)y * 8u + 4u),
                             _mm_unpacklo_epi64(t5, t7));
            _mm_storeu_si128((__m128i*)(xh4 + 3u * 64u + (size_t)y * 8u + 4u),
                             _mm_unpackhi_epi64(t5, t7));
        }
    }
}

#endif /* x86_64 SoA4 */

/* ======================= M10-6.3B：AVX2 量化（zigzag 直写 + 非零掩码） =======================

 * 语义 == scalar tc_quant_block_zigzag + 顺带产出 zigzag 序 64 位非零掩码
 * （bit sp = q_out[sp] != 0）。与 scalar bit-exact：
 *  - gather 按扫描序取 F[kTcZigzag[sp]]，量化数段（half_zz/dz_zz）同为扫描序；
 *  - clamp（vpmin/vpmax epi32）→ |·|（vpabsd）→ +half → 死区 max(num−dz,0)
 *    （num>dz?num−dz:0 ≡ max(num−dz,0)：num≤dz 时 num−dz≤0）；
 *  - 快速除法 q=(n·magic)>>51 的恒等 64 位分解：magic=(m1<<26)|m0，
 *    X=n·m1（≤2^51）、Y=n·m0（≤2^52）→
 *      q = (X>>25) + (((X&(2^25−1))<<26) + Y)>>51
 *    （纯整数恒等式；n=|F|+half < 2^25.1、magic ≤ 2^51+1 域内成立）；
 *  - 偶/奇 lane 各经一次 vpmuludq（ne/m1/m0 高 32 位右移入偶位），
 *    全程 64 位 lane 运算；末端 shuffle(0x08) 压低 32 位 + unpacklo_epi32
 *    交织复原 8×i32 顺序；
 *  - 符号恢复 vpsignd(q, F_clamped) == (F<0 ? −q : q)；零掩码 = eq 比较取反。
 */
#if defined(__x86_64__) || defined(_M_X64)
TOPOS_SIMD_TARGET
void tc_quant_block_zigzag_avx2(const tc_quant_ctx* ctx, const int32_t* F,
                                int32_t* q_out, uint64_t* nz_zz)
{
    const __m256i vmax = _mm256_set1_epi32(TC_TRANSFORM_MAX_ABS_F);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i lomask = _mm256_set1_epi32((int32_t)0xFFFFFFFFu);
    const __m256i m25 = _mm256_set1_epi64x((int64_t)(UINT64_C(0x1FFFFFF)));
    uint64_t nz = 0u;

    for (int v = 0; v < 8; ++v) {
        const __m256i gidx = _mm256_load_si256((const __m256i*)(kZzIdx32 + v * 8));
        __m256i f = _mm256_i32gather_epi32(F, gidx, 4);
        f = _mm256_min_epi32(_mm256_max_epi32(f, _mm256_sub_epi32(zero, vmax)), vmax);
        const __m256i mag = _mm256_abs_epi32(f);
        const __m256i num = _mm256_add_epi32(
            mag, _mm256_loadu_si256((const __m256i*)(ctx->half_zz + v * 8)));
        const __m256i nd = _mm256_sub_epi32(
            num, _mm256_loadu_si256((const __m256i*)(ctx->dz_zz + v * 8)));
        const __m256i ne = _mm256_max_epi32(nd, zero);

        const __m256i m1 = _mm256_loadu_si256((const __m256i*)(ctx->fd_zz_m1 + v * 8));
        const __m256i m0 = _mm256_loadu_si256((const __m256i*)(ctx->fd_zz_m0 + v * 8));
        const __m256i ne_e = _mm256_and_si256(ne, lomask);
        const __m256i ne_o = _mm256_srli_epi64(ne, 32);
        const __m256i m1_e = _mm256_and_si256(m1, lomask);
        const __m256i m1_o = _mm256_srli_epi64(m1, 32);
        const __m256i m0_e = _mm256_and_si256(m0, lomask);
        const __m256i m0_o = _mm256_srli_epi64(m0, 32);
        const __m256i x_e = _mm256_mul_epu32(ne_e, m1_e);
        const __m256i x_o = _mm256_mul_epu32(ne_o, m1_o);
        const __m256i y_e = _mm256_mul_epu32(ne_e, m0_e);
        const __m256i y_o = _mm256_mul_epu32(ne_o, m0_o);

        const __m256i q_e = _mm256_add_epi64(
            _mm256_srli_epi64(x_e, 25),
            _mm256_srli_epi64(
                _mm256_add_epi64(
                    _mm256_slli_epi64(_mm256_and_si256(x_e, m25), 26), y_e), 51));
        const __m256i q_o = _mm256_add_epi64(
            _mm256_srli_epi64(x_o, 25),
            _mm256_srli_epi64(
                _mm256_add_epi64(
                    _mm256_slli_epi64(_mm256_and_si256(x_o, m25), 26), y_o), 51));

        /* 偶/奇结果压回 8×i32：先各自压缩 64 位 lane 低 32 位（shuffle
         * 0x08 → [e0,e2|e4,e6] 布局）再一次 unpacklo_epi32 交织。
         * （直接对 q_e/q_o unpacklo 会取到 i32 元素 1 = E0 高 32 位——
         *  lane 2/3 恒 0，实测差分钉死。） */
        const __m256i q_e32 = _mm256_shuffle_epi32(q_e, 0x08);
        const __m256i q_o32 = _mm256_shuffle_epi32(q_o, 0x08);
        const __m256i q8 = _mm256_unpacklo_epi32(q_e32, q_o32);
        const __m256i qz = _mm256_sign_epi32(q8, f);
        _mm256_storeu_si256((__m256i*)(q_out + v * 8), qz);
        const uint32_t zbits = (uint32_t)_mm256_movemask_ps(
            _mm256_castsi256_ps(_mm256_cmpeq_epi32(qz, zero)));
        nz |= (uint64_t)(zbits ^ 0xFFu) << (v * 8);
    }
    *nz_zz = nz;
}

/* P-速⑥：自然序版——F 顺序读（免 8 次 gather）、q 顺序写、掩码为自然序
 * （bit k ↔ q_out[k]；调用方经 kTcZigzag 表翻译到扫描域）。数值与
 * tc_quant_block_zigzag_avx2 逐系数一致（同一数学，仅索引布局不同）。 */
TOPOS_SIMD_TARGET
void tc_quant_block_nat_avx2(const tc_quant_ctx* ctx, const int32_t* F,
                             int32_t* q_out, uint64_t* nz_nat)
{
    const __m256i vmax = _mm256_set1_epi32(TC_TRANSFORM_MAX_ABS_F);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i lomask = _mm256_set1_epi32((int32_t)0xFFFFFFFFu);
    const __m256i m25 = _mm256_set1_epi64x((int64_t)(UINT64_C(0x1FFFFFF)));
    uint64_t nz = 0u;

    for (int v = 0; v < 8; ++v) {
        __m256i f = _mm256_loadu_si256((const __m256i*)(F + v * 8));
        f = _mm256_min_epi32(_mm256_max_epi32(f, _mm256_sub_epi32(zero, vmax)), vmax);
        const __m256i mag = _mm256_abs_epi32(f);
        const __m256i num = _mm256_add_epi32(
            mag, _mm256_loadu_si256((const __m256i*)(ctx->half + v * 8)));
        const __m256i nd = _mm256_sub_epi32(
            num, _mm256_loadu_si256((const __m256i*)(ctx->dz + v * 8)));
        const __m256i ne = _mm256_max_epi32(nd, zero);

        const __m256i m1 = _mm256_loadu_si256((const __m256i*)(ctx->fd_nat_m1 + v * 8));
        const __m256i m0 = _mm256_loadu_si256((const __m256i*)(ctx->fd_nat_m0 + v * 8));
        const __m256i ne_e = _mm256_and_si256(ne, lomask);
        const __m256i ne_o = _mm256_srli_epi64(ne, 32);
        const __m256i m1_e = _mm256_and_si256(m1, lomask);
        const __m256i m1_o = _mm256_srli_epi64(m1, 32);
        const __m256i m0_e = _mm256_and_si256(m0, lomask);
        const __m256i m0_o = _mm256_srli_epi64(m0, 32);
        const __m256i x_e = _mm256_mul_epu32(ne_e, m1_e);
        const __m256i x_o = _mm256_mul_epu32(ne_o, m1_o);
        const __m256i y_e = _mm256_mul_epu32(ne_e, m0_e);
        const __m256i y_o = _mm256_mul_epu32(ne_o, m0_o);

        const __m256i q_e = _mm256_add_epi64(
            _mm256_srli_epi64(x_e, 25),
            _mm256_srli_epi64(
                _mm256_add_epi64(
                    _mm256_slli_epi64(_mm256_and_si256(x_e, m25), 26), y_e), 51));
        const __m256i q_o = _mm256_add_epi64(
            _mm256_srli_epi64(x_o, 25),
            _mm256_srli_epi64(
                _mm256_add_epi64(
                    _mm256_slli_epi64(_mm256_and_si256(x_o, m25), 26), y_o), 51));

        const __m256i q_e32 = _mm256_shuffle_epi32(q_e, 0x08);
        const __m256i q_o32 = _mm256_shuffle_epi32(q_o, 0x08);
        const __m256i q8 = _mm256_unpacklo_epi32(q_e32, q_o32);
        const __m256i qz = _mm256_sign_epi32(q8, f);
        _mm256_storeu_si256((__m256i*)(q_out + v * 8), qz);
        const uint32_t zbits = (uint32_t)_mm256_movemask_ps(
            _mm256_castsi256_ps(_mm256_cmpeq_epi32(qz, zero)));
        nz |= (uint64_t)(zbits ^ 0xFFu) << (v * 8);
    }
    *nz_nat = nz;
}
#endif /* x86_64 quant zz */
