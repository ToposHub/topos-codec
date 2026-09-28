/* 稀疏 basis 逆变换（M10-2B）——少量非零系数块的重建快路。
 *
 * 数学恒等：现行两遍逆变换中间无舍入，
 *   x̂[y][x] = round32( Σ_{u,v} clamp(q[u][v]·Q[u][v]) · W[u][v]·M[u][y]·M[v][x] )
 * 零系数项贡献为 0 → 只累加非零项与当前实现 bit-exact（整数环内重结合）。
 * 溢出论证：|p| ≤ 2^25（钳位域）、|K| = |W·M·M| ≤ 10228·169 < 2^21、
 * 项数 ≤ 65 → |acc| < 65·2^46 < 2^53，int64 精确。
 *
 * K 表 64×64 int32 = 16 KiB（进程级懒构建，单写者 CAS 模式，同
 * transform_avx2 的表发布次序）。基函数按 kTransformW/kTransformM 生成，
 * 与 AVX2/标量内核同一常量源。
 */
#ifndef TOPOS_INTERNAL_SPARSE_INVERSE_H
#define TOPOS_INTERNAL_SPARSE_INVERSE_H

#include <stdint.h>

/* 稀疏路径的非零系数上限（含 DC 计 1 + AC 上限 16；超过回退稠密内核）。
 * 真实 4K 密素材平均 ~8.75 非零 AC/块——上限覆盖主流形态。 */
#define TC_SPARSE_MAX_AC 16

/* 由 (反量化值, natural 序号) 对列表重建 xh[64]。
 * p[i] 为已按现行规则 clamp(q·Q) 的值；nat[i] ∈ [0,64)；nnz ≥ 1。
 * 返回值恒 TC_OK（纯计算，无失败路径）。 */
int32_t tc_sparse_inverse(const int32_t* p, const uint8_t* nat, uint32_t nnz,
                          int32_t xh[64]);

/* 基函数表就绪（热路径调用方入口一次；内部懒构建幂等） */
void tc_sparse_inverse_ensure(void);

#endif /* TOPOS_INTERNAL_SPARSE_INVERSE_H */
