/* decoder_ctx.c —— 持久 decoder context（M2，计划 §6.1）。
 *
 * 与 tc_frame_decode 共用 tc_decode_slices 统一任务队列核心；context 额外持有
 * grow-only alpha 行缓冲池（几何稳定时稳态零分配）。单实例不可重入，
 * 不同实例可并发（池等状态为实例私有；线程池/错误状态为进程级共享，见各自契约）。
 */
#include "codec.h"
#include "v7_scalable.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "../common/alloc.h"
#include "../common/checked.h"
#include "../common/error.h"
#include "../common/tpool.h"
#include "../bitstream/layer_directory.h"

struct tc_decoder {
    dec_scratch scratch;
    uint32_t magic;
    _Atomic uint32_t max_slice_workers; /* M10-4：0 = 默认（进程线程数） */
    /* RD3-05：批量描述按需扩容后跨调用复用；packet 字节仍借用调用方
     * 内存，不由 decoder 隐式复制或接管。 */
    topos_packet_view* batch_views;
    dec_frame_req* batch_reqs;
    uint32_t batch_capacity;
};

#define TC_DECODER_MAGIC 0x54444543u /* "TDEC" */

static int decoder_valid(const tc_decoder* dec)
{
    return dec != NULL && dec->magic == TC_DECODER_MAGIC;
}

int32_t tc_decoder_create(const topos_decoder_config* cfg, tc_decoder** out)
{
    if (out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "out == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (cfg != NULL) {
        if (cfg->struct_size != (uint32_t)sizeof(topos_decoder_config)
                || cfg->abi_version != (uint32_t)TOPOS_CODEC_ABI_VERSION) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "decoder config struct_size/abi_version");
            return TC_ERR_INVALID_ARGUMENT;
        }
    }
    tc_decoder* dec = (tc_decoder*)tc_alloc(sizeof(tc_decoder));
    if (dec == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "decoder context");
        return TC_ERR_OUT_OF_MEMORY;
    }
    tc_dec_alpha_pool_init(&dec->scratch.alpha);
    tc_dec_dc_pool_init(&dec->scratch.dc);
    tc_dec_intra_pool_init(&dec->scratch.intra);
    dec->batch_views = NULL;
    dec->batch_reqs = NULL;
    dec->batch_capacity = 0u;
    atomic_init(&dec->max_slice_workers,
                (cfg != NULL) ? cfg->max_slice_workers : 0u);
    dec->magic = TC_DECODER_MAGIC;
    *out = dec;
    return TC_OK;
}

int32_t tc_decoder_set_max_slice_workers(tc_decoder* dec, uint32_t workers)
{
    if (!decoder_valid(dec)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decoder handle invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* 0 表示跟随进程级池；非零值由 decode core 再钳到当前池上限，
     * 这里不拒绝未来 ABI 可能扩展的更大数值。 */
    atomic_store_explicit(&dec->max_slice_workers, workers, memory_order_release);
    return TC_OK;
}

void tc_decoder_destroy(tc_decoder* dec)
{
    if (!decoder_valid(dec)) { return; }
    tc_free(dec->batch_views);
    tc_free(dec->batch_reqs);
    tc_dec_alpha_pool_free(&dec->scratch.alpha);
    tc_dec_dc_pool_free(&dec->scratch.dc);
    tc_dec_intra_pool_free(&dec->scratch.intra);
    dec->magic = 0u;
    tc_free(dec);
}

/* 批量描述 arena 的双分配提交：任一新数组失败都保留旧容量，调用方可以
 * 安全重试；数组只保存解析结果和请求描述，不拥有外部 packet 字节。 */
static int32_t decoder_batch_reserve(tc_decoder* dec, uint32_t count)
{
    if (dec->batch_capacity >= count) { return TC_OK; }
    size_t views_bytes = 0u;
    size_t reqs_bytes = 0u;
    if (!tc_umul_size((size_t)count, sizeof(topos_packet_view), &views_bytes) ||
        !tc_umul_size((size_t)count, sizeof(dec_frame_req), &reqs_bytes)) {
        tc_set_error(TC_ERR_LIMIT_EXCEEDED, "decoder batch arena bytes overflow");
        return TC_ERR_LIMIT_EXCEEDED;
    }
    topos_packet_view* views = (topos_packet_view*)tc_alloc(views_bytes);
    if (views == NULL) {
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "decoder batch view arena");
        return TC_ERR_OUT_OF_MEMORY;
    }
    dec_frame_req* reqs = (dec_frame_req*)tc_alloc(reqs_bytes);
    if (reqs == NULL) {
        tc_free(views);
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "decoder batch request arena");
        return TC_ERR_OUT_OF_MEMORY;
    }
    tc_free(dec->batch_views);
    tc_free(dec->batch_reqs);
    dec->batch_views = views;
    dec->batch_reqs = reqs;
    dec->batch_capacity = count;
    return TC_OK;
}

static void decoder_apply_plan_info(const tc_decode_plan* plan,
                                    topos_frame_output* info)
{
    info->visible_width = (uint16_t)plan->target_width;
    info->visible_height = (uint16_t)plan->target_height;
    info->coded_width = (uint16_t)plan->target_width;
    info->coded_height = (uint16_t)plan->target_height;
    if (plan->drop_alpha != 0u && info->alpha_mode != 0u
            && info->plane_count >= 4u) {
        info->plane_count = 3u;
        info->alpha_mode = 0u;
        info->alpha_bit_depth = 0u;
    }
}

static int32_t decoder_parse_request(const uint8_t* packet, size_t size,
                                     const topos_decode_request* request,
                                     topos_packet_view* view, tc_decode_plan* plan)
{
    if (packet == NULL || request == NULL || view == NULL || plan == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decode request arguments invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    int32_t rc = tc_packet_parse_structure(packet, size, view);
    if (rc != TC_OK) { return rc; }
    return tc_decode_request_resolve(&view->fh, request, plan);
}

static void decoder_init_info(topos_frame_output* info)
{
    memset(info, 0, sizeof(*info));
    info->struct_size = (uint32_t)sizeof(topos_frame_output);
    info->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
}

static void decoder_full_plan(const topos_frame_header* fh, tc_decode_plan* plan)
{
    memset(plan, 0, sizeof(*plan));
    plan->mode = TC_DECODE_MODE_FULL;
    plan->scale = TC_DECODE_SCALE_FULL;
    plan->target_width = fh->visible_width;
    plan->target_height = fh->visible_height;
    plan->coefficient_limit = 63u;
}

int32_t tc_decoder_prepare(tc_decoder* dec, const uint8_t* packet, size_t size,
                           topos_frame_output* info)
{
    if (!decoder_valid(dec)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decoder handle invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (info == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "info == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (packet == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "packet == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (tc_v7b_packet_is(packet, size)) {
        topos_frame_header v7b_fh;
        int32_t v7b_rc = tc_v7_packet_header_decode(packet, size, &v7b_fh);
        if (v7b_rc != TC_OK) { return v7b_rc; }
        tc_fill_output_info(&v7b_fh, info);
        return TC_OK;
    }
    topos_packet_view view;
    int32_t rc = tc_packet_parse_structure(packet, size, &view);
    if (rc != TC_OK) { return rc; }
    tc_fill_output_info(&view.fh, info);
    return TC_OK;
}

int32_t tc_decoder_prepare_request(tc_decoder* dec, const uint8_t* packet, size_t size,
                                   const topos_decode_request* request,
                                   topos_frame_output* info)
{
    if (!decoder_valid(dec)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decoder handle invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (info == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "info == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    decoder_init_info(info);
    if (tc_v7b_packet_is(packet, size)) {
        topos_frame_header v7b_fh;
        tc_decode_plan v7b_plan;
        int32_t v7b_rc = tc_v7_packet_header_decode(packet, size, &v7b_fh);
        if (v7b_rc != TC_OK) { return v7b_rc; }
        v7b_rc = tc_decode_request_resolve(&v7b_fh, request, &v7b_plan);
        if (v7b_rc != TC_OK) { return v7b_rc; }
        /* V7-B owns a separate scalable decoder; keep its four-plane contract
         * until that path grows the same alpha omission support. */
        v7b_plan.drop_alpha = 0u;
        tc_fill_output_info(&v7b_fh, info);
        decoder_apply_plan_info(&v7b_plan, info);
        return TC_OK;
    }
    topos_packet_view view;
    tc_decode_plan plan;
    const int prof = tc_profile_enabled();
    const uint64_t t_scan = prof ? tc_profile_now_ns() : 0u;
    int32_t rc = decoder_parse_request(packet, size, request, &view, &plan);
    if (prof) { tc_dev_decode_stats_add_scan(tc_profile_now_ns() - t_scan); }
    if (rc != TC_OK) { return rc; }
    tc_fill_output_info(&view.fh, info);
    decoder_apply_plan_info(&plan, info);
    return TC_OK;
}

int32_t tc_decoder_decode(tc_decoder* dec, const uint8_t* packet, size_t size,
                          const topos_plane_view out[TC_FRAME_MAX_PLANES],
                          topos_frame_output* info)
{
    if (!decoder_valid(dec)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decoder handle invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (info == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "info == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (packet == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "packet == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    /* P1-16：单帧 context 解码必然写平面——out 为 NULL 时在触碰 out[p]
     * 之前拒绝（batch 入口的 query 模式 out==NULL 是合法语义，不受此限）。 */
    if (out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "out == NULL");
        return TC_ERR_INVALID_ARGUMENT;
    }
    memset(info, 0, sizeof(*info));
    info->struct_size = (uint32_t)sizeof(topos_frame_output);
    info->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;

    if (tc_v7b_packet_is(packet, size)) {
        topos_frame_header v7b_fh;
        int32_t v7b_rc = tc_v7_packet_header_decode(packet, size, &v7b_fh);
        if (v7b_rc != TC_OK) { return v7b_rc; }
        tc_fill_output_info(&v7b_fh, info);
        uint16_t* planes[TC_FRAME_MAX_PLANES] = { NULL, NULL, NULL, NULL };
        size_t strides[TC_FRAME_MAX_PLANES] = { 0u, 0u, 0u, 0u };
        for (uint32_t p = 0u; p < v7b_fh.plane_count; ++p) {
            const topos_plane_view* v = &out[p];
            if (v->struct_size != (uint32_t)sizeof(topos_plane_view) ||
                v->abi_version != (uint32_t)TOPOS_CODEC_ABI_VERSION ||
                v->pixels == NULL) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT, "v7b plane view %u invalid",
                             (unsigned)p);
                return TC_ERR_INVALID_ARGUMENT;
            }
            planes[p] = v->pixels;
            strides[p] = v->stride;
        }
        tc_v7b_decode_stats v7b_stats;
        int32_t v7b_decode_rc = tc_v7b_frame_decode(
            packet, size, TC_CODEC_AUTO_2K_MAX_DIM, TC_V7B_DECODE_FULL,
            planes, strides, info, &v7b_stats);
        if (v7b_decode_rc == TC_OK) {
            tc_dev_decode_stats_add_v7b(v7b_stats.bytes_read,
                                        v7b_stats.base_bytes_read,
                                        v7b_stats.segments_read,
                                        v7b_stats.segments_skipped);
        }
        return v7b_decode_rc;
    }

    if (tc_packet_is_v8(packet, size)) {
        /* V8（批 3）：查询 = 结构扫描；实解码 = 段并行解码 + 段级 conceal。 */
        if (out == NULL) { return TC_ERR_INVALID_ARGUMENT; }
        topos_v8_packet_view v8;
        int32_t rc8 = tc_packet_scan_v8_ex(packet, size, &v8, 0);
        if (rc8 != TC_OK) { return rc8; }
        tc_fill_output_info(&v8.fh, info);
        uint16_t* planes[TC_FRAME_MAX_PLANES];
        size_t strides[TC_FRAME_MAX_PLANES];
        for (uint32_t p = 0u; p < v8.fh.plane_count; ++p) {
            const topos_plane_view* v = &out[p];
            if (v->struct_size != (uint32_t)sizeof(topos_plane_view) ||
                v->abi_version != (uint32_t)TOPOS_CODEC_ABI_VERSION ||
                v->pixels == NULL) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT, "v8 plane view %u invalid",
                             (unsigned)p);
                return TC_ERR_INVALID_ARGUMENT;
            }
            planes[p] = v->pixels;
            strides[p] = v->stride;
        }
        return v8_frame_decode(packet, size, planes, strides, info);
    }

    const int prof = tc_profile_enabled();
    uint64_t t_scan = prof ? tc_profile_now_ns() : 0u;
    topos_packet_view view;
    int32_t rc = tc_packet_parse_structure(packet, size, &view);
    if (prof) { tc_dev_decode_stats_add_scan(tc_profile_now_ns() - t_scan); }
    if (rc != TC_OK) { return rc; }
    tc_fill_output_info(&view.fh, info);
    {
        const int32_t grc = tc_v7r3_stateless_p_guard(&view.fh);
        if (grc != TC_OK) { return grc; }
    }

    for (uint32_t p = 0u; p < view.fh.plane_count; ++p) {
        const topos_plane_view* v = &out[p];
        if (v->struct_size != (uint32_t)sizeof(topos_plane_view)
                || v->abi_version != (uint32_t)TOPOS_CODEC_ABI_VERSION) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "plane view %u struct_size/abi_version",
                         (unsigned)p);
            return TC_ERR_INVALID_ARGUMENT;
        }
        if (v->pixels == NULL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "plane view %u pixels == NULL",
                         (unsigned)p);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }

    uint16_t* planes[TC_FRAME_MAX_PLANES];
    size_t strides[TC_FRAME_MAX_PLANES];
    for (uint32_t p = 0u; p < view.fh.plane_count; ++p) {
        planes[p] = out[p].pixels;
        strides[p] = out[p].stride;
    }
    /* M0：scan 计时在 core 之前由调用方统计（tc_decode_slices 内聚合其余阶段） */
    const uint32_t workers = atomic_load_explicit(&dec->max_slice_workers,
                                                  memory_order_acquire);
    return tc_decode_slices(&view, planes, strides, &dec->scratch, info,
                            workers);
}

int32_t tc_decoder_decode_request(tc_decoder* dec, const uint8_t* packet, size_t size,
                                  const topos_decode_request* request,
                                  const topos_plane_view out[TC_FRAME_MAX_PLANES],
                                  topos_frame_output* info)
{
    if (!decoder_valid(dec)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decoder handle invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (info == NULL || packet == NULL || out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "request decode arguments invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    decoder_init_info(info);
    if (tc_v7b_packet_is(packet, size)) {
        topos_frame_header v7b_fh;
        tc_decode_plan v7b_plan;
        int32_t v7b_rc = tc_v7_packet_header_decode(packet, size, &v7b_fh);
        if (v7b_rc != TC_OK) { return v7b_rc; }
        v7b_rc = tc_decode_request_resolve(&v7b_fh, request, &v7b_plan);
        if (v7b_rc != TC_OK) { return v7b_rc; }
        v7b_plan.drop_alpha = 0u;
        tc_fill_output_info(&v7b_fh, info);
        decoder_apply_plan_info(&v7b_plan, info);
        uint16_t* planes[TC_FRAME_MAX_PLANES] = { NULL, NULL, NULL, NULL };
        size_t strides[TC_FRAME_MAX_PLANES] = { 0u, 0u, 0u, 0u };
        for (uint32_t p = 0u; p < v7b_fh.plane_count; ++p) {
            const topos_plane_view* v = &out[p];
            if (v->struct_size != (uint32_t)sizeof(topos_plane_view) ||
                v->abi_version != (uint32_t)TOPOS_CODEC_ABI_VERSION ||
                v->pixels == NULL) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT,
                             "v7b request plane view %u invalid", (unsigned)p);
                return TC_ERR_INVALID_ARGUMENT;
            }
            planes[p] = v->pixels;
            strides[p] = v->stride;
        }
        if (request->mode == TC_DECODE_MODE_FULL ||
            (request->mode == TC_DECODE_MODE_REDUCED && v7b_plan.scaled == 0u) ||
            (request->mode == TC_DECODE_MODE_AUTO_2K && v7b_plan.scaled == 0u)) {
            tc_v7b_decode_stats v7b_stats;
            int32_t v7b_decode_rc = tc_v7b_frame_decode(
                packet, size, TC_CODEC_AUTO_2K_MAX_DIM, TC_V7B_DECODE_FULL,
                planes, strides, info, &v7b_stats);
            if (v7b_decode_rc == TC_OK) {
                tc_dev_decode_stats_add_v7b(v7b_stats.bytes_read,
                                            v7b_stats.base_bytes_read,
                                            v7b_stats.segments_read,
                                            v7b_stats.segments_skipped);
            }
            return v7b_decode_rc;
        }
        if (request->mode == TC_DECODE_MODE_REDUCED ||
            request->mode == TC_DECODE_MODE_AUTO_2K) {
            tc_v7b_decode_stats v7b_stats;
            int32_t v7b_decode_rc = tc_v7b_frame_decode_reduced(
                packet, size, v7b_plan.target_width, v7b_plan.target_height,
                planes, strides, info, &v7b_stats);
            if (v7b_decode_rc == TC_OK) {
                tc_dev_decode_stats_add_v7b(v7b_stats.bytes_read,
                                            v7b_stats.base_bytes_read,
                                            v7b_stats.segments_read,
                                            v7b_stats.segments_skipped);
            }
            return v7b_decode_rc;
        }
        tc_set_error(TC_ERR_NOT_IMPLEMENTED,
                     "v7b context scaled request requires full-quality target reconstruction");
        return TC_ERR_NOT_IMPLEMENTED;
    }
    topos_packet_view view;
    tc_decode_plan plan;
    const int prof = tc_profile_enabled();
    const uint64_t t_scan = prof ? tc_profile_now_ns() : 0u;
    int32_t rc = decoder_parse_request(packet, size, request, &view, &plan);
    if (prof) { tc_dev_decode_stats_add_scan(tc_profile_now_ns() - t_scan); }
    if (rc != TC_OK) { return rc; }
    tc_fill_output_info(&view.fh, info);
    decoder_apply_plan_info(&plan, info);

    uint16_t* planes[TC_FRAME_MAX_PLANES];
    size_t strides[TC_FRAME_MAX_PLANES];
    memset(planes, 0, sizeof(planes));
    memset(strides, 0, sizeof(strides));
    for (uint32_t p = 0u; p < info->plane_count; ++p) {
        const topos_plane_view* v = &out[p];
        if (v->struct_size != (uint32_t)sizeof(topos_plane_view) ||
            v->abi_version != (uint32_t)TOPOS_CODEC_ABI_VERSION ||
            v->pixels == NULL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "request plane view %u invalid",
                         (unsigned)p);
            return TC_ERR_INVALID_ARGUMENT;
        }
        planes[p] = v->pixels;
        strides[p] = v->stride;
    }
    const uint32_t workers = atomic_load_explicit(&dec->max_slice_workers,
                                                  memory_order_acquire);
    if (!plan.scaled) {
        return tc_decode_slices_ex(&view, planes, strides, &dec->scratch, info,
                                   workers, plan.drop_alpha);
    }
    return tc_decode_slices_scaled_limit_ex(
        &view, planes, strides, &dec->scratch, info, workers,
        plan.target_width, plan.target_height, plan.coefficient_limit,
        plan.drop_alpha);
}

int32_t tc_decoder_decode_surface(tc_decoder* dec, const uint8_t* packet, size_t size,
                                  const topos_decode_surface* surface,
                                  topos_frame_output* info)
{
    if (surface == NULL ||
        (surface->memory_type != TC_DECODE_MEMORY_CPU &&
         surface->memory_type != TC_DECODE_MEMORY_HOST_VISIBLE_GPU)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decode surface invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (surface->struct_size != (uint32_t)sizeof(topos_decode_surface) ||
        surface->abi_version != (uint32_t)TOPOS_CODEC_ABI_VERSION) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decode surface struct_size/abi_version");
        return TC_ERR_INVALID_ARGUMENT;
    }
    return tc_decoder_decode(dec, packet, size, surface->planes, info);
}

int32_t tc_decoder_decode_surface_request(tc_decoder* dec, const uint8_t* packet, size_t size,
                                          const topos_decode_request* request,
                                          const topos_decode_surface* surface,
                                          topos_frame_output* info)
{
    if (surface == NULL ||
        (surface->memory_type != TC_DECODE_MEMORY_CPU &&
         surface->memory_type != TC_DECODE_MEMORY_HOST_VISIBLE_GPU)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decode surface invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (surface->struct_size != (uint32_t)sizeof(topos_decode_surface) ||
        surface->abi_version != (uint32_t)TOPOS_CODEC_ABI_VERSION ||
        request == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decode surface request invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (request->memory_type != surface->memory_type) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "request/surface memory_type mismatch");
        return TC_ERR_INVALID_ARGUMENT;
    }
    return tc_decoder_decode_request(dec, packet, size, request, surface->planes, info);
}

typedef struct decoder_v7b_batch_job_ctx {
    tc_decoder* decoder;
    const topos_batch_packet* packet;
    const topos_decode_request* request;
    const topos_plane_view* out;
    topos_frame_output* info;
    int32_t rc;
} decoder_v7b_batch_job_ctx;

static void decoder_v7b_batch_job_run(void* opaque)
{
    decoder_v7b_batch_job_ctx* job = (decoder_v7b_batch_job_ctx*)opaque;
    if (job->out == NULL) {
        job->rc = job->request != NULL
            ? tc_decoder_prepare_request(job->decoder, job->packet->data,
                                         job->packet->size, job->request, job->info)
            : tc_decoder_prepare(job->decoder, job->packet->data,
                                  job->packet->size, job->info);
    } else {
        job->rc = job->request != NULL
            ? tc_decoder_decode_request(job->decoder, job->packet->data,
                                        job->packet->size, job->request,
                                        job->out, job->info)
            : tc_decoder_decode(job->decoder, job->packet->data,
                                job->packet->size, job->out, job->info);
    }
}

/* 阶段4：context 跨帧批量解码（语义契约见 topos_codec.h）。 */
static int32_t tc_decoder_decode_batch_mode(
    tc_decoder* dec, const topos_batch_packet* packets, uint32_t count,
    const topos_decode_request* request, const topos_plane_view* out,
    topos_frame_output* infos)
{
    if (!decoder_valid(dec)) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT, "decoder handle invalid");
        return TC_ERR_INVALID_ARGUMENT;
    }
    if (count == 0u) { return TC_OK; }
    if (request != NULL) {
        int32_t request_rc = tc_decode_request_validate(request);
        if (request_rc != TC_OK) { return request_rc; }
    }
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
    for (uint32_t i = 0u; i < count; ++i) {
        if (packets[i].data == NULL) {
            tc_set_error(TC_ERR_INVALID_ARGUMENT, "packets[%u].data == NULL",
                         (unsigned)i);
            return TC_ERR_INVALID_ARGUMENT;
        }
    }

    int has_v7b = 0;
    int all_v7b = 1;
    for (uint32_t i = 0u; i < count; ++i) {
        if (tc_v7b_packet_is(packets[i].data, packets[i].size)) {
            has_v7b = 1;
        } else {
            all_v7b = 0;
        }
    }
    if (has_v7b != 0) {
        /* V7-B packets are not representable by dec_frame_req yet. An all-V7-B
         * batch can nevertheless run one frame job per pool task: the V7-B job
         * owns its temporary base/residual buffers, and any legacy base decode
         * invoked from a worker sees the tpool nested-call guard and runs inline.
         * Mixed batches stay serial because one decoder context's legacy scratch
         * is intentionally not shared between concurrent frame jobs. */
        if (all_v7b != 0) {
            size_t jobs_bytes = 0u;
            if (tc_umul_size((size_t)count, sizeof(tc_job), &jobs_bytes)) {
                tc_job* jobs = (tc_job*)tc_alloc(jobs_bytes);
                if (jobs != NULL) {
                    size_t ctx_bytes = 0u;
                    if (tc_umul_size((size_t)count, sizeof(decoder_v7b_batch_job_ctx),
                                     &ctx_bytes)) {
                        decoder_v7b_batch_job_ctx* ctx =
                            (decoder_v7b_batch_job_ctx*)tc_alloc(ctx_bytes);
                        if (ctx != NULL) {
                            for (uint32_t i = 0u; i < count; ++i) {
                                ctx[i].decoder = dec;
                                ctx[i].packet = &packets[i];
                                ctx[i].request = request;
                                ctx[i].out = out == NULL ? NULL
                                    : out + (size_t)i * TC_FRAME_MAX_PLANES;
                                ctx[i].info = &infos[i];
                                ctx[i].rc = TC_OK;
                                jobs[i].fn = decoder_v7b_batch_job_run;
                                jobs[i].ctx = &ctx[i];
                            }
                            const uint32_t workers = atomic_load_explicit(
                                &dec->max_slice_workers, memory_order_acquire);
                            (void)tc_parallel_for(jobs, count, workers);
                            int32_t first = TC_OK;
                            for (uint32_t i = 0u; i < count; ++i) {
                                if (ctx[i].rc != TC_OK && first == TC_OK) {
                                    first = ctx[i].rc;
                                }
                            }
                            tc_free(ctx);
                            tc_free(jobs);
                            return first;
                        }
                    }
                    tc_free(jobs);
                }
            }
        }

        /* Allocation failure or a mixed batch: stay safe and serial at the
         * frame boundary rather than nesting the legacy slice pool. */
        int32_t first = TC_OK;
        for (uint32_t i = 0u; i < count; ++i) {
            int32_t frame_rc;
            if (out == NULL) {
                frame_rc = request != NULL
                    ? tc_decoder_prepare_request(dec, packets[i].data, packets[i].size,
                                                 request, &infos[i])
                    : tc_decoder_prepare(dec, packets[i].data, packets[i].size,
                                         &infos[i]);
            } else {
                const topos_plane_view* frame_out = out + (size_t)i * TC_FRAME_MAX_PLANES;
                frame_rc = request != NULL
                    ? tc_decoder_decode_request(dec, packets[i].data, packets[i].size,
                                                request, frame_out, &infos[i])
                    : tc_decoder_decode(dec, packets[i].data, packets[i].size,
                                        frame_out, &infos[i]);
            }
            if (frame_rc != TC_OK && first == TC_OK) { first = frame_rc; }
        }
        return first;
    }

    int32_t reserve_rc = decoder_batch_reserve(dec, count);
    if (reserve_rc != TC_OK) { return reserve_rc; }
    topos_packet_view* views = dec->batch_views;

    /* M1：入口只做结构解析；任一包失败 → 整批拒绝，不产出任何帧 */
    const int prof = tc_profile_enabled();
    for (uint32_t i = 0u; i < count; ++i) {
        uint64_t t_scan = prof ? tc_profile_now_ns() : 0u;
        int32_t rc = tc_packet_parse_structure(packets[i].data, packets[i].size,
                                               &views[i]);
        if (prof) { tc_dev_decode_stats_add_scan(tc_profile_now_ns() - t_scan); }
        if (rc != TC_OK) {
            return rc;
        }
    }

    /* 查询模式：out == NULL → 仅填充几何，不重建（同单帧两段式） */
    if (out == NULL) {
        for (uint32_t i = 0u; i < count; ++i) {
            memset(&infos[i], 0, sizeof(infos[i]));
            infos[i].struct_size = (uint32_t)sizeof(topos_frame_output);
            infos[i].abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
            tc_fill_output_info(&views[i].fh, &infos[i]);
            tc_decode_plan plan;
            int32_t rc = request != NULL
                ? tc_decode_request_resolve(&views[i].fh, request, &plan)
                : (decoder_full_plan(&views[i].fh, &plan), TC_OK);
            if (rc != TC_OK) {
                return rc;
            }
            decoder_apply_plan_info(&plan, &infos[i]);
        }
        return TC_OK;
    }

    dec_frame_req* reqs = dec->batch_reqs;

    for (uint32_t i = 0u; i < count; ++i) {
        topos_frame_output* info = &infos[i];
        memset(info, 0, sizeof(*info));
        info->struct_size = (uint32_t)sizeof(topos_frame_output);
        info->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        tc_fill_output_info(&views[i].fh, info);
        {
            const int32_t grc = tc_v7r3_stateless_p_guard(&views[i].fh);
            if (grc != TC_OK) { return grc; }
        }

        dec_frame_req* r = &reqs[i];
        memset(r, 0, sizeof(*r));
        r->view = &views[i];
        r->info = info;
        r->scratch = &dec->scratch;
        r->nw_override = atomic_load_explicit(&dec->max_slice_workers,
                                              memory_order_acquire);
        tc_decode_plan plan;
        int32_t plan_rc = request != NULL
            ? tc_decode_request_resolve(&views[i].fh, request, &plan)
            : (decoder_full_plan(&views[i].fh, &plan), TC_OK);
        if (plan_rc != TC_OK) {
            tc_set_error(plan_rc, "frame %u decode request cannot be resolved",
                         (unsigned)i);
            r->rc = plan_rc;
            continue;
        }
        decoder_apply_plan_info(&plan, info);
        r->target_width = plan.target_width;
        r->target_height = plan.target_height;
        r->scaled = plan.scaled;
        r->coefficient_limit = plan.coefficient_limit;
        r->skip_alpha = plan.drop_alpha;
        const size_t base = (size_t)i * TC_FRAME_MAX_PLANES; /* P1-17 */
        for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) {
            r->planes[p] = out[base + p].pixels;
            r->strides[p] = out[base + p].stride;
        }
        /* 单帧路径同位校验：plane view ABI/pixels 非法 → 该帧拒绝 */
        for (uint32_t p = 0u; p < infos[i].plane_count; ++p) {
            const topos_plane_view* v = &out[base + p];
            if (v->struct_size != (uint32_t)sizeof(topos_plane_view)
                    || v->abi_version != (uint32_t)TOPOS_CODEC_ABI_VERSION) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT,
                             "frame %u plane view %u struct_size/abi_version",
                             (unsigned)i, (unsigned)p);
                r->rc = TC_ERR_INVALID_ARGUMENT;
                break;
            }
            if (v->pixels == NULL) {
                tc_set_error(TC_ERR_INVALID_ARGUMENT,
                             "frame %u plane view %u pixels == NULL",
                             (unsigned)i, (unsigned)p);
                r->rc = TC_ERR_INVALID_ARGUMENT;
                break;
            }
        }
    }

    const int32_t first = tc_decode_frames(reqs, count);
    return first;
}

int32_t tc_decoder_decode_batch(tc_decoder* dec,
                                const topos_batch_packet* packets, uint32_t count,
                                const topos_plane_view* out, topos_frame_output* infos)
{
    return tc_decoder_decode_batch_mode(dec, packets, count, NULL, out, infos);
}

int32_t tc_decoder_decode_batch_request(tc_decoder* dec,
                                        const topos_batch_packet* packets, uint32_t count,
                                        const topos_decode_request* request,
                                        const topos_plane_view* out,
                                        topos_frame_output* infos)
{
    return tc_decoder_decode_batch_mode(dec, packets, count, request, out, infos);
}
