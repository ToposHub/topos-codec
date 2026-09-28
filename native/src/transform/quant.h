/* 量化/反量化（spec §7.3–§7.4，阶段 2）。全整数、表驱动。 */
#ifndef TOPOS_INTERNAL_QUANT_H
#define TOPOS_INTERNAL_QUANT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct tc_qmatrix_set {
    uint16_t luma[64];   /* 自然序 [u][v]，值域 1..4095 */
    uint16_t chroma[64];
} tc_qmatrix_set;

/* qmatrix_id → 表集；未知 id 返回 NULL（spec 附录 A.3）。
 * 0=flat，1=Standard，2=444 Compact，3=422 Low Compact。
 * id 0 = flat；id 1 = Standard；id 2 = 444 Compact（GBR 4:4:4 12-bit
 * 体积档，频率权重比 Standard 更强）。 */
const tc_qmatrix_set* tc_qmatrix_by_id(uint8_t qmatrix_id);

/* qp 合法域上界（v1.5 / ADR-C031：63 → 95；qp>63 仅 version_minor≥4 流合法）。
 * 帧头 qp_base、编码配置、slice 有效 qp 的统一校验常量。 */
#define TC_QP_MAX 95u
/* qp>63 = v1.5 扩展域（minor 门控判断用）。 */
#define TC_QP_V15_MIN 64u

/* 编码端 qp_eff（= qp_base + delta + 位深偏移）的钳位上界：
 * qp_base≤63 的流沿用 63（历史行为冻结——bd/AQ 偏移顶格压回 63，
 * 保证旧配置字节不变）；qp_base≥64 的 v1.5 流放开到 95。 */
static inline uint32_t tc_qp_eff_ceiling(uint32_t qp_base)
{
    return qp_base >= TC_QP_V15_MIN ? TC_QP_MAX : 63u;
}

/* qp_scale[qp]（spec 附录 A.4，冻结；0..63 = v1.0 表，64..95 = v1.5 扩展）；
 * qp > 95 返回 0（无效）。 */
uint32_t tc_qp_scale(uint32_t qp);

/* Q = max(1, ((qm·qp_scale)+128)>>8)（spec §7.3）；qp > 95 返回 0。 */
uint32_t tc_quant_step(uint16_t qm, uint32_t qp);

/* 前向量化：dz = 0（i==0，DC）否则 Q>>2；q = sign·max(0,(|F|+Q/2−dz)/Q)。
 * |F| 钳位到 TC_TRANSFORM_MAX_ABS_F（合法值 22,140,352 < 2^25）。 */
void tc_quant_block(const int32_t F[64], const uint16_t qm[64], uint32_t qp,
                    int32_t q_out[64]);

/* 反量化：F' = q·Q（s64 中转后钳位 ±2^25 —— 解码侧对非法大 q 的确定性防御；
 * 合法码流的域校验在阶段 3 位流层实施）。 */
void tc_dequant_block(const int32_t q[64], const uint16_t qm[64], uint32_t qp,
                      int32_t Fp_out[64]);

/* ---- 阶段 9：预计算量化上下文 ----
 * (qm, qp) 在编码端每 plane、解码端每 slice 恒定 → Q/dz/精确快速除法一次建表，
 * 块循环内零表算零 idiv。数值与 tc_quant_block/tc_dequant_block 完全一致
 * （test_stage9 差分复验；fastdiv 精确性推导见 fastdiv.h）。 */
#include "fastdiv.h"

typedef struct tc_quant_ctx {
    uint32_t Q[64];
    uint32_t half[64];
    uint32_t dz[64];
    /* 批 4（bd≥13）：量化/反量化的系数域钳位与精确除法标志——
     * fastdiv magic 域 N_MAX=2^25 只覆盖 12-bit 的 |F|+Q/2；bd16 的
     * n ≈ 2^28.5 越域 → exact_div=1 时标量环走真除法（SIMD 分发同步
     * 门禁，见 codec.c 调用方）。f_clamp 同 color_store（|F|max ∝ max|x'|）。 */
    int64_t f_clamp;
    uint8_t exact_div;
    uint8_t reserved_q[7];
    tc_fastdiv fd[64];
    /* M10-6.3B：zigzag 序孪生表（gather 型 SIMD 量化内核用）——半加/死区按
     * 扫描位置重排；快速除法拆成恒 magic 形式的 (m1<<26)|m0 两个 u32 乘数
     * （2 的幂 d=2^s 用 magic=2^(51−s)+1：n < 2^26 时 (n·magic)>>51 == n>>s，
     * 乘法路径统一无分支；数值与 fd 逐系数一致，推导见 simd/transform_avx2.c）。 */
    uint32_t half_zz[64];
    uint32_t dz_zz[64];
    uint32_t fd_zz_m1[64]; /* magic >> 26（< 2^25） */
    uint32_t fd_zz_m0[64]; /* magic & (2^26−1) */
    /* P-速⑥：自然序孪生（免 gather 量化内核用）——与 fd 同索引，magic 同拆 */
    uint32_t fd_nat_m1[64];
    uint32_t fd_nat_m0[64];
} tc_quant_ctx;

void tc_quant_ctx_init(tc_quant_ctx* ctx, const uint16_t qm[64], uint32_t qp);
/* 批 4：位深感知初始化（bd ≤ 12 与 init 逐位一致；bd ≥ 13 → exact_div +
 * 位深外推 f_clamp；SIMD 分发由调用方按 exact_div 门禁到标量环） */
void tc_quant_ctx_init_bd(tc_quant_ctx* ctx, const uint16_t qm[64], uint32_t qp,
                          uint8_t bit_depth);

/* qp_scale 表变体（V7-R5，位流 flags bit3 选载）：
 * TC_QPTBL_CLASSIC = 冻结经典表；TC_QPTBL_REFINED = 细化表（qp≤63 与
 * 经典逐值一致，64..95 每 6 qp 翻倍）。 */
#define TC_QPTBL_CLASSIC 0u
#define TC_QPTBL_REFINED 1u
uint32_t tc_qp_scale_tbl(uint32_t table_sel, uint32_t qp);
uint32_t tc_quant_step_tbl(uint32_t table_sel, uint16_t qm, uint32_t qp);

/* 表选择版 ctx 初始化（数值与 *_tbl 访问器一致）；init_bd = 经典表包装。 */
void tc_quant_ctx_init_bd_tbl(tc_quant_ctx* ctx, const uint16_t qm[64],
                              uint32_t qp, uint8_t bit_depth,
                              uint32_t table_sel);

/* 位流 flags bit3（V7-R5 细化表）→ 量化表选择（编解码共用）；
 * 非 V7-R5 流 flags 校验拒绝该位 → 恒经典表。 */
static inline uint32_t tc_qtbl_of_flags(uint16_t flags)
{
    return (flags & 0x0008u) != 0u ? TC_QPTBL_REFINED : TC_QPTBL_CLASSIC;
}
void tc_quant_block_ctx(const tc_quant_ctx* ctx, const int32_t F[64], int32_t q_out[64]);
void tc_dequant_block_ctx(const tc_quant_ctx* ctx, const int32_t q[64], int32_t Fp_out[64]);

/* 反量化后只计算指定块内样本点，结果与完整反量化+逆变换逐位一致。 */
void tc_dequant_inverse_samples(const tc_quant_ctx* ctx, const int32_t q[64],
                                const uint8_t* xs, const uint8_t* ys,
                                uint32_t count, int32_t* out);

/* 只反量化并逆变换扫描序 [0..max_scan_pos] 的系数；63 等同完整路径。
 * 用于固定比例低频预览，结果是近似重建。 */
void tc_dequant_inverse_samples_limited(const tc_quant_ctx* ctx, const int32_t q[64],
                                        const uint8_t* xs, const uint8_t* ys,
                                        uint32_t count, uint8_t max_scan_pos,
                                        int32_t* out);

/* 精确 1/2 预览采样：固定坐标 (0,2,4,6)×(0,2,4,6)，按行主序。 */
void tc_dequant_inverse_samples_half_limited(const tc_quant_ctx* ctx, const int32_t q[64],
                                             uint32_t count, uint8_t max_scan_pos,
                                             int32_t* out);

/* R6：量化直写 zigzag 序（q_out[kTcZigzagInv[i]] = q_i）。数值与
 * tc_quant_block_ctx 完全一致，仅存储布局不同——编码侧统计遍与熵编码遍
 * 均改为顺序扫描（test_r6 差分复验）。 */
void tc_quant_block_zigzag(const tc_quant_ctx* ctx, const int32_t F[64], int32_t q_out[64]);

#endif /* TOPOS_INTERNAL_QUANT_H */
