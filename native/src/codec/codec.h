/* 完整标量 codec 核心（阶段 4）——内部头。
 *
 * 公共 ABI（topos_frame_config / tc_frame_encode / tc_frame_decode 等）见
 * include/topos_codec.h；本头只暴露内部共享件：
 *  - MED 三邻域预测（Alpha 路径，spec §8.2；编码/解码共用保证闭环一致）
 *  - dev 量化表覆盖钩子（topos_quality 调优 sweep 专用；非公共 ABI，
 *    设置后影响本进程内所有编码，仅允许单线程测试工具使用）
 */
#ifndef TOPOS_INTERNAL_CODEC_H
#define TOPOS_INTERNAL_CODEC_H

#include <stdint.h>

#include "../bitstream/packet.h"
#include "../common/tpool.h"
#include "../transform/quant.h"

static inline int32_t tc_med_predict(int32_t a, int32_t b, int32_t c)
{
    if (c >= a && c >= b) { return a < b ? a : b; } /* c ≥ max(a,b) → min */
    if (c <= a && c <= b) { return a < b ? b : a; } /* c ≤ min(a,b) → max */
    return a + b - c;
}

/* ---- M2：解码内部共享件（tc_frame_decode 与 tc_decoder context 共用） ---- */

/* alpha 行缓冲池（decoder context 持有，跨 slice/跨帧 grow-only 复用）。
 * 动态领取分发下任务序号与执行线程无绑定 → 槽位以 tc_pool_worker_slot()
 * （执行线程标识）取，同 worker 槽位串行复用。 */
typedef struct dec_alpha_pool {
    uint16_t* prev[TC_SLICE_MAX_THREADS];
    uint16_t* cur[TC_SLICE_MAX_THREADS];
    uint32_t slots; /* 已分配槽位数（≤ TC_SLICE_MAX_THREADS） */
    uint32_t elems; /* 每缓冲元素数（= alpha coded 宽） */
} dec_alpha_pool;

/* 颜色 slice DC 双行池（M10-1：每 worker 槽一对 prev/cur，64 字节对齐；
 * 容量为当前几何最大 plane_block_cols，几何增大时整体扩容）。 */
typedef struct dec_dc_pool {
    int32_t* prev[TC_SLICE_MAX_THREADS];
    int32_t* cur[TC_SLICE_MAX_THREADS];
    uint32_t slots;
    uint32_t elems; /* 每行元素数（= max plane_block_cols） */
} dec_dc_pool;

/* V3 intra 重建边界池：每个 worker 槽保存 top/bottom 两行的连续 u16
 * 缓冲；DC 行复用上面的 dec_dc_pool。 */
typedef struct dec_intra_pool {
    uint16_t* edges[TC_SLICE_MAX_THREADS];
    uint32_t slots;
    uint32_t elems; /* 每槽元素数（= 2 × 最大颜色 coded 宽） */
} dec_intra_pool;

/* decoder context 的全部 grow-only scratch（M10-1 打包传递）。 */
typedef struct dec_scratch {
    dec_alpha_pool alpha;
    dec_dc_pool dc;
    dec_intra_pool intra;
} dec_scratch;

/* RD1：统一请求解析后的内部执行计划。target_* 是最终输出几何；scaled=1
 * 走目标网格重建，coefficient_limit < 63 时启用低频 reduced sink。 */
typedef struct tc_decode_plan {
    uint32_t mode;
    uint32_t scale;
    uint32_t target_width;
    uint32_t target_height;
    uint8_t scaled;
    uint8_t coefficient_limit;
    uint8_t drop_alpha;
    uint8_t reserved[1];
} tc_decode_plan;

int32_t tc_decode_request_validate(const topos_decode_request* request);
int32_t tc_decode_request_resolve(const topos_frame_header* fh,
                                  const topos_decode_request* request,
                                  tc_decode_plan* plan);

void tc_dec_alpha_pool_init(dec_alpha_pool* pool);
void tc_dec_alpha_pool_free(dec_alpha_pool* pool);
void tc_dec_dc_pool_init(dec_dc_pool* pool);
void tc_dec_dc_pool_free(dec_dc_pool* pool);
void tc_dec_intra_pool_init(dec_intra_pool* pool);
void tc_dec_intra_pool_free(dec_intra_pool* pool);

/* 统一任务队列解码核心（结构已解析的 view）。planes_out[i]（i < plane_count）
 * 为调用方缓冲；strides 元素计（NULL/0 = tight）。scratch 非 NULL 时尝试池化
 * （alpha 行 + 颜色 DC 行；扩容失败的字段自动回退 slice 内临时分配）。
 * 返回 TC_OK / TC_WARN_CONCEALED / 负错误码，语义同 tc_frame_decode 全解码。 */
/* V7-R3（ADR-C048 D4）无状态像素解码入口的 P 包闸（查询模式不调用；
 * GOP context 经 tc_decode_slices 直连不受限）。 */
int32_t tc_v7r3_stateless_p_guard(const topos_frame_header* fh);

int32_t tc_decode_slices(const topos_packet_view* view,
                         uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                         const size_t strides[TC_FRAME_MAX_PLANES],
                         dec_scratch* scratch,
                         topos_frame_output* out_info,
                         uint32_t nw_override);

/* Request-aware variant used by the public decode-request path. */
int32_t tc_decode_slices_ex(const topos_packet_view* view,
                            uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                            const size_t strides[TC_FRAME_MAX_PLANES],
                            dec_scratch* scratch,
                            topos_frame_output* out_info,
                            uint32_t nw_override,
                            uint8_t skip_alpha);

int32_t tc_decode_slices_scaled(const topos_packet_view* view,
                                uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                const size_t strides[TC_FRAME_MAX_PLANES],
                                dec_scratch* scratch,
                                topos_frame_output* out_info,
                                uint32_t nw_override,
                                uint32_t target_width, uint32_t target_height);

int32_t tc_decode_slices_scaled_limit(const topos_packet_view* view,
                                      uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                      const size_t strides[TC_FRAME_MAX_PLANES],
                                      dec_scratch* scratch,
                                      topos_frame_output* out_info,
                                      uint32_t nw_override,
                                      uint32_t target_width, uint32_t target_height,
                                      uint8_t coefficient_limit);

int32_t tc_decode_slices_scaled_limit_ex(
                                      const topos_packet_view* view,
                                      uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                                      const size_t strides[TC_FRAME_MAX_PLANES],
                                      dec_scratch* scratch,
                                      topos_frame_output* out_info,
                                      uint32_t nw_override,
                                      uint32_t target_width, uint32_t target_height,
                                      uint8_t coefficient_limit,
                                      uint8_t skip_alpha);

/* ---- 阶段4：跨帧单批切片队列核心（tc_frame_decode_batch /
 * tc_decoder_decode_batch 共用）----
 * reqs[i].rc 进入时非 0 = 该帧已被调用方拒绝（prep/dispatch/finalize 全
 * 跳过）；离开时 = 该帧解码结果（TC_OK / TC_WARN_CONCEALED / 负错误码）。
 * 返回首个非 OK 帧的返回码（帧序最小）。批内 nw_override 须一致。
 * 全部切片任务进同一线程池批次（一次唤醒/汇合消化 N 帧）；任务总数
 * ≤ TC_MAX_SLICES 走栈（单帧路径稳态零分配不变），超限走堆，堆失败逐帧
 * 回退。批内 slice 输出内存互不相交 → 执行顺序不影响确定性输出（§11.1）。 */
typedef struct dec_frame_req {
    topos_packet_view* view; /* 调用方持有（批量入口为堆数组元素） */
    uint16_t* planes[TC_FRAME_MAX_PLANES];
    size_t strides[TC_FRAME_MAX_PLANES];
    topos_frame_output* info; /* 该帧输出槽（slice_status 写入其内） */
    dec_scratch* scratch;     /* 批内共享（可 NULL） */
    uint32_t nw_override;
    uint32_t task_base; /* 本帧任务在批任务数组中的起始索引 */
    uint32_t target_width;  /* 0 = 源尺寸；非 0 = target-size decode */
    uint32_t target_height;
    uint8_t scaled;
    uint8_t coefficient_limit; /* 扫描序保留上限；63 = 完整系数 */
    uint8_t skip_alpha;        /* 请求 DROP_ALPHA 时不构建 plane 3 任务 */
    uint8_t reserved_scaled[1];
    const tc_qmatrix_set* qms; /* prep 填充 */
    int32_t rc;
} dec_frame_req;

/* P1-17：批量解码 API/产品的帧数上限——超限在触碰调用方数组或大分配
 * 之前以 TC_ERR_LIMIT_EXCEEDED 拒绝（无回绕、无巨量分配尝试）。 */
#define TC_BATCH_MAX_FRAMES 65536u

int32_t tc_decode_frames(dec_frame_req* reqs, uint32_t nframes);

/* RD2 internal reference writer: use existing DCT/quant/token work to emit a
 * V7-A directory packet.  This is not part of the public capability query or
 * the default writer rollout; alpha is intentionally rejected for now. */
int32_t tc_v7_frame_encode(const topos_frame_config* cfg,
                           const topos_frame_input* input,
                           uint8_t* out, size_t out_cap,
                           topos_frame_stats* stats);

/* frame header → 解码输出描述（prepare/查询模式共用）。 */
void tc_fill_output_info(const topos_frame_header* fh, topos_frame_output* info);

/* ---- M0：解码阶段观测（TOPOS_CODEC_PROFILE=1 开启；内部 dev API） ---- */

typedef struct tc_decode_stage_stats {
    uint64_t scan_ns;        /* 结构解析（header/slice map） */
    uint64_t crc_ns;         /* 每 slice payload CRC（worker 内并行） */
    uint64_t entropy_ns;     /* Rice 符号解码（含 sink 外的解码流开销） */
    uint64_t dequant_idct_ns;/* 反量化 + 逆变换 + 重建存储（M10-0.2 起为每 slice
                                1/64 确定性抽样 × 总块数归一的估计值——逐块取
                                时钟扰动过大，不再作为绝对口径） */
    uint64_t output_ns;      /* 直写/crop；M3 后由采样计时估算（非独立阶段） */
    uint64_t alloc_ns;       /* 临时分配（M2 后稳态应趋 0；当前仅保留聚合位） */
    uint32_t blocks;         /* 颜色块总数 */
    uint32_t nonzero_ac;     /* 非零 AC 系数总数 */
    uint32_t dc_only_blocks; /* 仅 DC 非零的块数 */
    uint32_t slices;         /* slice 总数 */
    /* RD0-03：可比较的读/解/算/写计数。当前 V1/V2 仍须顺序消费整个
     * payload，因此 segments_skipped=0 是有意的真实结果；V7 目录跳段后
     * 将在同一字段体现减少的段读取。 */
    uint64_t packet_bytes_read;  /* 实际参与当前调用的 elementary packet 字节 */
    uint64_t segments_parsed;   /* 已解析并提交到 worker 的 slice/segment */
    uint64_t segments_skipped;  /* 未读取的可跳过 segment（V1/V2 恒为 0） */
    uint64_t entropy_symbols;    /* 实际消费的 DC/run/level/alpha pair 符号 */
    uint64_t coefficients_skipped; /* 熵码字已消费但 reduced 未保留的 AC */
    uint64_t idct_samples;       /* 逆变换实际计算的输出样本数 */
    uint64_t upload_bytes;       /* 写入 caller surface 的 uint16 平面字节数 */
    uint64_t frames;             /* 已完成统计的帧数 */
    uint64_t wall_ns;             /* 解码核心墙钟累计（batch 为一次 batch） */
    uint64_t p50_ns;              /* 最近样本的 p50（最多保留 256 次调用） */
    uint64_t p95_ns;
    uint64_t p99_ns;
} tc_decode_stage_stats;

void tc_dev_decode_stats_reset(void);
void tc_dev_decode_stats_get(tc_decode_stage_stats* out);
void tc_dev_decode_stats_add_scan(uint64_t ns);
void tc_dev_decode_stats_add_wall(uint64_t ns);
/* Add outer V7-B directory accounting after the embedded base packet has
 * already contributed its legacy slice counters. */
void tc_dev_decode_stats_add_v7b(uint64_t bytes_read, uint64_t base_bytes_read,
                                 uint32_t segments_read,
                                 uint32_t segments_skipped);

/* ---- M6：编码阶段观测（TOPOS_CODEC_PROFILE=1 开启；内部 dev API） ----
 * 口径与解码侧一致：worker 内以任务局部计时、join 后求和（无原子热开销）。 */

typedef struct tc_encode_stage_stats {
    uint64_t pad_ns;     /* visible→coded pad（含 alpha N-bit 预量化） */
    uint64_t fill_ns;    /* 采样加载 + forward DCT + 量化 + k 统计扫描 */
    uint64_t entropy_ns; /* Rice 符号编码 + bitwriter 提交（slice_encode） */
    uint64_t crc_ns;     /* 每 slice payload CRC */
    uint64_t copy_ns;    /* payload 自有拷贝 + slice header 编码 */
    uint64_t asm_ns;     /* 顺序组装（asm_append memcpy 进最终包） */
    uint32_t slices;     /* slice 总数 */
    uint32_t blocks;     /* 颜色块总数 */
    /* M10-6 取证补全：sized 路径此前未入账的阶段。墙钟字段为 native 调用
     * 线程上的闭合账（prep+probe+final+check ≈ sized 总墙钟）；dct/probe_fill
     * 为 worker CPU 累计（与 fill/entropy 同口径）。 */
    uint64_t dct_ns;        /* M7 prepare forward DCT（worker CPU 累计） */
    uint64_t probe_fill_ns; /* M7 probe 量化+token+精确位计数（worker CPU 累计） */
    uint64_t prep_wall_ns;  /* m7_prepare 墙钟（pad+DCT+alpha，含 join） */
    uint64_t probe_wall_ns; /* m7_probe 全部调用墙钟累计（含每 plane join） */
    uint64_t final_wall_ns; /* m7_final 最终落盘墙钟 */
    uint64_t check_wall_ns; /* 最终包自检 scan+逐 slice CRC 墙钟（plain/sized） */
} tc_encode_stage_stats;

void tc_dev_encode_stats_reset(void);
void tc_dev_encode_stats_get(tc_encode_stage_stats* out);

/* profile 助手（内部共享；decoder_ctx 等复用） */
int tc_profile_enabled(void);
uint64_t tc_profile_now_ns(void);

/* dev/test only：覆盖 qmatrix 查表（NULL 恢复规范表）。 */
void tc_dev_set_qmatrix_override(const tc_qmatrix_set* qms);

/* dev/test only：encode_sized 搜索路径选择——0（M10-7C 起默认）= strict
 * 最小命中 QP 搜索（取消 75% 提前停止，probe p50 ≤ 4 目标）；1 = legacy
 * 线性（oracle/回退，与 R5 行为逐字节一致）；2 = R6 插值快速（与 legacy
 * 逐字节差分对，test_r6 固定）。env TOPOS_SIZED_SEARCH=strict|linear|r6 同效。
 * strict 与 1/2 的最终 qp 语义不同（更贴目标、画质不低于），非 bit 兼容。 */
void tc_dev_set_sized_mode(int32_t mode);

/* 旧 dev 入口（R6 期 fast/legacy 二值）：非 0 → legacy；0 → 当前默认模式 */
void tc_dev_set_sized_search(int legacy);

/* dev/test only：M10-6.3A 跨帧 qp 提示——0（默认）= 启用（strict 搜索首探
 * 种子取上一帧 q*，进程级缓存常驻），1 = 禁用（首探回 qp_base）。
 * 提示只影响探测起点：q* 由 bytes(q) 唯一决定，开关两侧输出逐字节一致
 * （test_r6 差分钉死）；稳态序列探测次数 probe+final ≤ 3。 */
void tc_dev_set_qp_hint(int disable);

/* dev/test only：C2（速度计划 v2）hint 模型强化——0（默认）= 启用（提示
 * 携带 bytes(q*)/局部斜率：目标变化按对数线性外推种子、首跳先验用记忆
 * 斜率），1 = 禁用（回退 M10-6.3A 裸 q* 种子 + 固定先验 4.0）。
 * 仍为纯种子/先验：strict 括号解 q* 与开关无关，两侧 qp 轨迹与包字节
 * 逐值一致（test_c2_sized_hint 差分钉死）。 */
void tc_dev_set_qp_hint_model(int disable);

/* dev/test only：A1（速度计划 v2）差分机——F-cache 捕获。开启后每次
 * m7_prepare 成功（sized 编码）把全部颜色 plane 的 F 系数（自然序
 * [by*cols+bx]*64，i32）拷到 dev 缓冲；关闭时释放。布局查询给各 plane
 * 的块偏移（元素计，×64 后为系数偏移）。供 GPU 化（Metal）数值等价
 * 验收的参考对拍；生产路径默认关（零开销）。 */
void tc_dev_set_fcache_capture(int on);
int64_t tc_dev_fcache_elems(void);          /* 已捕获元素数（0 = 未捕获） */
int64_t tc_dev_fcache_copy(void* dst, size_t cap_elems);
void tc_dev_fcache_layout(uint32_t off_elems[3], uint32_t* planes);

/* dev/test only：A2（速度计划 v2）差分机——解码侧量化系数捕获。
 * 开启后（decode_frames 批）V1/V2/V7-R/R2 颜色 slice 改走 q_out 解码
 * （与生产直写路径同 core 同语义），把重建量化系数（natural 序稠密
 * [block_h][cols][64]，DC 预测已折叠）连同每 slice 反量化步长表 Q[64]
 * 拷到 dev 缓冲；关闭释放。捕获模式下输出平面不写——参考平面需另用
 * 关闭捕获的常规解码获得（确定性解码保证两次 q 一致）。
 * 区域在 dec_frame_build_jobs 主线程按 slice 序预分配，worker 写各自
 * 区域，无锁无竞争；V3/V6 intra 与 V7-A band 路径不在捕获范围。 */
typedef struct tc_dev_qcap_slice {
    uint32_t plane;      /* 0..2 */
    uint32_t block_y0;   /* slice 带首块行 */
    uint32_t block_h;    /* 带内块行数 */
    uint32_t cols;       /* plane 块列数 */
    uint32_t qp_eff;     /* 有效 qp（信息性；Q 表已随捕获交付） */
    uint32_t off_elems;  /* q 缓冲内偏移（i32 元素计） */
} tc_dev_qcap_slice;
void tc_dev_set_qcap_capture(int on);
int64_t tc_dev_qcap_elems(void);            /* 已捕获 i32 元素数 */
int64_t tc_dev_qcap_copy(void* dst, size_t cap_elems);
uint32_t tc_dev_qcap_slices(void);          /* 捕获 slice 元数据条数 */
int64_t tc_dev_qcap_meta_copy(void* dst, size_t cap_slices);
int64_t tc_dev_qcap_q_copy(void* dst, size_t cap_u32);   /* 每 slice Q[64] 拼接 */

/* A2 编辑器移交实验：稀疏发射（与 qcap 捕获联用，tc_dev_set_qcap_sparse）。
 * worker 解码 slice 后扫描稠密 q 直接产 CSR 段——dc（plane 稠密 i32，
 * 全局块号索引）+ 每 slice 的 off 段（slice 局部块号 0..blocks 的累计
 * 对数）+ pairs 段（(uint64)(int64)level<<6|nat，slice 序拼接、按最坏
 * 63 对/块预留——段基址由 meta 推导，确定性布局免 post-join 组装）。 */
void tc_dev_set_qcap_sparse(int on);
int64_t tc_dev_qcap_sp_dc_copy(void* dst, size_t cap_i32);
int64_t tc_dev_qcap_sp_off_copy(void* dst, size_t cap_u32);
int64_t tc_dev_qcap_sp_pairs_copy(void* dst, size_t cap_u64);
uint64_t tc_dev_qcap_sp_npairs(void);       /* 实际发射对数（由 sp_pairs_copy 刷新；fused 路径不自带累计，裸读=上次拷贝值） */

/* A2-4 生产直发目标：稀疏发射直接写调用方缓冲（典型 = GPU 可见 pinned
 * 内存，免 getter/上载趟）。布局与内部模式同构（pairs 最坏 63 对/块
 * 段拼接、off 每 slice [blocks+1] 连续段、dc plane 稠密）——段基址全部
 * 由 meta 确定性推导（soff = Σ(blocks+1) 前缀、spbase = Σ(63×blocks)
 * 前缀），静态几何下辅助表可预建一次。容量以元素计；本批总量超容或
 * pairs==NULL 时回落内部分配（getter 语义不变）。会话契约：一次
 * capture 会话一批 dec_frames（内部模式同此约束）。 */
/* A-enc 批 1 差分机：V7-R2 颜色带 token 捕获（fill 后 dc_m/npair/
 * run/lvl_m + 每 band Q/half/dz 表；段布局几何确定性推导）。 */
void tc_dev_set_tok_capture(int on);
uint32_t tc_dev_tok_bands(void);
int64_t tc_dev_tok_meta_copy(void* dst, size_t cap);
int64_t tc_dev_tok_dc_copy(void* dst, size_t cap_u32);
int64_t tc_dev_tok_np_copy(void* dst, size_t cap_u16);
int64_t tc_dev_tok_run_copy(void* dst, size_t cap_u8);
int64_t tc_dev_tok_lvl_copy(void* dst, size_t cap_u32);
int64_t tc_dev_tok_q_copy(void* dst, size_t cap_u32);
int64_t tc_dev_tok_half_copy(void* dst, size_t cap_u32);
int64_t tc_dev_tok_dz_copy(void* dst, size_t cap_u32);
int64_t tc_dev_tok_fdm1_copy(void* dst, size_t cap_u32);
int64_t tc_dev_tok_fdm0_copy(void* dst, size_t cap_u32);
const uint8_t* tc_dev_zigzag(void);

/* A3/V8 批 1：packet 级 rans2 slice introspection（GPU rANS 差分机输入）。
 * 单线程调用方缓冲直写；meta 每 slice 9×u32（plane, block_y0, block_h,
 * payload_off, payload_size, flags, dc_ctx_n, lvl_ctx_n, crc_ok），bytes =
 * payload 连接，tabs 每 slice 709×u16 归一化表（dc_freq[5×29] dc_cum[5×30]
 * run_freq[64] run_cum[65] lvl_freq[5×28] lvl_cum[5×29]，未用行全零）。
 * 仅 V7-R2 帧；表由解码器同源 tc_rans2_row_decode 产（位精确钉死）。 */
int32_t tc_dev_packet_slices(const uint8_t* packet, size_t packet_size,
                             uint32_t* meta_out, uint32_t meta_stride,
                             uint8_t* bytes_out, size_t bytes_cap,
                             uint16_t* tabs_out, uint32_t tabs_stride,
                             uint32_t* n_out, size_t* bytes_used_out);

/* C5 剖析：r2e 六段累计 ns（TOPOS_CODEC_PROFILE=1 才累积）——
 * [0]collect [1]模型选择 [2]前缀表 [3]fastdiv+scratch [4]后向环 [5]刷写拷贝 */
void tc_dev_r2e_stage_ns(uint64_t* dst6);

/* V8 批 0 实验：瓦片聚合表统计损失模拟（不写位流、不动生产路径）。
 * 前置：tc_dev_set_tok_capture(1) 会话已捕获一次 V7-R2 编码的 token。
 * 对该 token 集重放编码：
 *  - A（差分锚）：重导 hist/joints + 生产同构模型选择/发射，逐带必须
 *    复现生产 payload（前缀+流字节），任一失配即报 MALFORMED——
 *    该锚钉死后 B 路径数值才可信；
 *  - B：按 tile_rows 块行（0 = 整平面）把同平面带聚成瓦片，模型来自
 *    瓦片聚合计数（row_encode→row_decode 同源），表每瓦片一份 +
 *    每带瓦片模型流一份。
 * 段簿记为解析项：seg_blocks 块/段（V8 规格默认 16），S = Σ_p
 * ceil(blocks_p/seg_blocks)，目录按 12B/段、额外终态按 4B×(S−带数)。
 * 单线程、编码后调用；可对同一捕获多次调用（不同 tile_rows 扫描）。 */
typedef struct tc_dev_tile_sim_out {
    uint64_t a_total;         /* A：Σ 生产 payload（前缀+流） */
    uint64_t a_prefix;        /* A：Σ 前缀表字节 */
    uint64_t a_stream;        /* A：Σ 流字节（含每带 4B 终态） */
    uint64_t b_total;         /* B：瓦片表 + Σ 瓦片模型流 */
    uint64_t b_table;         /* B：Σ 瓦片前缀表字节 */
    uint64_t b_stream;        /* B：Σ 流字节（含每带 4B 终态） */
    uint32_t n_bands;         /* 捕获带数（回显） */
    uint32_t n_tiles;         /* 本次 tile_rows 下瓦片数（跨三平面） */
    uint32_t tile_rows;       /* 回显（0 = 整平面） */
    uint32_t seg_blocks;      /* 回显 */
    uint64_t seg_count;       /* S = Σ_p ceil(blocks_p/seg_blocks) */
    uint64_t seg_dir_bytes;   /* 12×S（目录上界口径；9B 压缩变体见落档） */
    uint64_t seg_state_extra; /* 4×(S − n_bands)：段化后额外终态 */
    uint32_t flags_lvl[3];    /* B 瓦片模型选择分布 [NONE, POS, PREV] */
    uint32_t flags_dc[2];     /* B 瓦片 DC 模型选择分布 [NONE, PREV] */
} tc_dev_tile_sim_out;
int32_t tc_dev_rans2_tile_sim(uint32_t tile_rows, uint32_t seg_blocks,
                              tc_dev_tile_sim_out* out);

/* V8 批 3 位精确 oracle：帧级 q-hash（编码/解码同构折叠——瓦片内块栅格
 * 序、瓦片序与平面序合并）。clean 流 round-trip 必须 enc==dec；
 * conceal 段按全零 q 折叠（hash 语义仍确定，但不与编码侧相等）。 */
uint64_t tc_dev_v8_enc_qhash(void);
uint64_t tc_dev_v8_dec_qhash(void);

/* V8 批 3：段并行 CPU 解码（v8_frame_decode；ctx/plain 入口内部分派）。
 * planes/strides 语义同 tc_frame_decode；out_info 填充含 per-tile 状态。 */
int32_t v8_frame_decode(const uint8_t* data, size_t size,
                        uint16_t* const planes_out[TC_FRAME_MAX_PLANES],
                        const size_t strides[TC_FRAME_MAX_PLANES],
                        topos_frame_output* out_info);

/* V8 批 4：GPU 导出（scan_v8 + 瓦片表同源解析 → 每 segment smeta 14×u32
 * + 每 tile 归一化表 709×u16；布局注释见 codec.c 实现）。 */
int32_t tc_dev_v8_gpu_export(const uint8_t* pkt, size_t size,
                             uint32_t* smeta_out, uint32_t smeta_stride,
                             uint16_t* tabs_out, uint32_t tabs_stride,
                             uint32_t* n_segments_out,
                             uint32_t* n_tiles_out,
                             size_t* stream_bytes_out);
void tc_dev_qcap_sp_set_targets(uint64_t* pairs, uint64_t pairs_cap,
                                uint32_t* off, uint64_t off_cap,
                                int32_t* dc, uint64_t dc_cap);

/* dev/test only：M6 token 编码路径开关——0（默认）= token 路径（可用时），
 * 1 = 强制 qbuf 双遍回退路径。两路径位流逐位一致（差分测试钉死）。 */
void tc_dev_set_token_encode(int disable);

/* dev/test only：M10-2A 专用 scan-to-plane 开关——0（默认）= 生产直写
 * 路径，1 = 强制 generic sink 路径（差分对拍/应急回退）。 */
void tc_dev_set_direct_scan(int disable);

/* dev/test only：M10-2B 稀疏重建阈值——非零 AC 对 ≤ 阈值走稀疏 basis
 * 逆变换（A/B 未达门，默认 0 = 禁用）。返回当前生效值。 */
void tc_dev_set_sparse_threshold(int n);
int tc_dev_sparse_threshold(void);

/* dev/test only：M10-2C 四块 SoA 批量 IDCT 开关——0（默认）= 启用，
 * 1 = 禁用（逐块内核，差分对拍/应急回退）。 */
void tc_dev_set_batch_idct(int disable);
int tc_dev_batch_idct(void);

/* dev/test only：M7 sized 搜索的 DCT-once/exact-probe 路径开关——
 * 0（默认）= F-cache 精确位计数 probe + final 一次落盘，
 * 1 = 强制整帧编码 probe 旧路径。两路径最终 qp/包逐字节一致。 */
/* dev/test only：M7 F-cache 精确 probe 路径开关（差分/回退）。 */
void tc_dev_set_sized_m7(int disable);

/* dev/test only：M10-6.1 进程级持久 enc_shared 缓存开关（1=禁用，走逐调用
 * 栈实例——差分基准/回退；0=默认启用）。稳态（几何不变）编码零分配。 */
void tc_dev_set_enc_cache(int disable);

/* dev/test only（P1-08）：enc_shared 缓存命中/回退量化——2+ 编码器并行
 * 压测的归因证据（回退栈实例 = 每调用全量分配）。relaxed 原子计数。 */
typedef struct tc_enc_cache_stats {
    uint64_t hits;              /* 借到缓存实例 */
    uint64_t fallback_busy;     /* busy 标志被占（并发竞争）→ 栈实例 */
    uint64_t fallback_disabled; /* 显式禁用 → 栈实例 */
    uint64_t fallback_alloc;    /* 缓存实例分配失败 → 栈实例 */
} tc_enc_cache_stats;

void tc_dev_enc_cache_stats_reset(void);
void tc_dev_enc_cache_stats_get(tc_enc_cache_stats* out);

/* dev/test only（N03 回归）：实测进程缓存实例的常驻槽位缓冲（qbuf/
 * dc×2/rbuf + tok 四项，malloc_size 逐槽核对）是否覆盖当前容量需求——
 * 不信任水位字段本身。返回违例计数（0 = 全部一致）。须在无编码在飞时
 * 调用（单测里编码调用已返回）。 */
int32_t tc_dev_enc_slots_validate(void);

/* dev/test only（R6 观测探针）：sized 搜索整帧编码次数/调用次数累计。
 * topos_quality perf 用其报告平均迭代数；P1-06 起为 relaxed 原子计数，
 * 多线程编码下读数为一致快照（并发期间读仍可能少计在飞调用）。 */
void tc_dev_sized_reset(void);
uint64_t tc_dev_sized_iters(void);
uint64_t tc_dev_sized_calls(void);

/* ---- M9：V2 canonical VLC 训练符号直方图（dev；spec v2 §4.2） ----
 * 在生产量化/token 遍（fill_color_band*）中累计 DC_CAT/RUN/LEVEL_CAT 符号
 * 频次——统计与编码器实际符号流同源，无独立复刻漂移。符号定义：
 *  - DC：cat = bitlen(map_signed(dc_diff))，域 0..28；
 *  - RUN：run 值 0..62，EOB 恒为 63；
 *  - LEVEL：cat = bitlen(map_signed(level))，域 1..27（索引 0 恒 0——
 *    cat=0 不是合法 level 符号）。
 * relaxed 原子累加：开启时与多线程编码共存（仅训练/调参工具开启；
 * 关闭（默认）时热路径仅一次分支判断）。 */
#define TC_SYM_HIST_DC 29u
#define TC_SYM_HIST_RUN 64u
#define TC_SYM_HIST_LVL 28u
#define TC_SYM_PAIR (TC_SYM_HIST_RUN * TC_SYM_HIST_LVL)

void tc_dev_symbol_hist_enable(int enable);
void tc_dev_symbol_hist_reset(void);
/* 拷贝输出当前累计（不重置）；数组长度须为上述常量。 */
void tc_dev_symbol_hist_get(uint64_t* dc, uint64_t* run, uint64_t* lvl);
/* 按颜色平面拷贝当前累计（plane 0/1/2；越界请求返回全零）。 */
void tc_dev_symbol_hist_plane_get(uint32_t plane, uint64_t* dc, uint64_t* run,
                                  uint64_t* lvl);
/* C1 原型用联合 (run, level-category) 直方图；索引 = run*28 + category。 */
void tc_dev_symbol_pair_hist_get(uint32_t plane, uint64_t* pair_hist);

/* ---- C036 前置测量：rANS order-1 上下文建模天花板（dev；ctx_ceiling.h） ----
 * 在生产量化/token 遍内按解码序因果上下文（扫描位置/前一符号/块 DC 类）
 * 累积条件联合直方图，逐 slice 以理想模型折叠符号位数——衡量格式扩展
 * 前的纯增益天花板，不改任何编码输出。仅 VLC/rANS 模式参与（Rice/alpha
 * 符号族不同）。累加器布局见 ctx_ceiling.h TC_CTX_ACC_*；上下文域同。 */
#define TC_CTX_ACC_COUNT 19u

void tc_dev_ctx_enable(int enable);
void tc_dev_ctx_reset(void);
void tc_dev_ctx_get(double* acc /*[TC_CTX_ACC_COUNT]*/);
/* pooled 联合（跨 slice 汇总，仅供报告结构检视；布局 ctx*syms + sym） */
void tc_dev_ctx_pooled_r1(uint64_t* out); /* [8*64]  */
void tc_dev_ctx_pooled_l1(uint64_t* out); /* [6*28]  */
void tc_dev_ctx_pooled_l3(uint64_t* out); /* [30*28] */
void tc_dev_ctx_pooled_d1(uint64_t* out); /* [5*29]  */

#endif /* TOPOS_INTERNAL_CODEC_H */
