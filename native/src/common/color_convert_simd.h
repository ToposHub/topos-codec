/* color_convert 的 SIMD 内核接口（u8 输入族）。
 *
 * 位精确契约：AVX2 内核与 color_convert.c 的标量路径**逐位一致**——
 *  - 浮点除法用 _mm256_div_ps（IEEE 正确舍入 = 标量 divss 逐位一致），
 *    禁止换成乘倒数；
 *  - 舍入用 _mm256_round_ps(NEAREST_EVEN) = rintf 默认舍入模式；
 *  - 逐算子顺序与标量 cvt_yuv_pixel/cvt_q 完全对应，全部显式内联函数
 *    （无 C 浮点表达式 → 无 FMA 收缩风险）；
 *  - 行尾 <8 像素与奇宽边缘经 tail 回调落回标量路径（同一份冻结数学，
 *    零重复）。
 * 分发：color_convert.c 的 cvt_rows 按运行时 CPU 标志 + dev 覆盖选择。 */
#ifndef TOPOS_INTERNAL_COLOR_CONVERT_SIMD_H
#define TOPOS_INTERNAL_COLOR_CONVERT_SIMD_H

#include <stdint.h>

/* 行尾标量回退：处理行 y 中 x ≥ x0 的像素（x0 恒为偶数——422 配对安全）。
 * user = 调用方（color_convert.c）的 cvt_ctx。 */
typedef void (*tc_cvt_tail_fn)(void* user, uint32_t y, uint32_t x0);

/* u8 输入族 SIMD 上下文（由 cvt_ctx 逐字段镜像填充；float 值原样传递）。 */
typedef struct tc_cvt_simd_ctx {
    const uint8_t* src;
    uint32_t src_stride;   /* 字节/行（已解析，非 0） */
    uint16_t* y_out;
    uint16_t* u_out;
    uint16_t* v_out;
    uint16_t* a_out;       /* NULL = 无 alpha 输出 */
    uint32_t y_st, u_st, v_st, a_st;
    uint32_t width;
    uint32_t mode;         /* TOPOS_CVT_OUT_* */
    uint32_t nch;          /* 3 或 4 */
    uint32_t ro, go, bo;   /* 通道偏移（样本 = 字节） */
    float    norm_div;     /* 255.0f（u8 族恒定） */
    float    kr, kb, kf;   /* kf = (float)(1-kr-kb)（double 求值一次舍入） */
    float    du, dv;
    float    y_mult, y_off, c_mult, c_off;
    float    scale;
    float    a_nmax;
    uint32_t alpha_shift;
    int      opaque;       /* alpha_opaque 或 nch==3 */
} tc_cvt_simd_ctx;

/* AVX2 可用性（运行时 cpuid；非 x86_64 编译切片恒 0）。 */
int tc_cvt_have_avx2(void);

/* dev 覆盖：-1=自动（默认，按 CPU 标志）/ 0=强制标量 / 1=强制 AVX2。
 * 单测奇偶校验用（强制 1 在无 AVX2 的机器上未定义——调用方自担）。 */
void tc_dev_cvt_simd_set(int32_t mode);

/* 处理行区间 [y0, y1) 的向量宽度主体；行尾经 tail 回调补齐。 */
void tc_cvt_rows_u8_avx2(const tc_cvt_simd_ctx* c, uint32_t y0, uint32_t y1,
                         tc_cvt_tail_fn tail, void* user);

#endif /* TOPOS_INTERNAL_COLOR_CONVERT_SIMD_H */
