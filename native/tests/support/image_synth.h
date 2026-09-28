/* image_synth —— 确定性测试图像合成器（阶段 4：单测 / golden_codec / topos_quality 共用）。
 *
 * xorshift64* 驱动（与 packet_synth 同源约定）：同一 cfg 产出 bit-exact 相同的平面，
 * 无墙钟、无未初始化读。内容按 kind 分五类，覆盖中间片典型负载：
 * 平场 / 双线性梯度 / 胶片颗粒（低频底 + 宽噪声）/ 高频细节 / 四象限混合。
 * 输出即 codec 输入契约：planar uint16、tight、Y/A 为 w×h、U/V 为 ceil(w/2)×h，
 * 颜色值域 0..1023（10-bit）、Alpha 0..65535。
 */
#ifndef TOPOS_TEST_IMAGE_SYNTH_H
#define TOPOS_TEST_IMAGE_SYNTH_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TC_SYNTH_FLAT = 0,
    TC_SYNTH_GRADIENT = 1,
    TC_SYNTH_GRAIN = 2,
    TC_SYNTH_DETAIL = 3,
    TC_SYNTH_MIXED = 4,
    TC_SYNTH_KIND_COUNT = 5
} tc_synth_kind;

typedef struct image_synth_cfg {
    uint64_t seed;
    uint32_t width;   /* visible luma 宽 */
    uint32_t height;  /* visible luma 高 */
    tc_synth_kind kind;
    uint32_t bit_depth; /* 0/10 = 10-bit（默认，历史输出逐字节不变）；12 =
                           * 图案 ×4 进 12-bit 值域（R4.1）。alpha 恒 16-bit 域 */
    uint32_t chroma_format; /* 0 = 4:2:2（默认，历史输出逐字节不变）；1 = 4:4:4
                             * U/V 全宽全高（R4.2） */
} image_synth_cfg;

/* 标准内容集（golden/report 统一引用；下标稳定，勿重排） */
enum {
    IMAGE_SYNTH_FLAT = 0,
    IMAGE_SYNTH_GRADIENT = 1,
    IMAGE_SYNTH_GRAIN = 2,
    IMAGE_SYNTH_DETAIL = 3,
    IMAGE_SYNTH_MIXED = 4,
    IMAGE_SYNTH_KIND_SET_COUNT = 5
};
const char* image_synth_kind_name(tc_synth_kind kind);

/* 生成可见平面（写入调用方缓冲；不需要的平面传 NULL）。
 * y: w×h；u/v: 4:2:2 为 ceil(w/2)×h、4:4:4（chroma_format=1）为 w×h；a: w×h。
 * 返回 0 成功，-1 参数非法。 */
int image_synth_build(const image_synth_cfg* cfg,
                      uint16_t* y, uint16_t* u, uint16_t* v, uint16_t* a);

/* 便捷：按 cfg 分配并生成四个平面（不需要 alpha 时 *a_out 为 NULL）。
 * 返回 0 成功（调用方 free 各平面），-1 参数非法，-2 OOM。 */
int image_synth_alloc(const image_synth_cfg* cfg, int with_alpha,
                      uint16_t** y_out, uint16_t** u_out, uint16_t** v_out,
                      uint16_t** a_out);

#endif /* TOPOS_TEST_IMAGE_SYNTH_H */
