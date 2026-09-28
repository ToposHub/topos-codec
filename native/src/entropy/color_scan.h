/* 颜色 slice 解码热路径融合（解码深化批次：M9 报告 §5 路径 1/2/4）。
 *
 * 与 slice_codec.c 的逐块离散版（tc_block_decode / tc_block_decode_vlc 每 块
 * 一次跨 TU 调用）语义逐位一致——差分测试（test_slice_codec 融合 vs 离散，
 * 逐截断位对拍）钉死：
 *  - 符号序列/域校验/错误码/消费位完全同序同值；
 *  - 快路未命中（长 unary/escape/窗口不足）不消费任何位，直接回退
 *    tc_rice_decode_inline / tc_vlc_*_slow，截断语义精确；
 *  - sink 语义增强 has_ac：解码器已知本块是否解出任何 (run,level) 对，
 *    sink 不再扫描 q[1..63]（Rice 域内 level m=0 → 系数 0 的病态流按 full
 *    路径处理——full IDCT 与 DC-only 闭式对该输入 bit-exact 等价，仅
 *    profile 的 dc_only 计数口径有差，注释见 sink 侧）；
 *  - EOB 紧随 DC 的块（高 QP 高频形态）跳过 64 字清零：has_ac=0 时 sink
 *    只读 q[0]。
 *
 * 结构收益（相对离散版）：
 *  1. 两级 LUT 行指针 / 码书指针 / 粘滞错误检查按 slice 预提升（原每符号一次
 *     atomic acquire + 参数重验）；
 *  2. reader 状态跨块驻留单函数内（原每块经调用边界换出/换入）；
 *  3. 与 bitio.h 字级 refill 配合，每符号装载为单次 8 字节载入。
 *
 * 本头只被 slice_codec.c 包含（内联展开单一定义点）；离散版保留为
 * 权威参考实现与非流式路径（CLI inspect / fuzz / 测试对拍）。
 */
#ifndef TOPOS_INTERNAL_COLOR_SCAN_H
#define TOPOS_INTERNAL_COLOR_SCAN_H

#include <stddef.h>
#include <stdint.h>

#include "../bitstream/slice_codec.h"
#include "../codec/color_store.h"
#include "../common/alloc.h"
#include "block_coding.h"
#include "rice.h"
#include "scan.h"
#include "vlc.h"

/* Rice 快路：与 tc_rice_decode_inline 的 LUT 段逐位一致（lut_row 为调用方
 * 预提升的 tc_rice_lut[k] 行，uint16 entry = (m<<4)|nbits）。短码先查
 * 8-bit 一级表，再查 12-bit 二级表；未命中回退 reservoir 慢路，不再为
 * 每个长码重新做 atomic acquire。
 * M10-6.3D：① 窗口 ≥12 位时免 refill（装载条件化——中部流 refill 后
 * ≥56 位，可连续供 4+ 符号；语义不变：窗口不足照旧装载后走同一判断）；
 * ② DC/level 变体消除域校验死码——LUT 命中域 m ≤ 2047 < 2^26 ≤ m_max，
 * 检查恒假（run m_max=63 在域内，保留检查的原变体）。 */
static inline int32_t tc_scan_rice_sym(tc_bitreader* br, const uint16_t* lut_row,
                                       uint32_t k, uint32_t m_max, uint32_t* m_out)
{
    return tc_rice_decode_lut_inline(br, tc_rice_short_lut[k], lut_row,
                                     k, m_max, m_out);
}

static inline int32_t tc_scan_rice_sym_big(tc_bitreader* br, const uint16_t* lut_row,
                                           uint32_t k, uint32_t m_max, uint32_t* m_out)
{
    return tc_rice_decode_lut_inline(br, tc_rice_short_lut[k], lut_row,
                                     k, m_max, m_out);
}

/* DC 行上下文已上移 slice_codec.h（M10-1：调用方 scratch 池化）。 */

/* 融合解码主体（Rice / VLC 两变体共用骨架，熵符号按 entropy 分支内联）。
 * 返回语义与 tc_color_slice_decode_stream 完全一致（含尾随字节校验）。
 * dc 由调用方提供（≥ cols 元素双行）；入口统一清零两条有效行——池化
 * scratch 的跨 slice 残留不复位将污染首行预测，此为唯一状态复位点。 */

static inline int32_t tc_color_scan_decode_scratch(const topos_frame_header* fh,
                                                    const topos_slice_header* sh,
                                                    const uint8_t* payload, size_t payload_size,
                                                    tc_color_block_sink_fn sink, void* ctx,
                                                    uint64_t* symbol_hash,
                                                    tc_scan_dc_ctx* dc)
{
    const int use_vlc = fh->version_major == 2u && fh->entropy_mode == 1u;
    tc_bitreader br;
    tc_bitreader_init(&br, payload, payload_size);

    uint32_t cols = fh->plane_block_cols[sh->plane];
    uint32_t band = sh->block_h;
    const int want_hash = symbol_hash != NULL;
    uint64_t hash = 0;

    /* 每 slice 一次：LUT 行 / 码书 / 错误预检（失败即返回，与离散版同码） */
    const uint16_t* lut_dc = NULL;
    const uint16_t* lut_lvl = NULL;
    const uint16_t* lut_run = NULL;
    const tc_vlc_book* dc_b = NULL;
    const tc_vlc_book* lvl_b = NULL;
    const tc_vlc_book* run_b = NULL;
    if (use_vlc != 0) {
        int32_t rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_DC, sh->k1);
        if (rc != TC_OK) { return rc; }
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_LVL, sh->k2);
        if (rc != TC_OK) { return rc; }
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_RUN, sh->k3);
        if (rc != TC_OK) { return rc; }
        dc_b = tc_vlc_book_get(TC_VLC_FAMILY_DC, sh->k1);
        lvl_b = tc_vlc_book_get(TC_VLC_FAMILY_LVL, sh->k2);
        run_b = tc_vlc_book_get(TC_VLC_FAMILY_RUN, sh->k3);
        if (dc_b == NULL || lvl_b == NULL || run_b == NULL) {
            tc_set_error(TC_ERR_STATE, "vlc book unavailable");
            return TC_ERR_STATE;
        }
    } else {
        int32_t rc = tc_rice_lut_ensure(sh->k1);
        if (rc != TC_OK) { return rc; }
        rc = tc_rice_lut_ensure(sh->k2);
        if (rc != TC_OK) { return rc; }
        rc = tc_rice_lut_ensure(sh->k3);
        if (rc != TC_OK) { return rc; }
        lut_dc = tc_rice_lut[sh->k1];
        lut_lvl = tc_rice_lut[sh->k2];
        lut_run = tc_rice_lut[sh->k3];
    }

    /* 池化 scratch 状态复位：两条有效行清零（等价旧 tc_calloc 初始语义） */
    {
        const size_t clear_bytes = (size_t)cols * sizeof(int32_t);
        memset(dc->prev_row, 0, clear_bytes);
        memset(dc->row, 0, clear_bytes);
    }

    int32_t blk[64];
    int32_t rc = TC_OK;
    for (uint32_t by = 0u; by < band; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            const int has_left = bx > 0u;
            const int has_top = by > 0u;
            const int32_t left_dc = has_left ? dc->row[bx - 1u] : 0;
            const int32_t top_dc = dc->prev_row[bx];

            /* DC 符号 + 预测（顺序与离散版一致：先解码后预测） */
            uint32_t m = 0;
            if (use_vlc != 0) {
                rc = tc_vlc_decode_cat(&br, dc_b, TC_RICE_M_MAX_DC, &m);
            } else {
                rc = tc_scan_rice_sym_big(&br, lut_dc, sh->k1, TC_RICE_M_MAX_DC, &m);
            }
            if (rc != TC_OK) { goto done; }
            const int32_t pred = tc_dc_predict(has_left, left_dc, has_top, top_dc);
            int32_t dc_val = 0;
            /* P0-07：DC 域检查（spec §7.6 ±2^25）；越界 = 恶意码流 →
             * reader 置粘滞错误，slice 级 concealment */
            rc = tc_dc_reconstruct_checked(pred, m, &dc_val);
            if (rc != TC_OK) {
                tc_bitreader_fail(&br, rc);
                goto done;
            }

            /* run 符号先行：EOB 紧随 DC → has_ac=0，免 64 字清零（sink 只读
             * q[0]；blk[1..63] 保留上一块残留，不被任何读者触碰） */
            uint32_t run = 0;
            if (use_vlc != 0) {
                rc = tc_vlc_decode_sym(&br, run_b, &run);
            } else {
                rc = tc_scan_rice_sym(&br, lut_run, sh->k3, TC_RICE_M_MAX_RUN, &run);
            }
            if (rc != TC_OK) { goto done; }

            if (run == 63u) {
                blk[0] = dc_val;
                if (want_hash != 0) {
                    /* 指纹路径要求 blk[1..63] 为解码语义值（全零）——免清零
                     * 优化只在无指纹时成立 */
                    for (uint32_t i = 1u; i < 64u; ++i) { blk[i] = 0; }
                    for (uint32_t i = 0u; i < 64u; ++i) {
                        hash = tc_symbol_hash_mix(hash, (uint64_t)(int64_t)blk[i]);
                    }
                }
                dc->row[bx] = dc_val;
                rc = sink(ctx, by * cols + bx, blk, 0u);
                if (rc != TC_OK) { goto done; }
                continue;
            }

            for (uint32_t i = 0u; i < 64u; ++i) { blk[i] = 0; }
            blk[0] = dc_val;
            uint32_t pos = 1u;
            uint32_t rm = 1u; /* 自然序非零行掩码：DC 恒在行 0 */
            for (;;) {
                uint64_t idx = (uint64_t)pos + (uint64_t)run;
                if (idx > 63u) {
                    tc_bitreader_fail(&br, TC_ERR_MALFORMED);
                    rc = TC_ERR_MALFORMED;
                    goto done;
                }
                uint32_t lvl = 0;
                if (use_vlc != 0) {
                    rc = tc_vlc_decode_cat(&br, lvl_b, TC_RICE_M_MAX_AC_LEVEL, &lvl);
                } else {
                    rc = tc_scan_rice_sym_big(&br, lut_lvl, sh->k2, TC_RICE_M_MAX_AC_LEVEL, &lvl);
                }
                if (rc != TC_OK) { goto done; }
                uint32_t nat = kTcZigzag[idx];
                blk[nat] = tc_rice_unmap_signed(lvl);
                rm |= 1u << (nat >> 3); /* 散射顺带积累：融合 IDCT 免 64 扫 */
                pos = (uint32_t)idx + 1u;

                if (use_vlc != 0) {
                    rc = tc_vlc_decode_sym(&br, run_b, &run);
                } else {
                    rc = tc_scan_rice_sym(&br, lut_run, sh->k3, TC_RICE_M_MAX_RUN, &run);
                }
                if (rc != TC_OK) { goto done; }
                if (run == 63u) { break; } /* EOB */
            }
            if (want_hash != 0) {
                for (uint32_t i = 0u; i < 64u; ++i) {
                    hash = tc_symbol_hash_mix(hash, (uint64_t)(int64_t)blk[i]);
                }
            }
            dc->row[bx] = dc_val;
            rc = sink(ctx, by * cols + bx, blk, rm);
            if (rc != TC_OK) { goto done; }
        }
        tc_scan_dc_swap(dc);
    }

done:
    if (rc != TC_OK) { return rc; }

    /* payload 必须被恰好消费（对齐填充允许；额外数据 = MALFORMED） */
    int32_t arc = tc_bitreader_align_byte(&br);
    if (arc != TC_OK) { return arc; }
    if (tc_bitreader_bits_consumed(&br) != (uint64_t)payload_size * 8u) {
        tc_set_error(TC_ERR_MALFORMED, "color slice trailing bytes");
        return TC_ERR_MALFORMED;
    }
    if (symbol_hash != NULL) { *symbol_hash = hash; }
    return TC_OK;
}

/* 无 scratch 兼容入口（离散/状态less 路径）：临时分配双行，语义与池化
 * 路径逐位一致（扫描入口统一清零，calloc 仅为分配语义）。 */
static inline int32_t tc_color_scan_decode(const topos_frame_header* fh,
                                           const topos_slice_header* sh,
                                           const uint8_t* payload, size_t payload_size,
                                           tc_color_block_sink_fn sink, void* ctx,
                                           uint64_t* symbol_hash)
{
    tc_scan_dc_ctx dc;
    dc.elems = fh->plane_block_cols[sh->plane] > 0u
                   ? (size_t)fh->plane_block_cols[sh->plane] : 1u;
    dc.prev_row = (int32_t*)tc_alloc(dc.elems * sizeof(int32_t));
    dc.row = (int32_t*)tc_alloc(dc.elems * sizeof(int32_t));
    if (dc.prev_row == NULL || dc.row == NULL) {
        tc_free(dc.prev_row);
        tc_free(dc.row);
        tc_set_error(TC_ERR_OUT_OF_MEMORY, "color slice dc buffers");
        return TC_ERR_OUT_OF_MEMORY;
    }
    int32_t rc = tc_color_scan_decode_scratch(fh, sh, payload, payload_size, sink, ctx,
                                              symbol_hash, &dc);
    tc_free(dc.prev_row);
    tc_free(dc.row);
    return rc;
}

/* M10-2A：专用 reduced scan-to-plane 生产热路径。
 * 与 tc_color_scan_decode_scratch 同骨架，但 sink 调用在块循环内直接展开为
 * tc_color_store_block（共享单一定义点）——每块不再经函数指针边界，reader
 * 状态/量化上下文/目的平面状态跨块驻留寄存器，DC-only fill、批量反量化
 * IDCT 与 store 在同一优化单元内。无 symbol_hash（生产不解指纹）。
 * 返回语义/错误码/消费位与 generic 版逐位一致（差分测试钉死）。 */
static inline int32_t tc_color_scan_to_plane_reduced(const topos_frame_header* fh,
                                                     const topos_slice_header* sh,
                                                     const uint8_t* payload, size_t payload_size,
                                                     tc_scan_dc_ctx* dc,
                                                     tc_color_store_ctx* store)
{
    const int use_vlc = fh->version_major == 2u && fh->entropy_mode == 1u;
    tc_bitreader br;
    tc_bitreader_init(&br, payload, payload_size);

    uint32_t cols = fh->plane_block_cols[sh->plane];
    uint32_t band = sh->block_h;
    const uint32_t coefficient_limit = store->coefficient_limit == 0u
                                     ? 63u : (uint32_t)store->coefficient_limit;

    /* 每 slice 一次：LUT 行 / 码书 / 错误预检（失败即返回，与离散版同码） */
    const uint16_t* lut_dc = NULL;
    const uint16_t* lut_lvl = NULL;
    const uint16_t* lut_run = NULL;
    const tc_vlc_book* dc_b = NULL;
    const tc_vlc_book* lvl_b = NULL;
    const tc_vlc_book* run_b = NULL;
    if (use_vlc != 0) {
        int32_t rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_DC, sh->k1);
        if (rc != TC_OK) { return rc; }
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_LVL, sh->k2);
        if (rc != TC_OK) { return rc; }
        rc = tc_vlc_tables_ensure(TC_VLC_FAMILY_RUN, sh->k3);
        if (rc != TC_OK) { return rc; }
        dc_b = tc_vlc_book_get(TC_VLC_FAMILY_DC, sh->k1);
        lvl_b = tc_vlc_book_get(TC_VLC_FAMILY_LVL, sh->k2);
        run_b = tc_vlc_book_get(TC_VLC_FAMILY_RUN, sh->k3);
        if (dc_b == NULL || lvl_b == NULL || run_b == NULL) {
            tc_set_error(TC_ERR_STATE, "vlc book unavailable");
            return TC_ERR_STATE;
        }
    } else {
        int32_t rc = tc_rice_lut_ensure(sh->k1);
        if (rc != TC_OK) { return rc; }
        rc = tc_rice_lut_ensure(sh->k2);
        if (rc != TC_OK) { return rc; }
        rc = tc_rice_lut_ensure(sh->k3);
        if (rc != TC_OK) { return rc; }
        lut_dc = tc_rice_lut[sh->k1];
        lut_lvl = tc_rice_lut[sh->k2];
        lut_run = tc_rice_lut[sh->k3];
    }

    /* 池化 scratch 状态复位：两条有效行清零（等价旧 tc_calloc 初始语义） */
    {
        const size_t clear_bytes = (size_t)cols * sizeof(int32_t);
        memset(dc->prev_row, 0, clear_bytes);
        memset(dc->row, 0, clear_bytes);
    }

    int32_t blk[64];
    int32_t rc = TC_OK;
    /* M10-2C：行内四块批量（稀疏禁用时启用；块缓冲 SoA + 尾块回退）。
     * scaled=1 时两条加速路径都关闭：sparse/batch 面向完整 8×8 直写，
     * scaled 的每块成本由逐点采样逆变换决定（只算目标采样点），走
     * tc_color_store_block_xy 的 scaled 分支才是最短路径。 */
    const int batch_on = store->scaled == 0u && tc_dev_sparse_threshold() == 0
                       && tc_dev_batch_idct() == 0;
    int64_t qsoa[64 * 4];
    int32_t xh4[4 * 64];
    uint32_t batch_idx[4];
    uint32_t batch_rm[4];
    int nb = 0;
    for (uint32_t by = 0u; by < band; ++by) {
        for (uint32_t bx = 0u; bx < cols; ++bx) {
            const int has_left = bx > 0u;
            const int has_top = by > 0u;
            const int32_t left_dc = has_left ? dc->row[bx - 1u] : 0;
            const int32_t top_dc = dc->prev_row[bx];

            uint32_t m = 0;
            if (use_vlc != 0) {
                rc = tc_vlc_decode_cat(&br, dc_b, TC_RICE_M_MAX_DC, &m);
            } else {
                rc = tc_scan_rice_sym_big(&br, lut_dc, sh->k1, TC_RICE_M_MAX_DC, &m);
            }
            if (rc != TC_OK) { goto done; }
            if (store->stats != NULL) { store->stats->entropy_symbols++; }
            const int32_t pred = tc_dc_predict(has_left, left_dc, has_top, top_dc);
            int32_t dc_val = 0;
            /* P0-07：DC 域检查（spec §7.6 ±2^25）；越界 = 恶意码流 →
             * reader 置粘滞错误，slice 级 concealment */
            rc = tc_dc_reconstruct_checked(pred, m, &dc_val);
            if (rc != TC_OK) {
                tc_bitreader_fail(&br, rc);
                goto done;
            }

            uint32_t run = 0;
            if (use_vlc != 0) {
                rc = tc_vlc_decode_sym(&br, run_b, &run);
            } else {
                rc = tc_scan_rice_sym(&br, lut_run, sh->k3, TC_RICE_M_MAX_RUN, &run);
            }
            if (rc != TC_OK) { goto done; }
            if (store->stats != NULL) { store->stats->entropy_symbols++; }

            if (run == 63u) {
                blk[0] = dc_val;
                dc->row[bx] = dc_val;
                rc = tc_color_store_block_xy(store, by * cols + bx, bx,
                                             store->block_y0 + by, blk, 0u);
                if (rc != TC_OK) { goto done; }
                continue;
            }

            /* M10-2B：AC 对先入稀疏收集缓冲（免 q[64] 清零/散写）；
             * 超阈值一次性回补 blk[] 并转入稠密路径 */
            uint8_t sp_nat[TC_SPARSE_MAX_AC];
            int32_t sp_lvl[TC_SPARSE_MAX_AC];
            uint32_t sp_n = 0u;
            int sp_dense = 0;
            uint32_t pos = 1u;
            uint32_t rm = 1u; /* 自然序非零行掩码：DC 恒在行 0 */
            const uint32_t sparse_thr = store->scaled != 0u
                                      ? 0u : (uint32_t)tc_dev_sparse_threshold();
            /* 扫描序单调递增：一旦当前位置越过 reduced band，后续 AC
             * 只需要消费 level/run 的码字以保持 bitstream 对齐，不必再做
             * zigzag 映射、signed unmap 或稀疏收集。旧实现虽然不写入高频，
             * 仍为每个高频符号执行了这些重建前工作。 */
            int discard_ac = 0;
            for (;;) {
                uint64_t idx = (uint64_t)pos + (uint64_t)run;
                if (idx > 63u) {
                    tc_bitreader_fail(&br, TC_ERR_MALFORMED);
                    rc = TC_ERR_MALFORMED;
                    goto done;
                }
                uint32_t lvl = 0;
                if (use_vlc != 0) {
                    rc = tc_vlc_decode_cat(&br, lvl_b, TC_RICE_M_MAX_AC_LEVEL, &lvl);
                } else {
                    rc = tc_scan_rice_sym_big(&br, lut_lvl, sh->k2, TC_RICE_M_MAX_AC_LEVEL, &lvl);
                }
                if (rc != TC_OK) { goto done; }
                if (store->stats != NULL) { store->stats->entropy_symbols++; }
                const int keep = discard_ac == 0 &&
                                 idx <= (uint64_t)coefficient_limit;
                if (keep == 0) {
                    if (store->stats != NULL) {
                        store->stats->coefficients_skipped++;
                    }
                    discard_ac = 1;
                } else {
                    const uint32_t nat = kTcZigzag[idx];
                    const int32_t lvl_val = tc_rice_unmap_signed(lvl);
                    if (sp_dense == 0) {
                        if (sp_n < sparse_thr && sp_n < (uint32_t)TC_SPARSE_MAX_AC) {
                            sp_nat[sp_n] = (uint8_t)nat;
                            sp_lvl[sp_n] = lvl_val;
                            sp_n++;
                        } else {
                            /* 回退：清零 + 回补已收对（含行掩码）后走稠密散写 */
                            sp_dense = 1;
                            for (uint32_t i = 0u; i < 64u; ++i) { blk[i] = 0; }
                            blk[0] = dc_val;
                            for (uint32_t i = 0u; i < sp_n; ++i) {
                                blk[sp_nat[i]] = sp_lvl[i];
                                rm |= 1u << (sp_nat[i] >> 3);
                            }
                        }
                    }
                    if (sp_dense != 0) {
                        blk[nat] = lvl_val;
                        rm |= 1u << (nat >> 3); /* 散射顺带积累：融合 IDCT 免 64 扫 */
                    }
                }
                pos = (uint32_t)idx + 1u;

                if (use_vlc != 0) {
                    rc = tc_vlc_decode_sym(&br, run_b, &run);
                } else {
                    rc = tc_scan_rice_sym(&br, lut_run, sh->k3, TC_RICE_M_MAX_RUN, &run);
                }
                if (rc != TC_OK) { goto done; }
                if (store->stats != NULL) { store->stats->entropy_symbols++; }
                if (run == 63u) { break; } /* EOB */
            }
            dc->row[bx] = dc_val;
            if (sp_dense == 0 && sp_n != 0u) {
                rc = tc_color_store_block_sparse_xy(store, by * cols + bx, bx,
                                                    store->block_y0 + by, dc_val,
                                                    sp_lvl, sp_nat, sp_n);
            } else if (sp_dense == 0 && sp_n == 0u) {
                /* reduced-coefficient 路径可能丢弃整块 AC，但 DC 仍需重建。 */
                blk[0] = dc_val;
                rc = tc_color_store_block_xy(store, by * cols + bx, bx,
                                             store->block_y0 + by, blk, 0u);
            } else if (batch_on != 0 && rm != 0u) {
                for (uint32_t i = 0u; i < 64u; ++i) {
                    qsoa[i * 4u + (uint32_t)nb] = blk[i];
                }
                batch_idx[nb] = by * cols + bx;
                batch_rm[nb] = rm;
                nb++;
                if (nb == 4) {
                    rc = tc_color_batch_commit(store, qsoa, xh4, batch_idx, batch_rm, 4);
                    nb = 0;
                }
            } else {
                rc = tc_color_store_block_xy(store, by * cols + bx, bx,
                                             store->block_y0 + by, blk, rm);
            }
            if (rc != TC_OK) { goto done; }
        }
        /* 行尾批量冲刷（批量块不跨块行——store 的块行偏移按 idx 计算，
         * 跨行提交在语义上等价，但行界冲刷保持尾块行为局部化） */
        if (nb != 0) {
            rc = tc_color_batch_flush(store, qsoa, xh4, batch_idx, batch_rm, &nb);
            if (rc != TC_OK) { goto done; }
        }
        tc_scan_dc_swap(dc);
    }

done:
    if (nb != 0) {
        /* 错误路径冲刷：已解码未提交的批量块仍落盘（slice 失败时该带
         * 随后整体 conceal 覆盖——此处仅为保持最大保真的一致行为） */
        (void)tc_color_batch_flush(store, qsoa, xh4, batch_idx, batch_rm, &nb);
    }
    if (rc != TC_OK) { return rc; }

    /* payload 必须被恰好消费（对齐填充允许；额外数据 = MALFORMED） */
    int32_t arc = tc_bitreader_align_byte(&br);
    if (arc != TC_OK) { return arc; }
    if (tc_bitreader_bits_consumed(&br) != (uint64_t)payload_size * 8u) {
        tc_set_error(TC_ERR_MALFORMED, "color slice trailing bytes");
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

/* Full mode has its own implementation so the reduced coefficient-limit
 * checks never enter the normal decode loop. */
#include "color_scan_full.h"

static inline int32_t tc_color_scan_to_plane(const topos_frame_header* fh,
                                             const topos_slice_header* sh,
                                             const uint8_t* payload, size_t payload_size,
                                             tc_scan_dc_ctx* dc,
                                             tc_color_store_ctx* store)
{
    const uint32_t coefficient_limit = store->coefficient_limit == 0u
                                     ? 63u : (uint32_t)store->coefficient_limit;
    if (coefficient_limit >= 63u) {
        return tc_color_scan_to_plane_full(fh, sh, payload, payload_size, dc, store);
    }
    return tc_color_scan_to_plane_reduced(fh, sh, payload, payload_size, dc, store);
}

#endif /* TOPOS_INTERNAL_COLOR_SCAN_H */
