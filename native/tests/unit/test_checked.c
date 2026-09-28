/* checked arithmetic：边界 + 与编译器 builtin 的随机交叉验证 */
#include "common/checked.h"
#include "mini_test.h"

#include <stddef.h>

int main(void)
{
    uint32_t out32 = 0;
    uint64_t out64 = 0;
    size_t outs = 0;

    /* 加法边界 */
    MT_CHECK(!tc_uadd_u32(0xFFFFFFFFu, 1u, &out32));
    MT_CHECK(tc_uadd_u32(0xFFFFFFFFu, 0u, &out32) && out32 == 0xFFFFFFFFu);
    MT_CHECK(tc_uadd_u32(0u, 0u, &out32) && out32 == 0u);
    MT_CHECK(!tc_uadd_u64(0xFFFFFFFFFFFFFFFFull, 1ull, &out64));
    MT_CHECK(tc_uadd_u64(0xFFFFFFFFFFFFFFFFull, 0ull, &out64) &&
             out64 == 0xFFFFFFFFFFFFFFFFull);
    MT_CHECK(!tc_uadd_size(SIZE_MAX, 1, &outs));
    MT_CHECK(tc_uadd_size(0, SIZE_MAX, &outs) && outs == SIZE_MAX);

    /* 减法 */
    MT_CHECK(!tc_usub_size(0, 1, &outs));
    MT_CHECK(tc_usub_size(10, 10, &outs) && outs == 0);
    MT_CHECK(tc_usub_size(SIZE_MAX, SIZE_MAX, &outs) && outs == 0);

    /* 乘法边界 */
    MT_CHECK(!tc_umul_u32(0x10000u, 0x10000u, &out32));
    MT_CHECK(tc_umul_u32(65536u, 65535u, &out32) && out32 == 0xFFFF0000u);
    MT_CHECK(tc_umul_u32(0u, 0xFFFFFFFFu, &out32) && out32 == 0u);
    MT_CHECK(!tc_umul_u64(0x100000000ull, 0x100000000ull, &out64));
    MT_CHECK(tc_umul_u64(1ull, 0xFFFFFFFFFFFFFFFFull, &out64) &&
             out64 == 0xFFFFFFFFFFFFFFFFull);
    MT_CHECK(tc_umul_size(0, SIZE_MAX, &outs) && outs == 0);

    /* 失败路径不写 out（阶段 1 契约） */
    out32 = 0xDEADBEEFu;
    MT_CHECK(!tc_umul_u32(0x10000u, 0x10000u, &out32) && out32 == 0xDEADBEEFu);

    /* offset_in_bounds：无溢出构造判据 */
    MT_CHECK(tc_offset_in_bounds(10, 5, 5));
    MT_CHECK(!tc_offset_in_bounds(10, 5, 6));
    MT_CHECK(tc_offset_in_bounds(10, 10, 0));      /* off == limit，len == 0 合法 */
    MT_CHECK(!tc_offset_in_bounds(10, 11, 0));
    MT_CHECK(!tc_offset_in_bounds(0, SIZE_MAX, 0));            /* 超大尺寸拒绝 */
    MT_CHECK(!tc_offset_in_bounds(0, SIZE_MAX, SIZE_MAX));
    MT_CHECK(!tc_offset_in_bounds(SIZE_MAX, 1, SIZE_MAX));     /* off+len 溢出场景 */
    MT_CHECK(tc_offset_in_bounds(SIZE_MAX, SIZE_MAX, 0));

    /* 随机交叉验证（与 __builtin 溢出判定必须一致，10 万组） */
#if defined(__GNUC__) || defined(__clang__)
    for (int i = 0; i < 100000; ++i) {
        uint64_t r = mt_rand_u64();
        uint32_t a = (uint32_t)r, b = (uint32_t)(r >> 32);
        uint32_t mine = 0, theirs = 0;
        bool ok_mine = tc_umul_u32(a, b, &mine);
        bool ok_builtin = !__builtin_mul_overflow(a, b, &theirs);
        MT_CHECK(ok_mine == ok_builtin);
        if (ok_mine) { MT_CHECK(mine == theirs); }

        ok_mine = tc_uadd_u32(a, b, &mine);
        ok_builtin = !__builtin_add_overflow(a, b, &theirs);
        MT_CHECK(ok_mine == ok_builtin);
        if (ok_mine) { MT_CHECK(mine == theirs); }
    }
#endif

    return MT_MAIN_RETURN();
}
