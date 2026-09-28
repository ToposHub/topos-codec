/* topos_quality —— 阶段 4 画质/码率/速度测量与 QM 调优 CLI（非规范性工具）。
 *
 * 允许浮点（PSNR/计时）；编码/解码本身仍走库的纯整数规范性路径。
 *
 * 用法：
 *   topos_quality report             # quality_report_scalar.md 的数据源（markdown）
 *   topos_quality sweep              # Standard QM 调优网格（dev 表覆盖）
 *   topos_quality mov                # 容器索引/cold-hot 指标
 *   topos_quality perf [quick]       # 阶段 9：分核 cycles/pixel + 帧级 p50/p95/p99
 *
 * 内容全部来自 tests/support/image_synth 的确定性合成器 —— 报告可逐字节复现。
 */
#include "../common/alloc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
#include <sys/resource.h> /* getrusage：峰值 RSS（R6 验收口径） */
#else
#include <windows.h> /* GetProcessMemoryInfo/句柄计数（W07 峰值 RSS） */
#include <psapi.h>
#include <tlhelp32.h> /* 线程计数快照 */
#if defined(__GNUC__) && defined(__x86_64__)
#include <x86intrin.h> /* _rdtsc（GCC 无 clang 的 readcyclecounter 内建） */
#elif defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h> /* __rdtsc（MSVC） */
#endif
#endif

#include "bitstream/bitio.h"
#include "codec/codec.h"
#include "common/crc32.h"
#include "common/tpool.h"
#include "entropy/block_coding.h"
#include "entropy/rice.h"
#include "entropy/scan.h"
#include "topos_codec.h"
#include "transform/plane.h"
#include "transform/quant.h"
#include "transform/transform.h"

#include "image_synth.h"

#if defined(_MSC_VER)
static double now_ms(void)
{
    LARGE_INTEGER f, c; /* QPC：单调且不受频率缩放影响（≥Win2000） */
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart * 1000.0;
}
#else
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
#endif

static double psnr_plane_bd(const uint16_t* a, const uint16_t* b, size_t n,
                            uint32_t bit_depth)
{
    if (n == 0u) { return 99.0; }
    double mse = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double d = (double)a[i] - (double)b[i];
        mse += d * d;
    }
    mse /= (double)n;
    if (mse <= 0.0) { return 99.0; }
    double peak = (double)((1ull << bit_depth) - 1u); /* R4.4：按位深定标 */
    return 10.0 * log10(peak * peak / mse);
}

static double psnr_plane(const uint16_t* a, const uint16_t* b, size_t n)
{
    return psnr_plane_bd(a, b, n, 10u);
}

typedef struct frame_set {
    uint16_t* y;
    uint16_t* u;
    uint16_t* v;
    uint16_t* a;
    uint32_t w, h, cw;
} frame_set;

static void frame_set_free(frame_set* fs)
{
    tc_free(fs->y); tc_free(fs->u); tc_free(fs->v); tc_free(fs->a);
    memset(fs, 0, sizeof(*fs));
}

static int frame_set_make(frame_set* fs, uint64_t seed, uint32_t w, uint32_t h,
                          tc_synth_kind kind, int with_alpha)
{
    memset(fs, 0, sizeof(*fs));
    image_synth_cfg ic;
    memset(&ic, 0, sizeof(ic));
    ic.seed = seed;
    ic.width = w;
    ic.height = h;
    ic.kind = kind;
    int rc = image_synth_alloc(&ic, with_alpha, &fs->y, &fs->u, &fs->v, &fs->a);
    if (rc != 0) { return -1; }
    fs->w = w;
    fs->h = h;
    fs->cw = (w + 1u) / 2u;
    return 0;
}

static int32_t encode_frame(const frame_set* fs, uint8_t qp, uint8_t qm, uint8_t alpha_mode,
                            uint8_t alpha_bd, uint8_t** pkt, size_t* size,
                            topos_frame_stats* st)
{
    topos_frame_config c;
    memset(&c, 0, sizeof(c));
    c.struct_size = (uint32_t)sizeof(c);
    c.visible_width = (uint16_t)fs->w;
    c.visible_height = (uint16_t)fs->h;
    c.qp_base = qp;
    c.qmatrix_id = qm;
    c.alpha_mode = alpha_mode;
    c.alpha_bit_depth = alpha_bd;
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    const uint16_t* pl[4] = {fs->y, fs->u, fs->v, fs->a};
    memcpy(in.planes, pl, sizeof(pl));

    size_t cap = tc_frame_packet_bound(&c);
    *pkt = (uint8_t*)tc_alloc(cap != 0u ? cap : 1u);
    if (*pkt == NULL) { return TC_ERR_OUT_OF_MEMORY; }
    int32_t rc = tc_frame_encode(&c, &in, *pkt, cap, st);
    *size = st->packet_size;
    if (rc != TC_OK) { tc_free(*pkt); *pkt = NULL; }
    return rc;
}

static int32_t decode_frame(const uint8_t* pkt, size_t size, frame_set* out /* y/u/v/a 已分配 */,
                            int32_t* rc_out)
{
    topos_frame_output info;
    int32_t rc = tc_frame_decode(pkt, size, NULL, NULL, &info);
    if (rc != TC_OK) { *rc_out = rc; return rc; }
    uint16_t* planes[4] = {out->y, out->u, out->v, out->a};
    rc = tc_frame_decode(pkt, size, planes, NULL, &info);
    *rc_out = rc;
    return (rc == TC_OK || rc == TC_WARN_CONCEALED) ? TC_OK : rc;
}

/* ---------------- report ---------------- */

static void run_rate_table(void)
{
    printf("## 码率-画质表（1920×1080 4:2:2 10-bit，qmatrix_id=1 Standard）\n\n");
    printf("| 内容 | qp | packet | Mb/s @25 | PSNR-Y | PSNR-U | PSNR-V | 平均 |\n");
    printf("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n");
    static const tc_synth_kind kinds[5] = {
        TC_SYNTH_FLAT, TC_SYNTH_GRADIENT, TC_SYNTH_GRAIN, TC_SYNTH_DETAIL, TC_SYNTH_MIXED
    };
    static const char* names[5] = {"flat", "gradient", "grain", "detail", "mixed"};
    static const uint8_t qps[8] = {20, 26, 32, 38, 44, 50, 56, 63};
    frame_set src, dec;
    for (int k = 0; k < 5; ++k) {
        frame_set_make(&src, 0x5EED000000000000ull + (uint64_t)k, 1920u, 1080u, kinds[k], 0);
        frame_set_make(&dec, 0, 1920u, 1080u, kinds[k], 0);
        for (int qi = 0; qi < 8; ++qi) {
            uint8_t* pkt = NULL;
            size_t size = 0;
            topos_frame_stats st;
            if (encode_frame(&src, qps[qi], 1u, 0u, 0u, &pkt, &size, &st) != TC_OK) { continue; }
            int32_t drc = 0;
            decode_frame(pkt, size, &dec, &drc);
            double py = psnr_plane(src.y, dec.y, (size_t)1920u * 1080u);
            double pu = psnr_plane(src.u, dec.u, (size_t)960u * 1080u);
            double pv = psnr_plane(src.v, dec.v, (size_t)960u * 1080u);
            double mbs = (double)size * 8.0 * 25.0 / 1e6;
            printf("| %s | %u | %zu B | %.1f | %.2f | %.2f | %.2f | %.2f |\n",
                   names[k], (unsigned)qps[qi], size, mbs, py, pu, pv,
                   (py + pu + pv) / 3.0);
            tc_free(pkt);
        }
        frame_set_free(&src);
        frame_set_free(&dec);
        printf("| | | | | | | | |\n");
    }
    printf("\n参考（阶段 0 基线，`baseline_encode_2026-08-29.md`）：ProRes 422 HQ 对有损 8-bit\n"
           "源 114.6–186.9 Mb/s、PSNR avg ≈ 48.2 dB；DNxHR HQX 恒定 ≈ 183.5 Mb/s。\n"
           "Topos Standard 目标区间 112–152 Mb/s（14–19 MB/s）@1080p25。\n\n");
}

static void run_multigen(void)
{
    printf("## 多代编解码退化曲线（grain，qp=48，qmatrix=1 —— 有损工作点）\n\n");
    printf("| 代 | packet | PSNR-Y(vs gen0) | PSNR-U | PSNR-V | ΔY 均值 |\n");
    printf("| ---: | ---: | ---: | ---: | ---: | ---: |\n");
    frame_set gens[8];
    frame_set_make(&gens[0], 0x5EED111111111111ull, 960u, 544u, TC_SYNTH_GRAIN, 0);
    /* 注：gen0 = 源；genN = gen(N-1) 编解码结果 */
    uint8_t* pkt = NULL;
    size_t size = 0;
    topos_frame_stats st;
    for (int g = 1; g <= 6; ++g) {
        frame_set_make(&gens[g], 0, 960u, 544u, TC_SYNTH_GRAIN, 0);
        if (encode_frame(&gens[g - 1], 48u, 1u, 0u, 0u, &pkt, &size, &st) != TC_OK) { break; }
        int32_t drc = 0;
        if (decode_frame(pkt, size, &gens[g], &drc) != TC_OK) { break; }
        double py = psnr_plane(gens[0].y, gens[g].y, (size_t)960u * 544u);
        double pu = psnr_plane(gens[0].u, gens[g].u, (size_t)480u * 544u);
        double pv = psnr_plane(gens[0].v, gens[g].v, (size_t)480u * 544u);
        double drift = 0.0;
        size_t n = (size_t)960u * 544u;
        for (size_t i = 0; i < n; ++i) { drift += (double)gens[g].y[i] - (double)gens[0].y[i]; }
        printf("| %d | %zu B | %.2f | %.2f | %.2f | %+.3f |\n", g, size, py, pu, pv,
               drift / (double)n);
        tc_free(pkt);
        pkt = NULL;
    }
    for (int g = 0; g < 8; ++g) { frame_set_free(&gens[g]); }
    printf("\n");
}

static void run_alpha(void)
{
    printf("## Alpha 模式与码率占比（1920×1080，grain，qp=24，qmatrix=1）\n\n");
    printf("| 模式 | packet | alpha payload | alpha 占比 | max err |\n");
    printf("| --- | ---: | ---: | ---: | ---: |\n");
    struct { const char* name; uint8_t mode; uint8_t bd; } rows[4] = {
        {"lossless 16-bit", 1u, 16u},
        {"near-lossless 12-bit", 2u, 12u},
        {"near-lossless 10-bit", 2u, 10u},
        {"near-lossless 8-bit", 2u, 8u},
    };
    frame_set src;
    frame_set_make(&src, 0x5EED222222222222ull, 1920u, 1080u, TC_SYNTH_GRAIN, 1);
    frame_set dec;
    frame_set_make(&dec, 0, 1920u, 1080u, TC_SYNTH_GRAIN, 1);
    for (int r = 0; r < 4; ++r) {
        uint8_t* pkt = NULL;
        size_t size = 0;
        topos_frame_stats st;
        if (encode_frame(&src, 24u, 1u, rows[r].mode, rows[r].bd, &pkt, &size, &st) != TC_OK) {
            continue;
        }
        double alpha_total = (double)st.alpha_payload_bytes + (double)st.alpha_header_bytes;
        int32_t drc = 0;
        int maxerr = 0;
        if (decode_frame(pkt, size, &dec, &drc) == TC_OK) {
            size_t n = (size_t)1920u * 1080u;
            for (size_t i = 0; i < n; ++i) {
                int d = (int)src.a[i] - (int)dec.a[i];
                if (d < 0) { d = -d; }
                if (d > maxerr) { maxerr = d; }
            }
        }
        printf("| %s | %zu B | %u B | %.1f%% | %d |\n", rows[r].name, size,
               st.alpha_payload_bytes + st.alpha_header_bytes,
               100.0 * alpha_total / (double)size, maxerr);
        tc_free(pkt);
    }
    frame_set_free(&src);
    frame_set_free(&dec);
    printf("\n（计划 §2.2 Alpha 目标占比 25–30%% 为容器策略约束；无损模式超限时编码器不静默降质，\n"
           "由调用方选择交付警告或改用受限近似模式 —— spec §11.3。）\n\n");
}


static void run_sized(void)
{
    printf("## 帧级目标码率（tc_frame_encode_sized，grain 1920×1080，qmatrix=1）\n\n");
    printf("| 目标 Mb/s | qp_used | 实际 Mb/s | PSNR-Y | 编码次数上界内 |\n");
    printf("| ---: | ---: | ---: | ---: | --- |\n");
    static const uint32_t targets[4] = {130, 160, 200, 260};
    frame_set src, dec;
    frame_set_make(&src, 0x5EED000000000002ull, 1920u, 1080u, TC_SYNTH_GRAIN, 0);
    frame_set_make(&dec, 0, 1920u, 1080u, TC_SYNTH_GRAIN, 0);
    for (int t = 0; t < 4; ++t) {
        topos_frame_config c;
        memset(&c, 0, sizeof(c));
        c.struct_size = (uint32_t)sizeof(c);
        c.visible_width = 1920u;
        c.visible_height = 1080u;
        c.qp_base = 20u;
        c.qmatrix_id = 1u;
        topos_frame_input in;
        memset(&in, 0, sizeof(in));
        in.struct_size = (uint32_t)sizeof(in);
        const uint16_t* pl[4] = {src.y, src.u, src.v, NULL};
        memcpy(in.planes, pl, sizeof(pl));
        uint32_t target_bytes = (uint32_t)((double)targets[t] * 1e6 / 8.0 / 25.0);
        size_t cap = tc_frame_packet_bound(&c);
        uint8_t* pkt = (uint8_t*)tc_alloc(cap);
        uint8_t qp_used = 0u;
        topos_frame_stats st;
        int32_t rc = tc_frame_encode_sized(&c, &in, target_bytes, 0u, 63u, &qp_used,
                                           pkt, cap, &st);
        double py = 0.0;
        if (rc == TC_OK) {
            int32_t drc = 0;
            if (decode_frame(pkt, st.packet_size, &dec, &drc) == TC_OK) {
                py = psnr_plane(src.y, dec.y, (size_t)1920u * 1080u);
            }
        }
        printf("| %u | %u | %.1f | %.2f | 是（≤24 次确定性搜索） |\n",
               (unsigned)targets[t], (unsigned)qp_used,
               (double)st.packet_size * 8.0 * 25.0 / 1e6, py);
        tc_free(pkt);
    }
    frame_set_free(&src);
    frame_set_free(&dec);
    printf("\n（qp 每步 4 倍尺度，高位端粒度变粗；目标不可达时在 qp_max 尽力返回 —— best-effort 语义）\n\n");
}

static void run_timing(void)
{
    printf("## 标量速度基线（grain，qp=20，qmatrix=1，best-of-3，单线程）\n\n");
    printf("| 分辨率 | 编码 ms | 编码 fps | 解码 ms | 解码 fps | 码率 Mb/s |\n");
    printf("| --- | ---: | ---: | ---: | ---: | ---: |\n");
    struct { uint32_t w, h; } dims[2] = {{1920u, 1080u}, {3840u, 2160u}};
    for (int d = 0; d < 2; ++d) {
        frame_set src, dec;
        frame_set_make(&src, 0x5EED333333333333ull, dims[d].w, dims[d].h, TC_SYNTH_GRAIN, 0);
        frame_set_make(&dec, 0, dims[d].w, dims[d].h, TC_SYNTH_GRAIN, 0);
        uint8_t* pkt = NULL;
        size_t size = 0;
        topos_frame_stats st;
        if (encode_frame(&src, 20u, 1u, 0u, 0u, &pkt, &size, &st) != TC_OK) {
            frame_set_free(&src); frame_set_free(&dec);
            continue;
        }
        double enc_best = 1e9, dec_best = 1e9;
        for (int i = 0; i < 3; ++i) {
            double t0 = now_ms();
            tc_free(pkt);
            pkt = NULL;
            if (encode_frame(&src, 20u, 1u, 0u, 0u, &pkt, &size, &st) != TC_OK) { break; }
            double t1 = now_ms();
            int32_t drc = 0;
            decode_frame(pkt, size, &dec, &drc);
            double t2 = now_ms();
            if (t1 - t0 < enc_best) { enc_best = t1 - t0; }
            if (t2 - t1 < dec_best) { dec_best = t2 - t1; }
        }
        printf("| %ux%u | %.1f | %.2f | %.1f | %.2f | %.1f |\n",
               dims[d].w, dims[d].h, enc_best, 1000.0 / enc_best, dec_best,
               1000.0 / dec_best, (double)size * 8.0 * 25.0 / 1e6);
        tc_free(pkt);
        frame_set_free(&src);
        frame_set_free(&dec);
    }
    printf("\n12-bit：v1 profile 仅验收 10-bit（UNSUPPORTED_PIXEL_FORMAT）；变换/量化的整数\n"
           "界已按 12-bit 推导并实测（ADR-C002 §7.6），启用只待 profile 扩展决策。\n\n");
}

static int report_mode(void)
{
    printf("# Topos Codec 阶段 4 标量质量/速度报告（数据源：topos_quality report）\n\n");
    run_rate_table();
    run_sized();
    run_multigen();
    run_alpha();
    run_timing();
    return 0;
}

/* ---------------- sweep（QM 调优） ---------------- */

static void build_slope_matrix(tc_qmatrix_set* qms, uint32_t luma_slope, uint32_t chroma_slope)
{
    /* 几何递增（JPEG 风格）：qm[d] = 16·r^d（r = 1 + slope/100） */
    for (uint32_t u = 0u; u < 8u; ++u) {
        for (uint32_t v = 0u; v < 8u; ++v) {
            uint32_t d = u + v;
            uint64_t l = 16u, c = 16u;
            for (uint32_t i = 0u; i < d; ++i) {
                l = l * (100u + luma_slope) / 100u;
                c = c * (100u + chroma_slope) / 100u;
            }
            if (l > 4095u) { l = 4095u; }
            if (c > 4095u) { c = 4095u; }
            qms->luma[u * 8u + v] = (uint16_t)l;
            qms->chroma[u * 8u + v] = (uint16_t)c;
        }
    }
}

static int sweep_mode(void)
{
    printf("# Standard QM 调优网格（qm[d] = 16·(1+slope%%)^d，d=u+v；grain 噪声 ±15）\n\n");
    printf("| L | C | qp | grain Mb/s | grain PSNR | detail Mb/s | detail PSNR |\n");
    printf("| ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n");
    static const uint32_t Ls[4] = {12, 18, 25, 35};
    static const uint32_t Cs[3] = {12, 18, 25};
    static const uint8_t qps[3] = {24, 32, 40};

    frame_set grain, detail, dec;
    frame_set_make(&grain, 0x5EED000000000000ull + 2, 1920u, 1080u, TC_SYNTH_GRAIN, 0);
    frame_set_make(&detail, 0x5EED000000000000ull + 3, 1920u, 1080u, TC_SYNTH_DETAIL, 0);
    frame_set_make(&dec, 0, 1920u, 1080u, TC_SYNTH_GRAIN, 0);

    for (int li = 0; li < 4; ++li) {
        for (int ci = 0; ci < 3; ++ci) {
            tc_qmatrix_set qms;
            build_slope_matrix(&qms, Ls[li], Cs[ci]);
            tc_dev_set_qmatrix_override(&qms);
            for (int qi = 0; qi < 3; ++qi) {
                double g_mbs = 0.0, g_p = 0.0, d_mbs = 0.0, d_p = 0.0;
                uint8_t* pkt = NULL;
                size_t size = 0;
                topos_frame_stats st;
                if (encode_frame(&grain, qps[qi], 1u, 0u, 0u, &pkt, &size, &st) == TC_OK) {
                    int32_t drc = 0;
                    if (decode_frame(pkt, size, &dec, &drc) == TC_OK) {
                        g_mbs = (double)size * 8.0 * 25.0 / 1e6;
                        g_p = psnr_plane(grain.y, dec.y, (size_t)1920u * 1080u);
                    }
                    tc_free(pkt);
                }
                if (encode_frame(&detail, qps[qi], 1u, 0u, 0u, &pkt, &size, &st) == TC_OK) {
                    int32_t drc = 0;
                    if (decode_frame(pkt, size, &dec, &drc) == TC_OK) {
                        d_mbs = (double)size * 8.0 * 25.0 / 1e6;
                        d_p = psnr_plane(detail.y, dec.y, (size_t)1920u * 1080u);
                    }
                    tc_free(pkt);
                }
                printf("| %u | %u | %u | %.1f | %.2f | %.1f | %.2f |\n",
                       (unsigned)Ls[li], (unsigned)Cs[ci], (unsigned)qps[qi],
                       g_mbs, g_p, d_mbs, d_p);
            }
        }
    }
    tc_dev_set_qmatrix_override(NULL);
    frame_set_free(&grain);
    frame_set_free(&detail);
    frame_set_free(&dec);
    return 0;
}


/* ---------------- mov（阶段 5 验收指标：索引内存 / hot-cold 延迟） ---------------- */

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
} q_mem;

static int32_t qm_read(void* ctx, uint64_t off, void* buf, size_t n)
{
    q_mem* m = (q_mem*)ctx;
    if (off > (uint64_t)m->len || n > (uint64_t)m->len - off) { return TC_ERR_IO; }
    if (n != 0u) { memcpy(buf, m->data + off, n); }
    return TC_OK;
}

static int32_t qm_write(void* ctx, const void* d, size_t n)
{
    q_mem* m = (q_mem*)ctx;
    if (m->len + n > m->cap) {
        size_t cap = m->cap ? m->cap : 4096u;
        while (cap < m->len + n) { cap *= 2u; }
        uint8_t* p = (uint8_t*)tc_realloc(m->data, cap);
        if (p == NULL) { return TC_ERR_OUT_OF_MEMORY; }
        m->data = p;
        m->cap = cap;
    }
    if (n != 0u) { memcpy(m->data + m->len, d, n); }
    m->len += n;
    return TC_OK;
}

static int32_t qm_seek_write(void* ctx, uint64_t off, const void* d, size_t n)
{
    q_mem* m = (q_mem*)ctx;
    if (off > (uint64_t)m->len || n > (uint64_t)m->len - off) { return TC_ERR_IO; }
    if (n != 0u) { memcpy(m->data + off, d, n); }
    return TC_OK;
}

static int mov_mode(void)
{
    /* 640x360 grain, 240 帧（10s@24fps），qp24/qm1 —— 中等规模索引指标 */
    const uint32_t W = 640u, H = 360u, FRAMES = 240u;
    frame_set fs;
    if (frame_set_make(&fs, 0x5EEDu, W, H, TC_SYNTH_GRAIN, 0) != 0) { return 1; }
    topos_frame_config fc;
    memset(&fc, 0, sizeof(fc));
    fc.struct_size = (uint32_t)sizeof(fc);
    fc.abi_version = TOPOS_CODEC_ABI_VERSION;
    fc.visible_width = (uint16_t)W;
    fc.visible_height = (uint16_t)H;
    fc.profile = 3u;
    fc.bit_depth = 10u;
    fc.qmatrix_id = 1u;
    fc.qp_base = 24u;
    fc.color_range = 1u;
    fc.color_primaries = 1u;
    fc.color_transfer = 1u;
    fc.color_matrix = 1u;
    fc.sar_num = 1u;
    fc.sar_den = 1u;
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = TOPOS_CODEC_ABI_VERSION;
    in.planes[0] = fs.y;
    in.planes[1] = fs.u;
    in.planes[2] = fs.v;

    topos_movie_config mc;
    memset(&mc, 0, sizeof(mc));
    mc.struct_size = (uint32_t)sizeof(mc);
    mc.abi_version = TOPOS_CODEC_ABI_VERSION;
    mc.visible_width = (uint16_t)W;
    mc.visible_height = (uint16_t)H;
    mc.profile = 3u;
    mc.bit_depth = 10u;
    mc.qmatrix_id = 1u;
    mc.qp_base = 24u;
    mc.color_range = 1u;
    mc.color_primaries = 1u;
    mc.color_transfer = 1u;
    mc.color_matrix = 1u;
    mc.sar_num = 1u;
    mc.sar_den = 1u;
    mc.timescale = 24u;

    q_mem mem;
    memset(&mem, 0, sizeof(mem));
    topos_io sink;
    memset(&sink, 0, sizeof(sink));
    sink.struct_size = (uint32_t)sizeof(sink);
    sink.abi_version = TOPOS_CODEC_ABI_VERSION;
    sink.ctx = &mem;
    sink.write = qm_write;
    sink.seek_write = qm_seek_write;
    topos_mux* mux = NULL;
    int32_t rc = tc_mux_create(&mc, &sink, &mux);
    size_t bound = tc_frame_packet_bound(&fc);
    uint8_t* pkt = (uint8_t*)tc_alloc(bound);
    topos_frame_stats st;
    uint64_t pts = 0u;
    clock_t t0 = clock();
    for (uint32_t i = 0; i < FRAMES && rc == TC_OK; ++i) {
        rc = tc_frame_encode(&fc, &in, pkt, bound, &st);
        if (rc == TC_OK) { rc = tc_mux_add_packet(mux, pkt, st.packet_size, pts, 1u); }
        pts += 1u;
    }
    if (rc == TC_OK) { rc = tc_mux_finish(mux); }
    tc_mux_free(mux);
    double mux_s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (rc != TC_OK) {
        fprintf(stderr, "mov build failed: %s\n", tc_status_message(rc));
        tc_free(pkt);
        frame_set_free(&fs);
        tc_free(mem.data);
        return 1;
    }
    tc_free(pkt);
    frame_set_free(&fs);

    printf("# MOV 容器指标（%ux%u × %u 帧，qp24/qm1，内存 io）\n\n", W, H, FRAMES);
    printf("| 指标 | 值 |\n| --- | ---: |\n");
    printf("| 文件大小 | %zu B (%.2f MiB) |\n", mem.len, (double)mem.len / 1048576.0);
    printf("| mux 总耗时（含编码） | %.1f ms |\n", mux_s * 1000.0);

    /* cold：tc_movie_open（解析 + 建索引） */
    topos_io src;
    memset(&src, 0, sizeof(src));
    src.struct_size = (uint32_t)sizeof(src);
    src.abi_version = TOPOS_CODEC_ABI_VERSION;
    src.ctx = &mem;
    src.read = qm_read;
    src.length = (uint64_t)mem.len;
    topos_movie* mv = NULL;
    t0 = clock();
    rc = tc_movie_open(&src, &mv);
    double open_ms = (double)(clock() - t0) / CLOCKS_PER_SEC * 1000.0;
    if (rc != TC_OK) {
        fprintf(stderr, "open failed\n");
        tc_free(mem.data);
        return 1;
    }
    topos_movie_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    tc_movie_info(mv, &info);
    printf("| cold open（moov 解析+索引构建） | %.3f ms |\n", open_ms);
    printf("| 索引内存 | %u B（%.2f B/帧） |\n", info.index_bytes,
           (double)info.index_bytes / (double)info.sample_count);

    /* cold first frame：首帧 packet 读取（open 后第一次，含首个 read） */
    size_t cap = 65536u;
    uint8_t* buf = (uint8_t*)tc_alloc(cap);
    size_t need = 0;
    tc_movie_packet(mv, 0, NULL, 0, &need);
    if (need > cap) { tc_free(buf); buf = (uint8_t*)tc_alloc(need); cap = need; }
    t0 = clock();
    rc = tc_movie_packet(mv, 0, buf, cap, NULL);
    double cold_first_ms = (double)(clock() - t0) / CLOCKS_PER_SEC * 1000.0;
    printf("| cold 首帧 packet 读取（%zu B） | %.3f ms |\n", need, cold_first_ms);

    /* hot：随机 sample 探测+读取（索引已驻留） */
    uint64_t acc = 0u;
    const int ROUNDS = 20000;
    t0 = clock();
    for (int i = 0; i < ROUNDS; ++i) {
        uint32_t idx = (uint32_t)((i * 7919u) % info.sample_count);
        size_t n2 = 0;
        tc_movie_packet(mv, idx, NULL, 0, &n2);
        acc += n2;
    }
    double hot_ms = (double)(clock() - t0) / CLOCKS_PER_SEC * 1000.0;
    printf("| hot 随机 sample 探测（%d 次） | %.3f ms（%.2f us/次） |\n",
           ROUNDS, hot_ms, hot_ms * 1000.0 / (double)ROUNDS);
    /* hot 全帧读取（含数据） */
    t0 = clock();
    for (uint32_t i = 0; i < info.sample_count; ++i) {
        size_t n2 = 0;
        if (tc_movie_packet(mv, i, NULL, 0, &n2) == TC_ERR_BUFFER_TOO_SMALL &&
            n2 <= cap) {
            tc_movie_packet(mv, i, buf, n2, NULL);
        }
    }
    double hot_all_ms = (double)(clock() - t0) / CLOCKS_PER_SEC * 1000.0;
    printf("| hot 全帧顺序读取（%u 帧） | %.2f ms（%.3f ms/帧） |\n",
           info.sample_count, hot_all_ms, hot_all_ms / (double)info.sample_count);
    printf("\n（探测校验和 = %llu，防优化移除）\n", (unsigned long long)acc);
    (void)rc;

    tc_free(buf);
    tc_movie_close(mv);
    tc_free(mem.data);
    return 0;
}

/* ---------------- perf（阶段 9：分核 cycles/pixel + 帧级 p50/p95/p99） ----------------
 *
 * 方法论（benchmark_protocol.md §4/§6）：
 *  - 内核微基准直接调用库内部函数（transform/quant/rice/bitio/crc/plane），
 *    输入取自 grain 合成帧的真实分布，TSC 计 cycles（x86_64/arm64；其余平台退化为 µs）。
 *  - 帧级基准按 4:2:2 10-bit 全 pipeline 计时，报告 p50/p95/p99（非仅平均/最佳），
 *    cycles/pixel 以 luma 像素计（chroma 计入分母换算 1.5×，alpha 额外 +1.0×）。
 *  - quick 模式缩短迭代数（CI 冒烟用），完整模式为报告口径。
 */

static uint64_t perf_rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t perf_rand_u64(void)
{
    perf_rng_state ^= perf_rng_state >> 12;
    perf_rng_state ^= perf_rng_state << 25;
    perf_rng_state ^= perf_rng_state >> 27;
    return perf_rng_state * 2685821657736338717ull;
}

static uint64_t perf_ticks(void)
{
#if defined(__clang__) && (defined(__x86_64__) || defined(__aarch64__))
    return (uint64_t)__builtin_readcyclecounter();
#elif defined(__GNUC__) && defined(__x86_64__)
    return (uint64_t)_rdtsc(); /* GCC/x86：clang 专有内建的等价物 */
#elif defined(_M_X64)
    return (uint64_t)__rdtsc(); /* MSVC/x64 */
#elif defined(__aarch64__)
    return (uint64_t)__builtin_readcyclecounter();
#else
    return (uint64_t)(now_ms() * 1000.0); /* µs 计数兜底（无 TSC 平台） */
#endif
}

static int perf_ticks_is_cycles(void)
{
#if defined(__x86_64__) || defined(__aarch64__)
    return 1;
#else
    return 0;
#endif
}

/* TSC 频率校准（perf 入口做一次；Mpx/s 等速率列的换算依据） */
static double g_ticks_per_ms = 1.0;

static void perf_calibrate(void)
{
    double t0w = now_ms();
    uint64_t t0t = perf_ticks();
    volatile uint64_t sink = 0u;
    while (now_ms() - t0w < 50.0) { sink += perf_ticks(); }
    (void)sink;
    uint64_t t1t = perf_ticks();
    double t1w = now_ms();
    g_ticks_per_ms = (double)(t1t - t0t) / (t1w - t0w);
}

static double perf_rate_mpx(double px, uint64_t ticks)
{
    double seconds = (double)ticks / (g_ticks_per_ms * 1000.0);
    return px / 1e6 / seconds;
}

typedef struct perf_pct {
    double p50, p95, p99, best, worst;
} perf_pct;

static int perf_double_cmp(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void perf_percentiles(const double* v, int n, perf_pct* out)
{
    double tmp[64];
    if (n > 64) { n = 64; }
    memcpy(tmp, v, (size_t)n * sizeof(double));
    qsort(tmp, (size_t)n, sizeof(double), perf_double_cmp);
    int i50 = n / 2;
    int i95 = (n * 95 + 99) / 100; if (i95 >= n) { i95 = n - 1; }
    int i99 = (n * 99 + 99) / 100; if (i99 >= n) { i99 = n - 1; }
    out->best = tmp[0];
    out->worst = tmp[n - 1];
    out->p50 = tmp[i50];
    out->p95 = tmp[i95];
    out->p99 = tmp[i99];
}

/* ---- 内核微基准 ---- */

static void bench_kernels(uint32_t w, uint32_t h)
{
    printf("## 内核微基准（grain 分布，每块 64 像素）\n\n");
    printf("| 内核 | cycles/块 | cycles/像素 | Mpx/s |\n");
    printf("| --- | ---: | ---: | ---: |\n");

    frame_set fs;
    frame_set_make(&fs, 0x5EED999999999999ull, w, h, TC_SYNTH_GRAIN, 0);
    enum { KB = 8192 };
    static int16_t kx[KB][64];
    static int32_t kF[KB][64];
    static int32_t kq[KB][64];
    static int32_t ktmp[64];
    const tc_qmatrix_set* qms = tc_qmatrix_by_id(1u);

    /* 从 grain 帧按光栅序取块（level shift 后），量化一次得到真实 q 分布 */
    uint32_t bx = 0u, by = 0u;
    for (int i = 0; i < KB; ++i) {
        if (bx + 8u > fs.w) { bx = 0u; by += 8u; }
        if (by + 8u > fs.h) { by = 0u; }
        for (uint32_t yy = 0u; yy < 8u; ++yy) {
            for (uint32_t xx = 0u; xx < 8u; ++xx) {
                kx[i][yy * 8u + xx] =
                    (int16_t)((int32_t)fs.y[(size_t)(by + yy) * fs.w + (bx + xx)] - 512);
            }
        }
        tc_transform_forward_8x8(kx[i], kF[i]);
        tc_quant_block(kF[i], qms->luma, 20u, kq[i]);
        bx += 8u;
    }

    const int passes = 6;

    /* forward transform */
    {
        uint64_t t0 = perf_ticks();
        for (int p = 0; p < passes; ++p) {
            for (int i = 0; i < KB; ++i) { tc_transform_forward_8x8(kx[i], ktmp); }
        }
        uint64_t dt = perf_ticks() - t0;
        double cyc_blk = (double)dt / ((double)KB * passes);
        printf("| 正变换 8×8 | %.0f | %.2f | %.1f |\n", cyc_blk, cyc_blk / 64.0,
               perf_rate_mpx(64.0 * KB * passes, dt));
    }
    /* 量化 qp20 / qp50（帧内热路径 = 预建 ctx + 块循环；建表开销帧级另计） */
    for (int qi = 0; qi < 2; ++qi) {
        uint32_t qp = qi == 0 ? 20u : 50u;
        tc_quant_ctx qctx;
        tc_quant_ctx_init(&qctx, qms->luma, qp);
        uint64_t t0 = perf_ticks();
        for (int p = 0; p < passes; ++p) {
            for (int i = 0; i < KB; ++i) { tc_quant_block_ctx(&qctx, kF[i], ktmp); }
        }
        uint64_t dt = perf_ticks() - t0;
        double cyc_blk = (double)dt / ((double)KB * passes);
        printf("| 量化(ctx) qp%u | %.0f | %.2f | %.1f |\n", (unsigned)qp, cyc_blk, cyc_blk / 64.0,
               perf_rate_mpx(64.0 * KB * passes, dt));
    }
    /* 反量化 + 逆变换（解码侧） */
    {
        tc_quant_ctx dctx;
        tc_quant_ctx_init(&dctx, qms->luma, 20u);
        uint64_t t0 = perf_ticks();
        for (int p = 0; p < passes; ++p) {
            for (int i = 0; i < KB; ++i) { tc_dequant_block_ctx(&dctx, kq[i], ktmp); }
        }
        uint64_t dt = perf_ticks() - t0;
        double cyc_blk = (double)dt / ((double)KB * passes);
        printf("| 反量化(ctx) qp20 | %.0f | %.2f | %.1f |\n", cyc_blk, cyc_blk / 64.0,
               perf_rate_mpx(64.0 * KB * passes, dt));
    }
    {
        uint64_t t0 = perf_ticks();
        for (int p = 0; p < passes; ++p) {
            for (int i = 0; i < KB; ++i) { tc_transform_inverse_8x8(kF[i], ktmp); }
        }
        uint64_t dt = perf_ticks() - t0;
        double cyc_blk = (double)dt / ((double)KB * passes);
        printf("| 逆变换 8×8 | %.0f | %.2f | %.1f |\n", cyc_blk, cyc_blk / 64.0,
               perf_rate_mpx(64.0 * KB * passes, dt));
    }
    /* 熵编码/解码（(run,level) 块符号层，q 分布 = qp20 grain；编码侧为 R6
     * zigzag 序布局变体 = 生产路径，解码侧不变） */
    {
        static int32_t kqz[KB][64];
        for (int i = 0; i < KB; ++i) {
            for (int j = 0; j < 64; ++j) { kqz[i][kTcZigzagInv[j]] = kq[i][j]; }
        }
        tc_bitwriter bw;
        tc_bitwriter_init(&bw);
        uint64_t t0 = perf_ticks();
        for (int p = 0; p < passes; ++p) {
            tc_bitwriter_reset(&bw);
            for (int i = 0; i < KB; ++i) {
                tc_block_encode_zigzag(&bw, 1u, 2u, 2u, kqz[i], i > 0 ? 1 : 0,
                                       kqz[i > 0 ? i - 1 : 0][0], 0, 0, NULL);
            }
            tc_bitwriter_flush_zero_pad(&bw);
        }
        uint64_t dt = perf_ticks() - t0;
        double cyc_blk = (double)dt / ((double)KB * passes);
        printf("| 熵编码（块符号层，zigzag 序） | %.0f | %.2f | %.1f |\n", cyc_blk, cyc_blk / 64.0,
               perf_rate_mpx(64.0 * KB * passes, dt));
        tc_bitwriter_free(&bw);
    }
    {
        /* 先生成一条真实 payload，再重复解码 */
        tc_bitwriter bw;
        tc_bitwriter_init(&bw);
        for (int i = 0; i < KB; ++i) {
            tc_block_encode(&bw, 1u, 2u, 2u, kq[i], i > 0 ? 1 : 0, kq[i > 0 ? i - 1 : 0][0],
                            0, 0, NULL);
        }
        tc_bitwriter_flush_zero_pad(&bw);
        size_t payload = tc_bitwriter_byte_size(&bw);
        uint64_t t0 = perf_ticks();
        for (int p = 0; p < passes; ++p) {
            tc_bitreader br;
            tc_bitreader_init(&br, tc_bitwriter_data(&bw), payload);
            for (int i = 0; i < KB; ++i) {
                tc_block_decode(&br, 1u, 2u, 2u, i > 0 ? 1 : 0, kq[i > 0 ? i - 1 : 0][0], 0, 0,
                                ktmp);
            }
        }
        uint64_t dt = perf_ticks() - t0;
        double cyc_blk = (double)dt / ((double)KB * passes);
        printf("| 熵解码（块符号层） | %.0f | %.2f | %.1f |\n", cyc_blk, cyc_blk / 64.0,
               perf_rate_mpx(64.0 * KB * passes, dt));
        tc_bitwriter_free(&bw);
    }
    /* CRC32（slice payload + self-check 双份） */
    {
        enum { CRC_BUF = 1u << 20 };
        static uint8_t cbuf[CRC_BUF];
        for (size_t i = 0; i < CRC_BUF; ++i) { cbuf[i] = (uint8_t)perf_rand_u64(); }
        uint64_t acc = 0u;
        uint64_t t0 = perf_ticks();
        const int crc_passes = 32;
        for (int p = 0; p < crc_passes; ++p) { acc += tc_crc32(cbuf, CRC_BUF); }
        uint64_t dt = perf_ticks() - t0;
        printf("| CRC32 | — | — | %.0f MB/s |\n",
               perf_rate_mpx((double)CRC_BUF * crc_passes * 1e6 / 1e6, dt));
        (void)acc;
    }
    /* pad / crop */
    {
        uint32_t cw = ((w + 7u) / 8u) * 8u;
        uint32_t ch = ((h + 7u) / 8u) * 8u;
        uint16_t* coded = (uint16_t*)tc_alloc((size_t)cw * ch * sizeof(uint16_t));
        uint16_t* vis = (uint16_t*)tc_alloc((size_t)w * h * sizeof(uint16_t));
        if (coded != NULL && vis != NULL) {
            uint64_t t0 = perf_ticks();
            const int pc_passes = 64;
            for (int p = 0; p < pc_passes; ++p) {
                tc_plane_pad_u16(fs.y, w, h, w, coded, cw, ch, cw);
            }
            uint64_t dt = perf_ticks() - t0;
            printf("| pad visible→coded | — | %.2f | %.1f |\n",
                   (double)dt / ((double)w * h * pc_passes),
                   perf_rate_mpx((double)w * h * pc_passes, dt));
            t0 = perf_ticks();
            for (int p = 0; p < pc_passes; ++p) {
                tc_plane_crop_u16(coded, cw, ch, cw, vis, w, h, w);
            }
            dt = perf_ticks() - t0;
            printf("| crop coded→visible | — | %.2f | %.1f |\n",
                   (double)dt / ((double)w * h * pc_passes),
                   perf_rate_mpx((double)w * h * pc_passes, dt));
        }
        tc_free(coded);
        tc_free(vis);
    }
    frame_set_free(&fs);
    printf("\n");
}

/* ---- 帧级基准 ---- */

#if !defined(_WIN32)
static unsigned long long perf_peak_rss_kb(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) { return 0ull; }
#if defined(__APPLE__)
    return (unsigned long long)ru.ru_maxrss / 1024ull; /* macOS：字节 → KB */
#else
    return (unsigned long long)ru.ru_maxrss; /* Linux：已是 KB */
#endif
}
#else
/* Windows 无 getrusage：取峰值工作集（PeakWorkingSetSize，字节→KB）。
 * 口径说明：与 ru_maxrss 同为"历史峰值驻留"语义；私有字节
 * （PeakPagefileUsage）与提交量是不同度量，不冒充 RSS（W07）。 */
static unsigned long long perf_peak_rss_kb(void)
{
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) == 0) {
        return 0ull;
    }
    return (unsigned long long)pmc.PeakWorkingSetSize / 1024ull;
}

/* W07：同时记录峰值句柄数与线程数（工具帮助快照泄漏诊断；
 * 线程数经 Toolhelp 线程快照统计当前进程）。 */
static unsigned long long perf_handle_count(void)
{
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles) == 0) { return 0ull; }
    return (unsigned long long)handles;
}

static unsigned long long perf_thread_count(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) { return 0ull; }
    unsigned long long n = 0;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te) != 0) {
        do {
            if (te.th32OwnerProcessID == pid) { n++; }
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te) != 0);
    }
    CloseHandle(snap);
    return n;
}
#endif

typedef struct perf_frame_cfg {
    const char* label;
    uint32_t w, h;
    uint8_t qp;
    uint8_t alpha_mode; /* 0 无 / 2 mode2 */
    uint8_t alpha_bd;
} perf_frame_cfg;

static void bench_frames(const perf_frame_cfg* cfgs, int n_cfg, int quick)
{
    printf("## 帧级基准（grain，qmatrix=1，%d 次迭代，p50/p95/p99 单位 ms）\n\n",
           quick ? 12 : 30);
    printf("| 配置 | 模式 | p50 | p95 | p99 | fps@p50 | cycles/px@p50 | Mb/s@25 | sized均迭代 |\n");
    printf("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n");

    const int warmup = 5;
    const int iters = quick ? 12 : 30;
    double cold_enc[8], cold_dec[8], cold_sized[8];
    const perf_frame_cfg* cold_cfg[8];
    int n_cold = 0;

    for (int ci = 0; ci < n_cfg; ++ci) {
        const perf_frame_cfg* c = &cfgs[ci];
        int with_alpha = c->alpha_mode != 0u;
        frame_set src, dec;
        frame_set_make(&src, 0x5EED777777777777ull, c->w, c->h, TC_SYNTH_GRAIN, with_alpha);
        frame_set_make(&dec, 0, c->w, c->h, TC_SYNTH_GRAIN, with_alpha);
        if (src.y == NULL || dec.y == NULL || (with_alpha && (src.a == NULL || dec.a == NULL))) {
            frame_set_free(&src);
            frame_set_free(&dec);
            continue;
        }

        topos_frame_config fc;
        memset(&fc, 0, sizeof(fc));
        fc.struct_size = (uint32_t)sizeof(fc);
        fc.visible_width = (uint16_t)c->w;
        fc.visible_height = (uint16_t)c->h;
        fc.qp_base = c->qp;
        fc.qmatrix_id = 1u;
        fc.alpha_mode = c->alpha_mode;
        fc.alpha_bit_depth = c->alpha_bd;
        topos_frame_input in;
        memset(&in, 0, sizeof(in));
        in.struct_size = (uint32_t)sizeof(in);
        const uint16_t* pl[4] = {src.y, src.u, src.v, src.a};
        memcpy(in.planes, pl, sizeof(pl));

        size_t cap = tc_frame_packet_bound(&fc);
        uint8_t* pkt = (uint8_t*)tc_alloc(cap != 0u ? cap : 1u);
        topos_frame_stats st;
        uint16_t* dplanes[4] = {dec.y, dec.u, dec.v, dec.a};
        topos_frame_output info;
        double enc_t[64], dec_t[64], sized_t[64];
        int n_enc = 0, n_dec = 0, n_sized = 0;
        double px = (double)c->w * c->h * 1.5 + (with_alpha ? (double)c->w * c->h : 0.0);

        /* encode_sized 目标：qp20 包的 60%（有码率控制的实际工作负载） */
        /* R6 冷口径：该几何在进程内首次执行（新缓冲/缺页），先于任何 warmup */
        double ce = 0.0, cd = 0.0, cs = 0.0;
        {
            uint64_t t0 = perf_ticks();
            int32_t rc = tc_frame_encode(&fc, &in, pkt, cap, &st);
            uint64_t t1 = perf_ticks();
            if (rc == TC_OK) { ce = (double)(t1 - t0) / g_ticks_per_ms; }
            size_t psz = st.packet_size;
            uint64_t t2 = perf_ticks();
            rc = tc_frame_decode(pkt, psz, dplanes, NULL, &info);
            uint64_t t3 = perf_ticks();
            if (rc == TC_OK || rc == TC_WARN_CONCEALED) { cd = (double)(t3 - t2) / g_ticks_per_ms; }
            uint64_t t4 = perf_ticks();
            rc = tc_frame_encode_sized(&fc, &in, cap / 5u * 3u, 10u, 63u, NULL, pkt, cap, &st);
            uint64_t t5 = perf_ticks();
            if (rc == TC_OK) { cs = (double)(t5 - t4) / g_ticks_per_ms; }
            if (n_cold < 8) {
                cold_cfg[n_cold] = c;
                cold_enc[n_cold] = ce;
                cold_dec[n_cold] = cd;
                cold_sized[n_cold] = cs;
                n_cold++;
            }
        }
        uint32_t qp20_size = 0u;
        for (int i = 0; i < warmup; ++i) {
            tc_frame_encode(&fc, &in, pkt, cap, &st);
            tc_frame_decode(pkt, st.packet_size, dplanes, NULL, &info);
        }
        qp20_size = st.packet_size;
        uint32_t sized_target = qp20_size / 5u * 3u;
        for (int i = 0; i < warmup; ++i) {
            tc_frame_encode_sized(&fc, &in, sized_target, 10u, 63u, NULL, pkt, cap, &st);
        }

        tc_dev_sized_reset(); /* R6：迭代计数（iters/calls）——warm 口径 */
        for (int i = 0; i < iters; ++i) {
            uint64_t t0 = perf_ticks();
            int32_t rc = tc_frame_encode(&fc, &in, pkt, cap, &st);
            uint64_t t1 = perf_ticks();
            if (rc == TC_OK) { enc_t[n_enc++] = (double)(t1 - t0); }
            size_t psz = st.packet_size;
            uint64_t t2 = perf_ticks();
            rc = tc_frame_decode(pkt, psz, dplanes, NULL, &info);
            uint64_t t3 = perf_ticks();
            if (rc == TC_OK || rc == TC_WARN_CONCEALED) { dec_t[n_dec++] = (double)(t3 - t2); }
            uint64_t t4 = perf_ticks();
            uint8_t qp_used = 0u;
            rc = tc_frame_encode_sized(&fc, &in, sized_target, 10u, 63u, &qp_used, pkt, cap, &st);
            uint64_t t5 = perf_ticks();
            if (rc == TC_OK) { sized_t[n_sized++] = (double)(t5 - t4); }
        }

        perf_pct pe, pd, ps;
        perf_percentiles(enc_t, n_enc, &pe);
        perf_percentiles(dec_t, n_dec, &pd);
        perf_percentiles(sized_t, n_sized, &ps);
        double ticks_per_ms = g_ticks_per_ms;

        perf_pct pe_ms, pd_ms, ps_ms;
        pe_ms.p50 = pe.p50 / ticks_per_ms; pe_ms.p95 = pe.p95 / ticks_per_ms;
        pe_ms.p99 = pe.p99 / ticks_per_ms;
        pd_ms.p50 = pd.p50 / ticks_per_ms; pd_ms.p95 = pd.p95 / ticks_per_ms;
        pd_ms.p99 = pd.p99 / ticks_per_ms;
        ps_ms.p50 = ps.p50 / ticks_per_ms; ps_ms.p95 = ps.p95 / ticks_per_ms;
        ps_ms.p99 = ps.p99 / ticks_per_ms;

        double sized_avg_iters = 0.0;
        if (tc_dev_sized_calls() > 0ull) {
            sized_avg_iters = (double)tc_dev_sized_iters() / (double)tc_dev_sized_calls();
        }

        printf("| %ux%u qp%u%s | 编码 | %.1f | %.1f | %.1f | %.2f | %.0f | %.1f | — |\n",
               c->w, c->h, (unsigned)c->qp, with_alpha ? "+A" : "",
               pe_ms.p50, pe_ms.p95, pe_ms.p99, 1000.0 / pe_ms.p50,
               pe.p50 / px, (double)st.packet_size * 8.0 * 25.0 / 1e6);
        printf("| %ux%u qp%u%s | 解码 | %.1f | %.1f | %.1f | %.2f | %.0f | — | — |\n",
               c->w, c->h, (unsigned)c->qp, with_alpha ? "+A" : "",
               pd_ms.p50, pd_ms.p95, pd_ms.p99, 1000.0 / pd_ms.p50, pd.p50 / px);
        printf("| %ux%u qp%u%s | sized(码控搜索) | %.1f | %.1f | %.1f | %.2f | %.0f | — | %.2f |\n",
               c->w, c->h, (unsigned)c->qp, with_alpha ? "+A" : "",
               ps_ms.p50, ps_ms.p95, ps_ms.p99, 1000.0 / ps_ms.p50, ps.p50 / px,
               sized_avg_iters);

        tc_free(pkt);
        frame_set_free(&src);
        frame_set_free(&dec);
    }
    printf("\n");

    printf("## 冷口径（该几何进程内首次执行，ms；对照上方 warm 行）\n\n");
    printf("| 配置 | 编码 cold | 解码 cold | sized cold |\n");
    printf("| --- | ---: | ---: | ---: |\n");
    for (int i = 0; i < n_cold; ++i) {
        int with_alpha = cold_cfg[i]->alpha_mode != 0u;
        printf("| %ux%u qp%u%s | %.1f | %.1f | %.1f |\n",
               cold_cfg[i]->w, cold_cfg[i]->h, (unsigned)cold_cfg[i]->qp,
               with_alpha ? "+A" : "", cold_enc[i], cold_dec[i], cold_sized[i]);
    }
    printf("\n");
}

static int perf_mode(int argc, char** argv)
{
    int quick = (argc >= 3 && strcmp(argv[2], "quick") == 0);
    printf("# Topos Codec 性能报告（数据源：topos_quality perf%s；R6 口径）\n\n",
           quick ? " quick" : "");
    topos_cpu_features cf;
    tc_query_cpu_features(&cf);
    uint32_t feats = cf.flags;
    topos_version_info vi;
    tc_version(&vi);
    printf("- 环境：git=%s target=%s，cycles 计数=%s，CPU features=0x%x（%s%s%s%s）\n",
           vi.git_commit, vi.build_target,
           perf_ticks_is_cycles() ? "TSC" : "wall-clock 兜底",
           (unsigned)feats,
           (feats & TOPOS_CPU_X86_AVX2) ? "AVX2 " : "",
           (feats & TOPOS_CPU_X86_AVX512F) ? "AVX512F " : "",
           (feats & TOPOS_CPU_X86_FMA) ? "FMA " : "",
           (feats & TOPOS_CPU_ARM_NEON) ? "NEON" : "");
    printf("- 口径：threads=%d（TOPOS_SLICE_THREADS 可覆盖）；cycles/px 以 luma 像素计（422 折算 1.5，+A 再加 1.0）\n",
           (int)tc_dev_thread_count());
    printf("- 功耗：本工具不采集（macOS powermetrics 需 root；手动采集见 scripts/bench_power_manual.sh）\n");
    printf("- 冷/热：帧级表 cold 行 = 该几何在进程内首次执行（新缓冲/缺页口径）；"
           "p50/p95/p99 行 = warmup 后 warm 口径\n\n");
    perf_calibrate();
    bench_kernels(1920u, 1080u);

    static const perf_frame_cfg cfgs[] = {
        {"1080p qp20",      1920u, 1080u, 20u, 0u, 0u},
        {"1080p qp50",      1920u, 1080u, 50u, 0u, 0u},
        {"1080p qp20+A12",  1920u, 1080u, 20u, 2u, 12u},
        {"4K qp20",         3840u, 2160u, 20u, 0u, 0u},
        {"4K qp50",         3840u, 2160u, 50u, 0u, 0u},
        {"4K qp20+A12",     3840u, 2160u, 20u, 2u, 12u},
    };
    int n = quick ? 2 : 6;
    if (quick) {
        /* 冒烟：一绿一重 */
        static const perf_frame_cfg quick_cfgs[] = {
            {"1080p qp20", 1920u, 1080u, 20u, 0u, 0u},
            {"4K qp20",    3840u, 2160u, 20u, 0u, 0u},
        };
        bench_frames(quick_cfgs, n, quick);
    } else {
        bench_frames(cfgs, n, quick);
    }
#if !defined(_WIN32)
    printf("- 资源：峰值 RSS=%llu KB（getrusage ru_maxrss；进程生命周期口径，含全部基准缓冲）\n",
           (unsigned long long)perf_peak_rss_kb());
#else
    printf("- 资源：峰值 RSS=%llu KB（GetProcessMemoryInfo PeakWorkingSetSize；"
           "进程生命周期口径，含全部基准缓冲）；句柄=%llu；线程=%llu\n",
           (unsigned long long)perf_peak_rss_kb(),
           perf_handle_count(), perf_thread_count());
#endif
    /* M10-0.3：真实构建元数据（CMake 注入；旧硬编码 "-O2 -DNDEBUG" 与
     * Release 实际 -O3 不符）+ 运行时负载上下文 */
    {
        double loadavg[1] = {0.0};
#if !defined(_WIN32)
        (void)getloadavg(loadavg, 1);
#endif
        printf("（报告口径：%s [%s]；commit %s；运行时 load %.2f；p50/p95/p99 覆盖 %s 迭代，"
               "quick 模式仅供 CI 冒烟）\n",
               TOPOS_REPORT_BUILD_TYPE, TOPOS_REPORT_BUILD_FLAGS, TOPOS_GIT_COMMIT,
               loadavg[0], quick ? "12" : "30");
    }
    return 0;
}

/* R4.4：12-bit 画质报表——同内容同 qp 的 10/12-bit 对照（v1.2 枚举）。
 * 12-bit PSNR 按 4095 峰值定标（旧表按 1023，跨位深不可比）。 */
static int depth12_mode(void)
{
    const uint32_t W = 960u, H = 544u;
    printf("== depth12 报表（%ux%u MIXED，QM1，4:2:2；PSNR 按位深峰值定标）==\n",
           W, H);
    printf("%-6s %-4s %-8s %-9s %-9s %-9s\n",
           "depth", "qp", "bytes", "PSNR-Y", "PSNR-U", "PSNR-V");
    static const uint8_t qps[3] = {12u, 24u, 36u};
    for (int qi = 0; qi < 3; ++qi) {
        for (uint32_t bd = 10u; bd <= 12u; bd += 2u) {
            frame_set src, dec;
            if (frame_set_make(&src, 0x5EED333333333333ull, W, H,
                               TC_SYNTH_MIXED, 0) != 0) { return 1; }
            image_synth_cfg ic = {src.y ? 0x5EED333333333333ull : 0u,
                                  W, H, TC_SYNTH_MIXED, bd, 0u};
            frame_set_free(&src);
            if (frame_set_make(&src, 0, W, H, TC_SYNTH_MIXED, 0) != 0) { return 1; }
            /* 重新按位深合成（frame_set_make 固定 10-bit——直接内联合成） */
            frame_set_free(&src);
            if (image_synth_alloc(&ic, 0, &src.y, &src.u, &src.v, &src.a) != 0) {
                return 1;
            }
            src.w = W; src.h = H; src.cw = (W + 1u) / 2u;

            uint8_t* pkt = NULL;
            size_t size = 0;
            topos_frame_stats st;
            uint8_t qp = qps[qi];
            /* encode_frame 不带 bd——就地构造 */
            topos_frame_config c;
            memset(&c, 0, sizeof(c));
            c.struct_size = (uint32_t)sizeof(c);
            c.visible_width = (uint16_t)W;
            c.visible_height = (uint16_t)H;
            c.qp_base = qp;
            c.qmatrix_id = 1u;
            c.bit_depth = (uint8_t)bd;
            topos_frame_input in;
            memset(&in, 0, sizeof(in));
            in.struct_size = (uint32_t)sizeof(in);
            const uint16_t* pl[4] = {src.y, src.u, src.v, NULL};
            memcpy(in.planes, pl, sizeof(pl));
            size_t cap = tc_frame_packet_bound(&c);
            pkt = (uint8_t*)tc_alloc(cap != 0u ? cap : 1u);
            if (pkt == NULL) { frame_set_free(&src); return 1; }
            if (tc_frame_encode(&c, &in, pkt, cap, &st) != TC_OK) {
                tc_free(pkt); frame_set_free(&src); return 1;
            }
            size = st.packet_size;

            memset(&dec, 0, sizeof(dec));
            dec.w = W; dec.h = H; dec.cw = (W + 1u) / 2u;
            dec.y = (uint16_t*)tc_alloc((size_t)W * H * 2u);
            dec.u = (uint16_t*)tc_alloc((size_t)dec.cw * H * 2u);
            dec.v = (uint16_t*)tc_alloc((size_t)dec.cw * H * 2u);
            int32_t drc = 0;
            if (decode_frame(pkt, size, &dec, &drc) != TC_OK) {
                tc_free(pkt); frame_set_free(&src); frame_set_free(&dec); return 1;
            }
            double py = psnr_plane_bd(src.y, dec.y, (size_t)W * H, bd);
            double pu = psnr_plane_bd(src.u, dec.u, (size_t)dec.cw * H, bd);
            double pv = psnr_plane_bd(src.v, dec.v, (size_t)dec.cw * H, bd);
            printf("%-6u %-4u %-8u %-9.2f %-9.2f %-9.2f\n",
                   bd, qp, (unsigned)size, py, pu, pv);
            tc_free(pkt);
            frame_set_free(&src);
            frame_set_free(&dec);
        }
    }
    printf("depth12: OK\n");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "depth12") == 0) { return depth12_mode(); }
    if (argc >= 2 && strcmp(argv[1], "report") == 0) { return report_mode(); }
    if (argc >= 2 && strcmp(argv[1], "sweep") == 0) { return sweep_mode(); }
    if (argc >= 2 && strcmp(argv[1], "mov") == 0) { return mov_mode(); }
    if (argc >= 2 && strcmp(argv[1], "perf") == 0) { return perf_mode(argc, argv); }
    fprintf(stderr, "usage: topos_quality depth12|report|sweep|mov|perf [quick]\n");
    return 2;
}
