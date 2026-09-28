/* packet_synth —— 确定性合法 elementary frame packet 合成器（测试/金标/fuzz corpus 共用）。
 *
 * 同一 cfg 产出 bit-exact 相同的包（xorshift64* 驱动，无墙钟/无未初始化读）。
 * golden_bitstream 与 fuzz corpus 都以此重建期望字节 —— 编码器输出一旦漂移立即被两道门拦下。
 */
#ifndef TOPOS_TEST_PACKET_SYNTH_H
#define TOPOS_TEST_PACKET_SYNTH_H

#include <stddef.h>
#include <stdint.h>

#include "topos_codec.h"

typedef struct packet_synth_cfg {
    uint64_t seed;
    uint16_t visible_w;
    uint16_t visible_h;
    uint8_t with_alpha;   /* 0/1 */
    uint16_t bands;       /* 每平面 slice 数（带数，自动均匀分割块行） */
    uint8_t qp_base;      /* 0..63 */
    uint8_t qmatrix_id;   /* 0/1/2/3 */
    uint8_t bit_depth;    /* 0→10 默认（kCfgs 历史输出不变）；12 = v1.2 扩展（R4.1） */
    uint8_t pixel_format; /* 0→4:2:2 默认（kCfgs 历史输出不变）；1 = 4:4:4 扩展（R4.2） */
    uint8_t profile;      /* 0→3 默认（kCfgs 历史输出不变）；5=Pro444；6=Extreme（R4.4） */
    uint8_t entropy_mode; /* 0→V1（kCfgs 历史输出不变）；1=V2+VLC（M9，major=2） */
} packet_synth_cfg;

/* 标准配置集（golden/corpus/单测统一引用；下标稳定，勿重排） */
enum {
    PACKET_SYNTH_CFG_MINIMAL = 0, /* 8x8 单块 */
    PACKET_SYNTH_CFG_TINY = 1,    /* 16x16 */
    PACKET_SYNTH_CFG_ODD = 2,     /* 36x20 非 8 倍数 → padding 路径 */
    PACKET_SYNTH_CFG_ALPHA = 3,   /* 64x32 + alpha + 多带 */
    PACKET_SYNTH_CFG_WIDE = 4,    /* 128x72 + alpha + 4 带 */
    PACKET_SYNTH_CFG_REMAINDER = 5, /* 48x48 + alpha + 5 带（非整除分割） */
    PACKET_SYNTH_CFG_COUNT = 6
};

const packet_synth_cfg* packet_synth_cfg_at(unsigned index);

/* 构造完整合法包（malloc；调用方 free(*out_data)）。
   TC_OK / TC_ERR_OUT_OF_MEMORY / 符号层错误码。 */
int32_t packet_synth_build(const packet_synth_cfg* cfg, uint8_t** out_data, size_t* out_size);

#endif /* TOPOS_TEST_PACKET_SYNTH_H */
