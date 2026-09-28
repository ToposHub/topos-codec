/* V7-R3 批 1：格式规则——cfg em=11 → (major 7, entropy 8) + GOP 字段
 * 激活（ADR-C048 D2/D3）。覆盖：
 *   1. cfg 域：em=12 拒绝；em=11 的 frame_type/ref_distance 匹配校验；
 *      reserved[6..] 强制零；
 *   2. 真编码：em=11 I/P 包头字节（6=major 7、45=entropy 8、15/16-17/18
 *      = GOP 字段）；I 解码逐位往返（V7 band 机器跟随 entropy 8）；
 *   3. 读端语义：P 包查询 OK、像素解码 TC_ERR_STATE（tc_decode_frames 闸）；
 *   4. 头规则组：(7,8) frame_type=2 → UNSUPPORTED；I ref!=0 / P ref!=1 →
 *      MALFORMED；(7,7) frame_type!=0 维持 V2.1 强制零 UNSUPPORTED；
 *   5. sized 拒绝 em=11（GOP context 是唯一写口）。 */
#include "bitstream/frame_header.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "mini_test.h"
#include "topos_codec.h"

#define W 64u
#define H 64u
#define CW ((W + 7u) / 8u * 8u)
#define CH ((H + 7u) / 8u * 8u)
#define CWC ((W / 2u + 7u) / 8u * 8u)

static topos_frame_config r3_cfg(uint8_t frame_type, uint16_t gop_id)
{
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = (uint16_t)W;
    cfg.visible_height = (uint16_t)H;
    cfg.profile = 3u;
    cfg.pixel_format = 0u;
    cfg.bit_depth = 10u;
    cfg.qmatrix_id = 0u;
    cfg.qp_base = 40u;
    cfg.slice_rows = 16u;
    cfg.color_range = 1u;
    cfg.color_primaries = 1u;
    cfg.color_transfer = 1u;
    cfg.color_matrix = 1u;
    cfg.reserved[0] = 11u;
    cfg.reserved[3] = frame_type;
    cfg.reserved[4] = gop_id;
    cfg.reserved[5] = frame_type == 0u ? 0u : 1u;
    return cfg;
}

static void fill_planes(uint16_t* y, uint16_t* c)
{
    for (size_t i = 0; i < CW * CH; ++i) { y[i] = (uint16_t)(400u + (i * 7u) % 500u); }
    for (size_t i = 0; i < CWC * CH; ++i) { c[i] = (uint16_t)(500u + (i * 3u) % 40u); }
}

static void make_input(const uint16_t* y, const uint16_t* c, topos_frame_input* in)
{
    memset(in, 0, sizeof(*in));
    in->struct_size = (uint32_t)sizeof(*in);
    in->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    in->planes[0] = y;
    in->planes[1] = c;
    in->planes[2] = c;
    in->strides[0] = CW;
    in->strides[1] = CWC;
    in->strides[2] = CWC;
}

int main(void)
{
    static uint16_t py[CW * CH], pc[CWC * CH];
    fill_planes(py, pc);
    topos_frame_input in;
    make_input(py, pc, &in);
    static uint8_t pkt_i[1 << 16], pkt_p[1 << 16];
    topos_frame_stats st;
    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;

    /* ---- 1. cfg 域 ---- */
    {
        topos_frame_config bad = r3_cfg(0u, 1u);
        bad.reserved[0] = 12u;
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = r3_cfg(0u, 1u);
        bad.reserved[5] = 1u; /* I 帧配 ref=1 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = r3_cfg(0u, 1u);
        bad.reserved[6] = 1u; /* em=11 时 reserved[6] 必须零 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
        bad = r3_cfg(2u, 1u); /* frame_type=2 */
        MT_CHECK_EQ_I64(tc_frame_config_validate(&bad), TC_ERR_INVALID_ARGUMENT);
    }

    /* ---- 2. 真编码：包头字节 + GOP 字段 ---- */
    {
        topos_frame_config ci = r3_cfg(0u, 1u);
        topos_frame_config cp = r3_cfg(1u, 1u);
        MT_CHECK_EQ_I64(tc_frame_encode(&ci, &in, pkt_i, sizeof(pkt_i), &st), TC_OK);
        MT_CHECK_EQ_U64(pkt_i[6], 7ull);  /* major = 7 */
        MT_CHECK_EQ_U64(pkt_i[45], 8ull); /* entropy = 8（V7-R3） */
        MT_CHECK_EQ_U64(pkt_i[15], 0ull); /* frame_type = I */
        MT_CHECK_EQ_U64((pkt_i[16] << 8) | pkt_i[17], 1ull);
        MT_CHECK_EQ_U64(pkt_i[18], 0ull); /* I ref_distance = 0 */
        MT_CHECK_EQ_I64(tc_frame_encode(&cp, &in, pkt_p, sizeof(pkt_p), &st), TC_OK);
        MT_CHECK_EQ_U64(pkt_p[15], 1ull);
        MT_CHECK_EQ_U64(pkt_p[18], 1ull); /* P ref_distance = 1 */
        /* u16 gop_id 全域 */
        topos_frame_config cw = r3_cfg(0u, 65535u);
        MT_CHECK_EQ_I64(tc_frame_encode(&cw, &in, pkt_i, sizeof(pkt_i), &st), TC_OK);
        MT_CHECK_EQ_U64((pkt_i[16] << 8) | pkt_i[17], 65535ull);
    }

    /* ---- 3. 读端：I 逐位往返；P 查询 OK / 像素 STATE ---- */
    {
        topos_frame_config ci = r3_cfg(0u, 1u);
        MT_CHECK_EQ_I64(tc_frame_encode(&ci, &in, pkt_i, sizeof(pkt_i), &st), TC_OK);
        topos_frame_output info;
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t)sizeof(info);
        info.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        MT_CHECK_EQ_I64(tc_frame_decode(pkt_i, st.packet_size, NULL, NULL, &info), TC_OK);
        static uint16_t dy[CW * CH], dc[CWC * CH];
        memset(dy, 0, sizeof(dy));
        uint16_t* planes[TC_FRAME_MAX_PLANES] = { dy, dc, dc, NULL };
        size_t strides[TC_FRAME_MAX_PLANES] = { CW, CWC, CWC };
        MT_CHECK_EQ_I64(tc_frame_decode(pkt_i, st.packet_size, planes, strides, &info), TC_OK);
        MT_CHECK(memcmp(dy, py, sizeof(py)) == 0);
        MT_CHECK(memcmp(dc, pc, sizeof(pc)) == 0);

        topos_frame_config cp = r3_cfg(1u, 1u);
        MT_CHECK_EQ_I64(tc_frame_encode(&cp, &in, pkt_p, sizeof(pkt_p), &st), TC_OK);
        MT_CHECK_EQ_I64(tc_frame_decode(pkt_p, st.packet_size, NULL, NULL, &info), TC_OK);
        uint16_t* planes2[TC_FRAME_MAX_PLANES] = { dy, dc, dc, NULL };
        MT_CHECK_EQ_I64(tc_frame_decode(pkt_p, st.packet_size, planes2, strides, &info),
                        TC_ERR_STATE);
    }

    /* ---- 4. 头规则组（直连 validate）---- */
    {
        topos_frame_header fh;
        memset(&fh, 0, sizeof(fh));
        fh.version_major = 7u;
        fh.entropy_mode = 8u;
        fh.profile = 3u;
        fh.pixel_format = 0u;
        fh.bit_depth = 10u;
        fh.plane_count = 3u;
        fh.coded_width = CW;
        fh.coded_height = CH;
        fh.visible_width = W;
        fh.visible_height = H;
        fh.qmatrix_id = 0u;
        fh.qp_base = 40u;
        fh.slice_count = 12u;
        fh.color_range = 1u;
        fh.color_primaries = 1u;
        fh.color_transfer = 1u;
        fh.color_matrix = 1u;
        fh.frame_packet_size = 4096u;

        topos_frame_header bad = fh;
        bad.frame_type = 2u;
        MT_CHECK_EQ_I64(tc_frame_header_validate(&bad), TC_ERR_UNSUPPORTED_VERSION);
        bad = fh;
        bad.frame_type = 0u;
        bad.ref_distance = 1u; /* I ref != 0 */
        MT_CHECK_EQ_I64(tc_frame_header_validate(&bad), TC_ERR_MALFORMED);
        bad = fh;
        bad.frame_type = 1u;
        bad.ref_distance = 0u; /* P ref != 1 */
        MT_CHECK_EQ_I64(tc_frame_header_validate(&bad), TC_ERR_MALFORMED);
        bad = fh;
        bad.entropy_mode = 9u; /* (7,9) 非法（9 只在 major 9） */
        MT_CHECK_EQ_I64(tc_frame_header_validate(&bad), TC_ERR_UNSUPPORTED_VERSION);
        /* (7,7) 维持 V2.1 强制零 */
        bad = fh;
        bad.entropy_mode = 7u;
        bad.frame_type = 1u;
        MT_CHECK_EQ_I64(tc_frame_header_validate(&bad), TC_ERR_UNSUPPORTED_VERSION);
        /* (7,8) 合法 I / 合法 P */
        bad = fh;
        MT_CHECK_EQ_I64(tc_frame_header_validate(&bad), TC_OK);
        bad.frame_type = 1u;
        bad.ref_distance = 1u;
        bad.gop_id = 7u;
        MT_CHECK_EQ_I64(tc_frame_header_validate(&bad), TC_OK);
    }

    /* ---- 5. sized 拒绝 em=11 ---- */
    {
        topos_frame_config ci = r3_cfg(0u, 1u);
        uint8_t out[1 << 16];
        MT_CHECK_EQ_I64(tc_frame_encode_sized(&ci, &in, 4096u, 20u, 60u,
                                              NULL, out, sizeof(out), &st),
                        TC_ERR_NOT_IMPLEMENTED);
    }

    return MT_MAIN_RETURN();
}
