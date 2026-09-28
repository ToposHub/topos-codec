/* fuzz_codec —— 阶段 4：完整解码路径 fuzz。
 *
 * LLVMFuzzerTestOneInput：tc_frame_decode 两段式（先 query 几何；小几何再做
 * 全平面解码，吃满 concealment / 反量化 / 逆变换 / MED 重建 / crop 路径）。
 * 不变量：返回码 ∈ 定义集；成功/警告时帧仍交付；无崩溃、无超大分配
 * （分配仅由已验证的 packet_scan 几何驱动，≤ plane 像素数 ×2B）。
 *
 * 复用 corpus_replay driver（-gen 确定性回放；libFuzzer 目标直接链接本文件）。
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "topos_codec.h"

static int status_is_defined(int32_t rc)
{
    return rc == TC_OK || rc == TC_WARN_CONCEALED || rc == TC_ERR_INVALID_ARGUMENT ||
           rc == TC_ERR_OUT_OF_MEMORY || rc == TC_ERR_UNSUPPORTED_VERSION ||
           rc == TC_ERR_UNSUPPORTED_PROFILE || rc == TC_ERR_UNSUPPORTED_PIXEL_FORMAT ||
           rc == TC_ERR_UNSUPPORTED_MATRIX || rc == TC_ERR_UNSUPPORTED_ALPHA_MODE ||
           rc == TC_ERR_LIMIT_EXCEEDED || rc == TC_ERR_MALFORMED ||
           rc == TC_ERR_TRUNCATED || rc == TC_ERR_CHECKSUM_MISMATCH;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    topos_frame_output info;
    int32_t rc = tc_frame_decode(data, size, NULL, NULL, &info);
    if (!status_is_defined(rc)) { abort(); }
    if (rc != TC_OK) { return 0; }

    /* 小几何 → 全解码（大几何仅结构路径，避免 fuzz 单输入耗时失控） */
    uint64_t pixels = (uint64_t)info.visible_width * (uint64_t)info.visible_height;
    if (pixels > 2u * 1024u * 1024u) { return 0; }

    uint16_t* planes[TC_FRAME_MAX_PLANES] = {NULL, NULL, NULL, NULL};
    uint32_t w[TC_FRAME_MAX_PLANES] = {0, 0, 0, 0};
    int alloc_ok = 1;
    for (uint32_t p = 0u; p < info.plane_count; ++p) {
        uint32_t pw = 0, ph = 0;
        if (tc_frame_plane_geometry(&info, p, &pw, &ph) != TC_OK) { alloc_ok = 0; break; }
        w[p] = pw;
        planes[p] = (uint16_t*)malloc((size_t)pw * ph * sizeof(uint16_t));
        if (planes[p] == NULL) { alloc_ok = 0; break; }
    }
    if (alloc_ok) {
        rc = tc_frame_decode(data, size, planes, NULL, &info);
        if (!status_is_defined(rc)) { abort(); }
        if (rc == TC_WARN_CONCEALED && info.concealed_slices == 0u) { abort(); }
        if (rc == TC_OK && info.concealed_slices != 0u) { abort(); }
    }
    for (int p = 0; p < TC_FRAME_MAX_PLANES; ++p) { free(planes[p]); }
    return 0;
}
