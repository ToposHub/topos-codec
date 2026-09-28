/* image_writer —— TPIM envelope 序列化（spec §4.6/§12；确定性输出）。
 *
 * 布局：preamble(64B 占位) → chunks（IDSC、PIXL、extras，按序）→ directory。
 * 写前做 IDSC 全字段校验 + 与 PIXL 内层 header 的十项交叉校验（编码器无法
 * 产出语义冲突文件）；sink 失败返回 TC_ERR_IO，完整性与落盘由调用方
 * （临时文件 + rename 原子替换，spec §12）保证。
 */
#include <string.h>

#include "image/image_container.h"

#include "common/alloc.h"
#include "common/checked.h"
#include "common/crc32.h"
#include "common/error.h"

/* sink 帮助 */
static int32_t tci_sink_write(const topos_io* sink, const void* data, size_t n)
{
    if (sink == NULL || sink->write == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "image write: sink write missing");
        return TC_ERR_INVALID_ARGUMENT;
    }
    const int32_t rc = sink->write(sink->ctx, data, n);
    if (rc != TC_OK) {
        tc_set_error(TC_ERR_IO, "image sink write(%zu) failed: %d", n, (int)rc);
        return TC_ERR_IO;
    }
    return TC_OK;
}

static int32_t tci_sink_seek_write(const topos_io* sink, uint64_t off, const void* data,
                                   size_t n)
{
    if (sink->seek_write == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "image write: sink seek_write missing");
        return TC_ERR_INVALID_ARGUMENT;
    }
    const int32_t rc = sink->seek_write(sink->ctx, off, data, n);
    if (rc != TC_OK) {
        tc_set_error(TC_ERR_IO, "image sink seek_write(%llu) failed: %d",
                     (unsigned long long)off, (int)rc);
        return TC_ERR_IO;
    }
    return TC_OK;
}

/* 从 PIXL payload 解析内层 header（writer 侧交叉校验入口） */
static int32_t tci_parse_inner_header(const void* pixl, size_t size, topos_frame_header* fh)
{
    if (size < TC_FRAME_HEADER_SIZE) {
        tc_set_error(TC_ERR_TRUNCATED, "PIXL shorter than frame header (%zu)", size);
        return TC_ERR_TRUNCATED;
    }
    return tc_frame_header_decode((const uint8_t*)pixl, TC_FRAME_HEADER_SIZE, fh);
}

int32_t tc_image_write(const topos_image_write_params* params, const topos_io* sink,
                       uint64_t* out_size)
{
    if (params == NULL || params->struct_size != (uint32_t)sizeof(topos_image_write_params) ||
        params->abi_version != TOPOS_IMAGE_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "image write: bad params");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (params->pixl_data == NULL || params->pixl_size == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "image write: PIXL empty");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (params->pixl_size > TC_IMG_MAX_CHUNK_PIXL) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "PIXL %zu > 256 MiB", params->pixl_size);
        return TC_ERR_LIMIT_EXCEEDED;
    }
    if (params->extra_count > TC_IMG_MAX_CHUNKS - 2u) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "extra_count %u too large", params->extra_count);
        return TC_ERR_LIMIT_EXCEEDED;
    }
    if (out_size == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "image write: null out_size");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* reserved 不承载功能的位必须为 0（与 codec P1-10 同风格）——否则未来
     * 启用新字段时旧调用方的误用会被静默吞掉 */
    for (uint32_t i = 0; i < 8u; ++i) {
        if (params->reserved[i] != 0u) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "write params reserved[%u] != 0", i);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    /* compatibility_flags：writer 固定产出 minor=0，本 minor 只定义 bit0/bit1
     * （spec §9.3）。不校验会写出自家 reader 直接 BAD_PREAMBLE 的文件
     * （审计 V4：传 0xFF 写出成功、probe 失败）。 */
    if (params->compatibility_flags & ~0x3u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "compatibility_flags %#x has undefined bits (minor=0 mask 0x3)",
                     (unsigned)params->compatibility_flags);
        return TC_ERR_INVALID_ARGUMENT;
    }

    const topos_image_idsc* idsc = &params->idsc;

    /* extras 预检：禁止 IDSC/PIXL/预留 FourCC；必须 optional；known 上限 */
    for (uint32_t i = 0; i < params->extra_count; ++i) {
        const topos_image_chunk_in* c = &params->extra_chunks[i];
        if (c->chunk_type == TC_IMG_CHUNK_IDSC || c->chunk_type == TC_IMG_CHUNK_PIXL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "extra[%u] duplicates critical chunk", i);
            return TC_ERR_INVALID_ARGUMENT;
        }
        if (tci_chunk_reserved(c->chunk_type)) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "extra[%u] reserved FourCC (v1)", i);
            return TC_ERR_INVALID_ARGUMENT;
        }
        if (!(c->chunk_flags & TC_IMG_CHUNK_FLAG_OPTIONAL) ||
            (c->chunk_flags & TC_IMG_CHUNK_FLAG_CRITICAL)) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "extra[%u] must be optional", i);
            return TC_ERR_INVALID_ARGUMENT;
        }
        if (c->chunk_flags & ~0xFu) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "extra[%u] undefined flag bits", i);
            return TC_ERR_INVALID_ARGUMENT;
        }
        if (c->data == NULL || c->size == 0u) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "extra[%u] empty", i);
            return TC_ERR_INVALID_ARGUMENT;
        }
        uint64_t limit = 0;
        if (tci_chunk_limit(c->chunk_type, &limit)) {
            if ((uint64_t)c->size > limit) {
                tc_set_error(TC_ERR_LIMIT_EXCEEDED,
                             "extra[%u] size %zu > limit %llu", i, c->size,
                             (unsigned long long)limit);
                return TC_ERR_LIMIT_EXCEEDED;
            }
        } else {
            /* 未知 optional chunk：保留通行，但设全局上限防滥用（spec §10） */
            if ((uint64_t)c->size > TC_IMG_MAX_CHUNK_XMP) {
                tc_set_error(TC_ERR_LIMIT_EXCEEDED, "extra[%u] unknown chunk too large", i);
                return TC_ERR_LIMIT_EXCEEDED;
            }
        }
        /* 受限内容校验（spec §7；与读端同规则） */
        if (c->chunk_type == TC_IMG_CHUNK_OCIO) {
            const int32_t vrc = tci_validate_ocio((const uint8_t*)c->data, c->size);
            if (vrc != TC_OK) { return vrc; }
        }
        if (c->chunk_type == TC_IMG_CHUNK_ICCP) {
            const int32_t vrc = tci_validate_iccp((const uint8_t*)c->data, c->size,
                                                  idsc->channel_model);
            if (vrc != TC_OK) { return vrc; }
        }
    }

    /* extras 之间禁止重复 FourCC（spec §2.9"同一语义只允许一个权威 chunk"；
     * 审计 V6：双 XMP 曾可写出，读端 read_metadata 只回第一个，第二个语义
     * 不可达）。重复 critical 由读端拒绝，重复 optional 在写端拒绝。 */
    for (uint32_t i = 0; i < params->extra_count; ++i) {
        for (uint32_t j = i + 1u; j < params->extra_count; ++j) {
            if (params->extra_chunks[j].chunk_type ==
                params->extra_chunks[i].chunk_type) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT,
                             "extra[%u]/extra[%u] duplicate FourCC '%c%c%c%c'",
                             i, j,
                             (int)(params->extra_chunks[i].chunk_type >> 24),
                             (int)(params->extra_chunks[i].chunk_type >> 16),
                             (int)(params->extra_chunks[i].chunk_type >> 8),
                             (int)params->extra_chunks[i].chunk_type);
                return TC_ERR_INVALID_ARGUMENT;
            }
        }
    }

    /* 引用-存在一致性：同一语义单一权威（spec §7） */
    int has_iccp = 0;
    int has_ocio = 0;
    for (uint32_t i = 0; i < params->extra_count; ++i) {
        if (params->extra_chunks[i].chunk_type == TC_IMG_CHUNK_ICCP) { has_iccp = 1; }
        if (params->extra_chunks[i].chunk_type == TC_IMG_CHUNK_OCIO) { has_ocio = 1; }
    }
    if ((idsc->iccp_ref != 0u) != (has_iccp != 0)) {
        tc_set_error(TC_IMG_ERR_METADATA_CONFLICT,
                     "iccp_ref=%u but ICCP chunk %s", (unsigned)idsc->iccp_ref,
                     has_iccp ? "present" : "missing");
        return TC_IMG_ERR_METADATA_CONFLICT;
    }
    if ((idsc->ocio_ref != 0u) != (has_ocio != 0)) {
        tc_set_error(TC_IMG_ERR_METADATA_CONFLICT,
                     "ocio_ref=%u but OCIO chunk %s", (unsigned)idsc->ocio_ref,
                     has_ocio ? "present" : "missing");
        return TC_IMG_ERR_METADATA_CONFLICT;
    }

    /* IDSC 字段校验 + 与 PIXL 的十项交叉校验（含绑定摘要） */
    int32_t rc = tci_idsc_validate(idsc);
    if (rc != TC_OK) { return rc; }
    topos_frame_header fh;
    rc = tci_parse_inner_header(params->pixl_data, params->pixl_size, &fh);
    if (rc != TC_OK) { return rc; }
    rc = tci_idsc_crosscheck(idsc, &fh, (const uint8_t*)params->pixl_data);
    if (rc != TC_OK) { return rc; }

    /* 布局计算：64(preamble) | IDSC | PIXL | extras | directory */
    const uint64_t idsc_off = TC_IMG_PREAMBLE_SIZE;
    uint64_t pixl_off = 0;
    if (!tc_uadd_u64(idsc_off, TC_IMG_IDSC_SIZE, &pixl_off)) {
        return TC_ERR_LIMIT_EXCEEDED;
    }
    uint64_t dir_off = 0;
    if (!tc_uadd_u64(pixl_off, params->pixl_size, &dir_off)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "envelope layout overflow");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    for (uint32_t i = 0; i < params->extra_count; ++i) {
        if (!tc_uadd_u64(dir_off, (uint64_t)params->extra_chunks[i].size, &dir_off) ||
            dir_off > 0xFFFFFFFFFFFFFF00ull) {
            tc_set_error(TC_ERR_LIMIT_EXCEEDED, "envelope layout overflow at extra %u", i);
            return TC_ERR_LIMIT_EXCEEDED;
        }
    }
    uint64_t total = 0;
    if (!tc_umul_u64((uint64_t)params->extra_count + 2u, TC_IMG_DIR_ENTRY_SIZE, &total) ||
        !tc_uadd_u64(dir_off, total, &total)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "envelope size overflow");
        return TC_ERR_LIMIT_EXCEEDED;
    }

    /* 1. preamble 占位（全 0，最后回填） */
    static const uint8_t kZero[TC_IMG_PREAMBLE_SIZE] = {0};
    rc = tci_sink_write(sink, kZero, sizeof(kZero));
    if (rc != TC_OK) { return rc; }

    /* 2. IDSC chunk */
    uint8_t idsc_bytes[TC_IMG_IDSC_SIZE];
    rc = tci_pack_idsc(idsc, idsc_bytes);
    if (rc != TC_OK) { return rc; }
    rc = tci_sink_write(sink, idsc_bytes, sizeof(idsc_bytes));
    if (rc != TC_OK) { return rc; }

    /* 3. PIXL chunk（原样嵌入） */
    rc = tci_sink_write(sink, params->pixl_data, params->pixl_size);
    if (rc != TC_OK) { return rc; }

    /* 4. extras（仅写数据；目录项统一在步骤 5 构建） */
    for (uint32_t i = 0; i < params->extra_count; ++i) {
        const topos_image_chunk_in* c = &params->extra_chunks[i];
        rc = tci_sink_write(sink, c->data, c->size);
        if (rc != TC_OK) { return rc; }
    }

    /* 5. directory（IDSC、PIXL、extras 顺序，与写盘顺序一致 → 升序） */
    const uint32_t count = params->extra_count + 2u;
    const size_t dir_alloc = (size_t)count * TC_IMG_DIR_ENTRY_SIZE;
    uint8_t* dir_bytes = (uint8_t*)tc_alloc(dir_alloc); /* ≤ 2 KiB */
    if (dir_bytes == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "directory build alloc");
        return TC_ERR_OUT_OF_MEMORY;
    }
    topos_image_dir_entry e;
    /* IDSC/PIXL entry 重新打包进目录缓冲 */
    memset(&e, 0, sizeof(e));
    e.chunk_type = TC_IMG_CHUNK_IDSC;
    e.chunk_flags = TC_IMG_CHUNK_FLAG_CRITICAL;
    e.chunk_offset = idsc_off;
    e.chunk_size = TC_IMG_IDSC_SIZE;
    e.chunk_crc32 = tc_crc32(idsc_bytes, sizeof(idsc_bytes));
    tci_pack_dir_entry(&e, dir_bytes);
    memset(&e, 0, sizeof(e));
    e.chunk_type = TC_IMG_CHUNK_PIXL;
    e.chunk_flags = TC_IMG_CHUNK_FLAG_CRITICAL;
    e.chunk_offset = pixl_off;
    e.chunk_size = (uint64_t)params->pixl_size;
    e.chunk_crc32 = tc_crc32(params->pixl_data, params->pixl_size);
    tci_pack_dir_entry(&e, dir_bytes + TC_IMG_DIR_ENTRY_SIZE);
    for (uint32_t i = 0; i < params->extra_count; ++i) {
        const topos_image_chunk_in* c = &params->extra_chunks[i];
        uint64_t off = pixl_off + params->pixl_size;
        for (uint32_t j = 0; j < i; ++j) {
            off += (uint64_t)params->extra_chunks[j].size;
        }
        memset(&e, 0, sizeof(e));
        e.chunk_type = c->chunk_type;
        e.chunk_flags = c->chunk_flags;
        e.chunk_offset = off;
        e.chunk_size = (uint64_t)c->size;
        e.chunk_crc32 = tc_crc32(c->data, c->size);
        tci_pack_dir_entry(&e, dir_bytes + (size_t)(i + 2) * TC_IMG_DIR_ENTRY_SIZE);
    }
    const uint32_t dir_crc = tc_crc32(dir_bytes, dir_alloc);
    rc = tci_sink_write(sink, dir_bytes, dir_alloc);
    tc_free(dir_bytes);
    if (rc != TC_OK) { return rc; }

    /* 6. 回填 preamble */
    topos_image_preamble pre;
    memset(&pre, 0, sizeof(pre));
    pre.struct_size = (uint32_t)sizeof(pre);
    pre.abi_version = TOPOS_IMAGE_ABI_VERSION;
    pre.file_version_major = 1u;
    pre.file_version_minor = 0u;
    pre.file_size = total;
    pre.directory_offset = dir_off;
    pre.directory_entry_size = TC_IMG_DIR_ENTRY_SIZE;
    pre.directory_count = count;
    pre.primary_image_index = 0u;
    pre.compatibility_flags = params->compatibility_flags;
    pre.directory_crc32 = dir_crc;
    uint8_t pre_bytes[TC_IMG_PREAMBLE_SIZE];
    rc = tci_pack_preamble(&pre, pre_bytes);
    if (rc != TC_OK) { return rc; }
    rc = tci_sink_seek_write(sink, 0, pre_bytes, sizeof(pre_bytes));
    if (rc != TC_OK) { return rc; }

    *out_size = total;
    return TC_OK;
}
