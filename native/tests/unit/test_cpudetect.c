/* CPU 探测骨架：结构契约 + 位域语义（不绑定具体机器能力，保证可移植） */
#include "topos_codec.h"
#include "mini_test.h"

#include <string.h>

int main(void)
{
    topos_cpu_features cf;
    memset(&cf, 0, sizeof cf);

    MT_CHECK_EQ_I64(tc_query_cpu_features(&cf), TC_OK);
    MT_CHECK_EQ_U64(cf.struct_size, sizeof(topos_cpu_features));
    MT_CHECK_EQ_U64(cf.abi_version, TOPOS_CODEC_ABI_VERSION);

    /* 能力位只允许出自已知集合 */
    const uint32_t known = TOPOS_CPU_X86_AVX2 | TOPOS_CPU_X86_AVX512F |
                           TOPOS_CPU_ARM_NEON | TOPOS_CPU_X86_FMA;
    MT_CHECK_EQ_U64(cf.flags & ~known, 0u);

    /* 探测必须是确定性的：连查 8 次结果一致 */
    for (int i = 0; i < 8; ++i) {
        topos_cpu_features again;
        MT_CHECK_EQ_I64(tc_query_cpu_features(&again), TC_OK);
        MT_CHECK_EQ_U64(again.flags, cf.flags);
    }

    /* 互斥架构位不得同时置位（x86 位与 ARM 位互斥） */
    const uint32_t x86_bits = TOPOS_CPU_X86_AVX2 | TOPOS_CPU_X86_AVX512F | TOPOS_CPU_X86_FMA;
    if ((cf.flags & x86_bits) != 0u) {
        MT_CHECK((cf.flags & TOPOS_CPU_ARM_NEON) == 0u);
    }

    /* 开发基准机（i9-9900K）应报出 AVX2 —— 仅在 x86_64 下断言，其他 CI 平台跳过 */
#if defined(__x86_64__) && (defined(__APPLE__) || defined(__linux__)) && \
    (defined(__clang__) || defined(__GNUC__))
    MT_CHECK((cf.flags & TOPOS_CPU_X86_AVX2) != 0u);
#endif

    printf("cpu flags: 0x%08x (avx2=%d avx512f=%d fma=%d neon=%d)\n",
           cf.flags,
           (cf.flags & TOPOS_CPU_X86_AVX2) ? 1 : 0,
           (cf.flags & TOPOS_CPU_X86_AVX512F) ? 1 : 0,
           (cf.flags & TOPOS_CPU_X86_FMA) ? 1 : 0,
           (cf.flags & TOPOS_CPU_ARM_NEON) ? 1 : 0);

    return MT_MAIN_RETURN();
}
