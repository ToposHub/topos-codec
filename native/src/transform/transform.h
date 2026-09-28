/* 8×8 整数变换（阶段 2，scalar 规范性实现）—— spec §7.2/§7.7，ADR-C002。
 *
 * 前置条件与界（spec §7.6，测试复证）：
 *  - forward 输入 |x'| ≤ 2047（12-bit level shift 后）→ |A| < 2^18，|F| ≤ 22,140,352 < 2^25；
 *  - inverse 输入 |coef| 会先钳位到 kInverseClamp；内部全部 s64 累加，|acc| < 2^53；
 *  - 全整数、无浮点、无未定义移位（负数不右移，见 tc_round_shift32）。
 */
#ifndef TOPOS_INTERNAL_TRANSFORM_H
#define TOPOS_INTERNAL_TRANSFORM_H

#include <stdint.h>

/* 防御性钳位（合法编码不会触及；见 ADR-C002 的分层安全说明） */
#define TC_TRANSFORM_MAX_ABS_F (1 << 25)

/* 批 4：宽位深（bd≥13）系数防御钳位。bd16 合法 |Fp| ≤ f_clamp ≈ 5.37e8 < 2^30；
 * 逆变换链 i64 域 |acc| ≤ 1.106e8·clamp ≈ 1.2e17 < 2^63（W·M 乘积 ≤ 132,964、
 * 两趟 8×13 求和 104²），钳位加宽不改变 |coef| ≤ 2^25 历史码流的任何位。
 * 注意：AVX2 全逆变换内核的 B 16-bit 分解域（|B|<2^46 → BHi 32 位）仅在
 * ±2^25 输入下成立 —— 该内核保持 12-bit 域，bd≥13 经 resolve 门控走标量；
 * 本宽钳位仅用于 scalar 逆变换与 samples 内核（i64 累加，域已证）。 */
#define TC_TRANSFORM_MAX_ABS_F_WIDE (1 << 30)

/* F = M · x' · Mᵀ，两趟一维整数矩阵乘（无中间舍入）。
 * x: level shift 后的 8×8（行主序）；F: 未归一系数。
 * 阶段 9 起经 SIMD 运行时分发（scalar 规范性参考，bit-exact）。 */
void tc_transform_forward_8x8(const int16_t x[64], int32_t F[64]);

/* 批 4：int32 输入前向（bd≥13 通用路；scalar 规范，|x| ≤ 2^(bd-1)-1） */
void tc_transform_forward_8x8_i32_scalar(const int32_t x[64], int32_t F[64]);

/* 批 4 阶段 2：i32 前向 SIMD 运行时分发（AVX2/NEON 内核与 scalar 在
 * bd≤16 全域 bit-exact —— i32 通道界 |A| ≤ 8·32767·13 < 2^23、
 * |F| ≤ 8·13·|A|max < 2^29，乘加全整数无舍入） */
void tc_transform_forward_8x8_i32(const int32_t x[64], int32_t F[64]);

/* 批 4 阶段 2：编码端系数预失真（16-bit 无损的数学保证；仅 bd≥13 编码路
 * 在前向之后、量化之前调用）。
 * 动机：W = round_half_up(2^32/(EuEv)) 的表项舍入使逆变换对每个系数带
 * |ε| ≤ 0.5/W 的相对增益偏差——12-bit 码域 <1 码不可见（批 0 试点 qp36
 * maxerr=0 的原因），16-bit 满摆 DC 偏差可放大到 ~9 码（2026-09-13
 * unit_codec 实测 maxerr=5），严格 maxerr=0 失效。
 * 机制：解码端每系数增益为 W/2^32 ≈ (1+ε)/(EuEv)；以
 * F' = round(F·2^32/(W·EuEv)) 预失真（恰抵消 (1+ε)）后，解码
 * round(Σ F'·W·M·M/2^32) 相对原块的总残差 ≤ 0.5·Σ_{u,v} M²/(EuEv)
 * ≈ 0.0079 ≪ 0.5 → 像素精确。
 * 仅编码侧取整自由度（码流语义 = 一组整数系数），解码器零改动；
 * bd ≤ 12 路不调用 → 既有 golden SHA 逐位不变。 */
void tc_transform_forward_predistort_i32(int32_t F[64]);

/* scalar 规范性实现（dispatch 回退与差分测试基准） */
void tc_transform_forward_8x8_scalar(const int16_t x[64], int32_t F[64]);
void tc_transform_inverse_8x8_scalar(const int32_t coef[64], int32_t xhat[64]);
void tc_transform_inverse_8x8_samples_scalar(const int32_t coef[64],
                                             const uint8_t* xs, const uint8_t* ys,
                                             uint32_t count, int32_t* out);
void tc_transform_inverse_8x8_samples_scalar_limited(const int32_t coef[64],
                                                     const uint8_t* xs, const uint8_t* ys,
                                                     uint32_t count, uint8_t max_scan_pos,
                                                     int32_t* out);

/* x̂' = round( (Mᵀ · (coef ⊙ W) · M) / 2^32 )，符号拆分舍入；
 * coef 为反量化后的未加权系数 F'（W 表内含 1/(E_uE_v) 归一）。 */
void tc_transform_inverse_8x8(const int32_t coef[64], int32_t xhat[64]);

/* 只求指定的 8×8 块内样本点。样本坐标均为 0..7，结果与完整逆变换在
 * 相同坐标的值逐位一致；当预览缩小到每块只有少量输出像素时，避免构造
 * 完整 64 像素块。 */
void tc_transform_inverse_8x8_samples(const int32_t coef[64],
                                      const uint8_t* xs, const uint8_t* ys,
                                      uint32_t count, int32_t* out);
void tc_transform_inverse_8x8_samples_limited(const int32_t coef[64],
                                               const uint8_t* xs, const uint8_t* ys,
                                               uint32_t count, uint8_t max_scan_pos,
                                               int32_t* out);

/* 调试/测试访问器：冻结表 */
const int16_t* tc_transform_matrix(void);   /* kTransformM[64] */
const uint32_t* tc_transform_weights(void); /* kTransformW[64] */
const uint32_t* tc_transform_energies(void);/* kTransformE[8]  */

#endif /* TOPOS_INTERNAL_TRANSFORM_H */
