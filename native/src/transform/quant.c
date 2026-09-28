#include "transform/quant.h"

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../entropy/scan.h"
#include "../simd/dispatch.h"
#include "transform/fastdiv.h"
#include "transform/transform.h"

/* qp_scale —— spec 附录 A.4，公式 max(1, round_half_up(2^((qp−4)/4))) 冻结值。
 * v1.5（ADR-C031）：qp 域 0..63 → 0..95（minor≥4 才合法；qp63 恰为表尾
 * 27555，64..95 为公式精确值，整数四分幂交叉验证）。0..63 与 v1.0 表
 * 逐值一致（含 qp63=27555 的历史冻结值，公式真值 27554，golden 钉死不改）。
 * qp95=7053950 ≈ qp63 的 256×：Proxy/LT 低码率档所需 ×16..36 粗化全覆盖。 */
static const uint32_t kQpScale[96] = {
    1,     1,     1,     1,     1,     1,     1,     2,
    2,     2,     3,     3,     4,     5,     6,     7,
    8,     10,    11,    13,    16,    19,    23,    27,
    32,    38,    45,    54,    64,    76,    91,    108,
    128,   152,   181,   215,   256,   304,   362,   431,
    512,   609,   724,   861,   1024,  1218,  1448,  1722,
    2048,  2435,  2896,  3444,  4096,  4871,  5793,  6889,
    8192,  9742,  11585, 13777, 16384, 19484, 23170, 27555,
    /* —— v1.5 扩展域（qp64..95，公式精确值）—— */
    32768, 38968, 46341, 55109, 65536, 77936, 92682, 110218,
    131072, 155872, 185364, 220436, 262144, 311744, 370728, 440872,
    524288, 623487, 741455, 881744, 1048576, 1246974, 1482910, 1763488,
    2097152, 2493948, 2965821, 3526975, 4194304, 4987896, 5931642, 7053950
};

/* 细化表（V7-R5，位流 flags bit3 选载）：qp≤63 与经典表逐值一致（冻结域）；
 * 64..95 放缓为每 6 qp 翻倍（32768·2^((qp−64)/6)）——攻击高 qp 区步长
 * 过粗→纹理 AC 整体归零（计划 P5 go 项）。A/B（锯齿诊断 §5.1）：仅
 * 4K standard 胜（+0.53 dB 全图 @ −0.1% 体积），产品仅 4K standard/hq
 * 档选用。qp95=1176987 仍为 qp63 的 ~43×，低码率粗化余量充足。 */
static const uint32_t kQpScaleRefined[96] = {
    1,     1,     1,     1,     1,     1,     1,     2,
    2,     2,     3,     3,     4,     5,     6,     7,
    8,     10,    11,    13,    16,    19,    23,    27,
    32,    38,    45,    54,    64,    76,    91,    108,
    128,   152,   181,   215,    256,   304,   362,   431,
    512,   609,   724,   861,   1024,  1218,  1448,  1722,
    2048,  2435,  2896,  3444,  4096,  4871,  5793,  6889,
    8192,  9742,  11585, 13777, 16384, 19484, 23170, 27555,
    32768, 36781, 41285, 46341, 52016, 58386, 65536, 73562,
    82570, 92682, 104032, 116772, 131072, 147123, 165140, 185364,
    208064, 233544, 262144, 294247, 330281, 370728, 416128, 467088,
    524288, 588493, 660561, 741455, 832255, 934175, 1048576, 1176987
};

static const tc_qmatrix_set kQmFlat = {
    { 16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16 },
    { 16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16 }
};

/* kQmStandard —— qmatrix_id=1「Topos Standard」（spec 附录 A.3，阶段 4 冻结）。
 * 生成式（表生成说明，产出为冻结整数；重生成见 ADR-C004 调优记录）：
 *   luma[u][v]   = round(16 · 1.25^(u+v))   → 16..364
 *   chroma[u][v] = round(16 · 1.18^(u+v))   → 16..162
 * 设计（ADR-C004）：几何递增的频率加权（JPEG 风格）——luma 高频比 chroma 更粗
 * （HVS 对亮度高频噪声最不敏感，控噪主导码率），chroma 相对精细（键控/肤色
 * 工作流保 chroma 保真）。Standard 的码率命中以帧级 qp 搜索（tc_frame_encode_sized）
 * 为主机制：矩阵固定频率响应，qp 承担内容自适应（与 ProRes 同构）。 */
static const tc_qmatrix_set kQmStandard = {
    { 16, 20, 25, 31, 39, 49, 61, 76,
      20, 25, 31, 39, 49, 61, 76, 95,
      25, 31, 39, 49, 61, 76, 95, 119,
      31, 39, 49, 61, 76, 95, 119, 149,
      39, 49, 61, 76, 95, 119, 149, 186,
      49, 61, 76, 95, 119, 149, 186, 233,
      61, 76, 95, 119, 149, 186, 233, 291,
      76, 95, 119, 149, 186, 233, 291, 364 },
    { 16, 19, 22, 26, 31, 37, 43, 51,
      19, 22, 26, 31, 37, 43, 51, 60,
      22, 26, 31, 37, 43, 51, 60, 71,
      26, 31, 37, 43, 51, 60, 71, 84,
      31, 37, 43, 51, 60, 71, 84, 99,
      37, 43, 51, 60, 71, 84, 99, 117,
      43, 51, 60, 71, 84, 99, 117, 138,
      51, 60, 71, 84, 99, 117, 138, 162 }
};

/* kQm444Compact —— qmatrix_id=2，GBR 4:4:4 12-bit 体积档专用矩阵。
 *
 * 4:4:4 GBR 每个平面都是全分辨率，沿用 flat/Standard 会让图片档明显
 * 大于同分辨率的中间片。该表在 Standard 的基础上提高中高频步长（luma
 * 约 1.8×、chroma 约 1.5×），保留 DC/低频的相对权重；它只改变编码端量化，不改变平面布局、
 * 解码路径或像素格式。qmatrix_id 入帧头，故旧解码器会明确拒绝未知矩阵，
 * 不会把 Compact 码流误解为 Standard。
 */
static const tc_qmatrix_set kQm444Compact = {
    { 16, 36, 45, 56, 70, 88, 110, 137,
      36, 45, 56, 70, 88, 110, 137, 171,
      45, 56, 70, 88, 110, 137, 171, 214,
      56, 70, 88, 110, 137, 171, 214, 268,
      70, 88, 110, 137, 171, 214, 268, 335,
      88, 110, 137, 171, 214, 268, 335, 419,
      110, 137, 171, 214, 268, 335, 419, 524,
      137, 171, 214, 268, 335, 419, 524, 655 },
    { 16, 28, 33, 39, 46, 56, 64, 76,
      28, 33, 39, 46, 56, 64, 76, 90,
      33, 39, 46, 56, 64, 76, 90, 106,
      39, 46, 56, 64, 76, 90, 106, 126,
      46, 56, 64, 76, 90, 106, 126, 148,
      56, 64, 76, 90, 106, 126, 148, 176,
      64, 76, 90, 106, 126, 148, 176, 207,
      76, 90, 106, 126, 148, 176, 207, 243 }
};

/* kQm422LowCompact —— qmatrix_id=3，4:2:2 10-bit Low 图片档专用矩阵。
 *
 * Low 已经到达 qp=63 的基础上限；这个轻度频率加权只收紧中高频，保留
 * DC/低频和 Standard 的整体形状，用来把 Low 与 Medium 的体积梯度拉开，
 * 不改变平面布局、预测或解码路径。它比 444 Compact 温和得多（luma
 * 约 1.08×、chroma 约 1.05×），因此只作为 422 Low 的体积微调。 */
static const tc_qmatrix_set kQm422LowCompact = {
    { 16, 22, 27, 33, 42, 53, 66, 82,
      22, 27, 33, 42, 53, 66, 82, 103,
      27, 33, 42, 53, 66, 82, 103, 129,
      33, 42, 53, 66, 82, 103, 129, 161,
      42, 53, 66, 82, 103, 129, 161, 201,
      53, 66, 82, 103, 129, 161, 201, 252,
      66, 82, 103, 129, 161, 201, 252, 314,
      82, 103, 129, 161, 201, 252, 314, 393 },
    { 16, 20, 23, 27, 33, 39, 45, 54,
      20, 23, 27, 33, 39, 45, 54, 63,
      23, 27, 33, 39, 45, 54, 63, 75,
      27, 33, 39, 45, 54, 63, 75, 88,
      33, 39, 45, 54, 63, 75, 88, 104,
      39, 45, 54, 63, 75, 88, 104, 123,
      45, 54, 63, 75, 88, 104, 123, 145,
      54, 63, 75, 88, 104, 123, 145, 170 }
};

/* kQmEdgeBalanced —— qmatrix_id=4，422 视频档「边缘均衡」矩阵。
 *
 * 诊断（docs/topos_edge_jag_diagnosis_2026-09-21.md）：flat（id=0）频率
 * 零形状保护——同尺寸下平均 PSNR 赢 ProRes 但边缘结构输（残差块网格化、
 * 栅格比 1.23 vs 1.08；均匀 qp 硬追平需 1.3–1.7× 体积）。本表把平坦区
 * 盈余换成边缘精度：**低频略粗、高频略细（HF:LF 步长比 ≈0.4）**，与
 * JPEG 系几何矩阵方向相反。
 *   luma[u][v]   = round(20 · 0.94^(u+v))  → 20..8
 *   chroma[u][v] = round(18 · 0.96^(u+v))  → 18..9（色度低频 18 比 flat
 *     粗 12.5% 出血、高频 9 比 flat 细 44%；色度 qp 偏移实零和（v1/v2
 *     A/B：色度 +1~2.7 dB 换 luma −0.6~1.1 dB），色度形状走矩阵不另
 *     开 qp 偏移。0.92 更细版实测全面回落已弃——出血 > 收益）
 *   冻结表对 .5 邻域尾值统一向更细一侧取岸（比公式值再细 1，见 9/10
 *   邻位）；表为唯一权威，公式仅作溯源。
 * 码率由帧级 sized 搜索重新平衡（体积守恒由码控保证，非矩阵保证）。
 * qmatrix_id 入帧头，旧解码器明确拒绝未知矩阵（UNSUPPORTED_MATRIX）。
 * A/B 裁决：bench_out/edge_jag/variants/（2026-09-21 战役）。 */
static const tc_qmatrix_set kQmEdgeBalanced = {
    { 20, 19, 18, 17, 16, 15, 14, 13,
      19, 18, 17, 16, 15, 14, 13, 12,
      18, 17, 16, 15, 14, 13, 12, 11,
      17, 16, 15, 14, 13, 12, 11, 11,
      16, 15, 14, 13, 12, 11, 11, 10,
      15, 14, 13, 12, 11, 11, 10, 9,
      14, 13, 12, 11, 11, 10, 9, 9,
      13, 12, 11, 11, 10, 9, 9, 8 },
    { 18, 17, 16, 16, 15, 14, 13, 13,
      17, 16, 16, 15, 14, 13, 13, 12,
      16, 16, 15, 14, 13, 13, 12, 11,
      16, 15, 14, 13, 13, 12, 11, 11,
      15, 14, 13, 13, 12, 11, 11, 10,
      14, 13, 13, 12, 11, 11, 10, 10,
      13, 13, 12, 11, 11, 10, 10, 9,
      13, 12, 11, 11, 10, 10, 9, 9 }
};

const tc_qmatrix_set* tc_qmatrix_by_id(uint8_t qmatrix_id)
{
    if (qmatrix_id == 0u) { return &kQmFlat; }
    if (qmatrix_id == 1u) { return &kQmStandard; }
    if (qmatrix_id == 2u) { return &kQm444Compact; }
    if (qmatrix_id == 3u) { return &kQm422LowCompact; }
    if (qmatrix_id == 4u) { return &kQmEdgeBalanced; }
    return NULL;
}

uint32_t tc_qp_scale(uint32_t qp)
{
    if (qp > TC_QP_MAX) { return 0u; }
    return kQpScale[qp];
}

uint32_t tc_qp_scale_tbl(uint32_t table_sel, uint32_t qp)
{
    if (qp > TC_QP_MAX) { return 0u; }
    if (table_sel == TC_QPTBL_REFINED) { return kQpScaleRefined[qp]; }
    if (table_sel == TC_QPTBL_CLASSIC) { return kQpScale[qp]; }
    return 0u; /* 未知表选择：无效（防御；调用方域校验前置） */
}

uint32_t tc_quant_step(uint16_t qm, uint32_t qp)
{
    return tc_quant_step_tbl(TC_QPTBL_CLASSIC, qm, qp);
}

uint32_t tc_quant_step_tbl(uint32_t table_sel, uint16_t qm, uint32_t qp)
{
    uint32_t s = tc_qp_scale_tbl(table_sel, qp);
    if (s == 0u) { return 0u; }
    /* v1.5 高 qp 域：qm 最大冻结值 655 × scale 7053950 ≈ 4.6e12 超出
     * uint32 —— 64 位中间量（每 slice 仅 ctx_init 64 次，非热路径）。 */
    uint32_t q = (uint32_t)((((uint64_t)qm * (uint64_t)s) + 128u) >> 8);
    return q == 0u ? 1u : q;
}

void tc_quant_block(const int32_t F[64], const uint16_t qm[64], uint32_t qp,
                    int32_t q_out[64])
{
    for (int i = 0; i < 64; ++i) {
        const uint32_t Q = tc_quant_step(qm[i], qp);
        const uint32_t dz = (i == 0) ? 0u : (Q >> 2);
        /* 先钳位再取绝对值：规避 INT32_MIN 取负 UB（防御路径，合法 |F| ≤ 2^24.4） */
        int32_t v = F[i];
        if (v > TC_TRANSFORM_MAX_ABS_F) { v = TC_TRANSFORM_MAX_ABS_F; }
        else if (v < -TC_TRANSFORM_MAX_ABS_F) { v = -TC_TRANSFORM_MAX_ABS_F; }
        const uint32_t mag = (uint32_t)(v < 0 ? -v : v);
        const uint32_t num = mag + (Q >> 1);
        const uint32_t qmag = num > dz ? (num - dz) / Q : 0u;
        q_out[i] = v < 0 ? -(int32_t)qmag : (int32_t)qmag;
    }
}

void tc_dequant_block(const int32_t q[64], const uint16_t qm[64], uint32_t qp,
                      int32_t Fp_out[64])
{
    for (int i = 0; i < 64; ++i) {
        const uint32_t Q = tc_quant_step(qm[i], qp);
        int64_t v = (int64_t)q[i] * (int64_t)Q;
        if (v > TC_TRANSFORM_MAX_ABS_F) { v = TC_TRANSFORM_MAX_ABS_F; }
        if (v < -TC_TRANSFORM_MAX_ABS_F) { v = -TC_TRANSFORM_MAX_ABS_F; }
        Fp_out[i] = (int32_t)v;
    }
}

/* ---- 阶段 9：预计算上下文（每 plane/slice 一次，替代每块 64 次表算 + 整数除法） ---- */

void tc_quant_ctx_init(tc_quant_ctx* ctx, const uint16_t qm[64], uint32_t qp)
{
    tc_quant_ctx_init_bd(ctx, qm, qp, 12u);
}

void tc_quant_ctx_init_bd(tc_quant_ctx* ctx, const uint16_t qm[64], uint32_t qp,
                          uint8_t bit_depth)
{
    tc_quant_ctx_init_bd_tbl(ctx, qm, qp, bit_depth, TC_QPTBL_CLASSIC);
}

void tc_quant_ctx_init_bd_tbl(tc_quant_ctx* ctx, const uint16_t qm[64],
                              uint32_t qp, uint8_t bit_depth,
                              uint32_t table_sel)
{
    ctx->f_clamp = 0;
    ctx->exact_div = (bit_depth > 12u) ? 1u : 0u;
    if (ctx->exact_div != 0) {
        /* |F|max ∝ max|x'|（12-bit 冻结域 2^24.4 @ |x'|≤2047 等比外推） */
        const int64_t x = (int64_t)((1u << (bit_depth - 1u)) - 1u);
        ctx->f_clamp = ((int64_t)TC_TRANSFORM_MAX_ABS_F * x) / 2047;
    }
    /* AC 死区 = Q/4（spec §7.4 冻结值；golden_transform/golden_codec 门禁）。
     * 历史：TC_DZ_DEV 实验旋钮已于 P3 扫频后移除（2026-09-03 战役销账：
     * dz 0~6/16 × 4 素材，工作码率区间 1.4~3.6 bpp 内 1/4 已最优，
     * 见 docs/Topos_码率效率优化计划_2026-09-03.md §7）。DC 恒 0。 */
    const uint32_t dz_num = 1u, dz_den = 4u;
    for (int i = 0; i < 64; ++i) {
        uint32_t q = tc_quant_step_tbl(table_sel, qm[i], qp);
        ctx->Q[i] = q;
        ctx->half[i] = q >> 1;
        ctx->dz[i] = (i == 0) ? 0u
            : (uint32_t)(((uint64_t)q * dz_num) / dz_den);
        tc_fastdiv_init(q, &ctx->fd[i]);
        /* M10-6.3B：zigzag 孪生（v1.5 域：fd.shift ≤ 25 → 51−s ≥ 26 无移位
         * UB；magic 恒 < 2^51 → m1 < 2^25 打包不变。域外大除数（>2^25−1，
         * 冻结 qm×qp 不可达）时 magic=0 → 孪生全 0，SIMD 内核不达该域） */
        uint64_t magic = ctx->fd[i].magic;
        if (magic == 0u) {
            magic = (UINT64_C(1) << (51u - ctx->fd[i].shift)) + 1u;
        }
        const uint32_t zz = kTcZigzagInv[i];
        ctx->half_zz[zz] = ctx->half[i];
        ctx->dz_zz[zz] = ctx->dz[i];
        ctx->fd_zz_m1[zz] = (uint32_t)(magic >> 26);
        ctx->fd_zz_m0[zz] = (uint32_t)(magic & (UINT64_C(0x3FFFFFF)));
        /* P-速⑥：自然序孪生（同 magic 拆分，自然索引） */
        ctx->fd_nat_m1[i] = (uint32_t)(magic >> 26);
        ctx->fd_nat_m0[i] = (uint32_t)(magic & (UINT64_C(0x3FFFFFF)));
    }
}

void tc_quant_block_ctx(const tc_quant_ctx* ctx, const int32_t F[64], int32_t q_out[64])
{
    const int64_t fclamp = ctx->f_clamp > 0 ? ctx->f_clamp : (int64_t)TC_TRANSFORM_MAX_ABS_F;
    for (int i = 0; i < 64; ++i) {
        /* 先钳位再取绝对值：规避 INT32_MIN 取负 UB（防御路径，合法 |F| ≤ 域钳位） */
        int32_t v = F[i];
        if (v > fclamp) { v = (int32_t)fclamp; }
        else if (v < -fclamp) { v = (int32_t)-fclamp; }
        const uint32_t mag = (uint32_t)(v < 0 ? -v : v);
        const uint32_t num = mag + ctx->half[i];
        /* deadzone 分支不可预测（阶段 9）：三元选择供 cmov，fastdiv(0)=0 保持语义 */
        const uint32_t num_eff = num > ctx->dz[i] ? num - ctx->dz[i] : 0u;
        /* 批 4：bd≥13 时 fastdiv magic 域（n ≤ 2^25）不再覆盖 → 真除法 */
        const uint32_t qmag = ctx->exact_div
            ? (ctx->Q[i] <= 1u ? num_eff : num_eff / ctx->Q[i])
            : tc_fastdiv_apply(num_eff, &ctx->fd[i]);
        q_out[i] = v < 0 ? -(int32_t)qmag : (int32_t)qmag;
    }
}

void tc_quant_block_zigzag(const tc_quant_ctx* ctx, const int32_t F[64], int32_t q_out[64])
{
    const int64_t fclamp = ctx->f_clamp > 0 ? ctx->f_clamp : (int64_t)TC_TRANSFORM_MAX_ABS_F;
    for (int i = 0; i < 64; ++i) {
        int32_t v = F[i];
        if (v > fclamp) { v = (int32_t)fclamp; }
        else if (v < -fclamp) { v = (int32_t)-fclamp; }
        const uint32_t mag = (uint32_t)(v < 0 ? -v : v);
        const uint32_t num = mag + ctx->half[i];
        const uint32_t num_eff = num > ctx->dz[i] ? num - ctx->dz[i] : 0u;
        const uint32_t qmag = ctx->exact_div
            ? (ctx->Q[i] <= 1u ? num_eff : num_eff / ctx->Q[i])
            : tc_fastdiv_apply(num_eff, &ctx->fd[i]);
        /* 与 tc_quant_block_ctx 数值逐系数一致，仅散射到扫描位置 */
        q_out[kTcZigzagInv[i]] = v < 0 ? -(int32_t)qmag : (int32_t)qmag;
    }
}

void tc_dequant_block_ctx(const tc_quant_ctx* ctx, const int32_t q[64], int32_t Fp_out[64])
{
    const int64_t fclamp = ctx->f_clamp > 0 ? ctx->f_clamp : (int64_t)TC_TRANSFORM_MAX_ABS_F;
    for (int i = 0; i < 64; ++i) {
        int64_t v = (int64_t)q[i] * (int64_t)ctx->Q[i];
        if (v > fclamp) { v = fclamp; }
        if (v < -fclamp) { v = -fclamp; }
        Fp_out[i] = (int32_t)v;
    }
}

void tc_dequant_inverse_samples(const tc_quant_ctx* ctx, const int32_t q[64],
                                const uint8_t* xs, const uint8_t* ys,
                                uint32_t count, int32_t* out)
{
    tc_dequant_inverse_samples_limited(ctx, q, xs, ys, count, 63u, out);
}

void tc_dequant_inverse_samples_limited(const tc_quant_ctx* ctx, const int32_t q[64],
                                        const uint8_t* xs, const uint8_t* ys,
                                        uint32_t count, uint8_t max_scan_pos,
                                        int32_t* out)
{
    if (ctx == NULL || q == NULL || xs == NULL || ys == NULL || out == NULL) { return; }
    int32_t Fp[64];
    memset(Fp, 0, sizeof(Fp));
    /* 批 4：钳位随位深外推（f_clamp；bd≤12 与旧 ±2^25 逐位一致） */
    const int64_t fclamp = ctx->f_clamp > 0 ? ctx->f_clamp : (int64_t)TC_TRANSFORM_MAX_ABS_F;
    const uint32_t limit = max_scan_pos > 63u ? 63u : (uint32_t)max_scan_pos;
    for (uint32_t scan_pos = 0u; scan_pos <= limit; ++scan_pos) {
        const uint32_t natural = kTcZigzag[scan_pos];
        int64_t v = (int64_t)q[natural] * (int64_t)ctx->Q[natural];
        if (v > fclamp) { v = fclamp; }
        else if (v < -fclamp) { v = -fclamp; }
        Fp[natural] = (int32_t)v;
    }
    tc_transform_inverse_8x8_samples_limited(Fp, xs, ys, count, max_scan_pos, out);
}

void tc_dequant_inverse_samples_half_limited(const tc_quant_ctx* ctx, const int32_t q[64],
                                             uint32_t count, uint8_t max_scan_pos,
                                             int32_t* out)
{
    tc_simd_dequant_inverse_samples_half_limited(ctx, q, count, max_scan_pos, out);
}
