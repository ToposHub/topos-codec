/* 16×16 混合变换（ADR-C037 试点）—— Haar2⊗M8，精确行正交整数。
 *
 * 规范模型与 8×8 相同（transform.h）：forward F = M16·x·M16ᵀ 全整数无
 * 舍入；inverse x̂ = round(M16ᵀ·(W16⊙coef)·M16 / 2^32)，符号拆分舍入。
 * 表由 tools/gen_transform16_tables.py 生成（含构造说明与可行性论证）。
 *
 * 界（生成器推导，测试复证）：
 *  - forward 输入 |x'| ≤ 2047 → 每趟行和 ≤ 208 → |F| < 2^26.4；
 *    防御钳位 TC_TRANSFORM16_MAX_ABS_F = 2^27（合法编码不触及）；
 *  - inverse 全部 s64 累加，|acc| < 2^53。
 *
 * 试点范围：仅 scalar 规范性实现（无 SIMD 分发、无 samples 变体）——
 * 测量工具 tools/bench_t16_ab.py 与单测使用；产品位流未接入。
 */
#ifndef TOPOS_INTERNAL_TRANSFORM16_H
#define TOPOS_INTERNAL_TRANSFORM16_H

#include <stdint.h>

#define TC_TRANSFORM16_MAX_ABS_F (1 << 27)

/* F = M16 · x' · M16ᵀ，两趟一维整数矩阵乘（无中间舍入）。
 * x: level shift 后的 16×16（行主序）；F: 未归一系数。 */
void tc_transform16_forward(const int16_t x[256], int32_t F[256]);

/* x̂' = round( (M16ᵀ · (coef ⊙ W16) · M16) / 2^32 )；coef 为反量化后的
 * 未加权系数（W16 内含 1/(E_uE_v) 归一）。 */
void tc_transform16_inverse(const int32_t coef[256], int32_t xhat[256]);

/* 调试/测试访问器：冻结表 */
const int16_t* tc_transform16_matrix(void);   /* kTransform16M[256] */
const uint32_t* tc_transform16_weights(void); /* kTransform16W[256] */
const uint32_t* tc_transform16_energies(void);/* kTransform16E[16]  */

#endif /* TOPOS_INTERNAL_TRANSFORM16_H */
