/* topos_codec.h — Topos Video Codec 稳定 C ABI（公共）。
 *
 * 规范：docs/bitstream_spec_v1.md（位流）与 ADR-C017（ABI 形态）。
 * 约定：
 *  - 所有公开 struct 第一字段 struct_size、第二字段 abi_version；
 *    调用方以全 0 初始化；lib 写入 struct_size = C 侧 sizeof。
 *    绑定层据此做镜像一致性校验（风险 R-18）。
 *  - reserved 字段：lib 不读不写（调用方可任意填充，round-trip 保持）。
 *    唯一例外：topos_frame_config.reserved[0] 承载熵模式（P1-10 过渡期
 *    载体），reserved[1..7] 必须为 0（encode 拒绝非零）。下一 ABI 以
 *    正式 entropy/profile 枚举替代 reserved 承载。
 *  - 状态码语义冻结于 spec §9；新增错误码只能追加更负的值并提升 ABI 版本。
 *  - 可重入性：本库全部函数为纯函数或线程局部错误状态，可多线程并发调用；
 *    context 型 API（阶段 4）单个 context 不可重入。
 *  - 指针所有权：返回的 const char* 一律为 lib 拥有的静态串或线程局部缓冲，
 *    调用方不得 free、不得跨线程共享、有效期至本线程下一次 lib 调用。
 */
#ifndef TOPOS_CODEC_H
#define TOPOS_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI 版本：公开结构布局或函数语义的每次不兼容变更 +1
 * v2（R3，2026-08-30）：追加 topos_alpha_budget_info + tc_mux_set_alpha_budget
 * + tc_movie_alpha_budget（纯新增，既有结构/函数不变；镜像不匹配将显式失败） */
#define TOPOS_CODEC_ABI_VERSION 2

/* 状态码 —— 与 bitstream_spec_v1.md §9 逐项一致，值冻结 */
enum {
    TC_OK                       =   0,
    TC_WARN_CONCEALED           =   1,
    TC_ERR_INVALID_ARGUMENT     =  -1,
    TC_ERR_OUT_OF_MEMORY        =  -2,
    TC_ERR_UNSUPPORTED_VERSION  =  -3,
    TC_ERR_UNSUPPORTED_PROFILE  =  -4,
    TC_ERR_UNSUPPORTED_PIXEL_FORMAT = -5,
    TC_ERR_UNSUPPORTED_MATRIX   =  -6,
    TC_ERR_UNSUPPORTED_ALPHA_MODE = -7,
    TC_ERR_LIMIT_EXCEEDED       =  -8,
    TC_ERR_MALFORMED            =  -9,
    TC_ERR_TRUNCATED            = -10,
    TC_ERR_CHECKSUM_MISMATCH    = -11,
    TC_ERR_STATE                = -12,
    TC_ERR_CANCELLED            = -13,
    TC_ERR_IO                   = -14,
    TC_ERR_BUFFER_TOO_SMALL     = -15,
    TC_ERR_NOT_IMPLEMENTED      = -16,
    /* V9 GOP context（topos_v9_micro_gop_plan §3.8；参考失效语义）：
     * 参考不可用/被污染时的同 GOP P 帧——调用方须跳到下一 I。 */
    TC_ERR_REFERENCE_INVALID    = -17
};

typedef struct topos_version_info {
    uint32_t    struct_size;      /* lib 写入 sizeof(topos_version_info) */
    uint32_t    abi_version;      /* == TOPOS_CODEC_ABI_VERSION */
    uint32_t    version_major;
    uint32_t    version_minor;
    uint32_t    version_patch;
    const char* git_commit;       /* 静态串，lib 所有 */
    const char* build_target;     /* 静态串：平台-编译器标识 */
    uint32_t    reserved[4];
} topos_version_info;

/* CPU 能力位（阶段 9 dispatch 使用；当前仅探测，不影响任何行为） */
#define TOPOS_CPU_X86_AVX2    (1u << 0)
#define TOPOS_CPU_X86_AVX512F (1u << 1)
#define TOPOS_CPU_ARM_NEON    (1u << 2)
#define TOPOS_CPU_X86_FMA     (1u << 3)

typedef struct topos_cpu_features {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t flags;               /* TOPOS_CPU_* 位或 */
    uint32_t reserved[5];
} topos_cpu_features;

/* Codec capability bits. These describe implemented public execution paths;
 * an internal parser or a base-only primitive is not advertised as V7 output. */
#define TC_CODEC_CAP_FULL_DECODE             (1u << 0)
#define TC_CODEC_CAP_FIXED_REDUCED_DECODE   (1u << 1)
#define TC_CODEC_CAP_AUTO_2K_DECODE         (1u << 2)
#define TC_CODEC_CAP_CPU_SURFACE_OUTPUT     (1u << 3)
#define TC_CODEC_CAP_HOST_VISIBLE_GPU_SURFACE (1u << 4)
#define TC_CODEC_CAP_V7A_BAND_DECODE       (1u << 5) /* 已退役（2026-09-13 归档）：常量保留仅为 ABI 兼容，tc_query_capabilities 不再广播 */
#define TC_CODEC_CAP_V7B_SCALABLE_BASE     (1u << 6)
#define TC_CODEC_CAP_DECODE_DROP_ALPHA     (1u << 7) /* request flags: omit alpha */

#define TC_CODEC_MAX_DIM 16384u
#define TC_CODEC_AUTO_2K_MAX_DIM 2048u

typedef struct topos_codec_capabilities {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t flags;               /* TC_CODEC_CAP_* 位或 */
    uint32_t max_width;           /* 当前公共 frame ABI 最大 visible width */
    uint32_t max_height;          /* 当前公共 frame ABI 最大 visible height */
    uint32_t auto2k_max_dim;      /* AUTO_2K 长边上限 */
    uint32_t reserved[4];
} topos_codec_capabilities;

/* V7-B scalable 发布状态（P0-04 四态，与 capability_manifest 的
 * reduced_decode.scalable_status_states 一一对应）。UI/CLI/日志必须以此
 * 区分「可复现参考实现」与「已发布能力」；reference/experimental 状态下
 * 不得把 opt-in 或 fallback 展示为 native scalable release。 */
typedef enum topos_scalable_status {
    TC_SCALABLE_STATUS_EXPERIMENTAL = 0, /* 开发中布局，不可写用户文件默认路径 */
    TC_SCALABLE_STATUS_REFERENCE = 1,    /* 读写链路完整可 opt-in，门禁未过 */
    TC_SCALABLE_STATUS_ELIGIBLE = 2,     /* P4 门禁全过，待 rollout 决策 */
    TC_SCALABLE_STATUS_DEFAULT = 3       /* 高分辨率新素材默认 writer */
} topos_scalable_status;

/* 返回 TC_OK 且填充 *out；out == NULL → TC_ERR_INVALID_ARGUMENT。 */
int32_t tc_query_scalable_status(topos_scalable_status* out);

/* 状态枚举 → 固定字符串（"experimental"/"reference"/"eligible"/"default"），
 * 未知值返回 "unknown"。永不失败、永不返回 NULL，供日志/CLI 直接使用。 */
const char* tc_scalable_status_name(topos_scalable_status status);

/* 返回 TOPOS_CODEC_ABI_VERSION（无失败路径） */
int32_t tc_abi_version(void);

/* TC_OK 且填充 *out；out == NULL → TC_ERR_INVALID_ARGUMENT（写 last_error） */
int32_t tc_version(topos_version_info* out);

/* 同上；flags 见 TOPOS_CPU_* */
int32_t tc_query_cpu_features(topos_cpu_features* out);

/* Public capability query. The V7-A bit covers public one-shot elementary-frame
 * reading; V7-B remains clear until scalable base/residual layouts are public. */
int32_t tc_query_capabilities(topos_codec_capabilities* out);

/* 能力协商（阶段 10）：给定 profile/pixel_format/bit_depth/alpha_mode 组合，
 * 本 lib 能否解码该码流。TC_OK = 支持；否则返回对应的 TC_ERR_UNSUPPORTED_*
 * （字段→错误码：profile→PROFILE、pixel_format 与 bit_depth→PIXEL_FORMAT
 *  —— v1.2/v1.3 枚举域 = YUV 4:2:2|4:4:4 × 10/12-bit、alpha_mode→ALPHA_MODE）。
 * 版本协商入口是 tc_abi_version()/tc_version()。纯函数，无失败路径以外的状态。 */
int32_t tc_query_support(uint32_t profile, uint32_t pixel_format, uint32_t bit_depth,
                         uint32_t alpha_mode);

/* 任意 status（含未知值）→ 非空静态串；未知码返回 "unknown status" */
const char* tc_status_message(int32_t status);

/* 本线程最近一次错误详情；无错误时返回 ""。
   返回串 lib 所有，有效期至本线程下一次 lib 调用。 */
const char* tc_last_error(void);

/* ===================== 阶段 4：elementary frame 编解码 =====================
 *
 * 完整标量 codec：plane padding → level shift → 整数 DCT → 量化 → 符号化/熵编码
 * → slice 组包（编码），及其逆过程 + concealment（解码）。
 * 规范：bitstream_spec §5/§7/§8/§9/§11；确定性（§11.1）：相同输入 + 相同配置
 * → bit-exact 相同输出。线程契约（P1-06 修订，与实现一致）：
 *  - 生产 encode/decode 入口可多线程并发调用：帧级状态 caller-owned，
 *    错误线程局部；编码侧进程级 enc_shared 缓存以原子 busy 标志互斥，
 *    竞争方回退栈实例（语义不变）。
 *  - dev 观测（tc_dev_*_stats / 符号直方图）为共享累加器，内部以互斥锁
 *    保护，可并发读写；默认关闭（TOPOS_CODEC_PROFILE），不进生产热路径。
 *  - dev 开关（tc_dev_set_*）为原子变量，可任意线程调用；语义上仍建议
 *    "配置于编码/解码开始前"，运行中切换只保证原子可见、不保证某帧用到
 *    切换前还是后的值。
 */

#define TC_FRAME_MAX_PLANES 4
/* slice_rows==0 时的默认带高（块行）。M10-6.4：32→16——8 物理核上带内
 * DC/熵链路串行，34 带/帧在 8 线程下尾部失衡吞掉近半并行度（2K 解码
 * 实测 259→382 fps，编码同步 +9~21%）；16 的带数翻倍后调度粒度足够，
 * 8 带粒度已无进一步收益。旧流按自身头字段解码，不受默认值影响。 */
#define TC_FRAME_DEFAULT_SLICE_ROWS 16u

/* 编码配置。以全 0 初始化（struct_size/abi_version 除外）即得到合法最小配置，
 * 再按需覆盖；颜色标签非法值在 encode 时按 §4.2 规则拒绝。 */
typedef struct topos_frame_config {
    uint32_t struct_size;
    uint32_t abi_version;
    uint16_t visible_width;      /* 1..16384 */
    uint16_t visible_height;     /* 1..16384 */
    uint8_t profile;            /* v1 仅 3（Standard） */
    uint8_t pixel_format;       /* 0 = YUV 4:2:2（默认）；1 = YUV 4:4:4
                                    （v1.3 枚举扩展，R4.2）；2 = GBR
                                    （v1.4，R4.3）；3 = CFA/TRAW
                                    （v1.6，批 1——4 相位平面） */
    uint8_t bit_depth;          /* v1 仅 10 */
    uint8_t qmatrix_id;         /* 0=flat / 1=Standard / 2=444 Compact / 3=422 Low Compact /
                                   4=Edge-Balanced（422 视频档，2026-09-21） */
    uint8_t qp_base;            /* v1.5 起 0..95（raw 档锚 45..82）；帧级码率调节主旋钮 */
    int8_t qp_delta_luma;       /* plane0 每 slice qp 偏移；qp_eff ∈ 0..95（v1.5） */
    int8_t qp_delta_chroma;     /* plane1/2 每 slice qp 偏移（slice 级码率调节） */
    uint8_t slice_rows;         /* 每带块行数（1 带行 = 8 像素行）；0=默认 16；
                                    须保证全帧 slice_count ≤ 512 */
    uint8_t alpha_mode;         /* 0 无 / 1 无损 / 2 受限近似（spec v1.1 §8.6） */
    uint8_t alpha_bit_depth;    /* mode1 必须 16；mode2 ∈ {8,10,12}（近似精度） */
    uint8_t alpha_premultiplied;/* → header flags bit0 */
    uint8_t color_range;        /* 0 limited / 1 full */
    uint8_t color_primaries;    /* §A.7 */
    uint8_t color_transfer;     /* §A.7 */
    uint8_t color_matrix;       /* §A.7（YUV：1/5/9） */
    uint8_t chroma_siting;      /* 0/1/2 */
    uint16_t sar_num;           /* 0/0 = 未指定 */
    uint16_t sar_den;
    uint32_t reserved[8];       /* reserved[0] 低字节 = 编码模式选择（单一权威
                                   模式表，2026-09-27 更新；与 codec.c 写域
                                   校验同步维护）。
                                   写域（编码可写）：
                                   0 = V1 流（默认，major=1，位流与旧版逐字
                                       节一致）；
                                   1 = V2 + canonical VLC（major=2）；
                                   8 = V7-R2（产品 intra 默认熵档，major=7）；
                                   9 = V8 段化 rANS（实验，major=8）；
                                   10 = V9 帧间微 GOP（major=9；帧级 sized
                                       不适用，走 GOP context 写口）；
                                   11 = V7-R3 帧间微 GOP（LP 载体，(7,8)）。
                                   退役域 {2..7}（V2-Rice/V3/V4/V5/V6/V7-R，
                                   V 代际收纳 2026-09-13 起显式拒绝——配置
                                   校验返回 TC_ERR_INVALID_ARGUMENT；对应
                                   位流代际仅保留解码，编号永久封存）。
                                   reserved[1]/[2] = V2 AQ/RDO（0/1）；
                                   reserved[3..7]：按模式承载结构化槽位
                                   （8/9/10/11 见 spec），其余必须为 0。 */
    } topos_frame_config;

/* 输入帧：planar uint16（值域 0..2^bit_depth−1；alpha 0..65535）。
 * planes[i] 为 NULL 的平面必须 i ≥ plane_count；Y/A 尺寸 visible_width×visible_height；
 * U/V 在 4:2:2（pixel_format=0）为 ceil(visible_width/2)×visible_height，
 * 在 4:4:4（pixel_format=1，R4.2）为 visible_width×visible_height（§5）。
 * strides 单位为 uint16 元素，0 = tight。 */
typedef struct topos_frame_input {
    uint32_t struct_size;
    uint32_t abi_version;
    const uint16_t* planes[TC_FRAME_MAX_PLANES];
    size_t strides[TC_FRAME_MAX_PLANES]; /* uint16 元素；0 = tight */
    uint32_t reserved[8];
} topos_frame_input;

/* 编码统计（spec §11.3：颜色/Alpha/总尺寸必须分别暴露）。 */
typedef struct topos_frame_stats {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t packet_size;          /* == frame_packet_size（含 53B frame header） */
    uint32_t color_payload_bytes;  /* plane0..2 slice payload 之和（不含 17B slice headers） */
    uint32_t alpha_payload_bytes;  /* plane3 slice payload 之和 */
    uint32_t color_header_bytes;   /* 53 + 17 × 颜色 slice 数 */
    uint32_t alpha_header_bytes;   /* 17 × alpha slice 数 */
    uint16_t slice_count;
    uint8_t qp_base;               /* 实际使用的 qp_base */
    uint8_t reserved8;
    uint16_t alpha_max_abs_error;  /* mode2：|源−近似| 最大值；mode0/1 为 0 */
    uint16_t reserved16;
    uint32_t reserved[8];
} topos_frame_stats;

/* 解码结果描述 + 逐 slice 状态（§9 concealment 规则的可查询面）。 */
#define TC_FRAME_SLICE_OK 0u
#define TC_FRAME_SLICE_CONCEALED 1u
#define TC_MAX_SLICES 512

typedef struct topos_frame_output {
    uint32_t struct_size;
    uint32_t abi_version;
    uint16_t visible_width;
    uint16_t visible_height;
    uint16_t coded_width;
    uint16_t coded_height;
    uint8_t plane_count;
    uint8_t bit_depth;
    uint8_t profile;
    uint8_t pixel_format;
    uint8_t alpha_mode;
    uint8_t alpha_bit_depth;
    uint8_t color_range;
    uint8_t color_primaries;
    uint8_t color_transfer;
    uint8_t color_matrix;
    uint8_t chroma_siting;
    uint16_t concealed_slices;      /* slice_status 中 CONCEALED 的个数（≤ slice_count） */
    uint16_t slice_count;           /* ≤ TC_MAX_SLICES（§10 上限 512） */
    uint16_t sar_num;
    uint16_t sar_den;
    uint8_t slice_status[TC_MAX_SLICES]; /* TC_FRAME_SLICE_*；包内顺序 */
    uint32_t reserved[8];
} topos_frame_output;

/* 仅校验配置（encode 的前置检查；NULL/struct_size 不符 → INVALID_ARGUMENT）。 */
int32_t tc_frame_config_validate(const topos_frame_config* cfg);

/* 编码输出缓冲上界估计（保守；超过 256 MiB 硬上限的情形按 256 MiB 返回）。
 * 仅供调用方分配参考；非保证，实际以 encode 返回为准。 */
size_t tc_frame_packet_bound(const topos_frame_config* cfg);

/* 编码一帧。out_cap 不足 → TC_ERR_BUFFER_TOO_SMALL（不写 out）；
 * 超 256 MiB → TC_ERR_LIMIT_EXCEEDED。stats 可 NULL。输出自检通过 §4 全结构校验。 */
int32_t tc_frame_encode(const topos_frame_config* cfg, const topos_frame_input* input,
                        uint8_t* out, size_t out_cap, topos_frame_stats* stats);

/* Explicit opt-in V7-B scalable encoder.  The base layer keeps its longest
 * edge at base_max_dim (0 selects TC_CODEC_AUTO_2K_MAX_DIM); the enhancement
 * residual preserves the source-resolution full decode.  This entry point is
 * intentionally not selected by tc_frame_encode or the default rollout until
 * the RD4-08 size/speed gates pass.  Alpha is currently unsupported.
 *
 * A size probe is available with out == NULL and out_cap == 0: it returns
 * TC_ERR_BUFFER_TOO_SMALL and, when stats is non-NULL, writes the required
 * packet_size.  A real encode returns TC_OK and writes one complete V7-B
 * elementary packet.  base_max_dim must be in [1, TC_CODEC_AUTO_2K_MAX_DIM]
 * unless it is zero. */
int32_t tc_frame_encode_scalable(const topos_frame_config* cfg,
                                 const topos_frame_input* input,
                                 uint32_t base_max_dim,
                                 uint8_t* out, size_t out_cap,
                                 topos_frame_stats* stats);

/* 目标尺寸编码（确定性 qp 搜索，§11.1 纯函数；2026-09-27 语义更新）：
 * 从 cfg->qp_base 种子起按预算双向搜索（strict 对数斜率跳步 + 括号二分，
 * M10-7C；探针超预算/耗尽/单调性破坏时回退 legacy 粗步 8 + 细步 1 线性
 * 搜索并按 < 3/4 target 回收画质）。probe 阶段只计字节数，不写 out——
 * 中间 QP 超出 out_cap 不终止搜索（C05）；容量约束只对最终选中包应用，
 * 最终包仍超容量才返回 TC_ERR_BUFFER_TOO_SMALL（out 原子不变，C04）。
 * best-effort：全域超限时返回 qp_max 包及实际大小，qp_used（可 NULL）
 * 暴露实际 qp。M7 DCT-once 精确位计数不可用（超限/OOM/禁用）时自动
 * 回退整帧编码 probe，决策语义一致。 */
int32_t tc_frame_encode_sized(const topos_frame_config* cfg, const topos_frame_input* input,
                              uint32_t target_bytes, uint8_t qp_min, uint8_t qp_max,
                              uint8_t* qp_used, uint8_t* out, size_t out_cap,
                              topos_frame_stats* stats);

/* 解码一帧。两段式：planes_out == NULL → 仅解析并填充 out_info（TC_OK 或整帧拒绝码）。
 * planes_out[i]（i < plane_count）为调用方缓冲：Y/A 尺寸 visible_w×visible_h；
 * U/V 在 4:2:2 为 ceil(visible_w/2)×visible_h、在 4:4:4（pf=1，R4.2）为
 * visible_w×visible_h，strides[i] 为 uint16 元素行距（0 = tight）。
 * slice 损坏按 §9 conceal：该带填中性值（颜色 2^(bd−1) / alpha 65535），
 * 返回 TC_WARN_CONCEALED（帧仍交付）。frame header/结构失败 → 负值错误码、不产出帧。 */
int32_t tc_frame_decode(const uint8_t* data, size_t size,
                        uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                        const size_t strides[TC_FRAME_MAX_PLANES],
                        topos_frame_output* out_info);

/* 目标尺寸解码：在 slice 熵解码后只重建目标采样网格，不创建完整源平面。
 * target 尺寸必须不大于源 visible 尺寸；码流仍需顺序读取并校验全部 slice，
 * 因而这是 target-size reconstruction，不是跳过压缩包读取。输出采样语义与
 * image preview 的 nearest 采样一致。 */
int32_t tc_frame_decode_scaled(const uint8_t* data, size_t size,
                               uint32_t target_width, uint32_t target_height,
                               uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                               const size_t strides[TC_FRAME_MAX_PLANES],
                               topos_frame_output* out_info);

/* 固定比例低频预览解码。此路径只保留目标采样所需的低频量化系数，
 * 并跳过高频系数的反量化/逆变换；为保持可变长熵码流的完整性，当前版本
 * 仍会消费高频码字但不写入系数矩阵（真正的 bitstream-level skip 需要新的
 * 分层/块长度码流版本）。输出是近似预览，不等同于完整解码后再缩放。 */
typedef enum tc_decode_scale {
    TC_DECODE_SCALE_FULL = 0,
    TC_DECODE_SCALE_HALF = 1,
    TC_DECODE_SCALE_THIRD = 2,
    TC_DECODE_SCALE_QUARTER = 3,
    TC_DECODE_SCALE_EIGHTH = 4
} tc_decode_scale;

/* 返回固定比例输出尺寸：ceil(source / divisor)。FULL 返回源尺寸。 */
int32_t tc_decode_scale_dimensions(uint32_t source_width, uint32_t source_height,
                                   tc_decode_scale scale,
                                   uint32_t* target_width, uint32_t* target_height);

int32_t tc_frame_decode_reduced(const uint8_t* data, size_t size,
                                tc_decode_scale scale,
                                uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                const size_t strides[TC_FRAME_MAX_PLANES],
                                topos_frame_output* out_info);

/* ===================== 阶段4：跨帧批量解码（单批跨帧切片队列） =====================
 *
 * 高频连续解码（播放/转码/审查）应优先批量入口：count 帧的全部 slice
 * 任务进同一线程池批次——一次唤醒/汇合消化 N 帧，消除逐帧派发/汇合与
 * 帧间串行段（单帧入口在 2K t8 下该项约占 30% 墙钟）。全 intra 无帧间
 * 依赖，批内输出内存互不相交，执行顺序不影响确定性输出（§11.1）。
 *
 * 逐帧语义与单帧入口一致：§9 conceal 帧仍交付（infos[i].concealed_slices
 * > 0，返回码含 TC_WARN_CONCEALED）；slice OOM 等致命错误只拒绝该帧，
 * 批内其余帧照常交付。返回值 = 首个非 OK 帧的返回码（帧序最小；错误
 * 定位以返回码为准）。并发调用单帧解码会被线程池 id_base 挤位退化
 * 单线程——跨帧并行必须经批量入口承载。 */

typedef struct topos_batch_packet {
    const uint8_t* data; /* 仅调用期间引用 */
    size_t size;
} topos_batch_packet;

/* 无状态批量解码。planes_out 为 count × TC_FRAME_MAX_PLANES 的扁平指针
 * 数组（行 = 帧），strides 同形（NULL = 全 tight；元素 0 = 该 plane tight）。
 * infos 为 count 项紧凑数组，逐帧 memset 重填。planes_out == NULL → 查询
 * 模式：仅逐帧填充几何（不重建；同单帧两段式）。packets[i] 结构解析失败
 * → 整批立即返回该错误码、不解码任何帧（infos 内容未定义）。count == 0
 * → TC_OK（无操作）。 */
int32_t tc_frame_decode_batch(const topos_batch_packet* packets, uint32_t count,
                              uint16_t* const* planes_out, const size_t* strides,
                              topos_frame_output* infos);

/* 固定比例批量低频预览解码。planes_out 的布局与 tc_frame_decode_batch 相同；
 * 所有帧进入同一跨帧切片任务批次。 */
int32_t tc_frame_decode_batch_reduced(const topos_batch_packet* packets, uint32_t count,
                                      tc_decode_scale scale,
                                      uint16_t* const* planes_out, const size_t* strides,
                                      topos_frame_output* infos);

/* plane 的 visible 尺寸（uint16 元素计）；供调用方分配输出缓冲。 */
int32_t tc_frame_plane_geometry(const topos_frame_output* info, uint32_t plane,
                                uint32_t* width, uint32_t* height);

/* ===================== 持久 decoder context（M2，低拷贝高频解码） =====================
 *
 * tc_frame_decode 为无状态单发接口；高频播放/连续解码应使用 context：
 *  - prepare 只做结构解析并填充几何（不扫 payload CRC——不承诺 payload 完整性），
 *    供调用方在 open/格式变化时分配/复用输出平面；
 *  - decode 单次调用完成整帧解码（结构解析 + 每 slice 一次 CRC + 统一并行重建），
 *    输出直写调用方 plane view（可见几何内 clip），无中间复制；
 *  - context 持有 grow-only 内部缓冲（alpha 行缓冲池），几何稳定时稳态零分配。
 *
 * 所有权与并发：单实例不可重入（一次仅一个 decode 在飞）；不同实例可并发。
 * packet 指针仅在调用期间引用。out_info 每次调用都会被重写（先 memset）。
 * 返回值语义与 tc_frame_decode 全解码模式一致（TC_OK / TC_WARN_CONCEALED /
 * 负错误码）。尺寸/步长校验失败 → TC_ERR_INVALID_ARGUMENT，不写输出。 */

typedef struct topos_plane_view {
    uint32_t struct_size;
    uint32_t abi_version;
    uint16_t* pixels; /* visible_w × visible_h 容量（stride ≥ visible_w） */
    size_t stride;    /* uint16 元素计行距；0 = tight（= visible_w） */
} topos_plane_view;

/* 输出内存域提示。CPU 与 HOST_VISIBLE_GPU 共享同一个无复制写入 ABI；后者
 * 由平台适配层传入已映射、可被 CPU 写入的 GPU/共享缓冲。codec 核心不持有
 * 原生 Metal/D3D/Vulkan 资源，也不负责提交 GPU 同步命令。
 *
 * surface ownership protocol（由平台层执行，codec 不代办）：
 *   acquire -> map -> tc_decoder_decode_surface[_request] -> unmap
 *   -> platform fence -> release
 * codec 只在同步调用期间写入 topos_plane_view.pixels，返回后不保存任何
 * surface/plane 指针；调用方不得在 decode 返回前 unmap 或复用该资源。 */
enum {
    TC_DECODE_MEMORY_CPU = 0u,
    TC_DECODE_MEMORY_HOST_VISIBLE_GPU = 1u
};

/* RD1：统一解码请求。新调用方通过 request 传递模式/目标尺寸，避免为
 * 1/2、1/3、1/4、1/8 和未来 V7 路径继续增加位置参数。旧入口保持 ABI
 * 不变；request 的 struct_size/abi_version 必须精确匹配本头文件。 */
typedef enum topos_decode_mode {
    TC_DECODE_MODE_FULL = 0u,     /* 完整源分辨率/完整系数 */
    TC_DECODE_MODE_SCALED = 1u,   /* 目标尺寸重建/完整系数 */
    TC_DECODE_MODE_REDUCED = 2u,  /* 固定比例低频重建 */
    TC_DECODE_MODE_AUTO_2K = 3u   /* 自动选择不超过 2K 的预览路径 */
} topos_decode_mode;

typedef enum tc_decode_quality {
    TC_DECODE_QUALITY_DEFAULT = 0u,
    TC_DECODE_QUALITY_FAST = 1u,
    TC_DECODE_QUALITY_BALANCED = 2u,
    TC_DECODE_QUALITY_HIGH = 3u
} tc_decode_quality;

typedef struct topos_decode_request {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t mode;          /* topos_decode_mode */
    uint32_t scale;         /* tc_decode_scale；仅 REDUCED 使用 */
    uint32_t target_width;  /* SCALED/AUTO_2K 可选；0 必须与 height 同为 0 */
    uint32_t target_height;
    uint32_t quality;       /* tc_decode_quality；V1 fallback 仅校验并保留 */
    uint32_t memory_type;   /* TC_DECODE_MEMORY_* */
    uint32_t flags;         /* TC_DECODE_FLAG_* 位或 */
    uint32_t reserved[4];   /* 必须为 0 */
} topos_decode_request;

/* 解码请求可选标志。DROP_ALPHA 仅影响带 Alpha 的帧：输出 plane_count
 * 变为 3、alpha_mode 变为 0，并跳过 Alpha slice 重建；不带 Alpha 或
 * native 不支持该标志的代际保持原有输出语义。 */
#define TC_DECODE_FLAG_DROP_ALPHA (1u << 0)

/* 统一无状态解码入口。request == NULL 不合法；仅查询时 planes_out == NULL，
 * out_info 仍返回实际目标几何。AUTO_2K 在当前 V1/V2 码流上选择固定比例
 * reduced fallback；V7-A 已由同一请求入口切换到可跳段路径，V7-B 的 FULL 与
 * REDUCED/AUTO_2K one-shot 已分别接入完整层/有界 base 层，持久 context、batch
 * 和 MOV sample 路径也已接入；默认 scalable capability 仍待 size/speed gate。 */
int32_t tc_frame_decode_request(const uint8_t* data, size_t size,
                                const topos_decode_request* request,
                                uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                const size_t strides[TC_FRAME_MAX_PLANES],
                                topos_frame_output* out_info);

typedef struct topos_decode_surface {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t memory_type;
    uint32_t reserved;
    topos_plane_view planes[TC_FRAME_MAX_PLANES];
} topos_decode_surface;

typedef struct topos_decoder_config {
    uint32_t struct_size;
    uint32_t abi_version;
    /* M10-4：本 context 每帧 slice 批次的 worker 预算（0 = 默认 = 进程线程
     * 数）。顺序流水两路各配半池时，tpool 按 id_base 把并发批次落到不相交
     * 的 worker 区间。sizeof 不变（原 reserved[7]），旧调用方全 0 即默认。 */
    uint32_t max_slice_workers;
    uint32_t reserved[7]; /* 全 0 初始化即合法最小配置 */
} topos_decoder_config;

typedef struct tc_decoder tc_decoder; /* opaque */

/* cfg 可 NULL（默认配置）。创建失败 → 负错误码，*out 不被写。 */
int32_t tc_decoder_create(const topos_decoder_config* cfg, tc_decoder** out);
void tc_decoder_destroy(tc_decoder* dec);

/* 更新 context 的 slice worker 预算。0 恢复为进程级默认值。
 * 可在 context 空闲时调用；实现以原子值发布，已在飞帧继续使用其
 * 已采样的预算，下一帧开始使用新值。 */
int32_t tc_decoder_set_max_slice_workers(tc_decoder* dec, uint32_t workers);

/* 几何/元数据查询：结构解析 + 填充 info。等价 tc_frame_decode 查询模式。 */
int32_t tc_decoder_prepare(tc_decoder* dec, const uint8_t* packet, size_t size,
                           topos_frame_output* info);

/* request 版本：返回 request 解析后的目标几何。AUTO_2K 会按每帧源尺寸
 * 选择 fixed-ratio reduced fallback；旧 prepare 入口保持 FULL 语义。 */
int32_t tc_decoder_prepare_request(tc_decoder* dec, const uint8_t* packet, size_t size,
                                   const topos_decode_request* request,
                                   topos_frame_output* info);

/* 单次整帧解码：out[i]（i < info.plane_count）须已填 struct_size/abi_version
 * 与有效 pixels。stride 校验失败即拒帧（不写任何输出）。 */
int32_t tc_decoder_decode(tc_decoder* dec, const uint8_t* packet, size_t size,
                          const topos_plane_view out[TC_FRAME_MAX_PLANES],
                          topos_frame_output* info);

/* request 版本：输出平面按 request 解析后的目标几何分配；旧 decode 入口保持
 * 完整源分辨率语义。 */
int32_t tc_decoder_decode_request(tc_decoder* dec, const uint8_t* packet, size_t size,
                                  const topos_decode_request* request,
                                  const topos_plane_view out[TC_FRAME_MAX_PLANES],
                                  topos_frame_output* info);

/* context 批量解码（阶段4 跨帧任务图；语义契约见 tc_frame_decode_batch）：
 * out 为 count × TC_FRAME_MAX_PLANES 的 topos_plane_view 扁平数组（行 = 帧，
 * 逐项须填 struct_size/abi_version/pixels）。out == NULL → 查询模式（仅填
 * 几何）。逐帧 stride/plane view 校验失败只拒该帧，批内其余帧照常交付。 */
int32_t tc_decoder_decode_batch(tc_decoder* dec,
                                const topos_batch_packet* packets, uint32_t count,
                                const topos_plane_view* out, topos_frame_output* infos);

/* request 版本：同一 request 应用于整批，AUTO_2K 对每帧独立解析几何。 */
int32_t tc_decoder_decode_batch_request(tc_decoder* dec,
                                        const topos_batch_packet* packets, uint32_t count,
                                        const topos_decode_request* request,
                                        const topos_plane_view* out,
                                        topos_frame_output* infos);

/* 直接写入 caller-owned surface。HOST_VISIBLE_GPU 仅表示该 surface 已由平台
 * 层完成映射/同步准备；函数本身不产生中间帧副本。 */
int32_t tc_decoder_decode_surface(tc_decoder* dec, const uint8_t* packet, size_t size,
                                  const topos_decode_surface* surface,
                                  topos_frame_output* info);

/* request 版本：memory_type 必须与 surface 一致；写入语义与旧 surface 入口相同。 */
int32_t tc_decoder_decode_surface_request(tc_decoder* dec, const uint8_t* packet, size_t size,
                                          const topos_decode_request* request,
                                          const topos_decode_surface* surface,
                                          topos_frame_output* info);

/* ============ V9 GOP context（topos_v9_micro_gop_plan；ADR-C047 P0） ============
 * 帧间微 GOP（major=9）的跨帧序列状态机 + 参考帧事务。
 *
 * 序列规则（§3.2/§3.8 冻结）：
 *   - 首个被接受的帧必须是 I（frame_type=0）；NO_REF 状态下 P 拒绝；
 *   - I 恒合法（任意帧可插 I；场景切换/回退 = 编码器纪律），推进 gop_id；
 *   - P 仅在 REF_READY 状态且 gop_id 与当前 GOP 一致时合法（跨 GOP 引用
 *     拒绝）；ref_distance 强校验在帧头（I=0/P=1）；
 *   - 任何被拒绝/损坏的帧不得成为参考（事务不提交）：坏帧后 REF_INVALID，
 *     同 GOP 后续 P 返回 TC_ERR_REFERENCE_INVALID 直到下一 I（调用方跳
 *     sync 点，禁静默沿用污染参考）；坏瓦片 CRC 的 conceal 帧（V8 像素机
 *     conceal 继续仍返回 TC_OK）同样按 MALFORMED 拒绝且不安装（§3.8）；
 *   - abort/reset 回滚到最近一次成功提交的状态（取消不留参考）。
 * 参考链归属（单链语义）：encode_frame 与 feed 共享同一条参考链——对
 * 同一帧交错调用两者会把 P 残差应用两次；解码链建模须用独立实例。
 * BUFFER_TOO_SMALL：全量不提交（含参考像素与 GOP 计数）——扩缓冲后以
 * 同输入重试，输出逐字节确定。
 * 单实例不可重入；不同实例可并发。 */
typedef enum topos_gop_ref_state {
    TOPOS_GOP_NO_REF       = 0, /* 尚无有效参考（期待 I） */
    TOPOS_GOP_REF_READY    = 1, /* 参考可用（I/P 重建已提交） */
    TOPOS_GOP_REF_INVALID  = 2  /* 参考失效（坏 P/取消）——跳到下一 I */
} topos_gop_ref_state;

typedef struct topos_gop_frame_info {
    uint32_t struct_size;    /* = sizeof(topos_gop_frame_info) */
    uint32_t abi_version;    /* = TOPOS_CODEC_ABI_VERSION */
    uint8_t  frame_type;     /* 0=I / 1=P（帧头原值） */
    uint8_t  ref_distance;   /* 帧头原值 */
    uint16_t gop_id;         /* 帧头原值 */
    uint32_t sample_index;   /* observe/feed 序号（0 起） */
    uint32_t ref_state;      /* topos_gop_ref_state——本帧提交后的参考状态 */
    uint32_t reserved[3];    /* 全 0 */
} topos_gop_frame_info;

typedef struct topos_gop_context topos_gop_context; /* opaque */

/* cfg 提供几何/像素格式域（V9 P 残差路径与参考槽按此分配；批 2 启用像素
 * 槽）。非 V9 cfg（reserved[0]!=10）→ TC_ERR_INVALID_ARGUMENT。 */
int32_t tc_gop_context_create(const topos_frame_config* cfg, topos_gop_context** out);
void tc_gop_context_close(topos_gop_context* ctx);

/* 序列观察：包扫描（V8 同构）+ GOP 序列状态机——零像素重建（批 1 验收
 * 口径：完整 scan/query GOP）。TC_OK：info 填充且状态已提交；扫描失败
 * 原样透传且状态 REF_INVALID；序列非法 → TC_ERR_MALFORMED。info 可 NULL。 */
int32_t tc_gop_context_observe(topos_gop_context* ctx, const uint8_t* data,
                               size_t size, topos_gop_frame_info* info);

/* 像素 feed（解码侧）：批 2 接线（zero-motion P 残差解码 + recon 链安装）。
 * out 语义同 tc_frame_decode。解码失败或 conceal 帧（concealed_slices>0）
 * → 参考不安装 + REF_INVALID + 负错误码（§3.8）。 */
int32_t tc_gop_context_feed(topos_gop_context* ctx, const uint8_t* data, size_t size,
                            const topos_plane_view out[TC_FRAME_MAX_PLANES],
                            topos_gop_frame_info* info);

/* 像素编码帧（编码侧）：批 2 接线（I/P 决策 + 锚 qp 残差编码 + 双编码
 * 择优回退 + 事务参考安装）。
 * force_intra=1 → 本帧强制 I（场景切换/回退）。out_cap 不足 →
 * TC_ERR_BUFFER_TOO_SMALL + *need_size（全量不提交，含参考；重试确定）。
 * 编码器策略（ADR-C049，不影响解码端可接受的序列）：
 *  - P 白编预检：残差 MAD ≥ 1<<(bit_depth−6) → 跳过 P 尝试直编 I
 *    （判定产物与 P≥I 回退逐字节相同：直编 I ≡ 回退 I）；
 *  - 全零残差 P：recon = ref 精确成立（X≡0 ⇒ X′≡0），跳过自解码。 */
int32_t tc_gop_context_encode_frame(topos_gop_context* ctx,
                                    const topos_frame_input* input,
                                    uint8_t* out, size_t out_cap, size_t* need_size,
                                    int force_intra, topos_frame_stats* stats);

/* 事务回滚：回到最近一次成功提交的状态（取消/出错后调用——不留参考）。
 * reset_to_i=1 → 额外回到 NO_REF（seek/换 GOP）。 */
void tc_gop_context_abort(topos_gop_context* ctx, int reset_to_i);

/* 当前参考状态查询（out_state 可 NULL 才允许 ctx==NULL 外的快捷检查）。 */
int32_t tc_gop_context_state(topos_gop_context* ctx, uint32_t* out_state,
                             uint16_t* out_gop_id);

/* ===================== MOV 容器（container_spec_v1.md） ===================== */

/* 读写回调式字节源/汇（库不做 I/O；fuzz 以内存实现）。
 * read：从绝对 offset 读恰好 len 字节到 buf，全量成功 TC_OK，短读/失败 TC_ERR_IO。
 * write：追加写（流末尾），失败 TC_ERR_IO。
 * seek_write：回到绝对 offset 覆写 len 字节（mux 回填 mdat 长度需要）。 */
typedef int32_t (*tc_io_read_fn)(void* ctx, uint64_t offset, void* buf, size_t len);
typedef int32_t (*tc_io_write_fn)(void* ctx, const void* data, size_t len);
typedef int32_t (*tc_io_seek_write_fn)(void* ctx, uint64_t offset, const void* data,
                                       size_t len);

typedef struct topos_io {
    uint32_t struct_size;
    uint32_t abi_version;
    void* ctx;
    tc_io_read_fn read;        /* reader 必需 */
    tc_io_write_fn write;      /* mux sink 必需 */
    tc_io_seek_write_fn seek_write; /* mux sink 必需（回填 mdat 64 位长度） */
    uint64_t length;           /* reader: 数据源总长（mux sink 忽略） */
    uint32_t reserved[4];
} topos_io;

#define TC_MOVIE_MAX_SAMPLES 67108864u /* 2^26；恶意采样表分配上限（§7.7） */

/* 电影级配置（tpcC 载荷 + sample entry 元数据的来源；帧字段逐帧以 packet 为权威）。 */
typedef struct topos_movie_config {
    uint32_t struct_size;
    uint32_t abi_version;
    uint16_t visible_width;
    uint16_t visible_height;
    uint8_t profile;
    uint8_t pixel_format;
    uint8_t bit_depth;
    uint8_t qmatrix_id;
    uint8_t qp_base;
    int8_t qp_delta_luma;
    int8_t qp_delta_chroma;
    uint8_t alpha_mode;
    uint8_t alpha_bit_depth;
    uint8_t alpha_premultiplied;
    uint8_t color_range;
    uint8_t color_primaries;
    uint8_t color_transfer;
    uint8_t color_matrix;
    uint8_t chroma_siting;
    uint16_t sar_num;
    uint16_t sar_den;
    uint32_t timescale;        /* 0 → 24000 之外的默认见实现；建议调用方显式给出 */
    /* reserved[0..4] 复用为音频轨声明（container_spec v1.1；见 TC_AUDIO_* 槽位
     * 常量）。全 0 = 无音轨——未显式声明音频的既有调用方行为逐字节不变
     * （sizeof 稳定，无 ABI 变更）。reserved[5] = 样本格式标志（v1.4；
     * TC_AUDIO_FMT_*，0=整数）。reserved[6..7] 保持 0。 */
    uint32_t reserved[8];
} topos_movie_config;

/* —— v1.1 音频轨（movie config reserved 槽位语义）—— */
#define TC_AUDIO_CODEC_NONE 0u   /* 无音轨（默认；槽位全 0） */
#define TC_AUDIO_CODEC_LPCM 1u   /* 'lpcm'（QuickTime 声样描述 = WAV 等价物，无损） */
#define TC_AUDIO_CODEC_MP4A 2u   /* 'mp4a'（AAC 包原样存储；编码在应用层完成） */
#define TC_AUDIO_LAYOUT_MONO   3u
#define TC_AUDIO_LAYOUT_STEREO 0u
#define TC_AUDIO_LAYOUT_5_1    1u
#define TC_AUDIO_LAYOUT_7_1    2u
/* chan atom 的 QuickTime 布局 tag（kAudioChannelLayoutTag_*：高 16 位布局
 * 标签、低 16 位声道数），mux 写入 lpcm/mp4a 声样描述的 chan 子原子 */
#define TC_AUDIO_CHAN_TAG_MONO   0x00640001u  /* (100<<16)|1，ffmpeg MOV_CH_LAYOUT_MONO 同值 */
#define TC_AUDIO_CHAN_TAG_STEREO 0x00650002u
#define TC_AUDIO_CHAN_TAG_5_1_A  0x008C0006u  /* kAudioChannelLayoutTag_MPEG_5_1_A */
#define TC_AUDIO_CHAN_TAG_7_1_A  0x008E0008u  /* kAudioChannelLayoutTag_MPEG_7_1_A */
/* 槽位下标（topos_movie_config.reserved[*]） */
#define TC_AUDIO_SLOT_CODEC      0u
#define TC_AUDIO_SLOT_RATE       1u
#define TC_AUDIO_SLOT_CHANNELS   2u
#define TC_AUDIO_SLOT_LAYOUT     3u
#define TC_AUDIO_SLOT_BITS       4u
/* v1.4：reserved[5] = 样本格式标志（位掩码）。0 = 有符号整数（旧调用方
 * 零值语义与旧文件读取行为逐字节不变）；bit0 = float32（仅 lpcm+32bit） */
#define TC_AUDIO_SLOT_FORMAT     5u
#define TC_AUDIO_FMT_INT         0u
#define TC_AUDIO_FMT_FLOAT32     (1u << 0)

typedef struct topos_movie_info {
    uint32_t struct_size;
    uint32_t abi_version;
    uint16_t visible_width;
    uint16_t visible_height;
    uint8_t profile;
    uint8_t pixel_format;
    uint8_t bit_depth;
    uint8_t qmatrix_id;
    uint8_t qp_base;
    int8_t qp_delta_luma;
    int8_t qp_delta_chroma;
    uint8_t alpha_mode;
    uint8_t alpha_bit_depth;
    uint8_t alpha_premultiplied;
    uint8_t color_range;
    uint8_t color_primaries;
    uint8_t color_transfer;
    uint8_t color_matrix;
    uint8_t chroma_siting;
    uint16_t sar_num;
    uint16_t sar_den;
    uint32_t timescale;
    uint32_t sample_count;     /* 总帧数 */
    uint32_t faststart;        /* 1 = moov 在 mdat 前 */
    uint32_t index_bytes;      /* 索引实际占用（可测量指标，spec §7） */
    /* reserved[0] 兼容性回读：tpcC 中的轨级 bitstream major（V1..V6）。
     * v1.7：reserved[1] = tpcD 档位（TC_TIER_*；无 tpcD 或未声明档位 = 0）。
     * 其余 reserved 保持为 0；复用保留槽不改变 info 的 ABI sizeof。 */
    uint32_t reserved[8];
} topos_movie_info;

/* Alpha 预算元数据（R3，spec §11.3 闭环；写入 tpcB atom——container_spec v1.1）。
 * 比例单位为基点 bp（×10000）：0.25 → 2500。仅 alpha_mode != 0 的电影可携带。 */
#define TC_ALPHA_BUDGET_RATIO_UNSET 0xFFFFu /* target 未声明（无档位定标时） */
#define TC_ALPHA_BUDGET_FLAG_OVERRUN 0x0001u    /* 超 hard cap 交付（记录/授权） */
#define TC_ALPHA_BUDGET_FLAG_AUTHORIZED 0x0002u /* 超限交付经调用方显式授权 */
#define TC_ALPHA_BUDGET_FLAG_ADAPTED 0x0004u    /* 位深经 12→10→8 自适应选定 */
/* 复验 P1-11（container_spec v1.3）：actual_ratio_bp 钳到 65535bp 时置位
 * ——真实比例 ≥6.5535 与恰好 6.5535 可区分；旧 reader 按未知位忽略。 */
#define TC_ALPHA_BUDGET_FLAG_RATIO_SATURATED 0x0008u
typedef struct topos_alpha_budget_info {
    uint32_t struct_size;
    uint32_t abi_version;
    uint16_t target_ratio_bp;  /* 目标比例（计划 §2.2；0xFFFF = 未声明） */
    uint16_t actual_ratio_bp;  /* 文件级实际比例（累计 payload 比例） */
    uint16_t max_abs_error;    /* 跨帧最大 |源−近似|（16-bit 域；mode1 恒 0） */
    uint16_t flags;            /* TC_ALPHA_BUDGET_FLAG_* */
    uint32_t frame_count;      /* 统计覆盖帧数（读侧强制 == sample_count） */
    uint32_t reserved[4];
} topos_alpha_budget_info;

typedef struct topos_mux topos_mux;     /* 不透明；单实例不可重入 */
typedef struct topos_movie topos_movie; /* 不透明；只读共享安全 */

/* —— v1.7 电影元数据（写入 tpcD atom；container_spec v1.7 §4）——
 * 导出档位 + 厂商标识，供读侧恢复精确档位展示（如 "Topos 422 HQ"）与
 * 外部工具自描述。声明走 tc_mux_set_movie_meta（仿 tc_mux_set_alpha_budget
 * 的 setter 模式——不扩 topos_movie_config，零 ABI 变更）；未声明则不写
 * tpcD，既有调用方产物逐字节不变。读侧：tier_id 经 tc_movie_info 的
 * reserved[1] 回读（0 = 无 tpcD / 未声明档位）；vendor/label 串仅落盘供
 * 外部工具，应用层经 tier_id → 本地档位表取展示名。 */
#define TC_TIER_NONE     0u   /* 未声明档位（旧文件 / 仅 vendor 的 tpcD） */
#define TC_TIER_PROXY    1u   /* Topos 422 Proxy（profile 3 质量预设） */
#define TC_TIER_LT       2u   /* Topos 422 LT（profile 3 质量预设） */
#define TC_TIER_STANDARD 3u   /* Topos 422（profile 3 基准档） */
#define TC_TIER_HQ       4u   /* Topos 422 HQ（profile 3 质量预设） */
#define TC_TIER_4444     5u   /* Topos 4444（独立 profile 5） */
#define TC_TIER_4444XQ   6u   /* Topos 4444 XQ（独立 profile 6） */
#define TC_TIER_LP       7u   /* Topos 422 LP（帧间微 GOP，profile 3 载体
                               * V7-R3；v1.8 产品化，ADR-C056。唯一非帧内
                               * 档——tier 只描述质量/档位语义，帧间性由
                               * 包头 major/熵域自描述，读侧不依赖 tier 判断） */
#define TC_TIER_RAW      8u   /* Topos RAW（视频 RAW 产品档，M4-R5；单 tier +
                               * 位深×比率参数化（D3 拍板），帧 profile 7 +
                               * pf=3 CFA；规格名 'Topos RAW <bd>-bit <N>:1'，
                               * 12 档共享图片线 qp 锚点；v1.9 tpcD 域扩展
                               * 0..7 → 0..8，纯域扩展，既有 golden 逐字节
                               * 不变——LP 先例 ADR-C056） */
typedef struct topos_movie_meta {
    uint32_t struct_size;
    uint32_t abi_version;
    uint8_t tier_id;        /* TC_TIER_*；0 = 未声明（仅 vendor/label 的 tpcD） */
    uint8_t reserved[3];    /* 必须 0 */
    char vendor[16];        /* NUL 收尾 UTF-8（≤15 字节，如 "TOPOS"）；空 = 不写 vendor 串 */
    char label[64];         /* NUL 收尾 UTF-8（≤63 字节，如 "Topos 422 HQ"）；空 = 不写 label 串 */
} topos_movie_meta;

/* RC1（M6）：TRAW 开发元数据（TRWM）。载荷对 native 不透明（v1 固定
 * 40B 含 CRC32，结构见 python topos_trwm.py）；stsd TPIC entry 以
 * 'trwm' 子原子原样承载（tpcD 同模式，旧 reader 按未知子原子跳过）。 */
#define TC_RAW_META_MAX 256u
typedef struct topos_raw_meta {
    uint32_t struct_size;   /* sizeof(topos_raw_meta) */
    uint32_t abi_version;   /* TOPOS_CODEC_ABI_VERSION */
    uint16_t payload_size;  /* 有效载荷字节数（1..TC_RAW_META_MAX） */
    uint16_t reserved;      /* 必须 0 */
    uint8_t payload[TC_RAW_META_MAX];
} topos_raw_meta;

/* mux：流式写出（标准布局 ftyp+mdat+moov；sink 需 write+seek_write）。
 * add_packet 的 packet 必须是合法 TPIC packet（magic+CRC 快速校验）。
 * finish 后 mux 仍需 free（错误路径同样）。确定性：同输入 → 逐字节相同输出。 */
int32_t tc_mux_create(const topos_movie_config* cfg, const topos_io* sink,
                      topos_mux** out);
int32_t tc_mux_add_packet(topos_mux* mux, const uint8_t* packet, size_t size,
                          uint64_t pts_tick, uint32_t dur_tick);
/* 记录 alpha 预算元数据（finish 前调用；可选——不调用则不写 tpcB）。
 * 仅 alpha_mode != 0 电影接受；比例 ≤ 10000bp（target 例外可 0xFFFF）。
 * 一致性：atom 内 alpha_mode/bit_depth 复制自电影配置，frame_count 读侧
 * 强制等于 sample 数（tpcC 式一致性规则）。 */
int32_t tc_mux_set_alpha_budget(topos_mux* mux, const topos_alpha_budget_info* info);
/* v1.7：记录电影元数据（finish 前调用；可选——不调用则不写 tpcD；
 * 重复调用以最后一次为准）。tier_id ∈ TC_TIER_*；vendor/label 为 NUL
 * 收尾 UTF-8（≤15/63 字节；空串 = 该串不写入）。 */
int32_t tc_mux_set_movie_meta(topos_mux* mux, const topos_movie_meta* meta);
/* RC1：记录 TRAW 开发元数据（finish 前调用；可选——不调用则不写 trwm
 * 原子；重复调用以最后一次为准）。载荷不透明原样承载。 */
int32_t tc_mux_set_raw_meta(topos_mux* mux, const topos_raw_meta* meta);
/* v1.1 音频轨：追加一个音频"样本"（chunk）到 mdat（finish 前可多次调用，
 * 与 tc_mux_add_packet 任意交错——各轨偏移独立记账，视频 chunk 偏移不受
 * 影响）。cfg 未声明音频（audio codec = NONE）→ TC_ERR_STATE。
 * lpcm：size 必须 == num_samples × channel_count × bits/8（逐调用精确）；
 * mp4a：一次调用 = 一个 AAC 包，num_samples 为其覆盖的采样帧数（通常
 * 1024），须先 tc_mux_set_audio_asc。数据按调用顺序原样拷贝（大端 PCM，
 * 'twos' 语义）。 */
int32_t tc_mux_add_audio(topos_mux* mux, const uint8_t* data, size_t size,
                         uint32_t num_samples);
/* v1.1 AAC 档：声明 AudioSpecificConfig 字节（应用层编码器 extradata 原样
 * 存入 esds）。仅 codec = MP4A 接受；首个 add_audio 前调用恰好一次。 */
int32_t tc_mux_set_audio_asc(topos_mux* mux, const uint8_t* asc, size_t size);
/* v1.5：tmcd 时间码轨（plan §2.7；finish 前至多一次）。
 * fps 为标称帧率，白名单 {24,25,30,48,50,60}；NTSC 分数帧率
 * （23.976/29.97/59.94）经 nominal 24/30/60 + drop_frame 表达。
 * DF 仅标称 30/60 合法；负时码不支持（flags bit2 恒 0）。
 * 写入：tmcd trak（恒末轨）+ 视频 trak tref 引用 + mdat 尾 4B 帧计数。 */
int32_t tc_mux_set_timecode(topos_mux* mux, uint32_t hh, uint32_t mm,
                            uint32_t ss, uint32_t ff, uint32_t fps,
                            uint32_t drop_frame);
/* v1.4：声明音频 AAC priming（编码器启动延迟样本数；写为音频 trak 的
 * edts/elst media_time——解码侧据此裁剪，使端到端对齐）。
 * finish 前至多一次；仅 mp4a 声明下合法（lpcm/NONE → INVALID_ARGUMENT）；
 * samples 须 > 0 且 < 已写入音频总采样数。取值必须来自端到端校准
 * （ADR-C052）——编码器自报值 ≠ 解码侧管线延迟后的实际偏移。 */
int32_t tc_mux_set_audio_priming(topos_mux* mux, uint32_t samples);
int32_t tc_mux_finish(topos_mux* mux);
void tc_mux_free(topos_mux* mux);

/* FastStart 后处理（qt-faststart 语义）：读 src（标准布局），产出 ftyp+moov+mdat
 * 到 dst；偏移重定位 + 必要时 stco→co64 升级。src 只需 read；dst 需 write。 */
int32_t tc_movie_faststart(const topos_io* src, const topos_io* dst);

/* reader：open 只解析 moov 建索引（不读 sample 数据）。info 可 NULL。 */
int32_t tc_movie_open(const topos_io* io, topos_movie** out);
/* P1-11（v2 追加）：fd 直读 open——native pread 绕过宿主 read 回调，消除
 * 逐包跨语言拷贝（Python 侧回调仅作回退）。fd 须以只读打开且 O_RDONLY；
 * lib 不 close、不 dup，fd 生命周期须覆盖 movie 的整个使用期；movie 打开
 * 期间并发读安全（同回调路径，纯 pread）。length = 文件字节数（调用方
 * fstat）。语义与其余 tc_movie_* 完全一致。 */
int32_t tc_movie_open_fd(int fd, uint64_t length, topos_movie** out);
int32_t tc_movie_info(const topos_movie* movie, topos_movie_info* info);
/* 读回 alpha 预算元数据；文件未携带 tpcB → TC_ERR_STATE（last_error 说明）。 */
int32_t tc_movie_alpha_budget(const topos_movie* movie, topos_alpha_budget_info* info);
/* v1.1 音频轨信息（demux 独立查询，不并入 movie info）。文件无音轨 →
 * codec = 0 且 sample_count = 0，返回 TC_OK（不报错）。 */
typedef struct topos_audio_track_info {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t codec;            /* TC_AUDIO_CODEC_*；0 = 无音轨 */
    uint32_t sample_rate;
    uint32_t channel_count;
    uint32_t channel_layout;   /* TC_AUDIO_LAYOUT_* */
    uint32_t bits_per_sample;  /* lpcm：16/24/32；mp4a：0 */
    uint32_t sample_count;     /* 总采样帧数（无音轨恒 0） */
    uint32_t chunk_count;      /* 容器音频"样本"（chunk）数 */
    /* reserved[0] = priming_samples（v1.4；音频 edts/elst media_time 读回，
     * 0 = 无 elst/旧文件——应用层据此自行跳过；语义在此冻结）
     * reserved[1] = sample_format（v1.4；TC_AUDIO_FMT_*：0=有符号整数、
     * 1=float32——由 lpcm formatFlags float 位推导；语义在此冻结） */
    uint32_t reserved[6];
    /* v1.6：轨名（trak/udta/©nam 读出；UTF-8，恒 NUL 收尾，无轨名空串） */
    char name[64];
} topos_audio_track_info;
int32_t tc_movie_audio_info(const topos_movie* movie, topos_audio_track_info* info);
/* ===== v1.6 多音轨（M-B8 / ADR-C054）=====
 * 轨序钉死（ADR-C056 同源规则）：video=1、audio=2..N+1、tmcd=N+2（恒末轨）。
 * v1.1 单轨族 API（tc_mux_add_audio / set_audio_asc / set_audio_priming、
 * tc_movie_audio_info / tc_movie_read_audio）语义不变 = 轨 0 视图；
 * v1.1 文件（单音轨）经新 API 读出与旧 API 完全一致。 */
#define TC_AUDIO_MAX_TRACKS 16u
/* 逐轨声明（finish 前；不占 movie_config reserved 槽位）。reserved 槽位
 * 已声明轨 0 时仍可追加轨 1..N-1；格式规则与 reserved 槽位一致（fail-fast）：
 * codec 白名单、采样率家族、声道/布局配对、float32 仅 lpcm+32bit、
 * mp4a 恒 bits=0/fmt=INT。轨名 ≤63 字节 UTF-8（NULL/空 = 不写轨名）。 */
typedef struct topos_audio_track_config {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t codec;            /* TC_AUDIO_CODEC_* */
    uint32_t sample_rate;
    uint32_t channel_count;
    uint32_t channel_layout;   /* TC_AUDIO_LAYOUT_* */
    uint32_t bits_per_sample;  /* lpcm：16/24/32；mp4a：0 */
    uint32_t sample_format;    /* TC_AUDIO_FMT_* */
    const char* name;          /* 轨名（©nam + hdlr 名；可 NULL） */
    uint32_t reserved[4];
} topos_audio_track_config;
/* 声明一条新音轨（追加；超 TC_AUDIO_MAX_TRACKS → LIMIT_EXCEEDED）。
 * 返回后即可对该轨 index（= 声明序）set_audio_track_asc/add_audio_to。 */
int32_t tc_mux_add_audio_track(topos_mux* mux, const topos_audio_track_config* cfg);
int32_t tc_mux_set_audio_track_asc(topos_mux* mux, uint32_t track,
                                   const uint8_t* asc, size_t size);
int32_t tc_mux_set_audio_track_priming(topos_mux* mux, uint32_t track,
                                       uint32_t samples);
int32_t tc_mux_add_audio_to(topos_mux* mux, uint32_t track,
                            const uint8_t* data, size_t size,
                            uint32_t num_samples);
/* demux：音轨数（无音轨 → 0，TC_OK）。 */
int32_t tc_movie_audio_track_count(const topos_movie* movie, uint32_t* out_count);
/* 按轨索引查询/读取（track ≥ 音轨数 → INVALID_ARGUMENT）；旧 API 恒轨 0。 */
int32_t tc_movie_audio_info_at(const topos_movie* movie, uint32_t track,
                               topos_audio_track_info* info);
int32_t tc_movie_read_audio_at(const topos_movie* movie, uint32_t track,
                               uint64_t start_chunk, uint32_t chunk_count,
                               uint8_t* buf, size_t cap, size_t* need_size);
/* v1.5 时间码轨读回（plan §2.7）。文件无 tmcd trak → TC_ERR_STATE。
 * hh/mm/ss/ff 由 start_frame_count 按 fps/DF 规则换算（C 侧完成）。
 * DF 换算规则：每分钟（非 10 整倍）跳 frame 0/1、每 10 分钟整不跳。 */
typedef struct topos_timecode_info {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t start_frame_count;  /* 时码原点起算的帧计数（tmcd 样本原值） */
    uint32_t fps;                /* 标称 fps（白名单值） */
    uint32_t drop_frame;         /* 0/1（tmcd flags bit0） */
    uint32_t flags;              /* tmcd stsd flags 原样 */
    int32_t  hh, mm, ss, ff;     /* 起始时码（换算完成） */
} topos_timecode_info;
int32_t tc_movie_timecode(const topos_movie* movie, topos_timecode_info* out);
/* RC1：读回 TRAW 开发元数据（无 trwm 原子 → TC_ERR_STATE）。 */
int32_t tc_movie_read_raw_meta(const topos_movie* movie, topos_raw_meta* out);
/* v1.1 连续读取音频 chunk 区间 [start_chunk, start_chunk+chunk_count) 的
 * 原始字节（lpcm = 采样帧字节流；mp4a = 包流拼接）。探测语义与
 * tc_movie_packet 一致：buf == NULL 或 cap 不足 → TC_ERR_BUFFER_TOO_SMALL
 * 且 *need_size 给出精确总字节数；成功返回 TC_OK。文件无音轨 → TC_ERR_STATE。 */
int32_t tc_movie_read_audio(const topos_movie* movie, uint64_t start_chunk,
                            uint32_t chunk_count, uint8_t* buf, size_t cap,
                            size_t* need_size);
/* O(1) 取第 i 个 sample：cap 不足 → TC_ERR_BUFFER_TOO_SMALL 且 *need_size 给出
 * 精确值（cap 可为 0 仅探测）；成功 TC_OK。 */
int32_t tc_movie_packet(const topos_movie* movie, uint32_t sample_index,
                        uint8_t* buf, size_t cap, size_t* need_size);
/* 读取预览所需的最小独立 packet：V7-B 只读取 header/directory/base
 * byte range，并返回可直接交给 decoder 的 embedded base packet；V1–V7-A
 * 兼容回退为完整 sample。cap 不足/仅探测时返回 BUFFER_TOO_SMALL，
 * *need_size 为返回 packet 的精确字节数。 */
int32_t tc_movie_packet_base(const topos_movie* movie, uint32_t sample_index,
                             uint8_t* buf, size_t cap, size_t* need_size);
/* 批量读取预览 packet：逐 sample 返回 V7-B embedded base，旧流回退完整
 * sample。offsets/sizes/base_flags 可为 NULL；base_flags[i] = 1 表示该槽
 * 是 V7-B base。探测/容量不足返回 BUFFER_TOO_SMALL 并填写 need_total。 */
int32_t tc_movie_packet_base_batch(const topos_movie* movie, uint32_t start,
                                   uint32_t count, uint8_t* arena, size_t cap,
                                   size_t* offsets_out, size_t* sizes_out,
                                   uint8_t* base_flags_out, size_t* need_total);
/* 批量读连续 sample 区间 [start, start+count) 进调用方 arena（读前批量预取）：
 * 各 sample 在 arena 内紧凑排列，offsets_out/sizes_out 给出布局（可为 NULL）。
 * 区间在文件内逐包连续（本库写入器恒连续）→ 合并为单次 io 读；非连续
 * （外来交错布局）逐包读入。探测语义同上：arena==NULL 或 cap 不足 →
 * TC_ERR_BUFFER_TOO_SMALL 且 *need_total 给出精确总字节数；count==0 → TC_OK
 * （不触碰其余指针）；区间越界 → TC_ERR_INVALID_ARGUMENT；区间越过文件尾 →
 * TC_ERR_MALFORMED（不读任何字节）；io 失败原样返回（arena 内容未定义）。 */
int32_t tc_movie_packet_batch(const topos_movie* movie, uint32_t start,
                              uint32_t count, uint8_t* arena, size_t cap,
                              size_t* offsets_out, size_t* sizes_out,
                              size_t* need_total);
int32_t tc_movie_packet_pts(const topos_movie* movie, uint32_t sample_index,
                            uint64_t* pts_tick, uint32_t* dur_tick);
/* 同步点查询（stss 缺失 = 全同步；用于 seek 定位）。 */
int32_t tc_movie_packet_sync(const topos_movie* movie, uint32_t sample_index,
                             uint8_t* is_sync);
/* V9（micro-gop 计划批 3）：≤ sample_index 的最近同步 sample（seek 定位：
 * 找前一 I → reset context → 顺序解到目标）。命中同步返回自身；无 stss
 * 的旧文件（默认全同步）返回自身；index 前无任何同步 → TC_ERR_STATE。 */
int32_t tc_movie_prev_sync(const topos_movie* movie, uint32_t sample_index,
                           uint32_t* sync_index);
void tc_movie_close(topos_movie* movie);

/* ———— P1-12 辅助输入转换（host 侧便利 API；非码流语义，可独立于编码使用）————
 * packed RGB(A)（u8 或 u16 小端）→ YUV/GBR 平面（小端 16-bit 容器，值域
 * 0..2^bit_depth−1）。数学与 Python 参考实现（ToposVideoEncoder）**逐位一致**：
 *   - 全程 float32（弱标量先舍入到 float32 再参与运算，同 numpy NEP 50）；
 *   - 系数/常数由 double 表达式一次舍入为 float；
 *   - np.rint 半到偶 = C rintf（默认舍入模式，本 TU 不改 fenv）；
 *   - 运算顺序与 numpy 逐算子对应，且本 TU 以 -ffp-contract=off 编译
 *     （禁 FMA 收缩）——任何一侧改动须重跑逐位一致测试；
 *   - 4:2:2 = 对**已量化整型**色度做水平 box（奇宽边缘补列），与参考实现
 *     的「先量化成平面、再子采样」两段式完全同序。
 * 线程：max_workers>1 时用内部常驻池按行分片（输出与单线程逐位一致；
 * 编码线程内并发调用安全——见头部线程契约）。 */
typedef enum topos_cvt_format {
    TOPOS_CVT_BGR24  = 0, /* u8    ×3：B,G,R        */
    TOPOS_CVT_RGB24  = 1, /* u8    ×3：R,G,B        */
    TOPOS_CVT_BGRA32 = 2, /* u8    ×4：B,G,R,A      */
    TOPOS_CVT_RGBA32 = 3, /* u8    ×4：R,G,B,A      */
    TOPOS_CVT_BGR48  = 4, /* u16LE ×3：B,G,R        */
    TOPOS_CVT_RGB48  = 5, /* u16LE ×3：R,G,B        */
    TOPOS_CVT_BGRA64 = 6, /* u16LE ×4：B,G,R,A      */
    TOPOS_CVT_RGBA64 = 7  /* u16LE ×4：R,G,B,A      */
} topos_cvt_format;

#define TOPOS_CVT_OUT_YUV422 0u /* 水平 box 子采样（产品 packed 路径） */
#define TOPOS_CVT_OUT_YUV444 1u /* 全宽色度                            */
#define TOPOS_CVT_OUT_GBR    2u /* G,B,R 直通量化（无矩阵折算）        */

typedef struct topos_cvt_params {
    uint32_t struct_size;   /* = sizeof(topos_cvt_params)，否则 INVALID_ARGUMENT */
    uint32_t format;        /* topos_cvt_format                                  */
    uint32_t width;         /* ≥1                                                */
    uint32_t height;        /* ≥1                                                */
    uint32_t out_mode;      /* TOPOS_CVT_OUT_*                                   */
    uint32_t bit_depth;     /* 10/12（输出满刻度域）                             */
    double   kr;            /* 前向矩阵红系数（out_mode=GBR 忽略）。double 承载：
                             * numpy 参考实现的派生常数（(1−kr−kb)、2(1−kr)）以
                             * float64 全精度求值后一次舍入为 float——若在此传
                             * float，派生常数差 1 ulp，x.5 边界 rint 翻转 */
    double   kb;            /* 前向矩阵蓝系数（同上） */
    uint32_t full_range;    /* 1=full / 0=limited（2^(n−8) 精确位移）            */
    uint32_t alpha_shift;   /* alpha 左移 = 16−声明位深；a_out==NULL 时忽略      */
    uint32_t alpha_opaque;  /* 1=忽略输入 alpha，写 a_nmax<<shift（输入 3 通道） */
    const void* src;        /* packed 像素，H×W×N（紧密或 src_stride）           */
    uint32_t src_stride;    /* 字节/行；0 = width × bytes_per_pixel              */
    uint16_t* y_out;        /* height×width                                      */
    uint32_t y_stride;      /* 元素/行；0 = width（YUV）/ width（GBR 同布局）    */
    uint16_t* u_out;        /* YUV：height×ceil(w/2)（422）或 height×width（444）；
                             * GBR：G 平面（height×width）                       */
    uint32_t u_stride;
    uint16_t* v_out;        /* YUV：V 平面；GBR：B 平面                          */
    uint32_t v_stride;
    uint16_t* a_out;        /* alpha 平面（height×width）；NULL=不输出           */
    uint32_t a_stride;
    uint32_t max_workers;   /* 0/1=单线程；>1 内部线程池行分片                   */
} topos_cvt_params;

/* 单次融合转换。错误：INVALID_ARGUMENT（参数/指针/越界）、LIMIT_EXCEEDED
 * （尺寸超限）、STATE（线程池不可用）。成功 TC_OK；不分配堆内存
 * （行分片任务栈上）。 */
int32_t tc_convert_packed_rgb(const topos_cvt_params* params);

/* 4:4:4 planar → 4:2:2 planar 的 host 侧辅助转换。
 *
 * 输入/输出均为 uint16 满刻度平面；U/V 对相邻横向样本做
 * ``(a + b + 1) >> 1``，奇数宽度复制边缘样本，和 Python 直通导出
 * 的 numpy 参考路径逐位一致。Y 平面无需转换，由调用方直接复用。
 * stride 单位为 uint16 元素，0 表示紧排；max_workers>1 时按行分片。
 * 该 API 只做平面重排，不改变位深或码值语义。 */
typedef struct topos_planar_444_to_422_params {
    uint32_t struct_size;   /* = sizeof(topos_planar_444_to_422_params) */
    uint32_t width;         /* 输入 4:4:4 宽度 */
    uint32_t height;        /* 输入/输出高度 */
    uint32_t bit_depth;     /* 10/12/16，仅用于参数校验 */
    const uint16_t* u_in;   /* 输入 U，height×width */
    size_t u_in_stride;     /* uint16 元素/行；0=width */
    const uint16_t* v_in;   /* 输入 V，height×width */
    size_t v_in_stride;     /* uint16 元素/行；0=width */
    uint16_t* u_out;        /* 输出 U，height×ceil(width/2) */
    size_t u_out_stride;    /* uint16 元素/行；0=ceil(width/2) */
    uint16_t* v_out;        /* 输出 V，height×ceil(width/2) */
    size_t v_out_stride;    /* uint16 元素/行；0=ceil(width/2) */
    uint32_t max_workers;   /* 0/1=单线程；>1 内部线程池行分片 */
} topos_planar_444_to_422_params;

/* 不分配堆内存；输入和输出缓冲不得重叠。 */
int32_t tc_convert_yuv444_to_422(
    const topos_planar_444_to_422_params* params);

#ifdef __cplusplus
}
#endif

#endif /* TOPOS_CODEC_H */
