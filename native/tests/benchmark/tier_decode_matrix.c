/* topos_tier_decode_matrix —— 分辨率 × 档位 解码速率矩阵基准（2026-09-10）。
 *
 * 档位语义 = 产品定义（src/shared/codec/topos_profiles.py target_bpp）：
 * 同一档位跨分辨率 = 同目标 bpp（Mbps 随像素数缩放），不是同 qp。
 * 编码走 tc_frame_encode_sized 逐帧确定性 qp 搜索（0..95，v1.5 域），
 * qp_used 一并输出——Proxy 档在 ≥4K 上命中 qp>63 正是 ADR-C031 的动机。
 * 解码 = 整帧 tc_frame_decode（真实输出平面），逐迭代输出 CSV，p50 由
 * 上层脚本聚合；1t 与多线程各测一轮（先 1t 后多t，进程内线程池复用）。
 *
 * CSV 列：kind,w,h,tier,bpp,target_bytes,qp_used,packet_bytes,mbps,threads,iteration,decode_ms
 */
#include "image_synth.h"
#include "topos_codec.h"
#include "codec/codec.h"
#include "common/alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define REFERENCE_FPS 24u

typedef struct tier_def {
    const char* id;
    double bpp; /* topos_profiles.py target_bpp（P4 定标） */
} tier_def;

typedef struct res_case {
    uint32_t w, h;
} res_case;

static uint64_t now_ns(void)
{
#ifdef _WIN32
    /* QPC 单调时钟（MSVC CRT 无 clock_gettime/CLOCK_MONOTONIC） */
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); }
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000000ull) / (uint64_t)freq.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0u) { return 0u; }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
#endif
}

static double now_ms(void) { return (double)now_ns() / 1e6; }

static const char* kind_name(tc_synth_kind k)
{
    switch (k) {
    case TC_SYNTH_GRADIENT: return "gradient";
    case TC_SYNTH_GRAIN: return "grain";
    default: return "?";
    }
}

int main(int argc, char** argv)
{
    uint32_t threads_multi = 16u;
    uint32_t iters_small = 30u;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            threads_multi = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters_small = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "usage: %s [--threads N] [--iters N]\n", argv[0]);
            return 2;
        }
    }

    /* 主表 16:9（spec §1.1 产品阶梯：2K/4K/6K/8K） */
    static const res_case resolutions[] = {
        {1920u, 1080u}, {3840u, 2160u}, {5760u, 3240u}, {7680u, 4320u},
    };
    /* topos_profiles.py TOPOS_PROFILE_TIERS target_bpp（P4 质量钉扎定标） */
    static const tier_def tiers[] = {
        {"proxy", 0.60509},
        {"lt", 1.23239},
        {"standard", 1.87491},
        {"hq", 2.75617},
    };
    static const tc_synth_kind kinds[2] = {TC_SYNTH_GRADIENT, TC_SYNTH_GRAIN};

    printf("kind,w,h,tier,bpp,target_bytes,qp_used,packet_bytes,mbps,threads,iteration,decode_ms\n");

    for (size_t ri = 0u; ri < sizeof(resolutions) / sizeof(resolutions[0]); ++ri) {
        const uint32_t w = resolutions[ri].w;
        const uint32_t h = resolutions[ri].h;
        /* 迭代数按分辨率缩放（8K 单帧数十 ms，10 次足够取中位） */
        const uint32_t iters = iters_small * 1920u / (w > 1920u ? w : 1920u) + 5u;

        image_synth_cfg synth;
        memset(&synth, 0, sizeof(synth));
        synth.seed = UINT64_C(0x5052324B30383031) ^ ((uint64_t)w << 16) ^ h;
        synth.width = w;
        synth.height = h;
        synth.bit_depth = 10u;
        synth.chroma_format = 0u;

        for (size_t ki = 0u; ki < sizeof(kinds) / sizeof(kinds[0]); ++ki) {
            synth.kind = kinds[ki];
            uint16_t *y = NULL, *u = NULL, *v = NULL, *a = NULL;
            if (image_synth_alloc(&synth, 0, &y, &u, &v, &a) != 0) {
                fprintf(stderr, "synth OOM at %ux%u\n", w, h);
                return 1;
            }
            topos_frame_config cfg;
            memset(&cfg, 0, sizeof(cfg));
            cfg.struct_size = (uint32_t)sizeof(cfg);
            cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
            cfg.visible_width = (uint16_t)w;
            cfg.visible_height = (uint16_t)h;
            cfg.profile = 3u;
            cfg.pixel_format = 0u;
            cfg.bit_depth = 10u;
            cfg.qmatrix_id = 1u;
            cfg.qp_base = 24u; /* sized 搜索覆盖（0..95 含 v1.5 域） */

            topos_frame_input in;
            memset(&in, 0, sizeof(in));
            in.struct_size = (uint32_t)sizeof(in);
            in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
            in.planes[0] = y;
            in.planes[1] = u;
            in.planes[2] = v;
            in.strides[0] = w;
            in.strides[1] = ((size_t)w + 1u) / 2u;
            in.strides[2] = ((size_t)w + 1u) / 2u;

            const size_t cap = tc_frame_packet_bound(&cfg);
            uint8_t* pkt = (uint8_t*)tc_alloc(cap ? cap : 1u);
            if (pkt == NULL) { return 1; }

            /* 解码输出平面（按首帧几何分配一次） */
            uint16_t* planes[4] = {NULL, NULL, NULL, NULL};
            size_t strides[4] = {0, 0, 0, 0};
            topos_frame_output info;
            memset(&info, 0, sizeof(info));
            int have_planes = 0;

            for (size_t ti = 0u; ti < sizeof(tiers) / sizeof(tiers[0]); ++ti) {
                const double samples = (double)w * (double)h
                    + 2.0 * (double)((w + 1u) / 2u) * (double)h; /* 422 样本数 */
                const uint32_t target = (uint32_t)(tiers[ti].bpp * samples / 8.0);
                topos_frame_stats st;
                uint8_t qp_used = 0u;
                topos_frame_config trial = cfg;
                int32_t rc = tc_frame_encode_sized(&trial, &in, target, 0u, 95u,
                                                   &qp_used, pkt, cap, &st);
                if (rc != TC_OK) {
                    fprintf(stderr, "sized encode failed %ux%u %s %s: %s\n",
                            w, h, kind_name(synth.kind), tiers[ti].id, tc_last_error());
                    return 1;
                }
                const double mbps = (double)st.packet_size * 8.0 * REFERENCE_FPS / 1e6;

                if (!have_planes) {
                    if (tc_frame_decode(pkt, st.packet_size, NULL, NULL, &info) != TC_OK) {
                        return 1;
                    }
                    for (uint32_t p = 0u; p < info.plane_count; ++p) {
                        uint32_t pw = 0u, ph = 0u;
                        if (tc_frame_plane_geometry(&info, p, &pw, &ph) != TC_OK) { return 1; }
                        planes[p] = (uint16_t*)tc_alloc((size_t)pw * ph * sizeof(uint16_t));
                        if (planes[p] == NULL) { return 1; }
                        strides[p] = pw;
                    }
                    have_planes = 1;
                }

                for (int pass = 0; pass < 2; ++pass) {
                    const uint32_t th = pass == 0 ? 1u : threads_multi;
                    tc_dev_set_thread_count(th);
                    /* 热身 2 次（页表/表预热；不计时） */
                    for (uint32_t warm = 0u; warm < 2u; ++warm) {
                        if (tc_frame_decode(pkt, st.packet_size, planes, strides, &info) != TC_OK) {
                            return 1;
                        }
                    }
                    for (uint32_t it = 0u; it < iters; ++it) {
                        const double t0 = now_ms();
                        if (tc_frame_decode(pkt, st.packet_size, planes, strides, &info) != TC_OK) {
                            return 1;
                        }
                        printf("%s,%u,%u,%s,%.5f,%u,%u,%u,%.2f,%u,%u,%.6f\n",
                               kind_name(synth.kind), w, h, tiers[ti].id, tiers[ti].bpp,
                               target, qp_used, st.packet_size, mbps, th, it,
                               now_ms() - t0);
                    }
                }
            }
            for (int p = 0; p < 4; ++p) { tc_free(planes[p]); }
            tc_free(pkt);
            free(y); free(u); free(v); free(a);
        }
    }
    return 0;
}
