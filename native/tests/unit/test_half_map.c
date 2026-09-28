/* test_half_map —— HALF 样本域（spec §15）映射 + IDSC 规则 + qp0 无损端到端。
 *
 * 覆盖：
 *  1. golden 向量（±0/±1/±0.5/最值/次正规/±Inf/NaN）——正向与逆向；
 *  2. 全 65536 域双射：inverse∘forward = id（含全部 NaN 位模式）、
 *     forward∘inverse = id（值域 [1,0xFFFF]），c=0 饱和定义；
 *  3. 有限值严格单调（码值序 == 浮点值序）；
 *  4. 批量 API 与标量逐位一致 / 原地映射 / NULL 与 n==0 契约；
 *  5. IDSC 校验：HALF 合法组合通过，域违规逐项拒绝（spec §15 交叉规则）；
 *  6. 真实 codec 端到端：half 码值 → pf2/bd16/profile5 qp0 编码 → TPIM
 *     信封（image_profile 4）→ probe/decode → 逆映射，逐位无损
 *     （qm0 下 qp_eff=0+12≤22 → Q=1，批 4 无损域核对的兑现验证）。
 */
#include <stdlib.h>
#include <string.h>

#include "mini_test.h"
#include "topos_codec.h"
#include "topos_image.h"
#include "image/image_container.h"
#include "image/half_map.h"
#include "image_file_synth.h"

/* —— half 位模式常量（IEEE 754 binary16） —— */
#define H_PZERO    0x0000u /* +0.0 */
#define H_NZERO    0x8000u /* -0.0（映射后与 +0 同码 0x8000，归一零） */
#define H_MIN_SUB  0x0001u /* +2^-24 最小次正规 */
#define H_PHALF    0x3800u /* +0.5 */
#define H_PONE     0x3C00u /* +1.0 */
#define H_NONE     0xBC00u /* -1.0 */
#define H_MAX_FIN  0x7BFFu /* +65504 最大有限 */
#define H_NMAX_FIN 0xFBFFu /* -65504 */
#define H_PINF     0x7C00u /* +Inf */
#define H_NINF     0xFC00u /* -Inf */
#define H_QNAN     0x7E00u /* qNaN（载荷 0x200） */

static void test_golden(void)
{
    static const struct { uint16_t h, c; } kGold[] = {
        { H_PZERO,    0x8000u }, /* 浮点零 → bd16 中值码（level shift 后为 0） */
        { H_MIN_SUB,  0x8001u },
        { H_PHALF,    0xB800u },
        { H_PONE,     0xBC00u },
        { H_NONE,     0x4400u },
        { H_MAX_FIN,  0xFBFFu },
        { H_NMAX_FIN, 0x0401u },
        { H_PINF,     0xFC00u },
        { H_NINF,     0x0400u },
        { H_QNAN,     0xFE00u },
    };
    for (size_t i = 0; i < sizeof(kGold) / sizeof(kGold[0]); ++i) {
        MT_CHECK_EQ_U64(tci_half_to_code(kGold[i].h), kGold[i].c);
        MT_CHECK_EQ_U64(tci_code_to_half(kGold[i].c), kGold[i].h);
    }
    /* ±0 归一：两个零映射同一码 0x8000；逆向恒返回 +0（规范冻结语义） */
    MT_CHECK_EQ_U64(tci_half_to_code(H_NZERO), 0x8000u);
    MT_CHECK_EQ_U64(tci_code_to_half(0x8000u), H_PZERO);
    /* 有限全域序：-65504 < -1.0 < 0 < 1.0 < 65504 */
    MT_CHECK(tci_half_to_code(H_NMAX_FIN) < tci_half_to_code(H_NONE));
    MT_CHECK(tci_half_to_code(H_NONE) < tci_half_to_code(H_PZERO));
    MT_CHECK(tci_half_to_code(H_PZERO) < tci_half_to_code(H_PONE));
    MT_CHECK(tci_half_to_code(H_PONE) < tci_half_to_code(H_MAX_FIN));
}

static void test_bijection_full_domain(void)
{
    /* inverse∘forward = id：全部 65536 个 half 位模式（含 NaN/Inf/次正规）；
     * 唯一例外 -0（0x8000）——零归一后返回 +0（golden 已钉死） */
    for (uint32_t h = 0; h <= 0xFFFFu; ++h) {
        const uint16_t c = tci_half_to_code((uint16_t)h);
        const uint16_t back = tci_code_to_half(c);
        if (h == H_NZERO) {
            if (back != H_PZERO) {
                mt_report(__FILE__, __LINE__, "-0 not canonicalized to +0");
                return;
            }
        } else if (back != (uint16_t)h) {
            mt_report(__FILE__, __LINE__, "inverse(forward(h)) != h");
            return;
        }
    }
    /* forward∘inverse = id：前向值域 [1,0xFFFF]（0 不可达，见下） */
    uint32_t seen_zero = 0u;
    for (uint32_t c = 1; c <= 0xFFFFu; ++c) {
        if (tci_half_to_code(tci_code_to_half((uint16_t)c)) != (uint16_t)c) {
            mt_report(__FILE__, __LINE__, "forward(inverse(c)) != c");
            return;
        }
    }
    for (uint32_t h = 0; h <= 0xFFFFu; ++h) {
        if (tci_half_to_code((uint16_t)h) == 0u) { seen_zero = 1u; }
    }
    MT_CHECK(seen_zero == 0u);               /* c=0 前向不可达 */
    MT_CHECK_EQ_U64(tci_code_to_half(0x0000u), 0xFFFFu); /* 损坏码饱和 → -NaN */
}

static void test_monotonic_finite(void)
{
    /* 正有限（含次正规）：h 升 → code 严格升 */
    uint16_t prev = tci_half_to_code(0x0001u);
    for (uint32_t h = 2; h <= 0x7BFFu; ++h) {
        const uint16_t c = tci_half_to_code((uint16_t)h);
        if (c <= prev) { mt_report(__FILE__, __LINE__, "positive finite not monotone"); return; }
        prev = c;
    }
    /* 负有限：h 升（幅值升 = 值降）→ code 严格降 */
    prev = tci_half_to_code(0x8001u);
    for (uint32_t h = 0x8002; h <= 0xFBFFu; ++h) {
        const uint16_t c = tci_half_to_code((uint16_t)h);
        if (c >= prev) { mt_report(__FILE__, __LINE__, "negative finite not monotone"); return; }
        prev = c;
    }
}

static void test_bulk_api(void)
{
    static uint16_t src[4096], out_separate[4096], in_place[4096];
    for (size_t i = 0; i < 4096; ++i) {
        src[i] = (uint16_t)(mt_rand_u64() & 0xFFFFu);
    }
    /* 公共 ABI 批量：与标量逐位一致 */
    MT_CHECK_EQ_I64(tc_image_half_to_codes(src, 4096, out_separate), TC_OK);
    MT_CHECK_EQ_I64(tc_image_codes_to_half(out_separate, 4096, in_place), TC_OK);
    for (size_t i = 0; i < 4096; ++i) {
        if (out_separate[i] != tci_half_to_code(src[i]) || in_place[i] != src[i]) {
            mt_report(__FILE__, __LINE__, "bulk != scalar");
            return;
        }
    }
    /* 原地映射（src == dst）等价 */
    memcpy(in_place, src, sizeof(src));
    MT_CHECK_EQ_I64(tc_image_half_to_codes(in_place, 4096, in_place), TC_OK);
    MT_CHECK(memcmp(in_place, out_separate, sizeof(src)) == 0);
    MT_CHECK_EQ_I64(tc_image_codes_to_half(in_place, 4096, in_place), TC_OK);
    MT_CHECK(memcmp(in_place, src, sizeof(src)) == 0);
    /* 契约：NULL 拒绝；n==0 合法空操作（指针仍须非 NULL） */
    MT_CHECK_EQ_I64(tc_image_half_to_codes(NULL, 4, out_separate), TC_ERR_INVALID_ARGUMENT);
    MT_CHECK_EQ_I64(tc_image_codes_to_half(out_separate, 4, NULL), TC_ERR_INVALID_ARGUMENT);
    MT_CHECK_EQ_I64(tc_image_half_to_codes(src, 0, out_separate), TC_OK);
}

/* —— IDSC 规则（spec §15 交叉规则；基础字段合法的 HALF 描述） —— */
static void half_idsc_base(topos_image_idsc* s)
{
    memset(s, 0, sizeof(*s));
    s->struct_size = (uint32_t)sizeof(*s);
    s->abi_version = TOPOS_IMAGE_ABI_VERSION;
    s->idsc_version_major = 1u;
    s->idsc_version_minor = 0u;
    s->display_x_max = 95;
    s->display_y_max = 63;
    s->data_x_max = 95;
    s->data_y_max = 63;
    s->orientation = 1u;
    s->pixel_aspect_num = 1u;
    s->pixel_aspect_den = 1u;
    s->channel_model = TC_IMG_CHANNEL_MODEL_RGB;
    s->channel_count = 3u;
    s->sample_kind = TC_IMG_SAMPLE_KIND_HALF;
    s->valid_bit_depth = 16u;
    s->container_bit_depth = 16u;
    s->storage_layout = TC_IMG_LAYOUT_PLANAR;
    s->subsampling = 0u;
    s->alpha_presence = TC_IMG_ALPHA_ABSENT;
    s->codec_id = TC_IMG_CODEC_ID_TPIC;
    s->payload_major = 8u;
    s->payload_minor = 0u;
    s->image_profile = TC_IMG_PROFILE_HALF_FLOAT;
    s->codec_profile = 5u;
    s->pixel_format = 2u;
    s->color_primaries = 9u;
    s->color_transfer = 8u; /* linear：HALF 样本域约定 */
    s->color_matrix = 0u;
    s->color_range = 1u;
    s->payload_header_crc32 = 0x12345678u;
}

static void mut_valid12(topos_image_idsc* p) { p->valid_bit_depth = 12u; }
static void mut_container12(topos_image_idsc* p) { p->container_bit_depth = 12u; }
static void mut_profile_xq(topos_image_idsc* p) { p->image_profile = TC_IMG_PROFILE_XQ; }
static void mut_uint(topos_image_idsc* p) { p->sample_kind = TC_IMG_SAMPLE_KIND_UINT; }
static void mut_yuv(topos_image_idsc* p) { p->channel_model = TC_IMG_CHANNEL_MODEL_YUV; }
static void mut_422(topos_image_idsc* p) { p->subsampling = 1u; }
static void mut_kind2(topos_image_idsc* p) { p->sample_kind = 2u; }
static void mut_profile5(topos_image_idsc* p) { p->image_profile = 5u; }

static void test_idsc_rules(void)
{
    topos_image_idsc s;
    half_idsc_base(&s);
    MT_CHECK_EQ_I64(tci_idsc_validate(&s), TC_OK);

    struct { const char* name; void (*mutate)(topos_image_idsc*); int32_t want; } kCases[] = {
        { "valid_bit_depth=12", mut_valid12, TC_IMG_ERR_CHUNK_CONFLICT },
        { "container=12", mut_container12, TC_IMG_ERR_BAD_DIRECTORY },
        { "profile=XQ", mut_profile_xq, TC_IMG_ERR_CHUNK_CONFLICT },
        { "UINT with profile4", mut_uint, TC_IMG_ERR_CHUNK_CONFLICT },
        { "YUV model", mut_yuv, TC_IMG_ERR_CHUNK_CONFLICT },
        { "subsampling 422", mut_422, TC_IMG_ERR_CHUNK_CONFLICT },
        { "sample_kind=2 reserved", mut_kind2, TC_ERR_UNSUPPORTED_VERSION },
        { "profile=5 reserved", mut_profile5, TC_ERR_UNSUPPORTED_PROFILE },
    };
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
        half_idsc_base(&s);
        kCases[i].mutate(&s);
        const int32_t rc = tci_idsc_validate(&s);
        if (rc != kCases[i].want) {
            fprintf(stderr, "FAIL %s:%d: case %s want %d got %d\n",
                    __FILE__, __LINE__, kCases[i].name, kCases[i].want, rc);
            mt_failures++;
        }
    }

    /* alpha：mode1（16-bit 无损）合法；mode2 近似对 HALF 无定义 */
    half_idsc_base(&s);
    s.channel_count = 4u;
    s.alpha_presence = TC_IMG_ALPHA_STRAIGHT;
    s.alpha_mode = 1u;
    s.alpha_bit_depth = 16u;
    MT_CHECK_EQ_I64(tci_idsc_validate(&s), TC_OK);
    s.alpha_mode = 2u;
    s.alpha_bit_depth = 12u;
    MT_CHECK_EQ_I64(tci_idsc_validate(&s), TC_IMG_ERR_CHUNK_CONFLICT);
}

/* —— 端到端：真实 encoder + TPIM 信封 + probe/decode + 逆映射 —— */

/* 端到端平面生成用独立固定种子的 xorshift（不共用 mt_rand 流——测试
 * 执行顺序改变噪声实现会翻转 token 域判定，内容必须与顺序无关）。
 * 注意：half 位模式按「幅值 |m| ≤ 0x7FFF + 符号位」构造（0x8000|m 为负、
 * m 为正）；禁止 0x8000±偏移写法——无符号回绕会落进 0x7Cxx/0x7Fxx 的
 * ±Inf/NaN 位模式区。 */
static uint32_t half_rand(void)
{
    static uint64_t s = 0x48D1590F1C2E3617ull;
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return (uint32_t)((s * 0x2545F4914F6CDD1Dull) >> 32);
}

static uint16_t half_from_magnitude(int32_t m)
{
    if (m < 0) {
        return (uint16_t)(0x8000u | ((uint32_t)(-m) & 0x7FFFu));
    }
    return (uint16_t)((uint32_t)m & 0x7FFFu);
}

static void build_half_planes(uint32_t w, uint32_t h,
                              uint16_t* g, uint16_t* b, uint16_t* r, uint16_t* a)
{
    const size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; ++i) {
        /* Q=1（qp≤19）token 域边界（实测，2026-09-19）：block DC 码值偏移
         * ≤0x3000 可编、≥0x3100 拒绝（|Δdc|≤2^27−1 × DC 增益≈10922）。
         * 测试内容收敛在码值 ±0x2C00 中心域——qp0 逐位无损的前提。 */
        /* G：码值域单调扫 −0x2C00→+0x2C00（穿过零点，覆盖符号翻转接缝） */
        const int32_t mg = (int32_t)((i * 0x5800u) / (n != 0u ? n - 1u : 1u)) - 0x2C00;
        g[i] = half_from_magnitude(mg);
        /* B：中心化确定性噪声 + 逐点埋入极端类（±Inf/NaN/±0/最小次正规
         * ——坏像素形态，单点 spike 在 qp0 域内可编，见 dbg 实测） */
        uint16_t hb = half_from_magnitude(
            (int32_t)(half_rand() % 0x2C01u) * ((half_rand() & 1u) ? 1 : -1));
        if (i == 0) { hb = 0x0000u; }        /* +0 */
        else if (i == 1) { hb = 0x7C00u; }   /* +Inf */
        else if (i == 2) { hb = 0xFC00u; }   /* -Inf */
        else if (i == 3) { hb = 0x7E00u; }   /* qNaN */
        else if (i == 4) { hb = 0x8001u; }   /* -2^-24 */
        b[i] = hb;
        /* R：近零窄域（暗部/低幅合成数据形态，±48） */
        r[i] = half_from_magnitude((int32_t)((i % 97u) - 48));
        /* A：中心化确定性噪声（码值 ±0x2C00；code 域保真断言） */
        a[i] = half_from_magnitude(
            (int32_t)(half_rand() % 0x2C01u) * ((half_rand() & 1u) ? 1 : -1));
    }
}

static void test_end_to_end_lossless(int with_alpha)
{
    enum { W = 96, H = 64 };
    const size_t n = (size_t)W * H;
    uint16_t* half_planes[4] = {
        malloc(n * 2), malloc(n * 2), malloc(n * 2), malloc(n * 2)
    };
    uint16_t* code_planes[4] = {
        malloc(n * 2), malloc(n * 2), malloc(n * 2), malloc(n * 2)
    };
    MT_CHECK(half_planes[0] && half_planes[1] && half_planes[2] && half_planes[3]);
    MT_CHECK(code_planes[0] && code_planes[1] && code_planes[2] && code_planes[3]);
    build_half_planes(W, H, half_planes[0], half_planes[1], half_planes[2], half_planes[3]);
    for (int p = 0; p < 4; ++p) {
        tci_half_map_plane(half_planes[p], n, code_planes[p]);
    }

    /* pf2/bd16/profile5 + qm0 + qp0（bd16 偏移 +12 → qp_eff=12 ≤ 22：Q=1 无损） */
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = W;
    cfg.visible_height = H;
    cfg.profile = 5u;
    cfg.pixel_format = 2u;
    cfg.bit_depth = 16u;
    cfg.qmatrix_id = 0u;
    cfg.qp_base = 0u;
    cfg.alpha_mode = with_alpha ? 1u : 0u;
    cfg.alpha_bit_depth = with_alpha ? 16u : 0u;
    cfg.reserved[0] = 8u; /* rans2（bd≥13 必需） */

    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    in.planes[0] = code_planes[0];
    in.planes[1] = code_planes[1];
    in.planes[2] = code_planes[2];
    in.planes[3] = with_alpha ? code_planes[3] : NULL;

    const int32_t vrc = tc_frame_config_validate(&cfg);
    if (vrc != TC_OK) {
        fprintf(stderr, "FAIL %s:%d: config validate rc=%d (%s)\n", __FILE__, __LINE__,
                vrc, tc_last_error());
        mt_failures++;
        goto done;
    }
    {
        const size_t cap = tc_frame_packet_bound(&cfg);
        uint8_t* pkt = malloc(cap ? cap : 1u);
        topos_frame_stats st;
        MT_CHECK(pkt != NULL);
        const int32_t erc = tc_frame_encode(&cfg, &in, pkt, cap, &st);
        if (erc != TC_OK) {
            fprintf(stderr, "FAIL %s:%d: encode rc=%d (%s)\n", __FILE__, __LINE__,
                    erc, tc_last_error());
            mt_failures++;
            free(pkt);
            goto done;
        }

        /* 信封：derive(image_profile=4) → write；交叉校验在 write 内强制 */
        topos_image_idsc idsc;
        const int32_t drc = tc_image_derive_idsc(pkt, st.packet_size,
                                                 TC_IMG_PROFILE_HALF_FLOAT, &idsc);
        if (drc != TC_OK || idsc.sample_kind != TC_IMG_SAMPLE_KIND_HALF) {
            fprintf(stderr, "FAIL %s:%d: derive rc=%d sample_kind=%u\n", __FILE__, __LINE__,
                    drc, (unsigned)idsc.sample_kind);
            mt_failures++;
            free(pkt);
            goto done;
        }
        {
            uint8_t* file_buf = malloc(1u << 20);
            image_mem_sink sink;
            topos_io io;
            MT_CHECK(file_buf != NULL);
            image_mem_sink_init(&sink, file_buf, 1u << 20, &io);

            topos_image_write_params wp;
            memset(&wp, 0, sizeof(wp));
            wp.struct_size = (uint32_t)sizeof(wp);
            wp.abi_version = TOPOS_IMAGE_ABI_VERSION;
            wp.idsc = idsc;
            wp.pixl_data = pkt;
            wp.pixl_size = st.packet_size;
            uint64_t file_size = 0u;
            const int32_t wrc = tc_image_write(&wp, &io, &file_size);
            if (wrc != TC_OK) {
                fprintf(stderr, "FAIL %s:%d: write rc=%d (%s)\n", __FILE__, __LINE__,
                        wrc, tc_last_error());
                mt_failures++;
                free(pkt);
                free(file_buf);
                goto done;
            }

            /* probe：sample_kind/profile 镜像 */
            topos_image_info info;
            image_mem_src src;
            topos_io rio;
            image_mem_src_init(&src, file_buf, file_size, &rio);
            const int32_t prc = tc_image_probe(&rio, &info);
            if (prc != TC_OK || info.idsc.sample_kind != TC_IMG_SAMPLE_KIND_HALF ||
                info.idsc.image_profile != TC_IMG_PROFILE_HALF_FLOAT ||
                info.idsc.valid_bit_depth != 16u || info.plane_count !=
                    (uint8_t)(with_alpha ? 4u : 3u)) {
                fprintf(stderr, "FAIL %s:%d: probe rc=%d kind=%u prof=%u planes=%u\n",
                        __FILE__, __LINE__, prc, (unsigned)info.idsc.sample_kind,
                        (unsigned)info.idsc.image_profile, (unsigned)info.plane_count);
                mt_failures++;
            }
            /* decode → 逆映射 → 与源 half 逐位一致 */
            {
                uint16_t* out[4] = { malloc(n * 2), malloc(n * 2), malloc(n * 2), malloc(n * 2) };
                topos_plane_view views[TC_FRAME_MAX_PLANES];
                topos_frame_output fout;
                for (int p = 0; p < 4; ++p) {
                    memset(&views[p], 0, sizeof(views[p]));
                    views[p].struct_size = (uint32_t)sizeof(views[p]);
                    views[p].abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
                    views[p].pixels = out[p];
                    views[p].stride = 0;
                }
                const int32_t drc2 = tc_image_decode(&rio, views, &fout);
                if (drc2 != TC_OK) {
                    fprintf(stderr, "FAIL %s:%d: decode rc=%d (%s)\n", __FILE__, __LINE__,
                            drc2, tc_last_error());
                    mt_failures++;
                } else {
                    const uint32_t planes = fout.plane_count;
                    for (uint32_t p = 0; p < planes; ++p) {
                        tci_code_unmap_plane(out[p], n, out[p]);
                    }
                    for (uint32_t p = 0; p < planes; ++p) {
                        if (memcmp(out[p], half_planes[p], n * 2u) != 0) {
                            fprintf(stderr, "FAIL %s:%d: plane %u not bit-exact "
                                    "(alpha=%d)\n", __FILE__, __LINE__, p, with_alpha);
                            mt_failures++;
                            break;
                        }
                    }
                }
                for (int p = 0; p < 4; ++p) { free(out[p]); }
            }
            /* 负例：同一 packet 派生 XQ（profile 2）必须被 write 交叉校验拒绝 */
            {
                topos_image_idsc bad;
                MT_CHECK_EQ_I64(tc_image_derive_idsc(pkt, st.packet_size,
                                                     TC_IMG_PROFILE_XQ, &bad), TC_OK);
                bad.sample_kind = TC_IMG_SAMPLE_KIND_HALF; /* 手拼：XQ + HALF */
                topos_image_write_params bp;
                memset(&bp, 0, sizeof(bp));
                bp.struct_size = (uint32_t)sizeof(bp);
                bp.abi_version = TOPOS_IMAGE_ABI_VERSION;
                bp.idsc = bad;
                bp.pixl_data = pkt;
                bp.pixl_size = st.packet_size;
                uint64_t dummy = 0u;
                MT_CHECK_EQ_I64(tc_image_write(&bp, &io, &dummy),
                                TC_IMG_ERR_CHUNK_CONFLICT);
            }
            free(file_buf);
        }
        free(pkt);
    }
done:
    for (int p = 0; p < 4; ++p) { free(half_planes[p]); free(code_planes[p]); }
}

/* qp0（Q=1）内容边界：整体高亮的平坦内容（block DC 偏移 0x4000 ≫ 界
 * 0x3100）的未量化 DC 超出熵 token 域，编码器显式拒绝（INVALID_ARGUMENT
 * + "raise qp"）——不静默损坏。规范语义：无损保证以系数域可编码为前提；
 * 大动态范围/整体偏亮偏暗内容须提高 qp（Q≥8 ⇔ qp≥20，见 quant 域）。 */
static void test_qp0_fullscale_noise_rejected(void)
{
    enum { W = 64, H = 64 };
    const size_t n = (size_t)W * H;
    uint16_t* pl[3] = { malloc(n * 2), malloc(n * 2), malloc(n * 2) };
    for (int p = 0; p < 3; ++p) {
        for (size_t i = 0; i < n; ++i) {
            pl[p][i] = 0xC000u; /* 全平面 +0x4000 DC 偏移（确定性越界） */
        }
    }
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = W;
    cfg.visible_height = H;
    cfg.profile = 5u;
    cfg.pixel_format = 2u;
    cfg.bit_depth = 16u;
    cfg.qmatrix_id = 0u;
    cfg.qp_base = 0u;
    cfg.reserved[0] = 8u;
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = (uint32_t)TOPOS_CODEC_ABI_VERSION;
    in.planes[0] = pl[0];
    in.planes[1] = pl[1];
    in.planes[2] = pl[2];
    const size_t cap = tc_frame_packet_bound(&cfg);
    uint8_t* pkt = malloc(cap ? cap : 1u);
    const int32_t rc = tc_frame_encode(&cfg, &in, pkt, cap, NULL);
    MT_CHECK_EQ_I64(rc, TC_ERR_INVALID_ARGUMENT);
    const char* err = tc_last_error();
    MT_CHECK(strstr(err, "token domain") != NULL);
    free(pkt);
    for (int p = 0; p < 3; ++p) { free(pl[p]); }
}

int main(void)
{
    test_golden();
    test_bijection_full_domain();
    test_monotonic_finite();
    test_bulk_api();
    test_idsc_rules();
    test_end_to_end_lossless(0);
    test_end_to_end_lossless(1);
    test_qp0_fullscale_noise_rejected();
    return MT_MAIN_RETURN();
}
