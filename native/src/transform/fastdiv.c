#include "transform/fastdiv.h"

void tc_fastdiv_init(uint32_t d, tc_fastdiv* out)
{
    out->divisor_fallback = d;
    if (d == 0u || d > TC_FASTDIV_D_MAX) {
        /* 域外（不可达：冻结 qm(≤655)×qp(≤95) 最大 Q = 18,045,205 < 2^25；
         * A.3 值域上界 4095 的假设矩阵在 qp 高段可超 → 走本回退，数值恒真）。
         * magic=0/shift=0 + divisor_fallback=d → apply 走精确真除（防御，
         * 数值正确；SIMD 孪生路径不达此处——见 quant.h 域不变量） */
        out->magic = 0u;
        out->shift = 0u;
        return;
    }
    if ((d & (d - 1u)) == 0u) { /* 2 的幂 */
        uint8_t s = 0u;
        while ((d >> s) != 1u) { s++; }
        out->magic = 0u;
        out->shift = s;
        return;
    }
    /* floor(2^51/d)+1；2^51 > N_MAX·d ⇒ 域内精确（见头文件推导） */
    out->magic = ((uint64_t)1 << 51) / (uint64_t)d + 1u;
    out->shift = 0u;
}
