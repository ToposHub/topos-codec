/* 稀疏 basis 逆变换实现（M10-2B）——设计依据见 sparse_inverse.h。 */
#include "sparse_inverse.h"

#include <stdatomic.h>
#include <stddef.h>

#include "transform.h"
#include "transform_tables.h"

/* K[nat][j]：nat = natural 序系数位（u=nat/8, v=nat%8），j = y*8+x 输出位。
 * 值 = W[u][v] · M[u][y] · M[v][x]（int32；|值| < 2^21）。 */
static int32_t s_basis[64][64];
static atomic_int s_basis_state; /* 0 未生成 / 1 生成中 / 2 就绪 */

static void sparse_basis_fill(void)
{
    for (int nat = 0; nat < 64; ++nat) {
        const int u = nat >> 3;
        const int v = nat & 7;
        const int32_t w = (int32_t)kTransformW[nat];
        for (int y = 0; y < 8; ++y) {
            const int32_t my = (int32_t)kTransformM[u * 8 + y];
            for (int x = 0; x < 8; ++x) {
                const int32_t mx = (int32_t)kTransformM[v * 8 + x];
                s_basis[nat][y * 8 + x] = w * my * mx;
            }
        }
    }
    atomic_store(&s_basis_state, 2); /* 表写完成后再发布（先序） */
}

void tc_sparse_inverse_ensure(void)
{
    if (atomic_load_explicit(&s_basis_state, memory_order_acquire) == 2) { return; }
    int expected = 0;
    if (atomic_compare_exchange_strong(&s_basis_state, &expected, 1)) {
        sparse_basis_fill(); /* 唯一写者（确定性数据；并发读者见 2 前不进表） */
        return;
    }
    while (atomic_load_explicit(&s_basis_state, memory_order_acquire) != 2) {
        /* 等待发布（自旋极短） */
    }
}

int32_t tc_sparse_inverse(const int32_t* p, const uint8_t* nat, uint32_t nnz,
                          int32_t xh[64])
{
    tc_sparse_inverse_ensure();
    int64_t acc[64];
    for (uint32_t j = 0u; j < 64u; ++j) { acc[j] = 0; }
    for (uint32_t i = 0u; i < nnz; ++i) {
        const int64_t pi = (int64_t)p[i];
        const int32_t* k = s_basis[nat[i]];
        /* 64 输出累加（int32×int32→int64；编译器可向量化 vpmuldq 形态） */
        for (uint32_t j = 0u; j < 64u; ++j) {
            acc[j] += pi * (int64_t)k[j];
        }
    }
    for (uint32_t j = 0u; j < 64u; ++j) {
        const int64_t v = acc[j];
        if (v >= 0) {
            xh[j] = (int32_t)((v + ((int64_t)1 << 31)) >> 32);
        } else {
            xh[j] = -(int32_t)((-v + ((int64_t)1 << 31)) >> 32);
        }
    }
    return 0; /* TC_OK：纯计算无失败路径（头文件契约） */
}
