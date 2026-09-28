/* Full-coefficient production scanner.
 *
 * This file is included by color_scan.h after the shared Rice/VLC helpers and
 * the reduced scanner have been declared.  Keeping the full loop separate is
 * intentional: the normal playback path must not execute reduced-preview
 * coefficient-limit branches for every decoded AC symbol.
 */
#ifndef TOPOS_INTERNAL_COLOR_SCAN_FULL_H
#define TOPOS_INTERNAL_COLOR_SCAN_FULL_H

static inline int32_t tc_color_scan_to_plane_full(const topos_frame_header* fh,
                                                  const topos_slice_header* sh,
                                                  const uint8_t* payload, size_t payload_size,
                                                  tc_scan_dc_ctx* dc,
                                                  tc_color_store_ctx* store)
{
    const int use_vlc = fh->version_major == 2u && fh->entropy_mode == 1u;
    tc_bitreader br;
    tc_bitreader_init(&br, payload, payload_size);

    const uint32_t cols = fh->plane_block_cols[sh->plane];
    const uint32_t band = sh->block_h;

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

    const size_t clear_bytes = (size_t)cols * sizeof(int32_t);
    memset(dc->prev_row, 0, clear_bytes);
    memset(dc->row, 0, clear_bytes);

    int32_t blk[64];
    int32_t rc = TC_OK;
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

            uint8_t sp_nat[TC_SPARSE_MAX_AC];
            int32_t sp_lvl[TC_SPARSE_MAX_AC];
            uint32_t sp_n = 0u;
            int sp_dense = 0;
            uint32_t pos = 1u;
            uint32_t rm = 1u;
            const uint32_t sparse_thr = store->scaled != 0u
                                      ? 0u : (uint32_t)tc_dev_sparse_threshold();
            for (;;) {
                const uint64_t idx = (uint64_t)pos + (uint64_t)run;
                if (idx > 63u) {
                    tc_bitreader_fail(&br, TC_ERR_MALFORMED);
                    rc = TC_ERR_MALFORMED;
                    goto done;
                }
                uint32_t lvl = 0;
                if (use_vlc != 0) {
                    rc = tc_vlc_decode_cat(&br, lvl_b, TC_RICE_M_MAX_AC_LEVEL, &lvl);
                } else {
                    rc = tc_scan_rice_sym_big(&br, lut_lvl, sh->k2,
                                              TC_RICE_M_MAX_AC_LEVEL, &lvl);
                }
                if (rc != TC_OK) { goto done; }
                if (store->stats != NULL) { store->stats->entropy_symbols++; }
                const uint32_t nat = kTcZigzag[idx];
                const int32_t lvl_val = tc_rice_unmap_signed(lvl);
                if (sp_dense == 0) {
                    if (sp_n < sparse_thr && sp_n < (uint32_t)TC_SPARSE_MAX_AC) {
                        sp_nat[sp_n] = (uint8_t)nat;
                        sp_lvl[sp_n] = lvl_val;
                        sp_n++;
                    } else {
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
                    rm |= 1u << (nat >> 3);
                }
                pos = (uint32_t)idx + 1u;

                if (use_vlc != 0) {
                    rc = tc_vlc_decode_sym(&br, run_b, &run);
                } else {
                    rc = tc_scan_rice_sym(&br, lut_run, sh->k3,
                                          TC_RICE_M_MAX_RUN, &run);
                }
                if (rc != TC_OK) { goto done; }
                if (store->stats != NULL) { store->stats->entropy_symbols++; }
                if (run == 63u) { break; }
            }
            dc->row[bx] = dc_val;
            if (sp_dense == 0 && sp_n != 0u) {
                rc = tc_color_store_block_sparse_xy(store, by * cols + bx, bx,
                                                    store->block_y0 + by, dc_val,
                                                    sp_lvl, sp_nat, sp_n);
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
        if (nb != 0) {
            rc = tc_color_batch_flush(store, qsoa, xh4, batch_idx, batch_rm, &nb);
            if (rc != TC_OK) { goto done; }
        }
        tc_scan_dc_swap(dc);
    }

done:
    if (nb != 0) {
        (void)tc_color_batch_flush(store, qsoa, xh4, batch_idx, batch_rm, &nb);
    }
    if (rc != TC_OK) { return rc; }
    const int32_t arc = tc_bitreader_align_byte(&br);
    if (arc != TC_OK) { return arc; }
    if (tc_bitreader_bits_consumed(&br) != (uint64_t)payload_size * 8u) {
        tc_set_error(TC_ERR_MALFORMED, "color slice trailing bytes");
        return TC_ERR_MALFORMED;
    }
    return TC_OK;
}

#endif /* TOPOS_INTERNAL_COLOR_SCAN_FULL_H */
