/* test_image_container —— TPIM pack/unpack + IDSC 规则 + 交叉校验（阶段 1）。 */
#include <stdlib.h>
#include <string.h>

#include "mini_test.h"
#include "image/image_container.h"
#include "image_file_synth.h"

static void test_preamble_roundtrip(void)
{
    topos_image_preamble p;
    memset(&p, 0, sizeof(p));
    p.file_version_major = 1u;
    p.file_version_minor = 0u;
    p.file_size = 0x123456789ull;
    p.directory_offset = 4096ull;
    p.directory_entry_size = TC_IMG_DIR_ENTRY_SIZE;
    p.directory_count = 3u;
    p.primary_image_index = 0u;
    p.compatibility_flags = 0x3u;
    p.directory_crc32 = 0xDEADBEEFu;

    uint8_t bytes[TC_IMG_PREAMBLE_SIZE];
    MT_CHECK_EQ_I64(tci_pack_preamble(&p, bytes), TC_OK);
    /* magic */
    MT_CHECK(bytes[0] == 'T' && bytes[1] == 'P' && bytes[2] == 'I' && bytes[3] == 'M');
    /* 大端字段位置抽查（file_size 低 32 位在 offset 20..23） */
    MT_CHECK_EQ_U64(bytes[8], 0); MT_CHECK_EQ_U64(bytes[11], TC_IMG_PREAMBLE_SIZE);
    MT_CHECK_EQ_U64(bytes[19], (uint8_t)(((uint64_t)p.file_size >> 32) & 0xFF));

    topos_image_preamble q;
    MT_CHECK_EQ_I64(tci_unpack_preamble(bytes, &q), TC_OK);
    MT_CHECK_EQ_U64(q.file_size, p.file_size);
    MT_CHECK_EQ_U64(q.directory_offset, p.directory_offset);
    MT_CHECK_EQ_U64(q.directory_count, p.directory_count);
    MT_CHECK_EQ_U64(q.directory_crc32, p.directory_crc32);
    MT_CHECK_EQ_U64(q.compatibility_flags, p.compatibility_flags);
}

static void test_preamble_negative(void)
{
    topos_image_preamble p;
    memset(&p, 0, sizeof(p));
    p.file_version_major = 1u;
    p.directory_entry_size = TC_IMG_DIR_ENTRY_SIZE;
    p.directory_count = 2u;
    uint8_t bytes[TC_IMG_PREAMBLE_SIZE];
    MT_CHECK_EQ_I64(tci_pack_preamble(&p, bytes), TC_OK);

    /* magic 破坏 */
    {
        uint8_t bad[TC_IMG_PREAMBLE_SIZE];
        memcpy(bad, bytes, sizeof(bytes));
        bad[0] = 'X';
        topos_image_preamble q;
        MT_CHECK_EQ_I64(tci_unpack_preamble(bad, &q), TC_IMG_ERR_BAD_MAGIC);
    }
    /* CRC 破坏 */
    {
        uint8_t bad[TC_IMG_PREAMBLE_SIZE];
        memcpy(bad, bytes, sizeof(bytes));
        bad[20] ^= 0xFF;
        topos_image_preamble q;
        MT_CHECK_EQ_I64(tci_unpack_preamble(bad, &q), TC_IMG_ERR_BAD_PREAMBLE);
    }
    /* 未知 flags 位（pack 产出合法 CRC，unpack 按字段规则拒绝） */
    {
        topos_image_preamble flagbad = p;
        flagbad.flags = 0x80u;
        uint8_t bad[TC_IMG_PREAMBLE_SIZE];
        MT_CHECK_EQ_I64(tci_pack_preamble(&flagbad, bad), TC_OK);
        topos_image_preamble q;
        MT_CHECK_EQ_I64(tci_unpack_preamble(bad, &q), TC_IMG_ERR_BAD_PREAMBLE);
    }
    /* file_version_major=2 → UNSUPPORTED_VERSION（spec §3 版本策略；
     * 经 pack 产出合法 CRC，避免先命中 CRC 检查） */
    {
        topos_image_preamble v2 = p;
        v2.file_version_major = 2u;
        uint8_t vb[TC_IMG_PREAMBLE_SIZE];
        MT_CHECK_EQ_I64(tci_pack_preamble(&v2, vb), TC_OK);
        topos_image_preamble q;
        MT_CHECK_EQ_I64(tci_unpack_preamble(vb, &q), TC_ERR_UNSUPPORTED_VERSION);
    }
    /* spec §14：minor 向后兼容演进，读端必须容忍（v1.1 文件可读） */
    {
        topos_image_preamble next = p;
        next.file_version_minor = 1u;
        uint8_t nb[TC_IMG_PREAMBLE_SIZE];
        MT_CHECK_EQ_I64(tci_pack_preamble(&next, nb), TC_OK);
        topos_image_preamble q;
        MT_CHECK_EQ_I64(tci_unpack_preamble(nb, &q), TC_OK);
    }
    /* minor=0 时 compatibility_flags 未定义位必须为 0（spec §9.3） */
    {
        topos_image_preamble cbit = p;
        cbit.compatibility_flags = 0x4u; /* bit2 未定义 */
        uint8_t cb[TC_IMG_PREAMBLE_SIZE];
        MT_CHECK_EQ_I64(tci_pack_preamble(&cbit, cb), TC_OK);
        topos_image_preamble q;
        MT_CHECK_EQ_I64(tci_unpack_preamble(cb, &q), TC_IMG_ERR_BAD_PREAMBLE);
    }
    /* reserved0 非零 */
    {
        uint8_t bad[TC_IMG_PREAMBLE_SIZE];
        memcpy(bad, bytes, sizeof(bytes));
        bad[56] = 1;
        topos_image_preamble q;
        MT_CHECK_EQ_I64(tci_unpack_preamble(bad, &q), TC_IMG_ERR_BAD_PREAMBLE);
    }
    /* count 超上限 */
    {
        topos_image_preamble big = p;
        big.directory_count = TC_IMG_MAX_CHUNKS + 1u;
        uint8_t big_bytes[TC_IMG_PREAMBLE_SIZE];
        MT_CHECK_EQ_I64(tci_pack_preamble(&big, big_bytes), TC_OK);
        topos_image_preamble q;
        MT_CHECK_EQ_I64(tci_unpack_preamble(big_bytes, &q), TC_ERR_LIMIT_EXCEEDED);
    }
}

static void test_dir_entry(void)
{
    topos_image_dir_entry e;
    memset(&e, 0, sizeof(e));
    e.chunk_type = TC_IMG_CHUNK_PIXL;
    e.chunk_flags = TC_IMG_CHUNK_FLAG_CRITICAL;
    e.chunk_offset = 0x1122334455667788ull;
    e.chunk_size = 999ull;
    e.chunk_crc32 = 0xCAFEBABEu;
    uint8_t bytes[TC_IMG_DIR_ENTRY_SIZE];
    MT_CHECK_EQ_I64(tci_pack_dir_entry(&e, bytes), TC_OK);
    topos_image_dir_entry q;
    MT_CHECK_EQ_I64(tci_unpack_dir_entry(bytes, &q), TC_OK);
    MT_CHECK_EQ_U64(q.chunk_type, e.chunk_type);
    MT_CHECK_EQ_U64(q.chunk_offset, e.chunk_offset);
    MT_CHECK_EQ_U64(q.chunk_size, e.chunk_size);
    MT_CHECK_EQ_U64(q.chunk_crc32, e.chunk_crc32);

    /* size=0 拒绝 */
    {
        e.chunk_size = 0;
        uint8_t bad[TC_IMG_DIR_ENTRY_SIZE];
        tci_pack_dir_entry(&e, bad);
        MT_CHECK_EQ_I64(tci_unpack_dir_entry(bad, &q), TC_IMG_ERR_BAD_DIRECTORY);
    }
    /* critical|optional 同时置位拒绝 */
    {
        e.chunk_size = 10;
        e.chunk_flags = TC_IMG_CHUNK_FLAG_CRITICAL | TC_IMG_CHUNK_FLAG_OPTIONAL;
        uint8_t bad[TC_IMG_DIR_ENTRY_SIZE];
        tci_pack_dir_entry(&e, bad);
        MT_CHECK_EQ_I64(tci_unpack_dir_entry(bad, &q), TC_IMG_ERR_BAD_DIRECTORY);
    }
    /* 未定义 flag 位拒绝 */
    {
        e.chunk_flags = 0x10u;
        uint8_t bad[TC_IMG_DIR_ENTRY_SIZE];
        tci_pack_dir_entry(&e, bad);
        MT_CHECK_EQ_I64(tci_unpack_dir_entry(bad, &q), TC_IMG_ERR_BAD_DIRECTORY);
    }
}

static void test_idsc_roundtrip_and_rules(void)
{
    uint8_t* file = NULL;
    size_t file_size = 0;
    const packet_synth_cfg* cfg = packet_synth_cfg_at(PACKET_SYNTH_CFG_ALPHA);
    MT_CHECK_EQ_I64(image_file_synth_build(cfg, TC_IMG_PROFILE_PREVIEW, NULL, 0,
                                           &file, &file_size), TC_OK);
    MT_CHECK(file != NULL);

    /* 从文件提取 IDSC 字节做 round-trip */
    uint8_t idsc_bytes[TC_IMG_IDSC_SIZE];
    memcpy(idsc_bytes, file + TC_IMG_PREAMBLE_SIZE, TC_IMG_IDSC_SIZE);
    topos_image_idsc s;
    MT_CHECK_EQ_I64(tci_unpack_idsc(idsc_bytes, sizeof(idsc_bytes), &s), TC_OK);
    uint8_t repacked[TC_IMG_IDSC_SIZE];
    MT_CHECK_EQ_I64(tci_pack_idsc(&s, repacked), TC_OK);
    MT_CHECK(memcmp(idsc_bytes, repacked, TC_IMG_IDSC_SIZE) == 0);

    /* 逐字段破坏 → 拒绝（抽查代表性规则） */
    const struct { size_t off; uint8_t val; int32_t want; } cases[] = {
        {12, 0x02, TC_IMG_ERR_BAD_DIRECTORY},      /* flags 未定义位 */
        {51, 0, TC_IMG_ERR_BAD_DIRECTORY},         /* orientation=0（LSB） */
        {55, 0, TC_IMG_ERR_BAD_DIRECTORY},         /* aspect_num=0（LSB） */
        {61, 5, TC_IMG_ERR_BAD_DIRECTORY},         /* channel_count */
        /* HALF（2026-09-19 激活，spec §15）：kind=1 本身合法，但在 UINT
         * 组合（image_profile≠4）上 → CHUNK_CONFLICT（不再是 reserved 拒绝；
         * HALF 档正例见 test_half_map） */
        {62, 1, TC_IMG_ERR_CHUNK_CONFLICT},
        {63, 11, TC_IMG_ERR_BAD_DIRECTORY},        /* valid_bit_depth */
        {65, 2, TC_ERR_UNSUPPORTED_VERSION},       /* storage_layout reserved */
        {66, 3, TC_IMG_ERR_BAD_DIRECTORY},         /* subsampling reserved */
        {68, 3, TC_IMG_ERR_BAD_DIRECTORY},         /* alpha_presence reserved */
        {73, 9, TC_ERR_UNSUPPORTED_VERSION},       /* payload_major unknown */
        {72, 1, TC_ERR_UNSUPPORTED_VERSION},       /* codec_id reserved */
        /* image_profile=4 已激活为 HALF_FLOAT：UINT 流携带 → CHUNK_CONFLICT
         *（保留拒绝从 >4 起，test_half_map 覆盖 profile=5 负例） */
        {75, 4, TC_IMG_ERR_CHUNK_CONFLICT},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t bad[TC_IMG_IDSC_SIZE];
        memcpy(bad, idsc_bytes, sizeof(bad));
        bad[cases[i].off] = cases[i].val;
        topos_image_idsc q;
        const int32_t rc = tci_unpack_idsc(bad, sizeof(bad), &q);
        if (rc != cases[i].want) {
            fprintf(stderr, "case %zu: off=%zu want=%d got=%d\n",
                    i, cases[i].off, cases[i].want, (int)rc);
            MT_CHECK(0);
        }
    }
    /* V4–V8 are valid inner TPIC revisions and must remain representable in
     * TPIM IDSC even though the payload bytes are checked separately.
     * （V7=rANS 族 / V8=段化多链；spec §14 内层版本独立演进） */
    {
        topos_image_idsc v;
        MT_CHECK_EQ_I64(tci_unpack_idsc(idsc_bytes, sizeof(idsc_bytes), &v), TC_OK);
        v.payload_major = 6u;
        MT_CHECK_EQ_I64(tci_idsc_validate(&v), TC_OK);
        v.payload_major = 7u;
        MT_CHECK_EQ_I64(tci_idsc_validate(&v), TC_OK);
        v.payload_major = 8u;
        MT_CHECK_EQ_I64(tci_idsc_validate(&v), TC_OK);
    }
    free(file);
}

static void test_idsc_crosscheck(void)
{
    /* 合法合成文件 → 篡改 IDSC 的绑定摘要/尺寸/色彩标签 → CHUNK_CONFLICT 稳定复现 */
    uint8_t* file = NULL;
    size_t file_size = 0;
    const packet_synth_cfg* cfg = packet_synth_cfg_at(PACKET_SYNTH_CFG_MINIMAL);
    MT_CHECK_EQ_I64(image_file_synth_build(cfg, TC_IMG_PROFILE_PREVIEW, NULL, 0,
                                           &file, &file_size), TC_OK);

    uint8_t idsc_bytes[TC_IMG_IDSC_SIZE];
    memcpy(idsc_bytes, file + TC_IMG_PREAMBLE_SIZE, TC_IMG_IDSC_SIZE);
    const size_t pixl_off = TC_IMG_PREAMBLE_SIZE + TC_IMG_IDSC_SIZE;
    topos_image_idsc s;
    MT_CHECK_EQ_I64(tci_unpack_idsc(idsc_bytes, sizeof(idsc_bytes), &s), TC_OK);

    topos_frame_header fh;
    const uint8_t* fh_raw = file + pixl_off;
    MT_CHECK_EQ_I64(tc_frame_header_decode(fh_raw, TC_FRAME_HEADER_SIZE, &fh), TC_OK);
    MT_CHECK_EQ_I64(tci_idsc_crosscheck(&s, &fh, fh_raw), TC_OK);

    /* 绑定摘要 */
    {
        topos_image_idsc bad = s;
        bad.payload_header_crc32 ^= 1u;
        MT_CHECK_EQ_I64(tci_idsc_crosscheck(&bad, &fh, fh_raw), TC_IMG_ERR_CHUNK_CONFLICT);
    }
    /* data window 尺寸 */
    {
        topos_image_idsc bad = s;
        bad.data_x_max -= 1;
        MT_CHECK_EQ_I64(tci_idsc_crosscheck(&bad, &fh, fh_raw), TC_IMG_ERR_CHUNK_CONFLICT);
    }
    /* pixel_format */
    {
        topos_image_idsc bad = s;
        bad.pixel_format = (uint8_t)(s.pixel_format == 0u ? 1u : 0u);
        MT_CHECK_EQ_I64(tci_idsc_crosscheck(&bad, &fh, fh_raw), TC_IMG_ERR_CHUNK_CONFLICT);
    }
    /* 色彩标签 */
    {
        topos_image_idsc bad = s;
        bad.color_range = (uint8_t)(s.color_range == 1u ? 0u : 1u);
        MT_CHECK_EQ_I64(tci_idsc_crosscheck(&bad, &fh, fh_raw), TC_IMG_ERR_CHUNK_CONFLICT);
    }
    /* payload 版本 */
    {
        topos_image_idsc bad = s;
        bad.payload_minor = (uint8_t)(s.payload_minor + 1u);
        MT_CHECK_EQ_I64(tci_idsc_crosscheck(&bad, &fh, fh_raw), TC_IMG_ERR_CHUNK_CONFLICT);
    }
    free(file);
}

static void test_crosscheck_sar_unspecified_equivalence(void)
{
    /* 内层 0/0（未指定）≡ IDSC 1/1 */
    uint8_t* file = NULL;
    size_t file_size = 0;
    const packet_synth_cfg* cfg = packet_synth_cfg_at(PACKET_SYNTH_CFG_TINY);
    MT_CHECK_EQ_I64(image_file_synth_build(cfg, TC_IMG_PROFILE_PREVIEW, NULL, 0,
                                           &file, &file_size), TC_OK);
    const size_t pixl_off2 = TC_IMG_PREAMBLE_SIZE + TC_IMG_IDSC_SIZE;
    topos_frame_header fh;
    const uint8_t* fh_raw = file + pixl_off2;
    MT_CHECK_EQ_I64(tc_frame_header_decode(fh_raw, TC_FRAME_HEADER_SIZE, &fh), TC_OK);
    MT_CHECK(fh.sar_num == 0u && fh.sar_den == 0u);
    topos_image_idsc s;
    memset(&s, 0, sizeof(s));
    MT_CHECK_EQ_I64(tci_unpack_idsc(file + TC_IMG_PREAMBLE_SIZE, TC_IMG_IDSC_SIZE, &s), TC_OK);
    MT_CHECK_EQ_U64(s.pixel_aspect_num, 1u);
    MT_CHECK_EQ_U64(s.pixel_aspect_den, 1u);
    MT_CHECK_EQ_I64(tci_idsc_crosscheck(&s, &fh, fh_raw), TC_OK);
    free(file);
}

int main(void)
{
    test_preamble_roundtrip();
    test_preamble_negative();
    test_dir_entry();
    test_idsc_roundtrip_and_rules();
    test_idsc_crosscheck();
    test_crosscheck_sar_unspecified_equivalence();
    return MT_MAIN_RETURN();
}
