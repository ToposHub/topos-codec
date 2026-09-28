/* 位流 fuzz corpus 生成 + 自检门（确定性）。
 *
 * 用法：
 *   gen_bitstream_corpus -dump <dir>     写出种子与变异样本（提交入库的 corpus）
 *   gen_bitstream_corpus -selftest [dir] 内存重建全部样本与变异并回放 fuzz 入口；
 *                                        另行回放 dir（若提供）下的 corpus 文件。
 *                                        违反不变量 → 非零退出（ctest 门禁）。
 *
 * 变异类（覆盖计划阶段 3 的四类）：截断（1/16 边界）、位翻转（LCG 定位）、
 * 伪造长度（frame_packet_size / slice_payload_size，含 CRC 修复变体）、
 * 超长码字（全 1 payload / 大 k 逃逸风暴）。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

#include "../../src/bitstream/frame_header.h"
#include "../../src/bitstream/packet.h"
#include "../../src/common/crc32.h"
#include "packet_synth.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static uint64_t lcg_state = 0x853C49E6748FEA9Bull;
static uint64_t lcg(void)
{
    lcg_state = lcg_state * 6364136223846793005ull + 1442695040888963407ull;
    return lcg_state >> 33;
}

static int g_failures = 0;

static void feed(const char* tag, const uint8_t* data, size_t size, int expect_ok_path)
{
    if (LLVMFuzzerTestOneInput(data, size) != 0) {
        fprintf(stderr, "INVARIANT VIOLATION (%s)\n", tag);
        g_failures++;
        return;
    }
    topos_packet_view view;
    int32_t rc = tc_packet_scan(data, size, &view);
    if (expect_ok_path) {
        /* 合法种子：必须完整通过（scan + 全部 CRC + 全部片符号解码） */
        if (rc != TC_OK) {
            fprintf(stderr, "SEED REJECTED (%s): %d %s\n", tag, rc, tc_last_error());
            g_failures++;
            return;
        }
        for (uint16_t i = 0; i < view.slice_count; ++i) {
            if (view.slice_crc_ok[i] != 1u) {
                fprintf(stderr, "SEED CRC BAD (%s slice %u)\n", tag, (unsigned)i);
                g_failures++;
            }
        }
    }
    (void)rc;
}

static void write_file(const char* dir, const char* name, const uint8_t* data, size_t size)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "wb");
    if (f == NULL) { fprintf(stderr, "cannot write %s\n", path); g_failures++; return; }
    fwrite(data, 1, size, f);
    fclose(f);
}

static void derive_and_feed(uint8_t* buf, size_t n, int dump, const char* dir,
                            const char* tag, const char* name, int expect_ok)
{
    if (dump && name != NULL) { write_file(dir, name, buf, n); }
    feed(tag, buf, n, expect_ok);
}

int main(int argc, char** argv)
{
    int dump = 0;
    const char* dir = NULL;
    if (argc >= 3 && strcmp(argv[1], "-dump") == 0) {
        dump = 1;
        dir = argv[2];
        mkdir(dir, 0755);
    } else if (argc >= 2 && strcmp(argv[1], "-selftest") == 0) {
        if (argc >= 3) { dir = argv[2]; }
    } else {
        fprintf(stderr, "usage: %s -dump <dir> | -selftest [corpusdir]\n", argv[0]);
        return 2;
    }

    /* 0) R4.1：12-bit 种子（v1.2 扩展枚举——独立构造，不进 kCfgs，
     * golden_bitstream 的 RECORD_COUNT 绑定 PACKET_SYNTH_CFG_COUNT 不受影响） */
    {
        packet_synth_cfg c12;
        memset(&c12, 0, sizeof(c12));
        c12.seed = 0xB12B12B12B12B12Bull;
        c12.visible_w = 48u;
        c12.visible_h = 32u;
        c12.with_alpha = 0u;
        c12.bands = 2u;
        c12.qp_base = 24u;
        c12.qmatrix_id = 1u;
        c12.bit_depth = 12u;
        uint8_t* pkt = NULL;
        size_t n = 0;
        int32_t rc = packet_synth_build(&c12, &pkt, &n);
        if (rc != TC_OK) {
            fprintf(stderr, "synth bd12 failed: %d\n", rc);
            g_failures++;
        } else {
            derive_and_feed(pkt, n, dump, dir, "seed_bd12", "seed_bd12.bin", 1);
            for (int k = 0; k <= 16; k += 4) {
                size_t cut = n * (size_t)k / 16u;
                if (cut >= n) { cut = n - 1u; }
                char tag[64], name[128];
                snprintf(tag, sizeof(tag), "trunc_bd12_%d", k);
                snprintf(name, sizeof(name), "trunc_bd12_%d.bin", k);
                derive_and_feed(pkt, cut, dump, dir, tag, name, 0);
            }
            for (int m = 0; m < 4; ++m) {
                uint8_t* buf = (uint8_t*)malloc(n);
                memcpy(buf, pkt, n);
                size_t pos = (size_t)(lcg() % (uint64_t)n);
                buf[pos] ^= (uint8_t)(1u << (lcg() % 8ull));
                char tag[64], name[128];
                snprintf(tag, sizeof(tag), "flip_bd12_%d", m);
                snprintf(name, sizeof(name), "flip_bd12_%d.bin", m);
                derive_and_feed(buf, n, dump, dir, tag, name, 0);
                free(buf);
            }
            free(pkt);
        }
    }

    /* 0v) M9：V2 canonical VLC 种子（major=2/entropy=1/cbv=1——独立构造
     * 不进 kCfgs，golden_bitstream 的 RECORD_COUNT 绑定不受影响；
     * spec v2 §5：fuzz 覆盖无效前缀/截断/类别极值/EOB 缺失/非零 pad） */
    {
        for (int vi = 0; vi < 2; ++vi) {
            packet_synth_cfg cv2;
            memset(&cv2, 0, sizeof(cv2));
            cv2.seed = 0xC0DEC0DEC0DE0000ull + (uint64_t)vi;
            cv2.visible_w = vi == 0 ? 48u : 36u;
            cv2.visible_h = 32u;
            cv2.with_alpha = (uint8_t)vi; /* 第二个带 alpha（alpha 恒 Rice） */
            cv2.bands = 2u;
            cv2.qp_base = 24u;
            cv2.qmatrix_id = 1u;
            cv2.bit_depth = vi == 0 ? 0u : 12u;
            cv2.entropy_mode = 1u;
            uint8_t* pkt = NULL;
            size_t n = 0;
            int32_t rc = packet_synth_build(&cv2, &pkt, &n);
            if (rc != TC_OK) {
                fprintf(stderr, "synth vlc%d failed: %d\n", vi, rc);
                g_failures++;
            } else {
                char seed_tag[32], seed_name[48];
                snprintf(seed_tag, sizeof(seed_tag), "seed_vlc%d", vi);
                snprintf(seed_name, sizeof(seed_name), "seed_vlc%d.bin", vi);
                derive_and_feed(pkt, n, dump, dir, seed_tag, seed_name, 1);
                for (int k = 0; k <= 16; k += 2) { /* 逐 1/8 密度截断 */
                    size_t cut = n * (size_t)k / 16u;
                    if (cut >= n) { cut = n - 1u; }
                    char tag[64], name[128];
                    snprintf(tag, sizeof(tag), "trunc_vlc%d_%d", vi, k);
                    snprintf(name, sizeof(name), "trunc_vlc%d_%d.bin", vi, k);
                    derive_and_feed(pkt, cut, dump, dir, tag, name, 0);
                }
                for (int m = 0; m < 6; ++m) {
                    uint8_t* buf = (uint8_t*)malloc(n);
                    memcpy(buf, pkt, n);
                    size_t pos = (size_t)(lcg() % (uint64_t)n);
                    buf[pos] ^= (uint8_t)(1u << (lcg() % 8ull));
                    char tag[64], name[128];
                    snprintf(tag, sizeof(tag), "flip_vlc%d_%d", vi, m);
                    snprintf(name, sizeof(name), "flip_vlc%d_%d.bin", vi, m);
                    derive_and_feed(buf, n, dump, dir, tag, name, 0);
                    free(buf);
                }
                free(pkt);
            }
        }
    }

    /* 0b) R4.2：4:4:4 种子（v1.3 扩展枚举，pf=1/minor=2——同 bd12 策略，
     * 独立构造不进 kCfgs，golden_bitstream 的 RECORD_COUNT 绑定不受影响） */
    {
        packet_synth_cfg c444;
        memset(&c444, 0, sizeof(c444));
        c444.seed = 0x4C4C4C4C3B3B3B3Bull;
        c444.visible_w = 48u;
        c444.visible_h = 32u;
        c444.with_alpha = 1u;
        c444.bands = 2u;
        c444.qp_base = 20u;
        c444.qmatrix_id = 1u;
        c444.bit_depth = 0u;
        c444.pixel_format = 1u;
        uint8_t* pkt = NULL;
        size_t n = 0;
        int32_t rc = packet_synth_build(&c444, &pkt, &n);
        if (rc != TC_OK) {
            fprintf(stderr, "synth pf444 failed: %d\n", rc);
            g_failures++;
        } else {
            derive_and_feed(pkt, n, dump, dir, "seed_pf444", "seed_pf444.bin", 1);
            for (int k = 0; k <= 16; k += 4) {
                size_t cut = n * (size_t)k / 16u;
                if (cut >= n) { cut = n - 1u; }
                char tag[64], name[128];
                snprintf(tag, sizeof(tag), "trunc_pf444_%d", k);
                snprintf(name, sizeof(name), "trunc_pf444_%d.bin", k);
                derive_and_feed(pkt, cut, dump, dir, tag, name, 0);
            }
            for (int m = 0; m < 4; ++m) {
                uint8_t* buf = (uint8_t*)malloc(n);
                memcpy(buf, pkt, n);
                size_t pos = (size_t)(lcg() % (uint64_t)n);
                buf[pos] ^= (uint8_t)(1u << (lcg() % 8ull));
                char tag[64], name[128];
                snprintf(tag, sizeof(tag), "flip_pf444_%d", m);
                snprintf(name, sizeof(name), "flip_pf444_%d.bin", m);
                derive_and_feed(buf, n, dump, dir, tag, name, 0);
                free(buf);
            }
            free(pkt);
        }
    }

    /* 0c) R4.3：GBR 种子（v1.4 扩展枚举，pf=2/minor=3/matrix=0——
     * 同前策略独立构造不进 kCfgs） */
    {
        packet_synth_cfg cgbr;
        memset(&cgbr, 0, sizeof(cgbr));
        cgbr.seed = 0x6B626B6272727272ull;
        cgbr.visible_w = 48u;
        cgbr.visible_h = 32u;
        cgbr.with_alpha = 1u;
        cgbr.bands = 2u;
        cgbr.qp_base = 20u;
        cgbr.qmatrix_id = 1u;
        cgbr.bit_depth = 0u;
        cgbr.pixel_format = 2u;
        uint8_t* pkt = NULL;
        size_t n = 0;
        int32_t rc = packet_synth_build(&cgbr, &pkt, &n);
        if (rc != TC_OK) {
            fprintf(stderr, "synth gbr failed: %d\n", rc);
            g_failures++;
        } else {
            derive_and_feed(pkt, n, dump, dir, "seed_gbr", "seed_gbr.bin", 1);
            for (int k = 0; k <= 16; k += 4) {
                size_t cut = n * (size_t)k / 16u;
                if (cut >= n) { cut = n - 1u; }
                char tag[64], name[128];
                snprintf(tag, sizeof(tag), "trunc_gbr_%d", k);
                snprintf(name, sizeof(name), "trunc_gbr_%d.bin", k);
                derive_and_feed(pkt, cut, dump, dir, tag, name, 0);
            }
            for (int m = 0; m < 4; ++m) {
                uint8_t* buf = (uint8_t*)malloc(n);
                memcpy(buf, pkt, n);
                size_t pos = (size_t)(lcg() % (uint64_t)n);
                buf[pos] ^= (uint8_t)(1u << (lcg() % 8ull));
                char tag[64], name[128];
                snprintf(tag, sizeof(tag), "flip_gbr_%d", m);
                snprintf(name, sizeof(name), "flip_gbr_%d.bin", m);
                derive_and_feed(buf, n, dump, dir, tag, name, 0);
                free(buf);
            }
            free(pkt);
        }
    }

    /* 0d) R4.4：Pro444/Extreme 种子（profile 5/6——同前策略独立构造） */
    {
        static const struct { uint64_t seed; const char* tag; uint8_t profile;
                              uint8_t pf; uint8_t bd; } pk[2] = {
            { 0x50524F3434342121ull, "pro444", 5u, 1u, 0u },
            { 0x585452454D453131ull, "extreme", 6u, 2u, 12u },
        };
        for (int i = 0; i < 2; ++i) {
            packet_synth_cfg c;
            memset(&c, 0, sizeof(c));
            c.seed = pk[i].seed;
            c.visible_w = 48u;
            c.visible_h = 32u;
            c.with_alpha = 0u;
            c.bands = 2u;
            c.qp_base = 22u;
            c.qmatrix_id = 1u;
            c.bit_depth = pk[i].bd;
            c.pixel_format = pk[i].pf;
            c.profile = pk[i].profile;
            uint8_t* pkt = NULL;
            size_t n = 0;
            int32_t rc = packet_synth_build(&c, &pkt, &n);
            if (rc != TC_OK) {
                fprintf(stderr, "synth %s failed: %d\n", pk[i].tag, rc);
                g_failures++;
                continue;
            }
            {
                char seed_name[128];
                snprintf(seed_name, sizeof(seed_name), "seed_%s.bin", pk[i].tag);
                derive_and_feed(pkt, n, dump, dir, pk[i].tag, seed_name, 1);
            }
            for (int k = 0; k <= 16; k += 4) {
                size_t cut = n * (size_t)k / 16u;
                if (cut >= n) { cut = n - 1u; }
                char tag[64], name[128];
                snprintf(tag, sizeof(tag), "trunc_%s_%d", pk[i].tag, k);
                snprintf(name, sizeof(name), "trunc_%s_%d.bin", pk[i].tag, k);
                derive_and_feed(pkt, cut, dump, dir, tag, name, 0);
            }
            for (int m = 0; m < 4; ++m) {
                uint8_t* buf = (uint8_t*)malloc(n);
                memcpy(buf, pkt, n);
                size_t pos = (size_t)(lcg() % (uint64_t)n);
                buf[pos] ^= (uint8_t)(1u << (lcg() % 8ull));
                char tag[64], name[128];
                snprintf(tag, sizeof(tag), "flip_%s_%d", pk[i].tag, m);
                snprintf(name, sizeof(name), "flip_%s_%d.bin", pk[i].tag, m);
                derive_and_feed(buf, n, dump, dir, tag, name, 0);
                free(buf);
            }
            free(pkt);
        }
    }

    /* 1) 合法种子（全部标准配置）+ 四类变异 */
    for (unsigned c = 0u; c < PACKET_SYNTH_CFG_COUNT; ++c) {
        uint8_t* pkt = NULL;
        size_t n = 0;
        int32_t rc = packet_synth_build(packet_synth_cfg_at(c), &pkt, &n);
        if (rc != TC_OK) {
            fprintf(stderr, "synth cfg %u failed: %d\n", c, rc);
            g_failures++;
            continue;
        }
        char tag[64];
        char name[128];
        snprintf(tag, sizeof(tag), "seed_cfg%u", c);
        snprintf(name, sizeof(name), "seed_cfg%u.bin", c);
        derive_and_feed(pkt, n, dump, dir, tag, name, 1);

        /* 截断：1/16 边界（含 0 与 n−1） */
        for (int k = 0; k <= 16; ++k) {
            size_t cut = n * (size_t)k / 16u;
            if (cut >= n) { cut = n - 1u; }
            snprintf(tag, sizeof(tag), "trunc_cfg%u_%d", c, k);
            snprintf(name, sizeof(name), "trunc_cfg%u_%d.bin", c, k);
            derive_and_feed(pkt, cut, dump, dir, tag, name, 0);
        }

        /* 位翻转：8 个 LCG 定点（帧头区与 payload 区混合） */
        for (int m = 0; m < 8; ++m) {
            uint8_t* buf = (uint8_t*)malloc(n);
            memcpy(buf, pkt, n);
            size_t pos = (size_t)(lcg() % (uint64_t)n);
            buf[pos] ^= (uint8_t)(1u << (lcg() % 8ull));
            snprintf(tag, sizeof(tag), "flip_cfg%u_%d", c, m);
            snprintf(name, sizeof(name), "flip_cfg%u_%d.bin", c, m);
            derive_and_feed(buf, n, dump, dir, tag, name, 0);
            free(buf);
        }

        /* 伪造 frame_packet_size：+1（CRC 修复）与 0xFFFFFFFF（原始） */
        {
            uint8_t* buf = (uint8_t*)malloc(n);
            memcpy(buf, pkt, n);
            uint32_t v = (uint32_t)n + 1u;
            uint8_t be[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8),
                              (uint8_t)v };
            memcpy(buf + 41, be, 4);
            uint32_t crc = tc_crc32(buf, 49u);
            buf[49] = (uint8_t)(crc >> 24); buf[50] = (uint8_t)(crc >> 16);
            buf[51] = (uint8_t)(crc >> 8); buf[52] = (uint8_t)crc;
            derive_and_feed(buf, n, dump, dir, "forge_fps_plus1", NULL, 0);
            memcpy(buf, pkt, n);
            memset(buf + 41, 0xFF, 4);
            snprintf(name, sizeof(name), "forge_fps_cfg%u_max.bin", c);
            derive_and_feed(buf, n, dump, dir, "forge_fps_max", name, 0);
            free(buf);
        }

        /* 伪造 slice payload_size：首片（offset 53）置 0 / 0xFFFFFFFF */
        {
            uint8_t* buf = (uint8_t*)malloc(n);
            memcpy(buf, pkt, n);
            memset(buf + TC_FRAME_HEADER_SIZE, 0xFF, 4);
            snprintf(name, sizeof(name), "forge_sps_cfg%u_max.bin", c);
            derive_and_feed(buf, n, dump, dir, "forge_sps_max", name, 0);
            memcpy(buf, pkt, n);
            memset(buf + TC_FRAME_HEADER_SIZE, 0, 4);
            derive_and_feed(buf, n, dump, dir, "forge_sps_zero", NULL, 0);
            free(buf);
        }

        /* 超长码字：首片 payload 全 1（31×N unary + 逃逸字面值风暴）*/
        {
            topos_packet_view probe;
            if (tc_packet_scan(pkt, n, &probe) == TC_OK && probe.slice_count > 0u) {
                size_t p0 = TC_FRAME_HEADER_SIZE + TC_SLICE_HEADER_SIZE;
                uint8_t* buf = (uint8_t*)malloc(n);
                memcpy(buf, pkt, n);
                memset(buf + p0, 0xFF, probe.slices[0].slice_payload_size);
                snprintf(name, sizeof(name), "overlong_cfg%u.bin", c);
                derive_and_feed(buf, n, dump, dir, "overlong_unary", name, 0);
                free(buf);
            }
        }

        free(pkt);
    }

    /* 2) 纯随机短输入（浅路径轰炸：magic/版本/尺寸字段） */
    for (int i = 0; i < 256; ++i) {
        uint8_t buf[64];
        for (size_t j = 0; j < sizeof(buf); ++j) { buf[j] = (uint8_t)lcg(); }
        feed("random64", buf, sizeof(buf), 0);
    }

    /* 3) 回放 corpus 目录（提供时） */
    if (dir != NULL && !dump) {
        for (unsigned c = 0u; c < PACKET_SYNTH_CFG_COUNT; ++c) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/seed_cfg%u.bin", dir, c);
            FILE* f = fopen(path, "rb");
            if (f == NULL) { continue; }
            uint8_t* buf = (uint8_t*)malloc(1u << 20);
            size_t got = fread(buf, 1, 1u << 20, f);
            fclose(f);
            feed("replay", buf, got, 0);
            free(buf);
        }
    }

    if (g_failures != 0) {
        fprintf(stderr, "bitstream corpus selftest: %d failures\n", g_failures);
        return 1;
    }
    printf("bitstream corpus selftest: OK\n");
    return 0;
}
