/* C036 前置测量：rANS order-1 上下文建模天花板（dev-only）。
 *
 * 在生产量化/token 遍（fill_color_band_tokens / fill_color_band_from_f）
 * 内按解码序因果上下文累积条件联合直方图，逐 slice 以理想（未量化）
 * 模型计算 order-0 与各上下文模型的符号位数，折叠进全局累加器：
 *   a1(模型) ≤ a0(同族) 恒成立（条件熵不等式，单测钉死）。
 * 不改变任何编码输出；仅在 tc_dev_ctx_enable(1) 后生效（默认关，热路径
 * 仅一次 relaxed 原子读 + 分支）。测量口径与决策报告见
 * docs/codec/topos_ctx_ceiling_2026-09-10.md。
 *
 * 上下文域（全部为解码端零成本可得的因果状态——不扩位流语义也能用）：
 *   R1 run | 前一系数扫描位置桶(8)：0=块首，否则 1+min((prev-1)>>3,6)
 *   R2 run | 前一对 run 桶(7)：{0,1,2-3,4-7,8-15,≥16} + 块首(6)
 *   R3 run | 本块 DC 类桶(4)：{0,1,2-3,≥4}
 *   L1 lvl | 同对 run 桶(6)：{0,1,2-3,4-7,8-15,≥16}
 *   L2 lvl | 前一对 lvl 类桶(5)：{0,1,2-3,≥4} + 块首(4)
 *   L3 lvl | run 桶(6) × 前 lvl 桶(5)，ctx = run_b*5 + prev_lvl_b
 *   L4 lvl | 本系数扫描位置桶(4)：{1-7,8-15,16-31,32-63}
 *   D1 dc  | 前块 DC 类桶(5)：{首块,0,1,2-3,≥4}
 * RUN 族含 EOB（sym 63）；lvl 族不含 EOB。 */
#ifndef TOPOS_CODEC_CTX_CEILING_H
#define TOPOS_CODEC_CTX_CEILING_H

#include <stdint.h>

/* 累加器定长布局（C/Python/报告三方镜像；单位：bit，除非注明） */
#define TC_CTX_ACC_COUNT 19u
#define TC_CTX_ACC_SLICES    0u  /* 折叠的色度 slice 数 */
#define TC_CTX_ACC_A0_DC     1u  /* order-0 理想符号位（DC 族，逐 slice 求和） */
#define TC_CTX_ACC_A0_RUN    2u
#define TC_CTX_ACC_A0_LVL    3u
#define TC_CTX_ACC_A1_R1     4u  /* order-1 理想符号位（各模型） */
#define TC_CTX_ACC_A1_R2     5u
#define TC_CTX_ACC_A1_R3     6u
#define TC_CTX_ACC_A1_L1     7u
#define TC_CTX_ACC_A1_L2     8u
#define TC_CTX_ACC_A1_L3     9u
#define TC_CTX_ACC_A1_L4     10u
#define TC_CTX_ACC_A1_D1     11u
#define TC_CTX_ACC_ADAPT_RUN 12u /* 逐 slice min(order-0, 最优 ctx − 2bit 信令) */
#define TC_CTX_ACC_ADAPT_LVL 13u
#define TC_CTX_ACC_ADAPT_DC  14u
#define TC_CTX_ACC_SYM_DC    15u /* 符号计数（非 bit） */
#define TC_CTX_ACC_SYM_RUN   16u
#define TC_CTX_ACC_SYM_LVL   17u
#define TC_CTX_ACC_EOB       18u /* EOB 计数（run 子集） */

/* 每捕获（一个 slice）的条件联合直方图 + 游走状态 */
typedef struct tc_ctx_cap tc_ctx_cap;

tc_ctx_cap* tc_ctx_cap_alloc(void);
void tc_ctx_cap_free(tc_ctx_cap* c);
/* 每块一次（先于该块所有 pair/eob 调用；重置对内游走状态） */
void tc_ctx_dc(tc_ctx_cap* c, uint32_t dcm);
/* 每非零 AC 系数一次；prev/pos 为解码端已知的扫描位置（prev 先于更新） */
void tc_ctx_pair(tc_ctx_cap* c, uint32_t prev, uint32_t pos,
                 uint32_t runv, uint32_t lvl_m);
/* 每块一次（在最后一对之后；prev 为块内最终位置） */
void tc_ctx_eob(tc_ctx_cap* c, uint32_t prev);
/* 量化遍收尾：以该 slice 的族直方图（VLC/rANS 模式下即码表源）折叠 */
void tc_ctx_fold(tc_ctx_cap* c, const uint32_t* dc_hist,
                 const uint32_t* run_hist, const uint32_t* lvl_hist);

/* 自包含 log2（atanh 级数，|err| < 1.5e-6；x ≥ 1；不引 libm 依赖）。
 * 导出仅为单测钉精度。 */
double tc_ctx_log2(double x);

int  tc_dev_ctx_active(void);
void tc_dev_ctx_enable(int enable);
void tc_dev_ctx_reset(void);
/* 拷贝输出当前累加器（不重置）；长度须为 TC_CTX_ACC_COUNT */
void tc_dev_ctx_get(double* acc);

/* pooled 联合（跨 slice 汇总，仅供报告结构检视；布局 ctx*syms + sym） */
void tc_dev_ctx_pooled_r1(uint64_t* out); /* [8*64]  */
void tc_dev_ctx_pooled_l1(uint64_t* out); /* [6*28]  */
void tc_dev_ctx_pooled_l3(uint64_t* out); /* [30*28] */
void tc_dev_ctx_pooled_d1(uint64_t* out); /* [5*29]  */

#endif /* TOPOS_CODEC_CTX_CEILING_H */
