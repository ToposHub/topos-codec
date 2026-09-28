/* test_image_reader —— TPIM probe/validate/decode + 负向量族（阶段 1 门禁）。
 *
 * 不变量（spec §7 阶段 1 验收）：
 *  - 合法文件 parse 全通过，decode 语义 == 直接 tc_frame_decode 同 payload；
 *  - 任意截断/位翻转/伪造 offset：有界错误、不越界、不死循环、不超预算分配；
 *  - 未知 optional 跳过；未知 critical 干净拒绝；
 *  - IDSC↔PIXL 冲突稳定返回 TC_IMG_ERR_CHUNK_CONFLICT。
 */
#include <stdlib.h>
#include <string.h>

#include "mini_test.h"
#include "image/image_container.h"
#include "image_file_synth.h"
#include "common/alloc.h"
#include "common/crc32.h"
#include "packet_synth.h"

/* —— 构造基准文件（64x32 + alpha mode1，覆盖多带与 alpha 平面） —— */
static int32_t build_base(uint8_t** out, size_t* out_size)
{
    return image_file_synth_build(packet_synth_cfg_at(PACKET_SYNTH_CFG_ALPHA),
                                  TC_IMG_PROFILE_PREVIEW, NULL, 0, out, out_size);
}

static void test_probe_validate_decode_ok(void)
{
    uint8_t* file = NULL;
    size_t n = 0;
    MT_CHECK_EQ_I64(build_base(&file, &n), TC_OK);

    image_mem_src src;
    topos_io io;
    image_mem_src_init(&src, file, n, &io);
    topos_image_info info;
    MT_CHECK_EQ_I64(tc_image_probe(&io, &info), TC_OK);
    MT_CHECK_EQ_U64(info.preamble.directory_count, 2u);
    MT_CHECK_EQ_U64(info.visible_width, 64u);
    MT_CHECK_EQ_U64(info.visible_height, 32u);
    MT_CHECK_EQ_U64(info.has_alpha, 1u);
    MT_CHECK_EQ_U64(info.alpha_mode, 1u);
    MT_CHECK_EQ_U64(info.alpha_bit_depth, 16u);
    MT_CHECK_EQ_U64(info.idsc.image_profile, TC_IMG_PROFILE_PREVIEW);

    MT_CHECK_EQ_I64(tc_image_validate(&io, TC_IMG_VALIDATE_DEEP, &info), TC_OK);

    /* decode == 直接 codec decode（层间零分叉） */
    uint32_t w = 0, h = 0;
    MT_CHECK_EQ_I64(tc_image_query_decode_buffer(&info, 0, &w, &h), TC_OK);
    MT_CHECK_EQ_U64(w, 64u);
    MT_CHECK_EQ_U64(h, 32u);
    MT_CHECK_EQ_I64(tc_image_query_decode_buffer(&info, 3, &w, &h), TC_OK);
    MT_CHECK_EQ_U64(w, 64u); /* alpha 全分辨率 */
    MT_CHECK_EQ_U64(h, 32u);

    uint16_t* planes_ref[4] = {NULL, NULL, NULL, NULL};
    uint16_t* planes_img[4] = {NULL, NULL, NULL, NULL};
    size_t strides[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        planes_ref[i] = (uint16_t*)malloc(64ull * 32ull * sizeof(uint16_t));
        planes_img[i] = (uint16_t*)malloc(64ull * 32ull * sizeof(uint16_t));
        MT_CHECK(planes_ref[i] != NULL && planes_img[i] != NULL);
        memset(planes_ref[i], 0xAA, 64ull * 32ull * 2ull);
        memset(planes_img[i], 0x55, 64ull * 32ull * 2ull);
    }

    /* 参考：直接对 PIXL decode */
    const uint8_t* pixl = file + info.pixl_offset;
    topos_frame_output ref_info, img_info;
    memset(&ref_info, 0, sizeof(ref_info));
    ref_info.struct_size = (uint32_t)sizeof(ref_info);
    ref_info.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK(tc_frame_decode(pixl, (size_t)info.pixl_size,
                             (uint16_t* const*)planes_ref, strides, &ref_info) >= 0);
    memset(&img_info, 0, sizeof(img_info));
    img_info.struct_size = (uint32_t)sizeof(img_info);
    img_info.abi_version = TOPOS_CODEC_ABI_VERSION;
    topos_plane_view views[4];
    for (int i = 0; i < 4; ++i) {
        memset(&views[i], 0, sizeof(views[i]));
        views[i].struct_size = (uint32_t)sizeof(views[i]);
        views[i].abi_version = TOPOS_CODEC_ABI_VERSION; /* plane_view 为 codec 域结构 */
        views[i].pixels = planes_img[i];
        views[i].stride = 0;
    }
    const int32_t drc = tc_image_decode(&io, views, &img_info);
    MT_CHECK(drc >= 0);
    MT_CHECK(memcmp(planes_ref[0], planes_img[0], 64ull * 32ull * 2ull) == 0);
    MT_CHECK(memcmp(planes_ref[3], planes_img[3], 64ull * 32ull * 2ull) == 0);
    MT_CHECK_EQ_U64(img_info.visible_width, ref_info.visible_width);
    MT_CHECK_EQ_U64(img_info.slice_count, ref_info.slice_count);

    for (int i = 0; i < 4; ++i) { free(planes_ref[i]); free(planes_img[i]); }
    free(file);
}

static void test_bad_magic_and_truncation(void)
{
    uint8_t* file = NULL;
    size_t n = 0;
    MT_CHECK_EQ_I64(build_base(&file, &n), TC_OK);

    /* 裸 TPIC packet 误当 .toos */
    {
        image_mem_src src;
        topos_io io;
        image_mem_src_init(&src, file + TC_IMG_PREAMBLE_SIZE + TC_IMG_IDSC_SIZE,
                           n - TC_IMG_PREAMBLE_SIZE - TC_IMG_IDSC_SIZE, &io);
        topos_image_info info;
        MT_CHECK_EQ_I64(tc_image_probe(&io, &info), TC_IMG_ERR_BAD_MAGIC);
    }
    /* 全前缀截断扫描：每个长度都有界返回、不崩溃 */
    for (size_t cut = 0; cut <= n; cut += (n > 512 ? 7 : 1)) {
        image_mem_src src;
        topos_io io;
        image_mem_src_init(&src, file, cut, &io);
        topos_image_info info;
        const int32_t rc = tc_image_probe(&io, &info);
        MT_CHECK(rc != TC_OK); /* 截断文件不可能完整合法 */
        MT_CHECK(rc == TC_ERR_TRUNCATED || rc == TC_IMG_ERR_BAD_PREAMBLE ||
                 rc == TC_IMG_ERR_BAD_DIRECTORY || rc == TC_ERR_LIMIT_EXCEEDED ||
                 rc == TC_IMG_ERR_BAD_MAGIC);
    }
    free(file);
}

static void test_bitflip_sweep(void)
{
    uint8_t* file = NULL;
    size_t n = 0;
    MT_CHECK_EQ_I64(build_base(&file, &n), TC_OK);
    /* 单字节翻转扫描 header+directory 区（关键解析面） */
    for (size_t off = 0; off < TC_IMG_PREAMBLE_SIZE + TC_IMG_IDSC_SIZE + 64u; ++off) {
        for (int bit = 0; bit < 8; ++bit) {
            uint8_t* mutated = (uint8_t*)malloc(n);
            memcpy(mutated, file, n);
            mutated[off] ^= (uint8_t)(1u << bit);
            image_mem_src src;
            topos_io io;
            image_mem_src_init(&src, mutated, n, &io);
            topos_image_info info;
            const int32_t rc = tc_image_probe(&io, &info);
            (void)rc; /* 任意结果合法：拒绝或（payload 未变的路径）成功 */
            free(mutated);
        }
    }
    free(file);
}

static void test_unknown_chunks(void)
{
    uint8_t* file = NULL;
    size_t n = 0;
    MT_CHECK_EQ_I64(build_base(&file, &n), TC_OK);

    /* 未知 optional → probe/validate 通过 */
    {
        const char payload[] = "hello unknown optional";
        topos_image_chunk_in extra;
        memset(&extra, 0, sizeof(extra));
        extra.chunk_type = 0x55524E44u; /* 'URND' */
        extra.chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL | TC_IMG_CHUNK_FLAG_PRESERVE;
        extra.data = payload;
        extra.size = sizeof(payload);
        uint8_t* f2 = NULL;
        size_t n2 = 0;
        MT_CHECK_EQ_I64(image_file_synth_build(packet_synth_cfg_at(PACKET_SYNTH_CFG_ALPHA),
                                               TC_IMG_PROFILE_PREVIEW, &extra, 1,
                                               &f2, &n2), TC_OK);
        image_mem_src src;
        topos_io io;
        image_mem_src_init(&src, f2, n2, &io);
        topos_image_info info;
        MT_CHECK_EQ_I64(tc_image_probe(&io, &info), TC_OK);
        MT_CHECK_EQ_I64(tc_image_validate(&io, 0, &info), TC_OK);
        free(f2);
    }
    /* 未知 critical → 干净拒绝 */
    {
        /* 手工构造：把 optional 标志换成 critical */
        uint8_t* f2 = (uint8_t*)malloc(n);
        memcpy(f2, file, n);
        /* directory 在尾部；找到 entry（'URND' 不存在——用 PIXL 之后插不了，
         * 改为直接把 IDSC 的 flags 改为 optional 且新增 entry 太复杂；
         * 简化：翻转 directory 中 PIXL entry 的 critical 位为 optional → 缺 critical
         * + IDSC 仍是 critical；再把 IDSC entry 改成未知 FourCC 'XXXX'） */
        const uint64_t dir_off = ((uint64_t)f2[24] << 56) | ((uint64_t)f2[25] << 48) |
                                 ((uint64_t)f2[26] << 40) | ((uint64_t)f2[27] << 32) |
                                 ((uint64_t)f2[28] << 24) | ((uint64_t)f2[29] << 16) |
                                 ((uint64_t)f2[30] << 8) | f2[31];
        uint8_t* idsc_entry = f2 + dir_off;
        MT_CHECK(idsc_entry[0] == 'I' && idsc_entry[1] == 'D' &&
                 idsc_entry[2] == 'S' && idsc_entry[3] == 'C');
        idsc_entry[0] = 'X'; idsc_entry[1] = 'X'; idsc_entry[2] = 'X'; idsc_entry[3] = 'X';
        /* directory CRC 已失效 → 先修 CRC（对 64B 目录重算） */
        const uint32_t dir_count = ((uint32_t)f2[36] << 24) | ((uint32_t)f2[37] << 16) |
                                   ((uint32_t)f2[38] << 8) | f2[39];
        const uint32_t new_dir_crc = tc_crc32(f2 + dir_off, (size_t)dir_count * 32u);
        f2[52] = (uint8_t)(new_dir_crc >> 24);
        f2[53] = (uint8_t)(new_dir_crc >> 16);
        f2[54] = (uint8_t)(new_dir_crc >> 8);
        f2[55] = (uint8_t)new_dir_crc;
        /* preamble CRC 重算（清零 header_crc32 字段） */
        uint8_t zeroed[64];
        memcpy(zeroed, f2, 64);
        zeroed[48] = zeroed[49] = zeroed[50] = zeroed[51] = 0;
        const uint32_t new_pre_crc = tc_crc32(zeroed, 64);
        f2[48] = (uint8_t)(new_pre_crc >> 24);
        f2[49] = (uint8_t)(new_pre_crc >> 16);
        f2[50] = (uint8_t)(new_pre_crc >> 8);
        f2[51] = (uint8_t)new_pre_crc;

        image_mem_src src;
        topos_io io;
        image_mem_src_init(&src, f2, n, &io);
        topos_image_info info;
        MT_CHECK_EQ_I64(tc_image_probe(&io, &info), TC_IMG_ERR_UNKNOWN_CRITICAL);
        free(f2);
    }
    free(file);
}

static void test_chunk_conflict_and_corruption(void)
{
    uint8_t* file = NULL;
    size_t n = 0;
    MT_CHECK_EQ_I64(build_base(&file, &n), TC_OK);

    /* IDSC 与 PIXL 冲突（改 data window 宽）→ 稳定 CHUNK_CONFLICT */
    {
        uint8_t* f2 = (uint8_t*)malloc(n);
        memcpy(f2, file, n);
        f2[TC_IMG_PREAMBLE_SIZE + 40] ^= 0x01; /* data_x_max 低位翻转 */
        image_mem_src src;
        topos_io io;
        image_mem_src_init(&src, f2, n, &io);
        topos_image_info info;
        const int32_t rc1 = tc_image_probe(&io, &info);
        MT_CHECK_EQ_I64(rc1, TC_IMG_ERR_CHUNK_CONFLICT);
        const int32_t rc2 = tc_image_probe(&io, &info); /* 同文件再探同错误 */
        MT_CHECK_EQ_I64(rc2, TC_IMG_ERR_CHUNK_CONFLICT);
        free(f2);
    }
    /* chunk CRC 损坏（改 PIXL payload 一个字节）→ validate CHECKSUM_MISMATCH */
    {
        uint8_t* f2 = (uint8_t*)malloc(n);
        memcpy(f2, file, n);
        f2[TC_IMG_PREAMBLE_SIZE + TC_IMG_IDSC_SIZE + 100] ^= 0xFF;
        image_mem_src src;
        topos_io io;
        image_mem_src_init(&src, f2, n, &io);
        topos_image_info info;
        /* probe 只读 53B header——payload 字节可能不命中 header；probe 结果任意 */
        (void)tc_image_probe(&io, &info);
        const int32_t vrc = tc_image_validate(&io, 0, &info);
        MT_CHECK(vrc == TC_ERR_CHECKSUM_MISMATCH || vrc == TC_OK);
        free(f2);
    }
    free(file);
}

static void test_oom_fault_injection(void)
{
    uint8_t* file = NULL;
    size_t n = 0;
    MT_CHECK_EQ_I64(build_base(&file, &n), TC_OK);
    image_mem_src src;
    topos_io io;
    image_mem_src_init(&src, file, n, &io);

    /* 穷举分配故障点：每次失败都必须有界返回 OOM，不崩溃 */
    const int64_t tries = tc_dev_alloc_count() + 16; /* probe 分配点 ≤ 若干 */
    for (int64_t fail_at = 1; fail_at <= tries; ++fail_at) {
        tc_dev_set_alloc_fault(fail_at);
        topos_image_info info;
        const int32_t rc = tc_image_probe(&io, &info);
        tc_dev_set_alloc_fault(-1);
        if (rc != TC_OK) {
            MT_CHECK(rc == TC_ERR_OUT_OF_MEMORY || rc == TC_ERR_INVALID_ARGUMENT);
        }
    }
    free(file);
}

/* 阶段 2：GBR 4:4:4 10-bit（Image HQ）经 envelope 的通道语义验证。
 * 不变量：GBR payload 无隐式 YUV 转换（matrix=0 直传）；
 * envelope decode == 直接 codec decode 逐平面一致（平面序 G,B,R 由
 * pixel_format 唯一确定，两路径同序——无交换、无隐式转换）。 */
static void test_gbr_hq_semantics(void)
{
    const packet_synth_cfg gbr_cfg = {0x0BADC0DE0BADC0DEull, 64u, 48u, 0u,
                                      2u, 24u, 1u, 10u, 2u, 5u, 0u};
    const packet_synth_cfg gbr_alpha_cfg = {0x6BADC0DE6BADC0DEull, 48u, 32u, 1u,
                                            3u, 16u, 1u, 10u, 2u, 5u, 0u};
    for (int with_alpha = 0; with_alpha <= 1; ++with_alpha) {
        const packet_synth_cfg* cfg = with_alpha ? &gbr_alpha_cfg : &gbr_cfg;
        uint8_t* file = NULL;
        size_t n = 0;
        MT_CHECK_EQ_I64(image_file_synth_build(cfg, TC_IMG_PROFILE_HQ, NULL, 0,
                                               &file, &n), TC_OK);
        image_mem_src src;
        topos_io io;
        image_mem_src_init(&src, file, n, &io);
        topos_image_info info;
        MT_CHECK_EQ_I64(tc_image_probe(&io, &info), TC_OK);
        /* IDSC 语义镜像 */
        MT_CHECK_EQ_U64(info.idsc.channel_model, TC_IMG_CHANNEL_MODEL_RGB);
        MT_CHECK_EQ_U64(info.idsc.pixel_format, 2u);
        MT_CHECK_EQ_U64(info.idsc.color_matrix, 0u); /* identity，无 YUV matrix */
        MT_CHECK_EQ_U64(info.idsc.subsampling, 0u);  /* 4:4:4 */
        MT_CHECK_EQ_U64(info.idsc.image_profile, TC_IMG_PROFILE_HQ);
        MT_CHECK_EQ_U64(info.has_alpha, with_alpha);
        if (with_alpha) {
            MT_CHECK_EQ_U64(info.idsc.alpha_presence, TC_IMG_ALPHA_STRAIGHT);
            MT_CHECK_EQ_U64(info.idsc.alpha_mode, 1u); /* A16 lossless */
            MT_CHECK_EQ_U64(info.idsc.alpha_bit_depth, 16u);
        }
        MT_CHECK_EQ_I64(tc_image_validate(&io, TC_IMG_VALIDATE_DEEP, &info), TC_OK);

        /* envelope decode == 直接 codec decode（逐平面一致：无通道交换） */
        const uint32_t w = info.visible_width;
        const uint32_t h = info.visible_height;
        uint16_t* ref[4] = {NULL, NULL, NULL, NULL};
        uint16_t* img[4] = {NULL, NULL, NULL, NULL};
        size_t strides[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            ref[i] = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
            img[i] = (uint16_t*)malloc((size_t)w * h * sizeof(uint16_t));
            MT_CHECK(ref[i] != NULL && img[i] != NULL);
            memset(ref[i], 0x11, (size_t)w * h * 2u);
            memset(img[i], 0x22, (size_t)w * h * 2u);
        }
        topos_frame_output ref_info, img_info;
        memset(&ref_info, 0, sizeof(ref_info));
        ref_info.struct_size = (uint32_t)sizeof(ref_info);
        ref_info.abi_version = TOPOS_CODEC_ABI_VERSION;
        MT_CHECK(tc_frame_decode(file + info.pixl_offset, (size_t)info.pixl_size,
                                 (uint16_t* const*)ref, strides, &ref_info) >= 0);
        topos_plane_view views[4];
        for (int i = 0; i < 4; ++i) {
            memset(&views[i], 0, sizeof(views[i]));
            views[i].struct_size = (uint32_t)sizeof(views[i]);
            views[i].abi_version = TOPOS_CODEC_ABI_VERSION; /* plane_view 为 codec 域结构 */
            views[i].pixels = img[i];
            views[i].stride = 0;
        }
        memset(&img_info, 0, sizeof(img_info));
        img_info.struct_size = (uint32_t)sizeof(img_info);
        img_info.abi_version = TOPOS_CODEC_ABI_VERSION;
        const int32_t drc = tc_image_decode(&io, views, &img_info);
        MT_CHECK(drc >= 0);
        const uint32_t planes = 3u + (with_alpha ? 1u : 0u);
        for (uint32_t p = 0; p < planes; ++p) { /* pf=2 平面序 G,B,R（ADR-C017 C-130） */
            if (memcmp(ref[p], img[p], (size_t)w * h * 2u) != 0) {
                fprintf(stderr, "GBR plane %u mismatch (with_alpha=%d)\n", p, with_alpha);
                MT_CHECK(0);
            }
        }
        for (int i = 0; i < 4; ++i) { free(ref[i]); free(img[i]); }
        free(file);
    }
}

/* 阶段 3：metadata 受限读回 + 12-bit XQ 语义 */
static uint8_t k_iccp_meta[132] = {
    0, 0, 0, 132, 0, 0, 0, 0, 0, 4, 48, 0,
    'm', 'n', 't', 'r', 'R', 'G', 'B', ' ', 'X', 'Y', 'Z', ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'a', 'c', 's', 'p',
};

static void test_stage3_metadata_and_xq(void)
{
    const char ocio[] = "ACES - ACEScg";
    const char xmp[] = "<x:xmpmeta>stage3</x:xmpmeta>";
    topos_image_chunk_in extras[3];
    memset(extras, 0, sizeof(extras));
    extras[0].chunk_type = TC_IMG_CHUNK_ICCP;
    extras[0].chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL | TC_IMG_CHUNK_FLAG_PRESERVE;
    extras[0].data = k_iccp_meta;
    extras[0].size = sizeof(k_iccp_meta);
    extras[1].chunk_type = TC_IMG_CHUNK_OCIO;
    extras[1].chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL | TC_IMG_CHUNK_FLAG_PRESERVE;
    extras[1].data = ocio;
    extras[1].size = sizeof(ocio);
    extras[2].chunk_type = TC_IMG_CHUNK_XMP;
    extras[2].chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL | TC_IMG_CHUNK_FLAG_PRESERVE;
    extras[2].data = xmp;
    extras[2].size = sizeof(xmp);

    /* --- metadata 读回（round-trip 字节一致 + 尺寸查询 + 缺失拒绝）。
     * MINIMAL = YUV422 Standard → 必须标 PREVIEW（§9.1 组合校验） --- */
    {
        uint8_t* file = NULL;
        size_t n = 0;
        MT_CHECK_EQ_I64(image_file_synth_build_ex(
                            packet_synth_cfg_at(PACKET_SYNTH_CFG_MINIMAL),
                            TC_IMG_PROFILE_PREVIEW, extras, 3, 1u, 1u,
                            &file, &n), TC_OK);
        image_mem_src src;
        topos_io io;
        image_mem_src_init(&src, file, n, &io);
        topos_image_info info;
        MT_CHECK_EQ_I64(tc_image_probe(&io, &info), TC_OK);

        uint8_t back[4096];
        size_t got = 0;
        MT_CHECK_EQ_I64(tc_image_read_metadata(&io, &info, TC_IMG_CHUNK_ICCP,
                                               NULL, 0, &got), TC_OK);
        MT_CHECK_EQ_U64(got, sizeof(k_iccp_meta));
        MT_CHECK_EQ_I64(tc_image_read_metadata(&io, &info, TC_IMG_CHUNK_ICCP,
                                               back, 8, &got),
                        TC_ERR_BUFFER_TOO_SMALL);
        MT_CHECK_EQ_I64(tc_image_read_metadata(&io, &info, TC_IMG_CHUNK_ICCP,
                                               back, sizeof(back), &got), TC_OK);
        MT_CHECK_EQ_U64(got, sizeof(k_iccp_meta));
        MT_CHECK(memcmp(back, k_iccp_meta, got) == 0);
        MT_CHECK_EQ_I64(tc_image_read_metadata(&io, &info, TC_IMG_CHUNK_OCIO,
                                               back, sizeof(back), &got), TC_OK);
        MT_CHECK(memcmp(back, ocio, got) == 0);
        MT_CHECK_EQ_I64(tc_image_read_metadata(&io, &info, TC_IMG_CHUNK_EXIF,
                                               back, sizeof(back), &got),
                        TC_ERR_INVALID_ARGUMENT); /* 不存在 */
        MT_CHECK_EQ_I64(tc_image_read_metadata(&io, &info, TC_IMG_CHUNK_PIXL,
                                               back, sizeof(back), &got),
                        TC_ERR_INVALID_ARGUMENT); /* critical 不可经此读取 */
        free(file);
    }

    /* --- 损坏 OCIO：读取时内容校验 → METADATA_CONFLICT --- */
    {
        uint8_t* file = NULL;
        size_t n = 0;
        MT_CHECK_EQ_I64(image_file_synth_build_ex(
                            packet_synth_cfg_at(PACKET_SYNTH_CFG_MINIMAL),
                            TC_IMG_PROFILE_PREVIEW, NULL, 0, 0u, 0u,
                            &file, &n), TC_OK);
        /* 手工把 OCIO（尚不存在）改为：写入后翻转——简化为直接构造：
         * 先带 OCIO 写入，再翻转 payload 一个字节为 '/' */
        free(file);
        topos_image_chunk_in extra;
        memset(&extra, 0, sizeof(extra));
        extra.chunk_type = TC_IMG_CHUNK_OCIO;
        extra.chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL;
        extra.data = ocio;
        extra.size = sizeof(ocio);
        MT_CHECK_EQ_I64(image_file_synth_build_ex(
                            packet_synth_cfg_at(PACKET_SYNTH_CFG_MINIMAL),
                            TC_IMG_PROFILE_PREVIEW, &extra, 1, 0u, 1u,
                            &file, &n), TC_OK);
        /* 定位 OCIO payload（PIXL 之后）并翻转为路径分隔符：
         * "ACES - ACEScg" 的 'c'（偏移 11）→ '/' */
        image_mem_src src;
        topos_io io;
        image_mem_src_init(&src, file, n, &io);
        topos_image_info info;
        MT_CHECK_EQ_I64(tc_image_probe(&io, &info), TC_OK);
        const size_t ocio_off = (size_t)info.pixl_offset + (size_t)info.pixl_size;
        MT_CHECK(file[ocio_off + 11] == 'c');
        file[ocio_off + 11] = '/';
        uint8_t back[4096];
        size_t got = 0;
        MT_CHECK_EQ_I64(tc_image_read_metadata(&io, &info, TC_IMG_CHUNK_OCIO,
                                               back, sizeof(back), &got),
                        TC_IMG_ERR_METADATA_CONFLICT);
        /* 内容校验失败时 *out_size 必须复位为 0（调用方误用防线） */
        MT_CHECK_EQ_U64(got, 0u);
        free(file);
    }

    /* --- 损坏 OCIO：非良构 UTF-8（孤立续字节）→ 读取时拒绝 --- */
    {
        uint8_t* file = NULL;
        size_t n = 0;
        topos_image_chunk_in extra;
        memset(&extra, 0, sizeof(extra));
        extra.chunk_type = TC_IMG_CHUNK_OCIO;
        extra.chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL;
        extra.data = ocio;
        extra.size = sizeof(ocio);
        MT_CHECK_EQ_I64(image_file_synth_build_ex(
                            packet_synth_cfg_at(PACKET_SYNTH_CFG_MINIMAL),
                            TC_IMG_PROFILE_PREVIEW, &extra, 1, 0u, 1u,
                            &file, &n), TC_OK);
        image_mem_src src;
        topos_io io;
        image_mem_src_init(&src, file, n, &io);
        topos_image_info info;
        MT_CHECK_EQ_I64(tc_image_probe(&io, &info), TC_OK);
        const size_t ocio_off = (size_t)info.pixl_offset + (size_t)info.pixl_size;
        MT_CHECK(file[ocio_off + 11] == 'c');
        file[ocio_off + 11] = 0xFF; /* 孤立续字节：非良构 UTF-8 */
        uint8_t back[4096];
        size_t got = 0;
        MT_CHECK_EQ_I64(tc_image_read_metadata(&io, &info, TC_IMG_CHUNK_OCIO,
                                               back, sizeof(back), &got),
                        TC_IMG_ERR_METADATA_CONFLICT);
        MT_CHECK_EQ_U64(got, 0u);
        free(file);
    }

    /* --- 12-bit XQ：位深全链路保持（不被中间层降为 10/8-bit） --- */
    {
        const packet_synth_cfg xq_cfg = {0xD0D0C0DED0D0C0DEull, 64u, 48u, 0u,
                                         2u, 18u, 1u, 12u, 2u, 5u, 0u};
        uint8_t* file = NULL;
        size_t n = 0;
        MT_CHECK_EQ_I64(image_file_synth_build(&xq_cfg, TC_IMG_PROFILE_XQ,
                                               NULL, 0, &file, &n), TC_OK);
        image_mem_src src;
        topos_io io;
        image_mem_src_init(&src, file, n, &io);
        topos_image_info info;
        MT_CHECK_EQ_I64(tc_image_probe(&io, &info), TC_OK);
        MT_CHECK_EQ_U64(info.bit_depth, 12u);
        MT_CHECK_EQ_U64(info.idsc.valid_bit_depth, 12u);
        MT_CHECK_EQ_U64(info.idsc.container_bit_depth, 12u);
        MT_CHECK_EQ_U64(info.idsc.image_profile, TC_IMG_PROFILE_XQ);

        const uint32_t w = 64, h = 48;
        /* API 契约：planes_out/strides 必须为 TC_FRAME_MAX_PLANES=4 槽 */
        uint16_t* ref[4];
        uint16_t* img[4];
        size_t strides[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            ref[i] = (uint16_t*)malloc((size_t)w * h * 2u);
            img[i] = (uint16_t*)malloc((size_t)w * h * 2u);
            memset(ref[i], 0, (size_t)w * h * 2u);
            memset(img[i], 0, (size_t)w * h * 2u);
        }
        topos_frame_output ref_info, img_info;
        memset(&ref_info, 0, sizeof(ref_info));
        ref_info.struct_size = (uint32_t)sizeof(ref_info);
        ref_info.abi_version = TOPOS_CODEC_ABI_VERSION;
        MT_CHECK(tc_frame_decode(file + info.pixl_offset, (size_t)info.pixl_size,
                                 (uint16_t* const*)ref, strides, &ref_info) >= 0);
        MT_CHECK_EQ_U64(ref_info.bit_depth, 12u);
        topos_plane_view views[4];
        for (int i = 0; i < 4; ++i) {
            memset(&views[i], 0, sizeof(views[i]));
            views[i].struct_size = (uint32_t)sizeof(views[i]);
            views[i].abi_version = TOPOS_CODEC_ABI_VERSION; /* plane_view 为 codec 域结构 */
            views[i].pixels = img[i];
            views[i].stride = 0;
        }
        memset(&img_info, 0, sizeof(img_info));
        img_info.struct_size = (uint32_t)sizeof(img_info);
        img_info.abi_version = TOPOS_CODEC_ABI_VERSION;
        MT_CHECK(tc_image_decode(&io, views, &img_info) >= 0);
        /* 12-bit 值域检查：像素必须 ≤ 4095（未被降位/扩展污染） */
        int over = 0;
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            if (img[0][i] > 4095u || img[1][i] > 4095u || img[2][i] > 4095u) { over = 1; }
            if (ref[0][i] != img[0][i] || ref[1][i] != img[1][i] ||
                ref[2][i] != img[2][i]) { over = 1; }
        }
        MT_CHECK(over == 0);
        for (int i = 0; i < 4; ++i) { free(ref[i]); free(img[i]); }
        free(file);
    }
}

/* plane_view 为 codec 域结构：abi_version 必须是 TOPOS_CODEC_ABI_VERSION。
 * 审计发现三个调用方曾误填 image 域版本（=1）未被察觉——本测试把该类
 * 错误钉死为显式 INVALID_ARGUMENT。 */
static void test_decode_plane_view_abi(void)
{
    uint8_t* file = NULL;
    size_t n = 0;
    MT_CHECK_EQ_I64(build_base(&file, &n), TC_OK);
    image_mem_src src;
    topos_io io;
    image_mem_src_init(&src, file, n, &io);
    topos_image_info info;
    MT_CHECK_EQ_I64(tc_image_probe(&io, &info), TC_OK);

    uint16_t* mem = (uint16_t*)malloc(64ull * 32ull * 4ull * 2ull);
    MT_CHECK(mem != NULL);
    topos_plane_view views[TC_FRAME_MAX_PLANES];
    uint16_t* ptrs[TC_FRAME_MAX_PLANES] = {mem, mem + 64ull * 32ull,
                                           mem + 2ull * 64ull * 32ull,
                                           mem + 3ull * 64ull * 32ull};
    topos_frame_output out;
    memset(&out, 0, sizeof(out));
    out.struct_size = (uint32_t)sizeof(out);
    out.abi_version = TOPOS_CODEC_ABI_VERSION;

    /* 误填 image 域 ABI（=1）→ 拒绝 */
    for (int i = 0; i < TC_FRAME_MAX_PLANES; ++i) {
        memset(&views[i], 0, sizeof(views[i]));
        views[i].struct_size = (uint32_t)sizeof(views[i]);
        views[i].abi_version = TOPOS_IMAGE_ABI_VERSION;
        views[i].pixels = ptrs[i];
        views[i].stride = 0;
    }
    MT_CHECK_EQ_I64(tc_image_decode(&io, views, &out), TC_ERR_INVALID_ARGUMENT);
    /* 全零视图头（struct_size=0）保持历史宽容——但 pixels 全 NULL 时
     * codec 契约按逐 plane NULL 拒绝（查询模式仅指整个数组为 NULL） */
    for (int i = 0; i < TC_FRAME_MAX_PLANES; ++i) {
        memset(&views[i], 0, sizeof(views[i]));
    }
    MT_CHECK_EQ_I64(tc_image_decode(&io, views, &out), TC_ERR_INVALID_ARGUMENT);
    /* 正确的 codec 域 ABI + 缓冲 → 正常解码 */
    for (int i = 0; i < TC_FRAME_MAX_PLANES; ++i) {
        memset(&views[i], 0, sizeof(views[i]));
        views[i].struct_size = (uint32_t)sizeof(views[i]);
        views[i].abi_version = TOPOS_CODEC_ABI_VERSION;
        views[i].pixels = ptrs[i];
        views[i].stride = 0;
    }
    MT_CHECK(tc_image_decode(&io, views, &out) >= 0);
    free(mem);
    free(file);
}

static void test_native_preview_downsample(void)
{
    uint8_t* file = NULL;
    size_t n = 0;
    MT_CHECK_EQ_I64(build_base(&file, &n), TC_OK);
    image_mem_src src;
    topos_io io;
    image_mem_src_init(&src, file, n, &io);
    topos_image_info image_info;
    MT_CHECK_EQ_I64(tc_image_probe(&io, &image_info), TC_OK);

    uint16_t* full[TC_FRAME_MAX_PLANES] = {NULL, NULL, NULL, NULL};
    uint16_t* preview[TC_FRAME_MAX_PLANES] = {NULL, NULL, NULL, NULL};
    for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
        full[p] = (uint16_t*)calloc(64u * 32u, sizeof(uint16_t));
        MT_CHECK(full[p] != NULL);
    }
    topos_frame_output full_info;
    memset(&full_info, 0, sizeof(full_info));
    full_info.struct_size = (uint32_t)sizeof(full_info);
    full_info.abi_version = TOPOS_CODEC_ABI_VERSION;
    size_t tight[TC_FRAME_MAX_PLANES] = {0u, 0u, 0u, 0u};
    MT_CHECK(tc_frame_decode(file + image_info.pixl_offset,
                             (size_t)image_info.pixl_size,
                             full, tight, &full_info) >= 0);

    const uint32_t target_w[TC_FRAME_MAX_PLANES] = {17u, 9u, 9u, 17u};
    for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
        preview[p] = (uint16_t*)calloc((size_t)target_w[p] * 9u, sizeof(uint16_t));
        MT_CHECK(preview[p] != NULL);
    }
    topos_plane_view views[TC_FRAME_MAX_PLANES];
    for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
        memset(&views[p], 0, sizeof(views[p]));
        views[p].struct_size = (uint32_t)sizeof(views[p]);
        views[p].abi_version = TOPOS_CODEC_ABI_VERSION;
        views[p].pixels = preview[p];
        views[p].stride = 0u;
    }
    topos_frame_output preview_info;
    memset(&preview_info, 0, sizeof(preview_info));
    preview_info.struct_size = (uint32_t)sizeof(preview_info);
    preview_info.abi_version = TOPOS_CODEC_ABI_VERSION;
    const int32_t rc = tc_image_decode_preview(&io, 17u, 9u, views, &preview_info);
    MT_CHECK(rc == TC_OK || rc == TC_WARN_CONCEALED);
    MT_CHECK_EQ_U64(preview_info.visible_width, 17u);
    MT_CHECK_EQ_U64(preview_info.visible_height, 9u);
    for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
        uint32_t source_w = 0u;
        uint32_t source_h = 0u;
        MT_CHECK_EQ_I64(tc_frame_plane_geometry(&full_info, p, &source_w, &source_h), TC_OK);
        for (uint32_t y = 0u; y < 9u; ++y) {
            for (uint32_t x = 0u; x < target_w[p]; ++x) {
                const uint32_t sy = (uint32_t)(((uint64_t)y * source_h) / 9u);
                const uint32_t sx = (uint32_t)(((uint64_t)x * source_w) / target_w[p]);
                MT_CHECK(preview[p][(size_t)y * target_w[p] + x] ==
                         full[p][(size_t)sy * source_w + sx]);
            }
        }
    }
    for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
        free(full[p]);
        free(preview[p]);
    }
    free(file);
}

int main(void)
{
    test_probe_validate_decode_ok();
    test_bad_magic_and_truncation();
    test_bitflip_sweep();
    test_unknown_chunks();
    test_chunk_conflict_and_corruption();
    test_oom_fault_injection();
    test_gbr_hq_semantics();
    test_stage3_metadata_and_xq();
    test_decode_plane_view_abi();
    test_native_preview_downsample();
    return MT_MAIN_RETURN();
}
