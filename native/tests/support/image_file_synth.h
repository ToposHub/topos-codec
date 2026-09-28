/* image_file_synth —— 确定性合法 .toos 文件合成器 + 内存 io（image 单测/golden/fuzz 共用）。
 *
 * packet_synth（合法 TPIC packet）+ tc_image_write（TPIM envelope）的组合：
 * 从 packet 内层 header 派生 IDSC（spec §6.1 全部一致），writer 侧交叉校验
 * 必然通过。全部确定性（无墙钟/无未初始化读）。
 */
#ifndef TOPOS_TEST_IMAGE_FILE_SYNTH_H
#define TOPOS_TEST_IMAGE_FILE_SYNTH_H

#include <stddef.h>
#include <stdint.h>

#include "topos_codec.h"
#include "topos_image.h"
#include "packet_synth.h"

/* —— 内存源（reader 用） —— */
typedef struct image_mem_src {
    const uint8_t* data;
    uint64_t len;
} image_mem_src;

int32_t image_mem_read(void* ctx, uint64_t off, void* buf, size_t n);
/* 组装 topos_io：read 指向 image_mem_read，length = len */
void image_mem_src_init(image_mem_src* s, const uint8_t* data, size_t len, topos_io* io);

/* —— 内存汇（writer 用；容量不足置 oom） —— */
typedef struct image_mem_sink {
    uint8_t* data;
    size_t len;
    size_t cap;
    int oom;
} image_mem_sink;

int32_t image_mem_write(void* ctx, const void* d, size_t n);
int32_t image_mem_seek_write(void* ctx, uint64_t off, const void* d, size_t n);
/* 组装 topos_io：write/seek_write 指向内存实现 */
void image_mem_sink_init(image_mem_sink* s, uint8_t* buf, size_t cap, topos_io* io);

/* 从合法 packet 派生 IDSC（与内层 header 十项交叉一致）。
 * image_profile：TC_IMG_PROFILE_*（必须与 packet 内容匹配，否则 writer 拒绝）。 */
int32_t image_file_synth_idsc(const uint8_t* pixl, size_t size, uint8_t image_profile,
                              topos_image_idsc* out);

/* 构造完整 .toos 文件字节（malloc；调用方 free(*out_data)）。
 * extras 可 NULL/0。refs=1 要求对应 chunk 在 extras 中（写端一致性校验）。 */
int32_t image_file_synth_build(const packet_synth_cfg* pcfg, uint8_t image_profile,
                               const topos_image_chunk_in* extras, uint32_t extra_count,
                               uint8_t** out_data, size_t* out_size);

/* 扩展版：显式指定 iccp_ref/ocio_ref（阶段 3 metadata 一致性） */
int32_t image_file_synth_build_ex(const packet_synth_cfg* pcfg, uint8_t image_profile,
                                  const topos_image_chunk_in* extras, uint32_t extra_count,
                                  uint8_t iccp_ref, uint8_t ocio_ref,
                                  uint8_t** out_data, size_t* out_size);

#endif /* TOPOS_TEST_IMAGE_FILE_SYNTH_H */
