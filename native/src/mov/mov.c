/* MOV 容器 mux/demux —— 阶段 5（container_spec_v1.md）。
 *
 * 路线：自研最小 writer/reader（ADR-C005），文件形态经 FFmpeg 8.1 oracle 验收。
 * mux：流式 sink（write + seek_write 回填 64 位 mdat 长度），索引累积于内存
 *      （~16 B/sample），moov 在 finish 时构建并追加 —— 标准布局 ftyp+mdat+moov。
 * demux：io.read 回调流式解析（不整读 moov）；采样表全部一致性核对通过后才
 *      分配索引；packet 读取 O(1)；单 sample 越界只失败该 sample（spec §7.6）。
 * faststart：独立后处理 pass，按索引重建（stts RLE / stco→co64 自动升级）。
 * 确定性：同输入 → 逐字节相同输出（时间字段全 0，spec §8）。
 */
#include "common/alloc.h"
#include "mov.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <errno.h>
#include <unistd.h>
#endif

#include "../bitstream/frame_header.h"
#include "../bitstream/packet.h"
#include "../bitstream/layer_directory.h"
#include "../codec/v7_scalable.h"
#include "../common/checked.h"
#include "../common/crc32.h"
#include "../common/endian.h"
#include "../common/error.h"

#define MOV_FTYP_BYTES 20u
#define MOV_MDAT_HDR 16u  /* [size=1][mdat][u64] 共 16B */

static const uint8_t MOV_FTYP[MOV_FTYP_BYTES] = {
    0, 0, 0, 0x14,
    'f', 't', 'y', 'p',            /* type */
    'q', 't', ' ', ' ',            /* major brand */
    0, 0, 2, 0,                    /* minor version */
    'q', 't', ' ', ' '             /* compatible brands */
};

/* —— 字节缓冲（moov 构建）—— */
typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
} mov_buf;

static int32_t buf_reserve(mov_buf* b, size_t extra)
{
    if (extra <= b->cap - b->len) { return TC_OK; }
    size_t need;
    if (!tc_uadd_size(b->len, extra, &need)) { return TC_ERR_LIMIT_EXCEEDED; }
    size_t cap = b->cap ? b->cap : 64u;
    while (cap < need) {
        if (cap > SIZE_MAX / 2u) { cap = need; break; }
        cap *= 2u;
    }
    uint8_t* p = (uint8_t*)tc_realloc(b->data, cap);
    if (p == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "mov buf grow");
        return TC_ERR_OUT_OF_MEMORY;
    }
    b->data = p;
    b->cap = cap;
    return TC_OK;
}

static int32_t buf_be16(mov_buf* b, uint16_t v)
{
    int32_t rc = buf_reserve(b, 2u);
    if (rc != TC_OK) { return rc; }
    tc_store_be16(b->data + b->len, v);
    b->len += 2u;
    return TC_OK;
}

static int32_t buf_be8(mov_buf* b, uint8_t v)
{
    int32_t rc = buf_reserve(b, 1u);
    if (rc != TC_OK) { return rc; }
    b->data[b->len] = v;
    b->len += 1u;
    return TC_OK;
}

static int32_t buf_be32(mov_buf* b, uint32_t v)
{
    int32_t rc = buf_reserve(b, 4u);
    if (rc != TC_OK) { return rc; }
    tc_store_be32(b->data + b->len, v);
    b->len += 4u;
    return TC_OK;
}

static int32_t buf_be64(mov_buf* b, uint64_t v)
{
    int32_t rc = buf_reserve(b, 8u);
    if (rc != TC_OK) { return rc; }
    tc_store_be64(b->data + b->len, v);
    b->len += 8u;
    return TC_OK;
}

static int32_t buf_bytes(mov_buf* b, const void* p, size_t n)
{
    int32_t rc = buf_reserve(b, n);
    if (rc != TC_OK) { return rc; }
    if (n != 0u) { memcpy(b->data + b->len, p, n); }
    b->len += n;
    return TC_OK;
}

static void buf_free(mov_buf* b)
{
    tc_free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/* atom 头占位 + 回填 */
static int32_t buf_atom_open(mov_buf* b, const char type[4], size_t* mark)
{
    *mark = b->len;
    int32_t rc = buf_be32(b, 0u);
    if (rc == TC_OK) { rc = buf_bytes(b, type, 4u); }
    return rc;
}

static void buf_atom_close(const mov_buf* b, size_t mark)
{
    tc_store_be32(b->data + mark, (uint32_t)(b->len - mark));
}

/* 连写 4 字节字段若干次（保留区/矩阵） */
static int32_t buf_be32_n(mov_buf* b, uint32_t v, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        int32_t rc = buf_be32(b, v);
        if (rc != TC_OK) { return rc; }
    }
    return TC_OK;
}

/* ISO 14496-1 描述符长度（expandable：7 bit/字节，最高位=续）。
 * 本库 esds 链长恒 < 128（ASC ≤ 64B），按 ffmpeg movenc 同款 4 字节补位
 * 形式写（80 80 80 XX）——严格解析器（CoreAudio）与宽松扫描器（ffmpeg）
 * 均按规范读到同一长度。≥ 128 走写侧不可达断言（fail-fast）。 */
static int32_t buf_desc_len4(mov_buf* b, uint32_t len)
{
    if (len >= 0x80u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "esds 描述符长度 ≥128（写侧不可达）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    int32_t rc = buf_be8(b, 0x80u);
    if (rc == TC_OK) { rc = buf_be8(b, 0x80u); }
    if (rc == TC_OK) { rc = buf_be8(b, 0x80u); }
    if (rc == TC_OK) { rc = buf_be8(b, (uint8_t)len); }
    return rc;
}

/* 单位矩阵（QuickTime 3×3，9 个 16.16） */
static int32_t buf_identity_matrix(mov_buf* b)
{
    for (int i = 0; i < 9; ++i) {
        int32_t rc = buf_be32(b, (i == 0 || i == 4 || i == 8) ? 0x00010000u : 0u);
        if (rc != TC_OK) { return rc; }
    }
    return TC_OK;
}

/* ============================ mux ============================ */

/* v1.6 M-B8：单条音轨的 mux 侧状态（由 v1.1 的散装 a_* 字段收纳而成；
 * 轨 0 恒为 v1.1 单轨视图——reserved 槽位声明 + 旧 API 语义不变）。 */
#define TOPOS_MAX_AUDIO_TRACKS 16u

typedef struct audio_track_state {
    uint32_t codec, rate, channels, layout, bits;
    uint32_t format;          /* v1.4：TC_AUDIO_FMT_*（float32 仅 lpcm+32） */
    uint32_t frame_bytes;     /* lpcm：每采样帧字节数 = channels × bits/8 */
    uint8_t asc_set;          /* mp4a：AudioSpecificConfig 已声明 */
    uint8_t* asc;
    size_t asc_size;
    uint32_t n;               /* 音频 chunk（容器"样本"）数 */
    uint32_t cap;
    uint64_t* offsets;
    uint32_t* sizes;
    uint32_t* durs;           /* 每 chunk 覆盖的采样帧数（= tick @rate） */
    uint64_t total_frames;
    uint32_t priming;         /* v1.4 mp4a：AAC priming 样本数（elst media_time） */
    char name[64];            /* v1.6：轨名（ trak/udta/©nam + hdlr 名；空 = 不写） */
} audio_track_state;

struct topos_mux {
    topos_movie_config cfg;   /* 规范化后 */
    topos_io sink;
    uint64_t written;         /* 已追加总字节 */
    uint64_t mdat_data_off;   /* 首 sample 绝对偏移 */
    uint64_t expect_pts;      /* 下一帧期望 pts（elst-free 单调约束） */
    uint8_t finished;
    uint8_t poisoned;         /* P1-15：写失败后置位——仅可 free，
                                 add/finish/budget 一律 TC_ERR_STATE */
    uint8_t budget_set;       /* tc_mux_set_alpha_budget 已调用 */
    uint8_t bitstream_major;  /* M9（ADR-C027 D-7）：0=未定（首包定型）；
                                  写入 tpcC 原子 version（1/2 ↔ major 1/2） */
    topos_alpha_budget_info budget;   /* R3：finish 时写 tpcB */
    /* v1.7：电影元数据（tc_mux_set_movie_meta 声明；finish 时写 tpcD） */
    topos_movie_meta meta;
    uint8_t meta_set;
    /* RC1：TRAW 开发元数据（tc_mux_set_raw_meta；finish 时写 trwm） */
    topos_raw_meta rmeta;
    uint8_t rmeta_set;
    uint32_t n;
    uint64_t* offsets;
    uint32_t* sizes;
    uint32_t* durs;
    uint8_t* sync;            /* 逐 sample 同步位（V9：只 I；其余代全 1） */
    uint8_t gop_open;         /* V9 mux 链校验：当前 GOP 是否已开（见过 I） */
    uint16_t cur_gop_id;      /* V9 mux 链校验：当前 GOP id */
    uint32_t cap;
    /* —— v1.1 音频轨（cfg reserved 槽位声明；NONE = 无音轨）——
     * v1.6 数组化（M-B8 两步走第一步）：散装 a_* 字段收纳为轨结构数组，
     * 现阶段 n_a ∈ {0,1}（轨 0 = 全部既有语义；行为逐字节不变）。 */
    audio_track_state a_trk[TOPOS_MAX_AUDIO_TRACKS];
    uint32_t n_a;             /* 已声明音轨数 */
    /* v1.5 tmcd 时间码轨（恒末轨；tc_set 门控） */
    uint8_t tc_set;
    uint32_t tc_hh, tc_mm, tc_ss, tc_ff, tc_fps, tc_df;
};

/* movie 配置 → frame 配置的字段映射（两结构语义一致，几何/颜色/alpha 规则共用） */
static void movie_cfg_to_frame(const topos_movie_config* mc, topos_frame_config* fc)
{
    memset(fc, 0, sizeof(*fc));
    fc->struct_size = (uint32_t)sizeof(*fc);
    fc->abi_version = TOPOS_CODEC_ABI_VERSION;
    fc->visible_width = mc->visible_width;
    fc->visible_height = mc->visible_height;
    fc->profile = mc->profile;
    fc->pixel_format = mc->pixel_format;
    fc->bit_depth = mc->bit_depth;
    /* movie_config 不承载逐帧熵选择。配置预检须用 16-bit 唯一产品
     * 写域的 rans2 锚验证；实际样本在 tc_mux_write_sample 再校验头域。
     * 全零默认会被 16-bit 的 V1/12-bit 冻结域规则误拒于 mux_create。 */
    if (mc->bit_depth == 16u) { fc->reserved[0] = 8u; }
    fc->qmatrix_id = mc->qmatrix_id;
    fc->qp_base = mc->qp_base;
    fc->qp_delta_luma = mc->qp_delta_luma;
    fc->qp_delta_chroma = mc->qp_delta_chroma;
    fc->slice_rows = TC_FRAME_DEFAULT_SLICE_ROWS;
    fc->alpha_mode = mc->alpha_mode;
    fc->alpha_bit_depth = mc->alpha_bit_depth;
    fc->alpha_premultiplied = mc->alpha_premultiplied;
    fc->color_range = mc->color_range;
    fc->color_primaries = mc->color_primaries;
    fc->color_transfer = mc->color_transfer;
    fc->color_matrix = mc->color_matrix;
    fc->chroma_siting = mc->chroma_siting;
    fc->sar_num = mc->sar_num;
    fc->sar_den = mc->sar_den;
}

/* v1.1 音频声明校验（cfg reserved 槽位）。返回 TC_OK 时经 out 输出规范化
 * 五元组 {codec, rate, channels, layout, bits}。NONE 时其余槽位必须全 0
 * （防"半声明"）；LPCM/MP4A 按格式各校各的字段。 */
/* v1.6：字段级校验（NONE 短路由调用方处理）；reserved 槽位与
 * tc_mux_add_audio_track 共用同一规则（fail-fast，无第二套标准）。 */
static int32_t audio_fields_validate(uint32_t codec, uint32_t rate,
                                     uint32_t channels, uint32_t layout,
                                     uint32_t bits, uint32_t fmt)
{
    if (codec != TC_AUDIO_CODEC_LPCM && codec != TC_AUDIO_CODEC_MP4A) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "音频 codec 非法（0=无 1=lpcm 2=mp4a）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* v1.4：全家族采样率（对齐 Apple 交付规范，44.1→192kHz） */
    if (rate != 44100u && rate != 48000u && rate != 88200u &&
        rate != 96000u && rate != 176400u && rate != 192000u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "音频采样率仅支持 44100/48000/88200/96000/176400/192000");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* 声道与布局必须成对声明（v1.6 起增 mono；1.0/2.0/5.1/7.1 四档） */
    if (!((channels == 1u && layout == TC_AUDIO_LAYOUT_MONO) ||
          (channels == 2u && layout == TC_AUDIO_LAYOUT_STEREO) ||
          (channels == 6u && layout == TC_AUDIO_LAYOUT_5_1) ||
          (channels == 8u && layout == TC_AUDIO_LAYOUT_7_1))) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "音频声道/布局不匹配（1↔mono 2↔stereo 6↔5.1 8↔7.1）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* v1.4：样本格式标志——仅 lpcm + 32bit + float 位合法 */
    if (fmt & ~TC_AUDIO_FMT_FLOAT32) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "音频样本格式标志非法（仅 bit0=float32）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (fmt == TC_AUDIO_FMT_FLOAT32 &&
        !(codec == TC_AUDIO_CODEC_LPCM && bits == 32u)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "float32 仅 lpcm+32bit 合法");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (codec == TC_AUDIO_CODEC_LPCM) {
        if (bits != 16u && bits != 24u && bits != 32u) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "lpcm 位深仅支持 16/24/32");
            return TC_ERR_INVALID_ARGUMENT;
        }
    } else if (bits != 0u && bits != 16u) {
        /* AAC 位深由码流自描述；声明槽位仅接受 0（缺省）或 16 */
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "mp4a 不接受位深声明（0 或 16）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    return TC_OK;
}

static int32_t audio_cfg_validate(const topos_movie_config* in, uint32_t out[5])
{
    const uint32_t codec = in->reserved[TC_AUDIO_SLOT_CODEC];
    const uint32_t rate = in->reserved[TC_AUDIO_SLOT_RATE];
    const uint32_t channels = in->reserved[TC_AUDIO_SLOT_CHANNELS];
    const uint32_t layout = in->reserved[TC_AUDIO_SLOT_LAYOUT];
    const uint32_t bits = in->reserved[TC_AUDIO_SLOT_BITS];
    if (codec == TC_AUDIO_CODEC_NONE) {
        if ((rate | channels | layout | bits) != 0u) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT,
                         "音频 codec=NONE 时其余声明槽位必须为 0（防半声明）");
            return TC_ERR_INVALID_ARGUMENT;
        }
        out[0] = out[1] = out[2] = out[3] = out[4] = 0u;
        return TC_OK;
    }
    int32_t rc = audio_fields_validate(codec, rate, channels, layout, bits,
                                       in->reserved[TC_AUDIO_SLOT_FORMAT]);
    if (rc != TC_OK) { return rc; }
    out[0] = codec;
    out[1] = rate;
    out[2] = channels;
    out[3] = layout;
    out[4] = (codec == TC_AUDIO_CODEC_LPCM) ? bits : 0u;
    return TC_OK;
}

static int32_t movie_cfg_normalize(const topos_movie_config* in, topos_movie_config* out)
{
    if (in == NULL || out == NULL ||
        in->struct_size != (uint32_t)sizeof(topos_movie_config) ||
        in->abi_version != TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "movie cfg NULL/镜像不符");
        return TC_ERR_INVALID_ARGUMENT;
    }
    *out = *in;
    /* sar 0/0 = 未指定：原样入 tpcC（读取侧归一 1/1），保证与 packet 逐字段一致 */
    if (out->timescale == 0u) { out->timescale = 24000u; }
    if (out->timescale < 1u || out->timescale > 1000000u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "timescale 超出 [1,1000000]");
        return TC_ERR_INVALID_ARGUMENT;
    }
    uint32_t audio[5];
    int32_t rc = audio_cfg_validate(out, audio);
    if (rc != TC_OK) { return rc; }
    /* 规范化回写（mp4a 的 bits 槽位归一为 0） */
    out->reserved[TC_AUDIO_SLOT_CODEC] = audio[0];
    out->reserved[TC_AUDIO_SLOT_RATE] = audio[1];
    out->reserved[TC_AUDIO_SLOT_CHANNELS] = audio[2];
    out->reserved[TC_AUDIO_SLOT_LAYOUT] = audio[3];
    out->reserved[TC_AUDIO_SLOT_BITS] = audio[4];
    /* v1.4：样本格式标志原样保留（NONE 时校验已保证为 0） */
    out->reserved[TC_AUDIO_SLOT_FORMAT] =
        in->reserved[TC_AUDIO_SLOT_FORMAT];
    topos_frame_config fc;
    movie_cfg_to_frame(out, &fc);
    return tc_frame_config_validate(&fc);
}

/* tpcC 载荷（28B，spec §4）。M9：原子 version = 轨级 bitstream major
 * （1/2 ↔ major 1/2；无包时 1——空轨保持 V1 兼容） */
static void build_tpcc(const topos_movie_config* cfg, uint8_t bitstream_major,
                       uint8_t out[28])
{
    memset(out, 0, 28u);
    memcpy(out, "TPCC", 4u);
    tc_store_be16(out + 4, bitstream_major != 0u ? bitstream_major : 1u);
    out[6] = cfg->profile;
    out[7] = cfg->pixel_format;
    out[8] = cfg->bit_depth;
    out[9] = cfg->qmatrix_id;
    out[10] = cfg->qp_base;
    out[11] = cfg->alpha_mode;
    out[12] = cfg->alpha_bit_depth;
    uint8_t flags = 0u;
    if (cfg->alpha_premultiplied != 0u) { flags |= 1u; }
    if (cfg->color_range != 0u) { flags |= 2u; }
    out[13] = flags;
    out[14] = cfg->color_primaries;
    out[15] = cfg->color_transfer;
    out[16] = cfg->color_matrix;
    out[17] = cfg->chroma_siting;
    tc_store_be16(out + 18, cfg->sar_num);
    tc_store_be16(out + 20, cfg->sar_den);
    /* 22..23 reserved 0 */
    tc_store_be32(out + 24, tc_crc32(out, 24u));
}

/* tpcB 载荷（24B，container_spec v1.1 §4；R3 alpha 预算元数据）。
 * 布局：'TPCB' | ver=1(u16) | mode(u8) | depth(u8) | target_bp(u16) |
 * actual_bp(u16) | max_err(u16) | flags(u16) | frames(u32) | CRC32(0..19)。 */
static void build_tpcb(const topos_movie_config* cfg,
                       const topos_alpha_budget_info* bi, uint8_t out[24])
{
    memset(out, 0, 24u);
    memcpy(out, "TPCB", 4u);
    tc_store_be16(out + 4, 1u);
    out[6] = cfg->alpha_mode;          /* 与 tpcC 冗余——读侧一致性校验 */
    out[7] = cfg->alpha_bit_depth;
    tc_store_be16(out + 8, bi->target_ratio_bp);
    tc_store_be16(out + 10, bi->actual_ratio_bp);
    tc_store_be16(out + 12, bi->max_abs_error);
    tc_store_be16(out + 14, bi->flags);
    tc_store_be32(out + 16, bi->frame_count);
    tc_store_be32(out + 20, tc_crc32(out, 20u));
}

/* tpcD 载荷（container_spec v1.7 §4；v1.7 电影元数据 = 档位 + 厂商标识）。
 * 布局：'TPCD' | ver=1(u16) | tier_id(u8) | reserved(u8=0) |
 * vendor_len(u16) | label_len(u16) | vendor UTF-8 | label UTF-8 |
 * CRC32(前缀)。定长域 12B + 串域 ≤78B + CRC 4B ≤ TPCD_PAYLOAD_MAX。
 * 解析侧按 version=1 严格校验（长度/保留域/CRC/串域长度一致性）。 */
#define TPCD_VENDOR_MAX 15u   /* == sizeof(topos_movie_meta.vendor) - 1 */
#define TPCD_LABEL_MAX  63u   /* == sizeof(topos_movie_meta.label)  - 1 */
#define TPCD_PAYLOAD_MAX (12u + TPCD_VENDOR_MAX + TPCD_LABEL_MAX + 4u)

static size_t build_tpcd(const topos_movie_meta* meta, uint8_t out[TPCD_PAYLOAD_MAX])
{
    const uint8_t vlen = (uint8_t)strlen(meta->vendor);
    const uint8_t llen = (uint8_t)strlen(meta->label);
    memset(out, 0, TPCD_PAYLOAD_MAX);
    memcpy(out, "TPCD", 4u);
    tc_store_be16(out + 4, 1u);
    out[6] = meta->tier_id;
    out[7] = 0u;
    tc_store_be16(out + 8, vlen);
    tc_store_be16(out + 10, llen);
    if (vlen != 0u) { memcpy(out + 12, meta->vendor, vlen); }
    if (llen != 0u) { memcpy(out + 12 + vlen, meta->label, llen); }
    const size_t payload = 12u + (size_t)vlen + (size_t)llen;
    tc_store_be32(out + payload, tc_crc32(out, payload));
    return payload + 4u;
}

static int32_t sink_write(topos_mux* m, const void* p, size_t n)
{
    if (n == 0u) { return TC_OK; }
    int32_t rc = m->sink.write(m->sink.ctx, p, n);
    if (rc != TC_OK) {
        tc_set_error(TC_ERR_IO, "sink write 失败");
        return TC_ERR_IO;
    }
    uint64_t w;
    if (!tc_uadd_u64(m->written, (uint64_t)n, &w)) { return TC_ERR_LIMIT_EXCEEDED; }
    m->written = w;
    return TC_OK;
}

int32_t tc_mux_create(const topos_movie_config* cfg, const topos_io* sink, topos_mux** out)
{
    if (sink == NULL || out == NULL ||
        sink->struct_size != (uint32_t)sizeof(topos_io) ||
        sink->abi_version != TOPOS_CODEC_ABI_VERSION ||
        sink->write == NULL || sink->seek_write == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "mux sink 需要 write+seek_write");
        return TC_ERR_INVALID_ARGUMENT;
    }
    topos_mux* m = (topos_mux*)tc_calloc(1u, sizeof(topos_mux));
    if (m == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "mux alloc");
        return TC_ERR_OUT_OF_MEMORY;
    }
    int32_t rc = movie_cfg_normalize(cfg, &m->cfg);
    if (rc != TC_OK) { tc_free(m); return rc; }
    m->a_trk[0].codec = m->cfg.reserved[TC_AUDIO_SLOT_CODEC];
    m->a_trk[0].rate = m->cfg.reserved[TC_AUDIO_SLOT_RATE];
    m->a_trk[0].channels = m->cfg.reserved[TC_AUDIO_SLOT_CHANNELS];
    m->a_trk[0].layout = m->cfg.reserved[TC_AUDIO_SLOT_LAYOUT];
    m->a_trk[0].bits = m->cfg.reserved[TC_AUDIO_SLOT_BITS];
    m->a_trk[0].format = m->cfg.reserved[TC_AUDIO_SLOT_FORMAT];
    if (m->a_trk[0].codec == TC_AUDIO_CODEC_LPCM) {
        uint64_t fb = (uint64_t)m->a_trk[0].channels * ((uint64_t)m->a_trk[0].bits / 8u);
        if (fb > (uint64_t)UINT32_MAX) { tc_free(m); return TC_ERR_LIMIT_EXCEEDED; }
        m->a_trk[0].frame_bytes = (uint32_t)fb;
    }
    m->n_a = (m->a_trk[0].codec != TC_AUDIO_CODEC_NONE) ? 1u : 0u;
    m->sink = *sink;

    uint8_t mdat_hdr[MOV_MDAT_HDR];
    tc_store_be32(mdat_hdr, 1u);
    memcpy(mdat_hdr + 4, "mdat", 4u);
    tc_store_be64(mdat_hdr + 8, 0u);

    rc = sink_write(m, MOV_FTYP, sizeof(MOV_FTYP));
    if (rc == TC_OK) { rc = sink_write(m, mdat_hdr, sizeof(mdat_hdr)); }
    if (rc != TC_OK) { tc_free(m); return rc; }
    m->mdat_data_off = m->written;
    *out = m;
    return TC_OK;
}

static int32_t mux_grow(topos_mux* m)
{
    if (m->n < m->cap) { return TC_OK; }
    if (m->cap >= TC_MOVIE_MAX_SAMPLES) { return TC_ERR_LIMIT_EXCEEDED; }
    uint32_t cap = (m->cap > TC_MOVIE_MAX_SAMPLES / 2u)
        ? TC_MOVIE_MAX_SAMPLES
        : (m->cap ? m->cap * 2u : 64u);
    uint64_t* off = (uint64_t*)tc_realloc(m->offsets, (size_t)cap * sizeof(uint64_t));
    if (off == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "mux index grow");
        return TC_ERR_OUT_OF_MEMORY;
    }
    m->offsets = off;
    uint32_t* sz = (uint32_t*)tc_realloc(m->sizes, (size_t)cap * sizeof(uint32_t));
    if (sz == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "mux index grow");
        return TC_ERR_OUT_OF_MEMORY;
    }
    m->sizes = sz;
    uint32_t* du = (uint32_t*)tc_realloc(m->durs, (size_t)cap * sizeof(uint32_t));
    if (du == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "mux index grow");
        return TC_ERR_OUT_OF_MEMORY;
    }
    m->durs = du;
    uint8_t* sy = (uint8_t*)tc_realloc(m->sync, (size_t)cap * sizeof(uint8_t));
    if (sy == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "mux index grow");
        return TC_ERR_OUT_OF_MEMORY;
    }
    m->sync = sy;
    m->cap = cap;
    return TC_OK;
}

/* Parse the packet structure used by the MOV sample path.  V1-V6 retain the
 * legacy slice-map parser; V7 has two directory contracts and must never be
 * handed to that parser because TPLD bytes are not slice headers.  V7-B's
 * base-only query validates the outer directory, base descriptor and embedded
 * base packet without reading or decoding the enhancement payload. */
static int32_t movie_parse_packet_structure(const uint8_t* packet, size_t size,
                                            topos_frame_header* fh)
{
    if (packet == NULL || fh == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "movie packet parser argument is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (size >= (size_t)TC_FRAME_HEADER_SIZE &&
        memcmp(packet, TC_FRAME_MAGIC, 4u) == 0 && packet[6] == 7u &&
        size > 45u && packet[45] == 5u) {
        /* V7-A/V7-B 目录包按熵三元组识别（entropy_mode=5）；
         * V7-R rANS（entropy_mode=6，ADR-C034）是常规 legacy packet。
         * V 代际收纳（D1，2026-09-13）：V7-A band 目录随归档退役——非
         * V7-B 的 em5 包（= V7-A 专用语法）在生产构建干净拒绝；V7-B
         * 可伸缩链（TRAW 批 3 底座）不受影响。 */
        if (tc_v7b_packet_is(packet, size)) {
            int32_t rc = tc_v7_packet_header_decode(packet, size, fh);
            if (rc != TC_OK) { return rc; }
            topos_frame_output base_info;
            tc_v7b_decode_stats stats;
            return tc_v7b_frame_decode(packet, size, TC_CODEC_MAX_DIM,
                                       TC_V7B_DECODE_BASE_ONLY, NULL, NULL,
                                       &base_info, &stats);
        }
        if (tc_frame_header_dev_replay_enabled() == 0) {
            tc_set_error(TC_ERR_UNSUPPORTED_VERSION,
                         "generation 7 (V7-A band decode) retired (V "
                         "consolidation 2026-09-13, see ADR-C0xx); archived "
                         "streams replay only with a TOPOS_DEV_REPLAY build "
                         "and TOPOS_DEV=1");
            return TC_ERR_UNSUPPORTED_VERSION;
        }
        {
            int32_t rc = tc_v7_packet_header_decode(packet, size, fh);
            if (rc != TC_OK) { return rc; }
        }
        tc_v7_directory_view directory;
        return tc_v7_directory_parse(packet, size, fh, &directory);
    }

    if (tc_packet_is_v8(packet, size) || tc_packet_is_v9(packet, size)) {
        /* V9（micro-gop 计划批 3）：包布局 V8 同构 → 同一扫描入口
         * （major 域 {8,9}），fh 供 tpcC/major/GOP 链一致性检查。 */
        /* V8：容器只做结构校验（免逐瓦片 CRC——写侧本就无 CRC 拒收语义，
         * 与 legacy 的 parse_structure 同级），fh 供 tpcC/major 一致性检查。 */
        topos_v8_packet_view v8;
        int32_t rc8 = tc_packet_scan_v8_ex(packet, size, &v8, 0);
        if (rc8 == TC_OK) { *fh = v8.fh; }
        return rc8;
    }

    topos_packet_view view;
    int32_t rc = tc_packet_parse_structure(packet, size, &view);
    if (rc == TC_OK) { *fh = view.fh; }
    return rc;
}

int32_t tc_mux_add_packet(topos_mux* m, const uint8_t* packet, size_t size,
                          uint64_t pts_tick, uint32_t dur_tick)
{
    if (m == NULL || m->finished != 0u || m->poisoned != 0u) {
        tc_set_error(TC_ERR_STATE, "mux 未创建/已 finish/已中毒（写失败仅可 free）");
        return TC_ERR_STATE;
    }
    if (packet == NULL || size == 0u || dur_tick == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "packet/dur 非法");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (pts_tick != m->expect_pts) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "pts 必须等于上一帧 pts+dur（无 edit list 布局）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    topos_frame_header packet_fh;
    /* M11-3a（2026-09-02 分段计时定位）：结构校验即可——本函数从不读取
     * slice_crc_ok，tc_packet_scan 的整包 CRC（4K hq ~3.3ms/帧）是纯死功，
     * 且写侧本就无 CRC 拒收语义；scan 内部即 parse_structure + CRC，
     * 结构/tpcC/边界校验逐项一致。写侧 CRC 强校验若将来立项须先 ADR。 */
    int32_t rc = movie_parse_packet_structure(packet, size, &packet_fh);
    if (rc != TC_OK) { return rc; }
    /* M9：轨级 bitstream major 一致性（tpcC 原子 version 绑定）。
     * P1-15：定型延迟到写成功——失败写不得让首个包污染轨级 major。 */
    if (m->bitstream_major != 0u && m->bitstream_major != packet_fh.version_major) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "混合 bitstream major（轨 %u vs 包 %u）",
                     (unsigned)m->bitstream_major,
                     (unsigned)packet_fh.version_major);
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* tpcC 一致性（spec §4；qp 系逐帧可变不校验） */
    const topos_frame_header* fh = &packet_fh;
    const topos_movie_config* c = &m->cfg;
    if (fh->visible_width != c->visible_width || fh->visible_height != c->visible_height ||
        fh->profile != c->profile || fh->pixel_format != c->pixel_format ||
        fh->bit_depth != c->bit_depth || fh->qmatrix_id != c->qmatrix_id ||
        fh->alpha_mode != c->alpha_mode || fh->alpha_bit_depth != c->alpha_bit_depth ||
        ((fh->flags & 1u) != 0u) != (c->alpha_premultiplied != 0u) ||
        fh->color_range != c->color_range || fh->color_primaries != c->color_primaries ||
        fh->color_transfer != c->color_transfer || fh->color_matrix != c->color_matrix ||
        fh->chroma_siting != c->chroma_siting || fh->sar_num != c->sar_num ||
        fh->sar_den != c->sar_den) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "packet header 与电影级配置不一致（tpcC 规则）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* V9（micro-gop 计划批 3）/ V7-R3（ADR-C048 批 3）：GOP 链校验 +
     * 逐 sample 同步位。调用方不得单独传 keyframe flag——帧型从已校验
     * packet 头推导。规则与 GOP context 序列状态机同构（§3.9）：首
     * sample 必须 I；P 须 gop_id 与当前 GOP 一致；I 开新 GOP。
     * V1..V8（含 V7-R/R2）恒全同步。 */
    uint8_t sample_sync = 1u;
    if (fh->version_major == 9u ||
        (fh->version_major == 7u && fh->entropy_mode == 8u)) {
        if (fh->frame_type == 0u) {
            m->gop_open = 1u;
            m->cur_gop_id = fh->gop_id;
        } else {
            if (m->gop_open == 0u) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT,
                             "v9 mux 链：首 sample 必须为 I（got P）");
                return TC_ERR_INVALID_ARGUMENT;
            }
            if (fh->gop_id != m->cur_gop_id) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT,
                             "v9 mux 链：P gop %u 与当前 GOP %u 不一致",
                             (unsigned)fh->gop_id, (unsigned)m->cur_gop_id);
                return TC_ERR_INVALID_ARGUMENT;
            }
            sample_sync = 0u;
        }
    }
    rc = mux_grow(m);
    if (rc != TC_OK) { return rc; }
    /* P1-15：全部候选先在局部完成 checked 算术，sink_write 成功才提交——
     * 失败写不得留下指向未写 sample 的索引/已推进的 expect_pts；
     * 失败后 mux 标 poisoned，仅可 free（禁止伪恢复/finish）。 */
    uint64_t e;
    if (!tc_uadd_u64(m->expect_pts, (uint64_t)dur_tick, &e)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "pts 累加溢出");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    const uint64_t off = m->written;
    rc = sink_write(m, packet, size);
    if (rc != TC_OK) {
        m->poisoned = 1u;
        return rc;
    }
    m->offsets[m->n] = off;
    m->sizes[m->n] = (uint32_t)size; /* parse_structure 已限 ≤ 256 MiB < u32 */
    m->durs[m->n] = dur_tick;
    m->sync[m->n] = sample_sync;
    m->n++;
    m->expect_pts = e;
    if (m->bitstream_major == 0u) { m->bitstream_major = packet_fh.version_major; }
    return TC_OK;
}

int32_t tc_mux_set_alpha_budget(topos_mux* m, const topos_alpha_budget_info* info)
{
    if (m == NULL || m->finished != 0u || m->poisoned != 0u) {
        tc_set_error(TC_ERR_STATE, "mux 未创建/已 finish/已中毒（写失败仅可 free）");
        return TC_ERR_STATE;
    }
    if (info == NULL ||
        info->struct_size != (uint32_t)sizeof(topos_alpha_budget_info) ||
        info->abi_version != TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "budget info NULL/镜像不符");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (m->cfg.alpha_mode == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "无 Alpha 平面的电影不携带预算元数据");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (info->actual_ratio_bp > 65535u ||
        (info->target_ratio_bp > 10000u &&
         info->target_ratio_bp != TC_ALPHA_BUDGET_RATIO_UNSET)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "预算比例超出 u16 表示域（actual ≤ 65535bp / target ≤ 10000bp）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    m->budget = *info;
    m->budget_set = 1u;
    return TC_OK;
}

int32_t tc_mux_set_movie_meta(topos_mux* m, const topos_movie_meta* meta)
{
    if (m == NULL || m->finished != 0u || m->poisoned != 0u) {
        tc_set_error(TC_ERR_STATE, "mux 未创建/已 finish/已中毒（写失败仅可 free）");
        return TC_ERR_STATE;
    }
    if (meta == NULL ||
        meta->struct_size != (uint32_t)sizeof(topos_movie_meta) ||
        meta->abi_version != TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "movie meta NULL/镜像不符");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (meta->tier_id > TC_TIER_RAW) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "tier_id 越界（TC_TIER_* 为 0..8）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (meta->reserved[0] != 0u || meta->reserved[1] != 0u ||
        meta->reserved[2] != 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "movie meta reserved 必须 0");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (strlen(meta->vendor) >= sizeof(meta->vendor)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "vendor 超 15 字节（UTF-8）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (strlen(meta->label) >= sizeof(meta->label)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "label 超 63 字节（UTF-8）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    m->meta = *meta;
    m->meta_set = 1u;
    return TC_OK;
}

/* RC1：记录 TRAW 开发元数据（finish 前调用；可选；重设以最后为准）。
 * 载荷不透明原样承载（结构/CRC 校验归宿主，见 topos_trwm.py）。 */
int32_t tc_mux_set_raw_meta(topos_mux* m, const topos_raw_meta* meta)
{
    if (m == NULL || meta == NULL ||
        meta->struct_size != (uint32_t)sizeof(topos_raw_meta) ||
        meta->abi_version != TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "set_raw_meta: NULL/镜像不符");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (meta->reserved != 0u || meta->payload_size == 0u
            || meta->payload_size > TC_RAW_META_MAX) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "set_raw_meta: payload 域非法");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (m->finished != 0u || m->poisoned != 0u) {
        tc_set_error(TC_ERR_STATE, "set_raw_meta: mux 已 finish/损坏");
        return TC_ERR_STATE;
    }
    m->rmeta = *meta;
    m->rmeta_set = 1u;
    return TC_OK;
}

/* 轨索引守卫（v1.6）：track < 已声明轨数 */
static int32_t mux_audio_track(topos_mux* m, uint32_t track,
                               audio_track_state** out)
{
    if (track >= m->n_a) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "音轨索引 %u 越界（已声明 %u 轨）", track, m->n_a);
        return TC_ERR_INVALID_ARGUMENT;
    }
    *out = &m->a_trk[track];
    return TC_OK;
}

/* 音频 chunk 索引扩容（与 mux_grow 同风格；上限复用 TC_MOVIE_MAX_SAMPLES） */
static int32_t mux_grow_audio(audio_track_state* t)
{
    if (t->n < t->cap) { return TC_OK; }
    if (t->cap >= TC_MOVIE_MAX_SAMPLES) { return TC_ERR_LIMIT_EXCEEDED; }
    uint32_t cap = (t->cap > TC_MOVIE_MAX_SAMPLES / 2u)
        ? TC_MOVIE_MAX_SAMPLES
        : (t->cap ? t->cap * 2u : 64u);
    uint64_t* off = (uint64_t*)tc_realloc(t->offsets, (size_t)cap * sizeof(uint64_t));
    if (off == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "mux audio index grow");
        return TC_ERR_OUT_OF_MEMORY;
    }
    t->offsets = off;
    uint32_t* sz = (uint32_t*)tc_realloc(t->sizes, (size_t)cap * sizeof(uint32_t));
    if (sz == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "mux audio index grow");
        return TC_ERR_OUT_OF_MEMORY;
    }
    t->sizes = sz;
    uint32_t* du = (uint32_t*)tc_realloc(t->durs, (size_t)cap * sizeof(uint32_t));
    if (du == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "mux audio index grow");
        return TC_ERR_OUT_OF_MEMORY;
    }
    t->durs = du;
    t->cap = cap;
    return TC_OK;
}

int32_t tc_mux_set_audio_asc(topos_mux* m, const uint8_t* asc, size_t size)
{
    return tc_mux_set_audio_track_asc(m, 0u, asc, size);
}

int32_t tc_mux_set_audio_track_asc(topos_mux* m, uint32_t track,
                                   const uint8_t* asc, size_t size)
{
    if (m == NULL || m->finished != 0u || m->poisoned != 0u) {
        tc_set_error(TC_ERR_STATE, "mux 未创建/已 finish/已中毒（写失败仅可 free）");
        return TC_ERR_STATE;
    }
    audio_track_state* t = NULL;
    int32_t rc = mux_audio_track(m, track, &t);
    if (rc != TC_OK) { return rc; }
    if (t->codec != TC_AUDIO_CODEC_MP4A) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "仅 mp4a 档接受 AudioSpecificConfig");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (asc == NULL || size == 0u || size > 64u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "AudioSpecificConfig 为空或超 64 字节");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (t->asc_set != 0u) {
        tc_set_error(TC_ERR_STATE, "AudioSpecificConfig 已声明（仅一次）");
        return TC_ERR_STATE;
    }
    uint8_t* copy = (uint8_t*)tc_alloc(size);
    if (copy == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "asc copy");
        return TC_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, asc, size);
    t->asc = copy;
    t->asc_size = size;
    t->asc_set = 1u;
    return TC_OK;
}

int32_t tc_mux_set_audio_priming(topos_mux* m, uint32_t samples)
{
    return tc_mux_set_audio_track_priming(m, 0u, samples);
}

/* v1.4：AAC priming 声明（音频 trak edts/elst media_time）。语义见头文件。 */
int32_t tc_mux_set_audio_track_priming(topos_mux* m, uint32_t track,
                                       uint32_t samples)
{
    if (m == NULL || m->finished != 0u || m->poisoned != 0u) {
        tc_set_error(TC_ERR_STATE, "mux 未创建/已 finish/已中毒（写失败仅可 free）");
        return TC_ERR_STATE;
    }
    audio_track_state* t = NULL;
    int32_t rc = mux_audio_track(m, track, &t);
    if (rc != TC_OK) { return rc; }
    if (t->codec != TC_AUDIO_CODEC_MP4A) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "priming 仅 mp4a 档合法（lpcm 采样精确恒零、NONE 无音轨）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (samples == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "priming 须 > 0（0 = 不写 elst，不声明即可）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if ((uint64_t)samples >= t->total_frames) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "priming %u 须 < 已写入音频总采样数 %llu",
                     samples, (unsigned long long)t->total_frames);
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (t->priming != 0u) {
        tc_set_error(TC_ERR_STATE, "priming 已声明（至多一次）");
        return TC_ERR_STATE;
    }
    t->priming = samples;
    return TC_OK;
}

/* v1.5：tmcd 时间码轨声明（语义见头文件）。DF 计数换算：
 * count = (hh*3600+mm*60+ss)*fps + ff - drop×(总分钟 - 总分钟/10)（DF 时）；
 * drop 每分钟跳帧数 = 4（60DF，SMPTE 12M：;00-;03 无效）/ 2（30DF），
 * 与 ffmpeg oracle 实测一致（'00:01:00;04'@60DF → 计数 3600）。 */
static uint32_t tmcd_df_per_minute(uint32_t fps)
{
    return (fps == 60u) ? 4u : 2u;
}

int32_t tc_mux_set_timecode(topos_mux* m, uint32_t hh, uint32_t mm,
                            uint32_t ss, uint32_t ff, uint32_t fps,
                            uint32_t drop_frame)
{
    if (m == NULL || m->finished != 0u || m->poisoned != 0u) {
        tc_set_error(TC_ERR_STATE, "mux 未创建/已 finish/已中毒（写失败仅可 free）");
        return TC_ERR_STATE;
    }
    if (m->tc_set != 0u) {
        tc_set_error(TC_ERR_STATE, "timecode 已声明（至多一次）");
        return TC_ERR_STATE;
    }
    if (fps != 24u && fps != 25u && fps != 30u && fps != 48u &&
        fps != 50u && fps != 60u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "timecode fps 白名单 {24,25,30,48,50,60}"
                     "（NTSC 分数帧率以标称值 + drop_frame 表达）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (drop_frame > 1u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "drop_frame ∈ {0,1}");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* DF 仅标称 30/60 合法（30/1.001、60/1.001 的跳帧语义） */
    if (drop_frame != 0u && fps != 30u && fps != 60u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "drop_frame 仅标称 30/60 fps 合法");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (hh >= 24u || mm >= 60u || ss >= 60u || ff >= fps) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "timecode 分量越界（hh<24 mm/ss<60 ff<fps）");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* DF 分钟首帧标签有效性（与 media 层 Timecode 同规）：
     * 非 10 分钟整的分钟 ss==0 时，30DF 拒 ff<2、60DF 拒 ff<4（跳帧号） */
    if (drop_frame != 0u && ss == 0u && mm % 10u != 0u) {
        const uint32_t drop = tmcd_df_per_minute(fps);
        if (ff < drop) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT,
                         "DF 分钟首帧 %02u 起有效（%uDF 跳 ff<%u）",
                         drop, fps, drop);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    /* 负时码本期不支持（flags bit2 恒 0）；无视频轨的容器形态不存在
     * （视频轨恒为轨 1），tmcd 时长依附视频轨时长成立。 */
    m->tc_hh = hh;
    m->tc_mm = mm;
    m->tc_ss = ss;
    m->tc_ff = ff;
    m->tc_fps = fps;
    m->tc_df = drop_frame;
    m->tc_set = 1u;
    return TC_OK;
}

int32_t tc_mux_add_audio(topos_mux* m, const uint8_t* data, size_t size,
                         uint32_t num_samples)
{
    /* v1.1 语义保留：未声明音轨 = STATE（非索引越界 INVALID） */
    if (m == NULL || m->finished != 0u || m->poisoned != 0u) {
        tc_set_error(TC_ERR_STATE, "mux 未创建/已 finish/已中毒（写失败仅可 free）");
        return TC_ERR_STATE;
    }
    if (m->n_a == 0u) {
        tc_set_error(TC_ERR_STATE, "movie config 未声明音频轨（audio codec = NONE）");
        return TC_ERR_STATE;
    }
    return tc_mux_add_audio_to(m, 0u, data, size, num_samples);
}

int32_t tc_mux_add_audio_to(topos_mux* m, uint32_t track,
                            const uint8_t* data, size_t size,
                            uint32_t num_samples)
{
    if (m == NULL || m->finished != 0u || m->poisoned != 0u) {
        tc_set_error(TC_ERR_STATE, "mux 未创建/已 finish/已中毒（写失败仅可 free）");
        return TC_ERR_STATE;
    }
    audio_track_state* t = NULL;
    int32_t rc = mux_audio_track(m, track, &t);
    if (rc != TC_OK) { return rc; }
    if (t->codec == TC_AUDIO_CODEC_NONE) {
        tc_set_error(TC_ERR_STATE, "音轨 %u 未声明格式（先 add_audio_track 或 "
                                  "movie_config reserved 槽位）", track);
        return TC_ERR_STATE;
    }
    if (data == NULL || size == 0u || num_samples == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "audio data/num_samples 非法");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (size > (size_t)UINT32_MAX) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "audio chunk 超 u32 表示域");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    if (t->codec == TC_AUDIO_CODEC_LPCM) {
        const uint64_t expect = (uint64_t)num_samples * (uint64_t)t->frame_bytes;
        if ((uint64_t)size != expect) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT,
                         "lpcm chunk 字节数 %zu ≠ num_samples×frame_bytes=%llu",
                         size, (unsigned long long)expect);
            return TC_ERR_INVALID_ARGUMENT;
        }
    } else if (t->asc_set == 0u) {
        tc_set_error(TC_ERR_STATE, "mp4a 需先声明 AudioSpecificConfig");
        return TC_ERR_STATE;
    }
    if (t->n >= TC_MOVIE_MAX_SAMPLES) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "audio chunk 数超上限");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    rc = mux_grow_audio(t);
    if (rc != TC_OK) { return rc; }
    /* P1-15 同款纪律：sink_write 成功才提交索引；失败置 poisoned 仅可 free */
    const uint64_t off = m->written;
    rc = sink_write(m, data, size);
    if (rc != TC_OK) {
        m->poisoned = 1u;
        return rc;
    }
    t->offsets[t->n] = off;
    t->sizes[t->n] = (uint32_t)size;
    t->durs[t->n] = num_samples;
    t->n++;
    uint64_t total;
    if (!tc_uadd_u64(t->total_frames, (uint64_t)num_samples, &total)) {
        return TC_ERR_LIMIT_EXCEEDED;
    }
    t->total_frames = total;
    return TC_OK;
}

/* v1.6：逐轨声明新音轨（M-B8）。格式规则与 reserved 槽位共用同一校验
 * （audio_fields_validate；NONE 不经此路径）。轨名 ≤63 字节 UTF-8。 */
int32_t tc_mux_add_audio_track(topos_mux* m, const topos_audio_track_config* cfg)
{
    if (m == NULL || m->finished != 0u || m->poisoned != 0u) {
        tc_set_error(TC_ERR_STATE, "mux 未创建/已 finish/已中毒（写失败仅可 free）");
        return TC_ERR_STATE;
    }
    if (cfg == NULL || cfg->struct_size != (uint32_t)sizeof(topos_audio_track_config) ||
        cfg->abi_version != TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "audio track cfg NULL/镜像不符");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (cfg->codec == TC_AUDIO_CODEC_NONE) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "add_audio_track 不接受 codec=NONE");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (cfg->reserved[0] | cfg->reserved[1] | cfg->reserved[2] | cfg->reserved[3]) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "audio track cfg reserved 必须 0");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (m->n_a >= TOPOS_MAX_AUDIO_TRACKS) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED,
                     "音轨数超上限（%u）", TOPOS_MAX_AUDIO_TRACKS);
        return TC_ERR_LIMIT_EXCEEDED;
    }
    int32_t rc = audio_fields_validate(cfg->codec, cfg->sample_rate,
                                       cfg->channel_count, cfg->channel_layout,
                                       cfg->bits_per_sample, cfg->sample_format);
    if (rc != TC_OK) { return rc; }
    audio_track_state* t = &m->a_trk[m->n_a];
    memset(t, 0, sizeof(*t));
    t->codec = cfg->codec;
    t->rate = cfg->sample_rate;
    t->channels = cfg->channel_count;
    t->layout = cfg->channel_layout;
    t->bits = (cfg->codec == TC_AUDIO_CODEC_LPCM) ? cfg->bits_per_sample : 0u;
    t->format = cfg->sample_format;
    if (t->codec == TC_AUDIO_CODEC_LPCM) {
        uint64_t fb = (uint64_t)t->channels * ((uint64_t)t->bits / 8u);
        if (fb > (uint64_t)UINT32_MAX) { return TC_ERR_LIMIT_EXCEEDED; }
        t->frame_bytes = (uint32_t)fb;
    }
    if (cfg->name != NULL && cfg->name[0] != '\0') {
        const size_t len = strlen(cfg->name);
        if (len >= sizeof(t->name)) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT,
                         "轨名超 63 字节（UTF-8）");
            return TC_ERR_INVALID_ARGUMENT;
        }
        memcpy(t->name, cfg->name, len + 1u);
    }
    m->n_a++;
    return TC_OK;
}

/* stsd 的 TPIC entry（colr/pasp/tpcC/tpcB/tpcD 子 atom，spec §4 顺序冻结；
 * tpcB（v1.1）/tpcD（v1.7）为尾部追加可选 atom——旧 reader 按未知子
 * atom 跳过） */
static int32_t build_stsd(const topos_movie_config* cfg, uint8_t bitstream_major,
                          const topos_alpha_budget_info* budget,
                          const topos_movie_meta* meta,
                          const topos_raw_meta* rmeta, mov_buf* b)
{
    size_t a;
    int32_t rc = buf_atom_open(b, "stsd", &a);
    if (rc != TC_OK) { return rc; }
    rc = buf_be32(b, 0u);                       /* version/flags */
    if (rc == TC_OK) { rc = buf_be32(b, 1u); }  /* entry_count */
    size_t ent;
    if (rc == TC_OK) { rc = buf_atom_open(b, "TPIC", &ent); }
    static const uint8_t ZERO6[6] = { 0 };
    if (rc == TC_OK) { rc = buf_bytes(b, ZERO6, 6u); }   /* reserved */
    if (rc == TC_OK) { rc = buf_be16(b, 1u); }           /* data_reference_index */
    if (rc == TC_OK) { rc = buf_be16(b, 0u); }           /* pre_defined */
    if (rc == TC_OK) { rc = buf_be16(b, 0u); }           /* reserved */
    if (rc == TC_OK) { rc = buf_be32_n(b, 0u, 3u); }     /* pre_defined ×3 */
    if (rc == TC_OK) { rc = buf_be16(b, cfg->visible_width); }
    if (rc == TC_OK) { rc = buf_be16(b, cfg->visible_height); }
    if (rc == TC_OK) { rc = buf_be32(b, 0x00480000u); }  /* hres 72dpi */
    if (rc == TC_OK) { rc = buf_be32(b, 0x00480000u); }  /* vres */
    if (rc == TC_OK) { rc = buf_be32(b, 0u); }           /* reserved */
    if (rc == TC_OK) { rc = buf_be16(b, 1u); }           /* frame_count */
    if (rc == TC_OK) {                                  /* compressorname（Pascal 串，32B 填满） */
        static const char NAME[] = "Topos Video Codec 2.0";
        uint8_t cn[32];
        memset(cn, 0, sizeof(cn));
        cn[0] = (uint8_t)(sizeof(NAME) - 1u);
        memcpy(cn + 1, NAME, sizeof(NAME) - 1u);
        rc = buf_bytes(b, cn, 32u);
    }
    if (rc == TC_OK) { rc = buf_be16(b, 0x0018u); }      /* depth */
    if (rc == TC_OK) { rc = buf_be16(b, 0xFFFFu); }      /* pre_defined */
    if (rc != TC_OK) { return rc; }

    /* colr（nclc） */
    {
        size_t t;
        rc = buf_atom_open(b, "colr", &t);
        if (rc == TC_OK) { rc = buf_bytes(b, "nclc", 4u); }
        if (rc == TC_OK) { rc = buf_be16(b, cfg->color_primaries); }
        if (rc == TC_OK) { rc = buf_be16(b, cfg->color_transfer); }
        if (rc == TC_OK) { rc = buf_be16(b, cfg->color_matrix); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* pasp（sar ≠ 1/1 时才写，spec §4） */
    if (cfg->sar_num != 1u || cfg->sar_den != 1u) {
        size_t t;
        rc = buf_atom_open(b, "pasp", &t);
        if (rc == TC_OK) { rc = buf_be32(b, cfg->sar_num); }
        if (rc == TC_OK) { rc = buf_be32(b, cfg->sar_den); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* tpcC */
    {
        uint8_t tpcc[28];
        build_tpcc(cfg, bitstream_major, tpcc);
        size_t t;
        rc = buf_atom_open(b, "tpcC", &t);
        if (rc == TC_OK) { rc = buf_bytes(b, tpcc, sizeof(tpcc)); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* tpcB（R3：仅 set_alpha_budget 后写入） */
    if (budget != NULL) {
        uint8_t tpcb[24];
        build_tpcb(cfg, budget, tpcb);
        size_t t;
        rc = buf_atom_open(b, "tpcB", &t);
        if (rc == TC_OK) { rc = buf_bytes(b, tpcb, sizeof(tpcb)); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* tpcD（v1.7：仅 set_movie_meta 后写入） */
    if (meta != NULL) {
        uint8_t tpcd[TPCD_PAYLOAD_MAX];
        const size_t tpcd_len = build_tpcd(meta, tpcd);
        size_t t;
        rc = buf_atom_open(b, "tpcD", &t);
        if (rc == TC_OK) { rc = buf_bytes(b, tpcd, tpcd_len); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* trwm（RC1：仅 set_raw_meta 后写入；载荷不透明原样承载） */
    if (rmeta != NULL) {
        size_t t;
        rc = buf_atom_open(b, "trwm", &t);
        if (rc == TC_OK) { rc = buf_bytes(b, rmeta->payload, rmeta->payload_size); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    buf_atom_close(b, ent);
    buf_atom_close(b, a);
    return TC_OK;
}

/* moov 构建共用的音频轨元数据（mux finish 与 faststart 共用；NULL = 无音轨） */
typedef struct audio_moov_meta {
    uint32_t codec, rate, channels, layout, bits;
    uint32_t track_id;       /* v1.6：轨 id（video=1、audio=2..N+1） */
    const char* name;        /* v1.6：轨名（NULL = 不写 ©nam；hdlr 名回退默认） */
    uint32_t format;         /* v1.4：TC_AUDIO_FMT_*（lpcm float32 → 0xB） */
    uint32_t priming;        /* v1.4 mp4a：elst media_time（0 = 不写 edts） */
    uint32_t n;              /* chunk 数（0 = 声明轨但无样本——finish 侧已拒绝，
                                 faststart 侧理论上不可达） */
    uint64_t total_frames;   /* 采样帧总数（tkhd/mdhd duration 的 tick 数） */
    const uint64_t* offsets;
    const uint32_t* sizes;
    const uint32_t* durs;    /* 每 chunk tick 数 = 采样帧数 */
    const uint8_t* asc;      /* mp4a AudioSpecificConfig（esds 原样存） */
    size_t asc_size;
} audio_moov_meta;

/* stts 等 dur RLE（视频/音频轨共用；单遍规范写法） */
static int32_t write_stts_rle(mov_buf* b, uint32_t n, const uint32_t* durs)
{
    size_t t;
    int32_t rc = buf_atom_open(b, "stts", &t);
    if (rc == TC_OK) { rc = buf_be32(b, 0u); }
    uint32_t runs = 0u;
    for (uint32_t i = 0; i < n; ++i) {
        if (i == 0u || durs[i] != durs[i - 1u]) { runs++; }
    }
    if (rc == TC_OK) { rc = buf_be32(b, runs); }
    uint32_t i = 0u;
    while (i < n && rc == TC_OK) {
        uint32_t j = i;
        while (j < n && durs[j] == durs[i]) { j++; }
        rc = buf_be32(b, j - i);
        if (rc == TC_OK) { rc = buf_be32(b, durs[i]); }
        i = j;
    }
    if (rc != TC_OK) { return rc; }
    buf_atom_close(b, t);
    return TC_OK;
}

/* stsc 单条目（1 sample/chunk；视频/音频轨共用；n==0 时不写） */
static int32_t write_stsc_single(mov_buf* b)
{
    size_t t;
    int32_t rc = buf_atom_open(b, "stsc", &t);
    if (rc == TC_OK) { rc = buf_be32(b, 0u); }
    if (rc == TC_OK) { rc = buf_be32(b, 1u); }
    if (rc == TC_OK) { rc = buf_be32(b, 1u); }   /* first_chunk */
    if (rc == TC_OK) { rc = buf_be32(b, 1u); }   /* samples_per_chunk */
    if (rc == TC_OK) { rc = buf_be32(b, 1u); }   /* sample_description_id */
    if (rc != TC_OK) { return rc; }
    buf_atom_close(b, t);
    return TC_OK;
}

/* 音频 stsd（container_spec v1.1 §a）。
 * lpcm 档：SoundDescription（version 0）+ 20B lpcm 扩展（Apple TN2120；
 * formatFlags 用 CoreAudio 常量：SignedInteger|BigEndian|Packed = 'twos' 语义）
 * + chan 布局子原子。
 * mp4a 档：SoundDescriptionV1 + esds（ISO 14496-1 描述符：tag 1B + 可变长
 * 7-bit 续位长度，四描述符同级链 ES→DCD→DSI→SL；AudioSpecificConfig 由
 * 应用层经 tc_mux_set_audio_asc 传入原样封装）。长度编码与嵌套必须规范——
 * CoreAudio/QuickTime 严格按长度解析（ffmpeg/VLC 顺序扫描可容错；M-B0
 * 互操作矩阵实测：非规范写法下 CoreAudio 报 0 packets/拒开）。 */
static int32_t build_audio_stsd(const audio_moov_meta* a, mov_buf* b)
{
    size_t s;
    int32_t rc = buf_atom_open(b, "stsd", &s);
    if (rc == TC_OK) { rc = buf_be32(b, 0u); }           /* version/flags */
    if (rc == TC_OK) { rc = buf_be32(b, 1u); }           /* entry_count */
    size_t ent;
    if (rc == TC_OK) {
        rc = buf_atom_open(b, a->codec == TC_AUDIO_CODEC_LPCM ? "lpcm" : "mp4a", &ent);
    }
    /* v1.4：sampleRate 字段为 16.16 定点（上限 65535.99Hz）——88.2/176.4/192k
     * 溢出，>65535 按 QuickTime SoundDescriptionV2 写（sampleRate=float64，
     * 与 ffmpeg movenc 同构）；V2 时 TN2120 lpcm 字段并入 entry 主体。
     * float32 亦走 V2——ffmpeg/VLC 对 V0/V1 lpcm 不读 formatFlags float 位
     * （实测 48k f32 直挂被识别为 pcm_s32be），V2 的 formatSpecificFlags
     * 才是 float 的可靠声明位 */
    const int stsd_v2 = a->rate > 65535u
                        || (a->codec == TC_AUDIO_CODEC_LPCM
                            && a->format == TC_AUDIO_FMT_FLOAT32);
    static const uint8_t ZERO6[6] = { 0 };
    if (rc == TC_OK) { rc = buf_bytes(b, ZERO6, 6u); }   /* reserved */
    if (rc == TC_OK) { rc = buf_be16(b, 1u); }           /* data_reference_index */
    if (rc == TC_OK) {
        rc = buf_be16(b, stsd_v2 ? 2u
                            : (a->codec == TC_AUDIO_CODEC_LPCM ? 0u : 1u));
    }                                                    /* version */
    if (rc == TC_OK) { rc = buf_be16(b, 0u); }           /* revision */
    if (rc == TC_OK) { rc = buf_be32(b, 0u); }           /* vendor */
    if (stsd_v2) {
        /* SoundDescriptionV2 主体（Apple TN/movenc 同构） */
        if (rc == TC_OK) { rc = buf_be16(b, 3u); }       /* always3 */
        if (rc == TC_OK) { rc = buf_be16(b, 16u); }      /* always16 */
        if (rc == TC_OK) { rc = buf_be16(b, 0xFFFEu); }  /* alwaysMinus2 */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }       /* always0 */
        if (rc == TC_OK) { rc = buf_be32(b, 0x00010000u); } /* soundRate 16.16 = 1.0 */
        if (rc == TC_OK) { rc = buf_be32(b, 20u); }      /* sizeOfStructOnly */
        if (rc == TC_OK) {
            const double sr = (double)a->rate;
            uint64_t bits64;
            memcpy(&bits64, &sr, sizeof(bits64));
            rc = buf_be64(b, bits64);                    /* sampleRate float64 BE */
        }
        if (rc == TC_OK) { rc = buf_be32(b, a->channels); }  /* numChannels */
        if (rc == TC_OK) { rc = buf_be32(b, 0x7F000000u); }  /* always7F000000 */
        if (rc == TC_OK) {                       /* constBitsPerChannel */
            rc = buf_be32(b, a->codec == TC_AUDIO_CODEC_LPCM ? a->bits : 16u);
        }
        if (rc == TC_OK) {                       /* formatSpecificFlags */
            rc = buf_be32(b,
                a->codec == TC_AUDIO_CODEC_LPCM
                    ? (a->format == TC_AUDIO_FMT_FLOAT32 ? 0x0000000Bu
                                                         : 0x0000000Eu)
                    : 0u);
        }
        if (rc == TC_OK) {                       /* constBytesPerAudioPacket */
            rc = buf_be32(b, a->codec == TC_AUDIO_CODEC_LPCM
                                 ? a->channels * (a->bits / 8u) : 0u);
        }
        if (rc == TC_OK) {                       /* constLPCMFramesPerAudioPacket */
            rc = buf_be32(b, a->codec == TC_AUDIO_CODEC_LPCM ? 1u : 0u);
        }
        if (rc != TC_OK) { return rc; }
    } else {
        if (rc == TC_OK) { rc = buf_be16(b, (uint16_t)a->channels); }  /* numChannels */
        if (rc == TC_OK) {                               /* sampleSize */
            rc = buf_be16(b,
                (uint16_t)(a->codec == TC_AUDIO_CODEC_LPCM ? a->bits : 16u));
        }
        if (rc == TC_OK) {
            rc = buf_be16(b,
                a->codec == TC_AUDIO_CODEC_LPCM ? 0u : (uint16_t)0xFFFEu);
        }                                                /* compressionID（mp4a V1 惯例 -2） */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }       /* packetSize */
        if (rc == TC_OK) { rc = buf_be32(b, a->rate << 16); }/* sampleRate（16.16） */
    }
    if (rc != TC_OK) { return rc; }

    if (a->codec == TC_AUDIO_CODEC_LPCM) {
        if (!stsd_v2) {
            const uint32_t frame_bytes = a->channels * (a->bits / 8u);
            /* v1.4：整数 = 0xE（signed|BE|packed，'twos' 语义）；
             * float32 = 0xB（float|BE|packed，kAudioFormatFlagIsFloat=0x1，
             * ffprobe 识别为 pcm_f32be） */
            rc = buf_be32(b, a->format == TC_AUDIO_FMT_FLOAT32 ? 0x0000000Bu
                                                               : 0x0000000Eu);
            if (rc == TC_OK) { rc = buf_be32(b, a->bits); }  /* constBitsPerChannel */
            if (rc == TC_OK) { rc = buf_be32(b, 0u); }       /* formatSpecificFlags */
            if (rc == TC_OK) { rc = buf_be32(b, frame_bytes); }
            if (rc == TC_OK) { rc = buf_be32(b, 1u); }   /* constLPCMFramesPerAudioPacket */
            if (rc != TC_OK) { return rc; }
        }
        /* chan（声道布局声明，sample entry 子原子；V2 亦写） */
        const uint32_t tag = (a->channels == 1u) ? TC_AUDIO_CHAN_TAG_MONO
                           : (a->channels == 2u) ? TC_AUDIO_CHAN_TAG_STEREO
                           : (a->channels == 6u) ? TC_AUDIO_CHAN_TAG_5_1_A
                                                 : TC_AUDIO_CHAN_TAG_7_1_A;
        size_t c;
        rc = buf_atom_open(b, "chan", &c);
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }       /* version/flags */
        if (rc == TC_OK) { rc = buf_be32(b, tag); }      /* channelLayoutTag */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }       /* channelBitmap */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }       /* numberChannelDescriptions */
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, c);
    } else {
        /* SoundDescriptionV1 扩展（mp4a 惯例；V2 时字段并入主体不重复写） */
        if (!stsd_v2) {
            if (rc == TC_OK) { rc = buf_be32(b, 1024u); }    /* samplesPerPacket */
            if (rc == TC_OK) { rc = buf_be32(b, 0u); }       /* bytesPerPacket */
            if (rc == TC_OK) { rc = buf_be32(b, 0u); }       /* bytesPerFrame */
            if (rc == TC_OK) { rc = buf_be32(b, 2u); }       /* bytesPerSample */
            if (rc != TC_OK) { return rc; }
        }
        /* QuickTime 声样扩展 'wave' 包裹（M-B0 实测：AVFoundation/QuickTime
         * 电影引擎要求 frma+esds+终结子的规范形态，直挂 esds 拒开文件；
         * 与 ffmpeg movenc 的 mov 产物逐字节同构）：
         *   wave { frma('mp4a'); 'mp4a'12B 空原子; esds; 8B null 终结子 } */
        const uint32_t dsi_len = (uint32_t)a->asc_size;
        const uint32_t dcd_len = 13u;
        const uint32_t es_len = 3u + (5u + dcd_len) + (5u + dsi_len) + (5u + 1u);
        size_t w;
        rc = buf_atom_open(b, "wave", &w);
        size_t f2;
        if (rc == TC_OK) { rc = buf_atom_open(b, "frma", &f2); }
        if (rc == TC_OK) { rc = buf_bytes(b, "mp4a", 4u); }
        if (rc == TC_OK) { buf_atom_close(b, f2); }
        /* 'mp4a' 12B 空原子（frma 之后、esds 之前；QuickTime 声组件链终结标记） */
        if (rc == TC_OK) { rc = buf_be32(b, 12u); }
        if (rc == TC_OK) { rc = buf_bytes(b, "mp4a", 4u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        size_t e;
        if (rc == TC_OK) { rc = buf_atom_open(b, "esds", &e); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }       /* version/flags */
        if (rc == TC_OK) { rc = buf_be8(b, 0x03u); }     /* ES_Descriptor tag */
        if (rc == TC_OK) { rc = buf_desc_len4(b, es_len); }
        if (rc == TC_OK) { rc = buf_be16(b, 2u); }       /* ES_ID */
        if (rc == TC_OK) { rc = buf_be8(b, 0u); }        /* flags */
        if (rc == TC_OK) { rc = buf_be8(b, 0x04u); }     /* DecoderConfigDescriptor tag */
        if (rc == TC_OK) { rc = buf_desc_len4(b, dcd_len); }
        if (rc == TC_OK) { rc = buf_be8(b, 0x40u); }     /* objectTypeIndication：AAC */
        if (rc == TC_OK) { rc = buf_be8(b, 0x15u); }     /* streamType audio<<2|1 */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }       /* bufferSizeDB（u24）高 16 位 */
        if (rc == TC_OK) { rc = buf_be8(b, 0u); }        /* bufferSizeDB 低 8 位 */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }       /* maxBitrate */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }       /* avgBitrate */
        if (rc == TC_OK) { rc = buf_be8(b, 0x05u); }     /* DecSpecificInfo tag（DCD 兄弟） */
        if (rc == TC_OK) { rc = buf_desc_len4(b, dsi_len); }
        if (rc == TC_OK) { rc = buf_bytes(b, a->asc, a->asc_size); }
        if (rc == TC_OK) { rc = buf_be8(b, 0x06u); }     /* SLConfigDescriptor tag（DCD 兄弟） */
        if (rc == TC_OK) { rc = buf_desc_len4(b, 1u); }
        if (rc == TC_OK) { rc = buf_be8(b, 0x02u); }
        if (rc == TC_OK) { buf_atom_close(b, e); }
        /* 8B null 终结子（wave 链尾） */
        if (rc == TC_OK) { rc = buf_be32(b, 8u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, w);
        /* chan（声道布局声明，与 lpcm 档同 tag 集） */
        const uint32_t tag = (a->channels == 1u) ? TC_AUDIO_CHAN_TAG_MONO
                           : (a->channels == 2u) ? TC_AUDIO_CHAN_TAG_STEREO
                           : (a->channels == 6u) ? TC_AUDIO_CHAN_TAG_5_1_A
                                                 : TC_AUDIO_CHAN_TAG_7_1_A;
        size_t c;
        rc = buf_atom_open(b, "chan", &c);
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }       /* version/flags */
        if (rc == TC_OK) { rc = buf_be32(b, tag); }      /* channelLayoutTag */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }       /* channelBitmap */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }       /* numberChannelDescriptions */
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, c);
    }
    if (rc != TC_OK) { return rc; }
    buf_atom_close(b, ent);
    buf_atom_close(b, s);
    return TC_OK;
}

/* v1.5 tmcd 时间码轨元数据（finish 透传；NULL = 无时码轨） */
typedef struct tmcd_moov_meta {
    uint32_t fps, df;
    uint32_t track_id;        /* 视频/音频之后（tmcd 恒末轨） */
    uint32_t movie_dur;       /* 视频时长（movie timescale=1000，与 mvhd 一致） */
    uint32_t frame_count;     /* 视频帧数（stts 时长 = n × frame_duration） */
    uint64_t sample_offset;   /* 4B 样本绝对偏移（mdat 内） */
    uint32_t start_count;     /* 起始帧计数（样本值；换算在 set_timecode 完成） */
} tmcd_moov_meta;

/* fd/ts 规则（ffmpeg -write_tmcd oracle 实测）：整数 fps 族 fd=512、
 * ts=fps×512（24→12288、25→12800、30→15360）；NTSC 分数族（23.976/29.97/
 * 59.94，以 df=1 或 ntsc 标记表达——本 API 以 df 表达）fd=1001、
 * ts=标称×1000（30000/1001→30000）。NDF 的 NTSC 输入与整数 fps 同形，
 * 差异（mdhd 时长网格 512 vs 1001）记录于 ADR-C055。 */
static void tmcd_ts_params(uint32_t fps, uint32_t df, uint32_t* ts, uint32_t* fd)
{
    /* oracle：整数 fps 族 512 网格（ts=fps×512）；NTSC 分数族（df 表达）
     * 1001 网格（ts=标称×1000 = 30000/60000，非 fps×1001） */
    *fd = df ? 1001u : 512u;
    *ts = df ? fps * 1000u : fps * 512u;
}

/* tmcd trak（tkhd + edts + mdia{mdhd,hdlr,minf{gmhd,hdlr,dinf,stbl}}）。
 * hdlr 用本库 ISO 风格（与视频/音频轨一致；oracle 的 mhlr+pascal 差异
 * 记录于 ADR-C055）；gmhd 走最小形态（gmin + tmcd{tcmi 零值}——播放器
 * 不依赖，oracle 的 text/Lucida Grande 装饰性字段省略）。 */
static int32_t build_tmcd_trak(const tmcd_moov_meta* t, mov_buf* b)
{
    uint32_t ts = 0u, fd = 0u;
    tmcd_ts_params(t->fps, t->df, &ts, &fd);
    const uint32_t mdhd_dur = t->frame_count * fd;

    size_t a_trak;
    int32_t rc = buf_atom_open(b, "trak", &a_trak);
    /* tkhd（flags=0x2 in-movie；volume 0；无几何） */
    {
        size_t tk;
        if (rc == TC_OK) { rc = buf_atom_open(b, "tkhd", &tk); }
        if (rc == TC_OK) { rc = buf_be32(b, 0x00000002u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, t->track_id); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, t->movie_dur); }
        if (rc == TC_OK) { rc = buf_be32_n(b, 0u, 2u); }
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }       /* layer */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }       /* alternate_group */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }       /* volume 0 */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }
        if (rc == TC_OK) { rc = buf_identity_matrix(b); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { buf_atom_close(b, tk); }
    }
    /* edts（单条目 media_time=0，oracle 同构） */
    {
        size_t ed, el;
        if (rc == TC_OK) { rc = buf_atom_open(b, "edts", &ed); }
        if (rc == TC_OK) { rc = buf_atom_open(b, "elst", &el); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }
        if (rc == TC_OK) { rc = buf_be32(b, t->movie_dur); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0x00010000u); }
        if (rc == TC_OK) { buf_atom_close(b, el); }
        if (rc == TC_OK) { buf_atom_close(b, ed); }
    }
    size_t a_mdia;
    if (rc == TC_OK) { rc = buf_atom_open(b, "mdia", &a_mdia); }
    /* mdhd */
    {
        size_t mh;
        if (rc == TC_OK) { rc = buf_atom_open(b, "mdhd", &mh); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, ts); }
        if (rc == TC_OK) { rc = buf_be32(b, mdhd_dur); }
        if (rc == TC_OK) { rc = buf_be16(b, 0x55C4u); }
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }
        if (rc == TC_OK) { buf_atom_close(b, mh); }
    }
    /* hdlr（ISO 风格，本库惯例） */
    {
        size_t hl;
        if (rc == TC_OK) { rc = buf_atom_open(b, "hdlr", &hl); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_bytes(b, "tmcd", 4u); }
        if (rc == TC_OK) { rc = buf_be32_n(b, 0u, 3u); }
        static const char NM[] = "Topos TimeCode Handler";
        if (rc == TC_OK) { rc = buf_bytes(b, NM, sizeof(NM)); }
        if (rc == TC_OK) { buf_atom_close(b, hl); }
    }
    size_t a_minf;
    if (rc == TC_OK) { rc = buf_atom_open(b, "minf", &a_minf); }
    /* gmhd（最小形态：gmin + tmcd{tcmi 零值}） */
    {
        size_t gm, tm2, ti;
        if (rc == TC_OK) { rc = buf_atom_open(b, "gmhd", &gm); }
        if (rc == TC_OK) { rc = buf_atom_open(b, "gmin", &tm2); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }        /* ver/flags */
        if (rc == TC_OK) { rc = buf_be16(b, 0x0040u); }   /* graphicsMode */
        if (rc == TC_OK) { rc = buf_be16(b, 0x8000u); }   /* opColor r */
        if (rc == TC_OK) { rc = buf_be16(b, 0x8000u); }   /* g */
        if (rc == TC_OK) { rc = buf_be16(b, 0x8000u); }   /* b */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }        /* balance */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }        /* reserved */
        if (rc == TC_OK) { buf_atom_close(b, tm2); }
        if (rc == TC_OK) { rc = buf_atom_open(b, "tmcd", &tm2); }
        if (rc == TC_OK) { rc = buf_atom_open(b, "tcmi", &ti); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }        /* ver/flags */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }        /* textFont */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }        /* textFlags */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }        /* height */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }        /* width */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }        /* bgColor.rgb */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }        /* bgColor.alpha(6B 域） */
        if (rc == TC_OK) { rc = buf_be32_n(b, 0u, 2u); }  /* defaultTextBox(8B) */
        if (rc == TC_OK) { rc = buf_be32_n(b, 0u, 2u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }        /* reserved */
        if (rc == TC_OK) { buf_atom_close(b, ti); }
        if (rc == TC_OK) { buf_atom_close(b, tm2); }
        if (rc == TC_OK) { buf_atom_close(b, gm); }
    }
    /* minf hdlr（dhlr）+ dinf（与视频/音频轨一致） */
    {
        size_t hl;
        if (rc == TC_OK) { rc = buf_atom_open(b, "hdlr", &hl); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_bytes(b, "dhlr", 4u); }
        if (rc == TC_OK) { rc = buf_be32_n(b, 0u, 3u); }
        static const char NM[] = "Data Handler";
        if (rc == TC_OK) { rc = buf_bytes(b, NM, sizeof(NM)); }
        if (rc == TC_OK) { buf_atom_close(b, hl); }
    }
    {
        size_t di, dr, ur;
        if (rc == TC_OK) { rc = buf_atom_open(b, "dinf", &di); }
        if (rc == TC_OK) { rc = buf_atom_open(b, "dref", &dr); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }
        if (rc == TC_OK) { rc = buf_atom_open(b, "url ", &ur); }
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, ur);
        buf_atom_close(b, dr);
        buf_atom_close(b, di);
    }
    size_t a_stbl;
    if (rc == TC_OK) { rc = buf_atom_open(b, "stbl", &a_stbl); }
    /* stsd：单 'tmcd' entry（语义字段与 oracle 逐字节对齐） */
    {
        size_t sd;
        if (rc == TC_OK) { rc = buf_atom_open(b, "stsd", &sd); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }
        size_t ent2;
        if (rc == TC_OK) { rc = buf_atom_open(b, "tmcd", &ent2); }
        static const uint8_t ZERO6[6] = { 0 };
        if (rc == TC_OK) { rc = buf_bytes(b, ZERO6, 6u); }
        if (rc == TC_OK) { rc = buf_be16(b, 1u); }        /* data_reference_index */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); } /* oracle 追加保留 u32（dri 与 flags 之间） */
        if (rc == TC_OK) { rc = buf_be32(b, t->df ? 0x1u : 0x0u); } /* flags（bit0=DF） */
        if (rc == TC_OK) { rc = buf_be32(b, ts); }        /* time_scale */
        if (rc == TC_OK) { rc = buf_be32(b, fd); }        /* frame_duration */
        if (rc == TC_OK) { rc = buf_be8(b, (uint8_t)t->fps); }
        if (rc == TC_OK) { rc = buf_be8(b, 0u); }         /* 保留 */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }
        if (rc == TC_OK) { buf_atom_close(b, ent2); }
        if (rc == TC_OK) { buf_atom_close(b, sd); }
    }
    /* stts（单条目：n × fd） */
    if (rc == TC_OK) {
        rc = buf_be32(b, 8u + 16u);                       /* 固定 24B 原子 */
        if (rc == TC_OK) { rc = buf_bytes(b, "stts", 4u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }
        if (rc == TC_OK) { rc = buf_be32(b, t->frame_count); }
        if (rc == TC_OK) { rc = buf_be32(b, fd); }
    }
    /* stsc（单条目：ver/flags 4 + count 4 + entry 12） */
    if (rc == TC_OK) {
        rc = buf_be32(b, 8u + 20u);
        if (rc == TC_OK) { rc = buf_bytes(b, "stsc", 4u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }   /* ver/flags */
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }   /* entry_count */
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }   /* first_chunk */
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }   /* samples_per_chunk */
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }   /* sample_description_index */
    }
    /* stsz（uniform 4B × 1） */
    if (rc == TC_OK) {
        rc = buf_be32(b, 8u + 12u);
        if (rc == TC_OK) { rc = buf_bytes(b, "stsz", 4u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 4u); }
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }
    }
    /* stco / co64（单条目；>4GB 文件 tmcd 样本恒在 mdat 尾=全文件最大
     * 偏移，按轨内独立判定升 co64——与音频轨同构，防 u32 截断写坏） */
    if (rc == TC_OK) {
        const int use_co64 = t->sample_offset > 0xFFFFFFFFull;
        const uint32_t ent = use_co64 ? 8u : 4u;
        rc = buf_be32(b, 8u + 8u + ent);
        if (rc == TC_OK) { rc = buf_bytes(b, use_co64 ? "co64" : "stco", 4u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }
        if (use_co64) {
            if (rc == TC_OK) { rc = buf_be64(b, t->sample_offset); }
        } else if (rc == TC_OK) {
            rc = buf_be32(b, (uint32_t)t->sample_offset);
        }
    }
    if (rc != TC_OK) { return rc; }
    buf_atom_close(b, a_stbl);
    buf_atom_close(b, a_minf);
    buf_atom_close(b, a_mdia);
    buf_atom_close(b, a_trak);
    return TC_OK;
}

/* 音频 trak（v1.1 第二轨；tkhd track_id=2、音量 1.0、无几何） */
static int32_t build_audio_trak(const audio_moov_meta* a, mov_buf* b)
{
    int use_co64 = 0;
    for (uint32_t i = 0; i < a->n; ++i) {
        if (a->offsets[i] > 0xFFFFFFFFull) { use_co64 = 1; break; }
    }
    size_t a_trak, a_mdia, a_minf, a_stbl;
    int32_t rc = buf_atom_open(b, "trak", &a_trak);
    /* tkhd */
    {
        size_t t;
        rc = buf_atom_open(b, "tkhd", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0x00000007u); }   /* Enabled|InMovie|InPreview */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }            /* creation */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }            /* modification */
        if (rc == TC_OK) { rc = buf_be32(b, a->track_id); }  /* track_id（v1.6：2..N+1） */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }            /* reserved */
        if (rc == TC_OK) {                                    /* duration（tick=采样帧） */
            rc = buf_be32(b, a->total_frames > 0xFFFFFFFFull
                             ? 0xFFFFFFFFu : (uint32_t)a->total_frames);
        }
        if (rc == TC_OK) { rc = buf_be32_n(b, 0u, 2u); }      /* reserved ×2 */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }            /* layer */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }            /* alternate_group */
        if (rc == TC_OK) { rc = buf_be16(b, 0x0100u); }       /* volume 1.0 */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }            /* reserved */
        if (rc == TC_OK) { rc = buf_identity_matrix(b); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }            /* width */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }            /* height */
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* edts/elst（v1.4）：仅 mp4a + priming>0 时写（lpcm 恒不写）。最小子集：
     * 单条目、media_time = priming（媒体 timescale = 采样率下的采样数）、
     * media_rate = 1.0；segment_duration = 轨时长换算到 movie timescale
     * （mvhd 恒 1000，见上方）。解码侧据此裁剪 AAC priming（ADR-C052）。
     * demux 侧约束一致：entry_count=1、media_time>0、rate==1.0。 */
    if (a->priming > 0u) {
        size_t ed;
        rc = buf_atom_open(b, "edts", &ed);
        size_t el;
        if (rc == TC_OK) { rc = buf_atom_open(b, "elst", &el); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }            /* version/flags */
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }            /* entry_count */
        const uint64_t seg =
            (a->total_frames * 1000ull + a->rate - 1ull) / a->rate; /* ceil */
        if (rc == TC_OK) {
            rc = buf_be32(b, seg > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)seg);
        }
        if (rc == TC_OK) { rc = buf_be32(b, a->priming); }    /* media_time */
        if (rc == TC_OK) { rc = buf_be32(b, 0x00010000u); }   /* media_rate 1.0 */
        if (rc == TC_OK) { buf_atom_close(b, el); }
        if (rc == TC_OK) { buf_atom_close(b, ed); }
        if (rc != TC_OK) { return rc; }
    }
    rc = buf_atom_open(b, "mdia", &a_mdia);
    if (rc != TC_OK) { return rc; }
    /* mdhd：timescale = 采样率 */
    {
        size_t t;
        rc = buf_atom_open(b, "mdhd", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, a->rate); }
        if (rc == TC_OK) {
            rc = buf_be32(b, a->total_frames > 0xFFFFFFFFull
                             ? 0xFFFFFFFFu : (uint32_t)a->total_frames);
        }
        if (rc == TC_OK) { rc = buf_be16(b, 0x55C4u); }       /* 语言 und */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* hdlr 'soun' */
    {
        size_t t;
        rc = buf_atom_open(b, "hdlr", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }            /* pre_defined */
        if (rc == TC_OK) { rc = buf_bytes(b, "soun", 4u); }
        if (rc == TC_OK) { rc = buf_be32_n(b, 0u, 3u); }
        /* v1.6：轨名进 hdlr 名（resolve/quicktime 显示源之一）；缺省回退 */
        if (rc == TC_OK) {
            if (a->name != NULL && a->name[0] != '\0') {
                rc = buf_bytes(b, a->name, strlen(a->name) + 1u);
            } else {
                static const char NM[] = "Topos Audio Handler";
                rc = buf_bytes(b, NM, sizeof(NM));
            }
        }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    rc = buf_atom_open(b, "minf", &a_minf);
    if (rc != TC_OK) { return rc; }
    /* smhd */
    {
        size_t t;
        rc = buf_atom_open(b, "smhd", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }            /* version/flags */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }            /* balance */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }            /* reserved */
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* minf hdlr（dhlr，与视频轨一致） */
    {
        size_t t;
        rc = buf_atom_open(b, "hdlr", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_bytes(b, "dhlr", 4u); }
        if (rc == TC_OK) { rc = buf_be32_n(b, 0u, 3u); }
        static const char NM[] = "Data Handler";
        if (rc == TC_OK) { rc = buf_bytes(b, NM, sizeof(NM)); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* dinf > dref > url（自包含） */
    {
        size_t t, d, u;
        rc = buf_atom_open(b, "dinf", &t);
        if (rc == TC_OK) { rc = buf_atom_open(b, "dref", &d); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }
        if (rc == TC_OK) { rc = buf_atom_open(b, "url ", &u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0x00000001u); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, u);
        buf_atom_close(b, d);
        buf_atom_close(b, t);
    }
    /* stbl */
    rc = buf_atom_open(b, "stbl", &a_stbl);
    if (rc == TC_OK) { rc = build_audio_stsd(a, b); }
    if (rc == TC_OK) { rc = write_stts_rle(b, a->n, a->durs); }
    if (rc == TC_OK && a->n != 0u) { rc = write_stsc_single(b); }
    /* stsz（逐 chunk 精确尺寸——lpcm 末 chunk 可短、mp4a 变长） */
    if (rc == TC_OK) {
        size_t t;
        rc = buf_atom_open(b, "stsz", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, a->n); }
        for (uint32_t k = 0; k < a->n && rc == TC_OK; ++k) { rc = buf_be32(b, a->sizes[k]); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* stco / co64 */
    if (rc == TC_OK) {
        size_t t;
        rc = buf_atom_open(b, use_co64 ? "co64" : "stco", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, a->n); }
        for (uint32_t k = 0; k < a->n && rc == TC_OK; ++k) {
            if (use_co64) { rc = buf_be64(b, a->offsets[k]); }
            else { rc = buf_be32(b, (uint32_t)a->offsets[k]); }
        }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    buf_atom_close(b, a_stbl);
    buf_atom_close(b, a_minf);
    buf_atom_close(b, a_mdia);
    /* v1.6：轨名（QuickTime 惯例 trak/udta/©nam；文本 = u16 长度 + UTF-8
     * + u16 0 终结，movenc 同构）。Resolve/Premiere 据此显示轨名。 */
    if (a->name != NULL && a->name[0] != '\0') {
        size_t ud, nm;
        if (rc == TC_OK) { rc = buf_atom_open(b, "udta", &ud); }
        if (rc == TC_OK) { rc = buf_atom_open(b, "\xa9""nam", &nm); }
        const size_t blen = strlen(a->name);
        if (rc == TC_OK) { rc = buf_be16(b, (uint16_t)blen); }
        if (rc == TC_OK) { rc = buf_bytes(b, a->name, blen); }
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, nm);
        buf_atom_close(b, ud);
    }
    buf_atom_close(b, a_trak);
    return TC_OK;
}

/* moov 构建（mux finish 与 faststart 共用）。
 * offsets 仅在 force_co64==0 时被扫描以决定 stco/co64；值不影响 moov 长度。
 * budget 非 NULL 时 stsd 追加 tpcB（faststart 从解析侧原样保留）。
 * meta 非 NULL 时 stsd 追加 tpcD（v1.7；faststart 从解析侧原样保留）。
 * audio 非 NULL 时追加音频 trak（第二轨；轨内 co64 独立判定）。 */
static int32_t build_moov(const topos_movie_config* cfg, uint8_t bitstream_major,
                          uint32_t n,
                          const uint64_t* offsets, const uint32_t* sizes,
                          const uint32_t* durs, uint64_t total_dur, int force_co64,
                          const topos_alpha_budget_info* budget,
                          const topos_movie_meta* meta,
                          const topos_raw_meta* rmeta, mov_buf* b,
                          const uint8_t* sync_map, const audio_moov_meta* audios,
                          uint32_t n_aud, const tmcd_moov_meta* tmcd)
{
    int use_co64 = force_co64;
    if (!use_co64) {
        for (uint32_t i = 0; i < n; ++i) {
            if (offsets[i] > 0xFFFFFFFFull) { use_co64 = 1; break; }
        }
    }
    size_t a_moov, a_trak, a_mdia, a_minf, a_stbl;
    int32_t rc = buf_atom_open(b, "moov", &a_moov);
    if (rc != TC_OK) { return rc; }
    /* mvhd */
    {
        size_t t;
        rc = buf_atom_open(b, "mvhd", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }                  /* version/flags */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }                  /* creation */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }                  /* modification */
        if (rc == TC_OK) { rc = buf_be32(b, 1000u); }               /* timescale */
        if (rc == TC_OK) {                                          /* duration（ms，饱和） */
            /* 复验 P1-13：total_dur ≤ 2^26×(2^32−1) ≈ 2^58，×1000 可溢出
             * u64 —— 乘法 checked，饱和语义与 tkhd/mdhd 一致。
             * v1.1：双轨时取视频/音频轨 ms 时长的较大者。 */
            uint64_t ms;
            if (!tc_umul_u64(total_dur, 1000ull, &ms)) {
                ms = UINT64_MAX;
            }
            ms /= (uint64_t)cfg->timescale;
            /* v1.6：逐音频轨 ms 时长，取较大者（mvhd = 全片时长） */
            for (uint32_t ai = 0; ai < n_aud; ++ai) {
                const audio_moov_meta* a = &audios[ai];
                if (a->total_frames == 0u) { continue; }
                uint64_t ams;
                if (!tc_umul_u64(a->total_frames, 1000ull, &ams)) {
                    ams = UINT64_MAX;
                }
                ams /= (uint64_t)a->rate;
                if (ams > ms) { ms = ams; }
            }
            rc = buf_be32(b, ms > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)ms);
        }
        if (rc == TC_OK) { rc = buf_be32(b, 0x00010000u); }         /* rate */
        if (rc == TC_OK) { rc = buf_be16(b, 0x0100u); }             /* volume */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }                  /* reserved */
        if (rc == TC_OK) { rc = buf_be32_n(b, 0u, 2u); }            /* reserved ×2 */
        if (rc == TC_OK) { rc = buf_identity_matrix(b); }
        if (rc == TC_OK) { rc = buf_be32_n(b, 0u, 6u); }            /* pre_defined */
        if (rc == TC_OK) {
            rc = buf_be32(b, 1u + n_aud + (tmcd != NULL ? 1u : 0u) + 1u);
        } /* next_track_id（v1.6：video=1、audio=2..N+1、tmcd=N+2） */
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    rc = buf_atom_open(b, "trak", &a_trak);
    if (rc != TC_OK) { return rc; }
    /* tkhd */
    {
        size_t t;
        rc = buf_atom_open(b, "tkhd", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0x00000007u); }         /* Enabled|InMovie|InPreview */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }                  /* creation */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }                  /* modification */
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }                  /* track_id */
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }                  /* reserved */
        if (rc == TC_OK) {                                          /* duration（轨 timescale，饱和） */
            rc = buf_be32(b, total_dur > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)total_dur);
        }
        if (rc == TC_OK) { rc = buf_be32_n(b, 0u, 2u); }            /* reserved ×2 */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }                  /* layer */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }                  /* alternate_group */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }                  /* volume（视频轨） */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }                  /* reserved */
        if (rc == TC_OK) { rc = buf_identity_matrix(b); }
        if (rc == TC_OK) { rc = buf_be32(b, (uint32_t)cfg->visible_width << 16); }
        if (rc == TC_OK) { rc = buf_be32(b, (uint32_t)cfg->visible_height << 16); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* v1.5：视频 trak tref 引用 tmcd 轨（QuickTime 惯例，oracle 同构：
     * tref{'tmcd': [tmcd_track_id]}） */
    if (tmcd != NULL) {
        size_t tr, tr2;
        rc = buf_atom_open(b, "tref", &tr);
        if (rc == TC_OK) { rc = buf_atom_open(b, "tmcd", &tr2); }
        if (rc == TC_OK) { rc = buf_be32(b, tmcd->track_id); }
        if (rc == TC_OK) { buf_atom_close(b, tr2); }
        if (rc == TC_OK) { buf_atom_close(b, tr); }
        if (rc != TC_OK) { return rc; }
    }
    rc = buf_atom_open(b, "mdia", &a_mdia);
    if (rc != TC_OK) { return rc; }
    /* mdhd */
    {
        size_t t;
        rc = buf_atom_open(b, "mdhd", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, cfg->timescale); }
        if (rc == TC_OK) { rc = buf_be32(b, total_dur > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)total_dur); }
        if (rc == TC_OK) { rc = buf_be16(b, 0x55C4u); }             /* 语言 und */
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* mdia hdlr */
    {
        size_t t;
        rc = buf_atom_open(b, "hdlr", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }                  /* pre_defined */
        if (rc == TC_OK) { rc = buf_bytes(b, "vide", 4u); }
        if (rc == TC_OK) { rc = buf_be32_n(b, 0u, 3u); }
        static const char NM[] = "Topos Video Handler";
        if (rc == TC_OK) { rc = buf_bytes(b, NM, sizeof(NM)); }     /* 含 NUL */
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    rc = buf_atom_open(b, "minf", &a_minf);
    if (rc != TC_OK) { return rc; }
    /* vmhd */
    {
        size_t t;
        rc = buf_atom_open(b, "vmhd", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0x00000001u); }
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }
        if (rc == TC_OK) { rc = buf_be16(b, 0u); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* minf hdlr */
    {
        size_t t;
        rc = buf_atom_open(b, "hdlr", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_bytes(b, "dhlr", 4u); }
        if (rc == TC_OK) { rc = buf_be32_n(b, 0u, 3u); }
        static const char NM[] = "Data Handler";
        if (rc == TC_OK) { rc = buf_bytes(b, NM, sizeof(NM)); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* dinf > dref > url */
    {
        size_t t, d, u;
        rc = buf_atom_open(b, "dinf", &t);
        if (rc == TC_OK) { rc = buf_atom_open(b, "dref", &d); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 1u); }
        if (rc == TC_OK) { rc = buf_atom_open(b, "url ", &u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0x00000001u); }         /* 自包含 */
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, u);
        buf_atom_close(b, d);
        buf_atom_close(b, t);
    }
    /* stbl */
    rc = buf_atom_open(b, "stbl", &a_stbl);
    if (rc == TC_OK) { rc = build_stsd(cfg, bitstream_major, budget, meta, rmeta, b); }
    /* stts（等 dur RLE，单遍规范写法） */
    if (rc == TC_OK) { rc = write_stts_rle(b, n, durs); }
    /* stss（n==0 时不写）。sync_map 非 NULL 时只列同步 sample——V9 轨
     * 只列 I（topos_v9_micro_gop_plan 批 3）；全同步位图（V1..V8）列出
     * 全部 1..n，与旧布局逐字节一致（container golden 零变化）。 */
    if (rc == TC_OK && n != 0u) {
        uint32_t sync_count = 0u;
        for (uint32_t k = 0; k < n; ++k) {
            sync_count += (sync_map == NULL || sync_map[k] != 0u) ? 1u : 0u;
        }
        if (sync_count != 0u) {
            size_t t;
            rc = buf_atom_open(b, "stss", &t);
            if (rc == TC_OK) { rc = buf_be32(b, 0u); }
            if (rc == TC_OK) { rc = buf_be32(b, sync_count); }
            for (uint32_t k = 0; k < n && rc == TC_OK; ++k) {
                if (sync_map == NULL || sync_map[k] != 0u) { rc = buf_be32(b, k + 1u); }
            }
            if (rc != TC_OK) { return rc; }
            buf_atom_close(b, t);
        }
    }
    /* stsc（单条：1 sample/chunk；n==0 时不写） */
    if (rc == TC_OK && n != 0u) { rc = write_stsc_single(b); }
    /* stsz（non-const 精确尺寸） */
    if (rc == TC_OK) {
        size_t t;
        rc = buf_atom_open(b, "stsz", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }   /* sample_size */
        if (rc == TC_OK) { rc = buf_be32(b, n); }
        for (uint32_t k = 0; k < n && rc == TC_OK; ++k) { rc = buf_be32(b, sizes[k]); }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    /* stco / co64 */
    if (rc == TC_OK) {
        size_t t;
        rc = buf_atom_open(b, use_co64 ? "co64" : "stco", &t);
        if (rc == TC_OK) { rc = buf_be32(b, 0u); }
        if (rc == TC_OK) { rc = buf_be32(b, n); }
        for (uint32_t k = 0; k < n && rc == TC_OK; ++k) {
            if (use_co64) { rc = buf_be64(b, offsets[k]); }
            else { rc = buf_be32(b, (uint32_t)offsets[k]); }
        }
        if (rc != TC_OK) { return rc; }
        buf_atom_close(b, t);
    }
    buf_atom_close(b, a_stbl);
    buf_atom_close(b, a_minf);
    buf_atom_close(b, a_mdia);
    buf_atom_close(b, a_trak);
    /* v1.1/v1.6 音频 trak（声明序 2..N+1；轨内 stco/co64 独立判定） */
    for (uint32_t ai = 0; ai < n_aud; ++ai) {
        rc = build_audio_trak(&audios[ai], b);
        if (rc != TC_OK) { return rc; }
    }
    /* v1.5 tmcd 时间码轨（恒末轨） */
    if (tmcd != NULL) {
        rc = build_tmcd_trak(tmcd, b);
        if (rc != TC_OK) { return rc; }
    }
    buf_atom_close(b, a_moov);
    return TC_OK;
}

int32_t tc_mux_finish(topos_mux* m)
{
    if (m == NULL || m->finished != 0u || m->poisoned != 0u) {
        tc_set_error(TC_ERR_STATE, "mux 未创建/已 finish/已中毒（写失败仅可 free）");
        return TC_ERR_STATE;
    }
    /* v1.6：逐轨半声明检查（声明即须有样本） */
    for (uint32_t ai = 0; ai < m->n_a; ++ai) {
        if (m->a_trk[ai].codec != TC_AUDIO_CODEC_NONE && m->a_trk[ai].n == 0u) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT,
                         "音轨 %u 已声明但未注入任何音频样本"
                         "（add_audio_to / add_audio）", ai);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    uint64_t total_dur = 0u;
    for (uint32_t i = 0; i < m->n; ++i) {
        if (!tc_uadd_u64(total_dur, (uint64_t)m->durs[i], &total_dur)) {
            return TC_ERR_LIMIT_EXCEEDED;
        }
    }
    audio_moov_meta ams[TOPOS_MAX_AUDIO_TRACKS];
    uint32_t n_am = 0u;
    for (uint32_t ai = 0; ai < m->n_a; ++ai) {
        const audio_track_state* t = &m->a_trk[ai];
        if (t->codec == TC_AUDIO_CODEC_NONE) { continue; }
        audio_moov_meta* am = &ams[n_am++];
        memset(am, 0, sizeof(*am));
        am->track_id = 2u + ai;             /* v1.6：audio=2..N+1 */
        am->name = (t->name[0] != '\0') ? t->name : NULL;
        am->codec = t->codec;
        am->rate = t->rate;
        am->channels = t->channels;
        am->layout = t->layout;
        am->bits = t->bits;
        am->format = t->format;
        am->n = t->n;
        am->total_frames = t->total_frames;
        am->offsets = t->offsets;
        am->sizes = t->sizes;
        am->durs = t->durs;
        am->asc = t->asc;
        am->asc_size = t->asc_size;
        am->priming = t->priming;
    }
    uint64_t mdat_payload_end = m->written; /* moov 未追加前的 payload 终点（tmcd 样本计入后推进） */
    /* v1.5 tmcd：4B 帧计数样本追加到 mdat 尾（恒末样本） */
    tmcd_moov_meta tm;
    tmcd_moov_meta* tm_ptr = NULL;
    if (m->tc_set != 0u) {
        memset(&tm, 0, sizeof(tm));
        const uint32_t total_minutes = m->tc_hh * 60u + m->tc_mm;
        const uint32_t df_per_min = tmcd_df_per_minute(m->tc_fps);
        const uint32_t drops =
            m->tc_df != 0u ? df_per_min * (total_minutes - total_minutes / 10u)
                           : 0u;
        tm.start_count =
            (m->tc_hh * 3600u + m->tc_mm * 60u + m->tc_ss) * m->tc_fps
            + m->tc_ff - drops;
        tm.fps = m->tc_fps;
        tm.df = m->tc_df;
        tm.track_id = 2u + n_am;   /* v1.6：audio=2..N+1、tmcd=N+2 恒末轨 */
        /* 视频轨 movie-timescale 时长（与 mvhd 同口径：ticks×1000/timescale） */
        tm.movie_dur = (uint32_t)((total_dur * 1000ull + m->cfg.timescale - 1ull)
                                  / m->cfg.timescale);
        tm.frame_count = m->n;
        uint8_t s4[4];
        tc_store_be32(s4, tm.start_count);
        int32_t src_rc = sink_write(m, s4, 4u);
        if (src_rc != TC_OK) { return src_rc; }
        tm.sample_offset = m->written - 4u;
        tm_ptr = &tm;
        /* 样本计入 mdat 载荷（长度回填与 moov 起点均依赖此值） */
        mdat_payload_end = m->written;
    }
    mov_buf b = { NULL, 0, 0 };
    int32_t rc = build_moov(&m->cfg, m->bitstream_major, m->n, m->offsets, m->sizes, m->durs, total_dur, 0,
                            m->budget_set ? &m->budget : NULL,
                            m->meta_set ? &m->meta : NULL,
                            m->rmeta_set ? &m->rmeta : NULL, &b, m->sync,
                            n_am != 0u ? ams : NULL, n_am, tm_ptr);
    if (rc == TC_OK) { rc = sink_write(m, b.data, b.len); }
    if (rc == TC_OK) {
        /* 回填 64 位 mdat 长度：头布局 [size=1][mdat][u64]，ext 字段在
         * ftyp 后 +8 处（ftyp=20，size 字段 20..24，type 24..28，u64 28..36）。 */
        uint64_t mdat_size = mdat_payload_end - m->mdat_data_off + MOV_MDAT_HDR;
        uint8_t sz8[8];
        tc_store_be64(sz8, mdat_size);
        int32_t iorc = m->sink.seek_write(m->sink.ctx,
                                          (uint64_t)MOV_FTYP_BYTES + 8u, sz8, 8u);
        if (iorc != TC_OK) {
            tc_set_error(TC_ERR_IO, "mdat 长度回填失败");
            rc = TC_ERR_IO;
        }
    }
    buf_free(&b);
    m->finished = 1u;
    return rc;
}

void tc_mux_free(topos_mux* m)
{
    if (m == NULL) { return; }
    tc_free(m->offsets);
    tc_free(m->sizes);
    tc_free(m->durs);
    tc_free(m->sync);
    for (uint32_t ai = 0; ai < m->n_a; ++ai) {
        tc_free(m->a_trk[ai].offsets);
        tc_free(m->a_trk[ai].sizes);
        tc_free(m->a_trk[ai].durs);
        tc_free(m->a_trk[ai].asc);
    }
    tc_free(m);
}

/* ============================ demux ============================ */

struct topos_movie {
    topos_io io;
    topos_movie_config cfg;
    uint32_t n;
    uint32_t timescale;
    uint16_t width, height;
    int faststart;
    uint8_t budget_set;             /* R3：stsd 携带 tpcB */
    uint8_t tpcc_major;             /* M9：tpcC 原子 version = 轨级 bitstream major */
    topos_alpha_budget_info budget;
    /* v1.7：stsd 携带 tpcD（档位/厂商；faststart 重建 moov 时原样保留） */
    topos_movie_meta meta;
    uint8_t meta_set;
    /* RC1：stsd 携带 trwm（TRAW 开发元数据；faststart 原样保留） */
    topos_raw_meta rmeta;
    uint8_t rmeta_set;
    uint64_t* offsets;
    uint32_t* sizes;
    uint64_t* pts;
    uint32_t* durs;
    uint8_t* sync; /* bitmap */
    uint32_t index_bytes;
    /* v1.1/v1.6 音频轨（可选；v1.6 起至多 TC_AUDIO_MAX_TRACKS 条，
     * 声明序 = 轨 id 序 2..N+1；轨 0 = v1.1 单轨视图） */
    audio_track_state a_trk[TOPOS_MAX_AUDIO_TRACKS];
    uint32_t n_a;                   /* 已解析音轨数 */
    /* v1.5 tmcd 时间码轨 */
    uint8_t tc_set;
    uint32_t tc_start_count, tc_fps, tc_df, tc_flags;
    uint32_t tc_movie_dur;          /* tkhd 时长（movie ts） */
    uint32_t tc_frame_count;        /* stts 展开 = n × fd 的 n */
};

static int32_t io_read(const topos_movie* mv, uint64_t off, void* buf, size_t len)
{
    if (len == 0u) { return TC_OK; }
    if (off > mv->io.length || len > mv->io.length - off) {
        tc_set_error(TC_ERR_TRUNCATED, "io 越界读 @%llu+%zu",
                     (unsigned long long)off, len);
        return TC_ERR_TRUNCATED;
    }
    int32_t rc = mv->io.read(mv->io.ctx, off, buf, len);
    if (rc != TC_OK) {
        tc_set_error(TC_ERR_IO, "io read 失败");
        return TC_ERR_IO;
    }
    return TC_OK;
}

typedef struct movie_sample_range {
    const topos_movie* movie;
    uint64_t sample_offset;
    uint64_t sample_size;
} movie_sample_range;

static int32_t movie_sample_range_read(void* ctx, uint64_t offset,
                                       void* buf, size_t len)
{
    const movie_sample_range* range = (const movie_sample_range*)ctx;
    if (range == NULL || range->movie == NULL || offset > range->sample_size ||
        (uint64_t)len > range->sample_size - offset ||
        range->sample_offset > UINT64_MAX - offset) {
        tc_set_error(TC_ERR_TRUNCATED, "sample range read @%llu+%zu",
                     (unsigned long long)offset, len);
        return TC_ERR_TRUNCATED;
    }
    return io_read(range->movie, range->sample_offset + offset, buf, len);
}

typedef struct {
    uint64_t size;
    uint8_t type[4];
    int ext;       /* 1 = 16B 头（64 位 size） */
    uint64_t body; /* body 绝对偏移 */
} atom_hdr;

/* 读 atom 头并做边界检查（spec §7.2）。size==0 = 延伸至 end（QT 顶层语义，
 * 嵌套上下文中按父容器 end 解释——有界，fuzz 安全）。 */
static int32_t read_atom(const topos_movie* mv, uint64_t off, uint64_t end, atom_hdr* h)
{
    if (off > end || end - off < 8ull) {
        tc_set_error(TC_ERR_MALFORMED, "atom 头越界");
        return TC_ERR_MALFORMED;
    }
    uint8_t hdr[8];
    int32_t rc = io_read(mv, off, hdr, 8u);
    if (rc != TC_OK) { return rc; }
    h->size = tc_load_be32(hdr);
    memcpy(h->type, hdr + 4, 4u);
    h->ext = 0;
    h->body = off + 8ull;
    if (h->size == 1ull) {
        if (end - off < 16ull) {
            tc_set_error(TC_ERR_MALFORMED, "ext size 头越界");
            return TC_ERR_MALFORMED;
        }
        uint8_t ext8[8];
        rc = io_read(mv, h->body, ext8, 8u);
        if (rc != TC_OK) { return rc; }
        h->size = tc_load_be64(ext8);
        if (h->size < 16ull) {
            tc_set_error(TC_ERR_MALFORMED, "ext size < 16");
            return TC_ERR_MALFORMED;
        }
        h->ext = 1;
        h->body = off + 16ull;
    } else if (h->size == 0ull) {
        h->size = end - off;
        if (h->size < 8ull) {
            tc_set_error(TC_ERR_MALFORMED, "空 atom");
            return TC_ERR_MALFORMED;
        }
    }
    if (h->size > end - off) {
        tc_set_error(TC_ERR_MALFORMED, "atom size 越界 type=%c%c%c%c",
                     h->type[0], h->type[1], h->type[2], h->type[3]);
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

static int type_is(const atom_hdr* h, const char t[4]) { return memcmp(h->type, t, 4u) == 0; }

/* full atom 的 version/flags + entry_count */
static int32_t read_entry_count(const topos_movie* mv, uint64_t body, uint64_t end,
                                uint32_t* entry_count)
{
    if (body > end || end - body < 8ull) {
        tc_set_error(TC_ERR_MALFORMED, "full atom 过短");
        return TC_ERR_MALFORMED;
    }
    uint8_t b[8];
    int32_t rc = io_read(mv, body, b, 8u);
    if (rc != TC_OK) { return rc; }
    *entry_count = tc_load_be32(b + 4);
    return TC_OK;
}

/* —— stbl 解析暂存（一致性核对后才分配索引）—— */
typedef struct {
    uint32_t stts_entry_count;
    uint64_t stts_off, stts_end;
    uint64_t sample_total;
    uint32_t stsz_sample_size;   /* uniform 或 0 */
    uint32_t stsz_count;
    uint64_t stsz_data_off, stsz_data_end;
    uint32_t stsc_count;
    uint64_t stsc_off, stsc_end;
    uint32_t chunk_count;
    uint64_t stco_off, stco_end;
    int is_co64;
    /* P1-18：singleton 表 atom 重复出现 = malformed（旧实现"最后覆盖"，
     * 恶意构造可用重复 atom 掩盖首个表的一致性核对） */
    int has_stsd, has_stts, has_stsz, has_stsc, has_stco;
    int has_stss;
    uint32_t stss_count;
    uint64_t stss_off, stss_end;
    uint8_t tpcc[28];
    int has_tpcc;
    uint8_t tpcb[24];
    int has_tpcb;
    int has_tpcd;                   /* v1.7：tpcD 已解析（载荷直存 mv->meta） */
    uint16_t entry_w, entry_h;
} stbl_state;

static int32_t parse_stsd(topos_movie* mv, uint64_t body, uint64_t end, stbl_state* st)
{
    uint32_t entry_count = 0;
    int32_t rc = read_entry_count(mv, body, end, &entry_count);
    if (rc != TC_OK) { return rc; }
    if (entry_count != 1u) {
        tc_set_error(TC_ERR_MALFORMED, "stsd entry_count=%u ≠ 1", entry_count);
        return TC_ERR_MALFORMED;
    }
    atom_hdr ent;
    rc = read_atom(mv, body + 8ull, end, &ent);
    if (rc != TC_OK) { return rc; }
    if (!type_is(&ent, "TPIC")) {
        tc_set_error(TC_ERR_MALFORMED, "非 Topos 流（entry=%c%c%c%c，仅支持 TPIC）",
                     ent.type[0], ent.type[1], ent.type[2], ent.type[3]);
        return TC_ERR_MALFORMED;
    }
    uint64_t hdr_len = (uint64_t)(ent.ext ? 16 : 8);
    if (ent.size < hdr_len + 78ull) {
        tc_set_error(TC_ERR_MALFORMED, "TPIC entry 过短");
        return TC_ERR_MALFORMED;
    }
    uint8_t f[70];
    rc = io_read(mv, ent.body, f, 70u);
    if (rc != TC_OK) { return rc; }
    st->entry_w = tc_load_be16(f + 24);
    st->entry_h = tc_load_be16(f + 26);
    uint64_t ent_end = ent.body + ent.size - hdr_len;
    /* 子 atom：只取 tpcC；colr/pasp 等跳过（固定字段 78B 后起） */
    uint64_t off = ent.body + 78ull;
    while (off < ent_end) {
        atom_hdr c;
        rc = read_atom(mv, off, ent_end, &c);
        if (rc != TC_OK) { return rc; }
        if (type_is(&c, "tpcC")) {
            uint64_t clen = c.size - (uint64_t)(c.ext ? 16 : 8);
            if (clen != 28ull) {
                tc_set_error(TC_ERR_MALFORMED, "tpcC 长度 %llu ≠ 28",
                             (unsigned long long)clen);
                return TC_ERR_MALFORMED;
            }
            uint8_t tp[28];
            rc = io_read(mv, c.body, tp, 28u);
            if (rc != TC_OK) { return rc; }
            uint16_t tpcc_ver = tc_load_be16(tp + 4);
            if (memcmp(tp, "TPCC", 4u) != 0 ||
                (tpcc_ver < 1u || tpcc_ver > 9u)) {
                /* 9 = V9（micro-gop 计划批 3 容器接入）；老二进制在此明确拒绝 */
                tc_set_error(TC_ERR_MALFORMED, "tpcC magic/version 不符");
                return TC_ERR_MALFORMED;
            }
            mv->tpcc_major = (uint8_t)tpcc_ver;
            if (tc_crc32(tp, 24u) != tc_load_be32(tp + 24)) {
                tc_set_error(TC_ERR_CHECKSUM_MISMATCH, "tpcC CRC 不符");
                return TC_ERR_CHECKSUM_MISMATCH;
            }
            if (st->has_tpcc) {
                tc_set_error(TC_ERR_MALFORMED, "重复 tpcC");
                return TC_ERR_MALFORMED;
            }
            memcpy(st->tpcc, tp, 28u);
            st->has_tpcc = 1;
        } else if (type_is(&c, "tpcB")) {
            /* R3 预算元数据（container_spec v1.1）：可选，出现即严格校验 */
            uint64_t clen = c.size - (uint64_t)(c.ext ? 16 : 8);
            if (clen != 24ull) {
                tc_set_error(TC_ERR_MALFORMED, "tpcB 长度 %llu ≠ 24",
                             (unsigned long long)clen);
                return TC_ERR_MALFORMED;
            }
            uint8_t tb[24];
            rc = io_read(mv, c.body, tb, 24u);
            if (rc != TC_OK) { return rc; }
            if (memcmp(tb, "TPCB", 4u) != 0 || tc_load_be16(tb + 4) != 1u) {
                tc_set_error(TC_ERR_MALFORMED, "tpcB magic/version 不符");
                return TC_ERR_MALFORMED;
            }
            if (tc_crc32(tb, 20u) != tc_load_be32(tb + 20)) {
                tc_set_error(TC_ERR_CHECKSUM_MISMATCH, "tpcB CRC 不符");
                return TC_ERR_CHECKSUM_MISMATCH;
            }
            if (st->has_tpcb) {
                tc_set_error(TC_ERR_MALFORMED, "重复 tpcB");
                return TC_ERR_MALFORMED;
            }
            memcpy(st->tpcb, tb, 24u);
            st->has_tpcb = 1;
        } else if (type_is(&c, "tpcD")) {
            /* v1.7 电影元数据（container_spec v1.7）：可选，出现即严格校验。
             * 布局见 build_tpcd；载荷长度随串域可变（16..TPCD_PAYLOAD_MAX）。 */
            uint64_t clen = c.size - (uint64_t)(c.ext ? 16 : 8);
            if (clen < 16ull || clen > (uint64_t)TPCD_PAYLOAD_MAX) {
                tc_set_error(TC_ERR_MALFORMED, "tpcD 长度 %llu 越界 [16,%u]",
                             (unsigned long long)clen, (unsigned)TPCD_PAYLOAD_MAX);
                return TC_ERR_MALFORMED;
            }
            uint8_t td[TPCD_PAYLOAD_MAX];
            rc = io_read(mv, c.body, td, (size_t)clen);
            if (rc != TC_OK) { return rc; }
            if (memcmp(td, "TPCD", 4u) != 0 || tc_load_be16(td + 4) != 1u) {
                tc_set_error(TC_ERR_MALFORMED, "tpcD magic/version 不符");
                return TC_ERR_MALFORMED;
            }
            if (td[7] != 0u || td[6] > TC_TIER_RAW) {
                tc_set_error(TC_ERR_MALFORMED, "tpcD reserved/tier_id 域非法");
                return TC_ERR_MALFORMED;
            }
            uint16_t vlen = tc_load_be16(td + 8);
            uint16_t llen = tc_load_be16(td + 10);
            if ((uint64_t)12u + (uint64_t)vlen + (uint64_t)llen + 4ull != clen ||
                vlen > TPCD_VENDOR_MAX || llen > TPCD_LABEL_MAX) {
                tc_set_error(TC_ERR_MALFORMED, "tpcD 串域长度不一致");
                return TC_ERR_MALFORMED;
            }
            if (tc_crc32(td, (uint32_t)(clen - 4ull)) !=
                tc_load_be32(td + clen - 4ull)) {
                tc_set_error(TC_ERR_CHECKSUM_MISMATCH, "tpcD CRC 不符");
                return TC_ERR_CHECKSUM_MISMATCH;
            }
            if (st->has_tpcd) {
                tc_set_error(TC_ERR_MALFORMED, "重复 tpcD");
                return TC_ERR_MALFORMED;
            }
            memset(&mv->meta, 0, sizeof(mv->meta));
            mv->meta.tier_id = td[6];
            if (vlen != 0u) { memcpy(mv->meta.vendor, td + 12, vlen); }
            if (llen != 0u) { memcpy(mv->meta.label, td + 12 + (size_t)vlen, llen); }
            st->has_tpcd = 1;
            mv->meta_set = 1u;
        } else if (type_is(&c, "trwm")) {
            /* RC1：TRAW 开发元数据（载荷不透明；仅限长 + 原样入 mv）。 */
            uint64_t clen = c.size - (uint64_t)(c.ext ? 16 : 8);
            if (clen < 8ull || clen > (uint64_t)TC_RAW_META_MAX) {
                tc_set_error(TC_ERR_MALFORMED, "trwm 长度 %llu 越界 [8,%u]",
                             (unsigned long long)clen, TC_RAW_META_MAX);
                return TC_ERR_MALFORMED;
            }
            if (mv->rmeta_set != 0u) {
                tc_set_error(TC_ERR_MALFORMED, "trwm 原子重复");
                return TC_ERR_MALFORMED;
            }
            rc = io_read(mv, c.body, mv->rmeta.payload, (size_t)clen);
            if (rc != TC_OK) { return rc; }
            mv->rmeta.payload_size = (uint16_t)clen;
            mv->rmeta.reserved = 0u;
            mv->rmeta_set = 1u;
        }
        off += c.size;
    }
    if (!st->has_tpcc) {
        tc_set_error(TC_ERR_MALFORMED, "缺 tpcC");
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

static int32_t parse_stts(topos_movie* mv, uint64_t body, uint64_t end, stbl_state* st)
{
    uint32_t entry_count = 0;
    int32_t rc = read_entry_count(mv, body, end, &entry_count);
    if (rc != TC_OK) { return rc; }
    uint64_t data = body + 8ull;
    uint64_t dend = data + (uint64_t)entry_count * 8ull;
    if (dend > end) {
        tc_set_error(TC_ERR_MALFORMED, "stts 表越界");
        return TC_ERR_MALFORMED;
    }
    uint64_t samples = 0u;
    uint8_t blk[4096];
    uint64_t off = data;
    while (off < dend) {
        size_t chunk = (size_t)((dend - off) > 4096ull ? 4096ull : (dend - off));
        chunk -= chunk % 8u;
        rc = io_read(mv, off, blk, chunk);
        if (rc != TC_OK) { return rc; }
        for (size_t p = 0; p < chunk; p += 8u) {
            uint32_t cnt = tc_load_be32(blk + p);
            uint32_t dlt = tc_load_be32(blk + p + 4u);
            if (dlt == 0u) {
                tc_set_error(TC_ERR_MALFORMED, "stts delta=0");
                return TC_ERR_MALFORMED;
            }
            if (!tc_uadd_u64(samples, cnt, &samples)) {
                tc_set_error(TC_ERR_LIMIT_EXCEEDED, "stts 计数溢出");
                return TC_ERR_LIMIT_EXCEEDED;
            }
        }
        off += chunk;
    }
    st->stts_entry_count = entry_count;
    st->stts_off = data;
    st->stts_end = dend;
    st->sample_total = samples;
    return TC_OK;
}

static int32_t parse_stsz(topos_movie* mv, uint64_t body, uint64_t end, stbl_state* st)
{
    if (body > end || end - body < 12ull) {
        tc_set_error(TC_ERR_MALFORMED, "stsz 过短");
        return TC_ERR_MALFORMED;
    }
    uint8_t b[12];
    int32_t rc = io_read(mv, body, b, 12u);
    if (rc != TC_OK) { return rc; }
    st->stsz_sample_size = tc_load_be32(b + 4);
    st->stsz_count = tc_load_be32(b + 8);
    st->stsz_data_off = body + 12ull;
    st->stsz_data_end = st->stsz_data_off + (uint64_t)st->stsz_count * 4ull;
    if (st->stsz_sample_size == 0u && st->stsz_data_end > end) {
        tc_set_error(TC_ERR_MALFORMED, "stsz 表越界");
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

static int32_t parse_stsc(topos_movie* mv, uint64_t body, uint64_t end, stbl_state* st)
{
    uint32_t entry_count = 0;
    int32_t rc = read_entry_count(mv, body, end, &entry_count);
    if (rc != TC_OK) { return rc; }
    st->stsc_count = entry_count;
    st->stsc_off = body + 8ull;
    st->stsc_end = st->stsc_off + (uint64_t)entry_count * 12ull;
    if (st->stsc_end > end) {
        tc_set_error(TC_ERR_MALFORMED, "stsc 表越界");
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

static int32_t parse_stco(topos_movie* mv, uint64_t body, uint64_t end, int is_co64,
                          stbl_state* st)
{
    uint32_t entry_count = 0;
    int32_t rc = read_entry_count(mv, body, end, &entry_count);
    if (rc != TC_OK) { return rc; }
    uint32_t esz = is_co64 ? 8u : 4u;
    st->chunk_count = entry_count;
    st->stco_off = body + 8ull;
    st->stco_end = st->stco_off + (uint64_t)entry_count * (uint64_t)esz;
    if (st->stco_end > end) {
        tc_set_error(TC_ERR_MALFORMED, "stco/co64 表越界");
        return TC_ERR_MALFORMED;
    }
    st->is_co64 = is_co64;
    return TC_OK;
}

static int32_t parse_stss(topos_movie* mv, uint64_t body, uint64_t end, stbl_state* st)
{
    uint32_t entry_count = 0;
    int32_t rc = read_entry_count(mv, body, end, &entry_count);
    if (rc != TC_OK) { return rc; }
    st->has_stss = 1;
    st->stss_count = entry_count;
    st->stss_off = body + 8ull;
    st->stss_end = st->stss_off + (uint64_t)entry_count * 4ull;
    if (st->stss_end > end) {
        tc_set_error(TC_ERR_MALFORMED, "stss 表越界");
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

/* stsc × stco 展开 → 绝对 sample 偏移/尺寸（视频/音频轨共用，spec §7.5） */
static int32_t stbl_expand_sample_offsets(const topos_movie* mv, const stbl_state* st,
                                          uint32_t n, uint64_t* offsets, uint32_t* sizes)
{
    uint32_t chunk_i = 0u;
    uint32_t si = 0u;
    uint64_t coff = st->stco_off;
    uint32_t esz = st->is_co64 ? 8u : 4u;
    uint8_t blk[4096];
    size_t blk_use = 0u, blk_pos = 0u;
    int32_t rc;
    for (uint32_t e = 0; e < st->stsc_count && si < n; ++e) {
        uint8_t ent[12];
        rc = io_read(mv, st->stsc_off + (uint64_t)e * 12ull, ent, 12u);
        if (rc != TC_OK) { return rc; }
        uint32_t first_chunk = tc_load_be32(ent);
        uint32_t spc = tc_load_be32(ent + 4);
        uint32_t next_first = 0u;
        if (e + 1u < st->stsc_count) {
            uint8_t nent[12];
            rc = io_read(mv, st->stsc_off + (uint64_t)(e + 1u) * 12ull, nent, 12u);
            if (rc != TC_OK) { return rc; }
            next_first = tc_load_be32(nent);
            if (next_first <= first_chunk) {
                tc_set_error(TC_ERR_MALFORMED, "stsc first_chunk 非严格递增");
                return TC_ERR_MALFORMED;
            }
        }
        if (first_chunk < 1u || spc == 0u || first_chunk != chunk_i + 1u) {
            tc_set_error(TC_ERR_MALFORMED, "stsc 字段非法/首块不连续");
            return TC_ERR_MALFORMED;
        }
        uint64_t run_chunks = (e + 1u < st->stsc_count)
            ? (uint64_t)(next_first - first_chunk)
            : UINT64_MAX; /* 末 run：按 sample 数截断 */
        for (uint64_t ck = 0u; ck < run_chunks && si < n; ++ck) {
            if (chunk_i >= st->chunk_count) {
                tc_set_error(TC_ERR_MALFORMED, "stco 条目不足");
                return TC_ERR_MALFORMED;
            }
            if (blk_use == 0u || blk_pos >= blk_use) {
                if (coff >= st->stco_end) {
                    tc_set_error(TC_ERR_MALFORMED, "stco 数据不足");
                    return TC_ERR_MALFORMED;
                }
                size_t want = (size_t)((st->stco_end - coff) > 4096ull ? 4096ull : (st->stco_end - coff));
                want -= want % (size_t)esz;
                if (want == 0u) {
                    tc_set_error(TC_ERR_MALFORMED, "stco 残尾");
                    return TC_ERR_MALFORMED;
                }
                rc = io_read(mv, coff, blk, want);
                if (rc != TC_OK) { return rc; }
                blk_use = want;
                blk_pos = 0u;
                coff += want;
            }
            uint64_t base = st->is_co64
                ? tc_load_be64(blk + blk_pos)
                : (uint64_t)tc_load_be32(blk + blk_pos);
            blk_pos += esz;
            chunk_i++;
            uint64_t o2 = base;
            for (uint32_t k = 0; k < spc && si < n; ++k) {
                offsets[si] = o2;
                o2 += (uint64_t)sizes[si];
                si++;
            }
        }
    }
    if (si != n || chunk_i != st->chunk_count) {
        tc_set_error(TC_ERR_MALFORMED, "stsc/stco 展开与 sample 数不一致");
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

/* 复验 P1-12：每个 sample [offset, offset+size) 必须完整落在某个 mdat
 * payload 区间内（视频/音频轨共用；区间差防回绕次序与原实现一致） */
static int32_t check_samples_in_mdat(uint32_t n, const uint64_t* offsets,
                                     const uint32_t* sizes,
                                     const uint64_t* mdat_lo, const uint64_t* mdat_hi,
                                     int mdat_count, const char* label)
{
    for (uint32_t i = 0u; i < n; ++i) {
        int inside = 0;
        for (int r = 0; r < mdat_count; ++r) {
            /* 次序：先确认 offsets < hi 再做差——offsets ≥ hi 时区间差
             * u64 下溢成大数会误判 inside（case2 反例） */
            if (offsets[i] >= mdat_lo[r] && offsets[i] < mdat_hi[r] &&
                (uint64_t)sizes[i] <= mdat_hi[r] - offsets[i]) {
                inside = 1;
                break;
            }
        }
        if (inside == 0) {
            tc_set_error(TC_ERR_MALFORMED,
                         "%s sample %u [off=%llu, size=%u] 不在 mdat payload 内",
                         label, i, (unsigned long long)offsets[i], sizes[i]);
            return TC_ERR_MALFORMED;
        }
    }
    return TC_OK;
}

/* P1-11：fd 直读回调——tc_io_read_fn 形态，ctx = fd 值。
 * 完整性：循环 pread 消化短读（EINTR 重试）；EOF/错误一律 TC_ERR_IO。
 * 线程安全：pread 不共享文件偏移，movie 只读共享语义不变。 */
static int32_t fd_read_cb(void* ctx, uint64_t offset, void* buf, size_t len)
{
    const int fd = (int)(intptr_t)ctx;
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0u;
#if defined(_WIN32)
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { return TC_ERR_IO; }
    while (got < len) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        const uint64_t pos = offset + (uint64_t)got;
        ov.Offset = (DWORD)(pos & 0xFFFFFFFFu);
        ov.OffsetHigh = (DWORD)(pos >> 32);
        DWORD n = 0u;
        if (ReadFile(h, p + got, (DWORD)(len - got), &n, &ov) == FALSE) {
            return TC_ERR_IO;
        }
        if (n == 0u) { return TC_ERR_IO; } /* 短读/EOF */
        got += (size_t)n;
    }
#else
    while (got < len) {
        const ssize_t n = pread(fd, p + got, len - got, (off_t)(offset + got));
        if (n < 0) {
            if (errno == EINTR) { continue; }
            return TC_ERR_IO;
        }
        if (n == 0) { return TC_ERR_IO; } /* 短读/EOF */
        got += (size_t)n;
    }
#endif
    return TC_OK;
}

/* 预扫 trak 的 mdia>hdlr 处理器子类型（vide/soun/…）。未找到时 out 置 0。
 * 用于 moov 遍历分派：视频轨走主解析，soun 轨按 v1.1 音频轨处理。 */
static int32_t trak_handler_type(const topos_movie* mv, uint64_t trak_body,
                                 uint64_t trak_end, uint8_t out[4])
{
    memset(out, 0, 4u);
    uint64_t o = trak_body;
    while (o < trak_end) {
        atom_hdr c;
        int32_t rc = read_atom(mv, o, trak_end, &c);
        if (rc != TC_OK) { return rc; }
        if (type_is(&c, "mdia")) {
            uint64_t d = c.body, dend = o + c.size;
            while (d < dend) {
                atom_hdr m2;
                rc = read_atom(mv, d, dend, &m2);
                if (rc != TC_OK) { return rc; }
                if (type_is(&m2, "hdlr") && m2.size - (uint64_t)(m2.ext ? 16 : 8) >= 12ull) {
                    uint8_t b4[4];
                    rc = io_read(mv, m2.body + 8ull, b4, 4u);
                    if (rc != TC_OK) { return rc; }
                    memcpy(out, b4, 4u);
                    return TC_OK;
                }
                d += m2.size;
            }
        }
        o += c.size;
    }
    return TC_OK;
}

/* —— v1.1 音频轨解析 —— */

/* 音频 stsd 解析出的声样描述信息（与 mdhd timescale 交叉核对前暂存） */
typedef struct {
    uint32_t codec, channels, layout, bits;
    uint32_t format;      /* v1.4：TC_AUDIO_FMT_*（formatFlags float 位） */
    uint32_t stsd_rate;   /* 16.16 已右移 */
} audio_stsd_info;

/* esds 描述符头：ISO 14496-1 expandable 长度（tag 1B + 变长 7-bit 续位，
 * 至多 4 字节）。写侧见 buf_desc_len4（80 80 80 XX 规范形式）。 */
static int32_t read_desc_header(const topos_movie* mv, uint64_t off, uint64_t end,
                                uint8_t* tag, uint32_t* len, uint64_t* payload_off)
{
    if (off > end || end - off < 2ull) {
        tc_set_error(TC_ERR_MALFORMED, "esds 描述符头越界");
        return TC_ERR_MALFORMED;
    }
    uint8_t c = 0u;
    int32_t rc = io_read(mv, off, &c, 1u);
    if (rc != TC_OK) { return rc; }
    *tag = c;
    uint32_t v = 0u;
    uint64_t p = off + 1ull;
    for (int i = 0; i < 4; ++i) {
        if (p >= end) {
            tc_set_error(TC_ERR_MALFORMED, "esds 描述符长度截断");
            return TC_ERR_MALFORMED;
        }
        rc = io_read(mv, p, &c, 1u);
        if (rc != TC_OK) { return rc; }
        v = (v << 7) | (uint32_t)(c & 0x7Fu);
        ++p;
        if ((c & 0x80u) == 0u) { break; }
        if (i == 3) {
            tc_set_error(TC_ERR_MALFORMED, "esds 描述符长度续位超限");
            return TC_ERR_MALFORMED;
        }
    }
    *len = v;
    *payload_off = p;
    if (*len > end - *payload_off) {
        tc_set_error(TC_ERR_MALFORMED, "esds 描述符长度越界");
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

/* mp4a 的 esds：验证同级链 ES→DCD(AAC 0x40)→DSI(ASC) 并原样提取 ASC。
 * 严格按描述符长度推进（ISO 14496-1；与写侧 buf_desc_len4 同一规范），
 * 畸形链（嵌套错误/长度不符）→ TC_ERR_MALFORMED。 */
static int32_t parse_esds_asc(const topos_movie* mv, uint64_t body, uint64_t end,
                              uint8_t* asc_out, uint32_t* asc_size)
{
    if (body > end || end - body < 4ull) {
        tc_set_error(TC_ERR_MALFORMED, "esds 过短");
        return TC_ERR_MALFORMED;
    }
    uint64_t off = body + 4ull; /* version/flags */
    uint8_t tag = 0u;
    uint32_t len = 0u;
    uint64_t payload = 0u;
    int32_t rc = read_desc_header(mv, off, end, &tag, &len, &payload);
    if (rc != TC_OK) { return rc; }
    if (tag != 0x03u) {
        tc_set_error(TC_ERR_MALFORMED, "esds 缺 ES_Descriptor");
        return TC_ERR_MALFORMED;
    }
    if (len < 5ull) {
        tc_set_error(TC_ERR_MALFORMED, "ES_Descriptor 过短");
        return TC_ERR_MALFORMED;
    }
    const uint64_t es_end = payload + len;
    off = payload + 3ull; /* ES_ID(2) + flags(1) */
    rc = read_desc_header(mv, off, es_end, &tag, &len, &payload);
    if (rc != TC_OK) { return rc; }
    if (tag != 0x04u || len < 13ull) {
        tc_set_error(TC_ERR_MALFORMED, "esds 缺 DecoderConfigDescriptor");
        return TC_ERR_MALFORMED;
    }
    uint8_t obj = 0u;
    rc = io_read(mv, payload, &obj, 1u);
    if (rc != TC_OK) { return rc; }
    if (obj != 0x40u) {
        tc_set_error(TC_ERR_MALFORMED, "esds objectTypeIndication 非 AAC(0x40)");
        return TC_ERR_MALFORMED;
    }
    /* DCD 是叶子节点：DSI 按 DCD 声明长度跳到其兄弟位置 */
    off = payload + len;
    rc = read_desc_header(mv, off, es_end, &tag, &len, &payload);
    if (rc != TC_OK) { return rc; }
    if (tag != 0x05u || len == 0u || len > 64u) {
        tc_set_error(TC_ERR_MALFORMED, "esds DecSpecificInfo(ASC) 缺失或超长");
        return TC_ERR_MALFORMED;
    }
    rc = io_read(mv, payload, asc_out, len);
    if (rc != TC_OK) { return rc; }
    *asc_size = len;
    return TC_OK;
}

/* 音频 sample entry 子原子扫描（chan / wave>esds / 直挂 esds）；
 * V0/V1 与 V2 声样描述共用（V2 的起始偏移由调用方给出）。
 * 布局一致性校验（chan 推导/采样率白名单）由调用方完成。 */
static int32_t parse_audio_stsd_children(topos_movie* mv, audio_track_state* t,
                                         uint64_t ent_end, uint64_t off,
                                         audio_stsd_info* ai)
{
    int32_t rc = TC_OK;
    while (off < ent_end) {
        atom_hdr c;
        rc = read_atom(mv, off, ent_end, &c);
        if (rc != TC_OK) { return rc; }
        if (type_is(&c, "chan")) {
            uint64_t clen = c.size - (uint64_t)(c.ext ? 16 : 8);
            if (clen < 16ull) {
                tc_set_error(TC_ERR_MALFORMED, "chan 原子过短");
                return TC_ERR_MALFORMED;
            }
            uint8_t ch[16];
            rc = io_read(mv, c.body, ch, 16u);
            if (rc != TC_OK) { return rc; }
            const uint32_t chan_tag = tc_load_be32(ch + 4); /* 跳过 ver/flags */
            if (chan_tag == TC_AUDIO_CHAN_TAG_MONO) {
                ai->layout = TC_AUDIO_LAYOUT_MONO;
            } else if (chan_tag == TC_AUDIO_CHAN_TAG_STEREO) {
                ai->layout = TC_AUDIO_LAYOUT_STEREO;
            } else if (chan_tag == TC_AUDIO_CHAN_TAG_5_1_A) {
                ai->layout = TC_AUDIO_LAYOUT_5_1;
            } else if (chan_tag == TC_AUDIO_CHAN_TAG_7_1_A) {
                ai->layout = TC_AUDIO_LAYOUT_7_1;
            } else {
                tc_set_error(TC_ERR_MALFORMED,
                             "chan layout tag 0x%08X 不支持", chan_tag);
                return TC_ERR_MALFORMED;
            }
        } else if (type_is(&c, "wave")) {
            /* QuickTime 声样扩展包裹：esds 的规范位置（写侧 build_audio_stsd
             * 产出 wave{frma;'mp4a'12B;esds;null}；直挂 esds 亦容忍） */
            if (ai->codec != TC_AUDIO_CODEC_MP4A) {
                tc_set_error(TC_ERR_MALFORMED, "wave 仅允许出现在 mp4a entry");
                return TC_ERR_MALFORMED;
            }
            const uint64_t w_hdr = (uint64_t)(c.ext ? 16 : 8);
            if (c.size < w_hdr + 4ull) {
                tc_set_error(TC_ERR_MALFORMED, "wave 原子过短");
                return TC_ERR_MALFORMED;
            }
            uint64_t wo = c.body;
            const uint64_t w_end = c.body + c.size - w_hdr;
            while (wo < w_end) {
                atom_hdr w2;
                rc = read_atom(mv, wo, w_end, &w2);
                if (rc != TC_OK) { return rc; }
                if (type_is(&w2, "esds")) {
                    uint32_t sz32 = 0u;
                    rc = parse_esds_asc(mv, w2.body, wo + w2.size,
                                        t->asc, &sz32);
                    if (rc != TC_OK) { return rc; }
                    t->asc_size = sz32;
                }
                wo += w2.size;
            }
        } else if (type_is(&c, "esds")) {
            if (ai->codec != TC_AUDIO_CODEC_MP4A) {
                tc_set_error(TC_ERR_MALFORMED, "esds 仅允许出现在 mp4a entry");
                return TC_ERR_MALFORMED;
            }
            {
                uint32_t sz32 = 0u;
                rc = parse_esds_asc(mv, c.body, off + c.size,
                                    t->asc, &sz32);
                if (rc != TC_OK) { return rc; }
                t->asc_size = sz32;
            }
            if (rc != TC_OK) { return rc; }
        }
        off += c.size;
    }
    return TC_OK;
}

static int32_t parse_audio_stsd(topos_movie* mv, audio_track_state* t,
                                uint64_t body, uint64_t end,
                                audio_stsd_info* ai)
{
    uint32_t entry_count = 0;
    int32_t rc = read_entry_count(mv, body, end, &entry_count);
    if (rc != TC_OK) { return rc; }
    if (entry_count != 1u) {
        tc_set_error(TC_ERR_MALFORMED, "音频 stsd entry_count=%u ≠ 1", entry_count);
        return TC_ERR_MALFORMED;
    }
    atom_hdr ent;
    rc = read_atom(mv, body + 8ull, end, &ent);
    if (rc != TC_OK) { return rc; }
    const uint64_t hdr_len = (uint64_t)(ent.ext ? 16 : 8);
    if (ent.size < hdr_len + 36ull) {
        tc_set_error(TC_ERR_MALFORMED, "音频 sample entry 过短");
        return TC_ERR_MALFORMED;
    }
    uint8_t f[36];
    rc = io_read(mv, ent.body, f, 36u);
    if (rc != TC_OK) { return rc; }
    ai->codec = 0u;
    ai->bits = 0u;
    ai->format = TC_AUDIO_FMT_INT;
    ai->layout = 0xFFFFFFFFu; /* 未定：由 chan/声道数推导 */
    const uint64_t ent_end = ent.body + ent.size - hdr_len;
    uint64_t off = ent.body + 36ull;
    const uint16_t entry_version = tc_load_be16(f + 8);
    if (entry_version == 2u) {
        /* SoundDescriptionV2（v1.4：>65535Hz 采样率；sampleRate=float64）。
         * body 公共段 16B（reserved6+dri+ver+rev+vendor），V2 扩展 48B，
         * 子原子自 body+64 起（与写侧/ffmpeg movenc 同构） */
        if (ent.size < hdr_len + 64ull) {
            tc_set_error(TC_ERR_MALFORMED, "音频 V2 entry 过短");
            return TC_ERR_MALFORMED;
        }
        uint8_t v2[48];
        rc = io_read(mv, ent.body + 16ull, v2, 48u);
        if (rc != TC_OK) { return rc; }
        /* body 公共段 16B；v2 相对 body+16：always3/16/-2/0 @0..8、
         * soundRate 1.0 @8、sizeOfStructOnly @12、sampleRate f64 @16、
         * numChannels @24、always7F @28、constBits @32、
         * formatSpecificFlags @36、bytesPerPacket @40、framesPerPacket @44 */
        uint64_t sr_bits = tc_load_be64(v2 + 16);
        double sr = 0.0;
        memcpy(&sr, &sr_bits, sizeof(sr));
        if (!(sr > 0.0) || sr > 4.0e9) {
            tc_set_error(TC_ERR_MALFORMED, "V2 sampleRate 非法");
            return TC_ERR_MALFORMED;
        }
        ai->stsd_rate = (uint32_t)(sr + 0.5);
        ai->channels = tc_load_be32(v2 + 24);
        const uint32_t flags = tc_load_be32(v2 + 36);   /* formatSpecificFlags */
        const uint32_t const_bits = tc_load_be32(v2 + 32);
        if (type_is(&ent, "lpcm")) {
            if ((flags & 0x2u) == 0u) {
                tc_set_error(TC_ERR_MALFORMED, "lpcm 仅支持大端（'twos' 语义）");
                return TC_ERR_MALFORMED;
            }
            if (const_bits != 16u && const_bits != 24u && const_bits != 32u) {
                tc_set_error(TC_ERR_MALFORMED, "lpcm 位深非法（%u）", const_bits);
                return TC_ERR_MALFORMED;
            }
            ai->codec = TC_AUDIO_CODEC_LPCM;
            ai->bits = const_bits;
            ai->format = (flags & 0x1u) ? TC_AUDIO_FMT_FLOAT32 : TC_AUDIO_FMT_INT;
            if (ai->format == TC_AUDIO_FMT_FLOAT32 && const_bits != 32u) {
                tc_set_error(TC_ERR_MALFORMED, "lpcm float 仅 32bit 合法");
                return TC_ERR_MALFORMED;
            }
        } else if (type_is(&ent, "mp4a")) {
            ai->codec = TC_AUDIO_CODEC_MP4A;
            ai->bits = 0u;
        } else {
            tc_set_error(TC_ERR_MALFORMED, "音频 V2 entry=%c%c%c%c 不支持",
                         ent.type[0], ent.type[1], ent.type[2], ent.type[3]);
            return TC_ERR_MALFORMED;
        }
        /* 子原子（wave/esds/chan）自 68B 起（V2 无 V0/V1 附加扩展） */
        return parse_audio_stsd_children(mv, t, ent_end,
                                         ent.body + 64ull, ai);
    }
    ai->channels = tc_load_be16(f + 16);
    ai->stsd_rate = tc_load_be32(f + 24) >> 16;
    if (type_is(&ent, "lpcm")) {
        if (ent.size < hdr_len + 48ull) {
            tc_set_error(TC_ERR_MALFORMED, "lpcm 缺 20B 扩展");
            return TC_ERR_MALFORMED;
        }
        uint8_t x[20];
        rc = io_read(mv, ent.body + 28ull, x, 20u);
        if (rc != TC_OK) { return rc; }
        const uint32_t flags = tc_load_be32(x);
        const uint32_t const_bits = tc_load_be32(x + 4);
        if ((flags & 0x2u) == 0u) {
            tc_set_error(TC_ERR_MALFORMED, "lpcm 仅支持大端（'twos' 语义）");
            return TC_ERR_MALFORMED;
        }
        if (const_bits != 16u && const_bits != 24u && const_bits != 32u) {
            tc_set_error(TC_ERR_MALFORMED, "lpcm 位深非法（%u）", const_bits);
            return TC_ERR_MALFORMED;
        }
        /* v1.4：float 位（kAudioFormatFlagIsFloat=0x1）→ sample_format；
         * 32bit 非 float = 有符号整数（0xE）；float 恒 32bit（0xB 形态） */
        ai->format = (flags & 0x1u) ? TC_AUDIO_FMT_FLOAT32 : TC_AUDIO_FMT_INT;
        if (ai->format == TC_AUDIO_FMT_FLOAT32 && const_bits != 32u) {
            tc_set_error(TC_ERR_MALFORMED, "lpcm float 仅 32bit 合法");
            return TC_ERR_MALFORMED;
        }
        off = ent.body + 48ull;
        ai->codec = TC_AUDIO_CODEC_LPCM;
        ai->bits = const_bits;
    } else if (type_is(&ent, "mp4a")) {
        ai->codec = TC_AUDIO_CODEC_MP4A;
        /* version 1 携带 16B 扩展（samplesPerPacket 等）；version 0 无扩展 */
        const uint16_t version = tc_load_be16(f + 8);
        if (version == 1u) {
            if (ent.size < hdr_len + 44ull) {
                tc_set_error(TC_ERR_MALFORMED, "mp4a v1 缺扩展");
                return TC_ERR_MALFORMED;
            }
            off = ent.body + 44ull;
        } else if (version != 0u) {
            tc_set_error(TC_ERR_MALFORMED, "mp4a version %u 不支持", version);
            return TC_ERR_MALFORMED;
        }
    } else {
        tc_set_error(TC_ERR_MALFORMED, "音频 entry=%c%c%c%c 不支持（仅 lpcm/mp4a）",
                     ent.type[0], ent.type[1], ent.type[2], ent.type[3]);
        return TC_ERR_MALFORMED;
    }

    rc = parse_audio_stsd_children(mv, t, ent_end, off, ai);
    if (rc != TC_OK) { return rc; }
    /* 布局一致性：chan 缺失时由声道数推导 */
    if (ai->layout == 0xFFFFFFFFu) {
        ai->layout = (ai->channels == 1u) ? TC_AUDIO_LAYOUT_MONO
                   : (ai->channels == 2u) ? TC_AUDIO_LAYOUT_STEREO
                   : (ai->channels == 6u) ? TC_AUDIO_LAYOUT_5_1
                   : (ai->channels == 8u) ? TC_AUDIO_LAYOUT_7_1
                   : 0xFFFFFFFFu;
        if (ai->layout == 0xFFFFFFFFu) {
            tc_set_error(TC_ERR_MALFORMED, "音频声道数非法（%u）", ai->channels);
            return TC_ERR_MALFORMED;
        }
    }
    /* v1.4：全家族采样率（与写侧白名单一致） */
    if (ai->stsd_rate != 44100u && ai->stsd_rate != 48000u &&
        ai->stsd_rate != 88200u && ai->stsd_rate != 96000u &&
        ai->stsd_rate != 176400u && ai->stsd_rate != 192000u) {
        tc_set_error(TC_ERR_MALFORMED, "音频采样率非法（%u）", ai->stsd_rate);
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

/* v1.5 tmcd trak 解析（fail-fast）：stsd 'tmcd' 语义字段 + 单样本 4B。
 * 畸形（entry≠'tmcd'、样本≠4B、stts 多条目、fps 越界、DF/fps 不匹配）
 * → TC_ERR_MALFORMED。 */
static int32_t parse_tmcd_trak(topos_movie* mv, uint64_t trak_body,
                               uint64_t trak_end)
{
    uint64_t o = trak_body;
    int have_mdhd = 0, have_stbl = 0, have_hdlr = 0;
    int have_stsd = 0, have_stts = 0, have_stsz = 0, have_stco = 0;
    stbl_state st;
    memset(&st, 0, sizeof(st));
    int32_t rc = TC_OK;
    while (o < trak_end) {
        atom_hdr c;
        rc = read_atom(mv, o, trak_end, &c);
        if (rc != TC_OK) { return rc; }
        if (type_is(&c, "mdia")) {
            uint64_t d = c.body, dend = o + c.size;
            while (d < dend) {
                atom_hdr m2;
                rc = read_atom(mv, d, dend, &m2);
                if (rc != TC_OK) { return rc; }
                if (type_is(&m2, "mdhd")) {
                    if (m2.size < 32ull) {
                        tc_set_error(TC_ERR_MALFORMED, "tmcd mdhd 过短");
                        return TC_ERR_MALFORMED;
                    }
                    have_mdhd = 1;
                } else if (type_is(&m2, "hdlr")) {
                    uint8_t b4[4];
                    rc = io_read(mv, m2.body + 8ull, b4, 4u);
                    if (rc != TC_OK) { return rc; }
                    if (memcmp(b4, "tmcd", 4u) != 0) {
                        tc_set_error(TC_ERR_MALFORMED, "tmcd trak hdlr 非法");
                        return TC_ERR_MALFORMED;
                    }
                    have_hdlr = 1;
                } else if (type_is(&m2, "minf")) {
                    uint64_t q = m2.body, qend = d + m2.size;
                    while (q < qend) {
                        atom_hdr q2;
                        rc = read_atom(mv, q, qend, &q2);
                        if (rc != TC_OK) { return rc; }
                        if (type_is(&q2, "stbl")) {
                            have_stbl = 1;
                            uint64_t s2 = q2.body, send = q + q2.size;
                            while (s2 < send) {
                                atom_hdr sc;
                                rc = read_atom(mv, s2, send, &sc);
                                if (rc != TC_OK) { return rc; }
                                if (type_is(&sc, "stsd")) {
                                    /* entry：size4 'tmcd'4 res6 dri2 保留4
                                     * flags4 ts4 fd4 fps1 pad3（oracle 同构） */
                                    uint8_t eh[8];
                                    rc = io_read(mv, sc.body + 8ull, eh, 8u);
                                    if (rc != TC_OK) { return rc; }
                                    const uint32_t esize = tc_load_be32(eh);
                                    /* 最小 36B = 8 头 + 28 载荷 */
                                    if (esize < 36ull) {
                                        tc_set_error(TC_ERR_MALFORMED,
                                                     "tmcd stsd entry 过短");
                                        return TC_ERR_MALFORMED;
                                    }
                                    uint8_t ty[4];
                                    rc = io_read(mv, sc.body + 12ull, ty, 4u);
                                    if (rc != TC_OK) { return rc; }
                                    if (memcmp(ty, "tmcd", 4u) != 0) {
                                        tc_set_error(TC_ERR_MALFORMED,
                                                     "tmcd stsd entry 类型非法");
                                        return TC_ERR_MALFORMED;
                                    }
                                    have_stsd = 1;
                                    uint8_t core[20];
                                    rc = io_read(mv, sc.body + 28ull, core, 20u);
                                    if (rc != TC_OK) { return rc; }
                                    /* core[0..3]=flags、[4..7]=ts、[8..11]=fd、
                                     * [12]=fps（sc.body+28/32/36/40；条目含
                                     * oracle 追加的 dri 后保留 u32） */
                                    mv->tc_flags = tc_load_be32(core);
                                    mv->tc_fps = core[12];
                                } else if (type_is(&sc, "stts")) {
                                    have_stts = 1;
                                    uint8_t tb[8];
                                    rc = io_read(mv, sc.body, tb, 8u);
                                    if (rc != TC_OK) { return rc; }
                                    if (tc_load_be32(tb + 4) != 1u) {
                                        tc_set_error(TC_ERR_MALFORMED,
                                                     "tmcd stts 条目数 ≠ 1");
                                        return TC_ERR_MALFORMED;
                                    }
                                    uint8_t td[8];
                                    rc = io_read(mv, sc.body + 8ull, td, 8u);
                                    if (rc != TC_OK) { return rc; }
                                    mv->tc_frame_count = tc_load_be32(td);
                                } else if (type_is(&sc, "stsz")) {
                                    have_stsz = 1;
                                    uint8_t zb[12];
                                    rc = io_read(mv, sc.body, zb, 12u);
                                    if (rc != TC_OK) { return rc; }
                                    if (tc_load_be32(zb + 4) != 4u ||
                                        tc_load_be32(zb + 8) != 1u) {
                                        tc_set_error(TC_ERR_MALFORMED,
                                                     "tmcd 样本须恰 1×4B");
                                        return TC_ERR_MALFORMED;
                                    }
                                } else if (type_is(&sc, "stco")) {
                                    have_stco = 1;
                                    uint8_t cb[12];
                                    rc = io_read(mv, sc.body, cb, 12u);
                                    if (rc != TC_OK) { return rc; }
                                    if (tc_load_be32(cb + 4) != 1u) {
                                        tc_set_error(TC_ERR_MALFORMED,
                                                     "tmcd stco 条目数 ≠ 1");
                                        return TC_ERR_MALFORMED;
                                    }
                                    const uint64_t soff = tc_load_be32(cb + 8);
                                    uint8_t sm[4];
                                    rc = io_read(mv, soff, sm, 4u);
                                    if (rc != TC_OK) { return rc; }
                                    mv->tc_start_count = tc_load_be32(sm);
                                } else if (type_is(&sc, "co64")) {
                                    /* >4GB 外部文件的合法变体（本库 mux 恒
                                     * 写 stco；读侧按规范 fail-fast 精神接纳） */
                                    have_stco = 1;
                                    uint8_t cb[16];
                                    rc = io_read(mv, sc.body, cb, 16u);
                                    if (rc != TC_OK) { return rc; }
                                    if (tc_load_be32(cb + 4) != 1u) {
                                        tc_set_error(TC_ERR_MALFORMED,
                                                     "tmcd co64 条目数 ≠ 1");
                                        return TC_ERR_MALFORMED;
                                    }
                                    const uint64_t soff = tc_load_be64(cb + 8);
                                    uint8_t sm[4];
                                    rc = io_read(mv, soff, sm, 4u);
                                    if (rc != TC_OK) { return rc; }
                                    mv->tc_start_count = tc_load_be32(sm);
                                } else if (type_is(&sc, "stsc")) {
                                    /* 合法形态，不消费 */
                                }
                                s2 += sc.size;
                            }
                        }
                        q += q2.size;
                    }
                }
                d += m2.size;
            }
        } else if (type_is(&c, "tkhd")) {
            /* tkhd v0: ver/flags(4) cre(4) mod(4) id(4) res(4) dur(4) → dur @body+20 */
            if (c.size >= (uint64_t)(c.ext ? 16 : 8) + 24ull) {
                uint8_t tb2[4];
                rc = io_read(mv, c.body + 20ull, tb2, 4u);
                if (rc == TC_OK) { mv->tc_movie_dur = tc_load_be32(tb2); }
            }
        }
        o += c.size;
    }
    if (!have_mdhd || !have_stbl || !have_hdlr) {
        tc_set_error(TC_ERR_MALFORMED, "tmcd trak 缺 mdhd/hdlr/stbl");
        return TC_ERR_MALFORMED;
    }
    /* 规范 A.6/D4：stsd/stts/stsz/stco 四表齐备且形态合法——缺失即畸形
     * （此前缺 stco 静默落到 start_count=0 + TC_OK，违背 fail-fast 承诺） */
    if (!have_stsd || !have_stts || !have_stsz || !have_stco) {
        tc_set_error(TC_ERR_MALFORMED,
                     "tmcd stbl 缺 stsd/stts/stsz/stco（之一）");
        return TC_ERR_MALFORMED;
    }
    /* fps 白名单 + DF/fps 一致性（与写侧同一规则） */
    if (mv->tc_fps != 24u && mv->tc_fps != 25u && mv->tc_fps != 30u &&
        mv->tc_fps != 48u && mv->tc_fps != 50u && mv->tc_fps != 60u) {
        tc_set_error(TC_ERR_MALFORMED, "tmcd fps 白名单外（%u）", mv->tc_fps);
        return TC_ERR_MALFORMED;
    }
    mv->tc_df = (mv->tc_flags & 0x1u) ? 1u : 0u;
    if (mv->tc_df != 0u && mv->tc_fps != 30u && mv->tc_fps != 60u) {
        tc_set_error(TC_ERR_MALFORMED, "tmcd DF 仅标称 30/60 合法");
        return TC_ERR_MALFORMED;
    }
    mv->tc_set = 1u;
    return TC_OK;
}

/* 音频 trak 表级解析（v1.1）：mdhd 采样率 + stbl 五表 + 一致性 + mdat 归属 */
static int32_t parse_audio_trak(topos_movie* mv, uint32_t slot,
                                uint64_t trak_body, uint64_t trak_end)
{
    audio_track_state* t = &mv->a_trk[slot];
    stbl_state st;
    memset(&st, 0, sizeof(st));
    audio_stsd_info ai;
    memset(&ai, 0, sizeof(ai));
    int have_stbl = 0;
    int have_edts = 0;
    int32_t rc;
    uint64_t o = trak_body;
    while (o < trak_end) {
        atom_hdr c;
        rc = read_atom(mv, o, trak_end, &c);
        if (rc != TC_OK) { return rc; }
        if (type_is(&c, "udta")) {
            /* v1.6：轨名 trak/udta/©nam（文本 = u16 长度 + UTF-8） */
            uint64_t uo = c.body, u_end = o + c.size;
            while (uo < u_end) {
                atom_hdr u;
                rc = read_atom(mv, uo, u_end, &u);
                if (rc != TC_OK) { return rc; }
                if (u.type[0] == 0xA9u && memcmp(u.type + 1, "nam", 3u) == 0) {
                    uint8_t hdr[2];
                    rc = io_read(mv, u.body, hdr, 2u);
                    if (rc != TC_OK) { return rc; }
                    uint32_t len = tc_load_be16(hdr);
                    if (len > sizeof(t->name) - 1u) { len = sizeof(t->name) - 1u; }
                    /* 文本域 = atom body − 2B 长度头；越界按无名处理
                     * （fail-safe：轨名是装饰性元数据，不拒文件） */
                    if ((uint64_t)len + 2ull > u.size - 8ull) { len = 0u; }
                    if (len != 0u) {
                        rc = io_read(mv, u.body + 2ull, t->name, len);
                        if (rc != TC_OK) { return rc; }
                        t->name[len] = '\0';
                    }
                }
                uo += u.size;
            }
        } else if (type_is(&c, "edts")) {
            /* v1.4：音频 elst 最小子集（AAC priming）。约束：单个 edts 内
             * 恰一个 elst、version/flags=0、entry_count=1、media_time>0、
             * media_rate==1.0；media_time < 总采样数在表解析后校验
             * （总计采样数届时已知）。视频 trak 出现 edts 在主解析拒绝。 */
            if (have_edts) {
                tc_set_error(TC_ERR_MALFORMED, "重复音频 edts");
                return TC_ERR_MALFORMED;
            }
            have_edts = 1;
            const uint64_t cend = o + c.size;
            uint64_t e = c.body;
            atom_hdr el;
            if (e >= cend) {
                tc_set_error(TC_ERR_MALFORMED, "edts 空");
                return TC_ERR_MALFORMED;
            }
            rc = read_atom(mv, e, cend, &el);
            if (rc != TC_OK) { return rc; }
            if (!type_is(&el, "elst")) {
                tc_set_error(TC_ERR_MALFORMED, "edts 内缺 elst");
                return TC_ERR_MALFORMED;
            }
            const uint64_t el_hdr = (uint64_t)(el.ext ? 16 : 8);
            if (el.size < el_hdr + 20ull) {
                tc_set_error(TC_ERR_MALFORMED, "elst 过短");
                return TC_ERR_MALFORMED;
            }
            uint8_t t8[20];
            rc = io_read(mv, el.body, t8, 20u);
            if (rc != TC_OK) { return rc; }
            if (tc_load_be32(t8) != 0u) {
                tc_set_error(TC_ERR_MALFORMED, "elst version/flags 非法（仅 v0）");
                return TC_ERR_MALFORMED;
            }
            if (tc_load_be32(t8 + 4) != 1u) {
                tc_set_error(TC_ERR_MALFORMED, "elst entry_count ≠ 1");
                return TC_ERR_MALFORMED;
            }
            /* t8+8: segment_duration（movie timescale；读侧不消费） */
            const uint32_t media_time = tc_load_be32(t8 + 12);
            if ((int32_t)media_time <= 0) {
                tc_set_error(TC_ERR_MALFORMED, "elst media_time 须 > 0");
                return TC_ERR_MALFORMED;
            }
            if (tc_load_be32(t8 + 16) != 0x00010000u) {
                tc_set_error(TC_ERR_MALFORMED, "elst media_rate ≠ 1.0");
                return TC_ERR_MALFORMED;
            }
            if (e + el.size != cend) {
                tc_set_error(TC_ERR_MALFORMED, "edts 含 elst 之外原子");
                return TC_ERR_MALFORMED;
            }
            t->priming = media_time;
        } else if (type_is(&c, "mdia")) {
            uint64_t d = c.body, dend = o + c.size;
            while (d < dend) {
                atom_hdr m2;
                rc = read_atom(mv, d, dend, &m2);
                if (rc != TC_OK) { return rc; }
                if (type_is(&m2, "mdhd")) {
                    if (m2.size < 32ull) {
                        tc_set_error(TC_ERR_MALFORMED, "音频 mdhd 过短");
                        return TC_ERR_MALFORMED;
                    }
                    uint8_t b[8];
                    rc = io_read(mv, m2.body + 12ull, b, 8u);
                    if (rc != TC_OK) { return rc; }
                    t->rate = tc_load_be32(b);
                    if (t->rate == 0u) {
                        tc_set_error(TC_ERR_MALFORMED, "音频 timescale=0");
                        return TC_ERR_MALFORMED;
                    }
                } else if (type_is(&m2, "minf")) {
                    have_stbl = 1;
                    uint64_t q = m2.body, qend = d + m2.size;
                    while (q < qend) {
                        atom_hdr q2;
                        rc = read_atom(mv, q, qend, &q2);
                        if (rc != TC_OK) { return rc; }
                        if (type_is(&q2, "stbl")) {
                            uint64_t s = q2.body, send = q + q2.size;
                            while (s < send) {
                                atom_hdr sc;
                                rc = read_atom(mv, s, send, &sc);
                                if (rc != TC_OK) { return rc; }
                                int seen = 0;
                                if (type_is(&sc, "stsd")) {
                                    seen = st.has_stsd; st.has_stsd = 1;
                                    if (!seen) {
                                        rc = parse_audio_stsd(mv, t, sc.body, s + sc.size, &ai);
                                        if (rc == TC_OK) {
                                            t->codec = ai.codec;
                                            t->channels = ai.channels;
                                            t->layout = ai.layout;
                                            t->bits = ai.bits;
    t->format = ai.format;
                                        }
                                    }
                                } else if (type_is(&sc, "stts")) {
                                    seen = st.has_stts; st.has_stts = 1;
                                    if (!seen) { rc = parse_stts(mv, sc.body, s + sc.size, &st); }
                                } else if (type_is(&sc, "stsz")) {
                                    seen = st.has_stsz; st.has_stsz = 1;
                                    if (!seen) { rc = parse_stsz(mv, sc.body, s + sc.size, &st); }
                                } else if (type_is(&sc, "stsc")) {
                                    seen = st.has_stsc; st.has_stsc = 1;
                                    if (!seen) { rc = parse_stsc(mv, sc.body, s + sc.size, &st); }
                                } else if (type_is(&sc, "stco") || type_is(&sc, "co64")) {
                                    seen = st.has_stco; st.has_stco = 1;
                                    if (!seen) {
                                        rc = parse_stco(mv, sc.body, s + sc.size,
                                                        type_is(&sc, "co64") ? 1 : 0, &st);
                                    }
                                }
                                if (seen) {
                                    tc_set_error(TC_ERR_MALFORMED,
                                                 "音频轨重复 singleton 表 atom（%c%c%c%c）",
                                                 sc.type[0], sc.type[1], sc.type[2], sc.type[3]);
                                    return TC_ERR_MALFORMED;
                                }
                                if (rc != TC_OK) { return rc; }
                                s += sc.size;
                            }
                        }
                        q += q2.size;
                    }
                }
                d += m2.size;
            }
        }
        o += c.size;
    }
    if (!have_stbl || !st.has_stsd || !st.has_stts || !st.has_stsz ||
        !st.has_stsc || !st.has_stco) {
        tc_set_error(TC_ERR_MALFORMED, "音频轨缺 stbl/stsd/stts/stsz/stsc/stco");
        return TC_ERR_MALFORMED;
    }
    /* 一致性（与视频轨同规则） */
    if (st.sample_total == 0ull) {
        tc_set_error(TC_ERR_MALFORMED, "音频 stts 空");
        return TC_ERR_MALFORMED;
    }
    if (st.sample_total > (uint64_t)TC_MOVIE_MAX_SAMPLES) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "音频 chunk 数超上限");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    if ((uint64_t)st.stsz_count != st.sample_total) {
        tc_set_error(TC_ERR_MALFORMED, "音频 stsz 计数 ≠ stts 展开");
        return TC_ERR_MALFORMED;
    }
    if (ai.codec == TC_AUDIO_CODEC_NONE) {
        tc_set_error(TC_ERR_MALFORMED, "音频 stsd 未识别 codec");
        return TC_ERR_MALFORMED;
    }
    if (t->rate != ai.stsd_rate) {
        tc_set_error(TC_ERR_MALFORMED,
                     "音频 mdhd timescale(%u) ≠ stsd sampleRate(%u)",
                     t->rate, ai.stsd_rate);
        return TC_ERR_MALFORMED;
    }
    const uint32_t an = (uint32_t)st.sample_total;
    t->offsets = (uint64_t*)tc_alloc((size_t)an * sizeof(uint64_t));
    t->sizes = (uint32_t*)tc_alloc((size_t)an * sizeof(uint32_t));
    t->durs = (uint32_t*)tc_alloc((size_t)an * sizeof(uint32_t));
    if (t->offsets == NULL || t->sizes == NULL || t->durs == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "音频索引分配");
        return TC_ERR_OUT_OF_MEMORY;
    }
    /* stsz → a_sizes */
    if (st.stsz_sample_size != 0u) {
        for (uint32_t i = 0; i < an; ++i) { t->sizes[i] = st.stsz_sample_size; }
    } else {
        uint8_t blk[4096];
        uint64_t tp = st.stsz_data_off;
        uint32_t si = 0u;
        while (si < an) {
            if (tp >= st.stsz_data_end) {
                tc_set_error(TC_ERR_MALFORMED, "音频 stsz 数据不足");
                return TC_ERR_MALFORMED;
            }
            size_t chunk = (size_t)((st.stsz_data_end - tp) > 4096ull ? 4096ull : (st.stsz_data_end - tp));
            chunk -= chunk % 4u;
            if (chunk == 0u) {
                tc_set_error(TC_ERR_MALFORMED, "音频 stsz 残尾");
                return TC_ERR_MALFORMED;
            }
            rc = io_read(mv, tp, blk, chunk);
            if (rc != TC_OK) { return rc; }
            for (size_t p = 0; p < chunk && si < an; p += 4u) {
                t->sizes[si++] = tc_load_be32(blk + p);
            }
            tp += chunk;
        }
        for (uint32_t i = 0; i < an; ++i) {
            if (t->sizes[i] == 0u) {
                tc_set_error(TC_ERR_MALFORMED, "音频 stsz size=0");
                return TC_ERR_MALFORMED;
            }
        }
    }
    /* 偏移展开 + mdat 归属 */
    rc = stbl_expand_sample_offsets(mv, &st, an, t->offsets, t->sizes);
    if (rc != TC_OK) { return rc; }
    /* mdat 区间在 tc_movie_open 顶层 walk 收集；此处经 mv 不可得——由调用方
     * （tc_movie_open）在展开后统一核对，见下方 audio_contain 延后标志。 */
    /* stts → a_durs / a_total_frames */
    {
        uint8_t blk[4096];
        uint64_t tp = st.stts_off;
        uint32_t si = 0u;
        uint64_t total = 0u;
        while (si < an) {
            if (tp >= st.stts_end) {
                tc_set_error(TC_ERR_MALFORMED, "音频 stts 展开不足");
                return TC_ERR_MALFORMED;
            }
            size_t chunk = (size_t)((st.stts_end - tp) > 4096ull ? 4096ull : (st.stts_end - tp));
            chunk -= chunk % 8u;
            if (chunk == 0u) {
                tc_set_error(TC_ERR_MALFORMED, "音频 stts 残尾");
                return TC_ERR_MALFORMED;
            }
            rc = io_read(mv, tp, blk, chunk);
            if (rc != TC_OK) { return rc; }
            for (size_t p = 0; p < chunk && si < an; p += 8u) {
                uint32_t cnt = tc_load_be32(blk + p);
                uint32_t dlt = tc_load_be32(blk + p + 4u);
                for (uint32_t k = 0; k < cnt && si < an; ++k) {
                    t->durs[si++] = dlt;
                    total += dlt;
                }
            }
            tp += chunk;
        }
        t->total_frames = total;
        /* v1.4：elst media_time < 轨总采样数（total 此时方为已知） */
        if (t->priming != 0u && (uint64_t)t->priming >= total) {
            tc_set_error(TC_ERR_MALFORMED, "elst media_time ≥ 轨总采样数");
            return TC_ERR_MALFORMED;
        }
    }
    t->n = an;
    return TC_OK;
}

int32_t tc_movie_open_fd(int fd, uint64_t length, topos_movie** out)
{
    if (fd < 0 || out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "fd < 0 或 out NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    topos_io io;
    memset(&io, 0, sizeof(io));
    io.struct_size = (uint32_t)sizeof(io);
    io.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    io.ctx = (void*)(intptr_t)fd;
    io.read = fd_read_cb;
    io.length = length;
    return tc_movie_open(&io, out);
}

int32_t tc_movie_open(const topos_io* io, topos_movie** out)
{
    int32_t rc = TC_ERR_MALFORMED;
    if (io == NULL || out == NULL || io->read == NULL ||
        io->struct_size != (uint32_t)sizeof(topos_io) ||
        io->abi_version != TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "io NULL/镜像不符/缺 read");
        return TC_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (io->length < 20ull) {
        tc_set_error(TC_ERR_TRUNCATED, "文件过短");
        return TC_ERR_TRUNCATED;
    }
    topos_movie* mv = (topos_movie*)tc_calloc(1u, sizeof(topos_movie));
    if (mv == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "movie alloc");
        return TC_ERR_OUT_OF_MEMORY;
    }
    mv->io = *io;

    /* 顶层 walk */
    uint64_t moov_off = UINT64_MAX, moov_end = 0, moov_body = 0, mdat_off = UINT64_MAX;
    uint64_t off = 0;
    int found_ftyp = 0;
    /* 复验 P1-12：mdat payload 区间（复验要求 sample 必须落在 mdat 内；
     * 容器规范布局恒单 mdat，多 mdat 上限 8 防表滥用） */
    uint64_t mdat_lo[8], mdat_hi[8];
    int mdat_count = 0;
    /* 截断 mdat 的声明终点（containment 按声明域判：越界读取由单帧
     * io-read 失败隔离，spec §7.2a/§7.6；0 = 未截断/不可得 */
    uint64_t trunc_declared_end = 0u;
    int trunc_mdat = 0;
    while (off + 8ull <= io->length) {
        atom_hdr h;
        rc = read_atom(mv, off, io->length, &h);
        if (rc != TC_OK) {
            /* 截断 mdat 收敛（spec §7.2a）：声明长度越过文件尾（未完成下载/写入）
             * → 按实际剩余长度继续；其余 atom 越界仍 MALFORMED。 */
            uint8_t hdr8[8];
            if (io_read(mv, off, hdr8, 8u) == TC_OK &&
                memcmp(hdr8 + 4, "mdat", 4u) == 0) {
                h.size = io->length - off;
                h.ext = 0;
                h.body = off + 8ull;
                /* 声明终点（ext 头可读时取声明的 u64 长度） */
                trunc_mdat = 1;
                if (tc_load_be32(hdr8) == 1u) {
                    uint8_t ext8[8];
                    if (io_read(mv, off + 8ull, ext8, 8u) == TC_OK) {
                        uint64_t declared = tc_load_be64(ext8);
                        /* P1-18：checked add 防回绕。声明终点允许越过文件尾
                         * （spec §7.2a 截断收敛：样本归属按声明域判，越界
                         * 读取由单帧 io 失败隔离）；仅近 UINT64_MAX 的声明
                         * （加法会回绕）钳到 EOF。 */
                        uint64_t adv;
                        if (declared >= 16ull && tc_uadd_u64(off, declared, &adv)) {
                            trunc_declared_end = adv;
                        }
                    }
                }
                if (trunc_declared_end == 0u) {
                    trunc_declared_end = io->length;
                }
            } else {
                goto fail;
            }
        }
        if (type_is(&h, "ftyp")) { found_ftyp = 1; }
        else if (type_is(&h, "moov")) {
            if (moov_off != UINT64_MAX) {
                rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "重复 moov");
                goto fail;
            }
            moov_off = off;
            moov_end = off + h.size;
            moov_body = h.body;
        } else if (type_is(&h, "mdat")) {
            mdat_off = off;
            /* P1-18：超上限明确拒绝（旧实现第 9 个 mdat 静默忽略——
             * 样本归属判定可能漏域） */
            if (mdat_count >= 8) {
                rc = TC_ERR_MALFORMED;
                tc_set_error(TC_ERR_MALFORMED, "mdat 数量超上限（>8）");
                goto fail;
            }
            mdat_lo[mdat_count] = h.body;
            mdat_hi[mdat_count] = trunc_mdat ? trunc_declared_end
                                             : (off + h.size);
            mdat_count++;
        }
        trunc_mdat = 0;
        trunc_declared_end = 0u;
        off += h.size;
    }
    if (off != io->length) {
        rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "顶层原子不闭合");
        goto fail;
    }
    if (!found_ftyp || moov_off == UINT64_MAX) {
        rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "缺 ftyp 或 moov");
        goto fail;
    }
    /* 复验 P1-12（最小复现 6）：mdat 必须存在——mdat FourCC 改 JUNK 的
     * 文件此前仍可 open/读包，与 container spec 的数据归属规则不一致 */
    if (mdat_count == 0) {
        rc = TC_ERR_MALFORMED;
        tc_set_error(TC_ERR_MALFORMED, "缺 mdat（无媒体数据容器）");
        goto fail;
    }
    mv->faststart = (mdat_off != UINT64_MAX && moov_off < mdat_off) ? 1 : 0;

    /* moov > trak > mdia > minf > stbl（固定深度）。
     * v1.1：音频 trak（hdlr='soun'）与视频 trak 分派处理——M-A1 先只识别
     * 并跳过（视频解码结果不受影响），表级解析与查询 API 在 M-A2 接入。 */
    stbl_state st;
    memset(&st, 0, sizeof(st));
    {
        int have_trak = 0, have_vide = 0, have_stbl = 0;
        uint64_t o = moov_body; /* 子 atom 从 body 起，非 moov 自身头部 */
        while (o < moov_end) {
            atom_hdr moov_c;
            rc = read_atom(mv, o, moov_end, &moov_c);
            if (rc != TC_OK) { goto fail; }
                if (!type_is(&moov_c, "trak")) { o += moov_c.size; continue; }
            uint8_t htype[4];
            rc = trak_handler_type(mv, moov_c.body, o + moov_c.size, htype);
            if (rc != TC_OK) { goto fail; }
            if (memcmp(htype, "soun", 4u) == 0) {
                /* v1.6：至多 TC_AUDIO_MAX_TRACKS 条（声明序分派） */
                if (mv->n_a >= TOPOS_MAX_AUDIO_TRACKS) {
                    rc = TC_ERR_MALFORMED;
                    tc_set_error(TC_ERR_MALFORMED,
                                 "音轨数超上限（%u）", TOPOS_MAX_AUDIO_TRACKS);
                    goto fail;
                }
                uint32_t slot = mv->n_a++;
                audio_track_state* t = &mv->a_trk[slot];
                memset(t, 0, sizeof(*t));
                t->asc = (uint8_t*)tc_alloc(64u); /* esds ASC ≤64B（写侧同限） */
                if (t->asc == NULL) {
                    rc = TC_ERR_OUT_OF_MEMORY;
                    tc_set_error(TC_ERR_OUT_OF_MEMORY, "asc buffer");
                    goto fail;
                }
                rc = parse_audio_trak(mv, slot, moov_c.body, o + moov_c.size);
                if (rc != TC_OK) { goto fail; }
                o += moov_c.size;
                continue;
            }
            if (memcmp(htype, "tmcd", 4u) == 0) {
                /* v1.5 时间码轨（恒末轨；≤1） */
                if (mv->tc_set != 0u) {
                    rc = TC_ERR_MALFORMED;
                    tc_set_error(TC_ERR_MALFORMED, "重复 tmcd trak");
                    goto fail;
                }
                rc = parse_tmcd_trak(mv, moov_c.body, o + moov_c.size);
                if (rc != TC_OK) { goto fail; }
                o += moov_c.size;
                continue;
            }
            if (have_trak) {
                rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "多视频 trak 不支持");
                goto fail;
            }
            have_trak = 1;
            uint64_t t = moov_c.body, tend = o + moov_c.size;
            while (t < tend) {
                atom_hdr trak_c;
                rc = read_atom(mv, t, tend, &trak_c);
                if (rc != TC_OK) { goto fail; }
                if (type_is(&trak_c, "mdia")) {
                    uint64_t d = trak_c.body, dend = t + trak_c.size;
                    while (d < dend) {
                        atom_hdr mdia_c;
                        rc = read_atom(mv, d, dend, &mdia_c);
                        if (rc != TC_OK) { goto fail; }
                        if (type_is(&mdia_c, "mdhd")) {
                            if (mdia_c.size < 32ull) {
                                rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "mdhd 过短");
                                goto fail;
                            }
                            uint8_t b[8];
                            rc = io_read(mv, mdia_c.body + 12ull, b, 8u);
                            if (rc != TC_OK) { goto fail; }
                            mv->timescale = tc_load_be32(b);
                            if (mv->timescale == 0u) {
                                rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "timescale=0");
                                goto fail;
                            }
                        } else if (type_is(&mdia_c, "hdlr")) {
                            if (mdia_c.size >= 24ull) {
                                uint8_t b[4];
                                rc = io_read(mv, mdia_c.body + 8ull, b, 4u);
                                if (rc != TC_OK) { goto fail; }
                                if (memcmp(b, "vide", 4u) == 0) { have_vide = 1; }
                            }
                        } else if (type_is(&mdia_c, "minf")) {
                            uint64_t f = mdia_c.body, fend = d + mdia_c.size;
                            while (f < fend) {
                                atom_hdr minf_c;
                                rc = read_atom(mv, f, fend, &minf_c);
                                if (rc != TC_OK) { goto fail; }
                                if (type_is(&minf_c, "stbl")) {
                                    have_stbl = 1;
                                    uint64_t s = minf_c.body, send = f + minf_c.size;
                                    while (s < send) {
                                        atom_hdr stc;
                                        rc = read_atom(mv, s, send, &stc);
                                        if (rc != TC_OK) { goto fail; }
                                        /* P1-18：singleton 重复 = malformed */
                                        int seen = 0;
                                        if (type_is(&stc, "stsd")) {
                                            seen = st.has_stsd; st.has_stsd = 1;
                                            if (!seen) { rc = parse_stsd(mv, stc.body, s + stc.size, &st); }
                                        } else if (type_is(&stc, "stts")) {
                                            seen = st.has_stts; st.has_stts = 1;
                                            if (!seen) { rc = parse_stts(mv, stc.body, s + stc.size, &st); }
                                        } else if (type_is(&stc, "stsz")) {
                                            seen = st.has_stsz; st.has_stsz = 1;
                                            if (!seen) { rc = parse_stsz(mv, stc.body, s + stc.size, &st); }
                                        } else if (type_is(&stc, "stsc")) {
                                            seen = st.has_stsc; st.has_stsc = 1;
                                            if (!seen) { rc = parse_stsc(mv, stc.body, s + stc.size, &st); }
                                        } else if (type_is(&stc, "stco") || type_is(&stc, "co64")) {
                                            seen = st.has_stco; st.has_stco = 1;
                                            if (!seen) {
                                                rc = parse_stco(mv, stc.body, s + stc.size,
                                                                type_is(&stc, "co64") ? 1 : 0, &st);
                                            }
                                        } else if (type_is(&stc, "stss")) {
                                            seen = st.has_stss; st.has_stss = 1;
                                            if (!seen) { rc = parse_stss(mv, stc.body, s + stc.size, &st); }
                                        }
                                        if (seen) {
                                            rc = TC_ERR_MALFORMED;
                                            tc_set_error(TC_ERR_MALFORMED,
                                                         "重复 singleton 表 atom（%c%c%c%c）",
                                                         stc.type[0], stc.type[1],
                                                         stc.type[2], stc.type[3]);
                                            goto fail;
                                        }
                                        if (rc != TC_OK) { goto fail; }
                                        s += stc.size;
                                    }
                                }
                                f += minf_c.size;
                            }
                        }
                        d += mdia_c.size;
                    }
                } else if (type_is(&trak_c, "edts")) {
                    /* v1.4：elst 仅音频 trak 最小子集；视频轨 pts 无空洞约束
                     * 不动——视频 trak 带 edts 一律拒绝（输入域不变）。 */
                    rc = TC_ERR_MALFORMED;
                    tc_set_error(TC_ERR_MALFORMED, "视频 trak 不允许 edts/elst");
                    goto fail;
                }
                t += trak_c.size;
            }
            o += moov_c.size;
        }
        if (!have_trak || !have_vide || !have_stbl) {
            rc = TC_ERR_MALFORMED;
            tc_set_error(TC_ERR_MALFORMED, "moov 缺 trak/vide/stbl");
            goto fail;
        }
    }

    /* —— 采样表一致性（spec §7.5）—— */
    if (st.sample_total == 0ull) {
        rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "空 stts");
        goto fail;
    }
    if (st.sample_total > (uint64_t)TC_MOVIE_MAX_SAMPLES) {
        rc = TC_ERR_LIMIT_EXCEEDED;
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "sample 数 %llu 超上限",
                     (unsigned long long)st.sample_total);
        goto fail;
    }
    if ((uint64_t)st.stsz_count != st.sample_total) {
        rc = TC_ERR_MALFORMED;
        tc_set_error(TC_ERR_MALFORMED, "stsz 计数 %u ≠ stts 展开 %llu",
                     st.stsz_count, (unsigned long long)st.sample_total);
        goto fail;
    }
    mv->n = (uint32_t)st.sample_total;

    {
        size_t n = (size_t)mv->n;
        mv->offsets = (uint64_t*)tc_alloc(n * sizeof(uint64_t));
        mv->sizes = (uint32_t*)tc_alloc(n * sizeof(uint32_t));
        mv->pts = (uint64_t*)tc_alloc(n * sizeof(uint64_t));
        mv->durs = (uint32_t*)tc_alloc(n * sizeof(uint32_t));
        mv->sync = (uint8_t*)tc_calloc(n / 8u + 1u, 1u);
        if (mv->offsets == NULL || mv->sizes == NULL || mv->pts == NULL ||
            mv->durs == NULL || mv->sync == NULL) {
            rc = TC_ERR_OUT_OF_MEMORY;
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "索引分配");
            goto fail;
        }
        mv->index_bytes = (uint32_t)(n * (sizeof(uint64_t) + 2u * sizeof(uint32_t) +
                                          sizeof(uint64_t)) + n / 8u + 1u);
    }

    /* stts → pts/durs */
    {
        uint8_t blk[4096];
        uint64_t tp = st.stts_off;
        uint32_t si = 0u;
        uint64_t acc = 0u;
        while (si < mv->n) {
            if (tp >= st.stts_end) {
                rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "stts 展开不足");
                goto fail;
            }
            size_t chunk = (size_t)((st.stts_end - tp) > 4096ull ? 4096ull : (st.stts_end - tp));
            chunk -= chunk % 8u;
            if (chunk == 0u) {
                rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "stts 残尾");
                goto fail;
            }
            rc = io_read(mv, tp, blk, chunk);
            if (rc != TC_OK) { goto fail; }
            for (size_t p = 0; p < chunk && si < mv->n; p += 8u) {
                uint32_t cnt = tc_load_be32(blk + p);
                uint32_t dlt = tc_load_be32(blk + p + 4u);
                for (uint32_t k = 0; k < cnt && si < mv->n; ++k) {
                    mv->pts[si] = acc;
                    mv->durs[si] = dlt;
                    acc += (uint64_t)dlt;
                    si++;
                }
            }
            tp += chunk;
        }
    }
    /* stsz */
    if (st.stsz_sample_size != 0u) {
        for (uint32_t i = 0; i < mv->n; ++i) { mv->sizes[i] = st.stsz_sample_size; }
    } else {
        uint8_t blk[4096];
        uint64_t tp = st.stsz_data_off;
        uint32_t si = 0u;
        while (si < mv->n) {
            if (tp >= st.stsz_data_end) {
                rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "stsz 数据不足");
                goto fail;
            }
            size_t chunk = (size_t)((st.stsz_data_end - tp) > 4096ull ? 4096ull : (st.stsz_data_end - tp));
            chunk -= chunk % 4u;
            if (chunk == 0u) {
                rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "stsz 残尾");
                goto fail;
            }
            rc = io_read(mv, tp, blk, chunk);
            if (rc != TC_OK) { goto fail; }
            for (size_t p = 0; p < chunk && si < mv->n; p += 4u) {
                mv->sizes[si++] = tc_load_be32(blk + p);
            }
            tp += chunk;
        }
        for (uint32_t i = 0; i < mv->n; ++i) {
            if (mv->sizes[i] == 0u) {
                rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "stsz size=0");
                goto fail;
            }
        }
    }
    /* stsc 展开 × stco → 绝对 sample 偏移（视频轨） */
    {
        int32_t erc = stbl_expand_sample_offsets(mv, &st, mv->n, mv->offsets, mv->sizes);
        if (erc != TC_OK) { rc = erc; goto fail; }
        /* 复验 P1-12：每个 sample [offset, offset+size) 必须完整落在某个
         * mdat payload 区间内（size ≤ u32，off+size 用区间差不溢出方式核对） */
        erc = check_samples_in_mdat(mv->n, mv->offsets, mv->sizes,
                                    mdat_lo, mdat_hi, mdat_count, "视频");
        if (erc != TC_OK) { rc = erc; goto fail; }
    }
    /* v1.1：音频 chunk 的 mdat 归属核对（区间在顶层 walk 收集） */
    for (uint32_t ai = 0; ai < mv->n_a; ++ai) {
        int32_t erc = check_samples_in_mdat(mv->a_trk[ai].n,
                                            mv->a_trk[ai].offsets,
                                            mv->a_trk[ai].sizes,
                                            mdat_lo, mdat_hi, mdat_count, "音频");
        if (erc != TC_OK) { rc = erc; goto fail; }
    }
    /* stss bitmap（缺失 = 全同步） */
    if (st.has_stss) {
        uint8_t blk[4096];
        uint64_t tp = st.stss_off;
        uint32_t left = st.stss_count;
        while (left != 0u) {
            if (tp >= st.stss_end) {
                rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "stss 数据不足");
                goto fail;
            }
            size_t chunk = (size_t)((st.stss_end - tp) > 4096ull ? 4096ull : (st.stss_end - tp));
            chunk -= chunk % 4u;
            if (chunk == 0u) {
                rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "stss 残尾");
                goto fail;
            }
            rc = io_read(mv, tp, blk, chunk);
            if (rc != TC_OK) { goto fail; }
            for (size_t p = 0; p < chunk && left != 0u; p += 4u) {
                uint32_t s = tc_load_be32(blk + p);
                left--;
                if (s < 1u || s > mv->n) {
                    rc = TC_ERR_MALFORMED; tc_set_error(TC_ERR_MALFORMED, "stss 越界");
                    goto fail;
                }
                mv->sync[(s - 1u) / 8u] |= (uint8_t)(1u << ((s - 1u) % 8u));
            }
            tp += chunk;
        }
    } else {
        memset(mv->sync, 0xFF, (size_t)mv->n / 8u + 1u);
    }

    /* tpcC → 配置（字段级复用 frame 校验） */
    {
        const uint8_t* t = st.tpcc;
        topos_movie_config* c = &mv->cfg;
        c->profile = t[6];
        c->pixel_format = t[7];
        c->bit_depth = t[8];
        c->qmatrix_id = t[9];
        c->qp_base = t[10];
        c->alpha_mode = t[11];
        c->alpha_bit_depth = t[12];
        c->alpha_premultiplied = (uint8_t)((t[13] & 1u) != 0u);
        c->color_range = (uint8_t)((t[13] & 2u) != 0u);
        c->color_primaries = t[14];
        c->color_transfer = t[15];
        c->color_matrix = t[16];
        c->chroma_siting = t[17];
        c->sar_num = tc_load_be16(t + 18);
        c->sar_den = tc_load_be16(t + 20);
        topos_frame_config fc;
        movie_cfg_to_frame(c, &fc);
        fc.visible_width = st.entry_w;
        fc.visible_height = st.entry_h;
        rc = tc_frame_config_validate(&fc);
        if (rc != TC_OK) { goto fail; }
        mv->width = st.entry_w;
        mv->height = st.entry_h;
        if (c->sar_num == 0u && c->sar_den == 0u) { c->sar_num = 1u; c->sar_den = 1u; }
        rc = TC_OK;
    }

    /* tpcB → 预算元数据（一致性：mode/depth 与 tpcC 逐字段、frames == sample 数） */
    if (rc == TC_OK && st.has_tpcb) {
        if (st.tpcb[6] != mv->cfg.alpha_mode || st.tpcb[7] != mv->cfg.alpha_bit_depth) {
            rc = TC_ERR_MALFORMED;
            tc_set_error(TC_ERR_MALFORMED,
                         "tpcB 与 tpcC 的 alpha_mode/bit_depth 不一致");
            goto fail;
        }
        if (tc_load_be32(st.tpcb + 16) != mv->n) {
            rc = TC_ERR_MALFORMED;
            tc_set_error(TC_ERR_MALFORMED, "tpcB frame_count ≠ sample 数");
            goto fail;
        }
        topos_alpha_budget_info* bi = &mv->budget;
        bi->target_ratio_bp = tc_load_be16(st.tpcb + 8);
        bi->actual_ratio_bp = tc_load_be16(st.tpcb + 10);
        bi->max_abs_error = tc_load_be16(st.tpcb + 12);
        bi->flags = tc_load_be16(st.tpcb + 14);
        bi->frame_count = tc_load_be32(st.tpcb + 16);
        mv->budget_set = 1u;
    }

    *out = mv;
    return TC_OK;

fail:
    tc_movie_close(mv);
    return rc;
}

int32_t tc_movie_info(const topos_movie* mv, topos_movie_info* info)
{
    if (mv == NULL || info == NULL ||
        info->struct_size != (uint32_t)sizeof(topos_movie_info) ||
        info->abi_version != TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "info NULL/镜像不符");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* reserved[0] is the ABI-stable compatibility slot for the track's
     * tpcC/bitstream major.  Clear all slots so callers never observe stack
     * data in fields that remain reserved. */
    memset(info->reserved, 0, sizeof(info->reserved));
    const topos_movie_config* c = &mv->cfg;
    info->visible_width = mv->width;
    info->visible_height = mv->height;
    info->profile = c->profile;
    info->pixel_format = c->pixel_format;
    info->bit_depth = c->bit_depth;
    info->qmatrix_id = c->qmatrix_id;
    info->qp_base = c->qp_base;
    info->qp_delta_luma = c->qp_delta_luma;
    info->qp_delta_chroma = c->qp_delta_chroma;
    info->alpha_mode = c->alpha_mode;
    info->alpha_bit_depth = c->alpha_bit_depth;
    info->alpha_premultiplied = c->alpha_premultiplied;
    info->color_range = c->color_range;
    info->color_primaries = c->color_primaries;
    info->color_transfer = c->color_transfer;
    info->color_matrix = c->color_matrix;
    info->chroma_siting = c->chroma_siting;
    info->sar_num = c->sar_num;
    info->sar_den = c->sar_den;
    info->timescale = mv->timescale;
    info->sample_count = mv->n;
    info->faststart = (uint32_t)mv->faststart;
    info->index_bytes = mv->index_bytes;
    info->reserved[0] = (uint32_t)mv->tpcc_major;
    /* v1.7：tpcD 档位（无 tpcD / 未声明档位 = TC_TIER_NONE） */
    info->reserved[1] = (mv->meta_set != 0u) ? (uint32_t)mv->meta.tier_id : 0u;
    return TC_OK;
}

int32_t tc_movie_alpha_budget(const topos_movie* mv, topos_alpha_budget_info* info)
{
    if (mv == NULL || info == NULL ||
        info->struct_size != (uint32_t)sizeof(topos_alpha_budget_info) ||
        info->abi_version != TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "info NULL/镜像不符");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (mv->budget_set == 0u) {
        tc_set_error(TC_ERR_STATE, "文件未记录 alpha 预算元数据（无 tpcB）");
        return TC_ERR_STATE;
    }
    *info = mv->budget;
    info->struct_size = (uint32_t)sizeof(topos_alpha_budget_info);
    info->abi_version = TOPOS_CODEC_ABI_VERSION;
    return TC_OK;
}

static int32_t movie_audio_info_at(const topos_movie* mv, uint32_t track,
                                   topos_audio_track_info* info)
{
    if (mv == NULL || info == NULL ||
        info->struct_size != (uint32_t)sizeof(topos_audio_track_info) ||
        info->abi_version != TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "audio info NULL/镜像不符");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (track >= mv->n_a) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "音轨索引 %u 越界（文件含 %u 轨）", track, mv->n_a);
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(((uint8_t*)info) + 8u, 0, sizeof(*info) - 8u);
    const audio_track_state* t = &mv->a_trk[track];
    info->codec = t->codec;
    info->sample_rate = t->rate;
    info->channel_count = t->channels;
    info->channel_layout = t->layout;
    info->bits_per_sample = t->bits;
    info->sample_count = (uint32_t)(t->total_frames > 0xFFFFFFFFull
                                    ? 0xFFFFFFFFull : t->total_frames);
    info->chunk_count = t->n;
    info->reserved[0] = t->priming;     /* v1.4 priming_samples；0 = 无 elst */
    info->reserved[1] = t->format;      /* v1.4 sample_format；0 = 整数 */
    memcpy(info->name, t->name, sizeof(info->name) - 1u);
    return TC_OK;
}

int32_t tc_movie_audio_info(const topos_movie* mv, topos_audio_track_info* info)
{
    /* v1.1 语义 = 轨 0 视图；无音轨时保持 v1.1 行为（codec=0，TC_OK） */
    if (mv != NULL && mv->n_a == 0u) {
        if (info == NULL ||
            info->struct_size != (uint32_t)sizeof(topos_audio_track_info) ||
            info->abi_version != TOPOS_CODEC_ABI_VERSION) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "audio info NULL/镜像不符");
            return TC_ERR_INVALID_ARGUMENT;
        }
        memset(((uint8_t*)info) + 8u, 0, sizeof(*info) - 8u);
        return TC_OK;
    }
    return movie_audio_info_at(mv, 0u, info);
}

int32_t tc_movie_audio_info_at(const topos_movie* mv, uint32_t track,
                               topos_audio_track_info* info)
{
    return movie_audio_info_at(mv, track, info);
}

int32_t tc_movie_audio_track_count(const topos_movie* mv, uint32_t* out_count)
{
    if (mv == NULL || out_count == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "audio count NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    *out_count = mv->n_a;
    return TC_OK;
}

/* v1.5：count → hh/mm/ss/ff（DF 逆换算：每分钟跳 2、每 10 分钟整不跳；
 * 分钟块内第 ≥1 分钟的 wall ff = 块内计数 + 2——oracle 数值验证：
 * 1799→00:00:59;29、1800→00:01:00;02、17982→00:10:00;00）。 */
/* RC1：读回 TRAW 开发元数据（无 trwm 原子 → TC_ERR_STATE）。 */
int32_t tc_movie_read_raw_meta(const topos_movie* mv, topos_raw_meta* out)
{
    if (mv == NULL || out == NULL ||
        out->struct_size != (uint32_t)sizeof(topos_raw_meta) ||
        out->abi_version != TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "read_raw_meta: NULL/镜像不符");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (mv->rmeta_set == 0u) {
        tc_set_error(TC_ERR_STATE, "文件无 trwm 原子");
        return TC_ERR_STATE;
    }
    *out = mv->rmeta;
    return TC_OK;
}

int32_t tc_movie_timecode(const topos_movie* mv, topos_timecode_info* out)
{
    if (mv == NULL || out == NULL ||
        out->struct_size != (uint32_t)sizeof(topos_timecode_info) ||
        out->abi_version != TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "timecode out NULL/镜像不符");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (mv->tc_set == 0u) {
        tc_set_error(TC_ERR_STATE, "文件无 tmcd 时间码轨");
        return TC_ERR_STATE;
    }
    out->start_frame_count = mv->tc_start_count;
    out->fps = mv->tc_fps;
    out->drop_frame = mv->tc_df;
    out->flags = mv->tc_flags;
    const uint32_t d = mv->tc_start_count;
    uint32_t mins = 0u, frame = 0u;
    if (mv->tc_df != 0u) {
        /* 每分钟跳帧数：60DF=4（;00-;03）、30DF=2（;00-;01）——ffmpeg
         * oracle 实测（'00:01:00;04'@60DF → 计数 3600，逆换算同式回 0;4） */
        const uint32_t dfpm = tmcd_df_per_minute(mv->tc_fps);
        const uint32_t fpm = mv->tc_fps * 60u - dfpm;    /* 1798 @30 */
        const uint32_t fp10 = mv->tc_fps * 600u - 9u * dfpm; /* 17982 @30 */
        const uint32_t ten = d / fp10;
        const uint32_t rem = d % fp10;
        uint32_t min_in_block = 0u, cnt = 0u;
        if (rem < mv->tc_fps * 60u) {
            min_in_block = 0u;
            cnt = rem;
        } else {
            min_in_block = 1u + (rem - mv->tc_fps * 60u) / fpm;
            cnt = (rem - mv->tc_fps * 60u) % fpm;
        }
        mins = ten * 10u + min_in_block;
        frame = (min_in_block == 0u) ? cnt : cnt + dfpm;
    } else {
        mins = d / (mv->tc_fps * 60u);
        frame = d % (mv->tc_fps * 60u);
    }
    out->hh = (int32_t)(mins / 60u);
    out->mm = (int32_t)(mins % 60u);
    out->ss = (int32_t)(frame / mv->tc_fps);
    out->ff = (int32_t)(frame % mv->tc_fps);
    return TC_OK;
}

static int32_t movie_read_audio_at(const topos_movie* mv, uint32_t track,
                                   uint64_t start_chunk, uint32_t chunk_count,
                                   uint8_t* buf, size_t cap, size_t* need_size)
{
    const audio_track_state* t = &mv->a_trk[track];
    if (start_chunk >= t->n ||
        (uint64_t)chunk_count > (uint64_t)t->n - start_chunk) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "音频 chunk 区间越界");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (need_size != NULL) { *need_size = 0u; }
    /* 总字节数（checked） */
    uint64_t total = 0u;
    for (uint32_t i = 0u; i < chunk_count; ++i) {
        if (!tc_uadd_u64(total, t->sizes[start_chunk + i], &total)) {
            tc_set_error(TC_ERR_LIMIT_EXCEEDED, "音频区间总字节数溢出");
            return TC_ERR_LIMIT_EXCEEDED;
        }
    }
    if (total > (uint64_t)SIZE_MAX) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "音频区间总字节数超 SIZE_MAX");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    if (need_size != NULL) { *need_size = (size_t)total; }
    if (chunk_count == 0u) { return TC_OK; }
    if (buf == NULL || cap < (size_t)total) { return TC_ERR_BUFFER_TOO_SMALL; }
    /* 区间逐 chunk 连续（本库写入器恒连续）→ 单次 io 读；否则逐 chunk 读入 */
    int contiguous = 1;
    for (uint32_t i = 1u; i < chunk_count; ++i) {
        if (t->offsets[start_chunk + i] !=
            t->offsets[start_chunk + i - 1u] + (uint64_t)t->sizes[start_chunk + i - 1u]) {
            contiguous = 0;
            break;
        }
    }
    if (contiguous) {
        return io_read(mv, t->offsets[start_chunk], buf, (size_t)total);
    }
    size_t off = 0u;
    for (uint32_t i = 0u; i < chunk_count; ++i) {
        int32_t rc = io_read(mv, t->offsets[start_chunk + i], buf + off,
                             t->sizes[start_chunk + i]);
        if (rc != TC_OK) { return rc; }
        off += t->sizes[start_chunk + i];
    }
    return TC_OK;
}

int32_t tc_movie_read_audio(const topos_movie* mv, uint64_t start_chunk,
                            uint32_t chunk_count, uint8_t* buf, size_t cap,
                            size_t* need_size)
{
    /* v1.1 语义 = 轨 0；无音轨 STATE */
    if (mv == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "movie NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (mv->n_a == 0u) {
        tc_set_error(TC_ERR_STATE, "文件无音频轨");
        return TC_ERR_STATE;
    }
    return movie_read_audio_at(mv, 0u, start_chunk, chunk_count, buf, cap,
                               need_size);
}

int32_t tc_movie_read_audio_at(const topos_movie* mv, uint32_t track,
                               uint64_t start_chunk, uint32_t chunk_count,
                               uint8_t* buf, size_t cap, size_t* need_size)
{
    if (mv == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "movie NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (mv->n_a == 0u) {
        tc_set_error(TC_ERR_STATE, "文件无音频轨");
        return TC_ERR_STATE;
    }
    if (track >= mv->n_a) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "音轨索引 %u 越界（文件含 %u 轨）", track, mv->n_a);
        return TC_ERR_INVALID_ARGUMENT;
    }
    return movie_read_audio_at(mv, track, start_chunk, chunk_count, buf, cap,
                               need_size);
}

/* Validate the packet header against the already parsed track metadata.  MOV
 * open intentionally stays moov-only, so this check runs when a sample is
 * actually requested (and for every sample in a batch); it never guesses the
 * track version from the first frame. */
static int32_t movie_validate_packet_header(const topos_movie* mv,
                                            uint32_t sample_index,
                                            const uint8_t* packet, size_t size)
{
    if (packet == NULL || size < TC_FRAME_HEADER_SIZE) {
        tc_set_error(TC_ERR_TRUNCATED, "sample %u shorter than frame header",
                     (unsigned)sample_index);
        return TC_ERR_TRUNCATED;
    }
    topos_frame_header fh;
    int32_t rc = movie_parse_packet_structure(packet, size, &fh);
    if (rc != TC_OK) { return rc; }
    if ((uint64_t)fh.frame_packet_size != (uint64_t)size) {
        tc_set_error(TC_ERR_MALFORMED,
                     "sample %u frame_packet_size %u != stsz %zu",
                     (unsigned)sample_index, (unsigned)fh.frame_packet_size, size);
        return TC_ERR_MALFORMED;
    }
    if (fh.version_major != mv->tpcc_major ||
        fh.visible_width != mv->width || fh.visible_height != mv->height ||
        fh.profile != mv->cfg.profile || fh.pixel_format != mv->cfg.pixel_format ||
        fh.bit_depth != mv->cfg.bit_depth || fh.qmatrix_id != mv->cfg.qmatrix_id ||
        fh.alpha_mode != mv->cfg.alpha_mode ||
        fh.alpha_bit_depth != mv->cfg.alpha_bit_depth ||
        (((fh.flags & 1u) != 0u) != (mv->cfg.alpha_premultiplied != 0u)) ||
        fh.color_range != mv->cfg.color_range ||
        fh.color_primaries != mv->cfg.color_primaries ||
        fh.color_transfer != mv->cfg.color_transfer ||
        fh.color_matrix != mv->cfg.color_matrix ||
        fh.chroma_siting != mv->cfg.chroma_siting ||
        ((fh.sar_num == 0u && fh.sar_den == 0u) ? 1u : fh.sar_num) != mv->cfg.sar_num ||
        ((fh.sar_num == 0u && fh.sar_den == 0u) ? 1u : fh.sar_den) != mv->cfg.sar_den) {
        tc_set_error(TC_ERR_MALFORMED,
                     "sample %u header 与 tpcC/TPIC sample entry 不一致",
                     (unsigned)sample_index);
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

/* V7-B's outer header is read without the payload.  Keep the same track-level
 * metadata checks as the complete sample path, while allowing the embedded
 * base packet to carry its own legacy major and reduced geometry. */
static int32_t movie_validate_v7b_outer_header(const topos_movie* mv,
                                               uint32_t sample_index,
                                               const topos_frame_header* fh,
                                               size_t sample_size)
{
    if (mv == NULL || fh == NULL ||
        (uint64_t)fh->frame_packet_size != (uint64_t)sample_size ||
        fh->version_major != mv->tpcc_major ||
        fh->visible_width != mv->width || fh->visible_height != mv->height ||
        fh->profile != mv->cfg.profile || fh->pixel_format != mv->cfg.pixel_format ||
        fh->bit_depth != mv->cfg.bit_depth || fh->qmatrix_id != mv->cfg.qmatrix_id ||
        fh->alpha_mode != mv->cfg.alpha_mode ||
        fh->alpha_bit_depth != mv->cfg.alpha_bit_depth ||
        (((fh->flags & 1u) != 0u) != (mv->cfg.alpha_premultiplied != 0u)) ||
        fh->color_range != mv->cfg.color_range ||
        fh->color_primaries != mv->cfg.color_primaries ||
        fh->color_transfer != mv->cfg.color_transfer ||
        fh->color_matrix != mv->cfg.color_matrix ||
        fh->chroma_siting != mv->cfg.chroma_siting ||
        ((fh->sar_num == 0u && fh->sar_den == 0u) ? 1u : fh->sar_num) != mv->cfg.sar_num ||
        ((fh->sar_num == 0u && fh->sar_den == 0u) ? 1u : fh->sar_den) != mv->cfg.sar_den) {
        tc_set_error(TC_ERR_MALFORMED,
                     "sample %u V7-B outer header 与 tpcC 不一致",
                     (unsigned)sample_index);
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

static int32_t movie_validate_v7b_base_header(const topos_movie* mv,
                                              uint32_t sample_index,
                                              const topos_frame_header* outer,
                                              const uint8_t* base,
                                              size_t base_size)
{
    if (mv == NULL || outer == NULL || base == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "V7-B base validation arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    topos_frame_header fh;
    int32_t rc = tc_frame_header_decode(base, base_size, &fh);
    if (rc != TC_OK) { return rc; }
    if (fh.visible_width > outer->visible_width ||
        fh.visible_height > outer->visible_height ||
        fh.plane_count != outer->plane_count || fh.alpha_mode != 0u ||
        fh.profile != mv->cfg.profile || fh.pixel_format != mv->cfg.pixel_format ||
        fh.bit_depth != mv->cfg.bit_depth || fh.qmatrix_id != mv->cfg.qmatrix_id ||
        fh.color_range != mv->cfg.color_range ||
        fh.color_primaries != mv->cfg.color_primaries ||
        fh.color_transfer != mv->cfg.color_transfer ||
        fh.color_matrix != mv->cfg.color_matrix ||
        fh.chroma_siting != mv->cfg.chroma_siting ||
        ((fh.sar_num == 0u && fh.sar_den == 0u) ? 1u : fh.sar_num) != mv->cfg.sar_num ||
        ((fh.sar_num == 0u && fh.sar_den == 0u) ? 1u : fh.sar_den) != mv->cfg.sar_den) {
        tc_set_error(TC_ERR_MALFORMED,
                     "sample %u V7-B base header 与轨级元数据不一致",
                     (unsigned)sample_index);
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

int32_t tc_movie_packet(const topos_movie* mv, uint32_t sample_index,
                        uint8_t* buf, size_t cap, size_t* need_size)
{
    if (mv == NULL || sample_index >= mv->n) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "sample 索引越界");
        return TC_ERR_INVALID_ARGUMENT;
    }
    size_t sz = (size_t)mv->sizes[sample_index];
    if (need_size != NULL) { *need_size = sz; }
    uint64_t off = mv->offsets[sample_index];
    if (off > mv->io.length || sz > mv->io.length - off) {
        tc_set_error(TC_ERR_MALFORMED, "sample %u 区间越界（不影响其它帧）", sample_index);
        return TC_ERR_MALFORMED;
    }
    if (buf == NULL || cap < sz) { return TC_ERR_BUFFER_TOO_SMALL; }
    int32_t rc = io_read(mv, off, buf, sz);
    if (rc != TC_OK) { return rc; }
    return movie_validate_packet_header(mv, sample_index, buf, sz);
}

int32_t tc_movie_packet_base(const topos_movie* mv, uint32_t sample_index,
                             uint8_t* buf, size_t cap, size_t* need_size)
{
    if (mv == NULL || sample_index >= mv->n) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "sample 索引越界");
        return TC_ERR_INVALID_ARGUMENT;
    }
    const size_t sample_size = (size_t)mv->sizes[sample_index];
    if (need_size != NULL) { *need_size = sample_size; }
    const uint64_t sample_offset = mv->offsets[sample_index];
    if (sample_offset > mv->io.length ||
        (uint64_t)sample_size > mv->io.length - sample_offset) {
        tc_set_error(TC_ERR_MALFORMED, "sample %u 区间越界（不影响其它帧）",
                     (unsigned)sample_index);
        return TC_ERR_MALFORMED;
    }

    /* Only V7 tracks can contain the scalable directory.  All other tracks
     * retain the exact tc_movie_packet contract and full-sample validation. */
    if (mv->tpcc_major != 7u) {
        return tc_movie_packet(mv, sample_index, buf, cap, need_size);
    }

    movie_sample_range range = {
        .movie = mv,
        .sample_offset = sample_offset,
        .sample_size = (uint64_t)sample_size,
    };
    size_t base_size = 0u;
    topos_frame_header outer;
    tc_v7b_decode_stats stats;
    int32_t rc = tc_v7b_read_base_packet(movie_sample_range_read, &range,
                                         (uint64_t)sample_size, buf, cap,
                                         &base_size, &outer, &stats);
    if (rc == TC_ERR_UNSUPPORTED_VERSION) {
        /* V7-A is a valid V7 track but has no spatial base packet. */
        return tc_movie_packet(mv, sample_index, buf, cap, need_size);
    }
    if (need_size != NULL) { *need_size = base_size; }
    if (rc == TC_ERR_BUFFER_TOO_SMALL) { return rc; }
    if (rc != TC_OK) { return rc; }
    rc = movie_validate_v7b_outer_header(mv, sample_index, &outer, sample_size);
    if (rc != TC_OK) { return rc; }
    return movie_validate_v7b_base_header(mv, sample_index, &outer, buf, base_size);
}

int32_t tc_movie_packet_base_batch(const topos_movie* mv, uint32_t start,
                                   uint32_t count, uint8_t* arena, size_t cap,
                                   size_t* offsets_out, size_t* sizes_out,
                                   uint8_t* base_flags_out, size_t* need_total)
{
    if (mv == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "movie NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (count == 0u) { return TC_OK; }
    if (start >= mv->n || count > mv->n - start) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "sample 区间越界");
        return TC_ERR_INVALID_ARGUMENT;
    }
    size_t total = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        size_t size = 0u;
        int32_t rc = tc_movie_packet_base(mv, start + i, NULL, 0u, &size);
        if (rc != TC_ERR_BUFFER_TOO_SMALL) { return rc; }
        if (offsets_out != NULL) { offsets_out[i] = total; }
        if (sizes_out != NULL) { sizes_out[i] = size; }
        if (base_flags_out != NULL) {
            base_flags_out[i] = size < (size_t)mv->sizes[start + i] ? 1u : 0u;
        }
        if (!tc_uadd_size(total, size, &total)) {
            tc_set_error(TC_ERR_LIMIT_EXCEEDED, "base batch total size");
            return TC_ERR_LIMIT_EXCEEDED;
        }
    }
    if (need_total != NULL) { *need_total = total; }
    if (arena == NULL || cap < total) { return TC_ERR_BUFFER_TOO_SMALL; }
    size_t offset = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        const size_t slot_offset = offsets_out != NULL ? offsets_out[i] : offset;
        size_t size = sizes_out != NULL ? sizes_out[i] : 0u;
        if (sizes_out == NULL) {
            int32_t probe_rc = tc_movie_packet_base(mv, start + i, NULL, 0u, &size);
            if (probe_rc != TC_ERR_BUFFER_TOO_SMALL) { return probe_rc; }
        }
        int32_t rc = tc_movie_packet_base(mv, start + i, arena + slot_offset, size, NULL);
        if (rc != TC_OK) { return rc; }
        if (!tc_uadd_size(offset, size, &offset)) {
            tc_set_error(TC_ERR_LIMIT_EXCEEDED, "base batch offset");
            return TC_ERR_LIMIT_EXCEEDED;
        }
    }
    return TC_OK;
}

int32_t tc_movie_packet_batch(const topos_movie* mv, uint32_t start,
                              uint32_t count, uint8_t* arena, size_t cap,
                              size_t* offsets_out, size_t* sizes_out,
                              size_t* need_total)
{
    if (mv == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "movie NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (count == 0u) { return TC_OK; }
    if (start >= mv->n || count > mv->n - start) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "sample 区间越界");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* 先算紧凑布局并整段校验文件界——任何越界都不触碰 arena（对齐单帧
     * 语义：坏区间拒绝且不影响其它读取） */
    uint64_t total = 0u;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t off = mv->offsets[start + i];
        uint64_t sz = (uint64_t)mv->sizes[start + i];
        if (off > mv->io.length || sz > mv->io.length - off) {
            tc_set_error(TC_ERR_MALFORMED, "sample %u 区间越界", start + i);
            return TC_ERR_MALFORMED;
        }
        if (offsets_out != NULL) { offsets_out[i] = (size_t)total; }
        if (sizes_out != NULL) { sizes_out[i] = (size_t)sz; }
        total += sz;
    }
    if (total > (uint64_t)SIZE_MAX) {
        tc_set_error(TC_ERR_MALFORMED, "区间总字节数溢出");
        return TC_ERR_MALFORMED;
    }
    if (need_total != NULL) { *need_total = (size_t)total; }
    if (arena == NULL || cap < (size_t)total) { return TC_ERR_BUFFER_TOO_SMALL; }
    /* 区间在文件内逐包连续 → 单次 io 读（顺序布局常态：一次系统调用取
     * 整批）；非连续（外来交错布局）逐包读入紧凑槽位 */
    int contiguous = 1;
    for (uint32_t i = 1; i < count; ++i) {
        if (mv->offsets[start + i] !=
            mv->offsets[start + i - 1u] + (uint64_t)mv->sizes[start + i - 1u]) {
            contiguous = 0;
            break;
        }
    }
    int32_t rc = TC_OK;
    if (contiguous) {
        rc = total == 0u ? TC_OK
                         : io_read(mv, mv->offsets[start], arena, (size_t)total);
        if (rc != TC_OK) { return rc; }
    } else {
        uint8_t* cur = arena;
        for (uint32_t i = 0; i < count; ++i) {
            size_t sz = (size_t)mv->sizes[start + i];
            rc = io_read(mv, mv->offsets[start + i], cur, sz);
            if (rc != TC_OK) { return rc; }
            cur += sz;
        }
    }
    size_t packet_off = 0u;
    for (uint32_t i = 0; i < count; ++i) {
        rc = movie_validate_packet_header(mv, start + i, arena + packet_off,
                                           (size_t)mv->sizes[start + i]);
        if (rc != TC_OK) { return rc; }
        packet_off += (size_t)mv->sizes[start + i];
    }
    return TC_OK;
}

int32_t tc_movie_packet_pts(const topos_movie* mv, uint32_t sample_index,
                            uint64_t* pts_tick, uint32_t* dur_tick)
{
    if (mv == NULL || sample_index >= mv->n || (pts_tick == NULL && dur_tick == NULL)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "参数非法");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (pts_tick != NULL) { *pts_tick = mv->pts[sample_index]; }
    if (dur_tick != NULL) { *dur_tick = mv->durs[sample_index]; }
    return TC_OK;
}

int32_t tc_movie_packet_sync(const topos_movie* mv, uint32_t sample_index, uint8_t* is_sync)
{
    if (mv == NULL || is_sync == NULL || sample_index >= mv->n) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "参数非法");
        return TC_ERR_INVALID_ARGUMENT;
    }
    *is_sync = (uint8_t)((mv->sync[sample_index / 8u] >> (sample_index % 8u)) & 1u);
    return TC_OK;
}

/* V9（topos_v9_micro_gop_plan 批 3）：≤ sample_index 的最近同步 sample。
 * seek 定位入口：命中本身为同步 → 返回自身；无 stss（默认全同步）→
 * 返回自身；位图指明 index 之前无任何同步（畸形流：首 sample 前 P）→
 * TC_ERR_STATE（调用方须结构化报错，禁静默从头解）。 */
int32_t tc_movie_prev_sync(const topos_movie* mv, uint32_t sample_index,
                           uint32_t* sync_index)
{
    if (mv == NULL || sync_index == NULL || sample_index >= mv->n) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "参数非法");
        return TC_ERR_INVALID_ARGUMENT;
    }
    uint32_t k = sample_index;
    for (;;) {
        if ((mv->sync[k / 8u] >> (k % 8u)) & 1u) { break; }
        if (k == 0u) {
            tc_set_error(TC_ERR_STATE,
                         "no sync sample at or before index %u (broken GOP chain)",
                         (unsigned)sample_index);
            return TC_ERR_STATE;
        }
        k--;
    }
    *sync_index = k;
    return TC_OK;
}

void tc_movie_close(topos_movie* mv)
{
    if (mv == NULL) { return; }
    tc_free(mv->offsets);
    tc_free(mv->sizes);
    tc_free(mv->pts);
    tc_free(mv->durs);
    tc_free(mv->sync);
    for (uint32_t ai = 0; ai < mv->n_a; ++ai) {
        tc_free(mv->a_trk[ai].offsets);
        tc_free(mv->a_trk[ai].sizes);
        tc_free(mv->a_trk[ai].durs);
        tc_free(mv->a_trk[ai].asc);
    }
    tc_free(mv);
}

/* ============================ FastStart ============================ */

/* faststart 偏移重排助手（视频样本后接逐轨音频 chunk 顺序布局） */
static void fs_assign_offsets(const topos_movie* mv, uint64_t base,
                              uint64_t* noff, uint64_t** anoffs)
{
    uint64_t acc = base;
    for (uint32_t i = 0; i < mv->n; ++i) {
        noff[i] = acc;
        acc += (uint64_t)mv->sizes[i];
    }
    for (uint32_t ai = 0; ai < mv->n_a; ++ai) {
        uint64_t* off = anoffs[ai];
        if (off == NULL) { continue; }
        for (uint32_t k = 0; k < mv->a_trk[ai].n; ++k) {
            off[k] = acc;
            acc += (uint64_t)mv->a_trk[ai].sizes[k];
        }
    }
}

static int fs_need_co64(const topos_movie* mv, const uint64_t* noff,
                        uint64_t** anoffs)
{
    for (uint32_t i = 0; i < mv->n; ++i) {
        if (noff[i] > 0xFFFFFFFFull) { return 1; }
    }
    for (uint32_t ai = 0; ai < mv->n_a; ++ai) {
        const uint64_t* off = anoffs[ai];
        if (off == NULL) { continue; }
        for (uint32_t k = 0; k < mv->a_trk[ai].n; ++k) {
            if (off[k] > 0xFFFFFFFFull) { return 1; }
        }
    }
    return 0;
}

int32_t tc_movie_faststart(const topos_io* src, const topos_io* dst)
{
    if (src == NULL || dst == NULL || dst->write == NULL ||
        dst->struct_size != (uint32_t)sizeof(topos_io) ||
        dst->abi_version != TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "faststart 参数非法");
        return TC_ERR_INVALID_ARGUMENT;
    }
    topos_movie* mv = NULL;
    int32_t rc = tc_movie_open(src, &mv);
    if (rc != TC_OK) { return rc; }
    if (mv->faststart != 0) { tc_movie_close(mv); return TC_OK; } /* 已是 */

    topos_movie_config cfg = mv->cfg;
    cfg.visible_width = mv->width;
    cfg.visible_height = mv->height;
    cfg.timescale = mv->timescale;
    const topos_alpha_budget_info* budget = mv->budget_set ? &mv->budget : NULL;
    const topos_movie_meta* meta = mv->meta_set ? &mv->meta : NULL;
    uint64_t total_dur = 0u;
    for (uint32_t i = 0; i < mv->n; ++i) {
        if (!tc_uadd_u64(total_dur, (uint64_t)mv->durs[i], &total_dur)) {
            tc_movie_close(mv);
            return TC_ERR_LIMIT_EXCEEDED;
        }
    }
    /* v1.1/v1.6 音频轨重定位（moov 内逐轨音频 chunk 偏移跟随视频顺序重排） */
    audio_moov_meta ams[TOPOS_MAX_AUDIO_TRACKS];
    uint32_t n_am = 0u;
    uint64_t* anoffs[TOPOS_MAX_AUDIO_TRACKS];
    memset(anoffs, 0, sizeof(anoffs));
    for (uint32_t ai = 0; ai < mv->n_a; ++ai) {
        const audio_track_state* t = &mv->a_trk[ai];
        audio_moov_meta* am = &ams[n_am++];
        memset(am, 0, sizeof(*am));
        am->track_id = 2u + ai;
        am->name = (t->name[0] != '\0') ? t->name : NULL;
        am->codec = t->codec;
        am->rate = t->rate;
        am->channels = t->channels;
        am->layout = t->layout;
        am->bits = t->bits;
        am->format = t->format;
        am->n = t->n;
        am->total_frames = t->total_frames;
        am->sizes = t->sizes;
        am->durs = t->durs;
        am->asc = t->asc_size != 0u ? t->asc : NULL;
        am->asc_size = t->asc_size;
        am->priming = t->priming;   /* v1.4：elst 随 faststart 重写保留 */
        anoffs[ai] = (uint64_t*)tc_calloc((size_t)t->n, sizeof(uint64_t));
        if (anoffs[ai] == NULL) {
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "faststart audio offsets");
            tc_movie_close(mv);
            return TC_ERR_OUT_OF_MEMORY;
        }
        am->offsets = anoffs[ai];
    }
    /* 新布局 ftyp + moov + mdat；offsets 值不影响 moov 长度 →
     * 先以全 0 偏移估 stco 尺寸，按新 mdat 起点算真实偏移，越界则升 co64 重算。 */
    uint64_t* noff = (uint64_t*)tc_calloc((size_t)mv->n, sizeof(uint64_t));
    mov_buf b = { NULL, 0, 0 };
    if (noff == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "faststart offsets");
        for (uint32_t ai = 0; ai < mv->n_a; ++ai) { tc_free(anoffs[ai]); }
        tc_movie_close(mv);
        return TC_ERR_OUT_OF_MEMORY;
    }
    /* v1.5：tmcd 元数据携带（样本重定位到 mdat 尾） */
    tmcd_moov_meta tm;
    tmcd_moov_meta* tm_ptr = NULL;
    if (mv->tc_set != 0u) {
        memset(&tm, 0, sizeof(tm));
        tm.fps = mv->tc_fps;
        tm.df = mv->tc_df;
        tm.track_id = 2u + mv->n_a;   /* v1.6：audio=2..N+1、tmcd=N+2 */
        tm.movie_dur = mv->tc_movie_dur;
        tm.frame_count = mv->tc_frame_count;
        tm.start_count = mv->tc_start_count;
        tm_ptr = &tm;
    }
    uint64_t tm_payload = 0u;
    for (uint32_t i = 0; i < mv->n; ++i) { tm_payload += (uint64_t)mv->sizes[i]; }
    for (uint32_t ai = 0; ai < mv->n_a; ++ai) {
        for (uint32_t k = 0; k < mv->a_trk[ai].n; ++k) {
            tm_payload += (uint64_t)mv->a_trk[ai].sizes[k];
        }
    }
    /* 读端 sync 位图 → 字节展开（build_moov 的 sync_map 语义）；
     * V9 轨经 faststart 必须保留只列 I 的 stss（批 3）。 */
    uint8_t* sync_bytes = (uint8_t*)tc_calloc((size_t)mv->n, sizeof(uint8_t));
    if (sync_bytes == NULL) {
        tc_free(noff);
        for (uint32_t ai = 0; ai < mv->n_a; ++ai) { tc_free(anoffs[ai]); }
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "faststart sync map");
        tc_movie_close(mv);
        return TC_ERR_OUT_OF_MEMORY;
    }
    for (uint32_t i = 0; i < mv->n; ++i) {
        sync_bytes[i] = (uint8_t)((mv->sync[i / 8u] >> (i % 8u)) & 1u);
    }
    if (tm_ptr != NULL) {
        tm_ptr->sample_offset = (uint64_t)MOV_FTYP_BYTES + b.len + MOV_MDAT_HDR
                                + tm_payload;
    }
    rc = build_moov(&cfg, mv->tpcc_major, mv->n, noff, mv->sizes, mv->durs, total_dur, 0, budget, meta, mv->rmeta_set ? &mv->rmeta : NULL, &b, sync_bytes, n_am != 0u ? ams : NULL, n_am, tm_ptr);
    int need_co64 = 0;
    if (rc == TC_OK) {
        fs_assign_offsets(mv, (uint64_t)MOV_FTYP_BYTES + (uint64_t)b.len + MOV_MDAT_HDR,
                          noff, anoffs);
        need_co64 = fs_need_co64(mv, noff, anoffs);
        if (need_co64) {
            /* co64 变体长度可能不同 → 重估起点 → 重算偏移 → 重建（≤2 轮收敛）。
             * build_moov 不清空 buf：每轮重建前必须释放，tmcd 样本偏移用
             * 释放前实测的 moov 长度回推（stco/co64 值不影响 moov 长度）。 */
            for (int round = 0; round < 2 && rc == TC_OK; ++round) {
                const uint64_t moov_len = (uint64_t)b.len;
                buf_free(&b);
                if (tm_ptr != NULL) {
                    tm_ptr->sample_offset = (uint64_t)MOV_FTYP_BYTES + moov_len
                                            + MOV_MDAT_HDR + tm_payload;
                }
                rc = build_moov(&cfg, mv->tpcc_major, mv->n, noff, mv->sizes, mv->durs, total_dur, 1, budget, meta, mv->rmeta_set ? &mv->rmeta : NULL, &b, sync_bytes, n_am != 0u ? ams : NULL, n_am, tm_ptr);
                if (rc != TC_OK) { break; }
                fs_assign_offsets(mv, (uint64_t)MOV_FTYP_BYTES + (uint64_t)b.len + MOV_MDAT_HDR,
                                  noff, anoffs);
            }
        } else {
            const uint64_t moov_len = (uint64_t)b.len; /* 第一次 build 实测 */
            buf_free(&b);
            if (tm_ptr != NULL) {
                tm_ptr->sample_offset = (uint64_t)MOV_FTYP_BYTES + moov_len
                                        + MOV_MDAT_HDR + tm_payload;
            }
            rc = build_moov(&cfg, mv->tpcc_major, mv->n, noff, mv->sizes, mv->durs, total_dur, 0, budget, meta, mv->rmeta_set ? &mv->rmeta : NULL, &b, sync_bytes, n_am != 0u ? ams : NULL, n_am, tm_ptr);
        }
    }
    if (rc == TC_OK) {
        uint8_t mdat_hdr[MOV_MDAT_HDR];
        tc_store_be32(mdat_hdr, 1u);
        memcpy(mdat_hdr + 4, "mdat", 4u);
        uint64_t payload = 0u;
        for (uint32_t i = 0; i < mv->n; ++i) { payload += (uint64_t)mv->sizes[i]; }
        for (uint32_t ai = 0; ai < mv->n_a; ++ai) {
            for (uint32_t k = 0; k < mv->a_trk[ai].n; ++k) {
                payload += (uint64_t)mv->a_trk[ai].sizes[k];
            }
        }
        tc_store_be64(mdat_hdr + 8, payload + MOV_MDAT_HDR + (tm_ptr ? 4u : 0u));

        int32_t w = dst->write(dst->ctx, MOV_FTYP, sizeof(MOV_FTYP));
        if (w == TC_OK) { w = dst->write(dst->ctx, b.data, b.len); }
        if (w == TC_OK) { w = dst->write(dst->ctx, mdat_hdr, sizeof(mdat_hdr)); }
        uint8_t* cp = (uint8_t*)tc_alloc(1024u * 1024u);
        if (cp == NULL && (mv->n != 0u || mv->n_a != 0u)) { w = TC_ERR_OUT_OF_MEMORY; }
        /* 视频 sample 拷贝 */
        for (uint32_t i = 0; w == TC_OK && cp != NULL && i < mv->n; ++i) {
            uint64_t left = (uint64_t)mv->sizes[i];
            uint64_t o = mv->offsets[i];
            while (left != 0u && w == TC_OK) {
                size_t chunk = left > 1048576ull ? 1048576u : (size_t)left;
                w = src->read(src->ctx, o, cp, chunk);
                if (w == TC_OK) { w = dst->write(dst->ctx, cp, chunk); }
                o += chunk;
                left -= chunk;
            }
        }
        /* 音频 chunk 拷贝（v1.1/v1.6 逐轨，声明序 = 布局序） */
        for (uint32_t ai = 0; w == TC_OK && cp != NULL && ai < mv->n_a; ++ai) {
        for (uint32_t k = 0; w == TC_OK && k < mv->a_trk[ai].n; ++k) {
            uint64_t left = (uint64_t)mv->a_trk[ai].sizes[k];
            uint64_t o = mv->a_trk[ai].offsets[k];
            while (left != 0u && w == TC_OK) {
                size_t chunk = left > 1048576ull ? 1048576u : (size_t)left;
                w = src->read(src->ctx, o, cp, chunk);
                if (w == TC_OK) { w = dst->write(dst->ctx, cp, chunk); }
                o += chunk;
                left -= chunk;
            }
        }
        }
        tc_free(cp);
        if (w != TC_OK) { rc = w; }
        /* v1.5：tmcd 4B 样本追加（重定位到 mdat 尾） */
        if (rc == TC_OK && tm_ptr != NULL) {
            uint8_t s4[4];
            tc_store_be32(s4, tm_ptr->start_count);
            rc = dst->write(dst->ctx, s4, 4u);
        }
    }
    tc_free(noff);
    for (uint32_t ai = 0; ai < mv->n_a; ++ai) { tc_free(anoffs[ai]); }
    tc_free(sync_bytes);
    buf_free(&b);
    tc_movie_close(mv);
    return rc;
}
