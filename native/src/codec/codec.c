/* codec.c —— 完整标量 codec（阶段 4，spec §5/§7/§8/§9/§11）。
 *
 * encode：plane padding（§5 边缘复制）→ level shift → 整数 DCT → deadzone 量化
 *         → Rice k 估计（输入的纯函数）→ 符号化/熵编码 → slice 组包
 *         → frame header（CRC）→ 结构自检（tc_packet_scan 必须通过）。
 * decode：packet_scan（整帧拒绝路径）→ 逐 slice 流式解码（不可信带高不放大分配）
 *         → 反量化 → 逆 DCT → 重建钳位（§7.7）→ concealment（§9）→ crop 到 visible。
 *
 * 确定性（§11.1）：无墙钟/线程/浮点依赖；相同输入 + 配置 → bit-exact 输出。
 * 失败路径零泄漏：所有早退统一走 cleanup（ASan 门禁强制）。
 */
#include "common/alloc.h"
#include "codec.h"
#include "ctx_ceiling.h"
#include <stdio.h>

/* 分配尺寸实测 shim（N03 槽位核对用）：返回分配块的可用字节数，
 * 三平台语义一致（均 ≥ 请求大小）：macOS malloc_size、Windows _msize、
 * glibc malloc_usable_size。 */
#if defined(__APPLE__)
#include <malloc/malloc.h>
static size_t tc_malloc_size(void* p) { return malloc_size(p); }
#elif defined(_WIN32)
#include <malloc.h>
static size_t tc_malloc_size(void* p) { return _msize(p); }
#else
#include <malloc.h>
static size_t tc_malloc_size(void* p) { return malloc_usable_size(p); }
#endif

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h> /* _BitScanReverse64（tc_log2_e8_u64） */
#endif
#include "intra.h"
#include "deblock.h"
#include "v7_scalable.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../bitstream/bitio.h"
#include "../bitstream/band_tokens.h"
#include "../bitstream/band_codec.h"
#include "../bitstream/frame_header.h"
#include "../bitstream/layer_directory.h"
#include "../bitstream/v7_frame_codec.h"
#include "../bitstream/packet.h"
#include "../bitstream/slice_codec.h"
#include "../entropy/vlc.h"
#include "../bitstream/slice_map.h"
#include "../common/checked.h"
#include "../common/crc32.h"
#include "../common/port_mutex.h"
#include "../common/tpool.h"
#include "../common/error.h"
#include "../entropy/block_coding.h"
#include "../entropy/rans.h"
#include "../entropy/rice.h"
#include "../entropy/scan.h"
#include "../simd/dispatch.h"
#include "../transform/plane.h"
#include "../transform/quant.h"
#include "../transform/transform.h"
#include "color_store.h"

#if defined(_WIN32)
#include <windows.h>
#endif

/* M4：重建存储 SSE2 内核（x86_64 恒可用；ARM 走标量回退） */
#if defined(__x86_64__) || defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#define TOPOS_SINK_SSE2 1
#endif

/* ---- M0：解码阶段观测（TOPOS_CODEC_PROFILE=1 开启；默认零开销） ---- */

uint64_t tc_profile_now_ns(void)
{
#if defined(_WIN32)
    LARGE_INTEGER c, f;
    if (QueryPerformanceCounter(&c) && QueryPerformanceFrequency(&f) && f.QuadPart > 0) {
        return (uint64_t)(c.QuadPart * 1000000000ull / f.QuadPart);
    }
    return 0u;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0u; }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

int tc_profile_enabled(void)
{
    /* P1-06：首触惰性读 env——原子化消除并发首触的 C 数据竞争
     * （两线程同时写同一结果值仍是 UB；relaxed 足够，值幂等）。 */
    static atomic_int cached = -1;
    int v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v < 0) {
        const char* e = getenv("TOPOS_CODEC_PROFILE");
        v = (e != NULL && e[0] == '1' && e[1] == '\0') ? 1 : 0;
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return v;
}

/* P1-06：dev 阶段统计的全局累加器保护——并发 encode/decode/stats
 * get/reset 时无数据竞争。全部访问点均已被 prof 开关门控（生产热路径
 * 零开销：prof=0 时不进锁）；锁本身为平台垫片静态初始化互斥锁。 */
static topos_mutex g_stats_mu = TOPOS_MUTEX_INIT;
#define TC_STATS_LOCK() topos_mutex_lock(&g_stats_mu)
#define TC_STATS_UNLOCK() topos_mutex_unlock(&g_stats_mu)

static tc_decode_stage_stats g_dec_stats; /* worker 经任务局部累积，汇合后统一
                                             加总（parallel_for 返回后无竞争） */
#define TC_DECODE_LATENCY_SAMPLES 256u
static uint64_t g_dec_latency[TC_DECODE_LATENCY_SAMPLES];
static uint32_t g_dec_latency_count;
static uint32_t g_dec_latency_next;

static int decode_latency_cmp(const void* a, const void* b)
{
    const uint64_t x = *(const uint64_t*)a;
    const uint64_t y = *(const uint64_t*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static uint64_t decode_latency_percentile(uint64_t* values, uint32_t count,
                                          uint32_t percentile)
{
    if (count == 0u) { return 0u; }
    qsort(values, count, sizeof(values[0]), decode_latency_cmp);
    const uint32_t index = ((count - 1u) * percentile) / 100u;
    return values[index];
}

void tc_dev_decode_stats_reset(void)
{
    topos_mutex_lock(&g_stats_mu);
    memset(&g_dec_stats, 0, sizeof(g_dec_stats));
    memset(g_dec_latency, 0, sizeof(g_dec_latency));
    g_dec_latency_count = 0u;
    g_dec_latency_next = 0u;
    topos_mutex_unlock(&g_stats_mu);
}

void tc_dev_decode_stats_get(tc_decode_stage_stats* out)
{
    if (out == NULL) { return; }
    uint64_t latency[TC_DECODE_LATENCY_SAMPLES];
    uint32_t latency_count = 0u;
    topos_mutex_lock(&g_stats_mu);
    *out = g_dec_stats;
    latency_count = g_dec_latency_count;
    memcpy(latency, g_dec_latency, sizeof(latency));
    topos_mutex_unlock(&g_stats_mu);
    out->p50_ns = decode_latency_percentile(latency, latency_count, 50u);
    out->p95_ns = decode_latency_percentile(latency, latency_count, 95u);
    out->p99_ns = decode_latency_percentile(latency, latency_count, 99u);
}

void tc_dev_decode_stats_add_scan(uint64_t ns)
{
    topos_mutex_lock(&g_stats_mu);
    g_dec_stats.scan_ns += ns;
    topos_mutex_unlock(&g_stats_mu);
}

void tc_dev_decode_stats_add_wall(uint64_t ns)
{
    topos_mutex_lock(&g_stats_mu);
    g_dec_stats.wall_ns += ns;
    g_dec_latency[g_dec_latency_next] = ns;
    g_dec_latency_next = (g_dec_latency_next + 1u) % TC_DECODE_LATENCY_SAMPLES;
    if (g_dec_latency_count < TC_DECODE_LATENCY_SAMPLES) { g_dec_latency_count++; }
    topos_mutex_unlock(&g_stats_mu);
}

void tc_dev_decode_stats_add_v7b(uint64_t bytes_read, uint64_t base_bytes_read,
                                 uint32_t segments_read,
                                 uint32_t segments_skipped)
{
    if (!tc_profile_enabled()) { return; }
    const uint64_t residual_bytes = bytes_read >= base_bytes_read
        ? bytes_read - base_bytes_read : 0u;
    topos_mutex_lock(&g_stats_mu);
    g_dec_stats.packet_bytes_read += residual_bytes;
    g_dec_stats.segments_parsed += (uint64_t)segments_read;
    g_dec_stats.segments_skipped += (uint64_t)segments_skipped;
    topos_mutex_unlock(&g_stats_mu);
}

/* ---- M6：编码阶段观测（同模式：任务局部计时，join 后求和） ---- */

static tc_encode_stage_stats g_enc_stats;

void tc_dev_encode_stats_reset(void)
{
    topos_mutex_lock(&g_stats_mu);
    memset(&g_enc_stats, 0, sizeof(g_enc_stats));
    topos_mutex_unlock(&g_stats_mu);
}

void tc_dev_encode_stats_get(tc_encode_stage_stats* out)
{
    if (out == NULL) { return; }
    topos_mutex_lock(&g_stats_mu);
    *out = g_enc_stats;
    topos_mutex_unlock(&g_stats_mu);
}

/* bit_depth ∈ {10,12}（R4.1，v1.2 枚举扩展）：level shift 中值/上限按位深
 * 推导。变换/量化整数界自 ADR-C002 起按 12-bit（|x'| ≤ 2047）设计，无需改。 */
static uint32_t bd_mid(uint8_t bd) { return 1u << (bd - 1u); }
static uint32_t bd_max(uint8_t bd) { return (1u << bd) - 1u; }

/* 12-bit 等效量化偏移（spec §A.4 v1.2）：qp_scale 每 4 qp 翻倍，位深 +2 →
 * 系数幅度 ×2 → qp_eff +4 保持同 qp 的相对量化精度/码率。仅编码侧计入
 * （qp_eff 经 slice qp_delta 完整入流，解码侧照读，无需位深知识）。 */
/* 12-bit 等效量化偏移（spec §A.4 v1.2）：qp_scale 每 4 qp 翻倍，幅度比
 * 2^(bd−10) → 偏移 = 2×(bd−10)。bd10→0 / bd12→4（历史逐位不变）/
 * bd16→12（批 4；解析式同式外推）。无损域核对：qm0（flat16）Q=1 保持到
 * qp_eff ≤ 22 → qp_base=0 的无损语义在 +12 下不受损；变换归一化
 * （EuEv ≈ 10816）使量化误差落到像素域 <1 码 → qp36 域实测像素精确
 * （批 0 试点 + 2026-09-13 CLI 复测一致）。RD 最优值由 raw16 锚点标定
 * 扫偏移实测复核（计划 §批 4）。 */
static int32_t bd_qp_offset(uint8_t bd) { return bd >= 12u ? 2 * (int32_t)(bd - 10) : 0; }

/* —— pf=3（TRAW CFA）平面语义辅助（topos_traw_format_plan 批 1）——
 * 4 个相位平面 R/Gr/Gb/B 全是"类亮度"单色平面：无色度语义（qm/qp delta/
 * RDO λ/AQ 色度路径一律不走），且平面 3 不再隐含 alpha（alpha 与 CFA
 * 互斥，由 frame header 交叉规则保证）。对 pf∈{0,1,2} 三个谓词与历史
 * 行为逐位等价——既有 golden SHA 不受影响。 */
static int is_chroma_plane(const topos_frame_header* fh, uint32_t p)
{
    return fh->pixel_format != 3u && (p == 1u || p == 2u);
}
static int is_alpha_plane(const topos_frame_header* fh, uint32_t p)
{
    return p == 3u && fh->alpha_mode != 0u;
}
/* 颜色平面数（历史 `3u` 的语义化：带 alpha 时 3 色平面 + plane3=alpha；
 * pf=3 无 alpha，plane_count=4 全为颜色平面） */
static uint32_t fh_color_planes(const topos_frame_header* fh)
{
    return (fh->alpha_mode != 0u) ? 3u : fh->plane_count;
}

/* ============================ dev 量化表覆盖 ============================ */

static _Atomic(const tc_qmatrix_set*) g_dev_qm; /* P1-06：dev 开关原子化 */

void tc_dev_set_qmatrix_override(const tc_qmatrix_set* qms)
{
    atomic_store_explicit(&g_dev_qm, qms, memory_order_relaxed);
}

static const tc_qmatrix_set* lookup_qm(uint8_t id)
{
    const tc_qmatrix_set* dev = atomic_load_explicit(&g_dev_qm, memory_order_relaxed);
    if (dev != NULL) { return dev; }
    return tc_qmatrix_by_id(id);
}

/* ============================ 配置 → frame header ============================ */

static uint32_t pad8_u32(uint32_t v) { return (v + 7u) / 8u * 8u; }

/* 0 值字段 → 默认（公共头约定：全 0 初始化即合法最小配置） */
/* v1.x 扩展代 minor 选择（写侧单一真相源）：
 * v1.2（R4.1）bd=12 → 1；v1.3（R4.2）pf=1 → 2；v1.4（R4.3）pf=2 → 3；
 * v1.0 语义流恒 0（旧 golden 逐字节不变；旧解码器按未知 minor 干净拒绝，
 * §13.1）。v1.5（ADR-C031）：qp_base ≥ 64 的粗量化域（Proxy/LT 低码率档）
 * → minor=4；qp≤63 流 minor 不变，字节完全向后兼容。
 * V2 恒 0（ADR-C027 D-6；qp 域开放由 major=2 表达）。
 * m7_final 在 sized 搜索选定 qp 后直接改 fh.qp_base，必须经此重算。 */
static void enc_fh_select_minor(topos_frame_header* fh)
{
    if (fh->version_major >= 2u) { fh->version_minor = 0u; return; }
    /* bd16 的扩展代高于所有像素布局；先选 pf 会把 GBR/YUV444
     * 16-bit 错写成 minor=3/2，mux 在 open 时即按版本交叉校验拒绝。 */
    fh->version_minor = (fh->bit_depth == 16u) ? 6u
                        : (fh->pixel_format == 3u) ? 5u
                        : (fh->pixel_format == 2u) ? 3u
                        : (fh->pixel_format == 1u) ? 2u
                        : (fh->bit_depth != 10u) ? 1u : 0u;
    if (fh->qp_base >= TC_QP_V15_MIN && fh->version_minor < 4u) {
        fh->version_minor = 4u;
    }
    /* H1：transfer=13（sRGB EOTF）须 minor≥7 载体（v1.8 扩展代） */
    if (fh->color_transfer == 13u && fh->version_minor < 7u) {
        fh->version_minor = 7u;
    }
}

static void cfg_to_frame_header(const topos_frame_config* cfg, topos_frame_header* fh)
{
    memset(fh, 0, sizeof(*fh));
    fh->flags = cfg->alpha_premultiplied != 0u ? (uint16_t)1u : (uint16_t)0u;
    /* V7-R4/R5 输出特性（em8 V7-R2 产品 intra + reserved[5]；域校验见
     * validate）：bit2=DEBLOCK、bit3=QPT2 细化表。旧解码器对保留 flags
     * 位非零干净拒绝。 */
    if ((cfg->reserved[0] & 0xFFu) == 8u && cfg->reserved[5] != 0u) {
        fh->flags = (uint16_t)(fh->flags
                               | ((cfg->reserved[5] & 1u) != 0u ? 0x0004u : 0u)
                               | ((cfg->reserved[5] & 2u) != 0u ? 0x0008u : 0u));
    }
    /* pf=3（TRAW）默认 profile 7（交叉规则要求）；其余保持 3 默认 */
    fh->profile = cfg->profile != 0u ? cfg->profile
                  : (cfg->pixel_format == 3u ? 7u : 3u);
    fh->pixel_format = cfg->pixel_format;  /* 0=4:2:2 默认；1=4:4:4；2=GBR；3=CFA */
    fh->bit_depth = cfg->bit_depth != 0u ? cfg->bit_depth : 10u;
    /* R4.3：cfg.matrix=0 = "默认"——按 pf 取默认矩阵（GBR/CFA → 0/identity，
     * YUV → 1/bt709）；显式 YUV 矩阵 + pf∈{2,3} 由 header 校验交叉规则拒绝 */
    fh->color_matrix = cfg->color_matrix != 0u ? cfg->color_matrix
                       : (fh->pixel_format == 2u || fh->pixel_format == 3u ? 0u : 1u);
    /* v1.2..v1.6 扩展代 minor 选择见 enc_fh_select_minor（m7_final 复用） */
    fh->qp_base = cfg->qp_base;
    enc_fh_select_minor(fh);
    fh->alpha_mode = cfg->alpha_mode;
    fh->alpha_bit_depth = cfg->alpha_bit_depth;
    fh->frame_type = 0u;
    fh->coded_width = (uint16_t)pad8_u32(cfg->visible_width);
    fh->coded_height = (uint16_t)pad8_u32(cfg->visible_height);
    fh->visible_width = cfg->visible_width;
    fh->visible_height = cfg->visible_height;
    /* TRAW CFA 恒 4 平面无 alpha（header 交叉规则兜底拒绝非法组合） */
    fh->plane_count = (uint8_t)(cfg->pixel_format == 3u
                                    ? 4u
                                    : 3u + (cfg->alpha_mode != 0u ? 1u : 0u));
    fh->qmatrix_id = cfg->qmatrix_id;                /* 0 = flat = 默认 */
    fh->qp_base = cfg->qp_base;
    fh->color_range = cfg->color_range;
    fh->color_primaries = cfg->color_primaries != 0u ? cfg->color_primaries : 1u;
    fh->color_transfer = cfg->color_transfer != 0u
                             ? cfg->color_transfer
                             : (cfg->pixel_format == 3u
                                    ? (cfg->bit_depth == 16u ? 8u
                                                             : TC_TRANSFER_TRAW_LOG0)
                                    : 13u);
    fh->chroma_siting = cfg->chroma_siting;
    /* H1：transfer 默认改派 sRGB(13) 在 select_minor 之后生效——重算
     * minor（幂等；m7_final 同款复用） */
    enc_fh_select_minor(fh);
    fh->sar_num = cfg->sar_num;
    fh->sar_den = cfg->sar_den;
    /* M9（ADR-C027）→ V 代际收纳（2026-09-13）→ V9（topos_v9_micro_gop_plan
     * 批 1）：reserved[0] 低字节 = 熵模式开关，写域 {0=V1 流（位流与旧版
     * 逐字节一致）, 1=V2+VLC, 8=V7-R2, 9=V8, 10=V9}；em∈{2..7} 已在 cfg
     * 域校验拒绝（em 编号封存），对应位流代际（major 3/4/5/6、(2,0)、
     * (7,6)）仅解码。 */
    uint8_t em = (uint8_t)(cfg->reserved[0] & 0xFFu);
    if (em == 8u) {
        /* V7-R2（ADR-C036）：V7-R + order-1 上下文扩展（lvl 族条件模型 +
         * per-slice 信令；slice k1/k2/k3 恒 0） */
        fh->version_major = 7u;
        fh->entropy_mode = 7u;
    } else if (em == 9u) {
        /* V8（topos_v8_format_plan 批 1）：段化多链 rANS + 瓦片共享表。 */
        fh->version_major = 8u;
        fh->entropy_mode = 8u;
    } else if (em == 10u) {
        /* V9（topos_v9_micro_gop_plan 批 1；ADR-C047 zero-motion IP-2）：
         * 包布局与 V8 同构（major/熵三元组区分），P 帧编码残差伪图。
         * GOP 序列输入 = reserved[3]/[4]/[5]（frame_type/gop_id/
         * ref_distance，域已校验）。编码/解码像素路径批 1 占位
         * NOT_IMPLEMENTED（批 2 接线残差路径 + recon 链）。 */
        fh->version_major = 9u;
        fh->entropy_mode = 9u;
        fh->frame_type = (uint8_t)cfg->reserved[3];
        fh->gop_id = (uint16_t)cfg->reserved[4];
        fh->ref_distance = (uint8_t)cfg->reserved[5];
    } else if (em == 11u) {
        /* V7-R3（ADR-C048，LP 载体更换）：V7-R2 同构子代际承载帧间微
         * GOP——(major 7, em 8)，V2.1 占位字节激活为 GOP 字段（规则组
         * 与 V9 §3.2 同构）。像素机 = 现行 V7 band 并行机器（编码/解码
         * 分派点按 entropy 8 跟随 V7-R2）。 */
        fh->version_major = 7u;
        fh->entropy_mode = 8u;
        fh->frame_type = (uint8_t)cfg->reserved[3];
        fh->gop_id = (uint16_t)cfg->reserved[4];
        fh->ref_distance = (uint8_t)cfg->reserved[5];
    } else if (em == 1u) {
        fh->version_major = 2u;
        fh->entropy_mode = 1u;
        fh->codebook_version = TC_VLC_CODEBOOK_VERSION;
    } else {
        fh->version_major = 1u;
    }
    if (fh->version_major >= 2u) {
        /* V2 minor 恒 0（ADR-C027 D-6；pf/bd 全枚举开放，无 v1.x 扩展代） */
        enc_fh_select_minor(fh);
    }
}

static uint32_t cfg_slice_rows(const topos_frame_config* cfg)
{
    return cfg->slice_rows != 0u ? (uint32_t)cfg->slice_rows : TC_FRAME_DEFAULT_SLICE_ROWS;
}

/* 每 plane 带数（须已 derive_geometry）。bands_out 可 NULL。 */
static int32_t frame_band_plan(const topos_frame_header* fh, uint32_t slice_rows,
                               uint32_t bands_out[4], uint32_t* total_slices)
{
    uint64_t total = 0u;
    for (uint32_t p = 0u; p < fh->plane_count; ++p) {
        uint64_t rows = fh->plane_block_rows[p];
        uint64_t bands = (rows + (uint64_t)slice_rows - 1u) / (uint64_t)slice_rows;
        if (bands == 0u) { bands = 1u; }
        if (bands_out != NULL) { bands_out[p] = (uint32_t)bands; }
        total += bands;
    }
    if (total > (uint64_t)TC_MAX_SLICE_COUNT) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "slice_rows %u yields %llu slices > %u",
                     (unsigned)slice_rows, (unsigned long long)total,
                     (unsigned)TC_MAX_SLICE_COUNT);
        return TC_ERR_LIMIT_EXCEEDED;
    }
    if (total_slices != NULL) { *total_slices = (uint32_t)total; }
    return TC_OK;
}

int32_t tc_frame_config_validate(const topos_frame_config* cfg)
{
    if (cfg == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "cfg == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (cfg->struct_size != 0u && cfg->struct_size != (uint32_t)sizeof(topos_frame_config)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "cfg struct_size %u != %u",
                     (unsigned)cfg->struct_size, (unsigned)sizeof(topos_frame_config));
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (cfg->abi_version != 0u && cfg->abi_version != (uint32_t)TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "cfg abi_version %u unsupported",
                     (unsigned)cfg->abi_version);
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (cfg->visible_width == 0u || cfg->visible_height == 0u ||
        cfg->visible_width > TC_MAX_CODED_DIM || cfg->visible_height > TC_MAX_CODED_DIM) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "visible %ux%u out of 1..16384",
                     (unsigned)cfg->visible_width, (unsigned)cfg->visible_height);
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (cfg->qp_base > TC_QP_MAX) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "qp_base %u > %u",
                     (unsigned)cfg->qp_base, (unsigned)TC_QP_MAX);
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (cfg->reserved[0] > 11u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "reserved[0] coding selection %u not in {0..11}",
                     (unsigned)cfg->reserved[0]);
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* V 代际收纳（2026-09-13）：写域收缩为 {0=V1, 1=V2-VLC, 8=V7-R2,
     * 9=V8}；V9（topos_v9_micro_gop_plan 批 1）增补 em=10。em∈{2..7}
     * （V2-Rice/V3/V4/V5/V6/V7-R）退役——显式拒绝，不静默回落；退役
     * 代际仅保留解码（读端 UNSUPPORTED_VERSION，见 frame_header 接受
     * 清单），em 编号永久封存不复用。 */
    if (cfg->reserved[0] >= 2u && cfg->reserved[0] <= 7u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "reserved[0] coding selection %u retired "
                     "(V consolidation 2026-09-13, see ADR-C0xx); "
                     "write domain is {0,1,8,9,10}",
                     (unsigned)cfg->reserved[0]);
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (cfg->reserved[1] > 1u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "reserved[1] AQ mode %u not in {0,1}",
                     (unsigned)cfg->reserved[1]);
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (cfg->reserved[2] > 1u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "reserved[2] RDO mode %u not in {0,1}",
                     (unsigned)cfg->reserved[2]);
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* P1-10：reserved 不承载功能的位必须为 0——否则未来启用新配置时旧
     * 调用方的误用会被静默吞掉（V2 以 reserved[0] 启用是过渡期载体）。
     * V8（em==9）例外：reserved[3]/[4] = 段/瓦片粒度（批 2，域校验在
     * enc_frame_setup）；V9（em==10）例外：reserved[3]/[4]/[5] =
     * frame_type/gop_id/ref_distance（GOP 序列输入，计划 §3.2；
     * frame_type/ref_distance 域校验在下方 V9 专属块）。 */
    {
        const uint8_t em_lo = (uint8_t)(cfg->reserved[0] & 0xFFu);
        /* em8（V7-R2 产品 intra）：reserved[3]/[4]=段/瓦片粒度（批 2 未
         * 启用，恒 0），reserved[5]=输出特性（V7-R4/R5，2026-09-21 锯齿
         * 战役 P6）；em10/11：GOP 三槽；em9（V8 段化试验，sized 未接入）：
         * [3]/[4] 粒度槽、[5..7] 恒 0。 */
        const int rsv_end = em_lo >= 10u ? 6 : (em_lo == 9u ? 5 : 3);
        for (int i = rsv_end; i < 8; ++i) {
            if (em_lo == 8u && i == 5) { continue; } /* 特性槽单独校验 */
            if (cfg->reserved[i] != 0u) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT,
                             "reserved[%d] must be 0 (got %u)",
                             i, (unsigned)cfg->reserved[i]);
                return TC_ERR_INVALID_ARGUMENT;
            }
        }
        if (em_lo == 8u && cfg->reserved[5] != 0u) {
            /* 输出特性 bit0=DEBLOCK（输出去块）/ bit1=QPT2（细化 qp 表），
             * 位流侧经 fh->flags bit2/bit3 选载——旧解码器对保留 flags 位
             * 非零干净拒绝（V2 规则组），满足"不可静默误读"。域与
             * qmatrix_id=4 同形：产品 4:2:2 10-bit 路径。 */
            const uint32_t feats = cfg->reserved[5];
            if ((feats & ~3u) != 0u || cfg->profile != 3u ||
                cfg->pixel_format != 0u || cfg->bit_depth != 10u) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT,
                             "reserved[5] output features %u require profile 3"
                             " / 4:2:2 / 10-bit (bitmask within {0..3})",
                             (unsigned)feats);
                return TC_ERR_INVALID_ARGUMENT;
            }
        }
        if (em_lo == 10u || em_lo == 11u) {
            /* V9 / V7-R3（ADR-C048）：frame_type 0=I/1=P；ref_distance
             * I=0/P=1（头域强校验同规则；此处提前给 cfg 域错误）。
             * gop_id 全域 u16。 */
            if (cfg->reserved[3] > 1u) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT,
                             "temporal frame_type %u not in {0,1}",
                             (unsigned)cfg->reserved[3]);
                return TC_ERR_INVALID_ARGUMENT;
            }
            const uint8_t want_ref = cfg->reserved[3] == 0u ? 0u : 1u;
            if (cfg->reserved[5] != want_ref) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT,
                             "temporal ref_distance %u != %u for frame_type %u",
                             (unsigned)cfg->reserved[5], (unsigned)want_ref,
                             (unsigned)cfg->reserved[3]);
                return TC_ERR_INVALID_ARGUMENT;
            }
        }
    }
    if (cfg->alpha_mode > 2u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "alpha_mode %u > 2", (unsigned)cfg->alpha_mode);
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (cfg->alpha_mode == 1u && cfg->alpha_bit_depth != 16u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "lossless alpha requires bit_depth 16");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (cfg->alpha_mode == 2u &&
        cfg->alpha_bit_depth != 8u && cfg->alpha_bit_depth != 10u &&
        cfg->alpha_bit_depth != 12u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "near-lossless alpha bit_depth %u not in {8,10,12}",
                     (unsigned)cfg->alpha_bit_depth);
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* 批 4 阶段 2：bd16 编码面已随融合前向 i32 重做解锁（i32 前向
     * AVX2/NEON/SCALAR 三路 + 量化 exact_div 真除法 + 宽域反量化/逆变换
     * 标量 compose）；SIMD 量化/融合内核为 2^25 域，bd≥13 经 resolve
     * wide 门控走标量/宽路（dispatch.h）。 */
    if (cfg->pixel_format == 3u) {
        /* TRAW（批 1）：alpha 与 CFA 互斥；qp_delta_chroma 无色度语义——
         * 非 0 显式拒绝（不静默忽略，防止调用方误用）。 */
        if (cfg->alpha_mode != 0u) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT,
                         "TRAW (pixel_format=3) does not support alpha");
            return TC_ERR_INVALID_ARGUMENT;
        }
        if (cfg->qp_delta_chroma != 0) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT,
                         "qp_delta_chroma has no chroma semantics for TRAW (must be 0)");
            return TC_ERR_INVALID_ARGUMENT;
        }
    }

    /* 其余字段规则（颜色 tag、profile、pixfmt…）复用 header 校验，单一事实源。
     * frame_packet_size 尚未知：以 header 大小占位通过该条规则（终值由组包后
     * 写入并经 self-check 的 tc_packet_scan 再校验）。 */
    topos_frame_header fh;
    cfg_to_frame_header(cfg, &fh);
    int32_t rc = tc_frame_derive_geometry(&fh);
    if (rc != TC_OK) { return rc; }
    rc = frame_band_plan(&fh, cfg_slice_rows(cfg), NULL, NULL);
    if (rc != TC_OK) { return rc; }
    fh.slice_count = (uint16_t)(cfg->pixel_format == 3u
                                    ? 4u
                                    : 3u + (cfg->alpha_mode != 0u ? 1u : 0u)); /* ≥ plane_count 占位 */
    fh.frame_packet_size = TC_FRAME_HEADER_SIZE;
    rc = tc_frame_header_validate(&fh);
    if (rc != TC_OK) { return rc; }
    /* 批 4 复查（2026-09-13 全链审计）→ V 代际收纳（同日）：bd≥13 只许
     * 宽域熵 {8=V7-R2, 9=V8}（V7-R em=7 已退役）。V1/V2-VLC 的解码核心
     * （block_coding.c VLC 族）是 12-bit 冻结验收域/字母表，且 VLC 表下
     * 标与 batch-commit 逆变换（±2^25 域）未宽化——放行组合会自查自拒
     * 或读越界。产品路径 rans2（em=8）为默认。置于校验末尾：枚举/组合
     * 等既有错误码优先。 */
    if (cfg->bit_depth >= 13u && cfg->reserved[0] != 8u && cfg->reserved[0] != 9u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "entropy mode %u is 12-bit frozen; bit_depth %u "
                     "requires rans2/v8 entropy (reserved[0] in {8,9})",
                     (unsigned)cfg->reserved[0], (unsigned)cfg->bit_depth);
        return TC_ERR_INVALID_ARGUMENT;
    }
    return TC_OK;
}

size_t tc_frame_packet_bound(const topos_frame_config* cfg)
{
    if (cfg == NULL || cfg->visible_width == 0u || cfg->visible_height == 0u ||
        cfg->visible_width > TC_MAX_CODED_DIM || cfg->visible_height > TC_MAX_CODED_DIM) {
        return 0u;
    }
    uint64_t cw = pad8_u32(cfg->visible_width);
    uint64_t ch = pad8_u32(cfg->visible_height);
    /* R4.2/R4.3：pf≠0（4:4:4/GBR）chroma coded 宽 = luma；4:2:2 保持半宽 pad8；
     * pf=3（TRAW CFA）全平面半宽半高 pad8。 */
    uint64_t cw_c, ch_c;
    if (cfg->pixel_format == 0u) {
        cw_c = pad8_u32((uint32_t)(((uint64_t)cfg->visible_width + 1u) / 2u));
        ch_c = ch;
    } else if (cfg->pixel_format == 3u) {
        cw_c = pad8_u32((uint32_t)(((uint64_t)cfg->visible_width + 1u) / 2u));
        ch_c = pad8_u32((uint32_t)(((uint64_t)cfg->visible_height + 1u) / 2u));
    } else {
        cw_c = cw;
        ch_c = ch;
    }
    /* 保守上界：颜色每块 ≤ 128 符号 × 64 bit（escape 最长）→ 1024 B；
     * alpha 每像素 ≤ 2 符号 × 64 bit → 16 B。仅作分配参考，非承诺。 */
    uint64_t blocks;
    if (cfg->pixel_format == 3u) {
        blocks = 4u * ((cw_c * ch_c) / 64u); /* 4 相位平面全半尺寸 */
    } else {
        blocks = (cw * ch) / 64u + 2u * ((cw_c * ch_c) / 64u);
    }
    /* 保守上界：颜色每块 ≤ 128 符号 × 64 bit（escape 最长）→ 1024 B
     * （V6 的 4096B staging 特例随 V6 退役移除，V 代际收纳 2026-09-13；
     * 这是分配提示，非位流承诺）。 */
    uint64_t bytes_per_color_block = 1024u;
    uint64_t color_planes = (cfg->pixel_format == 3u) ? 4u : 3u;
    uint64_t bound = 53u + color_planes * (17u + blocks * bytes_per_color_block);
    if (cfg->alpha_mode != 0u) {
        bound += 17u + cw * ch * 16u;
    }
    if (bound > (uint64_t)TC_MAX_PACKET_SIZE) { return (size_t)TC_MAX_PACKET_SIZE; }
    return (size_t)bound;
}

/* ============================ 输出装配（dry-run 友好） ============================ */

typedef struct asm_buf {
    uint8_t* out;
    size_t cap;
    size_t off;
    int32_t err; /* 0 / TC_ERR_BUFFER_TOO_SMALL（继续计数以报告精确所需）/ LIMIT_EXCEEDED */
    int grow;    /* C04：1 = out 为 grow-only 暂存（装配全程写暂存，成功才
                  * 一次性提交调用方 out——容量不足返回时 out 字节原子不变，
                  * 头文件契约"容量不足不写 out"）。0 = 旧直写语义。 */
} asm_buf;

/* C04：预扩暂存到 need（grow 模式；成功返回 TC_OK，暂存保证可写 need 字节） */
static int32_t asm_reserve(asm_buf* a, size_t need)
{
    if (need <= a->cap) { return TC_OK; }
    if (a->grow == 0) {
        tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "out_cap %zu < %zu bytes needed",
                     a->cap, need);
        return TC_ERR_BUFFER_TOO_SMALL;
    }
    size_t cap = (a->cap < 4096u) ? 4096u : a->cap;
    while (cap < need) {
        if (cap > (size_t)TC_MAX_PACKET_SIZE / 2u) { cap = (size_t)TC_MAX_PACKET_SIZE; break; }
        cap *= 2u;
    }
    if (cap < need) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "frame packet exceeds %u bytes",
                     (unsigned)TC_MAX_PACKET_SIZE);
        return TC_ERR_LIMIT_EXCEEDED;
    }
    uint8_t* grown = (uint8_t*)tc_realloc(a->out, cap);
    if (grown == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "packet staging %zu bytes", cap);
        return TC_ERR_OUT_OF_MEMORY;
    }
    a->out = grown;
    a->cap = cap;
    return TC_OK;
}

static void asm_append(asm_buf* a, const void* data, size_t n)
{
    size_t end = 0;
    if (!tc_uadd_size(a->off, n, &end) || end > (size_t)TC_MAX_PACKET_SIZE) {
        if (a->err == TC_OK) {
            a->err = TC_ERR_LIMIT_EXCEEDED;
            tc_set_error(TC_ERR_LIMIT_EXCEEDED, "frame packet exceeds %u bytes",
                         (unsigned)TC_MAX_PACKET_SIZE);
        }
        return;
    }
    if (a->err == TC_OK) {
        if (a->grow != 0) {
            if (end > a->cap) {
                /* grow-only 倍增（装配阶段单线程；跨帧复用免重复分配） */
                size_t cap = (a->cap < 4096u) ? 4096u : a->cap;
                while (cap < end) {
                    if (cap > (size_t)TC_MAX_PACKET_SIZE / 2u) { cap = (size_t)TC_MAX_PACKET_SIZE; break; }
                    cap *= 2u;
                }
                uint8_t* grown = (uint8_t*)tc_realloc(a->out, cap);
                if (grown == NULL) {
                    a->err = TC_ERR_OUT_OF_MEMORY;
                    tc_set_error(TC_ERR_OUT_OF_MEMORY, "packet staging %zu bytes", cap);
                    a->off = end;
                    return;
                }
                a->out = grown;
                a->cap = cap;
            }
            if (n > 0u) { memcpy(a->out + a->off, data, n); }
        } else {
            if (end > a->cap) {
                a->err = TC_ERR_BUFFER_TOO_SMALL; /* 继续计数；结束时报告精确所需尺寸 */
            } else if (n > 0u) {
                memcpy(a->out + a->off, data, n);
            }
        }
    }
    a->off = end;
}

/* ============================ Rice k 估计（输入的纯函数） ============================ */

static uint32_t rice_k_estimate(uint64_t sum, uint64_t count)
{
    if (count == 0u || sum == 0u) { return 0u; }
    uint64_t mean = sum / count;
    uint32_t k = 0u;
    while (k < 14u && (mean >> (k + 1u)) != 0u) { ++k; } /* floor(log2(mean)) */
    return k;
}

/* ============================ 编码 ============================ */

/* M6b：一个颜色带的紧凑 token 集（量化遍产出；熵编码遍只走 token）。
 * dc_m/npair 每 block 一项；run/lvl_m 为顺序追加的 (run, level) 对。
 * EOB 隐含于 npair（emit 时按块补 63）→ 符号序列与 qbuf 双遍路径逐位一致。 */
typedef struct enc_band_tok {
    uint32_t* dc_m;  /* [blocks] mapped DC 残差 */
    uint16_t* npair; /* [blocks] AC 对数（≤63） */
    uint8_t* run;    /* [pairs] run（≤62） */
    uint32_t* lvl_m; /* [pairs] mapped AC level */
    uint32_t token_count; /* 当前 block 区间的 run/level token 总数 */
    /* RD2-03：V7-A 单遍分桶结果。旧 V1-V6 路径不启用；V7 writer 接入后
     * 可把同一遍 callback 换成 band payload sink，不重复 DCT/量化/扫系数。 */
    tc_v7_band_stats v7_stats;
} enc_band_tok;

/* M6c：payload chunk（柔性数组；单链，槽内串行遍历/复位） */
typedef struct enc_pay_chunk {
    struct enc_pay_chunk* next;
    size_t used;
    size_t cap;
    uint8_t data[];
} enc_pay_chunk;

/* M7：alpha 带一次编码结果（qp 无关，final 复用） */
typedef struct enc_alpha_band {
    uint8_t hdr[TC_SLICE_HEADER_SIZE];
    const uint8_t* payload;
    uint32_t payload_size;
} enc_alpha_band;

/* V8（批 2）默认粒度：段 = 16 块、瓦片 = 32 块行（批 0 实测最优档） */
#define TC_V8_DEFAULT_SB_LOG2 4u
#define TC_V8_DEFAULT_TILE_LOG2 5u
#define TC_V8_MAX_SEG_BLOCKS 32u /* sb 上限（log2 5） */

typedef struct enc_shared {
    topos_frame_header fh;
    const topos_frame_input* in;
    uint32_t slice_rows;
    int32_t qp_delta_luma;   /* slice 级码率调节（plane0） */
    int32_t qp_delta_chroma; /* plane1/2 */
    asm_buf ab;
    tc_bitwriter bw;
    int bw_ok;
    uint16_t* coded;      /* 全 plane coded 拼接区：plane p 基址 = coded + coded_off[p] */
    const uint16_t* plane_src[4]; /* M11-2a：对齐直通时的调用方输入平面（只读） */
    int plane_direct[4];          /* 1 = band 任务直读 plane_src[p]（免 pad 拷贝） */
    size_t coded_cap;     /* 已分配字节（grow-only，阶段 9 跨 plane/跨迭代复用） */
    size_t coded_off[4];  /* M10-6.2：每 plane 元素偏移（统一提交需各 plane 共存） */
    int32_t* qbuf;        /* 颜色：max_cols × slice_rows 块量化系数（R6：zigzag 扫描序） */
    size_t qbuf_cap;
    int32_t* rbuf;        /* alpha：coded_w × slice_rows×8 残差 */
    size_t rbuf_cap;
    int32_t* dc_a;        /* DC 链行缓冲 ×2 */
    int32_t* dc_b;
    size_t dc_cap;
    uint32_t color_payload;
    uint32_t alpha_payload;
    uint8_t v8_sb_log2;    /* V8（批 2）：段粒度 log2（3/4/5 = 8/16/32 块） */
    uint8_t v8_tile_log2;  /* V8：瓦片粒度 log2（0=整平面；4/5/6 = 16/32/64 块行） */
    uint32_t color_hdr_bytes;
    uint32_t alpha_hdr_bytes;
    uint16_t alpha_max_err;
    int self_check; /* 帧级结构自检（§11.5）：plain 编码恒开；sized 迭代期关、最终包统一复验 */
    /* R6：worker 槽位缓冲常驻（grow-only，跨 plane/迭代/帧复用，消除每 plane
     * 每 sized 迭代的大块 malloc/缺页）。slot 0 别名上方共享缓冲本体；
     * 1..slot_n−1 为私有缓冲，尺寸与 slot0 同源（enc_slots_reserve）。 */
    int32_t* slot_qbuf[TC_SLICE_MAX_THREADS];
    int32_t* slot_dca[TC_SLICE_MAX_THREADS];
    int32_t* slot_dcb[TC_SLICE_MAX_THREADS];
    int32_t* slot_rbuf[TC_SLICE_MAX_THREADS];
    tc_bitwriter slot_bw[TC_SLICE_MAX_THREADS];
    int slot_bw_ok[TC_SLICE_MAX_THREADS];
    uint32_t slot_n; /* 已就绪槽位数（含 slot 0） */
    size_t slot_q_built; /* 槽位缓冲已建到的尺寸水位（grow-only） */
    size_t slot_dc_built;
    size_t slot_r_built;
    /* M6b：颜色带 token 化（量化遍直接产 (run,level) token + DC 残差，熵编码
     * 遍免二次扫描 64 系数/重推 DC 预测）。tok_ok=0（分配失败）时整帧回退
     * qbuf 双遍路径（fill_color_band + tc_color_slice_encode，位流逐位一致）。 */
    int tok_ok;
    enc_band_tok tok[TC_SLICE_MAX_THREADS]; /* 每 slot 独立数组（含 slot 0） */
    size_t tok_dc_built;
    size_t tok_pair_built;
    uint32_t tok_slots_built; /* M10-6.1：tok 数组已建到的连续槽数（≤ want） */
    /* M6c：per-slot payload chunk arena（消除每 slice 的 payload
     * malloc+memcpy+free；块内 bump 分配，同槽任务串行 → 无锁）。
     * 每 plane 起点逻辑复位（used=0，chunk 保留复用）。 */
    enc_pay_chunk* slot_pay[TC_SLICE_MAX_THREADS];
    /* C1（速度计划 v2，2026-09-12）：per-slot 熵编码 scratch 字节池——
     * rANS/rANS2 renorm 暂存与 V7-A 分桶三数组从池内雕刻（高水位
     * grow-only，跨 slice/迭代/帧复用）。复剖析：per-slice tc_alloc/
     * tc_free（分配器元数据 + 首触缺页）≈5% worker 时间。池内数据均为
     * 发射前写后读的暂存（无跨 slice 读依赖）；增长失败按 OOM 上报
     * （与旧 per-call 分配失败同码，test_oom 逐点契约不变）。 */
    uint8_t* slot_scratch[TC_SLICE_MAX_THREADS];
    size_t slot_scratch_built[TC_SLICE_MAX_THREADS];
    /* M7：sized probe 的 DCT-once 缓存——全部颜色 plane 的自然序 F 系数
     * （[by*cols+bx]*64 布局）。单块连续缓冲，plane p 基址 = fcache + off[p]。
     * fcache_ok=0（OOM/超上限）→ legacy 整帧编码 probe 回退。 */
    int32_t* fcache;
    size_t fcache_elems;   /* 已分配元素数（颜色 plane 总和） */
    size_t fcache_off[4];  /* 各 plane 块偏移（元素计，×64 后为系数偏移；pf=3 为 4 平面） */
    int fcache_ok;
    /* M7：alpha 一次编码结果（qp 无关 → 跨 probe 恒定；final 组装直接复用，
     * payload 驻留 arena——M7 流程内不得 enc_pay_reset） */
    enc_alpha_band alpha_bands[TC_MAX_SLICES];
    uint32_t alpha_band_n;
    int m7_alpha_capture; /* prepare 阶段置位：plane3 组装改为捕获进 alpha_bands */
    /* M10-6.3A：跨帧 qp 提示——上一帧 strict 搜索的 q*（进程级缓存常驻）。
     * 仅作下一帧首探种子：q* 由 bytes(q) 唯一决定，种子不改变结果，
     * 只缩短探测路径（稳态序列 2 探定界）。
     * C2（速度计划 v2）：提示升级为双工作点 (q*, bytes(q*)) 记忆——
     * 最近两个相异工作点的 log-linear 弦即码率模型；目标变化（GOP 头
     * 换码率）时沿弦外推直接落新工作点种子，冷启动首跳用弦斜率替代
     * 固定先验 4.0。仍是纯种子/先验：探针路径变化不影响 strict 括号
     * 不变量的解 q*（test_c2_sized_hint 差分钉死 qp 轨迹与包字节）。 */
    uint32_t qp_hint;
    int qp_hint_valid;
    uint32_t qp_hint_bytes;       /* 最近工作点 bytes(q*)（0 = 未记） */
    uint32_t qp_hint_prev;        /* 前一相异工作点 q*（0 = 无） */
    uint32_t qp_hint_prev_bytes;  /* 前一相异工作点 bytes */
    /* V2.x AQ（逐带 qp 偏移，掩蔽型，slice 粒度；F-cache 路径专用）：
     * m7_prepare 在 DCT 遍内顺带累计每带 Σ|F_AC|（零额外遍），barrier 后
     * 按对数比计算偏移（clamp ±2）；偏移与 qp 无关 → m7_probe 与最终
     * 编码（enc_band_pass_all）同源消费，sized 搜索口径一致。
     * aq_on=0（默认）时偏移全 0，行为与旧版逐位一致。 */
    int aq_on;
    int rdo_on;                    /* V2.x：reserved[2]=1 逐系数 level RDO */
    uint64_t aq_act[4][TC_MAX_SLICES];
    int8_t aq_off[4][TC_MAX_SLICES];
} enc_shared;

static void enc_free_members(enc_shared* e)
{
    if (e->bw_ok != 0) { tc_bitwriter_free(&e->bw); }
    tc_free(e->coded);
    tc_free(e->qbuf);
    tc_free(e->rbuf);
    tc_free(e->dc_a);
    tc_free(e->dc_b);
    /* 全槽位释放（不按 slot_n 截断——OOM 部分提交的槽也持有活指针；
     * free(NULL) 安全；M10-6.1 缓存复用使截断槽的泄漏可见） */
    for (uint32_t w = 1u; w < (uint32_t)TC_SLICE_MAX_THREADS; ++w) {
        if (e->slot_bw_ok[w] != 0) { tc_bitwriter_free(&e->slot_bw[w]); }
        tc_free(e->slot_qbuf[w]);
        tc_free(e->slot_dca[w]);
        tc_free(e->slot_dcb[w]);
        tc_free(e->slot_rbuf[w]);
    }
    for (uint32_t w = 0u; w < (uint32_t)TC_SLICE_MAX_THREADS; ++w) {
        tc_free(e->tok[w].dc_m);
        tc_free(e->tok[w].npair);
        tc_free(e->tok[w].run);
        tc_free(e->tok[w].lvl_m);
        tc_free(e->slot_scratch[w]);
        enc_pay_chunk* c = e->slot_pay[w];
        while (c != NULL) {
            enc_pay_chunk* n = c->next;
            tc_free(c);
            c = n;
        }
        e->slot_pay[w] = NULL;
    }
    tc_free(e->fcache);
    e->fcache = NULL;
    tc_free(e->ab.out); /* C04：包装配暂存（grow 模式持有） */
    e->ab.out = NULL;
    memset(e, 0, sizeof(*e));
}

/* ---------------- M10-6.1：进程级持久 enc_shared 缓存 ----------------
 *
 * 取证（0f404603）：每帧 enc_shared 建毁（4K 时 coded 33MB + F-cache 66MB
 * + 槽位/arena 等约 100MB malloc/free + 首触缺页）占 native 墙钟 ~20%
 * （闭合账外 16~40ms/帧）。缓存实例 grow-only 跨调用持有，atexit 释放。
 *
 * 并发契约：单缓存实例以原子 busy 标志互斥；竞争失败/分配失败/显式禁用
 * 的调用方回退栈上实例（与旧版逐调用实例语义一致）。fork 后子进程的
 * busy 可能停在 1（COW 快照）→ 子进程全部走栈回退，正确性不受影响。
 * arena 归零在 enc_frame_setup（上一调用的 payload 已在组装中拷入最终包）。 */
static enc_shared* s_enc_cache = NULL;
static atomic_int s_enc_cache_busy = 0;
static atomic_int s_enc_cache_disable = 0; /* P1-06 */
static int s_enc_cache_atexit_done = 0;

/* P1-08：命中/回退量化（relaxed 原子；并行压测归因用） */
static _Atomic uint64_t s_ec_hits;
static _Atomic uint64_t s_ec_fb_busy;
static _Atomic uint64_t s_ec_fb_disabled;
static _Atomic uint64_t s_ec_fb_alloc;

void tc_dev_enc_cache_stats_reset(void)
{
    atomic_store_explicit(&s_ec_hits, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_ec_fb_busy, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_ec_fb_disabled, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_ec_fb_alloc, 0u, memory_order_relaxed);
}

void tc_dev_enc_cache_stats_get(tc_enc_cache_stats* out)
{
    if (out == NULL) { return; }
    out->hits = atomic_load_explicit(&s_ec_hits, memory_order_relaxed);
    out->fallback_busy = atomic_load_explicit(&s_ec_fb_busy, memory_order_relaxed);
    out->fallback_disabled = atomic_load_explicit(&s_ec_fb_disabled, memory_order_relaxed);
    out->fallback_alloc = atomic_load_explicit(&s_ec_fb_alloc, memory_order_relaxed);
}

void tc_dev_set_enc_cache(int disable)
{
    atomic_store_explicit(&s_enc_cache_disable, disable ? 1 : 0,
                               memory_order_relaxed);
}

int32_t tc_dev_enc_slots_validate(void)
{
    /* N03 单测支撑：核对常驻槽位缓冲的实际分配（malloc_size 实测，不信
     * 水位字段）覆盖当前容量需求。返回违例计数（0 = 一致）。
     * 槽位 qbuf/dc×2/rbuf 四项与 tok 四项（tok_ok 时）逐槽核对；
     * slot_n 声称已建却为 NULL 亦计违例。仅进程缓存实例（无借用方时
     * 调用——单测里编码调用已返回，s_enc_cache 空闲）。 */
    enc_shared* e = s_enc_cache;
    if (e == NULL) { return 0; }
    int32_t bad = 0;
    for (uint32_t w = 1u; w < e->slot_n && w < (uint32_t)TC_SLICE_MAX_THREADS; ++w) {
        if (e->slot_qbuf[w] == NULL || tc_malloc_size(e->slot_qbuf[w]) < e->qbuf_cap) { bad++; }
        if (e->slot_dca[w] == NULL || tc_malloc_size(e->slot_dca[w]) < e->dc_cap) { bad++; }
        if (e->slot_dcb[w] == NULL || tc_malloc_size(e->slot_dcb[w]) < e->dc_cap) { bad++; }
        if (e->rbuf_cap > 0u &&
            (e->slot_rbuf[w] == NULL || tc_malloc_size(e->slot_rbuf[w]) < e->rbuf_cap)) { bad++; }
        if (e->tok_ok != 0 && w < e->tok_slots_built) {
            /* tok 尺寸推导（enc_slots_reserve 同源）：
             * dc_sz = blocks*4, np_sz = blocks*2, pairs_sz = blocks*63,
             * pair_lv_sz = pairs_sz*4；水位 tok_dc_built=dc_sz、
             * tok_pair_built=pair_lv_sz */
            const size_t np_need = e->tok_dc_built / 2u;
            const size_t run_need = e->tok_pair_built / 4u;
            if (e->tok[w].dc_m == NULL || tc_malloc_size(e->tok[w].dc_m) < e->tok_dc_built) { bad++; }
            if (e->tok[w].npair == NULL || tc_malloc_size(e->tok[w].npair) < np_need) { bad++; }
            if (e->tok[w].run == NULL || tc_malloc_size(e->tok[w].run) < run_need) { bad++; }
            if (e->tok[w].lvl_m == NULL || tc_malloc_size(e->tok[w].lvl_m) < e->tok_pair_built) { bad++; }
        }
    }
    return bad;
}

static void enc_cache_free_atexit(void)
{
    if (s_enc_cache != NULL) {
        enc_free_members(s_enc_cache);
        tc_free(s_enc_cache);
        s_enc_cache = NULL;
    }
}

typedef struct enc_loan {
    enc_shared* cached; /* 非 NULL：借到的缓存实例（release 只归还标志） */
    enc_shared local;   /* cached==NULL 时使用（release 释放成员） */
} enc_loan;

static enc_shared* enc_loan_acquire(enc_loan* loan)
{
    loan->cached = NULL;
    if (atomic_load_explicit(&s_enc_cache_disable, memory_order_relaxed) != 0) {
        atomic_fetch_add_explicit(&s_ec_fb_disabled, 1u, memory_order_relaxed);
    } else {
        int expected = 0;
        if (atomic_compare_exchange_strong(&s_enc_cache_busy, &expected, 1)) {
            if (s_enc_cache == NULL) {
                s_enc_cache = (enc_shared*)tc_alloc(sizeof(enc_shared));
                if (s_enc_cache != NULL) {
                    memset(s_enc_cache, 0, sizeof(*s_enc_cache));
                    if (s_enc_cache_atexit_done == 0) {
                        s_enc_cache_atexit_done = 1;
                        atexit(enc_cache_free_atexit);
                    }
                }
            }
            if (s_enc_cache != NULL) {
                loan->cached = s_enc_cache;
                atomic_fetch_add_explicit(&s_ec_hits, 1u, memory_order_relaxed);
                return loan->cached;
            }
            atomic_fetch_add_explicit(&s_ec_fb_alloc, 1u, memory_order_relaxed);
            atomic_store(&s_enc_cache_busy, 0); /* 实例分配失败 → 栈回退 */
        } else {
            atomic_fetch_add_explicit(&s_ec_fb_busy, 1u, memory_order_relaxed);
        }
    }
    memset(&loan->local, 0, sizeof(loan->local));
    return &loan->local;
}

static void enc_loan_release(enc_loan* loan)
{
    if (loan->cached != NULL) {
        atomic_store(&s_enc_cache_busy, 0);
        loan->cached = NULL;
    } else {
        enc_free_members(&loan->local);
    }
}

/* M6c：槽内 payload bump 分配（同槽任务串行 → 无锁）。空间不足时追加
 * 新 chunk（≥256KiB 或请求尺寸）；失败返回 NULL 由调用方按 OOM 报错。 */
static uint8_t* enc_pay_alloc(enc_shared* e, uint32_t slot, size_t size)
{
    enc_pay_chunk* c = e->slot_pay[slot];
    while (c != NULL) {
        if (c->cap - c->used >= size) {
            uint8_t* p = c->data + c->used;
            c->used += size;
            return p;
        }
        if (c->next == NULL) { break; }
        c = c->next;
    }
    size_t cs = size > ((size_t)256u << 10) ? size : ((size_t)256u << 10);
    enc_pay_chunk* n = (enc_pay_chunk*)tc_alloc(sizeof(enc_pay_chunk) + cs);
    if (n == NULL) { return NULL; }
    n->next = NULL;
    n->used = size;
    n->cap = cs;
    if (c == NULL) { e->slot_pay[slot] = n; } else { c->next = n; }
    return n->data;
}

/* 每 plane 起点：全部 chunk 逻辑复位（保留内存复用） */
static void enc_pay_reset(enc_shared* e)
{
    for (uint32_t w = 0u; w < (uint32_t)TC_SLICE_MAX_THREADS; ++w) {
        for (enc_pay_chunk* c = e->slot_pay[w]; c != NULL; c = c->next) { c->used = 0; }
    }
}

/* C1：槽位 scratch 池借用。need ≤ 已建水位 → 零分配命中；不足 → realloc
 * 增长并提交新水位（grow-only；调用点均在 job 入口绑定的本槽内，同槽
 * 任务串行 → 无锁、增长时无并发读者）。失败返回 NULL：调用方按 OOM 直报
 * （旧 per-call tc_alloc 失败的等价故障语义；realloc 失败不动旧指针）。 */
static uint8_t* enc_scratch_acquire(enc_shared* e, uint32_t slot, size_t need)
{
    if (e == NULL || slot >= (uint32_t)TC_SLICE_MAX_THREADS) { return NULL; }
    if (need <= e->slot_scratch_built[slot]) { return e->slot_scratch[slot]; }
    uint8_t* grown = (uint8_t*)tc_realloc(e->slot_scratch[slot], need);
    if (grown == NULL) { return NULL; }
    e->slot_scratch[slot] = grown;
    e->slot_scratch_built[slot] = need;
    return grown;
}

/* grow-only 保留共享缓冲（encode_sized 的 ≤24 次整帧迭代零重复 malloc/缺页；
 * 尺寸不变时为纯 no-op）。要求 fh 几何已派生。 */
static void enc_slots_reserve(enc_shared* e);
static int32_t enc_reserve_shared(enc_shared* e)
{
    if (e->slot_n < 1u) { e->slot_n = 1u; } /* slot0（本体）恒就绪 */
    uint32_t max_cols = 0u;
    uint64_t total_px = 0u; /* M10-6.2：全 plane coded 拼接（各 plane 共存） */
    for (uint32_t p = 0u; p < e->fh.plane_count; ++p) {
        if (e->fh.plane_block_cols[p] > max_cols) { max_cols = e->fh.plane_block_cols[p]; }
        e->coded_off[p] = (size_t)total_px;
        total_px += (uint64_t)e->fh.plane_coded_w[p] * (uint64_t)e->fh.plane_coded_h[p];
    }
    for (uint32_t p = e->fh.plane_count; p < 4u; ++p) { e->coded_off[p] = 0u; }
    if (total_px == 0u) { total_px = 1u; } /* 空 plane 防御（0×几何） */
    size_t qblocks = 0;
    if (!tc_umul_size((size_t)max_cols, (size_t)e->slice_rows, &qblocks)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "qbuf block count overflow");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    size_t qbytes = 0;
    if (!tc_umul_size(qblocks, 64u * sizeof(int32_t), &qbytes)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "qbuf size overflow");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    size_t dcbytes = 0;
    if (!tc_umul_size((size_t)max_cols, sizeof(int32_t), &dcbytes)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "dc buffer size overflow");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    size_t codedbytes = 0;
    if (!tc_umul_size((size_t)total_px, sizeof(uint16_t), &codedbytes)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "coded plane size overflow");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    if (qbytes > e->qbuf_cap) {
        int32_t* grown = (int32_t*)tc_realloc(e->qbuf, qbytes);
        if (grown == NULL) {
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "encode shared buffers");
            return TC_ERR_OUT_OF_MEMORY;
        }
        e->qbuf = grown;
        e->qbuf_cap = qbytes;
    }
    if (dcbytes > e->dc_cap) {
        int32_t* ga = (int32_t*)tc_realloc(e->dc_a, dcbytes);
        if (ga == NULL) {
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "encode shared buffers");
            return TC_ERR_OUT_OF_MEMORY;
        }
        e->dc_a = ga;
        int32_t* gb = (int32_t*)tc_realloc(e->dc_b, dcbytes);
        if (gb == NULL) {
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "encode shared buffers");
            return TC_ERR_OUT_OF_MEMORY;
        }
        e->dc_b = gb;
        e->dc_cap = dcbytes;
    }
    if (codedbytes > e->coded_cap) {
        uint16_t* grown = (uint16_t*)tc_realloc(e->coded, codedbytes);
        if (grown == NULL) {
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "encode shared buffers");
            return TC_ERR_OUT_OF_MEMORY;
        }
        e->coded = grown;
        e->coded_cap = codedbytes;
    }
    if (e->fh.alpha_mode != 0u) {
        size_t pixels = 0;
        if (!tc_umul_size((size_t)e->fh.plane_coded_w[3], (size_t)e->slice_rows * 8u, &pixels)) {
            tc_set_error(TC_ERR_LIMIT_EXCEEDED, "rbuf pixel count overflow");
            return TC_ERR_LIMIT_EXCEEDED;
        }
        size_t rbytes = 0;
        if (!tc_umul_size(pixels, sizeof(int32_t), &rbytes)) {
            tc_set_error(TC_ERR_LIMIT_EXCEEDED, "rbuf size overflow");
            return TC_ERR_LIMIT_EXCEEDED;
        }
        if (rbytes > e->rbuf_cap) {
            int32_t* grown = (int32_t*)tc_realloc(e->rbuf, rbytes);
            if (grown == NULL) {
                tc_set_error(TC_ERR_OUT_OF_MEMORY, "encode shared buffers");
                return TC_ERR_OUT_OF_MEMORY;
            }
            e->rbuf = grown;
            e->rbuf_cap = rbytes;
        }
    }
    if (e->bw_ok == 0) {
        int32_t rc = tc_bitwriter_init(&e->bw);
        if (rc == TC_OK) { e->bw_ok = 1; }
        enc_slots_reserve(e); /* bw 尚未就绪时也可先备槽位缓冲 */
        return rc;
    }
    enc_slots_reserve(e);
    return TC_OK;
}

/* R6：槽位缓冲常驻化。slot 尺寸需求与 slot0 同源（enc_reserve_shared 已算出
 * qbuf/dc/rbuf 字节数）；OOM 时以 slot_n 退化（多线程→少线程，正确性不受影响）。 */

/* M6：token 路径差分开关（dev/test）——禁用时 enc_slots_reserve 判退，
 * 全帧走 qbuf 双遍回退（差分测试对拍两路径逐字节一致性） */
static int s_tok_disable = 0;

void tc_dev_set_token_encode(int disable) { s_tok_disable = disable ? 1 : 0; }

/* M10-2A：专用 scan-to-plane 差分开关（dev/test）——0（默认）= 生产直写
 * 路径（sink 在扫描循环内展开），1 = 强制 generic 函数指针 sink 路径。
 * 两路径像素/错误码/消费位逐位一致（test_m10 / test_slice_codec 差分）。 */
static int s_direct_scan_disable = 0;

void tc_dev_set_direct_scan(int disable) { s_direct_scan_disable = disable ? 1 : 0; }

static int direct_scan_enabled(void) { return s_direct_scan_disable == 0; }

static int tc_packet_is_v7a(const uint8_t* data, size_t size)
{
    /* V7-A 实验包按熵三元组 (5,5,2) 识别；major=7 的产品路径 V7-R2
     * (entropy_mode=7, ADR-C036) 与 V7-B（tc_packet_is_v7b 前置探测）不走此。
     * V 代际收纳（D1，2026-09-13）：V7-A band decode 已归档——生产构建在
     * 探测链入口干净拒绝（UNSUPPORTED_VERSION 含回放指引），仅
     * TOPOS_DEV_REPLAY 构建 + TOPOS_DEV=1 可达专用入口。 */
    return data != NULL && size >= 7u && memcmp(data, TC_FRAME_MAGIC, 4u) == 0 &&
           data[6] == 7u && size > 45u && data[45] == 5u;
}

static int32_t v7a_retired_reject(void)
{
    tc_set_error(TC_ERR_UNSUPPORTED_VERSION,
                 "generation 7 (V7-A band decode) retired (V consolidation "
                 "2026-09-13, see ADR-C0xx); archived streams replay only "
                 "with a TOPOS_DEV_REPLAY build and TOPOS_DEV=1");
    return TC_ERR_UNSUPPORTED_VERSION;
}

static int tc_packet_is_v7b(const uint8_t* data, size_t size)
{
    return tc_v7b_packet_is(data, size);
}

/* M10-2B：稀疏重建阈值（dev/test）——非零 AC 对数 ≤ 阈值时走稀疏 basis
 * 逆变换；0 = 禁用（全走稠密 rowmask 内核）。
 * A/B 实测（4K 密素材，2026-09-01）：标量 int64 稀疏核在全部候选阈值
 * （4/8/12/16）均劣化——thr=12 时 IDCT CPU 63.2→82.4 ms/帧（+30%）、
 * wall 11.5→14.2 ms。根因：现行 AVX2 rowmask 内核已在行粒度利用稀疏性，
 * 且 64 输出的 round32 为稀疏路径无法摊销的固定成本；标量 int64 累加
 * （64 madd/系数）不敌向量稠密核。故默认禁用；代码与差分保留，待
 * vpmuldq 4-lane 向量稀疏核再评估（门：IDCT CPU ≥ −15%）。 */
static int s_sparse_threshold = 0;

void tc_dev_set_sparse_threshold(int n)
{
    if (n < 0) { n = 0; }
    if (n > TC_SPARSE_MAX_AC) { n = TC_SPARSE_MAX_AC; }
    s_sparse_threshold = n;
}

int tc_dev_sparse_threshold(void) { return s_sparse_threshold; }

/* M10-2C：四块 SoA 批量 IDCT 开关（dev/test）——0 = 启用，1 = 禁用
 * （默认禁用，逐块内核为生产路径）。
 * A/B 实测（4K 密素材，2026-09-01）：SoA4 wall 12.1~13.4 ms vs 逐块
 * 10.7~10.9 ms（劣化 15~25%）。根因：现行 1 块内核已是 lanes=x 的
 * 向量化形态（每指令处理 4 个输出的工作量），SoA 重组的指令数与逐块
 * 相当（mul 计数不变、仅广播共享），却额外付出 qsoa 跨步 staging
 * （256 写/组）+ 输出转置；且 pass B 权重从表载退化为运行时标量乘。
 * 逐块内核已接近该分解的向量化上限。按门（IDCT CPU ≥ −20%）默认
 * 禁用；代码/差分保留（bit-exact 已证），后续 IDCT 收益需换算法角度
 * （如 pass 间融合或 ARM NEON 对齐，见计划 M10-8）。 */
static atomic_int s_batch_idct_disable = 1; /* P1-06 */

void tc_dev_set_batch_idct(int disable)
{
    atomic_store_explicit(&s_batch_idct_disable, disable ? 1 : 0,
                               memory_order_relaxed);
}

int tc_dev_batch_idct(void)
{
    return atomic_load_explicit(&s_batch_idct_disable, memory_order_relaxed);
}

/* ---- M9：训练符号直方图（dev；详见 codec.h 说明） ---- */

static atomic_int s_sym_hist_on = 0; /* P1-06 */
static _Atomic uint64_t s_sym_dc[TC_SYM_HIST_DC];
static _Atomic uint64_t s_sym_run[TC_SYM_HIST_RUN];
static _Atomic uint64_t s_sym_lvl[TC_SYM_HIST_LVL];
static _Atomic uint64_t s_sym_dc_plane[3][TC_SYM_HIST_DC];
static _Atomic uint64_t s_sym_run_plane[3][TC_SYM_HIST_RUN];
static _Atomic uint64_t s_sym_lvl_plane[3][TC_SYM_HIST_LVL];
static _Atomic uint64_t s_sym_pair_plane[3][TC_SYM_PAIR];

/* cat = 有效位宽（m=0 → 0；m ∈ [2^(cat−1), 2^cat) → cat）——spec v2 §4.1 */
static inline uint32_t sym_cat_of(uint32_t m)
{
#if defined(__GNUC__) || defined(__clang__)
    return m == 0u ? 0u : (uint32_t)(32 - __builtin_clz(m));
#else
    uint32_t cat = 0u;
    while ((m >> cat) != 0u) { cat++; }
    return cat;
#endif
}

static inline void sym_hist_dc(uint32_t plane, uint32_t dcm)
{
    atomic_fetch_add_explicit(&s_sym_dc[sym_cat_of(dcm)], 1u, memory_order_relaxed);
    if (plane < 3u) {
        atomic_fetch_add_explicit(&s_sym_dc_plane[plane][sym_cat_of(dcm)], 1u,
                                  memory_order_relaxed);
    }
}

static inline void sym_hist_pair(uint32_t plane, uint32_t run, uint32_t lvl_m)
{
    atomic_fetch_add_explicit(&s_sym_run[run], 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_sym_lvl[sym_cat_of(lvl_m)], 1u, memory_order_relaxed);
    if (plane < 3u) {
        atomic_fetch_add_explicit(&s_sym_run_plane[plane][run], 1u, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_sym_lvl_plane[plane][sym_cat_of(lvl_m)], 1u,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&s_sym_pair_plane[plane][run * TC_SYM_HIST_LVL
                                                            + sym_cat_of(lvl_m)],
                                  1u, memory_order_relaxed);
    }
}

static inline void sym_hist_eob(uint32_t plane)
{
    atomic_fetch_add_explicit(&s_sym_run[63u], 1u, memory_order_relaxed);
    if (plane < 3u) {
        atomic_fetch_add_explicit(&s_sym_run_plane[plane][63u], 1u,
                                  memory_order_relaxed);
    }
}

void tc_dev_symbol_hist_enable(int enable)
{
    atomic_store_explicit(&s_sym_hist_on, enable ? 1 : 0, memory_order_relaxed);
}

void tc_dev_symbol_hist_reset(void)
{
    for (uint32_t i = 0u; i < TC_SYM_HIST_DC; ++i) {
        atomic_store_explicit(&s_sym_dc[i], 0u, memory_order_relaxed);
    }
    for (uint32_t i = 0u; i < TC_SYM_HIST_RUN; ++i) {
        atomic_store_explicit(&s_sym_run[i], 0u, memory_order_relaxed);
    }
    for (uint32_t i = 0u; i < TC_SYM_HIST_LVL; ++i) {
        atomic_store_explicit(&s_sym_lvl[i], 0u, memory_order_relaxed);
    }
    for (uint32_t p = 0u; p < 3u; ++p) {
        for (uint32_t i = 0u; i < TC_SYM_HIST_DC; ++i) {
            atomic_store_explicit(&s_sym_dc_plane[p][i], 0u, memory_order_relaxed);
        }
        for (uint32_t i = 0u; i < TC_SYM_HIST_RUN; ++i) {
            atomic_store_explicit(&s_sym_run_plane[p][i], 0u, memory_order_relaxed);
        }
        for (uint32_t i = 0u; i < TC_SYM_HIST_LVL; ++i) {
            atomic_store_explicit(&s_sym_lvl_plane[p][i], 0u, memory_order_relaxed);
        }
        for (uint32_t i = 0u; i < TC_SYM_PAIR; ++i) {
            atomic_store_explicit(&s_sym_pair_plane[p][i], 0u, memory_order_relaxed);
        }
    }
}

void tc_dev_symbol_hist_get(uint64_t* dc, uint64_t* run, uint64_t* lvl)
{
    for (uint32_t i = 0u; i < TC_SYM_HIST_DC; ++i) {
        dc[i] = atomic_load_explicit(&s_sym_dc[i], memory_order_relaxed);
    }
    for (uint32_t i = 0u; i < TC_SYM_HIST_RUN; ++i) {
        run[i] = atomic_load_explicit(&s_sym_run[i], memory_order_relaxed);
    }
    for (uint32_t i = 0u; i < TC_SYM_HIST_LVL; ++i) {
        lvl[i] = atomic_load_explicit(&s_sym_lvl[i], memory_order_relaxed);
    }
}

void tc_dev_symbol_hist_plane_get(uint32_t plane, uint64_t* dc, uint64_t* run,
                                  uint64_t* lvl)
{
    if (plane >= 3u) {
        if (dc != NULL) { memset(dc, 0, TC_SYM_HIST_DC * sizeof(*dc)); }
        if (run != NULL) { memset(run, 0, TC_SYM_HIST_RUN * sizeof(*run)); }
        if (lvl != NULL) { memset(lvl, 0, TC_SYM_HIST_LVL * sizeof(*lvl)); }
        return;
    }
    if (dc != NULL) {
        for (uint32_t i = 0u; i < TC_SYM_HIST_DC; ++i) {
            dc[i] = atomic_load_explicit(&s_sym_dc_plane[plane][i], memory_order_relaxed);
        }
    }
    if (run != NULL) {
        for (uint32_t i = 0u; i < TC_SYM_HIST_RUN; ++i) {
            run[i] = atomic_load_explicit(&s_sym_run_plane[plane][i], memory_order_relaxed);
        }
    }
    if (lvl != NULL) {
        for (uint32_t i = 0u; i < TC_SYM_HIST_LVL; ++i) {
            lvl[i] = atomic_load_explicit(&s_sym_lvl_plane[plane][i], memory_order_relaxed);
        }
    }
}

void tc_dev_symbol_pair_hist_get(uint32_t plane, uint64_t* pair_hist)
{
    if (pair_hist == NULL) { return; }
    if (plane >= 3u) {
        memset(pair_hist, 0, TC_SYM_PAIR * sizeof(*pair_hist));
        return;
    }
    for (uint32_t i = 0u; i < TC_SYM_PAIR; ++i) {
        pair_hist[i] = atomic_load_explicit(&s_sym_pair_plane[plane][i],
                                            memory_order_relaxed);
    }
}

static void enc_slots_reserve(enc_shared* e)
{
    uint32_t want = (uint32_t)tc_dev_thread_count();
    if (want > (uint32_t)TC_SLICE_MAX_THREADS) { want = (uint32_t)TC_SLICE_MAX_THREADS; }

    /* M6b：token 数组（每 slot 独立；blocks = max_cols × slice_rows 上界，
     * pairs 最坏 = 63/block）。任一分配失败 → tok_ok=0 永久回退 qbuf 双遍
     * 路径（确定性：进程内路径选择不再变化，且两路径位流逐位一致）。
     * 放在单线程早退之前：token 路径 1 线程同样启用。 */
    if (s_tok_disable != 0) { e->tok_ok = 0; goto qbuf_slots; }
    {
        uint32_t max_cols = 0u;
        for (uint32_t p = 0u; p < e->fh.plane_count; ++p) {
            if (e->fh.plane_block_cols[p] > max_cols) { max_cols = e->fh.plane_block_cols[p]; }
        }
        size_t blocks = 0;
        size_t dc_sz = 0;
        size_t np_sz = 0;
        size_t pairs_sz = 0;
        size_t pair_lv_sz = 0;
        if (max_cols == 0u ||
            !tc_umul_size((size_t)max_cols, (size_t)e->slice_rows, &blocks) ||
            !tc_umul_size(blocks, sizeof(uint32_t), &dc_sz) ||
            !tc_umul_size(blocks, sizeof(uint16_t), &np_sz) ||
            !tc_umul_size(blocks, 63u, &pairs_sz) ||
            !tc_umul_size(pairs_sz, sizeof(uint32_t), &pair_lv_sz)) {
            e->tok_ok = 0;
            return;
        }
        const int need_tok_grow = dc_sz > e->tok_dc_built || pair_lv_sz > e->tok_pair_built ||
                                  e->tok_slots_built < want; /* M10-6.1：缓存跨调用——
                                  槽位数须随线程配置增长（容量够 ≠ 全部槽已建） */
        if (e->tok_ok != 0 && !need_tok_grow) { goto qbuf_slots; }
        /* N03 修复：尺寸增长（或 tok_ok=0 修复重试）时，重建必须覆盖全部
         * 已建槽 max(want, tok_slots_built)——旧实现只重建 w < want 却在
         * 成功后无条件抬 tok_dc_built/tok_pair_built 水位。进程级线程数被
         * 临时调低（R6 策略）时编码只在低 want 下扩容，恢复高线程数的
         * 消费方（m7 探针/条带任务按当前线程数取槽）会拿到旧尺寸小数组
         * → tok 越界写堆（N03：组合回归 SIGSEGV/摘要不一致的根因）。 */
        uint32_t tok_wtop = want;
        if (e->tok_slots_built > tok_wtop) { tok_wtop = e->tok_slots_built; }
        for (uint32_t w = 0u; w < tok_wtop && w < (uint32_t)TC_SLICE_MAX_THREADS; ++w) {
            /* realloc 四项全部成功才提交（与下方槽位逻辑同约束） */
            uint32_t* dc = (uint32_t*)tc_realloc(e->tok[w].dc_m, dc_sz);
            uint16_t* np = dc != NULL ? (uint16_t*)tc_realloc(e->tok[w].npair, np_sz) : NULL;
            uint8_t* rn = np != NULL ? (uint8_t*)tc_realloc(e->tok[w].run, pairs_sz) : NULL;
            uint32_t* lv = rn != NULL ? (uint32_t*)tc_realloc(e->tok[w].lvl_m, pair_lv_sz) : NULL;
            if (dc == NULL || np == NULL || rn == NULL || lv == NULL) {
                /* 复验 P1-16 修正：realloc 成功即可能已移动（旧指针被
                 * realloc 释放）——必须提交成功项，禁止「free 新指针、
                 * 保留旧指针」（旧指针已死 → 释放时双重 free；M10-6.1
                 * 缓存复用使 realloc 带非 NULL 旧指针而暴露）。失败项的
                 * fault 未触达真实 realloc → 旧指针仍有效、原样保留；
                 * 尺寸不一致由 tok_ok=0 全帧回退 + 下次全组重建兜底。 */
                if (dc != NULL) { e->tok[w].dc_m = dc; }
                if (np != NULL) { e->tok[w].npair = np; }
                if (rn != NULL) { e->tok[w].run = rn; }
                if (lv != NULL) { e->tok[w].lvl_m = lv; }
                e->tok_ok = 0; /* 本次失败；已建槽位保留但整帧统一走回退路径 */
                goto qbuf_slots;
            }
            e->tok[w].dc_m = dc;
            e->tok[w].npair = np;
            e->tok[w].run = rn;
            e->tok[w].lvl_m = lv;
            e->tok_slots_built = w + 1u; /* 连续前缀（失败槽不提交，走 tok_ok=0） */
        }
        e->tok_ok = 1;
        e->tok_dc_built = dc_sz;
        e->tok_pair_built = pair_lv_sz;
    }

qbuf_slots:
    if (want < 2u) { return; } /* 单线程：slot0 即够 */

    const int need_grow = (e->qbuf_cap > e->slot_q_built) || (e->dc_cap > e->slot_dc_built) ||
                          (e->rbuf_cap > e->slot_r_built);
    if (!need_grow && e->slot_n >= want) { return; }

    /* N03 修复：need_grow 时重建覆盖全部已建槽 max(want, slot_n)——下方
     * 的水位更新会为 slot_n 内全部槽背书容量；只重建 w < want 时，低
     * 线程编码留下的 [want, slot_n) 槽保持旧尺寸，水位却是新值 →
     * 后续高线程消费方越界（与 tok 组同源缺陷）。 */
    uint32_t slot_wtop = want;
    if (need_grow && e->slot_n > slot_wtop) { slot_wtop = e->slot_n; }
    for (uint32_t w = 1u; w < slot_wtop; ++w) {
        if (need_grow || w >= e->slot_n) {
            /* 复验 P1-16：realloc 结果先落临时指针，四项全部成功才提交——
             * 此前逐项直接覆盖旧指针，单项失败会丢失原分配（泄漏）且留下
             * NULL 槽。失败时：释放本槽新成功的分配，旧指针原样保留，
             * slot_n 截断到确定完整的最小集合。 */
            int32_t* q = (int32_t*)tc_realloc(e->slot_qbuf[w], e->qbuf_cap);
            int32_t* a = q != NULL
                ? (int32_t*)tc_realloc(e->slot_dca[w], e->dc_cap) : NULL;
            int32_t* b = (a != NULL)
                ? (int32_t*)tc_realloc(e->slot_dcb[w], e->dc_cap) : NULL;
            int32_t* r = (e->rbuf_cap > 0u && b != NULL)
                ? (int32_t*)tc_realloc(e->slot_rbuf[w], e->rbuf_cap)
                : e->slot_rbuf[w];
            if (q == NULL || a == NULL || b == NULL ||
                (e->rbuf_cap > 0u && r == NULL)) {
                /* 同 tok 组：成功项即时提交（realloc 移动语义——旧指针已
                 * 释放，free 新指针保留旧指针 = 双重 free）；失败项保留
                 * 有效旧值。本槽四项尺寸不一致 → slot_n 截断到 w，后续
                 * 调用经 w >= slot_n 条件完整重建。 */
                if (q != NULL) { e->slot_qbuf[w] = q; }
                if (a != NULL) { e->slot_dca[w] = a; }
                if (b != NULL) { e->slot_dcb[w] = b; }
                if (r != NULL && r != e->slot_rbuf[w]) { e->slot_rbuf[w] = r; }
                if (e->slot_n > w) { e->slot_n = w; } /* 截断到确定完整的最小集合 */
                break;
            }
            e->slot_qbuf[w] = q;
            e->slot_dca[w] = a;
            e->slot_dcb[w] = b;
            e->slot_rbuf[w] = r;
            if (e->slot_bw_ok[w] == 0) {
                if (tc_bitwriter_init(&e->slot_bw[w]) != TC_OK) {
                    if (e->slot_n > w) { e->slot_n = w; }
                    break;
                }
                e->slot_bw_ok[w] = 1;
            }
            if (w + 1u > e->slot_n) { e->slot_n = w + 1u; }
        }
    }
    if (e->slot_n < 1u) { e->slot_n = 1u; }
    /* 保留槽位均已 ≥ 当前需求（失败槽已截断出 slot_n）→ 更新已建尺寸水位 */
    if (e->slot_n > 1u) {
        if (e->qbuf_cap > e->slot_q_built) { e->slot_q_built = e->qbuf_cap; }
        if (e->dc_cap > e->slot_dc_built) { e->slot_dc_built = e->dc_cap; }
        if (e->rbuf_cap > e->slot_r_built) { e->slot_r_built = e->rbuf_cap; }
    }
}

/* pad visible → coded（p=3 且 mode2 时顺带做 N-bit 近似量化并记录最大误差） */
static int32_t enc_prepare_plane(enc_shared* e, uint32_t p)
{
    const topos_frame_header* fh = &e->fh;
    uint32_t dst_w = fh->plane_coded_w[p];
    uint32_t dst_h = fh->plane_coded_h[p];
    uint32_t src_w = fh->plane_visible_w[p];
    uint32_t src_h = fh->plane_visible_h[p];
    size_t src_stride = e->in->strides[p] != 0u ? e->in->strides[p] : (size_t)src_w;

    /* M11-2a：几何已对齐（可见=coded）且紧排的颜色平面免拷贝直通——band
     * 任务直接读调用方输入；pad 全平面拷贝此前占编码线程 ~29%（4K t16
     * 单遍口径 sample：tc_plane_pad_u16 1017/3478 工作样本）。alpha 平面
     * （mode2 原位预量化会写 coded）与非对齐几何仍走拷贝路径。直通读到
     * 的字节与 pad 拷贝产物逐位相同 → 输出逐位不变（golden 钉死）。 */
    e->plane_direct[p] = (!is_alpha_plane(&e->fh, p) && src_w == dst_w && src_h == dst_h
                          && src_stride == (size_t)dst_w) ? 1 : 0;
    if (e->plane_direct[p] != 0) {
        if (e->in->planes[p] == NULL || src_w == 0u || src_h == 0u
                || src_w > TC_PLANE_MAX_DIM || src_h > TC_PLANE_MAX_DIM) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "input plane %u does not match config", (unsigned)p);
            return TC_ERR_INVALID_ARGUMENT;
        }
        e->plane_src[p] = e->in->planes[p];
        return TC_OK;
    }

    /* 防御（reserve 已按全 plane 总和保证）：须保留其它 plane 数据 → realloc */
    {
        size_t need = (e->coded_off[p] + (size_t)dst_w * (size_t)dst_h) * sizeof(uint16_t);
        if (need > e->coded_cap) {
            uint16_t* grown = (uint16_t*)tc_realloc(e->coded, need);
            if (grown == NULL) {
                tc_set_error(TC_ERR_OUT_OF_MEMORY, "coded plane %u", (unsigned)p);
                return TC_ERR_OUT_OF_MEMORY;
            }
            e->coded = grown;
            e->coded_cap = need;
        }
    }
    uint16_t* coded_p = e->coded + e->coded_off[p];
    if (!tc_plane_pad_u16(e->in->planes[p], src_w, src_h, src_stride,
                          coded_p, dst_w, dst_h, (size_t)dst_w)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "input plane %u does not match config", (unsigned)p);
        return TC_ERR_INVALID_ARGUMENT;
    }

    if (p == 3u && fh->alpha_mode == 2u) {
        uint32_t s = 16u - (uint32_t)fh->alpha_bit_depth;
        uint32_t half = 1u << (s - 1u);
        uint32_t top = (0xFFFFu >> s) << s;
        uint32_t max_err = 0u;
        for (uint32_t y = 0u; y < dst_h; ++y) {
            uint16_t* row = coded_p + (size_t)y * (size_t)dst_w;
            for (uint32_t x = 0u; x < dst_w; ++x) {
                uint32_t v = row[x];
                uint32_t q = ((v + half) >> s) << s;
                if (q > top) { q = top; }
                uint32_t err = v > q ? v - q : q - v;
                if (err > max_err) { max_err = err; }
                row[x] = (uint16_t)q;
            }
        }
        if (max_err > e->alpha_max_err) { e->alpha_max_err = (uint16_t)max_err; }
    }
    return TC_OK;
}

/* 填充一个颜色带的量化系数 + 收集 k 估计统计（DC 链/EOB 与熵编码完全同构）。
 * R6：qbuf 以 zigzag 扫描序存储（tc_quant_block_zigzag 直写）——统计遍与
 * 后续 tc_color_slice_encode 熵编码遍都是顺序扫描，双侧消除表间接寻址；
 * 数值与 natural 序逐系数一致 → k 估计与位流逐位不变（golden 冻结）。 */
static void fill_color_band(const topos_frame_header* fh, const uint16_t* coded,
                            uint32_t plane, const tc_quant_ctx* qctx,
                            int32_t* qbuf, int32_t* dc_a, int32_t* dc_b,
                            uint32_t y0, uint32_t h,
                            uint32_t* k1, uint32_t* k2, uint32_t* k3)
{
    uint32_t cols = fh->plane_block_cols[plane];
    int32_t* q = qbuf;
    uint64_t sum_dc = 0u, cnt_dc = 0u, sum_level = 0u, cnt_level = 0u, sum_run = 0u, cnt_run = 0u;
    /* M10-6.3B：SIMD 量化（zigzag 直写 + 非零掩码）；每带一次解析 */
    const tc_quant_zz_fn quant_zz = tc_simd_resolve_quant_zz(
        (uint8_t)(fh->bit_depth > 12u));
    /* P-速②：u16 平面行直载 forward + mid 提出块循环（同 token 版） */
    const tc_forward_rows_fn fwd_rows = tc_simd_resolve_forward_rows((uint8_t)(fh->bit_depth > 12u));
    const size_t plane_stride = (size_t)cols * 8u;
    const int32_t mid = (int32_t)bd_mid(fh->bit_depth);
    const int sym_on = atomic_load_explicit(&s_sym_hist_on, memory_order_relaxed) != 0;

    memset(dc_a, 0, (size_t)cols * sizeof(int32_t));
    memset(dc_b, 0, (size_t)cols * sizeof(int32_t));
    size_t bi = 0u;
    for (uint32_t by = 0u; by < h; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            const uint16_t* src = coded + (size_t)(y0 + by) * 8u * (size_t)cols * 8u +
                                  (size_t)(bx * 8u);
            int32_t F[64];
            fwd_rows(src, plane_stride, mid, F);
            uint64_t nz = 0u;
            quant_zz(qctx, F, q + bi * 64u, &nz);

            int has_left = bx > 0u ? 1 : 0;
            int has_top = by > 0u ? 1 : 0;
            int32_t pred = tc_dc_predict(has_left, dc_b[bx > 0u ? bx - 1u : 0u],
                                         has_top, dc_a[bx]);
            uint32_t dcm = tc_rice_map_signed(q[bi * 64u] - pred);
            sum_dc += dcm;
            cnt_dc++;
            dc_b[bx] = q[bi * 64u];
            if (sym_on) { sym_hist_dc(plane, dcm); }

            /* 掩码跳扫：tzcnt 直达非零位置，run = 位置差−1（与逐位扫零
             * 计数同值；token/统计序列不变） */
            nz >>= 1u;
            uint32_t prev = 0u;
            while (nz != 0u) {
                const uint32_t pos = tc_ctz64(nz) + 1u;
                uint32_t lm = tc_rice_map_signed(q[bi * 64u + pos]);
                const uint32_t run = pos - prev - 1u;
                sum_run += run;
                cnt_run++;
                sum_level += lm;
                cnt_level++;
                if (sym_on) { sym_hist_pair(plane, run, lm); }
                prev = pos;
                nz &= nz - 1u;
            }
            sum_run += 63u; /* EOB（run 符号值恒 63） */
            cnt_run++;
            if (sym_on) { sym_hist_eob(plane); }
            bi++;
        }
        int32_t* t = dc_a; dc_a = dc_b; dc_b = t;
    }
    *k1 = rice_k_estimate(sum_dc, cnt_dc);
    *k2 = rice_k_estimate(sum_level, cnt_level);
    *k3 = rice_k_estimate(sum_run, cnt_run);
}

/* M6b：token 版 fill —— 量化遍直接产 token + k 统计（不再写 qbuf、不再留
 * 64 系数扫描给熵编码遍）。数值路径与 fill_color_band 完全同源：
 * tc_quant_block_zigzag 输出、DC 链、k 统计累计值逐一相同 → 同 k；
 * emit_color_tokens 的符号序列与 tc_color_slice_encode 逐位相同 → 位流不变。 */
/* 批 4 复查（2026-09-13）：token 域前置硬校验（P4 遗留②升级为内存安全
 * 必修）。bd16 低 qp（Q<8）+ 满幅密集内容的 |Δdc|/|level| 可越出 rans
 * 字母表容量（DC bitlen≤28 / lvl bitlen≤27 → hist/freq 栈数组越界写）。
 * 违例值饱和防越界写、返回 TC_ERR_INVALID_ARGUMENT 整帧拒绝；12-bit
 * 域（|m|≤2^25）恒不触发，零行为变化。*/
static int32_t fill_color_band_tokens(const topos_frame_header* fh, const uint16_t* coded,
                                   uint32_t plane, const tc_quant_ctx* qctx,
                                   enc_band_tok* tok, int32_t* dc_a, int32_t* dc_b,
                                   uint32_t y0, uint32_t h,
                                   uint32_t* k1, uint32_t* k2, uint32_t* k3,
                                   uint32_t* dc_hist, uint32_t* run_hist,
                                   uint32_t* lvl_hist)
{
    uint32_t domain_violation = 0u;
    uint32_t cols = fh->plane_block_cols[plane];
    uint64_t sum_dc = 0u, cnt_dc = 0u, sum_level = 0u, cnt_level = 0u, sum_run = 0u, cnt_run = 0u;
    /* P-速⑥：自然序量化（免 gather）——q 顺序读写，掩码经 tc_zz_mask_from_nat
     * 翻译到扫描域，非零系数读 q_nat[kTcZigzag[pos]]（仅非零处一次 L1 查表）。
     * 数值与 gather 版逐系数一致（同一数学，索引布局不同）。 */
    const tc_quant_nat_fn quant_nat = tc_simd_resolve_quant_nat((uint8_t)(fh->bit_depth > 12u));
    /* P-速②：u16 平面行直载 forward（每带一次解析）+ mid 提出块循环 */
    const tc_forward_rows_fn fwd_rows = tc_simd_resolve_forward_rows((uint8_t)(fh->bit_depth > 12u));
    const size_t plane_stride = (size_t)cols * 8u;
    const int32_t mid = (int32_t)bd_mid(fh->bit_depth);
    /* P-速④：VLC 模式（hist 非 NULL）下 Rice k 统计为死值——调用方以
     * vlc_books_from_hist 的 book id 覆写 k1/k2/k3，从不清读。跳过每对
     * 6 个 u64 累计；k 输出仍按零计数写出（Rice 路径 need_rice 恒 1）。 */
    const int need_rice = dc_hist == NULL;
    const int sym_on = atomic_load_explicit(&s_sym_hist_on, memory_order_relaxed) != 0;
    const int capture_v7 = fh->version_major == 7u && fh->entropy_mode == 5u;
    if (capture_v7 != 0) { tc_v7_band_stats_reset(&tok->v7_stats); }
    /* C036 前置测量：VLC/rANS 模式（hist 非 NULL）下的 order-1 上下文捕获
     * （dev-only；Rice 路径与 alpha 不参与——符号族不同） */
    tc_ctx_cap* cap = (tc_dev_ctx_active() != 0 && dc_hist != NULL)
                          ? tc_ctx_cap_alloc() : NULL;

    memset(dc_a, 0, (size_t)cols * sizeof(int32_t));
    memset(dc_b, 0, (size_t)cols * sizeof(int32_t));
    size_t bi = 0u;
    size_t pi = 0u;
    /* P-速⑤：跨块软件流水——下一块 DCT 先行发射，与当前块 quant 的加载
     * 交叠（F 双缓冲隔开 256b store → load 转发停顿；数值与串行版
     * 逐值相同——DCT(b+1) 与 quant(b) 无数据依赖，仅调度重排）。 */
    int32_t Fpp[2][64];
    int32_t qn[64];
    const uint32_t total_by = h;
    {
        const uint16_t* src0 = coded + (size_t)y0 * 8u * (size_t)cols * 8u;
        fwd_rows(src0, plane_stride, mid, Fpp[0]);
    }
    for (uint32_t by = 0u; by < total_by; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            const int cur = (int)((by * cols + bx) & 1u);
            /* 预发射下一块 DCT（末块跳过） */
            {
                uint32_t nby = by;
                uint32_t nbx = bx + 1u;
                if (nbx >= cols) { nbx = 0u; nby++; }
                if (nby < total_by) {
                    const uint16_t* nsrc =
                        coded + (size_t)(y0 + nby) * 8u * (size_t)cols * 8u +
                        (size_t)(nbx * 8u);
                    fwd_rows(nsrc, plane_stride, mid, Fpp[cur ^ 1]);
                }
            }
            int32_t* F = Fpp[cur];
            uint64_t nat = 0u;
            quant_nat(qctx, F, qn, &nat);
            uint64_t nz = tc_zz_mask_from_nat(nat);
            tc_v7_band_bucket v7_bucket;
            if (capture_v7 != 0) { tc_v7_band_bucket_init(&v7_bucket, (uint32_t)bi); }

            int has_left = bx > 0u ? 1 : 0;
            int has_top = by > 0u ? 1 : 0;
            int32_t pred = tc_dc_predict(has_left, dc_b[bx > 0u ? bx - 1u : 0u],
                                         has_top, dc_a[bx]);
            uint32_t dcm = tc_rice_map_signed(qn[0] - pred);
            if (dcm > TC_RICE_M_MAX_DC_BD16) {
                dcm = TC_RICE_M_MAX_DC_BD16;
                domain_violation = 1u;
            }
            tok->dc_m[bi] = dcm;
            if (need_rice != 0) {
                sum_dc += dcm;
                cnt_dc++;
            }
            dc_b[bx] = qn[0];
            if (dc_hist != NULL) { dc_hist[tc_vlc_bitlen32(dcm)]++; }
            if (sym_on) { sym_hist_dc(plane, dcm); }
            if (cap != NULL) { tc_ctx_dc(cap, dcm); }

            nz >>= 1u; /* 掩码跳扫（同 fill_color_band）：token 序列不变 */
            uint32_t np = 0u;
            uint32_t prev = 0u;
            while (nz != 0u) {
                const uint32_t pos = tc_ctz64(nz) + 1u;
                uint32_t lm = tc_rice_map_signed(qn[kTcZigzag[pos]]);
                if (lm > TC_RICE_M_MAX_AC_LEVEL_BD16) {
                    lm = TC_RICE_M_MAX_AC_LEVEL_BD16;
                    domain_violation = 1u;
                }
                uint32_t runv = pos - prev - 1u;
                tok->run[pi] = (uint8_t)runv;
                tok->lvl_m[pi] = lm;
                pi++;
                if (capture_v7 != 0) {
                    (void)tc_v7_band_bucket_push(&v7_bucket, (uint8_t)pos, lm, NULL, NULL);
                }
                if (need_rice != 0) {
                    sum_run += runv;
                    cnt_run++;
                    sum_level += lm;
                    cnt_level++;
                }
                if (run_hist != NULL) { run_hist[runv]++; }
                if (lvl_hist != NULL) { lvl_hist[tc_vlc_bitlen32(lm)]++; }
                if (sym_on) { sym_hist_pair(plane, runv, lm); }
                if (cap != NULL) { tc_ctx_pair(cap, prev, pos, runv, lm); }
                prev = pos;
                nz &= nz - 1u;
                np++;
            }
            tok->npair[bi] = (uint16_t)np;
            if (need_rice != 0) {
                sum_run += 63u; /* EOB（run 符号值恒 63） */
                cnt_run++;
            }
            if (run_hist != NULL) { run_hist[63u]++; }
            if (sym_on) { sym_hist_eob(plane); }
            if (cap != NULL) { tc_ctx_eob(cap, prev); }
            if (capture_v7 != 0) { (void)tc_v7_band_stats_add_block(&tok->v7_stats, &v7_bucket); }
            bi++;
        }
        int32_t* t = dc_a; dc_a = dc_b; dc_b = t;
    }
    tok->token_count = (uint32_t)pi;
    if (cap != NULL) {
        tc_ctx_fold(cap, dc_hist, run_hist, lvl_hist);
        tc_ctx_cap_free(cap);
    }
    *k1 = rice_k_estimate(sum_dc, cnt_dc);
    *k2 = rice_k_estimate(sum_level, cnt_level);
    *k3 = rice_k_estimate(sum_run, cnt_run);
    if (domain_violation != 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "token domain violated (bit_depth %u qp too low for "
                     "full-scale content): raise qp or bit_depth <= 12",
                     (unsigned)fh->bit_depth);
        return TC_ERR_INVALID_ARGUMENT;
    }
    return TC_OK;
}

/* M6b：token → 位流。符号序列：每块 DC + (run,level)×npair + EOB(63)，
 * 与 tc_block_encode_zigzag 输出逐位一致（含 EOB 常量）。
 * M10-6.3C：符号对融合发射——run/level 码字（各 ≤63 位）在 ≤64 位时
 * 拼接为一次 64 位写入（位序列与逐符号写逐位一致；超长 escape 退两写）。 */
static int32_t emit_color_tokens(const enc_band_tok* tok, size_t blocks,
                                 uint32_t k1, uint32_t k2, uint32_t k3, tc_bitwriter* bw)
{
    size_t pi = 0u;
    for (size_t b = 0u; b < blocks; ++b) {
        uint64_t w = 0u;
        uint32_t n = tc_rice_code_word(k1, tok->dc_m[b], &w);
        int32_t rc = tc_bitwriter_put_bits64_inline(bw, n, w);
        if (rc != TC_OK) { return rc; }
        uint32_t np = tok->npair[b];
        for (uint32_t p = 0u; p < np; ++p, ++pi) {
            uint64_t wr = 0u, wl = 0u;
            const uint32_t nr = tc_rice_code_word(k3, tok->run[pi], &wr);
            const uint32_t nl = tc_rice_code_word(k2, tok->lvl_m[pi], &wl);
            if (nr + nl <= 64u) {
                rc = tc_bitwriter_put_bits64_inline(bw, nr + nl, (wr << nl) | wl);
            } else {
                rc = tc_bitwriter_put_bits64_inline(bw, nr, wr);
                if (rc == TC_OK) { rc = tc_bitwriter_put_bits64_inline(bw, nl, wl); }
            }
            if (rc != TC_OK) { return rc; }
        }
        n = tc_rice_code_word(k3, 63u, &w);
        rc = tc_bitwriter_put_bits64_inline(bw, n, w);
        if (rc != TC_OK) { return rc; }
    }
    return TC_OK;
}

/* ---- M9：V2 canonical VLC 编码端（spec v2 §4.2）----
 * 选表：token 遍累计三族 histogram，逐族对 4 本冻结表算精确 total_bits
 * （码长 + 类别后缀位），取最小、同分取最小 book id——不在运行时建树。
 * 冻结长度表直接参与计位；emit/decode 侧的书由 tc_vlc_tables_ensure 构建。 */

static inline uint32_t vlc_sym_bits_of(const uint8_t* lens, uint32_t sym, int is_cat)
{
    return (uint32_t)lens[sym] + ((is_cat != 0 && sym > 1u) ? sym - 1u : 0u);
}

/* lens_base 指向 [TC_VLC_BOOKS][nsym] 连续冻结表首元素（各族 nsym 不同） */
static uint8_t vlc_best_book(const uint8_t* lens_base, uint32_t nsym,
                             const uint32_t* hist, int is_cat)
{
    uint64_t best = UINT64_MAX;
    uint8_t best_id = 0u;
    for (uint32_t bk = 0u; bk < TC_VLC_BOOKS; ++bk) {
        const uint8_t* lens = lens_base + (size_t)bk * (size_t)nsym;
        uint64_t total = 0u;
        for (uint32_t sym = 0u; sym < nsym; ++sym) {
            if (hist[sym] != 0u) {
                total += (uint64_t)hist[sym] * vlc_sym_bits_of(lens, sym, is_cat);
            }
        }
        if (total < best) { best = total; best_id = (uint8_t)bk; }
    }
    return best_id;
}

/* P1：fill 遍融合直方图 → 各族最优 book（与旧 token 重读版确定性一致：
 * 同一直方图值域、同一 vlc_best_book 顺序；histogram 由 fill 遍累计，
 * 单一统计真相源，不再重读 token 数组）。 */
static void vlc_books_from_hist(const uint32_t* dc_hist, const uint32_t* run_hist,
                                const uint32_t* lvl_hist,
                                uint8_t* dc_bk, uint8_t* lvl_bk, uint8_t* run_bk)
{
    *dc_bk = vlc_best_book(&tc_vlc_dc_len[0][0], TC_VLC_DC_SYMS, dc_hist, 1);
    *lvl_bk = vlc_best_book(&tc_vlc_lvl_len[0][0], TC_VLC_LVL_SYMS, lvl_hist, 1);
    *run_bk = vlc_best_book(&tc_vlc_run_len[0][0], TC_VLC_RUN_SYMS, run_hist, 0);
}

/* token 集精确位计数（VLC 口径；选表同 vlc_books_from_tokens，结果确定性一致） */
static uint64_t color_tokens_bits_vlc(const enc_band_tok* tok, size_t blocks,
                                      uint8_t dc_bk, uint8_t lvl_bk, uint8_t run_bk)
{
    const uint8_t* dcl = tc_vlc_dc_len[dc_bk];
    const uint8_t* lvll = tc_vlc_lvl_len[lvl_bk];
    const uint8_t* runl = tc_vlc_run_len[run_bk];
    uint64_t bits = 0u;
    size_t pi = 0u;
    for (size_t b = 0u; b < blocks; ++b) {
        bits += vlc_sym_bits_of(dcl, tc_vlc_bitlen32(tok->dc_m[b]), 1);
        uint32_t np = tok->npair[b];
        for (uint32_t p = 0u; p < np; ++p, ++pi) {
            bits += runl[tok->run[pi]];
            bits += vlc_sym_bits_of(lvll, tc_vlc_bitlen32(tok->lvl_m[pi]), 1);
        }
        bits += runl[63u];
    }
    return bits;
}

/* token → VLC 位流（符号序列与 emit_color_tokens 同构；码字层替换）。
 * P-速③ 融合发射（位序列逐位不变，仅减少 writer 追加次数）：
 *  - (run, level) 对：run 码字 + level 码字 + 类别后缀 ≤64 位时单次 64 位
 *    追加（超界退两次——run ≤20、lvl ≤20+31 理论可达 71）；
 *  - 块尾 EOB 与下一块 DC 差分码融合（各 ≤20 / ≤20+31，超界同样退两写）。 */
static int32_t emit_color_tokens_vlc(const enc_band_tok* tok, size_t blocks,
                                     uint8_t dc_bk, uint8_t lvl_bk, uint8_t run_bk,
                                     tc_bitwriter* bw)
{
    int32_t rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_DC, dc_bk);
    if (rc != TC_OK) { return rc; }
    rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_LVL, lvl_bk);
    if (rc != TC_OK) { return rc; }
    rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_RUN, run_bk);
    if (rc != TC_OK) { return rc; }
    const tc_vlc_book* dcb = tc_vlc_book_get(TC_VLC_FAMILY_DC, dc_bk);
    const tc_vlc_book* lvlb = tc_vlc_book_get(TC_VLC_FAMILY_LVL, lvl_bk);
    const tc_vlc_book* runb = tc_vlc_book_get(TC_VLC_FAMILY_RUN, run_bk);
    if (dcb == NULL || lvlb == NULL || runb == NULL) {
        tc_set_error(TC_ERR_STATE, "vlc book unavailable");
        return TC_ERR_STATE;
    }
    /* EOB 常量（每块同码字） */
    const uint32_t eob_n = runb->len[63u];
    const uint64_t eob_v = runb->code[63u];
    uint64_t pend_v = 0u; /* 上一块 EOB 待融合（首块前恒空） */
    uint32_t pend_n = 0u;
    size_t pi = 0u;
    for (size_t b = 0u; b < blocks; ++b) {
        /* DC 差分类别码（码字 + (cat−1) 位后缀单字构造） */
        const uint32_t dcm = tok->dc_m[b];
        const uint32_t dcat = tc_vlc_bitlen32(dcm);
        const uint32_t dsb = dcat > 1u ? dcat - 1u : 0u;
        const uint32_t dn = (uint32_t)dcb->len[dcat] + dsb;
        const uint64_t dv =
            ((uint64_t)dcb->code[dcat] << dsb)
            | (uint64_t)(dcm & (dsb == 0u ? 0u : (1u << dsb) - 1u));
        if (pend_n + dn <= 64u) {
            rc = tc_bitwriter_put_bits64_inline(bw, pend_n + dn,
                                                (pend_v << dn) | dv);
            if (rc != TC_OK) { return rc; }
        } else {
            rc = tc_bitwriter_put_bits64_inline(bw, pend_n, pend_v);
            if (rc == TC_OK) { rc = tc_bitwriter_put_bits64_inline(bw, dn, dv); }
            if (rc != TC_OK) { return rc; }
        }
        const uint32_t np = tok->npair[b];
        for (uint32_t p = 0u; p < np; ++p, ++pi) {
            const uint32_t m = tok->lvl_m[pi];
            const uint32_t cat = tc_vlc_bitlen32(m);
            const uint32_t sb = cat > 1u ? cat - 1u : 0u;
            const uint32_t ln = (uint32_t)lvlb->len[cat] + sb;
            const uint64_t lv =
                ((uint64_t)lvlb->code[cat] << sb)
                | (uint64_t)(m & (sb == 0u ? 0u : (1u << sb) - 1u));
            const uint32_t rn = runb->len[tok->run[pi]];
            const uint64_t rv = runb->code[tok->run[pi]];
            if (rn + ln <= 64u) {
                rc = tc_bitwriter_put_bits64_inline(bw, rn + ln, (rv << ln) | lv);
                if (rc != TC_OK) { return rc; }
            } else {
                rc = tc_bitwriter_put_bits64_inline(bw, rn, rv);
                if (rc == TC_OK) { rc = tc_bitwriter_put_bits64_inline(bw, ln, lv); }
                if (rc != TC_OK) { return rc; }
            }
        }
        pend_v = eob_v;
        pend_n = eob_n;
    }
    return tc_bitwriter_put_bits64_inline(bw, pend_n, pend_v); /* 末块 EOB */
}

/* ---- V7-R rANS 编码端（ADR-C034；token → [121B 表][rANS 流]）----
 * 符号序列与 emit_color_tokens_vlc 同构（DC_CAT+后缀 / RUN / LEVEL_CAT+后缀 /
 * EOB=RUN63），仅码字层换 per-slice 精确频率 rANS。模型从 121B 表字节重建
 * （与解码侧同源——比例量化见 rans.h）。scratch 上界：每 rANS 操作 ≤2 字节
 * （x' < 2^31+2^13，重整化目标 ≥2^19）+ 终态 4B。 */
/* rANS 编码核心：建表/模型 + 后向编码。bw 非 NULL 时表 + 流经位写器
 * 落盘；bytes_out 非 NULL 时返回 payload 字节数（表 + 流）——m7 sized
 * 探针以 bw=NULL 复用（与最终编码同一代码路径 → 逐字节一致）。
 * C1：renorm scratch 从 es 的槽位池借用（跨 slice 复用，增长失败即 OOM）。 */
static int32_t color_tokens_rans_encode(const enc_band_tok* tok, size_t blocks,
                                        const uint32_t* dc_hist,
                                        const uint32_t* run_hist,
                                        const uint32_t* lvl_hist,
                                        tc_bitwriter* bw, size_t* bytes_out,
                                        enc_shared* es, uint32_t slot)
{
    uint8_t table[TC_RANS_TABLE_BYTES];
    tc_rans_table_encode(table, dc_hist, run_hist, lvl_hist);
    tc_rans_model dc_m, run_m, lvl_m;
    int32_t rc = tc_rans_table_decode(table, &dc_m, &run_m, &lvl_m);
    if (rc != TC_OK) { return rc; }

    /* 后缀位总数（类别 s 的后缀宽 = s>1 ? s−1 : 0） */
    uint64_t suffix_bits = 0u;
    for (uint32_t s = 2u; s < TC_RANS_DC_SYMS; ++s) {
        suffix_bits += (uint64_t)dc_hist[s] * (uint64_t)(s - 1u);
    }
    for (uint32_t s = 2u; s < TC_RANS_LVL_SYMS; ++s) {
        suffix_bits += (uint64_t)lvl_hist[s] * (uint64_t)(s - 1u);
    }
    const uint64_t ops = 2ull * (uint64_t)blocks + 2ull * (uint64_t)tok->token_count
                       + suffix_bits;
    const size_t cap = (size_t)(2ull * ops + 8ull);
    uint8_t* scratch = enc_scratch_acquire(es, slot, cap);
    if (scratch == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "rans scratch %zu bytes", cap);
        return TC_ERR_OUT_OF_MEMORY;
    }
    tc_rans_enc enc;
    tc_rans_enc_init(&enc, scratch, cap);

    /* 后向遍历（解码正向序的严格倒序）：块 b 从末块起——EOB、逆序对
     * （后缀 LSB→MSB、LEVEL_CAT、RUN）、DC 后缀、DC_CAT */
    size_t pi = (size_t)tok->token_count;
    for (size_t b = blocks; b-- > 0u;) {
        rc = tc_rans_put(&enc, &run_m, 63u);
        if (rc != TC_OK) { goto done; }
        const uint32_t np = tok->npair[b];
        for (uint32_t p = np; p-- > 0u;) {
            --pi;
            const uint32_t lm = tok->lvl_m[pi];
            rc = tc_rans_put_suffix(&enc, lm, tc_vlc_bitlen32(lm));
            if (rc != TC_OK) { goto done; }
            rc = tc_rans_put(&enc, &lvl_m, tc_vlc_bitlen32(lm));
            if (rc != TC_OK) { goto done; }
            rc = tc_rans_put(&enc, &run_m, tok->run[pi]);
            if (rc != TC_OK) { goto done; }
        }
        const uint32_t dcm = tok->dc_m[b];
        rc = tc_rans_put_suffix(&enc, dcm, tc_vlc_bitlen32(dcm));
        if (rc != TC_OK) { goto done; }
        rc = tc_rans_put(&enc, &dc_m, tc_vlc_bitlen32(dcm));
        if (rc != TC_OK) { goto done; }
    }
    rc = tc_rans_enc_flush(&enc);
    if (rc > 0) {
        const size_t rans_bytes = (size_t)rc; /* flush 布局总长 = pos + 4 */
        rc = TC_OK;
        if (bytes_out != NULL) {
            *bytes_out = (size_t)TC_RANS_TABLE_BYTES + rans_bytes;
        }
        if (bw != NULL) {
            /* C5：批量字节追加（reservoir 空直 memcpy）——替代逐字节
             * put_bits_inline(8) 循环（r2e 剖析：该段占 19.3%）。 */
            rc = tc_bitwriter_put_bytes(bw, table, TC_RANS_TABLE_BYTES);
            if (rc == TC_OK) {
                rc = tc_bitwriter_put_bytes(bw, scratch, rans_bytes);
            }
        }
    }
done:
    return rc; /* C1：scratch 归槽位池，无每 slice 释放 */
}

static int32_t emit_color_tokens_rans(const enc_band_tok* tok, size_t blocks,
                                      const uint32_t* dc_hist,
                                      const uint32_t* run_hist,
                                      const uint32_t* lvl_hist,
                                      tc_bitwriter* bw, enc_shared* es,
                                      uint32_t slot)
{
    return color_tokens_rans_encode(tok, blocks, dc_hist, run_hist, lvl_hist,
                                    bw, NULL, es, slot);
}

/* ---- V7-R2（ADR-C036）：order-1 上下文 rANS 发射 ---- */

/* 条件联合直方图（token 前向一遍重建，布局 ctx*syms + sym；上下文桶
 * 定义与解码侧 tc_rans2_*_ctx 同源）。 */
typedef struct rans2_joints {
    uint32_t lvl_pos[TC_RANS2_POS_CTX * TC_RANS_LVL_SYMS];
    uint32_t lvl_prev[TC_RANS2_PREV_CTX * TC_RANS_LVL_SYMS];
    uint32_t dc_prev[TC_RANS2_DC_CTX * TC_RANS_DC_SYMS];
} rans2_joints;

static void rans2_collect_joints(const enc_band_tok* tok, size_t blocks,
                                 rans2_joints* j)
{
    memset(j, 0, sizeof(*j));
    size_t pi = 0u;
    int first_block = 1;
    uint32_t prev_dcc = 0u;
    for (size_t b = 0u; b < blocks; ++b) {
        const uint32_t dcc = tc_vlc_bitlen32(tok->dc_m[b]);
        /* D1 上下文 = 前块（raster）DC 类——因果，与后向游走/解码器一致。
         * （初版误用本块自身类做上下文 = 非因果对角 joint，表与流错配。） */
        j->dc_prev[(size_t)(first_block ? 0u : tc_rans2_dc_ctx(prev_dcc, 1))
                       * TC_RANS_DC_SYMS + dcc]++;
        prev_dcc = dcc;
        first_block = 0;
        const uint32_t np = tok->npair[b];
        uint32_t prev_pos = 0u;
        uint32_t prev_cat = 0u;
        for (uint32_t p = 0u; p < np; ++p) {
            const uint32_t cat = tc_vlc_bitlen32(tok->lvl_m[pi]);
            const uint32_t pos = prev_pos + tok->run[pi] + 1u;
            j->lvl_pos[(size_t)tc_rans2_pos_ctx(pos) * TC_RANS_LVL_SYMS + cat]++;
            j->lvl_prev[(size_t)tc_rans2_prevlvl_ctx(prev_cat, p != 0u)
                            * TC_RANS_LVL_SYMS + cat]++;
            prev_cat = cat;
            prev_pos = pos;
            pi++;
        }
    }
}

/* 候选模型量化表代价（bit）：行内 8-bit 比例量化 → 模型 freq →
 * Σ count·(12 − log2 f)——即该候选下 rANS 符号位的精确值（重整化/终态
 * 开销各候选同量，argmin 不受影响）。tc_ctx_log2 为 IEEE-754 确定性
 * 纯函数（无 libm 依赖），仅用于编码端选择；选择结果入流信令，解码端
 * 无条件复现 → 位流确定性不受影响（spec §11.1 口径的例外已记录于
 * bitstream_spec_v7r2_ctx §5）。 */
static double rans2_rows_cost(const uint32_t* rows, uint32_t ctx_n, uint32_t nsym)
{
    double bits = 0.0;
    for (uint32_t c = 0u; c < ctx_n; ++c) {
        const uint32_t* cnt = rows + (size_t)c * nsym;
        uint8_t rb[TC_RANS_MAX_SYMS];
        tc_rans2_row_encode(rb, nsym, cnt);
        tc_rans_model m;
        if (tc_rans2_row_decode(rb, nsym, &m) != TC_OK) { continue; }
        for (uint32_t s = 0u; s < nsym; ++s) {
            if (cnt[s] != 0u) {
                bits += (double)cnt[s] * (12.0 - tc_ctx_log2((double)m.freq[s]));
            }
        }
    }
    return bits;
}

/* ---- S5（ADR-C045）：后向对环的值语义 put 引擎 ----
 * x / fastdiv 以值传递（SSA 驻留寄存器）——导出版经 tc_rans_enc* 指针
 * 访问 e->x，renorm 的 uint8_t 存储按 C 别名规则可能改写任意对象，
 * 迫使每次 put 后从栈重载 e->x/fd（P0 复剖析：后向环自体 ~47%×r2e，
 * 反汇编证实栈往返与调用开销为主体）。运算与 tc_rans_put_fast /
 * tc_rans_put_chunk / tc_rans_put_rawbits 逐位一致（新旧 dylib 差分钉死）。
 *
 * 域契约（省略的防御检查在此为死码，量化管线保证）：lvl bitlen ≤ 27、
 * dc bitlen ≤ 28、run ≤ 62 恒在族字母表内；被发射符号在本 slice 计数
 * ≥1 ⟹ freq ≥1（model_build 保底）。溢出路径逐函数镜像：put_x 置错误
 * 消息（同 put_fast），chunk_x 不置（同 put_chunk——flush 统一补报）。 */
static inline uint32_t rans2_put_chunk_x(uint32_t x, tc_rans_enc* e, uint32_t w,
                                         uint32_t c, int32_t* rc)
{
    const uint32_t thr = 1u << (31u - w);
    while (x >= thr) {
        if (e->pos >= e->cap) {
            e->overflow = 1;
            *rc = TC_ERR_BUFFER_TOO_SMALL;
            return x;
        }
        e->buf[e->pos++] = (uint8_t)(x & 0xFFu);
        x >>= 8;
    }
    return (x << w) | (c & ((1u << w) - 1u));
}

/* tc_rans_put_suffix 的值语义替身（cat−1 个幅度位；cat>24 拆高低块，
 * 次序同 rans.h 原函数）。b==0 为无操作。 */
static inline uint32_t rans2_put_suffix_x(uint32_t x, tc_rans_enc* e, uint32_t cat,
                                          uint32_t m, int32_t* rc)
{
    uint32_t b = cat > 1u ? cat - 1u : 0u;
    if (b == 0u || b > 27u) { return x; }
    if (b > 23u) {
        x = rans2_put_chunk_x(x, e, b - 16u, m >> 16u, rc);
        if (*rc != TC_OK) { return x; }
        b = 16u;
    }
    return rans2_put_chunk_x(x, e, b, m, rc);
}

/* tc_rans_put_fast 的值语义替身（f/cum/fd 由调用方按 ctx 行预取；
 * fd 字段预读于 renorm 存储之前——免 char-store 别名重载，分派与
 * tc_fastdiv_apply 同码）。 */
static inline uint32_t rans2_put_x(uint32_t x, tc_rans_enc* e, uint32_t f,
                                   uint32_t cum, tc_fastdiv fd, int32_t* rc)
{
    const uint64_t magic = fd.magic;
    const uint32_t fshift = fd.shift;
    const uint32_t fallback = fd.divisor_fallback;
    const uint32_t x_max = ((TC_RANS_L >> TC_RANS_SCALE_BITS) << 8) * f;
    while (x >= x_max) {
        if (e->pos >= e->cap) {
            e->overflow = 1;
            tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "rans encode overflow (%zu/%zu)",
                         e->pos, e->cap);
            *rc = TC_ERR_BUFFER_TOO_SMALL;
            return x;
        }
        e->buf[e->pos++] = (uint8_t)(x & 0xFFu);
        x >>= 8;
    }
    uint32_t q;
#if defined(__SIZEOF_INT128__)
    if (magic != 0u) {
        q = (uint32_t)(((unsigned __int128)x * magic) >> 51);
    } else if (fshift != 0u) {
        q = x >> fshift;
    } else {
        q = fallback <= 1u ? x : x / fallback;
    }
#elif defined(_MSC_VER) && defined(_M_X64)
    /* 同 fastdiv.h 的 _umul128 路径：product>>51 == hi<<13 | lo>>51 */
    if (magic != 0u) {
        uint64_t hi = 0;
        uint64_t lo = _umul128((uint64_t)x, magic, &hi);
        q = (uint32_t)((hi << 13) | (lo >> 51));
    } else if (fshift != 0u) {
        q = x >> fshift;
    } else {
        q = fallback <= 1u ? x : x / fallback;
    }
#else
    /* 平台兜底：真除数值恒等，同 fastdiv.h 策略 */
    q = fallback <= 1u ? x : x / fallback;
#endif
    return (q << TC_RANS_SCALE_BITS) + (x - q * f) + cum;
}

/* V7-R2 编码核心：joints/hists → per-slice 模型 argmin → flags + 条件表
 * + rANS 流。bw/bytes_out 契约同 color_tokens_rans_encode（m7 sized 探针
 * 以 bw=NULL 复用，与最终编码逐字节一致）。C1：scratch 从 es 槽位池借用。 */
/* C5 剖析（TOPOS_CODEC_PROFILE=1）：r2e 内部六段累计 ns——
 * [0]collect_joints [1]模型选择(rows_cost×4) [2]前缀表(row_enc/dec)
 * [3]fastdiv+scratch [4]后向环 [5]刷写+bitwriter 拷贝。getter 供
 * 差分机读取；profile 关闭时零开销（每 slice 6 次分支）。 */
static _Atomic uint64_t g_r2e_stage_ns[6];

void tc_dev_r2e_stage_ns(uint64_t* dst)
{
    for (int i = 0; i < 6; ++i) {
        dst[i] = atomic_load_explicit(&g_r2e_stage_ns[i], memory_order_relaxed);
    }
}

static int32_t color_tokens_rans2_encode(const enc_band_tok* tok, size_t blocks,
                                         const uint32_t* dc_hist,
                                         const uint32_t* run_hist,
                                         const uint32_t* lvl_hist,
                                         tc_bitwriter* bw, size_t* bytes_out,
                                         enc_shared* es, uint32_t slot)
{
    const int r2e_prof = tc_profile_enabled();
    uint64_t r2e_pt = r2e_prof ? tc_profile_now_ns() : 0u;
#define TC_R2E_STAGE(i) do { \
    if (r2e_prof) { uint64_t now_ = tc_profile_now_ns(); \
        atomic_fetch_add_explicit(&g_r2e_stage_ns[i], now_ - r2e_pt, \
                                  memory_order_relaxed); r2e_pt = now_; } } while (0)
    rans2_joints joints;
    rans2_collect_joints(tok, blocks, &joints);
    TC_R2E_STAGE(0);

    /* 模型选择：代价 = 量化表精确符号位 + 表字节（族间独立可加；
     * 候选序 NONE→POS→PREV，严格小于才切换——确定性平手裁决） */
    uint32_t lvl_model = TC_RANS2_LVL_NONE;
    double best = rans2_rows_cost(lvl_hist, 1u, TC_RANS_LVL_SYMS)
                + 8.0 * (double)TC_RANS_LVL_SYMS;
    const double pos_cost = rans2_rows_cost(joints.lvl_pos, TC_RANS2_POS_CTX,
                                            TC_RANS_LVL_SYMS)
                          + 8.0 * (double)(TC_RANS2_POS_CTX * TC_RANS_LVL_SYMS);
    if (pos_cost < best) {
        best = pos_cost;
        lvl_model = TC_RANS2_LVL_POS;
    }
    const double prev_cost = rans2_rows_cost(joints.lvl_prev, TC_RANS2_PREV_CTX,
                                             TC_RANS_LVL_SYMS)
                           + 8.0 * (double)(TC_RANS2_PREV_CTX * TC_RANS_LVL_SYMS);
    if (prev_cost < best) {
        lvl_model = TC_RANS2_LVL_PREV;
    }
    uint32_t dc_model = TC_RANS2_DC_NONE;
    const double dc_none_cost = rans2_rows_cost(dc_hist, 1u, TC_RANS_DC_SYMS)
                              + 8.0 * (double)TC_RANS_DC_SYMS;
    const double dc_prev_cost = rans2_rows_cost(joints.dc_prev, TC_RANS2_DC_CTX,
                                                TC_RANS_DC_SYMS)
                              + 8.0 * (double)(TC_RANS2_DC_CTX * TC_RANS_DC_SYMS);
    if (dc_prev_cost < dc_none_cost) {
        dc_model = TC_RANS2_DC_PREV;
    }

    TC_R2E_STAGE(1);
    const uint32_t lvl_ctx_n = lvl_model == TC_RANS2_LVL_POS ? TC_RANS2_POS_CTX
                             : (lvl_model == TC_RANS2_LVL_PREV ? TC_RANS2_PREV_CTX
                                                               : 1u);
    const uint32_t dc_ctx_n = dc_model == TC_RANS2_DC_PREV ? TC_RANS2_DC_CTX : 1u;

    /* 前缀 = flags + 条件表（行序 = ctx 0..C−1；编码模型从写出的字节重建，
     * 与解码侧同源） */
    uint8_t prefix[TC_RANS2_PREFIX_MAX];
    size_t off = 0u;
    prefix[off++] = (uint8_t)(lvl_model | (dc_model << 2));
    tc_rans_model dc_m[TC_RANS2_DC_CTX];
    tc_rans_model lvl_m[TC_RANS2_PREV_CTX];
    tc_rans_model run_m;
    const uint32_t* dc_rows = dc_model == TC_RANS2_DC_PREV ? joints.dc_prev : dc_hist;
    for (uint32_t c = 0u; c < dc_ctx_n; ++c) {
        tc_rans2_row_encode(prefix + off, TC_RANS_DC_SYMS,
                            dc_rows + (size_t)c * TC_RANS_DC_SYMS);
        int32_t rc = tc_rans2_row_decode(prefix + off, TC_RANS_DC_SYMS, &dc_m[c]);
        if (rc != TC_OK) { return rc; }
        off += TC_RANS_DC_SYMS;
    }
    tc_rans2_row_encode(prefix + off, TC_RANS_RUN_SYMS, run_hist);
    int32_t rc = tc_rans2_row_decode(prefix + off, TC_RANS_RUN_SYMS, &run_m);
    if (rc != TC_OK) { return rc; }
    off += TC_RANS_RUN_SYMS;
    const uint32_t* lvl_rows = lvl_model == TC_RANS2_LVL_POS ? joints.lvl_pos
                             : (lvl_model == TC_RANS2_LVL_PREV ? joints.lvl_prev
                                                               : lvl_hist);
    for (uint32_t c = 0u; c < lvl_ctx_n; ++c) {
        tc_rans2_row_encode(prefix + off, TC_RANS_LVL_SYMS,
                            lvl_rows + (size_t)c * TC_RANS_LVL_SYMS);
        rc = tc_rans2_row_decode(prefix + off, TC_RANS_LVL_SYMS, &lvl_m[c]);
        if (rc != TC_OK) { return rc; }
        off += TC_RANS_LVL_SYMS;
    }

    TC_R2E_STAGE(2);
    /* S4（速度计划 P1）：per-model per-symbol 快除表——与模型同源同寿命
     * （除数 = freq[sym]；freq 0 槽不可编码，垫 d=1 不达）。每 slice 约
     * 349 次 init（≈µs 级），换后向环每符号一次 u32 idiv 为 128-bit
     * mulhi（P0：tc_rans_put 占 band 时间 41%）。 */
    tc_fastdiv dc_fd[TC_RANS2_DC_CTX][TC_RANS_DC_SYMS];
    tc_fastdiv lvl_fd[TC_RANS2_PREV_CTX][TC_RANS_LVL_SYMS];
    tc_fastdiv run_fd[TC_RANS_RUN_SYMS];
    for (uint32_t c = 0u; c < dc_ctx_n; ++c) {
        for (uint32_t s = 0u; s < TC_RANS_DC_SYMS; ++s) {
            const uint32_t d = dc_m[c].freq[s] != 0u ? dc_m[c].freq[s] : 1u;
            tc_fastdiv_init(d, &dc_fd[c][s]);
        }
    }
    for (uint32_t s = 0u; s < TC_RANS_RUN_SYMS; ++s) {
        const uint32_t d = run_m.freq[s] != 0u ? run_m.freq[s] : 1u;
        tc_fastdiv_init(d, &run_fd[s]);
    }
    for (uint32_t c = 0u; c < lvl_ctx_n; ++c) {
        for (uint32_t s = 0u; s < TC_RANS_LVL_SYMS; ++s) {
            const uint32_t d = lvl_m[c].freq[s] != 0u ? lvl_m[c].freq[s] : 1u;
            tc_fastdiv_init(d, &lvl_fd[c][s]);
        }
    }

    /* 后缀位总数与 scratch 上界：与 rans 同式（符号数/后缀位不因上下文变） */
    uint64_t suffix_bits = 0u;
    for (uint32_t s = 2u; s < TC_RANS_DC_SYMS; ++s) {
        suffix_bits += (uint64_t)dc_hist[s] * (uint64_t)(s - 1u);
    }
    for (uint32_t s = 2u; s < TC_RANS_LVL_SYMS; ++s) {
        suffix_bits += (uint64_t)lvl_hist[s] * (uint64_t)(s - 1u);
    }
    const uint64_t ops = 2ull * (uint64_t)blocks + 2ull * (uint64_t)tok->token_count
                       + suffix_bits;
    const size_t cap = (size_t)(2ull * ops + 8ull);
    uint8_t* scratch = enc_scratch_acquire(es, slot, cap);
    if (scratch == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "rans2 scratch %zu bytes", cap);
        return TC_ERR_OUT_OF_MEMORY;
    }
    TC_R2E_STAGE(3);
    tc_rans_enc enc;
    tc_rans_enc_init(&enc, scratch, cap);

    /* 后向遍历（解码正向序的严格倒序）。上下文回放：
     *  - D1：块 b 的 ctx = 首块?0:桶(前块 dc 类)——dc_m 数组直查；
     *  - L4：块内前向扫描预算各对 zigzag 位置——仅 POS 模型需要
     *    （NONE/PREV 不再支付该前缀和遍，S5）；
     *  - L2：编码对 p 的前向邻居 p−1 即后向游走的"上一已编码对"——
     *    p==0 为块首，编码完一对更新 prev_cat。
     * S5：按 lvl_model 专化块内对环——模型分支/argmin 双精度比较提出
     * 环外（每块一次分派），x 值语义引擎见上；位流与原
     * tc_rans_put_fast 环逐位一致。 */
    uint32_t x = enc.x;
    size_t pi = (size_t)tok->token_count;
    for (size_t b = blocks; b-- > 0u;) {
        const uint32_t np = tok->npair[b];
        pi -= np;
        x = rans2_put_x(x, &enc, run_m.freq[63u], run_m.cum[63u], run_fd[63u],
                        &rc);
        if (rc != TC_OK) { goto done; }
        if (lvl_model == TC_RANS2_LVL_POS) {
            uint8_t pos_block[63];
            uint32_t prev = 0u;
            for (uint32_t p = 0u; p < np; ++p) {
                prev += tok->run[pi + p] + 1u;
                pos_block[p] = (uint8_t)prev;
            }
            for (uint32_t p = np; p-- > 0u;) {
                const uint32_t lm = tok->lvl_m[pi + p];
                const uint32_t lsym = tc_vlc_bitlen32(lm);
                x = rans2_put_suffix_x(x, &enc, lsym, lm, &rc);
                if (rc != TC_OK) { goto done; }
                const uint32_t lc = tc_rans2_pos_ctx(pos_block[p]);
                                                x = rans2_put_x(x, &enc, lvl_m[lc].freq[lsym], lvl_m[lc].cum[lsym],
                                lvl_fd[lc][lsym], &rc);
                if (rc != TC_OK) { goto done; }
                const uint32_t r = tok->run[pi + p];
                x = rans2_put_x(x, &enc, run_m.freq[r], run_m.cum[r], run_fd[r],
                                &rc);
                if (rc != TC_OK) { goto done; }
            }
        } else if (lvl_model == TC_RANS2_LVL_PREV) {
            for (uint32_t p = np; p-- > 0u;) {
                const uint32_t lm = tok->lvl_m[pi + p];
                const uint32_t lsym = tc_vlc_bitlen32(lm);
                x = rans2_put_suffix_x(x, &enc, lsym, lm, &rc);
                if (rc != TC_OK) { goto done; }
                /* 前 lvl 上下文 = 前向邻居 p−1 的类（后向游走中尚未编码）——
                 * 直接数组回查，勿用游走状态（那是 p+1） */
                const uint32_t lc = tc_rans2_prevlvl_ctx(
                    p != 0u ? tc_vlc_bitlen32(tok->lvl_m[pi + p - 1u]) : 0u,
                    p != 0u);
                                                x = rans2_put_x(x, &enc, lvl_m[lc].freq[lsym], lvl_m[lc].cum[lsym],
                                lvl_fd[lc][lsym], &rc);
                if (rc != TC_OK) { goto done; }
                const uint32_t r = tok->run[pi + p];
                x = rans2_put_x(x, &enc, run_m.freq[r], run_m.cum[r], run_fd[r],
                                &rc);
                if (rc != TC_OK) { goto done; }
            }
        } else {
            for (uint32_t p = np; p-- > 0u;) {
                const uint32_t lm = tok->lvl_m[pi + p];
                const uint32_t lsym = tc_vlc_bitlen32(lm);
                x = rans2_put_suffix_x(x, &enc, lsym, lm, &rc);
                if (rc != TC_OK) { goto done; }
                                x = rans2_put_x(x, &enc, lvl_m[0u].freq[lsym], lvl_m[0u].cum[lsym],
                                lvl_fd[0u][lsym], &rc);
                if (rc != TC_OK) { goto done; }
                const uint32_t r = tok->run[pi + p];
                x = rans2_put_x(x, &enc, run_m.freq[r], run_m.cum[r], run_fd[r],
                                &rc);
                if (rc != TC_OK) { goto done; }
            }
        }
        const uint32_t dcm = tok->dc_m[b];
        const uint32_t dsym = tc_vlc_bitlen32(dcm);
        x = rans2_put_suffix_x(x, &enc, dsym, dcm, &rc);
        if (rc != TC_OK) { goto done; }
        const uint32_t dprev_cat = b != 0u ? tc_vlc_bitlen32(tok->dc_m[b - 1u]) : 0u;
        const uint32_t dctx = dc_model == TC_RANS2_DC_PREV
            ? tc_rans2_dc_ctx(dprev_cat, b != 0u) : 0u;
                x = rans2_put_x(x, &enc, dc_m[dctx].freq[dsym], dc_m[dctx].cum[dsym],
                        dc_fd[dctx][dsym], &rc);
        if (rc != TC_OK) { goto done; }
    }
    TC_R2E_STAGE(4);
    enc.x = x;
    rc = tc_rans_enc_flush(&enc);
    if (rc > 0) {
        const size_t rans_bytes = (size_t)rc;
        rc = TC_OK;
        if (bytes_out != NULL) {
            *bytes_out = off + rans_bytes;
        }
        if (bw != NULL) {
            /* C5：批量字节追加（同上） */
            rc = tc_bitwriter_put_bytes(bw, prefix, off);
            if (rc == TC_OK) {
                rc = tc_bitwriter_put_bytes(bw, scratch, rans_bytes);
            }
        }
    }
done:
    TC_R2E_STAGE(5);
    enc.x = x;
    return rc; /* C1：scratch 归槽位池，无每 slice 释放 */
#undef TC_R2E_STAGE
}

static int32_t emit_color_tokens_rans2(const enc_band_tok* tok, size_t blocks,
                                       const uint32_t* dc_hist,
                                       const uint32_t* run_hist,
                                       const uint32_t* lvl_hist,
                                       tc_bitwriter* bw, enc_shared* es,
                                       uint32_t slot)
{
    return color_tokens_rans2_encode(tok, blocks, dc_hist, run_hist, lvl_hist,
                                     bw, NULL, es, slot);
}

/* V8 批 0 实验专用：给定（已构建）模型的流发射——与
 * color_tokens_rans2_encode 的后向环/flush 逐位同构，仅模型来源改为
 * 参数（瓦片聚合表模拟；生产路径不调用）。返回流字节数（含 4B 终态），
 * 不含前缀表；suffix_bits 从 token 直导（cat = bitlen，与 hist 同值）。 */
static int32_t rans2_encode_stream_with_models(const enc_band_tok* tok,
                                               size_t blocks,
                                               uint32_t lvl_model,
                                               uint32_t dc_model,
                                               const tc_rans_model* dc_m,
                                               const tc_rans_model* run_m,
                                               const tc_rans_model* lvl_m,
                                               enc_shared* es, uint32_t slot,
                                               size_t* stream_out)
{
    const uint32_t lvl_ctx_n = lvl_model == TC_RANS2_LVL_POS ? TC_RANS2_POS_CTX
                             : (lvl_model == TC_RANS2_LVL_PREV ? TC_RANS2_PREV_CTX
                                                               : 1u);
    const uint32_t dc_ctx_n = dc_model == TC_RANS2_DC_PREV ? TC_RANS2_DC_CTX : 1u;
    tc_fastdiv dc_fd[TC_RANS2_DC_CTX][TC_RANS_DC_SYMS];
    tc_fastdiv lvl_fd[TC_RANS2_PREV_CTX][TC_RANS_LVL_SYMS];
    tc_fastdiv run_fd[TC_RANS_RUN_SYMS];
    for (uint32_t c = 0u; c < dc_ctx_n; ++c) {
        for (uint32_t s = 0u; s < TC_RANS_DC_SYMS; ++s) {
            const uint32_t d = dc_m[c].freq[s] != 0u ? dc_m[c].freq[s] : 1u;
            tc_fastdiv_init(d, &dc_fd[c][s]);
        }
    }
    for (uint32_t s = 0u; s < TC_RANS_RUN_SYMS; ++s) {
        const uint32_t d = run_m->freq[s] != 0u ? run_m->freq[s] : 1u;
        tc_fastdiv_init(d, &run_fd[s]);
    }
    for (uint32_t c = 0u; c < lvl_ctx_n; ++c) {
        for (uint32_t s = 0u; s < TC_RANS_LVL_SYMS; ++s) {
            const uint32_t d = lvl_m[c].freq[s] != 0u ? lvl_m[c].freq[s] : 1u;
            tc_fastdiv_init(d, &lvl_fd[c][s]);
        }
    }
    uint64_t suffix_bits = 0u;
    {
        size_t pi = 0u;
        for (size_t b = 0u; b < blocks; ++b) {
            const uint32_t dcat = tc_vlc_bitlen32(tok->dc_m[b]);
            if (dcat >= 2u) { suffix_bits += dcat - 1u; }
            const uint32_t np = tok->npair[b];
            for (uint32_t p = 0u; p < np; ++p) {
                const uint32_t lcat = tc_vlc_bitlen32(tok->lvl_m[pi + p]);
                if (lcat >= 2u) { suffix_bits += lcat - 1u; }
            }
            pi += np;
        }
    }
    const uint64_t ops = 2ull * (uint64_t)blocks + 2ull * (uint64_t)tok->token_count
                       + suffix_bits;
    const size_t cap = (size_t)(2ull * ops + 8ull);
    uint8_t* scratch = enc_scratch_acquire(es, slot, cap);
    if (scratch == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "rans2 sim scratch %zu bytes", cap);
        return TC_ERR_OUT_OF_MEMORY;
    }
    tc_rans_enc enc;
    tc_rans_enc_init(&enc, scratch, cap);
    int32_t rc = TC_OK;
    uint32_t x = enc.x;
    size_t pi = (size_t)tok->token_count;
    for (size_t b = blocks; b-- > 0u;) {
        const uint32_t np = tok->npair[b];
        pi -= np;
                x = rans2_put_x(x, &enc, run_m->freq[63u], run_m->cum[63u], run_fd[63u],
                        &rc);
        if (rc != TC_OK) { goto done; }
        if (lvl_model == TC_RANS2_LVL_POS) {
            uint8_t pos_block[63];
            uint32_t prev = 0u;
            for (uint32_t p = 0u; p < np; ++p) {
                prev += tok->run[pi + p] + 1u;
                pos_block[p] = (uint8_t)prev;
            }
            for (uint32_t p = np; p-- > 0u;) {
                const uint32_t lm = tok->lvl_m[pi + p];
                const uint32_t lsym = tc_vlc_bitlen32(lm);
                x = rans2_put_suffix_x(x, &enc, lsym, lm, &rc);
                if (rc != TC_OK) { goto done; }
                const uint32_t lc = tc_rans2_pos_ctx(pos_block[p]);
                                                x = rans2_put_x(x, &enc, lvl_m[lc].freq[lsym], lvl_m[lc].cum[lsym],
                                lvl_fd[lc][lsym], &rc);
                if (rc != TC_OK) { goto done; }
                const uint32_t r = tok->run[pi + p];
                                x = rans2_put_x(x, &enc, run_m->freq[r], run_m->cum[r], run_fd[r],
                                &rc);
                if (rc != TC_OK) { goto done; }
            }
        } else if (lvl_model == TC_RANS2_LVL_PREV) {
            for (uint32_t p = np; p-- > 0u;) {
                const uint32_t lm = tok->lvl_m[pi + p];
                const uint32_t lsym = tc_vlc_bitlen32(lm);
                x = rans2_put_suffix_x(x, &enc, lsym, lm, &rc);
                if (rc != TC_OK) { goto done; }
                const uint32_t lc = tc_rans2_prevlvl_ctx(
                    p != 0u ? tc_vlc_bitlen32(tok->lvl_m[pi + p - 1u]) : 0u,
                    p != 0u);
                                x = rans2_put_x(x, &enc, lvl_m[lc].freq[lsym], lvl_m[lc].cum[lsym],
                                lvl_fd[lc][lsym], &rc);
                if (rc != TC_OK) { goto done; }
                const uint32_t r = tok->run[pi + p];
                                x = rans2_put_x(x, &enc, run_m->freq[r], run_m->cum[r], run_fd[r],
                                &rc);
                if (rc != TC_OK) { goto done; }
            }
        } else {
            for (uint32_t p = np; p-- > 0u;) {
                const uint32_t lm = tok->lvl_m[pi + p];
                const uint32_t lsym = tc_vlc_bitlen32(lm);
                x = rans2_put_suffix_x(x, &enc, lsym, lm, &rc);
                if (rc != TC_OK) { goto done; }
                x = rans2_put_x(x, &enc, lvl_m[0u].freq[lsym], lvl_m[0u].cum[lsym],
                                lvl_fd[0u][lsym], &rc);
                if (rc != TC_OK) { goto done; }
                const uint32_t r = tok->run[pi + p];
                                x = rans2_put_x(x, &enc, run_m->freq[r], run_m->cum[r], run_fd[r],
                                &rc);
                if (rc != TC_OK) { goto done; }
            }
        }
        const uint32_t dcm = tok->dc_m[b];
        const uint32_t dsym = tc_vlc_bitlen32(dcm);
        x = rans2_put_suffix_x(x, &enc, dsym, dcm, &rc);
        if (rc != TC_OK) { goto done; }
        const uint32_t dprev_cat = b != 0u ? tc_vlc_bitlen32(tok->dc_m[b - 1u]) : 0u;
        const uint32_t dctx = dc_model == TC_RANS2_DC_PREV
            ? tc_rans2_dc_ctx(dprev_cat, b != 0u) : 0u;
        x = rans2_put_x(x, &enc, dc_m[dctx].freq[dsym], dc_m[dctx].cum[dsym],
                        dc_fd[dctx][dsym], &rc);
        if (rc != TC_OK) { goto done; }
    }
    enc.x = x;
    rc = tc_rans_enc_flush(&enc);
    if (rc > 0) {
        *stream_out = (size_t)rc;
        rc = TC_OK;
    }
done:
    return rc;
}

/* V8 批 0 实验专用：生产同构的模型选择镜像（严格复制
 * color_tokens_rans2_encode 的候选序/平手裁决——位差由调用方的 A 锚
 * 差分校验兜底）。 */
static void rans2_sim_pick_models(const uint32_t* dc_hist,
                                  const uint32_t* lvl_hist,
                                  const rans2_joints* joints,
                                  uint32_t* lvl_model_out, uint32_t* dc_model_out)
{
    uint32_t lvl_model = TC_RANS2_LVL_NONE;
    double best = rans2_rows_cost(lvl_hist, 1u, TC_RANS_LVL_SYMS)
                + 8.0 * (double)TC_RANS_LVL_SYMS;
    const double pos_cost = rans2_rows_cost(joints->lvl_pos, TC_RANS2_POS_CTX,
                                            TC_RANS_LVL_SYMS)
                          + 8.0 * (double)(TC_RANS2_POS_CTX * TC_RANS_LVL_SYMS);
    if (pos_cost < best) {
        best = pos_cost;
        lvl_model = TC_RANS2_LVL_POS;
    }
    const double prev_cost = rans2_rows_cost(joints->lvl_prev, TC_RANS2_PREV_CTX,
                                             TC_RANS_LVL_SYMS)
                           + 8.0 * (double)(TC_RANS2_PREV_CTX * TC_RANS_LVL_SYMS);
    if (prev_cost < best) {
        lvl_model = TC_RANS2_LVL_PREV;
    }
    uint32_t dc_model = TC_RANS2_DC_NONE;
    const double dc_none_cost = rans2_rows_cost(dc_hist, 1u, TC_RANS_DC_SYMS)
                              + 8.0 * (double)TC_RANS_DC_SYMS;
    const double dc_prev_cost = rans2_rows_cost(joints->dc_prev, TC_RANS2_DC_CTX,
                                                TC_RANS_DC_SYMS)
                              + 8.0 * (double)(TC_RANS2_DC_CTX * TC_RANS_DC_SYMS);
    if (dc_prev_cost < dc_none_cost) {
        dc_model = TC_RANS2_DC_PREV;
    }
    *lvl_model_out = lvl_model;
    *dc_model_out = dc_model;
}

/* M7：符号精确位长（q ≤ 30 → q+1+k；escape → 63；spec §6.2 与 writer 一致） */
static inline uint64_t rice_bits_exact(uint32_t m, uint32_t k)
{
    uint32_t q = m >> k;
    return q <= 30u ? (uint64_t)q + 1u + (uint64_t)k : 63u;
}

/* M7：token 集精确位计数（含每块 EOB）——与 emit_color_tokens 写出的位数
 * 严格一致（flush 只补 <8 位到字节界）。 */
static uint64_t color_tokens_bits(const enc_band_tok* tok, size_t blocks,
                                  uint32_t k1, uint32_t k2, uint32_t k3)
{
    uint64_t bits = (uint64_t)blocks * rice_bits_exact(63u, k3); /* 每块 EOB */
    size_t total_pairs = 0u;
    for (size_t b = 0u; b < blocks; ++b) {
        bits += rice_bits_exact(tok->dc_m[b], k1);
        total_pairs += tok->npair[b];
    }
    for (size_t p = 0u; p < total_pairs; ++p) {
        bits += rice_bits_exact(tok->run[p], k3);
        bits += rice_bits_exact(tok->lvl_m[p], k2);
    }
    return bits;
}

/* M7：F-cache 版 fill——量化源为缓存的 DCT 系数（无 pad/采样加载/DCT）。
 * token/k 逻辑与 fill_color_band_tokens 逐值同源（F 相同 → qz 相同）。 */
/* ===================== V2.x 逐系数 level RDO 精修（2026-09-04） =====================
 * reserved[2]=1 启用（编码端决策，码流零格式变更——level 集合任何取值
 * 都是合法流，解码侧/金样本零感知；probe 与 final 共用本函数 → 计数/
 * 发射逐值一致）。AC level 候选 {l±1, 置零} 贪心精修，判据
 * ΔD + λ·ΔR < 0（D = 变换域 SSE——Parseval 下 ∝ 像素域；R = VLC
 * 符号位差分）。
 * 策略定标（4K444/2K422/12-bit 素材扫描，2026-09-04）：
 *  - λ = 0.080·Q²·2^((qp−52)/4)（Q² 为失能量纲归一；qp 坡度吸收帧
 *    工作点斜率——率低→最优 k 大，实测 ≈每 4 qp 翻倍）；
 *  - 色度（plane≠0）λ×0.5：luma↔chroma 预算交换的中档（μ 扫描
 *    25/50/100%：444 家族 50% ≈ +0.4~0.6 dB Y / −1~3 dB UV @同码率，
 *    100% 可到 +0.9~1.9 dB Y 但 12-bit UV −5.3 dB——激进档留人工选择）；
 *  - 码率先验：qp 档位 VLC 本（本 id = qp 档位设计点）；±1 差分同本，
 *    本选择误差大部分抵消；最终表仍由真实直方图重选（发射精确）；
 *  - 置零的 RUN 结构效应精确建模：本 pair（run,level）消失 + 相邻
 *    pair run 合并（块内合并 run ≤ 62，RUN 符号域内恒可表示）；
 *  - DC 不精修（跨块预测链耦合，范围外）。 */
static uint32_t rdo_qp_bucket(uint32_t qp)
{
    return qp < 18u ? 0u : qp < 30u ? 1u : qp < 42u ? 2u : 3u;
}

static inline uint32_t rdo_cat_bits(const uint8_t* len, uint32_t m)
{
    uint32_t cat = tc_vlc_bitlen32(m);
    return (uint32_t)len[cat] + (cat > 1u ? cat - 1u : 0u);
}

static void tc_rdo_refine(uint64_t* nz_io, int32_t q_nat[64], const int32_t* F,
                          const tc_quant_ctx* qctx, uint32_t qp_eff,
                          int is_chroma)
{
    const uint8_t* lvll = tc_vlc_lvl_len[rdo_qp_bucket(qp_eff)];
    const uint8_t* runl = tc_vlc_run_len[rdo_qp_bucket(qp_eff)];
    /* 色度 λ×0.5 仅对真色度平面（pf≠3 的 plane1/2）；TRAW CFA 全平面同 λ */
    const double lam_k = 0.080
        * pow(2.0, ((double)qp_eff - 52.0) / 4.0)
        * (is_chroma ? 0.5 : 1.0);

    /* 非零 AC 列表（zz 序，DC 排除）+ 动态存活标记（置零改变相邻 run）。
     * P-速⑥：q 为自然序布局（位级语义不变——读取经 kTcZigzag 定位）。 */
    uint32_t pos[63];
    uint8_t alive[63];
    uint32_t n = 0u;
    uint64_t nz = *nz_io >> 1u; /* bit j ↔ 扫描位置 j+1 */
    while (nz != 0u) {
        pos[n] = (uint32_t)tc_ctz64(nz) + 1u;
        alive[n] = 1u;
        ++n;
        nz &= nz - 1u;
    }
    for (uint32_t i = 0u; i < n; ++i) {
        const uint32_t p = pos[i];
        const uint32_t nat = kTcZigzag[p];
        const int32_t l = q_nat[nat];
        const int64_t Q = (int64_t)qctx->Q[nat];
        const int64_t f = (int64_t)F[nat];
        const int64_t e0 = f - (int64_t)l * Q;
        const double lam = lam_k * (double)Q * (double)Q;
        const uint32_t r0 = rdo_cat_bits(lvll, tc_rice_map_signed(l));
        /* 前后存活邻居（run 的当前真实值由存活邻居间距决定） */
        int32_t ia = (int32_t)i - 1;
        while (ia >= 0 && alive[ia] == 0u) { --ia; }
        int32_t ib = (int32_t)i + 1;
        while ((uint32_t)ib < n && alive[ib] == 0u) { ++ib; }
        const uint32_t run_i = (ia >= 0)
            ? pos[i] - pos[ia] - 1u : pos[i] - 1u;
        int32_t best_l = l;
        double best_gain = 0.0;
        /* ±1（保持非零 → RUN 结构不变） */
        for (int32_t d = -1; d <= 1; d += 2) {
            const int32_t lc = l + d;
            if (lc == 0) { continue; }
            const int64_t ec = f - (int64_t)lc * Q;
            const double dd = (double)(ec * ec - e0 * e0);
            const double dr = (double)((int64_t)rdo_cat_bits(
                lvll, tc_rice_map_signed(lc)) - (int64_t)r0);
            const double g = -(dd + lam * dr);
            if (g > best_gain) { best_gain = g; best_l = lc; }
        }
        /* 置零：本 pair 消失 + 相邻存活 pair 的 run 合并（色度 λ×0.5
         * 已并入 lam_k——444 家族静态 chroma_qp_offset=4 之上无乘子
         * 置零会把 UV 打穿，12-bit 实测 −5.3 dB） */
        {
            double dr = -((double)((int64_t)r0 + (int64_t)runl[run_i]));
            if ((uint32_t)ib < n) {
                const uint32_t run_b = pos[ib] - pos[i] - 1u;
                const uint32_t rm = pos[ib] - (ia >= 0 ? pos[ia] : 0u) - 1u;
                dr += (double)((int64_t)runl[rm] - (int64_t)runl[run_b]);
            }
            const double dd = (double)(f * f - e0 * e0);
            const double g = -(dd + lam * dr);
            if (g > best_gain) { best_gain = g; best_l = 0; }
        }
        if (best_l != l) {
            q_nat[nat] = best_l;
            if (best_l == 0) { alive[i] = 0u; }
        }
    }
    /* 重建非零掩码（扫描域 bit p；等价于逐位置重扫——alive 恰跟踪
     * "精修后非零"，初始位即非零 q；DC 位恒清——提取侧本就右移丢弃） */
    uint64_t mask = 0u;
    for (uint32_t i = 0u; i < n; ++i) {
        if (alive[i] != 0u) { mask |= UINT64_C(1) << (pos[i] - 1u); }
    }
    *nz_io = mask << 1u;
}

static int32_t fill_color_band_from_f(const topos_frame_header* fh, const int32_t* fplane,
                                   uint32_t plane, const tc_quant_ctx* qctx,
                                   enc_band_tok* tok, int32_t* dc_a, int32_t* dc_b,
                                   uint32_t y0, uint32_t h,
                                   uint32_t* k1, uint32_t* k2, uint32_t* k3,
                                   uint32_t* dc_hist, uint32_t* run_hist,
                                   uint32_t* lvl_hist, uint32_t qp_eff_hint,
                                   int rdo_on)
{
    uint32_t domain_violation = 0u;  /* 同 fill_color_band_tokens（P4②） */
    uint32_t cols = fh->plane_block_cols[plane];
    uint64_t sum_dc = 0u, cnt_dc = 0u, sum_level = 0u, cnt_level = 0u, sum_run = 0u, cnt_run = 0u;
    /* P-速⑥：自然序量化（免 gather；掩码翻译 + 非零处 kTcZigzag 读） */
    const tc_quant_nat_fn quant_nat = tc_simd_resolve_quant_nat((uint8_t)(fh->bit_depth > 12u));
    /* P-速④：VLC 模式下 Rice k 统计为死值（同 fill_color_band_tokens）；
     * sym_hist 开关提出循环（每带一次）。 */
    const int need_rice = dc_hist == NULL;
    const int sym_on = atomic_load_explicit(&s_sym_hist_on, memory_order_relaxed) != 0;
    const int capture_v7 = fh->version_major == 7u && fh->entropy_mode == 5u;
    if (capture_v7 != 0) { tc_v7_band_stats_reset(&tok->v7_stats); }
    /* C036 前置测量：VLC/rANS 模式（hist 非 NULL）下的 order-1 上下文捕获
     * （dev-only；Rice 路径与 alpha 不参与——符号族不同） */
    tc_ctx_cap* cap = (tc_dev_ctx_active() != 0 && dc_hist != NULL)
                          ? tc_ctx_cap_alloc() : NULL;

    memset(dc_a, 0, (size_t)cols * sizeof(int32_t));
    memset(dc_b, 0, (size_t)cols * sizeof(int32_t));
    size_t bi = 0u;
    size_t pi = 0u;
    for (uint32_t by = 0u; by < h; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            const int32_t* F = fplane + ((size_t)(y0 + by) * (size_t)cols + (size_t)bx) * 64u;
            int32_t qn[64];
            uint64_t nat = 0u;
            quant_nat(qctx, F, qn, &nat);
            uint64_t nz = tc_zz_mask_from_nat(nat);
            if (rdo_on != 0) {
                tc_rdo_refine(&nz, qn, F, qctx, qp_eff_hint,
                              is_chroma_plane(fh, plane) ? 1 : 0);
            }
            tc_v7_band_bucket v7_bucket;
            if (capture_v7 != 0) { tc_v7_band_bucket_init(&v7_bucket, (uint32_t)bi); }

            int has_left = bx > 0u ? 1 : 0;
            int has_top = by > 0u ? 1 : 0;
            int32_t pred = tc_dc_predict(has_left, dc_b[bx > 0u ? bx - 1u : 0u],
                                         has_top, dc_a[bx]);
            uint32_t dcm = tc_rice_map_signed(qn[0] - pred);
            if (dcm > TC_RICE_M_MAX_DC_BD16) {
                dcm = TC_RICE_M_MAX_DC_BD16;
                domain_violation = 1u;
            }
            tok->dc_m[bi] = dcm;
            if (need_rice != 0) {
                sum_dc += dcm;
                cnt_dc++;
            }
            dc_b[bx] = qn[0];
            if (dc_hist != NULL) { dc_hist[tc_vlc_bitlen32(dcm)]++; }
            if (sym_on) { sym_hist_dc(plane, dcm); }
            if (cap != NULL) { tc_ctx_dc(cap, dcm); }

            nz >>= 1u; /* 掩码跳扫（同 fill_color_band_tokens） */
            uint32_t np = 0u;
            uint32_t prev = 0u;
            while (nz != 0u) {
                const uint32_t pos = tc_ctz64(nz) + 1u;
                uint32_t lm = tc_rice_map_signed(qn[kTcZigzag[pos]]);
                if (lm > TC_RICE_M_MAX_AC_LEVEL_BD16) {
                    lm = TC_RICE_M_MAX_AC_LEVEL_BD16;
                    domain_violation = 1u;
                }
                uint32_t runv = pos - prev - 1u;
                tok->run[pi] = (uint8_t)runv;
                tok->lvl_m[pi] = lm;
                pi++;
                if (capture_v7 != 0) {
                    (void)tc_v7_band_bucket_push(&v7_bucket, (uint8_t)pos, lm, NULL, NULL);
                }
                if (need_rice != 0) {
                    sum_run += runv;
                    cnt_run++;
                    sum_level += lm;
                    cnt_level++;
                }
                if (run_hist != NULL) { run_hist[runv]++; }
                if (lvl_hist != NULL) { lvl_hist[tc_vlc_bitlen32(lm)]++; }
                if (sym_on) { sym_hist_pair(plane, runv, lm); }
                if (cap != NULL) { tc_ctx_pair(cap, prev, pos, runv, lm); }
                prev = pos;
                nz &= nz - 1u;
                np++;
            }
            tok->npair[bi] = (uint16_t)np;
            if (need_rice != 0) {
                sum_run += 63u; /* EOB（run 符号值恒 63） */
                cnt_run++;
            }
            if (run_hist != NULL) { run_hist[63u]++; }
            if (sym_on) { sym_hist_eob(plane); }
            if (cap != NULL) { tc_ctx_eob(cap, prev); }
            if (capture_v7 != 0) { (void)tc_v7_band_stats_add_block(&tok->v7_stats, &v7_bucket); }
            bi++;
        }
        int32_t* t = dc_a; dc_a = dc_b; dc_b = t;
    }
    tok->token_count = (uint32_t)pi;
    if (cap != NULL) {
        tc_ctx_fold(cap, dc_hist, run_hist, lvl_hist);
        tc_ctx_cap_free(cap);
    }
    *k1 = rice_k_estimate(sum_dc, cnt_dc);
    *k2 = rice_k_estimate(sum_level, cnt_level);
    *k3 = rice_k_estimate(sum_run, cnt_run);
    if (domain_violation != 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "token domain violated (bit_depth %u qp too low for "
                     "full-scale content): raise qp or bit_depth <= 12",
                     (unsigned)fh->bit_depth);
        return TC_ERR_INVALID_ARGUMENT;
    }
    return TC_OK;
}

/* 生成一个 alpha 带的 MED 残差 + k 估计统计（行 y>0,x=0 时 a/c 不可用 → pred=b） */
static void fill_alpha_residuals(const topos_frame_header* fh, const uint16_t* coded,
                                 int32_t* rbuf, uint32_t y0, uint32_t h,
                                 uint32_t* k1, uint32_t* k2)
{
    uint32_t w = fh->plane_coded_w[3];
    uint32_t rows = h * 8u;
    uint32_t py0 = y0 * 8u;
    size_t stride = (size_t)w;
    int32_t* r = rbuf;
    uint64_t sum_level = 0u, cnt_level = 0u, sum_run = 0u, cnt_run = 0u;
    uint64_t run = 0u;

    for (uint32_t y = 0u; y < rows; ++y) {
        const uint16_t* row = coded + (size_t)(py0 + y) * stride;
        const uint16_t* above = row - stride; /* y==0 时不可用（不读） */
        for (uint32_t x = 0u; x < w; ++x) {
            int32_t pred;
            if (y == 0u) {
                pred = x == 0u ? 0 : (int32_t)row[x - 1u];
            } else if (x == 0u) {
                pred = (int32_t)above[0];
            } else {
                pred = tc_med_predict((int32_t)row[x - 1u], (int32_t)above[x],
                                      (int32_t)above[x - 1u]);
            }
            int32_t res = (int32_t)row[x] - pred;
            r[(size_t)y * stride + x] = res;
            if (res != 0) {
                sum_run += run;
                cnt_run++;
                sum_level += tc_rice_map_signed(res);
                cnt_level++;
                run = 0u;
            } else {
                run++;
            }
        }
    }
    if (run > 0u) { /* 尾零终结对：run 符号 */
        sum_run += run;
        cnt_run++;
    }
    *k1 = rice_k_estimate(sum_level, cnt_level);
    *k2 = rice_k_estimate(sum_run, cnt_run);
}

/* ---- 阶段 9：band 任务（slice 级并行执行单元；单线程时同一代码路径顺序执行） ---- */

typedef struct enc_worker_slot {
    int32_t* qbuf;       /* cols × slice_rows × 64（颜色；token 路径下仅回退用） */
    int32_t* dc_a;       /* cols ×2（DC 链行缓冲） */
    int32_t* dc_b;
    int32_t* rbuf;       /* coded_w × slice_rows×8（alpha 残差） */
    tc_bitwriter bw;
    int bw_ok;
    enc_band_tok tok;    /* M6b：token 路径（tok_ok=0 时为全零，不可用） */
    int tok_ok;
} enc_worker_slot;

typedef struct enc_band_task {
    /* 输入（只读共享；coded 各带行不相交） */
    const topos_frame_header* fh;
    const uint16_t* coded;
    uint32_t plane;
    uint32_t y0, h;
    const tc_quant_ctx* qctx;
    int32_t qp_eff;
    enc_worker_slot* slots_base; /* 解码深化：动态领取分发——槽位改为 job 入口
                                  * 按执行线程绑定（原 i%nw 与静态条带互锁） */
    uint32_t nw;
    enc_worker_slot* slot; /* job 入口绑定（worker 槽位） */
    enc_shared* es;        /* M6c：payload arena（slot_idx 槽内 bump） */
    uint32_t slot_idx;
    const int32_t* fsrc;   /* M7：非 NULL 时量化源 = 该 plane 的 F-cache 基址 */
    /* 输出 */
    uint8_t hdr[TC_SLICE_HEADER_SIZE];
    const uint8_t* payload;
    uint32_t payload_size;
    const uint8_t* v7_payload[TC_V7_BAND_COUNT];
    uint32_t v7_payload_size[TC_V7_BAND_COUNT];
    uint32_t v7_payload_crc32[TC_V7_BAND_COUNT];
    int is_alpha;
    int32_t rc;
    /* M6 观测：阶段耗时（任务局部，join 后由主线程求和） */
    uint64_t fill_ns, entropy_ns, crc_ns, copy_ns;
    uint32_t blocks;
} enc_band_task;

/* RD2-03/RD2-04 bridge: turn the token arrays already produced by the
 * quantization pass into five independently encoded V7-A segments.  C1:
 * the per-band scratch trio (pair_count/local_runs/level_m) is carved
 * from the slot scratch pool (single contiguous block, 4B-aligned step)
 * and needs no per-slice free; the encoded payloads stay in the slot
 * arena until directory assembly. */
static int32_t enc_emit_v7_segments(enc_band_task* t, enc_worker_slot* s,
                                    uint32_t k_dc, uint32_t k_level)
{
    const uint32_t cols = t->fh->plane_block_cols[t->plane];
    size_t block_count_size = 0u;
    if (!tc_umul_size((size_t)t->h, (size_t)cols, &block_count_size) ||
        block_count_size == 0u || block_count_size > (size_t)UINT32_MAX) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a encoder block count");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    const uint32_t block_count = (uint32_t)block_count_size;
    const uint32_t token_count = s->tok.token_count;
    size_t pair_bytes = 0u;
    if (!tc_umul_size(block_count_size, sizeof(uint16_t), &pair_bytes)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a encoder pair storage");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    size_t lvl_bytes = 0u;
    if (!tc_umul_size((size_t)token_count, sizeof(uint32_t), &lvl_bytes)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a encoder level storage");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    /* 每带条带布局：[pair_count 2B][pad4][local_runs 1B][pad4][level_m 4B]
     * ——池基址 realloc 对齐（≥16B）+ 4B 步进 ⇒ 全部成员自然对齐。 */
    const size_t runs_off = (pair_bytes + 3u) & ~(size_t)3u;
    const size_t lvl_off = runs_off + (((size_t)token_count + 3u) & ~(size_t)3u);
    const size_t stride = lvl_off + lvl_bytes;
    size_t total = 0u;
    if (!tc_umul_size(stride, (size_t)TC_V7_BAND_COUNT, &total) || total == 0u) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a encoder scratch stride");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    uint8_t* pool = enc_scratch_acquire(t->es, t->slot_idx, total);
    if (pool == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "v7a scratch %zu bytes", total);
        return TC_ERR_OUT_OF_MEMORY;
    }

    tc_v7_band_storage storage[TC_V7_BAND_COUNT];
    memset(storage, 0, sizeof(storage));
    for (uint32_t band = 0u; band < TC_V7_BAND_COUNT; ++band) {
        uint8_t* base = pool + (size_t)band * stride;
        storage[band].pair_count = (uint16_t*)base;
        storage[band].dc_m = band == 0u ? s->tok.dc_m : NULL;
        storage[band].block_capacity = block_count;
        storage[band].token_capacity = token_count;
        storage[band].local_runs = base + runs_off;
        storage[band].level_m = (uint32_t*)(base + lvl_off);
    }

    int32_t rc = tc_v7_band_partition_tokens(block_count, s->tok.dc_m, s->tok.npair,
                                             s->tok.run, s->tok.lvl_m, token_count, storage);
    if (rc != TC_OK) { return rc; }

    for (uint32_t band = 0u; band < TC_V7_BAND_COUNT; ++band) {
        uint64_t pairs = s->tok.v7_stats.pair_count[band];
        uint32_t band_k_run = rice_k_estimate(s->tok.v7_stats.local_run_sum[band], pairs);
        uint32_t band_k_level = rice_k_estimate(s->tok.v7_stats.level_sum[band], pairs);
        if (band == 0u) {
            band_k_run = k_dc; /* B0 local run is a fixed two-bit field. */
            band_k_level = k_level;
        }
        tc_v7_band_input input;
        input.block_count = block_count;
        input.dc_m = band == 0u ? storage[band].dc_m : NULL;
        input.pair_count = storage[band].pair_count;
        input.local_runs = storage[band].local_runs;
        input.level_m = storage[band].level_m;
        input.token_count = storage[band].token_count;
        tc_bitwriter_reset(&s->bw);
        rc = tc_v7_band_encode(band, band_k_run, band_k_level, &input, &s->bw,
                               &t->v7_payload_size[band],
                               &t->v7_payload_crc32[band]);
        if (rc != TC_OK) { return rc; }
        uint8_t* payload = enc_pay_alloc(t->es, t->slot_idx,
                                         (size_t)t->v7_payload_size[band]);
        if (payload == NULL) {
            return TC_ERR_OUT_OF_MEMORY;
        }
        memcpy(payload, tc_bitwriter_data(&s->bw), t->v7_payload_size[band]);
        t->v7_payload[band] = payload;
    }
    return TC_OK;
}

/* ---- A-enc（速度计划 A 轴编码 GPU 化）批 1：token 捕获差分机 ----
 * dev/test only（V7-R2 颜色带）：fill 完成后拷出 dc_m/npair/run/lvl_m
 * 四件套 + 该 band 的量化上下文表（Q/half/dz——GPU kernel 的单一真相
 * 源，免测试侧复刻 qp 钳位链）。段布局全由几何确定性推导（plane 序 ×
 * 带行序；dc/np plane 稠密、run/lvl 最坏 63 对/块段，getter 压实），
 * worker 并行乱序写入无竞争。容量在帧入口（frame_encode_ready，主
 * 线程）按几何惰性扩容——worker 段指针整帧稳定。 */
typedef struct tc_dev_tok_band {
    uint32_t plane;
    uint32_t y0;
    uint32_t h;
    uint32_t cols;
    uint32_t token_count;   /* 该带实际 (run,lvl) 对数（压实用） */
} tc_dev_tok_band;

static int s_dev_tok_on = 0;
static uint32_t* s_dev_tok_dc = NULL;     /* plane 稠密 blocks 项 */
static uint16_t* s_dev_tok_np = NULL;
static uint8_t* s_dev_tok_run = NULL;     /* band 最坏段拼接 */
static uint32_t* s_dev_tok_lvl = NULL;
static tc_dev_tok_band* s_dev_tok_meta = NULL;
static uint32_t s_dev_tok_bands = 0u;     /* 本帧带数（getter 域） */
static uint32_t s_dev_tok_bands_cap = 0u;
static uint32_t s_dev_tok_base[3];        /* plane → band 序基 */
static uint32_t s_dev_tok_rows[3];        /* 几何缓存（段基推导用） */
static uint32_t s_dev_tok_cols[3];
static size_t s_dev_tok_blocks_cap = 0u;
static size_t s_dev_tok_run_cap = 0u;     /* 对数计（run 字节 = 同值） */
static uint32_t* s_dev_tok_q = NULL;      /* bands × 64 三表 */
static uint32_t* s_dev_tok_half = NULL;
static uint32_t* s_dev_tok_dz = NULL;
static uint32_t* s_dev_tok_fdm1 = NULL;  /* bands × 64 fastdiv 孪生 */
static uint32_t* s_dev_tok_fdm0 = NULL;

void tc_dev_set_tok_capture(int on)
{
    s_dev_tok_on = (on != 0) ? 1 : 0;
    if (s_dev_tok_on == 0) {
        tc_free(s_dev_tok_dc); tc_free(s_dev_tok_np);
        tc_free(s_dev_tok_run); tc_free(s_dev_tok_lvl);
        tc_free(s_dev_tok_meta); tc_free(s_dev_tok_q);
        tc_free(s_dev_tok_half); tc_free(s_dev_tok_dz);
        tc_free(s_dev_tok_fdm1); tc_free(s_dev_tok_fdm0);
        s_dev_tok_dc = NULL; s_dev_tok_np = NULL;
        s_dev_tok_run = NULL; s_dev_tok_lvl = NULL;
        s_dev_tok_meta = NULL; s_dev_tok_q = NULL;
        s_dev_tok_half = NULL; s_dev_tok_dz = NULL;
        s_dev_tok_fdm1 = NULL; s_dev_tok_fdm0 = NULL;
        s_dev_tok_bands = 0u; s_dev_tok_bands_cap = 0u;
        s_dev_tok_blocks_cap = 0u; s_dev_tok_run_cap = 0u;
    }
}

const uint8_t* tc_dev_zigzag(void) { return kTcZigzag; }

/* A3/V8 批 1：packet 级 rans2 slice introspection——GPU rANS 差分机的
 * 输入侧。单线程、调用方缓冲直写、零热路径副作用：tc_packet_scan 解析
 * → 每 slice 拷贝 payload + 以解码器同源路径（tc_rans2_row_decode）解析
 * 前缀出归一化 freq/cum 表。kernel 侧免表构建（位精确性由"同一 C 代码
 * 产表"钉死；前缀解析属 per-slice O(prefix) 设置成本，不进符号环）。
 *
 * meta 每 slice 9×u32：plane, block_y0, block_h, payload_off, payload_size,
 * flags, dc_ctx_n, lvl_ctx_n, crc_ok（rANS 流起点 = payload + 1 + dc_ctx_n*29 +
 * 64 + lvl_ctx_n*28，kernel 由 ctx_n 派生）。bytes_out = payload 连接。
 * tabs 每 slice 709×u16 固定段：dc_freq[5×29] dc_cum[5×30] run_freq[64]
 * run_cum[65] lvl_freq[5×28] lvl_cum[5×29]——未用条件行全零（解码同 C：
 * 永不查询）。仅 V7-R2（major=7 emode=7）帧，其余熵族 UNSUPPORTED。 */
#define TC_DEV_RANS2_TABS 709u
int32_t tc_dev_packet_slices(const uint8_t* packet, size_t packet_size,
                             uint32_t* meta_out, uint32_t meta_stride,
                             uint8_t* bytes_out, size_t bytes_cap,
                             uint16_t* tabs_out, uint32_t tabs_stride,
                             uint32_t* n_out, size_t* bytes_used_out)
{
    if (packet == NULL || meta_out == NULL || bytes_out == NULL
        || tabs_out == NULL || n_out == NULL || bytes_used_out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "packet slices args NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (meta_stride < 9u || tabs_stride < TC_DEV_RANS2_TABS) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "packet slices stride");
        return TC_ERR_INVALID_ARGUMENT;
    }
    topos_packet_view view;
    int32_t rc = tc_packet_scan(packet, packet_size, &view);
    if (rc != TC_OK) { return rc; }
    if (!(view.fh.version_major == 7u && view.fh.entropy_mode == 7u)) {
        tc_set_error(TC_ERR_UNSUPPORTED_VERSION, "packet slices: frame not rans2 (version)");
        return TC_ERR_UNSUPPORTED_VERSION;
    }
    size_t pos = 0u;
    for (uint32_t si = 0u; si < view.slice_count; ++si) {
        const topos_slice_header* sh = &view.slices[si];
        const uint8_t* payload = view.payloads[si];
        const size_t psz = sh->slice_payload_size;
        if (pos + psz > bytes_cap) {
            tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "packet slices bytes %zu+%zu",
                         pos, psz);
            return TC_ERR_BUFFER_TOO_SMALL;
        }
        if (psz < 1u + TC_RANS_TABLE_BYTES + TC_RANS_STATE_BYTES) {
            tc_set_error(TC_ERR_MALFORMED, "packet slices payload %zu", psz);
            return TC_ERR_MALFORMED;
        }
        const uint8_t flags = payload[0];
        const uint32_t lvl_model = flags & 0x3u;
        const uint32_t dc_model = (flags >> 2u) & 0x1u;
        if ((flags & 0xF8u) != 0u || lvl_model > TC_RANS2_LVL_PREV) {
            tc_set_error(TC_ERR_MALFORMED, "packet slices flags %u",
                         (unsigned)flags);
            return TC_ERR_MALFORMED;
        }
        const uint32_t lvl_ctx_n = lvl_model == TC_RANS2_LVL_POS ? TC_RANS2_POS_CTX
                             : (lvl_model == TC_RANS2_LVL_PREV ? TC_RANS2_PREV_CTX
                                                               : 1u);
        const uint32_t dc_ctx_n = dc_model == TC_RANS2_DC_PREV ? TC_RANS2_DC_CTX : 1u;
        const size_t prefix = 1u + (size_t)dc_ctx_n * TC_RANS_DC_SYMS
                            + TC_RANS_RUN_SYMS + (size_t)lvl_ctx_n * TC_RANS_LVL_SYMS;
        if (psz < prefix + TC_RANS_STATE_BYTES) {
            tc_set_error(TC_ERR_TRUNCATED, "packet slices prefix %zu+%zu",
                         prefix, psz);
            return TC_ERR_TRUNCATED;
        }
        uint16_t* tab = tabs_out + (size_t)si * tabs_stride;
        memset(tab, 0, (size_t)TC_DEV_RANS2_TABS * sizeof(uint16_t));
        uint16_t* dc_f = tab;
        uint16_t* dc_c = tab + 5u * TC_RANS_DC_SYMS;
        uint16_t* run_f = dc_c + 5u * (TC_RANS_DC_SYMS + 1u);
        uint16_t* run_c = run_f + TC_RANS_RUN_SYMS;
        uint16_t* lvl_f = run_c + (TC_RANS_RUN_SYMS + 1u);
        uint16_t* lvl_c = lvl_f + 5u * TC_RANS_LVL_SYMS;
        size_t off = 1u;
        for (uint32_t c = 0u; c < dc_ctx_n; ++c) {
            tc_rans_model m;
            if (tc_rans2_row_decode(payload + off, TC_RANS_DC_SYMS, &m) != TC_OK) {
                tc_set_error(TC_ERR_MALFORMED, "packet slices dc row %u",
                             (unsigned)c);
                return TC_ERR_MALFORMED;
            }
            memcpy(dc_f + (size_t)c * TC_RANS_DC_SYMS, m.freq,
                   TC_RANS_DC_SYMS * sizeof(uint16_t));
            memcpy(dc_c + (size_t)c * (TC_RANS_DC_SYMS + 1u), m.cum,
                   (TC_RANS_DC_SYMS + 1u) * sizeof(uint16_t));
            off += TC_RANS_DC_SYMS;
        }
        {
            tc_rans_model m;
            if (tc_rans2_row_decode(payload + off, TC_RANS_RUN_SYMS, &m) != TC_OK) {
                tc_set_error(TC_ERR_MALFORMED, "packet slices run row");
                return TC_ERR_MALFORMED;
            }
            memcpy(run_f, m.freq, TC_RANS_RUN_SYMS * sizeof(uint16_t));
            memcpy(run_c, m.cum, (TC_RANS_RUN_SYMS + 1u) * sizeof(uint16_t));
            off += TC_RANS_RUN_SYMS;
        }
        for (uint32_t c = 0u; c < lvl_ctx_n; ++c) {
            tc_rans_model m;
            if (tc_rans2_row_decode(payload + off, TC_RANS_LVL_SYMS, &m) != TC_OK) {
                tc_set_error(TC_ERR_MALFORMED, "packet slices lvl row %u",
                             (unsigned)c);
                return TC_ERR_MALFORMED;
            }
            memcpy(lvl_f + (size_t)c * TC_RANS_LVL_SYMS, m.freq,
                   TC_RANS_LVL_SYMS * sizeof(uint16_t));
            memcpy(lvl_c + (size_t)c * (TC_RANS_LVL_SYMS + 1u), m.cum,
                   (TC_RANS_LVL_SYMS + 1u) * sizeof(uint16_t));
            off += TC_RANS_LVL_SYMS;
        }
        uint32_t* me = meta_out + (size_t)si * meta_stride;
        me[0] = sh->plane;
        me[1] = sh->block_y0;
        me[2] = sh->block_h;
        me[3] = (uint32_t)pos;
        me[4] = (uint32_t)psz;
        me[5] = flags;
        me[6] = dc_ctx_n;
        me[7] = lvl_ctx_n;
        me[8] = view.slice_crc_ok[si]; /* CRC 门控（GPU conceal 判定归调用方） */
        memcpy(bytes_out + pos, payload, psz);
        pos += psz;
    }
    *n_out = view.slice_count;
    *bytes_used_out = pos;
    return TC_OK;
}


uint32_t tc_dev_tok_bands(void) { return s_dev_tok_bands; }

int64_t tc_dev_tok_meta_copy(void* dst, size_t cap)
{
    if (s_dev_tok_meta == NULL || dst == NULL || cap < s_dev_tok_bands) { return 0; }
    memcpy(dst, s_dev_tok_meta, (size_t)s_dev_tok_bands * sizeof(tc_dev_tok_band));
    return (int64_t)s_dev_tok_bands;
}

int64_t tc_dev_tok_dc_copy(void* dst, size_t cap)
{
    size_t total = 0u;
    for (uint32_t p = 0u; p < 3u; ++p) {
        total += (size_t)s_dev_tok_rows[p] * (size_t)s_dev_tok_cols[p];
    }
    if (s_dev_tok_dc == NULL || dst == NULL || cap < total) { return 0; }
    memcpy(dst, s_dev_tok_dc, total * sizeof(uint32_t));
    return (int64_t)total;
}

int64_t tc_dev_tok_np_copy(void* dst, size_t cap)
{
    size_t total = 0u;
    for (uint32_t p = 0u; p < 3u; ++p) {
        total += (size_t)s_dev_tok_rows[p] * (size_t)s_dev_tok_cols[p];
    }
    if (s_dev_tok_np == NULL || dst == NULL || cap < total) { return 0; }
    memcpy(dst, s_dev_tok_np, total * sizeof(uint16_t));
    return (int64_t)total;
}

/* run/lvl 压实拷贝：跳过最坏段洞（段基/长度全几何推导） */
int64_t tc_dev_tok_run_copy(void* dst, size_t cap)
{
    if (s_dev_tok_run == NULL || dst == NULL) { return 0; }
    size_t seg = 0u, dstp = 0u;
    for (uint32_t b = 0u; b < s_dev_tok_bands; ++b) {
        const tc_dev_tok_band* m = &s_dev_tok_meta[b];
        const size_t blocks = (size_t)m->h * (size_t)m->cols;
        if (m->token_count != 0u && dstp + m->token_count <= cap) {
            memcpy((uint8_t*)dst + dstp, s_dev_tok_run + seg, m->token_count);
            dstp += m->token_count;
        }
        seg += 63u * blocks;
    }
    return (int64_t)dstp;
}

int64_t tc_dev_tok_lvl_copy(void* dst, size_t cap)
{
    if (s_dev_tok_lvl == NULL || dst == NULL) { return 0; }
    size_t seg = 0u, dstp = 0u;
    for (uint32_t b = 0u; b < s_dev_tok_bands; ++b) {
        const tc_dev_tok_band* m = &s_dev_tok_meta[b];
        const size_t blocks = (size_t)m->h * (size_t)m->cols;
        if (m->token_count != 0u && dstp + m->token_count <= cap) {
            memcpy((uint32_t*)dst + dstp, s_dev_tok_lvl + seg,
                   (size_t)m->token_count * sizeof(uint32_t));
            dstp += m->token_count;
        }
        seg += 63u * blocks;
    }
    return (int64_t)dstp;
}

static int64_t dev_tok_table_copy(uint32_t* src, void* dst, size_t cap)
{
    const size_t n = (size_t)s_dev_tok_bands * 64u;
    if (src == NULL || dst == NULL || cap < n) { return 0; }
    memcpy(dst, src, n * sizeof(uint32_t));
    return (int64_t)n;
}

int64_t tc_dev_tok_q_copy(void* dst, size_t cap)
{ return dev_tok_table_copy(s_dev_tok_q, dst, cap); }

int64_t tc_dev_tok_half_copy(void* dst, size_t cap)
{ return dev_tok_table_copy(s_dev_tok_half, dst, cap); }

int64_t tc_dev_tok_dz_copy(void* dst, size_t cap)
{ return dev_tok_table_copy(s_dev_tok_dz, dst, cap); }

int64_t tc_dev_tok_fdm1_copy(void* dst, size_t cap)
{ return dev_tok_table_copy(s_dev_tok_fdm1, dst, cap); }

int64_t tc_dev_tok_fdm0_copy(void* dst, size_t cap)
{ return dev_tok_table_copy(s_dev_tok_fdm0, dst, cap); }

/* 帧入口（frame_encode_ready，主线程）：按 fh 几何惰性扩容并缓存段基
 * 推导量。slice_rows 分带与 enc_encode_plane_opt 同式；仅 V7-R2 颜色。 */
static void dev_tok_prepare(const topos_frame_header* fh, uint32_t slice_rows)
{
    s_dev_tok_bands = 0u;
    if (fh->version_major != 7u || fh->entropy_mode != 7u) { return; }
    uint32_t bands = 0u, base = 0u;
    size_t blocks_total = 0u;
    for (uint32_t p = 0u; p < 3u; ++p) {
        s_dev_tok_base[p] = base;
        s_dev_tok_rows[p] = fh->plane_block_rows[p];
        s_dev_tok_cols[p] = fh->plane_block_cols[p];
        uint32_t nb = (fh->plane_block_rows[p] + slice_rows - 1u) / slice_rows;
        if (nb == 0u) { nb = 1u; }
        base += nb;
        bands += nb;
        blocks_total += (size_t)fh->plane_block_rows[p]
                      * (size_t)fh->plane_block_cols[p];
    }
    const size_t run_total = 63u * blocks_total;
    if (run_total > (128u << 20)) { return; }   /* 差分机上界 128M 对 */
    if (blocks_total > s_dev_tok_blocks_cap) {
        uint32_t* g = (uint32_t*)tc_realloc(s_dev_tok_dc, blocks_total * 4u);
        if (g == NULL) { return; }
        s_dev_tok_dc = g;
        uint16_t* gn = (uint16_t*)tc_realloc(s_dev_tok_np, blocks_total * 2u);
        if (gn == NULL) { return; }
        s_dev_tok_np = gn;
        s_dev_tok_blocks_cap = blocks_total;
    }
    if (run_total > s_dev_tok_run_cap) {
        uint8_t* gr = (uint8_t*)tc_realloc(s_dev_tok_run, run_total);
        if (gr == NULL) { return; }
        s_dev_tok_run = gr;
        uint32_t* gl = (uint32_t*)tc_realloc(s_dev_tok_lvl, run_total * 4u);
        if (gl == NULL) { return; }
        s_dev_tok_lvl = gl;
        s_dev_tok_run_cap = run_total;
    }
    if (bands > s_dev_tok_bands_cap) {
        tc_dev_tok_band* gm = (tc_dev_tok_band*)tc_realloc(
            s_dev_tok_meta, (size_t)bands * sizeof(tc_dev_tok_band));
        if (gm == NULL) { return; }
        s_dev_tok_meta = gm;
        uint32_t* gq = (uint32_t*)tc_realloc(s_dev_tok_q, (size_t)bands * 64u * 4u);
        if (gq == NULL) { return; }
        s_dev_tok_q = gq;
        uint32_t* gh = (uint32_t*)tc_realloc(s_dev_tok_half, (size_t)bands * 64u * 4u);
        if (gh == NULL) { return; }
        s_dev_tok_half = gh;
        uint32_t* gd = (uint32_t*)tc_realloc(s_dev_tok_dz, (size_t)bands * 64u * 4u);
        if (gd == NULL) { return; }
        s_dev_tok_dz = gd;
        uint32_t* gm1 = (uint32_t*)tc_realloc(s_dev_tok_fdm1, (size_t)bands * 64u * 4u);
        if (gm1 == NULL) { return; }
        s_dev_tok_fdm1 = gm1;
        uint32_t* gm0 = (uint32_t*)tc_realloc(s_dev_tok_fdm0, (size_t)bands * 64u * 4u);
        if (gm0 == NULL) { return; }
        s_dev_tok_fdm0 = gm0;
        s_dev_tok_bands_cap = bands;
    }
    s_dev_tok_bands = bands;
}

/* worker（enc_band_job）：fill 后拷出。段基全几何推导（免 meta 先序
 * 读——worker 并行乱序）；run/lvl 最坏段（压实 getter 跳洞）。 */
static void dev_tok_capture_band(uint32_t plane, uint32_t y0, uint32_t h,
                                 uint32_t cols, uint32_t slice_rows,
                                 const enc_band_tok* tok,
                                 const tc_quant_ctx* qctx)
{
    const uint32_t bi = s_dev_tok_base[plane] + y0 / slice_rows;
    if (bi >= s_dev_tok_bands) { return; }
    tc_dev_tok_band* m = &s_dev_tok_meta[bi];
    const size_t blocks = (size_t)h * (size_t)cols;
    /* plane 稠密 dc/np 段基（带内偏移 = y0×cols） */
    size_t dc_base = 0u;
    for (uint32_t pp = 0u; pp < plane; ++pp) {
        dc_base += (size_t)s_dev_tok_rows[pp] * (size_t)s_dev_tok_cols[pp];
    }
    /* run/lvl 最坏段基：Σ_{p<plane} 63×rows×cols + 63×y0×cols */
    size_t run_base = 0u;
    for (uint32_t pp = 0u; pp < plane; ++pp) {
        run_base += 63u * (size_t)s_dev_tok_rows[pp] * (size_t)s_dev_tok_cols[pp];
    }
    run_base += 63u * (size_t)y0 * (size_t)cols;
    memcpy(s_dev_tok_dc + dc_base + (size_t)y0 * cols, tok->dc_m, blocks * 4u);
    memcpy(s_dev_tok_np + dc_base + (size_t)y0 * cols, tok->npair, blocks * 2u);
    if (tok->token_count != 0u) {
        memcpy(s_dev_tok_run + run_base, tok->run, tok->token_count);
        memcpy(s_dev_tok_lvl + run_base, tok->lvl_m,
               (size_t)tok->token_count * 4u);
    }
    m->plane = plane;
    m->y0 = y0;
    m->h = h;
    m->cols = cols;
    m->token_count = tok->token_count;
    memcpy(s_dev_tok_q + (size_t)bi * 64u, qctx->Q, 64u * 4u);
    memcpy(s_dev_tok_half + (size_t)bi * 64u, qctx->half, 64u * 4u);
    memcpy(s_dev_tok_dz + (size_t)bi * 64u, qctx->dz, 64u * 4u);
    memcpy(s_dev_tok_fdm1 + (size_t)bi * 64u, qctx->fd_nat_m1, 64u * 4u);
    memcpy(s_dev_tok_fdm0 + (size_t)bi * 64u, qctx->fd_nat_m0, 64u * 4u);
}

/* ---- V8 批 0 实验：瓦片聚合表统计损失模拟 ----
 *
 * A 锚：重导 hist/joints 后以生产同构路径重放每带编码，payload 必须
 * 与生产逐带相等（钉死重导与镜像选择的正确性）；B：瓦片聚合计数
 * → 瓦片级模型选择 + 同源模型构建（row_encode→row_decode），每带以
 * 瓦片模型发射（表每瓦片一份）。不写位流；段簿记为解析项。 */

typedef struct tile_acc {
    uint32_t dc[TC_RANS_DC_SYMS];
    uint32_t run[TC_RANS_RUN_SYMS];
    uint32_t lvl[TC_RANS_LVL_SYMS];
    rans2_joints j;
} tile_acc;

typedef struct tile_models {
    uint32_t lvl_model;
    uint32_t dc_model;
    tc_rans_model dc_m[TC_RANS2_DC_CTX];
    tc_rans_model run_m;
    tc_rans_model lvl_m[TC_RANS2_PREV_CTX];
} tile_models;

/* 单带 token 视图重建（零拷贝指向捕获 statics；dc/np 平面稠密段，
 * run/lvl 63×blocks 最坏段基——与 dev_tok_capture_band 布局同源）。 */
static void tile_sim_band_view(uint32_t bi, enc_band_tok* tok,
                               uint32_t* plane_out, size_t* blocks_out)
{
    const tc_dev_tok_band* m = &s_dev_tok_meta[bi];
    const uint32_t plane = m->plane;
    const size_t cols = m->cols;
    size_t dc_base = 0u;
    size_t run_base = 0u;
    for (uint32_t pp = 0u; pp < plane; ++pp) {
        dc_base += (size_t)s_dev_tok_rows[pp] * (size_t)s_dev_tok_cols[pp];
        run_base += 63u * (size_t)s_dev_tok_rows[pp] * (size_t)s_dev_tok_cols[pp];
    }
    dc_base += (size_t)m->y0 * cols;
    run_base += 63u * (size_t)m->y0 * cols;
    tok->dc_m = s_dev_tok_dc + dc_base;
    tok->npair = s_dev_tok_np + dc_base;
    tok->run = s_dev_tok_run + run_base;
    tok->lvl_m = s_dev_tok_lvl + run_base;
    tok->token_count = m->token_count;
    *plane_out = plane;
    *blocks_out = (size_t)m->h * cols;
}

/* 从 token 重导三族 hist（cat = bitlen(map)；EOB 每块一次——与后向环
 * 发射序同源；正确性由 A 锚差分兜底）。 */
static void tile_sim_derive_hists(const enc_band_tok* tok, size_t blocks,
                                  uint32_t* dc_hist, uint32_t* run_hist,
                                  uint32_t* lvl_hist)
{
    memset(dc_hist, 0, TC_RANS_DC_SYMS * sizeof(uint32_t));
    memset(run_hist, 0, TC_RANS_RUN_SYMS * sizeof(uint32_t));
    memset(lvl_hist, 0, TC_RANS_LVL_SYMS * sizeof(uint32_t));
    size_t pi = 0u;
    for (size_t b = 0u; b < blocks; ++b) {
        dc_hist[tc_vlc_bitlen32(tok->dc_m[b])]++;
        run_hist[63u]++;
        for (uint32_t p = 0u; p < tok->npair[b]; ++p) {
            run_hist[tok->run[pi]]++;
            lvl_hist[tc_vlc_bitlen32(tok->lvl_m[pi])]++;
            pi++;
        }
    }
}

/* 计数 → 模型（row_encode→row_decode 同源，两侧从同一字节重建）。 */
static int32_t tile_sim_build_row(const uint32_t* count, uint32_t nsym,
                                  tc_rans_model* m)
{
    uint8_t rb[TC_RANS_MAX_SYMS];
    tc_rans2_row_encode(rb, nsym, count);
    return tc_rans2_row_decode(rb, nsym, m);
}

int32_t tc_dev_rans2_tile_sim(uint32_t tile_rows, uint32_t seg_blocks,
                              tc_dev_tile_sim_out* out)
{
    if (out == NULL || seg_blocks == 0u || seg_blocks > 256u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "tile sim args");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (s_dev_tok_on == 0 || s_dev_tok_bands == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "tile sim: no tok capture session");
        return TC_ERR_INVALID_ARGUMENT;
    }
    out->n_bands = s_dev_tok_bands;
    out->tile_rows = tile_rows;
    out->seg_blocks = seg_blocks;

    int32_t rc = TC_OK;
    enc_shared es;
    memset(&es, 0, sizeof(es));
    enc_band_tok tok;
    uint32_t dc_hist[TC_RANS_DC_SYMS];
    uint32_t run_hist[TC_RANS_RUN_SYMS];
    uint32_t lvl_hist[TC_RANS_LVL_SYMS];
    rans2_joints joints;
    tc_rans_model dc_m[TC_RANS2_DC_CTX];
    tc_rans_model lvl_m[TC_RANS2_PREV_CTX];
    tc_rans_model run_m;
    tile_acc* accs = NULL;
    tile_models* tms = NULL;

    /* 瓦片索引：plane 内 tile_rows 块行一片（0 = 整平面一片）；带序
     * （plane 主序、y0 升序）下瓦片连续。 */
    uint32_t plane_tile_base[4];
    uint32_t n_tiles = 0u;
    for (uint32_t p = 0u; p < 3u; ++p) {
        plane_tile_base[p] = n_tiles;
        const uint32_t rows = s_dev_tok_rows[p];
        if (tile_rows == 0u || rows == 0u) {
            n_tiles += 1u;
        } else {
            n_tiles += (rows + tile_rows - 1u) / tile_rows;
        }
    }
    plane_tile_base[3] = n_tiles;
    accs = (tile_acc*)tc_realloc(NULL, (size_t)n_tiles * sizeof(tile_acc));
    tms = (tile_models*)tc_realloc(NULL, (size_t)n_tiles * sizeof(tile_models));
    if (accs == NULL || tms == NULL) {
        rc = TC_ERR_OUT_OF_MEMORY;
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "tile sim accs");
        goto done;
    }
    memset(accs, 0, (size_t)n_tiles * sizeof(tile_acc));
    out->n_tiles = n_tiles;

    for (uint32_t bi = 0u; bi < s_dev_tok_bands; ++bi) {
        uint32_t plane = 0u;
        size_t blocks = 0u;
        tile_sim_band_view(bi, &tok, &plane, &blocks);
        tile_sim_derive_hists(&tok, blocks, dc_hist, run_hist, lvl_hist);
        rans2_collect_joints(&tok, blocks, &joints);

        /* A 锚：生产 payload（逐带精确表）+ 镜像重放差分 */
        size_t payload = 0u;
        rc = color_tokens_rans2_encode(&tok, blocks, dc_hist, run_hist,
                                       lvl_hist, NULL, &payload, &es, 0u);
        if (rc != TC_OK) { goto done; }
        uint32_t lvl_model = 0u;
        uint32_t dc_model = 0u;
        rans2_sim_pick_models(dc_hist, lvl_hist, &joints,
                              &lvl_model, &dc_model);
        const uint32_t lvl_ctx_n = lvl_model == TC_RANS2_LVL_POS
            ? TC_RANS2_POS_CTX
            : (lvl_model == TC_RANS2_LVL_PREV ? TC_RANS2_PREV_CTX : 1u);
        const uint32_t dc_ctx_n = dc_model == TC_RANS2_DC_PREV
            ? TC_RANS2_DC_CTX : 1u;
        const uint32_t* dc_rows = dc_model == TC_RANS2_DC_PREV
            ? joints.dc_prev : dc_hist;
        for (uint32_t c = 0u; c < dc_ctx_n; ++c) {
            rc = tile_sim_build_row(dc_rows + (size_t)c * TC_RANS_DC_SYMS,
                                    TC_RANS_DC_SYMS, &dc_m[c]);
            if (rc != TC_OK) { goto done; }
        }
        rc = tile_sim_build_row(run_hist, TC_RANS_RUN_SYMS, &run_m);
        if (rc != TC_OK) { goto done; }
        const uint32_t* lvl_rows = lvl_model == TC_RANS2_LVL_POS
            ? joints.lvl_pos
            : (lvl_model == TC_RANS2_LVL_PREV ? joints.lvl_prev : lvl_hist);
        for (uint32_t c = 0u; c < lvl_ctx_n; ++c) {
            rc = tile_sim_build_row(lvl_rows + (size_t)c * TC_RANS_LVL_SYMS,
                                    TC_RANS_LVL_SYMS, &lvl_m[c]);
            if (rc != TC_OK) { goto done; }
        }
        size_t stream = 0u;
        rc = rans2_encode_stream_with_models(&tok, blocks, lvl_model, dc_model,
                                             dc_m, &run_m, lvl_m, &es, 0u,
                                             &stream);
        if (rc != TC_OK) { goto done; }
        const size_t prefix = 1u + (size_t)dc_ctx_n * TC_RANS_DC_SYMS
                            + TC_RANS_RUN_SYMS
                            + (size_t)lvl_ctx_n * TC_RANS_LVL_SYMS;
        if (prefix + stream != payload) {
            tc_set_error(TC_ERR_MALFORMED,
                         "tile sim A anchor mismatch band %u: %zu+%zu vs %zu",
                         (unsigned)bi, prefix, stream, payload);
            rc = TC_ERR_MALFORMED;
            goto done;
        }
        out->a_total += payload;
        out->a_prefix += prefix;
        out->a_stream += stream;

        /* B 累积：并入所属瓦片 */
        const uint32_t tid = plane_tile_base[plane]
                           + (tile_rows == 0u ? 0u
                                              : s_dev_tok_meta[bi].y0 / tile_rows);
        tile_acc* acc = &accs[tid];
        for (uint32_t s = 0u; s < TC_RANS_DC_SYMS; ++s) {
            acc->dc[s] += dc_hist[s];
        }
        for (uint32_t s = 0u; s < TC_RANS_RUN_SYMS; ++s) {
            acc->run[s] += run_hist[s];
        }
        for (uint32_t s = 0u; s < TC_RANS_LVL_SYMS; ++s) {
            acc->lvl[s] += lvl_hist[s];
        }
        for (uint32_t s = 0u; s < TC_RANS2_POS_CTX * TC_RANS_LVL_SYMS; ++s) {
            acc->j.lvl_pos[s] += joints.lvl_pos[s];
        }
        for (uint32_t s = 0u; s < TC_RANS2_PREV_CTX * TC_RANS_LVL_SYMS; ++s) {
            acc->j.lvl_prev[s] += joints.lvl_prev[s];
        }
        for (uint32_t s = 0u; s < TC_RANS2_DC_CTX * TC_RANS_DC_SYMS; ++s) {
            acc->j.dc_prev[s] += joints.dc_prev[s];
        }
    }

    /* B：瓦片级模型选择 + 同源构建 */
    for (uint32_t tid = 0u; tid < n_tiles; ++tid) {
        tile_acc* acc = &accs[tid];
        tile_models* tm = &tms[tid];
        rans2_sim_pick_models(acc->dc, acc->lvl, &acc->j,
                              &tm->lvl_model, &tm->dc_model);
        out->flags_lvl[tm->lvl_model]++;
        out->flags_dc[tm->dc_model]++;
        const uint32_t lvl_ctx_n = tm->lvl_model == TC_RANS2_LVL_POS
            ? TC_RANS2_POS_CTX
            : (tm->lvl_model == TC_RANS2_LVL_PREV ? TC_RANS2_PREV_CTX : 1u);
        const uint32_t dc_ctx_n = tm->dc_model == TC_RANS2_DC_PREV
            ? TC_RANS2_DC_CTX : 1u;
        const uint32_t* dc_rows = tm->dc_model == TC_RANS2_DC_PREV
            ? acc->j.dc_prev : acc->dc;
        for (uint32_t c = 0u; c < dc_ctx_n; ++c) {
            rc = tile_sim_build_row(dc_rows + (size_t)c * TC_RANS_DC_SYMS,
                                    TC_RANS_DC_SYMS, &tm->dc_m[c]);
            if (rc != TC_OK) { goto done; }
        }
        rc = tile_sim_build_row(acc->run, TC_RANS_RUN_SYMS, &tm->run_m);
        if (rc != TC_OK) { goto done; }
        const uint32_t* lvl_rows = tm->lvl_model == TC_RANS2_LVL_POS
            ? acc->j.lvl_pos
            : (tm->lvl_model == TC_RANS2_LVL_PREV ? acc->j.lvl_prev : acc->lvl);
        for (uint32_t c = 0u; c < lvl_ctx_n; ++c) {
            rc = tile_sim_build_row(lvl_rows + (size_t)c * TC_RANS_LVL_SYMS,
                                    TC_RANS_LVL_SYMS, &tm->lvl_m[c]);
            if (rc != TC_OK) { goto done; }
        }
        out->b_table += 1u + (uint64_t)dc_ctx_n * TC_RANS_DC_SYMS
                      + TC_RANS_RUN_SYMS
                      + (uint64_t)lvl_ctx_n * TC_RANS_LVL_SYMS;
    }
    /* B：每带以瓦片模型发射 */
    for (uint32_t bi = 0u; bi < s_dev_tok_bands; ++bi) {
        uint32_t plane = 0u;
        size_t blocks = 0u;
        tile_sim_band_view(bi, &tok, &plane, &blocks);
        const uint32_t tid = plane_tile_base[plane]
                           + (tile_rows == 0u ? 0u
                                              : s_dev_tok_meta[bi].y0 / tile_rows);
        tile_models* tm = &tms[tid];
        size_t stream = 0u;
        rc = rans2_encode_stream_with_models(&tok, blocks, tm->lvl_model,
                                             tm->dc_model, tm->dc_m,
                                             &tm->run_m, tm->lvl_m, &es, 0u,
                                             &stream);
        if (rc != TC_OK) { goto done; }
        out->b_stream += stream;
    }
    out->b_total = out->b_table + out->b_stream;

    /* 段簿记（解析项）：S = Σ ceil(blocks_p/seg_blocks) */
    for (uint32_t p = 0u; p < 3u; ++p) {
        const uint64_t blocks_p = (uint64_t)s_dev_tok_rows[p]
                                * (uint64_t)s_dev_tok_cols[p];
        out->seg_count += (blocks_p + seg_blocks - 1u) / seg_blocks;
    }
    out->seg_dir_bytes = 12u * out->seg_count;
    out->seg_state_extra = out->seg_count > (uint64_t)s_dev_tok_bands
        ? 4u * (out->seg_count - (uint64_t)s_dev_tok_bands)
        : 0u;

done:
    tc_free(accs);
    tc_free(tms);
    tc_free(es.slot_scratch[0]);
    return rc;
}

void tc_dev_v8_enc_qhash_set(uint64_t h);
uint64_t tc_dev_v8_enc_qhash(void);
static int s_v8_qhash_on = 0; /* q-hash oracle 开关：默认 OFF（生产零开销）；
                               * 差分测试经 tc_dev_v8_set_qhash(1) 开启 */
void tc_dev_v8_set_qhash(int on) { s_v8_qhash_on = on != 0; }

/* ==================== V8 编码器（批 2：段化多状态 rANS + 瓦片共享表）====================
 * 串行正确性优先（并行化/码控集成随批 4/5）。语义锚：
 *  - 段 = sb 连续栅格块（平面主序）；DC 预测与 dc_prev 上下文因果闭合于段
 *    （与批 3 解码器、批 4 GPU kernel 的段独立性契约一致；批 0 模拟器的
 *    collect/pick/emit 三原语在段粒度下直接复用）；
 *  - 瓦片模型 = 成员段聚合计数 argmin（与生产同构的镜像选择）+ 定长 350B 表；
 *  - 布局（批 1 冻结）：per plane [dir S×12B][tables T×350B][streams][crc T×4B]。 */

/* 单段 token 集（内联数组；调用方经 enc_band_tok 指针视图借用——
 * 复用 rans2_collect_joints / rans2_encode_stream_with_models）。 */
typedef struct v8_seg_tok {
    uint32_t dc_m[TC_V8_MAX_SEG_BLOCKS];
    uint16_t npair[TC_V8_MAX_SEG_BLOCKS];
    uint8_t run[TC_V8_MAX_SEG_BLOCKS * 63u];
    uint32_t lvl_m[TC_V8_MAX_SEG_BLOCKS * 63u];
    uint32_t token_count;
} v8_seg_tok;

/* enc_band_tok 指针视图（dc_m/npair/run/lvl_m 指向段内联数组；token_count
 * 同步；v7_stats 不初始化——V8 路径不读）。 */
static inline void v8_seg_view(const v8_seg_tok* tok, uint32_t blocks,
                               enc_band_tok* view)
{
    view->dc_m = (uint32_t*)tok->dc_m;
    view->npair = (uint16_t*)tok->npair;
    view->run = (uint8_t*)tok->run;
    view->lvl_m = (uint32_t*)tok->lvl_m;
    view->token_count = tok->token_count;
    (void)blocks;
}

/* 瓦片行 DCT + 自然序量化 → qtile [row_local][bx][64]（相对 r0）。 */
static int32_t v8_quant_tile_rows(enc_shared* e, uint32_t plane,
                                  const tc_quant_ctx* qctx, uint32_t r0,
                                  uint32_t nrows, int32_t* qtile)
{
    const topos_frame_header* fh = &e->fh;
    const uint32_t cols = fh->plane_block_cols[plane];
    const tc_quant_nat_fn quant_nat = tc_simd_resolve_quant_nat((uint8_t)(fh->bit_depth > 12u));
    const tc_forward_rows_fn fwd_rows = tc_simd_resolve_forward_rows((uint8_t)(fh->bit_depth > 12u));
    const size_t plane_stride = (size_t)cols * 8u;
    const int32_t mid = (int32_t)bd_mid(fh->bit_depth);
    const uint16_t* coded = e->plane_direct[plane] != 0
        ? e->plane_src[plane]
        : e->coded + e->coded_off[plane];
    for (uint32_t r = 0u; r < nrows; ++r) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            const uint16_t* src = coded
                + (size_t)(r0 + r) * 8u * plane_stride + (size_t)bx * 8u;
            int32_t* dst = qtile + ((size_t)r * cols + bx) * 64u;
            int32_t qn[64];
            uint64_t nat = 0u;
            fwd_rows(src, plane_stride, mid, dst);
            quant_nat(qctx, dst, qn, &nat);
            memcpy(dst, qn, sizeof(qn));
        }
    }
    return TC_OK;
}

/* 段 token 化（从瓦片量化条纹）。DC 预测段局部因果：left = 段内前块、
 * top = 段内上块行（i ≥ cols；(run,lvl) 推导与 fill_color_band_from_f
 * 逐位同构（zigzag 跳扫 + rice map；EOB 每块一次）。 */
static int32_t v8_tokenize_segment(const int32_t* qtile, uint32_t qtile_r0,
                                uint32_t cols, uint32_t seg0, uint32_t nblocks,
                                v8_seg_tok* tok, uint32_t* dc_hist,
                                uint32_t* run_hist, uint32_t* lvl_hist)
{
    uint32_t domain_violation = 0u;  /* 同 fill_color_band_tokens（P4②） */
    size_t pi = 0u;
    int32_t seg_dc[TC_V8_MAX_SEG_BLOCKS];
    for (uint32_t i = 0u; i < nblocks; ++i) {
        const uint32_t abs_blk = seg0 + i;
        const int32_t* q = qtile
            + (size_t)(abs_blk / cols - qtile_r0) * (size_t)cols * 64u
            + (size_t)(abs_blk % cols) * 64u;
        uint64_t nat = 0u;
        for (uint32_t s = 0u; s < 64u; ++s) {
            if (q[s] != 0) { nat |= 1ull << s; }
        }
        uint64_t nz = tc_zz_mask_from_nat(nat);
        seg_dc[i] = q[0];
        const int has_left = i != 0u ? 1 : 0;
        const int has_top = i >= cols ? 1 : 0;
        /* 实参无条件求值：下溢索引须钳位（has_top=0 时值不被读取） */
        const int32_t pred = tc_dc_predict(has_left,
                                           i != 0u ? seg_dc[i - 1u] : 0,
                                           has_top,
                                           seg_dc[i >= cols ? i - cols : 0u]);
        uint32_t dcm = tc_rice_map_signed(q[0] - pred);
        if (dcm > TC_RICE_M_MAX_DC_BD16) {
            dcm = TC_RICE_M_MAX_DC_BD16;
            domain_violation = 1u;
        }
        tok->dc_m[i] = dcm;
        dc_hist[tc_vlc_bitlen32(dcm)]++;
        nz >>= 1u;
        uint32_t np = 0u;
        uint32_t prev = 0u;
        while (nz != 0u) {
            const uint32_t pos = tc_ctz64(nz) + 1u;
            uint32_t lm = tc_rice_map_signed(q[kTcZigzag[pos]]);
            if (lm > TC_RICE_M_MAX_AC_LEVEL_BD16) {
                lm = TC_RICE_M_MAX_AC_LEVEL_BD16;
                domain_violation = 1u;
            }
            const uint32_t runv = pos - prev - 1u;
            tok->run[pi] = (uint8_t)runv;
            tok->lvl_m[pi] = lm;
            pi++;
            run_hist[runv]++;
            lvl_hist[tc_vlc_bitlen32(lm)]++;
            prev = pos;
            nz &= nz - 1u;
            np++;
        }
        tok->npair[i] = (uint16_t)np;
        run_hist[63u]++;
    }
    tok->token_count = (uint32_t)pi;
    if (domain_violation != 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "token domain violated (bit_depth 16 qp too low for "
                     "full-scale content): raise qp");
        return TC_ERR_INVALID_ARGUMENT;
    }
    return TC_OK;
}

/* 瓦片表写出（定长 350B；未用条件行 = 全零计数占位 {1,0,...}）。 */
static void v8_write_table(uint8_t* t350, uint32_t lvl_model, uint32_t dc_model,
                           const uint32_t* dc_rows, const uint32_t* run_counts,
                           const uint32_t* lvl_rows)
{
    static const uint32_t zeros[TC_RANS_MAX_SYMS];
    const uint32_t dc_ctx_n = dc_model == TC_RANS2_DC_PREV ? TC_RANS2_DC_CTX : 1u;
    const uint32_t lvl_ctx_n = lvl_model == TC_RANS2_LVL_POS ? TC_RANS2_POS_CTX
                             : (lvl_model == TC_RANS2_LVL_PREV ? TC_RANS2_PREV_CTX : 1u);
    t350[0] = (uint8_t)(lvl_model | (dc_model << 2));
    uint8_t* w = t350 + 1u;
    for (uint32_t c = 0u; c < TC_RANS2_DC_CTX; ++c) {
        tc_rans2_row_encode(w, TC_RANS_DC_SYMS,
                            c < dc_ctx_n ? dc_rows + (size_t)c * TC_RANS_DC_SYMS : zeros);
        w += TC_RANS_DC_SYMS;
    }
    tc_rans2_row_encode(w, TC_RANS_RUN_SYMS, run_counts);
    w += TC_RANS_RUN_SYMS;
    for (uint32_t c = 0u; c < TC_RANS2_PREV_CTX; ++c) {
        tc_rans2_row_encode(w, TC_RANS_LVL_SYMS,
                            c < lvl_ctx_n ? lvl_rows + (size_t)c * TC_RANS_LVL_SYMS : zeros);
        w += TC_RANS_LVL_SYMS;
    }
}

/* 单平面 V8 载荷组装（[dir][tables][streams][crcs]）。返回 malloc 缓冲。 */
static int32_t v8_encode_plane(enc_shared* e, uint32_t plane,
                               const tc_quant_ctx* qctx, int32_t qp_delta,
                               uint32_t sb_log2, uint32_t tile_log2,
                               uint8_t** payload_out, size_t* payload_len_out,
                               uint32_t* tiles_out, uint64_t* plane_qhash_out)
{
    const topos_frame_header* fh = &e->fh;
    const uint32_t cols = fh->plane_block_cols[plane];
    const uint32_t rows = fh->plane_block_rows[plane];
    const uint32_t blocks = rows * cols;
    const uint32_t sb = 1u << sb_log2;
    const uint32_t tile_rows = tile_log2 == 0u ? 0u : (1u << tile_log2);
    const uint32_t tile_rows_eff = tile_rows == 0u ? rows : tile_rows;
    const uint32_t S = (blocks + sb - 1u) / sb;
    const uint32_t T = tile_rows == 0u ? 1u : (rows + tile_rows - 1u) / tile_rows;
    *tiles_out = T;
    *plane_qhash_out = 0u;

    /* 瓦片量化条纹（瓦片行 + 末段跨界溢出行）+ 目录/表区——单块槽 0
     * scratch 内雕刻（enc_scratch_acquire 同槽同指针，分区防别名） */
    const uint32_t ovf_rows = (sb + cols - 1u) / cols;
    const uint32_t qrows = tile_rows_eff + ovf_rows;
    const size_t qtile_bytes = (size_t)qrows * cols * 64u * sizeof(int32_t);
    const size_t dir_bytes = (size_t)S * TC_V8_DIR_ENTRY_BYTES;
    const size_t table_bytes = (size_t)T * TC_V8_TABLE_BYTES;
    uint8_t* scratch = enc_scratch_acquire(e, 0u, qtile_bytes + dir_bytes + table_bytes);
    if (scratch == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "v8 scratch %zu", qtile_bytes + dir_bytes + table_bytes);
        return TC_ERR_OUT_OF_MEMORY;
    }
    int32_t* qtile = (int32_t*)scratch;
    uint8_t* dir = scratch + qtile_bytes;
    uint8_t* tables = dir + dir_bytes;

    /* 流 arena（独立最小 enc_shared——qtile 占用槽 0，emit 引擎须隔离 scratch） */
    enc_shared ves;
    memset(&ves, 0, sizeof(ves));
    uint8_t* streams = NULL;
    size_t streams_cap = 0u;
    size_t streams_len = 0u;
    int32_t rc = TC_OK;
    v8_seg_tok* seg_tok = (v8_seg_tok*)tc_realloc(NULL, sizeof(v8_seg_tok));
    if (seg_tok == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "v8 seg tok");
        rc = TC_ERR_OUT_OF_MEMORY;
        goto done_plane;
    }

    for (uint32_t t = 0u; t < T && rc == TC_OK; ++t) {
        const uint32_t r0 = t * tile_rows_eff;
        const uint32_t r1 = rows < r0 + tile_rows_eff ? rows : r0 + tile_rows_eff;
        const uint32_t rq = rows < r1 + ovf_rows ? rows : r1 + ovf_rows;
        rc = v8_quant_tile_rows(e, plane, qctx, r0, rq - r0, qtile);
        if (rc != TC_OK) { goto done_plane; }
        /* 瓦片成员段 = [⌊t·tile_rows·cols/sb⌋, ⌊(t+1)·…/sb⌋)（批 1 扫描同式） */
        const uint64_t fb = (uint64_t)t * tile_rows_eff * cols;
        const uint64_t fn = (uint64_t)(t + 1u) * tile_rows_eff * cols;
        uint32_t sf = (uint32_t)(fb / sb); if (sf > S) { sf = S; }
        uint32_t sn = (uint32_t)(fn / sb); if (sn > S) { sn = S; }

        /* pass 1：成员段 token 化 → 瓦片聚合计数（order-0 三族 + 联合） */
        tile_acc acc;
        memset(&acc, 0, sizeof(acc));
        for (uint32_t s2 = sf; s2 < sn; ++s2) {
            const uint32_t b0 = s2 * sb;
            const uint32_t nb = blocks - b0 < sb ? blocks - b0 : sb;
            uint32_t dh[TC_RANS_DC_SYMS] = {0};
            uint32_t rh[TC_RANS_RUN_SYMS] = {0};
            uint32_t lh[TC_RANS_LVL_SYMS] = {0};
            rc = v8_tokenize_segment(qtile, r0, cols, b0, nb, seg_tok, dh, rh, lh);
            if (rc != TC_OK) { goto done_plane; }
            enc_band_tok sview;
            v8_seg_view(seg_tok, nb, &sview);
            for (uint32_t x = 0u; x < TC_RANS_DC_SYMS; ++x) { acc.dc[x] += dh[x]; }
            for (uint32_t x = 0u; x < TC_RANS_RUN_SYMS; ++x) { acc.run[x] += rh[x]; }
            for (uint32_t x = 0u; x < TC_RANS_LVL_SYMS; ++x) { acc.lvl[x] += lh[x]; }
            rans2_joints j;
            rans2_collect_joints(&sview, nb, &j);
            for (uint32_t x = 0u; x < TC_RANS2_POS_CTX * TC_RANS_LVL_SYMS; ++x) {
                acc.j.lvl_pos[x] += j.lvl_pos[x];
            }
            for (uint32_t x = 0u; x < TC_RANS2_PREV_CTX * TC_RANS_LVL_SYMS; ++x) {
                acc.j.lvl_prev[x] += j.lvl_prev[x];
            }
            for (uint32_t x = 0u; x < TC_RANS2_DC_CTX * TC_RANS_DC_SYMS; ++x) {
                acc.j.dc_prev[x] += j.dc_prev[x];
            }
        }
        /* 瓦片模型 argmin + 表写出（rows 来源同生产镜像选择） */
        tile_models tm;
        rans2_sim_pick_models(acc.dc, acc.lvl, &acc.j, &tm.lvl_model, &tm.dc_model);
        const uint32_t lvl_sel = tm.lvl_model;
        const uint32_t* t_dc_rows = tm.dc_model == TC_RANS2_DC_PREV ? acc.j.dc_prev : acc.dc;
        const uint32_t* t_lvl_rows = lvl_sel == TC_RANS2_LVL_POS ? acc.j.lvl_pos
            : (lvl_sel == TC_RANS2_LVL_PREV ? acc.j.lvl_prev : acc.lvl);
        v8_write_table(tables + (size_t)t * TC_V8_TABLE_BYTES,
                       tm.lvl_model, tm.dc_model, t_dc_rows, acc.run, t_lvl_rows);
        /* 瓦片模型构建（row_encode→row_decode 同源——解码端从表字节重建
         * 同一模型；freq≥1 契约由 byte≥1 保） */
        {
            const uint32_t dc_ctx_n = tm.dc_model == TC_RANS2_DC_PREV ? TC_RANS2_DC_CTX : 1u;
            const uint32_t lvl_ctx_n = lvl_sel == TC_RANS2_LVL_POS ? TC_RANS2_POS_CTX
                                     : (lvl_sel == TC_RANS2_LVL_PREV ? TC_RANS2_PREV_CTX : 1u);
            for (uint32_t c = 0u; c < dc_ctx_n; ++c) {
                rc = tile_sim_build_row(t_dc_rows + (size_t)c * TC_RANS_DC_SYMS,
                                        TC_RANS_DC_SYMS, &tm.dc_m[c]);
                if (rc != TC_OK) { goto done_plane; }
            }
            rc = tile_sim_build_row(acc.run, TC_RANS_RUN_SYMS, &tm.run_m);
            if (rc != TC_OK) { goto done_plane; }
            for (uint32_t c = 0u; c < lvl_ctx_n; ++c) {
                rc = tile_sim_build_row(t_lvl_rows + (size_t)c * TC_RANS_LVL_SYMS,
                                        TC_RANS_LVL_SYMS, &tm.lvl_m[c]);
                if (rc != TC_OK) { goto done_plane; }
            }
        }

        /* pass 2：逐段重 token 化（确定性）→ 瓦片模型后向 rANS → 流追加 */
        for (uint32_t s2 = sf; s2 < sn && rc == TC_OK; ++s2) {
            const uint32_t b0 = s2 * sb;
            const uint32_t nb = blocks - b0 < sb ? blocks - b0 : sb;
            uint32_t dh[TC_RANS_DC_SYMS] = {0};
            uint32_t rh[TC_RANS_RUN_SYMS] = {0};
            uint32_t lh[TC_RANS_LVL_SYMS] = {0};
            rc = v8_tokenize_segment(qtile, r0, cols, b0, nb, seg_tok, dh, rh, lh);
            if (rc != TC_OK) { break; }
            enc_band_tok sview;
            v8_seg_view(seg_tok, nb, &sview);
            size_t stream_len = 0u;
            rc = rans2_encode_stream_with_models(&sview, nb,
                                                 tm.lvl_model, tm.dc_model,
                                                 tm.dc_m, &tm.run_m, tm.lvl_m,
                                                 &ves, 0u, &stream_len);
            if (rc != TC_OK) { goto done_plane; }
            if (streams_len + stream_len > streams_cap) {
                size_t ncap = streams_cap != 0u ? streams_cap * 2u : (size_t)1u << 20;
                while (ncap < streams_len + stream_len) { ncap *= 2u; }
                uint8_t* ns = (uint8_t*)tc_realloc(streams, ncap);
                if (ns == NULL) {
                    tc_set_error(TC_ERR_OUT_OF_MEMORY, "v8 streams %zu", ncap);
                    rc = TC_ERR_OUT_OF_MEMORY;
                    goto done_plane;
                }
                streams = ns;
                streams_cap = ncap;
            }
            memcpy(streams + streams_len, ves.slot_scratch[0], stream_len);
            uint8_t* de = dir + (size_t)s2 * TC_V8_DIR_ENTRY_BYTES;
            tc_store_be32(de, (uint32_t)streams_len);
            tc_store_be32(de + 4u, (uint32_t)stream_len);
            de[8] = (uint8_t)(64 + qp_delta);
            de[9] = 0u;
            de[10] = 0u;
            de[11] = 0u;
            streams_len += stream_len;
        }
        /* 帧 q-hash（位精确 oracle，解码侧同构折叠）：段内块栅格序折叠 +
         * 段序/瓦片序/平面序合并 */
        if (s_v8_qhash_on) {
            /* 段 hash 加法合并（与任务分组无关；编解码两侧同构） */
            for (uint32_t s2 = sf; s2 < sn; ++s2) {
                const uint32_t b0 = s2 * sb;
                const uint32_t nb = blocks - b0 < sb ? blocks - b0 : sb;
                uint64_t sh = 0u;
                for (uint32_t i = 0u; i < nb; ++i) {
                    const uint32_t abs_blk = b0 + i;
                    const int32_t* q = qtile
                        + (size_t)(abs_blk / cols - r0) * (size_t)cols * 64u
                        + (size_t)(abs_blk % cols) * 64u;
                    for (uint32_t s3 = 0u; s3 < 64u; ++s3) {
                        sh = tc_symbol_hash_mix(sh, (uint64_t)(int64_t)q[s3]);
                    }
                }
                *plane_qhash_out += sh;
            }
        }
    }

done_plane:
    if (rc != TC_OK) {
        tc_free(streams);
        tc_free(seg_tok);
        tc_free(ves.slot_scratch[0]);
        return rc;
    }
    /* 组装：dir + tables + streams + crcs（CRC = 表 350B + 成员段流连续区间） */
    const size_t payload_len = (size_t)S * TC_V8_DIR_ENTRY_BYTES
                             + (size_t)T * TC_V8_TABLE_BYTES
                             + streams_len + (size_t)T * 4u;
    uint8_t* payload = (uint8_t*)tc_realloc(NULL, payload_len != 0u ? payload_len : 1u);
    if (payload == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "v8 plane payload %zu", payload_len);
        tc_free(streams);
        tc_free(seg_tok);
        tc_free(ves.slot_scratch[0]);
        return TC_ERR_OUT_OF_MEMORY;
    }
    memcpy(payload, dir, (size_t)S * TC_V8_DIR_ENTRY_BYTES);
    memcpy(payload + (size_t)S * TC_V8_DIR_ENTRY_BYTES, tables,
           (size_t)T * TC_V8_TABLE_BYTES);
    const size_t stream_base = (size_t)S * TC_V8_DIR_ENTRY_BYTES
                             + (size_t)T * TC_V8_TABLE_BYTES;
    if (streams_len != 0u) { memcpy(payload + stream_base, streams, streams_len); }
    const uint8_t* dirp = payload;
    for (uint32_t t = 0u; t < T; ++t) {
        const uint64_t fb = (uint64_t)t * tile_rows_eff * cols;
        const uint64_t fn = (uint64_t)(t + 1u) * tile_rows_eff * cols;
        uint32_t sf = (uint32_t)(fb / sb); if (sf > S) { sf = S; }
        uint32_t sn = (uint32_t)(fn / sb); if (sn > S) { sn = S; }
        const uint32_t soff = tc_load_be32(dirp + (size_t)sf * TC_V8_DIR_ENTRY_BYTES);
        uint32_t sbytes = 0u;
        for (uint32_t s2 = sf; s2 < sn; ++s2) {
            sbytes += tc_load_be32(dirp + (size_t)s2 * TC_V8_DIR_ENTRY_BYTES + 4u);
        }
        uint32_t crc = tc_crc32_update(0u,
            payload + (size_t)S * TC_V8_DIR_ENTRY_BYTES + (size_t)t * TC_V8_TABLE_BYTES,
            TC_V8_TABLE_BYTES);
        crc = tc_crc32_update(crc, payload + stream_base + soff, sbytes);
        tc_store_be32(payload + stream_base + streams_len + (size_t)t * 4u, crc);
    }
    tc_free(streams);
    tc_free(seg_tok);
    tc_free(ves.slot_scratch[0]);
    *payload_out = payload;
    *payload_len_out = payload_len;
    return TC_OK;
}

/* V8 帧编码（批 2）：颜色三平面串行；alpha 未支持（批 5 评估）。
 * 布局：[53B header（后写）][8B ext][per plane payload]；写后自检 = scan_v8。 */
static int32_t v8_frame_encode_ready(enc_shared* e, uint8_t* out, size_t out_cap,
                                     topos_frame_stats* stats)
{
    if (e->fh.alpha_mode != 0u) {
        tc_set_error(TC_ERR_NOT_IMPLEMENTED, "v8 alpha pending (format-plan batch 5)");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    /* TRAW（pf=3）走 rANS2 链（V7-R2）；V8 三平面机器不承载 CFA（批 1） */
    if (e->fh.pixel_format == 3u) {
        tc_set_error(TC_ERR_NOT_IMPLEMENTED, "v8 does not carry CFA (TRAW uses rans2)");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    const topos_frame_header* fh = &e->fh;
    const tc_qmatrix_set* qms = lookup_qm(fh->qmatrix_id);
    if (qms == NULL) {
        tc_set_error(TC_ERR_UNSUPPORTED_MATRIX, "qmatrix_id %u", (unsigned)fh->qmatrix_id);
        return TC_ERR_UNSUPPORTED_MATRIX;
    }
    /* plane pad（M11 管线复用）+ per-plane qp（镜像 enc_band_pass_all） */
    for (uint32_t p = 0u; p < 3u; ++p) {
        int32_t prc = enc_prepare_plane(e, p);
        if (prc != TC_OK) { return prc; }
    }
    tc_quant_ctx qctx[3];
    int32_t qp_delta[3];
    for (uint32_t p = 0u; p < 3u; ++p) {
        const uint16_t* qm = (p == 0u) ? qms->luma : qms->chroma;
        const int32_t delta = (p == 0u) ? e->qp_delta_luma : e->qp_delta_chroma;
        int32_t qe = (int32_t)fh->qp_base + delta + bd_qp_offset(fh->bit_depth);
        if (qe < 0) { qe = 0; }
        const int32_t ceiling = (int32_t)tc_qp_eff_ceiling(fh->qp_base);
        if (qe > ceiling) { qe = ceiling; }
        qp_delta[p] = qe - (int32_t)fh->qp_base;
        tc_quant_ctx_init_bd_tbl(&qctx[p], qm, (uint32_t)qe, fh->bit_depth,
                                 tc_qtbl_of_flags(fh->flags));
    }

    uint8_t* payloads[3] = {NULL, NULL, NULL};
    size_t payload_lens[3] = {0u, 0u, 0u};
    uint32_t tiles[3] = {0u, 0u, 0u};
    uint64_t plane_hashes[3] = {0u, 0u, 0u};
    int32_t rc = TC_OK;
    size_t total = TC_FRAME_HEADER_SIZE + TC_V8_EXT_HEADER_SIZE;
    for (uint32_t p = 0u; p < 3u && rc == TC_OK; ++p) {
        rc = v8_encode_plane(e, p, &qctx[p], qp_delta[p], e->v8_sb_log2,
                             e->v8_tile_log2, &payloads[p], &payload_lens[p],
                             &tiles[p], &plane_hashes[p]);
        if (rc == TC_OK) { total += payload_lens[p]; }
    }
    if (rc == TC_OK) {
        uint64_t fh_ = 0u;
        for (uint32_t p = 0u; p < 3u; ++p) {
            fh_ += plane_hashes[p];  /* 加法合并（与解码器同构，分组无关） */
        }
        tc_dev_v8_enc_qhash_set(fh_);
    }
    if (rc != TC_OK) { goto done_frame; }

    e->fh.slice_count = (uint16_t)(tiles[0] + tiles[1] + tiles[2]);
    if (e->fh.slice_count < 3u || e->fh.slice_count > TC_MAX_SLICE_COUNT) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v8 tile count %u out of [3,512]",
                     (unsigned)e->fh.slice_count);
        rc = TC_ERR_LIMIT_EXCEEDED;
        goto done_frame;
    }
    e->fh.frame_packet_size = (uint32_t)total;
    if (out == NULL || total > out_cap) {
        tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "out_cap %zu < %zu bytes needed",
                     out_cap, total);
        rc = TC_ERR_BUFFER_TOO_SMALL;
        goto done_frame;
    }

    /* 写出：ext header + 平面载荷；帧头最后（frame_packet_size 已定） */
    memset(out + TC_FRAME_HEADER_SIZE, 0, TC_V8_EXT_HEADER_SIZE);
    out[TC_FRAME_HEADER_SIZE] = (uint8_t)e->v8_sb_log2;
    out[TC_FRAME_HEADER_SIZE + 1u] = (uint8_t)e->v8_tile_log2;
    size_t off = TC_FRAME_HEADER_SIZE + TC_V8_EXT_HEADER_SIZE;
    for (uint32_t p = 0u; p < 3u; ++p) {
        memcpy(out + off, payloads[p], payload_lens[p]);
        off += payload_lens[p];
    }
    rc = tc_frame_header_encode(&e->fh, out);
    if (rc != TC_OK) { goto done_frame; }

    /* 自检（§11.5 V8 版）：scan_v8 结构 + 全瓦片 CRC */
    {
        topos_v8_packet_view view;
        rc = tc_packet_scan_v8(out, total, &view);
        if (rc == TC_OK) {
            for (uint32_t t = 0u; t < view.tile_count; ++t) {
                if (view.tiles[t].crc_ok == 0u) {
                    tc_set_error(TC_ERR_MALFORMED, "v8 self-check: tile %u crc", t);
                    rc = TC_ERR_MALFORMED;
                    break;
                }
            }
        } else {
            tc_wrap_error(TC_ERR_MALFORMED, "v8 self-check scan failed: "); /* C14 */
        }
    }
    if (rc != TC_OK) { goto done_frame; }

    memset(stats, 0, sizeof(*stats));
    stats->struct_size = (uint32_t)sizeof(*stats);
    stats->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    stats->packet_size = (uint32_t)total;
    for (uint32_t p = 0u; p < 3u; ++p) { stats->color_payload_bytes += (uint32_t)payload_lens[p]; }
    stats->color_header_bytes = (uint32_t)(TC_FRAME_HEADER_SIZE + TC_V8_EXT_HEADER_SIZE);
    stats->slice_count = e->fh.slice_count;
    stats->qp_base = fh->qp_base;

done_frame:
    for (uint32_t p = 0u; p < 3u; ++p) { tc_free(payloads[p]); }
    return rc;
}

/* ==================== V8 解码器（批 3：CPU 参考解码/回退路径）====================
 * scan_v8 → 瓦片表解析 + LUT 单次构建（每平面一次，禁每段建表）→
 * 段并行（任务 = 瓦片；段间零共享写）前向 rANS → 融合重建
 * （tc_color_store 复用，dinv 内核）→ 段级 conceal（瓦片 CRC 门控：
 * 全零 q → mid 填充，与 V7 dev_qcap_zero_slice 语义一致）。
 * 位精确 oracle：帧级 q-hash（tc_symbol_hash_mix 折叠；瓦片内块序栅格、
 * 跨瓦片/跨平面顺序合并；编码/解码同构）经 tc_dev_v8_enc_qhash /
 * tc_dev_v8_dec_qhash 导出——clean 流差分必须逐位相等。 */

static _Atomic uint64_t g_v8_enc_qhash;
static _Atomic uint64_t g_v8_dec_qhash;

void tc_dev_v8_enc_qhash_set(uint64_t h) { atomic_store(&g_v8_enc_qhash, h); }
uint64_t tc_dev_v8_enc_qhash(void) { return atomic_load(&g_v8_enc_qhash); }
uint64_t tc_dev_v8_dec_qhash(void) { return atomic_load(&g_v8_dec_qhash); }

/* 瓦片解码资产（每平面构建一次） */
typedef struct v8_tile_dec {
    uint32_t lvl_model;
    uint32_t dc_model;
    tc_rans_model dc_m[TC_RANS2_DC_CTX];
    tc_rans_model run_m;
    tc_rans_model lvl_m[TC_RANS2_PREV_CTX];
    tc_rans_lut dc_lut[TC_RANS2_DC_CTX];
    tc_rans_lut run_lut;
    tc_rans_lut lvl_lut[TC_RANS2_PREV_CTX];
} v8_tile_dec;

static int32_t v8_tile_dec_build(const uint8_t* t350, v8_tile_dec* td)
{
    const uint8_t flags = t350[0];
    const uint32_t lvl_model = flags & 0x3u;
    const uint32_t dc_model = (flags >> 2) & 0x1u;
    if ((flags & 0xF8u) != 0u || lvl_model > TC_RANS2_LVL_PREV) {
        tc_set_error(TC_ERR_MALFORMED, "v8 tile flags %u", (unsigned)flags);
        return TC_ERR_MALFORMED;
    }
    const uint32_t lvl_ctx_n = lvl_model == TC_RANS2_LVL_POS ? TC_RANS2_POS_CTX
                             : (lvl_model == TC_RANS2_LVL_PREV ? TC_RANS2_PREV_CTX : 1u);
    const uint32_t dc_ctx_n = dc_model == TC_RANS2_DC_PREV ? TC_RANS2_DC_CTX : 1u;
    td->lvl_model = lvl_model;
    td->dc_model = dc_model;
    /* 表布局定长：dc 行 0..4 + run + lvl 行 0..4（未用行占位）——行基址
     * 固定，只构建已选 ctx 行的模型/LUT */
    const uint8_t* dc_base = t350 + 1u;
    const uint8_t* run_base = dc_base + (size_t)TC_RANS2_DC_CTX * TC_RANS_DC_SYMS;
    const uint8_t* lvl_base = run_base + TC_RANS_RUN_SYMS;
    for (uint32_t c = 0u; c < dc_ctx_n; ++c) {
        if (tc_rans2_row_decode(dc_base + (size_t)c * TC_RANS_DC_SYMS,
                                TC_RANS_DC_SYMS, &td->dc_m[c]) != TC_OK) {
            tc_set_error(TC_ERR_MALFORMED, "v8 tile dc row %u", c);
            return TC_ERR_MALFORMED;
        }
    }
    if (tc_rans2_row_decode(run_base, TC_RANS_RUN_SYMS, &td->run_m) != TC_OK) {
        tc_set_error(TC_ERR_MALFORMED, "v8 tile run row");
        return TC_ERR_MALFORMED;
    }
    for (uint32_t c = 0u; c < lvl_ctx_n; ++c) {
        if (tc_rans2_row_decode(lvl_base + (size_t)c * TC_RANS_LVL_SYMS,
                                TC_RANS_LVL_SYMS, &td->lvl_m[c]) != TC_OK) {
            tc_set_error(TC_ERR_MALFORMED, "v8 tile lvl row %u", c);
            return TC_ERR_MALFORMED;
        }
    }
    /* 未用行 LUT 不建（解码只查已选 ctx 行） */
    for (uint32_t c = 0u; c < dc_ctx_n; ++c) { tc_rans_lut_build(&td->dc_lut[c], &td->dc_m[c]); }
    tc_rans_lut_build(&td->run_lut, &td->run_m);
    for (uint32_t c = 0u; c < lvl_ctx_n; ++c) { tc_rans_lut_build(&td->lvl_lut[c], &td->lvl_m[c]); }
    return TC_OK;
}

/* 段解码 → 稠密 q（nb×64 自然序）+ 块 rowmask。瓦片模型/LUT 由调用方
 * 传入（共享只读）；段局部 DC 因果与编码器 v8_tokenize_segment 同构。
 * store 非 NULL：逐块融合重建（免 q 二传）；hash_out 仅 oracle 开启时折叠。 */
static int32_t v8_decode_segment_q(const uint8_t* stream, size_t len,
                                   const v8_tile_dec* td, uint32_t nb,
                                   uint32_t cols, int32_t* seg_q,
                                   uint32_t* rowmasks, uint64_t* hash_out,
                                   tc_color_store_ctx* store, uint32_t seg0)
{
    tc_rans_dec dec;
    int32_t rc = tc_rans_dec_init(&dec, stream, len);
    if (rc != TC_OK) { return rc; }
    const uint32_t lvl_model = td->lvl_model;
    const uint32_t dc_model = td->dc_model;
    int32_t seg_dc[TC_V8_MAX_SEG_BLOCKS];
    uint32_t prev_dc_cat = 0u;
    uint64_t hash = 0u;
    for (uint32_t i = 0u; i < nb; ++i) {
        int32_t* blk = seg_q + (size_t)i * 64u;
        memset(blk, 0, 64u * sizeof(int32_t));
        const uint32_t dctx_d = (i == 0u || dc_model != TC_RANS2_DC_PREV)
            ? 0u : tc_rans2_dc_ctx(prev_dc_cat, 1);
        uint32_t cat = 0u;
        rc = tc_rans_get_lut(&dec, &td->dc_lut[dctx_d], &td->dc_m[dctx_d], &cat);
        if (rc != TC_OK) { return rc; }
        uint32_t m = 0u;
        rc = tc_rans_get_suffix(&dec, cat, &m);
        if (rc != TC_OK) { return rc; }
        if (m > TC_RICE_M_MAX_DC) {
            tc_set_error(TC_ERR_MALFORMED, "v8 dc magnitude %u", m);
            return TC_ERR_MALFORMED;
        }
        const int has_left = i != 0u ? 1 : 0;
        const int has_top = i >= cols ? 1 : 0;
        const int32_t pred = tc_dc_predict(has_left,
                                           i != 0u ? seg_dc[i - 1u] : 0,
                                           has_top,
                                           seg_dc[i >= cols ? i - cols : 0u]);
        rc = tc_dc_reconstruct_checked(pred, m, &blk[0]);
        if (rc != TC_OK) { return rc; }
        seg_dc[i] = blk[0];
        prev_dc_cat = cat;
        uint32_t rowmask = 1u;
        uint32_t pos = 1u;
        uint32_t prev_cat = 0u;
        int has_prev = 0;
        for (;;) {
            uint32_t run_sym = 0u;
            rc = tc_rans_get_lut(&dec, &td->run_lut, &td->run_m, &run_sym);
            if (rc != TC_OK) { return rc; }
            if (run_sym == 63u) { break; } /* EOB */
            const uint64_t idx = (uint64_t)pos + (uint64_t)run_sym;
            if (idx > 63u) {
                tc_set_error(TC_ERR_MALFORMED, "v8 ac index %llu > 63",
                             (unsigned long long)idx);
                return TC_ERR_MALFORMED;
            }
            uint32_t lc = 0u;
            if (lvl_model == TC_RANS2_LVL_POS) {
                lc = tc_rans2_pos_ctx((uint32_t)idx);
            } else if (lvl_model == TC_RANS2_LVL_PREV) {
                lc = tc_rans2_prevlvl_ctx(prev_cat, has_prev);
            }
            rc = tc_rans_get_lut(&dec, &td->lvl_lut[lc], &td->lvl_m[lc], &cat);
            if (rc != TC_OK) { return rc; }
            rc = tc_rans_get_suffix(&dec, cat, &m);
            if (rc != TC_OK) { return rc; }
            if (m > TC_RICE_M_MAX_AC_LEVEL) {
                tc_set_error(TC_ERR_MALFORMED, "v8 level magnitude %u", m);
                return TC_ERR_MALFORMED;
            }
            const uint32_t natural = kTcZigzag[idx];
            blk[natural] = tc_rice_unmap_signed(m);
            rowmask |= 1u << (natural >> 3);
            prev_cat = cat;
            has_prev = 1;
            pos = (uint32_t)idx + 1u;
        }
        if (rowmasks != NULL) { rowmasks[i] = rowmask; }
        if (store != NULL) {
            const uint32_t abs_blk = seg0 + i;
            (void)tc_color_store_block_xy(store, abs_blk, abs_blk % cols,
                                          abs_blk / cols, blk, rowmask);
        }
        if (s_v8_qhash_on) {
            for (uint32_t s = 0u; s < 64u; ++s) {
                hash = tc_symbol_hash_mix(hash, (uint64_t)(int64_t)blk[s]);
            }
        }
    }
    if (dec.pos != dec.size) {
        tc_set_error(TC_ERR_MALFORMED, "v8 segment %zu trailing bytes",
                     dec.size - dec.pos);
        return TC_ERR_MALFORMED;
    }
    *hash_out = hash;
    return TC_OK;
}

/* 平面解码上下文（并行任务只读共享） */
typedef struct v8_plane_dec {
    topos_v8_packet_view* view;   /* 非 const：crc 任务回写 tile crc_ok */
    uint32_t plane;
    const uint8_t* data;
    uint32_t tile_base;        /* 全帧瓦片序基（view.tiles 下标） */
    v8_tile_dec* tiles;        /* T_p 个 */
    uint16_t* dst;
    size_t stride;
    uint32_t vis_w, vis_h;
    const tc_qmatrix_set* qms;
    int32_t qp_base;
    int32_t qp_delta;          /* plane 级（luma/chroma） */
    uint8_t bd;
    tc_dequant_inverse_fn dinv;
    uint32_t tile_count;
    uint64_t* tile_hash;       /* T_p 出（顺序合并成平面/帧 hash） */
    /* 段级 qp 去重缓存（典型 1 项）：免每段重建量化表（批 3 perf） */
    tc_quant_ctx* qcache;
    uint8_t* qcache_biased;
    uint32_t qcache_n;
} v8_plane_dec;

/* 解码任务 1：瓦片 CRC 重算（结构扫描 verify_crc=0 后并行做且每瓦片一次
 * ——与 V7「slice worker 对唯一 payload 做且只做一次 CRC」设计同构） */
typedef struct v8_crc_job {
    v8_plane_dec* pd;
    uint32_t tile;             /* plane 内瓦片序 */
} v8_crc_job;

static void v8_crc_job_fn(void* vctx)
{
    v8_crc_job* jt = (v8_crc_job*)vctx;
    v8_plane_dec* pd = jt->pd;
    topos_v8_tile_info* ti = &pd->view->tiles[pd->tile_base + jt->tile];
    uint32_t crc = tc_crc32_update(0u, pd->data + ti->table_off, TC_V8_TABLE_BYTES);
    crc = tc_crc32_update(crc, pd->data + ti->stream_off, ti->stream_bytes);
    ti->crc_ok = crc == ti->crc_stored ? 1u : 0u;
}

/* 解码任务 2：段块（tile 内连续段区间；跨块写域不相交） */
typedef struct v8_chunk_job {
    v8_plane_dec* pd;
    uint32_t tile;             /* plane 内瓦片序 */
    uint32_t seg_first;        /* 段块起点（tile 内段序） */
    uint32_t seg_count;        /* 段块段数 */
    uint32_t concealed;        /* 本块 conceal 段数 */
    uint64_t sh_sum;           /* 段 hash 和（加法合并，分组无关） */
} v8_chunk_job;

static void v8_chunk_job_fn(void* vctx)
{
    v8_chunk_job* jt = (v8_chunk_job*)vctx;
    v8_plane_dec* pd = jt->pd;
    const topos_v8_packet_view* view = pd->view;
    const uint32_t p = pd->plane;
    const uint32_t cols = view->fh.plane_block_cols[p];
    const uint32_t rows = view->fh.plane_block_rows[p];
    const uint32_t blocks = rows * cols;
    const topos_v8_tile_info* ti = &view->tiles[pd->tile_base + jt->tile];
    v8_tile_dec* td = &pd->tiles[jt->tile];
    const int conceal = ti->crc_ok == 0u;

    tc_color_store_ctx store;
    memset(&store, 0, sizeof(store));
    store.dst = pd->dst;
    store.stride = pd->stride;
    store.cols = cols;
    store.block_y0 = 0u;
    store.vis_w = pd->vis_w;
    store.vis_h = pd->vis_h;
    store.dst_w = pd->vis_w;
    store.dst_h = pd->vis_h;
    store.scaled = 0u;
    store.coefficient_limit = 63u;
    store.mid = bd_mid(pd->bd);
    store.max = (uint32_t)(1u << pd->bd) - 1u;
    store.w0 = tc_transform_weights()[0];
    store.dinv = pd->dinv;
    store.stats = NULL;

    int32_t seg_q[TC_V8_MAX_SEG_BLOCKS * 64];
    const uint32_t sb = view->segment_blocks;
    for (uint32_t s = jt->seg_first; s < jt->seg_first + jt->seg_count; ++s) {
        const uint32_t b0 = s * sb;
        const uint32_t nb = blocks - b0 < sb ? blocks - b0 : sb;
        const uint8_t* dir_e = pd->data + view->plane_dir_off[p]
                             + (size_t)s * TC_V8_DIR_ENTRY_BYTES;
        const uint32_t soff = tc_load_be32(dir_e);
        const uint32_t slen = tc_load_be32(dir_e + 4u);
        const uint8_t biased = dir_e[8];
        uint32_t qi = 0u;
        while (qi < pd->qcache_n && pd->qcache_biased[qi] != biased) { qi++; }
        store.qctx = &pd->qcache[qi];
        uint64_t sh = 0u;
        if (conceal) {
            /* 瓦片 conceal：全零 q → DC-only 闭式 → mid 填充 */
            int32_t zero_q[64];
            memset(zero_q, 0, sizeof(zero_q));
            for (uint32_t i = 0u; i < nb; ++i) {
                const uint32_t abs_blk = b0 + i;
                (void)tc_color_store_block_xy(&store, abs_blk, abs_blk % cols,
                                              abs_blk / cols, zero_q, 0u);
            }
        } else {
            const int32_t drc = v8_decode_segment_q(
                pd->data + view->plane_stream_off[p] + soff, slen,
                td, nb, cols, seg_q, NULL, &sh, &store, b0);
            if (drc != TC_OK) {
                /* 段级 conceal：解码半途失败 → 该段全部块补 mid */
                jt->concealed++;
                int32_t zero_q[64];
                memset(zero_q, 0, sizeof(zero_q));
                for (uint32_t i = 0u; i < nb; ++i) {
                    const uint32_t abs_blk = b0 + i;
                    (void)tc_color_store_block_xy(&store, abs_blk, abs_blk % cols,
                                                  abs_blk / cols, zero_q, 0u);
                }
            }
        }
        jt->sh_sum += sh;
    }
}

/* V8 批 4：GPU 导出——scan_v8 + 瓦片表解析（解码器同源 row_decode）→
 * 每 segment smeta（14×u32）+ 每 tile 归一化表（709×u16，布局同
 * tc_dev_packet_slices：dc_f[145] dc_c[150] run_f[64] run_c[65]
 * lvl_f[140] lvl_c[145]；未用行全零）。smeta：plane, seg0, nblocks,
 * stream_off, stream_len, flags, dc_ctx_n, lvl_ctx_n, crc_ok, plane_cols,
 * dc0, off0, pairs0, tab_index。dc0/off0/pairs0 为平面 CSR 契约前缀
 * （dc 平面稠密 / off 每段 [nblocks+1] / pairs 最坏 63×nblocks）。 */
static uint32_t view_tile_base(const topos_v8_packet_view* view, uint32_t plane)
{
    uint32_t base = 0u;
    for (uint32_t pp = 0u; pp < plane; ++pp) { base += view->tiles_per_plane[pp]; }
    return base;
}

int32_t tc_dev_v8_gpu_export(const uint8_t* pkt, size_t size,
                             uint32_t* smeta_out, uint32_t smeta_stride,
                             uint16_t* tabs_out, uint32_t tabs_stride,
                             uint32_t* n_segments_out,
                             uint32_t* n_tiles_out,
                             size_t* stream_bytes_out)
{
    topos_v8_packet_view view;
    int32_t rc = tc_packet_scan_v8_ex(pkt, size, &view, 1);
    if (rc != TC_OK) { return rc; }
    const uint32_t n_tiles = view.tile_count;
    const uint32_t n_segs = view.segs_per_plane[0] + view.segs_per_plane[1]
                          + view.segs_per_plane[2];
    *n_segments_out = n_segs;
    *n_tiles_out = n_tiles;
    if (smeta_out == NULL || tabs_out == NULL) {
        /* 容量探针：仅计数 */
        *stream_bytes_out = size;
        return TC_OK;
    }
    if (smeta_stride < 14u || tabs_stride < 709u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v8 gpu export args");
        return TC_ERR_INVALID_ARGUMENT;
    }

    /* 瓦片表解析（解码器同源） */
    for (uint32_t t = 0u; t < n_tiles; ++t) {
        const topos_v8_tile_info* ti = &view.tiles[t];
        uint16_t* tab = tabs_out + (size_t)t * tabs_stride;
        memset(tab, 0, (size_t)709u * sizeof(uint16_t));
        const uint8_t* t350 = pkt + ti->table_off;
        const uint8_t flags = t350[0];
        const uint32_t lvl_model = flags & 0x3u;
        const uint32_t dc_model = (flags >> 2u) & 0x1u;
        const uint32_t lvl_ctx_n = lvl_model == TC_RANS2_LVL_POS
            ? TC_RANS2_POS_CTX
            : (lvl_model == TC_RANS2_LVL_PREV ? TC_RANS2_PREV_CTX : 1u);
        const uint32_t dc_ctx_n = dc_model == TC_RANS2_DC_PREV
            ? TC_RANS2_DC_CTX : 1u;
        uint16_t* dc_f = tab;
        uint16_t* dc_c = tab + 145u;
        uint16_t* run_f = dc_c + 150u;
        uint16_t* run_c = run_f + 64u;
        uint16_t* lvl_f = run_c + 65u;
        uint16_t* lvl_c = lvl_f + 140u;
        const uint8_t* dc_base = t350 + 1u;
        const uint8_t* run_base = dc_base + (size_t)TC_RANS2_DC_CTX * TC_RANS_DC_SYMS;
        const uint8_t* lvl_base = run_base + TC_RANS_RUN_SYMS;
        for (uint32_t c = 0u; c < dc_ctx_n; ++c) {
            tc_rans_model m;
            if (tc_rans2_row_decode(dc_base + (size_t)c * TC_RANS_DC_SYMS,
                                    TC_RANS_DC_SYMS, &m) != TC_OK) {
                tc_set_error(TC_ERR_MALFORMED, "v8 export dc row %u", c);
                return TC_ERR_MALFORMED;
            }
            memcpy(dc_f + (size_t)c * TC_RANS_DC_SYMS, m.freq,
                   TC_RANS_DC_SYMS * sizeof(uint16_t));
            memcpy(dc_c + (size_t)c * (TC_RANS_DC_SYMS + 1u), m.cum,
                   (TC_RANS_DC_SYMS + 1u) * sizeof(uint16_t));
        }
        {
            tc_rans_model m;
            if (tc_rans2_row_decode(run_base, TC_RANS_RUN_SYMS, &m) != TC_OK) {
                tc_set_error(TC_ERR_MALFORMED, "v8 export run row");
                return TC_ERR_MALFORMED;
            }
            memcpy(run_f, m.freq, TC_RANS_RUN_SYMS * sizeof(uint16_t));
            memcpy(run_c, m.cum, (TC_RANS_RUN_SYMS + 1u) * sizeof(uint16_t));
        }
        for (uint32_t c = 0u; c < lvl_ctx_n; ++c) {
            tc_rans_model m;
            if (tc_rans2_row_decode(lvl_base + (size_t)c * TC_RANS_LVL_SYMS,
                                    TC_RANS_LVL_SYMS, &m) != TC_OK) {
                tc_set_error(TC_ERR_MALFORMED, "v8 export lvl row %u", c);
                return TC_ERR_MALFORMED;
            }
            memcpy(lvl_f + (size_t)c * TC_RANS_LVL_SYMS, m.freq,
                   TC_RANS_LVL_SYMS * sizeof(uint16_t));
            memcpy(lvl_c + (size_t)c * (TC_RANS_LVL_SYMS + 1u), m.cum,
                   (TC_RANS_LVL_SYMS + 1u) * sizeof(uint16_t));
        }
    }

    /* segment smeta + 瓦片归属 */
    size_t stream_bytes = 0u;
    uint32_t seg = 0u;
    size_t off0 = 0u;
    size_t pairs0 = 0u;
    uint32_t plane_dc_base = 0u;
    for (uint32_t p = 0u; p < view.fh.plane_count; ++p) {
        const uint32_t cols = view.fh.plane_block_cols[p];
        const uint32_t blocks = view.fh.plane_block_rows[p] * cols;
        for (uint32_t s = 0u; s < view.segs_per_plane[p]; ++s) {
            const uint8_t* dir_e = pkt + view.plane_dir_off[p]
                                 + (size_t)s * TC_V8_DIR_ENTRY_BYTES;
            uint32_t* me = smeta_out + (size_t)seg * smeta_stride;
            me[0] = p;
            me[1] = s * view.segment_blocks;
            const uint32_t nb = blocks - me[1] < view.segment_blocks
                ? blocks - me[1] : view.segment_blocks;
            me[2] = nb;
            me[3] = view.plane_stream_off[p] + tc_load_be32(dir_e);
            me[4] = tc_load_be32(dir_e + 4u);
            stream_bytes += me[4];
            /* 段 → 瓦片（段归首块所在瓦片；瓦片段区间连续） */
            uint32_t tile_g = view_tile_base(&view, p);
            for (uint32_t t = 0u, base = 0u; t < view.tiles_per_plane[p]; ++t) {
                const topos_v8_tile_info* ti = &view.tiles[view_tile_base(&view, p) + t];
                if (s >= ti->seg_first && s < ti->seg_first + ti->seg_count) {
                    tile_g = view_tile_base(&view, p) + t;
                    break;
                }
                (void)base;
            }
            const topos_v8_tile_info* ti = &view.tiles[tile_g];
            me[8] = ti->crc_ok;
            me[9] = cols;
            me[10] = plane_dc_base + me[1];
            me[11] = (uint32_t)off0;
            me[12] = (uint32_t)pairs0;
            me[13] = tile_g;
            /* flags/ctx_n 从瓦片表读（host 同源解析） */
            {
                const uint8_t* t350 = pkt + ti->table_off;
                const uint8_t flags = t350[0];
                const uint32_t lm = flags & 0x3u;
                const uint32_t dm = (flags >> 2u) & 0x1u;
                me[5] = flags;
                me[6] = dm == TC_RANS2_DC_PREV ? TC_RANS2_DC_CTX : 1u;
                me[7] = lm == TC_RANS2_LVL_POS ? TC_RANS2_POS_CTX
                      : (lm == TC_RANS2_LVL_PREV ? TC_RANS2_PREV_CTX : 1u);
            }
            off0 += (size_t)nb + 1u;
            pairs0 += 63u * (size_t)nb;
            seg++;
        }
        plane_dc_base += blocks;
    }
    *stream_bytes_out = stream_bytes;
    return TC_OK;
}

int32_t v8_frame_decode(const uint8_t* data, size_t size,
                               uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                               const size_t strides[TC_FRAME_MAX_PLANES],
                               topos_frame_output* out_info)
{
    topos_v8_packet_view view;
    int32_t rc = tc_packet_scan_v8_ex(data, size, &view, 0);
    if (rc != TC_OK) { return rc; }
    /* TRAW（pf=3）不承载 V8（编码侧同拒）——V8 机器三平面色度语义 */
    if (view.fh.pixel_format == 3u) {
        tc_set_error(TC_ERR_UNSUPPORTED_PIXEL_FORMAT,
                     "v8 does not carry CFA (TRAW uses rans2)");
        return TC_ERR_UNSUPPORTED_PIXEL_FORMAT;
    }
    tc_fill_output_info(&view.fh, out_info);

    const uint32_t plane_count = view.fh.plane_count;
    v8_plane_dec* pls = (v8_plane_dec*)tc_alloc((size_t)plane_count * sizeof(v8_plane_dec));
    if (pls == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "v8 dec planes");
        return TC_ERR_OUT_OF_MEMORY;
    }
    memset(pls, 0, (size_t)plane_count * sizeof(v8_plane_dec));
    /* planes_out 逐平面非 NULL（与 V7 路径同契约）；strides==NULL = tight */
    for (uint32_t p = 0u; p < plane_count; ++p) {
        if (planes_out[p] == NULL) {
            tc_free(pls);
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "v8 planes_out[%u] == NULL",
                         (unsigned)p);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    uint32_t total_tiles = 0u;
    int32_t brc = TC_OK;
    for (uint32_t p = 0u; p < plane_count && brc == TC_OK; ++p) {
        v8_plane_dec* pd = &pls[p];
        const uint32_t T = view.tiles_per_plane[p];
        pd->view = &view;
        pd->plane = p;
        pd->data = data;
        pd->tile_base = total_tiles;
        pd->tiles = (v8_tile_dec*)tc_alloc((size_t)T * sizeof(v8_tile_dec));
        pd->tile_hash = (uint64_t*)tc_alloc((size_t)T * sizeof(uint64_t));
        if (pd->tiles == NULL || pd->tile_hash == NULL) { brc = TC_ERR_OUT_OF_MEMORY; break; }
        for (uint32_t t = 0u; t < T; ++t) {
            brc = v8_tile_dec_build(data + view.tiles[pd->tile_base + t].table_off,
                                    &pd->tiles[t]);
            if (brc != TC_OK) { break; }
        }
        if (brc != TC_OK) { break; }
        pd->dst = planes_out[p];
        pd->stride = strides == NULL || strides[p] == 0u
                   ? view.fh.plane_visible_w[p] : strides[p];
        pd->vis_w = view.fh.plane_visible_w[p];
        pd->vis_h = view.fh.plane_visible_h[p];
        pd->qms = lookup_qm(view.fh.qmatrix_id);
        if (pd->qms == NULL) {
            tc_set_error(TC_ERR_UNSUPPORTED_MATRIX, "qmatrix_id %u",
                         (unsigned)view.fh.qmatrix_id);
            brc = TC_ERR_UNSUPPORTED_MATRIX;
            break;
        }
        pd->qp_base = (int32_t)view.fh.qp_base;
        pd->bd = view.fh.bit_depth;
        pd->dinv = tc_simd_resolve_dequant_inverse(
            (uint8_t)(pd->bd > 12u ? 1u : 0u));
        pd->tile_count = T;
        /* 段 qp 去重 → 预建量化 ctx（顺序；并行任务只读） */
        {
            const uint8_t* dirp = data + view.plane_dir_off[p];
            uint8_t seen[256];
            memset(seen, 0, sizeof(seen));
            uint32_t n = 0u;
            for (uint32_t s = 0u; s < view.segs_per_plane[p]; ++s) {
                const uint8_t biased = dirp[(size_t)s * TC_V8_DIR_ENTRY_BYTES + 8];
                n += seen[biased] == 0u ? 1u : 0u;
                seen[biased] = 1u;
            }
            pd->qcache_n = n;
            pd->qcache = (tc_quant_ctx*)tc_alloc((size_t)n * sizeof(tc_quant_ctx));
            pd->qcache_biased = (uint8_t*)tc_alloc(n != 0u ? n : 1u);
            if (pd->qcache == NULL || pd->qcache_biased == NULL) { brc = TC_ERR_OUT_OF_MEMORY; break; }
            const uint16_t* qm = (p == 0u) ? pd->qms->luma : pd->qms->chroma;
            uint32_t wq = 0u;
            for (uint32_t bv = 0u; bv < 256u; ++bv) {
                if (seen[bv] == 0u) { continue; }
                int32_t qe = (int32_t)view.fh.qp_base + (int32_t)bv - 64;
                if (qe < 0) { qe = 0; }
                const int32_t ceiling = (int32_t)tc_qp_eff_ceiling((uint32_t)view.fh.qp_base);
                if (qe > ceiling) { qe = ceiling; }
                tc_quant_ctx_init_bd_tbl(&pd->qcache[wq], qm, (uint32_t)qe, view.fh.bit_depth,
                                         tc_qtbl_of_flags(view.fh.flags));
                pd->qcache_biased[wq] = (uint8_t)bv;
                wq++;
            }
        }
        total_tiles += T;
    }
    if (brc != TC_OK) {
        for (uint32_t p = 0u; p < plane_count; ++p) {
            tc_free(pls[p].tiles);
            tc_free(pls[p].tile_hash);
            tc_free(pls[p].qcache);
            tc_free(pls[p].qcache_biased);
        }
        tc_free(pls);
        return brc;
    }

    /* 任务 1：瓦片 CRC 并行重算（每瓦片一次） */
    {
        v8_crc_job* cj = (v8_crc_job*)tc_alloc((size_t)total_tiles * sizeof(v8_crc_job));
        tc_job* jb = (tc_job*)tc_alloc((size_t)total_tiles * sizeof(tc_job));
        if (cj == NULL || jb == NULL) {
            tc_free(cj); tc_free(jb);
            brc = TC_ERR_OUT_OF_MEMORY;
        } else {
            for (uint32_t p = 0u; p < plane_count; ++p) {
                for (uint32_t t = 0u; t < pls[p].tile_count; ++t) {
                    const uint32_t g = pls[p].tile_base + t;
                    cj[g].pd = &pls[p];
                    cj[g].tile = t;
                    jb[g].fn = v8_crc_job_fn;
                    jb[g].ctx = &cj[g];
                }
            }
            tc_parallel_for(jb, total_tiles, (uint32_t)tc_dev_thread_count());
        }
        tc_free(cj);
        tc_free(jb);
    }

    /* 任务 2：段块并行解码（CHUNK 段/任务）+ 汇总 */
    uint16_t concealed = 0u;
    uint64_t frame_hash = 0u;
    const uint32_t CHUNK = 64u;
    uint32_t total_chunks = 0u;
    for (uint32_t p = 0u; p < plane_count; ++p) {
        for (uint32_t t = 0u; t < pls[p].tile_count; ++t) {
            const topos_v8_tile_info* ti = &view.tiles[pls[p].tile_base + t];
            const uint32_t n = ti->seg_count != 0u
                ? (ti->seg_count + CHUNK - 1u) / CHUNK : 1u;
            total_chunks += n;
        }
    }
    v8_chunk_job* cj = (v8_chunk_job*)tc_alloc((size_t)total_chunks * sizeof(v8_chunk_job));
    tc_job* jb = (tc_job*)tc_alloc((size_t)total_chunks * sizeof(tc_job));
    if (cj == NULL || jb == NULL) {
        tc_free(cj); tc_free(jb);
        brc = TC_ERR_OUT_OF_MEMORY;
    } else {
        uint32_t g = 0u;
        for (uint32_t p = 0u; p < plane_count; ++p) {
            for (uint32_t t = 0u; t < pls[p].tile_count; ++t) {
                const topos_v8_tile_info* ti = &view.tiles[pls[p].tile_base + t];
                const uint32_t n = ti->seg_count != 0u
                    ? (ti->seg_count + CHUNK - 1u) / CHUNK : 1u;
                for (uint32_t c = 0u; c < n; ++c) {
                    cj[g].pd = &pls[p];
                    cj[g].tile = t;
                    cj[g].seg_first = ti->seg_first + c * CHUNK;
                    const uint32_t rem = ti->seg_count - c * CHUNK;
                    cj[g].seg_count = rem < CHUNK ? rem : CHUNK;
                    cj[g].concealed = 0u;
                    cj[g].sh_sum = 0u;
                    jb[g].fn = v8_chunk_job_fn;
                    jb[g].ctx = &cj[g];
                    g++;
                }
            }
        }
        tc_parallel_for(jb, total_chunks, (uint32_t)tc_dev_thread_count());
        g = 0u;
        for (uint32_t p = 0u; p < plane_count; ++p) {
            uint64_t ph = 0u;
            for (uint32_t t = 0u; t < pls[p].tile_count; ++t) {
                const topos_v8_tile_info* ti = &view.tiles[pls[p].tile_base + t];
                const uint32_t n = ti->seg_count != 0u
                    ? (ti->seg_count + CHUNK - 1u) / CHUNK : 1u;
                uint64_t th = 0u;
                uint32_t tile_concealed = 0u;
                for (uint32_t c = 0u; c < n; ++c) {
                    th += cj[g].sh_sum;
                    tile_concealed += cj[g].concealed;
                    g++;
                }
                const uint8_t st = (tile_concealed != 0u || ti->crc_ok == 0u)
                    ? TC_FRAME_SLICE_CONCEALED : TC_FRAME_SLICE_OK;
                out_info->slice_status[pls[p].tile_base + t] = st;
                if (st != TC_FRAME_SLICE_OK) { concealed++; }
                ph += th;
            }
            frame_hash += ph;
        }
        atomic_store(&g_v8_dec_qhash, frame_hash);
        out_info->concealed_slices = concealed;
        tc_free(cj);
        tc_free(jb);
    }
    for (uint32_t p = 0u; p < plane_count; ++p) {
        tc_free(pls[p].tiles);
        tc_free(pls[p].tile_hash);
        tc_free(pls[p].qcache);
        tc_free(pls[p].qcache_biased);
    }
    tc_free(pls);
    return brc;
}

static void enc_band_job(void* vctx)
{
    enc_band_task* t = (enc_band_task*)vctx;
    /* 解码深化：动态领取分发下同 (i%nw) 任务可能并发——槽位按执行线程绑定
     * （槽内容为 scratch：bw 每任务 reset、dc_a/dc_b 每 band memset、tok
     * 每 band 重写，输出与槽位身份无关 → 编码确定性不受分发顺序影响）。
     * worker id ≤ 7 < TC_SLICE_MAX_THREADS 恒在数组界内；嵌套顺序执行时
     * 单线程逐任务，任意槽位皆安全（钳回 0）。 */
    uint32_t wslot = tc_pool_worker_slot();
    if (wslot >= t->nw) { wslot = 0u; }
    t->slot = &t->slots_base[wslot];
    t->slot_idx = wslot;
    enc_worker_slot* s = t->slot;
    tc_bitwriter* bw = &s->bw;
    const int prof = tc_profile_enabled();

    if (t->fh->version_major == 7u && t->fh->entropy_mode == 5u && is_alpha_plane(t->fh, t->plane)) {
        t->rc = TC_ERR_NOT_IMPLEMENTED;
        tc_set_error(TC_ERR_NOT_IMPLEMENTED, "v7a alpha encode is not implemented");
        return;
    }
    if (t->fh->version_major == 7u && t->fh->entropy_mode == 5u && s->tok_ok == 0) {
        t->rc = TC_ERR_OUT_OF_MEMORY;
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "v7a token storage unavailable");
        return;
    }
    if (t->fh->version_major == 7u && t->fh->entropy_mode == 6u && s->tok_ok == 0) {
        /* rANS 同 V2-VLC：符号表来自 token 遍——不得静默降级 */
        t->rc = TC_ERR_OUT_OF_MEMORY;
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "rans token storage unavailable");
        return;
    }
    if (t->fh->version_major == 7u &&
        (t->fh->entropy_mode == 7u || t->fh->entropy_mode == 8u) &&
        s->tok_ok == 0) {
        /* V7-R2/R3 同 V7-R：token 遍是唯一符号源 */
        t->rc = TC_ERR_OUT_OF_MEMORY;
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "rans token storage unavailable");
        return;
    }

    topos_slice_header sh;
    memset(&sh, 0, sizeof(sh));
    sh.plane = (uint8_t)t->plane;
    sh.block_y0 = (uint16_t)t->y0;
    sh.block_h = (uint16_t)t->h;

    tc_bitwriter_reset(bw);
    int32_t rc;
    uint64_t t0 = prof ? tc_profile_now_ns() : 0u;
    if (is_alpha_plane(t->fh, t->plane)) {
        uint32_t k1 = 0u, k2 = 0u;
        fill_alpha_residuals(t->fh, t->coded, s->rbuf, t->y0, t->h, &k1, &k2);
        sh.k1 = (uint8_t)k1;
        sh.k2 = (uint8_t)k2;
        sh.k3 = 0u;
        sh.qp_delta_biased = 64u; /* alpha 无量化；qp 字段无效但须通过域校验 */
        size_t pixels = (size_t)t->fh->plane_coded_w[3] * ((size_t)t->h * 8u);
        if (prof) { t->fill_ns = tc_profile_now_ns() - t0; t0 = tc_profile_now_ns(); }
        rc = tc_alpha_slice_encode(t->fh, &sh, s->rbuf, pixels, bw);
    } else {
        uint32_t k1 = 0u, k2 = 0u, k3 = 0u;
        if (t->fh->version_major == 3u || t->fh->version_major == 6u) {
            sh.qp_delta_biased = (uint8_t)(64 + t->qp_eff - (int32_t)t->fh->qp_base);
            rc = tc_intra_slice_encode(t->fh, &sh, t->coded, t->qctx, bw);
            if (prof) {
                t->fill_ns = tc_profile_now_ns() - t0;
                t0 = tc_profile_now_ns();
                t->blocks = t->h * t->fh->plane_block_cols[t->plane];
            }
            goto emit_done;
        }
        if (s->tok_ok != 0 &&
            !(t->fh->version_major == 4u && t->fh->entropy_mode == 2u) &&
            !(t->fh->version_major == 5u && t->fh->entropy_mode == 3u) &&
            t->fh->version_major != 6u) {
            /* M6b token 路径：量化遍产 token（含 k 统计），emit 遍只走 token；
             * M7：fsrc 非 NULL 时量化源换 F-cache（无采样加载/DCT） */
            uint8_t vlc = (uint8_t)(t->fh->version_major == 2u
                                    && t->fh->entropy_mode == 1u);
            const uint8_t rans = (uint8_t)(t->fh->version_major == 7u
                                           && t->fh->entropy_mode == 6u);
            const uint8_t rans2 = (uint8_t)(t->fh->version_major == 7u
                                            && (t->fh->entropy_mode == 7u ||
                                                t->fh->entropy_mode == 8u));
            uint32_t dc_hist_vlc[TC_VLC_DC_SYMS];
            uint32_t run_hist_vlc[TC_VLC_RUN_SYMS];
            uint32_t lvl_hist_vlc[TC_VLC_LVL_SYMS];
            if (vlc != 0u || rans != 0u || rans2 != 0u) {
                memset(dc_hist_vlc, 0, sizeof(dc_hist_vlc));
                memset(run_hist_vlc, 0, sizeof(run_hist_vlc));
                memset(lvl_hist_vlc, 0, sizeof(lvl_hist_vlc));
            }
            uint32_t* dc_hist_p = (vlc != 0u || rans != 0u || rans2 != 0u) ? dc_hist_vlc : NULL;
            uint32_t* run_hist_p = (vlc != 0u || rans != 0u || rans2 != 0u) ? run_hist_vlc : NULL;
            uint32_t* lvl_hist_p = (vlc != 0u || rans != 0u || rans2 != 0u) ? lvl_hist_vlc : NULL;
            if (t->fsrc != NULL) {
                rc = fill_color_band_from_f(t->fh, t->fsrc, t->plane, t->qctx, &s->tok,
                                       s->dc_a, s->dc_b, t->y0, t->h, &k1, &k2, &k3,
                                       dc_hist_p, run_hist_p, lvl_hist_p,
                                       (uint32_t)t->qp_eff,
                                       (vlc != 0 && t->es->rdo_on != 0) ? 1 : 0);
            } else {
                rc = fill_color_band_tokens(t->fh, t->coded, t->plane, t->qctx, &s->tok,
                                       s->dc_a, s->dc_b, t->y0, t->h, &k1, &k2, &k3,
                                       dc_hist_p, run_hist_p, lvl_hist_p);
            }
            if (rc != TC_OK) { t->rc = rc; return; }
            size_t nblocks = (size_t)t->h * t->fh->plane_block_cols[t->plane];
            if (rans != 0u) {
                /* V7-R：k 无语义恒 0（slice_map 契约） */
                sh.k1 = 0u;
                sh.k2 = 0u;
                sh.k3 = 0u;
                sh.qp_delta_biased = (uint8_t)(64 + t->qp_eff - (int32_t)t->fh->qp_base);
                if (prof) {
                    t->fill_ns = tc_profile_now_ns() - t0;
                    t0 = tc_profile_now_ns();
                    t->blocks = t->h * t->fh->plane_block_cols[t->plane];
                }
                rc = emit_color_tokens_rans(&s->tok, nblocks, dc_hist_vlc,
                                            run_hist_vlc, lvl_hist_vlc, bw,
                                            t->es, t->slot_idx);
                if (rc != TC_OK) { t->rc = rc; return; }
                goto emit_done;
            }
            if (rans2 != 0u) {
                /* V7-R2（ADR-C036）：同 V7-R 契约（k 恒 0）；emit 内部
                 * per-slice 选条件模型并信令 */
                sh.k1 = 0u;
                sh.k2 = 0u;
                sh.k3 = 0u;
                sh.qp_delta_biased = (uint8_t)(64 + t->qp_eff - (int32_t)t->fh->qp_base);
                if (prof) {
                    t->fill_ns = tc_profile_now_ns() - t0;
                    t0 = tc_profile_now_ns();
                    t->blocks = t->h * t->fh->plane_block_cols[t->plane];
                }
                if (s_dev_tok_on != 0) {
                    dev_tok_capture_band(t->plane, t->y0, t->h,
                                         t->fh->plane_block_cols[t->plane],
                                         t->es->slice_rows, &s->tok, t->qctx);
                }
                rc = emit_color_tokens_rans2(&s->tok, nblocks, dc_hist_vlc,
                                             run_hist_vlc, lvl_hist_vlc, bw,
                                             t->es, t->slot_idx);
                if (rc != TC_OK) { t->rc = rc; return; }
                goto emit_done;
            }
            if (t->fh->version_major == 7u && t->fh->entropy_mode == 5u) {
                if (prof) {
                    t->fill_ns = tc_profile_now_ns() - t0;
                    t0 = tc_profile_now_ns();
                    t->blocks = t->h * t->fh->plane_block_cols[t->plane];
                }
                t->rc = enc_emit_v7_segments(t, s, k1, k2);
                if (t->rc == TC_OK && prof) {
                    t->entropy_ns = tc_profile_now_ns() - t0;
                }
                return;
            }
            if (vlc != 0u) {
                /* M9 VLC：选表 + token emit；书 id 写入 slice k1/k2/k3 位置 */
                uint8_t dc_bk = 0u, lvl_bk = 0u, run_bk = 0u;
                vlc_books_from_hist(dc_hist_vlc, run_hist_vlc, lvl_hist_vlc,
                                    &dc_bk, &lvl_bk, &run_bk);
                sh.k1 = dc_bk;
                sh.k2 = lvl_bk;
                sh.k3 = run_bk;
                sh.qp_delta_biased = (uint8_t)(64 + t->qp_eff - (int32_t)t->fh->qp_base);
                if (prof) {
                    t->fill_ns = tc_profile_now_ns() - t0;
                    t0 = tc_profile_now_ns();
                    t->blocks = t->h * t->fh->plane_block_cols[t->plane];
                }
                rc = emit_color_tokens_vlc(&s->tok, nblocks, dc_bk, lvl_bk, run_bk, bw);
                if (rc != TC_OK) { t->rc = rc; return; }
                goto emit_done;
            }
            sh.k1 = (uint8_t)k1;
            sh.k2 = (uint8_t)k2;
            sh.k3 = (uint8_t)k3;
            sh.qp_delta_biased = (uint8_t)(64 + t->qp_eff - (int32_t)t->fh->qp_base);
            if (prof) {
                t->fill_ns = tc_profile_now_ns() - t0;
                t0 = tc_profile_now_ns();
                t->blocks = t->h * t->fh->plane_block_cols[t->plane];
            }
            rc = emit_color_tokens(&s->tok, nblocks, k1, k2, k3, bw);
        } else {
            if ((t->fh->version_major == 2u && t->fh->entropy_mode == 1u) ||
                (t->fh->version_major == 7u && t->fh->entropy_mode == 6u) ||
                (t->fh->version_major == 7u &&
                 (t->fh->entropy_mode == 7u || t->fh->entropy_mode == 8u))) {
                /* VLC/rANS 需要 token 遍的符号 histogram（单一统计真相源）；
                 * qbuf 回退仅在 OOM 后出现——V2-VLC/V7-R/R2 流不得静默降级 */
                t->rc = TC_ERR_OUT_OF_MEMORY;
                return;
            }
            fill_color_band(t->fh, t->coded, t->plane, t->qctx, s->qbuf, s->dc_a, s->dc_b,
                            t->y0, t->h, &k1, &k2, &k3);
            if (t->fh->version_major == 5u && t->fh->entropy_mode == 3u) {
                /* C2 builds the pair table from qbuf; use the shortest
                 * frozen DC/LEVEL books for the remaining two families. */
                k1 = 0u; k2 = 0u; k3 = 0u;
            } else if (t->fh->version_major == 4u && t->fh->entropy_mode == 2u) {
                /* C1 uses the frozen VLC books; qbuf is only the coefficient
                 * staging path, so the Rice estimates are deliberately ignored. */
                k1 = 3u; k2 = 3u; k3 = 3u;
            }
            sh.k1 = (uint8_t)k1;
            sh.k2 = (uint8_t)k2;
            sh.k3 = (uint8_t)k3;
            sh.qp_delta_biased = (uint8_t)(64 + t->qp_eff - (int32_t)t->fh->qp_base);
            if (prof) {
                t->fill_ns = tc_profile_now_ns() - t0;
                t0 = tc_profile_now_ns();
                t->blocks = t->h * t->fh->plane_block_cols[t->plane];
            }
            rc = tc_color_slice_encode(t->fh, &sh, s->qbuf, bw);
        }
    }
emit_done:
    if (rc != TC_OK) { t->rc = rc; return; }
    rc = tc_bitwriter_flush_zero_pad(bw);
    if (rc != TC_OK) { t->rc = rc; return; }
    if (prof) { t->entropy_ns = tc_profile_now_ns() - t0; t0 = tc_profile_now_ns(); }

    size_t payload_size = tc_bitwriter_byte_size(bw);
    sh.slice_payload_size = (uint32_t)payload_size;
    sh.slice_crc32 = tc_crc32(tc_bitwriter_data(bw), payload_size);
    if (prof) { t->crc_ns = tc_profile_now_ns() - t0; t0 = tc_profile_now_ns(); }

    t->rc = tc_slice_header_encode(&sh, t->hdr);
    if (t->rc != TC_OK) { return; }
    /* M6c：payload 落 slot arena（bump；同槽位 bitwriter 会被后续 band 复用
     * 覆写，仍需一份自有拷贝，但不再每 slice malloc/free） */
    uint8_t* pbuf = enc_pay_alloc(t->es, t->slot_idx, payload_size);
    if (pbuf == NULL) { t->rc = TC_ERR_OUT_OF_MEMORY; return; }
    memcpy(pbuf, tc_bitwriter_data(bw), payload_size);
    if (prof) { t->copy_ns = tc_profile_now_ns() - t0; }
    t->payload = pbuf;
    t->payload_size = (uint32_t)payload_size;
    t->is_alpha = is_alpha_plane(t->fh, t->plane) ? 1 : 0;
}

static int32_t enc_encode_plane_opt(enc_shared* e, uint32_t p, int use_fcache,
                                    int do_pay_reset)
{
    const topos_frame_header* fh = &e->fh;
    const int prof = tc_profile_enabled();
    uint64_t tp = prof ? tc_profile_now_ns() : 0u;
    int32_t rc = TC_OK;
    if (use_fcache == 0) {
        rc = enc_prepare_plane(e, p);
        if (prof) { TC_STATS_LOCK(); g_enc_stats.pad_ns += tc_profile_now_ns() - tp; TC_STATS_UNLOCK(); }
        if (rc != TC_OK) { return rc; }
    }

    const tc_qmatrix_set* qms = lookup_qm(fh->qmatrix_id);
    if (qms == NULL) {
        tc_set_error(TC_ERR_UNSUPPORTED_MATRIX, "qmatrix_id %u", (unsigned)fh->qmatrix_id);
        return TC_ERR_UNSUPPORTED_MATRIX;
    }
    const uint16_t* qm = (p == 0u) ? qms->luma : qms->chroma;

    uint32_t rows = fh->plane_block_rows[p];
    tc_quant_ctx qctx;
    int32_t qp_eff = 0;
    if (p != 3u) {
        /* 每 plane 有效 qp：qp_base + delta + 位深偏移，钳位 0..63（§4.3
         * 解码不变量；偏移入流后解码侧无需位深知识）；
         * Q/dz/fastdiv 每 plane 建表一次（阶段 9） */
        int32_t delta = (p == 0u) ? e->qp_delta_luma : e->qp_delta_chroma;
        qp_eff = (int32_t)fh->qp_base + delta + bd_qp_offset(fh->bit_depth);
        if (qp_eff < 0) { qp_eff = 0; }
        {   /* v1.5：qp_base≤63 沿用 63 顶格（字节兼容），≥64 放开到 95 */
            const int32_t ceiling = (int32_t)tc_qp_eff_ceiling(fh->qp_base);
            if (qp_eff > ceiling) { qp_eff = ceiling; }
        }
        tc_quant_ctx_init_bd_tbl(&qctx, qm, (uint32_t)qp_eff, fh->bit_depth,
                                 tc_qtbl_of_flags(fh->flags));
    }

    /* band 划分与顺序版一致：y0 += slice_rows */
    uint32_t bands = (rows + e->slice_rows - 1u) / e->slice_rows;
    if (bands == 0u) { bands = 1u; }

    /* worker 槽位（R6 常驻）：slot0 复用 enc_shared 缓冲本体；1..nw−1 借用
     * 常驻槽位缓冲（enc_slots_reserve 预备，grow-only 跨迭代/帧复用）；
     * 条带 i%nw 绑定使同槽任务串行（不变量与阶段 9 相同） */
    uint32_t nw_alloc = (uint32_t)tc_dev_thread_count();
    if (nw_alloc > bands) { nw_alloc = bands; }
    if (nw_alloc > (uint32_t)TC_SLICE_MAX_THREADS) { nw_alloc = (uint32_t)TC_SLICE_MAX_THREADS; }
    if (nw_alloc > e->slot_n) { nw_alloc = e->slot_n; } /* OOM 退化：用已就绪槽位 */
    /* N03：tok 数组按 tok_slots_built 收敛（tok_ok=0 时任务走 qbuf 回退、
     * 不触碰 tok 数组，槽位数不受此限）。slot_n 与 tok_slots_built 只在
     * 部分失败路径可分叉；成功路径两者均 ≥ want。 */
    if (e->tok_ok != 0 && nw_alloc > (uint32_t)e->tok_slots_built) {
        nw_alloc = (uint32_t)e->tok_slots_built;
    }

    enc_worker_slot slots[TC_SLICE_MAX_THREADS];
    memset(slots, 0, sizeof(slots));
    for (uint32_t w = 0u; w < nw_alloc; ++w) {
        if (w == 0u) {
            slots[0].qbuf = e->qbuf;
            slots[0].dc_a = e->dc_a;
            slots[0].dc_b = e->dc_b;
            slots[0].rbuf = e->rbuf;
            slots[0].bw = e->bw;
            slots[0].bw_ok = e->bw_ok;
        } else {
            slots[w].qbuf = e->slot_qbuf[w];
            slots[w].dc_a = e->slot_dca[w];
            slots[w].dc_b = e->slot_dcb[w];
            slots[w].rbuf = e->slot_rbuf[w];
            slots[w].bw = e->slot_bw[w];
            slots[w].bw_ok = e->slot_bw_ok[w];
        }
        /* M6b：token 数组指针仅借用（worker 不扩容）；tok_ok 全局一致 */
        slots[w].tok = e->tok[w];
        slots[w].tok_ok = e->tok_ok;
    }
    uint32_t nw = nw_alloc;

    /* 任务数组（槽位 job 入口按执行线程绑定） */
    if (do_pay_reset != 0) {
        enc_pay_reset(e); /* M6c：上一 plane 的 payload 已在组装中消费，逻辑复位 */
    }
    const int32_t* fsrc = use_fcache != 0
        ? e->fcache + e->fcache_off[p] * 64u : (const int32_t*)NULL;
    enc_band_task tasks[TC_MAX_SLICES];
    tc_job jobs[TC_MAX_SLICES];
    uint32_t ti = 0u;
    for (uint32_t y0 = 0u; y0 < rows && ti < bands; y0 += e->slice_rows, ++ti) {
        uint32_t h = rows - y0;
        if (h > e->slice_rows) { h = e->slice_rows; }
        enc_band_task* t = &tasks[ti];
        memset(t, 0, sizeof(*t));
        t->fh = fh;
        /* M11-2a：对齐直通平面读调用方输入（只读；cast 去 const 安全：
         * 颜色 band 路径不写平面，mode2 alpha 恒走拷贝路径） */
        t->coded = e->plane_direct[p] != 0
            ? (uint16_t*)(uintptr_t)e->plane_src[p]
            : e->coded + e->coded_off[p];
        t->plane = p;
        t->y0 = y0;
        t->h = h;
        t->qctx = &qctx;
        t->qp_eff = qp_eff;
        t->slots_base = slots;
        t->nw = nw;
        t->slot = NULL; /* job 入口按执行线程绑定 */
        t->es = e;
        t->slot_idx = 0u;
        t->fsrc = fsrc;
        t->rc = TC_OK;
        jobs[ti].fn = enc_band_job;
        jobs[ti].ctx = t;
    }
    (void)tc_parallel_for(jobs, ti, nw);

    /* M6 观测：任务局部阶段耗时求和（join 后无竞争） */
    if (prof) {
        TC_STATS_LOCK();
        for (uint32_t i = 0u; i < ti; ++i) {
            g_enc_stats.fill_ns += tasks[i].fill_ns;
            g_enc_stats.entropy_ns += tasks[i].entropy_ns;
            g_enc_stats.crc_ns += tasks[i].crc_ns;
            g_enc_stats.copy_ns += tasks[i].copy_ns;
            g_enc_stats.blocks += tasks[i].blocks;
        }
        g_enc_stats.slices += ti;
        TC_STATS_UNLOCK();
    }

    /* 顺序组装（确定性：band 升序；统计折叠与顺序版逐字节一致） */
    rc = TC_OK;
    uint64_t ta = prof ? tc_profile_now_ns() : 0u;
    if (p == 3u && e->m7_alpha_capture != 0 && ti <= TC_MAX_SLICES) {
        /* M7：alpha 一次编码——捕获 hdr/payload 供 final 组装复用（不进 ab） */
        for (uint32_t i = 0u; i < ti; ++i) {
            enc_band_task* t = &tasks[i];
            if (t->rc != TC_OK) { rc = t->rc; break; }
            memcpy(e->alpha_bands[i].hdr, t->hdr, sizeof(t->hdr));
            e->alpha_bands[i].payload = t->payload;
            e->alpha_bands[i].payload_size = t->payload_size;
            e->alpha_payload += t->payload_size;
            e->alpha_hdr_bytes += TC_SLICE_HEADER_SIZE;
        }
        e->alpha_band_n = ti;
    } else {
        for (uint32_t i = 0u; i < ti; ++i) {
            enc_band_task* t = &tasks[i];
            if (t->rc != TC_OK) { rc = t->rc; break; }
            asm_append(&e->ab, t->hdr, sizeof(t->hdr));
            asm_append(&e->ab, t->payload, t->payload_size);
            if (e->ab.err == TC_ERR_LIMIT_EXCEEDED) { rc = e->ab.err; break; }
            if (t->is_alpha != 0) {
                e->alpha_payload += t->payload_size;
                e->alpha_hdr_bytes += TC_SLICE_HEADER_SIZE;
            } else {
                e->color_payload += t->payload_size;
                e->color_hdr_bytes += TC_SLICE_HEADER_SIZE;
            }
        }
    }
    if (prof) { TC_STATS_LOCK(); g_enc_stats.asm_ns += tc_profile_now_ns() - ta; TC_STATS_UNLOCK(); }
    /* M6c：payload 归 slot arena（enc_pay_reset 复位），无逐任务 free */

    /* R6：槽位缓冲常驻 enc_shared——不再逐 plane 释放。bitwriter 为结构体
     * 拷贝借出：worker 内部扩容只更新局部副本的缓冲指针，必须回写全部
     * 槽位（漏写会遗留 realloc 迁走后的悬垂指针；小帧初容量够用不触发）。 */
    for (uint32_t w = 0u; w < nw_alloc; ++w) {
        if (w == 0u) {
            e->bw = slots[0].bw;
            e->bw_ok = slots[0].bw_ok;
        } else {
            e->slot_bw[w] = slots[w].bw;
            e->slot_bw_ok[w] = slots[w].bw_ok;
        }
    }
    return rc;
}

/* M10-6.2：全 plane 统一提交——消除按 plane 串行三轮 parallel_for（取证：
 * 1080p sr16 每轮仅 ~9 任务喂 8 worker，3 轮尾部失衡 + join 使单遍有效并行
 * 仅 2.0~2.2x）。流程：pad（串行，use_fcache=1 时跳过）→ 一次 parallel_for
 * 提交 plane 0..plane_end-1 全部 band → 逐 plane 顺序组装。任务间无依赖
 * （band 独立、coded 各 plane 偏移共存），提交顺序展开 = 逐 plane 版拼接
 * → 组装字节序不变 → 包逐字节一致（1/2/4/8 线程 parity 门保持）。 */
static int32_t enc_band_pass_all(enc_shared* e, uint32_t plane_end, int use_fcache,
                                 int do_pay_reset)
{
    const topos_frame_header* fh = &e->fh;
    const int prof = tc_profile_enabled();
    int32_t rc = TC_OK;

    if (use_fcache == 0) {
        for (uint32_t p = 0u; p < plane_end; ++p) {
            uint64_t tp = prof ? tc_profile_now_ns() : 0u;
            rc = enc_prepare_plane(e, p);
            if (prof) { TC_STATS_LOCK(); g_enc_stats.pad_ns += tc_profile_now_ns() - tp; TC_STATS_UNLOCK(); }
            if (rc != TC_OK) { return rc; }
        }
    }

    const tc_qmatrix_set* qms = lookup_qm(fh->qmatrix_id);
    if (qms == NULL) {
        tc_set_error(TC_ERR_UNSUPPORTED_MATRIX, "qmatrix_id %u", (unsigned)fh->qmatrix_id);
        return TC_ERR_UNSUPPORTED_MATRIX;
    }
    tc_quant_ctx qctx[4];
    int32_t qp_eff[4];
    uint32_t max_bands = 1u;
    for (uint32_t p = 0u; p < plane_end; ++p) {
        qp_eff[p] = 0;
        if (!is_alpha_plane(fh, p)) {
            /* pf=3（TRAW）：无色度语义——全平面 luma qm + luma delta */
            const uint16_t* qm = is_chroma_plane(fh, p) ? qms->chroma : qms->luma;
            const int32_t delta = is_chroma_plane(fh, p) ? e->qp_delta_chroma
                                                         : e->qp_delta_luma;
            int32_t qe = (int32_t)fh->qp_base + delta + bd_qp_offset(fh->bit_depth);
            if (qe < 0) { qe = 0; }
            {   /* v1.5：qp_base≤63 顶格 63（字节兼容），≥64 放开到 95 */
                const int32_t ceiling = (int32_t)tc_qp_eff_ceiling(fh->qp_base);
                if (qe > ceiling) { qe = ceiling; }
            }
            qp_eff[p] = qe;
            tc_quant_ctx_init_bd_tbl(&qctx[p], qm, (uint32_t)qe, fh->bit_depth,
                                 tc_qtbl_of_flags(fh->flags));
        }
        uint32_t rows = fh->plane_block_rows[p];
        uint32_t bands = (rows + e->slice_rows - 1u) / e->slice_rows;
        if (bands == 0u) { bands = 1u; }
        if (bands > max_bands) { max_bands = bands; }
    }

    uint32_t nw_alloc = (uint32_t)tc_dev_thread_count();
    if (nw_alloc > max_bands) { nw_alloc = max_bands; }
    if (nw_alloc > (uint32_t)TC_SLICE_MAX_THREADS) { nw_alloc = (uint32_t)TC_SLICE_MAX_THREADS; }
    if (nw_alloc > e->slot_n) { nw_alloc = e->slot_n; } /* OOM 退化：用已就绪槽位 */
    /* N03：tok 数组按 tok_slots_built 收敛（tok_ok=0 时任务走 qbuf 回退、
     * 不触碰 tok 数组，槽位数不受此限）。slot_n 与 tok_slots_built 只在
     * 部分失败路径可分叉；成功路径两者均 ≥ want。 */
    if (e->tok_ok != 0 && nw_alloc > (uint32_t)e->tok_slots_built) {
        nw_alloc = (uint32_t)e->tok_slots_built;
    }
    if (nw_alloc < 1u) { nw_alloc = 1u; }

    enc_worker_slot slots[TC_SLICE_MAX_THREADS];
    memset(slots, 0, sizeof(slots));
    for (uint32_t w = 0u; w < nw_alloc; ++w) {
        if (w == 0u) {
            slots[0].qbuf = e->qbuf;
            slots[0].dc_a = e->dc_a;
            slots[0].dc_b = e->dc_b;
            slots[0].rbuf = e->rbuf;
            slots[0].bw = e->bw;
            slots[0].bw_ok = e->bw_ok;
        } else {
            slots[w].qbuf = e->slot_qbuf[w];
            slots[w].dc_a = e->slot_dca[w];
            slots[w].dc_b = e->slot_dcb[w];
            slots[w].rbuf = e->slot_rbuf[w];
            slots[w].bw = e->slot_bw[w];
            slots[w].bw_ok = e->slot_bw_ok[w];
        }
        slots[w].tok = e->tok[w];
        slots[w].tok_ok = e->tok_ok;
    }
    uint32_t nw = nw_alloc;

    if (do_pay_reset != 0) {
        enc_pay_reset(e); /* 每 pass 一次（原逐 plane 复位；峰值驻留=整帧 payload） */
    }

    /* V2.x AQ + P3 亮度空间 AQ：每 plane 9 档量化变体（偏移 −4..+4，
     * clamp 后仅 9 种；色度偏移计算仍钳 ±2，高档位仅亮度使用）；
     * 任务只持指针（逐任务内嵌 qctx ≈2.3KB ×512 会爆栈——实测 Bus error） */
    tc_quant_ctx aq_v[4][9];
    int aq_v_ok = 0;
    if (e->aq_on != 0) {
        for (uint32_t p = 0u; p < plane_end && !is_alpha_plane(fh, p); ++p) {
            const uint16_t* qmp = is_chroma_plane(fh, p) ? qms->chroma : qms->luma;
            for (int d = 0; d < 9; ++d) {
                int32_t qev = qp_eff[p] + (d - 4);
                if (qev < 0) { qev = 0; }
                /* v1.5（ADR-C031）：AQ 变体域随 qp_base 条件天花板——
                 * 硬编码 63 在 qp_base≥64 时编码量化与码流声明的 qp_eff
                 * 失配（编码 63 / 解码 77+），重建崩坏（2026-09-10 实测
                 * proxy@qp79 色度 10 dB）。与下方偏移应用点同源钳位。 */
                if ((uint32_t)qev > tc_qp_eff_ceiling(e->fh.qp_base)) {
                    qev = (int32_t)tc_qp_eff_ceiling(e->fh.qp_base);
                }
                tc_quant_ctx_init_bd_tbl(&aq_v[p][d], qmp, (uint32_t)qev, e->fh.bit_depth,
                                         tc_qtbl_of_flags(e->fh.flags));
            }
        }
        aq_v_ok = 1;
    }

    enc_band_task tasks[TC_MAX_SLICES];
    tc_job jobs[TC_MAX_SLICES];
    uint32_t plane_band0[5]; /* 组装分组：plane p 的任务区间 [band0[p], band0[p+1]) */
    uint32_t ti = 0u;
    for (uint32_t p = 0u; p < plane_end; ++p) {
        plane_band0[p] = ti;
        uint32_t rows = fh->plane_block_rows[p];
        uint32_t bands = (rows + e->slice_rows - 1u) / e->slice_rows;
        if (bands == 0u) { bands = 1u; }
        const int32_t* fsrc = (use_fcache != 0 && !is_alpha_plane(fh, p))
            ? e->fcache + e->fcache_off[p] * 64u : (const int32_t*)NULL;
        for (uint32_t y0 = 0u; y0 < rows && ti < TC_MAX_SLICES;
             y0 += e->slice_rows, ++ti) {
            uint32_t h = rows - y0;
            if (h > e->slice_rows) { h = e->slice_rows; }
            enc_band_task* t = &tasks[ti];
            memset(t, 0, sizeof(*t));
            t->fh = fh;
            /* M11-2a：对齐直通平面读调用方输入（只读；cast 去 const 安全：
             * 颜色 band 路径不写平面，mode2 alpha 恒走拷贝路径） */
            t->coded = e->plane_direct[p] != 0
                ? (uint16_t*)(uintptr_t)e->plane_src[p]
                : e->coded + e->coded_off[p];
            t->plane = p;
            t->y0 = y0;
            t->h = h;
            t->qp_eff = qp_eff[p];
            t->qctx = &qctx[p];
            if (aq_v_ok != 0 && !is_alpha_plane(fh, p)) {
                /* V2.x AQ：本带偏移（qp 无关，m7_prepare 算好） */
                int32_t aq_off_v = e->aq_off[p][y0 / e->slice_rows];
                if (aq_off_v != 0) {
                    int d = aq_off_v + 4;
                    if (d < 0) { d = 0; }
                    if (d > 8) { d = 8; }
                    t->qctx = &aq_v[p][d];
                    t->qp_eff = qp_eff[p] + aq_off_v;
                    if (t->qp_eff < 0) { t->qp_eff = 0; }
                    if ((uint32_t)t->qp_eff > tc_qp_eff_ceiling(e->fh.qp_base)) {
                        t->qp_eff = (int32_t)tc_qp_eff_ceiling(e->fh.qp_base);
                    }
                }
            }
            t->slots_base = slots;
            t->nw = nw;
            t->slot = NULL; /* job 入口按执行线程绑定 */
            t->es = e;
            t->slot_idx = 0u;
            t->fsrc = fsrc;
            t->rc = TC_OK;
            jobs[ti].fn = enc_band_job;
            jobs[ti].ctx = t;
        }
    }
    plane_band0[plane_end] = ti;
    (void)tc_parallel_for(jobs, ti, nw);

    if (prof) {
        TC_STATS_LOCK();
        for (uint32_t i = 0u; i < ti; ++i) {
            g_enc_stats.fill_ns += tasks[i].fill_ns;
            g_enc_stats.entropy_ns += tasks[i].entropy_ns;
            g_enc_stats.crc_ns += tasks[i].crc_ns;
            g_enc_stats.copy_ns += tasks[i].copy_ns;
            g_enc_stats.blocks += tasks[i].blocks;
        }
        g_enc_stats.slices += ti;
        TC_STATS_UNLOCK();
    }

    if (fh->version_major == 7u && fh->entropy_mode == 5u) {
        /* V7-A has no legacy slice headers: the task order itself becomes the
         * canonical directory order, while each task contributes five band
         * payloads.  Keep these descriptor views on the caller stack; the
         * directory writer copies payloads directly into the final packet.
         * (V7-R rANS 走下方常规 slice 组装——按熵三元组区分) */
        if (plane_end != 3u || ti != (uint32_t)fh->slice_count) {
            rc = TC_ERR_NOT_IMPLEMENTED;
            tc_set_error(TC_ERR_NOT_IMPLEMENTED,
                         "v7a encoder currently supports color-only frames");
        } else {
            tc_v7_slice_geometry slices[TC_MAX_SLICES];
            tc_v7_segment_input segments[TC_MAX_SLICES * TC_V7_BAND_COUNT];
            memset(slices, 0, sizeof(slices));
            memset(segments, 0, sizeof(segments));
            uint64_t color_payload = 0u;
            for (uint32_t i = 0u; i < ti && rc == TC_OK; ++i) {
                enc_band_task* t = &tasks[i];
                if (t->rc != TC_OK) {
                    rc = t->rc;
                    break;
                }
                slices[i].block_y0 = t->y0;
                slices[i].block_h = (uint16_t)t->h;
                slices[i].plane_index = (uint8_t)t->plane;
                for (uint32_t band = 0u; band < TC_V7_BAND_COUNT; ++band) {
                    const uint32_t index = i * TC_V7_BAND_COUNT + band;
                    segments[index].data = t->v7_payload[band];
                    segments[index].size = t->v7_payload_size[band];
                    if (!tc_uadd_u64(color_payload,
                                     (uint64_t)t->v7_payload_size[band],
                                     &color_payload)) {
                        rc = TC_ERR_LIMIT_EXCEEDED;
                        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "v7a color payload size");
                        break;
                    }
                }
            }
            size_t needed = 0u;
            if (rc == TC_OK) {
                rc = tc_v7_directory_write(fh, slices, segments, NULL, 0u, &needed);
                if (rc == TC_ERR_BUFFER_TOO_SMALL) {
                    e->ab.off = needed;
                    e->ab.err = TC_ERR_BUFFER_TOO_SMALL;
                    rc = TC_ERR_BUFFER_TOO_SMALL;
                    /* C04：grow 模式预扩暂存后重写（首帧 ab.out 为 NULL） */
                    if (asm_reserve(&e->ab, needed) == TC_OK) {
                        rc = tc_v7_directory_write(fh, slices, segments,
                                                   e->ab.out, e->ab.cap, &needed);
                        if (rc == TC_OK) { e->ab.err = TC_OK; }
                    } else if (e->ab.err == TC_OK) {
                        e->ab.err = rc;
                    }
                }
            }
            if (rc == TC_OK) {
                e->ab.off = needed;
                e->fh.frame_packet_size = (uint32_t)needed;
                e->color_payload = (uint32_t)color_payload;
                e->color_hdr_bytes = (uint32_t)(needed - TC_FRAME_HEADER_SIZE -
                                                color_payload);
            }
        }
        goto v7_pass_done;
    }

    /* 逐 plane 顺序组装（与逐 plane 版字节序一致；alpha 捕获分支同源） */
    rc = TC_OK;
    uint64_t ta = prof ? tc_profile_now_ns() : 0u;
    for (uint32_t p = 0u; p < plane_end && rc == TC_OK; ++p) {
        uint32_t i0 = plane_band0[p];
        uint32_t i1 = plane_band0[p + 1u];
        if (p == 3u && e->m7_alpha_capture != 0) {
            for (uint32_t i = i0; i < i1; ++i) {
                enc_band_task* t = &tasks[i];
                if (t->rc != TC_OK) { rc = t->rc; break; }
                memcpy(e->alpha_bands[i - i0].hdr, t->hdr, sizeof(t->hdr));
                e->alpha_bands[i - i0].payload = t->payload;
                e->alpha_bands[i - i0].payload_size = t->payload_size;
                e->alpha_payload += t->payload_size;
                e->alpha_hdr_bytes += TC_SLICE_HEADER_SIZE;
            }
            e->alpha_band_n = i1 - i0;
        } else {
            for (uint32_t i = i0; i < i1; ++i) {
                enc_band_task* t = &tasks[i];
                if (t->rc != TC_OK) { rc = t->rc; break; }
                asm_append(&e->ab, t->hdr, sizeof(t->hdr));
                asm_append(&e->ab, t->payload, t->payload_size);
                if (e->ab.err == TC_ERR_LIMIT_EXCEEDED) { rc = e->ab.err; break; }
                if (t->is_alpha != 0) {
                    e->alpha_payload += t->payload_size;
                    e->alpha_hdr_bytes += TC_SLICE_HEADER_SIZE;
                } else {
                    e->color_payload += t->payload_size;
                    e->color_hdr_bytes += TC_SLICE_HEADER_SIZE;
                }
            }
        }
    }
    if (prof) { TC_STATS_LOCK(); g_enc_stats.asm_ns += tc_profile_now_ns() - ta; TC_STATS_UNLOCK(); }

v7_pass_done:

    /* bitwriter 结构体拷贝借出回写（与 enc_encode_plane_opt 同源语义） */
    for (uint32_t w = 0u; w < nw_alloc; ++w) {
        if (w == 0u) {
            e->bw = slots[0].bw;
            e->bw_ok = slots[0].bw_ok;
        } else {
            e->slot_bw[w] = slots[w].bw;
            e->slot_bw_ok[w] = slots[w].bw_ok;
        }
    }
    return rc;
}

/* alpha 捕获单 plane 通道（M10-6.2 后唯一逐 plane 调用方：m7_prepare 的
 * alpha 一次编码；颜色 plane 全部走 enc_band_pass_all 统一提交） */

static void fill_stats(const enc_shared* e, topos_frame_stats* stats)
{
    stats->packet_size = (uint32_t)e->ab.off;
    stats->color_payload_bytes = e->color_payload;
    stats->alpha_payload_bytes = e->alpha_payload;
    stats->color_header_bytes = TC_FRAME_HEADER_SIZE + e->color_hdr_bytes;
    stats->alpha_header_bytes = e->alpha_hdr_bytes;
    stats->slice_count = e->fh.slice_count;
    stats->qp_base = e->fh.qp_base;
    stats->alpha_max_abs_error = e->alpha_max_err;
}

/* 单帧编码主体（e 可跨调用复用：尺寸不变时零重复分配）。
 * 帧级统计计数器每次调用重置；e 的释放权归调用方。 */
/* 帧级准备（frame_encode_shared 与 M7 m7_prepare 共用）：
 * 配置/输入/stride 校验 → 几何派生 → 共享缓冲 reserve。 */
static int32_t enc_frame_setup(enc_shared* e, const topos_frame_config* cfg,
                               const topos_frame_input* input)
{
    int32_t rc = tc_frame_config_validate(cfg);
    if (rc != TC_OK) { return rc; }
    if (input == NULL ||
        (input->struct_size != 0u && input->struct_size != (uint32_t)sizeof(topos_frame_input))) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "input struct invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }

    e->in = input;
    e->slice_rows = cfg_slice_rows(cfg);
    e->qp_delta_luma = cfg->qp_delta_luma;
    e->qp_delta_chroma = cfg->qp_delta_chroma;
    /* V2.x AQ 开关（reserved[1]；0=默认关闭，行为与旧版逐位一致）。
     * 偏移数组每帧归零：fallback 路径（无 F-cache）与关闭态同观感。
     * TRAW（pf=3）冻结关闭：单色相位平面无色度语义（计划 §3.1/风险登记）。 */
    e->aq_on = (cfg->reserved[1] == 1u && cfg->pixel_format != 3u) ? 1 : 0;
    e->rdo_on = (cfg->reserved[2] == 1u) ? 1 : 0;
    /* V8（em==9）参数：reserved[3]/[4] 覆盖段/瓦片粒度（0 = 批 0 默认档）。
     * V9（em==10）/ V7-R3（em==11）：reserved[3]/[4]/[5] = frame_type/
     * gop_id/ref_distance（GOP 序列输入，非粒度）——段/瓦片粒度恒取
     * V8 默认档（V7-R3 走 legacy band 路径，不消费 v8 粒度）。 */
    const int is_temporal_cfg = ((cfg->reserved[0] & 0xFFu) == 10u ||
                                 (cfg->reserved[0] & 0xFFu) == 11u);
    e->v8_sb_log2 = (!is_temporal_cfg && cfg->reserved[3] != 0u)
                        ? (uint8_t)cfg->reserved[3] : TC_V8_DEFAULT_SB_LOG2;
    e->v8_tile_log2 = (!is_temporal_cfg && cfg->reserved[4] != 0u)
                          ? (uint8_t)cfg->reserved[4] : TC_V8_DEFAULT_TILE_LOG2;
    if (e->v8_sb_log2 < 3u || e->v8_sb_log2 > 5u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v8 reserved[3] sb_log2 %u not in {3,4,5}",
                     (unsigned)e->v8_sb_log2);
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (e->v8_tile_log2 != 0u && (e->v8_tile_log2 < 4u || e->v8_tile_log2 > 6u)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "v8 reserved[4] tile_log2 %u not in {0,4,5,6}",
                     (unsigned)e->v8_tile_log2);
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(e->aq_off, 0, sizeof(e->aq_off));
    e->color_payload = 0u;
    e->alpha_payload = 0u;
    e->color_hdr_bytes = 0u;
    e->alpha_hdr_bytes = 0u;
    e->alpha_max_err = 0u;
    enc_pay_reset(e); /* M10-6.1：跨调用复用时 arena 归零——上一调用的
                       * payload 已在组装中拷入最终包，返回后即死数据 */
    cfg_to_frame_header(cfg, &e->fh);
    rc = tc_frame_derive_geometry(&e->fh);
    if (rc != TC_OK) { return rc; }
    uint32_t total_slices = 0u;
    rc = frame_band_plan(&e->fh, e->slice_rows, NULL, &total_slices);
    if (rc != TC_OK) { return rc; }
    e->fh.slice_count = (uint16_t)total_slices;
    e->fh.frame_packet_size = TC_FRAME_HEADER_SIZE; /* 占位；终值经 self-check 复验 */
    rc = tc_frame_header_validate(&e->fh);
    if (rc != TC_OK) { return rc; }

    for (uint32_t p = 0u; p < e->fh.plane_count; ++p) {
        if (input->planes[p] == NULL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "input plane %u == NULL", (unsigned)p);
            return TC_ERR_INVALID_ARGUMENT;
        }
        /* stride 契约防御：非 0 时必须 >= 该 plane 的 visible 宽（uint16 元素计）。
         * 低于此值会读到行外内存（阶段 5 CLI 事故固化此校验）。 */
        if (input->strides[p] != 0u &&
            input->strides[p] < (size_t)e->fh.plane_visible_w[p]) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT,
                         "input stride[%u]=%zu < visible 宽 %u（strides 为 uint16 元素行距）",
                         (unsigned)p, input->strides[p],
                         (unsigned)e->fh.plane_visible_w[p]);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }

    return enc_reserve_shared(e);
}

static int32_t frame_encode_ready(enc_shared* e, uint8_t* out, size_t out_cap,
                                  topos_frame_stats* stats)
{
    int32_t rc;
    if (e->fh.version_major == 8u) {
        return v8_frame_encode_ready(e, out, out_cap, stats); /* V8（批 2） */
    }
    if (e->fh.version_major == 9u) {
        /* V9（topos_v9_micro_gop_plan 批 2；ADR-C047 zero-motion IP-2）：
         * 包布局 V8 同构 → 像素机直连。帧语义：
         *   - I（frame_type=0）：直接编码源帧；
         *   - P（frame_type=1）：编码「残差伪图」X=clip(curr−ref+mid)——
         *     参考的持有与残差合成是 GOP context 的职责（tc_gop_context_
         *     encode_frame），本直连入口只负责把给定平面编码成 V9 包。
         * alpha 沿用 V8 门（no-alpha capability，计划 §3.7 允许）。 */
        return v8_frame_encode_ready(e, out, out_cap, stats);
    }
    if (s_dev_tok_on != 0) { dev_tok_prepare(&e->fh, e->slice_rows); }   /* A-enc 差分机 */
    /* C04：装配全程写 grow-only 暂存——成功才一次性提交调用方 out；
     * 容量不足/失败返回时 out 字节原子不变（头文件 §285 契约）。
     * ab.out/cap 跨调用保留（enc_shared 缓存复用 → 稳态零分配）。 */
    e->ab.grow = 1;
    e->ab.off = TC_FRAME_HEADER_SIZE; /* header 位置预留 */
    e->ab.err = TC_OK;

    /* M10-6.2：全 plane（含 alpha）统一提交（原逐 plane 三/四轮串行） */
    rc = enc_band_pass_all(e, e->fh.plane_count, 0, 1);
    if (rc == TC_OK && e->ab.err == TC_ERR_LIMIT_EXCEEDED) { rc = e->ab.err; }
    if (rc == TC_OK) {
        e->fh.frame_packet_size = (uint32_t)e->ab.off;
        /* V7-A（熵 5）无 legacy 帧头契约；V7-R rANS（熵 6）是常规 packet */
        if (e->ab.err == TC_OK &&
            !(e->fh.version_major == 7u && e->fh.entropy_mode == 5u)) {
            rc = tc_frame_header_encode(&e->fh, e->ab.out); /* C04：写暂存 */
        }
    }
    if (rc == TC_OK && e->ab.err == TC_OK && e->self_check &&
        !(e->fh.version_major == 7u && e->fh.entropy_mode == 5u)) {
        /* §11.5：编码器输出必须通过自身完整结构校验（plain 编码恒开；
         * sized 迭代期关闭，搜索结束后对最终包统一复验——R6） */
        const int prof_chk = tc_profile_enabled();
        uint64_t tc0 = prof_chk ? tc_profile_now_ns() : 0u;
        topos_packet_view view;
        rc = tc_packet_scan(e->ab.out, e->ab.off, &view); /* C04：扫暂存 */
        if (rc == TC_OK) {
            for (uint32_t si = 0u; si < view.slice_count; ++si) {
                if (view.slice_crc_ok[si] == 0u) {
                    tc_set_error(TC_ERR_MALFORMED, "self-check: slice %u crc", (unsigned)si);
                    rc = TC_ERR_MALFORMED;
                    break;
                }
            }
        } else {
            tc_wrap_error(TC_ERR_MALFORMED, "self-check scan failed: "); /* C14 */
        }
        if (prof_chk) { TC_STATS_LOCK(); g_enc_stats.check_wall_ns += tc_profile_now_ns() - tc0; TC_STATS_UNLOCK(); }
    }

    if (rc == TC_OK && e->ab.err != TC_OK) { rc = e->ab.err; } /* C04：装配 OOM 等 */
    if (rc != TC_OK) { return rc; }
    fill_stats(e, stats);
    /* C04 提交：全部成功后才拷贝到调用方 out——失败路径 out 原子不变 */
    if (e->ab.off > out_cap) {
        tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "out_cap %zu < %zu bytes needed",
                     out_cap, e->ab.off);
        return TC_ERR_BUFFER_TOO_SMALL;
    }
    if (out != NULL && e->ab.off > 0u) { memcpy(out, e->ab.out, e->ab.off); }
    return TC_OK;
}

static int32_t frame_encode_shared(enc_shared* e, const topos_frame_config* cfg,
                                   const topos_frame_input* input,
                                   uint8_t* out, size_t out_cap, topos_frame_stats* stats)
{
    topos_frame_stats local_stats;
    memset(&local_stats, 0, sizeof(local_stats));
    if (stats == NULL) { stats = &local_stats; }
    stats->struct_size = (uint32_t)sizeof(topos_frame_stats);
    stats->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    if (out == NULL && out_cap != 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "out == NULL with cap %zu", out_cap);
        return TC_ERR_INVALID_ARGUMENT;
    }
    int32_t rc = enc_frame_setup(e, cfg, input);
    if (rc != TC_OK) { return rc; }
    return frame_encode_ready(e, out, out_cap, stats);
}

/* Internal RD2 reference entry point.  It deliberately does not change the
 * public ABI/configuration selector yet: the caller supplies an ordinary
 * frame config, while this opt-in path swaps only the on-wire coding tuple to
 * V7-A after the shared geometry/input validation has completed. */
int32_t tc_v7_frame_encode(const topos_frame_config* cfg,
                           const topos_frame_input* input,
                           uint8_t* out, size_t out_cap,
                           topos_frame_stats* stats)
{
    if (cfg == NULL || (out == NULL && out_cap != 0u)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7a encode arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* V 代际收纳（D1，2026-09-13）：V7-A 写端随归档退役——公共入口拒绝，
     * 仅 TOPOS_DEV_REPLAY 构建 + TOPOS_DEV=1 可达（供 band 语差异与考古） */
    if (tc_frame_header_dev_replay_enabled() == 0) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "v7a band encode retired (V consolidation 2026-09-13, "
                     "see ADR-C0xx); available only in a TOPOS_DEV_REPLAY "
                     "build with TOPOS_DEV=1");
        return TC_ERR_INVALID_ARGUMENT;
    }
    topos_frame_config staging = *cfg;
    /* V1（em=0）is only a validation/setup carrier; the prepared header is
     * replaced with the V7-A tuple before any color task runs.
     * （V 代际收纳 2026-09-13：原载体 em=2(V2-Rice) 随写域收缩改为 em=0
     * ——V1 与 V2-Rice 编码路径逐字节一致（stage9 parity 锚），setup
     * 行为不变；载体必须落在收缩后的合法写域 {0,1,8,9} 内。） */
    staging.reserved[0] = 0u;
    staging.reserved[1] = 0u;
    staging.reserved[2] = 0u;
    int32_t rc = tc_frame_config_validate(&staging);
    if (rc != TC_OK) { return rc; }
    if (cfg->alpha_mode != 0u) {
        tc_set_error(TC_ERR_NOT_IMPLEMENTED,
                     "v7a reference encoder does not support alpha");
        return TC_ERR_NOT_IMPLEMENTED;
    }

    topos_frame_stats local_stats;
    memset(&local_stats, 0, sizeof(local_stats));
    if (stats == NULL) { stats = &local_stats; }
    stats->struct_size = (uint32_t)sizeof(topos_frame_stats);
    stats->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;

    enc_loan loan;
    enc_shared* e = enc_loan_acquire(&loan);
    e->self_check = 0; /* legacy tc_packet_scan intentionally rejects V7-A */
    rc = enc_frame_setup(e, &staging, input);
    if (rc == TC_OK) {
        e->fh.version_major = 7u;
        e->fh.version_minor = 0u;
        e->fh.entropy_mode = 5u;
        e->fh.codebook_version = 5u;
        e->fh.coding_mode = 2u;
        e->fh.frame_packet_size = TC_FRAME_HEADER_SIZE;
        rc = tc_frame_header_validate_v7a_contract(&e->fh);
    }
    if (rc == TC_OK) {
        rc = frame_encode_ready(e, out, out_cap, stats);
    }
    enc_loan_release(&loan);
    return rc;
}

/* V2.x AQ：固定 qp 路径的 m7 借道（定义在 M7 节；此处前置声明） */
static int32_t m7_prepare(enc_shared* e, const topos_frame_config* cfg,
                          const topos_frame_input* input);
static int32_t m7_final(enc_shared* e, uint32_t qp, uint8_t* out, size_t out_cap,
                        topos_frame_stats* stats);

int32_t tc_frame_encode(const topos_frame_config* cfg, const topos_frame_input* input,
                        uint8_t* out, size_t out_cap, topos_frame_stats* stats)
{
    enc_loan loan;
    enc_shared* e = enc_loan_acquire(&loan);
    e->self_check = 1; /* plain 编码：每次输出都过完整结构自检（§11.5，行为不变） */
    int32_t rc;
    if (cfg != NULL && (cfg->reserved[1] == 1u || cfg->reserved[2] == 1u)
        && out != NULL) {
        /* V2.x AQ/RDO：固定 qp 路径同样生效——m7 预pass（DCT-once）后
         * F-cache 量化出流（AQ 在 prepare 算逐带偏移；RDO 在 fill 的
         * 量化后逐系数精修）。m7_final 不变量：偏移/精修全关时与
         * frame_encode_shared 同 qp 逐字节一致（两标志全关的位流不受
         * 影响）。fcache 不可用（>512MB 上限 / OOM / token 路径缺失）
         * → 回退 plain：aq_off 归零、RDO 跳过，确定性降级为关（上限
         * 与 token 探测是输入的纯函数；realloc OOM 属系统失效场景）。 */
        rc = m7_prepare(e, cfg, input);
        if (rc == TC_OK && e->fcache_ok != 0) {
            topos_frame_stats local_s;
            memset(&local_s, 0, sizeof(local_s));
            topos_frame_stats* sp = stats != NULL ? stats : &local_s;
            rc = m7_final(e, cfg->qp_base, out, out_cap, sp);
            if (rc == TC_OK) {
                /* 与 frame_encode_shared 的 §11.5 自检同保证 */
                topos_packet_view view;
                int32_t src = tc_packet_scan(out, sp->packet_size, &view);
                if (src == TC_OK) {
                    for (uint32_t si = 0u; si < view.slice_count; ++si) {
                        if (view.slice_crc_ok[si] == 0u) {
                            tc_set_error(TC_ERR_MALFORMED,
                                         "self-check: slice %u crc", (unsigned)si);
                            src = TC_ERR_MALFORMED;
                            break;
                        }
                    }
                } else {
                    tc_wrap_error(TC_ERR_MALFORMED, "self-check scan failed: "); /* C14 */
                }
                if (src != TC_OK) { rc = src; }
            }
        } else if (rc == TC_OK) {
            rc = frame_encode_shared(e, cfg, input, out, out_cap, stats);
        }
    } else {
        rc = frame_encode_shared(e, cfg, input, out, out_cap, stats);
    }
    enc_loan_release(&loan);
    return rc;
}

/* ---------------- M7：encode_sized 的 DCT-once + exact bit-count ----------------
 *
 * 计划 §11：qp 搜索不再每候选整帧重编码。
 *  1. prepare：全部颜色 plane pad+forward DCT 一次（F-cache）；alpha 全编码
 *     一次（qp 无关，payload/hdr 捕获复用）；
 *  2. probe：每候选 qp 只做 quant+token（F-cache 源）+ 精确位计数
 *     （rice_bits_exact 与 writer 写出位数严格一致，含每 slice 字节对齐、
 *     17B slice header、53B frame header、alpha 常量部分）→ 与整帧编码的
 *     packet_size 逐值相等 → 搜索决策与 legacy 完全一致；
 *  3. final：选定 qp 走 F-cache 量化 + token emit + CRC/组装（alpha 复用）。
 *
 * 回退：fcache 超上限/OOM、token 路径不可用、dev 禁用或搜索违反单调性时，
 * 整体走 legacy 整帧编码 probe（sized_search_linear/fast 旧路径）。 */

#define TC_ENC_FCACHE_MAX_BYTES (512u * 1024u * 1024u) /* 8K 422 ≈ 265MB 仍可缓存 */

typedef struct m7_dct_task {
    const topos_frame_header* fh;
    const uint16_t* coded;
    uint32_t plane, y0, h;
    int32_t* fplane; /* 该 plane 的 F-cache 基址（绝对块索引 [by*cols+bx]*64） */
    uint64_t dct_ns; /* M10-6 取证：worker CPU 计时（join 后求和） */
    uint64_t act;    /* V2.x AQ：Σ|F_AC|（act_wanted 时累计） */
    int act_wanted;
} m7_dct_task;

static void m7_dct_job(void* vctx)
{
    m7_dct_task* t = (m7_dct_task*)vctx;
    const int prof = tc_profile_enabled();
    uint64_t t0 = prof ? tc_profile_now_ns() : 0u;
    uint32_t cols = t->fh->plane_block_cols[t->plane];
    const int32_t mid = (int32_t)bd_mid(t->fh->bit_depth);
    /* P-速②：u16 平面行直载 forward（每任务一次解析） */
    const tc_forward_rows_fn fwd_rows = tc_simd_resolve_forward_rows((uint8_t)(t->fh->bit_depth > 12u));
    const size_t plane_stride = (size_t)cols * 8u;
    for (uint32_t by = 0u; by < t->h; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            const uint16_t* src = t->coded +
                (size_t)(t->y0 + by) * 8u * (size_t)cols * 8u + (size_t)(bx * 8u);
            int32_t* dst = t->fplane +
                ((size_t)(t->y0 + by) * (size_t)cols + (size_t)bx) * 64u;
            fwd_rows(src, plane_stride, mid, dst);
            if (t->act_wanted != 0) {
                /* V2.x AQ：热缓存上顺带累计 AC 绝对和（活动度代理） */
                uint64_t a = 0u;
                for (uint32_t k = 1u; k < 64u; ++k) {
                    int32_t v = dst[k];
                    a += (uint64_t)(v < 0 ? -v : v);
                }
                t->act += a;
            }
        }
    }
    if (prof) { t->dct_ns = tc_profile_now_ns() - t0; }
}

static int32_t tc_ilog2_u64(uint64_t x)
{
    int32_t r = 0;
    while (x > 1u) { x >>= 1; ++r; }
    return r;
}

/* P3 亮度空间 AQ：1/8 倍频程定点 log2（floor）——tc_ilog2_u64 的 2×
 * 频程粒度把 1.5× 活动度比折进同一格（偏移不触发，旧全平面实验
 * "偏移基本不触发"的同源根因）；尾数取前导 1 后 3 位。x=0 → 0。 */
static int32_t tc_log2_e8_u64(uint64_t x)
{
    if (x < 2u) { return 0; }
#if defined(__GNUC__) || defined(__clang__)
    int lz = __builtin_clzll(x);
#elif defined(_MSC_VER) && defined(_M_X64)
    unsigned long msb = 0;
    _BitScanReverse64(&msb, x);
    int lz = 63 - (int)msb;
#else
    int lz = 63;
    while ((x >> (uint32_t)lz) == 0ull) { lz--; } /* x ≥ 2 保证终止 */
#endif
    int32_t int_part = 63 - lz;
    /* 前导 1 左移到 bit63，再右移 61 取 3 位尾数（floor(frac×8)） */
    uint64_t frac_bits = (x << (uint32_t)(lz + 1)) >> 61;
    return int_part * 8 + (int32_t)frac_bits;
}

/* prepare：校验+几何+reserve → F-cache 分配 → 颜色 plane pad+DCT → alpha 一次编码。
 * 返回 TC_OK 时看 e->fcache_ok：0 = 不可用（调用方回退 legacy probe）。 */
static void dev_fcache_capture(const enc_shared* e); /* A1 差分机（定义见下） */
static int32_t m7_prepare(enc_shared* e, const topos_frame_config* cfg,
                          const topos_frame_input* input)
{
    int32_t rc = enc_frame_setup(e, cfg, input);
    if (rc != TC_OK) { return rc; }
    e->fcache_ok = 0;
    e->alpha_band_n = 0u;
    /* Intra residuals depend on reconstructed neighbours at this QP.
     * rANS（V7-R）走 m7 精确探针（真实后向编码，与最终编码逐字节一致）。
     * V8/V9（major 8/9）例外：段化 rANS 机器不是 m7 的 F-cache/Rice 写端
     * ——m7_final 会产出「major 8/9 帧头 + legacy Rice payload」的坏包
     * （self-check 拦截为 MALFORMED）。此处按 fcache 不可用回退 plain
     * （AQ/RDO 确定性降级为关，与 token 路径缺失同语义；V9 复审
     * 2026-09-14——此前 em=10+AQ 经 GOP context 可直达本洞）。 */
    if (e->fh.version_major == 8u || e->fh.version_major == 9u ||
        e->fh.version_major == 3u || e->fh.version_major == 6u ||
        (e->fh.version_major == 4u && e->fh.entropy_mode == 2u) ||
        (e->fh.version_major == 5u && e->fh.entropy_mode == 3u)) { return TC_OK; }
    if (e->tok_ok == 0) { return TC_OK; } /* token 路径不可用 → 回退 */

    uint64_t total_blocks = 0u;
    const uint32_t color_planes = fh_color_planes(&e->fh);
    for (uint32_t p = 0u; p < color_planes; ++p) {
        uint64_t blocks = (uint64_t)e->fh.plane_block_cols[p] * (uint64_t)e->fh.plane_block_rows[p];
        e->fcache_off[p] = (size_t)total_blocks;
        total_blocks += blocks;
    }
    uint64_t elems64 = total_blocks * 64u;
    size_t elems = 0;
    size_t fbytes = 0;
    if (total_blocks == 0u || elems64 > (uint64_t)SIZE_MAX ||
        !tc_umul_size((size_t)elems64, sizeof(int32_t), &fbytes) ||
        fbytes > (size_t)TC_ENC_FCACHE_MAX_BYTES) {
        return TC_OK; /* 超上限 → 回退（非错误） */
    }
    elems = (size_t)elems64;
    if (elems > e->fcache_elems) {
        int32_t* grown = (int32_t*)tc_realloc(e->fcache, fbytes);
        if (grown == NULL) { return TC_OK; } /* OOM → 回退 */
        e->fcache = grown;
        e->fcache_elems = elems;
    }

    /* M10-6.2：pad 全 plane（串行）→ 三 plane 全部 DCT band 一次提交
     * （原逐 plane 三轮；任务只读各 plane 自己的 coded 偏移区，无冲突） */
    const int prof = tc_profile_enabled();
    for (uint32_t p = 0u; p < color_planes; ++p) {
        uint64_t tp = prof ? tc_profile_now_ns() : 0u;
        rc = enc_prepare_plane(e, p);
        if (prof) { TC_STATS_LOCK(); g_enc_stats.pad_ns += tc_profile_now_ns() - tp; TC_STATS_UNLOCK(); }
        if (rc != TC_OK) { return rc; }
    }
    {
        m7_dct_task tasks[TC_MAX_SLICES];
        tc_job jobs[TC_MAX_SLICES];
        uint32_t ti = 0u;
        for (uint32_t p = 0u; p < color_planes; ++p) {
            uint32_t rows = e->fh.plane_block_rows[p];
            uint32_t bands = (rows + e->slice_rows - 1u) / e->slice_rows;
            if (bands == 0u) { bands = 1u; }
            for (uint32_t y0 = 0u; y0 < rows && ti < TC_MAX_SLICES;
                 y0 += e->slice_rows, ++ti) {
                uint32_t h = rows - y0;
                if (h > e->slice_rows) { h = e->slice_rows; }
                tasks[ti].fh = &e->fh;
                /* M11-2a：对齐直通平面读调用方输入（只读；cast 去 const 安全：
                 * 颜色 DCT band 路径不写平面，mode2 alpha 恒走拷贝路径） */
                tasks[ti].coded = e->plane_direct[p] != 0
                    ? (uint16_t*)(uintptr_t)e->plane_src[p]
                    : e->coded + e->coded_off[p];
                tasks[ti].plane = p;
                tasks[ti].y0 = y0;
                tasks[ti].h = h;
                tasks[ti].fplane = e->fcache + e->fcache_off[p] * 64u;
                tasks[ti].dct_ns = 0u;
                tasks[ti].act = 0u;
                tasks[ti].act_wanted = e->aq_on;
                jobs[ti].fn = m7_dct_job;
                jobs[ti].ctx = &tasks[ti];
            }
        }
        uint32_t nw = (uint32_t)tc_dev_thread_count();
        if (nw > ti) { nw = ti; }
        if (nw > (uint32_t)TC_SLICE_MAX_THREADS) { nw = (uint32_t)TC_SLICE_MAX_THREADS; }
        if (nw < 1u) { nw = 1u; }
        (void)tc_parallel_for(jobs, ti, nw);
        if (e->aq_on != 0) {
            /* V2.x AQ：活动度归位（任务序 = plane 主序带序），按对数比
             * 计算逐带偏移。与 qp 无关 → probe/final 同源。
             * 【实测纠偏 2026-09-03，证据 aq_rd_experiment 复跑+强度扫描】
             * 1. 全平面掩蔽方向（忙带粗量化含 luma）在 luma-PSNR 口径
             *    同码率 **−0.37~−0.48 dB**（2K422）——此前记录的
             *    "+0.37~+0.48" 是实验打印 ΔPSNR=AQ0−AQ1 的符号误读；
             * 2. 带（16 块行）粒度下 luma 活动度被平均，偏移基本不触发，
             *    全平面 ≈ 色度专属；
             * 3. 色度专属（plane 0 = Y/G 偏移恒 0）强度 2×clamp±2：
             *    同 qp 下 luma 逐位不变 + 忙色度带省字节（帕累托），
             *    同码率 luma +0.19~+0.23 dB（2K422，UV −0.84 dB）、
             *    444 家族 ≈0（已有静态 chroma_qp_offset=4，自适应无增量）；
             *    强度 ≥3 / clamp 3 时 UV 崩塌（−22 dB）→ 封顶 ±2。
             * 结论：切片粒度 AQ 天花板 ≈422 家族 2.5% 码率，默认保持关
             * （reserved[1]=1 选用）；追平 ProRes 需逐块 RDO（V2.x 工程）。
             * P3（2026-09-21，画质对齐计划 §5 P3）：plane 0 亮度空间 AQ
             * 重新入场（块均值归一 + 1/8 倍频程 log + 强度 3×±4，见下方
             * 实现注释）——色度 ±2 封顶与亮度新参数并存；"luma 逐位不变"
             * 不变量随之退役。
             * P3 验收实测（bench_out/topos_quality/p3_spatial_aq_ab.json，
             * 同母版 2K/4K proxy/lt 各 487 帧、tier bpp 同码率）：
             * ΔPSNR_Y = −0.05~−0.27 dB、ΔSSIM ≤0、Δblock_index 持平
             *（±0.01）、编码吞吐 −8%~−23%——**带粒度亮度重分配两个符号
             * 方向均为负收益**（掩蔽方向见 2026-09-03 证据；纹理优先方向
             * 见 P3），默认关维持（reserved[1]=1 实验选用）。块级 QP 信令
             *（位流版本变更）归 P5 评估，此处保留参数面与触发验证
             *（test_stage9 合成上忙/下平：忙带 MAE 21→1）。 */
            for (uint32_t i = 0u; i < ti; ++i) {
                e->aq_act[tasks[i].plane][tasks[i].y0 / e->slice_rows] =
                    tasks[i].act;
            }
            for (uint32_t p = 1u; p < color_planes; ++p) {   /* 色度专属：p0=Y/G 恒 0（pf=3 时 AQ 已强制关） */
                uint32_t rows_p = e->fh.plane_block_rows[p];
                uint32_t bands = (rows_p + e->slice_rows - 1u) / e->slice_rows;
                if (bands == 0u) { bands = 1u; }
                if (bands < 2u) { continue; }
                uint64_t sum = 0u;
                for (uint32_t b = 0u; b < bands; ++b) {
                    sum += e->aq_act[p][b];
                }
                int32_t lg_avg = tc_ilog2_u64(sum / (uint64_t)bands + 1u);
                for (uint32_t b = 0u; b < bands; ++b) {
                    int32_t diff =
                        tc_ilog2_u64(e->aq_act[p][b] + 1u) - lg_avg;
                    int32_t off = diff * 2;    /* 强度 2：2× 活动度差 → ±2 qp */
                    if (off > 2) { off = 2; }
                    if (off < -2) { off = -2; }
                    e->aq_off[p][b] = (int8_t)off;
                }
            }
            /* P3 亮度空间 AQ（2026-09-21，画质对齐计划 §5 P3）：plane 0
             *（YUV 的 Y / GBR 的 G）按【块均值活动度】对数比施加偏移，
             * 强度 3 / 钳位 ±4。与 2026-09-03 全平面负收益实验的三点差异：
             * 1. 块均值归一——带内块数不同（短尾带/色度半宽）时原始带和
             *    被块数稀释，均值才反映"这块区域多忙"；
             * 2. slice_rows=8 产品带高（实验期 16）：纹理带/平坦带在 8 块
             *    行粒度可分离（藤编/织物区整带高纹理）；
             * 3. 钳位 ±4（色度 ±2 是 UV 崩塌封顶，亮度无此结构劣势）——
             *    平坦带真正回收码字给纹理带。
             * 位流零格式变更：qp_delta_biased 逐 slice 承载有效 qp
             * （v1.0 冻结能力），解码端按 slice 重建量化表，零改动。 */
            {
                uint32_t rows0 = e->fh.plane_block_rows[0];
                uint32_t cols0 = e->fh.plane_block_cols[0];
                uint32_t bands0 = (rows0 + e->slice_rows - 1u) / e->slice_rows;
                if (bands0 == 0u) { bands0 = 1u; }
                if (bands0 >= 2u && cols0 != 0u) {
                    uint64_t mean_act[TC_MAX_SLICES];
                    uint64_t total_act = 0u, total_blk = 0u;
                    for (uint32_t b = 0u; b < bands0; ++b) {
                        uint32_t hb = rows0 - b * e->slice_rows;
                        if (hb > e->slice_rows) { hb = e->slice_rows; }
                        uint64_t blk = (uint64_t)cols0 * (uint64_t)hb;
                        mean_act[b] = e->aq_act[0][b] / blk;
                        total_act += e->aq_act[0][b];
                        total_blk += blk;
                    }
                    uint64_t avg_mean =
                        total_blk ? total_act / total_blk : 0u;
                    int32_t lg_avg0 = tc_log2_e8_u64(avg_mean + 1u);
                    for (uint32_t b = 0u; b < bands0; ++b) {
                        int32_t diff =
                            tc_log2_e8_u64(mean_act[b] + 1u) - lg_avg0;
                        /* 符号与色度掩蔽方向【相反】：diff>0（忙带）→ 负偏移
                         *（更细量化、得码字）；diff<0（平坦带）→ 正偏移
                         *（更粗量化、让码字）——P3 的"从平坦区回收、给
                         * 纹理/边缘"分配方向。1/8 倍频程 × 强度 3 / 8：
                         * 比值 1.5×→−1、2×→−3、4×+→−4（钳位）。 */
                        int32_t off = -((diff * 3 + 4) / 8);   /* P3 强度 3 */
                        if (off > 4) { off = 4; }
                        if (off < -4) { off = -4; }
                        e->aq_off[0][b] = (int8_t)off;

                    }
                }
            }
        }
        if (prof) {
            TC_STATS_LOCK();
            for (uint32_t i = 0u; i < ti; ++i) {
                g_enc_stats.dct_ns += tasks[i].dct_ns;
            }
            TC_STATS_UNLOCK();
        }
    }

    /* alpha 一次编码（qp 无关）：捕获 hdr/payload（驻留 arena，勿 reset） */
    if (e->fh.alpha_mode != 0u) {
        e->m7_alpha_capture = 1;
        rc = enc_encode_plane_opt(e, 3u, 0, 0); /* pad 走 coded；不复位 arena */
        e->m7_alpha_capture = 0;
        if (rc != TC_OK) { return rc; }
    }
    e->fcache_ok = 1;
    dev_fcache_capture(e); /* A1 差分机：捕获本帧 F 平面（dev 开关默认关） */
    return TC_OK;
}

/* ---- A1（速度计划 v2）差分机：F-cache 捕获（dev/test only） ----
 *
 * 目的：GPU 化一期（DCT+量化 → Metal）的数值等价验收前置——把
 * m7_prepare 产出的 F 平面（自然序 [by*cols+bx]*64，i32）拷出到
 * dev 侧缓冲，供外部参考实现（numpy/MSL kernel）逐值对拍。
 * 默认关闭（零拷贝零开销）；捕获点在 workers join 后的主线程。 */
static int s_dev_fcache_on = 0;
static int32_t* s_dev_fcache_buf = NULL;
static size_t s_dev_fcache_elems = 0;
static size_t s_dev_fcache_off[3];
static uint32_t s_dev_fcache_planes = 0;

void tc_dev_set_fcache_capture(int on)
{
    s_dev_fcache_on = (on != 0) ? 1 : 0;
    if (s_dev_fcache_on == 0) {
        tc_free(s_dev_fcache_buf);
        s_dev_fcache_buf = NULL;
        s_dev_fcache_elems = 0u;
        s_dev_fcache_planes = 0u;
    }
}

int64_t tc_dev_fcache_elems(void)
{
    return (int64_t)s_dev_fcache_elems;
}

int64_t tc_dev_fcache_copy(void* dst, size_t cap_elems)
{
    if (s_dev_fcache_buf == NULL || dst == NULL || cap_elems < s_dev_fcache_elems) {
        return 0;
    }
    memcpy(dst, s_dev_fcache_buf, s_dev_fcache_elems * sizeof(int32_t));
    return (int64_t)s_dev_fcache_elems;
}

void tc_dev_fcache_layout(uint32_t off_elems[3], uint32_t* planes)
{
    if (off_elems != NULL) {
        for (int p = 0; p < 3; ++p) {
            off_elems[p] = (uint32_t)s_dev_fcache_off[p];
        }
    }
    if (planes != NULL) { *planes = s_dev_fcache_planes; }
}

static void dev_fcache_capture(const enc_shared* e)
{
    if (s_dev_fcache_on == 0 || e->fcache_ok == 0 || e->fcache == NULL) { return; }
    /* 当帧几何的精确元素数（fcache_elems 为 grow-only 水位——小几何
     * 复用大缓存时大于当帧用量，直接拷会带入上一帧的陈旧尾部） */
    uint64_t exact = 0u;
    for (int p = 0; p < 3; ++p) {
        exact += (uint64_t)e->fh.plane_block_rows[p] * (uint64_t)e->fh.plane_block_cols[p] * 64u;
    }
    const size_t need = (size_t)exact;
    if (s_dev_fcache_buf == NULL || s_dev_fcache_elems < need) {
        int32_t* grown = (int32_t*)tc_realloc(s_dev_fcache_buf, need * sizeof(int32_t));
        if (grown == NULL) { return; } /* 保持旧捕获；差分机自行判空 */
        s_dev_fcache_buf = grown;
    }
    memcpy(s_dev_fcache_buf, e->fcache, need * sizeof(int32_t));
    s_dev_fcache_elems = need;
    for (int p = 0; p < 3; ++p) { s_dev_fcache_off[p] = (uint32_t)e->fcache_off[p]; }
    s_dev_fcache_planes = 3u;
}

/* ---- A2（速度计划 v2）差分机：解码侧量化系数捕获（dev/test only）----
 *
 * 目的：解码 GPU 化一期（CPU rANS 出 token → GPU 反量化+IDCT）的数值
 * 等价验收前置——把颜色 slice 的重建量化系数 q（natural 序稠密）与每
 * slice Q 表拷出到 dev 缓冲，供外部参考实现（numpy/MSL kernel）重建
 * 平面逐字节对拍。默认关闭（生产路径零分支外开销：任务字段恒 NULL）。 */
static int s_dev_qcap_on = 0;
static int32_t* s_dev_qcap_buf = NULL;
static size_t s_dev_qcap_elems = 0u;
static size_t s_dev_qcap_cap = 0u;           /* s_dev_qcap_buf 容量（i32 计） */
static uint32_t* s_dev_qcap_q = NULL;        /* slices × 64 步长表 */
static tc_dev_qcap_slice* s_dev_qcap_meta = NULL;
static uint32_t s_dev_qcap_slices = 0u;

/* A2-4 前置声明：稀疏三缓冲定义在本文件后段（sparse 块），捕获会话
 * 重置需要在此释放——见 tc_dev_set_qcap_capture(0)。 */
static int32_t* s_dev_qcap_sp_dc;
static uint32_t* s_dev_qcap_sp_off;
static uint64_t* s_dev_qcap_sp_pairs;
static size_t s_dev_qcap_sp_dc_elems;
static size_t s_dev_qcap_sp_off_elems;
static size_t s_dev_qcap_sp_pairs_cap;
static uint64_t s_dev_qcap_sp_npairs;

/* A2-4 生产直发目标（ext 模式）：稀疏三缓冲改用调用方内存（GPU 可见
 * pinned），容量元素计；超容/未设时回落内部分配。 */
static uint64_t* s_dev_qcap_ext_pairs = NULL;
static uint64_t s_dev_qcap_ext_pairs_cap = 0u;
static uint32_t* s_dev_qcap_ext_off = NULL;
static uint64_t s_dev_qcap_ext_off_cap = 0u;
static int32_t* s_dev_qcap_ext_dc = NULL;
static uint64_t s_dev_qcap_ext_dc_cap = 0u;

void tc_dev_qcap_sp_set_targets(uint64_t* pairs, uint64_t pairs_cap,
                                uint32_t* off, uint64_t off_cap,
                                int32_t* dc, uint64_t dc_cap)
{
    if (pairs == NULL || off == NULL || dc == NULL) {
        s_dev_qcap_ext_pairs = NULL;
        s_dev_qcap_ext_off = NULL;
        s_dev_qcap_ext_dc = NULL;
        return;
    }
    s_dev_qcap_ext_pairs = pairs;
    s_dev_qcap_ext_pairs_cap = pairs_cap;
    s_dev_qcap_ext_off = off;
    s_dev_qcap_ext_off_cap = off_cap;
    s_dev_qcap_ext_dc = dc;
    s_dev_qcap_ext_dc_cap = dc_cap;
}

void tc_dev_set_qcap_capture(int on)
{
    s_dev_qcap_on = (on != 0) ? 1 : 0;
    if (s_dev_qcap_on == 0) {
        tc_free(s_dev_qcap_buf);
        tc_free(s_dev_qcap_q);
        tc_free(s_dev_qcap_meta);
        s_dev_qcap_buf = NULL;
        s_dev_qcap_cap = 0u;
        s_dev_qcap_q = NULL;
        s_dev_qcap_meta = NULL;
        s_dev_qcap_elems = 0u;
        s_dev_qcap_slices = 0u;
        /* A2-4：稀疏三缓冲的累计计数随捕获会话重置（sparse 旗标保持）——
         * 否则逐帧重挂会话下 fill level 无限增长（steady-state 内存泄漏
         * 式膨胀，4K 444 每帧 +1.5MB dc / +3.9MB off / +~24MB 预留）。
         * ext 直发模式下仅解除指针（调用方内存不释放，setter 语义）。 */
        if (s_dev_qcap_sp_pairs != s_dev_qcap_ext_pairs) {
            tc_free(s_dev_qcap_sp_dc);
            tc_free(s_dev_qcap_sp_off);
            tc_free(s_dev_qcap_sp_pairs);
        }
        s_dev_qcap_sp_dc = NULL;
        s_dev_qcap_sp_off = NULL;
        s_dev_qcap_sp_pairs = NULL;
        s_dev_qcap_sp_dc_elems = 0u;
        s_dev_qcap_sp_off_elems = 0u;
        s_dev_qcap_sp_pairs_cap = 0u;
        s_dev_qcap_sp_npairs = 0u;
    }
}

int64_t tc_dev_qcap_elems(void)
{
    return (int64_t)s_dev_qcap_elems;
}

int64_t tc_dev_qcap_copy(void* dst, size_t cap_elems)
{
    if (s_dev_qcap_buf == NULL || dst == NULL || cap_elems < s_dev_qcap_elems) {
        return 0;
    }
    memcpy(dst, s_dev_qcap_buf, s_dev_qcap_elems * sizeof(int32_t));
    return (int64_t)s_dev_qcap_elems;
}

uint32_t tc_dev_qcap_slices(void)
{
    return s_dev_qcap_slices;
}

int64_t tc_dev_qcap_meta_copy(void* dst, size_t cap_slices)
{
    if (s_dev_qcap_meta == NULL || dst == NULL || cap_slices < s_dev_qcap_slices) {
        return 0;
    }
    memcpy(dst, s_dev_qcap_meta, (size_t)s_dev_qcap_slices * sizeof(tc_dev_qcap_slice));
    return (int64_t)s_dev_qcap_slices;
}

int64_t tc_dev_qcap_q_copy(void* dst, size_t cap_u32)
{
    if (s_dev_qcap_q == NULL || dst == NULL || cap_u32 < (size_t)s_dev_qcap_slices * 64u) {
        return 0;
    }
    memcpy(dst, s_dev_qcap_q, (size_t)s_dev_qcap_slices * 64u * sizeof(uint32_t));
    return (int64_t)((size_t)s_dev_qcap_slices * 64u);
}

/* ---- A2 编辑器移交实验：稀疏发射（dev/test only，与 qcap 联用） ----
 * 段布局确定性推导（免 post-join 组装；O(nslices) 扫描，dev 路径无谓）： */
static size_t dev_qcap_sp_off_seg(uint32_t si)
{
    size_t off = 0u;
    for (uint32_t s = 0u; s < si && s < s_dev_qcap_slices; ++s) {
        off += (size_t)s_dev_qcap_meta[s].block_h * (size_t)s_dev_qcap_meta[s].cols + 1u;
    }
    return off;
}

static size_t dev_qcap_sp_pairs_seg(uint32_t si)
{
    size_t off = 0u;
    for (uint32_t s = 0u; s < si && s < s_dev_qcap_slices; ++s) {
        off += 63ull * (size_t)s_dev_qcap_meta[s].block_h * (size_t)s_dev_qcap_meta[s].cols;
    }
    return off;
}

static size_t dev_qcap_sp_dc_seg(uint32_t plane)
{
    size_t off = 0u;
    for (int p = 0; p < (int)plane; ++p) {
        uint32_t rows = 0u, cols = 0u;
        for (uint32_t s = 0u; s < s_dev_qcap_slices; ++s) {
            if (s_dev_qcap_meta[s].plane == (uint32_t)p) {
                const uint32_t end = s_dev_qcap_meta[s].block_y0 + s_dev_qcap_meta[s].block_h;
                if (end > rows) { rows = end; }
                cols = s_dev_qcap_meta[s].cols;
            }
        }
        off += (size_t)rows * (size_t)cols;
    }
    return off;
}

static int s_dev_qcap_sparse = 0;
static int32_t* s_dev_qcap_sp_dc = NULL;      /* plane 稠密（全局块号） */
static uint32_t* s_dev_qcap_sp_off = NULL;    /* 每 slice 段 [blocks+1] 拼接 */
static uint64_t* s_dev_qcap_sp_pairs = NULL;  /* 每 slice 最坏段拼接 */
static size_t s_dev_qcap_sp_dc_elems = 0u;
static size_t s_dev_qcap_sp_off_elems = 0u;
static size_t s_dev_qcap_sp_pairs_cap = 0u;
static uint64_t s_dev_qcap_sp_npairs = 0u;

void tc_dev_set_qcap_sparse(int on)
{
    s_dev_qcap_sparse = (on != 0) ? 1 : 0;
    if (s_dev_qcap_sparse == 0) {
        if (s_dev_qcap_sp_dc != s_dev_qcap_ext_dc) { tc_free(s_dev_qcap_sp_dc); }
        if (s_dev_qcap_sp_off != s_dev_qcap_ext_off) { tc_free(s_dev_qcap_sp_off); }
        if (s_dev_qcap_sp_pairs != s_dev_qcap_ext_pairs) { tc_free(s_dev_qcap_sp_pairs); }
        s_dev_qcap_sp_dc = NULL;
        s_dev_qcap_sp_off = NULL;
        s_dev_qcap_sp_pairs = NULL;
        s_dev_qcap_sp_dc_elems = 0u;
        s_dev_qcap_sp_off_elems = 0u;
        s_dev_qcap_sp_pairs_cap = 0u;
        s_dev_qcap_sp_npairs = 0u;
    }
}

int64_t tc_dev_qcap_sp_dc_copy(void* dst, size_t cap_i32)
{
    if (s_dev_qcap_sp_dc == NULL || dst == NULL || cap_i32 < s_dev_qcap_sp_dc_elems) {
        return 0;
    }
    memcpy(dst, s_dev_qcap_sp_dc, s_dev_qcap_sp_dc_elems * sizeof(int32_t));
    return (int64_t)s_dev_qcap_sp_dc_elems;
}

int64_t tc_dev_qcap_sp_off_copy(void* dst, size_t cap_u32)
{
    if (s_dev_qcap_sp_off == NULL || dst == NULL || cap_u32 < s_dev_qcap_sp_off_elems) {
        return 0;
    }
    memcpy(dst, s_dev_qcap_sp_off, s_dev_qcap_sp_off_elems * sizeof(uint32_t));
    return (int64_t)s_dev_qcap_sp_off_elems;
}

int64_t tc_dev_qcap_sp_pairs_copy(void* dst, size_t cap_u64)
{
    if (s_dev_qcap_sp_pairs == NULL || dst == NULL) { return 0; }
    /* 只拷实际用量（最坏预留段的尾部是未初始化内存） */
    uint64_t used = 0u;
    for (uint32_t si = 0u; si < s_dev_qcap_slices; ++si) {
        const size_t blocks = (size_t)s_dev_qcap_meta[si].block_h
                              * (size_t)s_dev_qcap_meta[si].cols;
        used += s_dev_qcap_sp_off[dev_qcap_sp_off_seg(si) + blocks];
    }
    if (cap_u64 < (size_t)used) { return 0; }
    /* 段拼接拷贝：最坏段间的洞跳过 */
    size_t dstp = 0u;
    for (uint32_t si = 0u; si < s_dev_qcap_slices; ++si) {
        const size_t blocks = (size_t)s_dev_qcap_meta[si].block_h
                              * (size_t)s_dev_qcap_meta[si].cols;
        const size_t n = s_dev_qcap_sp_off[dev_qcap_sp_off_seg(si) + blocks];
        if (n != 0u) {
            memcpy((uint64_t*)dst + dstp, s_dev_qcap_sp_pairs + dev_qcap_sp_pairs_seg(si),
                   n * sizeof(uint64_t));
            dstp += n;
        }
    }
    s_dev_qcap_sp_npairs = used;
    return (int64_t)dstp;
}

uint64_t tc_dev_qcap_sp_npairs(void)
{
    return s_dev_qcap_sp_npairs;
}

/* 批入口（dec_frames_core_impl，主线程）调用：统计本批可捕获 slice 的
 * 精确总量并一次性扩容三个缓冲。此后 reserve 只做区域切分——worker
 * 拿到的指针在整个批内稳定（逐 slice realloc 会让先前任务的区域指针
 * 悬空 = use-after-free 堆损坏，首版实测教训）。 */
static int s_dev_qcap_ready = 0;   /* 本批已预扩容（reserve 前提） */

static void dev_qcap_prepare_batch(const dec_frame_req* reqs, uint32_t nframes)
{
    s_dev_qcap_ready = 0;
    if (s_dev_qcap_on == 0) { return; }
    uint64_t total = 0u;
    uint32_t slices = 0u;
    for (uint32_t i = 0u; i < nframes; ++i) {
        const topos_frame_header* fh = &reqs[i].view->fh;
        if (fh->version_major == 3u || fh->version_major == 6u) { continue; }
        for (uint32_t si = 0u; si < reqs[i].view->slice_count; ++si) {
            const topos_slice_header* sh = &reqs[i].view->slices[si];
            if (sh->plane == 3u) { continue; }
            total += (uint64_t)sh->block_h * (uint64_t)fh->plane_block_cols[sh->plane] * 64u;
            slices++;
        }
    }
    if (total > 0x08000000ull) { return; } /* 128M i32 = 512MB（覆盖 8K 444）差分机上界 */
    if (s_dev_qcap_elems + (size_t)total > s_dev_qcap_cap) {
        int32_t* grown = (int32_t*)tc_realloc(
            s_dev_qcap_buf, (s_dev_qcap_elems + (size_t)total) * sizeof(int32_t));
        if (grown == NULL) { return; } /* 保持旧捕获；本批走常规直写 */
        s_dev_qcap_buf = grown;
        s_dev_qcap_cap = s_dev_qcap_elems + (size_t)total;
    }
    uint32_t* grown_q = (uint32_t*)tc_realloc(
        s_dev_qcap_q, (size_t)(s_dev_qcap_slices + slices) * 64u * sizeof(uint32_t));
    if (grown_q == NULL) { return; }
    s_dev_qcap_q = grown_q;
    tc_dev_qcap_slice* meta = (tc_dev_qcap_slice*)tc_realloc(
        s_dev_qcap_meta, (size_t)(s_dev_qcap_slices + slices) * sizeof(tc_dev_qcap_slice));
    if (meta == NULL) { return; }
    s_dev_qcap_meta = meta;
    if (s_dev_qcap_sparse != 0) {
        /* dc = Σ_p rows_p×cols_p；off = Σ_s (blocks_s+1)；pairs = Σ_s 63×blocks_s
         *（最坏预留，512MB 上界；实际对数由 off 段尾给出） */
        uint64_t dc_total = 0u, off_total = 0u, pairs_total = 0u;
        for (uint32_t s = 0u; s < slices; ++s) {
            const uint64_t blocks = (uint64_t)0u; /* 占位——由 meta 推导见下 */
            (void)blocks;
            break;
        }
        /* 先记本批各 slice 的 blocks（meta 尚未写入——用 reqs 重算） */
        for (uint32_t i = 0u; i < nframes; ++i) {
            const topos_frame_header* fh = &reqs[i].view->fh;
            if (fh->version_major == 3u || fh->version_major == 6u) { continue; }
            for (uint32_t si = 0u; si < reqs[i].view->slice_count; ++si) {
                const topos_slice_header* sh = &reqs[i].view->slices[si];
                if (sh->plane == 3u) { continue; }
                const uint64_t blocks =
                    (uint64_t)sh->block_h * (uint64_t)fh->plane_block_cols[sh->plane];
                off_total += blocks + 1u;
                pairs_total += 63ull * blocks;
            }
        }
        /* plane 稠密 dc 总量：按 plane 求 rows×cols（跨 slice 求 max end） */
        for (uint32_t pl = 0u; pl < 3u; ++pl) {
            uint32_t rows = 0u, cols = 0u;
            for (uint32_t i = 0u; i < nframes; ++i) {
                const topos_frame_header* fh = &reqs[i].view->fh;
                if (fh->version_major == 3u || fh->version_major == 6u) { continue; }
                for (uint32_t si = 0u; si < reqs[i].view->slice_count; ++si) {
                    const topos_slice_header* sh = &reqs[i].view->slices[si];
                    if (sh->plane != pl) { continue; }
                    const uint32_t end = (uint32_t)sh->block_y0 + (uint32_t)sh->block_h;
                    if (end > rows) { rows = end; }
                    cols = fh->plane_block_cols[pl];
                }
            }
            dc_total += (uint64_t)rows * (uint64_t)cols;
        }
        if (pairs_total > (64ull << 20)) { return; } /* 512MB 上界（64M 对） */
        if (s_dev_qcap_ext_pairs != NULL
                && s_dev_qcap_ext_pairs_cap >= pairs_total
                && s_dev_qcap_ext_off_cap >= off_total
                && s_dev_qcap_ext_dc_cap >= dc_total) {
            /* A2-4 ext 直发：本批从基址 0 起用调用方缓冲（会话=一批） */
            s_dev_qcap_sp_dc = s_dev_qcap_ext_dc;
            s_dev_qcap_sp_off = s_dev_qcap_ext_off;
            s_dev_qcap_sp_pairs = s_dev_qcap_ext_pairs;
            s_dev_qcap_sp_dc_elems = (size_t)dc_total;
            s_dev_qcap_sp_off_elems = (size_t)off_total;
            s_dev_qcap_sp_pairs_cap = (size_t)pairs_total;
        } else {
            int32_t* grown_dc = (int32_t*)tc_realloc(
                s_dev_qcap_sp_dc, (size_t)(s_dev_qcap_sp_dc_elems + dc_total) * sizeof(int32_t));
            if (grown_dc == NULL) { return; }
            s_dev_qcap_sp_dc = grown_dc;
            uint32_t* grown_off = (uint32_t*)tc_realloc(
                s_dev_qcap_sp_off, (size_t)(s_dev_qcap_sp_off_elems + off_total) * sizeof(uint32_t));
            if (grown_off == NULL) { return; }
            s_dev_qcap_sp_off = grown_off;
            uint64_t* grown_pairs = (uint64_t*)tc_realloc(
                s_dev_qcap_sp_pairs, (size_t)(s_dev_qcap_sp_pairs_cap + pairs_total) * sizeof(uint64_t));
            if (grown_pairs == NULL) { return; }
            s_dev_qcap_sp_pairs = grown_pairs;
            s_dev_qcap_sp_dc_elems += (size_t)dc_total;
            s_dev_qcap_sp_off_elems += (size_t)off_total;
            s_dev_qcap_sp_pairs_cap += (size_t)pairs_total;
        }
    }
    s_dev_qcap_ready = 1;
}

/* 主线程（dec_frame_build_jobs）调用：切分该 slice 的捕获区域并记元
 * 数据（无分配——容量已在批入口一次到位）；未就绪返回 NULL（常规直写）。 */
static int32_t* dev_qcap_reserve(const topos_frame_header* fh, const topos_slice_header* sh,
                                 uint32_t** q_out_region)
{
    if (s_dev_qcap_ready == 0) { return NULL; }
    const uint32_t plane = sh->plane;
    const uint32_t cols = fh->plane_block_cols[plane];
    const size_t elems = (size_t)sh->block_h * (size_t)cols * 64u;

    int32_t* region = s_dev_qcap_buf + s_dev_qcap_elems;
    uint32_t* qrow = s_dev_qcap_q + (size_t)s_dev_qcap_slices * 64u;
    tc_dev_qcap_slice* m = &s_dev_qcap_meta[s_dev_qcap_slices];
    m->plane = plane;
    m->block_y0 = sh->block_y0;
    m->block_h = (uint32_t)sh->block_h;
    m->cols = cols;
    m->qp_eff = (uint32_t)(tc_slice_effective_qp(fh->qp_base, sh->qp_delta_biased));
    m->off_elems = (uint32_t)s_dev_qcap_elems;
    s_dev_qcap_elems += elems;
    s_dev_qcap_slices++;
    if (q_out_region != NULL) { *q_out_region = qrow; }
    return region;
}

/* worker（dec_slice_job）调用：扫描该 slice 的稠密 q → CSR 段。段基址
 * 由元数据确定性推导；dc 按 plane 全局块号写（slice 带内区域互斥）。 */
static void dev_qcap_emit_sparse(uint32_t gi, const int32_t* q)
{
    if (s_dev_qcap_sparse == 0 || gi >= s_dev_qcap_slices) { return; }
    const tc_dev_qcap_slice* m = &s_dev_qcap_meta[gi];
    const size_t cols = m->cols;
    const size_t band = m->block_h;
    const size_t dc_base = dev_qcap_sp_dc_seg(m->plane);
    uint32_t* off = s_dev_qcap_sp_off + dev_qcap_sp_off_seg(gi);
    uint64_t* pr = s_dev_qcap_sp_pairs + dev_qcap_sp_pairs_seg(gi);
    uint32_t cnt = 0u;
    for (size_t r = 0u; r < band; ++r) {
        int32_t* dc_row = s_dev_qcap_sp_dc
            + dc_base + ((size_t)m->block_y0 + r) * cols;
        for (size_t c = 0u; c < cols; ++c) {
            const int32_t* qb = q + (r * cols + c) * 64u;
            dc_row[c] = qb[0];
            off[r * cols + c] = cnt;   /* CSR 起点（块前累计） */
            for (uint32_t i = 1u; i < 64u; ++i) {
                if (qb[i] != 0) {
                    pr[cnt++] = ((uint64_t)(int64_t)qb[i] << 6) | (uint64_t)i;
                }
            }
        }
    }
    off[band * cols] = cnt;
}

/* A2-4：slice 失败时清零该 slice 的 CSR 段——全零系数的 GPU 重建 =
 * round(0/2^32)+mid = mid，与 conceal_band 的 bd_mid 填充逐值一致
 * （融合发射半途失败与 legacy 未发射两种情形统一走这里）。 */
static void dev_qcap_zero_slice(uint32_t gi)
{
    if (s_dev_qcap_sparse == 0 || gi >= s_dev_qcap_slices) { return; }
    const tc_dev_qcap_slice* m = &s_dev_qcap_meta[gi];
    const size_t cols = m->cols;
    const size_t blocks = (size_t)m->block_h * cols;
    memset(s_dev_qcap_sp_off + dev_qcap_sp_off_seg(gi), 0,
           (blocks + 1u) * sizeof(uint32_t));
    int32_t* dcr = s_dev_qcap_sp_dc + dev_qcap_sp_dc_seg(m->plane)
                 + (size_t)m->block_y0 * cols;
    memset(dcr, 0, blocks * sizeof(int32_t));
}

typedef struct m7_probe_task {
    const topos_frame_header* fh;
    uint32_t plane, y0, h;
    const tc_quant_ctx* qctx;
    uint32_t qp_eff;            /* RDO 精修的 qp 档位先验 */
    int rdo;                    /* reserved[2]：逐系数 level RDO */
    enc_worker_slot* slots_base; /* 解码深化：job 入口按执行线程绑定槽位 */
    uint32_t nw;
    enc_worker_slot* slot;
    enc_shared* es;             /* C1：槽位 scratch 池宿主 */
    const int32_t* fplane;
    uint64_t slice_bytes; /* out：17 + ceil(bits/8) */
    uint64_t fill_ns;     /* M10-6 取证：量化+token+位计数 CPU（join 后求和） */
    int32_t rc;
} m7_probe_task;

static void m7_probe_job(void* vctx)
{
    m7_probe_task* t = (m7_probe_task*)vctx;
    const int prof = tc_profile_enabled();
    uint64_t t0 = prof ? tc_profile_now_ns() : 0u;
    uint32_t wslot = tc_pool_worker_slot();
    if (wslot >= t->nw) { wslot = 0u; }
    enc_worker_slot* s = &t->slots_base[wslot];
    uint32_t cols = t->fh->plane_block_cols[t->plane];
    uint32_t k1 = 0u, k2 = 0u, k3 = 0u;
    uint8_t vlc = (uint8_t)(t->fh->version_major == 2u && t->fh->entropy_mode == 1u);
    uint8_t rans = (uint8_t)(t->fh->version_major == 7u && t->fh->entropy_mode == 6u);
    uint8_t rans2 = (uint8_t)(t->fh->version_major == 7u &&
                              (t->fh->entropy_mode == 7u || t->fh->entropy_mode == 8u));
    uint32_t dc_hist_vlc[TC_VLC_DC_SYMS];
    uint32_t run_hist_vlc[TC_VLC_RUN_SYMS];
    uint32_t lvl_hist_vlc[TC_VLC_LVL_SYMS];
    uint32_t* dc_hist_p = (vlc != 0u || rans != 0u || rans2 != 0u) ? dc_hist_vlc : NULL;
    uint32_t* run_hist_p = (vlc != 0u || rans != 0u || rans2 != 0u) ? run_hist_vlc : NULL;
    uint32_t* lvl_hist_p = (vlc != 0u || rans != 0u || rans2 != 0u) ? lvl_hist_vlc : NULL;
    if (dc_hist_p != NULL) {
        memset(dc_hist_vlc, 0, sizeof(dc_hist_vlc));
        memset(run_hist_vlc, 0, sizeof(run_hist_vlc));
        memset(lvl_hist_vlc, 0, sizeof(lvl_hist_vlc));
    }
    t->rc = fill_color_band_from_f(t->fh, t->fplane, t->plane, t->qctx, &s->tok,
                           s->dc_a, s->dc_b, t->y0, t->h, &k1, &k2, &k3,
                           dc_hist_p, run_hist_p, lvl_hist_p, t->qp_eff,
                           (vlc != 0 && t->rdo != 0) ? 1 : 0);
    if (t->rc != TC_OK) { return; }
    if (rans2 != 0u) {
        /* 精确字节探针（与最终编码同路径逐字节一致；模型选择在内） */
        size_t payload = 0u;
        t->rc = color_tokens_rans2_encode(&s->tok, (size_t)t->h * (size_t)cols,
                                          dc_hist_vlc, run_hist_vlc, lvl_hist_vlc,
                                          NULL, &payload, t->es, wslot);
        if (t->rc == TC_OK) {
            t->slice_bytes = (uint64_t)TC_SLICE_HEADER_SIZE + (uint64_t)payload;
        }
        if (prof) { t->fill_ns = tc_profile_now_ns() - t0; }
        return;
    }
    if (rans != 0u) {
        /* 精确字节探针：真实后向编码（与最终编码同路径，逐字节一致） */
        size_t payload = 0u;
        t->rc = color_tokens_rans_encode(&s->tok, (size_t)t->h * (size_t)cols,
                                         dc_hist_vlc, run_hist_vlc, lvl_hist_vlc,
                                         NULL, &payload, t->es, wslot);
        if (t->rc == TC_OK) {
            t->slice_bytes = (uint64_t)TC_SLICE_HEADER_SIZE + (uint64_t)payload;
        }
        if (prof) { t->fill_ns = tc_profile_now_ns() - t0; }
        return;
    }
    uint64_t bits;
    if (vlc != 0u) {
        uint8_t dc_bk = 0u, lvl_bk = 0u, run_bk = 0u;
        vlc_books_from_hist(dc_hist_vlc, run_hist_vlc, lvl_hist_vlc,
                            &dc_bk, &lvl_bk, &run_bk);
        bits = color_tokens_bits_vlc(&s->tok, (size_t)t->h * (size_t)cols,
                                     dc_bk, lvl_bk, run_bk);
    } else {
        bits = color_tokens_bits(&s->tok, (size_t)t->h * (size_t)cols, k1, k2, k3);
    }
    t->slice_bytes = (uint64_t)TC_SLICE_HEADER_SIZE + ((bits + 7u) >> 3);
    if (prof) { t->fill_ns = tc_profile_now_ns() - t0; }
    t->rc = TC_OK;
}

/* exact probe：返回 qp 候选的精确 packet_size（与整帧编码逐值相等）。
 * M10-6.2：三 plane 全部 band 一次提交（原逐 plane 三轮；probe 是 sized
 * 墙钟主项（取证 54~60%），轮次 ×3 的 join/尾部失衡直接放大）。 */
static int32_t m7_probe(enc_shared* e, uint32_t qp, uint32_t* bytes_out)
{
    const tc_qmatrix_set* qms = lookup_qm(e->fh.qmatrix_id);
    if (qms == NULL) {
        tc_set_error(TC_ERR_UNSUPPORTED_MATRIX, "qmatrix_id %u", (unsigned)e->fh.qmatrix_id);
        return TC_ERR_UNSUPPORTED_MATRIX;
    }
    uint64_t frame = (uint64_t)TC_FRAME_HEADER_SIZE + (uint64_t)e->alpha_payload +
                     (uint64_t)e->alpha_hdr_bytes;

    tc_quant_ctx qctx[4];
    int32_t qp_effs[4];
    uint32_t max_bands = 1u;
    for (uint32_t p = 0u; p < fh_color_planes(&e->fh); ++p) {
        const uint16_t* qm = is_chroma_plane(&e->fh, p) ? qms->chroma : qms->luma;
        const int32_t delta = is_chroma_plane(&e->fh, p) ? e->qp_delta_chroma
                                                         : e->qp_delta_luma;
        int32_t qp_eff = (int32_t)qp + delta + bd_qp_offset(e->fh.bit_depth);
        if (qp_eff < 0) { qp_eff = 0; }
        {   /* v1.5：同 frame_encode_shared 的条件钳位（候选 qp 即 qp_base） */
            const int32_t ceiling = (int32_t)tc_qp_eff_ceiling((uint32_t)qp);
            if (qp_eff > ceiling) { qp_eff = ceiling; }
        }
        qp_effs[p] = qp_eff;
        tc_quant_ctx_init_bd_tbl(&qctx[p], qm, (uint32_t)qp_eff, e->fh.bit_depth,
                                 tc_qtbl_of_flags(e->fh.flags));
        uint32_t rows = e->fh.plane_block_rows[p];
        uint32_t bands = (rows + e->slice_rows - 1u) / e->slice_rows;
        if (bands == 0u) { bands = 1u; }
        if (bands > max_bands) { max_bands = bands; }
    }

    uint32_t nw_alloc = (uint32_t)tc_dev_thread_count();
    if (nw_alloc > max_bands) { nw_alloc = max_bands; }
    if (nw_alloc > (uint32_t)TC_SLICE_MAX_THREADS) { nw_alloc = (uint32_t)TC_SLICE_MAX_THREADS; }
    if (nw_alloc > e->slot_n) { nw_alloc = e->slot_n; }
    /* N03：tok 数组按 tok_slots_built 收敛（tok_ok=0 时探针不启用——
     * m7_prepare 已回退，此钳位是同口径防御） */
    if (e->tok_ok != 0 && nw_alloc > (uint32_t)e->tok_slots_built) {
        nw_alloc = (uint32_t)e->tok_slots_built;
    }
    if (nw_alloc < 1u) { nw_alloc = 1u; }

    /* 槽位借用（仅 token/dc 链；与 enc_encode_plane_opt 同源） */
    enc_worker_slot slots[TC_SLICE_MAX_THREADS];
    memset(slots, 0, sizeof(slots));
    for (uint32_t w = 0u; w < nw_alloc; ++w) {
        if (w == 0u) {
            slots[0].dc_a = e->dc_a;
            slots[0].dc_b = e->dc_b;
        } else {
            slots[w].dc_a = e->slot_dca[w];
            slots[w].dc_b = e->slot_dcb[w];
        }
        slots[w].tok = e->tok[w];
        slots[w].tok_ok = e->tok_ok;
    }

    /* V2.x AQ + P3：每 plane 9 档量化变体（同 enc_band_pass_all；任务只持指针） */
    tc_quant_ctx aq_v[4][9];
    if (e->aq_on != 0) {
        for (uint32_t p = 0u; p < fh_color_planes(&e->fh); ++p) {
            const uint16_t* qmp = is_chroma_plane(&e->fh, p) ? qms->chroma : qms->luma;
            for (int d = 0; d < 9; ++d) {
                int32_t qev = qp_effs[p] + (d - 4);
                if (qev < 0) { qev = 0; }
                /* v1.5：条件天花板以【候选 qp】为源（同上方主钳位 3693）。
                 * fh.qp_base 在 sized 探测期间仍是搜索起点值——按它取 63
                 * 会让 qp≥64 候选的全部偏移带回落 63 量化，探针高估 bytes、
                 * 搜索冲顶而实包欠目标（AQ-on proxy 复现落 q83/q95、
                 * 0.50/0.66×T，2026-09-11）。 */
                if ((uint32_t)qev > tc_qp_eff_ceiling(qp)) {
                    qev = (int32_t)tc_qp_eff_ceiling(qp);
                }
                tc_quant_ctx_init_bd_tbl(&aq_v[p][d], qmp, (uint32_t)qev, e->fh.bit_depth,
                                         tc_qtbl_of_flags(e->fh.flags));
            }
        }
    }

    m7_probe_task tasks[TC_MAX_SLICES];
    tc_job jobs[TC_MAX_SLICES];
    uint32_t ti = 0u;
    for (uint32_t p = 0u; p < fh_color_planes(&e->fh); ++p) {
        uint32_t rows = e->fh.plane_block_rows[p];
        uint32_t bands = (rows + e->slice_rows - 1u) / e->slice_rows;
        if (bands == 0u) { bands = 1u; }
        for (uint32_t y0 = 0u; y0 < rows && ti < TC_MAX_SLICES;
             y0 += e->slice_rows, ++ti) {
            uint32_t h = rows - y0;
            if (h > e->slice_rows) { h = e->slice_rows; }
            tasks[ti].fh = &e->fh;
            tasks[ti].plane = p;
            tasks[ti].y0 = y0;
            tasks[ti].h = h;
            tasks[ti].qctx = &qctx[p];
            tasks[ti].qp_eff = (uint32_t)qp_effs[p];
            tasks[ti].rdo = e->rdo_on;
            if (e->aq_on != 0) {
                /* V2.x AQ：与最终编码同源的逐带偏移（sized 搜索口径一致） */
                int32_t aq_off_v = e->aq_off[p][y0 / e->slice_rows];
                if (aq_off_v != 0) {
                    int d = aq_off_v + 4;
                    if (d < 0) { d = 0; }
                    if (d > 8) { d = 8; }
                    tasks[ti].qctx = &aq_v[p][d];
                    int32_t qe = qp_effs[p] + aq_off_v;
                    if (qe < 0) { qe = 0; }
                    /* 同上：候选 qp 为天花板源（fh.qp_base 此时非最终值） */
                    if ((uint32_t)qe > tc_qp_eff_ceiling(qp)) {
                        qe = (int32_t)tc_qp_eff_ceiling(qp);
                    }
                    tasks[ti].qp_eff = (uint32_t)qe;
                }
            }
            tasks[ti].slots_base = slots;
            tasks[ti].nw = nw_alloc;
            tasks[ti].slot = NULL;
            tasks[ti].es = e; /* C1：探针与最终编码共用槽位 scratch 池 */
            tasks[ti].fplane = e->fcache + e->fcache_off[p] * 64u;
            tasks[ti].slice_bytes = 0u;
            tasks[ti].fill_ns = 0u;
            tasks[ti].rc = TC_OK;
            jobs[ti].fn = m7_probe_job;
            jobs[ti].ctx = &tasks[ti];
        }
    }
    (void)tc_parallel_for(jobs, ti, nw_alloc);
    if (tc_profile_enabled()) {
        TC_STATS_LOCK();
        for (uint32_t i = 0u; i < ti; ++i) {
            g_enc_stats.probe_fill_ns += tasks[i].fill_ns;
        }
        TC_STATS_UNLOCK();
    }
    for (uint32_t i = 0u; i < ti; ++i) {
        if (tasks[i].rc != TC_OK) { return tasks[i].rc; }
        frame += tasks[i].slice_bytes;
    }
    *bytes_out = (uint32_t)frame;
    return TC_OK;
}

/* final：选定 qp 的全量编码（F-cache 量化 + alpha 复用 → out/stats）。
 * 输出与 frame_encode_shared(同 qp) 逐字节一致。 */
static int32_t m7_final(enc_shared* e, uint32_t qp, uint8_t* out, size_t out_cap,
                        topos_frame_stats* stats)
{
    topos_frame_stats local;
    if (stats == NULL) { stats = &local; }
    memset(stats, 0, sizeof(*stats));
    stats->struct_size = (uint32_t)sizeof(topos_frame_stats);
    stats->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;

    e->fh.qp_base = (uint8_t)qp;
    enc_fh_select_minor(&e->fh);   /* v1.5：qp≥64 候选 → minor=4 同步 */
    e->color_payload = 0u;
    e->color_hdr_bytes = 0u;
    /* C04：装配全程写 grow-only 暂存，成功才一次性提交 out（原子语义；
     * ab.out/cap 跨调用保留 → 缓存复用稳态零分配） */
    e->ab.grow = 1;
    e->ab.off = TC_FRAME_HEADER_SIZE;
    e->ab.err = TC_OK;

    int32_t rc = TC_OK;
    /* M10-6.2：三 plane 一次提交（F-cache 源；不复位 alpha arena——alpha
     * payload 驻留在 chunk 链上，bump 追加不迁移已分配数据） */
    rc = enc_band_pass_all(e, fh_color_planes(&e->fh), 1, 0);
    if (rc == TC_OK) {
        for (uint32_t i = 0u; i < e->alpha_band_n && rc == TC_OK; ++i) {
            asm_append(&e->ab, e->alpha_bands[i].hdr, TC_SLICE_HEADER_SIZE);
            asm_append(&e->ab, e->alpha_bands[i].payload, e->alpha_bands[i].payload_size);
            if (e->ab.err == TC_ERR_LIMIT_EXCEEDED) { rc = e->ab.err; }
        }
    }
    if (rc == TC_OK && e->ab.err == TC_ERR_LIMIT_EXCEEDED) { rc = e->ab.err; }
    if (rc == TC_OK) {
        e->fh.frame_packet_size = (uint32_t)e->ab.off;
        if (e->ab.err == TC_OK) {
            rc = tc_frame_header_encode(&e->fh, e->ab.out); /* C04：写暂存 */
        }
    }
    if (rc == TC_OK && e->ab.err != TC_OK) { rc = e->ab.err; } /* C04：装配 OOM */
    if (rc != TC_OK) { return rc; }
    fill_stats(e, stats);
    /* C04 提交：全部成功后才拷贝到调用方 out——失败路径 out 原子不变 */
    if (e->ab.off > out_cap) {
        tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "out_cap %zu < %zu bytes needed",
                     out_cap, e->ab.off);
        return TC_ERR_BUFFER_TOO_SMALL;
    }
    if (out != NULL && e->ab.off > 0u) { memcpy(out, e->ab.out, e->ab.off); }
    return TC_OK;
}

/* ---------------- R6：encode_sized qp 搜索 ----------------
 *
 * 语义（与阶段 4/9 冻结行为一致，差分测试 test_r6 固定）：
 *   1. 起点 qp 钳入 [qp_min, qp_max] 编码一次；
 *   2. 预算超支时升 qp 至首个满足预算的点（legacy 为粗 +8 阶梯 + 细 +1）；
 *   3. 用量 < 75% 预算时回收画质（qp−1 逐级试探，失败恢复上一可用输出）。
 *
 * R6 快速路径：bytes(qp) 随 qp 单调不增（qp_scale 每 4 qp 翻倍的码率结构），
 * 以少量插值探测确定两个边界——fit 边界（首个 bytes ≤ target）与 75% 用量
 * 边界——再按 legacy 的阶梯/回收语义闭式推导最终 qp。任何观测到的单调性
 * 破坏（k 估计翻转导致的尺寸抖动）、探测预算耗尽或 legacy 迭代数会触及
 * max_iters 上限时，整体回退 legacy 线性搜索重跑（结果与旧行为逐字节一致）。
 *
 * ---------------- M10-7C：strict 目标搜索（mode 0，新默认） ----------------
 *
 * 语义变更（计划 §7C，非 bit 兼容；legacy/R6 永久保留为 oracle/回退）：
 *   - 取消 75% 提前停止：取 [qp_min, qp_max] 内 bytes(q) ≤ target 的最小 q
 *     （fit 在高 q 侧：bytes 随 q 单调不增）；全域超限 → qp_max best-effort；
 *   - 探测策略：qp_scale 对数斜率跳步（默认 s=4，两点后实测修正）+ 括号
 *     二分。括号不变量：hi = 最大已测超限 < q* ≤ lo = 最小已测命中，
 *     q* ∈ (hi, lo]；解出条件 lo == hi+1（或 lo == qp_min 未见过超限）——
 *     最小性由构造成立（q*−1 已实测超限，或达 qp_min）；
 *   - 单调性破坏 / 预算耗尽 / m7 F-cache 不可用 → 回退 legacy 线性
 *     （与旧行为逐字节一致）。真实素材取证：R6/legacy 6~10 probe/帧是
 *     sized 墙钟主项（54~60%）；strict 目标 p50 ≤ 4。 */

static atomic_int s_sized_legacy_env_read = 0; /* P1-06 */

/* R6 观测探针（dev/test only）：sized 搜索的整帧编码次数累计——
 * bench/ADR 迭代削减证据（每次 tc_frame_encode_sized 计 1 次调用）。 */
static _Atomic uint64_t s_sized_iters = 0u; /* P1-06 */
static _Atomic uint64_t s_sized_calls = 0u;

void tc_dev_sized_reset(void)
{
    atomic_store_explicit(&s_sized_iters, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_sized_calls, 0u, memory_order_relaxed);
}

uint64_t tc_dev_sized_iters(void)
{
    return atomic_load_explicit(&s_sized_iters, memory_order_relaxed);
}
uint64_t tc_dev_sized_calls(void)
{
    return atomic_load_explicit(&s_sized_calls, memory_order_relaxed);
}

static void sized_count_encode(void)
{
    atomic_fetch_add_explicit(&s_sized_iters, 1u, memory_order_relaxed);
}

/* 搜索模式（M10-7C）：0 = strict（新默认）；1 = legacy 线性（oracle/回退）；
 * 2 = R6 快速（与 legacy 逐字节差分对，test_r6 固定）。 */
static atomic_int s_sized_mode = 0; /* P1-06 */

void tc_dev_set_sized_mode(int32_t mode)
{
    atomic_store_explicit(&s_sized_legacy_env_read, 1,
                          memory_order_relaxed); /* 显式设置后不再读 env */
    if (mode < 0) { mode = 0; }
    if (mode > 2) { mode = 0; }
    atomic_store_explicit(&s_sized_mode, mode, memory_order_relaxed);
}

/* 旧 dev 入口（R6 期语义 fast/legacy 二值）：非 0 → legacy；0 → 当前默认
 * （M10-7C 起为 strict）。新代码/测试用 tc_dev_set_sized_mode。 */
void tc_dev_set_sized_search(int legacy)
{
    tc_dev_set_sized_mode(legacy ? 1 : 0);
}

/* M10-6.3A：跨帧 qp 提示开关（默认启用；差分对拍/应急回退）。 */
static atomic_int s_qp_hint_disable = 0; /* P1-06 */

void tc_dev_set_qp_hint(int disable)
{
    atomic_store_explicit(&s_qp_hint_disable, disable ? 1 : 0,
                          memory_order_relaxed);
}

/* C2（速度计划 v2）：hint 模型强化开关（bytes/斜率外推；默认启用）。
 * 仅差分对拍用——关闭时回退 M10-6.3A 语义（种子 = 裸 q*，先验 4.0）。 */
static atomic_int s_qp_hint_model_disable = 0;

void tc_dev_set_qp_hint_model(int disable)
{
    atomic_store_explicit(&s_qp_hint_model_disable, disable ? 1 : 0,
                          memory_order_relaxed);
}

/* M7：sized 搜索的 DCT-once/exact-probe 路径开关（dev/test 差分与回退） */
static atomic_int s_m7_disable = 0; /* P1-06 */

void tc_dev_set_sized_m7(int disable)
{
    atomic_store_explicit(&s_m7_disable, disable ? 1 : 0,
                               memory_order_relaxed);
}

static void sized_legacy_env_init(void)
{
    if (atomic_load_explicit(&s_sized_legacy_env_read, memory_order_relaxed) != 0) { return; }
    atomic_store_explicit(&s_sized_legacy_env_read, 1, memory_order_relaxed);
    const char* env = getenv("TOPOS_SIZED_SEARCH");
    if (env == NULL) { return; }
    if (strcmp(env, "linear") == 0) {
        atomic_store_explicit(&s_sized_mode, 1, memory_order_relaxed);
    } else if (strcmp(env, "r6") == 0) {
        atomic_store_explicit(&s_sized_mode, 2, memory_order_relaxed);
    } else if (strcmp(env, "strict") == 0) {
        atomic_store_explicit(&s_sized_mode, 0, memory_order_relaxed);
    }
}

/* legacy 线性搜索：与 R5 及之前完全相同的循环体（差分基准 + 回退路径） */
/* C05 前置声明：探测只取字节数（定义见 sized_probe_ctx 之后） */
static int32_t sized_probe_bytes(enc_shared* e, const topos_frame_config* cfg,
                                 const topos_frame_input* input,
                                 topos_frame_stats* sp);

static int32_t sized_search_linear(enc_shared* e, const topos_frame_config* cfg,
                                   const topos_frame_input* input, uint32_t target_bytes,
                                   uint8_t qp_min, uint8_t qp_max, uint8_t* qp_used,
                                   uint8_t* out, size_t out_cap, topos_frame_stats* sp)
{
    topos_frame_config c = *cfg;
    uint32_t qp = c.qp_base;
    if (qp < (uint32_t)qp_min) { qp = (uint32_t)qp_min; }
    if (qp > (uint32_t)qp_max) { qp = (uint32_t)qp_max; }
    uint32_t iters = 0u;
    const uint32_t max_iters = 24u;

    /* C05：探测写内部暂存只计字节数——首个/中间 QP 超 out_cap 的
     * TC_ERR_BUFFER_TOO_SMALL 不再伪装成终止条件（历史缺陷：QP20 探测
     * 超容量即返回 -15，后续 QP 可命中也不尝试）。容量约束只对最终
     * 选包应用（尾部提交）。探测/恢复语义与 legacy 逐字节一致。 */
    c.qp_base = (uint8_t)qp;
    int32_t rc = sized_probe_bytes(e, &c, input, sp);
    sized_count_encode();
    iters++;
    while (rc >= 0 && sp->packet_size > target_bytes && qp + 8u <= qp_max &&
           iters < max_iters) {
        qp += 8u;
        c.qp_base = (uint8_t)qp;
        rc = sized_probe_bytes(e, &c, input, sp);
        sized_count_encode();
        iters++;
    }
    while (rc >= 0 && sp->packet_size > target_bytes && qp < qp_max && iters < max_iters) {
        qp += 1u;
        c.qp_base = (uint8_t)qp;
        rc = sized_probe_bytes(e, &c, input, sp);
        sized_count_encode();
        iters++;
    }
    while (rc >= 0 && sp->packet_size <= target_bytes &&
           (uint64_t)sp->packet_size * 4u < (uint64_t)target_bytes * 3u &&
           qp > qp_min && iters < max_iters) {
        uint32_t cand = qp - 1u;
        c.qp_base = (uint8_t)cand;
        int32_t rc2 = sized_probe_bytes(e, &c, input, sp);
        sized_count_encode();
        iters++;
        if (rc2 >= 0 && sp->packet_size <= target_bytes) {
            qp = cand;
            rc = rc2;
            continue;
        }
        c.qp_base = (uint8_t)qp;
        rc = sized_probe_bytes(e, &c, input, sp); /* 恢复：暂存回到 qp 的包 */
        sized_count_encode();
        iters++;
        break;
    }
    /* 尾部提交：最后一次成功探测的包已在暂存（qp 未再变，编码器确定性
     * → 与直接编码逐字节一致）；超 out_cap 才返回 -15，out 原子不变。 */
    if (rc >= 0) {
        if (sp->packet_size > out_cap) {
            tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "out_cap %zu < %zu bytes needed",
                         out_cap, (size_t)sp->packet_size);
            rc = TC_ERR_BUFFER_TOO_SMALL;
        } else if (out != NULL && sp->packet_size > 0u) {
            memcpy(out, e->ab.out, sp->packet_size);
        }
    }
    if (qp_used != NULL) { *qp_used = (uint8_t)qp; }
    return rc;
}

#define SIZED_MAX_PTS 24
#define SIZED_PROBE_BUDGET 24u /* 快速路径整帧编码上限（≈ legacy max_iters；超出回退） */

typedef struct sized_probe_ctx {
    enc_shared* e;
    const topos_frame_config* cfg;
    const topos_frame_input* input;
    uint8_t* out;
    size_t out_cap;
    topos_frame_stats* sp;
    uint32_t target;
    uint8_t qp_min;
    uint8_t qp_max;
    uint32_t pts_qp[SIZED_MAX_PTS];
    uint32_t pts_bytes[SIZED_MAX_PTS];
    uint32_t npts;
    uint32_t budget;
    int violated;   /* 单调性破坏或预算耗尽 → 回退 legacy */
    uint32_t last_qp;
    int has_last;
    int m7;         /* M7：probe 走 F-cache 精确位计数（final 走 m7_final） */
} sized_probe_ctx;

/* C05：探测只取字节数——写内部 grow-only 暂存（out=NULL/0），任何 QP 的
 * 容量错误不再终止搜索（sp->packet_size 在暂存装配后即为精确字节数）；
 * 容量约束只对最终选包应用（见 sized_commit_out / linear 尾部提交）。 */
static int32_t sized_probe_bytes(enc_shared* e, const topos_frame_config* cfg,
                                 const topos_frame_input* input,
                                 topos_frame_stats* sp)
{
    int32_t rc = frame_encode_shared(e, cfg, input, NULL, 0u, sp);
    if (rc == TC_ERR_BUFFER_TOO_SMALL) { rc = TC_OK; } /* 暂存装配成功，字节数在 sp */
    return rc;
}

static int32_t sized_encode_at(sized_probe_ctx* c, uint32_t qp)
{
    if (c->budget == 0u) { c->violated = 1; return TC_ERR_LIMIT_EXCEEDED; }
    if (c->m7 != 0) {
        /* M7 exact probe：F-cache quant+token+精确位计数 → packet_size 逐值
         * 等于整帧编码（搜索决策不变）；out 不写（final 统一落盘） */
        uint32_t bytes = 0u;
        const int prof = tc_profile_enabled();
        uint64_t tw = prof ? tc_profile_now_ns() : 0u;
        int32_t rc = m7_probe(c->e, qp, &bytes);
        if (prof) { TC_STATS_LOCK(); g_enc_stats.probe_wall_ns += tc_profile_now_ns() - tw; TC_STATS_UNLOCK(); }
        sized_count_encode();
        c->budget--;
        c->last_qp = qp;
        c->has_last = 1;
        if (rc < 0) { return rc; }
        c->sp->packet_size = bytes;
        for (uint32_t i = 0u; i < c->npts; ++i) {
            if (c->pts_qp[i] < qp && c->pts_bytes[i] < bytes) { c->violated = 1; }
            if (c->pts_qp[i] > qp && c->pts_bytes[i] > bytes) { c->violated = 1; }
        }
        c->pts_qp[c->npts] = qp;
        c->pts_bytes[c->npts] = bytes;
        c->npts++;
        return TC_OK;
    }
    topos_frame_config cc = *c->cfg;
    cc.qp_base = (uint8_t)qp;
    /* C05：整帧探测走暂存（out=NULL/0）——中间 QP 超容量不再伪装成
     * 终止错误；首次候选超容量但后续 QP 可命中时搜索继续。 */
    int32_t rc = sized_probe_bytes(c->e, &cc, c->input, c->sp);
    sized_count_encode();
    c->budget--;
    c->last_qp = qp;
    c->has_last = 1;
    if (rc < 0) { return rc; }
    uint32_t b = c->sp->packet_size;
    for (uint32_t i = 0u; i < c->npts; ++i) {
        if (c->pts_qp[i] < qp && c->pts_bytes[i] < b) { c->violated = 1; }
        if (c->pts_qp[i] > qp && c->pts_bytes[i] > b) { c->violated = 1; }
    }
    c->pts_qp[c->npts] = qp;
    c->pts_bytes[c->npts] = b;
    c->npts++;
    return TC_OK;
}

/* C05：最终选包提交——把暂存中最后一次成功探测的包（qp == q_final）
 * 拷贝进调用方 out 并应用 out_cap；最终包超容量时才返回
 * TC_ERR_BUFFER_TOO_SMALL（out 字节原子不变，C04 哨兵语义）。 */
static int32_t sized_commit_out(const sized_probe_ctx* c)
{
    if (c->sp->packet_size > c->out_cap) {
        tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "out_cap %zu < %zu bytes needed",
                     c->out_cap, (size_t)c->sp->packet_size);
        return TC_ERR_BUFFER_TOO_SMALL;
    }
    if (c->out != NULL && c->sp->packet_size > 0u) {
        memcpy(c->out, c->e->ab.out, c->sp->packet_size);
    }
    return TC_OK;
}

static int sized_bytes_at(const sized_probe_ctx* c, uint32_t qp, uint32_t* bytes_out)
{
    for (uint32_t i = 0u; i < c->npts; ++i) {
        if (c->pts_qp[i] == qp) { *bytes_out = c->pts_bytes[i]; return 1; }
    }
    return 0;
}

/* 插值步长：qp_scale 每 4 qp 减半 → bytes ≈ B·2^(−Δq/s)，s∈[1,16] */
static double sized_fit_slope(const sized_probe_ctx* c)
{
    double s = 4.0;
    for (uint32_t i = c->npts; i-- > 1u;) { /* pts 追加序：从最近往回找相邻 over 对 */
        uint32_t q1 = c->pts_qp[i], b1 = c->pts_bytes[i];
        for (uint32_t j = i; j-- > 0u;) {
            uint32_t q0 = c->pts_qp[j], b0 = c->pts_bytes[j];
            if (q1 > q0 && b0 > b1 && b0 > 0u) {
                double dq = (double)(q1 - q0);
                double dr = log2((double)b0 / (double)b1);
                if (dr > 0.05) {
                    double si = dq / dr;
                    if (si < 1.0) { si = 1.0; }
                    if (si > 16.0) { si = 16.0; }
                    return si;
                }
            }
        }
    }
    return s;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

static int32_t sized_search_fast(enc_shared* e, const topos_frame_config* cfg,
                                 const topos_frame_input* input, uint32_t target,
                                 uint8_t qp_min, uint8_t qp_max, uint8_t* qp_used,
                                 uint8_t* out, size_t out_cap, topos_frame_stats* sp,
                                 int* fallback)
{
    sized_probe_ctx c;
    memset(&c, 0, sizeof(c));
    c.e = e; c.cfg = cfg; c.input = input; c.out = out; c.out_cap = out_cap; c.sp = sp;
    c.target = target; c.qp_min = qp_min; c.qp_max = qp_max;
    c.budget = SIZED_PROBE_BUDGET;

    /* M7：DCT-once 准备（F-cache + alpha 一次编码）。失败/超限/禁用 →
     * 整帧编码 probe 旧路径（决策与输出不变）。 */
    if (atomic_load_explicit(&s_m7_disable, memory_order_relaxed) == 0) {
        const int prof = tc_profile_enabled();
        uint64_t tw = prof ? tc_profile_now_ns() : 0u;
        int32_t prc = m7_prepare(e, cfg, input);
        if (prof) { TC_STATS_LOCK(); g_enc_stats.prep_wall_ns += tc_profile_now_ns() - tw; TC_STATS_UNLOCK(); }
        if (prc != TC_OK) { return prc; }
        if (e->fcache_ok != 0) { c.m7 = 1; }
    }

    uint32_t q0 = cfg->qp_base;
    if (q0 < (uint32_t)qp_min) { q0 = (uint32_t)qp_min; }
    if (q0 > (uint32_t)qp_max) { q0 = (uint32_t)qp_max; }
    uint32_t q_final = q0; /* 早退路径（编码错误）下作为 qp_used 兜底 */

    int32_t rc = sized_encode_at(&c, q0);
    if (rc < 0 || c.violated) { goto done; }
    uint32_t qp = q0;
    uint32_t iters = 1u;

    /* ---- 阶段 1：精确复刻 legacy 升阶梯（粗 +8 / 细 +1；max_iters 语义同）----
     * 探测点与 legacy 完全一致：结果恒等，且阶梯点位于高 qp（编码便宜）——
     * 模型跳步会把探测集中到边界低 qp 区（编码最贵），窄边界场景反而更慢
     * （T0/T1 实测教训，ADR-C022 D-2 修正）。快速化只作用于回收阶段。 */
    while (rc >= 0 && sp->packet_size > target && qp + 8u <= qp_max && iters < 24u) {
        qp += 8u;
        rc = sized_encode_at(&c, qp);
        if (rc < 0 || c.violated) { goto done; }
        iters++;
    }
    while (rc >= 0 && sp->packet_size > target && qp < qp_max && iters < 24u) {
        qp += 1u;
        rc = sized_encode_at(&c, qp);
        if (rc < 0 || c.violated) { goto done; }
        iters++;
    }
    uint32_t q_a = qp;

    /* ---- 阶段 2：回收边界（P(q) = bytes(q)·4 < target·3，大 q 为真）----
     * legacy 从 q_a 逐级 −1 接受，停在首个 NOT P； unfit 候选触发恢复 +1。
     * 快速路径：插值 + 二分直接定位 q_c = max{q ≤ q_a : NOT P(q)}（bytes
     * 单调不增 ⇒ P 单调），以 ~2-3 次探测替代 q_a − q_c 次逐级编码。 */
    q_final = q_a;
    int restore_extra = 0;
    {
        uint32_t ba = 0;
        (void)sized_bytes_at(&c, q_a, &ba);
        int fits_a = ba <= target;
        int p_a = (uint64_t)ba * 4u < (uint64_t)target * 3u;
        if (fits_a && p_a && q_a > qp_min && iters < 24u) {
            uint32_t lo = q_a;      /* P 真（已测） */
            uint32_t hi = 0;        /* NOT P 哨兵 UNKNOWN */
            int have_notp = 0;
            for (;;) {
                uint32_t next;
                if (!have_notp) {
                    double dist = sized_fit_slope(&c) *
                                  log2((0.75 * (double)target) / (double)ba);
                    if (dist < 0.0) { dist = 0.0; }
                    uint32_t d = (uint32_t)(dist + 0.999);
                    next = clamp_u32(lo - d - 1u, qp_min, lo - 1u);
                    /* 首探直接外推；之后在 [hi, lo) 上按区间二分 */
                } else {
                    next = (hi + lo) / 2u; /* hi < lo：NOT P 下界侧 */
                    if (next == hi) { next = hi + 1u; }
                    if (next >= lo) { next = lo - 1u; }
                }
                rc = sized_encode_at(&c, next);
                if (rc < 0 || c.violated) { goto done; }
                uint32_t bn = 0;
                (void)sized_bytes_at(&c, next, &bn);
                int p_n = (uint64_t)bn * 4u < (uint64_t)target * 3u;
                if (p_n) {
                    lo = next;
                    ba = bn;
                    if (lo == qp_min) { break; } /* 到下界仍 P：legacy 接受到 qp_min */
                } else {
                    have_notp = 1;
                    hi = next;
                    if (hi + 1u == lo) { break; } /* 边界相邻：hi = max NOT P */
                }
                if (have_notp && hi + 1u >= lo) { break; }
                if (!have_notp && lo == qp_min) { break; }
            }
            if (have_notp) {
                uint32_t bc = 0;
                (void)sized_bytes_at(&c, hi, &bc);
                if (bc <= target) {
                    q_final = hi;
                } else {
                    q_final = hi + 1u; /* unfit → legacy 恢复上一可用（探测+恢复共 2 次编码） */
                    restore_extra = 2;
                }
            } else {
                q_final = qp_min;
            }
        }
    }

    /* ---- 阶段 3：legacy 迭代计数复核（触及 max_iters → 回退保真）----
     * 升阶编码数 = iters（与 legacy 相同）；回收若走 legacy 将逐级编码
     * (q_a − q_final) 次 + 恢复额外 restore_extra 次。 */
    {
        uint32_t iters_check = iters;
        if (q_final < q_a) { iters_check += q_a - q_final; }
        iters_check += (uint32_t)restore_extra;
        if (iters_check > 24u) { c.violated = 1; goto done; }
    }

    /* ---- 阶段 4：最终包落盘（out/sp 必须对应 q_final） ---- */
    if (c.m7 != 0) {
        /* M7：probe 从不写 out → 统一从 F-cache 全量落盘一次（与
         * frame_encode_shared(q_final) 逐字节一致；alpha 复用缓存） */
        const int prof = tc_profile_enabled();
        uint64_t tw = prof ? tc_profile_now_ns() : 0u;
        rc = m7_final(c.e, q_final, out, out_cap, sp);
        if (prof) { TC_STATS_LOCK(); g_enc_stats.final_wall_ns += tc_profile_now_ns() - tw; TC_STATS_UNLOCK(); }
        sized_count_encode();
        if (rc < 0) { goto done; }
    } else {
        /* C05：非 M7 探测走暂存——最终包只在缺探测时补编一次，然后
         * 统一提交调用方 out（超容量才返回 -15，out 原子不变） */
        if (!c.has_last || c.last_qp != q_final) {
            rc = sized_encode_at(&c, q_final);
            if (rc < 0 || c.violated) { goto done; }
        }
        rc = sized_commit_out(&c);
        if (rc < 0) { goto done; }
    }

done:
    if (c.violated != 0) { *fallback = 1; return TC_OK; /* 占位；调用方回退重跑 */ }
    if (qp_used != NULL) { *qp_used = (uint8_t)q_final; }
    return rc;
}

/* M10-7C：实测对数斜率（最近一对 q 升 bytes 降的探测点）；无有效对返回 def */
static double strict_slope_from_pts(const sized_probe_ctx* c, double def)
{
    for (uint32_t i = c->npts; i-- > 1u;) {
        uint32_t q1 = c->pts_qp[i], b1 = c->pts_bytes[i];
        for (uint32_t j = i; j-- > 0u;) {
            uint32_t q0 = c->pts_qp[j], b0 = c->pts_bytes[j];
            if (q1 > q0 && b0 > b1 && b1 > 0u) {
                double dr = log2((double)b0 / (double)b1);
                if (dr > 0.05) {
                    double s = (double)(q1 - q0) / dr;
                    if (s < 1.0) { s = 1.0; }
                    if (s > 16.0) { s = 16.0; }
                    return s;
                }
            }
        }
    }
    return def;
}

/* M10-7C strict：最小命中 QP 搜索（新默认）。括号不变量（fit 在高 q 侧）：
 * hi = 最大已测超限（< q*），lo = 最小已测命中（≥ q*），q* ∈ (hi, lo]。
 * 解出：lo == hi+1（q*−1 已实测超限）或 lo == qp_min（未见过超限）；
 * 全域超限（qp_max 超限且无命中）→ qp_max best-effort（同 legacy 语义）。 */
static int32_t sized_search_strict(enc_shared* e, const topos_frame_config* cfg,
                                   const topos_frame_input* input, uint32_t target,
                                   uint8_t qp_min, uint8_t qp_max, uint8_t* qp_used,
                                   uint8_t* out, size_t out_cap, topos_frame_stats* sp,
                                   int* fallback)
{
    sized_probe_ctx c;
    memset(&c, 0, sizeof(c));
    c.e = e; c.cfg = cfg; c.input = input; c.out = out; c.out_cap = out_cap; c.sp = sp;
    c.target = target; c.qp_min = qp_min; c.qp_max = qp_max;
    c.budget = SIZED_PROBE_BUDGET;

    /* strict 依赖 m7 精确位计数（probe 不落盘）；不可用 → legacy 线性 */
    if (atomic_load_explicit(&s_m7_disable, memory_order_relaxed) == 0) {
        const int prof = tc_profile_enabled();
        uint64_t tw = prof ? tc_profile_now_ns() : 0u;
        int32_t prc = m7_prepare(e, cfg, input);
        if (prof) { TC_STATS_LOCK(); g_enc_stats.prep_wall_ns += tc_profile_now_ns() - tw; TC_STATS_UNLOCK(); }
        if (prc != TC_OK) { return prc; }
        if (e->fcache_ok != 0) { c.m7 = 1; }
    }
    if (c.m7 == 0) { *fallback = 1; return TC_OK; }

    /* 首探种子：M10-6.3A 跨帧提示优先于 qp_base（几何/目标变化时种子
     * 偏远也只是多探几次，不改变收敛结果）。
     * C2 模型强化：双工作点弦线外推——最近两个相异 (q*, bytes) 的
     * log-linear 弦与目标字节求交即为新工作点估计（target == 上次
     * target 时外推项为 0，种子退化为裸 q*，稳态行为与 M10-6.3A 一致）。 */
    uint32_t q0 = cfg->qp_base;
    if (atomic_load_explicit(&s_qp_hint_disable, memory_order_relaxed) == 0 &&
        e->qp_hint_valid != 0) {
        q0 = e->qp_hint;
        if (atomic_load_explicit(&s_qp_hint_model_disable, memory_order_relaxed) == 0 &&
            e->qp_hint_bytes > 0u && target > 0u) {
            double dr = log2((double)e->qp_hint_bytes / (double)target);
            double slope = 0.0;
            if (e->qp_hint_prev_bytes > 0u && e->qp_hint_prev != e->qp_hint) {
                /* 弦斜率：q 大 ↔ bytes 小（单调已验的两次工作点） */
                const uint32_t q_hi = e->qp_hint > e->qp_hint_prev ? e->qp_hint
                                                                    : e->qp_hint_prev;
                const uint32_t q_lo = q_hi == e->qp_hint ? e->qp_hint_prev : e->qp_hint;
                const uint32_t b_hi = q_hi == e->qp_hint ? e->qp_hint_bytes
                                                         : e->qp_hint_prev_bytes;
                const uint32_t b_lo = q_hi == e->qp_hint ? e->qp_hint_prev_bytes
                                                         : e->qp_hint_bytes;
                if (q_hi > q_lo && b_lo > b_hi && b_hi > 0u) {
                    slope = (double)(q_hi - q_lo) / log2((double)b_lo / (double)b_hi);
                }
            }
            if (slope > 0.0) {
                if (slope < 0.25) { slope = 0.25; }
                if (slope > 32.0) { slope = 32.0; }
            } else {
                slope = 8.0; /* 单点历史：保守中位斜率（仅外推用，不影响解） */
            }
            double dq = slope * dr;
            if (dq > 32.0) { dq = 32.0; }
            if (dq < -32.0) { dq = -32.0; }
            const double q_seed = (double)q0 + dq;
            q0 = q_seed > 0.0 ? (uint32_t)(q_seed + 0.5) : 0u;
        }
    }
    if (q0 < (uint32_t)qp_min) { q0 = (uint32_t)qp_min; }
    if (q0 > (uint32_t)qp_max) { q0 = (uint32_t)qp_max; }

    int have_lo = 0, have_hi = 0; /* lo=最小命中；hi=最大超限 */
    uint32_t lo = 0u, hi = 0u;
    uint32_t q_final = (uint32_t)qp_max;

    /* C2：首跳先验 = 双工作点弦斜率（模型关/无弦历史 → 4.0，同旧版；
     * 曾试无历史时提先验至 10——4K 实测冷启动 8→10 探（过冲括号更宽，
     * 插值步更多），固定 4.0 的欠冲逐步括号反而更稳，回退）。 */
    double prior_slope = 4.0;
    if (atomic_load_explicit(&s_qp_hint_model_disable, memory_order_relaxed) == 0 &&
        e->qp_hint_bytes > 0u && e->qp_hint_prev_bytes > 0u &&
        e->qp_hint_prev != e->qp_hint) {
        const uint32_t q_hi = e->qp_hint > e->qp_hint_prev ? e->qp_hint : e->qp_hint_prev;
        const uint32_t q_lo = q_hi == e->qp_hint ? e->qp_hint_prev : e->qp_hint;
        const uint32_t b_hi = q_hi == e->qp_hint ? e->qp_hint_bytes : e->qp_hint_prev_bytes;
        const uint32_t b_lo = q_hi == e->qp_hint ? e->qp_hint_prev_bytes : e->qp_hint_bytes;
        if (q_hi > q_lo && b_lo > b_hi && b_hi > 0u) {
            double s = (double)(q_hi - q_lo) / log2((double)b_lo / (double)b_hi);
            if (s < 1.0) { s = 1.0; }
            if (s > 16.0) { s = 16.0; }
            prior_slope = s;
        }
    }

    int32_t rc = sized_encode_at(&c, q0);
    if (rc < 0 || c.violated) { goto done; }
    {
        uint32_t b0 = 0u;
        (void)sized_bytes_at(&c, q0, &b0);
        if (b0 <= target) { lo = q0; have_lo = 1; }
        else { hi = q0; have_hi = 1; }
    }

    for (;;) {
        if (have_lo && (!have_hi ? (lo == (uint32_t)qp_min)
                                 : (lo == hi + 1u))) {
            q_final = lo;
            break;
        }
        if (have_hi && !have_lo && hi == (uint32_t)qp_max) {
            q_final = (uint32_t)qp_max; /* 全域超限：best-effort（同 legacy） */
            break;
        }
        if (c.budget == 0u) { c.violated = 1; break; }

        uint32_t q;
        if (!have_lo) {
            /* 只有超限：向上斜率跳步（dr>0；target==0 时直接拉满） */
            uint32_t bh = 0u;
            (void)sized_bytes_at(&c, hi, &bh);
            double dr = target > 0u && bh > target
                ? log2((double)bh / (double)target) : 16.0;
            double slope = strict_slope_from_pts(&c, prior_slope);
            uint32_t dq = (uint32_t)(slope * dr + 0.999);
            if (dq < 1u) { dq = 1u; }
            if (dq > 32u) { dq = 32u; }
            q = hi + dq;
            if (q >= (uint32_t)qp_max) { q = (uint32_t)qp_max; }
        } else if (!have_hi) {
            /* 只有命中：向下斜率跳步（取消 75% 提前停止，直探最小命中） */
            uint32_t bl = 0u;
            (void)sized_bytes_at(&c, lo, &bl);
            double dr = bl > 0u && (double)target > (double)bl
                ? log2((double)target / (double)bl) : 16.0;
            double slope = strict_slope_from_pts(&c, prior_slope);
            uint32_t dq = (uint32_t)(slope * dr + 0.999);
            if (dq < 1u) { dq = 1u; }
            if (dq > 32u) { dq = 32u; }
            q = (dq < lo) ? lo - dq : (uint32_t)qp_min;
            if (q < (uint32_t)qp_min) { q = (uint32_t)qp_min; }
            if (q >= lo) { q = lo - 1u; } /* dq≥1 下冗余防御 */
        } else {
            /* 括号 (hi, lo)：对数插值定位 bytes(q)≈target */
            uint32_t bh = 0u, bl = 0u;
            (void)sized_bytes_at(&c, hi, &bh);
            (void)sized_bytes_at(&c, lo, &bl);
            double q_pred = (double)hi;
            if (bh > 0u && bl > 0u && bh > bl) {
                q_pred = (double)hi +
                    ((double)lo - (double)hi) *
                    (log2((double)bh) - log2((double)(target > 0u ? target : 1u))) /
                    (log2((double)bh) - log2((double)bl));
            } else {
                q_pred = (double)((hi + lo) / 2u);
            }
            q = (uint32_t)(q_pred + 0.5);
            if (q <= hi) { q = hi + 1u; }
            if (q >= lo) { q = lo - 1u; }
            /* 已测点（插值命中旧探测）→ 取区间内任一未测点（中点优先） */
            {
                uint32_t dummy = 0u;
                if (sized_bytes_at(&c, q, &dummy)) {
                    uint32_t mid = (hi + lo) / 2u;
                    if (mid <= hi || mid >= lo) { mid = hi + 1u; }
                    q = mid;
                    for (; q < lo; ++q) {
                        if (!sized_bytes_at(&c, q, &dummy)) { break; }
                    }
                    if (q >= lo) { q_final = lo; break; } /* 内部全已测 → lo 即最小命中 */
                }
            }
        }

        rc = sized_encode_at(&c, q);
        if (rc < 0 || c.violated) { break; }
        uint32_t b = 0u;
        (void)sized_bytes_at(&c, q, &b);
        if (b <= target) {
            if (!have_lo || q < lo) { lo = q; }
            have_lo = 1;
        } else {
            if (!have_hi || q > hi) { hi = q; }
            have_hi = 1;
        }
    }

done:
    if (c.violated != 0) { *fallback = 1; return TC_OK; }
    if (rc < 0) { return rc; }

    {
        const int prof = tc_profile_enabled();
        uint64_t tw = prof ? tc_profile_now_ns() : 0u;
        rc = m7_final(c.e, q_final, out, out_cap, sp);
        if (prof) { TC_STATS_LOCK(); g_enc_stats.final_wall_ns += tc_profile_now_ns() - tw; TC_STATS_UNLOCK(); }
        sized_count_encode();
    }
    if (rc < 0) { return rc; }
    if (qp_used != NULL) { *qp_used = (uint8_t)q_final; }
    /* C2：双工作点记忆——最近两个相异 q* 各携 bytes(q*)，构成下一帧
     * 目标变化时的弦线外推模型。同 q* 重复（稳态）只刷新 bytes；
     * hint 无效化场景（禁用期）不推进历史。 */
    {
        uint32_t bf = 0u;
        if (sized_bytes_at(&c, q_final, &bf) != 0) {
            if (e->qp_hint_valid != 0 && e->qp_hint != q_final) {
                e->qp_hint_prev = e->qp_hint;
                e->qp_hint_prev_bytes = e->qp_hint_bytes;
            }
            e->qp_hint = q_final; /* M10-6.3A：本帧 q* 即下一帧首探种子 */
            e->qp_hint_valid = 1;
            e->qp_hint_bytes = bf;
        }
    }
    return rc;
}

int32_t tc_frame_encode_sized(const topos_frame_config* cfg, const topos_frame_input* input,
                              uint32_t target_bytes, uint8_t qp_min, uint8_t qp_max,
                              uint8_t* qp_used, uint8_t* out, size_t out_cap,
                              topos_frame_stats* stats)
{
    int32_t valid = tc_frame_config_validate(cfg);
    if (valid != TC_OK) { return valid; }
    /* V8（em==9）：版本机/包结构已冻结（批 1），编码器批 2 落地。
     * V9（em==10）/ V7-R3（em==11）：P0 固定锚 qp——无 sized 码控写口
     * （唯一写入口是 GOP context encode_frame；qp 搜索会绕过 I/P 决策
     * 与残差语义，V9 复审 2026-09-14 收口）。 */
    if ((cfg->reserved[0] & 0xFFu) == 9u) {
        tc_set_error(TC_ERR_NOT_IMPLEMENTED, "v8 sized encode lands in format-plan batch 2");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    if ((cfg->reserved[0] & 0xFFu) == 10u ||
        (cfg->reserved[0] & 0xFFu) == 11u) {
        tc_set_error(TC_ERR_NOT_IMPLEMENTED,
                     "temporal sized encode is not a write path (fixed anchor-qp P0; "
                     "use tc_gop_context_encode_frame)");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    /* TRAW（pf=3，批 1）：档位 = 固定锚点 qp 的 CQ 预设（无码控机）；严格
     * 比率码控 CB（target_bytes 硬预算）为后补选项（计划 §7）——批 1 显式
     * 拒绝而非走 3 平面 sized 路径（probe 只覆盖 3 平面会系统性低估体积）。 */
    if (cfg->pixel_format == 3u) {
        tc_set_error(TC_ERR_NOT_IMPLEMENTED,
                     "TRAW sized rate control is a post-v1 option (plan §7); "
                     "use fixed anchor-qp tiers");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    if (qp_min > TC_QP_MAX || qp_max > TC_QP_MAX || qp_min > qp_max) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "qp range %u..%u invalid",
                     (unsigned)qp_min, (unsigned)qp_max);
        return TC_ERR_INVALID_ARGUMENT;
    }
    topos_frame_stats local;
    memset(&local, 0, sizeof(local));
    topos_frame_stats* sp = stats != NULL ? stats : &local;

    sized_legacy_env_init();
    atomic_fetch_add_explicit(&s_sized_calls, 1u, memory_order_relaxed);

    /* R6：迭代间复用共享缓冲 + 常驻槽位（grow-only）；迭代期跳过逐次自检，
     * 搜索结束后对最终包统一执行一次完整 scan+CRC（与 plain 编码同保证）。
     * M10-6.1：借进程级持久缓存（跨帧 grow-only；竞争/禁用回退栈实例）。 */
    enc_loan loan;
    enc_shared* e = enc_loan_acquire(&loan);
    e->self_check = 0;

    uint8_t qu = 0;
    int fallback = 0;
    int32_t rc;
    /* V 代际收纳（2026-09-13）：em∈{3..6} 的穷举探针特例随代际退役移除
     * （cfg 域校验已拒绝，不可达）；闭包预测的非单调 sized 搜索特例由
     * V3/V6 专用，保留代际走 strict/fast/linear 常规搜索。 */
    if (atomic_load_explicit(&s_sized_mode, memory_order_relaxed) == 1) {
        rc = sized_search_linear(e, cfg, input, target_bytes, qp_min, qp_max, &qu,
                                 out, out_cap, sp);
    } else {
        /* mode 0 = strict（M10-7C 新默认）；mode 2 = R6 快速（oracle 对） */
        int32_t (*search)(enc_shared*, const topos_frame_config*,
                          const topos_frame_input*, uint32_t,
                          uint8_t, uint8_t, uint8_t*, uint8_t*,
                          size_t, topos_frame_stats*, int*);
        search = (atomic_load_explicit(&s_sized_mode, memory_order_relaxed) == 2)
                     ? sized_search_fast
                     : sized_search_strict;
        rc = search(e, cfg, input, target_bytes, qp_min, qp_max, &qu,
                    out, out_cap, sp, &fallback);
        if (fallback != 0) {
            rc = sized_search_linear(e, cfg, input, target_bytes, qp_min, qp_max, &qu,
                                     out, out_cap, sp);
        }
    }

    if (rc == TC_OK) {
        /* 最终包自检：结构 scan + 逐 slice CRC（等价 plain 路径 §11.5） */
        const int prof_chk = tc_profile_enabled();
        uint64_t tc0 = prof_chk ? tc_profile_now_ns() : 0u;
        topos_packet_view view;
        int32_t src = tc_packet_scan(out, sp->packet_size, &view);
        if (src == TC_OK) {
            for (uint32_t si = 0u; si < view.slice_count; ++si) {
                if (view.slice_crc_ok[si] == 0u) {
                    tc_set_error(TC_ERR_MALFORMED, "self-check: slice %u crc", (unsigned)si);
                    src = TC_ERR_MALFORMED;
                    break;
                }
            }
        } else {
            tc_wrap_error(TC_ERR_MALFORMED, "self-check scan failed: "); /* C14 */
        }
        if (src != TC_OK) { rc = src; }
        if (prof_chk) { TC_STATS_LOCK(); g_enc_stats.check_wall_ns += tc_profile_now_ns() - tc0; TC_STATS_UNLOCK(); }
    }

    enc_loan_release(&loan);
    if (qp_used != NULL) { *qp_used = qu; }
    return rc;
}

/* ============================ 解码 ============================ */

/* M3：sink 直写调用方最终 plane（可见几何内 clip 存储），不再经过 coded
 * 中间平面 + crop 复制。宽高 8 对齐时 clip 恒为整块直写。
 * M10-2A：重建存储体（DC-only 闭式 / 融合反量化+IDCT / clip store）上移
 * color_store.h 与专用 scan-to-plane 路径共享单一定义；本 sink 仅保留
 * 函数指针包装（generic/oracle/inspect/fuzz 路径）。 */
static int32_t color_block_sink(void* vctx, uint32_t idx, const int32_t* q, uint32_t ac_rowmask)
{
    return tc_color_store_block_reduced((tc_color_store_ctx*)vctx, idx, q, ac_rowmask);
}

typedef struct alpha_sink_ctx {
    uint16_t* dst;
    size_t stride;      /* M3：调用方 plane 步长（元素计） */
    uint32_t w;         /* coded 宽（预测链覆盖含 pad 列） */
    uint32_t vis_w;     /* 可见宽——列存储 clip */
    uint32_t vis_h;     /* 可见高——行存储 clip */
    uint32_t dst_w;     /* 目标宽；scaled=0 时等于 vis_w */
    uint32_t dst_h;     /* 目标高；scaled=0 时等于 vis_h */
    uint8_t scaled;     /* 只写目标采样点，预测链仍覆盖完整源行 */
    uint32_t y0_rows;   /* 带首像素行（block_y0 × 8），dst 写入基偏移 */
    uint32_t mask;      /* mode2：(1<<s)−1；mode1 为 0 */
    uint16_t* prev;     /* 带内上一行重建值（w 元素） */
    uint16_t* cur;      /* 当前行填充中 */
    tc_decode_stage_stats* stats; /* RD0-03：alpha pair 计数 */
} alpha_sink_ctx;

static int32_t alpha_pair_sink(void* vctx, size_t pos, uint32_t run, int32_t level)
{
    alpha_sink_ctx* c = (alpha_sink_ctx*)vctx;
    if (c->stats != NULL) { c->stats->entropy_symbols += 2u; }
    size_t level_at = pos + (size_t)run;
    size_t end = level != 0 ? level_at + 1u : level_at;
    for (size_t i = pos; i < end; ++i) {
        uint32_t y = (uint32_t)(i / c->w);
        uint32_t x = (uint32_t)(i % c->w);
        int32_t pred;
        if (y == 0u) {
            pred = x == 0u ? 0 : (int32_t)c->cur[x - 1u];
        } else if (x == 0u) {
            pred = (int32_t)c->prev[0];
        } else {
            pred = tc_med_predict((int32_t)c->cur[x - 1u], (int32_t)c->prev[x],
                                  (int32_t)c->prev[x - 1u]);
        }
        int32_t v = pred + (i == level_at ? level : 0);
        if (v < 0) { v = 0; }
        else if (v > 65535) { v = 65535; }
        if (c->mask != 0u && ((uint32_t)v & c->mask) != 0u) {
            tc_set_error(TC_ERR_MALFORMED, "alpha mode2 value %d not multiple of %u",
                         (int)v, (unsigned)c->mask + 1u);
            return TC_ERR_MALFORMED;
        }
        c->cur[x] = (uint16_t)v;
        /* M3：直写最终 plane——pad 行/列只参与预测状态，不落盘。
         * 行 clip 必须按全局行号（y0_rows + y）判：y 是带内局部行号，
         * 末带含 8 对齐 pad 行时局部 y 可越过 vis_h − y0_rows；旧写法
         * 「y < vis_h」仅在单带（y0_rows=0）时与之等价，多带末带的
         * pad 行（全局 ≥ vis_h）会写穿平面末端（ASan 越界写）。 */
        if (c->y0_rows + y < c->vis_h && x < c->vis_w) {
            if (c->scaled == 0u) {
                c->dst[(size_t)(c->y0_rows + y) * c->stride + x] = (uint16_t)v;
            } else {
                const uint32_t sy = c->y0_rows + y;
                const uint32_t tx = (uint32_t)((((uint64_t)(x + 1u) * c->dst_w
                                                  + c->vis_w - 1u) / c->vis_w) - 1u);
                const uint32_t ty = (uint32_t)((((uint64_t)(sy + 1u) * c->dst_h
                                                  + c->vis_h - 1u) / c->vis_h) - 1u);
                if (tx < c->dst_w && ty < c->dst_h &&
                    (uint32_t)(((uint64_t)tx * c->vis_w) / c->dst_w) == x &&
                    (uint32_t)(((uint64_t)ty * c->vis_h) / c->dst_h) == sy) {
                    c->dst[(size_t)ty * c->stride + tx] = (uint16_t)v;
                }
            }
        }
        if (x == c->w - 1u) {
            uint16_t* t = c->prev; c->prev = c->cur; c->cur = t;
        }
    }
    return TC_OK;
}

/* M3：conceal 直写调用方 plane（可见几何内），不再经由 coded */
static void conceal_band(uint16_t* dst, size_t stride, uint32_t vis_w, uint32_t vis_h,
                         uint32_t block_y0, uint32_t block_h, uint16_t fill)
{
    uint32_t y0 = block_y0 * 8u;
    uint32_t rows = block_h * 8u;
    if (rows > vis_h - y0) { rows = vis_h - y0; } /* y0 < vis_h 恒成立（末带） */
    for (uint32_t y = 0u; y < rows; ++y) {
        uint16_t* r = dst + (size_t)(y0 + y) * stride;
        for (uint32_t x = 0u; x < vis_w; ++x) { r[x] = fill; }
    }
}

static void conceal_band_scaled(uint16_t* dst, size_t stride,
                                uint32_t source_h, uint32_t dst_w, uint32_t dst_h,
                                uint32_t block_y0, uint32_t block_h, uint16_t fill)
{
    const uint32_t source_y0 = block_y0 * 8u;
    const uint32_t source_y1 = source_y0 + block_h * 8u;
    uint32_t y0 = (uint32_t)(((uint64_t)source_y0 * dst_h + source_h - 1u) / source_h);
    uint32_t y1 = (uint32_t)(((uint64_t)source_y1 * dst_h + source_h - 1u) / source_h);
    if (y1 > dst_h) { y1 = dst_h; }
    for (uint32_t y = y0; y < y1; ++y) {
        uint16_t* row = dst + (size_t)y * stride;
        for (uint32_t x = 0u; x < dst_w; ++x) { row[x] = fill; }
    }
}

void tc_fill_output_info(const topos_frame_header* fh, topos_frame_output* info)
{
    info->visible_width = fh->visible_width;
    info->visible_height = fh->visible_height;
    info->coded_width = fh->coded_width;
    info->coded_height = fh->coded_height;
    info->plane_count = fh->plane_count;
    info->bit_depth = fh->bit_depth;
    info->profile = fh->profile;
    info->pixel_format = fh->pixel_format;
    info->alpha_mode = fh->alpha_mode;
    info->alpha_bit_depth = fh->alpha_bit_depth;
    info->color_range = fh->color_range;
    info->color_primaries = fh->color_primaries;
    info->color_transfer = fh->color_transfer;
    info->color_matrix = fh->color_matrix;
    info->chroma_siting = fh->chroma_siting;
    info->sar_num = fh->sar_num;
    info->sar_den = fh->sar_den;
    info->slice_count = fh->slice_count;
}

/* ---- 阶段 9：解码 slice 任务（带内行不相交；单线程时同一代码路径顺序执行） ---- */

/* M2：alpha 行缓冲池实现（结构体定义见 codec.h） */
void tc_dec_alpha_pool_init(dec_alpha_pool* pool)
{
    memset(pool, 0, sizeof(*pool));
}

void tc_dec_alpha_pool_free(dec_alpha_pool* pool)
{
    for (uint32_t i = 0u; i < pool->slots; ++i) {
        tc_free_aligned(pool->prev[i]);
        tc_free_aligned(pool->cur[i]);
        pool->prev[i] = NULL;
        pool->cur[i] = NULL;
    }
    pool->slots = 0u;
    pool->elems = 0u;
}

void tc_dec_dc_pool_init(dec_dc_pool* pool)
{
    memset(pool, 0, sizeof(*pool));
}

void tc_dec_dc_pool_free(dec_dc_pool* pool)
{
    for (uint32_t i = 0u; i < pool->slots; ++i) {
        tc_free_aligned(pool->prev[i]);
        tc_free_aligned(pool->cur[i]);
        pool->prev[i] = NULL;
        pool->cur[i] = NULL;
    }
    pool->slots = 0u;
    pool->elems = 0u;
}

void tc_dec_intra_pool_init(dec_intra_pool* pool)
{
    memset(pool, 0, sizeof(*pool));
}

void tc_dec_intra_pool_free(dec_intra_pool* pool)
{
    for (uint32_t i = 0u; i < pool->slots; ++i) {
        tc_free_aligned(pool->edges[i]);
        pool->edges[i] = NULL;
    }
    pool->slots = 0u;
    pool->elems = 0u;
}

/* 双行槽位池两阶段扩容（M10-1e，alpha/dc 共用骨架）：
 *  - 槽位增长且尺寸不变 → 只补 [have, want) 新槽（旧槽指针原样保留）；
 *  - 尺寸增长（几何变大）→ 为全部 want 槽分配新尺寸，任一失败 → 释放全部
 *    新缓冲返回 0，旧池原样保留（此前 alpha 池「elems 不足直接 return 0」
 *    会让同一 context 在解码过更大几何后永久退化为每 slice 临时分配）；
 *  - 全部成功 → 释放被替换的旧缓冲、写入新指针。
 * 行缓冲为 slice 内 scratch（alpha 行写后读、DC 行入口清零），无内容迁移。 */
static int pool_pair_grow(void** prev, void** cur, uint32_t have, uint32_t want,
                          size_t elem_size, uint32_t want_elems, int resize_all)
{
    void* np[TC_SLICE_MAX_THREADS];
    void* nc[TC_SLICE_MAX_THREADS];
    memset(np, 0, sizeof(np));
    memset(nc, 0, sizeof(nc));
    size_t bytes = (size_t)want_elems * elem_size;
    for (uint32_t i = 0u; i < want; ++i) {
        if (!resize_all && i < have) { continue; } /* 旧槽保留 */
        np[i] = tc_alloc_aligned(bytes, 64u);
        nc[i] = tc_alloc_aligned(bytes, 64u);
        if (np[i] == NULL || nc[i] == NULL) {
            for (uint32_t j = 0u; j <= i; ++j) {
                tc_free_aligned(np[j]);
                tc_free_aligned(nc[j]);
            }
            return 0; /* 旧池保持可用（几何回退后仍按旧尺寸池化） */
        }
    }
    if (resize_all) {
        for (uint32_t i = 0u; i < have; ++i) {
            tc_free_aligned(prev[i]);
            tc_free_aligned(cur[i]);
        }
    }
    for (uint32_t i = resize_all ? 0u : have; i < want; ++i) {
        prev[i] = np[i];
        cur[i] = nc[i];
    }
    return 1;
}

/* grow-only；失败返回 0（调用方回退每 slice 自分配） */
static int dec_alpha_pool_ensure(dec_alpha_pool* pool, uint32_t want_slots, uint32_t elems)
{
    if (pool->slots >= want_slots && pool->elems >= elems) { return 1; }
    if (elems == 0u) { elems = 1u; }
    const int resize_all = pool->elems != 0u && pool->elems < elems;
    if (!pool_pair_grow((void**)pool->prev, (void**)pool->cur, pool->slots, want_slots,
                        sizeof(uint16_t), elems, resize_all)) {
        return 0;
    }
    pool->slots = want_slots;
    pool->elems = elems;
    return 1;
}

static int dec_dc_pool_ensure(dec_dc_pool* pool, uint32_t want_slots, uint32_t elems)
{
    if (pool->slots >= want_slots && pool->elems >= elems) { return 1; }
    if (elems == 0u) { elems = 1u; }
    const int resize_all = pool->elems != 0u && pool->elems < elems;
    if (!pool_pair_grow((void**)pool->prev, (void**)pool->cur, pool->slots, want_slots,
                        sizeof(int32_t), elems, resize_all)) {
        return 0;
    }
    pool->slots = want_slots;
    pool->elems = elems;
    return 1;
}

/* V3 边界槽位扩容：尺寸增大时先完整分配新池，失败保持旧池可用，
 * 与 alpha/DC 双行池的 grow-only 语义一致。 */
static int dec_intra_pool_ensure(dec_intra_pool* pool, uint32_t want_slots,
                                 uint32_t elems)
{
    if (pool->slots >= want_slots && pool->elems >= elems) { return 1; }
    if (elems == 0u) { elems = 1u; }
    const int resize_all = pool->elems != 0u && pool->elems < elems;
    uint16_t* next[TC_SLICE_MAX_THREADS];
    memset(next, 0, sizeof(next));
    size_t bytes = (size_t)elems * sizeof(uint16_t);
    for (uint32_t i = 0u; i < want_slots; ++i) {
        if (!resize_all && i < pool->slots) { continue; }
        next[i] = (uint16_t*)tc_alloc_aligned(bytes, 64u);
        if (next[i] == NULL) {
            for (uint32_t j = 0u; j <= i; ++j) { tc_free_aligned(next[j]); }
            return 0;
        }
    }
    if (resize_all) {
        for (uint32_t i = 0u; i < pool->slots; ++i) {
            tc_free_aligned(pool->edges[i]);
        }
        memset(pool->edges, 0, sizeof(pool->edges));
    }
    for (uint32_t i = resize_all ? 0u : pool->slots; i < want_slots; ++i) {
        pool->edges[i] = next[i];
    }
    pool->slots = want_slots;
    pool->elems = elems;
    return 1;
}

typedef struct dec_slice_task {
    const topos_frame_header* fh;
    const topos_slice_header* sh;
    const uint8_t* payload;
    const tc_qmatrix_set* qms;
    uint16_t* dst;        /* M3：调用方最终 plane（可见几何） */
    size_t dst_stride;    /* 元素计步长（>= vis_w） */
    uint32_t vis_w;       /* 平面可见宽 */
    uint32_t vis_h;       /* 平面可见高 */
    uint32_t dst_w;       /* 目标输出宽 */
    uint32_t dst_h;       /* 目标输出高 */
    uint8_t scaled;       /* target-size reconstruction */
    uint8_t coefficient_limit; /* 扫描序低频保留上限 */
    dec_scratch* scratch; /* M10-1：非 NULL 时 alpha 行 + 颜色 DC 行取池槽位 */
    uint32_t worker_slot; /* 池槽位（job 入口按执行线程绑定） */
    tc_dequant_inverse_fn dinv; /* M10-1d：帧入口解析的融合内核（免每块 atomic） */
    uint32_t si;          /* 全帧 slice 序号（slice_status 写入位） */
    uint8_t* status_out;  /* out_info->slice_status */
    int32_t fatal_rc;     /* OOM 等致命错误（非 conceal）；0 = 无 */
    uint8_t stats_on;     /* M0 观测开关（tc_profile_enabled()，帧级快照） */
    tc_decode_stage_stats stats; /* M0 观测（profile 关闭时不累积） */
    int32_t* qcap_q;      /* A2 差分机：q 捕获区域（NULL = 常规直写） */
    uint32_t* qcap_Q;     /* A2 差分机：该 slice Q[64] 写入位 */
    uint32_t qcap_gi;     /* A2 差分机：捕获元数据全局序号（稀疏发射用） */
} dec_slice_task;

static void dec_slice_job(void* vctx)
{
    dec_slice_task* t = (dec_slice_task*)vctx;
    const topos_frame_header* fh = t->fh;
    const topos_slice_header* sh = t->sh;
    uint32_t cur_plane = sh->plane;
    uint32_t w = fh->plane_coded_w[cur_plane];
    int slice_ok = 1;
    const int prof = t->stats_on != 0;
    /* 解码深化：动态领取分发后 si%nw 不再与 worker 绑定——槽位资源
     * （alpha 行 + 颜色 DC 行）以执行线程标识取槽（越界自分配兜底） */
    t->worker_slot = tc_pool_worker_slot();
    uint64_t t_crc = prof ? tc_profile_now_ns() : 0u;

    /* M1：CRC 归 slice worker 做且只做一次——主线程不再串行预扫整帧 payload；
     * 失败直接 conceal，杜绝「payload 已坏但 Rice 语法恰好可消费」被当正常
     * slice 解码的漏洞（旧路径 dec_slice_task 未携带 slice_crc_ok）。 */
    const int crc_ok =
        tc_crc32(t->payload, (size_t)sh->slice_payload_size) == sh->slice_crc32;
    if (prof) { t->stats.crc_ns += tc_profile_now_ns() - t_crc; }
    if (!crc_ok) {
        slice_ok = 0;
    } else if (is_alpha_plane(fh, cur_plane)) {
        uint32_t s = 16u - (uint32_t)fh->alpha_bit_depth; /* mode2 时 <16 */
        uint32_t mask = fh->alpha_mode == 2u ? ((1u << s) - 1u) : 0u;
        uint16_t* prev = NULL;
        uint16_t* cur = NULL;
        int own_rows = 1; /* 池槽位可用且容量足够时借用，否则 slice 自分配
                           *（扩容失败的帧回退临时分配——容量不足的旧池
                           * 行绝不可借：越界写即堆损坏） */
        dec_alpha_pool* apool = t->scratch != NULL ? &t->scratch->alpha : NULL;
        if (apool != NULL && t->worker_slot < apool->slots
                && apool->prev[t->worker_slot] != NULL
                && apool->elems >= w) {
            prev = apool->prev[t->worker_slot];
            cur = apool->cur[t->worker_slot];
            own_rows = 0;
        } else {
            prev = (uint16_t*)tc_alloc((size_t)w * sizeof(uint16_t));
            cur = (uint16_t*)tc_alloc((size_t)w * sizeof(uint16_t));
        }
        if (prev == NULL || cur == NULL) {
            tc_free(prev);
            tc_free(cur);
            t->fatal_rc = TC_ERR_OUT_OF_MEMORY;
            slice_ok = 0;
        } else {
            alpha_sink_ctx actx;
            actx.dst = t->dst;
            actx.stride = t->dst_stride;
            actx.w = w;
            actx.vis_w = t->vis_w;
            actx.vis_h = t->vis_h;
            actx.dst_w = t->dst_w;
            actx.dst_h = t->dst_h;
            actx.scaled = t->scaled;
            actx.y0_rows = (uint32_t)sh->block_y0 * 8u;
            actx.mask = mask;
            actx.prev = prev;
            actx.cur = cur;
            actx.stats = prof ? &t->stats : NULL;
            uint64_t t_ent = prof ? tc_profile_now_ns() : 0u;
            int32_t drc = tc_alpha_slice_decode_stream(fh, sh, t->payload,
                                                       sh->slice_payload_size,
                                                       alpha_pair_sink, &actx, NULL);
            if (prof) { t->stats.entropy_ns += tc_profile_now_ns() - t_ent; }
            if (own_rows != 0) {
                tc_free(prev);
                tc_free(cur);
            }
            if (drc != TC_OK) {
                slice_ok = 0;
            }
        }
    } else {
        int32_t qp_eff = tc_slice_effective_qp(fh->qp_base, sh->qp_delta_biased);
        tc_quant_ctx qctx;
        tc_quant_ctx_init_bd_tbl(&qctx, is_chroma_plane(fh, cur_plane) ? t->qms->chroma : t->qms->luma,
                                 (uint32_t)qp_eff, fh->bit_depth,
                                 tc_qtbl_of_flags(fh->flags));
        tc_color_store_ctx cctx;
        cctx.dst = t->dst;
        cctx.stride = t->dst_stride;
        cctx.qctx = &qctx;
        cctx.cols = fh->plane_block_cols[cur_plane];
        cctx.block_y0 = sh->block_y0;
        cctx.vis_w = t->vis_w;
        cctx.vis_h = t->vis_h;
        cctx.dst_w = t->dst_w;
        cctx.dst_h = t->dst_h;
        cctx.scaled = t->scaled;
        cctx.coefficient_limit = t->coefficient_limit;
        cctx.mid = bd_mid(fh->bit_depth);
        cctx.max = bd_max(fh->bit_depth);
        /* 批 4 复查（2026-09-13）：字段式初始化曾漏 f_clamp（栈垃圾参与
         * DC-only/稀疏块钳位判定）——按 mid/max 显式推导，勿再依赖
         * 未初始化内存 */
        cctx.f_clamp = tc_color_store_f_clamp(cctx.mid, cctx.max);
        cctx.w0 = tc_transform_weights()[0];
        cctx.dinv = t->dinv; /* M10-1d：帧入口已解析（含 AVX2 表预热） */
        cctx.stats = prof ? &t->stats : NULL;
        cctx.sampled_ns = 0u;
        cctx.sampled_blocks = 0u;
        cctx.sampled_store_ns = 0u;
        cctx.sampled_store_blocks = 0u;
        cctx.sampled_entropy_ns = 0u;
        cctx.sampled_recon_ns = 0u;
        cctx.sampled_entropy_blocks = 0u;
        cctx.sampled_recon_blocks = 0u;

        /* V3 使用 intra 自己的重建边界语义，并复用 decoder-context 的
         * worker-slot DC/边界 scratch；不要为同一 slice 再建立临时副本。 */
        tc_scan_dc_ctx dc = {0};
        int own_dc = 0;
        uint16_t* edge_scratch = NULL;
        size_t edge_elems = 0u;
        int dc_ready = fh->version_major == 3u || fh->version_major == 6u;
        if (fh->version_major == 3u || fh->version_major == 6u) {
            /* decoder context 的两个池都按执行线程槽位复用；池不可用时
             * 保持 V3 内部受界限的 slice 临时分配回退。 */
            dec_dc_pool* dpool = t->scratch != NULL ? &t->scratch->dc : NULL;
            if (dpool != NULL && t->worker_slot < dpool->slots
                    && dpool->prev[t->worker_slot] != NULL
                    && dpool->cur[t->worker_slot] != NULL
                    && dpool->elems >= (uint32_t)cctx.cols) {
                dc.prev_row = dpool->prev[t->worker_slot];
                dc.row = dpool->cur[t->worker_slot];
                dc.elems = dpool->elems;
            }
            dec_intra_pool* ipool = t->scratch != NULL ? &t->scratch->intra : NULL;
            if (ipool != NULL && t->worker_slot < ipool->slots
                    && ipool->edges[t->worker_slot] != NULL
                    && ipool->elems >= cctx.cols * 16u) {
                edge_scratch = ipool->edges[t->worker_slot];
                edge_elems = ipool->elems;
            }
        } else {
            /* M10-1a：颜色 DC 双行取池槽位（grow-only，几何稳定零分配） */
            own_dc = 1;
            dec_dc_pool* dpool = t->scratch != NULL ? &t->scratch->dc : NULL;
            if (dpool != NULL && t->worker_slot < dpool->slots
                    && dpool->prev[t->worker_slot] != NULL
                    && dpool->elems >= (uint32_t)cctx.cols) {
                dc.prev_row = dpool->prev[t->worker_slot];
                dc.row = dpool->cur[t->worker_slot];
                dc.elems = dpool->elems;
                own_dc = 0;
            } else {
                dc.elems = cctx.cols > 0u ? (size_t)cctx.cols : 1u;
                dc.prev_row = (int32_t*)tc_alloc(dc.elems * sizeof(int32_t));
                dc.row = (int32_t*)tc_alloc(dc.elems * sizeof(int32_t));
            }
            dc_ready = dc.prev_row != NULL && dc.row != NULL;
        }
        if (!dc_ready) {
            tc_free(dc.prev_row);
            tc_free(dc.row);
            t->fatal_rc = TC_ERR_OUT_OF_MEMORY;
            slice_ok = 0;
        } else {
            uint64_t t_ent = prof ? tc_profile_now_ns() : 0u;
            /* M10-2A：生产走专用 scan-to-plane（sink 展开进扫描循环）；
             * generic sink 版保留为 oracle/差分/应急回退（dev 开关）。 */
            int32_t drc;
            if (t->qcap_q != NULL) {
                /* A2 差分机：q_out 捕获替代平面直写（与生产直写同 core
                 * 同语义；输出平面不写——参考平面另用关闭捕获的解码）。
                 * A2-4：sparse=1 且 V7-R2 走生产融合发射（环内直出 CSR、
                 * 跳过稠密 q 与 63 扫描）；sparse=2 强制 legacy 扫描路径
                 * （差分 oracle）。失败清零 CSR 段 = GPU 重建 mid（conceal
                 * 语义，见 dev_qcap_zero_slice）。 */
                memcpy(t->qcap_Q, qctx.Q, 64u * sizeof(uint32_t));
                if (s_dev_qcap_sparse == 1
                        && fh->version_major == 7u && fh->entropy_mode == 7u) {
                    tc_sp_emit_ctx em;
                    const tc_dev_qcap_slice* m = &s_dev_qcap_meta[t->qcap_gi];
                    em.pairs = s_dev_qcap_sp_pairs + dev_qcap_sp_pairs_seg(t->qcap_gi);
                    em.off = s_dev_qcap_sp_off + dev_qcap_sp_off_seg(t->qcap_gi);
                    em.dc = s_dev_qcap_sp_dc + dev_qcap_sp_dc_seg(m->plane);
                    em.plane_cols = m->cols;
                    em.block_y0 = m->block_y0;
                    em.cnt = 0u;
                    drc = tc_color_rans2_decode_sparse(fh, sh, t->payload,
                                                       sh->slice_payload_size,
                                                       &dc, &em);
                    if (drc != TC_OK) { dev_qcap_zero_slice(t->qcap_gi); }
                } else {
                    drc = tc_color_slice_decode(fh, sh, t->payload,
                                                sh->slice_payload_size, t->qcap_q, NULL);
                    if (drc == TC_OK && s_dev_qcap_sparse != 0) {
                        dev_qcap_emit_sparse(t->qcap_gi, t->qcap_q);
                    } else if (drc != TC_OK && s_dev_qcap_sparse != 0) {
                        dev_qcap_zero_slice(t->qcap_gi);
                    }
                }
            } else if (fh->version_major == 3u || fh->version_major == 6u) {
                drc = tc_intra_slice_decode(fh, sh, t->payload,
                    sh->slice_payload_size, NULL, NULL, NULL, NULL, &cctx,
                    dc.prev_row != NULL ? &dc : NULL, edge_scratch, edge_elems);
            } else if (direct_scan_enabled()) {
                drc = tc_color_slice_decode_to_plane(fh, sh, t->payload,
                                                     sh->slice_payload_size, &dc, &cctx);
            } else {
                drc = tc_color_slice_decode_stream_scratch(
                    fh, sh, t->payload, sh->slice_payload_size,
                    color_block_sink, &cctx, NULL, &dc);
            }
            if (prof) {
                /* 分项计时：V3 自己提供熵和重建的抽样时钟；V1/V2 仍使用
                 * color_store 的逆变换抽样，并以 slice 墙钟闭合。 */
                uint64_t scan_wall = tc_profile_now_ns() - t_ent;
                if (fh->version_major == 3u || fh->version_major == 6u) {
                    uint32_t total_blocks =
                        sh->block_h * fh->plane_block_cols[cur_plane];
                    uint64_t ent_est = 0u;
                    uint64_t recon_est = 0u;
                    if (cctx.sampled_entropy_blocks != 0u) {
                        ent_est = cctx.sampled_entropy_ns * (uint64_t)total_blocks
                                  / (uint64_t)cctx.sampled_entropy_blocks;
                    }
                    if (cctx.sampled_recon_blocks != 0u) {
                        recon_est = cctx.sampled_recon_ns * (uint64_t)total_blocks
                                    / (uint64_t)cctx.sampled_recon_blocks;
                    }
                    /* 计时读数来自同一批样本，过估时按总墙钟比例闭合，
                     * 避免把抽样误差报告成超过 slice 实际耗时。 */
                    if (ent_est > scan_wall) { ent_est = scan_wall; }
                    if (recon_est > scan_wall - ent_est) {
                        recon_est = scan_wall - ent_est;
                    }
                    t->stats.entropy_ns += ent_est;
                    t->stats.dequant_idct_ns += recon_est;
                    if (scan_wall > ent_est + recon_est) {
                        t->stats.entropy_ns += scan_wall - ent_est - recon_est;
                    }
                } else {
                    uint64_t dq_est = 0u;
                    uint64_t store_est = 0u;
                    if (cctx.sampled_blocks != 0u) {
                        uint32_t total_blocks =
                            sh->block_h * fh->plane_block_cols[cur_plane];
                        dq_est = cctx.sampled_ns * (uint64_t)total_blocks
                                 / (uint64_t)cctx.sampled_blocks;
                    }
                    if (cctx.sampled_store_blocks != 0u) {
                        uint32_t total_blocks =
                            sh->block_h * fh->plane_block_cols[cur_plane];
                        store_est = cctx.sampled_store_ns * (uint64_t)total_blocks
                                    / (uint64_t)cctx.sampled_store_blocks;
                    }
                    if (dq_est > scan_wall) { dq_est = scan_wall; }
                    if (store_est > dq_est) { store_est = dq_est; }
                    t->stats.entropy_ns += scan_wall - dq_est;
                    t->stats.dequant_idct_ns += dq_est - store_est;
                    t->stats.output_ns += store_est;
                }
            }
            if (own_dc != 0) {
                tc_free(dc.prev_row);
                tc_free(dc.row);
            }
            if (drc != TC_OK) {
                slice_ok = 0;
                /* 阶段 10：分配失败是系统态而非码流损坏 —— 记 fatal 整帧上报 OOM，
                 * 不得伪装成 concealment（WARN_CONCEALED 语义 = 数据可疑） */
                if (drc == TC_ERR_OUT_OF_MEMORY) { t->fatal_rc = drc; }
            }
        }
    }

    if (!slice_ok) { /* §9：仅该带 conceal，帧仍交付（致命错误另行上报） */
        if (t->qcap_q != NULL && s_dev_qcap_sparse != 0) {
            /* CRC 失败/早失败路径不经过解码分支——CSR 段可能仍是上一帧
             * 残留（首帧=未初始化）；统一在此清零（解码错误已零，幂等）。 */
            dev_qcap_zero_slice(t->qcap_gi);
        }
        uint16_t fill = is_alpha_plane(fh, cur_plane)
            ? (uint16_t)0xFFFFu
            : (uint16_t)bd_mid(fh->bit_depth);
        if (t->scaled != 0u) {
            conceal_band_scaled(t->dst, t->dst_stride, t->vis_h, t->dst_w, t->dst_h,
                               sh->block_y0, sh->block_h, fill);
        } else {
            conceal_band(t->dst, t->dst_stride, t->vis_w, t->vis_h,
                         sh->block_y0, sh->block_h, fill);
        }
        t->status_out[t->si] = TC_FRAME_SLICE_CONCEALED;
    } else {
        t->status_out[t->si] = TC_FRAME_SLICE_OK;
    }
}

/* 阶段4：跨帧单批切片队列核心（dec_frame_req 定义见 codec.h）。 */

/* 帧级预备（串行段）：qmatrix 查找 + scratch 池扩容 + 熵表预热。
 * 失败 = 该帧拒绝（rc 已设 + tc_set_error）。 */
static int32_t dec_frame_prep(dec_frame_req* r, uint32_t nw)
{
    const topos_packet_view* view = r->view;
    r->qms = lookup_qm(view->fh.qmatrix_id);
    if (r->qms == NULL) {
        tc_set_error(TC_ERR_UNSUPPORTED_MATRIX, "qmatrix_id %u",
                     (unsigned)view->fh.qmatrix_id);
        return TC_ERR_UNSUPPORTED_MATRIX;
    }

    if (r->scratch != NULL) {
        if (view->fh.alpha_mode != 0u && r->skip_alpha == 0u) {
            /* M10-1e：几何增大时整体换新（两阶段，失败保持旧池可继续按旧
             * 几何池化，不再永久退化临时分配） */
            (void)dec_alpha_pool_ensure(&r->scratch->alpha, nw,
                                        view->fh.plane_coded_w[3]);
        }
        /* M10-1a：颜色 DC 行池容量 = 最大 plane_block_cols */
        uint32_t max_cols = 0u;
        for (uint32_t p = 0u; p < view->fh.plane_count; ++p) {
            if (view->fh.plane_block_cols[p] > max_cols) {
                max_cols = view->fh.plane_block_cols[p];
            }
        }
        (void)dec_dc_pool_ensure(&r->scratch->dc, nw, max_cols);
        if (view->fh.version_major == 3u || view->fh.version_major == 6u) {
            uint32_t max_width = 0u;
            for (uint32_t p = 0u; p < view->fh.plane_count; ++p) {
                if (view->fh.plane_coded_w[p] > max_width) {
                    max_width = view->fh.plane_coded_w[p];
                }
            }
            /* 每槽保存 top/bottom 两行，宽度来自已校验的 coded geometry。 */
            if (max_width <= UINT32_MAX / 2u) {
                (void)dec_intra_pool_ensure(&r->scratch->intra, nw,
                                            max_width * 2u);
            }
        }
    }

    /* M10-1b：帧入口一次预热熵表（Rice LUT / VLC book）——首个使用某表
     * 的 slice 不再在 worker 内持 mutex 建表；slice 内 ensure 恒命中
     * acquire 快路。错误码不在预热处拦截（无效 k 由 slice 路径正常报错）。 */
    const int use_vlc = (view->fh.version_major >= 2u
                         && view->fh.entropy_mode == 1u) ||
                        (view->fh.version_major == 4u
                         && view->fh.entropy_mode == 2u) ||
                        (view->fh.version_major == 5u
                         && view->fh.entropy_mode == 3u);
    for (uint32_t si = 0u; si < view->slice_count; ++si) {
        const topos_slice_header* sh = &view->slices[si];
        if (use_vlc != 0 && sh->plane != 3u) {
            (void)tc_vlc_tables_ensure(TC_VLC_FAMILY_DC, sh->k1);
            (void)tc_vlc_tables_ensure(TC_VLC_FAMILY_LVL, sh->k2);
            (void)tc_vlc_tables_ensure(TC_VLC_FAMILY_RUN, sh->k3);
        } else {
            (void)tc_rice_lut_ensure(sh->k1);
            (void)tc_rice_lut_ensure(sh->k2);
            (void)tc_rice_lut_ensure(sh->k3);
        }
    }
    return TC_OK;
}

/* M3：全 plane slice 任务构建——sink 直写调用方最终 plane（可见几何内
 * clip），不再分配 coded 中间平面、不再按 plane 分三轮、不再 crop。
 * 带内行不相交 + plane 间缓冲不相交 → 并发写安全；alpha/DC 行缓冲按
 * worker 槽位私有（动态领取下 job 入口绑定）。1080p 任务数从每 plane 5
 * → 全帧 15，8 线程不再因平面串行饿死。
 * 任务写入批任务数组 [base, base+slice_count)；stride 非法 → 该帧拒绝。 */
static int32_t dec_frame_build_jobs(const dec_frame_req* r, uint32_t base,
                                    dec_slice_task* tasks, tc_job* jobs,
                                    tc_dequant_inverse_fn dinv, int prof,
                                    uint32_t* built_out)
{
    const topos_packet_view* view = r->view;
    uint32_t built = 0u;
    for (uint32_t si = 0u; si < view->slice_count; ++si) {
        const uint32_t plane = view->slices[si].plane;
        if (r->skip_alpha != 0u && is_alpha_plane(&view->fh, plane)) {
            /* Keep slice_status indexed to the original packet while omitting
             * the expensive alpha entropy/reconstruction task entirely. */
            r->info->slice_status[si] = TC_FRAME_SLICE_OK;
            continue;
        }
        dec_slice_task* t = &tasks[base + built];
        memset(t, 0, sizeof(*t));
        const uint32_t source_w = view->fh.plane_visible_w[plane];
        const uint32_t source_h = view->fh.plane_visible_h[plane];
        uint32_t dw = source_w;
        uint32_t dh = source_h;
        if (r->scaled != 0u) {
            /* pf=3（TRAW CFA）：全平面半宽半高；pf=0 仅 U/V 半宽 */
            if (view->fh.pixel_format == 3u) {
                dw = (r->target_width + 1u) / 2u;
                dh = (r->target_height + 1u) / 2u;
            } else {
                const int chroma422 = view->fh.pixel_format == 0u && (plane == 1u || plane == 2u);
                dw = chroma422 ? (r->target_width + 1u) / 2u : r->target_width;
                dh = r->target_height;
            }
        }
        size_t dstride = r->strides[plane] != 0u ? r->strides[plane] : (size_t)dw;
        if (dstride < (size_t)dw) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT,
                         "plane %u stride %zu < visible width %u",
                         (unsigned)plane, dstride, (unsigned)dw);
            return TC_ERR_INVALID_ARGUMENT;
        }
        t->fh = &view->fh;
        t->sh = &view->slices[si];
        t->payload = view->payloads[si];
        t->qms = r->qms;
        t->dst = r->planes[plane];
        t->dst_stride = dstride;
        t->vis_w = source_w;
        t->vis_h = source_h;
        t->dst_w = dw;
        t->dst_h = dh;
        t->scaled = r->scaled;
        t->coefficient_limit = r->coefficient_limit;
        t->scratch = r->scratch; /* job 内按 plane 取 alpha/dc 子池 */
        t->worker_slot = 0u; /* job 入口按 worker 槽位取（动态领取） */
        /* 批 4：bd≥13 帧覆写为宽域标量 compose（SIMD 融合内核为 2^25 域） */
        t->dinv = (t->fh->bit_depth > 12u) ? &tc_simd_dequant_inverse_wide : dinv;
        t->si = si;
        t->status_out = r->info->slice_status;
        t->stats_on = (uint8_t)prof;
        /* A2 差分机：颜色 slice 预分配捕获区域（V3/V6 intra 语义不同、
         * alpha 无 sink 概念，均不捕获；主线程顺序执行，无竞争） */
        if (s_dev_qcap_on != 0 && plane != 3u
                && view->fh.version_major != 3u && view->fh.version_major != 6u) {
            t->qcap_gi = s_dev_qcap_slices;   /* reserve 前取值 = 本 slice 元数据序号 */
            t->qcap_q = dev_qcap_reserve(&view->fh, &view->slices[si], &t->qcap_Q);
        }
        jobs[base + built].fn = dec_slice_job;
        jobs[base + built].ctx = t;
        ++built;
    }
    *built_out = built;
    return TC_OK;
}

/* 缩放交付的去块周期：解码器的缩放交付是"每格取首样本"的整数抽样，源 8×8
 * 块界在目标域落到 8/d 的倍数上（d = 源/目标 抽样比）。DC 台阶幅度不随抽样
 * 改变，故沿用源域推导的 alpha/beta，只把网格周期换成 unit = 8/d。
 * 只认 d = 2（unit = 4，4K→2K 预览的常态）：d = 3 网格非整数（块界不落在
 * 目标采样点上）、d ≥ 4 边界窗口重叠（并行等价性不成立），均保持不过滤。
 * 返回 0 = 不做。 */
static uint32_t dec_scaled_deblock_unit(const dec_frame_req* r)
{
    if (r->target_width == 0u || r->target_height == 0u) { return 0u; }
    if (r->target_width * 2u != r->view->fh.visible_width
            || r->target_height * 2u != r->view->fh.visible_height) {
        return 0u;
    }
    return 4u;
}

/* 帧级汇合（串行段）：M0 统计聚合 + 状态扫描 → 该帧 rc + concealed 计数 */
static int32_t dec_frame_finalize(dec_frame_req* r,
                                  const dec_slice_task* tasks, uint32_t count)
{
    uint32_t concealed = 0u;
    if (count > 0u && tasks[0].stats_on != 0) { /* M0：worker 局部累计 → 全局聚合（汇合后无竞争） */
        TC_STATS_LOCK();
        g_dec_stats.packet_bytes_read += r->view->fh.frame_packet_size;
        g_dec_stats.segments_parsed += count;
        g_dec_stats.frames++;
        for (uint32_t p = 0u; p < r->info->plane_count; ++p) {
            uint32_t pw = (uint32_t)r->info->visible_width;
            uint32_t ph = (uint32_t)r->info->visible_height;
            if (r->info->pixel_format == 3u) {
                /* TRAW CFA：全平面半宽半高 */
                pw = (pw + 1u) / 2u;
                ph = (ph + 1u) / 2u;
            } else if (r->info->pixel_format == 0u && (p == 1u || p == 2u)) {
                pw = (pw + 1u) / 2u;
            }
            g_dec_stats.upload_bytes += (uint64_t)pw
                                      * (uint64_t)ph
                                      * sizeof(uint16_t);
        }
        for (uint32_t si = 0u; si < count; ++si) {
            const tc_decode_stage_stats* st = &tasks[si].stats;
            g_dec_stats.crc_ns += st->crc_ns;
            g_dec_stats.entropy_ns += st->entropy_ns;
            g_dec_stats.dequant_idct_ns += st->dequant_idct_ns;
            g_dec_stats.output_ns += st->output_ns;
            g_dec_stats.alloc_ns += st->alloc_ns;
            g_dec_stats.blocks += st->blocks;
            g_dec_stats.nonzero_ac += st->nonzero_ac;
            g_dec_stats.dc_only_blocks += st->dc_only_blocks;
            g_dec_stats.entropy_symbols += st->entropy_symbols;
            g_dec_stats.coefficients_skipped += st->coefficients_skipped;
            g_dec_stats.idct_samples += st->idct_samples;
        }
        g_dec_stats.slices += count;
        TC_STATS_UNLOCK();
    }

    /* V7-R4/R5（P6，2026-09-21）：flags bit2 → 输出去块。TPIC（major 7）
     * 包经 tc_decode_slices 汇合后在此交付前滤波；v7b/V8 包走各自专用
     * 解码器（特性域校验已把 flags 位限定在 V7-R2 intra，不受影响）。
     * v2.1：缩放交付（预览/代理路径）同样过滤——在**目标域**以抽样后的块界
     * 周期运行同一套权重（1/2 抽样 → 4，见 dec_scaled_deblock_unit）；非整数
     * 抽样比保持不过滤。此前缩放交付完全不过滤，是预览里 8 周期小色块的来源。
     * alpha 平面（plane 3）不过滤；强度 qp = 帧级 qp_base（产品流全帧
     * 单一 qp，位流验证见锯齿诊断 §2.1）；QPT2 流滤波强度随细化表同源。 */
    if ((r->view->fh.flags & 0x0004u) != 0u) {
        const uint32_t qtbl = tc_qtbl_of_flags(r->view->fh.flags);
        const uint32_t dqp = (uint32_t)r->view->fh.qp_base;
        const uint32_t unit = r->scaled != 0u ? dec_scaled_deblock_unit(r) : 0u;
        if (r->scaled == 0u || unit != 0u) {
            for (uint32_t p = 0; p < r->view->fh.plane_count && p < 3u; ++p) {
                /* 目标几何按 slice 任务侧同一规则推导（dec_slice_task 的
                 * dst_w/dh：TRAW 全平面半宽半高；4:2:2 仅 U/V 半宽）。 */
                uint32_t pw = r->view->fh.plane_visible_w[p];
                uint32_t ph = r->view->fh.plane_visible_h[p];
                if (r->scaled != 0u) {
                    if (r->view->fh.pixel_format == 3u) {
                        pw = (r->target_width + 1u) / 2u;
                        ph = (r->target_height + 1u) / 2u;
                    } else {
                        const int c422 = r->view->fh.pixel_format == 0u
                                         && (p == 1u || p == 2u);
                        pw = c422 ? (r->target_width + 1u) / 2u : r->target_width;
                        ph = r->target_height;
                    }
                }
                /* stride 0 = tight（解码 API 约定），与 slice 任务侧同口径 */
                const size_t dstride = r->strides[p] != 0u
                    ? r->strides[p] : (size_t)pw;
                if (dstride < (size_t)pw) { continue; }
                if (r->scaled == 0u) {
                    tc_deblock_plane(r->planes[p], (int32_t)dstride, pw, ph,
                                     dqp, qtbl,
                                     (uint8_t)r->view->fh.bit_depth);
                } else {
                    tc_deblock_plane_scaled(r->planes[p], (int32_t)dstride,
                                            pw, ph, dqp, qtbl,
                                            (uint8_t)r->view->fh.bit_depth, unit);
                }
            }
        }
    }
    (void)r;

    for (uint32_t si = 0u; si < count; ++si) {
        if (tasks[si].fatal_rc != 0) {
            /* fatal（OOM）：整帧拒绝，concealed 计数无意义 */
            tc_set_error(tasks[si].fatal_rc, "slice %u decode oom", (unsigned)si);
            return tasks[si].fatal_rc;
        }
        if (tasks[si].status_out[tasks[si].si] == TC_FRAME_SLICE_CONCEALED) {
            concealed++;
        }
    }

    r->info->concealed_slices = (uint16_t)concealed;
    if (concealed != 0u) {
        tc_set_error(TC_WARN_CONCEALED, "%u of %u slices concealed",
                     (unsigned)concealed, (unsigned)count);
        return TC_WARN_CONCEALED;
    }
    return TC_OK;
}

/* 阶段4：跨帧单批切片队列核心实现——nframes 个请求的全部切片任务进同
 * 一次线程池批次（一次唤醒/汇合消化 N 帧，消除逐帧派发/汇合与帧间串行段；
 * 并发 tc_parallel_for 调用会被 id_base 挤位退化单线程，故跨帧并行必须
 * 由本核心以单批承载）。任务数组 total ≤ TC_MAX_SLICES 走栈（单帧路径
 * 稳态零分配不变），超限走堆；堆分配失败逐帧回退（每帧 ≤512 恒走栈）。 */
static int32_t dec_frames_core_impl(dec_frame_req* reqs, uint32_t nframes)
{
    const int prof = tc_profile_enabled();

    /* P1-17：任务总数 checked 累加；堆分配 checked 乘法（32 位 size_t
     * 平台的 count×sizeof 回绕防线） */
    uint32_t total = 0u;
    for (uint32_t i = 0u; i < nframes; ++i) {
        if (!tc_uadd_u32(total, reqs[i].view->slice_count, &total)) {
            tc_set_error(TC_ERR_LIMIT_EXCEEDED, "batch slice 总数溢出");
            return TC_ERR_LIMIT_EXCEEDED;
        }
    }

    /* M10-4：nw_override（decoder context 预算）优先；0 = 进程线程数 */
    uint32_t nw = reqs[0].nw_override != 0u ? reqs[0].nw_override
                                            : (uint32_t)tc_dev_thread_count();
    if (nw > total) { nw = total; }
    if (nw > (uint32_t)TC_SLICE_MAX_THREADS) { nw = (uint32_t)TC_SLICE_MAX_THREADS; }
    if (nw == 0u) { nw = 1u; }

    dec_slice_task stack_tasks[TC_MAX_SLICES];
    tc_job stack_jobs[TC_MAX_SLICES];
    dec_slice_task* tasks = stack_tasks;
    tc_job* jobs = stack_jobs;
    size_t tasks_bytes = 0u, jobs_bytes = 0u;
    int heap = total > (uint32_t)TC_MAX_SLICES;
    if (heap && (!tc_umul_size((size_t)total, sizeof(dec_slice_task), &tasks_bytes)
                 || !tc_umul_size((size_t)total, sizeof(tc_job), &jobs_bytes))) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "batch 任务数组字节溢出");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    if (heap) {
        dec_slice_task* ht = (dec_slice_task*)tc_alloc(tasks_bytes);
        tc_job* hj = (tc_job*)tc_alloc(jobs_bytes);
        if (ht == NULL || hj == NULL) {
            tc_free(ht);
            tc_free(hj);
            /* 正确性优先：逐帧回退（单帧 total ≤ 512 恒走栈，不再递归入堆路径） */
            int32_t first = TC_OK;
            for (uint32_t i = 0u; i < nframes; ++i) {
                if (reqs[i].rc == TC_OK) {
                    reqs[i].rc = dec_frames_core_impl(&reqs[i], 1u);
                }
                if (first == TC_OK) { first = reqs[i].rc; }
            }
            return first;
        }
        tasks = ht;
        jobs = hj;
    }

    /* M10-1d：批入口一次解析 SIMD 融合内核（含 AVX2 表生成）——块循环经
     * 解析指针直调，不再每块 atomic ready 检查（4K 密素材 ~44 万次/帧） */
    const tc_dequant_inverse_fn dinv = tc_simd_resolve_dequant_inverse(0u);

    /* A2 差分机：批入口一次性扩容捕获缓冲（此后 reserve 只切分，
     * worker 区域指针批内稳定） */
    if (s_dev_qcap_on != 0) { dev_qcap_prepare_batch(reqs, nframes); }

    uint32_t built = 0u;
    for (uint32_t i = 0u; i < nframes; ++i) {
        if (reqs[i].rc != TC_OK) { continue; } /* 调用方已拒绝（参数校验） */
        int32_t rc = dec_frame_prep(&reqs[i], nw);
        if (rc != TC_OK) {
            reqs[i].rc = rc;
            continue; /* 该帧拒绝；其余帧照常入队 */
        }
        uint32_t frame_built = 0u;
        rc = dec_frame_build_jobs(&reqs[i], built, tasks, jobs, dinv, prof,
                                  &frame_built);
        if (rc != TC_OK) {
            reqs[i].rc = rc;
            continue;
        }
        reqs[i].task_base = built;
        built += frame_built;
    }

    if (built > 0u) { (void)tc_parallel_for(jobs, built, nw); }

    int32_t first = TC_OK;
    for (uint32_t i = 0u; i < nframes; ++i) {
        if (reqs[i].rc == TC_OK) {
            uint32_t frame_task_count = 0u;
            for (uint32_t si = 0u; si < reqs[i].view->slice_count; ++si) {
                if (reqs[i].skip_alpha == 0u
                        || !is_alpha_plane(&reqs[i].view->fh,
                                           reqs[i].view->slices[si].plane)) {
                    ++frame_task_count;
                }
            }
            reqs[i].rc = dec_frame_finalize(
                &reqs[i], &tasks[reqs[i].task_base], frame_task_count);
        }
        if (first == TC_OK) { first = reqs[i].rc; }
    }

    if (heap) {
        tc_free(tasks);
        tc_free(jobs);
    }
    return first;
}

/* V7-R3（ADR-C048 D4）无状态入口 P 包闸：GOP context 经内部
 * tc_decode_slices 直连，不经过本函数。 */
int32_t tc_v7r3_stateless_p_guard(const topos_frame_header* fh)
{
    if (fh->version_major == 7u && fh->entropy_mode == 8u &&
        fh->frame_type != 0u) {
        tc_set_error(TC_ERR_STATE,
                     "v7r3 P-frame decode requires tc_gop_context "
                     "(stateless entry is I-only)");
        return TC_ERR_STATE;
    }
    return TC_OK;
}

/* 内部共享入口（codec.h；decoder_ctx 批量路径复用） */
int32_t tc_decode_frames(dec_frame_req* reqs, uint32_t nframes)
{
    const int prof = tc_profile_enabled();
    const uint64_t t0 = prof ? tc_profile_now_ns() : 0u;
    const int32_t rc = dec_frames_core_impl(reqs, nframes);
    if (prof) { tc_dev_decode_stats_add_wall(tc_profile_now_ns() - t0); }
    return rc;
}

/* M2 内部共享：统一任务队列解码核心（tc_frame_decode 与 tc_decoder 共用）。
 * planes_out[i]（i < plane_count）为调用方缓冲；strides 元素计（NULL/0 = tight）。
 * scratch 非 NULL 时 alpha 行 + 颜色 DC 行池化（grow-only；扩容失败字段自动
 * 回退 slice 内临时分配）。 */
int32_t tc_decode_slices_ex(const topos_packet_view* view,
                                uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                const size_t strides[TC_FRAME_MAX_PLANES],
                                dec_scratch* scratch,
                                topos_frame_output* out_info,
                                uint32_t nw_override,
                                uint8_t skip_alpha)
{
    dec_frame_req r;
    r.view = (topos_packet_view*)view; /* core 只读 view */
    for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
        r.planes[p] = planes_out != NULL ? planes_out[p] : NULL;
        r.strides[p] = strides != NULL ? strides[p] : 0u;
    }
    r.info = out_info;
    r.scratch = scratch;
    r.nw_override = nw_override;
    r.task_base = 0u;
    r.target_width = 0u;
    r.target_height = 0u;
    r.scaled = 0u;
    r.coefficient_limit = 63u;
    r.skip_alpha = skip_alpha;
    r.qms = NULL;
    r.rc = TC_OK;
    return tc_decode_frames(&r, 1u);
}

int32_t tc_decode_slices(const topos_packet_view* view,
                         uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                         const size_t strides[TC_FRAME_MAX_PLANES],
                         dec_scratch* scratch,
                         topos_frame_output* out_info,
                         uint32_t nw_override)
{
    return tc_decode_slices_ex(view, planes_out, strides, scratch, out_info,
                               nw_override, 0u);
}

int32_t tc_decode_slices_scaled_limit_ex(const topos_packet_view* view,
                                uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                const size_t strides[TC_FRAME_MAX_PLANES],
                                dec_scratch* scratch,
                                topos_frame_output* out_info,
                                uint32_t nw_override,
                                uint32_t target_width, uint32_t target_height,
                                uint8_t coefficient_limit,
                                uint8_t skip_alpha)
{
    if (view == NULL || out_info == NULL || target_width == 0u || target_height == 0u ||
        target_width > view->fh.visible_width || target_height > view->fh.visible_height) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "scaled decode target exceeds source");
        return TC_ERR_INVALID_ARGUMENT;
    }
    dec_frame_req r;
    memset(&r, 0, sizeof(r));
    r.view = (topos_packet_view*)view;
    for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
        r.planes[p] = planes_out != NULL ? planes_out[p] : NULL;
        r.strides[p] = strides != NULL ? strides[p] : 0u;
    }
    r.info = out_info;
    r.scratch = scratch;
    r.nw_override = nw_override;
    r.target_width = target_width;
    r.target_height = target_height;
    r.scaled = 1u;
    r.coefficient_limit = coefficient_limit;
    r.skip_alpha = skip_alpha;
    r.rc = TC_OK;
    return tc_decode_frames(&r, 1u);
}

int32_t tc_decode_slices_scaled_limit(const topos_packet_view* view,
                                      uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                      const size_t strides[TC_FRAME_MAX_PLANES],
                                      dec_scratch* scratch,
                                      topos_frame_output* out_info,
                                      uint32_t nw_override,
                                      uint32_t target_width, uint32_t target_height,
                                      uint8_t coefficient_limit)
{
    return tc_decode_slices_scaled_limit_ex(
        view, planes_out, strides, scratch, out_info, nw_override,
        target_width, target_height, coefficient_limit, 0u);
}

int32_t tc_decode_slices_scaled(const topos_packet_view* view,
                                uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                const size_t strides[TC_FRAME_MAX_PLANES],
                                dec_scratch* scratch,
                                topos_frame_output* out_info,
                                uint32_t nw_override,
                                uint32_t target_width, uint32_t target_height)
{
    return tc_decode_slices_scaled_limit(view, planes_out, strides, scratch, out_info,
                                         nw_override, target_width, target_height, 63u);
}

int32_t tc_frame_decode(const uint8_t* data, size_t size,
                        uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                        const size_t strides[TC_FRAME_MAX_PLANES],
                        topos_frame_output* out_info)
{
    if (out_info == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "out_info == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = (uint32_t)sizeof(topos_frame_output);
    out_info->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    if (data == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "data == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (tc_packet_is_v7b(data, size)) {
        tc_v7b_decode_stats v7b_stats;
        int32_t v7b_rc = tc_v7b_frame_decode(
            data, size, TC_CODEC_AUTO_2K_MAX_DIM, TC_V7B_DECODE_FULL,
            planes_out, strides, out_info, &v7b_stats);
        if (planes_out != NULL && v7b_rc == TC_OK) {
            tc_dev_decode_stats_add_v7b(v7b_stats.bytes_read,
                                        v7b_stats.base_bytes_read,
                                        v7b_stats.segments_read,
                                        v7b_stats.segments_skipped);
        }
        return v7b_rc;
    }
    if (tc_packet_is_v7a(data, size)) {
        if (tc_frame_header_dev_replay_enabled() == 0) { return v7a_retired_reject(); }
        return tc_v7_frame_decode(data, size, TC_V7_BAND_COUNT - 1u,
                                  planes_out, strides, out_info, NULL);
    }
    if (tc_packet_is_v8(data, size)) {
        /* V8（批 3）：查询模式 = 全结构扫描 + out_info；实解码 = 结构扫描
         * （免 CRC）+ 瓦片任务内并行 CRC 重算 + 段级 conceal。 */
        if (planes_out == NULL) {
            topos_v8_packet_view v8;
            int32_t rc8 = tc_packet_scan_v8(data, size, &v8);
            if (rc8 != TC_OK) { return rc8; }
            tc_fill_output_info(&v8.fh, out_info);
            return TC_OK;
        }
        return v8_frame_decode(data, size, planes_out, strides, out_info);
    }
    if (tc_packet_is_v9(data, size)) {
        /* V9（topos_v9_micro_gop_plan 批 2）：包布局 V8 同构——查询模式
         * = 同构扫描 + out_info（GOP 元数据可查）；I 帧直连 V8 像素机；
         * P 帧需要参考 → 仅 GOP context（TC_ERR_STATE）。 */
        topos_v8_packet_view v9;
        int32_t rc9 = tc_packet_scan_v8_ex(data, size, &v9, 0);
        if (rc9 != TC_OK) { return rc9; }
        if (v9.fh.version_major != 9u) {
            tc_set_error(TC_ERR_UNSUPPORTED_VERSION,
                         "v9 entry accepts major=9 only (got %u)",
                         (unsigned)v9.fh.version_major);
            return TC_ERR_UNSUPPORTED_VERSION;
        }
        tc_fill_output_info(&v9.fh, out_info);
        if (planes_out == NULL) { return TC_OK; } /* 仅查询模式 */
        if (v9.fh.frame_type != 0u) {
            tc_set_error(TC_ERR_STATE,
                         "v9 P-frame decode requires tc_gop_context "
                         "(stateless entry is I-only)");
            return TC_ERR_STATE;
        }
        return v8_frame_decode(data, size, planes_out, strides, out_info);
    }

    topos_packet_view view;
    /* M1：解码入口只做结构解析；payload CRC 由各 slice worker 对唯一
     * payload 做且只做一次（查询模式 planes_out==NULL 同样不扫 CRC）。 */
    const int prof = tc_profile_enabled();
    uint64_t t_scan = prof ? tc_profile_now_ns() : 0u;
    int32_t rc = tc_packet_parse_structure(data, size, &view);
    if (prof) { TC_STATS_LOCK(); g_dec_stats.scan_ns += tc_profile_now_ns() - t_scan; TC_STATS_UNLOCK(); }
    if (rc != TC_OK) { return rc; }
    tc_fill_output_info(&view.fh, out_info);
    if (planes_out == NULL) { return TC_OK; } /* 仅查询模式 */
    rc = tc_v7r3_stateless_p_guard(&view.fh);
    if (rc != TC_OK) { return rc; }

    for (uint32_t p = 0u; p < view.fh.plane_count; ++p) {
        if (planes_out[p] == NULL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "planes_out[%u] == NULL", (unsigned)p);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }

    /* M10-1a：无状态入口不持池（行为与历史一致：slice 内临时分配）；
     * 稳态零分配经 tc_decoder context（产品路径）达成。 */
    return tc_decode_slices(&view, planes_out, strides, NULL, out_info, 0u);
}

int32_t tc_frame_decode_scaled(const uint8_t* data, size_t size,
                               uint32_t target_width, uint32_t target_height,
                               uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                               const size_t strides[TC_FRAME_MAX_PLANES],
                               topos_frame_output* out_info)
{
    if (out_info == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "out_info == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = (uint32_t)sizeof(topos_frame_output);
    out_info->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    if (data == NULL || target_width == 0u || target_height == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "scaled decode invalid arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (tc_packet_is_v7a(data, size)) {
        if (tc_frame_header_dev_replay_enabled() == 0) { return v7a_retired_reject(); }
        return tc_v7_frame_decode_scaled(data, size, TC_V7_BAND_COUNT - 1u,
                                         target_width, target_height,
                                         planes_out, strides, out_info, NULL);
    }
    if (tc_packet_is_v8(data, size)) {
        /* V8（批 1）：结构识别；缩放解码随批 3 落地。 */
        topos_v8_packet_view v8;
        int32_t rc8 = tc_packet_scan_v8(data, size, &v8);
        if (rc8 != TC_OK) { return rc8; }
        tc_fill_output_info(&v8.fh, out_info);
        tc_set_error(TC_ERR_NOT_IMPLEMENTED, "v8 scaled decode lands in format-plan batch 3");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    if (tc_packet_is_v9(data, size)) {
        /* V9（批 1）：结构识别走同构扫描；缩放解码不在 V9 窗口范围
         * （计划 §10：inter 缩放路径未定义 → 明确错误码）。 */
        topos_v8_packet_view v9;
        int32_t rc9 = tc_packet_scan_v8(data, size, &v9);
        if (rc9 != TC_OK) { return rc9; }
        tc_fill_output_info(&v9.fh, out_info);
        tc_set_error(TC_ERR_NOT_IMPLEMENTED, "v9 scaled decode not defined");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    topos_packet_view view;
    int32_t rc = tc_packet_parse_structure(data, size, &view);
    if (rc != TC_OK) { return rc; }
    if (target_width > view.fh.visible_width || target_height > view.fh.visible_height ||
        target_width > UINT16_MAX || target_height > UINT16_MAX) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "scaled target %ux%u exceeds source %ux%u",
                     (unsigned)target_width, (unsigned)target_height,
                     (unsigned)view.fh.visible_width, (unsigned)view.fh.visible_height);
        return TC_ERR_INVALID_ARGUMENT;
    }
    tc_fill_output_info(&view.fh, out_info);
    {
        const int32_t grc = tc_v7r3_stateless_p_guard(&view.fh);
        if (grc != TC_OK) { return grc; }
    }
    out_info->visible_width = (uint16_t)target_width;
    out_info->visible_height = (uint16_t)target_height;
    out_info->coded_width = (uint16_t)target_width;
    out_info->coded_height = (uint16_t)target_height;
    if (planes_out == NULL) { return TC_OK; }
    for (uint32_t p = 0u; p < view.fh.plane_count; ++p) {
        if (planes_out[p] == NULL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "scaled planes_out[%u] == NULL", (unsigned)p);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    return tc_decode_slices_scaled(&view, planes_out, strides, NULL, out_info, 0u,
                                   target_width, target_height);
}

int32_t tc_decode_scale_dimensions(uint32_t source_width, uint32_t source_height,
                                   tc_decode_scale scale,
                                   uint32_t* target_width, uint32_t* target_height)
{
    if (target_width == NULL || target_height == NULL || source_width == 0u ||
        source_height == 0u || scale < TC_DECODE_SCALE_FULL ||
        scale > TC_DECODE_SCALE_EIGHTH) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "invalid decode scale dimensions");
        return TC_ERR_INVALID_ARGUMENT;
    }
    uint32_t divisor = 1u;
    switch (scale) {
    case TC_DECODE_SCALE_FULL: divisor = 1u; break;
    case TC_DECODE_SCALE_HALF: divisor = 2u; break;
    case TC_DECODE_SCALE_THIRD: divisor = 3u; break;
    case TC_DECODE_SCALE_QUARTER: divisor = 4u; break;
    case TC_DECODE_SCALE_EIGHTH: divisor = 8u; break;
    default: return TC_ERR_INVALID_ARGUMENT;
    }
    *target_width = (source_width + divisor - 1u) / divisor;
    *target_height = (source_height + divisor - 1u) / divisor;
    return TC_OK;
}

static uint8_t tc_decode_scale_coefficient_limit(tc_decode_scale scale)
{
    switch (scale) {
    case TC_DECODE_SCALE_HALF: return 24u;
    case TC_DECODE_SCALE_THIRD: return 16u;
    case TC_DECODE_SCALE_QUARTER: return 10u;
    case TC_DECODE_SCALE_EIGHTH: return 4u;
    case TC_DECODE_SCALE_FULL: return 63u;
    default: return 0u;
    }
}

int32_t tc_decode_request_validate(const topos_decode_request* request)
{
    if (request == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decode request == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (request->struct_size != (uint32_t)sizeof(topos_decode_request) ||
        request->abi_version != (uint32_t)TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "decode request struct_size/abi_version");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (request->mode > TC_DECODE_MODE_AUTO_2K ||
        request->quality > TC_DECODE_QUALITY_HIGH ||
        (request->memory_type != TC_DECODE_MEMORY_CPU &&
         request->memory_type != TC_DECODE_MEMORY_HOST_VISIBLE_GPU) ||
        (request->flags & ~TC_DECODE_FLAG_DROP_ALPHA) != 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decode request enum/flags invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0u; i < 4u; ++i) {
        if (request->reserved[i] != 0u) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "decode request reserved nonzero");
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    if ((request->target_width == 0u) != (request->target_height == 0u)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "decode request target width/height must be paired");
        return TC_ERR_INVALID_ARGUMENT;
    }
    switch ((topos_decode_mode)request->mode) {
    case TC_DECODE_MODE_FULL:
        if (request->scale != TC_DECODE_SCALE_FULL ||
            request->target_width != 0u || request->target_height != 0u) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "full decode request has target/scale");
            return TC_ERR_INVALID_ARGUMENT;
        }
        break;
    case TC_DECODE_MODE_SCALED:
        if (request->scale != TC_DECODE_SCALE_FULL || request->target_width == 0u) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "scaled decode request requires target");
            return TC_ERR_INVALID_ARGUMENT;
        }
        break;
    case TC_DECODE_MODE_REDUCED:
        if (request->scale > TC_DECODE_SCALE_EIGHTH) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "reduced decode request scale invalid");
            return TC_ERR_INVALID_ARGUMENT;
        }
        if (request->target_width != 0u || request->target_height != 0u) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT,
                         "reduced decode request does not accept target");
            return TC_ERR_INVALID_ARGUMENT;
        }
        break;
    case TC_DECODE_MODE_AUTO_2K:
        if (request->scale != TC_DECODE_SCALE_FULL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "auto2k decode request scale invalid");
            return TC_ERR_INVALID_ARGUMENT;
        }
        break;
    default:
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decode request mode invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    return TC_OK;
}

int32_t tc_decode_request_resolve(const topos_frame_header* fh,
                                  const topos_decode_request* request,
                                  tc_decode_plan* plan)
{
    if (fh == NULL || plan == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decode request resolve args NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    int32_t rc = tc_decode_request_validate(request);
    if (rc != TC_OK) { return rc; }
    memset(plan, 0, sizeof(*plan));
    plan->mode = request->mode;
    plan->scale = TC_DECODE_SCALE_FULL;
    plan->target_width = fh->visible_width;
    plan->target_height = fh->visible_height;
    plan->coefficient_limit = 63u;
    plan->drop_alpha = (request->flags & TC_DECODE_FLAG_DROP_ALPHA) != 0u
        && fh->alpha_mode != 0u;

    const uint32_t source_width = fh->visible_width;
    const uint32_t source_height = fh->visible_height;
    switch ((topos_decode_mode)request->mode) {
    case TC_DECODE_MODE_FULL:
        return TC_OK;
    case TC_DECODE_MODE_SCALED:
        plan->target_width = request->target_width;
        plan->target_height = request->target_height;
        plan->scaled = 1u;
        break;
    case TC_DECODE_MODE_REDUCED:
        plan->scale = request->scale;
        rc = tc_decode_scale_dimensions(source_width, source_height,
                                        (tc_decode_scale)request->scale,
                                        &plan->target_width, &plan->target_height);
        if (rc != TC_OK) { return rc; }
        plan->scaled = request->scale != TC_DECODE_SCALE_FULL ? 1u : 0u;
        plan->coefficient_limit = tc_decode_scale_coefficient_limit(
            (tc_decode_scale)request->scale);
        break;
    case TC_DECODE_MODE_AUTO_2K: {
        if (request->target_width != 0u) {
            plan->target_width = request->target_width;
            plan->target_height = request->target_height;
            plan->scaled = (plan->target_width != source_width ||
                            plan->target_height != source_height) ? 1u : 0u;
            break;
        }
        if (source_width <= 2048u && source_height <= 2048u) {
            return TC_OK;
        }
        const tc_decode_scale scales[] = {
            TC_DECODE_SCALE_HALF, TC_DECODE_SCALE_THIRD,
            TC_DECODE_SCALE_QUARTER, TC_DECODE_SCALE_EIGHTH
        };
        for (size_t i = 0u; i < sizeof(scales) / sizeof(scales[0]); ++i) {
            uint32_t tw = 0u;
            uint32_t th = 0u;
            rc = tc_decode_scale_dimensions(source_width, source_height, scales[i],
                                            &tw, &th);
            if (rc != TC_OK) { return rc; }
            if (tw <= 2048u && th <= 2048u) {
                plan->scale = scales[i];
                plan->target_width = tw;
                plan->target_height = th;
                plan->scaled = 1u;
                plan->coefficient_limit = tc_decode_scale_coefficient_limit(scales[i]);
                break;
            }
        }
        break;
    }
    default:
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (plan->target_width == 0u || plan->target_height == 0u ||
        plan->target_width > source_width || plan->target_height > source_height ||
        plan->target_width > UINT16_MAX || plan->target_height > UINT16_MAX) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "decode target %ux%u exceeds source %ux%u",
                     (unsigned)plan->target_width, (unsigned)plan->target_height,
                     (unsigned)source_width, (unsigned)source_height);
        return TC_ERR_INVALID_ARGUMENT;
    }
    return TC_OK;
}

static void tc_apply_decode_plan_info(const tc_decode_plan* plan,
                                      topos_frame_output* out_info)
{
    out_info->visible_width = (uint16_t)plan->target_width;
    out_info->visible_height = (uint16_t)plan->target_height;
    out_info->coded_width = (uint16_t)plan->target_width;
    out_info->coded_height = (uint16_t)plan->target_height;
    if (plan->drop_alpha != 0u && out_info->alpha_mode != 0u
            && out_info->plane_count >= 4u) {
        out_info->plane_count = 3u;
        out_info->alpha_mode = 0u;
        out_info->alpha_bit_depth = 0u;
    }
}

int32_t tc_frame_decode_request(const uint8_t* data, size_t size,
                                const topos_decode_request* request,
                                uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                const size_t strides[TC_FRAME_MAX_PLANES],
                                topos_frame_output* out_info)
{
    if (out_info == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "out_info == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = (uint32_t)sizeof(topos_frame_output);
    out_info->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    if (data == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "data == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    int32_t rc = tc_decode_request_validate(request);
    if (rc != TC_OK) { return rc; }

    if (tc_packet_is_v7b(data, size)) {
        topos_frame_header v7b_fh;
        rc = tc_v7_packet_header_decode(data, size, &v7b_fh);
        if (rc != TC_OK) { return rc; }
        tc_decode_plan v7b_plan;
        rc = tc_decode_request_resolve(&v7b_fh, request, &v7b_plan);
        if (rc != TC_OK) { return rc; }
        v7b_plan.drop_alpha = 0u;
        tc_fill_output_info(&v7b_fh, out_info);
        tc_apply_decode_plan_info(&v7b_plan, out_info);
        if (planes_out == NULL) { return TC_OK; }
        if (request->mode == TC_DECODE_MODE_FULL ||
            (request->mode == TC_DECODE_MODE_REDUCED && v7b_plan.scaled == 0u) ||
            (request->mode == TC_DECODE_MODE_AUTO_2K && v7b_plan.scaled == 0u)) {
            tc_v7b_decode_stats v7b_stats;
            int32_t v7b_rc = tc_v7b_frame_decode(
                data, size, TC_CODEC_AUTO_2K_MAX_DIM, TC_V7B_DECODE_FULL,
                planes_out, strides, out_info, &v7b_stats);
            if (v7b_rc == TC_OK) {
                tc_dev_decode_stats_add_v7b(v7b_stats.bytes_read,
                                            v7b_stats.base_bytes_read,
                                            v7b_stats.segments_read,
                                            v7b_stats.segments_skipped);
            }
            return v7b_rc;
        }
        if (request->mode == TC_DECODE_MODE_REDUCED ||
            request->mode == TC_DECODE_MODE_AUTO_2K) {
            tc_v7b_decode_stats v7b_stats;
            int32_t v7b_rc = tc_v7b_frame_decode_reduced(
                data, size, v7b_plan.target_width, v7b_plan.target_height,
                planes_out, strides, out_info, &v7b_stats);
            if (v7b_rc == TC_OK) {
                tc_dev_decode_stats_add_v7b(v7b_stats.bytes_read,
                                            v7b_stats.base_bytes_read,
                                            v7b_stats.segments_read,
                                            v7b_stats.segments_skipped);
            }
            return v7b_rc;
        }
        tc_set_error(TC_ERR_NOT_IMPLEMENTED,
                     "v7b scaled request requires full-quality target reconstruction");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    if (tc_packet_is_v7a(data, size)) {
        if (tc_frame_header_dev_replay_enabled() == 0) { return v7a_retired_reject(); }
        topos_frame_header v7_fh;
        rc = tc_v7_packet_header_decode(data, size, &v7_fh);
        if (rc != TC_OK) { return rc; }
        tc_decode_plan v7_plan;
        rc = tc_decode_request_resolve(&v7_fh, request, &v7_plan);
        if (rc != TC_OK) { return rc; }
        v7_plan.drop_alpha = 0u;
        tc_fill_output_info(&v7_fh, out_info);
        tc_apply_decode_plan_info(&v7_plan, out_info);
        if (planes_out == NULL) { return TC_OK; }
        for (uint32_t p = 0u; p < v7_fh.plane_count; ++p) {
            if (planes_out[p] == NULL) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT, "request planes_out[%u] == NULL",
                             (unsigned)p);
                return TC_ERR_INVALID_ARGUMENT;
            }
        }
        uint32_t max_band = v7_plan.coefficient_limit <= 4u ? 0u
                          : v7_plan.coefficient_limit <= 10u ? 1u
                          : v7_plan.coefficient_limit <= 16u ? 2u
                          : v7_plan.coefficient_limit <= 24u ? 3u : 4u;
        return tc_v7_frame_decode_scaled(data, size, max_band,
                                         v7_plan.target_width, v7_plan.target_height,
                                         planes_out, strides, out_info, NULL);
    }
    if (tc_packet_is_v8(data, size)) {
        /* V8：查询模式 = 全结构扫描 + out_info；实解码 = FULL 走批 3
         * 段化解码器（reduced/auto2k 未定义 V8 缩放路径 → 明确错误码）。 */
        topos_v8_packet_view v8;
        rc = tc_packet_scan_v8(data, size, &v8);
        if (rc != TC_OK) { return rc; }
        tc_fill_output_info(&v8.fh, out_info);
        if (planes_out == NULL) { return TC_OK; } /* 仅查询模式 */
        if (request->mode != TC_DECODE_MODE_FULL) {
            tc_set_error(TC_ERR_NOT_IMPLEMENTED, "v8 reduced/auto2k path not defined");
            return TC_ERR_NOT_IMPLEMENTED;
        }
        return v8_frame_decode(data, size, planes_out, strides, out_info);
    }
    if (tc_packet_is_v9(data, size)) {
        /* V9（批 2）：查询模式 = 同构扫描 + out_info；I 帧 FULL 直连 V8
         * 像素机；P 帧 → GOP context。缩放路径不在 V9 窗口（计划 §10）。 */
        topos_v8_packet_view v9;
        rc = tc_packet_scan_v8_ex(data, size, &v9, 0);
        if (rc != TC_OK) { return rc; }
        if (v9.fh.version_major != 9u) {
            tc_set_error(TC_ERR_UNSUPPORTED_VERSION,
                         "v9 entry accepts major=9 only (got %u)",
                         (unsigned)v9.fh.version_major);
            return TC_ERR_UNSUPPORTED_VERSION;
        }
        tc_fill_output_info(&v9.fh, out_info);
        if (planes_out == NULL) { return TC_OK; } /* 仅查询模式 */
        if (v9.fh.frame_type != 0u) {
            tc_set_error(TC_ERR_STATE,
                         "v9 P-frame decode requires tc_gop_context "
                         "(stateless entry is I-only)");
            return TC_ERR_STATE;
        }
        if (request->mode != TC_DECODE_MODE_FULL) {
            tc_set_error(TC_ERR_NOT_IMPLEMENTED, "v9 reduced/auto2k path not defined");
            return TC_ERR_NOT_IMPLEMENTED;
        }
        return v8_frame_decode(data, size, planes_out, strides, out_info);
    }

    topos_packet_view view;
    const int prof = tc_profile_enabled();
    uint64_t t_scan = prof ? tc_profile_now_ns() : 0u;
    rc = tc_packet_parse_structure(data, size, &view);
    if (prof) {
        TC_STATS_LOCK();
        g_dec_stats.scan_ns += tc_profile_now_ns() - t_scan;
        TC_STATS_UNLOCK();
    }
    if (rc != TC_OK) { return rc; }

    tc_decode_plan plan;
    rc = tc_decode_request_resolve(&view.fh, request, &plan);
    if (rc != TC_OK) { return rc; }
    tc_fill_output_info(&view.fh, out_info);
    tc_apply_decode_plan_info(&plan, out_info);
    if (planes_out == NULL) { return TC_OK; }
    rc = tc_v7r3_stateless_p_guard(&view.fh);
    if (rc != TC_OK) { return rc; }
    for (uint32_t p = 0u; p < out_info->plane_count; ++p) {
        if (planes_out[p] == NULL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "request planes_out[%u] == NULL",
                         (unsigned)p);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    if (!plan.scaled) {
        return tc_decode_slices_ex(&view, planes_out, strides, NULL, out_info,
                                   0u, plan.drop_alpha);
    }
    return tc_decode_slices_scaled_limit_ex(
        &view, planes_out, strides, NULL, out_info, 0u,
        plan.target_width, plan.target_height, plan.coefficient_limit,
        plan.drop_alpha);
}

int32_t tc_frame_decode_reduced(const uint8_t* data, size_t size,
                                tc_decode_scale scale,
                                uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                const size_t strides[TC_FRAME_MAX_PLANES],
                                topos_frame_output* out_info)
{
    if (out_info == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "out_info == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (scale == TC_DECODE_SCALE_FULL) {
        return tc_frame_decode(data, size, planes_out, strides, out_info);
    }
    if (data == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "data == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (tc_packet_is_v7b(data, size)) {
        topos_frame_header v7b_fh;
        int32_t v7b_rc = tc_v7_packet_header_decode(data, size, &v7b_fh);
        if (v7b_rc != TC_OK) { return v7b_rc; }
        uint32_t target_width = 0u;
        uint32_t target_height = 0u;
        v7b_rc = tc_decode_scale_dimensions(v7b_fh.visible_width,
                                             v7b_fh.visible_height, scale,
                                             &target_width, &target_height);
        if (v7b_rc != TC_OK) { return v7b_rc; }
        tc_v7b_decode_stats v7b_stats;
        v7b_rc = tc_v7b_frame_decode_reduced(
            data, size, target_width, target_height, planes_out, strides,
            out_info, &v7b_stats);
        if (planes_out != NULL && v7b_rc == TC_OK) {
            tc_dev_decode_stats_add_v7b(v7b_stats.bytes_read,
                                        v7b_stats.base_bytes_read,
                                        v7b_stats.segments_read,
                                        v7b_stats.segments_skipped);
        }
        return v7b_rc;
    }
    if (tc_packet_is_v7a(data, size)) {
        if (tc_frame_header_dev_replay_enabled() == 0) { return v7a_retired_reject(); }
        topos_frame_header v7_fh;
        int32_t v7_rc = tc_v7_packet_header_decode(data, size, &v7_fh);
        if (v7_rc != TC_OK) { return v7_rc; }
        uint32_t target_width = 0u;
        uint32_t target_height = 0u;
        v7_rc = tc_decode_scale_dimensions(v7_fh.visible_width, v7_fh.visible_height,
                                           scale, &target_width, &target_height);
        if (v7_rc != TC_OK) { return v7_rc; }
        return tc_v7_frame_decode_scaled(data, size,
                                         scale == TC_DECODE_SCALE_HALF ? 3u
                                         : scale == TC_DECODE_SCALE_THIRD ? 2u
                                         : scale == TC_DECODE_SCALE_QUARTER ? 1u : 0u,
                                         target_width, target_height,
                                         planes_out, strides, out_info, NULL);
    }
    topos_packet_view view;
    int32_t rc = tc_packet_parse_structure(data, size, &view);
    if (rc != TC_OK) { return rc; }
    uint32_t target_width = 0u;
    uint32_t target_height = 0u;
    rc = tc_decode_scale_dimensions(view.fh.visible_width, view.fh.visible_height,
                                    scale, &target_width, &target_height);
    if (rc != TC_OK) { return rc; }
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = (uint32_t)sizeof(topos_frame_output);
    out_info->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    tc_fill_output_info(&view.fh, out_info);
    {
        const int32_t grc = tc_v7r3_stateless_p_guard(&view.fh);
        if (grc != TC_OK) { return grc; }
    }
    out_info->visible_width = (uint16_t)target_width;
    out_info->visible_height = (uint16_t)target_height;
    out_info->coded_width = (uint16_t)target_width;
    out_info->coded_height = (uint16_t)target_height;
    if (planes_out == NULL) { return TC_OK; }
    for (uint32_t p = 0u; p < view.fh.plane_count; ++p) {
        if (planes_out[p] == NULL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "reduced planes_out[%u] == NULL",
                         (unsigned)p);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    const uint8_t limit = scale == TC_DECODE_SCALE_HALF ? 24u
                        : scale == TC_DECODE_SCALE_THIRD ? 16u
                        : scale == TC_DECODE_SCALE_QUARTER ? 10u : 4u;
    return tc_decode_slices_scaled_limit(&view, planes_out, strides, NULL, out_info, 0u,
                                         target_width, target_height, limit);
}

/* 阶段4：无状态跨帧批量解码——count 帧全部切片进同一线程池批次
 * （跨帧任务图；语义契约见 topos_codec.h tc_frame_decode_batch）。 */
static int32_t tc_frame_decode_batch_mode(const topos_batch_packet* packets, uint32_t count,
                                           tc_decode_scale scale,
                                           uint16_t* const* planes_out, const size_t* strides,
                                           topos_frame_output* infos)
{
    if (count == 0u) { return TC_OK; }
    /* P1-17：产品批量帧数上限——先于一切数组触碰/大分配拒绝 */
    if (count > TC_BATCH_MAX_FRAMES) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "batch count %u > %u",
                     (unsigned)count, (unsigned)TC_BATCH_MAX_FRAMES);
        return TC_ERR_LIMIT_EXCEEDED;
    }
    if (infos == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "infos == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (packets == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "batch packets == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (scale < TC_DECODE_SCALE_FULL || scale > TC_DECODE_SCALE_EIGHTH) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "invalid decode scale %u", (unsigned)scale);
        return TC_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        if (packets[i].data == NULL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "packets[%u].data == NULL",
                         (unsigned)i);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }

    size_t views_bytes = 0u;
    if (!tc_umul_size((size_t)count, sizeof(topos_packet_view), &views_bytes)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "batch views 字节溢出");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    topos_packet_view* views = (topos_packet_view*)tc_alloc(views_bytes);
    if (views == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "decode batch context");
        return TC_ERR_OUT_OF_MEMORY;
    }

    /* M1：入口只做结构解析；任一包失败 → 整批拒绝，不产出任何帧 */
    const int prof = tc_profile_enabled();
    for (uint32_t i = 0u; i < count; ++i) {
        uint64_t t_scan = prof ? tc_profile_now_ns() : 0u;
        int32_t rc = tc_packet_parse_structure(packets[i].data, packets[i].size,
                                               &views[i]);
        if (prof) { TC_STATS_LOCK(); g_dec_stats.scan_ns += tc_profile_now_ns() - t_scan; TC_STATS_UNLOCK(); }
        if (rc != TC_OK) {
            tc_free(views);
            return rc;
        }
    }

    /* 查询模式：planes_out == NULL → 仅填充几何，不重建（同单帧两段式） */
    if (planes_out == NULL) {
        for (uint32_t i = 0u; i < count; ++i) {
            memset(&infos[i], 0, sizeof(infos[i]));
            infos[i].struct_size = (uint32_t)sizeof(topos_frame_output);
            infos[i].abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
            tc_fill_output_info(&views[i].fh, &infos[i]);
            if (scale != TC_DECODE_SCALE_FULL) {
                uint32_t tw = 0u;
                uint32_t th = 0u;
                (void)tc_decode_scale_dimensions(views[i].fh.visible_width,
                                                  views[i].fh.visible_height,
                                                  scale, &tw, &th);
                infos[i].visible_width = (uint16_t)tw;
                infos[i].visible_height = (uint16_t)th;
                infos[i].coded_width = (uint16_t)tw;
                infos[i].coded_height = (uint16_t)th;
            }
        }
        tc_free(views);
        return TC_OK;
    }

    size_t reqs_bytes = 0u;
    if (!tc_umul_size((size_t)count, sizeof(dec_frame_req), &reqs_bytes)) {
        tc_free(views);
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "batch reqs 字节溢出");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    dec_frame_req* reqs = (dec_frame_req*)tc_alloc(reqs_bytes);
    if (reqs == NULL) {
        tc_free(views);
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "decode batch context");
        return TC_ERR_OUT_OF_MEMORY;
    }

    for (uint32_t i = 0u; i < count; ++i) {
        topos_frame_output* info = &infos[i];
        memset(info, 0, sizeof(*info));
        info->struct_size = (uint32_t)sizeof(topos_frame_output);
        info->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        tc_fill_output_info(&views[i].fh, info);
        uint32_t target_width = (uint32_t)info->visible_width;
        uint32_t target_height = (uint32_t)info->visible_height;
        uint8_t coefficient_limit = 63u;
        if (scale != TC_DECODE_SCALE_FULL) {
            (void)tc_decode_scale_dimensions((uint32_t)info->visible_width,
                                              (uint32_t)info->visible_height,
                                              scale, &target_width, &target_height);
            info->visible_width = (uint16_t)target_width;
            info->visible_height = (uint16_t)target_height;
            info->coded_width = (uint16_t)target_width;
            info->coded_height = (uint16_t)target_height;
            coefficient_limit = scale == TC_DECODE_SCALE_HALF ? 24u
                              : scale == TC_DECODE_SCALE_THIRD ? 16u
                              : scale == TC_DECODE_SCALE_QUARTER ? 10u : 4u;
        }

        {
            const int32_t grc = tc_v7r3_stateless_p_guard(&views[i].fh);
            if (grc != TC_OK) {
                tc_free(views);
                return grc;
            }
        }
        const size_t base = (size_t)i * TC_FRAME_MAX_PLANES; /* P1-17 */
        dec_frame_req* r = &reqs[i];
        memset(r, 0, sizeof(*r));
        r->view = &views[i];
        r->info = info;
        r->target_width = target_width;
        r->target_height = target_height;
        r->scaled = scale != TC_DECODE_SCALE_FULL ? 1u : 0u;
        r->coefficient_limit = coefficient_limit;
        for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
            r->planes[p] = planes_out[base + p];
            r->strides[p] = strides != NULL ? strides[base + p] : 0u;
        }
        /* 单帧路径同位校验：plane 指针 NULL → 该帧拒绝（其余帧不受影响） */
        for (uint32_t p = 0u; p < views[i].fh.plane_count; ++p) {
            if (r->planes[p] == NULL) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT,
                             "frame %u planes_out[%u] == NULL", (unsigned)i,
                             (unsigned)p);
                r->rc = TC_ERR_INVALID_ARGUMENT;
                break;
            }
        }
    }

    const int32_t first = tc_decode_frames(reqs, count);
    tc_free(views);
    tc_free(reqs);
    return first;
}

int32_t tc_frame_decode_batch(const topos_batch_packet* packets, uint32_t count,
                              uint16_t* const* planes_out, const size_t* strides,
                              topos_frame_output* infos)
{
    return tc_frame_decode_batch_mode(packets, count, TC_DECODE_SCALE_FULL,
                                      planes_out, strides, infos);
}

int32_t tc_frame_decode_batch_reduced(const topos_batch_packet* packets, uint32_t count,
                                      tc_decode_scale scale,
                                      uint16_t* const* planes_out, const size_t* strides,
                                      topos_frame_output* infos)
{
    return tc_frame_decode_batch_mode(packets, count, scale, planes_out, strides, infos);
}

int32_t tc_frame_plane_geometry(const topos_frame_output* info, uint32_t plane,
                                uint32_t* width, uint32_t* height)
{
    if (info == NULL || width == NULL || height == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "plane geometry args NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (plane >= info->plane_count) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "plane %u >= plane_count %u",
                     (unsigned)plane, (unsigned)info->plane_count);
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (info->pixel_format == 3u) {
        /* TRAW CFA（批 1）：4 相位平面各 (ceil(w/2))×(ceil(h/2)) */
        *width = ((uint32_t)info->visible_width + 1u) / 2u;
        *height = ((uint32_t)info->visible_height + 1u) / 2u;
        return TC_OK;
    }
    /* R4.2/R4.3：pf≠0（4:4:4/GBR）U/V 全宽；4:2:2（pf=0）保持 ceil(visible/2) */
    uint32_t chroma_w = (info->pixel_format == 0u)
                            ? ((uint32_t)info->visible_width + 1u) / 2u
                            : (uint32_t)info->visible_width;
    *width = (plane == 1u || plane == 2u) ? chroma_w
                                          : (uint32_t)info->visible_width;
    *height = (uint32_t)info->visible_height;
    return TC_OK;
}
