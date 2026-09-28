#include "base_encoder.h"

#include <string.h>

#include "../common/alloc.h"
#include "../common/checked.h"
#include "../common/error.h"
#include "../transform/base_scale.h"

int32_t tc_base_split_budget(uint32_t total_bytes, uint32_t base_hint,
                            uint32_t residual_hint, tc_base_budget* out)
{
    if (out == NULL || total_bytes == 0u) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base budget arguments");
        return TC_ERR_INVALID_ARGUMENT;
    }
    uint64_t weight = (uint64_t)base_hint + (uint64_t)residual_hint;
    if (weight == 0u) {
        base_hint = 1u;
        residual_hint = 1u;
        weight = 2u;
    }
    const uint64_t base = ((uint64_t)total_bytes * base_hint) / weight;
    out->total_bytes = total_bytes;
    out->base_bytes = (uint32_t)base;
    out->residual_bytes = total_bytes - out->base_bytes;
    return TC_OK;
}

/* pf=3（TRAW CFA）恒 4 平面且无 alpha；其余 3 色平面 + 可选 alpha */
static uint32_t base_plane_count(const topos_frame_config* config)
{
    return (config->pixel_format == 3u)
               ? 4u
               : (uint32_t)(3u + (config->alpha_mode != 0u ? 1u : 0u));
}

static uint8_t plane_bit_depth(const topos_frame_config* config, uint32_t plane)
{
    if (plane == 3u && config->pixel_format != 3u) {
        return config->alpha_mode == 0u ? 0u : config->alpha_bit_depth;
    }
    return config->bit_depth != 0u ? config->bit_depth : 10u;
}

static int32_t source_plane_geometry(const topos_frame_config* config,
                                     uint32_t plane,
                                     uint32_t* width, uint32_t* height)
{
    if (plane >= base_plane_count(config)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base source plane %u out of range", plane);
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (!tc_base_plane_dimensions(config->visible_width, config->visible_height,
                                  config->pixel_format, plane,
                                  config->chroma_siting, width, height)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base source plane %u geometry", plane);
        return TC_ERR_INVALID_ARGUMENT;
    }
    return TC_OK;
}

static int32_t base_plane_prepare(const topos_frame_config* source_config,
                                  const topos_frame_input* source_input,
                                  const topos_frame_config* base_config,
                                  uint32_t plane, tc_base_frame* out)
{
    uint32_t source_w = 0u, source_h = 0u;
    uint32_t base_w = 0u, base_h = 0u;
    int32_t rc = source_plane_geometry(source_config, plane, &source_w, &source_h);
    if (rc != TC_OK) { return rc; }
    if (!tc_base_plane_dimensions(base_config->visible_width, base_config->visible_height,
                                  base_config->pixel_format, plane,
                                  base_config->chroma_siting, &base_w, &base_h)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base destination plane %u geometry", plane);
        return TC_ERR_INVALID_ARGUMENT;
    }
    out->plane_width[plane] = base_w;
    out->plane_height[plane] = base_h;
    if (source_input->planes[plane] == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base input plane %u == NULL", plane);
        return TC_ERR_INVALID_ARGUMENT;
    }
    size_t source_stride = source_input->strides[plane] != 0u
                               ? source_input->strides[plane] : (size_t)source_w;
    if (source_stride < (size_t)source_w) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "base input stride[%u]=%zu < source width %u",
                     plane, source_stride, source_w);
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (source_w == base_w && source_h == base_h) {
        out->input.planes[plane] = source_input->planes[plane];
        out->input.strides[plane] = source_stride;
        return TC_OK;
    }
    size_t bytes = 0u;
    if (!tc_umul_size((size_t)base_w, (size_t)base_h, &bytes) ||
        !tc_umul_size(bytes, sizeof(uint16_t), &bytes)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "base plane %u allocation size", plane);
        return TC_ERR_LIMIT_EXCEEDED;
    }
    uint16_t* reduced = (uint16_t*)tc_alloc(bytes);
    if (reduced == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "base plane %u allocation %zu", plane, bytes);
        return TC_ERR_OUT_OF_MEMORY;
    }
    if (!tc_base_downsample_u16(source_input->planes[plane], source_w, source_h,
                                source_stride, reduced, base_w, base_h, base_w,
                                plane_bit_depth(source_config, plane))) {
        tc_free(reduced);
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base plane %u downsample", plane);
        return TC_ERR_INVALID_ARGUMENT;
    }
    out->owned_planes[plane] = reduced;
    out->plane_bytes[plane] = bytes;
    out->owns_plane[plane] = 1u;
    out->input.planes[plane] = reduced;
    out->input.strides[plane] = (size_t)base_w;
    return TC_OK;
}

int32_t tc_base_frame_prepare(const topos_frame_config* source_config,
                              const topos_frame_input* source_input,
                              uint32_t max_dim,
                              tc_base_frame* out)
{
    if (out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base output is NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    int32_t rc = tc_frame_config_validate(source_config);
    if (rc != TC_OK) { return rc; }
    if (source_input == NULL ||
        (source_input->struct_size != 0u &&
         source_input->struct_size != (uint32_t)sizeof(topos_frame_input))) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base input struct invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (max_dim == 0u || max_dim > TC_PLANE_MAX_DIM) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base max_dim %u", max_dim);
        return TC_ERR_INVALID_ARGUMENT;
    }
    uint32_t base_w = 0u, base_h = 0u;
    if (!tc_base_scale_dimensions(source_config->visible_width,
                                  source_config->visible_height, max_dim,
                                  &base_w, &base_h)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "base luma geometry");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (base_w > UINT16_MAX || base_h > UINT16_MAX) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "base dimensions exceed ABI");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    out->config = *source_config;
    out->config.struct_size = (uint32_t)sizeof(topos_frame_config);
    out->config.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    out->config.visible_width = (uint16_t)base_w;
    out->config.visible_height = (uint16_t)base_h;
    memset(&out->input, 0, sizeof(out->input));
    out->input.struct_size = (uint32_t)sizeof(topos_frame_input);
    out->input.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;

    for (uint32_t plane = 0u; plane <
         base_plane_count(source_config); ++plane) {
        rc = base_plane_prepare(source_config, source_input, &out->config, plane, out);
        if (rc != TC_OK) {
            tc_base_frame_release(out);
            return rc;
        }
    }
    return TC_OK;
}

void tc_base_frame_release(tc_base_frame* base)
{
    if (base == NULL) { return; }
    for (uint32_t plane = 0u; plane < TC_FRAME_MAX_PLANES; ++plane) {
        if (base->owns_plane[plane] != 0u) {
            tc_free(base->owned_planes[plane]);
        }
    }
    memset(base, 0, sizeof(*base));
}

int32_t tc_base_frame_encode(const topos_frame_config* source_config,
                             const topos_frame_input* source_input,
                             uint32_t max_dim,
                             uint8_t* out, size_t out_cap,
                             topos_frame_stats* stats)
{
    tc_base_frame base;
    int32_t rc = tc_base_frame_prepare(source_config, source_input, max_dim, &base);
    if (rc == TC_OK) {
        rc = tc_frame_encode(&base.config, &base.input, out, out_cap, stats);
    }
    tc_base_frame_release(&base);
    return rc;
}

int32_t tc_base_frame_encode_sized(const topos_frame_config* source_config,
                                   const topos_frame_input* source_input,
                                   uint32_t max_dim, uint32_t target_bytes,
                                   uint8_t qp_min, uint8_t qp_max,
                                   uint8_t* qp_used, uint8_t* out, size_t out_cap,
                                   topos_frame_stats* stats)
{
    tc_base_frame base;
    int32_t rc = tc_base_frame_prepare(source_config, source_input, max_dim, &base);
    if (rc == TC_OK) {
        rc = tc_frame_encode_sized(&base.config, &base.input, target_bytes,
                                   qp_min, qp_max, qp_used, out, out_cap, stats);
    }
    tc_base_frame_release(&base);
    return rc;
}
