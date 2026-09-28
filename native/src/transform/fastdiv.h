/* 有界精确快速除法（round-up magic，阶段 9）—— 量化热点的纯优化路径。
 *
 * 域与正确性（test_stage9 差分覆盖 + 下列推导）：
 *  - 被除数 n ≤ TC_FASTDIV_N_MAX；除数 d ∈ [1, TC_FASTDIV_D_MAX]；
 *  - d 为 2 的幂 → magic==0，直接移位；
 *  - 否则 magic = floor(2^51/d)+1，q = (n·magic) >> 51。取 N_MAX·d < 2^50
 *    ≪ 2^51（v1.5/ADR-C031：N_MAX=D_MAX=2^25−1；另需 d² < 2^51 保证
 *    n=d−1 处 floor 不越界——d < 2^25.5 才精确，D_MAX 取 2^25−1 留半位余量）：
 *    记 e = magic·d − 2^51 ∈ [1, d]，则 n·magic = (n·2^51 + n·e)/d，
 *    n·e/(d·2^51) < n·d/(d·2^51) = n/2^51 < 1/d，故对一切 n ≤ N_MAX
 *    floor(n·magic / 2^51) == floor(n/d)（进位不会跨过下一个整数）。
 *    N_MAX 依据：量化调用方 n = |F|+Q/2 ≤ 2^24.4+2^23.1 < 2^25（Q 最大
 *    18,045,205 = qm655×qp95_scale>>8，冻结表上界）；旧 N_MAX=2^27 对应
 *    d<2^23，v1.5 高 qp 域步长超 2^23 后收紧 N 并同步扩 D。
 *  - 乘法经 unsigned __int128（clang/gcc x86_64/arm64 均有；MSVC 走除法兜底）。
 */
#ifndef TOPOS_INTERNAL_FASTDIV_H
#define TOPOS_INTERNAL_FASTDIV_H

#include <stdint.h>

#if defined(_MSC_VER) && defined(_M_X64) && !defined(__SIZEOF_INT128__)
#include <intrin.h> /* _umul128（tc_fastdiv_apply 的 MSVC 乘高位路径） */
#endif

#define TC_FASTDIV_N_MAX ((1u << 25) - 1u)
#define TC_FASTDIV_D_MAX ((1u << 25) - 1u)

typedef struct tc_fastdiv {
    uint64_t magic; /* 0 表示 2 的幂（用 shift） */
    uint32_t divisor_fallback;
    uint8_t shift;
} tc_fastdiv;

void tc_fastdiv_init(uint32_t d, tc_fastdiv* out);

static inline uint32_t tc_fastdiv_apply(uint32_t n, const tc_fastdiv* fd)
{
#if defined(__SIZEOF_INT128__)
    if (fd->magic != 0u) {
        return (uint32_t)(((unsigned __int128)n * fd->magic) >> 51);
    }
    if (fd->shift != 0u) {
        return n >> fd->shift;       /* 2 的幂（d=2^k, k≥1） */
    }
    /* magic==0 且 shift==0：d=1（恒等）或域外防御（d>D_MAX → init 的
     * 保守回退）——后者必须真除，不得当 2^0 幂处理（v1.5 修复：
     * 域外曾静默返回 n 本身 → 量化电平未除尽，码率暴涨且重建失真） */
    return fd->divisor_fallback <= 1u ? n : n / fd->divisor_fallback;
#elif defined(_MSC_VER) && defined(_M_X64)
    /* MSVC 无 __int128：_umul128 高 64 位实现同语义（product>>51 ==
     * hi<<13 | lo>>51，逐位一致），免去每符号一次 u32 idiv */
    if (fd->magic != 0u) {
        uint64_t hi = 0;
        uint64_t lo = _umul128((uint64_t)n, fd->magic, &hi);
        return (uint32_t)((hi << 13) | (lo >> 51));
    }
    if (fd->shift != 0u) {
        return n >> fd->shift;
    }
    return fd->divisor_fallback <= 1u ? n : n / fd->divisor_fallback;
#else
    return n / fd->divisor_fallback; /* 平台兜底：数值恒等（阶段 10 MSVC 矩阵） */
#endif
}

#endif /* TOPOS_INTERNAL_FASTDIV_H */
