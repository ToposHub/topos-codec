#include "common/cpudetect.h"

#include "topos_codec.h"

#if defined(_WIN32) && (defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__))
#include <intrin.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define TOPOS_ARCH_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
#define TOPOS_ARCH_ARM 1
#endif

uint32_t tc_internal_cpu_flags(void)
{
    uint32_t flags = 0u;

#if defined(TOPOS_ARCH_X86)
/* clang-cl 同时定义 __clang__ 与 _MSC_VER：走 _MSC_VER 的 cpuid/xgetbv 路径，
 * 避免 __builtin_cpu_supports 依赖 compiler-rt 的 __cpu_model 运行时符号 */
#if defined(_WIN32) && defined(__clang__) && !defined(_MSC_VER)
    /* Zig 的 clang 前端支持 GCC 风格的 __builtin_cpu_* 语法，但不提供
     * GCC 的 __cpu_model/__cpu_indicator_init 运行时符号。Windows 下改用
     * clang/MinGW 兼容的 intrin.h，避免 DLL 链接到不存在的 libgcc 符号。 */
    int cpu[4] = { 0, 0, 0, 0 };
    __cpuid(cpu, 1);
    uint32_t ecx1 = (uint32_t)cpu[2];
    int has_osxsave = (ecx1 & (1u << 27)) != 0u;
    int has_avx = (ecx1 & (1u << 28)) != 0u;
    uint64_t xcr0 = has_osxsave ? (uint64_t)_xgetbv(0) : 0u;
    int ymm_ok = (xcr0 & 0x6u) == 0x6u;
    if (has_avx && ymm_ok) {
        if (ecx1 & (1u << 12)) { flags |= TOPOS_CPU_X86_FMA; }
        __cpuid(cpu, 0);
        if (cpu[0] >= 7) {
            __cpuidex(cpu, 7, 0);
            uint32_t ebx7 = (uint32_t)cpu[1];
            if (ebx7 & (1u << 5)) { flags |= TOPOS_CPU_X86_AVX2; }
            if ((ebx7 & (1u << 16)) && ((xcr0 & 0xE6u) == 0xE6u)) {
                flags |= TOPOS_CPU_X86_AVX512F;
            }
        }
    }
#elif (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2"))    { flags |= TOPOS_CPU_X86_AVX2; }
    if (__builtin_cpu_supports("avx512f")) { flags |= TOPOS_CPU_X86_AVX512F; }
    if (__builtin_cpu_supports("fma"))     { flags |= TOPOS_CPU_X86_FMA; }
#elif defined(_MSC_VER)
    /* 阶段 10 构建矩阵：MSVC cpuid/xgetbv 路径（保守判定，含 OS XSAVE 使能检查） */
    int cpu[4] = { 0, 0, 0, 0 };
    __cpuid(cpu, 1);
    uint32_t ecx1 = (uint32_t)cpu[2];
    int has_osxsave = (ecx1 & (1u << 27)) != 0u;
    int has_avx = (ecx1 & (1u << 28)) != 0u;
    uint64_t xcr0 = has_osxsave ? _xgetbv(_XCR_XFEATURE_ENABLED_MASK) : 0u;
    int ymm_ok = (xcr0 & 0x6u) == 0x6u;
    if (has_avx && ymm_ok) {
        if (ecx1 & (1u << 12)) { flags |= TOPOS_CPU_X86_FMA; }
        __cpuid(cpu, 0);
        if (cpu[0] >= 7) {
            __cpuidex(cpu, 7, 0);
            uint32_t ebx7 = (uint32_t)cpu[1];
            if (ebx7 & (1u << 5)) { flags |= TOPOS_CPU_X86_AVX2; }
            if ((ebx7 & (1u << 16)) && ((xcr0 & 0xE6u) == 0xE6u)) {
                flags |= TOPOS_CPU_X86_AVX512F;
            }
        }
    }
#else
    /* 未知编译器：保守返回 0（dispatch 自动退标量） */
#endif
#elif defined(TOPOS_ARCH_ARM)
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
    flags |= TOPOS_CPU_ARM_NEON;
#endif
#endif

    return flags;
}
