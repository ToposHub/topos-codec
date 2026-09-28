/* fuzz 入口（阶段 3 位流）：
 * 不变量 —— tc_packet_scan 与逐片符号解码对任意输入：
 *   1) 不崩溃、不挂起（所有循环有界）；
 *   2) 返回值 ∈ spec §9 错误码集合；
 *   3) 无堆分配放大（scan 用栈上 view；符号解码走 O(1) 指纹路径）。
 * libFuzzer 与 ctest 回放 driver（corpus_replay.c）共用本入口。
 */
#include <stddef.h>
#include <stdint.h>

#include "bitstream/packet.h"
#include "bitstream/slice_codec.h"

static int code_is_defined(int32_t rc)
{
    return rc == TC_OK || rc == TC_WARN_CONCEALED ||
           (rc <= TC_ERR_INVALID_ARGUMENT && rc >= TC_ERR_NOT_IMPLEMENTED);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    topos_packet_view view;
    int32_t rc = tc_packet_scan(data, size, &view);
    if (!code_is_defined(rc)) { return 1; }
    if (rc != TC_OK) { return 0; }

    for (uint16_t i = 0; i < view.slice_count; ++i) {
        int32_t src;
        if (view.slices[i].plane == 3u) {
            src = tc_alpha_slice_decode(&view.fh, &view.slices[i], view.payloads[i],
                                        (size_t)view.slices[i].slice_payload_size,
                                        NULL, 0u, NULL);
        } else {
            src = tc_color_slice_decode(&view.fh, &view.slices[i], view.payloads[i],
                                        (size_t)view.slices[i].slice_payload_size,
                                        NULL, NULL);
        }
        if (!code_is_defined(src)) { return 1; }
    }
    return 0;
}
