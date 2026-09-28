/* image_reader —— TPIM probe / validate / decode（spec §2.11 校验顺序冻结）。
 *
 * 安全边界：
 *  - probe 只允许 ≤ 64×32(directory) + 128(IDSC) + 53(packet header) 的有界读取
 *    与同量级临时分配，禁止像素级分配；
 *  - validate 逐 chunk 以 64 KiB 流式 CRC，不整块分配；
 *  - decode 整读 PIXL（≤ 256 MiB 上限）后交给 codec；preview 走 target-size
 *    reconstruction，不建立完整源平面。
 */
#include <string.h>

#include "image/image_container.h"

#include "common/alloc.h"
#include "common/checked.h"
#include "common/crc32.h"
#include "common/error.h"

#define TCI_IO_BUF_SIZE (64u * 1024u)

const char* tc_image_status_message(int32_t status)
{
    switch (status) {
    case TC_OK: return "ok";
    case TC_WARN_CONCEALED: return "ok (concealed slices)";
    case TC_ERR_INVALID_ARGUMENT: return "invalid argument";
    case TC_ERR_OUT_OF_MEMORY: return "out of memory";
    case TC_ERR_UNSUPPORTED_VERSION: return "unsupported version";
    case TC_ERR_UNSUPPORTED_PROFILE: return "unsupported profile";
    case TC_ERR_UNSUPPORTED_PIXEL_FORMAT: return "unsupported pixel format";
    case TC_ERR_UNSUPPORTED_MATRIX: return "unsupported matrix";
    case TC_ERR_UNSUPPORTED_ALPHA_MODE: return "unsupported alpha mode";
    case TC_ERR_LIMIT_EXCEEDED: return "limit exceeded";
    case TC_ERR_MALFORMED: return "malformed data";
    case TC_ERR_TRUNCATED: return "truncated data";
    case TC_ERR_CHECKSUM_MISMATCH: return "checksum mismatch";
    case TC_ERR_STATE: return "invalid state";
    case TC_ERR_CANCELLED: return "cancelled";
    case TC_ERR_IO: return "io error";
    case TC_ERR_BUFFER_TOO_SMALL: return "buffer too small";
    case TC_ERR_NOT_IMPLEMENTED: return "not implemented";
    case TC_IMG_ERR_BAD_MAGIC: return "bad magic (not TPIM)";
    case TC_IMG_ERR_BAD_PREAMBLE: return "bad preamble";
    case TC_IMG_ERR_BAD_DIRECTORY: return "bad chunk directory";
    case TC_IMG_ERR_UNKNOWN_CRITICAL: return "unknown critical chunk";
    case TC_IMG_ERR_CHUNK_CONFLICT: return "IDSC/PIXL conflict";
    case TC_IMG_ERR_METADATA_CONFLICT: return "color metadata conflict";
    case TC_IMG_ERR_LIMIT: return "image limit exceeded";
    case TC_IMG_ERR_IO_WRITE_FAILED: return "atomic write failed";
    default: return "unknown status";
    }
}

/* io 帮助：短读/失败统一 TC_ERR_IO */
static int32_t tci_read_at(const topos_io* io, uint64_t off, void* buf, size_t n)
{
    if (io == NULL || io->read == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "image io: read callback missing");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (n == 0u) { return TC_OK; }
    const int32_t rc = io->read(io->ctx, off, buf, n);
    if (rc != TC_OK) {
        tc_set_error(TC_ERR_IO, "image read_at(%llu,%zu) failed: %d",
                     (unsigned long long)off, n, (int)rc);
        return TC_ERR_IO;
    }
    return TC_OK;
}

/* 已知 critical chunk 判定（v1：IDSC/PIXL） */
static int chunk_is_critical_known(uint32_t type)
{
    return type == TC_IMG_CHUNK_IDSC || type == TC_IMG_CHUNK_PIXL;
}

/* 目录扫描共享主体：校验排序/越界/重叠/重复，输出 entry 数组（caller 分配） */
static int32_t tci_scan_directory(const uint8_t* dir_bytes, uint32_t count,
                                  uint64_t file_size, uint64_t dir_offset,
                                  topos_image_dir_entry* entries)
{
    uint64_t prev_end = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t rc = tci_unpack_dir_entry(dir_bytes + (size_t)i * TC_IMG_DIR_ENTRY_SIZE,
                                                &entries[i]);
        if (rc != TC_OK) { return rc; }
        const topos_image_dir_entry* e = &entries[i];

        /* 越界（checked：offset+size 不得回绕/越过 file_size） */
        uint64_t end = 0;
        if (e->chunk_offset < TC_IMG_PREAMBLE_SIZE ||
            !tc_uadd_u64(e->chunk_offset, e->chunk_size, &end) || end > file_size) {
            tc_set_error(TC_IMG_ERR_BAD_DIRECTORY,
                         "chunk '%c%c%c%c' offset/size out of bounds",
                         (int)(e->chunk_type >> 24), (int)(e->chunk_type >> 16),
                         (int)(e->chunk_type >> 8), (int)e->chunk_type);
            return TC_IMG_ERR_BAD_DIRECTORY;
        }
        /* 与 directory 区间重叠禁止（spec §2.4：chunk 不得指向 preamble 或
         * directory 自身；双向区间测试——旧单向检查漏掉"chunk 落在 directory
         * 区间内部"的别名情况）。count ≤ 64 → dir_end 无溢出（caller 已验界） */
        const uint64_t dir_end = dir_offset + (uint64_t)count * TC_IMG_DIR_ENTRY_SIZE;
        if (e->chunk_offset < dir_end && end > dir_offset) {
            tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "chunk overlaps directory");
            return TC_IMG_ERR_BAD_DIRECTORY;
        }
        /* 升序 + 不重叠（相邻区间允许紧贴） */
        if (e->chunk_offset < prev_end) {
            tc_set_error(TC_IMG_ERR_BAD_DIRECTORY,
                         "directory not ascending / chunk overlap at entry %u", i);
            return TC_IMG_ERR_BAD_DIRECTORY;
        }
        prev_end = end;
    }
    /* 重复 critical chunk 禁止（spec §4） */
    for (uint32_t i = 0; i < count; ++i) {
        if (!(entries[i].chunk_flags & TC_IMG_CHUNK_FLAG_CRITICAL)) { continue; }
        for (uint32_t j = (uint32_t)(i + 1); j < count; ++j) {
            if ((entries[j].chunk_flags & TC_IMG_CHUNK_FLAG_CRITICAL) &&
                entries[j].chunk_type == entries[i].chunk_type) {
                tc_set_error(TC_IMG_ERR_BAD_DIRECTORY,
                             "duplicate critical chunk '%c%c%c%c'",
                             (int)(entries[i].chunk_type >> 24),
                             (int)(entries[i].chunk_type >> 16),
                             (int)(entries[i].chunk_type >> 8),
                             (int)entries[i].chunk_type);
                return TC_IMG_ERR_BAD_DIRECTORY;
            }
        }
    }
    return TC_OK;
}

/* probe 主体（validate 复用） */
static int32_t tci_probe_impl(const topos_io* io, topos_image_info* out)
{
    if (out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "image probe: null out");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->abi_version = TOPOS_IMAGE_ABI_VERSION;

    if (io == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "image probe: null io");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (io->length < TC_IMG_PREAMBLE_SIZE) {
        tc_set_error(TC_ERR_TRUNCATED, "file shorter than preamble (%llu)",
                     (unsigned long long)io->length);
        return TC_ERR_TRUNCATED;
    }

    uint8_t pre[TC_IMG_PREAMBLE_SIZE];
    int32_t rc = tci_read_at(io, 0, pre, sizeof(pre));
    if (rc != TC_OK) { return rc; }
    rc = tci_unpack_preamble(pre, &out->preamble);
    if (rc != TC_OK) { return rc; }
    if (out->preamble.file_size != io->length) {
        tc_set_error(TC_IMG_ERR_BAD_PREAMBLE, "preamble file_size %llu != actual %llu",
                     (unsigned long long)out->preamble.file_size,
                     (unsigned long long)io->length);
        return TC_IMG_ERR_BAD_PREAMBLE;
    }

    /* directory 越界（checked）：end = directory_offset + count×32 */
    uint64_t dir_len = 0;
    uint64_t dir_end = 0;
    if (out->preamble.directory_offset < TC_IMG_PREAMBLE_SIZE ||
        !tc_umul_u64(out->preamble.directory_count, TC_IMG_DIR_ENTRY_SIZE, &dir_len) ||
        !tc_uadd_u64(out->preamble.directory_offset, dir_len, &dir_end) ||
        dir_end > out->preamble.file_size) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "directory out of bounds");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }

    uint8_t* dir_bytes = (uint8_t*)tc_alloc((size_t)dir_len); /* ≤ 2 KiB */
    if (dir_bytes == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "directory alloc (%llu)",
                     (unsigned long long)dir_len);
        return TC_ERR_OUT_OF_MEMORY;
    }
    rc = tci_read_at(io, out->preamble.directory_offset, dir_bytes, (size_t)dir_len);
    if (rc != TC_OK) { tc_free(dir_bytes); return rc; }
    if (tc_crc32(dir_bytes, (size_t)dir_len) != out->preamble.directory_crc32) {
        tc_free(dir_bytes);
        tc_set_error(TC_IMG_ERR_BAD_PREAMBLE, "directory_crc32 mismatch");
        return TC_IMG_ERR_BAD_PREAMBLE;
    }

    topos_image_dir_entry entries[TC_IMG_MAX_CHUNKS];
    rc = tci_scan_directory(dir_bytes, out->preamble.directory_count,
                            out->preamble.file_size, out->preamble.directory_offset,
                            entries);
    tc_free(dir_bytes);
    if (rc != TC_OK) { return rc; }

    /* 必需 chunk 存在性 + 未知 critical 拒绝 + 已知 optional 长度上限 */
    uint32_t idsc_index = TC_IMG_MAX_CHUNKS;
    uint32_t pixl_index = TC_IMG_MAX_CHUNKS;
    int has_iccp = 0;
    int has_ocio = 0;
    for (uint32_t i = 0; i < out->preamble.directory_count; ++i) {
        const topos_image_dir_entry* e = &entries[i];
        const int critical = (e->chunk_flags & TC_IMG_CHUNK_FLAG_CRITICAL) != 0;
        if (critical && !chunk_is_critical_known(e->chunk_type)) {
            tc_set_error(TC_IMG_ERR_UNKNOWN_CRITICAL, "unknown critical chunk '%c%c%c%c'",
                         (int)(e->chunk_type >> 24), (int)(e->chunk_type >> 16),
                         (int)(e->chunk_type >> 8), (int)e->chunk_type);
            return TC_IMG_ERR_UNKNOWN_CRITICAL;
        }
        uint64_t limit = 0;
        if (tci_chunk_limit(e->chunk_type, &limit) && e->chunk_size > limit) {
            tc_set_error(TC_IMG_ERR_LIMIT, "chunk '%c%c%c%c' size %llu > limit %llu",
                         (int)(e->chunk_type >> 24), (int)(e->chunk_type >> 16),
                         (int)(e->chunk_type >> 8), (int)e->chunk_type,
                         (unsigned long long)e->chunk_size, (unsigned long long)limit);
            return TC_IMG_ERR_LIMIT;
        }
        if (e->chunk_type == TC_IMG_CHUNK_IDSC) {
            idsc_index = i;
        } else if (e->chunk_type == TC_IMG_CHUNK_PIXL) {
            pixl_index = i;
        } else if (e->chunk_type == TC_IMG_CHUNK_ICCP) {
            has_iccp = 1;
        } else if (e->chunk_type == TC_IMG_CHUNK_OCIO) {
            has_ocio = 1;
        }
    }
    if (idsc_index == TC_IMG_MAX_CHUNKS || pixl_index == TC_IMG_MAX_CHUNKS) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "missing required chunk (IDSC/PIXL)");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }

    /* 引用-存在一致性占位：需先解出 IDSC 后与 iccp_ref/ocio_ref 比对
     * （见下方 has_iccp/has_ocio 使用点） */

    /* IDSC（固定 128B） */
    if (entries[idsc_index].chunk_size != TC_IMG_IDSC_SIZE) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "IDSC chunk size %llu != 128",
                     (unsigned long long)entries[idsc_index].chunk_size);
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    uint8_t idsc_bytes[TC_IMG_IDSC_SIZE];
    rc = tci_read_at(io, entries[idsc_index].chunk_offset, idsc_bytes, sizeof(idsc_bytes));
    if (rc != TC_OK) { return rc; }
    rc = tci_unpack_idsc(idsc_bytes, sizeof(idsc_bytes), &out->idsc);
    if (rc != TC_OK) { return rc; }

    /* 引用-存在一致性：同一语义单一权威（spec §7） */
    if ((out->idsc.iccp_ref != 0u) != has_iccp) {
        tc_set_error(TC_IMG_ERR_METADATA_CONFLICT,
                     "iccp_ref=%u but ICCP chunk %s", (unsigned)out->idsc.iccp_ref,
                     has_iccp ? "present" : "missing");
        return TC_IMG_ERR_METADATA_CONFLICT;
    }
    if ((out->idsc.ocio_ref != 0u) != has_ocio) {
        tc_set_error(TC_IMG_ERR_METADATA_CONFLICT,
                     "ocio_ref=%u but OCIO chunk %s", (unsigned)out->idsc.ocio_ref,
                     has_ocio ? "present" : "missing");
        return TC_IMG_ERR_METADATA_CONFLICT;
    }

    /* 内层 packet header（53B）+ 交叉校验 */
    out->pixl_offset = entries[pixl_index].chunk_offset;
    out->pixl_size = entries[pixl_index].chunk_size;
    if (out->pixl_size > TC_IMG_MAX_CHUNK_PIXL) {
        tc_set_error(TC_IMG_ERR_LIMIT, "PIXL size %llu > 256 MiB",
                     (unsigned long long)out->pixl_size);
        return TC_IMG_ERR_LIMIT;
    }
    if (out->pixl_size < TC_FRAME_HEADER_SIZE) {
        tc_set_error(TC_ERR_TRUNCATED, "PIXL shorter than frame header");
        return TC_ERR_TRUNCATED;
    }
    uint8_t fh_bytes[TC_FRAME_HEADER_SIZE];
    rc = tci_read_at(io, out->pixl_offset, fh_bytes, sizeof(fh_bytes));
    if (rc != TC_OK) { return rc; }
    topos_frame_header fh;
    rc = tc_frame_header_decode(fh_bytes, sizeof(fh_bytes), &fh);
    if (rc != TC_OK) { return rc; } /* codec 错误码语义直传（MALFORMED/TRUNCATED/...） */
    rc = tci_idsc_crosscheck(&out->idsc, &fh, fh_bytes);
    if (rc != TC_OK) { return rc; }
    if ((uint64_t)fh.frame_packet_size != out->pixl_size) {
        tc_set_error(TC_IMG_ERR_CHUNK_CONFLICT, "frame_packet_size %u != PIXL chunk size %llu",
                     (unsigned)fh.frame_packet_size, (unsigned long long)out->pixl_size);
        return TC_IMG_ERR_CHUNK_CONFLICT;
    }

    /* 便捷镜像 */
    out->visible_width = fh.visible_width;
    out->visible_height = fh.visible_height;
    out->plane_count = fh.plane_count;
    out->has_alpha = (fh.alpha_mode != 0u) ? 1u : 0u;
    out->bit_depth = fh.bit_depth;
    out->profile = fh.profile;
    out->pixel_format = fh.pixel_format;
    out->alpha_mode = fh.alpha_mode;
    out->alpha_bit_depth = fh.alpha_bit_depth;
    out->alpha_premultiplied = (fh.flags & 1u) ? 1u : 0u;
    return TC_OK;
}

int32_t tc_image_probe(const topos_io* io, topos_image_info* out)
{
    return tci_probe_impl(io, out);
}

int32_t tc_image_validate(const topos_io* io, uint32_t flags, topos_image_info* out)
{
    const int32_t rc = tci_probe_impl(io, out);
    if (rc != TC_OK) { return rc; }

    /* 重新读取目录字节以拿到全部 entry（probe 未保留 entries） */
    uint64_t dir_len = 0;
    if (!tc_umul_u64(out->preamble.directory_count, TC_IMG_DIR_ENTRY_SIZE, &dir_len)) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "directory bounds (validate)");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    uint8_t* dir_bytes = (uint8_t*)tc_alloc((size_t)dir_len);
    if (dir_bytes == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "validate directory alloc");
        return TC_ERR_OUT_OF_MEMORY;
    }
    int32_t vrc = tci_read_at(io, out->preamble.directory_offset, dir_bytes,
                              (size_t)dir_len);
    if (vrc != TC_OK) { tc_free(dir_bytes); return vrc; }

    uint8_t* buf = (uint8_t*)tc_alloc(TCI_IO_BUF_SIZE);
    if (buf == NULL) {
        tc_free(dir_bytes);
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "validate stream buffer alloc");
        return TC_ERR_OUT_OF_MEMORY;
    }

    const uint32_t count = out->preamble.directory_count;
    for (uint32_t i = 0; i < count; ++i) {
        topos_image_dir_entry e;
        vrc = tci_unpack_dir_entry(dir_bytes + (size_t)i * TC_IMG_DIR_ENTRY_SIZE, &e);
        if (vrc != TC_OK) { break; }
        uint32_t crc = 0;
        uint64_t off = e.chunk_offset;
        uint64_t left = e.chunk_size;
        while (left > 0u) {
            const size_t n = (size_t)(left > TCI_IO_BUF_SIZE ? TCI_IO_BUF_SIZE : left);
            vrc = tci_read_at(io, off, buf, n);
            if (vrc != TC_OK) { goto done; }
            crc = tc_crc32_update(crc, buf, n);
            off += n;
            left -= n;
        }
        if (crc != e.chunk_crc32) {
            tc_set_error(TC_ERR_CHECKSUM_MISMATCH,
                         "chunk '%c%c%c%c' crc mismatch (store %08x calc %08x)",
                         (int)(e.chunk_type >> 24), (int)(e.chunk_type >> 16),
                         (int)(e.chunk_type >> 8), (int)e.chunk_type,
                         (unsigned)e.chunk_crc32, (unsigned)crc);
            vrc = TC_ERR_CHECKSUM_MISMATCH;
            break;
        }
    }
done:
    tc_free(buf);
    tc_free(dir_bytes);
    if (vrc != TC_OK) { return vrc; }

    if (flags & TC_IMG_VALIDATE_DEEP) {
        /* DEEP：整读 PIXL 并对内层 packet 做结构探测（不重建像素） */
        uint8_t* pixl = (uint8_t*)tc_alloc((size_t)out->pixl_size);
        if (pixl == NULL) {
            tc_set_error(TC_ERR_OUT_OF_MEMORY, "validate deep PIXL alloc (%llu)",
                         (unsigned long long)out->pixl_size);
            return TC_ERR_OUT_OF_MEMORY;
        }
        const int32_t rrc = tci_read_at(io, out->pixl_offset, pixl, (size_t)out->pixl_size);
        if (rrc != TC_OK) { tc_free(pixl); return rrc; }
        topos_frame_output info;
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t)sizeof(info);
        info.abi_version = TOPOS_CODEC_ABI_VERSION;
        const int32_t drc = tc_frame_decode(pixl, (size_t)out->pixl_size, NULL, NULL, &info);
        tc_free(pixl);
        if (drc != TC_OK) { return drc; }
    }
    return TC_OK;
}

int32_t tc_image_query_decode_buffer(const topos_image_info* info, uint32_t plane,
                                     uint32_t* width, uint32_t* height)
{
    if (info == NULL || info->struct_size != (uint32_t)sizeof(topos_image_info) ||
        info->abi_version != TOPOS_IMAGE_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "query_decode_buffer: bad info");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* info.plane_count 镜像自内层 header（含 alpha 平面，3+alpha）；几何单源于 codec */
    topos_frame_output fo;
    memset(&fo, 0, sizeof(fo));
    fo.struct_size = (uint32_t)sizeof(fo);
    fo.abi_version = TOPOS_CODEC_ABI_VERSION;
    fo.visible_width = info->visible_width;
    fo.visible_height = info->visible_height;
    fo.plane_count = info->plane_count;
    fo.alpha_mode = info->alpha_mode;
    fo.bit_depth = info->bit_depth;
    fo.pixel_format = info->pixel_format;
    return tc_frame_plane_geometry(&fo, plane, width, height);
}

int32_t tc_image_decode(const topos_io* io,
                        const topos_plane_view planes[TC_FRAME_MAX_PLANES],
                        topos_frame_output* out_info)
{
    topos_image_info info;
    const int32_t prc = tci_probe_impl(io, &info);
    if (prc != TC_OK) { return prc; }

    /* plane view 校验（struct_size/abi_version/pixels） */
    uint16_t* plane_ptrs[TC_FRAME_MAX_PLANES] = {NULL, NULL, NULL, NULL};
    size_t strides[TC_FRAME_MAX_PLANES] = {0, 0, 0, 0};
    if (planes == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "image decode: null planes");
        return TC_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < TC_FRAME_MAX_PLANES; ++i) {
        if (planes[i].struct_size != 0u) {
            /* topos_plane_view 是 codec 域结构（topos_codec.h）：abi 必须是
             * TOPOS_CODEC_ABI_VERSION。曾有三个调用方误填 TOPOS_IMAGE_ABI_
             * VERSION（=1）未被发现——本检查把该类错误从潜伏地雷变成
             * 显式拒绝。struct_size==0 的全零视图保持历史宽容（pixels
             * 为 NULL 时本就跳过）。 */
            if (planes[i].struct_size != (uint32_t)sizeof(topos_plane_view)) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT, "image decode: plane %u struct_size", i);
                return TC_ERR_INVALID_ARGUMENT;
            }
            if (planes[i].abi_version != TOPOS_CODEC_ABI_VERSION) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT,
                             "image decode: plane %u abi_version %u != codec %u "
                             "(topos_plane_view 是 codec 域结构)",
                             i, (unsigned)planes[i].abi_version,
                             (unsigned)TOPOS_CODEC_ABI_VERSION);
                return TC_ERR_INVALID_ARGUMENT;
            }
        }
        if (planes[i].pixels == NULL) { continue; }
        plane_ptrs[i] = planes[i].pixels;
        strides[i] = planes[i].stride;
    }

    /* 整读 PIXL（有界 ≤ 256 MiB；上界已在 probe 校验） */
    uint8_t* pixl = (uint8_t*)tc_alloc((size_t)info.pixl_size);
    if (pixl == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "image decode PIXL alloc (%llu)",
                     (unsigned long long)info.pixl_size);
        return TC_ERR_OUT_OF_MEMORY;
    }
    const int32_t rrc = tci_read_at(io, info.pixl_offset, pixl, (size_t)info.pixl_size);
    if (rrc != TC_OK) { tc_free(pixl); return rrc; }

    if (out_info != NULL) {
        memset(out_info, 0, sizeof(*out_info));
        out_info->struct_size = (uint32_t)sizeof(*out_info);
        out_info->abi_version = TOPOS_CODEC_ABI_VERSION;
    }
    const int32_t drc = tc_frame_decode(pixl, (size_t)info.pixl_size,
                                        (uint16_t* const*)plane_ptrs, strides, out_info);
    tc_free(pixl);
    return drc;
}

static int32_t tci_validate_preview_view(const topos_plane_view* view,
                                         uint32_t plane, uint32_t width)
{
    if (view == NULL || view->struct_size != (uint32_t)sizeof(topos_plane_view) ||
        view->abi_version != TOPOS_CODEC_ABI_VERSION || view->pixels == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "image preview: plane %u view is invalid", (unsigned)plane);
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (view->stride != 0u && view->stride < (size_t)width) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "image preview: plane %u stride %zu < width %u",
                     (unsigned)plane, view->stride, (unsigned)width);
        return TC_ERR_INVALID_ARGUMENT;
    }
    return TC_OK;
}

int32_t tc_image_decode_preview(const topos_io* io,
                                uint32_t target_width,
                                uint32_t target_height,
                                const topos_plane_view planes[TC_FRAME_MAX_PLANES],
                                topos_frame_output* out_info)
{
    if (planes == NULL || out_info == NULL || target_width == 0u ||
        target_height == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "image preview: invalid arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }

    topos_image_info info;
    int32_t rc = tci_probe_impl(io, &info);
    if (rc != TC_OK) { return rc; }
    if (target_width > (uint32_t)info.visible_width ||
        target_height > (uint32_t)info.visible_height ||
        target_width > UINT16_MAX || target_height > UINT16_MAX) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "image preview: target %ux%u exceeds source %ux%u",
                     (unsigned)target_width, (unsigned)target_height,
                     (unsigned)info.visible_width, (unsigned)info.visible_height);
        return TC_ERR_INVALID_ARGUMENT;
    }

    uint8_t* pixl = NULL;
    uint16_t* target_planes[TC_FRAME_MAX_PLANES] = {NULL, NULL, NULL, NULL};
    size_t target_strides[TC_FRAME_MAX_PLANES] = {0u, 0u, 0u, 0u};
    int32_t result = TC_OK;
    topos_frame_output target_out;
    memset(&target_out, 0, sizeof(target_out));
    target_out.struct_size = (uint32_t)sizeof(target_out);
    target_out.abi_version = TOPOS_CODEC_ABI_VERSION;

    pixl = (uint8_t*)tc_alloc((size_t)info.pixl_size);
    if (pixl == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "image preview PIXL alloc (%llu)",
                     (unsigned long long)info.pixl_size);
        result = TC_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    rc = tci_read_at(io, info.pixl_offset, pixl, (size_t)info.pixl_size);
    if (rc != TC_OK) { result = rc; goto cleanup; }

    /* 查询一次 packet，只取得目标平面数量/格式。目标严格匹配固定比例时
     * 复用低频 reduced request；任意包围盒仍使用精确 scaled request。 */
    topos_decode_request request;
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint32_t)sizeof(request);
    request.abi_version = TOPOS_CODEC_ABI_VERSION;
    request.mode = TC_DECODE_MODE_SCALED;
    request.target_width = target_width;
    request.target_height = target_height;
    request.memory_type = TC_DECODE_MEMORY_CPU;
    const tc_decode_scale scales[] = {
        TC_DECODE_SCALE_HALF, TC_DECODE_SCALE_THIRD,
        TC_DECODE_SCALE_QUARTER, TC_DECODE_SCALE_EIGHTH
    };
    for (size_t i = 0u; i < sizeof(scales) / sizeof(scales[0]); ++i) {
        uint32_t tw = 0u;
        uint32_t th = 0u;
        if (tc_decode_scale_dimensions(info.visible_width, info.visible_height,
                                       scales[i], &tw, &th) == TC_OK &&
            tw == target_width && th == target_height) {
            request.mode = TC_DECODE_MODE_REDUCED;
            request.scale = scales[i];
            request.target_width = 0u;
            request.target_height = 0u;
            break;
        }
    }
    rc = tc_frame_decode_request(pixl, (size_t)info.pixl_size, &request,
                                 NULL, NULL, &target_out);
    if (rc != TC_OK) { result = rc; goto cleanup; }
    if (target_out.plane_count == 0u || target_out.plane_count > TC_FRAME_MAX_PLANES) {
        tc_set_error(TC_ERR_MALFORMED, "image preview: invalid source plane count");
        result = TC_ERR_MALFORMED;
        goto cleanup;
    }
    for (uint32_t p = 0u; p < target_out.plane_count; ++p) {
        uint32_t width = 0u;
        uint32_t height = 0u;
        rc = tc_frame_plane_geometry(&target_out, p, &width, &height);
        if (rc != TC_OK) { result = rc; goto cleanup; }
        rc = tci_validate_preview_view(&planes[p], p, width);
        if (rc != TC_OK) { result = rc; goto cleanup; }
        target_planes[p] = planes[p].pixels;
        target_strides[p] = planes[p].stride;
    }
    result = tc_frame_decode_request(pixl, (size_t)info.pixl_size, &request,
                                     target_planes, target_strides, &target_out);
    if (result >= 0) { *out_info = target_out; }

cleanup:
    tc_free(pixl);
    return result;
}

/* —— 阶段 3：可选 metadata chunk 受限读取 —— */

/* 在目录中定位 chunk（重新读目录 ≤2KiB，probe 不保留大状态） */
static int32_t tci_locate_chunk(const topos_io* io, const topos_image_info* info,
                                uint32_t chunk_type, topos_image_dir_entry* out_entry)
{
    uint64_t dir_len = 0;
    if (!tc_umul_u64(info->preamble.directory_count, TC_IMG_DIR_ENTRY_SIZE, &dir_len)) {
        tc_set_error(TC_IMG_ERR_BAD_DIRECTORY, "directory bounds (locate)");
        return TC_IMG_ERR_BAD_DIRECTORY;
    }
    uint8_t* dir_bytes = (uint8_t*)tc_alloc((size_t)dir_len);
    if (dir_bytes == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "locate directory alloc");
        return TC_ERR_OUT_OF_MEMORY;
    }
    int32_t rc = tci_read_at(io, info->preamble.directory_offset, dir_bytes,
                             (size_t)dir_len);
    if (rc != TC_OK) { tc_free(dir_bytes); return rc; }
    for (uint32_t i = 0; i < info->preamble.directory_count; ++i) {
        rc = tci_unpack_dir_entry(dir_bytes + (size_t)i * TC_IMG_DIR_ENTRY_SIZE,
                                  out_entry);
        if (rc != TC_OK) { tc_free(dir_bytes); return rc; }
        if (out_entry->chunk_type == chunk_type) {
            tc_free(dir_bytes);
            return TC_OK;
        }
    }
    tc_free(dir_bytes);
    tc_set_error(TC_ERR_INVALID_ARGUMENT, "chunk '%c%c%c%c' not present",
                 (int)(chunk_type >> 24), (int)(chunk_type >> 16),
                 (int)(chunk_type >> 8), (int)chunk_type);
    return TC_ERR_INVALID_ARGUMENT;
}

int32_t tc_image_read_metadata(const topos_io* io, const topos_image_info* info,
                               uint32_t chunk_type, void* buffer, size_t cap,
                               size_t* out_size)
{
    if (io == NULL || info == NULL || out_size == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "read_metadata: null arg");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (info->struct_size != (uint32_t)sizeof(topos_image_info) ||
        info->abi_version != TOPOS_IMAGE_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "read_metadata: bad info");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (chunk_type == TC_IMG_CHUNK_IDSC || chunk_type == TC_IMG_CHUNK_PIXL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "read_metadata: critical chunk");
        return TC_ERR_INVALID_ARGUMENT;
    }
    *out_size = 0;

    topos_image_dir_entry e;
    int32_t rc = tci_locate_chunk(io, info, chunk_type, &e);
    if (rc != TC_OK) { return rc; }
    if ((e.chunk_flags & TC_IMG_CHUNK_FLAG_CRITICAL) ||
        tci_chunk_reserved(e.chunk_type)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "read_metadata: not a readable optional chunk");
        return TC_ERR_INVALID_ARGUMENT;
    }

    uint64_t limit = 0;
    if (tci_chunk_limit(chunk_type, &limit)) {
        if (e.chunk_size > limit) {
            tc_set_error(TC_IMG_ERR_LIMIT, "metadata chunk %llu > limit %llu",
                         (unsigned long long)e.chunk_size, (unsigned long long)limit);
            return TC_IMG_ERR_LIMIT;
        }
    } else if (e.chunk_size > TC_IMG_MAX_CHUNK_XMP) {
        tc_set_error(TC_IMG_ERR_LIMIT, "unknown metadata chunk too large");
        return TC_IMG_ERR_LIMIT;
    }
    *out_size = (size_t)e.chunk_size;
    if (buffer == NULL || cap == 0u) { return TC_OK; } /* 只查询尺寸 */
    if (cap < (size_t)e.chunk_size) {
        tc_set_error(TC_ERR_BUFFER_TOO_SMALL, "metadata cap %zu < %llu",
                     cap, (unsigned long long)e.chunk_size);
        return TC_ERR_BUFFER_TOO_SMALL;
    }

    /* 分块读入（上限 16MiB 内循环，缓冲 caller 提供无需分配） */
    uint64_t off = e.chunk_offset;
    size_t left = (size_t)e.chunk_size;
    uint8_t* dst = (uint8_t*)buffer;
    while (left > 0u) {
        const size_t n = left > TCI_IO_BUF_SIZE ? TCI_IO_BUF_SIZE : left;
        rc = tci_read_at(io, off, dst, n);
        if (rc != TC_OK) { return rc; }
        off += n;
        dst += n;
        left -= n;
    }

    /* 受限内容校验（读端同写端规则；spec §7 / ADR-I003）。失败时复位
     * *out_size——避免调用方只看 out_size 不看返回码时误用半验证数据。 */
    if (chunk_type == TC_IMG_CHUNK_OCIO) {
        const int32_t vrc = tci_validate_ocio((const uint8_t*)buffer,
                                              (size_t)e.chunk_size);
        if (vrc != TC_OK) { *out_size = 0; }
        return vrc;
    }
    if (chunk_type == TC_IMG_CHUNK_ICCP) {
        const int32_t vrc = tci_validate_iccp((const uint8_t*)buffer,
                                              (size_t)e.chunk_size,
                                              info->idsc.channel_model);
        if (vrc != TC_OK) { *out_size = 0; }
        return vrc;
    }
    return TC_OK;
}

/* —— 阶段 4：能力查询 —— */

int32_t tc_image_query_capabilities(topos_image_capabilities* out)
{
    if (out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "query_capabilities: null out");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->abi_version = TOPOS_IMAGE_ABI_VERSION;
    out->file_version_major = 1u;
    out->file_version_minor = 0u;
    out->max_coded_dim = TC_MAX_CODED_DIM;
    out->max_pixl_bytes = TC_IMG_MAX_CHUNK_PIXL;
    out->max_chunks = TC_IMG_MAX_CHUNKS;
    out->profiles_mask = (1u << TC_IMG_PROFILE_PREVIEW) |
                         (1u << TC_IMG_PROFILE_HQ) |
                         (1u << TC_IMG_PROFILE_XQ) |
                         (1u << TC_IMG_PROFILE_RAW) |     /* TRAW（批 1） */
                         (1u << TC_IMG_PROFILE_HALF_FLOAT); /* HALF（2026-09-19） */
    out->metadata_mask = (1u << 0) | /* ICCP */
                         (1u << 1) | /* OCIO */
                         (1u << 2) | /* XMP */
                         (1u << 3) | /* EXIF */
                         (1u << 4) | /* THMB */
                         (1u << 5);  /* HASH */
    out->half_float = 1u; /* sample_kind=HALF 可读写（spec §15） */
    /* tile_roi/multi_image 恒 0（v1 非目标） */
    return TC_OK;
}

/* —— 阶段 4：从 packet 派生一致 IDSC（编码器/适配器共用） —— */

int32_t tc_image_derive_idsc(const void* packet, size_t size, uint8_t image_profile,
                             topos_image_idsc* out)
{
    if (packet == NULL || out == NULL || size < TC_FRAME_HEADER_SIZE) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "derive_idsc: bad arg");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->abi_version = TOPOS_IMAGE_ABI_VERSION;
    out->idsc_version_major = 1u;
    out->idsc_version_minor = 0u;
    out->flags = TC_IMG_IDSC_FLAG_ORIENTATION_NORMALIZED;

    const uint8_t* bytes = (const uint8_t*)packet;
    topos_frame_header fh;
    int32_t rc = tc_frame_header_decode(bytes, TC_FRAME_HEADER_SIZE, &fh);
    if (rc != TC_OK) { return rc; }

    const int32_t w = (int32_t)fh.visible_width;
    const int32_t h = (int32_t)fh.visible_height;
    out->display_x_min = 0;
    out->display_y_min = 0;
    out->display_x_max = w - 1;
    out->display_y_max = h - 1;
    out->data_x_min = 0;
    out->data_y_min = 0;
    out->data_x_max = w - 1;
    out->data_y_max = h - 1;
    out->orientation = 1u;
    /* sar 0/0 = 未指定 ≡ 方形像素 1/1（spec §6.1-8） */
    out->pixel_aspect_num = (fh.sar_num == 0u) ? 1u : fh.sar_num;
    out->pixel_aspect_den = (fh.sar_den == 0u) ? 1u : fh.sar_den;
    out->channel_model = (fh.pixel_format == 2u)
                             ? TC_IMG_CHANNEL_MODEL_RGB
                             : (fh.pixel_format == 3u)
                                   ? TC_IMG_CHANNEL_MODEL_CFA
                                   : TC_IMG_CHANNEL_MODEL_YUV;
    /* TRAW CFA：恒 4 相位平面（无 alpha）；其余 3 平面 + 可选 alpha */
    out->channel_count = (uint8_t)(fh.pixel_format == 3u
                                       ? 4u
                                       : 3u + (fh.alpha_mode != 0u ? 1u : 0u));
    /* HALF（spec §15）：image_profile 4 声明样本域为 half 码值映射；
     * 其余 profile 恒 UINT。交叉规则（profile 4 ⇔ pf2/bd16/profile5 ⇔
     * sample_kind=HALF）由 tci_idsc_validate/crosscheck 在 write 路径强制 */
    out->sample_kind = (image_profile == TC_IMG_PROFILE_HALF_FLOAT)
                           ? TC_IMG_SAMPLE_KIND_HALF
                           : TC_IMG_SAMPLE_KIND_UINT;
    out->valid_bit_depth = fh.bit_depth;
    out->container_bit_depth = (fh.bit_depth >= 12u) ? fh.bit_depth : 10u;
    out->storage_layout = TC_IMG_LAYOUT_PLANAR;
    /* CFA：2 = 双维减半（RGGB 相位栅格；语义为相位拆分而非色度下采样） */
    out->subsampling = (fh.pixel_format == 0u) ? 1u
                       : (fh.pixel_format == 3u) ? 2u
                                                 : 0u;
    out->chroma_siting = fh.chroma_siting;
    out->alpha_presence = (fh.alpha_mode != 0u)
                              ? ((fh.flags & 1u) ? TC_IMG_ALPHA_PREMULT
                                                 : TC_IMG_ALPHA_STRAIGHT)
                              : TC_IMG_ALPHA_ABSENT;
    out->alpha_mode = fh.alpha_mode;
    out->alpha_bit_depth = fh.alpha_bit_depth;
    out->alpha_max_abs_err = 0u; /* 从 packet 无法得知误差上界；mode2 由写入方覆盖 */
    out->codec_id = TC_IMG_CODEC_ID_TPIC;
    out->payload_major = fh.version_major;
    out->payload_minor = fh.version_minor;
    out->image_profile = image_profile;
    out->codec_profile = fh.profile;
    out->pixel_format = fh.pixel_format;
    out->color_primaries = fh.color_primaries;
    out->color_transfer = fh.color_transfer;
    out->color_matrix = fh.color_matrix;
    out->color_range = fh.color_range;
    out->iccp_ref = 0u;
    out->ocio_ref = 0u;
    out->payload_header_crc32 = ((uint32_t)bytes[49] << 24) |
                                ((uint32_t)bytes[50] << 16) |
                                ((uint32_t)bytes[51] << 8) | (uint32_t)bytes[52];
    return TC_OK;
}
