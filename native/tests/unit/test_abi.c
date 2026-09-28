/* 公共 ABI：version/query/状态码/struct 布局约定 */
#include "topos_codec.h"
#include "topos_codec_version.h"
#include "mini_test.h"

#include <string.h>

int main(void)
{
    /* abi 版本 */
    MT_CHECK_EQ_I64(tc_abi_version(), TOPOS_CODEC_ABI_VERSION);

    /* tc_version 正常路径：字段填充 + reserved 不被触碰 */
    topos_version_info info;
    memset(&info, 0xAA, sizeof info);
    MT_CHECK_EQ_I64(tc_version(&info), TC_OK);
    MT_CHECK_EQ_U64(info.struct_size, sizeof(topos_version_info));
    MT_CHECK_EQ_U64(info.abi_version, TOPOS_CODEC_ABI_VERSION);
    MT_CHECK_EQ_U64(info.version_major, TOPOS_CODEC_VERSION_MAJOR);
    MT_CHECK_EQ_U64(info.version_minor, TOPOS_CODEC_VERSION_MINOR);
    MT_CHECK_EQ_U64(info.version_patch, TOPOS_CODEC_VERSION_PATCH);
    MT_CHECK(info.git_commit != NULL && info.git_commit[0] != '\0');
    MT_CHECK(info.build_target != NULL && info.build_target[0] != '\0');
    for (int i = 0; i < 4; ++i) {
        MT_CHECK_EQ_U64(info.reserved[i], 0xAAAAAAAAu);
    }

    /* NULL → INVALID_ARGUMENT + last_error 非空 */
    MT_CHECK_EQ_I64(tc_version(NULL), TC_ERR_INVALID_ARGUMENT);
    const char* err = tc_last_error();
    MT_CHECK(err != NULL && err[0] != '\0');
    MT_CHECK(strstr(err, "NULL") != NULL);

    /* 状态码消息：已知码非空；未知码 → "unknown status" */
    static const int32_t known[] = {
        TC_OK, TC_WARN_CONCEALED,
        TC_ERR_INVALID_ARGUMENT, TC_ERR_OUT_OF_MEMORY, TC_ERR_UNSUPPORTED_VERSION,
        TC_ERR_UNSUPPORTED_PROFILE, TC_ERR_UNSUPPORTED_PIXEL_FORMAT,
        TC_ERR_UNSUPPORTED_MATRIX, TC_ERR_UNSUPPORTED_ALPHA_MODE,
        TC_ERR_LIMIT_EXCEEDED, TC_ERR_MALFORMED, TC_ERR_TRUNCATED,
        TC_ERR_CHECKSUM_MISMATCH, TC_ERR_STATE, TC_ERR_CANCELLED, TC_ERR_IO,
        TC_ERR_BUFFER_TOO_SMALL, TC_ERR_NOT_IMPLEMENTED
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        const char* msg = tc_status_message(known[i]);
        MT_CHECK(msg != NULL && msg[0] != '\0');
    }
    MT_CHECK(strcmp(tc_status_message(12345), "unknown status") == 0);
    MT_CHECK(strcmp(tc_status_message(-9999), "unknown status") == 0);
    MT_CHECK(strcmp(tc_status_message(2), "unknown status") == 0);

    /* 值冻结校验（spec §9 逐项） */
    MT_CHECK_EQ_I64(TC_OK, 0);
    MT_CHECK_EQ_I64(TC_WARN_CONCEALED, 1);
    MT_CHECK_EQ_I64(TC_ERR_INVALID_ARGUMENT, -1);
    MT_CHECK_EQ_I64(TC_ERR_LIMIT_EXCEEDED, -8);
    MT_CHECK_EQ_I64(TC_ERR_MALFORMED, -9);
    MT_CHECK_EQ_I64(TC_ERR_TRUNCATED, -10);
    MT_CHECK_EQ_I64(TC_ERR_CHECKSUM_MISMATCH, -11);
    MT_CHECK_EQ_I64(TC_ERR_NOT_IMPLEMENTED, -16);

    /* tc_query_cpu_features */
    topos_cpu_features cf;
    memset(&cf, 0xAA, sizeof cf);
    MT_CHECK_EQ_I64(tc_query_cpu_features(&cf), TC_OK);
    MT_CHECK_EQ_U64(cf.struct_size, sizeof(topos_cpu_features));
    MT_CHECK_EQ_U64(cf.abi_version, TOPOS_CODEC_ABI_VERSION);
    for (int i = 0; i < 5; ++i) {
        MT_CHECK_EQ_U64(cf.reserved[i], 0xAAAAAAAAu);
    }
    /* 已知能力位之外的位必须为 0 */
    MT_CHECK_EQ_U64(cf.flags & ~(uint32_t)(TOPOS_CPU_X86_AVX2 | TOPOS_CPU_X86_AVX512F |
                                           TOPOS_CPU_ARM_NEON | TOPOS_CPU_X86_FMA), 0u);
#if defined(__aarch64__)
    MT_CHECK((cf.flags & TOPOS_CPU_ARM_NEON) != 0);
#endif
    MT_CHECK_EQ_I64(tc_query_cpu_features(NULL), TC_ERR_INVALID_ARGUMENT);

    /* RD7-01：公共 capability gate 必须只报告已经可消费的路径；reserved
     * 仍遵循 ABI 约定由库不触碰。 */
    topos_codec_capabilities caps;
    memset(&caps, 0xAA, sizeof caps);
    MT_CHECK_EQ_I64(tc_query_capabilities(&caps), TC_OK);
    MT_CHECK_EQ_U64(caps.struct_size, sizeof(topos_codec_capabilities));
    MT_CHECK_EQ_U64(caps.abi_version, TOPOS_CODEC_ABI_VERSION);
    MT_CHECK_EQ_U64(caps.max_width, TC_CODEC_MAX_DIM);
    MT_CHECK_EQ_U64(caps.max_height, TC_CODEC_MAX_DIM);
    MT_CHECK_EQ_U64(caps.auto2k_max_dim, TC_CODEC_AUTO_2K_MAX_DIM);
    MT_CHECK_EQ_U64(caps.flags,
                    TC_CODEC_CAP_FULL_DECODE |
                    TC_CODEC_CAP_FIXED_REDUCED_DECODE |
                    TC_CODEC_CAP_AUTO_2K_DECODE |
                    TC_CODEC_CAP_CPU_SURFACE_OUTPUT |
                    TC_CODEC_CAP_HOST_VISIBLE_GPU_SURFACE |
                    TC_CODEC_CAP_DECODE_DROP_ALPHA);
                    /* V 代际收纳（2026-09-13）：V7-A 位随归档不再广播；
                     * 半尺寸优化（2026-09-20）：DECODE_DROP_ALPHA 随
                     * TC_DECODE_FLAG_DROP_ALPHA 支持一并广播 */
    for (int i = 0; i < 4; ++i) {
        MT_CHECK_EQ_U64(caps.reserved[i], 0xAAAAAAAAu);
    }
    MT_CHECK_EQ_I64(tc_query_capabilities(NULL), TC_ERR_INVALID_ARGUMENT);


    /* P0-04：V7-B scalable 四态发布状态。reference/experimental 语义下
     * capability 位图不得包含 V7B_SCALABLE_BASE（release 能力位）。 */
    topos_scalable_status scalable = TC_SCALABLE_STATUS_DEFAULT;
    MT_CHECK_EQ_I64(tc_query_scalable_status(&scalable), TC_OK);
    MT_CHECK(scalable == TC_SCALABLE_STATUS_EXPERIMENTAL ||
             scalable == TC_SCALABLE_STATUS_REFERENCE ||
             scalable == TC_SCALABLE_STATUS_ELIGIBLE ||
             scalable == TC_SCALABLE_STATUS_DEFAULT);
    if (scalable == TC_SCALABLE_STATUS_EXPERIMENTAL ||
        scalable == TC_SCALABLE_STATUS_REFERENCE) {
        MT_CHECK_EQ_U64(caps.flags & TC_CODEC_CAP_V7B_SCALABLE_BASE, 0u);
    }
    MT_CHECK(strcmp(tc_scalable_status_name(scalable), "experimental") == 0 ||
             strcmp(tc_scalable_status_name(scalable), "reference") == 0 ||
             strcmp(tc_scalable_status_name(scalable), "eligible") == 0 ||
             strcmp(tc_scalable_status_name(scalable), "default") == 0);
    MT_CHECK(strcmp(tc_scalable_status_name((topos_scalable_status)99), "unknown") == 0);
    MT_CHECK(tc_scalable_status_name(TC_SCALABLE_STATUS_REFERENCE) != NULL);
    MT_CHECK_EQ_I64(tc_query_scalable_status(NULL), TC_ERR_INVALID_ARGUMENT);

    return MT_MAIN_RETURN();
}
