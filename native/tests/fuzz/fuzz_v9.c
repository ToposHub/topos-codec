/* fuzz_v9 —— V9 位流 + GOP context fuzz（topos_v9_micro_gop_plan 批 2）。
 *
 * 输入路径（LLVMFuzzerTestOneInput）：
 *   A) 直接：输入本身像 V9 包（magic+major=9）→ 查询解码（同构扫描）+
 *      实解码（小几何才分配平面）+ GOP context observe/feed；
 *   B) 合成：真实编码器产出合法 V8 包（几何/qp 从输入小集合取）→ 头部
 *      补丁成 V9（major/熵字节 + 头 CRC 重算）→ 未变异合法 I 包契约
 *      （查询 TC_OK）→ 受控字节变异（数量/位置 LCG(input)）+ 截断扫描。
 *
 * 不变量（对任意输入）：不崩溃、不挂起、返回码 ∈ spec 定义集（含
 * TC_ERR_STATE / TC_ERR_REFERENCE_INVALID）；GOP observe/feed 状态机对
 * 任意包序列返回码有定义。
 *
 * 复用 corpus_replay driver（-gen 确定性回放；libFuzzer 目标直接链接本文件）。
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/crc32.h"
#include "topos_codec.h"

static int status_is_defined(int32_t rc)
{
    return rc == TC_OK || rc == TC_WARN_CONCEALED || rc == TC_ERR_INVALID_ARGUMENT ||
           rc == TC_ERR_OUT_OF_MEMORY || rc == TC_ERR_UNSUPPORTED_VERSION ||
           rc == TC_ERR_UNSUPPORTED_PROFILE || rc == TC_ERR_UNSUPPORTED_PIXEL_FORMAT ||
           rc == TC_ERR_UNSUPPORTED_MATRIX || rc == TC_ERR_UNSUPPORTED_ALPHA_MODE ||
           rc == TC_ERR_LIMIT_EXCEEDED || rc == TC_ERR_MALFORMED ||
           rc == TC_ERR_TRUNCATED || rc == TC_ERR_CHECKSUM_MISMATCH ||
           rc == TC_ERR_NOT_IMPLEMENTED || rc == TC_ERR_REFERENCE_INVALID;
}

static uint64_t g_rng;

static uint64_t rng(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}

static topos_gop_context* g_gop = NULL;

/* 惰性创建进程级 GOP 观察窗（跨输入累计序列状态——状态机本身即被 fuzz） */
static topos_gop_context* fuzz_gop(void)
{
    if (g_gop != NULL) { return g_gop; }
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = 64u;
    cfg.visible_height = 64u;
    cfg.profile = 3u;
    cfg.pixel_format = 0u;
    cfg.bit_depth = 10u;
    cfg.qmatrix_id = 0u;
    cfg.qp_base = 30u;
    cfg.color_range = 1u;
    cfg.color_primaries = 1u;
    cfg.color_transfer = 1u;
    cfg.color_matrix = 1u;
    cfg.reserved[0] = 10u;
    if (tc_gop_context_create(&cfg, &g_gop) != TC_OK) { g_gop = NULL; }
    return g_gop;
}

static void fuzz_direct(const uint8_t* data, size_t size)
{
    topos_frame_output info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    int32_t rc = tc_frame_decode(data, size, NULL, NULL, &info);
    if (!status_is_defined(rc)) { abort(); }
    if (rc != TC_OK) { return; }

    /* 实解码：小几何才分配平面（V9 批 2：I 可解；P → TC_ERR_STATE）。
     * 全部平面先过尺寸检查再分配——任一超限/失败即跳过（无泄漏）。 */
    uint16_t* planes[TC_FRAME_MAX_PLANES] = { NULL, NULL, NULL, NULL };
    int small = 1;
    for (uint32_t p = 0u; p < info.plane_count && p < TC_FRAME_MAX_PLANES; ++p) {
        uint32_t pw = 0u, ph = 0u;
        if (tc_frame_plane_geometry(&info, p, &pw, &ph) != TC_OK ||
            (uint64_t)pw * (uint64_t)ph > 256ull * 256ull) { small = 0; break; }
        planes[p] = (uint16_t*)calloc((size_t)pw * ph, sizeof(uint16_t));
        if (planes[p] == NULL) { small = 0; break; }
    }
    if (small) {
        rc = tc_frame_decode(data, size, planes, NULL, &info);
        if (!status_is_defined(rc)) { abort(); }
    }
    for (int p = 0; p < TC_FRAME_MAX_PLANES; ++p) { free(planes[p]); }

    /* GOP 序列观察：任意输入返回码有定义（序列合法性归单测矩阵）。 */
    if (fuzz_gop() != NULL) {
        topos_gop_frame_info gi;
        memset(&gi, 0, sizeof(gi));
        gi.struct_size = (uint32_t)sizeof(gi);
        gi.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        rc = tc_gop_context_observe(g_gop, data, size, &gi);
        if (!status_is_defined(rc)) { abort(); }
    }
}

static int is_v9_shaped(const uint8_t* data, size_t size)
{
    static const uint8_t kMagic[4] = { 'T', 'P', 'I', 'C' };
    return size >= 53u && memcmp(data, kMagic, 4u) == 0 && data[6] == 9u;
}

static void fuzz_synthesized(const uint8_t* data, size_t size)
{
    if (size < 16u) { return; }
    typedef struct { uint16_t w, h; } fuzz_dims;
    static const fuzz_dims kDims[2] = { { 64u, 64u }, { 96u, 80u } };
    const fuzz_dims* d = &kDims[data[0] % 2u];
    const uint8_t qp = (uint8_t)(20u + data[1] % 40u);
    const uint32_t mutations = (uint32_t)(data[2] % 5u);

    g_rng = 0x5EED000000000000ull;
    for (size_t i = 3u; i < 11u && i < size; ++i) {
        g_rng = (g_rng << 8) | data[i];
    }

    /* 1) 真实编码器产出合法 V8 包（V8 全枚举开放，几何受控） */
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = d->w;
    cfg.visible_height = d->h;
    cfg.profile = 3u;
    cfg.pixel_format = 0u;
    cfg.bit_depth = 10u;
    cfg.qmatrix_id = 0u;
    cfg.qp_base = qp;
    cfg.slice_rows = 16u;
    cfg.color_range = 1u;
    cfg.color_primaries = 1u;
    cfg.color_transfer = 1u;
    cfg.color_matrix = 1u;
    cfg.reserved[0] = 9u;

    uint16_t* planes[3] = { NULL, NULL, NULL };
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    const uint32_t lim = 1u << 10;
    int ok = 1;
    for (uint32_t p = 0u; p < 3u; ++p) {
        const uint32_t cols = p == 0u ? (d->w + 7u) / 8u : (d->w + 15u) / 16u;
        const uint32_t rows = (d->h + 7u) / 8u;
        const size_t n = (size_t)cols * 8u * rows * 8u;
        planes[p] = (uint16_t*)malloc(n * sizeof(uint16_t));
        if (planes[p] == NULL) { ok = 0; break; }
        for (size_t i = 0; i < n; ++i) { planes[p][i] = (uint16_t)(rng() % lim); }
        in.planes[p] = planes[p];
        in.strides[p] = (size_t)cols * 8u;
    }

    uint8_t* pkt = NULL;
    size_t pkt_size = 0u;
    if (ok) {
        size_t cap = tc_frame_packet_bound(&cfg);
        pkt = (uint8_t*)malloc(cap);
        topos_frame_stats st;
        memset(&st, 0, sizeof(st));
        st.struct_size = (uint32_t)sizeof(st);
        st.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        if (pkt == NULL || tc_frame_encode(&cfg, &in, pkt, cap, &st) != TC_OK) {
            ok = 0;
        } else {
            pkt_size = st.packet_size;
        }
    }

    if (ok) {
        /* 2) 头部补丁成 V9：major(6)/熵(45) + 头 CRC(49..52) 重算。
         *    段目录/瓦片表/段流/瓦片 CRC 与 V8 同构，扫描层无需改动。 */
        pkt[6] = 9u;
        pkt[45] = 9u;
        uint32_t crc = tc_crc32(pkt, 49u);
        pkt[49] = (uint8_t)(crc >> 24);
        pkt[50] = (uint8_t)(crc >> 16);
        pkt[51] = (uint8_t)(crc >> 8);
        pkt[52] = (uint8_t)crc;

        /* 3) 未变异合法 V9 I 包：查询必 TC_OK（实解码契约归 golden/单测） */
        {
            topos_frame_output info;
            memset(&info, 0, sizeof(info));
            info.struct_size = (uint32_t)sizeof(info);
            info.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
            if (tc_frame_decode(pkt, pkt_size, NULL, NULL, &info) != TC_OK) { abort(); }
        }

        /* 4) 受控变异 → 定义集不变量（含查询/实解码/GOP observe） */
        for (uint32_t j = 0u; j < mutations; ++j) {
            pkt[rng() % pkt_size] ^= 0xFFu;
        }
        fuzz_direct(pkt, pkt_size);

        /* 5) 全前缀截断扫描（步长 7 兼顾覆盖与耗时；任意前缀有界返回） */
        for (size_t len = 0u; len <= pkt_size; len += 7u) {
            topos_frame_output info;
            memset(&info, 0, sizeof(info));
            info.struct_size = (uint32_t)sizeof(info);
            info.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
            int32_t rc = tc_frame_decode(pkt, len, NULL, NULL, &info);
            if (!status_is_defined(rc)) { abort(); }
        }
    }

    free(pkt);
    for (uint32_t p = 0u; p < 3u; ++p) { free(planes[p]); }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (is_v9_shaped(data, size)) {
        fuzz_direct(data, size);
        return 0;
    }
    fuzz_synthesized(data, size);
    return 0;
}
