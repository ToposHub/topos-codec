/* 平面 padding/crop（spec §5，阶段 2）：非 8 倍数尺寸的确定性边缘复制填充
 * 与裁剪。stride 单位为 uint16 元素数。 */
#ifndef TOPOS_INTERNAL_PLANE_H
#define TOPOS_INTERNAL_PLANE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TC_PLANE_MAX_DIM 16384u

/* visible → coded（向上取 8 的倍数）；0 < visible ≤ 16384 否则返回 0（无效） */
uint32_t tc_plane_coded_from_visible(uint32_t visible);

/* 边缘复制填充：src（src_w×src_h）→ dst（dst_w×dst_h，≥ src 尺寸）。
 * 失败（参数非法/尺寸关系不满足）返回 false 且不写 dst。 */
bool tc_plane_pad_u16(const uint16_t* src, uint32_t src_w, uint32_t src_h,
                      size_t src_stride, uint16_t* dst, uint32_t dst_w,
                      uint32_t dst_h, size_t dst_stride);

/* 裁剪：src（≥ dst 尺寸）左上角 → dst。失败返回 false。 */
bool tc_plane_crop_u16(const uint16_t* src, uint32_t src_w, uint32_t src_h,
                       size_t src_stride, uint16_t* dst, uint32_t dst_w,
                       uint32_t dst_h, size_t dst_stride);

#endif /* TOPOS_INTERNAL_PLANE_H */
