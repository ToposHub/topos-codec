/* Frame header（固定 53 字节，spec §4.2 冻结）＋平面几何推导（§5）。
 *
 * 解码校验顺序（v1 §4.2 说明，冻结）：size ≥ 53（TRUNCATED）→ magic（MALFORMED）
 * → header_crc32（CHECKSUM_MISMATCH，整帧拒绝）→ 逐字段规则（按表偏移序）。
 * 编码侧执行同一套字段规则（编码器无法产出非法 header）。
 */
#ifndef TOPOS_INTERNAL_FRAME_HEADER_H
#define TOPOS_INTERNAL_FRAME_HEADER_H

#include <stddef.h>
#include <stdint.h>

#include "topos_codec.h"

#define TC_FRAME_HEADER_SIZE 53u
#define TC_FRAME_MAGIC "TPIC" /* 0x54 0x50 0x49 0x43 */
#define TC_MAX_CODED_DIM 16384u
#define TC_MAX_SLICE_COUNT 512u
#define TC_MAX_PACKET_SIZE 268435456u /* 256 MiB（spec §10） */

/* TRAW（Topos RAW）PWL-log12 传递曲线 id（topos_traw_format_plan §3.1/§3.2，
 * 2026-09-13 拍板）。值域取 FFmpeg color_trc 空间（1=bt709/8=linear/16=smpte2084/
 * 18=arib-std-b67 同族）中 19..255 自由区，20 = 私有 PWL-log12（参数冻结 §3.2）。
 * 交叉规则：value 20 ⇔ profile 7（TRAW），非 TRAW 流携带即拒。 */
#define TC_TRANSFER_TRAW_LOG0 20u

typedef struct topos_frame_header {
    /* —— §4.2 序列化字段 —— */
    uint8_t version_major; /* byte 6。V1=1；V2=2；V3=3（spatial intra + VLC）。
                            * 构造方必须显式设置（0 非法——杜绝静默 V1）。 */
    uint8_t version_minor; /* byte 7。v1.2（R4.1）：bd=12 扩展流写 1；
                                * v1.3（R4.2）：pf=1 扩展流写 2；
                                * v1.0 语义流恒 0；V2.0 恒 0 */
    uint16_t flags; /* 仅 bit0 alpha_premultiplied */
    uint8_t profile;
    uint8_t pixel_format;
    uint8_t bit_depth;
    uint8_t alpha_mode;
    uint8_t alpha_bit_depth;
    uint8_t frame_type;
    uint16_t gop_id;
    uint8_t ref_distance;
    uint16_t coded_width;
    uint16_t coded_height;
    uint16_t visible_width;
    uint16_t visible_height;
    uint8_t plane_count;
    uint8_t qmatrix_id;
    uint8_t qp_base;
    uint16_t slice_count;
    uint8_t color_range;
    uint8_t color_primaries;
    uint8_t color_transfer;
    uint8_t color_matrix;
    uint8_t chroma_siting;
    uint16_t sar_num;
    uint16_t sar_den;
    uint32_t frame_packet_size;
    /* —— V2 激活字段（byte 45–47；ADR-C027 D-1/D-2，V1 恒 0）—— */
    uint8_t entropy_mode;     /* byte 45：0=Rice / 1=canonical VLC */
    uint8_t codebook_version; /* byte 46：mode=0→0；mode=1→TC_VLC_CODEBOOK_VERSION */
    uint8_t coding_mode;      /* byte 47：V1/V2=0；V3=1（spatial intra） */
    /* —— 派生几何（不序列化；tc_frame_derive_geometry 填充）—— */
    uint16_t plane_coded_w[4];
    uint16_t plane_coded_h[4];
    uint16_t plane_visible_w[4];
    uint16_t plane_visible_h[4];
    uint16_t plane_block_cols[4];
    uint16_t plane_block_rows[4];
} topos_frame_header;

/* 校验序列化字段（不含 CRC 与 frame_packet_size==实际尺寸 这类包级检查）。
 * 供编码与解码共用；失败返回相应错误码并写 tc_last_error。 */
int32_t tc_frame_header_validate(const topos_frame_header* fh);

/* V7-A 目录解析使用的帧头注册表校验。该函数只验证 V7-A 的版本/能力字段，
 * 不把 major=7 加入旧 frame_header_validate() 的 V1-V6 解码分支；因此旧
 * reader 仍会对 V7 packet 返回 TC_ERR_UNSUPPORTED_VERSION。 */
int32_t tc_frame_header_validate_v7a_contract(const topos_frame_header* fh);

/* V 代际收纳（D2，2026-09-13）内部查询：退役代际考古回放是否放行。
 * 非 TOPOS_DEV_REPLAY 构建恒 0（生产零回放面）；回放构建 = TOPOS_DEV=1。
 * codec.c（V7-A 探测链）与 mov.c（V7-A 目录分支）用它做同口径闸门。 */
int tc_frame_header_dev_replay_enabled(void);

/* 编码：校验 → 写 53 字节（BE）→ 计算 header_crc32（对偏移 0..48）。 */
int32_t tc_frame_header_encode(const topos_frame_header* fh, uint8_t out[TC_FRAME_HEADER_SIZE]);

/* V7-A header codec.  These entry points are intentionally separate from the
 * V1-V6 packet codec: an old reader must continue to reject major=7 before it
 * interprets TPLD as legacy slice bytes. */
int32_t tc_frame_header_encode_v7a(const topos_frame_header* fh,
                                   uint8_t out[TC_FRAME_HEADER_SIZE]);
int32_t tc_frame_header_decode_v7a(const uint8_t* data, size_t size,
                                   topos_frame_header* fh);

/* 解码：CRC + 全部字段规则；成功后自动调用 tc_frame_derive_geometry。 */
int32_t tc_frame_header_decode(const uint8_t* data, size_t size, topos_frame_header* fh);

/* 由 visible/coded 推导每平面几何（plane_count..alpha 已须合法）。幂等。 */
int32_t tc_frame_derive_geometry(topos_frame_header* fh);

/* —— 色彩标签命名（inspect CLI / 日志用；未定义值返回 "reserved"/"unknown"）—— */
const char* tc_color_range_name(uint8_t v);
const char* tc_color_primaries_name(uint8_t v);
const char* tc_color_transfer_name(uint8_t v);
const char* tc_color_matrix_name(uint8_t v);
const char* tc_chroma_siting_name(uint8_t v);
const char* tc_profile_name(uint8_t v);
const char* tc_pixel_format_name(uint8_t v);
const char* tc_alpha_mode_name(uint8_t v);

#endif /* TOPOS_INTERNAL_FRAME_HEADER_H */
