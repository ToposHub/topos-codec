/* gop_context.c —— V9 帧间微 GOP context（topos_v9_micro_gop_plan 批 2；
 * ADR-C047 zero-motion IP-2）。
 *
 * 职责：GOP 序列状态机 + 参考帧事务 + zero-motion P 帧的残差合成/重建链
 *   （合成/重建为行带池并行——逐像素语义与线程数无关，LP 实时化
 *   2026-09-14）。
 * 包布局 V8 同构（§3.5 冻结）→ 像素编解码直连 V8 机器：
 *   - I 帧：源帧直接经 v8_frame_encode_ready/v8_frame_decode；
 *   - P 帧：X = clip(curr − ref + bd_mid) 残差伪图（§2.3 域重合，免电平
 *     偏移）→ 编码成 V9 包；解码侧 out = clip(ref + X′ − bd_mid)；
 *   - recon 链：编码器重建 = 解码器实现同一函数（decode + clip 加法），
 *     逐位一致由构造保证（零漂的直接证据 = 重复帧 P 残差恒 0 → recon
 *     逐位等于参考，见 test_v9_codec / golden_v9）。
 *
 * I 回退判定（§3.6）：force_intra / 参考不 READY → I；P 先编，
 * P_size ≥ last_I_size → 弃 P 改编 I（内容切换/运动失稳的廉价预判——
 * P 对正确参考应远小于 I；以最近 I 尺寸作 I 候选估计，免每帧双编）。
 * P 白编预检（ADR-C049，2026-09-14）：残差 MAD ≥ 1<<(bd−6) 时跳过 P
 * 尝试直编 I——实测 P 胜层 MAD ≤5.5、全回退层 ≥32.5，判定产物与回退
 * 路径逐字节相同（直编 I ≡ 回退 I）。全零残差精确跳过：X≡0 ⇒ X′≡0
 * （量化不动点）⇒ recon=ref，跳过自解码/重建。
 * 决定规则确定性、线程无关（MAD/非零计数为整数归约）。
 *
 * 序列规则（§3.2/§3.8）：NO_REF 下 P → MALFORMED；REF_INVALID 下 P →
 * TC_ERR_REFERENCE_INVALID；跨 GOP P → MALFORMED；I 恒合法（编码器侧
 * 每 I 递增 gop_id 回绕 u16；解码端接受任意 gop_id 的 I）。
 * 事务：staging 态入口克隆，整帧成功才提交；abort 回滚（参考槽双缓冲
 * swap，失败即不 swap）。feed 前校验包几何与 ctx cfg 一致（scratch 按
 * cfg 分配——异几何包在此拒绝，杜绝解码越界）。
 * 参考链归属：encode_frame 与 feed 共享同一条参考链——对同一帧交错
 * 调用两者会把 P 残差应用两次（recon 复查：V9 复审 2026-09-14）。
 * 解码链建模须用独立实例（golden_v9 / test_v9_codec 的 dctx）。
 * conceal 闸（§3.8）：坏瓦片 CRC 的帧按 concealed_slices>0 拒绝且不
 * 安装为参考（V8/V7 解码对坏 CRC 都是 conceal 继续，返回 TC_OK——
 * 静默漂移源）。
 * 载体（ADR-C048）：cfg reserved[0]=10 → V9（major 9，V8 同构包，
 * scan_v8 + v8 像素机）；=11 → V7-R3（major 7 em 8，V7 同构包，
 * parse_structure/scan + V7 band 并行机器经 tc_decode_slices）。序列
 * 规则/事务/conceal 闸对两载体完全同构。
 * 单实例不可重入；不同实例可并发（实例私有状态）。 */
#include "gop_context.h"

#include <string.h>

#include "../common/alloc.h"
#include "../common/error.h"
#include "../bitstream/packet.h"
#include "codec.h"

#define TC_GOP_CTX_MAGIC 0x474F5039u /* "GOP9" */
#define TC_GOP_MAX_PLANES 3u         /* V9 颜色三平面（no-alpha capability） */

struct topos_gop_context {
    uint32_t magic;
    topos_frame_config cfg;      /* 已校验的 V9 cfg */
    /* 几何（visible 域；V8 像素机读写 visible 区域） */
    uint32_t planes;
    uint32_t vis_w[TC_GOP_MAX_PLANES];
    uint32_t vis_h[TC_GOP_MAX_PLANES];
    uint32_t bd_mid;
    /* 参考帧槽（committed）+ 工作槽（staging/残差）——swap 式事务 */
    uint16_t* ref[TC_GOP_MAX_PLANES];
    uint16_t* scratch[TC_GOP_MAX_PLANES];
    /* committed：最近一次成功提交的序列状态 */
    uint32_t ref_state;          /* topos_gop_ref_state */
    uint16_t gop_id;             /* 当前 GOP id（最近一次 I 的帧头值） */
    uint32_t samples;            /* 已提交帧数（observe/feed 序号） */
    uint32_t last_i_bytes;       /* 最近 I 包大小（P 回退阈值；0=尚无） */
    uint16_t next_gop_id;        /* 编码器侧 I 递增计数（回绕 u16） */
    /* 编码包缓冲常驻（LP 实时化 2026-09-14）：packet_bound 每帧 tc_alloc
     * + free 会让大块 mmap 反复建页/回收（2K ~67MB、4K ~268MB），改
     * ctx 内 grow-only 缓存，close 统一释放 */
    uint8_t* enc_buf[2];         /* [0]=P 候选 / [1]=I 候选 */
    size_t enc_cap[2];
    /* staging：本次调用的工作态（入口克隆，成功才写回） */
    uint32_t st_ref_state;
    uint16_t st_gop_id;
    uint32_t st_samples;
};

static int gop_ctx_valid(const topos_gop_context* ctx)
{
    return ctx != NULL && ctx->magic == TC_GOP_CTX_MAGIC;
}

static void gop_fill_info(const topos_gop_context* ctx, const topos_frame_header* fh,
                          topos_gop_frame_info* info)
{
    if (info == NULL) { return; }
    memset(info, 0, sizeof(*info));
    info->struct_size = (uint32_t)sizeof(*info);
    info->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    info->frame_type = fh->frame_type;
    info->ref_distance = fh->ref_distance;
    info->gop_id = fh->gop_id;
    info->sample_index = ctx->st_samples;
    info->ref_state = ctx->st_ref_state;
}

static uint32_t gop_plane_vis_w(uint8_t pixel_format, uint16_t visible_width)
{
    return pixel_format == 0u ? ((uint32_t)visible_width + 1u) / 2u
                              : (uint32_t)visible_width;
}

static void gop_free_slots(topos_gop_context* ctx)
{
    for (uint32_t p = 0u; p < TC_GOP_MAX_PLANES; ++p) {
        if (ctx->ref[p] != NULL) { tc_free(ctx->ref[p]); ctx->ref[p] = NULL; }
        if (ctx->scratch[p] != NULL) { tc_free(ctx->scratch[p]); ctx->scratch[p] = NULL; }
    }
    for (uint32_t b = 0u; b < 2u; ++b) {
        if (ctx->enc_buf[b] != NULL) { tc_free(ctx->enc_buf[b]); ctx->enc_buf[b] = NULL; }
        ctx->enc_cap[b] = 0u;
    }
}

/* 编码包缓冲（槽位 grow-only；cap 由 ctx cfg 唯一确定，实际只分配一次） */
static int32_t gop_enc_buf_get(topos_gop_context* ctx, uint32_t slot, size_t cap,
                               uint8_t** out)
{
    if (ctx->enc_cap[slot] < cap) {
        uint8_t* nb = (uint8_t*)tc_realloc(ctx->enc_buf[slot], cap);
        if (nb == NULL) {
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "gop encode buffer");
            return TC_ERR_OUT_OF_MEMORY;
        }
        ctx->enc_buf[slot] = nb;
        ctx->enc_cap[slot] = cap;
    }
    *out = ctx->enc_buf[slot];
    return TC_OK;
}

static int32_t gop_alloc_slots(topos_gop_context* ctx)
{
    if (ctx->ref[0] != NULL) { return TC_OK; } /* 已分配 */
    for (uint32_t p = 0u; p < ctx->planes; ++p) {
        const size_t n = (size_t)ctx->vis_w[p] * ctx->vis_h[p];
        ctx->ref[p] = (uint16_t*)tc_alloc(n * sizeof(uint16_t));
        ctx->scratch[p] = (uint16_t*)tc_alloc(n * sizeof(uint16_t));
        if (ctx->ref[p] == NULL || ctx->scratch[p] == NULL) {
            gop_free_slots(ctx);
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "gop reference slots");
            return TC_ERR_OUT_OF_MEMORY;
        }
        memset(ctx->ref[p], 0, n * sizeof(uint16_t));
    }
    return TC_OK;
}

static void gop_stage_reset(topos_gop_context* ctx)
{
    ctx->st_ref_state = ctx->ref_state;
    ctx->st_gop_id = ctx->gop_id;
    ctx->st_samples = ctx->samples;
}

/* 载体扫描（ADR-C048）：按包身份分派——V9 走 scan_v8_ex（V8 同构，
 * verify_crc 控制瓦片 CRC 重算），其余走 V7 家族扫描（tc_packet_scan
 * = 结构+CRC / parse_structure = 仅结构；parse_structure 对 major≥8
 * 自带 UNSUPPORTED 拒绝）。返回 TC_OK 时 *fh 填充。 */
static int32_t gop_scan(const uint8_t* data, size_t size, int verify_crc,
                        topos_frame_header* fh)
{
    if (tc_packet_is_v9(data, size)) {
        topos_v8_packet_view v;
        int32_t rc = tc_packet_scan_v8_ex(data, size, &v, verify_crc);
        if (rc != TC_OK) { return rc; }
        *fh = v.fh;
        return TC_OK;
    }
    topos_packet_view v;
    int32_t rc = verify_crc ? tc_packet_scan(data, size, &v)
                            : tc_packet_parse_structure(data, size, &v);
    if (rc != TC_OK) { return rc; }
    *fh = v.fh;
    return TC_OK;
}

/* 序列状态机（observe/feed 共用）：扫描 + 几何一致 + 帧型/gop 规则。
 * 返回 TC_OK 时 fh 填充且 staging 已按规则推进（未提交）。
 * to_commit=1（observe）：扫描/几何失败立即提交 REF_INVALID；
 * to_commit=0（feed 前置）：失败不动 staging（decode 失败由 feed 置）。
 * 序列违规（P-first/跨 GOP/REF_INVALID）两种模式下都不动状态——参考
 * 未被触碰，仍有效。 */
static int32_t gop_sequence_check(topos_gop_context* ctx, const uint8_t* data,
                                  size_t size, topos_frame_header* fh,
                                  int verify_crc, int to_commit)
{
    int32_t rc = gop_scan(data, size, verify_crc, fh);
    if (rc != TC_OK) {
        if (to_commit) {
            ctx->st_ref_state = (uint32_t)TOPOS_GOP_REF_INVALID;
            ctx->ref_state = ctx->st_ref_state;
        }
        return rc;
    }
    const int is_v9 = fh->version_major == 9u;
    const int is_r3 = fh->version_major == 7u && fh->entropy_mode == 8u;
    if (!is_v9 && !is_r3) {
        tc_set_error(TC_ERR_UNSUPPORTED_VERSION,
                     "gop context accepts v9/v7r3 packets only "
                     "(got major %u em %u)",
                     (unsigned)fh->version_major, (unsigned)fh->entropy_mode);
        rc = TC_ERR_UNSUPPORTED_VERSION;
    } else if (fh->visible_width != ctx->cfg.visible_width ||
               fh->visible_height != ctx->cfg.visible_height ||
               fh->pixel_format != ctx->cfg.pixel_format ||
               fh->bit_depth != ctx->cfg.bit_depth ||
               fh->plane_count != ctx->planes) {
        /* 几何不一致：scratch 按 cfg 分配，异几何包在此拒绝（防越界） */
        tc_set_error(TC_ERR_MALFORMED,
                     "gop packet geometry %ux%u pf%u bd%u != context cfg",
                     (unsigned)fh->visible_width,
                     (unsigned)fh->visible_height,
                     (unsigned)fh->pixel_format,
                     (unsigned)fh->bit_depth);
        rc = TC_ERR_MALFORMED;
    }
    if (rc != TC_OK) {
        if (to_commit) {
            ctx->st_ref_state = (uint32_t)TOPOS_GOP_REF_INVALID;
            ctx->ref_state = ctx->st_ref_state;
        }
        return rc;
    }
    if (fh->frame_type == 0u) {
        ctx->st_gop_id = fh->gop_id;
        ctx->st_ref_state = (uint32_t)TOPOS_GOP_REF_READY;
    } else {
        if (ctx->st_ref_state == (uint32_t)TOPOS_GOP_NO_REF) {
            tc_set_error(TC_ERR_MALFORMED,
                         "gop sequence: first frame must be I (got P)");
            return TC_ERR_MALFORMED;
        }
        if (ctx->st_ref_state == (uint32_t)TOPOS_GOP_REF_INVALID) {
            tc_set_error(TC_ERR_REFERENCE_INVALID,
                         "gop sequence: reference invalid (await next I)");
            return TC_ERR_REFERENCE_INVALID;
        }
        if (fh->gop_id != ctx->st_gop_id) {
            tc_set_error(TC_ERR_MALFORMED,
                         "gop sequence: cross-GOP reference (P gop %u != %u)",
                         (unsigned)fh->gop_id, (unsigned)ctx->st_gop_id);
            return TC_ERR_MALFORMED;
        }
        ctx->st_ref_state = (uint32_t)TOPOS_GOP_REF_READY;
    }
    return TC_OK;
}

int32_t tc_gop_context_create(const topos_frame_config* cfg, topos_gop_context** out)
{
    if (out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "out == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (cfg == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "cfg == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    int32_t rc = tc_frame_config_validate(cfg);
    if (rc != TC_OK) { return rc; }
    const uint8_t em = (uint8_t)(cfg->reserved[0] & 0xFFu);
    if (em != 10u && em != 11u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "gop context requires temporal cfg (reserved[0] 10=v9 / "
                     "11=v7r3, got %u)",
                     (unsigned)em);
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* no-alpha capability（计划 §3.7 允许的显式收缩；禁静默降级）。
     * CFA pf=3 随 TRAW（V7-R2 链），V9 三平面颜色机器不承载。 */
    if (cfg->alpha_mode != 0u) {
        tc_set_error(TC_ERR_NOT_IMPLEMENTED,
                     "v9 P0 is color-only (no-alpha capability, plan §3.7)");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    if (cfg->pixel_format == 3u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "v9 does not carry CFA (TRAW uses rans2)");
        return TC_ERR_INVALID_ARGUMENT;
    }
    topos_gop_context* ctx = (topos_gop_context*)tc_alloc(sizeof(*ctx));
    if (ctx == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "gop context");
        return TC_ERR_OUT_OF_MEMORY;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->magic = TC_GOP_CTX_MAGIC;
    ctx->cfg = *cfg;
    ctx->planes = 3u;
    ctx->vis_w[0] = cfg->visible_width;
    ctx->vis_h[0] = cfg->visible_height;
    for (uint32_t p = 1u; p < 3u; ++p) {
        ctx->vis_w[p] = gop_plane_vis_w(cfg->pixel_format, cfg->visible_width);
        ctx->vis_h[p] = cfg->visible_height;
    }
    ctx->bd_mid = 1u << (cfg->bit_depth - 1u);
    ctx->ref_state = (uint32_t)TOPOS_GOP_NO_REF;
    ctx->st_ref_state = (uint32_t)TOPOS_GOP_NO_REF;
    *out = ctx;
    return TC_OK;
}

void tc_gop_context_close(topos_gop_context* ctx)
{
    if (ctx == NULL) { return; }
    gop_free_slots(ctx);
    tc_free(ctx);
}

int32_t tc_gop_context_observe(topos_gop_context* ctx, const uint8_t* data,
                               size_t size, topos_gop_frame_info* info)
{
    if (!gop_ctx_valid(ctx)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "gop context invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (data == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "data == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    gop_stage_reset(ctx);
    topos_frame_header fh;
    int32_t rc = gop_sequence_check(ctx, data, size, &fh, 1, 1);
    if (rc != TC_OK) { return rc; }
    gop_fill_info(ctx, &fh, info);
    ctx->st_samples += 1u;
    /* 提交事务 */
    ctx->ref_state = ctx->st_ref_state;
    ctx->gop_id = ctx->st_gop_id;
    ctx->samples = ctx->st_samples;
    return TC_OK;
}

/* 编码帧配置：I 开新 GOP（next_gop_id+1）；P 携当前 GOP */
static void gop_build_cfg(const topos_gop_context* ctx, int intra,
                          topos_frame_config* out)
{
    *out = ctx->cfg;
    if (intra) {
        out->reserved[3] = 0u;
        out->reserved[4] = (uint32_t)(ctx->next_gop_id + 1u);
        out->reserved[5] = 0u;
    } else {
        out->reserved[3] = 1u;
        out->reserved[4] = (uint32_t)ctx->gop_id;
        out->reserved[5] = 1u;
    }
}

/* 残差合成/重建标量环 → 行带池并行（LP 实时化 2026-09-14）：逐像素语义
 * 逐位不变、行间独立，band 任务经常驻池动态领取——结果与参与线程数
 * 无关（确定性硬门禁）。每平面带数 ≤ GOP_BAND_MAX（3 平面 ≤96 任务，
 * 栈上数组）；nw ≤1 时 tc_parallel_for 顺序内联。 */

#define GOP_BAND_MAX 32u

typedef struct gop_res_band {
    const uint16_t* cur;   /* 输入行基址（行距 cur_stride） */
    const uint16_t* ref;   /* 参考行基址（紧凑） */
    uint16_t* dst;         /* scratch 行基址（紧凑） */
    size_t cur_stride;
    uint32_t w;
    uint32_t y0;
    uint32_t h;
    uint32_t mid;
    uint32_t maxv;
    /* 归约（join 后由调用线程汇总；整数加法次序无关 → 确定性）：
     * sad = Σ|cur−ref|（P 白编预检 MAD 门），nz = 非零残差计数（全零
     * 残差 ⇒ X′≡0 ⇒ recon=ref 的精确跳过依据） */
    uint64_t sad;
    uint64_t nz;
} gop_res_band;

static void gop_res_band_fn(void* vctx)
{
    gop_res_band* b = (gop_res_band*)vctx;
    uint64_t sad = 0u;
    uint64_t nz = 0u;
    for (uint32_t y = b->y0; y < b->y0 + b->h; ++y) {
        const uint16_t* crow = b->cur + (size_t)y * b->cur_stride;
        const uint16_t* rrow = b->ref + (size_t)y * b->w;
        uint16_t* drow = b->dst + (size_t)y * b->w;
        for (uint32_t x = 0u; x < b->w; ++x) {
            const int32_t d = (int32_t)crow[x] - (int32_t)rrow[x];
            sad += (uint64_t)(d < 0 ? -d : d);
            nz += (uint64_t)(d != 0);
            int32_t v = d + (int32_t)b->mid;
            if (v < 0) { v = 0; }
            if (v > (int32_t)b->maxv) { v = (int32_t)b->maxv; }
            drow[x] = (uint16_t)v;
        }
    }
    b->sad = sad;
    b->nz = nz;
}

typedef struct gop_rec_band {
    const uint16_t* src;   /* X′ 行基址（紧凑） */
    uint16_t* ref;         /* 就地推进 */
    uint16_t* out;         /* 可 NULL（编码器侧只推进 ref） */
    size_t out_stride;
    uint32_t w;
    uint32_t y0;
    uint32_t h;
    uint32_t mid;
    uint32_t maxv;
} gop_rec_band;

static void gop_rec_band_fn(void* vctx)
{
    const gop_rec_band* b = (const gop_rec_band*)vctx;
    for (uint32_t y = b->y0; y < b->y0 + b->h; ++y) {
        const uint16_t* srow = b->src + (size_t)y * b->w;
        uint16_t* rrow = b->ref + (size_t)y * b->w;
        uint16_t* drow = (b->out != NULL) ? b->out + (size_t)y * b->out_stride : NULL;
        for (uint32_t x = 0u; x < b->w; ++x) {
            int32_t v = (int32_t)rrow[x] + (int32_t)srow[x] - (int32_t)b->mid;
            if (v < 0) { v = 0; }
            if (v > (int32_t)b->maxv) { v = (int32_t)b->maxv; }
            if (drow != NULL) { drow[x] = (uint16_t)v; }
            rrow[x] = (uint16_t)v; /* ref 就地推进（P recon = out） */
        }
    }
}

/* I 帧输出行拷贝（feed 路径 ref→out；4K 33MB 串行 memcpy ~3ms → 并行） */
typedef struct gop_cpy_band {
    const uint16_t* src;   /* 紧凑参考行 */
    uint16_t* dst;
    size_t dst_stride;
    uint32_t w;
    uint32_t y0;
    uint32_t h;
} gop_cpy_band;

static void gop_cpy_band_fn(void* vctx)
{
    const gop_cpy_band* b = (const gop_cpy_band*)vctx;
    const size_t bytes = (size_t)b->w * sizeof(uint16_t);
    for (uint32_t y = b->y0; y < b->y0 + b->h; ++y) {
        memcpy(b->dst + (size_t)y * b->dst_stride, b->src + (size_t)y * b->w, bytes);
    }
}

/* 残差伪图 X = clip(curr − ref + mid)（scratch ← input − ref）。
 * sad/nz（可 NULL）= |cur−ref| 和 ≠0 样本的全帧归约。 */
static void gop_residual_into_scratch(topos_gop_context* ctx,
                                      const topos_frame_input* input,
                                      uint64_t* out_sad, uint64_t* out_nz)
{
    const uint32_t nw = (uint32_t)tc_dev_thread_count();
    uint32_t per_plane = nw * 2u;
    if (per_plane > GOP_BAND_MAX) { per_plane = GOP_BAND_MAX; }
    tc_job jobs[TC_GOP_MAX_PLANES * GOP_BAND_MAX];
    gop_res_band bands[TC_GOP_MAX_PLANES * GOP_BAND_MAX];
    uint32_t ti = 0u;
    uint64_t sad = 0u;
    uint64_t nz = 0u;
    for (uint32_t p = 0u; p < ctx->planes; ++p) {
        const uint32_t w = ctx->vis_w[p];
        const uint32_t rows = ctx->vis_h[p];
        const uint16_t* cur = (const uint16_t*)input->planes[p];
        const size_t cstride = input->strides[p] == 0 ? w : input->strides[p];
        const uint16_t* ref = ctx->ref[p];
        uint16_t* dst = ctx->scratch[p];
        const uint32_t bh = (rows + per_plane - 1u) / per_plane;
        const uint32_t mid = ctx->bd_mid;
        const uint32_t maxv = 2u * mid - 1u;
        for (uint32_t y0 = 0u; y0 < rows; y0 += bh) {
            uint32_t hh = rows - y0;
            if (hh > bh) { hh = bh; }
            gop_res_band* b = &bands[ti];
            b->cur = cur;
            b->ref = ref;
            b->dst = dst;
            b->cur_stride = cstride;
            b->w = w;
            b->y0 = y0;
            b->h = hh;
            b->mid = mid;
            b->maxv = maxv;
            b->sad = 0u;
            b->nz = 0u;
            jobs[ti].fn = gop_res_band_fn;
            jobs[ti].ctx = b;
            ++ti;
        }
    }
    (void)tc_parallel_for(jobs, ti, nw);
    for (uint32_t i = 0u; i < ti; ++i) {
        sad += bands[i].sad;
        nz += bands[i].nz;
    }
    if (out_sad != NULL) { *out_sad = sad; }
    if (out_nz != NULL) { *out_nz = nz; }
}

/* P 重建：out（可 NULL）= clip(ref + X′ − mid)；ref 就地推进为 out */
static void gop_reconstruct_p(topos_gop_context* ctx,
                              const topos_plane_view out[TC_FRAME_MAX_PLANES])
{
    const uint32_t nw = (uint32_t)tc_dev_thread_count();
    uint32_t per_plane = nw * 2u;
    if (per_plane > GOP_BAND_MAX) { per_plane = GOP_BAND_MAX; }
    tc_job jobs[TC_GOP_MAX_PLANES * GOP_BAND_MAX];
    gop_rec_band bands[TC_GOP_MAX_PLANES * GOP_BAND_MAX];
    uint32_t ti = 0u;
    for (uint32_t p = 0u; p < ctx->planes; ++p) {
        const uint32_t w = ctx->vis_w[p];
        const uint32_t rows = ctx->vis_h[p];
        const uint16_t* src = ctx->scratch[p];
        uint16_t* ref = ctx->ref[p];
        uint16_t* dst = (out != NULL) ? out[p].pixels : NULL;
        const size_t dstride = (out != NULL && out[p].stride != 0) ? out[p].stride : w;
        const uint32_t bh = (rows + per_plane - 1u) / per_plane;
        const uint32_t mid = ctx->bd_mid;
        const uint32_t maxv = 2u * mid - 1u;
        for (uint32_t y0 = 0u; y0 < rows; y0 += bh) {
            uint32_t hh = rows - y0;
            if (hh > bh) { hh = bh; }
            gop_rec_band* b = &bands[ti];
            b->src = src;
            b->ref = ref;
            b->out = dst;
            b->out_stride = dstride;
            b->w = w;
            b->y0 = y0;
            b->h = hh;
            b->mid = mid;
            b->maxv = maxv;
            jobs[ti].fn = gop_rec_band_fn;
            jobs[ti].ctx = b;
            ++ti;
        }
    }
    (void)tc_parallel_for(jobs, ti, nw);
}

/* 像素解码（载体分派）：V9 → v8_frame_decode；V7-R3 → parse_structure +
 * tc_decode_slices（内部入口，绕过 tc_decode_frames 的 P 包 STATE 闸——
 * 序列合法性由本 context 的状态机裁决）。planes 紧凑（scratch 布局）；
 * finfo 必填（v8 契约 + concealed 计数消费）。 */
static int32_t gop_decode_pixels(const topos_gop_context* ctx,
                                 const uint8_t* data, size_t size,
                                 uint16_t* const planes[TC_FRAME_MAX_PLANES],
                                 topos_frame_output* finfo)
{
    if ((ctx->cfg.reserved[0] & 0xFFu) == 10u) {
        return v8_frame_decode(data, size, planes, NULL, finfo);
    }
    topos_packet_view view;
    int32_t rc = tc_packet_parse_structure(data, size, &view);
    if (rc != TC_OK) { return rc; }
    return tc_decode_slices(&view, planes, NULL, NULL, finfo, 0u);
}

int32_t tc_gop_context_feed(topos_gop_context* ctx, const uint8_t* data, size_t size,
                            const topos_plane_view out[TC_FRAME_MAX_PLANES],
                            topos_gop_frame_info* info)
{
    if (!gop_ctx_valid(ctx)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "gop context invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (data == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "data == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    int32_t rc = gop_alloc_slots(ctx);
    if (rc != TC_OK) { return rc; }
    gop_stage_reset(ctx);

    topos_frame_header fh;
    rc = gop_sequence_check(ctx, data, size, &fh, 0, 0);
    if (rc != TC_OK) {
        /* 序列违规/几何不一致：状态不变（参考未被触碰） */
        return rc;
    }

    if (out != NULL) {
        for (uint32_t p = 0u; p < ctx->planes; ++p) {
            if (out[p].pixels == NULL) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT, "out[%u].pixels == NULL",
                             (unsigned)p);
                return TC_ERR_INVALID_ARGUMENT;
            }
        }
    }

    uint16_t* const planes[TC_FRAME_MAX_PLANES] = {
        ctx->scratch[0], ctx->scratch[1], ctx->scratch[2], NULL
    };
    topos_frame_output finfo; /* 解码核心要求非 NULL out_info */
    memset(&finfo, 0, sizeof(finfo));
    finfo.struct_size = (uint32_t)sizeof(finfo);
    finfo.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    rc = gop_decode_pixels(ctx, data, size, planes, &finfo);
    if (rc == TC_OK && finfo.concealed_slices != 0u) {
        /* §3.8：坏瓦片/slice conceal 的帧不得安装为参考（解码核心
         * conceal 继续仍返回 TC_OK——静默漂移源，recon 复审 2026-09-14） */
        tc_set_error(TC_ERR_MALFORMED,
                     "gop feed: %u concealed tiles (frame not installable "
                     "as reference, plan §3.8)",
                     (unsigned)finfo.concealed_slices);
        rc = TC_ERR_MALFORMED;
    }
    if (rc == TC_OK && fh.frame_type == 0u) {
        /* I：swap scratch→ref（新参考 = 解码面）；旧 ref 槽回收为 scratch */
        for (uint32_t p = 0u; p < ctx->planes; ++p) {
            uint16_t* tmp = ctx->ref[p];
            ctx->ref[p] = ctx->scratch[p];
            ctx->scratch[p] = tmp;
        }
        if (out != NULL) {
            /* 行带并行拷贝（I：新参考 = 解码面 → out；4K 33MB 串行 ~3ms） */
            const uint32_t nw = (uint32_t)tc_dev_thread_count();
            uint32_t per_plane = nw * 2u;
            if (per_plane > GOP_BAND_MAX) { per_plane = GOP_BAND_MAX; }
            tc_job jobs[TC_GOP_MAX_PLANES * GOP_BAND_MAX];
            gop_cpy_band bands[TC_GOP_MAX_PLANES * GOP_BAND_MAX];
            uint32_t ti = 0u;
            for (uint32_t p = 0u; p < ctx->planes; ++p) {
                const uint32_t w = ctx->vis_w[p];
                const size_t dstride = out[p].stride == 0 ? w : out[p].stride;
                const uint32_t bh = (ctx->vis_h[p] + per_plane - 1u) / per_plane;
                for (uint32_t y0 = 0u; y0 < ctx->vis_h[p]; y0 += bh) {
                    uint32_t hh = ctx->vis_h[p] - y0;
                    if (hh > bh) { hh = bh; }
                    gop_cpy_band* b = &bands[ti];
                    b->src = ctx->ref[p];
                    b->dst = out[p].pixels;
                    b->dst_stride = dstride;
                    b->w = w;
                    b->y0 = y0;
                    b->h = hh;
                    jobs[ti].fn = gop_cpy_band_fn;
                    jobs[ti].ctx = b;
                    ++ti;
                }
            }
            (void)tc_parallel_for(jobs, ti, nw);
        }
    } else if (rc == TC_OK) {
        /* P：解码残差伪图 X′ → out = clip(ref + X′ − mid)；ref 就地推进 */
        gop_reconstruct_p(ctx, out);
    }
    if (rc != TC_OK) {
        /* 解码失败：该帧不得安装为参考（§3.8）→ REF_INVALID 提交 */
        ctx->st_ref_state = (uint32_t)TOPOS_GOP_REF_INVALID;
        ctx->ref_state = ctx->st_ref_state;
        return rc;
    }
    gop_fill_info(ctx, &fh, info);
    ctx->st_samples += 1u;
    ctx->ref_state = ctx->st_ref_state;
    ctx->gop_id = ctx->st_gop_id;
    ctx->samples = ctx->st_samples;
    return TC_OK;
}

int32_t tc_gop_context_encode_frame(topos_gop_context* ctx,
                                    const topos_frame_input* input,
                                    uint8_t* out, size_t out_cap, size_t* need_size,
                                    int force_intra, topos_frame_stats* stats)
{
    if (!gop_ctx_valid(ctx)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "gop context invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (input == NULL || input->planes[0] == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "input == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (need_size != NULL) { *need_size = 0u; }
    int32_t rc = gop_alloc_slots(ctx);
    if (rc != TC_OK) { return rc; }
    gop_stage_reset(ctx);

    const int want_intra = force_intra != 0
        || ctx->st_ref_state != (uint32_t)TOPOS_GOP_REF_READY;
    topos_frame_config cfg_p;
    gop_build_cfg(ctx, want_intra, &cfg_p);
    const size_t cap = tc_frame_packet_bound(&ctx->cfg);
    uint8_t* buf_p = NULL;
    uint8_t* buf_i = NULL;
    topos_frame_stats st_p;
    topos_frame_stats st_i;
    memset(&st_p, 0, sizeof(st_p));
    st_p.struct_size = (uint32_t)sizeof(st_p);
    st_p.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    st_i = st_p;
    /* 包缓冲 ctx 常驻（grow-only）——每帧 tc_alloc/free 大块 mmap 的
     * 建页/回收开销消除（LP 实时化 2026-09-14） */
    rc = gop_enc_buf_get(ctx, 0u, cap, &buf_p);
    if (rc != TC_OK) { return rc; }

    size_t p_size = 0u;
    int have_p = 0;
    uint64_t res_nz = 1u; /* 全零残差标记（仅 P 重建路径消费） */
    if (!want_intra) {
        /* P：残差伪图（scratch ← input − ref）+ 全帧 |cur−ref| 归约 */
        uint64_t sad = 0u;
        gop_residual_into_scratch(ctx, input, &sad, &res_nz);
        /* §3.6 预检（ADR-C049）：残差 MAD ≥ 1<<(bd−6)（满幅 ~1.56%）时
         * P 必然 ≥ 最近 I（实测 P 胜层 MAD ≤5.5，全回退层 ≥32.5，门槛
         * 居间隙、两侧 ≥2× 边际）→ 跳过 P 白编直编 I。判定改变的是
         * 「哪些帧编 I」（编码器策略），不是包格式；直编 I 与回退产出的
         * I 包逐字节相同（同 cfg 同输入同编码器）——golden_v9 F3 /
         * golden_v73 F3 不受影响。归约整数求和与线程数无关。 */
        uint64_t total = 0u;
        for (uint32_t p = 0u; p < ctx->planes; ++p) {
            total += (uint64_t)ctx->vis_w[p] * ctx->vis_h[p];
        }
        const uint64_t mad = sad / total;
        const uint32_t mad_gate = 1u
            << (ctx->cfg.bit_depth > 6u ? ctx->cfg.bit_depth - 6u : 0u);
        if (mad < mad_gate) {
            topos_frame_input rin;
            memset(&rin, 0, sizeof(rin));
            rin.struct_size = (uint32_t)sizeof(rin);
            rin.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
            for (uint32_t p = 0u; p < ctx->planes; ++p) {
                rin.planes[p] = ctx->scratch[p];
                rin.strides[p] = ctx->vis_w[p];
            }
            rc = tc_frame_encode(&cfg_p, &rin, buf_p, cap, &st_p);
            if (rc == TC_OK) {
                have_p = 1;
                p_size = st_p.packet_size;
                /* §3.6 I 回退：P ≥ 最近 I → 内容切换/运动失稳，弃 P 改编 I
                 * （last_i_bytes==0 时参考必不 READY，不会走到这里） */
                if (ctx->last_i_bytes != 0u && p_size >= ctx->last_i_bytes) {
                    have_p = 0;
                }
            } else if (rc != TC_ERR_BUFFER_TOO_SMALL) {
                return rc;
            } else {
                tc_set_error(TC_ERR_LIMIT_EXCEEDED,
                             "v9 P packet exceeds bound (input geometry mismatch)");
                return TC_ERR_LIMIT_EXCEEDED;
            }
        }
    }

    size_t chosen_size = 0u;
    topos_frame_stats* chosen_st = &st_i;
    uint8_t* chosen = NULL;
    int chosen_intra = 1;
    if (!have_p) {
        /* I 候选：源帧直编（首帧/强制/参考失效/P 回退/预检门） */
        rc = gop_enc_buf_get(ctx, 1u, cap, &buf_i);
        if (rc != TC_OK) { return rc; }
        gop_build_cfg(ctx, 1, &cfg_p);
        rc = tc_frame_encode(&cfg_p, input, buf_i, cap, &st_i);
        if (rc != TC_OK) {
            return rc;
        }
        chosen = buf_i;
        chosen_size = st_i.packet_size;
        chosen_st = &st_i;
        chosen_intra = 1;
    } else {
        chosen = buf_p;
        chosen_size = p_size;
        chosen_st = &st_p;
        chosen_intra = 0;
    }

    if (out_cap < chosen_size) {
        /* 事务全量不提交（含参考像素/GOP 计数）：调用方扩缓冲后以同输入
         * 重试，重试逐字节确定——P 残差只读参考，recon 必须晚于本检查，
         * 否则重试会对「解码端从未收到的包」的重建计算残差（参考污染，
         * V9 复审 2026-09-14 修复：out_cap 检查前移到 recon 之前）。 */
        if (need_size != NULL) { *need_size = chosen_size; }
        tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "out_cap %zu < %zu bytes needed",
                     out_cap, chosen_size);
        return TC_ERR_BUFFER_TOO_SMALL;
    }

    if (chosen_intra) {
        /* recon：解码 I 包（编码器重建 = 解码器同源，逐位一致）并 swap
         * 参考槽；v8_frame_decode 要求非 NULL out_info */
        topos_frame_output finfo;
        memset(&finfo, 0, sizeof(finfo));
        finfo.struct_size = (uint32_t)sizeof(finfo);
        finfo.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        uint16_t* const planes[TC_FRAME_MAX_PLANES] = {
            ctx->scratch[0], ctx->scratch[1], ctx->scratch[2], NULL
        };
        rc = gop_decode_pixels(ctx, chosen, chosen_size, planes, &finfo);
        if (rc == TC_OK && finfo.concealed_slices != 0u) {
            /* 防御：自产新包不应 conceal；若发生则参考不安装（§3.8） */
            tc_set_error(TC_ERR_MALFORMED,
                         "gop encode recon: %u concealed tiles",
                         (unsigned)finfo.concealed_slices);
            rc = TC_ERR_MALFORMED;
        }
        if (rc == TC_OK) {
            for (uint32_t p = 0u; p < ctx->planes; ++p) {
                uint16_t* tmp = ctx->ref[p];
                ctx->ref[p] = ctx->scratch[p];
                ctx->scratch[p] = tmp;
            }
        }
    } else if (res_nz == 0u) {
        /* 全零残差精确跳过（LP 实时化 2026-09-14）：X≡0 ⇒ 量化不动点
         * X′≡0 ⇒ recon = clip(ref + 0 − mid) = ref 逐位成立（ref 恒在
         * [0,maxv]）——跳过自解码与重建，参考不推进即重建。零漂由
         * 构造保证（test_v9_codec 重复帧锚 / golden_v9 F2 复核）。 */
    } else {
        /* recon：解码 chosen 包；v8_frame_decode 要求非 NULL out_info */
        topos_frame_output finfo;
        memset(&finfo, 0, sizeof(finfo));
        finfo.struct_size = (uint32_t)sizeof(finfo);
        finfo.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        uint16_t* const planes[TC_FRAME_MAX_PLANES] = {
            ctx->scratch[0], ctx->scratch[1], ctx->scratch[2], NULL
        };
        rc = gop_decode_pixels(ctx, chosen, chosen_size, planes, &finfo);
        if (rc == TC_OK && finfo.concealed_slices != 0u) {
            /* 防御：自产新包不应 conceal；若发生则参考不安装（§3.8） */
            tc_set_error(TC_ERR_MALFORMED,
                         "gop encode recon: %u concealed tiles",
                         (unsigned)finfo.concealed_slices);
            rc = TC_ERR_MALFORMED;
        }
        if (rc == TC_OK) {
            gop_reconstruct_p(ctx, NULL); /* ref 就地推进 */
        }
    }
    if (rc != TC_OK) {
        return rc;
    }

    memcpy(out, chosen, chosen_size);
    if (need_size != NULL) { *need_size = chosen_size; }
    if (stats != NULL) { *stats = *chosen_st; }

    /* 提交事务（编码侧不推进 samples——sample_index 是 observe/feed 的
     * 解码侧观察计数） */
    if (chosen_intra) {
        ctx->next_gop_id = (uint16_t)(ctx->next_gop_id + 1u); /* 回绕 u16 */
        ctx->gop_id = ctx->next_gop_id;
        ctx->last_i_bytes = (uint32_t)chosen_size;
    }
    ctx->ref_state = (uint32_t)TOPOS_GOP_REF_READY;
    ctx->st_ref_state = ctx->ref_state;
    ctx->st_samples = ctx->samples;
    return TC_OK;
}

void tc_gop_context_abort(topos_gop_context* ctx, int reset_to_i)
{
    if (!gop_ctx_valid(ctx)) { return; }
    if (reset_to_i != 0) {
        ctx->ref_state = (uint32_t)TOPOS_GOP_NO_REF;
    }
    ctx->st_ref_state = ctx->ref_state;
    ctx->st_gop_id = ctx->gop_id;
    ctx->st_samples = ctx->samples;
}

int32_t tc_gop_context_state(topos_gop_context* ctx, uint32_t* out_state,
                             uint16_t* out_gop_id)
{
    if (!gop_ctx_valid(ctx)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "gop context invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (out_state != NULL) { *out_state = ctx->ref_state; }
    if (out_gop_id != NULL) { *out_gop_id = ctx->gop_id; }
    return TC_OK;
}
