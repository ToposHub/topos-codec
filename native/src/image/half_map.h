/* half_map —— HALF（IEEE 754 binary16）样本 ↔ u16 码值冻结映射（lib 内部）。
 *
 * 规范：docs/image/topos_image_file_spec_v0.md §15（HALF 样本域，2026-09-19 冻结）。
 *
 * 设计（与 codec 内核的关系）：
 *  - codec 位流规范禁浮点（bitstream_spec §2.4）；HALF 模式不触碰任何规范
 *    步骤——half 样本在 host 侧经本映射转为 u16「码值」，之后与 UINT 样本
 *    走完全相同的 bd=16 整数管线（level shift/DCT/量化/熵编码零改动）；
 *  - 映射是 sign-magnitude → 单调偏移二进制（对称对数域）双射：
 *      code = (h & 0x8000) ? 0x8000 - (h & 0x7FFF) : 0x8000 + (h & 0x7FFF)
 *    有限值域严格单调（码值序 == 浮点值序），±0 归一为 0x8000（恰为
 *    bd16 level-shift 中值 → 浮点零映射为整数零）；Inf/NaN 占据码值域
 *    两端（-NaN < -Inf < 有限 < 0 < 有限 < +Inf < +NaN）；
 *  - 全部有限 half（32768 个正值 + 32768 个负值）与码值一一对应 →
 *    qp=0（Q=1、deadzone=0）时整数管线逐位可逆，HALF 文件可无损往返。
 */
#ifndef TOPOS_INTERNAL_IMAGE_HALF_MAP_H
#define TOPOS_INTERNAL_IMAGE_HALF_MAP_H

#include <stddef.h>
#include <stdint.h>

/* 单元素映射（规范参考实现；golden 向量见 test_half_map.c / spec §15） */
static inline uint16_t tci_half_to_code(uint16_t h)
{
    const uint16_t m = (uint16_t)(h & 0x7FFFu);
    return (h & 0x8000u) ? (uint16_t)(0x8000u - m)
                         : (uint16_t)(0x8000u + m);
}

static inline uint16_t tci_code_to_half(uint16_t c)
{
    if (c >= 0x8000u) {
        return (uint16_t)(c - 0x8000u);
    }
    /* c=0 不可由正向映射产生（前向值域 [1,0xFFFF]）；损坏/对抗码流可能
     * 解出该值——饱和到 m=0x7FFF（0xFFFF = -NaN），保持全域确定性 */
    uint32_t m = 0x8000u - (uint32_t)c;
    if (m > 0x7FFFu) {
        m = 0x7FFFu;
    }
    return (uint16_t)(0x8000u | m);
}

/* 批量转换（n 元素；src==dst 允许，逐元素独立）；公共 ABI 见 topos_image.h */
void tci_half_map_plane(const uint16_t* src, size_t n, uint16_t* dst);
void tci_code_unmap_plane(const uint16_t* src, size_t n, uint16_t* dst);

#endif /* TOPOS_INTERNAL_IMAGE_HALF_MAP_H */
