/* golden_image —— 阶段 1 conformance：.toos golden 文件（字节级冻结）。
 *
 * golden 集合（docs/image/test_vector_manifest.json）：
 *   golden_image_v1.bin        Preview：64x48 YUV422 10-bit 无 Alpha（IDSC+PIXL 最小目录）
 *   golden_image_v1_alpha.bin  Preview：64x32 + Alpha mode1 (A16) straight
 *
 * gen   = 确定性重建并写出（有意变更时手动执行）；
 * check = 重建并与入库 golden 逐字节比对 —— writer/布局/IDSC 任何无声漂移在此失败
 *         （spec §2.11 确定性；同 golden_codec/golden_mov 约定）。
 *
 * 用法：golden_image gen|check <dir>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../support/image_file_synth.h"
#include "../support/packet_synth.h"
#include "topos_codec.h"
#include "topos_image.h"

typedef struct golden_case {
    const char* name;
    packet_synth_cfg pcfg;
    uint8_t image_profile;
    const topos_image_chunk_in* extras;
    uint32_t extra_count;
    uint8_t iccp_ref;
    uint8_t ocio_ref;
} golden_case;

/* —— 阶段 3 metadata 载荷（确定性；受限校验须全部通过） —— */

/* 最小合法 ICC：132B 头，offset36 'acsp'，colorSpace 'RGB ' */
static uint8_t k_iccp[132] = {
    0, 0, 0, 132,                                    /* 0: size */
    0, 0, 0, 0,                                      /* 4: CMM */
    0, 4, 48, 0,                                     /* 8: version 4.3 */
    'm', 'n', 't', 'r',                              /* 12: device class */
    'R', 'G', 'B', ' ',                              /* 16: colorSpace */
    'X', 'Y', 'Z', ' ',                              /* 20: PCS */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,              /* 24: date */
    'a', 'c', 's', 'p',                              /* 36: signature */
};
static const char k_ocio[] = "ACES - ACEScg";
static const char k_xmp[] = "<?xpacket begin=?><x:xmpmeta>topos-image-golden</x:xmpmeta><?xpacket end?>";
static const uint8_t k_exif[16] = {'I', 'I', 42, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static uint8_t k_hash[36] = {1, 0, 0, 0}; /* algo=1 (SHA-256) + 摘要占位 */
static uint8_t* k_thmb = NULL;           /* 8x8 TPIC packet（确定性合成） */
static size_t k_thmb_size = 0;

static int build_meta_payloads(void)
{
    if (k_thmb != NULL) { return 0; }
    /* HASH 摘要确定性填充 */
    uint64_t st = 0x600D5EED5EEDull;
    for (int i = 4; i < 36; ++i) {
        st ^= st << 13; st ^= st >> 7; st ^= st << 17;
        k_hash[i] = (uint8_t)(st >> 33);
    }
    const int32_t rc = packet_synth_build(packet_synth_cfg_at(PACKET_SYNTH_CFG_MINIMAL),
                                          &k_thmb, &k_thmb_size);
    return rc == TC_OK ? 0 : 1;
}

#define k_meta_count 6
static topos_image_chunk_in k_meta[k_meta_count];

static void init_meta_chunks(void)
{
    k_meta[0].chunk_type = TC_IMG_CHUNK_ICCP;
    k_meta[0].chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL | TC_IMG_CHUNK_FLAG_PRESERVE;
    k_meta[0].data = k_iccp;  k_meta[0].size = sizeof(k_iccp);
    k_meta[1].chunk_type = TC_IMG_CHUNK_OCIO;
    k_meta[1].chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL | TC_IMG_CHUNK_FLAG_PRESERVE;
    k_meta[1].data = k_ocio;  k_meta[1].size = sizeof(k_ocio);
    k_meta[2].chunk_type = TC_IMG_CHUNK_XMP;
    k_meta[2].chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL | TC_IMG_CHUNK_FLAG_PRESERVE;
    k_meta[2].data = k_xmp;   k_meta[2].size = sizeof(k_xmp);
    k_meta[3].chunk_type = TC_IMG_CHUNK_EXIF;
    k_meta[3].chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL | TC_IMG_CHUNK_FLAG_PRESERVE;
    k_meta[3].data = k_exif;  k_meta[3].size = sizeof(k_exif);
    k_meta[4].chunk_type = TC_IMG_CHUNK_THMB;
    k_meta[4].chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL | TC_IMG_CHUNK_FLAG_DROPPABLE;
    k_meta[4].data = k_thmb;  k_meta[4].size = k_thmb_size;
    k_meta[5].chunk_type = TC_IMG_CHUNK_HASH;
    k_meta[5].chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL | TC_IMG_CHUNK_FLAG_PRESERVE;
    k_meta[5].data = k_hash;  k_meta[5].size = sizeof(k_hash);
}

/* kCfgs 之外的自定义确定性配置（64x48 无 Alpha 双带）；
 * 第二例 = kCfgs[PACKET_SYNTH_CFG_ALPHA] 的字面拷贝（保持字节稳定）；
 * 第三例 = 阶段 2 Image HQ：GBR 4:4:4 10-bit（pf=2/profile=5/minor=3，matrix=0）；
 * 第四例 = 阶段 3 Image XQ：GBR 4:4:4 12-bit；
 * 第五例 = 阶段 3 metadata：HQ + ICCP/OCIO/XMP/EXIF/THMB/HASH 全可选 chunk */
static const golden_case k_cases[] = {
    {"golden_image_v1.bin",
     {0x53504543ull, 64u, 48u, 0u, 2u, 28u, 1u, 0u, 0u, 3u, 0u},
     TC_IMG_PROFILE_PREVIEW, NULL, 0, 0u, 0u},
    {"golden_image_v1_alpha.bin",
     {0xFEDCBA9876543210ull, 64u, 32u, 1u, 3u, 0u, 0u, 0u, 0u, 0u, 0u},
     TC_IMG_PROFILE_PREVIEW, NULL, 0, 0u, 0u},
    {"golden_image_v1_gbr10.bin",
     {0x0BADC0DE0BADC0DEull, 64u, 48u, 0u, 2u, 24u, 1u, 10u, 2u, 5u, 0u},
     TC_IMG_PROFILE_HQ, NULL, 0, 0u, 0u},
    {"golden_image_v1_gbr12.bin",
     {0xD0D0C0DED0D0C0DEull, 64u, 48u, 0u, 2u, 18u, 1u, 12u, 2u, 5u, 0u},
     TC_IMG_PROFILE_XQ, NULL, 0, 0u, 0u},
    {"golden_image_v1_meta.bin",
     {0x0BADC0DE0BADC0DEull, 64u, 48u, 0u, 2u, 24u, 1u, 10u, 2u, 5u, 0u},
     TC_IMG_PROFILE_HQ, k_meta, k_meta_count, 1u, 1u},
    /* 第六例 = v1.6（TRAW 批 1）：Image RAW——CFA 4 相位平面 12-bit
     * （pf=3/profile=7/minor=5，qm0/transfer=LOG0/matrix=0） */
    {"golden_image_v1_traw.bin",
     {0x7E2A0CFA7E2A0CFAull, 64u, 48u, 0u, 2u, 24u, 0u, 12u, 3u, 7u, 0u},
     TC_IMG_PROFILE_RAW, NULL, 0, 0u, 0u},
};
#define k_case_count (sizeof(k_cases) / sizeof(k_cases[0]))

static int build_case(const golden_case* c, uint8_t** out, size_t* out_size)
{
    return image_file_synth_build_ex(&c->pcfg, c->image_profile,
                                     c->extras, c->extra_count,
                                     c->iccp_ref, c->ocio_ref,
                                     out, out_size);
}

static int check_case(const golden_case* c, const char* dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, c->name);
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "golden_image: missing %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    const long file_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_len <= 0) { fclose(f); return 1; }
    uint8_t* stored = (uint8_t*)malloc((size_t)file_len);
    if (stored == NULL) { fclose(f); return 1; }
    const size_t got = fread(stored, 1, (size_t)file_len, f);
    fclose(f);
    if (got != (size_t)file_len) { free(stored); return 1; }

    uint8_t* rebuilt = NULL;
    size_t rebuilt_size = 0;
    const int32_t rc = build_case(c, &rebuilt, &rebuilt_size);
    if (rc != TC_OK) {
        fprintf(stderr, "golden_image: rebuild %s failed: %s\n", c->name,
                tc_image_status_message(rc));
        free(stored);
        return 1;
    }
    int ok = (rebuilt_size == (size_t)file_len) &&
             (memcmp(rebuilt, stored, rebuilt_size) == 0);
    if (!ok) {
        fprintf(stderr, "golden_image: %s BYTES DIFFER (stored %ld, rebuilt %zu)\n",
                c->name, file_len, rebuilt_size);
    } else {
        /* 语义复核：probe + validate + decode 查询必须通过 */
        image_mem_src src;
        topos_io io;
        image_mem_src_init(&src, stored, (size_t)file_len, &io);
        topos_image_info info;
        memset(&info, 0, sizeof(info));
        if (tc_image_probe(&io, &info) != TC_OK ||
            tc_image_validate(&io, TC_IMG_VALIDATE_DEEP, &info) != TC_OK) {
            fprintf(stderr, "golden_image: %s semantic check failed\n", c->name);
            ok = 0;
        }
    }
    free(rebuilt);
    free(stored);
    return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: golden_image gen|check <dir>\n");
        return 2;
    }
    const char* dir = argv[2];
    if (strcmp(argv[1], "gen") == 0) {
        if (build_meta_payloads() != 0) {
            fprintf(stderr, "meta payload build failed\n");
            return 1;
        }
        init_meta_chunks();
        for (size_t i = 0; i < k_case_count; ++i) {
            uint8_t* data = NULL;
            size_t size = 0;
            if (build_case(&k_cases[i], &data, &size) != TC_OK) {
                fprintf(stderr, "gen %s failed\n", k_cases[i].name);
                return 1;
            }
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", dir, k_cases[i].name);
            FILE* f = fopen(path, "wb");
            if (f == NULL) { free(data); return 1; }
            const size_t w = fwrite(data, 1, size, f);
            fclose(f);
            free(data);
            if (w != size) { return 1; }
            printf("gen %s (%zu bytes)\n", path, size);
        }
        return 0;
    }
    if (strcmp(argv[1], "check") == 0) {
        if (build_meta_payloads() != 0) {
            fprintf(stderr, "meta payload build failed\n");
            return 1;
        }
        init_meta_chunks();
        for (size_t i = 0; i < k_case_count; ++i) {
            if (check_case(&k_cases[i], dir) != 0) { return 1; }
        }
        printf("golden_image: OK (%zu cases)\n", k_case_count);
        return 0;
    }
    fprintf(stderr, "usage: golden_image gen|check <dir>\n");
    return 2;
}
