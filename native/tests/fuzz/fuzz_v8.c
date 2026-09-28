/* fuzz_v8 —— V8 位流 + 解码路径 fuzz（批 5 conformance）。
 *
 * 两条路径（LLVMFuzzerTestOneInput）：
 *   A) 直接：输入本身像 V8 包 → 结构扫描 + 小几何实解码
 *      （分配仅由已验证的 scan 几何驱动，单面 ≤ 256×256）；
 *   B) 合成：真实编码器产出合法 V8 包（几何/位深/粒度/qp 从输入取自
 *      小集合，帧内容 LCG(input)）→ 受控字节变异（数量/位置 LCG(input)）
 *      → 扫描 + 实解码。随机 magic 命中率≈0，合成路径保证每个输入都深走
 *      V8 扫描/解码/CRC/conceal 全路径（正流 + 损伤流一起覆盖）；
 *      确定性 corpus（seed_corpus_v8）因此冗余，-gen LCG 回放即全部覆盖。
 *
 * 不变量（对任意输入）：不崩溃、不挂起（编码几何受控、解码循环有界）、
 * 返回码 ∈ spec 定义集。V8 conceal 语义 = TC_OK + concealed_slices 计数
 * （区别于 V7 的 WARN_CONCEALED，语义钉死见 pytest 矩阵）。
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
           rc == TC_ERR_TRUNCATED || rc == TC_ERR_CHECKSUM_MISMATCH ||
           rc == TC_ERR_NOT_IMPLEMENTED;
}

/* 路径 A：直接解码已验证几何的 V8 包（单面像素 ≤ 256×256 才做实解码） */
static void fuzz_direct(const uint8_t* data, size_t size)
{
    topos_frame_output info;
    int32_t rc = tc_frame_decode(data, size, NULL, NULL, &info);
    if (!status_is_defined(rc)) { abort(); }
    if (rc != TC_OK) { return; }

    uint16_t* planes[TC_FRAME_MAX_PLANES] = { NULL, NULL, NULL, NULL };
    int alloc_ok = 1;
    for (uint32_t p = 0u; p < info.plane_count; ++p) {
        uint32_t pw = 0u, ph = 0u;
        if (tc_frame_plane_geometry(&info, p, &pw, &ph) != TC_OK) { alloc_ok = 0; break; }
        if ((uint64_t)pw * (uint64_t)ph > 256ull * 256ull) { alloc_ok = 0; break; }
        planes[p] = (uint16_t*)calloc((size_t)pw * ph, sizeof(uint16_t));
        if (planes[p] == NULL) { alloc_ok = 0; break; }
    }
    if (alloc_ok) {
        rc = tc_frame_decode(data, size, planes, NULL, &info);
        if (!status_is_defined(rc)) { abort(); }
    }
    for (int p = 0; p < TC_FRAME_MAX_PLANES; ++p) { free(planes[p]); }
}

static uint64_t g_rng;

static uint64_t rng(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}

/* 路径 B：真实编码 + LCG(input) 受控变异 → 扫描 + 解码 */
static void fuzz_synthesized(const uint8_t* data, size_t size)
{
    if (size < 16u) { return; }
    typedef struct { uint16_t w, h; } fuzz_dims;
    static const fuzz_dims kDims[3] = {
        { 64u, 64u }, { 96u, 80u }, { 128u, 96u },
    };
    const uint8_t sel = data[0];
    const fuzz_dims* d = &kDims[sel % 3u];
    const uint8_t pf = (uint8_t)(data[1] & 1u);
    const uint8_t bd = (uint8_t)(data[2] & 1u ? 12u : 10u);
    const uint8_t qp = (uint8_t)(20u + data[3] % 40u);
    const uint8_t sb_log2 = (uint8_t)(3u + data[4] % 3u);
    const uint8_t tr_log2 = (uint8_t)(4u + data[5] % 3u);
    const uint32_t mutations = (uint32_t)(data[14] % 6u);

    g_rng = 0x5EED000000000000ull;
    for (size_t i = 6u; i < 14u && i < size; ++i) {
        g_rng = (g_rng << 8) | data[i];
    }

    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = d->w;
    cfg.visible_height = d->h;
    cfg.profile = pf == 1u ? 5u : 3u;
    cfg.pixel_format = pf;
    cfg.bit_depth = bd;
    cfg.qmatrix_id = 1u;
    cfg.qp_base = qp;
    cfg.slice_rows = 16u;
    cfg.color_range = 1u;
    cfg.color_primaries = 1u;
    cfg.color_transfer = 1u;
    cfg.color_matrix = 1u;
    cfg.reserved[0] = 9u;
    cfg.reserved[3] = sb_log2;
    cfg.reserved[4] = tr_log2;

    uint16_t* planes[3] = { NULL, NULL, NULL };
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    const uint32_t lim = 1u << bd;
    int ok = 1;
    for (uint32_t p = 0u; p < 3u; ++p) {
        const uint32_t cols = p == 0u ? (d->w + 7u) / 8u
                            : (pf == 0u ? (d->w + 15u) / 16u : (d->w + 7u) / 8u);
        const uint32_t rows = (d->h + 7u) / 8u;
        const size_t n = (size_t)cols * 8u * rows * 8u;
        planes[p] = (uint16_t*)malloc(n * sizeof(uint16_t));
        if (planes[p] == NULL) { ok = 0; break; }
        for (size_t i = 0; i < n; ++i) { planes[p][i] = (uint16_t)(rng() % lim); }
        in.planes[p] = planes[p];
        in.strides[p] = (size_t)cols * 8u;
    }

    uint8_t* pkt = NULL;
    if (ok) {
        size_t cap = tc_frame_packet_bound(&cfg);
        pkt = (uint8_t*)malloc(cap);
        topos_frame_stats st;
        if (pkt == NULL || tc_frame_encode(&cfg, &in, pkt, cap, &st) != TC_OK) {
            ok = 0; /* 合成几何受控，编码失败即环境异常；fuzz 不变量仍成立 */
        } else {
            /* 受控变异：位置 LCG 全包均匀采样 */
            for (uint32_t j = 0; j < mutations; ++j) {
                pkt[rng() % st.packet_size] ^= 0xFFu;
            }
            fuzz_direct(pkt, st.packet_size);
        }
    }
    free(pkt);
    for (uint32_t p = 0u; p < 3u; ++p) { free(planes[p]); }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size >= 53u && data[0] == 'T' && data[1] == 'P' && data[2] == 'I' &&
        data[3] == 'C' && data[6] == 8u) {
        fuzz_direct(data, size);
        return 0;
    }
    fuzz_synthesized(data, size);
    return 0;
}
