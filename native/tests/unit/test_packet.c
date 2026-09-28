/* packet：synth 往返、逐字节截断/翻转矩阵、伪造字段、排列破坏、CRC 行为 */
#include "bitstream/packet.h"
#include "bitstream/slice_codec.h"

#include <stdlib.h>
#include <string.h>

#include "common/crc32.h"
#include "common/endian.h"
#include "mini_test.h"
#include "packet_synth.h"

/* 在副本上 patch 并保持 frame CRC 自洽（供字段级伪造测试） */
static int32_t scan_patched(const uint8_t* src, size_t size, size_t off,
                            const uint8_t* patch, size_t plen, topos_packet_view* view)
{
    uint8_t* buf = (uint8_t*)malloc(size);
    if (buf == NULL) { return TC_ERR_OUT_OF_MEMORY; }
    memcpy(buf, src, size);
    memcpy(buf + off, patch, plen);
    if (off < TC_FRAME_HEADER_SIZE) {
        tc_store_be32(buf + 49, tc_crc32(buf, 49u));
    }
    int32_t rc = tc_packet_scan(buf, size, view);
    free(buf);
    return rc;
}

static int32_t decode_all_slices(const topos_packet_view* v)
{
    for (uint16_t i = 0; i < v->slice_count; ++i) {
        int32_t rc;
        if (v->slices[i].plane == 3u) {
            rc = tc_alpha_slice_decode(&v->fh, &v->slices[i], v->payloads[i],
                                       (size_t)v->slices[i].slice_payload_size, NULL, 0u, NULL);
        } else {
            rc = tc_color_slice_decode(&v->fh, &v->slices[i], v->payloads[i],
                                       (size_t)v->slices[i].slice_payload_size, NULL, NULL);
        }
        if (rc != TC_OK) { return rc; }
    }
    return TC_OK;
}

int main(void)
{
    /* ---- 全配置合成 → 扫描 → 逐片符号解码全通过 ---- */
    for (unsigned c = 0u; c < PACKET_SYNTH_CFG_COUNT; ++c) {
        const packet_synth_cfg* cfg = packet_synth_cfg_at(c);
        uint8_t* data = NULL;
        size_t size = 0;
        int32_t rc = packet_synth_build(cfg, &data, &size);
        if (rc != TC_OK) {
            fprintf(stderr, "FAIL cfg %u synth: %d (%s)\n", c, rc, tc_last_error());
            mt_failures++;
            continue;
        }
        topos_packet_view view;
        rc = tc_packet_scan(data, size, &view);
        if (rc != TC_OK) {
            fprintf(stderr, "FAIL cfg %u scan: %d (%s)\n", c, rc, tc_last_error());
            mt_failures++;
            free(data);
            continue;
        }
        MT_CHECK_EQ_U64(view.slice_count, (uint64_t)((uint32_t)cfg->bands * view.fh.plane_count));
        for (uint16_t i = 0; i < view.slice_count; ++i) {
            if (view.slice_crc_ok[i] != 1u) {
                mt_report(__FILE__, __LINE__, "synth slice crc must be ok");
                break;
            }
        }
        int32_t drc = decode_all_slices(&view);
        if (drc != TC_OK) {
            fprintf(stderr, "FAIL cfg %u slice decode: %d\n", c, drc);
            mt_failures++;
        }

        /* 确定性：同 cfg 重建 bit-exact */
        uint8_t* again = NULL;
        size_t again_size = 0;
        if (packet_synth_build(cfg, &again, &again_size) == TC_OK) {
            if (again_size != size || memcmp(again, data, size) != 0) {
                mt_report(__FILE__, __LINE__, "synth determinism");
            }
            free(again);
        }
        free(data);
    }

    /* 用 cfg TINY 做破坏矩阵 */
    uint8_t* pkt = NULL;
    size_t n = 0;
    MT_CHECK_EQ_I64(packet_synth_build(packet_synth_cfg_at(PACKET_SYNTH_CFG_TINY), &pkt, &n),
                    TC_OK);
    MT_CHECK(n > 70u);

    /* ---- 任意截断位置：必然整帧拒绝（不越界、不崩溃）---- */
    {
        int bad = 0;
        for (size_t cut = 0; cut < n; ++cut) {
            topos_packet_view view;
            int32_t rc = tc_packet_scan(pkt, cut, &view);
            if (rc >= 0) { bad = 1; break; }
        }
        MT_CHECK_EQ_I64(bad, 0);
    }

    /* ---- 逐字节翻转：要么拒绝，要么可扫（CRC 破坏可被 payload CRC 消化）----
       扫描通过时符号解码允许失败（concealment 路径），但绝不崩溃/挂起。 */
    {
        uint8_t* buf = (uint8_t*)malloc(n);
        MT_CHECK(buf != NULL);
        for (size_t i = 0; i < n; ++i) {
            memcpy(buf, pkt, n);
            buf[i] ^= (uint8_t)(1u << (mt_rand_u64() % 8ull));
            topos_packet_view view;
            int32_t rc = tc_packet_scan(buf, n, &view);
            if (rc == TC_OK) { (void)decode_all_slices(&view); }
        }
        free(buf);
    }

    /* ---- 伪造 frame_packet_size（重算 CRC 后仍被布局校验拒绝）---- */
    {
        topos_packet_view view;
        uint8_t plus1[4] = { 0, 0, 0, 0 };
        uint32_t wrong = (uint32_t)n + 1u;
        tc_store_be32(plus1, wrong);
        MT_CHECK_EQ_I64(scan_patched(pkt, n, 41u, plus1, 4u, &view), TC_ERR_MALFORMED);
        tc_store_be32(plus1, (uint32_t)n - 1u);
        MT_CHECK_EQ_I64(scan_patched(pkt, n, 41u, plus1, 4u, &view), TC_ERR_MALFORMED);
        tc_store_be32(plus1, 0xFFFFFFFFu);
        MT_CHECK_EQ_I64(scan_patched(pkt, n, 41u, plus1, 4u, &view), TC_ERR_LIMIT_EXCEEDED);
    }

    /* ---- 伪造 slice payload size（无 slice header CRC → 布局拒绝）---- */
    {
        topos_packet_view view;
        topos_packet_view probe;
        MT_CHECK_EQ_I64(tc_packet_scan(pkt, n, &probe), TC_OK);
        size_t sh0 = TC_FRAME_HEADER_SIZE; /* 第一个 slice header 偏移 */
        uint8_t huge[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        MT_CHECK_EQ_I64(scan_patched(pkt, n, sh0, huge, 4u, &view), TC_ERR_TRUNCATED);
        uint8_t zero4[4] = { 0, 0, 0, 0 };
        /* payload=0：结构可通过（随后由精确耗尽校验拒绝） */
        int32_t rc = scan_patched(pkt, n, sh0, zero4, 4u, &view);
        MT_CHECK(rc == TC_ERR_MALFORMED || rc == TC_ERR_TRUNCATED);
    }

    /* ---- 追加字节 → MALFORMED ---- */
    {
        uint8_t* buf = (uint8_t*)malloc(n + 1u);
        MT_CHECK(buf != NULL);
        memcpy(buf, pkt, n);
        buf[n] = 0;
        topos_packet_view view;
        MT_CHECK_EQ_I64(tc_packet_scan(buf, n + 1u, &view), TC_ERR_MALFORMED);
        free(buf);
    }

    /* ---- 排列破坏：首 slice block_y0=1（缺口）---- */
    {
        topos_packet_view view;
        uint8_t y1[2] = { 0, 1 };
        MT_CHECK_EQ_I64(scan_patched(pkt, n, TC_FRAME_HEADER_SIZE + 5u, y1, 2u, &view),
                        TC_ERR_MALFORMED);
    }
    /* ---- qp 有效值越界（qp_base=20, delta=0 → −40）---- */
    {
        topos_packet_view view;
        uint8_t d0[1] = { 0 };
        MT_CHECK_EQ_I64(scan_patched(pkt, n, TC_FRAME_HEADER_SIZE + 9u, d0, 1u, &view),
                        TC_ERR_MALFORMED);
    }
    /* ---- k3=15（颜色 slice）---- */
    {
        topos_packet_view view;
        uint8_t k15[1] = { 15 };
        MT_CHECK_EQ_I64(scan_patched(pkt, n, TC_FRAME_HEADER_SIZE + 12u, k15, 1u, &view),
                        TC_ERR_MALFORMED);
    }

    /* ---- payload 翻转：扫描仍 OK（整帧结构完好），该片 CRC 标 0 且符号解码失败 ---- */
    {
        topos_packet_view probe;
        MT_CHECK_EQ_I64(tc_packet_scan(pkt, n, &probe), TC_OK);
        size_t p0 = TC_FRAME_HEADER_SIZE + TC_SLICE_HEADER_SIZE; /* slice0 payload 起点 */
        uint8_t* buf = (uint8_t*)malloc(n);
        MT_CHECK(buf != NULL);
        memcpy(buf, pkt, n);
        buf[p0] ^= 0x80u;
        topos_packet_view view;
        MT_CHECK_EQ_I64(tc_packet_scan(buf, n, &view), TC_OK);
        MT_CHECK_EQ_U64(view.slice_crc_ok[0], 0ull);
        MT_CHECK(view.slice_crc_ok[1] == 1u);
        int32_t drc = decode_all_slices(&view);
        MT_CHECK(drc == TC_ERR_TRUNCATED || drc == TC_ERR_MALFORMED);
        free(buf);
    }

    /* ---- 交换前两个 slice（Y 带顺序破坏）→ MALFORMED ---- */
    {
        topos_packet_view probe;
        MT_CHECK_EQ_I64(tc_packet_scan(pkt, n, &probe), TC_OK);
        if (probe.slice_count >= 2u) {
            size_t s0 = TC_FRAME_HEADER_SIZE;
            size_t len0 = TC_SLICE_HEADER_SIZE + probe.slices[0].slice_payload_size;
            size_t s1 = s0 + len0;
            size_t len1 = TC_SLICE_HEADER_SIZE + probe.slices[1].slice_payload_size;
            uint8_t* buf = (uint8_t*)malloc(n);
            MT_CHECK(buf != NULL);
            memcpy(buf, pkt, n);
            uint8_t* tmp = (uint8_t*)malloc(len0 > len1 ? len0 : len1);
            memcpy(tmp, buf + s0, len0);
            memcpy(buf + s0, buf + s1, len1);
            memcpy(buf + s0 + len1, tmp, len0);
            topos_packet_view view;
            int32_t rc = tc_packet_scan(buf, n, &view);
            if (probe.slices[0].block_y0 == probe.slices[1].block_y0) {
                /* cfg TINY 单带单平面无第二 Y slice：换位后跨平面乱序必然 MALFORMED */
                MT_CHECK_EQ_I64(rc, TC_ERR_MALFORMED);
            }
            free(tmp);
            free(buf);
        }
    }

    free(pkt);
    return MT_MAIN_RETURN();
}
