#include "transform/plane.h"

#include <string.h>

#include "common/checked.h"

uint32_t tc_plane_coded_from_visible(uint32_t visible)
{
    if (visible == 0u || visible > TC_PLANE_MAX_DIM) { return 0u; }
    return ((visible + 7u) / 8u) * 8u;
}

static bool dims_valid(uint32_t w, uint32_t h, size_t stride)
{
    return w >= 1u && h >= 1u && w <= TC_PLANE_MAX_DIM && h <= TC_PLANE_MAX_DIM &&
           stride >= (size_t)w;
}

bool tc_plane_pad_u16(const uint16_t* src, uint32_t src_w, uint32_t src_h,
                      size_t src_stride, uint16_t* dst, uint32_t dst_w,
                      uint32_t dst_h, size_t dst_stride)
{
    if (src == NULL || dst == NULL) { return false; }
    if (!dims_valid(src_w, src_h, src_stride) || !dims_valid(dst_w, dst_h, dst_stride)) {
        return false;
    }
    if (dst_w < src_w || dst_h < src_h) { return false; }
    /* 尺寸上限内乘积仍走 checked 通道；16K²·2B 约 512 MiB，不能依赖
     * 32 位 size_t 或调用方的隐式乘法安全。 */
    size_t need = 0;
    if (!tc_umul_size(dst_stride, (size_t)dst_h, &need)) { return false; }
    (void)need;

    for (uint32_t y = 0; y < dst_h; ++y) {
        const uint32_t sy = y < src_h ? y : src_h - 1u;
        const uint16_t* srow = src + (size_t)sy * src_stride;
        uint16_t* drow = dst + (size_t)y * dst_stride;
        /* 可见区按行整拷（阶段 9：替代逐像素）；右边缘 ≤7 像素复制末列 */
        memcpy(drow, srow, (size_t)src_w * sizeof(uint16_t));
        const uint16_t edge = srow[src_w - 1u];
        for (uint32_t x = src_w; x < dst_w; ++x) { drow[x] = edge; }
    }
    return true;
}

bool tc_plane_crop_u16(const uint16_t* src, uint32_t src_w, uint32_t src_h,
                       size_t src_stride, uint16_t* dst, uint32_t dst_w,
                       uint32_t dst_h, size_t dst_stride)
{
    if (src == NULL || dst == NULL) { return false; }
    if (!dims_valid(src_w, src_h, src_stride) || !dims_valid(dst_w, dst_h, dst_stride)) {
        return false;
    }
    if (dst_w > src_w || dst_h > src_h) { return false; }

    for (uint32_t y = 0; y < dst_h; ++y) {
        const uint16_t* srow = src + (size_t)y * src_stride;
        uint16_t* drow = dst + (size_t)y * dst_stride;
        memcpy(drow, srow, (size_t)dst_w * sizeof(uint16_t)); /* 阶段 9：按行整拷 */
    }
    return true;
}
