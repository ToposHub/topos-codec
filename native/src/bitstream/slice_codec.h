/* Slice 符号层编解码（spec §7.5 / §8.3；阶段 4 在其外接 plane tiling 与重建）。
 *
 * 颜色 slice：band 内块行 × 平面块列，行主序逐块调用块符号层；
 * DC 预测上下文严格闭合于 slice 内（band 首块行无 top、x=0 无 left —— slice 独立性）。
 * Alpha slice：band 像素（coded 宽 × block_h·8）的残差流。
 *
 * 解码为流式：逐块/逐符号折叠进 symbol_hash（splitmix64），可选写回数组；
 * q_out/residuals_out 传 NULL 即 O(1) 内存 —— fuzz 的「无超大分配」门禁靠这一点。
 */
#ifndef TOPOS_INTERNAL_SLICE_CODEC_H
#define TOPOS_INTERNAL_SLICE_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "bitio.h"
#include "frame_header.h"
#include "slice_map.h"

/* 颜色：q_blocks 行主序 [block_h][cols][64]，元素 natural 序。 */
int32_t tc_color_slice_encode(const topos_frame_header* fh, const topos_slice_header* sh,
                              const int32_t* q_blocks, tc_bitwriter* bw);

/* 解码：q_out 可 NULL（仅校验+指纹）。symbol_hash 可 NULL。 */
int32_t tc_color_slice_decode(const topos_frame_header* fh, const topos_slice_header* sh,
                              const uint8_t* payload, size_t payload_size,
                              int32_t* q_out, uint64_t* symbol_hash);

/* Alpha：residual_count == plane_coded_w[3] × block_h·8 */
int32_t tc_alpha_slice_encode(const topos_frame_header* fh, const topos_slice_header* sh,
                              const int32_t* residuals, size_t residual_count, tc_bitwriter* bw);
int32_t tc_alpha_slice_decode(const topos_frame_header* fh, const topos_slice_header* sh,
                              const uint8_t* payload, size_t payload_size,
                              int32_t* residuals_out, size_t residual_count,
                              uint64_t* symbol_hash);

/* ---- 流式解码（阶段 4）：逐块/逐对回调，调用方随收随重建。
 * O(1) 额外内存（颜色 O(cols) DC 上下文），不受不可信 band_h 摆布的分配上界。
 * 语义/校验与上方整缓冲版逐字节一致；sink 返回非 TC_OK 即中止并透传。
 * has_ac（解码深化批次）→ ac_rowmask：自然序频率行掩码，bit r = 第 r 行
 * 存在解码写入的系数（DC 恒置 bit0；域内 level=0 的病态写入亦置位——
 * 超集标记，与处理零行数学等价）。0 = 无任何 (run,level) 对（sink 只可
 * 读 q[0]）；非 0 时 sink/融合 IDCT 可跳过零行。 */
typedef int32_t (*tc_color_block_sink_fn)(void* ctx, uint32_t block_index,
                                          const int32_t q_natural[64],
                                          uint32_t ac_rowmask);
typedef int32_t (*tc_alpha_pair_sink2_fn)(void* ctx, size_t pos, uint32_t run, int32_t level);

/* DC 行上下文（两行滚动；M10-1 起由调用方持有——decoder context 池化时
 * 稳态零分配）。prev_row/row 各 ≥ plane_block_cols 个元素；扫描入口统一
 * 清零两条有效行（首行预测与 calloc 语义对齐），跨行滚动复用同一对缓冲。 */
typedef struct tc_scan_dc_ctx {
    int32_t* prev_row;
    int32_t* row;
    size_t elems;
} tc_scan_dc_ctx;

static inline void tc_scan_dc_swap(tc_scan_dc_ctx* dc)
{
    int32_t* tmp = dc->prev_row;
    dc->prev_row = dc->row;
    dc->row = tmp;
}

int32_t tc_color_slice_decode_stream(const topos_frame_header* fh, const topos_slice_header* sh,
                                     const uint8_t* payload, size_t payload_size,
                                     tc_color_block_sink_fn sink, void* ctx,
                                     uint64_t* symbol_hash);

/* M10-1：调用方提供 DC 行 scratch（池化零分配路径）。dc->prev_row/row 为
 * ≥ plane_block_cols 元素的缓冲；语义/错误码/消费位与无 scratch 版逐位一致。 */
int32_t tc_color_slice_decode_stream_scratch(const topos_frame_header* fh,
                                     const topos_slice_header* sh,
                                     const uint8_t* payload, size_t payload_size,
                                     tc_color_block_sink_fn sink, void* ctx,
                                     uint64_t* symbol_hash, tc_scan_dc_ctx* dc);

/* M10-2A：专用 scan-to-plane 生产热路径（sink 在块循环内展开，无函数指针
 * 边界）。store 需填 dst/stride/qctx/cols/block_y0/vis_w/vis_h/mid/max/w0/
 * dinv（stats 可 NULL）。dc 为池化 scratch。语义/错误码/消费位与
 * tc_color_slice_decode_stream_scratch + 重建 sink 逐位一致（差分测试钉死）。 */
struct tc_color_store_ctx;
int32_t tc_color_slice_decode_to_plane(const topos_frame_header* fh,
                                       const topos_slice_header* sh,
                                       const uint8_t* payload, size_t payload_size,
                                       tc_scan_dc_ctx* dc,
                                       struct tc_color_store_ctx* store);

/* A2-4 生产融合发射（V7-R2）：rANS2 符号环内直出 CSR，跳过稠密 q[64]
 * 中转与 63 扫描。发射顺序 = zigzag 解码序（与 dense-scan 发射的自然序
 * 不同，但 zigzag 是双射、kernel 的 G 装配与像素求和序固定 → 位精确
 * 等价）。dc 池由调用方提供（同 to_plane 契约）。 */
typedef struct tc_sp_emit_ctx {
    uint64_t* pairs;      /* 本 slice 最坏预留段基址（63 对/块） */
    uint32_t* off;        /* 本 slice [blocks+1] 段（块前累计 CSR 起点） */
    int32_t* dc;          /* plane 稠密 DC（全局块号）区域基址 */
    uint32_t plane_cols;  /* plane 块列数（dc 全局块号步长） */
    uint32_t block_y0;    /* slice 首块行（dc 全局行基） */
    uint32_t cnt;         /* 运行对计数（worker 局部，入口置 0） */
} tc_sp_emit_ctx;

int32_t tc_color_rans2_decode_sparse(const topos_frame_header* fh,
                                     const topos_slice_header* sh,
                                     const uint8_t* payload, size_t payload_size,
                                     tc_scan_dc_ctx* dc, tc_sp_emit_ctx* em);
int32_t tc_alpha_slice_decode_stream(const topos_frame_header* fh, const topos_slice_header* sh,
                                     const uint8_t* payload, size_t payload_size,
                                     tc_alpha_pair_sink2_fn sink, void* ctx,
                                     uint64_t* symbol_hash);

#endif /* TOPOS_INTERNAL_SLICE_CODEC_H */
