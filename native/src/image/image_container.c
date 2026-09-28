/* image_container —— TPIM pack/unpack + IDSC 规则实现（spec §3/§4/§6）。 */
#include "image/image_container.h"

#include <string.h>

#include "common/checked.h"
#include "common/crc32.h"
#include "common/error.h"

/* —— BE 读写助手（显式偏移） —— */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t rd64(const uint8_t* p)
{
    return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void wr64(uint8_t* p, uint64_t v)
{
    wr32(p, (uint32_t)(v >> 32)); wr32(p + 4, (uint32_t)v);
}

bool tci_chunk_limit(uint32_t type, uint64_t* limit)
{
    switch (type) {
    case TC_IMG_CHUNK_ICCP: *limit = TC_IMG_MAX_CHUNK_ICCP; return true;
    case TC_IMG_CHUNK_OCIO: *limit = TC_IMG_MAX_CHUNK_OCIO; return true;
    case TC_IMG_CHUNK_XMP:  *limit = TC_IMG_MAX_CHUNK_XMP;  return true;
    case TC_IMG_CHUNK_EXIF: *limit = TC_IMG_MAX_CHUNK_EXIF; return true;
    case TC_IMG_CHUNK_THMB: *limit = TC_IMG_MAX_CHUNK_THMB; return true;
    case TC_IMG_CHUNK_HASH: *limit = TC_IMG_MAX_CHUNK_HASH; return true;
    /* v1.7：档位/厂商元数据（tpcD 同构载荷；≤12+15+63+4=94B，取整） */
    case TC_IMG_CHUNK_TMET: *limit = 128u; return true;
    /* RC1：TRAW 开发元数据（v1 固定 40B 载荷；上限留扩展余量） */
    case TC_IMG_CHUNK_TRWM: *limit = TC_IMG_MAX_CHUNK_TRWM; return true;
    default: return false;
    }
}

bool tci_chunk_reserved(uint32_t type)
{
    return type == TC_IMG_CHUNK_TILE || type == TC_IMG_CHUNK_TDIR ||
           type == TC_IMG_CHUNK_MIPM || type == TC_IMG_CHUNK_AUXC;
}

/* ==================== preamble ==================== */

int32_t tci_pack_preamble(const topos_image_preamble* p, uint8_t out[TC_IMG_PREAMBLE_SIZE])
{
    if (p == NULL || out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "image preamble pack: null arg");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, TC_IMG_PREAMBLE_SIZE);
    wr32(out + 0, TCI_MAGIC_TPIM);
    wr16(out + 4, p->file_version_major);
    wr16(out + 6, p->file_version_minor);
    wr32(out + 8, TC_IMG_PREAMBLE_SIZE);
    wr32(out + 12, p->flags);
    wr64(out + 16, p->file_size);
    wr64(out + 24, p->directory_offset);
    wr32(out + 32, p->directory_entry_size);
    wr32(out + 36, p->directory_count);
    wr32(out + 40, p->primary_image_index);
    wr32(out + 44, p->compatibility_flags);
    wr32(out + 48, 0); /* header_crc32 占位 */
    wr32(out + 52, p->directory_crc32);
    wr64(out + 56, 0); /* reserved0 */
    const uint32_t crc = tc_crc32(out, TC_IMG_PREAMBLE_SIZE);
    wr32(out + 48, crc);
    return TC_OK;
}

int32_t tci_unpack_preamble(const uint8_t in[TC_IMG_PREAMBLE_SIZE],
                            topos_image_preamble* out)
{
    if (in == NULL || out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "image preamble unpack: null arg");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->abi_version = TOPOS_IMAGE_ABI_VERSION;

    if (rd32(in + 0) != TCI_MAGIC_TPIM) {
        tc_set_error(TC_IMG_ERR_BAD_MAGIC, "preamble magic mismatch (not TPIM)");
        return TC_IMG_ERR_BAD_MAGIC;
    }
    out->file_version_major = rd16(in + 4);
    out->file_version_minor = rd16(in + 6);
    out->flags = rd32(in + 12);
    out->file_size = rd64(in + 16);
    out->directory_offset = rd64(in + 24);
    out->directory_entry_size = rd32(in + 32);
    out->directory_count = rd32(in + 36);
    out->primary_image_index = rd32(in + 40);
    out->compatibility_flags = rd32(in + 44);
    out->header_crc32 = rd32(in + 48);
    out->directory_crc32 = rd32(in + 52);

    /* CRC：对完整 64B、header_crc32 字段清零后计算 */
    uint8_t zeroed[TC_IMG_PREAMBLE_SIZE];
    memcpy(zeroed, in, TC_IMG_PREAMBLE_SIZE);
    wr32(zeroed + 48, 0);
    if (tc_crc32(zeroed, TC_IMG_PREAMBLE_SIZE) != out->header_crc32) {
        tc_set_error(TC_IMG_ERR_BAD_PREAMBLE, "preamble header_crc32 mismatch");
        return TC_IMG_ERR_BAD_PREAMBLE;
    }

    /* 字段规则（spec §3） */
    if (out->file_version_major != 1u) {
        tc_set_error(TC_ERR_UNSUPPORTED_VERSION, "unsupported file_version_major=%u",
                     (unsigned)out->file_version_major);
        return TC_ERR_UNSUPPORTED_VERSION;
    }
    /* spec §14：minor 向后兼容演进（新增 optional chunk / compatibility 位），
     * 读端必须容忍——v1 读端接受同 major 的任意 minor，不做 !=0 硬拒。 */
    if (rd32(in + 8) != TC_IMG_PREAMBLE_SIZE) {
        tc_set_error(TC_IMG_ERR_BAD_PREAMBLE, "preamble_size != 64");
        return TC_IMG_ERR_BAD_PREAMBLE;
    }
    if (out->flags != 0u) {
        tc_set_error(TC_IMG_ERR_BAD_PREAMBLE, "preamble flags undefined bits set");
        return TC_IMG_ERR_BAD_PREAMBLE;
    }
    /* spec §9.3：本 minor（=0）只定义 bit0/bit1，未定义位必须为 0；
     * minor > 0 的文件可能携带后续版本定义的新位（§14 容忍）。 */
    if (out->file_version_minor == 0u && (out->compatibility_flags & ~0x3u) != 0u) {
        tc_set_error(TC_IMG_ERR_BAD_PREAMBLE,
                     "compatibility_flags undefined bits set (minor=0)");
        return TC_IMG_ERR_BAD_PREAMBLE;
    }
    if (out->directory_entry_size != TC_IMG_DIR_ENTRY_SIZE) {
        tc_set_error(TC_IMG_ERR_BAD_PREAMBLE, "directory_entry_size != 32");
        return TC_IMG_ERR_BAD_PREAMBLE;
    }
    if (out->directory_count < 2u || out->directory_count > TC_IMG_MAX_CHUNKS) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "directory_count=%u out of [2,%u]",
                     (unsigned)out->directory_count, (unsigned)TC_IMG_MAX_CHUNKS);
        return TC_ERR_LIMIT_EXCEEDED;
    }
    if (out->primary_image_index != 0u) {
        tc_set_error(TC_IMG_ERR_BAD_PREAMBLE, "primary_image_index != 0 (v1)");
        return TC_IMG_ERR_BAD_PREAMBLE;
    }
    static const uint8_t kZero[8] = {0};
    if (memcmp(in + 56, kZero, 8) != 0) {
        tc_set_error(TC_IMG_ERR_BAD_PREAMBLE, "preamble reserved0 != 0");
        return TC_IMG_ERR_BAD_PREAMBLE;
    }
    return TC_OK;
}

/* ==================== directory entry ==================== */

int32_t tci_pack_dir_entry(const topos_image_dir_entry* e, uint8_t out[TC_IMG_DIR_ENTRY_SIZE])
{
    if (e == NULL || out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "image dir entry pack: null arg");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, TC_IMG_DIR_ENTRY_SIZE);
    wr32(out + 0, e->chunk_type);
    wr32(out + 4, e->chunk_flags);
    wr64(out + 8, e->chunk_offset);
    wr64(out + 16, e->chunk_size);
    wr32(out + 24, e->chunk_crc32);
    wr32(out + 28, 0);
    return TC_OK;
}

int32_t tci_unpack_dir_entry(const uint8_t in[TC_IMG_DIR_ENTRY_SIZE],
                             topos_image_dir_entry* out)
{
    if (in == NULL || out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "image dir entry unpack: null arg");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->chunk_type = rd32(in + 0);
    out->chunk_flags = rd32(in + 4);
    out->chunk_offset = rd64(in + 8);
    out->chunk_size = rd64(in + 16);
    out->chunk_crc32 = rd32(in + 24);
    if (rd32(in + 28) != 0u) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "dir entry reserved != 0");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    if (out->chunk_size == 0u) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "dir entry chunk_size == 0");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    /* critical/optional 互斥；都不置位视为 optional（spec §4） */
    const uint32_t both = TC_IMG_CHUNK_FLAG_CRITICAL | TC_IMG_CHUNK_FLAG_OPTIONAL;
    if ((out->chunk_flags & both) == both) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "dir entry critical|optional both set");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    if (out->chunk_flags & ~0xFu) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "dir entry undefined flag bits");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    return TC_OK;
}

/* ==================== IDSC ==================== */

int32_t tci_pack_idsc(const topos_image_idsc* s, uint8_t out[TC_IMG_IDSC_SIZE])
{
    if (s == NULL || out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "idsc pack: null arg");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, TC_IMG_IDSC_SIZE);
    wr32(out + 0, 0x49445343u); /* 'IDSC' */
    wr16(out + 4, s->idsc_version_major);
    wr16(out + 6, s->idsc_version_minor);
    wr32(out + 8, TC_IMG_IDSC_SIZE);
    wr32(out + 12, s->flags);
    wr32(out + 16, (uint32_t)s->display_x_min);
    wr32(out + 20, (uint32_t)s->display_y_min);
    wr32(out + 24, (uint32_t)s->display_x_max);
    wr32(out + 28, (uint32_t)s->display_y_max);
    wr32(out + 32, (uint32_t)s->data_x_min);
    wr32(out + 36, (uint32_t)s->data_y_min);
    wr32(out + 40, (uint32_t)s->data_x_max);
    wr32(out + 44, (uint32_t)s->data_y_max);
    wr32(out + 48, s->orientation);
    wr32(out + 52, s->pixel_aspect_num);
    wr32(out + 56, s->pixel_aspect_den);
    out[60] = s->channel_model;
    out[61] = s->channel_count;
    out[62] = s->sample_kind;
    out[63] = s->valid_bit_depth;
    out[64] = s->container_bit_depth;
    out[65] = s->storage_layout;
    out[66] = s->subsampling;
    out[67] = s->chroma_siting;
    out[68] = s->alpha_presence;
    out[69] = s->alpha_mode;
    out[70] = s->alpha_bit_depth;
    out[71] = s->alpha_max_abs_err;
    out[72] = s->codec_id;
    out[73] = s->payload_major;
    out[74] = s->payload_minor;
    out[75] = s->image_profile;
    out[76] = s->codec_profile;
    out[77] = s->pixel_format;
    out[78] = s->color_primaries;
    out[79] = s->color_transfer;
    out[80] = s->color_matrix;
    out[81] = s->color_range;
    out[82] = s->iccp_ref;
    out[83] = s->ocio_ref;
    wr32(out + 84, s->payload_header_crc32);
    /* out[88..127] reserved 保持 0 */
    return TC_OK;
}

int32_t tci_unpack_idsc(const uint8_t* in, size_t size, topos_image_idsc* out)
{
    if (in == NULL || out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "idsc unpack: null arg");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (size != TC_IMG_IDSC_SIZE) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC chunk size=%zu != 128", size);
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->abi_version = TOPOS_IMAGE_ABI_VERSION;
    if (rd32(in + 0) != 0x49445343u) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC magic mismatch");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    out->idsc_version_major = rd16(in + 4);
    out->idsc_version_minor = rd16(in + 6);
    out->flags = rd32(in + 12);
    out->display_x_min = (int32_t)rd32(in + 16);
    out->display_y_min = (int32_t)rd32(in + 20);
    out->display_x_max = (int32_t)rd32(in + 24);
    out->display_y_max = (int32_t)rd32(in + 28);
    out->data_x_min = (int32_t)rd32(in + 32);
    out->data_y_min = (int32_t)rd32(in + 36);
    out->data_x_max = (int32_t)rd32(in + 40);
    out->data_y_max = (int32_t)rd32(in + 44);
    out->orientation = rd32(in + 48);
    out->pixel_aspect_num = rd32(in + 52);
    out->pixel_aspect_den = rd32(in + 56);
    out->channel_model = in[60];
    out->channel_count = in[61];
    out->sample_kind = in[62];
    out->valid_bit_depth = in[63];
    out->container_bit_depth = in[64];
    out->storage_layout = in[65];
    out->subsampling = in[66];
    out->chroma_siting = in[67];
    out->alpha_presence = in[68];
    out->alpha_mode = in[69];
    out->alpha_bit_depth = in[70];
    out->alpha_max_abs_err = in[71];
    out->codec_id = in[72];
    out->payload_major = in[73];
    out->payload_minor = in[74];
    out->image_profile = in[75];
    out->codec_profile = in[76];
    out->pixel_format = in[77];
    out->color_primaries = in[78];
    out->color_transfer = in[79];
    out->color_matrix = in[80];
    out->color_range = in[81];
    out->iccp_ref = in[82];
    out->ocio_ref = in[83];
    out->payload_header_crc32 = rd32(in + 84);

    if (out->idsc_version_major != 1u || out->idsc_version_minor != 0u) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "unsupported IDSC version %u.%u",
                     (unsigned)out->idsc_version_major, (unsigned)out->idsc_version_minor);
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    if (rd32(in + 8) != TC_IMG_IDSC_SIZE) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC idsc_size != 128");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    /* spec §6：reserved（offset 88..127）必须为 0——与 preamble reserved0、
     * 目录项 reserved 的强制风格一致执行 */
    {
        static const uint8_t kZero[40] = {0};
        if (memcmp(in + 88, kZero, 40) != 0) {
            tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC reserved bytes != 0");
            return TC_IMG_ERR_BAD_DIRECTORY;
        }
    }
    return tci_idsc_validate(out);
}

int32_t tci_idsc_validate(const topos_image_idsc* s)
{
    if (s == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "idsc validate: null");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (s->flags & ~TC_IMG_IDSC_FLAG_ORIENTATION_NORMALIZED) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC flags undefined bits");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    if (s->display_x_max < s->display_x_min || s->display_y_max < s->display_y_min) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC display window inverted");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    if (s->data_x_max < s->data_x_min || s->data_y_max < s->data_y_min) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC data window inverted");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    /* display ⊇ data（spec §6.1-2） */
    if (s->display_x_min > s->data_x_min || s->display_y_min > s->data_y_min ||
        s->display_x_max < s->data_x_max || s->display_y_max < s->data_y_max) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "display window does not contain data window");
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }
    if (s->orientation < 1u || s->orientation > 8u) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC orientation %u not in 1..8",
                     (unsigned)s->orientation);
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    if (s->pixel_aspect_num < 1u || s->pixel_aspect_den < 1u) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC pixel_aspect < 1:1");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    switch (s->channel_model) {
    case TC_IMG_CHANNEL_MODEL_RGB:
    case TC_IMG_CHANNEL_MODEL_YUV:
    case TC_IMG_CHANNEL_MODEL_GRAY:
    case TC_IMG_CHANNEL_MODEL_CFA: /* TRAW（批 1）：Bayer 相位平面 */
        break;
    default:
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC channel_model=%u reserved",
                     (unsigned)s->channel_model);
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    if (s->channel_count != 3u && s->channel_count != 4u) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC channel_count=%u invalid",
                     (unsigned)s->channel_count);
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    if (s->sample_kind != TC_IMG_SAMPLE_KIND_UINT &&
        s->sample_kind != TC_IMG_SAMPLE_KIND_HALF) {
        /* 2（FLOAT）等仍 reserved——旧值 1 于 2026-09-19 激活为 HALF */
        tc_set_error(TC_ERR_UNSUPPORTED_VERSION, "IDSC sample_kind=%u reserved (UINT/HALF only)",
                     (unsigned)s->sample_kind);
        return TC_ERR_UNSUPPORTED_VERSION;
    }
    if (s->valid_bit_depth != 8u && s->valid_bit_depth != 10u &&
        s->valid_bit_depth != 12u && s->valid_bit_depth != 16u) {
        /* 批 4：+16（TRAW linear 归档档；16-bit 容器枚举本就存在） */
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC valid_bit_depth=%u invalid",
                     (unsigned)s->valid_bit_depth);
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    if (s->container_bit_depth != 10u && s->container_bit_depth != 12u &&
        s->container_bit_depth != 16u) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC container_bit_depth=%u invalid",
                     (unsigned)s->container_bit_depth);
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    if (s->valid_bit_depth > s->container_bit_depth) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC valid_bit_depth > container_bit_depth");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    if (s->storage_layout != TC_IMG_LAYOUT_PLANAR) {
        tc_set_error(TC_ERR_UNSUPPORTED_VERSION, "IDSC storage_layout=%u reserved (v1 planar)",
                     (unsigned)s->storage_layout);
        return TC_ERR_UNSUPPORTED_VERSION;
    }
    if (s->subsampling > 2u) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC subsampling=%u reserved",
                     (unsigned)s->subsampling);
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    if (s->alpha_presence > TC_IMG_ALPHA_PREMULT) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC alpha_presence=%u reserved",
                     (unsigned)s->alpha_presence);
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    if (s->alpha_mode > 2u) {
        tc_set_error(TC_ERR_UNSUPPORTED_ALPHA_MODE, "IDSC alpha_mode=%u reserved",
                     (unsigned)s->alpha_mode);
        return TC_ERR_UNSUPPORTED_ALPHA_MODE;
    }
    if (s->alpha_presence == TC_IMG_ALPHA_ABSENT) {
        if (s->alpha_mode != 0u || s->alpha_bit_depth != 0u || s->alpha_max_abs_err != 0u) {
            tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "IDSC alpha fields set while absent");
            return TC_IMG_ERR_CHUNK_CONFLICT;
        }
        if (s->channel_count != 3u && s->channel_model != TC_IMG_CHANNEL_MODEL_CFA) {
            /* TRAW（批 1）：CFA 无 alpha 但恒 4 相位平面——4 通道合法；
             * 其余模型仍要求 4 通道必带 alpha */
            tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "IDSC channel_count=4 while alpha absent");
            return TC_IMG_ERR_CHUNK_CONFLICT;
        }
        if (s->channel_model == TC_IMG_CHANNEL_MODEL_CFA && s->channel_count != 4u) {
            tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "IDSC CFA requires channel_count=4");
            return TC_IMG_ERR_CHUNK_CONFLICT;
        }
    } else {
        if (s->alpha_mode == 0u) {
            tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "IDSC alpha present but alpha_mode=0");
            return TC_IMG_ERR_CHUNK_CONFLICT;
        }
        if (s->alpha_mode == 1u) {
            if (s->alpha_bit_depth != 16u || s->alpha_max_abs_err != 0u) {
                tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT,
                             "IDSC alpha mode1 requires 16-bit/err=0");
                return TC_IMG_ERR_CHUNK_CONFLICT;
            }
        } else { /* mode 2 */
            if (s->alpha_bit_depth != 8u && s->alpha_bit_depth != 10u &&
                s->alpha_bit_depth != 12u) {
                tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "IDSC alpha mode2 depth invalid");
                return TC_IMG_ERR_CHUNK_CONFLICT;
            }
        }
        if (s->channel_count != 4u) {
            tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "IDSC channel_count=3 while alpha present");
            return TC_IMG_ERR_CHUNK_CONFLICT;
        }
    }
    if (s->codec_id != TC_IMG_CODEC_ID_TPIC) {
        tc_set_error(TC_ERR_UNSUPPORTED_VERSION, "IDSC codec_id=%u reserved (v1 TPIC)",
                     (unsigned)s->codec_id);
        return TC_ERR_UNSUPPORTED_VERSION;
    }
    /* payload version is an explicit contract between IDSC and the embedded
     * TPIC header.  Do not accept zero/unknown values and later infer them
     * from the first payload byte.  Accept range follows the codec version
     * machine (spec §14：内层 packet 版本独立演进)；V7=rANS/V8=段化，
     * 老二进制在此以 UNSUPPORTED_VERSION 明确拒绝新流。 */
    if (s->payload_major < 1u || s->payload_major > 8u) {
        tc_set_error(TC_ERR_UNSUPPORTED_VERSION,
                     "IDSC payload_major=%u not in supported TPIC V1..V8",
                     (unsigned)s->payload_major);
        return TC_ERR_UNSUPPORTED_VERSION;
    }
    if (s->image_profile > TC_IMG_PROFILE_HALF_FLOAT) {
        tc_set_error(TC_ERR_UNSUPPORTED_PROFILE, "IDSC image_profile=%u reserved in v1",
                     (unsigned)s->image_profile);
        return TC_ERR_UNSUPPORTED_PROFILE;
    }
    /* HALF 样本域交叉规则（spec §15，2026-09-19）：
     *  - 只随 GBR 4:4:4（channel_model=RGB、无子采样）出现；
     *  - 样本域冻结为 16-bit 容器满域码值（valid=container=16）；
     *  - 仅 image_profile 4 携带；alpha 只允许 mode1（无损 16-bit，
     *    alpha 平面同样为 half 码值）——mode2 的 8/10/12 近似语义
     *    建立在整数码值域上，对 HALF 无定义；
     *  - 反方向：profile 4 携带 UINT 一样是元数据谎言，拒绝。 */
    if (s->sample_kind == TC_IMG_SAMPLE_KIND_HALF) {
        if (s->image_profile != TC_IMG_PROFILE_HALF_FLOAT) {
            tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT,
                         "IDSC sample_kind=HALF requires image_profile 4 (HALF_FLOAT)");
            return TC_IMG_ERR_CHUNK_CONFLICT;
        }
        if (s->valid_bit_depth != 16u || s->container_bit_depth != 16u) {
            tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT,
                         "IDSC HALF requires valid/container_bit_depth 16");
            return TC_IMG_ERR_CHUNK_CONFLICT;
        }
        if (s->channel_model != TC_IMG_CHANNEL_MODEL_RGB) {
            tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT,
                         "IDSC HALF requires GBR channel_model (RGB)");
            return TC_IMG_ERR_CHUNK_CONFLICT;
        }
        if (s->subsampling != 0u) {
            tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "IDSC HALF requires 4:4:4");
            return TC_IMG_ERR_CHUNK_CONFLICT;
        }
        if (s->alpha_mode == 2u) {
            tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT,
                         "IDSC HALF alpha mode2 (approx 8/10/12) undefined for half domain");
            return TC_IMG_ERR_CHUNK_CONFLICT;
        }
    } else if (s->image_profile == TC_IMG_PROFILE_HALF_FLOAT) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT,
                     "IDSC image_profile 4 requires sample_kind=HALF");
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }
    if (s->iccp_ref > 1u || s->ocio_ref > 1u) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC iccp/ocio_ref invalid");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    return TC_OK;
}

/* 严格 UTF-8 序列校验（RFC 3629：拒绝过长编码、代理区、>U+10FFFF）。
 * OCIO 名称声明为 UTF-8（spec §5）——旧校验只拦控制字符，任意 ≥0x80 的
 * 乱码字节都能通过"可打印 UTF-8"名义。 */
static int utf8_well_formed(const uint8_t* data, size_t size)
{
    size_t i = 0;
    while (i < size) {
        const uint8_t b = data[i];
        if (b < 0x80u) { ++i; continue; }
        uint32_t cp = 0;
        size_t n = 0;
        if ((b & 0xE0u) == 0xC0u) { cp = b & 0x1Fu; n = 1; }
        else if ((b & 0xF0u) == 0xE0u) { cp = b & 0x0Fu; n = 2; }
        else if ((b & 0xF8u) == 0xF0u) { cp = b & 0x07u; n = 3; }
        else { return 0; } /* 孤立续字节 / 0xF8+ */
        if (i + n >= size) { return 0; } /* 序列越界（续字节不足） */
        for (size_t k = 1; k <= n; ++k) {
            const uint8_t c = data[i + k];
            if ((c & 0xC0u) != 0x80u) { return 0; }
            cp = (cp << 6) | (uint32_t)(c & 0x3Fu);
        }
        /* 过长编码 / 代理区 / 越界码点 */
        if ((n == 1u && cp < 0x80u) || (n == 2u && cp < 0x800u) ||
            (n == 3u && cp < 0x10000u) || cp > 0x10FFFFu ||
            (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return 0;
        }
        i += n + 1u;
    }
    return 1;
}

int32_t tci_validate_ocio(const uint8_t* data, size_t size)
{
    if (data == NULL || size == 0u) {
        tc_set_error(TC_IMG_ERR_METADATA_CONFLICT, "OCIO chunk empty");
        return TC_IMG_ERR_METADATA_CONFLICT;
    }
    if (size > TC_IMG_MAX_CHUNK_OCIO) {
        tc_set_error(TC_IMG_ERR_LIMIT, "OCIO %zu > 4 KiB", size);
        return TC_IMG_ERR_LIMIT;
    }
    if (data[size - 1u] == 0u) { --size; } /* 容忍单个结尾 NUL（C 字符串直写） */
    if (size == 0u) {
        tc_set_error(TC_IMG_ERR_METADATA_CONFLICT, "OCIO chunk empty");
        return TC_IMG_ERR_METADATA_CONFLICT;
    }
    for (size_t i = 0; i < size; ++i) {
        const uint8_t c = data[i];
        if (c < 0x20u || c == 0x7Fu) {
            tc_set_error(TC_IMG_ERR_METADATA_CONFLICT, "OCIO control byte at %zu", i);
            return TC_IMG_ERR_METADATA_CONFLICT;
        }
        if (c == '/' || c == '\\' || c == ':') {
            tc_set_error(TC_IMG_ERR_METADATA_CONFLICT,
                         "OCIO path separator at %zu (names must not be paths)", i);
            return TC_IMG_ERR_METADATA_CONFLICT;
        }
    }
    if (!utf8_well_formed(data, size)) {
        tc_set_error(TC_IMG_ERR_METADATA_CONFLICT,
                     "OCIO name is not well-formed UTF-8");
        return TC_IMG_ERR_METADATA_CONFLICT;
    }
    static const char* kBadSuffix[] = {".so", ".dylib", ".dll", ".bundle"};
    for (size_t k = 0; k < sizeof(kBadSuffix) / sizeof(kBadSuffix[0]); ++k) {
        const size_t n = strlen(kBadSuffix[k]);
        if (size >= n && memcmp(data + size - n, kBadSuffix[k], n) == 0) {
            tc_set_error(TC_IMG_ERR_METADATA_CONFLICT, "OCIO library-like suffix");
            return TC_IMG_ERR_METADATA_CONFLICT;
        }
    }
    return TC_OK;
}

int32_t tci_validate_iccp(const uint8_t* data, size_t size, uint8_t channel_model)
{
    if (data == NULL || size < 132u) {
        tc_set_error(TC_IMG_ERR_METADATA_CONFLICT, "ICCP shorter than ICC header");
        return TC_IMG_ERR_METADATA_CONFLICT;
    }
    if (size > TC_IMG_MAX_CHUNK_ICCP) {
        tc_set_error(TC_IMG_ERR_LIMIT, "ICCP %zu > 8 MiB", size);
        return TC_IMG_ERR_LIMIT;
    }
    if (data[36] != 'a' || data[37] != 'c' || data[38] != 's' || data[39] != 'p') {
        tc_set_error(TC_IMG_ERR_METADATA_CONFLICT, "ICCP missing 'acsp' signature");
        return TC_IMG_ERR_METADATA_CONFLICT;
    }
    if (channel_model == TC_IMG_CHANNEL_MODEL_RGB) {
        static const uint8_t kRGB[4] = {'R', 'G', 'B', ' '};
        static const uint8_t kCMYK[4] = {'C', 'M', 'Y', 'K'};
        if (memcmp(data + 16, kRGB, 4) != 0
                && memcmp(data + 16, kCMYK, 4) != 0) {
            tc_set_error(TC_IMG_ERR_METADATA_CONFLICT,
                         "ICCP colorSpace != 'RGB '/'CMYK ' for RGB image (ICC/CICP conflict)");
            return TC_IMG_ERR_METADATA_CONFLICT;
        }
        /* H5（D5 软转换路线）：'CMYK ' = 打印交付意图元数据——存储保持
         * RGB（入口 CMYK→RGB、出口 RGB→CMYK 由宿主色彩管理承担）；
         * 像素域契约不变，读端不因 CMYK profile 改变重建语义。 */
    }
    return TC_OK;
}

int32_t tci_idsc_crosscheck(const topos_image_idsc* s, const topos_frame_header* fh,
                            const uint8_t raw[TC_FRAME_HEADER_SIZE])
{
    if (s == NULL || fh == NULL || raw == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "idsc crosscheck: null arg");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* 1. data window == inner visible 尺寸 */
    const int64_t dw = (int64_t)s->data_x_max - (int64_t)s->data_x_min + 1;
    const int64_t dh = (int64_t)s->data_y_max - (int64_t)s->data_y_min + 1;
    if (dw != (int64_t)fh->visible_width || dh != (int64_t)fh->visible_height) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT,
                     "data window %lldx%lld != packet visible %ux%u",
                     (long long)dw, (long long)dh,
                     (unsigned)fh->visible_width, (unsigned)fh->visible_height);
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }
    /* 2. display ⊇ data（tci_idsc_validate 已查；此处防御性复查） */
    if (s->display_x_min > s->data_x_min || s->display_y_min > s->data_y_min ||
        s->display_x_max < s->data_x_max || s->display_y_max < s->data_y_max) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "display window does not contain data window");
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }
    if (fh->pixel_format != s->pixel_format) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "pixel_format %u != IDSC %u",
                     (unsigned)fh->pixel_format, (unsigned)s->pixel_format);
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }
    if (fh->bit_depth != s->valid_bit_depth) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "bit_depth %u != IDSC valid_bit_depth %u",
                     (unsigned)fh->bit_depth, (unsigned)s->valid_bit_depth);
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }
    if (fh->profile != s->codec_profile) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "codec profile %u != IDSC %u",
                     (unsigned)fh->profile, (unsigned)s->codec_profile);
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }
    /* image_profile ↔ codec 组合（spec §9.1：一一映射，不得互相替代——
     * Preview=YUV422/codec3，HQ=GBR10/codec5，XQ=GBR12/codec5|6）。
     * 兑现 topos_image.h 对 tc_image_write 交叉校验的承诺。 */
    {
        int combo_ok = 0;
        switch (s->image_profile) {
        case TC_IMG_PROFILE_PREVIEW:
            /* spec §9.1：Preview = YUV422（profile 3）10/12-bit（codec 层已拒
             * 绝 pf0+8bit，此处为纵深防御——手拼 packet 场景） */
            combo_ok = (fh->pixel_format == 0u && fh->profile == 3u &&
                        (fh->bit_depth == 10u || fh->bit_depth == 12u));
            break;
        case TC_IMG_PROFILE_HQ:
            combo_ok = (fh->pixel_format == 2u && fh->bit_depth == 10u &&
                        fh->profile == 5u);
            break;
        case TC_IMG_PROFILE_XQ:
            combo_ok = (fh->pixel_format == 2u && fh->bit_depth == 12u &&
                        (fh->profile == 5u || fh->profile == 6u));
            break;
        case TC_IMG_PROFILE_RAW:
            /* TRAW（批 1/批 4）：CFA 4 相位平面 + codec profile 7 +
             * 12-bit LOG0 制作模式 / 16-bit linear 归档模式（批 4 内核
             * 加宽 + 传递曲线冻结对 12→LOG0 / 16→linear） */
            combo_ok = (fh->pixel_format == 3u &&
                        (fh->bit_depth == 12u || fh->bit_depth == 16u) &&
                        fh->profile == 7u);
            break;
        case TC_IMG_PROFILE_HALF_FLOAT:
            /* HALF（spec §15，2026-09-19）：GBR 4:4:4 float16 = pf2 + bd16 +
             * codec profile 5 + sample_kind=HALF。码流本身是合法 bd16 GBR
             * 整数流（老工具按整数解读得到单调码值图，可逆不损坏）；
             * sample_kind 联动由 tci_idsc_validate 的双向规则钉死。 */
            combo_ok = (fh->pixel_format == 2u && fh->bit_depth == 16u &&
                        fh->profile == 5u &&
                        s->sample_kind == TC_IMG_SAMPLE_KIND_HALF);
            break;
        default: /* reserved/越界已在 tci_idsc_validate 拒绝；防御性拒绝 */
            combo_ok = 0;
            break;
        }
        if (!combo_ok) {
            tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT,
                         "image_profile %u != codec combo (pf=%u bit_depth=%u profile=%u)",
                         (unsigned)s->image_profile, (unsigned)fh->pixel_format,
                         (unsigned)fh->bit_depth, (unsigned)fh->profile);
            return TC_IMG_ERR_CHUNK_CONFLICT;
        }
    }
    if (fh->alpha_mode != s->alpha_mode || fh->alpha_bit_depth != s->alpha_bit_depth) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "alpha mode/depth != IDSC");
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }
    const uint32_t premul = (fh->flags & 1u) ? TC_IMG_ALPHA_PREMULT : TC_IMG_ALPHA_STRAIGHT;
    if (s->alpha_presence != TC_IMG_ALPHA_ABSENT && s->alpha_presence != premul) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "alpha association != IDSC");
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }
    if (fh->color_range != s->color_range || fh->color_primaries != s->color_primaries ||
        fh->color_transfer != s->color_transfer || fh->color_matrix != s->color_matrix ||
        fh->chroma_siting != s->chroma_siting) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "color tags != IDSC");
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }
    /* sar ↔ pixel_aspect：0/0 = 未指定 ≡ 方形像素 1/1（spec §6.1-8）；非零必须精确相等 */
    {
        const uint32_t want_num = (fh->sar_num == 0u && fh->sar_den == 0u) ? 1u : fh->sar_num;
        const uint32_t want_den = (fh->sar_num == 0u && fh->sar_den == 0u) ? 1u : fh->sar_den;
        if (want_num != s->pixel_aspect_num || want_den != s->pixel_aspect_den) {
            tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "sar %ux%u != IDSC pixel_aspect %ux%u",
                         (unsigned)fh->sar_num, (unsigned)fh->sar_den,
                         (unsigned)s->pixel_aspect_num, (unsigned)s->pixel_aspect_den);
            return TC_IMG_ERR_CHUNK_CONFLICT;
        }
    }
    /* 绑定摘要：内层 header_crc32 字段（序列化偏移 49..52；decode 已校验其
     * 对前 49B 的正确性），与 IDSC 记录值精确相等 */
    {
        const uint32_t stored_crc =
            ((uint32_t)raw[49] << 24) | ((uint32_t)raw[50] << 16) |
            ((uint32_t)raw[51] << 8) | raw[52];
        if (stored_crc != s->payload_header_crc32) {
            tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT,
                         "payload_header_crc32 %08x != IDSC binding digest %08x",
                         (unsigned)stored_crc, (unsigned)s->payload_header_crc32);
            return TC_IMG_ERR_CHUNK_CONFLICT;
        }
    }
    if (fh->version_major != s->payload_major || fh->version_minor != s->payload_minor) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "payload version != IDSC");
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }
    /* channel_model ↔ pixel_format；channel_count ↔ plane_count（内层
     * plane_count 已含 alpha 平面：3 + alpha） */
    const uint8_t want_model =
        (fh->pixel_format == 2u) ? TC_IMG_CHANNEL_MODEL_RGB
                                 : (fh->pixel_format == 3u)
                                       ? TC_IMG_CHANNEL_MODEL_CFA
                                       : TC_IMG_CHANNEL_MODEL_YUV;
    if (s->channel_model != want_model) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "channel_model %u != pixel_format %u implies %u",
                     (unsigned)s->channel_model, (unsigned)fh->pixel_format,
                     (unsigned)want_model);
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }
    const uint8_t has_alpha = (fh->alpha_mode != 0u) ? 1u : 0u;
    if (s->channel_count != fh->plane_count) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "channel_count %u != plane_count %u",
                     (unsigned)s->channel_count, (unsigned)fh->plane_count);
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }
    if ((has_alpha != 0u) != (s->alpha_presence != TC_IMG_ALPHA_ABSENT ? 1u : 0u)) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "alpha presence mismatch");
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }
    return TC_OK;
}
