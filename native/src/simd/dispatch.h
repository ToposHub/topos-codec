/* SIMD 运行时分发（阶段 9）—— 内部 dev API。
 *
 * 设计（ADR-C009）：
 *  - scalar 恒为规范性参考（spec §1：可替换执行后端不得改变码流与重建）；
 *  - 分发表进程内一次初始化（C11 atomics release/acquire），函数指针只读；
 *  - tc_dev_set_simd_mode 供差分测试强制后端（0=auto 1=scalar 2=simd），
 *    属内部接口（macOS/Linux dylib 默认导出；公共 ABI 不含）；
 *  - SIMD 内核编译为 target attribute 局部启用（库本体仍为基线指令集，
 *    无 AVX2 CPU 上加载/运行不受影响）。
 */
#ifndef TOPOS_INTERNAL_SIMD_DISPATCH_H
#define TOPOS_INTERNAL_SIMD_DISPATCH_H

#include <stddef.h>
#include <stdint.h>

/* 模式 */
enum {
    TC_SIMD_AUTO = 0,
    TC_SIMD_SCALAR = 1,
    TC_SIMD_FORCE = 2,
};

struct tc_quant_ctx;

/* 经分发的变换入口（transform.c 调用；语义与直接调用标量版一致） */
void tc_simd_forward_8x8(const int16_t x[64], int32_t F[64]);
void tc_simd_inverse_8x8(const int32_t coef[64], int32_t xhat[64]);

/* 批 4 阶段 2：int32 输入前向分发（bd≥13 通用路）。AVX2/NEON 内核与
 * tc_transform_forward_8x8_i32_scalar 在 bd≤16 全域 bit-exact（i32 通道界
 * |A| < 2^23、|F| < 2^29，全整数无舍入；test_transform 三路差分钉死）。 */
void tc_simd_forward_8x8_i32(const int32_t x[64], int32_t F[64]);

/* 采样逆变换：一次并行计算最多 4 个目标样本，结果与 scalar 逐位一致。 */
void tc_simd_inverse_8x8_samples(const int32_t coef[64], const uint8_t* xs,
                                  const uint8_t* ys, uint32_t count, int32_t* out);
/* RD3-02：只遍历固定低频 band 的 zigzag 系数（B0/B1/B2/B3 或 full）。 */
void tc_simd_inverse_8x8_samples_limited(const int32_t coef[64],
                                          const uint8_t* xs, const uint8_t* ys,
                                          uint32_t count, uint8_t max_scan_pos,
                                          int32_t* out);
/* 精确 1/2 预览采样：固定输出坐标 (0,2,4,6)×(0,2,4,6)，按行主序。
 * count 为有效输出样本数（边缘块可小于 16）；无专用 SIMD 内核时复用
 * 上面的通用 limited 入口。 */
void tc_simd_inverse_8x8_samples_half_limited(const int32_t coef[64],
                                               uint32_t count, uint8_t max_scan_pos,
                                               int32_t* out);
/* 精确 1/2 采样的融合反量化 + 逆变换；count 同上。 */
void tc_simd_dequant_inverse_samples_half_limited(const struct tc_quant_ctx* ctx,
                                                  const int32_t q[64], uint32_t count,
                                                  uint8_t max_scan_pos, int32_t* out);

/* P-速②：u16 平面行直载 forward（8×8 块首行指针 + 行距 u16 元素数）。
 * 语义 == 逐元素 (int16_t)((int32_t)src[y*stride+x] − mid) 后
 * tc_transform_forward_8x8。无 AVX2 后端时为 gather+forward compose 回退。
 * 批 4：wide != 0（bd≥13）时返回 i32 域 compose（减 mid 后走
 * tc_simd_forward_8x8_i32，i16 截断域不复存在）。解析周期同 quant_zz。 */
typedef void (*tc_forward_rows_fn)(const uint16_t* src, size_t stride,
                                   int32_t mid, int32_t F[64]);
tc_forward_rows_fn tc_simd_resolve_forward_rows(uint8_t wide);

/* 融合反量化 + 逆变换（解码深化批次；解码 sink 热路径）。
 * 语义 == tc_dequant_block_ctx + tc_simd_inverse_8x8（AVX2 后端另含零行
 * 跳过的恒等优化）；无 SIMD 后端时为 compose 回退。
 * ac_rowmask：自然序非零行掩码（调用方解码侧给出；须为实际非零行的
 * 超集——0 行不处理，数学上与处理零行等价）。 */
void tc_simd_dequant_inverse_8x8(const struct tc_quant_ctx* ctx, const int32_t q[64],
                                 int32_t xhat[64], uint32_t ac_rowmask);

/* 已解析内核类型（M10-1d：slice/frame 入口一次解析，块循环免 atomic）。 */
typedef void (*tc_dequant_inverse_fn)(const struct tc_quant_ctx* ctx, const int32_t q[64],
                                      int32_t xhat[64], uint32_t ac_rowmask);

/* 一次完成 SIMD dispatch 与内核表初始化（AVX2 表懒生成），返回直接可调的
 * 融合反量化内核；无融合后端时返回 compose 回退包装。
 * 批 4：wide != 0（bd≥13，|Fp| 越出 SIMD 内核 2^25 域）时返回标量 compose
 * （tc_dequant_block_ctx f_clamp 钳位 + scalar 逆变换宽钳位）。
 * 返回值按调用方解析周期有效（frame/slice 级）——tc_dev_set_simd_mode
 * 重选后需重新解析才生效（解码每帧入口重取，满足差分测试语义）。 */
tc_dequant_inverse_fn tc_simd_resolve_dequant_inverse(uint8_t wide);

/* 批 4：宽位深融合反量化+逆变换的稳定入口（= 标量 compose；可直接取址
 * 作 fn 指针用，dec 帧内 per-frame 覆写用）。语义 ==
 * tc_dequant_block_ctx + tc_transform_inverse_8x8_scalar（宽钳位）。 */
void tc_simd_dequant_inverse_wide(const struct tc_quant_ctx* ctx, const int32_t q[64],
                                  int32_t xhat[64], uint32_t ac_rowmask);

/* M10-2C：四块 SoA 批量反量化+逆变换。qsoa[64][4]（int64 元素 = int32 值
 * 符号扩展）；xh4 = [4][64] 连续输出；rm_union = 4 块行掩码并集。
 * 与逐块 tc_simd_dequant_inverse_8x8 bit-exact（test_transform 差分）。 */
void tc_simd_dequant_inverse_8x8x4(const struct tc_quant_ctx* ctx, const int64_t* qsoa,
                                   int32_t* xh4, uint32_t rm_union);

/* M10-6.3B：gather 型量化（zigzag 直写 + 64 位非零掩码）。
 * 语义 == tc_quant_block_zigzag，另产出 nz_zz（bit sp = q_out[sp] != 0）；
 * 无 AVX2 后端时为 scalar 回退（量化同源 + 掩码顺序推导）。
 * 批 4：wide != 0（bd≥13，fastdiv n 域 ≤2^25 不覆盖）恒返回 scalar 回退
 * （tc_quant_block_zigzag exact_div 真除法路）。 */
typedef void (*tc_quant_zz_fn)(const struct tc_quant_ctx* ctx, const int32_t F[64],
                               int32_t q_out[64], uint64_t* nz_zz);
tc_quant_zz_fn tc_simd_resolve_quant_zz(uint8_t wide);

/* P-速⑥：自然序量化（F 顺序读、q 顺序写、掩码自然序 bit k ↔ q_out[k]；
 * zigzag 域翻译由调用方经 kTcZigzag/kTcZigzagInv 完成）。数值与
 * tc_simd_resolve_quant_zz 的内核逐系数一致。解析周期语义同上（wide 恒
 * scalar 回退）。 */
typedef void (*tc_quant_nat_fn)(const struct tc_quant_ctx* ctx, const int32_t F[64],
                                int32_t q_out[64], uint64_t* nz_nat);
tc_quant_nat_fn tc_simd_resolve_quant_nat(uint8_t wide);

/* 掩码翻译：自然序非零掩码 → 扫描序掩码（bit p = nat bit kTcZigzag[p]）。
 * 确定性纯表查（8×256 懒建表，进程内只读）。 */
uint64_t tc_zz_mask_from_nat(uint64_t nat_mask);

/* dev 强制模式（线程安全：原子发布；下一次调用生效） */
void tc_dev_set_simd_mode(int32_t mode);
int32_t tc_dev_simd_mode(void);

/* 当前生效后端描述："scalar" / "avx2" / "neon"（诊断/测试断言用） */
const char* tc_dev_simd_backend(void);

/* 本构建支持的 SIMD 后端存在性（编译期门控；测试用） */
int tc_simd_have_avx2(void);
int tc_simd_have_neon(void);

#endif /* TOPOS_INTERNAL_SIMD_DISPATCH_H */
