/* ADR-C030：V7-B shared transform pyramid —— scalar 参考分析/合成。
 *
 * 全部算术按 ADR-C030 冻结：整数 lifting、floor/rdiv3 公式、尾组补位、
 * 单点钳位（仅最终 base 平面）+ 稠密逃逸通道。内部中间量以 int32 承载，
 * 级联表 {2:[f2], 3:[f3], 4:[f2,f2], 6:[f3,f2]} 冻结。
 *
 * 所有权约定（P1-03）：tc_pyramid_plane 唯一拥有 base、全部子带系数与
 * 逃逸数组；分析过程不产生第二份源尺寸像素平面（staging 每级复用）。
 * 释放必须经 tc_pyramid_plane_release。
 */
#ifndef TOPOS_INTERNAL_PYRAMID_TRANSFORM_H
#define TOPOS_INTERNAL_PYRAMID_TRANSFORM_H

#include <stddef.h>
#include <stdint.h>

#include "topos_codec.h"

/* 源:base 每轴比例（plan §1.1 主表档位）。 */
typedef enum tc_pyramid_ratio {
    TC_PYRAMID_RATIO_2 = 2,
    TC_PYRAMID_RATIO_3 = 3,
    TC_PYRAMID_RATIO_4 = 4,
    TC_PYRAMID_RATIO_6 = 6
} tc_pyramid_ratio;

/* 冻结级联的每级因子（factor[i] 作用于第 i 级输入；i=0 为源平面）。 */
#define TC_PYRAMID_MAX_LEVELS 2u

typedef struct tc_pyramid_band {
    int32_t* coeffs;     /* 有符号高频系数，dense，stride == width */
    uint32_t width;
    uint32_t height;
} tc_pyramid_band;

/* 每级 3（N=2: LH/HL/HH）或 8（N=3: 除 LL 外 8 相位）个子带。
 * 子带序 = 字典序 (hphase, vphase) 行优先、跳过 (L,L)，见 ADR §2.5。 */
#define TC_PYRAMID_MAX_BANDS_PER_LEVEL 8u

typedef struct tc_pyramid_level {
    uint32_t input_width;    /* 本级输入平面几何（级 0 = 源平面） */
    uint32_t input_height;
    uint32_t factor;         /* 本级 lifting 因子（2 或 3），分析时冻结 */
    uint32_t ll_width;       /* 本级输出 LL 几何（末级 LL = base） */
    uint32_t ll_height;
    uint32_t band_count;
    tc_pyramid_band bands[TC_PYRAMID_MAX_BANDS_PER_LEVEL];
} tc_pyramid_level;

typedef struct tc_pyramid_plane {
    uint32_t source_width;
    uint32_t source_height;
    uint8_t bit_depth;
    uint32_t level_count;
    tc_pyramid_level levels[TC_PYRAMID_MAX_LEVELS];
    uint16_t* base;          /* 钳位后 uint16 低频平面（stride == base_width） */
    uint32_t base_width;
    uint32_t base_height;
    int32_t* escape;         /* base 几何稠密逃逸 = raw - clamp(raw) */
} tc_pyramid_plane;

/* 冻结级联表查询。返回 TC_OK 并填 base/base_level 几何；ratio 非法、
 * source 几何为 0 或超上限返回对应错误码。允许 ragged 尺寸（级内按
 * ADR 尾组规则处理）；产品 writer 在更上层强制整除比例。 */
int32_t tc_pyramid_geometry(tc_pyramid_ratio ratio,
                            uint32_t source_width, uint32_t source_height,
                            uint32_t* base_width, uint32_t* base_height,
                            uint32_t* level_count);

/* 单遍源平面分析。src 为 uint16 平面（元素步长 source_stride）。out 必须
 * 未持有资源（调用方先 memset 0）。无量化路径：base ⊕ escape ⊕ bands
 * 经 tc_pyramid_synthesize_plane 逐位还原 src。 */
int32_t tc_pyramid_analyze_plane(const uint16_t* src, size_t source_stride,
                                 uint32_t source_width, uint32_t source_height,
                                 uint8_t bit_depth, tc_pyramid_ratio ratio,
                                 tc_pyramid_plane* out);

/* 合成：base + escape + bands → dst（逐位等于分析输入）。dst 平面几何
 * 必须等于 source 几何。pyr 的 base 内容允许被替换（例如换成解码后的
 * base 平面），此时输出为有损重建——逃逸与 bands 语义不变。 */
int32_t tc_pyramid_synthesize_plane(const tc_pyramid_plane* pyr,
                                    uint16_t* dst, size_t dst_stride);

void tc_pyramid_plane_release(tc_pyramid_plane* pyr);

#endif /* TOPOS_INTERNAL_PYRAMID_TRANSFORM_H */
