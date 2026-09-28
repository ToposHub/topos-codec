/* golden_codec —— 阶段 4 conformance（v1 冻结：完整标量 codec 全链路）。
 *
 * 8 组确定性配置（image_synth 合成图 → tc_frame_encode → tc_frame_decode），
 * 每组两条记录：
 *   A 该配置的完整 frame packet 字节（编码侧冻结：QM 表、k 估计、qp 策略、
 *     padding、slice 划分任何漂移都会逐字节失败）
 *   B 解码输出的平面指纹 u64（解码侧冻结：反量化、逆变换、重建钳位、MED、
 *     crop 的任何漂移都会失败 —— spec §13.3 后端一致性）
 *
 * 文件格式：magic "TPC1"(4) | version u32be=1 | count u32be | fold u64be |
 *           记录{ u32be len | bytes }（同 golden_bitstream 约定）。
 *
 * 用法：golden_codec gen <file> | check <file>
 * 有意变更（QM 调优、k 策略等）时：走 ADR + 版本规则，手动重新生成并入库。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/common/endian.h"
#include "../support/image_synth.h"
#include "topos_codec.h"

static uint64_t g_fold;

static void fold_reset(void) { g_fold = 0x5C0DE5C0DE5C0DE5ull; }

static uint64_t mix64(uint64_t h, uint64_t v)
{
    h += 0x9E3779B97F4A7C15ull;
    uint64_t z = v;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return h ^ (z ^ (z >> 31));
}

static void fold_bytes(const uint8_t* data, size_t size)
{
    for (size_t i = 0; i < size; ++i) { g_fold = mix64(g_fold, data[i]); }
}

/* —— 配置矩阵（下标稳定，勿重排） —— */

typedef struct codec_case {
    uint64_t seed;
    uint16_t w, h;
    tc_synth_kind kind;
    uint8_t qmatrix_id;
    uint8_t qp_base;
    int8_t qp_delta_chroma;
    uint8_t slice_rows;   /* 0 = 默认 32 */
    uint8_t alpha_mode;
    uint8_t alpha_bit_depth;
    uint32_t bit_depth;   /* R4.1：0/10 = v1 用例（历史字节不变）；12 = 扩展集 */
    uint32_t pixel_format; /* R4.2：1 = 4:4:4；R4.3：2 = GBR（0 = 默认 4:2:2） */
    uint8_t profile;      /* R4.4：0/3 = Standard；5 = Pro444；6 = Extreme */
    uint32_t entropy_mode; /* M9：cfg.reserved[0]——1 = V2+VLC（V1 表恒 0） */
} codec_case;

static const codec_case kCases[] = {
    { 0x0102030405060708ull, 8u, 8u, TC_SYNTH_FLAT, 0u, 0u, 0, 0u, 0u, 0u, 0u, 0u, 0u },
    { 0x1112131415161718ull, 64u, 48u, TC_SYNTH_GRAIN, 1u, 20u, 0, 0u, 0u, 0u, 0u, 0u, 0u },
    { 0x2122232425262728ull, 36u, 20u, TC_SYNTH_MIXED, 0u, 33u, 0, 3u, 1u, 16u, 0u, 0u, 0u },
    { 0x3132333435363738ull, 48u, 48u, TC_SYNTH_DETAIL, 1u, 12u, 0, 2u, 2u, 12u, 0u, 0u, 0u },
    { 0x4142434445464748ull, 96u, 64u, TC_SYNTH_GRAIN, 1u, 24u, 6, 0u, 0u, 0u, 0u, 0u, 0u },
    { 0x5152535455565758ull, 128u, 72u, TC_SYNTH_GRADIENT, 0u, 8u, 0, 8u, 0u, 0u, 0u, 0u, 0u },
    { 0x6162636465666768ull, 16u, 16u, TC_SYNTH_FLAT, 1u, 63u, 0, 0u, 0u, 0u, 0u, 0u, 0u },
    { 0x7172737475767778ull, 64u, 48u, TC_SYNTH_DETAIL, 1u, 42u, 0, 1u, 2u, 8u, 0u, 0u, 0u },
};

/* R4.1：12-bit YUV 4:2:2 扩展集（v1.2 枚举；qp 偏移 +4 已在编码路径）。
 * 独立 golden 文件 golden_codec_v1_bd12.bin——v1 文件保持字节冻结。 */
static const codec_case kCases12[] = {
    { 0x8182838485868788ull, 64u, 48u, TC_SYNTH_GRAIN, 1u, 20u, 0, 0u, 0u, 0u, 12u, 0u, 0u },
    { 0x9192939495969798ull, 36u, 20u, TC_SYNTH_MIXED, 0u, 33u, 0, 3u, 1u, 16u, 12u, 0u, 0u },
    { 0xA1A2A3A4A5A6A7A8ull, 48u, 48u, TC_SYNTH_DETAIL, 1u, 12u, 0, 2u, 2u, 12u, 12u, 0u, 0u },
    { 0xB1B2B3B4B5B6B7B8ull, 96u, 64u, TC_SYNTH_GRADIENT, 1u, 24u, 6, 0u, 0u, 0u, 12u, 0u, 0u },
    { 0xC1C2C3C4C5C6C7C8ull, 16u, 16u, TC_SYNTH_FLAT, 1u, 63u, 0, 0u, 2u, 8u, 12u, 0u, 0u },
};

/* R4.2：YUV 4:4:4 扩展集（v1.3 枚举，帧头 minor=2；含 12-bit 组合）。
 * 独立 golden 文件 golden_codec_v1_pf444.bin——前两文件保持字节冻结。 */
static const codec_case kCases444[] = {
    { 0xD1D2D3D4D5D6D7D8ull, 64u, 48u, TC_SYNTH_GRAIN, 1u, 20u, 0, 0u, 0u, 0u, 0u, 1u, 0u },
    { 0xE1E2E3E4E5E6E7E8ull, 36u, 20u, TC_SYNTH_MIXED, 0u, 33u, 0, 3u, 1u, 16u, 0u, 1u, 0u },
    { 0xF1F2F3F4F5F6F7F8ull, 48u, 48u, TC_SYNTH_DETAIL, 1u, 12u, 0, 2u, 2u, 12u, 0u, 1u, 0u },
    { 0x0F1E2D3C4B5A6978ull, 96u, 64u, TC_SYNTH_GRADIENT, 1u, 24u, 6, 0u, 0u, 0u, 12u, 1u, 0u },
    { 0x1F2E3D4C5B6A7988ull, 16u, 16u, TC_SYNTH_FLAT, 1u, 63u, 0, 0u, 2u, 8u, 0u, 1u, 0u },
    { 0x2F3E4D5C6B7A8999ull, 64u, 48u, TC_SYNTH_GRAIN, 0u, 28u, 3, 0u, 0u, 0u, 12u, 1u, 0u },
};

/* R4.3：GBR 4:4:4 扩展集（v1.4 枚举，pf=2/minor=3/matrix=0；含 12-bit
 * 组合）。独立 golden 文件 golden_codec_v1_gbr.bin——前三文件字节冻结。 */
static const codec_case kCasesGBR[] = {
    { 0x3F4E5D6C7B8A9901ull, 64u, 48u, TC_SYNTH_GRAIN, 1u, 20u, 0, 0u, 0u, 0u, 0u, 2u, 0u },
    { 0x4F5E6D7C8B9A0112ull, 36u, 20u, TC_SYNTH_MIXED, 0u, 33u, 0, 3u, 1u, 16u, 0u, 2u, 0u },
    { 0x5F6E7D8C9BA02234ull, 48u, 48u, TC_SYNTH_DETAIL, 1u, 12u, 0, 2u, 2u, 12u, 0u, 2u, 0u },
    { 0x6F7E8D9CAB103456ull, 96u, 64u, TC_SYNTH_GRADIENT, 1u, 24u, 6, 0u, 0u, 0u, 12u, 2u, 0u },
    { 0x7F8E9DACB0215678ull, 16u, 16u, TC_SYNTH_FLAT, 1u, 63u, 0, 0u, 2u, 8u, 0u, 2u, 0u },
    { 0x8F9EADBCD0327899ull, 64u, 48u, TC_SYNTH_GRAIN, 0u, 28u, 3, 0u, 0u, 0u, 12u, 2u, 0u },
};

/* R4.4：Pro444/Extreme 扩展集（profile 5/6 激活；格式交叉规则见
 * frame_header）。独立 golden 文件 golden_codec_v1_profiles.bin。 */
static const codec_case kCasesProfiles[] = {
    { 0x9A8B7C6D5E4F3021ull,  64u,  48u, TC_SYNTH_GRAIN,    1u, 20u, 0,  0u, 0u,  0u,  0u, 1u, 5u },
    { 0xA9B8C7D6E5F41332ull,  48u,  48u, TC_SYNTH_DETAIL,   1u, 12u, 0,  2u, 2u, 12u, 12u, 1u, 5u },
    { 0xB8C7D6E5F4A42443ull,  96u,  64u, TC_SYNTH_GRADIENT, 1u, 24u, 6,  0u, 0u,  0u, 12u, 2u, 5u },
    { 0xC7D6E5F4A3B53554ull,  16u,  16u, TC_SYNTH_FLAT,     1u, 63u, 0,  0u, 0u,  0u, 12u, 1u, 6u },
    { 0xD6E5F4A3B2C64665ull,  64u,  48u, TC_SYNTH_GRAIN,    0u, 28u, 3,  0u, 0u,  0u, 12u, 2u, 6u },
    { 0xE5F4A3B2C1D75776ull,  36u,  20u, TC_SYNTH_MIXED,    0u, 33u, 0,  3u, 1u, 16u, 12u, 2u, 6u },
};

/* M9：V2 canonical VLC 用例集（golden_codec_v2_vlc.bin；spec v2 §5 golden
 * 矩阵：10/12-bit × 422/444/GBR × alpha 有/无）。冻结：码表版本 1 +
 * 选表策略 + VLC 位流任何漂移都会逐字节失败；V1 各 golden 文件字节不变。 */
static const codec_case kCasesVLC[] = {
    { 0x0A1B2C3D4E5F6071ull,  64u,  48u, TC_SYNTH_GRAIN,    1u, 20u, 0,  0u, 0u,  0u,  0u, 0u, 0u, 1u },
    { 0x1A2B3C4D5E6F7082ull,  48u,  48u, TC_SYNTH_DETAIL,   1u, 12u, 0,  2u, 0u,  0u,  0u, 0u, 0u, 1u },
    { 0x2A3B4C5D6E7F8093ull,  36u,  20u, TC_SYNTH_MIXED,    0u, 33u, 0,  3u, 0u,  0u, 12u, 0u, 0u, 1u },
    { 0x3A4B5C6D7E8F90A4ull,  96u,  64u, TC_SYNTH_GRAIN,    1u, 24u, 6,  0u, 0u,  0u, 12u, 1u, 0u, 1u },
    { 0x4A5B6C7D8E9FA0B5ull,  16u,  16u, TC_SYNTH_FLAT,     1u, 63u, 0,  0u, 0u,  0u,  0u, 1u, 0u, 1u },
    { 0x5A6B7C8D9EAFB0C6ull,  64u,  48u, TC_SYNTH_GRADIENT, 0u, 28u, 3,  0u, 0u,  0u,  0u, 2u, 0u, 1u },
    { 0x6A7B8C9DAFB0C1D7ull,  36u,  20u, TC_SYNTH_MIXED,    0u, 33u, 0,  3u, 1u, 16u,  0u, 0u, 0u, 1u },
    { 0x7A8B9DAFB0C1D2E8ull,  64u,  48u, TC_SYNTH_DETAIL,   1u, 12u, 0,  2u, 2u, 12u, 12u, 1u, 0u, 1u },
};

/* V3: directional residual prediction + canonical VLC. Independent vectors
 * freeze odd geometry/padding, all color formats and both color bit depths.
 * V 代际收纳（2026-09-13）：V3 退役，golden_codec_v3_intra.bin 归档至
 * tests/fuzz/corpus_retired/（TOPOS_DEV 回放用）；同一 case 网格改挂在
 * 保留代际 V7-R2（cfg em=8）上重建等价覆盖（golden_codec_v7r2_intra.bin）。
 * 平移规则：仅改 entropy_mode 字段，几何/位深/qp/alpha 网格逐格不变。 */
static const codec_case kCasesIntraR2[] = {
    { 0x301u, 73u, 39u, TC_SYNTH_GRADIENT, 1u, 24u, 0, 2u, 0u, 0u, 10u, 0u, 0u, 8u },
    { 0x302u, 35u, 21u, TC_SYNTH_DETAIL,   1u, 18u, 0, 1u, 1u, 16u, 12u, 0u, 0u, 8u },
    { 0x303u, 73u, 39u, TC_SYNTH_MIXED,    0u, 31u, 0, 2u, 2u, 12u, 10u, 1u, 0u, 8u },
    { 0x304u, 35u, 21u, TC_SYNTH_GRADIENT, 1u, 24u, 0, 1u, 0u, 0u, 12u, 1u, 0u, 8u },
    { 0x305u, 73u, 39u, TC_SYNTH_DETAIL,   1u, 18u, 0, 2u, 1u, 16u, 10u, 2u, 0u, 8u },
    { 0x306u, 35u, 21u, TC_SYNTH_MIXED,    0u, 31u, 0, 1u, 2u, 8u, 12u, 2u, 0u, 8u },
    { 0x307u,  1u,  1u, TC_SYNTH_FLAT,     0u, 0u, 0, 1u, 0u, 0u, 10u, 0u, 0u, 8u },
    { 0x308u, 16u, 16u, TC_SYNTH_GRAIN,    1u, 63u, 0, 1u, 0u, 0u, 12u, 2u, 0u, 8u },
};

/* v1.6（TRAW 批 1）：CFA 4 相位平面 12-bit（profile 7 / minor=5 /
 * transfer=LOG0 / qm0）。V1-Rice 与 rans2（产品默认熵）双覆盖。
 * 独立 golden 文件 golden_codec_v1_traw.bin。 */
static const codec_case kCasesTRAW[] = {
    { 0x7219000000000001ull, 64u, 48u, TC_SYNTH_GRAIN,    0u, 20u, 0, 0u, 0u, 0u, 12u, 3u, 7u, 0u },
    { 0x7219000000000002ull, 36u, 20u, TC_SYNTH_GRADIENT, 0u, 33u, 0, 3u, 0u, 0u, 12u, 3u, 7u, 0u },
    { 0x7219000000000003ull, 48u, 48u, TC_SYNTH_DETAIL,   0u, 12u, 0, 0u, 0u, 0u, 12u, 3u, 7u, 0u },
    { 0x7219000000000004ull, 64u, 48u, TC_SYNTH_MIXED,    0u, 24u, 0, 2u, 0u, 0u, 12u, 3u, 7u, 0u },
    { 0x7219000000000005ull, 64u, 48u, TC_SYNTH_GRAIN,    0u, 20u, 0, 0u, 0u, 0u, 12u, 3u, 7u, 8u },
    { 0x7219000000000006ull, 48u, 48u, TC_SYNTH_DETAIL,   0u, 63u, 0, 0u, 0u, 0u, 12u, 3u, 7u, 8u },
};

static const codec_case* g_cases = kCases;
static uint32_t g_case_count = 0u;

#define CASE_COUNT (g_case_count)
#define RECORD_COUNT (CASE_COUNT * 2u)

static int32_t run_case(uint32_t index, uint8_t** pkt_out, size_t* pkt_size, uint64_t* dec_fold)
{
    const codec_case* cc = &g_cases[index];
    uint16_t *y = NULL, *u = NULL, *v = NULL, *a = NULL;
    uint16_t* cfa[4] = {NULL, NULL, NULL, NULL};
    int rc;
    if (cc->pixel_format == 3u) {
        /* TRAW（v1.6）：4 相位平面 R/Gr/Gb/B，各 (W/2)×(H/2)——复用 luma
         * 合成器（u/v/a 槽 NULL），4 个异化种子产独立相位纹理 */
        const uint32_t pw = cc->w / 2u, ph = cc->h / 2u;
        for (int i = 0; i < 4; ++i) {
            image_synth_cfg pc;
            memset(&pc, 0, sizeof(pc));
            pc.seed = cc->seed + (uint64_t)i * 0x100000000ull;
            pc.width = pw;
            pc.height = ph;
            pc.kind = cc->kind;
            pc.bit_depth = cc->bit_depth;
            cfa[i] = (uint16_t*)malloc((size_t)pw * ph * sizeof(uint16_t));
            if (cfa[i] == NULL) { rc = TC_ERR_OUT_OF_MEMORY; goto cfa_fail; }
            if (image_synth_build(&pc, cfa[i], NULL, NULL, NULL) != 0) {
                free(cfa[i]);
                rc = TC_ERR_OUT_OF_MEMORY;
                goto cfa_fail;
            }
        }
        y = cfa[0]; u = cfa[1]; v = cfa[2]; a = cfa[3];
        goto cfa_done;
    cfa_fail:
        for (int i = 0; i < 4; ++i) { free(cfa[i]); }
        return rc;
    } else {

    image_synth_cfg ic;
    memset(&ic, 0, sizeof(ic));
    ic.seed = cc->seed;
    ic.width = cc->w;
    ic.height = cc->h;
    ic.kind = cc->kind;
    ic.bit_depth = cc->bit_depth;
    ic.chroma_format = cc->pixel_format != 0u ? 1u : 0u; /* pf≠0 = 全宽合成 */

    rc = image_synth_alloc(&ic, cc->alpha_mode != 0u, &y, &u, &v, &a);
    if (rc != 0) { return TC_ERR_OUT_OF_MEMORY; }
    }
cfa_done:;

    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(topos_frame_config);
    cfg.visible_width = cc->w;
    cfg.visible_height = cc->h;
    cfg.qmatrix_id = cc->qmatrix_id;
    cfg.qp_base = cc->qp_base;
    cfg.qp_delta_chroma = cc->qp_delta_chroma;
    cfg.slice_rows = cc->slice_rows != 0u ? cc->slice_rows : 32u; /* 0 钉历史默认 32（M10-6.4 改 16）——冻结字节不随默认值漂移 */
    cfg.alpha_mode = cc->alpha_mode;
    cfg.alpha_bit_depth = cc->alpha_bit_depth;
    cfg.bit_depth = cc->bit_depth;   /* R4.1：12 走 v1.2 扩展枚举 */
    cfg.pixel_format = cc->pixel_format; /* R4.2：1 = 4:4:4；R4.3：2 = GBR */
    cfg.color_matrix = (cc->pixel_format == 2u || cc->pixel_format == 3u)
                           ? 0u : 1u; /* GBR/CFA 契约：identity */
    cfg.profile = cc->profile != 0u ? cc->profile
                  : (cc->pixel_format == 3u ? 7u : 3u); /* R4.4/TRAW */
    cfg.reserved[0] = cc->entropy_mode; /* M9：0=V1；1=V2+VLC */
    /* H1 golden 双轨钉扎（2026-09-21）：frozen 流显式固定旧默认色彩域
     * （primaries=1/transfer=1/range=0 透传）——cfg 默认 transfer 已改派
     * sRGB(13)（minor≥7 代），重建须逐字节等于入库 golden，故此处显式
     * 钉住历史值；新默认由 pytest 侧新增用例覆盖（tests/media）。
     * TRAW（pf=3）按位深复现旧默认冻结对（12→LOG0 / 16→linear）。 */
    cfg.color_range = 0u;
    cfg.color_primaries = 1u;
    cfg.color_transfer = (cc->pixel_format == 3u)
        ? (cc->bit_depth == 16u ? 8u : 20u)
        : 1u;

    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(topos_frame_input);
    const uint16_t* pl[4] = {y, u, v, a};
    memcpy(in.planes, pl, sizeof(pl));

    size_t cap = tc_frame_packet_bound(&cfg);
    uint8_t* pkt = (uint8_t*)malloc(cap != 0u ? cap : 1u);
    if (pkt == NULL) {
        free(y); free(u); free(v); free(a);
        return TC_ERR_OUT_OF_MEMORY;
    }
    topos_frame_stats st;
    int32_t crc = tc_frame_encode(&cfg, &in, pkt, cap, &st);
    if (crc != TC_OK) {
        free(pkt); free(y); free(u); free(v); free(a);
        return crc;
    }

    topos_frame_output info;
    uint16_t* dec[4] = {NULL, NULL, NULL, NULL};
    crc = tc_frame_decode(pkt, st.packet_size, NULL, NULL, &info);
    if (crc == TC_OK) {
        uint32_t pw[4] = {0, 0, 0, 0};
        uint32_t ph[4] = {0, 0, 0, 0};
        for (uint32_t p = 0u; p < info.plane_count; ++p) {
            uint32_t w = 0, h = 0;
            (void)tc_frame_plane_geometry(&info, p, &w, &h);
            pw[p] = w;
            ph[p] = h; /* 每平面可见高（TRAW CFA = 帧高一半；fold 界必须逐平面） */
            dec[p] = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
        }
        crc = tc_frame_decode(pkt, st.packet_size, dec, NULL, &info);
        uint64_t fold = 0xDED0C0DE00C0FFEEull;
        if (crc == TC_OK || crc == TC_WARN_CONCEALED) {
            for (uint32_t p = 0u; p < info.plane_count; ++p) {
                fold = mix64(fold, (uint64_t)p);
                if (dec[p] == NULL) { continue; }
                for (uint32_t r = 0u; r < ph[p]; ++r) {
                    for (uint32_t x = 0u; x < pw[p]; ++x) {
                        fold = mix64(fold, (uint64_t)dec[p][(size_t)r * pw[p] + x]);
                    }
                }
            }
        }
        *dec_fold = fold;
        for (int p = 0; p < 4; ++p) { free(dec[p]); }
    }
    free(y); free(u); free(v); free(a);
    if (crc != TC_OK) {
        free(pkt);
        return crc;
    }
    *pkt_out = pkt;
    *pkt_size = st.packet_size;
    return TC_OK;
}

static int32_t build_record(uint32_t index, uint8_t** out, size_t* out_size)
{
    uint32_t case_idx = index / 2u;
    uint8_t* pkt = NULL;
    size_t n = 0;
    uint64_t dec_fold = 0;
    int32_t rc = run_case(case_idx, &pkt, &n, &dec_fold);
    if (rc != TC_OK) { return rc; }
    if ((index & 1u) == 0u) { /* 偶数：packet 字节 */
        *out = pkt;
        *out_size = n;
        return TC_OK;
    }
    free(pkt); /* 奇数：解码平面指纹 */
    uint8_t* foldb = (uint8_t*)malloc(8u);
    if (foldb == NULL) { return TC_ERR_OUT_OF_MEMORY; }
    uint64_t v = dec_fold;
    for (int i = 7; i >= 0; --i) {
        foldb[i] = (uint8_t)(v & 0xFFu);
        v >>= 8;
    }
    *out = foldb;
    *out_size = 8u;
    return TC_OK;
}

#define GOLDEN_MAGIC "TPC1"
#define GOLDEN_VERSION 1u

static int gen_mode(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (f == NULL) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    uint8_t hdr[24] = {0};
    memcpy(hdr, GOLDEN_MAGIC, 4u);
    tc_store_be32(hdr + 4, GOLDEN_VERSION);
    tc_store_be32(hdr + 8, RECORD_COUNT);
    tc_store_be64(hdr + 16, 0u);
    fwrite(hdr, 1, sizeof(hdr), f);

    fold_reset();
    for (uint32_t i = 0; i < RECORD_COUNT; ++i) {
        uint8_t* data = NULL;
        size_t n = 0;
        int32_t rc = build_record(i, &data, &n);
        if (rc != TC_OK) {
            fprintf(stderr, "builder %u failed: %d (%s)\n", (unsigned)i, rc, tc_last_error());
            fclose(f);
            return 1;
        }
        uint8_t lenb[4];
        tc_store_be32(lenb, (uint32_t)n);
        fwrite(lenb, 1, 4u, f);
        fwrite(data, 1, n, f);
        g_fold = mix64(g_fold, (uint64_t)n);
        fold_bytes(data, n);
        free(data);
    }
    fseek(f, 16, SEEK_SET);
    uint8_t foldb[8];
    tc_store_be64(foldb, g_fold);
    fwrite(foldb, 1, 8u, f);
    fclose(f);
    printf("generated %u records, fold %016llx\n", (unsigned)RECORD_COUNT,
           (unsigned long long)g_fold);
    return 0;
}

static int check_mode(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    uint8_t* all = (uint8_t*)malloc(64u * 1024u * 1024u);
    if (all == NULL) { fclose(f); return 1; }
    size_t total = fread(all, 1, 64u * 1024u * 1024u, f);
    fclose(f);
    if (total < 24u || memcmp(all, GOLDEN_MAGIC, 4u) != 0) {
        fprintf(stderr, "bad magic\n");
        free(all);
        return 1;
    }
    uint32_t version = tc_load_be32(all + 4);
    uint32_t count = tc_load_be32(all + 8);
    uint64_t fold = tc_load_be64(all + 16);
    if (version != GOLDEN_VERSION || count != RECORD_COUNT) {
        fprintf(stderr, "version/count mismatch: %u/%u\n", (unsigned)version, (unsigned)count);
        free(all);
        return 1;
    }

    size_t off = 24u;
    fold_reset();
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 4u > total) { fprintf(stderr, "record %u truncated\n", (unsigned)i); break; }
        uint32_t n = tc_load_be32(all + off);
        off += 4u;
        if (off + n > total) { fprintf(stderr, "record %u truncated\n", (unsigned)i); break; }
        const uint8_t* stored = all + off;

        uint8_t* expect = NULL;
        size_t expect_n = 0;
        int32_t rc = build_record(i, &expect, &expect_n);
        if (rc != TC_OK) {
            fprintf(stderr, "record %u builder failed: %d (%s)\n", (unsigned)i, rc,
                    tc_last_error());
            free(all);
            return 1;
        }
        if (expect_n != (size_t)n || memcmp(expect, stored, expect_n) != 0) {
            fprintf(stderr, "record %u MISMATCH: stored %u bytes, rebuilt %zu\n",
                    (unsigned)i, (unsigned)n, expect_n);
            free(expect);
            free(all);
            return 1;
        }
        free(expect);
        g_fold = mix64(g_fold, (uint64_t)n);
        fold_bytes(stored, n);
        off += n;
    }
    if (off != total) {
        fprintf(stderr, "trailing bytes after last record\n");
        free(all);
        return 1;
    }
    if (g_fold != fold) {
        fprintf(stderr, "fold mismatch: %016llx != %016llx\n",
                (unsigned long long)g_fold, (unsigned long long)fold);
        free(all);
        return 1;
    }
    free(all);
    printf("golden codec: %u records byte-exact, fold %016llx OK\n",
           (unsigned)count, (unsigned long long)fold);
    return 0;
}

int main(int argc, char** argv)
{
    g_case_count = (uint32_t)(sizeof(kCases) / sizeof(kCases[0]));
    if (argc == 4 && strcmp(argv[3], "bd12") == 0) {
        /* R4.1：12-bit 扩展用例集（golden_codec_v1_bd12.bin） */
        g_cases = kCases12;
        g_case_count = (uint32_t)(sizeof(kCases12) / sizeof(kCases12[0]));
    } else if (argc == 4 && strcmp(argv[3], "pf444") == 0) {
        /* R4.2：4:4:4 扩展用例集（golden_codec_v1_pf444.bin） */
        g_cases = kCases444;
        g_case_count = (uint32_t)(sizeof(kCases444) / sizeof(kCases444[0]));
    } else if (argc == 4 && strcmp(argv[3], "gbr") == 0) {
        /* R4.3：GBR 扩展用例集（golden_codec_v1_gbr.bin） */
        g_cases = kCasesGBR;
        g_case_count = (uint32_t)(sizeof(kCasesGBR) / sizeof(kCasesGBR[0]));
    } else if (argc == 4 && strcmp(argv[3], "profiles") == 0) {
        /* R4.4：Pro444/Extreme 用例集（golden_codec_v1_profiles.bin） */
        g_cases = kCasesProfiles;
        g_case_count = (uint32_t)(sizeof(kCasesProfiles) / sizeof(kCasesProfiles[0]));
    } else if (argc == 4 && strcmp(argv[3], "vlc") == 0) {
        /* M9：V2 canonical VLC 用例集（golden_codec_v2_vlc.bin） */
        g_cases = kCasesVLC;
        g_case_count = (uint32_t)(sizeof(kCasesVLC) / sizeof(kCasesVLC[0]));
    } else if (argc == 4 && strcmp(argv[3], "intra-r2") == 0) {
        /* V 代际收纳：V3 intra 网格在 V7-R2（cfg em=8）上的等价重建
         * （golden_codec_v7r2_intra.bin；V3 原宿主已归档） */
        g_cases = kCasesIntraR2;
        g_case_count = (uint32_t)(sizeof(kCasesIntraR2) / sizeof(kCasesIntraR2[0]));
    } else if (argc == 4 && strcmp(argv[3], "traw") == 0) {
        /* v1.6（TRAW 批 1）：CFA 12-bit 用例集（golden_codec_v1_traw.bin） */
        g_cases = kCasesTRAW;
        g_case_count = (uint32_t)(sizeof(kCasesTRAW) / sizeof(kCasesTRAW[0]));
    }
    if (argc >= 3 && strcmp(argv[1], "gen") == 0) { return gen_mode(argv[2]); }
    if (argc >= 3 && strcmp(argv[1], "check") == 0) { return check_mode(argv[2]); }
    fprintf(stderr, "usage: golden_codec gen|check <file> [bd12|pf444|gbr|profiles|vlc|intra-r2|traw]\n");
    return 2;
}
