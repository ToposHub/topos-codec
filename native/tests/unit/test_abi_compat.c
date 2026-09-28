/* ABI 兼容钉死（阶段 10）：公共结构体尺寸/字段偏移、全部错误码数值、
 * struct_size 容忍语义、能力协商矩阵。
 *
 * 布局值在 LP64（macOS/Linux）与 LLP64（Windows x64）一致 —— 全部字段为
 * 定宽整数/指针/size_t，无 long。任何一项变动 = ABI 破坏：必须提升
 * TOPOS_CODEC_ABI_VERSION 并走迁移文档，而不是悄悄改布局。
 */
#include <stddef.h>
#include <string.h>

#include "topos_codec.h"

#include "mini_test.h"

int main(void)
{
    /* ---- 结构体尺寸（字节） ---- */
    MT_CHECK_EQ_U64(sizeof(topos_version_info), 56u);
    MT_CHECK_EQ_U64(sizeof(topos_cpu_features), 32u);
    MT_CHECK_EQ_U64(sizeof(topos_frame_config), 64u);
    MT_CHECK_EQ_U64(sizeof(topos_frame_input), 104u);
    MT_CHECK_EQ_U64(sizeof(topos_frame_stats), 68u);
    MT_CHECK_EQ_U64(sizeof(topos_frame_output), 580u);
    MT_CHECK_EQ_U64(sizeof(topos_io), 64u);
    MT_CHECK_EQ_U64(sizeof(topos_movie_config), 68u);
    MT_CHECK_EQ_U64(sizeof(topos_movie_info), 80u);

    /* ---- 关键字段偏移（首尾锚点 + 尾部 reserved，防止中部插入字段） ---- */
    MT_CHECK_EQ_U64(offsetof(topos_version_info, struct_size), 0u);
    MT_CHECK_EQ_U64(offsetof(topos_version_info, abi_version), 4u);
    MT_CHECK_EQ_U64(offsetof(topos_version_info, git_commit), 24u);
    MT_CHECK_EQ_U64(offsetof(topos_version_info, reserved), 40u);

    MT_CHECK_EQ_U64(offsetof(topos_cpu_features, flags), 8u);

    MT_CHECK_EQ_U64(offsetof(topos_frame_config, visible_width), 8u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_config, qp_base), 16u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_config, qp_delta_luma), 17u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_config, slice_rows), 19u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_config, alpha_mode), 20u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_config, alpha_bit_depth), 21u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_config, sar_num), 28u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_config, reserved), 32u);

    MT_CHECK_EQ_U64(offsetof(topos_frame_input, planes), 8u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_input, strides), 40u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_input, reserved), 72u);

    MT_CHECK_EQ_U64(offsetof(topos_frame_stats, packet_size), 8u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_stats, color_payload_bytes), 12u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_stats, alpha_max_abs_error), 32u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_stats, reserved), 36u);

    MT_CHECK_EQ_U64(offsetof(topos_frame_output, coded_width), 12u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_output, plane_count), 16u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_output, alpha_mode), 20u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_output, concealed_slices), 28u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_output, slice_status), 36u);
    MT_CHECK_EQ_U64(offsetof(topos_frame_output, reserved), 548u);

    MT_CHECK_EQ_U64(offsetof(topos_io, ctx), 8u);
    MT_CHECK_EQ_U64(offsetof(topos_io, read), 16u);
    MT_CHECK_EQ_U64(offsetof(topos_io, write), 24u);
    MT_CHECK_EQ_U64(offsetof(topos_io, seek_write), 32u);
    MT_CHECK_EQ_U64(offsetof(topos_io, length), 40u);
    MT_CHECK_EQ_U64(offsetof(topos_io, reserved), 48u);

    MT_CHECK_EQ_U64(offsetof(topos_movie_config, visible_width), 8u);
    MT_CHECK_EQ_U64(offsetof(topos_movie_config, timescale), 32u);
    MT_CHECK_EQ_U64(offsetof(topos_movie_config, reserved), 36u);

    MT_CHECK_EQ_U64(offsetof(topos_movie_info, sample_count), 36u);
    MT_CHECK_EQ_U64(offsetof(topos_movie_info, faststart), 40u);
    MT_CHECK_EQ_U64(offsetof(topos_movie_info, index_bytes), 44u);
    MT_CHECK_EQ_U64(offsetof(topos_movie_info, reserved), 48u);

    /* ---- 错误码数值全量冻结（spec §9；test_abi 抽查，此处全量） ---- */
    MT_CHECK_EQ_I64(TC_OK, 0);
    MT_CHECK_EQ_I64(TC_WARN_CONCEALED, 1);
    MT_CHECK_EQ_I64(TC_ERR_INVALID_ARGUMENT, -1);
    MT_CHECK_EQ_I64(TC_ERR_OUT_OF_MEMORY, -2);
    MT_CHECK_EQ_I64(TC_ERR_UNSUPPORTED_VERSION, -3);
    MT_CHECK_EQ_I64(TC_ERR_UNSUPPORTED_PROFILE, -4);
    MT_CHECK_EQ_I64(TC_ERR_UNSUPPORTED_PIXEL_FORMAT, -5);
    MT_CHECK_EQ_I64(TC_ERR_UNSUPPORTED_MATRIX, -6);
    MT_CHECK_EQ_I64(TC_ERR_UNSUPPORTED_ALPHA_MODE, -7);
    MT_CHECK_EQ_I64(TC_ERR_LIMIT_EXCEEDED, -8);
    MT_CHECK_EQ_I64(TC_ERR_MALFORMED, -9);
    MT_CHECK_EQ_I64(TC_ERR_TRUNCATED, -10);
    MT_CHECK_EQ_I64(TC_ERR_CHECKSUM_MISMATCH, -11);
    MT_CHECK_EQ_I64(TC_ERR_STATE, -12);
    MT_CHECK_EQ_I64(TC_ERR_CANCELLED, -13);
    MT_CHECK_EQ_I64(TC_ERR_IO, -14);
    MT_CHECK_EQ_I64(TC_ERR_BUFFER_TOO_SMALL, -15);
    MT_CHECK_EQ_I64(TC_ERR_NOT_IMPLEMENTED, -16);

    /* ---- struct_size 容忍语义：0（未初始化调用方）与精确值均可，
     *      任何其它值（旧小/新大）拒绝 —— v1 严格策略，变更须过 ABI 版本 ---- */
    {
        topos_frame_config c;
        memset(&c, 0, sizeof c);
        c.visible_width = 64u;
        c.visible_height = 48u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK); /* struct_size=0 容忍 */
        c.struct_size = (uint32_t)sizeof(c);
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_OK);
        c.struct_size = (uint32_t)sizeof(c) - 4u; /* 模拟旧 header 编译的调用方 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_ERR_INVALID_ARGUMENT);
        c.struct_size = (uint32_t)sizeof(c) + 8u; /* 模拟新 header 编译的调用方 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&c), TC_ERR_INVALID_ARGUMENT);
        MT_CHECK_EQ_I64(tc_frame_config_validate(NULL), TC_ERR_INVALID_ARGUMENT);
    }

    /* ---- 能力协商矩阵（阶段 10） ---- */
    MT_CHECK_EQ_I64(tc_query_support(3u, 0u, 10u, 0u), TC_OK);
    MT_CHECK_EQ_I64(tc_query_support(3u, 0u, 10u, 1u), TC_OK);
    MT_CHECK_EQ_I64(tc_query_support(3u, 0u, 10u, 2u), TC_OK);
    MT_CHECK_EQ_I64(tc_query_support(4u, 0u, 10u, 0u), TC_ERR_UNSUPPORTED_PROFILE);
    MT_CHECK_EQ_I64(tc_query_support(0u, 0u, 10u, 0u), TC_ERR_UNSUPPORTED_PROFILE);
    MT_CHECK_EQ_I64(tc_query_support(1u, 0u, 10u, 0u), TC_ERR_UNSUPPORTED_PROFILE);
    MT_CHECK_EQ_I64(tc_query_support(2u, 0u, 10u, 0u), TC_ERR_UNSUPPORTED_PROFILE);
    /* R4.4：Pro444（5）= 4:4:4 10/12；Extreme（6）= 4:4:4 12 only */
    MT_CHECK_EQ_I64(tc_query_support(5u, 1u, 10u, 0u), TC_OK);
    MT_CHECK_EQ_I64(tc_query_support(5u, 2u, 12u, 2u), TC_OK);
    MT_CHECK_EQ_I64(tc_query_support(5u, 0u, 10u, 0u), TC_ERR_UNSUPPORTED_PIXEL_FORMAT);
    MT_CHECK_EQ_I64(tc_query_support(6u, 1u, 12u, 0u), TC_OK);
    MT_CHECK_EQ_I64(tc_query_support(6u, 2u, 12u, 2u), TC_OK);
    MT_CHECK_EQ_I64(tc_query_support(6u, 1u, 10u, 0u), TC_ERR_UNSUPPORTED_PIXEL_FORMAT);
    MT_CHECK_EQ_I64(tc_query_support(6u, 0u, 12u, 0u), TC_ERR_UNSUPPORTED_PIXEL_FORMAT);
    /* R4.1（v1.2 枚举扩展）：YUV 4:2:2 12-bit 已支持；11/13 仍拒绝。
     * R4.2（v1.3 枚举扩展）：YUV 4:4:4（pf=1）10/12-bit 已支持；
     * pf=2（GBR）保留 → R4.3，pf=3 未知拒绝 */
    MT_CHECK_EQ_I64(tc_query_support(3u, 0u, 12u, 0u), TC_OK);
    MT_CHECK_EQ_I64(tc_query_support(3u, 0u, 12u, 2u), TC_OK);
    MT_CHECK_EQ_I64(tc_query_support(3u, 1u, 10u, 0u), TC_OK);
    MT_CHECK_EQ_I64(tc_query_support(3u, 1u, 12u, 2u), TC_OK);
    MT_CHECK_EQ_I64(tc_query_support(3u, 2u, 10u, 0u), TC_OK);
    MT_CHECK_EQ_I64(tc_query_support(3u, 2u, 12u, 2u), TC_OK);
    MT_CHECK_EQ_I64(tc_query_support(3u, 3u, 10u, 0u), TC_ERR_UNSUPPORTED_PIXEL_FORMAT);
    MT_CHECK_EQ_I64(tc_query_support(3u, 0u, 11u, 0u), TC_ERR_UNSUPPORTED_PIXEL_FORMAT);
    MT_CHECK_EQ_I64(tc_query_support(3u, 0u, 13u, 0u), TC_ERR_UNSUPPORTED_PIXEL_FORMAT);
    MT_CHECK_EQ_I64(tc_query_support(3u, 1u, 11u, 0u), TC_ERR_UNSUPPORTED_PIXEL_FORMAT);
    MT_CHECK_EQ_I64(tc_query_support(3u, 0u, 8u, 0u), TC_ERR_UNSUPPORTED_PIXEL_FORMAT);
    MT_CHECK_EQ_I64(tc_query_support(3u, 0u, 10u, 3u), TC_ERR_UNSUPPORTED_ALPHA_MODE);

    return MT_MAIN_RETURN();
}
