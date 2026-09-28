#include "frame_header.h"

#include <string.h>

#include "../common/crc32.h"
#include "../common/endian.h"
#include "../common/error.h"
#include "../transform/quant.h"

/* 首个不通过的字段按 §4.2 表序返回；详情写线程局部错误 */

static int32_t vfail(int32_t code, const char* field, unsigned long long detail)
{
    tc_set_error(code, "frame header field %s = %llu rejected", field, detail);
    return code;
}

/* —— V 代际收纳（D2，2026-09-13）：退役代际读端闸门 ——
 * 默认（生产）构建零回放面：退役代际一律 UNSUPPORTED_VERSION，信息含
 * 退役指引。考古回放需双重门：构建期 -DTOPOS_DEV_REPLAY（CMake 同名
 * option，默认 OFF）+ 运行期环境 TOPOS_DEV=1——防止任何默认产物静默
 * 解码退役位流。em/major 编号永久封存不复用。 */
#if defined(TOPOS_DEV_REPLAY)
#include <stdlib.h>
static int tc_dev_replay_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("TOPOS_DEV");
        cached = (env != NULL && env[0] == '1') ? 1 : 0;
    }
    return cached;
}
#else
static int tc_dev_replay_enabled(void)
{
    return 0;
}
#endif

int tc_frame_header_dev_replay_enabled(void)
{
    return tc_dev_replay_enabled();
}

static const char* retire_name(unsigned major)
{
    switch (major) {
    case 2u: return "V2 Rice";
    case 3u: return "V3 intra";
    case 4u: return "V4 C1 pair";
    case 5u: return "V5 C2 table";
    case 6u: return "V6 range intra";
    case 7u: return "V7-R rans";
    default: return "retired generation";
    }
}

static int32_t vretire(unsigned major)
{
    tc_set_error(TC_ERR_UNSUPPORTED_VERSION,
                 "generation %u (%s) retired (V consolidation 2026-09-13, "
                 "see ADR-C0xx); archived streams replay only with a "
                 "TOPOS_DEV_REPLAY build and TOPOS_DEV=1",
                 major, retire_name(major));
    return TC_ERR_UNSUPPORTED_VERSION;
}

int32_t tc_frame_header_validate_v7a_contract(const topos_frame_header* fh)
{
    if (fh == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a frame header is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (fh->version_major != 7u) {
        return vfail(TC_ERR_UNSUPPORTED_VERSION, "version_major(V7-A)", fh->version_major);
    }
    if (fh->version_minor != 0u) {
        return vfail(TC_ERR_UNSUPPORTED_VERSION, "v7a version_minor", fh->version_minor);
    }
    if (fh->entropy_mode != 5u || fh->codebook_version != 5u || fh->coding_mode != 2u) {
        return vfail(TC_ERR_UNSUPPORTED_VERSION, "v7a entropy/codebook/coding",
                     ((unsigned long long)fh->entropy_mode << 16) |
                     ((unsigned long long)fh->codebook_version << 8) |
                     (unsigned long long)fh->coding_mode);
    }
    if ((fh->flags & ~(uint16_t)0x0001u) != 0u) {
        return vfail(TC_ERR_UNSUPPORTED_VERSION, "v7a flags", fh->flags);
    }
    if (fh->frame_type != 0u || fh->gop_id != 0u || fh->ref_distance != 0u) {
        return vfail(TC_ERR_UNSUPPORTED_VERSION, "v7a frame_type/gop/ref",
                     ((unsigned long long)fh->frame_type << 24) |
                     ((unsigned long long)fh->gop_id << 8) |
                     (unsigned long long)fh->ref_distance);
    }
    return TC_OK;
}

static int32_t validate_impl(const topos_frame_header* fh, int allow_retired)
{
    /* —— 版本分流（ADR-C027 D-8：单点；major 决定规则组）—— */
    if (fh->version_major == 2u || fh->version_major == 3u ||
        fh->version_major == 4u || fh->version_major == 5u ||
        fh->version_major == 6u || fh->version_major == 7u ||
        fh->version_major == 8u || fh->version_major == 9u) {
        /* V 代际收纳（2026-09-13）：major 3..6 与 V2-Rice/V7-R 组合退役。
         * major 3..6 已被 decode 接受清单前置闸门拦截（validate 亦单独
         * 把关——公共 API 可直接调用），此处兜底；V2-Rice/V7-R 在保留
         * major 内按熵字段拦截。allow_retired 仅限内部规则域复用
         * （validate_v7a_common_fields 把 V7-A 元数据伪装成 (6,4) 复用
         * V6 域规则——那是规则载体不是真实 V6 流，代际裁决归 V7-A
         * 专用合同），公共入口恒 = tc_dev_replay_enabled()。 */
        if (fh->version_major >= 3u && fh->version_major <= 6u &&
            allow_retired == 0) {
            return vretire(fh->version_major);
        }
        if (fh->version_major == 2u && fh->entropy_mode == 0u &&
            allow_retired == 0) {
            return vretire(2u);
        }
        if (fh->version_major == 7u && fh->entropy_mode == 6u &&
            allow_retired == 0) {
            return vretire(7u);
        }
        /* V2.0：minor 恒 0；占位字段一律干净拒绝（V2.1 启用时修订本 ADR） */
        if (fh->version_minor != 0u) {
            return vfail(TC_ERR_UNSUPPORTED_VERSION, "v2 version_minor", fh->version_minor);
        }
        if (fh->version_major == 4u) {
            if (fh->entropy_mode != 2u || fh->codebook_version != 2u ||
                fh->coding_mode != 0u) {
                return vfail(TC_ERR_UNSUPPORTED_VERSION, "v4 C1 entropy fields",
                             fh->entropy_mode);
            }
        } else if (fh->version_major == 5u) {
            if (fh->entropy_mode != 3u || fh->codebook_version != 3u ||
                fh->coding_mode != 0u) {
                return vfail(TC_ERR_UNSUPPORTED_VERSION, "v5 C2 entropy fields",
                             fh->entropy_mode);
            }
        } else if (fh->version_major == 6u) {
            if (fh->entropy_mode != 4u || fh->codebook_version != 4u ||
                fh->coding_mode != 1u) {
                return vfail(TC_ERR_UNSUPPORTED_VERSION, "v6 range entropy fields",
                             fh->entropy_mode);
            }
        } else if (fh->version_major == 7u) {
            /* V7-R（ADR-C034）/V7-R2（ADR-C036）/V7-R3（ADR-C048）：
             * major=7 的产品路径 = V2 语义 + per-slice rANS；熵三元组
             * (6,0,0)/(7,0,0)/(8,0,0)——R3 = R2 同构熵机承载帧间微 GOP
             * （GOP 字段激活见下方规则组）。V7-A 实验包 (5,5,2) 只经
             * v7a 专用入口（tc_packet_is_v7a 按 entropy_mode 分流），
             * 此处保持拒绝。 */
            if (!((fh->entropy_mode == 6u || fh->entropy_mode == 7u ||
                   fh->entropy_mode == 8u) &&
                  fh->codebook_version == 0u && fh->coding_mode == 0u)) {
                return vfail(TC_ERR_UNSUPPORTED_VERSION, "v7 rans entropy fields",
                             fh->entropy_mode);
            }
        } else if (fh->version_major == 8u) {
            /* V8（topos_v8_format_plan 批 1 冻结）：段化多链 rANS +
             * 瓦片共享表——熵三元组唯一合法 (8,0,0)；包结构由
             * tc_packet_scan_v8 专扫（扩展头/段目录/瓦片表/瓦片 CRC），
             * V7 扫描入口对 major=8 明确拒绝。 */
            if (!(fh->entropy_mode == 8u && fh->codebook_version == 0u &&
                  fh->coding_mode == 0u)) {
                return vfail(TC_ERR_UNSUPPORTED_VERSION, "v8 entropy fields",
                             fh->entropy_mode);
            }
        } else if (fh->version_major == 9u) {
            /* V9（topos_v9_micro_gop_plan 批 1 冻结；ADR-C047 zero-motion
             * IP-2）：包结构与 V8 同构（major 区分），熵三元组唯一合法
             * (9,0,0)；frame_type/gop_id/ref_distance 本代激活（下方
             * GOP 规则组强校验，V2.1 强制零不适用于 major=9）。 */
            if (!(fh->entropy_mode == 9u && fh->codebook_version == 0u &&
                  fh->coding_mode == 0u)) {
                return vfail(TC_ERR_UNSUPPORTED_VERSION, "v9 entropy fields",
                             fh->entropy_mode);
            }
        } else if (fh->entropy_mode > 1u) {
            return vfail(TC_ERR_UNSUPPORTED_VERSION, "entropy_mode", fh->entropy_mode);
        }
        if (fh->version_major == 3u && fh->entropy_mode != 1u) {
            return vfail(TC_ERR_UNSUPPORTED_VERSION, "v3 entropy_mode", fh->entropy_mode);
        }
        if (fh->version_major != 4u && fh->version_major != 5u &&
            fh->version_major != 6u && fh->version_major != 7u &&
            fh->version_major != 8u && fh->version_major != 9u &&
            (fh->entropy_mode == 0u ? fh->codebook_version != 0u
                                     : fh->codebook_version != 1u)) {
            return vfail(TC_ERR_UNSUPPORTED_VERSION, "codebook_version",
                         fh->codebook_version);
        }
        if (fh->version_major != 4u && fh->version_major != 5u &&
            fh->version_major != 6u && fh->version_major != 7u &&
            fh->version_major != 8u && fh->version_major != 9u &&
            fh->coding_mode != (fh->version_major == 3u ? 1u : 0u)) {
            return vfail(TC_ERR_UNSUPPORTED_VERSION, "coding_mode", fh->coding_mode);
        }
        if ((fh->flags & ~(uint16_t)0x0003u) != 0u) {
            /* V7-R4/R5（2026-09-21 锯齿战役 P6）：bit2=DEBLOCK /
             * bit3=QPT2 细化表合法化——但仅产品 4:2:2 10-bit intra 路径
             * （major 7 / 熵 7（V7-R2）/ I 帧 / profile 3 / bd 10，与
             * qmatrix_id=4 域同形）；其余保留位与域外使用仍干净拒绝
             *（旧解码器行为不变：任何保留位非零 → MALFORMED）。 */
            const uint16_t new_bits = (uint16_t)(fh->flags & 0x000Cu);
            if ((fh->flags & ~(uint16_t)(0x0003u | 0x000Cu)) != 0u) {
                return vfail(TC_ERR_MALFORMED, "v2 flags(reserved bits)", fh->flags);
            }
            if (new_bits != 0u) {
                const int dom = fh->version_major == 7u &&
                                fh->entropy_mode == 7u &&
                                fh->frame_type == 0u &&
                                fh->profile == 3u &&
                                fh->bit_depth == 10u;
                if (!dom) {
                    return vfail(TC_ERR_UNSUPPORTED_VERSION,
                                 "flags r4/r5 domain", fh->flags);
                }
            }
        }
        if ((fh->flags & 0x0002u) != 0u) { /* tile_layout：V2.0 未实现 */
            return vfail(TC_ERR_UNSUPPORTED_VERSION, "tile_layout(V2.0)", fh->flags);
        }
        if (fh->version_major == 9u ||
            (fh->version_major == 7u && fh->entropy_mode == 8u)) {
            /* GOP 字段规则组（topos_v9_micro_gop_plan §3.2 冻结；
             * V7-R3 同构沿用，ADR-C048 D3。ADR-C047 P0：仅
             * frame_type∈{0=I,1=P}、ref_distance∈{0,1}）：
             * I 帧 ref_distance 恒 0；P 帧恒 1（只参考同 GOP 紧邻上一张
             * 重建帧）；gop_id 全域 u16（回绕是编码器纪律，跨帧一致性
             * 归 GOP context 序列校验，非单帧头域）。 */
            if (fh->frame_type > 1u) {
                return vfail(TC_ERR_UNSUPPORTED_VERSION, "v9 frame_type",
                             fh->frame_type);
            }
            if (fh->frame_type == 0u && fh->ref_distance != 0u) {
                return vfail(TC_ERR_MALFORMED, "v9 I ref_distance(!=0)",
                             fh->ref_distance);
            }
            if (fh->frame_type == 1u && fh->ref_distance != 1u) {
                return vfail(TC_ERR_MALFORMED, "v9 P ref_distance(!=1)",
                             fh->ref_distance);
            }
        } else if (fh->frame_type != 0u || fh->gop_id != 0u || fh->ref_distance != 0u) {
            return vfail(TC_ERR_UNSUPPORTED_VERSION, "frame_type/gop/ref(V2.1)",
                         fh->frame_type);
        }
    } else if (fh->version_major != 1u) {
        return vfail(TC_ERR_UNSUPPORTED_VERSION, "version_major", fh->version_major);
    } else {
        /* V1 永久规则：V2 字段全 0（等价旧 reserved0==0）+ 旧 flags/帧字段域 */
        if (fh->entropy_mode != 0u || fh->codebook_version != 0u ||
            fh->coding_mode != 0u) {
            return vfail(TC_ERR_MALFORMED, "v1 entropy fields(!=0)", fh->entropy_mode);
        }
        if ((fh->flags & ~(uint16_t)0x0001u) != 0u) {
            return vfail(TC_ERR_MALFORMED, "flags(reserved bits)", fh->flags);
        }
        if (fh->frame_type != 0u) {
            return vfail(TC_ERR_UNSUPPORTED_VERSION, "frame_type(V2.1)", fh->frame_type);
        }
        if (fh->gop_id != 0u) {
            return vfail(TC_ERR_MALFORMED, "gop_id(reserved)", fh->gop_id);
        }
        if (fh->ref_distance != 0u) {
            return vfail(TC_ERR_MALFORMED, "ref_distance(reserved)", fh->ref_distance);
        }
    }
    if (fh->profile < 1u || fh->profile > 7u) {
        return vfail(TC_ERR_UNSUPPORTED_PROFILE, "profile", fh->profile);
    }
    if (fh->profile == 7u) {
        /* TRAW（topos_traw_format_plan；2026-09-13 拍板）：profile 7 ⇔
         * pf=3（CFA 4 相位平面）∧ bd ∈ {12（PWL-log12 制作档，批 1）/
         * 16（linear 归档档，批 4 内核加宽）} ∧ 无 alpha ∧ qm0（冻结）∧
         * 无色度语义（siting/matrix=0）∧ transfer 随位深冻结
         * （12→TRAW_LOG0 / 16→linear）。旧读端按未知 profile 干净拒绝；
         * V1 流另受 minor 扩展代门控（v1.6=bd12 / v1.7=bd16）。 */
        if (fh->pixel_format != 3u) {
            return vfail(TC_ERR_MALFORMED, "profile(TRAW requires CFA pf=3)",
                         fh->pixel_format);
        }
        if (fh->bit_depth != 12u && fh->bit_depth != 16u) {
            return vfail(TC_ERR_MALFORMED, "profile(TRAW requires 12/16-bit)",
                         fh->bit_depth);
        }
        if (fh->alpha_mode != 0u) {
            return vfail(TC_ERR_MALFORMED, "profile(TRAW alpha unsupported)", fh->alpha_mode);
        }
        if (fh->qmatrix_id != 0u) {
            return vfail(TC_ERR_MALFORMED, "profile(TRAW frozen qm0)", fh->qmatrix_id);
        }
        const uint8_t traw_transfer = (fh->bit_depth == 12u)
                                          ? TC_TRANSFER_TRAW_LOG0 : 8u;
        if (fh->color_transfer != traw_transfer) {
            return vfail(TC_ERR_MALFORMED,
                         "profile(TRAW transfer mismatch for bit depth)",
                         fh->color_transfer);
        }
    } else {
        if (fh->profile != 3u && fh->profile != 5u && fh->profile != 6u) {
            /* §4.4：v1 已验收 Standard；R4.4 激活 Pro444(5)/Extreme(6)。
             * Proxy(1)/LT(2)/HQ(4) 由应用层以 profile3+质量预设表达（ADR-C011）。 */
            return vfail(TC_ERR_UNSUPPORTED_PROFILE, "profile(not 3/5/6)", fh->profile);
        }
        if (fh->pixel_format == 3u) {
            /* pf=3 只随 TRAW（profile 7）出现——反方向交叉 */
            return vfail(TC_ERR_MALFORMED, "pixel_format(CFA requires TRAW profile 7)",
                         fh->pixel_format);
        }
    }
    /* R4.4：格式交叉规则——Pro444 = 4:4:4 10/12；Extreme = 4:4:4 12-bit。
     * Standard(3) 不限（v1.2–v1.4 枚举扩展均以 profile3 交付）。 */
    if (fh->profile == 5u || fh->profile == 6u) {
        if (fh->pixel_format == 0u) {
            return vfail(TC_ERR_MALFORMED,
                         "profile/format(Pro444|Extreme requires 4:4:4)",
                         fh->pixel_format);
        }
        if (fh->profile == 6u && fh->bit_depth != 12u) {
            return vfail(TC_ERR_MALFORMED,
                         "profile/format(Extreme requires 12-bit)",
                         fh->bit_depth);
        }
    }
    if (fh->pixel_format > 3u) {
        return vfail(TC_ERR_UNSUPPORTED_PIXEL_FORMAT, "pixel_format", fh->pixel_format);
    }
    /* v1.2（R4.1）：YUV 4:2:2 位深枚举扩展 {10,12}（minor=1）；
     * v1.3（R4.2）：YUV 4:4:4 枚举扩展 pf=1（minor=2）；
     * v1.4（R4.3）：GBR 4:4:4 枚举扩展 pf=2（minor=3，matrix=0 契约）；
     * v1.5（ADR-C031）：qp 域扩展 64..95（minor=4，Proxy/LT 低码率档）；
     * v1.6（TRAW 批 1）：CFA 枚举扩展 pf=3+profile=7（minor=5）。
     * 均为 minor bump，旧解码器按 minor>已知 或未知枚举干净拒绝；
     * 扩展枚举不得出现在低于其扩展代的 minor 流。
     * V2：minor 恒 0 且全枚举开放（spec v2 §1）——本组规则仅 major=1。 */
    if (fh->bit_depth != 10u && fh->bit_depth != 12u && fh->bit_depth != 16u) {
        /* v1.7（批 4）：bd=16 枚举开放（TRAW linear 归档 + 视频/图片 16-bit） */
        return vfail(TC_ERR_UNSUPPORTED_PIXEL_FORMAT, "bit_depth", fh->bit_depth);
    }
    if (fh->version_major == 1u) {
        if (fh->version_minor > 7u) {
            /* H1（2026-09-21）：v1.8 扩展代（sRGB EOTF transfer=13 载体）
             * 开放 minor=7；>7 仍 UNSUPPORTED（§13.1 前向安全） */
            return vfail(TC_ERR_UNSUPPORTED_VERSION, "version_minor", fh->version_minor);
        }
        if (fh->version_minor == 0u &&
            (fh->bit_depth != 10u || fh->pixel_format != 0u)) {
            /* v1.0 声明的流不得使用扩展枚举（旧解码器按未知枚举拒绝；
             * 本解码器按版本声明不一致拒绝——枚举本身已知，非 MALFORMED） */
            return vfail(TC_ERR_UNSUPPORTED_VERSION,
                         "v1.2 extended enum in version_minor=0 stream",
                         fh->bit_depth);
        }
        if (fh->version_minor == 1u && fh->pixel_format != 0u) {
            return vfail(TC_ERR_UNSUPPORTED_VERSION,
                         "v1.3 extended enum in version_minor=1 stream",
                         fh->pixel_format);
        }
        if (fh->version_minor == 2u && fh->pixel_format > 1u) {
            /* v1.4 扩展枚举（pf=2）不得出现在 v1.3（minor=2）包络 */
            return vfail(TC_ERR_UNSUPPORTED_VERSION,
                         "v1.4 extended enum in version_minor=2 stream",
                         fh->pixel_format);
        }
        if (fh->version_minor < 5u &&
            (fh->pixel_format == 3u || fh->profile == 7u)) {
            /* v1.6 扩展枚举（pf=3/profile=7 TRAW）不得出现在 minor<5 包络
             * （旧读端 minor 域校验先于此拒绝——UNSUPPORTED_VERSION 契约） */
            return vfail(TC_ERR_UNSUPPORTED_VERSION,
                         "v1.6 extended enum in version_minor<5 stream",
                         fh->pixel_format);
        }
        if (fh->version_minor < 6u && fh->bit_depth == 16u) {
            /* v1.7 扩展枚举（bd=16，批 4）不得出现在 minor<6 包络 */
            return vfail(TC_ERR_UNSUPPORTED_VERSION,
                         "v1.7 extended enum in version_minor<6 stream",
                         fh->bit_depth);
        }
    }
    if (fh->alpha_mode > 2u) {
        return vfail(TC_ERR_MALFORMED, "alpha_mode", fh->alpha_mode);
    }
    if (fh->alpha_mode == 0u) {
        if (fh->alpha_bit_depth != 0u) {
            return vfail(TC_ERR_MALFORMED, "alpha_bit_depth(!=0 w/o alpha)", fh->alpha_bit_depth);
        }
    } else if (fh->alpha_mode == 1u) {
        if (fh->alpha_bit_depth != 16u) {
            return vfail(TC_ERR_MALFORMED, "alpha_bit_depth(!=16 lossless)", fh->alpha_bit_depth);
        }
    } else { /* alpha_mode == 2：受限近似（spec v1.1 §8.6，阶段 4 实现激活） */
        if (fh->alpha_bit_depth != 8u && fh->alpha_bit_depth != 10u &&
            fh->alpha_bit_depth != 12u) {
            return vfail(TC_ERR_MALFORMED, "alpha_bit_depth(!in{8,10,12} near-lossless)",
                         fh->alpha_bit_depth);
        }
    }
    /* frame_type/gop_id/ref_distance：major 分流块已按 V1/V2 规则校验 */
    if (fh->coded_width == 0u || fh->coded_height == 0u ||
        (fh->coded_width % 8u) != 0u || (fh->coded_height % 8u) != 0u ||
        fh->coded_width > TC_MAX_CODED_DIM || fh->coded_height > TC_MAX_CODED_DIM) {
        return vfail(TC_ERR_LIMIT_EXCEEDED, "coded_w/h(16384,%8)", fh->coded_width);
    }
    if (fh->visible_width == 0u || fh->visible_height == 0u) {
        return vfail(TC_ERR_MALFORMED, "visible_w/h(>=1)", fh->visible_width);
    }
    if (fh->visible_width > fh->coded_width || fh->visible_height > fh->coded_height) {
        return vfail(TC_ERR_LIMIT_EXCEEDED, "visible>coded", fh->visible_width);
    }
    /* pf=3（TRAW CFA）恒 4 相位平面且无 alpha（交叉规则已拒 alpha≠0）；
     * 其余 pf：3 平面 + 可选 alpha。 */
    uint8_t expect_planes = (fh->pixel_format == 3u)
                                ? 4u
                                : (uint8_t)(3u + (fh->alpha_mode != 0u ? 1u : 0u));
    if (fh->plane_count != expect_planes) {
        return vfail(TC_ERR_MALFORMED, "plane_count", fh->plane_count);
    }
    if (fh->qmatrix_id > 4u) {
        return vfail(TC_ERR_UNSUPPORTED_MATRIX, "qmatrix_id", fh->qmatrix_id);
    }
    if (fh->qmatrix_id == 2u &&
        !(fh->pixel_format == 2u && fh->bit_depth == 12u &&
          (fh->profile == 5u || fh->profile == 6u))) {
        return vfail(TC_ERR_UNSUPPORTED_MATRIX,
                     "qmatrix_id=2 requires GBR 4:4:4 12-bit profile 5/6",
                     fh->qmatrix_id);
    }
    if (fh->qmatrix_id == 3u &&
        !(fh->pixel_format == 0u && fh->bit_depth == 10u && fh->profile == 3u)) {
        return vfail(TC_ERR_UNSUPPORTED_MATRIX,
                     "qmatrix_id=3 requires YUV 4:2:2 10-bit profile 3",
                     fh->qmatrix_id);
    }
    /* id=4「边缘均衡」（2026-09-21 画质战役）：与 id=3 同域——YUV
     * 4:2:2 10-bit profile 3（422 视频档专用）。 */
    if (fh->qmatrix_id == 4u &&
        !(fh->pixel_format == 0u && fh->bit_depth == 10u && fh->profile == 3u)) {
        return vfail(TC_ERR_UNSUPPORTED_MATRIX,
                     "qmatrix_id=4 requires YUV 4:2:2 10-bit profile 3",
                     fh->qmatrix_id);
    }
    if (fh->qp_base > TC_QP_MAX) {
        return vfail(TC_ERR_MALFORMED, "qp_base", fh->qp_base);
    }
    /* v1.5（ADR-C031）：qp 64..95 只在 version_minor≥4 的 v1 流合法
     * （V2 minor 恒 0 同样开放——V2 解码器随本变更同步支持）。 */
    if (fh->qp_base >= TC_QP_V15_MIN &&
        !((fh->version_major == 1u && fh->version_minor >= 4u) ||
          fh->version_major >= 2u)) {
        return vfail(TC_ERR_UNSUPPORTED_VERSION,
                     "v1.5 qp>=64 in version_minor<4 stream", fh->qp_base);
    }
    if (fh->slice_count == 0u) {
        return vfail(TC_ERR_MALFORMED, "slice_count(>=1)", fh->slice_count);
    }
    if (fh->slice_count > TC_MAX_SLICE_COUNT) {
        return vfail(TC_ERR_LIMIT_EXCEEDED, "slice_count(<=512)", fh->slice_count);
    }
    if (fh->slice_count < (uint16_t)fh->plane_count) {
        return vfail(TC_ERR_MALFORMED, "slice_count(<plane_count)", fh->slice_count);
    }
    if (fh->color_range > 1u) {
        return vfail(TC_ERR_MALFORMED, "color_range", fh->color_range);
    }
    if (fh->chroma_siting > 2u) {
        return vfail(TC_ERR_MALFORMED, "chroma_siting", fh->chroma_siting);
    }
    if ((fh->pixel_format == 2u || fh->pixel_format == 3u) && fh->chroma_siting != 0u) {
        /* R4.3：GBR 无色度子采样，采样位置标签无意义——非 0 即拒；
         * TRAW CFA 同理（相位平面无色度采样语义） */
        return vfail(TC_ERR_MALFORMED, "chroma_siting(!=0 w/ GBR|CFA)", fh->chroma_siting);
    }
    /* 色彩标签：未知即拒（§13.1；解释错误比拒绝更糟）。
     * R4.3 交叉规则：matrix=0（identity）为 GBR（pf=2）契约，GBR 不得携带
     * YUV 矩阵；YUV（pf∈{0,1}）不得携带 identity。TRAW（pf=3）同 GBR：
     * 相位平面无矩阵语义，恒 identity。transfer=TRAW_LOG0 为 TRAW 私有值，
     * 非 TRAW 流携带即拒（解释错误比拒绝更糟）。 */
    switch (fh->color_primaries) {
    case 1: case 6: case 9: case 12: break;
    default: return vfail(TC_ERR_MALFORMED, "color_primaries(tag)", fh->color_primaries);
    }
    switch (fh->color_transfer) {
    case 1: case 8: case 16: case 18: case TC_TRANSFER_TRAW_LOG0: break;
    case 13u:
        /* H1（2026-09-21）：sRGB EOTF（CICP 13）——v1.8 新增枚举。
         * minor≥7 门控仅约束 v1 码流（major=1 扩展代域）；V7/V8/V9
         * 载体（major≥7，minor 恒 0 自描述）不适用——旧读端对 13 一律
         * 按未知 transfer 干净拒绝（本 switch default，§13.1）。
         * H1 冻结原则：写侧显式标注，非读侧猜测。 */
        if (fh->version_major == 1u && fh->version_minor < 7u) {
            return vfail(TC_ERR_UNSUPPORTED_VERSION,
                         "v1.8 sRGB transfer(13) in version_minor<7 stream",
                         fh->version_minor);
        }
        break;
    default: return vfail(TC_ERR_MALFORMED, "color_transfer(tag)", fh->color_transfer);
    }
    /* 批 4：profile 7 位深-传递曲线冻结对（§3.2）——12-bit → LOG0(20)、
     * 16-bit → linear(8)；LOG0 仍为 TRAW 私有值（非 TRAW 流携带即拒）。 */
    if (fh->profile == 7u) {
        const uint32_t traw_transfer = (fh->bit_depth == 16u) ? 8u
                                                              : TC_TRANSFER_TRAW_LOG0;
        if (fh->color_transfer != traw_transfer) {
            return vfail(TC_ERR_MALFORMED,
                         "color_transfer(profile7 frozen pair: 12->LOG0 / 16->linear)",
                         fh->color_transfer);
        }
    } else if (fh->color_transfer == TC_TRANSFER_TRAW_LOG0) {
        return vfail(TC_ERR_MALFORMED, "color_transfer(TRAW_LOG0 is profile-7 private)",
                     fh->color_transfer);
    }
    if (fh->pixel_format == 2u || fh->pixel_format == 3u) {
        if (fh->color_matrix != 0u) {
            return vfail(TC_ERR_MALFORMED,
                         "color_matrix(!=0 identity for GBR)", fh->color_matrix);
        }
    } else {
        switch (fh->color_matrix) {
        case 1: case 5: case 9: break;
        default: return vfail(TC_ERR_MALFORMED,
                              "color_matrix(tag; 0=identity 仅限 GBR)",
                              fh->color_matrix);
        }
    }
    if ((fh->sar_num == 0u) != (fh->sar_den == 0u)) {
        return vfail(TC_ERR_MALFORMED, "sar(0 iff 0)", fh->sar_num);
    }
    if (fh->frame_packet_size == 0u || fh->frame_packet_size > TC_MAX_PACKET_SIZE) {
        return vfail(TC_ERR_LIMIT_EXCEEDED, "frame_packet_size(<=256MiB)", fh->frame_packet_size);
    }
    return TC_OK;
}

int32_t tc_frame_header_validate(const topos_frame_header* fh)
{
    return validate_impl(fh, tc_dev_replay_enabled());
}

int32_t tc_frame_derive_geometry(topos_frame_header* fh)
{
    if (fh->pixel_format == 3u) {
        /* TRAW（批 1）：CFA 4 相位平面 R/Gr/Gb/B——每平面 (W/2)×(H/2)
         * （RGGB 相位下采样；奇数维 ceil/2）。coded 维度 pad8 独立推导。 */
        uint32_t vis_w = ((uint32_t)fh->visible_width + 1u) / 2u;
        uint32_t vis_h = ((uint32_t)fh->visible_height + 1u) / 2u;
        uint16_t coded_w = (uint16_t)(((vis_w + 7u) / 8u) * 8u);
        uint16_t coded_h = (uint16_t)(((vis_h + 7u) / 8u) * 8u);
        for (uint32_t p = 0u; p < 4u; ++p) {
            fh->plane_coded_w[p] = coded_w;
            fh->plane_coded_h[p] = coded_h;
            fh->plane_visible_w[p] = (uint16_t)vis_w;
            fh->plane_visible_h[p] = (uint16_t)vis_h;
            fh->plane_block_cols[p] = (uint16_t)(coded_w / 8u);
            fh->plane_block_rows[p] = (uint16_t)(coded_h / 8u);
        }
        if (coded_w > TC_MAX_CODED_DIM || coded_h > TC_MAX_CODED_DIM) {
            tc_set_error(TC_ERR_LIMIT_EXCEEDED, "cfa plane coded dim exceeds 16384");
            return TC_ERR_LIMIT_EXCEEDED;
        }
        return TC_OK;
    }
    /* R4.2：pf=1（4:4:4）chroma 全宽全高；pf=0（4:2:2）保持 ceil/2 半宽
     * （历史语义；pf=2 GBR 同为全宽，但 validate 先行拒绝，此处仅防御） */
    uint32_t chroma_vis_w = (fh->pixel_format == 0u)
                                ? ((uint32_t)fh->visible_width + 1u) / 2u
                                : (uint32_t)fh->visible_width;
    uint16_t chroma_coded_w = (fh->pixel_format == 0u)
                                  ? (uint16_t)(((chroma_vis_w + 7u) / 8u) * 8u)
                                  : fh->coded_width;

    /* Y 与 [A] */
    for (uint32_t p = 0u; p < 4u; ++p) {
        int is_chroma = (p == 1u || p == 2u);
        fh->plane_coded_w[p] = is_chroma ? chroma_coded_w : fh->coded_width;
        fh->plane_coded_h[p] = fh->coded_height;
        fh->plane_visible_w[p] = is_chroma ? (uint16_t)chroma_vis_w : fh->visible_width;
        fh->plane_visible_h[p] = fh->visible_height;
        fh->plane_block_cols[p] = (uint16_t)(fh->plane_coded_w[p] / 8u);
        fh->plane_block_rows[p] = (uint16_t)(fh->plane_coded_h[p] / 8u);
    }
    if (fh->plane_coded_w[1] > TC_MAX_CODED_DIM || fh->plane_coded_h[1] > TC_MAX_CODED_DIM) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "chroma coded dim exceeds 16384");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    return TC_OK;
}

int32_t tc_frame_header_encode(const topos_frame_header* fh, uint8_t out[TC_FRAME_HEADER_SIZE])
{
    int32_t rc = tc_frame_header_validate(fh);
    if (rc != TC_OK) { return rc; }

    out[0] = (uint8_t)'T'; out[1] = (uint8_t)'P'; out[2] = (uint8_t)'I'; out[3] = (uint8_t)'C';
    tc_store_be16(out + 4, TC_FRAME_HEADER_SIZE);
    out[6] = fh->version_major;
    out[7] = fh->version_minor;
    tc_store_be16(out + 8, fh->flags);
    out[10] = fh->profile;
    out[11] = fh->pixel_format;
    out[12] = fh->bit_depth;
    out[13] = fh->alpha_mode;
    out[14] = fh->alpha_bit_depth;
    out[15] = fh->frame_type;
    tc_store_be16(out + 16, fh->gop_id);
    out[18] = fh->ref_distance;
    tc_store_be16(out + 19, fh->coded_width);
    tc_store_be16(out + 21, fh->coded_height);
    tc_store_be16(out + 23, fh->visible_width);
    tc_store_be16(out + 25, fh->visible_height);
    out[27] = fh->plane_count;
    out[28] = fh->qmatrix_id;
    out[29] = fh->qp_base;
    tc_store_be16(out + 30, fh->slice_count);
    out[32] = fh->color_range;
    out[33] = fh->color_primaries;
    out[34] = fh->color_transfer;
    out[35] = fh->color_matrix;
    out[36] = fh->chroma_siting;
    tc_store_be16(out + 37, fh->sar_num);
    tc_store_be16(out + 39, fh->sar_den);
    tc_store_be32(out + 41, fh->frame_packet_size);
    /* V2 拆分字段（ADR-C027 D-1）；V1 全 0 == 旧 reserved0 */
    out[45] = fh->entropy_mode;
    out[46] = fh->codebook_version;
    out[47] = fh->coding_mode;
    out[48] = 0u; /* reserved_v2_0 */
    tc_store_be32(out + 49, tc_crc32(out, 49u));
    return TC_OK;
}

static int32_t validate_v7a_common_fields(const topos_frame_header* fh)
{
    /* V7-A registers a new entropy/coding tuple, but the rest of the frame
     * metadata keeps the already audited V6 domain rules.  Validate a copy so
     * the V1-V6 validator remains a strict old-version entry point. */
    topos_frame_header common = *fh;
    common.version_major = 6u;
    common.version_minor = 0u;
    common.entropy_mode = 4u;
    common.codebook_version = 4u;
    common.coding_mode = 1u;
    /* (6,4) 是规则域载体非真实 V6 流——绕过代际退役闸门（allow_retired=1）；
     * V7-A/V7-B 的代际裁决由专用合同 + 探测链承担。 */
    return validate_impl(&common, 1);
}

static void frame_header_pack(const topos_frame_header* fh,
                              uint8_t out[TC_FRAME_HEADER_SIZE])
{
    out[0] = (uint8_t)'T'; out[1] = (uint8_t)'P'; out[2] = (uint8_t)'I'; out[3] = (uint8_t)'C';
    tc_store_be16(out + 4, TC_FRAME_HEADER_SIZE);
    out[6] = fh->version_major;
    out[7] = fh->version_minor;
    tc_store_be16(out + 8, fh->flags);
    out[10] = fh->profile;
    out[11] = fh->pixel_format;
    out[12] = fh->bit_depth;
    out[13] = fh->alpha_mode;
    out[14] = fh->alpha_bit_depth;
    out[15] = fh->frame_type;
    tc_store_be16(out + 16, fh->gop_id);
    out[18] = fh->ref_distance;
    tc_store_be16(out + 19, fh->coded_width);
    tc_store_be16(out + 21, fh->coded_height);
    tc_store_be16(out + 23, fh->visible_width);
    tc_store_be16(out + 25, fh->visible_height);
    out[27] = fh->plane_count;
    out[28] = fh->qmatrix_id;
    out[29] = fh->qp_base;
    tc_store_be16(out + 30, fh->slice_count);
    out[32] = fh->color_range;
    out[33] = fh->color_primaries;
    out[34] = fh->color_transfer;
    out[35] = fh->color_matrix;
    out[36] = fh->chroma_siting;
    tc_store_be16(out + 37, fh->sar_num);
    tc_store_be16(out + 39, fh->sar_den);
    tc_store_be32(out + 41, fh->frame_packet_size);
    out[45] = fh->entropy_mode;
    out[46] = fh->codebook_version;
    out[47] = fh->coding_mode;
    out[48] = 0u;
    tc_store_be32(out + 49, tc_crc32(out, 49u));
}

int32_t tc_frame_header_encode_v7a(const topos_frame_header* fh,
                                   uint8_t out[TC_FRAME_HEADER_SIZE])
{
    if (fh == NULL || out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a frame header encode argument is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    int32_t rc = tc_frame_header_validate_v7a_contract(fh);
    if (rc != TC_OK) { return rc; }
    rc = validate_v7a_common_fields(fh);
    if (rc != TC_OK) { return rc; }
    frame_header_pack(fh, out);
    return TC_OK;
}

int32_t tc_frame_header_decode(const uint8_t* data, size_t size, topos_frame_header* fh)
{
    if (size < TC_FRAME_HEADER_SIZE) {
        tc_set_error(TC_ERR_TRUNCATED, "frame header needs %u bytes, got %zu",
                     (unsigned)TC_FRAME_HEADER_SIZE, size);
        return TC_ERR_TRUNCATED;
    }
    if (memcmp(data, TC_FRAME_MAGIC, 4u) != 0) {
        tc_set_error(TC_ERR_MALFORMED, "magic mismatch (not TPIC)");
        return TC_ERR_MALFORMED;
    }
    uint32_t crc_calc = tc_crc32(data, 49u);
    if (tc_load_be32(data + 49) != crc_calc) {
        tc_set_error(TC_ERR_CHECKSUM_MISMATCH, "frame header crc32 %08x != %08x",
                     (unsigned)tc_load_be32(data + 49), (unsigned)crc_calc);
        return TC_ERR_CHECKSUM_MISMATCH;
    }

    memset(fh, 0, sizeof(*fh));
    uint16_t header_size = tc_load_be16(data + 4);
    if (header_size != TC_FRAME_HEADER_SIZE) {
        return vfail(TC_ERR_MALFORMED, "header_size(!=53)", header_size);
    }
    uint8_t major = data[6];
    uint8_t minor = data[7];
    /* V 代际收纳：major 3..6 退役（读端闸门；回放见 tc_dev_replay_enabled）*/
    if (major >= 3u && major <= 6u && tc_dev_replay_enabled() == 0) {
        return vretire(major);
    }
    if (major != 1u && major != 2u && major != 3u && major != 4u && major != 5u &&
        major != 6u && major != 7u && major != 8u && major != 9u) {
        return vfail(TC_ERR_UNSUPPORTED_VERSION, "version_major", major);
    }
    fh->version_major = major;
    fh->version_minor = minor; /* 域校验（版本规则 + 枚举交叉）在 validate */
    fh->flags = tc_load_be16(data + 8);
    fh->profile = data[10];
    fh->pixel_format = data[11];
    fh->bit_depth = data[12];
    fh->alpha_mode = data[13];
    fh->alpha_bit_depth = data[14];
    fh->frame_type = data[15];
    fh->gop_id = tc_load_be16(data + 16);
    fh->ref_distance = data[18];
    fh->coded_width = tc_load_be16(data + 19);
    fh->coded_height = tc_load_be16(data + 21);
    fh->visible_width = tc_load_be16(data + 23);
    fh->visible_height = tc_load_be16(data + 25);
    fh->plane_count = data[27];
    fh->qmatrix_id = data[28];
    fh->qp_base = data[29];
    fh->slice_count = tc_load_be16(data + 30);
    fh->color_range = data[32];
    fh->color_primaries = data[33];
    fh->color_transfer = data[34];
    fh->color_matrix = data[35];
    fh->chroma_siting = data[36];
    fh->sar_num = tc_load_be16(data + 37);
    fh->sar_den = tc_load_be16(data + 39);
    fh->frame_packet_size = tc_load_be32(data + 41);
    if (major == 1u) {
        uint32_t reserved0 = tc_load_be32(data + 45);
        if (reserved0 != 0u) {
            return vfail(TC_ERR_MALFORMED, "reserved0(!=0)", reserved0);
        }
        /* fh->entropy_mode/codebook_version/coding_mode 保持 memset 的 0 */
    } else {
        fh->entropy_mode = data[45];
        fh->codebook_version = data[46];
        fh->coding_mode = data[47];
        if (data[48] != 0u) {
            return vfail(TC_ERR_MALFORMED, "reserved_v2_0(!=0)", data[48]);
        }
    }

    int32_t rc = tc_frame_header_validate(fh);
    if (rc != TC_OK) { return rc; }
    return tc_frame_derive_geometry(fh);
}

int32_t tc_frame_header_decode_v7a(const uint8_t* data, size_t size,
                                   topos_frame_header* fh)
{
    if (fh == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a frame header output is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (size < TC_FRAME_HEADER_SIZE) {
        tc_set_error(TC_ERR_TRUNCATED, "v7a frame header needs %u bytes, got %zu",
                     (unsigned)TC_FRAME_HEADER_SIZE, size);
        return TC_ERR_TRUNCATED;
    }
    if (memcmp(data, TC_FRAME_MAGIC, 4u) != 0) {
        tc_set_error(TC_ERR_MALFORMED, "v7a frame header magic mismatch");
        return TC_ERR_MALFORMED;
    }
    if (tc_load_be32(data + 49u) != tc_crc32(data, 49u)) {
        tc_set_error(TC_ERR_CHECKSUM_MISMATCH, "v7a frame header crc mismatch");
        return TC_ERR_CHECKSUM_MISMATCH;
    }
    if (tc_load_be16(data + 4u) != TC_FRAME_HEADER_SIZE || data[6] != 7u) {
        tc_set_error(TC_ERR_UNSUPPORTED_VERSION, "not a V7-A frame header");
        return TC_ERR_UNSUPPORTED_VERSION;
    }

    memset(fh, 0, sizeof(*fh));
    fh->version_major = data[6];
    fh->version_minor = data[7];
    fh->flags = tc_load_be16(data + 8u);
    fh->profile = data[10];
    fh->pixel_format = data[11];
    fh->bit_depth = data[12];
    fh->alpha_mode = data[13];
    fh->alpha_bit_depth = data[14];
    fh->frame_type = data[15];
    fh->gop_id = tc_load_be16(data + 16u);
    fh->ref_distance = data[18];
    fh->coded_width = tc_load_be16(data + 19u);
    fh->coded_height = tc_load_be16(data + 21u);
    fh->visible_width = tc_load_be16(data + 23u);
    fh->visible_height = tc_load_be16(data + 25u);
    fh->plane_count = data[27];
    fh->qmatrix_id = data[28];
    fh->qp_base = data[29];
    fh->slice_count = tc_load_be16(data + 30u);
    fh->color_range = data[32];
    fh->color_primaries = data[33];
    fh->color_transfer = data[34];
    fh->color_matrix = data[35];
    fh->chroma_siting = data[36];
    fh->sar_num = tc_load_be16(data + 37u);
    fh->sar_den = tc_load_be16(data + 39u);
    fh->frame_packet_size = tc_load_be32(data + 41u);
    fh->entropy_mode = data[45];
    fh->codebook_version = data[46];
    fh->coding_mode = data[47];
    if (data[48] != 0u) {
        return vfail(TC_ERR_MALFORMED, "v7 reserved_v2_0(!=0)", data[48]);
    }
    int32_t rc = tc_frame_header_validate_v7a_contract(fh);
    if (rc != TC_OK) { return rc; }
    rc = validate_v7a_common_fields(fh);
    if (rc != TC_OK) { return rc; }
    if ((uint64_t)size < (uint64_t)fh->frame_packet_size) {
        return vfail(TC_ERR_TRUNCATED, "v7 frame_packet_size", fh->frame_packet_size);
    }
    if ((uint64_t)fh->frame_packet_size != (uint64_t)size) {
        return vfail(TC_ERR_MALFORMED, "v7 frame_packet_size", fh->frame_packet_size);
    }
    return tc_frame_derive_geometry(fh);
}

/* —— 命名辅助（inspect/日志）—— */

const char* tc_color_range_name(uint8_t v)
{
    return v == 0u ? "limited" : v == 1u ? "full" : "unknown";
}
const char* tc_color_primaries_name(uint8_t v)
{
    switch (v) {
    case 1: return "bt709";
    case 6: return "smpte170m";
    case 9: return "bt2020";
    case 12: return "smpte431p3";
    default: return "unknown";
    }
}
const char* tc_color_transfer_name(uint8_t v)
{
    switch (v) {
    case 1: return "bt709";
    case 8: return "linear";
    case 16: return "smpte2084";
    case 18: return "arib-std-b67";
    case TC_TRANSFER_TRAW_LOG0: return "topos-traw-log0";
    default: return "unknown";
    }
}
const char* tc_color_matrix_name(uint8_t v)
{
    switch (v) {
    case 0: return "identity(GBR)";
    case 1: return "bt709";
    case 5: return "smpte170m";
    case 9: return "bt2020nc";
    default: return "unknown";
    }
}
const char* tc_chroma_siting_name(uint8_t v)
{
    switch (v) {
    case 0: return "left";
    case 1: return "center";
    case 2: return "topleft";
    default: return "unknown";
    }
}
const char* tc_profile_name(uint8_t v)
{
    switch (v) {
    case 1: return "Proxy";
    case 2: return "LT";
    case 3: return "Standard";
    case 4: return "HQ";
    case 5: return "Pro444";
    case 6: return "Extreme";
    case 7: return "TRAW";
    default: return "unknown";
    }
}
const char* tc_pixel_format_name(uint8_t v)
{
    switch (v) {
    case 0: return "YUV 4:2:2";
    case 1: return "YUV 4:4:4";
    case 2: return "GBR 4:4:4";
    case 3: return "Bayer CFA";
    default: return "unknown";
    }
}
const char* tc_alpha_mode_name(uint8_t v)
{
    switch (v) {
    case 0: return "none";
    case 1: return "lossless";
    case 2: return "restricted-approx"; /* R3 激活（mode2 N-bit 预量化）——复验 P2-09：不再标 reserved */
    default: return "unknown";
    }
}
