/* 4:4:4 planar → 4:2:2 planar host 侧辅助转换。
 *
 * 与 timeline_export.py 的 numpy 参考路径保持逐位一致：先对已量化
 * uint16 色度做水平 box 平均，奇宽复制边缘样本，再按行并行。该文件不
 * 参与码流语义，只负责把 Topos 原生解码平面交给 4:2:2 编码器。 */

#include "topos_codec.h"

#include <stddef.h>
#include <stdint.h>

#include "error.h"
#include "tpool.h"
#include "color_convert_simd.h"
#include "planar_convert_simd.h"

#define TC_PLANAR_MAX_DIM 32768u
#define TC_PLANAR_MAX_CHUNKS 128u

typedef struct planar_job_arg {
    const topos_planar_444_to_422_params* params;
    size_t u_in_stride;
    size_t v_in_stride;
    size_t u_out_stride;
    size_t v_out_stride;
    uint32_t chroma_width;
    uint32_t y0;
    uint32_t y1;
    uint8_t use_avx2;
} planar_job_arg;

static void planar_convert_rows(const planar_job_arg* arg)
{
    const topos_planar_444_to_422_params* p = arg->params;
#if defined(__x86_64__) || defined(_M_X64)
    if (arg->use_avx2 != 0u) {
        const tc_planar_convert_simd_ctx ctx = {
            p->u_in, p->v_in, p->u_out, p->v_out,
            arg->u_in_stride, arg->v_in_stride,
            arg->u_out_stride, arg->v_out_stride,
            p->width, arg->chroma_width,
        };
        tc_planar_convert_rows_avx2(&ctx, arg->y0, arg->y1);
        return;
    }
#endif
    for (uint32_t y = arg->y0; y < arg->y1; ++y) {
        const uint16_t* u_src = p->u_in + (size_t)y * arg->u_in_stride;
        const uint16_t* v_src = p->v_in + (size_t)y * arg->v_in_stride;
        uint16_t* u_dst = p->u_out + (size_t)y * arg->u_out_stride;
        uint16_t* v_dst = p->v_out + (size_t)y * arg->v_out_stride;
        for (uint32_t x = 0u; x < arg->chroma_width; ++x) {
            const uint32_t sx = x * 2u;
            const uint32_t sx1 = sx + 1u < p->width ? sx + 1u : sx;
            u_dst[x] = (uint16_t)(((uint32_t)u_src[sx]
                                   + (uint32_t)u_src[sx1] + 1u) >> 1u);
            v_dst[x] = (uint16_t)(((uint32_t)v_src[sx]
                                   + (uint32_t)v_src[sx1] + 1u) >> 1u);
        }
    }
}

static void planar_convert_job(void* opaque)
{
    planar_convert_rows((const planar_job_arg*)opaque);
}

int32_t tc_convert_yuv444_to_422(
    const topos_planar_444_to_422_params* params)
{
    if (params == NULL
            || params->struct_size
                != (uint32_t)sizeof(topos_planar_444_to_422_params)
            || params->width == 0u || params->height == 0u
            || params->width > TC_PLANAR_MAX_DIM
            || params->height > TC_PLANAR_MAX_DIM
            || (params->bit_depth != 10u && params->bit_depth != 12u
                && params->bit_depth != 16u)
            || params->u_in == NULL || params->v_in == NULL
            || params->u_out == NULL || params->v_out == NULL) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "planar 444->422: 参数越界或空指针");
        return TC_ERR_INVALID_ARGUMENT;
    }

    const uint32_t chroma_width = (params->width + 1u) / 2u;
    const size_t u_in_stride = params->u_in_stride != 0u
        ? params->u_in_stride : (size_t)params->width;
    const size_t v_in_stride = params->v_in_stride != 0u
        ? params->v_in_stride : (size_t)params->width;
    const size_t u_out_stride = params->u_out_stride != 0u
        ? params->u_out_stride : (size_t)chroma_width;
    const size_t v_out_stride = params->v_out_stride != 0u
        ? params->v_out_stride : (size_t)chroma_width;
    if (u_in_stride < (size_t)params->width
            || v_in_stride < (size_t)params->width
            || u_out_stride < (size_t)chroma_width
            || v_out_stride < (size_t)chroma_width) {
        tc_set_error(TC_ERR_INVALID_ARGUMENT,
                     "planar 444->422: stride 小于平面宽度");
        return TC_ERR_INVALID_ARGUMENT;
    }

    uint32_t workers = params->max_workers;
    if (workers == 0u) { workers = 1u; }
    if (workers > 64u) { workers = 64u; }
    if (workers <= 1u || params->height == 1u) {
        planar_job_arg arg = {
            params, u_in_stride, v_in_stride, u_out_stride, v_out_stride,
            chroma_width, 0u, params->height,
#if defined(__x86_64__) || defined(_M_X64)
            (uint8_t)(tc_cvt_have_avx2() != 0),
#else
            0u,
#endif
        };
        planar_convert_rows(&arg);
        return TC_OK;
    }

    uint32_t chunks = workers * 2u;
    if (chunks > TC_PLANAR_MAX_CHUNKS) { chunks = TC_PLANAR_MAX_CHUNKS; }
    if (chunks > params->height) { chunks = params->height; }
    const uint32_t chunk_height =
        (params->height + chunks - 1u) / chunks;
    const uint8_t use_avx2 =
#if defined(__x86_64__) || defined(_M_X64)
        (uint8_t)(tc_cvt_have_avx2() != 0);
#else
        0u;
#endif
    planar_job_arg args[TC_PLANAR_MAX_CHUNKS];
    tc_job jobs[TC_PLANAR_MAX_CHUNKS];
    uint32_t y = 0u;
    uint32_t count = 0u;
    while (count < chunks && y < params->height) {
        uint32_t y1 = y + chunk_height;
        if (y1 > params->height) { y1 = params->height; }
        args[count] = (planar_job_arg){
            params, u_in_stride, v_in_stride, u_out_stride, v_out_stride,
            chroma_width, y, y1, use_avx2,
        };
        jobs[count].fn = planar_convert_job;
        jobs[count].ctx = &args[count];
        y = y1;
        ++count;
    }
    (void)tc_parallel_for(jobs, count, workers);
    return TC_OK;
}
