#include "image_file_synth.h"

#include <stdlib.h>
#include <string.h>

#include "bitstream/frame_header.h"
#include "topos_codec.h"
#include "topos_image.h"

/* ==================== 内存 io ==================== */

int32_t image_mem_read(void* ctx, uint64_t off, void* buf, size_t n)
{
    image_mem_src* s = (image_mem_src*)ctx;
    if (off > s->len || n > s->len - off) { return TC_ERR_IO; }
    if (n != 0u) { memcpy(buf, s->data + off, n); }
    return TC_OK;
}

void image_mem_src_init(image_mem_src* s, const uint8_t* data, size_t len, topos_io* io)
{
    s->data = data;
    s->len = (uint64_t)len;
    memset(io, 0, sizeof(*io));
    io->struct_size = (uint32_t)sizeof(*io);
    io->abi_version = TOPOS_CODEC_ABI_VERSION;
    io->ctx = s;
    io->read = image_mem_read;
    io->length = s->len;
}

int32_t image_mem_write(void* ctx, const void* d, size_t n)
{
    image_mem_sink* s = (image_mem_sink*)ctx;
    if (s->len + n > s->cap) { s->oom = 1; return TC_ERR_IO; }
    if (n != 0u) { memcpy(s->data + s->len, d, n); }
    s->len += n;
    return TC_OK;
}

int32_t image_mem_seek_write(void* ctx, uint64_t off, const void* d, size_t n)
{
    image_mem_sink* s = (image_mem_sink*)ctx;
    if (off > (uint64_t)s->cap || n > s->cap - (size_t)off) { s->oom = 1; return TC_ERR_IO; }
    if (n != 0u) { memcpy(s->data + off, d, n); }
    if (off + n > s->len) { s->len = (size_t)off + n; }
    return TC_OK;
}

void image_mem_sink_init(image_mem_sink* s, uint8_t* buf, size_t cap, topos_io* io)
{
    s->data = buf;
    s->len = 0;
    s->cap = cap;
    s->oom = 0;
    memset(io, 0, sizeof(*io));
    io->struct_size = (uint32_t)sizeof(*io);
    io->abi_version = TOPOS_CODEC_ABI_VERSION;
    io->ctx = s;
    io->write = image_mem_write;
    io->seek_write = image_mem_seek_write;
}

/* ==================== 文件合成 ==================== */

int32_t image_file_synth_idsc(const uint8_t* pixl, size_t size, uint8_t image_profile,
                              topos_image_idsc* out)
{
    if (pixl == NULL || out == NULL || size < TC_FRAME_HEADER_SIZE) {
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->abi_version = TOPOS_IMAGE_ABI_VERSION;
    out->idsc_version_major = 1u;
    out->idsc_version_minor = 0u;
    out->flags = TC_IMG_IDSC_FLAG_ORIENTATION_NORMALIZED;

    topos_frame_header fh;
    const int32_t rc = tc_frame_header_decode(pixl, TC_FRAME_HEADER_SIZE, &fh);
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
    out->pixel_aspect_num = (fh.sar_num == 0u) ? 1u : fh.sar_num;
    out->pixel_aspect_den = (fh.sar_den == 0u) ? 1u : fh.sar_den;
    out->channel_model = (fh.pixel_format == 2u)
                             ? TC_IMG_CHANNEL_MODEL_RGB
                             : (fh.pixel_format == 3u)
                                   ? TC_IMG_CHANNEL_MODEL_CFA /* TRAW（批 1） */
                                   : TC_IMG_CHANNEL_MODEL_YUV;
    out->channel_count = (uint8_t)(fh.pixel_format == 3u
                                       ? 4u /* TRAW CFA：4 相位平面 */
                                       : 3u + (fh.alpha_mode != 0u ? 1u : 0u));
    out->sample_kind = TC_IMG_SAMPLE_KIND_UINT;
    out->valid_bit_depth = fh.bit_depth;
    out->container_bit_depth = (fh.bit_depth == 12u) ? 12u : 10u;
    out->storage_layout = TC_IMG_LAYOUT_PLANAR;
    out->subsampling = (fh.pixel_format == 0u) ? 1u
                       : (fh.pixel_format == 3u) ? 2u /* CFA 2×2 相位栅格 */
                                                 : 0u;
    out->chroma_siting = fh.chroma_siting;
    out->alpha_presence = (fh.alpha_mode != 0u)
                              ? ((fh.flags & 1u) ? TC_IMG_ALPHA_PREMULT : TC_IMG_ALPHA_STRAIGHT)
                              : TC_IMG_ALPHA_ABSENT;
    out->alpha_mode = fh.alpha_mode;
    out->alpha_bit_depth = fh.alpha_bit_depth;
    out->alpha_max_abs_err = 0u; /* synth 只产 mode1 无损 */
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
    /* 绑定摘要 = 序列化 header 的 header_crc32 字段（偏移 49..52） */
    out->payload_header_crc32 =
        ((uint32_t)pixl[49] << 24) | ((uint32_t)pixl[50] << 16) |
        ((uint32_t)pixl[51] << 8) | (uint32_t)pixl[52];
    return TC_OK;
}

int32_t image_file_synth_build_ex(const packet_synth_cfg* pcfg, uint8_t image_profile,
                                  const topos_image_chunk_in* extras, uint32_t extra_count,
                                  uint8_t iccp_ref, uint8_t ocio_ref,
                                  uint8_t** out_data, size_t* out_size)
{
    *out_data = NULL;
    *out_size = 0;
    uint8_t* pixl = NULL;
    size_t pixl_size = 0;
    int32_t rc = packet_synth_build(pcfg, &pixl, &pixl_size);
    if (rc != TC_OK) { return rc; }

    topos_image_idsc idsc;
    rc = image_file_synth_idsc(pixl, pixl_size, image_profile, &idsc);
    if (rc != TC_OK) { free(pixl); return rc; }
    idsc.iccp_ref = iccp_ref;
    idsc.ocio_ref = ocio_ref;

    topos_image_write_params params;
    memset(&params, 0, sizeof(params));
    params.struct_size = (uint32_t)sizeof(params);
    params.abi_version = TOPOS_IMAGE_ABI_VERSION;
    params.idsc = idsc;
    params.pixl_data = pixl;
    params.pixl_size = pixl_size;
    params.extra_chunks = extras;
    params.extra_count = extra_count;

    size_t cap = 4096u + pixl_size;
    for (uint32_t i = 0; i < extra_count; ++i) { cap += extras[i].size; }
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (buf == NULL) { free(pixl); return TC_ERR_OUT_OF_MEMORY; }

    image_mem_sink sink;
    topos_io io;
    image_mem_sink_init(&sink, buf, cap, &io);
    uint64_t written = 0;
    rc = tc_image_write(&params, &io, &written);
    free(pixl);
    if (rc != TC_OK || sink.oom) {
        free(buf);
        return (rc != TC_OK) ? rc : TC_ERR_OUT_OF_MEMORY;
    }
    *out_data = buf;
    *out_size = sink.len;
    (void)written;
    return TC_OK;
}

int32_t image_file_synth_build(const packet_synth_cfg* pcfg, uint8_t image_profile,
                               const topos_image_chunk_in* extras, uint32_t extra_count,
                               uint8_t** out_data, size_t* out_size)
{
    return image_file_synth_build_ex(pcfg, image_profile, extras, extra_count, 0u, 0u,
                                     out_data, out_size);
}
