/* Slice header（固定 17 字节，spec §4.3 冻结）＋ 全帧 slice 排列/覆盖验证。 */
#ifndef TOPOS_INTERNAL_SLICE_MAP_H
#define TOPOS_INTERNAL_SLICE_MAP_H

#include <stddef.h>
#include <stdint.h>

#include "frame_header.h"

#define TC_SLICE_HEADER_SIZE 17u

typedef struct topos_slice_header {
    uint32_t slice_payload_size;
    uint8_t plane; /* 0=Y 1=U 2=V 3=A */
    uint16_t block_y0;
    uint16_t block_h;
    uint8_t qp_delta_biased; /* 有效 qp = qp_base + (qp_delta_biased − 64) ∈ 0..95（v1.5 扩展）*/
    uint8_t k1;
    uint8_t k2;
    uint8_t k3;
    uint32_t slice_crc32; /* 对本 slice payload 字节计算（不含 header） */
} topos_slice_header;

/* 有效 qp（供 inspect 与阶段 4 反量化）；调用方保证 qp_base 合法 */
static inline int32_t tc_slice_effective_qp(uint8_t qp_base, uint8_t qp_delta_biased)
{
    return (int32_t)qp_base + (int32_t)qp_delta_biased - 64;
}

/* 编码：写 17 字节 BE（字段规则与 tc_slice_map_validate 一致，这里只做字节布局）。 */
int32_t tc_slice_header_encode(const topos_slice_header* sh, uint8_t out[TC_SLICE_HEADER_SIZE]);

/* 解码 + 单头字段域校验（k ∈ 0..14；qp 有效值 0..63 在 map_validate 校验，因为需要 qp_base）。 */
int32_t tc_slice_header_decode(const uint8_t* data, size_t size, topos_slice_header* sh);

/* 全帧排列/覆盖验证（spec §4.3，违反 → TC_ERR_MALFORMED 整帧拒绝）：
 * 平面顺序 Y→U→V→[A]、平面内 block_y0 升序、恰好平铺 [0, plane_block_rows)、
 * block_h ≥ 1、qp 有效值 0..63、k1/k2/k3 ∈ 0..14、alpha slice k3==0、plane 合法。
 * fh 须已通过 tc_frame_derive_geometry。 */
int32_t tc_slice_map_validate(const topos_frame_header* fh,
                              const topos_slice_header* slices, uint16_t slice_count);

#endif /* TOPOS_INTERNAL_SLICE_MAP_H */
