/* 平面 padding/crop：奇数尺寸、边缘复制正确性、stride 处理、roundtrip */
#include "transform/plane.h"
#include "mini_test.h"

#include <string.h>

int main(void)
{
    /* coded_from_visible（含清单奇数尺寸） */
    MT_CHECK_EQ_U64(tc_plane_coded_from_visible(1), 8);
    MT_CHECK_EQ_U64(tc_plane_coded_from_visible(7), 8);
    MT_CHECK_EQ_U64(tc_plane_coded_from_visible(8), 8);
    MT_CHECK_EQ_U64(tc_plane_coded_from_visible(9), 16);
    MT_CHECK_EQ_U64(tc_plane_coded_from_visible(33), 40);
    MT_CHECK_EQ_U64(tc_plane_coded_from_visible(1919), 1920);
    MT_CHECK_EQ_U64(tc_plane_coded_from_visible(8192), 8192);
    MT_CHECK_EQ_U64(tc_plane_coded_from_visible(16384), 16384);
    MT_CHECK_EQ_U64(tc_plane_coded_from_visible(0), 0);
    MT_CHECK_EQ_U64(tc_plane_coded_from_visible(16385), 0);
    MT_CHECK_EQ_U64(tc_plane_coded_from_visible(0xFFFFFFFFu), 0);

    /* 7×5 → 8×8 padding：边缘复制（含四角） */
    {
        uint16_t src[7 * 5];
        for (int y = 0; y < 5; ++y) {
            for (int x = 0; x < 7; ++x) { src[y * 7 + x] = (uint16_t)(y * 10 + x); }
        }
        uint16_t dst[8 * 8];
        memset(dst, 0xEE, sizeof dst);
        MT_CHECK(tc_plane_pad_u16(src, 7, 5, 7, dst, 8, 8, 8));
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const uint32_t sy = (uint32_t)(y < 5 ? y : 4);
                const uint32_t sx = (uint32_t)(x < 7 ? x : 6);
                MT_CHECK_EQ_U64(dst[y * 8 + x], src[sy * 7 + sx]);
            }
        }
        /* roundtrip：crop 回 7×5 == 原 */
        uint16_t back[7 * 5];
        MT_CHECK(tc_plane_crop_u16(dst, 8, 8, 8, back, 7, 5, 7));
        MT_CHECK(memcmp(back, src, sizeof src) == 0);
    }

    /* 1×1 → 8×8：全部填充同一值 */
    {
        uint16_t one = 4242;
        uint16_t dst[64];
        MT_CHECK(tc_plane_pad_u16(&one, 1, 1, 1, dst, 8, 8, 8));
        for (int i = 0; i < 64; ++i) { MT_CHECK_EQ_U64(dst[i], 4242); }
    }

    /* 33×17 → 40×24：随机值 + stride 场景（源 stride > 宽，含行间填充） */
    {
        enum { SW = 33, SH = 17, SS = 40, DW = 40, DH = 24, DS = 44 };
        static uint16_t src[SH * SS];
        static uint16_t dst[DH * DS];
        for (int y = 0; y < SH; ++y) {
            for (int x = 0; x < SW; ++x) { src[y * SS + x] = (uint16_t)(mt_rand_u64() & 0x3FF); }
        }
        MT_CHECK(tc_plane_pad_u16(src, SW, SH, SS, dst, DW, DH, DS));
        for (int y = 0; y < DH; ++y) {
            for (int x = 0; x < DW; ++x) {
                const uint32_t sy = (uint32_t)(y < SH ? y : SH - 1);
                const uint32_t sx = (uint32_t)(x < SW ? x : SW - 1);
                MT_CHECK_EQ_U64(dst[y * DS + x], src[sy * SS + sx]);
            }
        }
        /* crop 回原始区域 */
        static uint16_t back[SH * SS];
        MT_CHECK(tc_plane_crop_u16(dst, DW, DH, DS, back, SW, SH, SS));
        for (int y = 0; y < SH; ++y) {
            MT_CHECK(memcmp(&back[y * SS], &src[y * SS], SW * sizeof(uint16_t)) == 0);
        }
    }

    /* 参数非法路径 */
    {
        uint16_t buf[64];
        MT_CHECK(!tc_plane_pad_u16(NULL, 1, 1, 1, buf, 8, 8, 8));
        MT_CHECK(!tc_plane_pad_u16(buf, 1, 1, 1, NULL, 8, 8, 8));
        MT_CHECK(!tc_plane_pad_u16(buf, 1, 1, 1, buf, 8, 8, 0));       /* stride < 宽 */
        MT_CHECK(!tc_plane_pad_u16(buf, 8, 8, 8, buf, 4, 4, 4));       /* dst < src */
        MT_CHECK(!tc_plane_pad_u16(buf, 0, 8, 8, buf, 8, 8, 8));       /* 零尺寸 */
        MT_CHECK(!tc_plane_pad_u16(buf, 8193, 8, 8193, buf, 8192, 8, 8192));
        MT_CHECK(!tc_plane_crop_u16(buf, 4, 4, 4, buf, 8, 8, 8));      /* dst > src */
        MT_CHECK(!tc_plane_crop_u16(NULL, 4, 4, 4, buf, 2, 2, 2));
    }

    return MT_MAIN_RETURN();
}
