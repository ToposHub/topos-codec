/* fuzz_image —— 阶段 1：TPIM 容器层 fuzz。
 *
 * LLVMFuzzerTestOneInput 两条路径：
 *  1. 任意字节当 .toos 探测：probe/validate 返回码 ∈ 定义集；成功且几何小
 *     时做完整 decode（≤128x128 有界分配）；
 *  2. 输入驱动变异合法基准文件：单字节翻转/目录字段篡改后 probe ——
 *     不崩溃、不越界、不死循环（规格不变量）。
 *
 * 复用 corpus_replay driver（-gen 确定性回放；libFuzzer 目标直接链接本文件）。
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "topos_codec.h"
#include "topos_image.h"
#include "../support/image_file_synth.h"
#include "../support/packet_synth.h"

static int status_is_defined(int32_t rc)
{
    return rc == TC_OK || rc >= TC_WARN_CONCEALED ||
           rc == TC_ERR_INVALID_ARGUMENT || rc == TC_ERR_OUT_OF_MEMORY ||
           rc == TC_ERR_UNSUPPORTED_VERSION || rc == TC_ERR_UNSUPPORTED_PROFILE ||
           rc == TC_ERR_UNSUPPORTED_PIXEL_FORMAT || rc == TC_ERR_UNSUPPORTED_MATRIX ||
           rc == TC_ERR_UNSUPPORTED_ALPHA_MODE ||
           rc == TC_ERR_LIMIT_EXCEEDED || rc == TC_ERR_MALFORMED ||
           rc == TC_ERR_TRUNCATED || rc == TC_ERR_CHECKSUM_MISMATCH ||
           rc == TC_ERR_IO || rc == TC_ERR_BUFFER_TOO_SMALL ||
           rc == TC_IMG_ERR_BAD_MAGIC || rc == TC_IMG_ERR_BAD_PREAMBLE ||
           rc == TC_IMG_ERR_BAD_DIRECTORY || rc == TC_IMG_ERR_UNKNOWN_CRITICAL ||
           rc == TC_IMG_ERR_CHUNK_CONFLICT || rc == TC_IMG_ERR_METADATA_CONFLICT ||
           rc == TC_IMG_ERR_LIMIT || rc == TC_IMG_ERR_IO_WRITE_FAILED;
}

/* 合法基准文件（首次调用时构建，之后只读复用） */
static uint8_t* g_base = NULL;
static size_t g_base_size = 0;

static int build_base_once(void)
{
    if (g_base != NULL) { return 0; }
    const int32_t rc = image_file_synth_build(packet_synth_cfg_at(PACKET_SYNTH_CFG_ALPHA),
                                              TC_IMG_PROFILE_PREVIEW, NULL, 0,
                                              &g_base, &g_base_size);
    return rc == TC_OK ? 0 : 1;
}

static int probe_decode_defined(const uint8_t* data, size_t size)
{

    image_mem_src src;
    topos_io io;
    image_mem_src_init(&src, data, size, &io);
    topos_image_info info;
    memset(&info, 0, sizeof(info));
    const int32_t rc = tc_image_probe(&io, &info);
    if (!status_is_defined(rc)) { return 1; }

    const int32_t vrc = tc_image_validate(&io, TC_IMG_VALIDATE_DEEP, &info);
    if (!status_is_defined(vrc)) { return 1; }

    /* 小几何才做真实解码（fuzz 分配有界） */
    if (info.visible_width <= 128u && info.visible_height <= 128u) {
        const size_t npix = (size_t)info.visible_width * (size_t)info.visible_height;
        uint16_t* mem = (uint16_t*)malloc(npix * 4u * sizeof(uint16_t));
        if (mem != NULL) {
            topos_plane_view views[4];
            uint16_t* ptrs[4] = {mem, mem + npix, mem + 2 * npix, mem + 3 * npix};
            for (int i = 0; i < 4; ++i) {
                memset(&views[i], 0, sizeof(views[i]));
                views[i].struct_size = (uint32_t)sizeof(views[i]);
                views[i].abi_version = TOPOS_CODEC_ABI_VERSION; /* plane_view 为 codec 域结构 */
                views[i].pixels = ptrs[i];
                views[i].stride = 0;
            }
            topos_frame_output out;
            memset(&out, 0, sizeof(out));
            out.struct_size = (uint32_t)sizeof(out);
            out.abi_version = TOPOS_CODEC_ABI_VERSION;
            const int32_t drc = tc_image_decode(&io, views, &out);
            if (!status_is_defined(drc)) { free(mem); return 1; }
            free(mem);
        }
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (build_base_once() != 0) { return 1; }

    /* 路径 1：任意字节 */
    if (probe_decode_defined(data, size) != 0) { return 1; }

    /* 路径 2：输入驱动变异基准文件 */
    if (size >= 4u) {
        uint8_t* mutated = (uint8_t*)malloc(g_base_size);
        if (mutated == NULL) { return 1; }
        memcpy(mutated, g_base, g_base_size);
        const size_t flips = (size_t)(data[0] | 1u) < 8u ? (size_t)(data[0] | 1u) : 8u;
        for (size_t i = 0; i < flips && (4u + i * 3u) < size; ++i) {
            const size_t off = ((size_t)data[1 + i * 3] << 16 |
                                (size_t)data[2 + i * 3] << 8 |
                                (size_t)data[3 + i * 3]) % g_base_size;
            mutated[off] ^= (uint8_t)(0x01u << (data[(4 + i * 3) % size] & 7u));
        }
        const int bad = probe_decode_defined(mutated, g_base_size);
        free(mutated);
        if (bad) { return 1; }
    }
    return 0;
}
