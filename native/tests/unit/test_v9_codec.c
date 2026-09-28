/* V9 批 2：zero-motion IP-2 端到端——GOP context 编解码 roundtrip。
 *
 * 核心不变量：
 *   1. 零漂锚（非零残差版）：f1 = f0+Δ（Δ 过量化门，解码输出 ≠ f0 解），
 *      F2 = dctx 解码 F1 的输出 = 编码器参考 → F2 残差恒 0 → d2 == d1
 *      逐位相等（编码器/解码器重建同源的构造性证据；解码链用独立 dctx
 *      建模真实解码器——单实例交错 encode+feed 会把 P 残差应用两次，
 *      V9 复审 2026-09-14 修正）；
 *   2. 序列状态：gop_id 每 I 递增、P 携带当前 GOP、sample 递增；
 *   3. I 回退：force_intra 与「P ≥ 最近 I」两路都产生 I 包（帧头
 *      frame_type=0）且 gop 推进；
 *   4. 事务：BUFFER_TOO_SMALL 全量不提交（状态与参考像素都不变）——
 *      扩缓冲重试的包与「无 BTS 发生」的干净历史逐字节一致（回归：
 *      旧实现在 out_cap 检查前推进参考，重试针对未发出包的重建算残差）；
 *   5. conceal 闸（§3.8）：坏瓦片 CRC 的 P 包（V8 像素机 conceal 继续
 *      仍返回 TC_OK）→ feed MALFORMED + REF_INVALID；同 GOP 后续 P →
 *      REFERENCE_INVALID；下一 I 恢复；
 *   6. P 包无状态解码 → TC_ERR_STATE；异几何 feed → MALFORMED；
 *   7. no-alpha capability：alpha cfg create → NOT_IMPLEMENTED。 */
#include "bitstream/packet.h"

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
#define NPLANES 3u

static uint64_t rng_state = 0x243F6A8885A308D3ull;

static uint64_t rng(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1Dull;
}

static topos_frame_config base_cfg(void)
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
    cfg.reserved[0] = 10u;
    return cfg;
}

/* 平面缓冲组（luma CW×CH + chroma CWC×CH ×2） */
typedef struct {
    uint16_t y[CW * CH];
    uint16_t u[CWC * CH];
    uint16_t v[CWC * CH];
} frame_buf;

static void fill_noise(frame_buf* f, uint64_t seed)
{
    rng_state = seed;
    for (size_t i = 0; i < CW * CH; ++i) { f->y[i] = (uint16_t)(rng() % 1024u); }
    for (size_t i = 0; i < CWC * CH; ++i) { f->u[i] = (uint16_t)(rng() % 1024u); }
    for (size_t i = 0; i < CWC * CH; ++i) { f->v[i] = (uint16_t)(rng() % 1024u); }
}

/* 非零残差 delta：±amp 每 step 个 luma 样本（qp40 下远过量 化门——
 * 解码输出与 f0 解码输出可证不同，见 dec_a != dec_b 断言） */
static void fill_delta(const frame_buf* base, frame_buf* f, uint32_t amp,
                       uint32_t step, uint64_t seed)
{
    rng_state = seed;
    memcpy(f, base, sizeof(*f));
    for (size_t i = 0; i < CW * CH; i += step) {
        int32_t v = (int32_t)f->y[i] + (int32_t)(rng() % (2u * amp + 1u)) - (int32_t)amp;
        if (v < 0) { v = 0; }
        if (v > 1023) { v = 1023; }
        f->y[i] = (uint16_t)v;
    }
}

static void make_input(const frame_buf* f, topos_frame_input* in)
{
    memset(in, 0, sizeof(*in));
    in->struct_size = (uint32_t)sizeof(*in);
    in->abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    in->planes[0] = f->y;
    in->planes[1] = f->u;
    in->planes[2] = f->v;
    in->strides[0] = CW;
    in->strides[1] = CWC;
    in->strides[2] = CWC;
}

static int frames_equal(const frame_buf* a, const frame_buf* b) { return memcmp(a, b, sizeof(*a)) == 0; }

/* 绑定 out 视图到帧缓冲 */
static void bind_out(topos_plane_view out[TC_FRAME_MAX_PLANES], frame_buf* f)
{
    memset(out, 0, TC_FRAME_MAX_PLANES * sizeof(out[0]));
    for (int p = 0; p < TC_FRAME_MAX_PLANES; ++p) {
        out[p].struct_size = (uint32_t)sizeof(out[p]);
        out[p].abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    }
    out[0].pixels = f->y; out[0].stride = CW;
    out[1].pixels = f->u; out[1].stride = CWC;
    out[2].pixels = f->v; out[2].stride = CWC;
}

int main(void)
{
    topos_frame_config cfg = base_cfg();
    topos_gop_context* ctx = NULL;
    MT_CHECK_EQ_I64(tc_gop_context_create(&cfg, &ctx), TC_OK);
    MT_CHECK(ctx != NULL);

    frame_buf f0, f1, f_mid, f_cut;
    fill_noise(&f0, 0x1111111111111111ull);
    fill_delta(&f0, &f1, 60u, 3u, 0xDEADBEEFCAFEF00Dull);
    fill_delta(&f0, &f_mid, 40u, 4u, 0x0123456789ABCDEFull);
    fill_noise(&f_cut, 0x2222222222222222ull);

    topos_frame_input in0, in1, in_mid;
    make_input(&f0, &in0);
    make_input(&f1, &in1);
    make_input(&f_mid, &in_mid);

    static uint8_t pkt0[1 << 17], pkt1[1 << 17], pkt2[1 << 17], pkt3[1 << 17];
    static uint8_t pkt_bts[1 << 17], pkt_ref[1 << 17];
    size_t sz0 = 0, sz1 = 0, sz2 = 0, sz3 = 0, sz_bts = 0, sz_ref = 0;
    topos_frame_stats st;
    memset(&st, 0, sizeof(st));
    st.struct_size = (uint32_t)sizeof(st);
    st.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;

    /* ---- 序列：I(f0) P(f1)——f1 残差非零（过量化门） ---- */
    MT_CHECK_EQ_I64(tc_gop_context_encode_frame(ctx, &in0, pkt0, sizeof(pkt0), &sz0, 0, &st), TC_OK);
    MT_CHECK(sz0 > 0u);
    MT_CHECK_EQ_U64(st.packet_size, sz0);
    {
        /* 包头 frame_type=0（字节 15）、gop_id=1（字节 16-17，首个 I → 1） */
        MT_CHECK_EQ_U64(pkt0[15], 0ull);
        MT_CHECK_EQ_U64((pkt0[16] << 8) | pkt0[17], 1ull);
    }
    uint32_t state = 9u;
    uint16_t gop = 0u;
    MT_CHECK_EQ_I64(tc_gop_context_state(ctx, &state, &gop), TC_OK);
    MT_CHECK_EQ_U64(state, (uint32_t)TOPOS_GOP_REF_READY);
    MT_CHECK_EQ_U64(gop, 1ull);

    MT_CHECK_EQ_I64(tc_gop_context_encode_frame(ctx, &in1, pkt1, sizeof(pkt1), &sz1, 0, &st), TC_OK);
    MT_CHECK_EQ_U64(pkt1[15], 1ull);
    MT_CHECK_EQ_U64((pkt1[16] << 8) | pkt1[17], 1ull);
    MT_CHECK(sz1 < sz0);

    /* ---- 解码链：独立 dctx（真实解码器模型） ---- */
    topos_gop_context* dctx = NULL;
    MT_CHECK_EQ_I64(tc_gop_context_create(&cfg, &dctx), TC_OK);
    static frame_buf dec_a, dec_b, dec_c;
    topos_plane_view out[TC_FRAME_MAX_PLANES];
    topos_gop_frame_info gi;
    memset(&gi, 0, sizeof(gi));

    bind_out(out, &dec_a);
    MT_CHECK_EQ_I64(tc_gop_context_feed(dctx, pkt0, sz0, out, &gi), TC_OK);
    MT_CHECK_EQ_U64(gi.sample_index, 0ull);
    MT_CHECK_EQ_U64(gi.frame_type, 0ull);
    MT_CHECK_EQ_U64(gi.gop_id, 1ull);

    bind_out(out, &dec_b);
    MT_CHECK_EQ_I64(tc_gop_context_feed(dctx, pkt1, sz1, out, &gi), TC_OK);
    MT_CHECK_EQ_U64(gi.sample_index, 1ull);
    MT_CHECK_EQ_U64(gi.frame_type, 1ull);
    MT_CHECK_EQ_U64(gi.gop_id, 1ull);
    /* f1 残差确实过量化门：P 帧解码输出 ≠ I 帧解码输出 */
    MT_CHECK(!frames_equal(&dec_a, &dec_b));

    /* ---- 零漂锚（非零残差版）：F2 = dec_b = 编码器参考 → 残差恒 0 ---- */
    {
        frame_buf f2;
        memcpy(&f2, &dec_b, sizeof(f2));
        topos_frame_input in2;
        make_input(&f2, &in2);
        MT_CHECK_EQ_I64(tc_gop_context_encode_frame(ctx, &in2, pkt2, sizeof(pkt2), &sz2, 0, &st), TC_OK);
        MT_CHECK_EQ_U64(pkt2[15], 1ull);
        MT_CHECK(sz2 < sz1); /* 全零残差包 < 非零残差包 */
        bind_out(out, &dec_c);
        MT_CHECK_EQ_I64(tc_gop_context_feed(dctx, pkt2, sz2, out, &gi), TC_OK);
        MT_CHECK_EQ_U64(gi.sample_index, 2ull);
        MT_CHECK(frames_equal(&dec_b, &dec_c));
    }

    /* ---- 事务：BUFFER_TOO_SMALL 全量不提交（状态与参考都不动）；
     *      重试包 == 干净历史（无 BTS 发生）的同类帧逐字节一致 ---- */
    {
        size_t need = 0u;
        uint16_t gop_before = gop;
        uint32_t state_before = state;
        MT_CHECK_EQ_I64(tc_gop_context_encode_frame(ctx, &in_mid, pkt_bts, 64u, &need, 0, &st),
                        TC_ERR_BUFFER_TOO_SMALL);
        MT_CHECK(need > 64u);
        MT_CHECK_EQ_I64(tc_gop_context_state(ctx, &state, &gop), TC_OK);
        MT_CHECK_EQ_U64(gop, gop_before);
        MT_CHECK_EQ_U64(state, state_before);
        /* 扩缓冲重试（同输入） */
        MT_CHECK_EQ_I64(tc_gop_context_encode_frame(ctx, &in_mid, pkt_bts, sizeof(pkt_bts), &sz_bts, 0, &st), TC_OK);
        MT_CHECK_EQ_U64(pkt_bts[15], 1ull); /* 小 delta → P（未触发 I 回退） */
        /* 干净历史参照：I(f0) P(f1) P(f2dup) P(f_mid) */
        topos_gop_context* ctx2 = NULL;
        MT_CHECK_EQ_I64(tc_gop_context_create(&cfg, &ctx2), TC_OK);
        frame_buf f2c;
        memcpy(&f2c, &dec_b, sizeof(f2c));
        topos_frame_input in2c;
        make_input(&f2c, &in2c);
        size_t sz_tmp = 0u;
        MT_CHECK_EQ_I64(tc_gop_context_encode_frame(ctx2, &in0, pkt_ref, sizeof(pkt_ref), &sz_tmp, 0, &st), TC_OK);
        MT_CHECK_EQ_U64(sz_tmp, sz0);
        MT_CHECK_EQ_I64(tc_gop_context_encode_frame(ctx2, &in1, pkt_ref, sizeof(pkt_ref), &sz_tmp, 0, &st), TC_OK);
        MT_CHECK_EQ_U64(sz_tmp, sz1);
        MT_CHECK_EQ_I64(tc_gop_context_encode_frame(ctx2, &in2c, pkt_ref, sizeof(pkt_ref), &sz_tmp, 0, &st), TC_OK);
        MT_CHECK_EQ_U64(sz_tmp, sz2);
        MT_CHECK_EQ_I64(tc_gop_context_encode_frame(ctx2, &in_mid, pkt_ref, sizeof(pkt_ref), &sz_ref, 0, &st), TC_OK);
        tc_gop_context_close(ctx2);
        MT_CHECK_EQ_U64(sz_bts, sz_ref);
        MT_CHECK(memcmp(pkt_bts, pkt_ref, sz_bts) == 0);
    }

    /* ---- 解码确定性：同包重 feed（新 ctx）→ 逐位一致 ---- */
    {
        topos_gop_context* dctx2 = NULL;
        MT_CHECK_EQ_I64(tc_gop_context_create(&cfg, &dctx2), TC_OK);
        static frame_buf dec_r;
        topos_plane_view out2[TC_FRAME_MAX_PLANES];
        bind_out(out2, &dec_r);
        MT_CHECK_EQ_I64(tc_gop_context_feed(dctx2, pkt0, sz0, out2, &gi), TC_OK);
        MT_CHECK_EQ_I64(tc_gop_context_feed(dctx2, pkt1, sz1, out2, &gi), TC_OK);
        MT_CHECK(frames_equal(&dec_b, &dec_r));
        tc_gop_context_close(dctx2);
    }

    /* ---- I 回退：force_intra → gop 推进 ---- */
    MT_CHECK_EQ_I64(tc_gop_context_encode_frame(ctx, &in1, pkt3, sizeof(pkt3), &sz3, 1, &st), TC_OK);
    MT_CHECK_EQ_U64(pkt3[15], 0ull);
    MT_CHECK_EQ_U64((pkt3[16] << 8) | pkt3[17], 2ull); /* 第二个 I → gop 2 */

    /* ---- I 回退：切镜帧 P ≥ 最近 I → 自动改编 I（gop 3） ---- */
    {
        topos_frame_input inc;
        make_input(&f_cut, &inc);
        MT_CHECK_EQ_I64(tc_gop_context_encode_frame(ctx, &inc, pkt3, sizeof(pkt3), &sz3, 0, &st), TC_OK);
        MT_CHECK_EQ_U64(pkt3[15], 0ull);
        MT_CHECK_EQ_U64((pkt3[16] << 8) | pkt3[17], 3ull);
    }

    /* ---- P 白编预检门（ADR-C049）：高 MAD 帧直编 I ≡ 强制 I（逐字节
     * 一致——门控改变「是否白编 P」，不改变产出包） ---- */
    {
        topos_frame_input inc;
        make_input(&f_cut, &inc);
        topos_gop_context* ga = NULL;
        topos_gop_context* gb = NULL;
        MT_CHECK_EQ_I64(tc_gop_context_create(&cfg, &ga), TC_OK);
        MT_CHECK_EQ_I64(tc_gop_context_create(&cfg, &gb), TC_OK);
        static uint8_t pa[1 << 17], pb[1 << 17];
        size_t sa = 0u, sb = 0u;
        MT_CHECK_EQ_I64(tc_gop_context_encode_frame(ga, &in0, pa, sizeof(pa), &sa, 0, &st), TC_OK);
        MT_CHECK_EQ_I64(tc_gop_context_encode_frame(gb, &in0, pb, sizeof(pb), &sb, 0, &st), TC_OK);
        MT_CHECK_EQ_U64(sa, sb);
        MT_CHECK(memcmp(pa, pb, sa) == 0);
        /* f_cut 全帧噪声（MAD ≈ 256 ≥ 门 1<<(10-6)=16）→ 门控直编 I */
        MT_CHECK_EQ_I64(tc_gop_context_encode_frame(ga, &inc, pa, sizeof(pa), &sa, 0, &st), TC_OK);
        MT_CHECK_EQ_U64(pa[15], 0ull);
        MT_CHECK_EQ_I64(tc_gop_context_encode_frame(gb, &inc, pb, sizeof(pb), &sb, 1, &st), TC_OK);
        MT_CHECK_EQ_U64(sb, sa);
        MT_CHECK(memcmp(pa, pb, sa) == 0);
        tc_gop_context_close(ga);
        tc_gop_context_close(gb);
    }

    /* ---- conceal 闸（§3.8）：坏瓦片 CRC 的 P 包不得安装为参考 ---- */
    {
        topos_gop_context* cctx = NULL;
        MT_CHECK_EQ_I64(tc_gop_context_create(&cfg, &cctx), TC_OK);
        static frame_buf dec_c0;
        bind_out(out, &dec_c0);
        MT_CHECK_EQ_I64(tc_gop_context_feed(cctx, pkt0, sz0, out, &gi), TC_OK);
        /* 毁第一瓦片流中段一字节（结构完好；瓦片 CRC 失配 → conceal） */
        static uint8_t bad[1 << 17];
        memcpy(bad, pkt1, sz1);
        topos_v8_packet_view vview;
        MT_CHECK_EQ_I64(tc_packet_scan_v8_ex(bad, sz1, &vview, 0), TC_OK);
        MT_CHECK(vview.tile_count > 0u);
        const size_t soff = vview.tiles[0].stream_off;
        const size_t slen = vview.tiles[0].stream_bytes;
        bad[soff + (slen > 1u ? slen / 2u : 0u)] ^= 0xFFu;
        MT_CHECK_EQ_I64(tc_gop_context_feed(cctx, bad, sz1, NULL, &gi),
                        TC_ERR_MALFORMED);
        MT_CHECK_EQ_I64(tc_gop_context_state(cctx, &state, NULL), TC_OK);
        MT_CHECK_EQ_U64(state, (uint32_t)TOPOS_GOP_REF_INVALID);
        /* 同 GOP 后续 P → REFERENCE_INVALID（禁沿用污染参考） */
        MT_CHECK_EQ_I64(tc_gop_context_feed(cctx, pkt1, sz1, NULL, &gi),
                        TC_ERR_REFERENCE_INVALID);
        /* 下一 I 恢复 */
        bind_out(out, &dec_c0);
        MT_CHECK_EQ_I64(tc_gop_context_feed(cctx, pkt0, sz0, out, &gi), TC_OK);
        tc_gop_context_close(cctx);
    }

    /* ---- P 包无状态解码 → TC_ERR_STATE ---- */
    {
        topos_frame_output info;
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t)sizeof(info);
        info.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
        static uint16_t py[CW * CH];
        static uint16_t pc[CWC * CH];
        uint16_t* planes[TC_FRAME_MAX_PLANES] = { py, pc, pc, NULL };
        MT_CHECK_EQ_I64(tc_frame_decode(pkt1, sz1, planes, NULL, &info), TC_ERR_STATE);
        MT_CHECK_EQ_I64(tc_frame_decode(pkt1, sz1, NULL, NULL, &info), TC_OK);
    }

    /* ---- 异几何 feed → MALFORMED（scratch 防越界闸） ---- */
    {
        topos_frame_config cfg2 = base_cfg();
        cfg2.visible_width = 60u;
        cfg2.visible_height = 48u;
        topos_gop_context* ctx3 = NULL;
        MT_CHECK_EQ_I64(tc_gop_context_create(&cfg2, &ctx3), TC_OK);
        MT_CHECK_EQ_I64(tc_gop_context_feed(ctx3, pkt0, sz0, NULL, &gi), TC_ERR_MALFORMED);
        tc_gop_context_close(ctx3);
    }

    /* ---- abort(reset_to_i) → NO_REF；下一帧自动 I（gop 4） ---- */
    tc_gop_context_abort(ctx, 1);
    MT_CHECK_EQ_I64(tc_gop_context_encode_frame(ctx, &in0, pkt3, sizeof(pkt3), &sz3, 0, &st), TC_OK);
    MT_CHECK_EQ_U64(pkt3[15], 0ull);
    MT_CHECK_EQ_U64((pkt3[16] << 8) | pkt3[17], 4ull);

    /* ---- no-alpha / CFA capability ---- */
    {
        topos_frame_config bad = base_cfg();
        bad.alpha_mode = 1u;
        bad.alpha_bit_depth = 16u;
        topos_gop_context* bctx = (void*)1;
        MT_CHECK_EQ_I64(tc_gop_context_create(&bad, &bctx), TC_ERR_NOT_IMPLEMENTED);
        MT_CHECK(bctx == NULL);
        bad = base_cfg();
        bad.pixel_format = 3u;
        bctx = NULL;
        /* pf3+profile3 在 cfg validate 的交叉规则先拒（TRAW 载体）；
         * V9 不承载 CFA 的语义闸在 create（gop_context.c） */
        MT_CHECK_EQ_I64(tc_gop_context_create(&bad, &bctx), TC_ERR_MALFORMED);
        MT_CHECK(bctx == NULL);
    }

    tc_gop_context_close(dctx);
    tc_gop_context_close(ctx);
    return MT_MAIN_RETURN();
}
