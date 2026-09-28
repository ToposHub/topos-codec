#include "topos_codec.h"
#include "topos_codec_version.h"
#include "common/cpudetect.h"
#include "common/error.h"

#include <stddef.h>

int32_t tc_abi_version(void)
{
    return (int32_t)TOPOS_CODEC_ABI_VERSION;
}

int32_t tc_version(topos_version_info* out)
{
    if (out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "tc_version: out is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    out->struct_size   = (uint32_t)sizeof(topos_version_info);
    out->abi_version   = TOPOS_CODEC_ABI_VERSION;
    out->version_major = TOPOS_CODEC_VERSION_MAJOR;
    out->version_minor = TOPOS_CODEC_VERSION_MINOR;
    out->version_patch = TOPOS_CODEC_VERSION_PATCH;
    out->git_commit    = TOPOS_GIT_COMMIT;
    out->build_target  = TOPOS_BUILD_TARGET;
    /* reserved 不触碰（公共 ABI 约定） */
    return TC_OK;
}

int32_t tc_query_cpu_features(topos_cpu_features* out)
{
    if (out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "tc_query_cpu_features: out is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    out->struct_size = (uint32_t)sizeof(topos_cpu_features);
    out->abi_version = TOPOS_CODEC_ABI_VERSION;
    out->flags       = tc_internal_cpu_flags();
    return TC_OK;
}

int32_t tc_query_capabilities(topos_codec_capabilities* out)
{
    if (out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "tc_query_capabilities: out is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    out->struct_size = (uint32_t)sizeof(topos_codec_capabilities);
    out->abi_version = TOPOS_CODEC_ABI_VERSION;
    /* V 代际收纳（D1，2026-09-13）：TC_CODEC_CAP_V7A_BAND_DECODE 不再广播
     * ——V7-A band decode 归档（零产品消费者，manifest 同步 false）。 */
    out->flags = TC_CODEC_CAP_FULL_DECODE |
                 TC_CODEC_CAP_FIXED_REDUCED_DECODE |
                 TC_CODEC_CAP_AUTO_2K_DECODE |
                 TC_CODEC_CAP_CPU_SURFACE_OUTPUT |
                 TC_CODEC_CAP_HOST_VISIBLE_GPU_SURFACE |
                 TC_CODEC_CAP_DECODE_DROP_ALPHA;
    out->max_width = TC_CODEC_MAX_DIM;
    out->max_height = TC_CODEC_MAX_DIM;
    out->auto2k_max_dim = TC_CODEC_AUTO_2K_MAX_DIM;
    return TC_OK;
}

int32_t tc_query_scalable_status(topos_scalable_status* out)
{
    if (out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "tc_query_scalable_status: out is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* P0-04：V7-B minor-2 读写链路完整且可 opt-in 复现，但 RD4-08
     * 质量/体积/速度门禁未过（渐变 PSNR 落差 ~31 dB、编码 p50 超
     * +5% 预算），因此停留在 reference。升级到 eligible 必须先过
     * P4 matched-quality gate；default 只能由 rollout 决策改写。 */
    *out = TC_SCALABLE_STATUS_REFERENCE;
    return TC_OK;
}

const char* tc_scalable_status_name(topos_scalable_status status)
{
    switch (status) {
    case TC_SCALABLE_STATUS_EXPERIMENTAL: return "experimental";
    case TC_SCALABLE_STATUS_REFERENCE:    return "reference";
    case TC_SCALABLE_STATUS_ELIGIBLE:     return "eligible";
    case TC_SCALABLE_STATUS_DEFAULT:      return "default";
    default:                              return "unknown";
    }
}

int32_t tc_query_support(uint32_t profile, uint32_t pixel_format, uint32_t bit_depth,
                         uint32_t alpha_mode)
{
    if (profile != 3u && profile != 5u && profile != 6u && profile != 7u) {
        /* v1 已验收 Standard；R4.4 激活 Pro444(5)/Extreme(6)（§4.4）；
         * TRAW 批 1 激活 profile 7（topos_traw_format_plan §3.1） */
        tc_set_error(TC_ERR_UNSUPPORTED_PROFILE,
                     "profile %u not in {3 Standard, 5 Pro444, 6 Extreme, 7 TRAW}",
                     profile);
        return TC_ERR_UNSUPPORTED_PROFILE;
    }
    if (pixel_format > 3u ||
        (bit_depth != 10u && bit_depth != 12u && bit_depth != 16u)) {
        tc_set_error(TC_ERR_UNSUPPORTED_PIXEL_FORMAT,
                     "pixel_format %u / bit_depth %u != YUV 4:2:2|4:4:4|GBR|CFA 10/12/16-bit"
                     " (v1.2/v1.3/v1.4/v1.6/v1.7 枚举扩展, R4.1/R4.2/R4.3/TRAW 批 1/批 4)",
                     pixel_format, bit_depth);
        return TC_ERR_UNSUPPORTED_PIXEL_FORMAT;
    }
    /* TRAW：profile 7 ⇔ CFA（pf=3）∧ bd ∈ {12,16} ∧ 无 alpha（与 frame_header
     * 交叉规则同源；批 4 加宽 16-bit linear 归档档） */
    if ((profile == 7u) != (pixel_format == 3u)) {
        tc_set_error(TC_ERR_UNSUPPORTED_PIXEL_FORMAT,
                     "profile 7 (TRAW) requires pixel_format 3 (CFA) and vice versa");
        return TC_ERR_UNSUPPORTED_PIXEL_FORMAT;
    }
    if (profile == 7u && bit_depth != 12u && bit_depth != 16u) {
        tc_set_error(TC_ERR_UNSUPPORTED_PIXEL_FORMAT,
                     "profile 7 (TRAW) requires 12/16-bit (批 4 阶段 2 解锁 16-bit "
                     "linear 归档档)");
        return TC_ERR_UNSUPPORTED_PIXEL_FORMAT;
    }
    if (profile == 7u && alpha_mode != 0u) {
        tc_set_error(TC_ERR_UNSUPPORTED_PIXEL_FORMAT,
                     "profile 7 (TRAW) requires no alpha");
        return TC_ERR_UNSUPPORTED_PIXEL_FORMAT;
    }
    /* 批 4 阶段 2：bd=16 编码面随融合前向 i32 重做开放（TRAW linear 归档 /
     * 视频 / 图片 16-bit 共批；SIMD 域门控见 dispatch.h） */
    /* R4.4：profile/格式交叉（与 frame_header 同一规则） */
    if ((profile == 5u || profile == 6u) && pixel_format == 0u) {
        tc_set_error(TC_ERR_UNSUPPORTED_PIXEL_FORMAT,
                     "profile %u (Pro444/Extreme) requires 4:4:4 pixel_format",
                     profile);
        return TC_ERR_UNSUPPORTED_PIXEL_FORMAT;
    }
    if (profile == 6u && bit_depth != 12u) {
        tc_set_error(TC_ERR_UNSUPPORTED_PIXEL_FORMAT,
                     "profile 6 (Extreme) requires 12-bit");
        return TC_ERR_UNSUPPORTED_PIXEL_FORMAT;
    }
    if (alpha_mode > 2u) {
        tc_set_error(TC_ERR_UNSUPPORTED_ALPHA_MODE, "alpha_mode %u > 2", alpha_mode);
        return TC_ERR_UNSUPPORTED_ALPHA_MODE;
    }
    return TC_OK;
}

const char* tc_status_message(int32_t status)
{
    switch (status) {
    case TC_OK:                       return "OK";
    case TC_WARN_CONCEALED:           return "concealment applied to one or more slices";
    case TC_ERR_INVALID_ARGUMENT:     return "invalid argument";
    case TC_ERR_OUT_OF_MEMORY:        return "out of memory";
    case TC_ERR_UNSUPPORTED_VERSION:  return "unsupported bitstream/ABI version";
    case TC_ERR_UNSUPPORTED_PROFILE:  return "unsupported profile";
    case TC_ERR_UNSUPPORTED_PIXEL_FORMAT: return "unsupported pixel format or bit depth";
    case TC_ERR_UNSUPPORTED_MATRIX:   return "unsupported quantization matrix id";
    case TC_ERR_UNSUPPORTED_ALPHA_MODE: return "unsupported alpha mode";
    case TC_ERR_LIMIT_EXCEEDED:       return "decoder hard limit exceeded";
    case TC_ERR_MALFORMED:            return "malformed bitstream";
    case TC_ERR_TRUNCATED:            return "truncated bitstream";
    case TC_ERR_CHECKSUM_MISMATCH:    return "checksum mismatch";
    case TC_ERR_STATE:                return "invalid call order or context state";
    case TC_ERR_CANCELLED:            return "operation cancelled";
    case TC_ERR_IO:                   return "I/O callback failure";
    case TC_ERR_BUFFER_TOO_SMALL:     return "caller buffer too small";
    case TC_ERR_NOT_IMPLEMENTED:      return "capability reserved but not implemented";
    default:                          return "unknown status";
    }
}
