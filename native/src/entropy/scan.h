/* Zigzag 扫描表（spec 附录 A.2，冻结）。
 * kTcZigzag[i] = 第 i 个扫描位置的 natural 索引（u*8+v）；
 * test_block_coding.c 以对角遍历算法生成参考表交叉验证。
 * kTcZigzagInv[n] = natural 索引 n 的扫描位置（kTcZigzag 的精确逆，
 * 由生成脚本断言 zig[inv[n]]==n 全对；R6 编码侧 zigzag 序布局使用）。
 */
#ifndef TOPOS_INTERNAL_SCAN_H
#define TOPOS_INTERNAL_SCAN_H

#include <stdint.h>

static const uint8_t kTcZigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35,  42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

static const uint8_t kTcZigzagInv[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
};

#endif /* TOPOS_INTERNAL_SCAN_H */
