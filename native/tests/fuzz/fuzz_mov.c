/* fuzz_mov —— 阶段 5：MOV 容器层 fuzz。
 *
 * LLVMFuzzerTestOneInput：任意字节 → 内存 io → tc_movie_open →（成功时）
 * info 不变量 + 全 sample 探测/读取 + 音频轨不变量（M-B2）+ faststart
 * 往返（写回内存再开）。
 * 不变量：返回码 ∈ 定义集；open 成功则 sample_count ≥ 1 且索引数组非空；
 * 单 sample 读取失败不影响其它 sample 状态；音频轨声明时 sample/chunk
 * 计数自洽、priming < 总采样数、首块探测/读取不越界；faststart 输出可
 * 再次 open 且 sample_count/pts/音频元数据一致。分配上限 = 索引 25B/sample
 * （§7.7）。
 *
 * 复用 corpus_replay driver（-gen 确定性回放；libFuzzer 目标直接链接本文件）。
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "topos_codec.h"

static int status_is_defined(int32_t rc)
{
    /* M-B2 修正：TC_OK(0) 亦属定义集——音频种子入库前本 driver 的 CI 语料
     * 全部 open 失败，成功分支从未被执行（首个有效文件即暴露此判定缺口） */
    return rc == TC_OK || rc >= TC_WARN_CONCEALED || rc == TC_ERR_INVALID_ARGUMENT ||
           rc == TC_ERR_OUT_OF_MEMORY || rc == TC_ERR_LIMIT_EXCEEDED ||
           rc == TC_ERR_MALFORMED || rc == TC_ERR_TRUNCATED ||
           rc == TC_ERR_CHECKSUM_MISMATCH || rc == TC_ERR_IO ||
           rc == TC_ERR_BUFFER_TOO_SMALL;
}

/* —— 内存 io —— */
typedef struct {
    const uint8_t* data;
    uint64_t len;
} mem_src;

static int32_t ms_read(void* ctx, uint64_t off, void* buf, size_t n)
{
    mem_src* s = (mem_src*)ctx;
    if (off > s->len || n > s->len - off) { return TC_ERR_IO; }
    if (n != 0u) { memcpy(buf, s->data + off, n); }
    return TC_OK;
}

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
    int oom;
} mem_sink;

static int32_t msk_write(void* ctx, const void* d, size_t n)
{
    mem_sink* s = (mem_sink*)ctx;
    if (s->len + n > s->cap) {
        size_t cap = s->cap ? s->cap : 256u;
        while (cap < s->len + n) { cap *= 2u; }
        uint8_t* p = (uint8_t*)realloc(s->data, cap);
        if (p == NULL) { s->oom = 1; return TC_ERR_OUT_OF_MEMORY; }
        s->data = p;
        s->cap = cap;
    }
    if (n != 0u) { memcpy(s->data + s->len, d, n); }
    s->len += n;
    return TC_OK;
}

static int32_t msk_seek_write(void* ctx, uint64_t off, const void* d, size_t n)
{
    mem_sink* s = (mem_sink*)ctx;
    if (off > (uint64_t)s->len || n > (uint64_t)s->len - off) { return TC_ERR_IO; }
    if (n != 0u) { memcpy(s->data + off, d, n); }
    return TC_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > 8u * 1024u * 1024u) { return 0; } /* 容器头远小于此；防耗时失控 */

    mem_src src = { data, (uint64_t)size };
    topos_io io;
    memset(&io, 0, sizeof(io));
    io.struct_size = (uint32_t)sizeof(io);
    io.abi_version = TOPOS_CODEC_ABI_VERSION;
    io.ctx = &src;
    io.read = ms_read;
    io.length = (uint64_t)size;

    topos_movie* mv = NULL;
    int32_t rc = tc_movie_open(&io, &mv);
    if (!status_is_defined(rc)) { abort(); }
    if (rc != TC_OK) { return 0; }
    if (mv == NULL) { abort(); }

    topos_movie_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    int32_t irc = tc_movie_info(mv, &info);
    if (!status_is_defined(irc)) { abort(); }
    if (irc == TC_OK && info.sample_count == 0u) { abort(); }

    /* M-B2：音频轨不变量（声明 → 计数自洽 + priming 有界 + 读取不越界） */
    topos_audio_track_info ai;
    memset(&ai, 0, sizeof(ai));
    ai.struct_size = (uint32_t)sizeof(ai);
    ai.abi_version = TOPOS_CODEC_ABI_VERSION;
    int32_t arc = tc_movie_audio_info(mv, &ai);
    if (!status_is_defined(arc)) { abort(); }
    if (arc == TC_OK) {
        if (ai.codec != TC_AUDIO_CODEC_NONE) {
            if (ai.sample_count == 0u || ai.chunk_count == 0u) { abort(); }
            if (ai.sample_rate == 0u || ai.channel_count == 0u) { abort(); }
            /* elst media_time < 轨总采样数（0 = 无 elst，合法） */
            if (ai.reserved[0] >= ai.sample_count) { abort(); }
            size_t need = 0;
            int32_t prc = tc_movie_read_audio(mv, 0, 1u, NULL, 0, &need);
            if (!status_is_defined(prc)) { abort(); }
            if (prc == TC_OK && need == 0u) { abort(); }
            if (prc == TC_OK && need <= 4u * 1024u * 1024u) {
                uint8_t* abuf = (uint8_t*)malloc(need);
                if (abuf != NULL) {
                    size_t got = 0;
                    int32_t rrc = tc_movie_read_audio(mv, 0, 1u, abuf, need, &got);
                    if (!status_is_defined(rrc)) { abort(); }
                    if (rrc == TC_OK && got != need) { abort(); }
                    free(abuf);
                }
            }
            /* 越界起始 chunk：拒绝路径必须返回定义状态 */
            int32_t orc = tc_movie_read_audio(mv, ai.chunk_count, 1u, NULL, 0, &need);
            if (!status_is_defined(orc)) { abort(); }
        } else {
            if (ai.sample_count != 0u || ai.chunk_count != 0u) { abort(); }
        }
    }

    /* M-B8：多轨不变量（track_count 自洽 + 逐轨 info/priming 有界/读取
     * 探测 + 越界轨拒绝）——v1.6 落地后 fuzz 补齐（时序缝隙修复） */
    uint32_t n_tracks = 0;
    int32_t trc = tc_movie_audio_track_count(mv, &n_tracks);
    if (!status_is_defined(trc)) { abort(); }
    if (trc == TC_OK) {
        for (uint32_t ti = 0; ti < n_tracks && ti < 16u; ++ti) {
            topos_audio_track_info tai;
            memset(&tai, 0, sizeof(tai));
            tai.struct_size = (uint32_t)sizeof(tai);
            tai.abi_version = TOPOS_CODEC_ABI_VERSION;
            int32_t itc = tc_movie_audio_info_at(mv, ti, &tai);
            if (!status_is_defined(itc)) { abort(); }
            if (itc == TC_OK && tai.codec != TC_AUDIO_CODEC_NONE) {
                if (tai.sample_count == 0u || tai.chunk_count == 0u) { abort(); }
                if (tai.sample_rate == 0u || tai.channel_count == 0u) { abort(); }
                if (tai.reserved[0] >= tai.sample_count) { abort(); }
                size_t tneed = 0;
                int32_t tprc = tc_movie_read_audio_at(mv, ti, 0, 1u, NULL, 0,
                                                      &tneed);
                if (!status_is_defined(tprc)) { abort(); }
                int32_t torc = tc_movie_read_audio_at(mv, ti, tai.chunk_count,
                                                      1u, NULL, 0, &tneed);
                if (!status_is_defined(torc)) { abort(); }
            }
        }
        /* 越界轨索引：拒绝路径必须返回定义状态 */
        topos_audio_track_info oob;
        memset(&oob, 0, sizeof(oob));
        oob.struct_size = (uint32_t)sizeof(oob);
        oob.abi_version = TOPOS_CODEC_ABI_VERSION;
        int32_t oorc = tc_movie_audio_info_at(mv, n_tracks, &oob);
        if (!status_is_defined(oorc)) { abort(); }
    }

    /* M-B9：tmcd 不变量（有 → 分量域内 + fps/DF 一致；无 → STATE） */
    topos_timecode_info tci;
    memset(&tci, 0, sizeof(tci));
    tci.struct_size = (uint32_t)sizeof(tci);
    tci.abi_version = TOPOS_CODEC_ABI_VERSION;
    int32_t krc = tc_movie_timecode(mv, &tci);
    /* 无 tmcd → TC_ERR_STATE 是该 API 的定义结果（不在通用白名单内，
     * 与 M-B2 修 status_is_defined 缺 TC_OK 同类的判定缺口——此处局部
     * 特判而非放宽全局白名单） */
    if (krc != TC_OK && krc != TC_ERR_STATE) { abort(); }
    if (krc == TC_OK) {
        if (tci.hh < 0 || tci.hh > 23 || tci.mm < 0 || tci.mm > 59 ||
            tci.ss < 0 || tci.ss > 59 || tci.ff < 0) { abort(); }
        if (tci.fps != 24u && tci.fps != 25u && tci.fps != 30u &&
            tci.fps != 48u && tci.fps != 50u && tci.fps != 60u) { abort(); }
        if ((uint32_t)tci.ff >= tci.fps) { abort(); }
        if (tci.drop_frame != 0u && tci.fps != 30u && tci.fps != 60u) { abort(); }
    }

    /* 全 sample 探测 + 小 sample 读取（大 sample 仅探测，控耗时） */
    uint8_t* buf = NULL;
    size_t buf_cap = 0u;
    for (uint32_t i = 0; i < info.sample_count && i < 4096u; ++i) {
        size_t need = 0;
        int32_t prc = tc_movie_packet(mv, i, NULL, 0, &need);
        if (!status_is_defined(prc)) { abort(); }
        if (prc != TC_ERR_BUFFER_TOO_SMALL) { continue; } /* 越界 sample（§7.6） */
        if (need <= 1u * 1024u * 1024u) {
            if (need > buf_cap) {
                free(buf);
                buf = (uint8_t*)malloc(need);
                buf_cap = buf ? need : 0u;
                if (buf == NULL) { break; }
            }
            prc = tc_movie_packet(mv, i, buf, need, NULL);
            if (!status_is_defined(prc)) { abort(); }
        }
        uint64_t pts = 0;
        uint32_t dur = 0;
        if (tc_movie_packet_pts(mv, i, &pts, &dur) != TC_OK) { abort(); }
        uint8_t sync = 0;
        if (tc_movie_packet_sync(mv, i, &sync) != TC_OK) { abort(); }
        if (sync > 1u) { abort(); }
    }
    free(buf);

    /* faststart 往返（只对非 faststart 输入；输出可再 open 且计数一致） */
    if (info.sample_count <= 64u && info.faststart == 0u) {
        mem_sink sink;
        memset(&sink, 0, sizeof(sink));
        topos_io dst;
        memset(&dst, 0, sizeof(dst));
        dst.struct_size = (uint32_t)sizeof(dst);
        dst.abi_version = TOPOS_CODEC_ABI_VERSION;
        dst.ctx = &sink;
        dst.write = msk_write;
        dst.seek_write = msk_seek_write;
        int32_t frc = tc_movie_faststart(&io, &dst);
        if (!status_is_defined(frc)) { abort(); }
        if (frc == TC_OK && !sink.oom) {
            mem_src src2 = { sink.data, (uint64_t)sink.len };
            topos_io io2;
            memset(&io2, 0, sizeof(io2));
            io2.struct_size = (uint32_t)sizeof(io2);
            io2.abi_version = TOPOS_CODEC_ABI_VERSION;
            io2.ctx = &src2;
            io2.read = ms_read;
            io2.length = (uint64_t)sink.len;
            topos_movie* mv2 = NULL;
            int32_t rc2 = tc_movie_open(&io2, &mv2);
            if (!status_is_defined(rc2)) { abort(); }
            if (rc2 == TC_OK) {
                topos_movie_info info2;
                memset(&info2, 0, sizeof(info2));
                info2.struct_size = (uint32_t)sizeof(info2);
                info2.abi_version = TOPOS_CODEC_ABI_VERSION;
                if (tc_movie_info(mv2, &info2) == TC_OK) {
                    if (info2.sample_count != info.sample_count) { abort(); }
                    if (info2.faststart != 1u) { abort(); }
                    for (uint32_t i = 0; i < info.sample_count; ++i) {
                        uint64_t p1 = 0, p2 = 0;
                        uint32_t d1 = 0, d2 = 0;
                        tc_movie_packet_pts(mv, i, &p1, &d1);
                        tc_movie_packet_pts(mv2, i, &p2, &d2);
                        if (p1 != p2 || d1 != d2) { abort(); }
                    }
                }
                /* M-B2：faststart 前后音频元数据一致（含 elst priming） */
                topos_audio_track_info ai2;
                memset(&ai2, 0, sizeof(ai2));
                ai2.struct_size = (uint32_t)sizeof(ai2);
                ai2.abi_version = TOPOS_CODEC_ABI_VERSION;
                if (tc_movie_audio_info(mv2, &ai2) == TC_OK) {
                    if (ai2.codec != ai.codec ||
                        ai2.sample_count != ai.sample_count ||
                        ai2.chunk_count != ai.chunk_count ||
                        ai2.reserved[0] != ai.reserved[0]) { abort(); }
                }
                /* M-B8：faststart 前后轨数与逐轨元数据一致 */
                uint32_t n_tracks2 = 0;
                if (tc_movie_audio_track_count(mv2, &n_tracks2) == TC_OK &&
                    n_tracks2 != n_tracks) { abort(); }
                for (uint32_t ti = 0; ti < n_tracks2 && ti < 16u; ++ti) {
                    topos_audio_track_info b1, b2;
                    memset(&b1, 0, sizeof(b1));
                    memset(&b2, 0, sizeof(b2));
                    b1.struct_size = (uint32_t)sizeof(b1);
                    b2.struct_size = (uint32_t)sizeof(b2);
                    b1.abi_version = TOPOS_CODEC_ABI_VERSION;
                    b2.abi_version = TOPOS_CODEC_ABI_VERSION;
                    if (tc_movie_audio_info_at(mv, ti, &b1) == TC_OK &&
                        tc_movie_audio_info_at(mv2, ti, &b2) == TC_OK) {
                        if (b1.codec != b2.codec ||
                            b1.sample_count != b2.sample_count ||
                            b1.chunk_count != b2.chunk_count ||
                            b1.sample_rate != b2.sample_rate ||
                            b1.channel_count != b2.channel_count ||
                            b1.bits_per_sample != b2.bits_per_sample ||
                            b1.reserved[0] != b2.reserved[0] ||
                            b1.reserved[1] != b2.reserved[1]) { abort(); }
                    }
                }
                /* M-B9：faststart 前后 tmcd 一致 */
                topos_timecode_info tc2;
                memset(&tc2, 0, sizeof(tc2));
                tc2.struct_size = (uint32_t)sizeof(tc2);
                tc2.abi_version = TOPOS_CODEC_ABI_VERSION;
                int32_t krc2 = tc_movie_timecode(mv2, &tc2);
                if (krc2 != TC_OK && krc2 != TC_ERR_STATE) { abort(); }
                if (krc2 != krc) { abort(); }
                if (krc2 == TC_OK &&
                    (tc2.start_frame_count != tci.start_frame_count ||
                     tc2.fps != tci.fps || tc2.drop_frame != tci.drop_frame)) {
                    abort();
                }
                tc_movie_close(mv2);
            }
        }
        free(sink.data);
    }

    tc_movie_close(mv);
    return 0;
}
