/* test_image_writer —— TPIM 序列化：确定性、round-trip、extras 保留、边界拒绝。 */
#include <stdlib.h>
#include <string.h>

#include "mini_test.h"
#include "image/image_container.h"
#include "image_file_synth.h"
#include "common/alloc.h"
#include "packet_synth.h"

static void test_deterministic_and_roundtrip(void)
{
    uint8_t* f1 = NULL;
    uint8_t* f2 = NULL;
    size_t n1 = 0, n2 = 0;
    const packet_synth_cfg* cfg = packet_synth_cfg_at(PACKET_SYNTH_CFG_ODD);
    MT_CHECK_EQ_I64(image_file_synth_build(cfg, TC_IMG_PROFILE_PREVIEW, NULL, 0,
                                           &f1, &n1), TC_OK);
    MT_CHECK_EQ_I64(image_file_synth_build(cfg, TC_IMG_PROFILE_PREVIEW, NULL, 0,
                                           &f2, &n2), TC_OK);
    /* 确定性：同输入两次构建逐字节一致 */
    MT_CHECK_EQ_U64(n1, n2);
    MT_CHECK(memcmp(f1, f2, n1) == 0);

    /* round-trip：write → probe → write（重打包）→ 逐字节一致 */
    image_mem_src src;
    topos_io io;
    image_mem_src_init(&src, f1, n1, &io);
    topos_image_info info;
    MT_CHECK_EQ_I64(tc_image_probe(&io, &info), TC_OK);

    uint8_t idsc_bytes[TC_IMG_IDSC_SIZE];
    memcpy(idsc_bytes, f1 + TC_IMG_PREAMBLE_SIZE, TC_IMG_IDSC_SIZE);
    topos_image_idsc idsc;
    MT_CHECK_EQ_I64(tci_unpack_idsc(idsc_bytes, sizeof(idsc_bytes), &idsc), TC_OK);

    uint8_t* pixl = (uint8_t*)malloc((size_t)info.pixl_size);
    memcpy(pixl, f1 + info.pixl_offset, (size_t)info.pixl_size);

    image_mem_sink sink;
    topos_io sio;
    uint8_t* out = (uint8_t*)malloc(n1 + 4096);
    image_mem_sink_init(&sink, out, n1 + 4096, &sio);
    topos_image_write_params params;
    memset(&params, 0, sizeof(params));
    params.struct_size = (uint32_t)sizeof(params);
    params.abi_version = TOPOS_IMAGE_ABI_VERSION;
    params.idsc = idsc;
    params.pixl_data = pixl;
    params.pixl_size = (size_t)info.pixl_size;
    uint64_t written = 0;
    MT_CHECK_EQ_I64(tc_image_write(&params, &sio, &written), TC_OK);
    MT_CHECK_EQ_U64(written, n1);
    MT_CHECK(memcmp(out, f1, n1) == 0);

    free(out);
    free(pixl);
    free(f1);
    free(f2);
}

/* 最小合法 ICC 头：132B，offset36 'acsp'，colorSpace 'RGB ' */
static uint8_t k_iccp_min[132] = {
    0, 0, 0, 132, 0, 0, 0, 0, 0, 4, 48, 0,
    'm', 'n', 't', 'r', 'R', 'G', 'B', ' ', 'X', 'Y', 'Z', ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'a', 'c', 's', 'p',
};

static void test_optional_chunks_preserved(void)
{
    const char ocio[] = "ACES - ACEScg";
    topos_image_chunk_in extras[2];
    memset(extras, 0, sizeof(extras));
    extras[0].chunk_type = TC_IMG_CHUNK_ICCP;
    extras[0].chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL | TC_IMG_CHUNK_FLAG_PRESERVE;
    extras[0].data = k_iccp_min;
    extras[0].size = sizeof(k_iccp_min);
    extras[1].chunk_type = TC_IMG_CHUNK_OCIO;
    extras[1].chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL | TC_IMG_CHUNK_FLAG_PRESERVE;
    extras[1].data = ocio;
    extras[1].size = sizeof(ocio);

    uint8_t* file = NULL;
    size_t n = 0;
    /* refs=1 与 chunk 存在一致（spec §7 单一权威）。
     * MINIMAL = YUV422 Standard → 必须标 PREVIEW（§9.1 组合校验） */
    MT_CHECK_EQ_I64(image_file_synth_build_ex(
                        packet_synth_cfg_at(PACKET_SYNTH_CFG_MINIMAL),
                        TC_IMG_PROFILE_PREVIEW, extras, 2, 1u, 1u,
                        &file, &n), TC_OK);
    image_mem_src src;
    topos_io io;
    image_mem_src_init(&src, file, n, &io);
    topos_image_info info;
    MT_CHECK_EQ_I64(tc_image_probe(&io, &info), TC_OK);
    MT_CHECK_EQ_U64(info.preamble.directory_count, 4u);
    MT_CHECK_EQ_I64(tc_image_validate(&io, 0, &info), TC_OK);
    MT_CHECK_EQ_U64(info.idsc.iccp_ref, 1u);
    MT_CHECK_EQ_U64(info.idsc.ocio_ref, 1u);
    free(file);
}

static void test_write_rejects(void)
{
    uint8_t* pixl = NULL;
    size_t pixl_size = 0;
    MT_CHECK_EQ_I64(packet_synth_build(packet_synth_cfg_at(PACKET_SYNTH_CFG_MINIMAL),
                                       &pixl, &pixl_size), TC_OK);
    topos_image_idsc idsc;
    MT_CHECK_EQ_I64(image_file_synth_idsc(pixl, pixl_size, TC_IMG_PROFILE_PREVIEW,
                                          &idsc), TC_OK);

    topos_image_write_params p;
    memset(&p, 0, sizeof(p));
    p.struct_size = (uint32_t)sizeof(p);
    p.abi_version = TOPOS_IMAGE_ABI_VERSION;
    p.idsc = idsc;
    p.pixl_data = pixl;
    p.pixl_size = pixl_size;

    image_mem_sink sink;
    topos_io io;
    uint8_t buf[65536];
    uint64_t written = 0;

    /* IDSC↔PIXL 冲突（绑定摘要改 1 位） */
    {
        topos_image_write_params bad = p;
        bad.idsc.payload_header_crc32 ^= 1u;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written), TC_IMG_ERR_CHUNK_CONFLICT);
    }
    /* image_profile 与内容不匹配（YUV422 内容标 HQ）→ spec §9.1 组合校验拒绝
     * （兑现 topos_image.h 对 tc_image_write 交叉校验的承诺） */
    {
        topos_image_write_params bad = p;
        bad.idsc.image_profile = TC_IMG_PROFILE_HQ;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written), TC_IMG_ERR_CHUNK_CONFLICT);
    }
    /* PIXL 超 256MiB 上限（仅尺寸检查，不实际分配） */
    {
        topos_image_write_params bad = p;
        bad.pixl_size = (size_t)TC_IMG_MAX_CHUNK_PIXL + 1u;
        bad.pixl_data = buf; /* 指针仅为非空检查 */
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written), TC_ERR_LIMIT_EXCEEDED);
    }
    /* extras 含 critical 位 */
    {
        topos_image_chunk_in extra;
        memset(&extra, 0, sizeof(extra));
        extra.chunk_type = TC_IMG_CHUNK_ICCP;
        extra.chunk_flags = TC_IMG_CHUNK_FLAG_CRITICAL;
        extra.data = "x";
        extra.size = 1;
        topos_image_write_params bad = p;
        bad.extra_chunks = &extra;
        bad.extra_count = 1;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written), TC_ERR_INVALID_ARGUMENT);
    }
    /* extras 重复 IDSC */
    {
        topos_image_chunk_in extra;
        memset(&extra, 0, sizeof(extra));
        extra.chunk_type = TC_IMG_CHUNK_IDSC;
        extra.chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL;
        extra.data = "x";
        extra.size = 1;
        topos_image_write_params bad = p;
        bad.extra_chunks = &extra;
        bad.extra_count = 1;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written), TC_ERR_INVALID_ARGUMENT);
    }
    /* 预留 FourCC 拒绝产出 */
    {
        topos_image_chunk_in extra;
        memset(&extra, 0, sizeof(extra));
        extra.chunk_type = TC_IMG_CHUNK_TILE;
        extra.chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL;
        extra.data = "x";
        extra.size = 1;
        topos_image_write_params bad = p;
        bad.extra_chunks = &extra;
        bad.extra_count = 1;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written), TC_ERR_INVALID_ARGUMENT);
    }
    /* known optional 超长 */
    {
        topos_image_chunk_in extra;
        memset(&extra, 0, sizeof(extra));
        extra.chunk_type = TC_IMG_CHUNK_OCIO;
        extra.chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL;
        extra.data = buf;
        extra.size = (size_t)TC_IMG_MAX_CHUNK_OCIO + 1u;
        topos_image_write_params bad = p;
        bad.extra_chunks = &extra;
        bad.extra_count = 1;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written), TC_ERR_LIMIT_EXCEEDED);
    }
    /* struct_size 错误 */
    {
        topos_image_write_params bad = p;
        bad.struct_size = 1u;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written), TC_ERR_INVALID_ARGUMENT);
    }
    /* compatibility_flags 含未定义位（审计 V4：曾可写出自家 reader 拒绝的
     * 文件——minor=0 只定义 bit0/bit1） */
    {
        topos_image_write_params bad = p;
        bad.compatibility_flags = 0xFFu;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written), TC_ERR_INVALID_ARGUMENT);
        /* 合法位（bit0|bit1）应照常通过 */
        topos_image_write_params ok = p;
        ok.compatibility_flags = 0x3u;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&ok, &io, &written), TC_OK);
    }
    /* extras 之间重复 FourCC（审计 V6：双 XMP 曾可写出，read_metadata 只回
     * 第一个——违反 spec §2.9 单一权威） */
    {
        topos_image_chunk_in extras[2];
        memset(extras, 0, sizeof(extras));
        extras[0].chunk_type = TC_IMG_CHUNK_XMP;
        extras[0].chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL;
        extras[0].data = "<a/>";
        extras[0].size = 4;
        extras[1].chunk_type = TC_IMG_CHUNK_XMP;
        extras[1].chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL;
        extras[1].data = "<b/>";
        extras[1].size = 4;
        topos_image_write_params bad = p;
        bad.extra_chunks = extras;
        bad.extra_count = 2;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written), TC_ERR_INVALID_ARGUMENT);
        /* 不重复的组合（XMP+EXIF）应照常通过 */
        extras[1].chunk_type = TC_IMG_CHUNK_EXIF;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written), TC_OK);
    }
    /* write params reserved 必须为 0（防未来启用时旧误用被静默吞掉） */
    {
        topos_image_write_params bad = p;
        bad.reserved[3] = 1u;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written), TC_ERR_INVALID_ARGUMENT);
    }
    free(pixl);
}

/* 阶段 3：metadata 写端受限校验（spec §7 / ADR-I003） */
static void test_write_metadata_validation(void)
{
    uint8_t* pixl = NULL;
    size_t pixl_size = 0;
    MT_CHECK_EQ_I64(packet_synth_build(packet_synth_cfg_at(PACKET_SYNTH_CFG_MINIMAL),
                                       &pixl, &pixl_size), TC_OK);
    topos_image_idsc idsc;
    MT_CHECK_EQ_I64(image_file_synth_idsc(pixl, pixl_size, TC_IMG_PROFILE_PREVIEW,
                                          &idsc), TC_OK);
    topos_image_write_params p;
    memset(&p, 0, sizeof(p));
    p.struct_size = (uint32_t)sizeof(p);
    p.abi_version = TOPOS_IMAGE_ABI_VERSION;
    p.idsc = idsc;
    p.pixl_data = pixl;
    p.pixl_size = pixl_size;

    image_mem_sink sink;
    topos_io io;
    uint8_t buf[65536];
    uint64_t written = 0;

    /* OCIO 带路径分隔符 → 拒绝 */
    {
        topos_image_chunk_in extra;
        memset(&extra, 0, sizeof(extra));
        extra.chunk_type = TC_IMG_CHUNK_OCIO;
        extra.chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL;
        extra.data = "/usr/local/lib/ocio/config";
        extra.size = 27;
        topos_image_write_params bad = p;
        bad.extra_chunks = &extra;
        bad.extra_count = 1;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written),
                        TC_IMG_ERR_METADATA_CONFLICT);
    }
    /* OCIO 动态库后缀 → 拒绝 */
    {
        topos_image_chunk_in extra;
        memset(&extra, 0, sizeof(extra));
        extra.chunk_type = TC_IMG_CHUNK_OCIO;
        extra.chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL;
        extra.data = "evil.dylib";
        extra.size = 10;
        topos_image_write_params bad = p;
        bad.extra_chunks = &extra;
        bad.extra_count = 1;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written),
                        TC_IMG_ERR_METADATA_CONFLICT);
    }
    /* ICCP 缺 'acsp' → 拒绝 */
    {
        uint8_t bad_icc[132];
        memcpy(bad_icc, k_iccp_min, sizeof(bad_icc));
        bad_icc[36] = 'X'; bad_icc[37] = 'Y'; bad_icc[38] = 'Z'; bad_icc[39] = 'Z';
        topos_image_chunk_in extra;
        memset(&extra, 0, sizeof(extra));
        extra.chunk_type = TC_IMG_CHUNK_ICCP;
        extra.chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL;
        extra.data = bad_icc;
        extra.size = sizeof(bad_icc);
        topos_image_write_params bad = p;
        bad.extra_chunks = &extra;
        bad.extra_count = 1;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written),
                        TC_IMG_ERR_METADATA_CONFLICT);
    }
    /* iccp_ref=1 但无 ICCP chunk → 拒绝 */
    {
        topos_image_write_params bad = p;
        bad.idsc.iccp_ref = 1u;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written),
                        TC_IMG_ERR_METADATA_CONFLICT);
    }
    /* ICCP 存在但 iccp_ref=0 → 拒绝（单一权威） */
    {
        topos_image_chunk_in extra;
        memset(&extra, 0, sizeof(extra));
        extra.chunk_type = TC_IMG_CHUNK_ICCP;
        extra.chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL;
        extra.data = k_iccp_min;
        extra.size = sizeof(k_iccp_min);
        topos_image_write_params bad = p;
        bad.extra_chunks = &extra;
        bad.extra_count = 1;
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        MT_CHECK_EQ_I64(tc_image_write(&bad, &io, &written),
                        TC_IMG_ERR_METADATA_CONFLICT);
    }
    free(pixl);
}

static void test_write_oom(void)
{
    uint8_t* pixl = NULL;
    size_t pixl_size = 0;
    MT_CHECK_EQ_I64(packet_synth_build(packet_synth_cfg_at(PACKET_SYNTH_CFG_MINIMAL),
                                       &pixl, &pixl_size), TC_OK);
    topos_image_idsc idsc;
    MT_CHECK_EQ_I64(image_file_synth_idsc(pixl, pixl_size, TC_IMG_PROFILE_PREVIEW,
                                          &idsc), TC_OK);
    topos_image_write_params p;
    memset(&p, 0, sizeof(p));
    p.struct_size = (uint32_t)sizeof(p);
    p.abi_version = TOPOS_IMAGE_ABI_VERSION;
    p.idsc = idsc;
    p.pixl_data = pixl;
    p.pixl_size = pixl_size;

    for (int64_t fail_at = 1; fail_at <= 8; ++fail_at) {
        tc_dev_set_alloc_fault(fail_at);
        image_mem_sink sink;
        topos_io io;
        uint8_t buf[65536];
        image_mem_sink_init(&sink, buf, sizeof(buf), &io);
        uint64_t written = 0;
        const int32_t rc = tc_image_write(&p, &io, &written);
        tc_dev_set_alloc_fault(-1);
        if (rc != TC_OK) {
            MT_CHECK(rc == TC_ERR_OUT_OF_MEMORY);
        }
    }
    free(pixl);
}

int main(void)
{
    test_deterministic_and_roundtrip();
    test_optional_chunks_preserved();
    test_write_rejects();
    test_write_metadata_validation();
    test_write_oom();
    return MT_MAIN_RETURN();
}
