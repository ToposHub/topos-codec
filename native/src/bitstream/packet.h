/* Elementary frame packet 扫描（spec §4.1 布局）。
 *
 * tc_packet_scan 完成：frame header（CRC+字段）→ 逐 slice header（checked 边界）
 * → 精确耗尽校验（53 + Σ(17+payload) == size == frame_packet_size）
 * → slice 排列/覆盖验证 → 每片 payload CRC 标记（不失败；concealment 决策归阶段 4）。
 * 无堆分配（view 为调用方栈/静态对象）——fuzz 输入不可能诱发超大分配。
 */
#ifndef TOPOS_INTERNAL_PACKET_H
#define TOPOS_INTERNAL_PACKET_H

#include <stddef.h>
#include <stdint.h>

#include "frame_header.h"
#include "slice_map.h"
#include "topos_codec.h"

typedef struct topos_packet_view {
    topos_frame_header fh;
    uint16_t slice_count;
    topos_slice_header slices[TC_MAX_SLICE_COUNT];
    const uint8_t* payloads[TC_MAX_SLICE_COUNT]; /* 指向 data 内部，不拥有 */
    uint8_t slice_crc_ok[TC_MAX_SLICE_COUNT];    /* 1 = payload CRC 匹配 */
} topos_packet_view;

/* 结构解析（无 payload CRC）：header/边界/精确耗尽/排列覆盖校验。
 * TC_OK：view 填充完毕（含派生几何），slice_crc_ok 全 0（未验证）。
 * 注意：本原语不承诺 payload 完整性——正式解码 worker 必做且只做一次 CRC。 */
int32_t tc_packet_parse_structure(const uint8_t* data, size_t size, topos_packet_view* view);

/* 全结构校验扫描（结构 + 逐 slice payload CRC 标记，不失败；
 * concealment 决策归解码阶段）。TC_OK：view 填充完毕（含派生几何）。
 * 失败：spec §9 整帧拒绝路径（TRUNCATED / MALFORMED / CHECKSUM / UNSUPPORTED 类 / LIMIT）。 */
int32_t tc_packet_scan(const uint8_t* data, size_t size, topos_packet_view* view);

/* ---- V8（major=8, entropy_mode=8）：段化多链 rANS 包结构（批 1 冻结）----
 *
 * 布局（topos_v8_format_plan §2.2 批 1 定稿；V1~V7 逐字节不变）：
 *   [53B frame header]（major=8, entropy_mode=8, codebook/coding=0；
 *       slice_count 复用 = 全帧瓦片总数 T ∈ [plane_count, 512]）
 *   [8B V8 扩展头]：[0] segment_blocks_log2 ∈ {3,4,5}（= 8/16/32 块/段）
 *                   [1] tile_rows_log2 ∈ {0,4,5,6}（0 = 整平面一片；
 *                       4/5/6 = 16/32/64 块行；批 0 实测默认 5）
 *                   [2..7] 保留恒 0
 *   per plane p（0..plane_count−1 顺序串联）：
 *     [段目录 S_p × 12B]：stream_off u32 BE（相对本平面码流区基址）、
 *         stream_len u32 BE（含 4B 终态）、qp_delta_biased u8（V7 slice
 *         同语义）、保留 u8×3 恒 0
 *     [瓦片表区 T_p × 350B 定长]：每表 1B flags（V7-R2 位域同构：[1:0]
 *         lvl 模型、[2] dc 模型、[7:3] 恒 0）+ dc 条件行 5×29 + run 64 +
 *         lvl 条件行 5×28；未用行占位 {1,0,...}（row_encode 全零语义）
 *     [码流区]：段 0..S_p−1 连续；段流 = 4B 大端终态 + 数据（V7-R2
 *         flush 布局同构）
 *     [瓦片 CRC T_p × u32 BE]：CRC_i 增量覆盖表 i 的 350B + 成员段流
 *         字节（连续区间）；不含目录（目录损坏 = 结构性拒绝，不进
 *         conceal 判定）
 *   S_p = ceil(plane_block_rows[p]·plane_block_cols[p] / segment_blocks)
 *   T_p = tile_rows==0 ? 1 : ceil(plane_block_rows[p] / tile_rows)
 *   瓦片 t 成员段 = [seg_first(t), seg_first(t+1))，seg_first(t) =
 *       ⌊t·tile_rows·cols / segment_blocks⌋（段归首块所在瓦片，可跨
 *       瓦片边界；零成员瓦片合法——极窄平面 × 大段粒度退化）
 *   精确耗尽：53 + 8 + Σ_p (12·S_p + 350·T_p + Σlen_p + 4·T_p) == size
 *
 * 老二进制（版本白名单 ≤ 7）对 V8 流返回 UNSUPPORTED_VERSION——由其
 * frame_header 白名单保证；本库 V7 入口（tc_packet_scan/parse_structure）
 * 对 major=8 同样明确拒绝（V8 走 scan_v8 专用入口）。 */
#define TC_V8_EXT_HEADER_SIZE 8u
#define TC_V8_TABLE_BYTES 350u
#define TC_V8_DIR_ENTRY_BYTES 12u
#define TC_V8_STATE_BYTES 4u /* 段流尾 rANS 终态（= entropy/rans.h TC_RANS_STATE_BYTES；免引依赖） */

typedef struct topos_v8_tile_info {
    uint32_t plane;
    uint32_t tile_id;      /* plane 内瓦片序 */
    uint32_t seg_first;    /* 成员段起点（plane 内段序） */
    uint32_t seg_count;    /* 成员段数（可为 0：退化窄平面） */
    uint32_t table_off;    /* 表基址（相对包首） */
    uint32_t stream_off;   /* 首成员段流基址（相对包首） */
    uint32_t stream_bytes; /* 成员段流总字节 */
    uint32_t crc_stored;   /* 包内存储的瓦片 CRC */
    uint8_t crc_ok;        /* 1 = 重算一致；0 = 不匹配（conceal 判定归解码） */
} topos_v8_tile_info;

typedef struct topos_v8_packet_view {
    topos_frame_header fh;
    uint32_t segment_blocks;
    uint32_t tile_rows;          /* 0 = 整平面一片 */
    uint32_t tiles_per_plane[4];
    uint32_t segs_per_plane[4];
    uint32_t plane_dir_off[4];   /* 各区基址（相对包首） */
    uint32_t plane_table_off[4];
    uint32_t plane_stream_off[4];
    uint32_t plane_crc_off[4];
    uint32_t tile_count;         /* == fh.slice_count */
    topos_v8_tile_info tiles[TC_MAX_SLICE_COUNT];
} topos_v8_packet_view;

/* 廉价探测（size ≥ 53 + magic + major==8；头部 CRC 交给 decode） */
int32_t tc_packet_is_v8(const uint8_t* data, size_t size);

/* V9 廉价探测（major==9）：包布局与 V8 同构（topos_v9_micro_gop_plan
 * 批 1 冻结），扫描走 tc_packet_scan_v8（major 域 {8,9}）。 */
int32_t tc_packet_is_v9(const uint8_t* data, size_t size);

/* V8 全结构校验扫描（结构 + 逐瓦片 CRC 标记，不失败；conceal 决策归
 * 解码阶段）。TC_OK：view 填充完毕（含派生几何/区基址/瓦片段映射）。
 * verify_crc=0：跳过逐瓦片 CRC（解码路径——CRC 由瓦片任务并行重算，
 * 与 V7「slice worker 对唯一 payload 做且只做一次」同设计）。
 * V9（major=9）包布局同构继承，同入口扫描（major 域 {8,9}；
 * ADR-C046 版本预算：包扫描层 V9 零新增结构分支）。 */
int32_t tc_packet_scan_v8_ex(const uint8_t* data, size_t size,
                             topos_v8_packet_view* view, int verify_crc);
int32_t tc_packet_scan_v8(const uint8_t* data, size_t size, topos_v8_packet_view* view);

#endif /* TOPOS_INTERNAL_PACKET_H */
