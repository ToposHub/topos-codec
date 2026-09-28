/* 块符号层 —— 颜色块 DC/AC 与 Alpha 残差流的编码单元（spec §7.5 / §8.3）。
 *
 * 颜色块（8×8 量化系数，natural 序 q[u*8+v]）：
 *  - DC：slice 内差分。pred = 双邻 ((left+top+1)>>1) / left / top / 0；
 *    「跨 slice 一律不可用」由调用方（slice_codec）只传入 slice 内邻居保证。
 *    (left+top+1)>>1 以 round-half-up 符号拆分实现（负数不右移，spec §2.4）。
 *  - AC：(run, level) 对沿 zigzag；run 无符号 Rice(k3)，level 有符号 Rice(k2)；
 *    末尾必须紧跟 run=63 的 EOB；全零块也输出 EOB。
 *  - 解码不变量（spec v1 §7.5，修正冻结）：字面系数索引 idx = pos + run ≤ 63；
 *    EOB 接受于任意 pos（1..64）。每块符号数 ≤ 65（有界完成性）。
 *
 * Alpha 残差流（无 EOB，像素计数约束终止）：
 *  - 数据对 (run, level≠0)：run ≤ 剩余−1，level 有符号 Rice(k1)、run Rice(k2)；
 *  - 尾零终结对 (trailing_run, level=0)：要求 pos + run == count（覆盖「尾部全零」，
 *    含整带全零）。数据流中 level≠0 由构造保证，终结语义无歧义（spec v1 §8.3）。
 */
#ifndef TOPOS_INTERNAL_BLOCK_CODING_H
#define TOPOS_INTERNAL_BLOCK_CODING_H

#include <stddef.h>
#include <stdint.h>

#include "../bitstream/bitio.h"

struct tc_vlc_book; /* 前置声明（VLC 变体参数；完整定义见 entropy/vlc.h） */

#define TC_C2_PAIR_SYMS 58u
#define TC_C2_PAIR_ESCAPE 56u
#define TC_C2_PAIR_EOB 57u

/* splitmix64 finalizer：符号流确定性指纹（跨平台一致；decode 的 O(1) 内存校验路径） */
static inline uint64_t tc_symbol_hash_mix(uint64_t h, uint64_t v)
{
    h += 0x9E3779B97F4A7C15ull;
    uint64_t z = v;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return h ^ (z ^ (z >> 31));
}

/* DC 预测：可用性以 has_* 表达（不可用者值被忽略） */
int32_t tc_dc_predict(int has_left, int32_t left_dc, int has_top, int32_t top_dc);

/* 编码一个颜色块。dc_out（可 NULL）返回本块 DC 量化值供后续块预测。 */
int32_t tc_block_encode(tc_bitwriter* bw, uint32_t k1, uint32_t k2, uint32_t k3,
                        const int32_t q_natural[64],
                        int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                        int32_t* dc_out);

/* R6：zigzag 序输入的编码变体（q_zig[sp] = 第 sp 个扫描位置的系数）。
 * 输出位流与 tc_block_encode(natural 序等价输入) 逐位一致（test_r6 差分）。
 * q_zig[0] 即 DC（zigzag 扫描位置 0 == natural 0）。 */
int32_t tc_block_encode_zigzag(tc_bitwriter* bw, uint32_t k1, uint32_t k2, uint32_t k3,
                               const int32_t q_zig[64],
                               int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                               int32_t* dc_out);

/* ---- V2 canonical VLC 变体（spec v2 §4.1；major=2 且 entropy_mode=1 的颜色 slice） ----
 * 符号语义与 Rice 版逐值同源：DC 差分 → DC_CAT(+后缀)；(run,level) 对 →
 * RUN 符号 + LEVEL_CAT(+后缀)；EOB = RUN 63。域校验同 V1 常量
 * （|DC 残差| ≤ 2^26 → m ≤ 2^27；|level| ≤ 2^25 → m ≤ 2^26；run ≤ 62）。
 * 书 id 由 slice header 的 k1/k2/k3 位置承载（0..3）。 */
int32_t tc_block_encode_zigzag_vlc(tc_bitwriter* bw,
                                   const struct tc_vlc_book* dc_book,
                                   const struct tc_vlc_book* lvl_book,
                                   const struct tc_vlc_book* run_book,
                                   const int32_t q_zig[64],
                                   int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                                   int32_t* dc_out);
int32_t tc_block_decode_vlc(tc_bitreader* br,
                            const struct tc_vlc_book* dc_book,
                            const struct tc_vlc_book* lvl_book,
                            const struct tc_vlc_book* run_book,
                            int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                            int32_t q_natural[64]);

/* C1 experimental joint AC syntax (major=4, entropy_mode=2).  The common
 * alphabet folds run 0..7 and level categories 1..7 into one RUN-family
 * symbol; symbol 62 is an explicit escape carrying a raw run and the normal
 * level category, while 63 remains EOB.  This keeps the syntax deterministic
 * and independently truncation-checkable without changing the V2 stream. */
int32_t tc_block_encode_zigzag_c1(tc_bitwriter* bw,
                                  const struct tc_vlc_book* dc_book,
                                  const struct tc_vlc_book* lvl_book,
                                  const struct tc_vlc_book* pair_book,
                                  const int32_t q_zig[64],
                                  int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                                  int32_t* dc_out);
int32_t tc_block_decode_c1(tc_bitreader* br,
                           const struct tc_vlc_book* dc_book,
                           const struct tc_vlc_book* lvl_book,
                           const struct tc_vlc_book* pair_book,
                           int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                           int32_t q_natural[64]);

/* C2 experimental per-slice table.  The first 56 symbols are the same common
 * run/category pairs as C1; 56 is escape and 57 is EOB.  lengths_out has 58
 * active entries and four zero reserved entries for the slice header. */
int32_t tc_c2_pair_book_build(const int32_t* q_blocks, size_t block_count,
                              struct tc_vlc_book* book,
                              uint8_t lengths_out[64]);
int32_t tc_block_encode_zigzag_c2(tc_bitwriter* bw,
                                  const struct tc_vlc_book* dc_book,
                                  const struct tc_vlc_book* lvl_book,
                                  const struct tc_vlc_book* pair_book,
                                  const int32_t q_zig[64],
                                  int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                                  int32_t* dc_out);
int32_t tc_block_decode_c2(tc_bitreader* br,
                           const struct tc_vlc_book* dc_book,
                           const struct tc_vlc_book* lvl_book,
                           const struct tc_vlc_book* pair_book,
                           int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                           int32_t q_natural[64]);

/* 解码一个颜色块到 q_natural（未出现的系数为 0）。
 * 返回 TC_OK / TC_ERR_TRUNCATED / TC_ERR_MALFORMED（域或不变量违反）。 */
int32_t tc_block_decode(tc_bitreader* br, uint32_t k1, uint32_t k2, uint32_t k3,
                        int has_left, int32_t left_dc, int has_top, int32_t top_dc,
                        int32_t q_natural[64]);

/* Alpha 残差流：residuals[0..count)。level 用 k_level、run 用 k_run（§4.3：k1/k2）。
 * decode 的 hash_acc（可 NULL）对 count 个残差逐个折叠（含零），供 O(1) 内存指纹路径。 */
int32_t tc_alpha_residuals_encode(tc_bitwriter* bw, uint32_t k_level, uint32_t k_run,
                                  const int32_t* residuals, size_t count);
int32_t tc_alpha_residuals_decode(tc_bitreader* br, uint32_t k_level, uint32_t k_run,
                                  int32_t* residuals_out /*可 NULL：只校验*/,
                                  size_t count, uint64_t* hash_acc /*可 NULL*/);

/* Alpha 数据对流式解码（阶段 4）：每解码出一对回调一次。
 * 数据对：pos..pos+run−1 为零残差，pos+run 处残差 = level（≠0）；
 * 终结对：level == 0 且 run == 剩余像素数（回调后流结束）。
 * sink 返回非 TC_OK 即中止并透传（码流错误优先于 sink 错误）。 */
typedef int32_t (*tc_alpha_pair_sink_fn)(void* ctx, size_t pos, uint32_t run, int32_t level);

int32_t tc_alpha_pairs_decode(tc_bitreader* br, uint32_t k_level, uint32_t k_run,
                              size_t count, tc_alpha_pair_sink_fn sink, void* ctx,
                              uint64_t* hash_acc /*可 NULL*/);

#endif /* TOPOS_INTERNAL_BLOCK_CODING_H */
