/* mov（阶段 5）：mux/demux 往返、确定性、随机访问、FastStart、co64、
 * 损坏注入（atom/偏移/截断 mdat）、tpcC 规则、错误路径、索引内存指标。 */
#include "common/crc32.h"
#include "common/endian.h"
#include "codec/v7_scalable.h"
#include "image_synth.h"
#include "mini_test.h"
#include "topos_codec.h"

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
/* O_BINARY 仅 Windows 存在；POSIX 平台打开标志无二进制/文本之分 */
#ifndef O_BINARY
#define O_BINARY 0
#endif
#if !defined(_MSC_VER)
#include <unistd.h>
#define port_fd_write write
#define port_fd_close close
#else
/* MSVC：无 unistd.h/ssize_t，fd 函数为下划线名；o3 co64 走 64 位偏移 */
#include <io.h>
#include <process.h>
#define getpid _getpid
#if !defined(_SSIZE_T_DEFINED)
typedef long long ssize_t;
#define _SSIZE_T_DEFINED
#endif
#define port_fd_write _write
#define port_fd_close _close
#endif

#if !defined(__APPLE__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* memmem */
#endif

#if defined(_WIN32)
/* MinGW 无 memmem/pread（POSIX/GNU 扩展）：提供等价 shim（仅测试用）。
 * pread shim 用 64 位偏移（_lseeki64），o3 co64 测试偏移超过 4 GiB。 */
#include <errno.h>
#include <stdint.h>
#include <io.h>
static void* port_memmem(const void* hay, size_t hay_len,
                         const void* needle, size_t needle_len)
{
    const uint8_t* h = (const uint8_t*)hay;
    const uint8_t* n = (const uint8_t*)needle;
    if (needle_len == 0u) { return (void*)h; }
    if (hay_len < needle_len || hay == NULL || needle == NULL) { return NULL; }
    for (size_t i = 0; i + needle_len <= hay_len; ++i) {
        if (h[i] == n[0] && memcmp(h + i, n, needle_len) == 0) {
            return (void*)(h + i);
        }
    }
    return NULL;
}
#define memmem port_memmem
static long long port_pread(int fd, void* buf, size_t count, long long offset)
{
    long long prev = _lseeki64(fd, 0, SEEK_CUR);
    if (prev < 0 || _lseeki64(fd, offset, SEEK_SET) < 0) { return -1; }
    long long n = _read(fd, buf, (unsigned int)count);
    int saved = errno;
    _lseeki64(fd, prev, SEEK_SET);
    errno = saved;
    return n;
}
#define pread port_pread
#ifndef O_BINARY
#define O_BINARY 0
#endif
/* Windows off_t 为 32 位：o3 co64 偏移 >4 GiB，必须走 64 位 lseek */
#define port_lseek _lseeki64
#else
#define port_lseek lseek
#endif

/* ---------- 内存 io（mux sink / movie 源共用） ---------- */

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
    int fail_write;   /* 注入写失败 */
} mem_file;

static int32_t mem_read(void* ctx, uint64_t off, void* buf, size_t len)
{
    mem_file* f = (mem_file*)ctx;
    if (off > (uint64_t)f->len || len > (uint64_t)f->len - off) { return TC_ERR_IO; }
    if (len != 0u) { memcpy(buf, f->data + off, len); }
    return TC_OK;
}

typedef struct {
    mem_file* file;
    size_t calls;
    size_t bytes;
    uint64_t forbidden_lo;
    uint64_t forbidden_hi;
    int touched_forbidden;
} trace_mem_file;

static int32_t trace_mem_read(void* ctx, uint64_t off, void* buf, size_t len)
{
    trace_mem_file* trace = (trace_mem_file*)ctx;
    if (trace == NULL || trace->file == NULL) { return TC_ERR_IO; }
    trace->calls++;
    trace->bytes += len;
    if (off < trace->forbidden_hi &&
        (uint64_t)len > trace->forbidden_lo -
            (off < trace->forbidden_lo ? off : trace->forbidden_lo)) {
        trace->touched_forbidden = 1;
    }
    return mem_read(trace->file, off, buf, len);
}

static int32_t mem_write(void* ctx, const void* data, size_t len)
{
    mem_file* f = (mem_file*)ctx;
    if (f->fail_write) { return TC_ERR_IO; }
    if (f->len + len > f->cap) {
        size_t cap = f->cap ? f->cap : 256u;
        while (cap < f->len + len) { cap *= 2u; }
        uint8_t* p = (uint8_t*)realloc(f->data, cap);
        if (p == NULL) { return TC_ERR_OUT_OF_MEMORY; }
        f->data = p;
        f->cap = cap;
    }
    if (len != 0u) { memcpy(f->data + f->len, data, len); }
    f->len += len;
    return TC_OK;
}

static int32_t mem_seek_write(void* ctx, uint64_t off, const void* data, size_t len)
{
    mem_file* f = (mem_file*)ctx;
    if (f->fail_write) { return TC_ERR_IO; }
    if (off > (uint64_t)f->len || len > (uint64_t)f->len - off) { return TC_ERR_IO; }
    if (len != 0u) { memcpy(f->data + off, data, len); }
    return TC_OK;
}

static void mem_io_src(mem_file* f, topos_io* io)
{
    memset(io, 0, sizeof(*io));
    io->struct_size = (uint32_t)sizeof(topos_io);
    io->abi_version = TOPOS_CODEC_ABI_VERSION;
    io->ctx = f;
    io->read = mem_read;
    io->length = (uint64_t)f->len;
}

static void mem_io_sink(mem_file* f, topos_io* io)
{
    mem_io_src(f, io);
    io->write = mem_write;
    io->seek_write = mem_seek_write;
    io->length = 0;
}

static void mem_free(mem_file* f)
{
    free(f->data);
    memset(f, 0, sizeof(*f));
}

/* ---------- 帧 packet 生成（确定性，直接走阶段 4 编码器） ---------- */

#define TEST_FRAMES 7

static uint8_t* g_pkts[TEST_FRAMES];
static size_t g_pkt_sizes[TEST_FRAMES];

static void build_test_packets(void)
{
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
        topos_frame_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.struct_size = (uint32_t)sizeof(cfg);
        cfg.abi_version = TOPOS_CODEC_ABI_VERSION;
        cfg.visible_width = 64u;
        cfg.visible_height = 40u;
        cfg.profile = 3u;
        cfg.bit_depth = 10u;
        cfg.qmatrix_id = 1u;
        cfg.qp_base = 28u + (uint8_t)(i * 5u); /* 逐帧 qp 可变（tpcC 规则豁免项） */
        cfg.color_range = 1u;
        cfg.color_primaries = 1u;
        cfg.color_transfer = 1u;
        cfg.color_matrix = 1u;
        cfg.sar_num = 1u;
        cfg.sar_den = 1u;

        image_synth_cfg sc;
        memset(&sc, 0, sizeof(sc));
        sc.width = 64u;
        sc.height = 40u;
        sc.kind = (tc_synth_kind)(i % TC_SYNTH_KIND_COUNT);
        sc.seed = 100u + (uint64_t)i;
        uint16_t* pl[4] = { NULL, NULL, NULL, NULL };
        image_synth_alloc(&sc, 0, &pl[0], &pl[1], &pl[2], &pl[3]);

        topos_frame_input in;
        memset(&in, 0, sizeof(in));
        in.struct_size = (uint32_t)sizeof(in);
        in.abi_version = TOPOS_CODEC_ABI_VERSION;
        in.planes[0] = pl[0];
        in.planes[1] = pl[1];
        in.planes[2] = pl[2];

        size_t bound = tc_frame_packet_bound(&cfg);
        uint8_t* buf = (uint8_t*)malloc(bound);
        topos_frame_stats st;
        int32_t rc = tc_frame_encode(&cfg, &in, buf, bound, &st);
        MT_CHECK_EQ_I64(rc, TC_OK);
        g_pkts[i] = buf;
        g_pkt_sizes[i] = st.packet_size;
        free(pl[0]);
        free(pl[1]);
        free(pl[2]);
    }
}

static uint8_t* build_packet_with_coding(uint32_t coding, size_t* out_size)
{
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = TOPOS_CODEC_ABI_VERSION;
    cfg.visible_width = 64u;
    cfg.visible_height = 40u;
    cfg.profile = 3u;
    cfg.bit_depth = 10u;
    cfg.qmatrix_id = 1u;
    cfg.qp_base = 28u;
    cfg.reserved[0] = coding;
    cfg.color_range = 1u;
    cfg.color_primaries = 1u;
    cfg.color_transfer = 1u;
    cfg.color_matrix = 1u;
    cfg.sar_num = 1u;
    cfg.sar_den = 1u;

    image_synth_cfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.width = 64u;
    sc.height = 40u;
    sc.kind = TC_SYNTH_GRADIENT;
    sc.seed = 0x7A11u + coding;
    uint16_t* planes[4] = {NULL, NULL, NULL, NULL};
    image_synth_alloc(&sc, 0, &planes[0], &planes[1], &planes[2], &planes[3]);

    topos_frame_input input;
    memset(&input, 0, sizeof(input));
    input.struct_size = (uint32_t)sizeof(input);
    input.abi_version = TOPOS_CODEC_ABI_VERSION;
    input.planes[0] = planes[0];
    input.planes[1] = planes[1];
    input.planes[2] = planes[2];
    size_t bound = tc_frame_packet_bound(&cfg);
    uint8_t* packet = (uint8_t*)malloc(bound);
    topos_frame_stats stats;
    const int32_t rc = tc_frame_encode(&cfg, &input, packet, bound, &stats);
    MT_CHECK_EQ_I64(rc, TC_OK);
    for (uint32_t i = 0u; i < 4u; ++i) { free(planes[i]); }
    if (rc != TC_OK || packet == NULL) {
        free(packet);
        return NULL;
    }
    *out_size = stats.packet_size;
    return packet;
}

static void base_movie_cfg(topos_movie_config* c)
{
    memset(c, 0, sizeof(*c));
    c->struct_size = (uint32_t)sizeof(*c);
    c->abi_version = TOPOS_CODEC_ABI_VERSION;
    c->visible_width = 64u;
    c->visible_height = 40u;
    c->profile = 3u;
    c->pixel_format = 0u;
    c->bit_depth = 10u;
    c->qmatrix_id = 1u;
    c->qp_base = 28u;
    c->color_range = 1u;
    c->color_primaries = 1u;
    c->color_transfer = 1u;
    c->color_matrix = 1u;
    c->sar_num = 1u;
    c->sar_den = 1u;
    c->timescale = 24u;
}

/* 标准多路复用（多帧）→ mem_file */
static void mux_all(mem_file* out, const topos_movie_config* cfg)
{
    memset(out, 0, sizeof(*out));
    topos_io sink;
    mem_io_sink(out, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(cfg, &sink, &m), TC_OK);
    int32_t rc = TC_OK;
    for (uint32_t i = 0; i < TEST_FRAMES && rc == TC_OK; ++i) {
        uint64_t pts = (uint64_t)i;
        rc = tc_mux_add_packet(m, g_pkts[i], g_pkt_sizes[i], pts, 1u);
    }
    MT_CHECK_EQ_I64(rc, TC_OK);
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    tc_mux_free(m);
}

/* 全量读回校验：每帧字节/pts/dur/sync */
static void verify_movie(topos_movie* mv)
{
    topos_movie_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_info(mv, &info), TC_OK);
    MT_CHECK_EQ_U64(info.sample_count, TEST_FRAMES);
    MT_CHECK_EQ_U64(info.visible_width, 64u);
    MT_CHECK_EQ_U64(info.visible_height, 40u);
    MT_CHECK_EQ_U64(info.timescale, 24u);
    MT_CHECK(info.index_bytes > 0u);
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
        size_t need = 0;
        MT_CHECK_EQ_I64(tc_movie_packet(mv, i, NULL, 0, &need), TC_ERR_BUFFER_TOO_SMALL);
        MT_CHECK_EQ_U64(need, g_pkt_sizes[i]);
        uint8_t* buf = (uint8_t*)malloc(need);
        MT_CHECK_EQ_I64(tc_movie_packet(mv, i, buf, need, NULL), TC_OK);
        MT_CHECK(memcmp(buf, g_pkts[i], need) == 0);
        free(buf);
        uint64_t pts = 99;
        uint32_t dur = 0;
        MT_CHECK_EQ_I64(tc_movie_packet_pts(mv, i, &pts, &dur), TC_OK);
        MT_CHECK_EQ_U64(pts, (uint64_t)i);
        MT_CHECK_EQ_U64(dur, 1u);
        uint8_t sync = 0;
        MT_CHECK_EQ_I64(tc_movie_packet_sync(mv, i, &sync), TC_OK);
        MT_CHECK_EQ_U64(sync, 1u); /* 全 intra 全同步 */
    }
}

/* ---------- 用例 ---------- */

static void test_roundtrip_and_determinism(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file a, b;
    mux_all(&a, &cfg);
    mux_all(&b, &cfg);
    MT_CHECK(a.len == b.len && memcmp(a.data, b.data, a.len) == 0); /* 确定性（spec §8） */
    MT_CHECK(a.len > 20u + 12u);

    topos_movie* mv = NULL;
    topos_io src;
    mem_io_src(&a, &src);
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
    MT_CHECK(mv != NULL);
    topos_movie_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    tc_movie_info(mv, &info);
    MT_CHECK_EQ_U64(info.faststart, 0u); /* mux 默认 moov 在后 */
    verify_movie(mv);
    /* tpcC 镜像字段 */
    MT_CHECK_EQ_U64(info.profile, 3u);
    MT_CHECK_EQ_U64(info.qmatrix_id, 1u);
    MT_CHECK_EQ_U64(info.color_matrix, 1u);
    tc_movie_close(mv);
    mem_free(&a);
    mem_free(&b);
}

static void test_faststart(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file std;
    mux_all(&std, &cfg);

    topos_io src, dstio;
    mem_io_src(&std, &src);
    mem_file fs;
    memset(&fs, 0, sizeof(fs));
    mem_io_sink(&fs, &dstio);
    MT_CHECK_EQ_I64(tc_movie_faststart(&src, &dstio), TC_OK);

    /* ftyp 之后紧跟 moov（FastStart 布局） */
    MT_CHECK(fs.len > 20u + 8u);
    MT_CHECK(memcmp(fs.data + 4, "ftyp", 4u) == 0);        /* 类型字段正确 */
    MT_CHECK(memcmp(fs.data + 24, "moov", 4u) == 0);       /* ftyp 后即 moov（size 在 20..24） */

    /* 双布局均可打开，且内容一致 */
    topos_movie* mv = NULL;
    topos_io fsio;
    mem_io_src(&fs, &fsio);
    MT_CHECK_EQ_I64(tc_movie_open(&fsio, &mv), TC_OK);
    topos_movie_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    tc_movie_info(mv, &info);
    MT_CHECK_EQ_U64(info.faststart, 1u);
    verify_movie(mv);
    tc_movie_close(mv);

    /* 幂等：已是 faststart 再跑 → TC_OK 原样 */
    mem_file fs2;
    memset(&fs2, 0, sizeof(fs2));
    topos_io fs2io, dst2;
    mem_io_src(&fs, &fs2io);
    mem_io_sink(&fs2, &dst2);
    MT_CHECK_EQ_I64(tc_movie_faststart(&fs2io, &dst2), TC_OK);
    MT_CHECK(fs2.len == 0u);
    mem_free(&fs2);
    mem_free(&fs);
    mem_free(&std);
}

static void test_foreign_rejected(void)
{
    /* 非 TPIC entry（手搓最小 mov 壳）→ MALFORMED */
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file f;
    mux_all(&f, &cfg);
    /* 用 entry 独有模式定位（TPIC + 6×0 保留 + data_reference_index=1）；
     * 不能裸搜 "TPIC"——每个 elementary packet 的魔数也是 "TPIC" */
    static const uint8_t ENTRY_PAT[14] = {
        'T', 'P', 'I', 'C', 0, 0, 0, 0, 0, 0, 0, 1, 0, 0
    };
    uint8_t* hit = (uint8_t*)memmem(f.data, f.len, ENTRY_PAT, sizeof(ENTRY_PAT));
    MT_CHECK(hit != NULL);
    if (hit != NULL) { memcpy(hit, "ap4h", 4u); }
    topos_movie* mv = (topos_movie*)0x1;
    topos_io io;
    mem_io_src(&f, &io);
    MT_CHECK_EQ_I64(tc_movie_open(&io, &mv), TC_ERR_MALFORMED);
    MT_CHECK(mv == NULL);
    mem_free(&f);
}

static void test_corruption_isolated(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file f;
    mux_all(&f, &cfg);

    /* 截断 mdat：中段帧区间越界 → 仅该帧失败，其余正常（spec §7.6） */
    mem_file cut;
    cut.data = (uint8_t*)malloc(f.len / 2u);
    cut.len = f.len / 2u;
    cut.cap = f.len / 2u;
    memcpy(cut.data, f.data, cut.len);
    topos_movie* mv = NULL;
    topos_io io;
    mem_io_src(&cut, &io);
    /* moov 在 mdat 之后 → 截半后 moov 缺失 → 整体 MALFORMED（合法行为） */
    MT_CHECK_EQ_I64(tc_movie_open(&io, &mv), TC_ERR_MALFORMED);
    MT_CHECK(mv == NULL);
    mem_free(&cut);

    /* 快速定位每帧偏移：faststart 版里 moov 在前，截 mdat 保留 moov */
    mem_file fs;
    memset(&fs, 0, sizeof(fs));
    {
        topos_io src, d;
        mem_io_src(&f, &src);
        mem_io_sink(&fs, &d);
        MT_CHECK_EQ_I64(tc_movie_faststart(&src, &d), TC_OK);
    }
    /* 找 mdat 起点 */
    size_t mdat_at = 0;
    for (size_t k = 0; k + 8u < fs.len;) {
        uint32_t sz = tc_load_be32(fs.data + k);
        if (memcmp(fs.data + k + 4u, "mdat", 4u) == 0) { mdat_at = k; break; }
        if (sz < 8u) { break; }
        k += sz;
    }
    MT_CHECK(mdat_at != 0);
    /* mdat 截到第 3 帧中途：前 2 帧可读、第 2 帧起区间越界 */
    size_t keep = mdat_at + 16u + g_pkt_sizes[0] + g_pkt_sizes[1] + g_pkt_sizes[2] / 2u;
    mem_file trunc;
    trunc.data = (uint8_t*)malloc(keep);
    trunc.cap = keep;
    trunc.len = keep;
    memcpy(trunc.data, fs.data, keep);
    /* mdat 头仍是完整声明长度 —— reader 端以 io.length 判界 */
    topos_movie* mv2 = NULL;
    topos_io tio;
    mem_io_src(&trunc, &tio);
    MT_CHECK_EQ_I64(tc_movie_open(&tio, &mv2), TC_OK); /* moov 完整可索引 */
    uint8_t tmp[4096];
    MT_CHECK_EQ_I64(tc_movie_packet(mv2, 0, tmp, sizeof(tmp), NULL),
                    g_pkt_sizes[0] <= sizeof(tmp) ? TC_OK : TC_ERR_BUFFER_TOO_SMALL);
    size_t need = 0;
    MT_CHECK_EQ_I64(tc_movie_packet(mv2, 2, NULL, 0, &need), TC_ERR_MALFORMED); /* 越界帧 */
    MT_CHECK_EQ_I64(tc_movie_packet(mv2, 1, NULL, 0, &need), TC_ERR_BUFFER_TOO_SMALL);
    MT_CHECK_EQ_U64(need, g_pkt_sizes[1]); /* 邻帧不受污染 */
    tc_movie_close(mv2);
    mem_free(&trunc);

    /* 坏 atom size：moov 尺寸越过文件尾 → MALFORMED */
    mem_file bad;
    bad.data = (uint8_t*)malloc(f.len);
    bad.cap = f.len;
    bad.len = f.len;
    memcpy(bad.data, f.data, f.len);
    {
        uint8_t* moov = (uint8_t*)memmem(bad.data, bad.len, "moov", 4u);
        MT_CHECK(moov != NULL);
        if (moov != NULL) {
            tc_store_be32(moov - 4, (uint32_t)(bad.len - (size_t)(moov - 4 - bad.data) + 16u));
        }
        topos_movie* mv3 = NULL;
        topos_io bio;
        mem_io_src(&bad, &bio);
        MT_CHECK_EQ_I64(tc_movie_open(&bio, &mv3), TC_ERR_MALFORMED);
    }

    /* tpcC CRC 破坏：定位 'tpcC' 改一字节 → CHECKSUM_MISMATCH */
    mem_file bad2;
    bad2.data = (uint8_t*)malloc(f.len);
    bad2.cap = f.len;
    bad2.len = f.len;
    memcpy(bad2.data, f.data, f.len);
    {
        uint8_t* p = (uint8_t*)memmem(bad2.data, bad2.len, "tpcC", 4u);
        MT_CHECK(p != NULL);
        p[12] ^= 0x40u; /* 载荷区 */
        topos_movie* mv4 = NULL;
        topos_io bio2;
        mem_io_src(&bad2, &bio2);
        MT_CHECK_EQ_I64(tc_movie_open(&bio2, &mv4), TC_ERR_CHECKSUM_MISMATCH);
        mem_free(&bad2);
    }
    mem_free(&bad);

    mem_free(&f);
    mem_free(&fs);
}

static void test_mux_rejections(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);

    /* sink 镜像不符 / 缺 seek_write */
    topos_io bad_sink;
    mem_file mf;
    memset(&mf, 0, sizeof(mf));
    mem_io_sink(&mf, &bad_sink);
    bad_sink.seek_write = NULL;
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &bad_sink, &m), TC_ERR_INVALID_ARGUMENT);
    bad_sink.seek_write = mem_seek_write;
    bad_sink.struct_size = 4u;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &bad_sink, &m), TC_ERR_INVALID_ARGUMENT);
    bad_sink.struct_size = (uint32_t)sizeof(topos_io);

    /* 配置非法（timescale 越界） */
    topos_movie_config bad_cfg = cfg;
    bad_cfg.timescale = 2000000u;
    MT_CHECK_EQ_I64(tc_mux_create(&bad_cfg, &bad_sink, &m), TC_ERR_INVALID_ARGUMENT);

    /* 正常创建后：pts 乱序拒绝 / 非法 packet 拒绝 / 不一致 packet 拒绝 */
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &bad_sink, &m), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[0], g_pkt_sizes[0], 5u, 1u),
                    TC_ERR_INVALID_ARGUMENT); /* 首帧 pts 必须 0 */
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[0], g_pkt_sizes[0], 0u, 0u),
                    TC_ERR_INVALID_ARGUMENT); /* dur=0 */
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[0], 3u, 0u, 1u),
                    TC_ERR_TRUNCATED);        /* 3 字节非 packet */
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[0], g_pkt_sizes[0], 0u, 1u), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[0], g_pkt_sizes[0], 3u, 1u),
                    TC_ERR_INVALID_ARGUMENT); /* pts 必须 1 */
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[0], g_pkt_sizes[0], 2u, 1u),
                    TC_ERR_STATE);            /* finish 后拒收 */
    tc_mux_free(m);
    tc_mux_free(NULL);
    mem_free(&mf);

    /* 与电影配置不一致的 packet（改 bit_depth → 另一份 packet） */
    {
        topos_frame_config fc;
        memset(&fc, 0, sizeof(fc));
        fc.struct_size = (uint32_t)sizeof(fc);
        fc.abi_version = TOPOS_CODEC_ABI_VERSION;
        fc.visible_width = 64u;
        fc.visible_height = 40u;
        fc.profile = 3u;
        fc.bit_depth = 10u;
        fc.qmatrix_id = 1u;
        fc.qp_base = 30u;
        fc.color_range = 1u;
        fc.color_primaries = 1u;
        fc.color_transfer = 1u;
        fc.color_matrix = 1u;
        image_synth_cfg sc;
        memset(&sc, 0, sizeof(sc));
        sc.width = 64u;
        sc.height = 40u;
        sc.kind = TC_SYNTH_FLAT;
        uint16_t* pl[4] = { NULL, NULL, NULL, NULL };
        image_synth_alloc(&sc, 0, &pl[0], &pl[1], &pl[2], &pl[3]);
        topos_frame_input in;
        memset(&in, 0, sizeof(in));
        in.struct_size = (uint32_t)sizeof(in);
        in.abi_version = TOPOS_CODEC_ABI_VERSION;
        in.planes[0] = pl[0];
        in.planes[1] = pl[1];
        in.planes[2] = pl[2];
        size_t bound = tc_frame_packet_bound(&fc);
        uint8_t* buf = (uint8_t*)malloc(bound);
        topos_frame_stats st;
        MT_CHECK_EQ_I64(tc_frame_encode(&fc, &in, buf, bound, &st), TC_OK);

        mem_file mf2;
        memset(&mf2, 0, sizeof(mf2));
        topos_io sink2;
        mem_io_sink(&mf2, &sink2);
        topos_movie_config other = cfg;
        other.qmatrix_id = 0u; /* 与 packet(1) 不一致 */
        topos_mux* m2 = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&other, &sink2, &m2), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_add_packet(m2, buf, st.packet_size, 0u, 1u),
                        TC_ERR_INVALID_ARGUMENT);
        tc_mux_free(m2);
        free(buf);
        free(pl[0]);
        free(pl[1]);
        free(pl[2]);
        mem_free(&mf2);
    }
}

static void test_open_rejections(void)
{
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(NULL, &mv), TC_ERR_INVALID_ARGUMENT);
    topos_io io;
    memset(&io, 0, sizeof(io));
    io.struct_size = (uint32_t)sizeof(io);
    io.abi_version = TOPOS_CODEC_ABI_VERSION;
    io.read = mem_read;
    mem_file tiny;
    memset(&tiny, 0, sizeof(tiny));
    mem_io_src(&tiny, &io);
    MT_CHECK_EQ_I64(tc_movie_open(&io, &mv), TC_ERR_TRUNCATED); /* < 20B */

    /* 空 movie（0 帧 finish）→ open 拒绝（空 stts） */
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file empty;
    memset(&empty, 0, sizeof(empty));
    topos_io sink;
    mem_io_sink(&empty, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    tc_mux_free(m);
    topos_io eio;
    mem_io_src(&empty, &eio);
    MT_CHECK_EQ_I64(tc_movie_open(&eio, &mv), TC_ERR_MALFORMED);
    mem_free(&empty);

    /* info/packet 参数校验 */
    topos_movie_config cfg2;
    base_movie_cfg(&cfg2);
    mem_file f;
    mux_all(&f, &cfg2);
    topos_io fio;
    mem_io_src(&f, &fio);
    MT_CHECK_EQ_I64(tc_movie_open(&fio, &mv), TC_OK);
    topos_movie_info info;
    MT_CHECK_EQ_I64(tc_movie_info(mv, &info), TC_ERR_INVALID_ARGUMENT); /* 未初始化镜像 */
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_info(mv, &info), TC_OK);
    MT_CHECK_EQ_I64(tc_movie_packet(mv, TEST_FRAMES, NULL, 0, NULL),
                    TC_ERR_INVALID_ARGUMENT);
    MT_CHECK_EQ_I64(tc_movie_packet_pts(mv, 999u, NULL, NULL),
                    TC_ERR_INVALID_ARGUMENT);
    tc_movie_close(mv);
    tc_movie_close(NULL);
    mem_free(&f);
    mem_free(&tiny);
}

static void test_write_failure_path(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file f;
    memset(&f, 0, sizeof(f));
    topos_io sink;
    mem_io_sink(&f, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
    f.fail_write = 1;
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[0], g_pkt_sizes[0], 0u, 1u), TC_ERR_IO);
    /* P1-15：失败写后 mux 中毒——恢复 sink 也只可 free：
     * 同 PTS retry 与 finish 一律 TC_ERR_STATE（旧语义为伪恢复 TC_ERR_IO） */
    f.fail_write = 0;
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[0], g_pkt_sizes[0], 0u, 1u),
                    TC_ERR_STATE);
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_ERR_STATE);
    tc_mux_free(m);
    mem_free(&f);
}

static void test_co64_and_layout_rules(void)
{
    /* 小文件必须用 stco（无 co64 字样）且 mdat 64 位头在 ftyp 后：
     * [size=1 @20][mdat @24][u64 长度 @28] */
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file f;
    mux_all(&f, &cfg);
    MT_CHECK_EQ_U64(tc_load_be32(f.data + 20u), 1u);          /* 64 位 size 形式 */
    MT_CHECK(memcmp(f.data + 24u, "mdat", 4u) == 0);
    int has_co64 = 0;
    for (size_t k = 0; k + 4u <= f.len; ++k) {
        if (memcmp(f.data + k, "co64", 4u) == 0) { has_co64 = 1; break; }
    }
    MT_CHECK(has_co64 == 0);
    uint64_t mdat_size = tc_load_be64(f.data + 28u);
    uint64_t payload = 0;
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) { payload += g_pkt_sizes[i]; }
    MT_CHECK_EQ_U64(mdat_size, payload + 16u); /* 回填正确（16B 头） */
    mem_free(&f);
}

/* ---------- R3：tpcB（alpha 预算元数据）---------- */

/* mode2 a12 带 alpha 的确定性包（image_synth alpha 值域 0..65535 = 16-bit 容器域） */
static uint8_t* g_apkts[TEST_FRAMES];
static size_t g_apkt_sizes[TEST_FRAMES];

static void build_alpha_packets(void)
{
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
        topos_frame_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.struct_size = (uint32_t)sizeof(cfg);
        cfg.abi_version = TOPOS_CODEC_ABI_VERSION;
        cfg.visible_width = 64u;
        cfg.visible_height = 40u;
        cfg.profile = 3u;
        cfg.bit_depth = 10u;
        cfg.qmatrix_id = 1u;
        cfg.qp_base = 28u;
        cfg.alpha_mode = 2u;
        cfg.alpha_bit_depth = 12u;
        cfg.color_range = 1u;
        cfg.color_primaries = 1u;
        cfg.color_transfer = 1u;
        cfg.color_matrix = 1u;
        cfg.sar_num = 1u;
        cfg.sar_den = 1u;

        image_synth_cfg sc;
        memset(&sc, 0, sizeof(sc));
        sc.width = 64u;
        sc.height = 40u;
        sc.kind = (tc_synth_kind)(i % TC_SYNTH_KIND_COUNT);
        sc.seed = 500u + (uint64_t)i;
        uint16_t* pl[4] = { NULL, NULL, NULL, NULL };
        MT_CHECK_EQ_I64(image_synth_alloc(&sc, 1, &pl[0], &pl[1], &pl[2], &pl[3]), 0);

        topos_frame_input in;
        memset(&in, 0, sizeof(in));
        in.struct_size = (uint32_t)sizeof(in);
        in.abi_version = TOPOS_CODEC_ABI_VERSION;
        in.planes[0] = pl[0];
        in.planes[1] = pl[1];
        in.planes[2] = pl[2];
        in.planes[3] = pl[3];

        size_t bound = tc_frame_packet_bound(&cfg);
        uint8_t* buf = (uint8_t*)malloc(bound);
        topos_frame_stats st;
        MT_CHECK_EQ_I64(tc_frame_encode(&cfg, &in, buf, bound, &st), TC_OK);
        g_apkts[i] = buf;
        g_apkt_sizes[i] = st.packet_size;
        free(pl[0]);
        free(pl[1]);
        free(pl[2]);
        free(pl[3]);
    }
}

static void mux_all_alpha_budget(mem_file* out, const topos_movie_config* cfg,
                                 const topos_alpha_budget_info* bud)
{
    memset(out, 0, sizeof(*out));
    topos_io sink;
    mem_io_sink(out, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(cfg, &sink, &m), TC_OK);
    int32_t rc = TC_OK;
    for (uint32_t i = 0; i < TEST_FRAMES && rc == TC_OK; ++i) {
        rc = tc_mux_add_packet(m, g_apkts[i], g_apkt_sizes[i], (uint64_t)i, 1u);
    }
    MT_CHECK_EQ_I64(rc, TC_OK);
    if (bud != NULL) { MT_CHECK_EQ_I64(tc_mux_set_alpha_budget(m, bud), TC_OK); }
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    tc_mux_free(m);
}

static size_t find_bytes(const mem_file* f, const char* tag)
{
    size_t n = strlen(tag);
    for (size_t k = 0; k + n <= f->len; ++k) {
        if (memcmp(f->data + k, tag, n) == 0) { return k; }
    }
    return (size_t)-1;
}

static void test_alpha_budget_atom(void)
{
    build_alpha_packets();
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg.alpha_mode = 2u;
    cfg.alpha_bit_depth = 12u;

    topos_alpha_budget_info bi;
    memset(&bi, 0, sizeof(bi));
    bi.struct_size = (uint32_t)sizeof(bi);
    bi.abi_version = TOPOS_CODEC_ABI_VERSION;
    bi.target_ratio_bp = 2500u;
    bi.actual_ratio_bp = 2417u;
    bi.max_abs_error = 9u;
    bi.flags = TC_ALPHA_BUDGET_FLAG_ADAPTED;
    bi.frame_count = TEST_FRAMES;

    /* 1) 默认不写 tpcB；读侧显式"无预算记录"（旧文件兼容） */
    mem_file f;
    mux_all_alpha_budget(&f, &cfg, NULL);
    MT_CHECK(find_bytes(&f, "tpcB") == (size_t)-1);
    topos_io src;
    mem_io_src(&f, &src);
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
    topos_alpha_budget_info rb;
    memset(&rb, 0, sizeof(rb));
    rb.struct_size = (uint32_t)sizeof(rb);
    rb.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_alpha_budget(mv, &rb), TC_ERR_STATE);
    tc_movie_close(mv);
    mem_free(&f);

    /* 2) set → tpcB 写入 + 全字段读回 */
    mux_all_alpha_budget(&f, &cfg, &bi);
    MT_CHECK(find_bytes(&f, "tpcB") != (size_t)-1);
    mem_io_src(&f, &src);
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
    MT_CHECK_EQ_I64(tc_movie_alpha_budget(mv, &rb), TC_OK);
    MT_CHECK_EQ_U64(rb.target_ratio_bp, 2500u);
    MT_CHECK_EQ_U64(rb.actual_ratio_bp, 2417u);
    MT_CHECK_EQ_U64(rb.max_abs_error, 9u);
    MT_CHECK_EQ_U64(rb.flags, TC_ALPHA_BUDGET_FLAG_ADAPTED);
    MT_CHECK_EQ_U64(rb.frame_count, (uint64_t)TEST_FRAMES);
    tc_movie_close(mv);
    mem_free(&f);

    /* 3) frame_count 与 sample 数不一致 → MALFORMED（tpcC 式一致性） */
    topos_alpha_budget_info bad = bi;
    bad.frame_count = TEST_FRAMES + 1u;
    mux_all_alpha_budget(&f, &cfg, &bad);
    mem_io_src(&f, &src);
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_ERR_MALFORMED);
    mem_free(&f);

    /* 4) tpcB CRC 破坏 → CHECKSUM_MISMATCH（载荷基址 = atom 类型偏移 +4） */
    mux_all_alpha_budget(&f, &cfg, &bi);
    size_t at = find_bytes(&f, "tpcB");
    MT_CHECK(at != (size_t)-1 && at + 4u + 11u < f.len);
    f.data[at + 4u + 11u] ^= 0x01u;   /* actual_ratio_bp 高字节（CRC 覆盖域） */
    mem_io_src(&f, &src);
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_ERR_CHECKSUM_MISMATCH);
    mem_free(&f);

    /* 5) tpcB 与 tpcC 的 mode/depth 不一致 → MALFORMED（同步修复 CRC 以越过校验和） */
    mux_all_alpha_budget(&f, &cfg, &bi);
    at = find_bytes(&f, "tpcB");
    f.data[at + 4u + 6u] = 1u;        /* alpha_mode 位改 mode1 */
    tc_store_be32(f.data + at + 4u + 20u,
                  tc_crc32(f.data + at + 4u, 20u));   /* 重算 tpcB CRC */
    mem_io_src(&f, &src);
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_ERR_MALFORMED);
    mem_free(&f);

    /* 6) finish 后 set → STATE；无 alpha 电影 set → INVALID_ARGUMENT */
    memset(&f, 0, sizeof(f));
    topos_io sink;
    mem_io_sink(&f, &sink);
    topos_mux* m = NULL;
    topos_movie_config base;
    base_movie_cfg(&base);
    MT_CHECK_EQ_I64(tc_mux_create(&base, &sink, &m), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[0], g_pkt_sizes[0], 0u, 1u), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_set_alpha_budget(m, &bi), TC_ERR_INVALID_ARGUMENT);
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_set_alpha_budget(m, &bi), TC_ERR_STATE);
    tc_mux_free(m);
    mem_free(&f);

    for (uint32_t i = 0; i < TEST_FRAMES; ++i) { free(g_apkts[i]); }
}

/* v1.7：电影元数据 tpcD（档位/厂商）——写入/读回/faststart 保留/畸形拒绝 */
static void test_movie_meta_atom(void)
{
    build_test_packets();
    topos_movie_config cfg;
    base_movie_cfg(&cfg);

    topos_movie_meta meta;
    memset(&meta, 0, sizeof(meta));
    meta.struct_size = (uint32_t)sizeof(meta);
    meta.abi_version = TOPOS_CODEC_ABI_VERSION;
    meta.tier_id = TC_TIER_HQ;
    memcpy(meta.vendor, "TOPOS", 5u);
    memcpy(meta.label, "Topos 422 HQ", 12u);

    /* 1) 默认不写 tpcD；读侧 reserved[1] = TC_TIER_NONE（旧文件兼容） */
    mem_file f;
    memset(&f, 0, sizeof(f));
    {
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
        for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
            MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[i], g_pkt_sizes[i],
                                              (uint64_t)i, 1u), TC_OK);
        }
        MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
        tc_mux_free(m);
    }
    MT_CHECK(find_bytes(&f, "tpcD") == (size_t)-1);
    topos_io src;
    mem_io_src(&f, &src);
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
    topos_movie_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_info(mv, &info), TC_OK);
    MT_CHECK_EQ_U64(info.reserved[1], TC_TIER_NONE);
    tc_movie_close(mv);
    mem_free(&f);

    /* 2) set → tpcD 写入 + reserved[1] 读回 + faststart 原样保留 */
    memset(&f, 0, sizeof(f));
    {
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
        for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
            MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[i], g_pkt_sizes[i],
                                              (uint64_t)i, 1u), TC_OK);
        }
        MT_CHECK_EQ_I64(tc_mux_set_movie_meta(m, &meta), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
        tc_mux_free(m);
    }
    MT_CHECK(find_bytes(&f, "tpcD") != (size_t)-1);
    mem_io_src(&f, &src);
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
    MT_CHECK_EQ_I64(tc_movie_info(mv, &info), TC_OK);
    MT_CHECK_EQ_U64(info.reserved[1], TC_TIER_HQ);
    mem_file f2;
    memset(&f2, 0, sizeof(f2));
    topos_io dst;
    mem_io_sink(&f2, &dst);
    MT_CHECK_EQ_I64(tc_movie_faststart(&src, &dst), TC_OK);
    tc_movie_close(mv);
    mem_free(&f);
    topos_io src2;
    mem_io_src(&f2, &src2);
    MT_CHECK_EQ_I64(tc_movie_open(&src2, &mv), TC_OK);
    MT_CHECK_EQ_I64(tc_movie_info(mv, &info), TC_OK);
    MT_CHECK_EQ_U64(info.reserved[1], TC_TIER_HQ);
    tc_movie_close(mv);
    mem_free(&f2);

    /* 2b) v1.8 LP 档（tier 7，帧间微 GOP）roundtrip——写/读/域边界 */
    memset(&f, 0, sizeof(f));
    {
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
        for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
            MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[i], g_pkt_sizes[i],
                                              (uint64_t)i, 1u), TC_OK);
        }
        topos_movie_meta lp = meta;
        lp.tier_id = TC_TIER_LP;
        memset(lp.label, 0, sizeof(lp.label));
        memcpy(lp.label, "Topos 422 LP", 12u);
        MT_CHECK_EQ_I64(tc_mux_set_movie_meta(m, &lp), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
        tc_mux_free(m);
    }
    mem_io_src(&f, &src);
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
    MT_CHECK_EQ_I64(tc_movie_info(mv, &info), TC_OK);
    MT_CHECK_EQ_U64(info.reserved[1], TC_TIER_LP);
    tc_movie_close(mv);
    mem_free(&f);

    /* 3) setter 校验：镜像不符 / tier 越界 / reserved 非零 / 串超长 /
     *    合法声明 → finish 后再 set = STATE */
    memset(&f, 0, sizeof(f));
    {
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[0], g_pkt_sizes[0], 0u, 1u),
                        TC_OK);
        topos_movie_meta bad = meta;
        bad.struct_size = 0u;
        MT_CHECK_EQ_I64(tc_mux_set_movie_meta(m, &bad), TC_ERR_INVALID_ARGUMENT);
        bad = meta;
        bad.tier_id = TC_TIER_RAW + 1u;
        MT_CHECK_EQ_I64(tc_mux_set_movie_meta(m, &bad), TC_ERR_INVALID_ARGUMENT);
        bad = meta;
        bad.reserved[2] = 1u;
        MT_CHECK_EQ_I64(tc_mux_set_movie_meta(m, &bad), TC_ERR_INVALID_ARGUMENT);
        bad = meta;
        memset(bad.label, 'a', sizeof(bad.label));  /* 无 NUL → strlen = 64 超长 */
        MT_CHECK_EQ_I64(tc_mux_set_movie_meta(m, &bad), TC_ERR_INVALID_ARGUMENT);
        bad = meta;
        memset(bad.vendor, 'v', sizeof(bad.vendor)); /* strlen = 16 超长 */
        MT_CHECK_EQ_I64(tc_mux_set_movie_meta(m, &bad), TC_ERR_INVALID_ARGUMENT);
        MT_CHECK_EQ_I64(tc_mux_set_movie_meta(m, &meta), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_set_movie_meta(m, &meta), TC_ERR_STATE);
        tc_mux_free(m);
        mem_free(&f);
    }

    /* 4) tpcD CRC 破坏 → CHECKSUM_MISMATCH（vendor 串域翻位，CRC 覆盖域内；
     *    载荷基址 = atom 类型偏移 +4，vendor 起于载荷偏移 12） */
    memset(&f, 0, sizeof(f));
    {
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
        for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
            MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[i], g_pkt_sizes[i],
                                              (uint64_t)i, 1u), TC_OK);
        }
        MT_CHECK_EQ_I64(tc_mux_set_movie_meta(m, &meta), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
        tc_mux_free(m);
    }
    size_t at = find_bytes(&f, "tpcD");
    MT_CHECK(at != (size_t)-1 && at + 4u + 13u < f.len);
    f.data[at + 4u + 13u] ^= 0x01u;
    mem_io_src(&f, &src);
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_ERR_CHECKSUM_MISMATCH);
    mem_free(&f);

    /* 5) tpcD version=2 → MALFORMED（version 严格 =1，先于 CRC 校验） */
    memset(&f, 0, sizeof(f));
    {
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
        for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
            MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[i], g_pkt_sizes[i],
                                              (uint64_t)i, 1u), TC_OK);
        }
        MT_CHECK_EQ_I64(tc_mux_set_movie_meta(m, &meta), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
        tc_mux_free(m);
    }
    at = find_bytes(&f, "tpcD");
    MT_CHECK(at != (size_t)-1 && at + 4u + 5u < f.len);
    f.data[at + 4u + 5u] = 2u;
    mem_io_src(&f, &src);
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_ERR_MALFORMED);
    mem_free(&f);

    /* 6) 解析侧 tier 域边界：tier=TC_TIER_RAW 合法（v1.9，M4-R5）；tier
     *    越界（RAW+1=9，域检查先于 CRC）→ MALFORMED */
    memset(&f, 0, sizeof(f));
    {
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
        for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
            MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[i], g_pkt_sizes[i],
                                              (uint64_t)i, 1u), TC_OK);
        }
        MT_CHECK_EQ_I64(tc_mux_set_movie_meta(m, &meta), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
        tc_mux_free(m);
    }
    at = find_bytes(&f, "tpcD");
    MT_CHECK(at != (size_t)-1 && at + 4u + 6u < f.len);
    f.data[at + 4u + 6u] = (uint8_t)(TC_TIER_RAW + 1u);
    mem_io_src(&f, &src);
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_ERR_MALFORMED);
    mem_free(&f);
}

/* M4-R5（v1.9，2026-09-21）：视频 RAW 单 tier（TC_TIER_RAW=8）容器级
 * 往返——写/读/域边界（RAW+1=9 拒绝）；纯域扩展，既有 golden 逐字节
 * 不变（LP 先例 ADR-C056）。 */
static void test_traw_tier_roundtrip(void)
{
    topos_movie_config cfg;
    topos_movie_info info;
    topos_movie* mv = NULL;
    mem_file f;
    topos_io src;
    size_t at;

    base_movie_cfg(&cfg);

    topos_movie_meta meta;
    memset(&meta, 0, sizeof(meta));
    meta.struct_size = (uint32_t)sizeof(meta);
    meta.abi_version = TOPOS_CODEC_ABI_VERSION;
    meta.tier_id = TC_TIER_HQ;
    memcpy(meta.vendor, "TOPOS", 5u);
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;

    /* tier=8 写/读 */
    memset(&f, 0, sizeof(f));
    {
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
        for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
            MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[i], g_pkt_sizes[i],
                                              (uint64_t)i, 1u), TC_OK);
        }
        topos_movie_meta raw = meta;
        raw.tier_id = TC_TIER_RAW;
        memset(raw.label, 0, sizeof(raw.label));
        const char* lbl = "Topos RAW 12-bit 4:1";
        memcpy(raw.label, lbl, strlen(lbl));
        MT_CHECK_EQ_I64(tc_mux_set_movie_meta(m, &raw), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
        tc_mux_free(m);
    }
    mem_io_src(&f, &src);
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
    MT_CHECK_EQ_I64(tc_movie_info(mv, &info), TC_OK);
    MT_CHECK_EQ_U64(info.reserved[1], TC_TIER_RAW);
    tc_movie_close(mv);
    mem_free(&f);

    /* 解析侧边界：tier=RAW+1=9 → MALFORMED（域检查先于 CRC） */
    memset(&f, 0, sizeof(f));
    {
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* m = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
        for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
            MT_CHECK_EQ_I64(tc_mux_add_packet(m, g_pkts[i], g_pkt_sizes[i],
                                              (uint64_t)i, 1u), TC_OK);
        }
        MT_CHECK_EQ_I64(tc_mux_set_movie_meta(m, &meta), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
        tc_mux_free(m);
    }
    at = find_bytes(&f, "tpcD");
    MT_CHECK(at != (size_t)-1 && at + 4u + 6u < f.len);
    f.data[at + 4u + 6u] = (uint8_t)(TC_TIER_RAW + 1u);
    mem_io_src(&f, &src);
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_ERR_MALFORMED);
    mem_free(&f);
}

/* R4.2：4:4:4（pf=1）容器级往返——tpcC pixel_format 一致性双向验证 */
static void test_pf444_mux_roundtrip(void)
{
    /* pf=1 包（frame 编码出口：帧头 minor=2 + U/V 全宽） */
    topos_frame_config fc;
    memset(&fc, 0, sizeof(fc));
    fc.struct_size = (uint32_t)sizeof(fc);
    fc.abi_version = TOPOS_CODEC_ABI_VERSION;
    fc.visible_width = 48u;
    fc.visible_height = 32u;
    fc.profile = 3u;
    fc.pixel_format = 1u;
    fc.bit_depth = 10u;
    fc.qmatrix_id = 1u;
    fc.qp_base = 24u;
    fc.color_range = 1u;
    fc.color_primaries = 1u;
    fc.color_transfer = 1u;
    fc.color_matrix = 1u;
    fc.sar_num = 1u;
    fc.sar_den = 1u;

    image_synth_cfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.width = 48u;
    sc.height = 32u;
    sc.kind = TC_SYNTH_MIXED;
    sc.seed = 0x50F3434343434343ull;
    sc.chroma_format = 1u;
    uint16_t* pl[4] = { NULL, NULL, NULL, NULL };
    MT_CHECK_EQ_I64(image_synth_alloc(&sc, 0, &pl[0], &pl[1], &pl[2], &pl[3]), 0);
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = TOPOS_CODEC_ABI_VERSION;
    in.planes[0] = pl[0];
    in.planes[1] = pl[1];
    in.planes[2] = pl[2];
    size_t bound = tc_frame_packet_bound(&fc);
    uint8_t* pkt = (uint8_t*)malloc(bound);
    topos_frame_stats st;
    MT_CHECK_EQ_I64(tc_frame_encode(&fc, &in, pkt, bound, &st), TC_OK);
    MT_CHECK_EQ_U64(pkt[7], 2ull);   /* v1.3 minor（pf=1 扩展代） */
    MT_CHECK_EQ_U64(pkt[11], 1ull);  /* pf=1 入头 */

    /* 1) pf=1 mux → open → packet 取回解码：U 全宽 */
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg.visible_width = 48u;
    cfg.visible_height = 32u;
    cfg.pixel_format = 1u;
    cfg.qp_base = 24u;
    mem_file f;
    memset(&f, 0, sizeof(f));
    topos_io sink;
    mem_io_sink(&f, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, pkt, st.packet_size, 0u, 1u), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    tc_mux_free(m);

    topos_io src;
    mem_io_src(&f, &src);
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
    topos_movie_info mi;
    memset(&mi, 0, sizeof(mi));
    mi.struct_size = (uint32_t)sizeof(mi);
    mi.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_info(mv, &mi), TC_OK);
    MT_CHECK_EQ_U64(mi.pixel_format, 1ull);   /* tpcC 回读 */
    size_t rn = 0;
    MT_CHECK_EQ_I64(tc_movie_packet(mv, 0u, NULL, 0u, &rn), TC_ERR_BUFFER_TOO_SMALL);
    uint8_t* rp = (uint8_t*)malloc(rn);
    MT_CHECK(rp != NULL);
    MT_CHECK_EQ_I64(tc_movie_packet(mv, 0u, rp, rn, NULL), TC_OK);
    topos_frame_output info;
    MT_CHECK_EQ_I64(tc_frame_decode(rp, rn, NULL, NULL, &info), TC_OK);
    MT_CHECK_EQ_U64(info.pixel_format, 1ull);
    uint32_t w = 0, h = 0;
    MT_CHECK_EQ_I64(tc_frame_plane_geometry(&info, 1u, &w, &h), TC_OK);
    MT_CHECK_EQ_U64(w, 48ull); /* 4:4:4：U = 全宽 */
    free(rp);
    tc_movie_close(mv);
    mem_free(&f);

    /* 2) tpcC 不一致：pf=0 电影收 pf=1 包 → INVALID_ARGUMENT */
    topos_movie_config c0;
    base_movie_cfg(&c0);
    c0.visible_width = 48u;
    c0.visible_height = 32u;
    c0.pixel_format = 0u;
    memset(&f, 0, sizeof(f));
    mem_io_sink(&f, &sink);
    MT_CHECK_EQ_I64(tc_mux_create(&c0, &sink, &m), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, pkt, st.packet_size, 0u, 1u),
                    TC_ERR_INVALID_ARGUMENT);
    tc_mux_free(m);
    mem_free(&f);

    free(pkt);
    free(pl[0]);
    free(pl[1]);
    free(pl[2]);
}

/* R4.3：GBR（pf=2）容器级往返——tpcC pixel_format/matrix 双向一致性 */
static void test_gbr_mux_roundtrip(void)
{
    /* pf=2 包（matrix=0 identity / siting=0 / 帧头 minor=3） */
    topos_frame_config fc;
    memset(&fc, 0, sizeof(fc));
    fc.struct_size = (uint32_t)sizeof(fc);
    fc.abi_version = TOPOS_CODEC_ABI_VERSION;
    fc.visible_width = 48u;
    fc.visible_height = 32u;
    fc.profile = 3u;
    fc.pixel_format = 2u;
    fc.bit_depth = 10u;
    fc.qmatrix_id = 1u;
    fc.qp_base = 24u;
    fc.color_range = 1u;
    fc.color_primaries = 1u;
    fc.color_transfer = 1u;
    fc.color_matrix = 0u;   /* cfg 显式 0 + pf=2 → identity（cfg_to_frame_header
                              * pf 感知默认；显式 YUV 矩阵会被拒） */
    fc.sar_num = 1u;
    fc.sar_den = 1u;

    image_synth_cfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.width = 48u;
    sc.height = 32u;
    sc.kind = TC_SYNTH_MIXED;
    sc.seed = 0x1162162162162162ull;
    sc.chroma_format = 1u;   /* GBR 三平面全宽 */
    uint16_t* pl[4] = { NULL, NULL, NULL, NULL };
    MT_CHECK_EQ_I64(image_synth_alloc(&sc, 0, &pl[0], &pl[1], &pl[2], &pl[3]), 0);
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = TOPOS_CODEC_ABI_VERSION;
    in.planes[0] = pl[0];
    in.planes[1] = pl[1];
    in.planes[2] = pl[2];
    size_t bound = tc_frame_packet_bound(&fc);
    uint8_t* pkt = (uint8_t*)malloc(bound);
    topos_frame_stats st;
    MT_CHECK_EQ_I64(tc_frame_encode(&fc, &in, pkt, bound, &st), TC_OK);
    MT_CHECK_EQ_U64(pkt[7], 3ull);   /* v1.4 minor（pf=2 扩展代） */
    MT_CHECK_EQ_U64(pkt[11], 2ull);  /* pf=2 入头 */
    MT_CHECK_EQ_U64(pkt[35], 0ull);  /* matrix=0 identity */

    /* 1) GBR mux → open → 解码：三平面全宽 */
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg.visible_width = 48u;
    cfg.visible_height = 32u;
    cfg.pixel_format = 2u;
    cfg.qp_base = 24u;
    cfg.color_matrix = 0u;
    mem_file f;
    memset(&f, 0, sizeof(f));
    topos_io sink;
    mem_io_sink(&f, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, pkt, st.packet_size, 0u, 1u), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    tc_mux_free(m);

    topos_io src;
    mem_io_src(&f, &src);
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
    topos_movie_info mi;
    memset(&mi, 0, sizeof(mi));
    mi.struct_size = (uint32_t)sizeof(mi);
    mi.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_info(mv, &mi), TC_OK);
    MT_CHECK_EQ_U64(mi.pixel_format, 2ull);   /* tpcC 回读 */
    MT_CHECK_EQ_U64(mi.color_matrix, 0ull);
    size_t rn = 0;
    MT_CHECK_EQ_I64(tc_movie_packet(mv, 0u, NULL, 0u, &rn), TC_ERR_BUFFER_TOO_SMALL);
    uint8_t* rp = (uint8_t*)malloc(rn);
    MT_CHECK(rp != NULL);
    MT_CHECK_EQ_I64(tc_movie_packet(mv, 0u, rp, rn, NULL), TC_OK);
    topos_frame_output info;
    MT_CHECK_EQ_I64(tc_frame_decode(rp, rn, NULL, NULL, &info), TC_OK);
    MT_CHECK_EQ_U64(info.pixel_format, 2ull);
    uint32_t w = 0, h = 0;
    MT_CHECK_EQ_I64(tc_frame_plane_geometry(&info, 1u, &w, &h), TC_OK);
    MT_CHECK_EQ_U64(w, 48ull); /* GBR：B 平面全宽 */
    free(rp);
    tc_movie_close(mv);
    mem_free(&f);

    /* 2) 交叉规则前置：matrix=1 + pf=2 的电影配置在 mux_create 即被拒
     * （tc_frame_config_validate 的 matrix 交叉规则，先于任何包写入） */
    topos_movie_config cm;
    base_movie_cfg(&cm);
    cm.visible_width = 48u;
    cm.visible_height = 32u;
    cm.pixel_format = 2u;
    cm.color_matrix = 1u;
    memset(&f, 0, sizeof(f));
    mem_io_sink(&f, &sink);
    m = NULL;   /* create 失败不写 *out（内部已释放） */
    MT_CHECK_EQ_I64(tc_mux_create(&cm, &sink, &m), TC_ERR_MALFORMED);
    MT_CHECK(m == NULL);
    mem_free(&f);

    free(pkt);
    free(pl[0]);
    free(pl[1]);
    free(pl[2]);
}

/* R4.4：Pro444（profile=5）容器级往返——tpcC profile 一致性双向验证 */
static void test_pro444_mux_roundtrip(void)
{
    topos_frame_config fc;
    memset(&fc, 0, sizeof(fc));
    fc.struct_size = (uint32_t)sizeof(fc);
    fc.abi_version = TOPOS_CODEC_ABI_VERSION;
    fc.visible_width = 48u;
    fc.visible_height = 32u;
    fc.profile = 5u;             /* Pro444 */
    fc.pixel_format = 1u;
    fc.bit_depth = 10u;
    fc.qmatrix_id = 1u;
    fc.qp_base = 24u;
    fc.color_range = 1u;
    fc.color_primaries = 1u;
    fc.color_transfer = 1u;
    fc.color_matrix = 1u;
    fc.sar_num = 1u;
    fc.sar_den = 1u;

    image_synth_cfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.width = 48u;
    sc.height = 32u;
    sc.kind = TC_SYNTH_MIXED;
    sc.seed = 0x50524F3434342121ull;
    sc.chroma_format = 1u;
    uint16_t* pl[4] = { NULL, NULL, NULL, NULL };
    MT_CHECK_EQ_I64(image_synth_alloc(&sc, 0, &pl[0], &pl[1], &pl[2], &pl[3]), 0);
    topos_frame_input in;
    memset(&in, 0, sizeof(in));
    in.struct_size = (uint32_t)sizeof(in);
    in.abi_version = TOPOS_CODEC_ABI_VERSION;
    in.planes[0] = pl[0];
    in.planes[1] = pl[1];
    in.planes[2] = pl[2];
    size_t bound = tc_frame_packet_bound(&fc);
    uint8_t* pkt = (uint8_t*)malloc(bound);
    topos_frame_stats st;
    MT_CHECK_EQ_I64(tc_frame_encode(&fc, &in, pkt, bound, &st), TC_OK);
    MT_CHECK_EQ_U64(pkt[10], 5ull);  /* profile=5 入头 */

    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg.visible_width = 48u;
    cfg.visible_height = 32u;
    cfg.profile = 5u;
    cfg.pixel_format = 1u;
    cfg.qp_base = 24u;
    mem_file f;
    memset(&f, 0, sizeof(f));
    topos_io sink;
    mem_io_sink(&f, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, pkt, st.packet_size, 0u, 1u), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    tc_mux_free(m);

    topos_io src;
    mem_io_src(&f, &src);
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&src, &mv), TC_OK);
    topos_movie_info mi;
    memset(&mi, 0, sizeof(mi));
    mi.struct_size = (uint32_t)sizeof(mi);
    mi.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_info(mv, &mi), TC_OK);
    MT_CHECK_EQ_U64(mi.profile, 5ull);    /* tpcC 回读 */
    size_t rn = 0;
    MT_CHECK_EQ_I64(tc_movie_packet(mv, 0u, NULL, 0u, &rn), TC_ERR_BUFFER_TOO_SMALL);
    uint8_t* rp = (uint8_t*)malloc(rn);
    MT_CHECK(rp != NULL);
    MT_CHECK_EQ_I64(tc_movie_packet(mv, 0u, rp, rn, NULL), TC_OK);
    topos_frame_output info;
    MT_CHECK_EQ_I64(tc_frame_decode(rp, rn, NULL, NULL, &info), TC_OK);
    MT_CHECK_EQ_U64(info.profile, 5ull);
    free(rp);
    tc_movie_close(mv);
    mem_free(&f);

    /* tpcC 不一致：Standard 电影收 Pro444 包 → INVALID_ARGUMENT */
    topos_movie_config c3;
    base_movie_cfg(&c3);
    c3.visible_width = 48u;
    c3.visible_height = 32u;
    memset(&f, 0, sizeof(f));
    mem_io_sink(&f, &sink);
    MT_CHECK_EQ_I64(tc_mux_create(&c3, &sink, &m), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_add_packet(m, pkt, st.packet_size, 0u, 1u),
                    TC_ERR_INVALID_ARGUMENT);
    tc_mux_free(m);
    mem_free(&f);

    free(pkt);
    free(pl[0]);
    free(pl[1]);
    free(pl[2]);
}

/* ================= R5：MOV 与媒体元数据收口 ================= */
/* fixtures：手工 co64（双布局）、四级未知 atom 跳过、损坏 sample 表逐类拒绝、
 * VFR 多 run 展开、SAR/colr 权威源（tpcC）、长时文件 O(1) 索引。
 * 测试侧定位器是独立于 reader 的迷你 atom walker（oracle 思想下沉到单测）。 */

typedef struct {
    uint64_t size;   /* 含头 */
    uint64_t body;   /* body 起始偏移 */
    char type[5];
    int ext;         /* 64 位 size 形式 */
} t_atom;

static int t_read_atom(const mem_file* f, uint64_t off, uint64_t limit, t_atom* a)
{
    if (off + 8ull > (uint64_t)f->len || off + 8ull > limit) { return -1; }
    uint32_t sz = tc_load_be32(f->data + off);
    memcpy(a->type, f->data + off + 4u, 4u);
    a->type[4] = '\0';
    if (sz == 1u) {
        if (off + 16ull > (uint64_t)f->len || off + 16ull > limit) { return -1; }
        a->size = tc_load_be64(f->data + off + 8u);
        a->body = off + 16ull;
        a->ext = 1;
    } else {
        if (sz < 8u) { return -1; }
        a->size = sz;
        a->body = off + 8ull;
        a->ext = 0;
    }
    if (a->size == 0ull || off + a->size > (uint64_t)f->len ||
        off + a->size > limit) {
        return -1;
    }
    return 0;
}

/* 容器 body 范围 [body,end) 内按 fourcc 找直接子 atom 头偏移 */
static uint64_t t_find_child(const mem_file* f, uint64_t body, uint64_t end,
                             const char* fourcc)
{
    uint64_t o = body;
    while (o + 8ull <= end) {
        t_atom a;
        if (t_read_atom(f, o, end, &a) != 0) { return UINT64_MAX; }
        if (memcmp(a.type, fourcc, 4u) == 0) { return o; }
        o += a.size;
    }
    return UINT64_MAX;
}

/* 沿 "moov:trak:mdia:minf:stbl[:stsd:TPIC]" 逐级下钻；
 * anc[] 记录沿途各级头偏移（供 size 补丁），返回末级头偏移。 */
static uint64_t t_chain(const mem_file* f, const char* path,
                        uint64_t* anc, int* n_anc)
{
    uint64_t o = 0u, end = (uint64_t)f->len, last = UINT64_MAX;
    int n = 0;
    const char* p = path;
    while (*p != '\0') {
        char fourcc[5];
        memcpy(fourcc, p, 4u);
        fourcc[4] = '\0';
        uint64_t c = t_find_child(f, o, end, fourcc);
        if (c == UINT64_MAX) { return UINT64_MAX; }
        t_atom a;
        if (t_read_atom(f, c, end, &a) != 0) { return UINT64_MAX; }
        anc[n++] = c;
        last = c;
        o = a.body;
        end = c + a.size;
        p += 4u;
        if (*p == ':') { ++p; } else { break; } /* 末段无冒号 */
    }
    *n_anc = n;
    return last;
}

/* stsd entry 定位辅助：stsd body 先是 8B ver/flags+entry_count，再是 entry */
static uint64_t t_find_child_ex(const mem_file* f, const char* path,
                                const char* leaf, uint64_t* anc, int* n_anc)
{
    /* 沿 path 下钻记录祖先，然后在末级容器内跳过 stsd 头 8B 找 leaf */
    uint64_t at = t_chain(f, path, anc, n_anc);
    if (at == UINT64_MAX) { return UINT64_MAX; }
    t_atom pa;
    if (t_read_atom(f, at, (uint64_t)f->len, &pa) != 0) { return UINT64_MAX; }
    uint64_t child = pa.body;
    if (memcmp(pa.type, "stsd", 4u) == 0) { child += 8u; }
    t_atom la;
    if (t_read_atom(f, child, at + pa.size, &la) != 0) { return UINT64_MAX; }
    if (memcmp(la.type, leaf, 4u) != 0) { return UINT64_MAX; }
    anc[(*n_anc)++] = child;
    return child;
}

/* 在 path 容器 body 起点插入 8B 未知 atom（JUNK），沿途祖先 size+8；
 * "top" = 文件尾追加；末级 "stsd-entry" = stsd entry 固定字段后。 */
static int t_insert_junk(const mem_file* f, const char* path, mem_file* out)
{
    memset(out, 0, sizeof(*out));
    out->data = (uint8_t*)malloc(f->len + 8u);
    out->cap = f->len + 8u;
    if (out->data == NULL) { return -1; }
    if (strcmp(path, "top") == 0) {
        memcpy(out->data, f->data, f->len);
        tc_store_be32(out->data + f->len, 8u);
        memcpy(out->data + f->len + 4u, "JUNK", 4u);
        out->len = f->len + 8u;
        return 0;
    }
    if (strcmp(path, "stsd-entry") == 0) {
        /* stsd entry 固定字段后插入：祖先含 stsd 与 entry 自身 */
        uint64_t anc2[8];
        int n2 = 0;
        uint64_t ent = t_find_child_ex(f, "moov:trak:mdia:minf:stbl:stsd", "TPIC",
                                       anc2, &n2);
        if (ent == UINT64_MAX) { free(out->data); memset(out, 0, sizeof(*out)); return -1; }
        t_atom ea;
        if (t_read_atom(f, ent, (uint64_t)f->len, &ea) != 0) { return -1; }
        uint64_t insert_at = ea.body + 78u;
        memcpy(out->data, f->data, insert_at);
        tc_store_be32(out->data + insert_at, 8u);
        memcpy(out->data + insert_at + 4u, "JUNK", 4u);
        memcpy(out->data + insert_at + 8u, f->data + insert_at, f->len - insert_at);
        out->len = f->len + 8u;
        for (int i = 0; i < n2; ++i) {
            uint32_t sz = tc_load_be32(out->data + anc2[i]);
            tc_store_be32(out->data + anc2[i], sz + 8u);
        }
        return 0;
    }
    uint64_t anc[8];
    int n = 0;
    uint64_t at = t_chain(f, path, anc, &n);
    if (at == UINT64_MAX) { free(out->data); memset(out, 0, sizeof(*out)); return -1; }
    t_atom a;
    if (t_read_atom(f, at, (uint64_t)f->len, &a) != 0) { return -1; }
    uint64_t insert_at = a.body;
    if (memcmp(a.type, "TPIC", 4u) == 0) {
        insert_at += 78u; /* 固定字段后 */
    }
    memcpy(out->data, f->data, insert_at);
    tc_store_be32(out->data + insert_at, 8u);
    memcpy(out->data + insert_at + 4u, "JUNK", 4u);
    memcpy(out->data + insert_at + 8u, f->data + insert_at, f->len - insert_at);
    out->len = f->len + 8u;
    for (int i = 0; i < n; ++i) { /* 祖先均在插入点之前，偏移不变 */
        uint32_t sz = tc_load_be32(out->data + anc[i]);
        tc_store_be32(out->data + anc[i], sz + 8u);
    }
    return 0;
}

/* 手工 stco → co64（条目 4B→8B 原值拓宽，沿途祖先 size += 4*count）。
 * shift_mdat=1（快启布局：mdat 在 moov 后）时条目值同步 +delta。 */
static int t_upgrade_co64(const mem_file* f, int shift_mdat, mem_file* out)
{
    memset(out, 0, sizeof(*out));
    uint64_t anc[8];
    int n = 0;
    uint64_t stbl = t_chain(f, "moov:trak:mdia:minf:stbl", anc, &n);
    if (stbl == UINT64_MAX) { return -1; }
    t_atom stbl_a;
    if (t_read_atom(f, stbl, (uint64_t)f->len, &stbl_a) != 0) { return -1; }
    uint64_t stco = t_find_child(f, stbl_a.body, stbl + stbl_a.size, "stco");
    if (stco == UINT64_MAX) { return -1; }
    t_atom sa;
    if (t_read_atom(f, stco, stbl + stbl_a.size, &sa) != 0) { return -1; }
    uint32_t count = tc_load_be32(f->data + stco + 12u);
    if (count == 0u || sa.size != 16ull + (uint64_t)count * 4ull) { return -1; }
    uint64_t delta = (uint64_t)count * 4ull;
    out->cap = (size_t)(f->len + delta);
    out->data = (uint8_t*)malloc(out->cap);
    if (out->data == NULL) { return -1; }
    memcpy(out->data, f->data, stco);
    tc_store_be32(out->data + stco, (uint32_t)(sa.size + delta));
    memcpy(out->data + stco + 4u, "co64", 4u);
    memcpy(out->data + stco + 8u, f->data + stco + 8u, 8u); /* ver/flags + count */
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t v = tc_load_be32(f->data + stco + 16u + (uint64_t)i * 4ull);
        if (shift_mdat) { v += delta; }
        tc_store_be64(out->data + stco + 16u + (uint64_t)i * 8ull, v);
    }
    memcpy(out->data + stco + sa.size + delta, f->data + stco + sa.size,
           (size_t)((uint64_t)f->len - (stco + sa.size)));
    out->len = (size_t)(f->len + delta);
    for (int i = 0; i < n; ++i) {
        uint32_t sz = tc_load_be32(out->data + anc[i]);
        tc_store_be32(out->data + anc[i], sz + (uint32_t)delta);
    }
    return 0;
}

static void open_and_verify_all(const mem_file* f)
{
    topos_movie* mv = NULL;
    topos_io io;
    mem_io_src(f, &io);
    MT_CHECK_EQ_I64(tc_movie_open(&io, &mv), TC_OK);
    if (mv != NULL) {
        verify_movie(mv);
        tc_movie_close(mv);
    }
}

static void expect_open_malformed(const mem_file* f)
{
    topos_movie* mv = (topos_movie*)0x1;
    topos_io io;
    mem_io_src(f, &io);
    MT_CHECK_EQ_I64(tc_movie_open(&io, &mv), TC_ERR_MALFORMED);
    MT_CHECK(mv == NULL);
}

/* fourcc atom 头（size 字段在类型前 4B）内字段覆写；field_off 相对类型偏移 */
static void patch_u32(mem_file* f, const char* fourcc, uint64_t field_off, uint32_t val)
{
    size_t at = find_bytes(f, fourcc);
    MT_CHECK(at != (size_t)-1);
    if (at != (size_t)-1) { tc_store_be32(f->data + at + field_off, val); }
}

/* R5-1：手工 co64 读回（标准 + 快启双布局）与 faststart 重选 stco */
static void test_r5_hand_co64(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);

    /* 标准布局（mdat 在 moov 前）：条目值不变，仅拓宽 */
    mem_file f;
    mux_all(&f, &cfg);
    mem_file g;
    MT_CHECK(t_upgrade_co64(&f, 0, &g) == 0);
    MT_CHECK(find_bytes(&g, "co64") != (size_t)-1);
    MT_CHECK(find_bytes(&g, "stco") == (size_t)-1);
    open_and_verify_all(&g);

    /* faststart(手工 co64) → 按新偏移重选 stco（降级合法，spec §3 冻结规则） */
    {
        mem_file fs;
        memset(&fs, 0, sizeof(fs));
        topos_io src, dst;
        mem_io_src(&g, &src);
        mem_io_sink(&fs, &dst);
        MT_CHECK_EQ_I64(tc_movie_faststart(&src, &dst), TC_OK);
        MT_CHECK(find_bytes(&fs, "co64") == (size_t)-1);
        open_and_verify_all(&fs);
        mem_free(&fs);
    }
    mem_free(&g);
    mem_free(&f);

    /* 快启布局 + 手工 co64：moov 变宽使 mdat 后移 delta，条目值同步平移 */
    mem_file std2, fs2;
    mux_all(&std2, &cfg);
    {
        topos_io src, dst;
        mem_io_src(&std2, &src);
        memset(&fs2, 0, sizeof(fs2));
        mem_io_sink(&fs2, &dst);
        MT_CHECK_EQ_I64(tc_movie_faststart(&src, &dst), TC_OK);
    }
    mem_free(&std2);
    MT_CHECK(t_upgrade_co64(&fs2, 1, &g) == 0);
    MT_CHECK(find_bytes(&g, "co64") != (size_t)-1);
    open_and_verify_all(&g);
    mem_free(&g);
    mem_free(&fs2);
}

/* R5-2：未知 atom 四级跳过（顶层/moov/stbl/stsd entry） */
static void test_r5_unknown_atoms(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file f, g;
    mux_all(&f, &cfg);
    static const char* kPaths[] = {
        "top",
        "moov",
        "moov:trak:mdia:minf:stbl",
        "stsd-entry",
    };
    for (size_t i = 0; i < sizeof(kPaths) / sizeof(kPaths[0]); ++i) {
        MT_CHECK(t_insert_junk(&f, kPaths[i], &g) == 0);
        open_and_verify_all(&g);
        mem_free(&g);
    }
    mem_free(&f);
}

/* R5-3：损坏 sample 表逐类拒绝（每类独立命中一条校验分支） */
static void test_r5_corrupt_sample_tables(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file f;

    /* stsz 计数 -1：与 stts 展开不一致 */
    mux_all(&f, &cfg);
    patch_u32(&f, "stsz", 12u, TEST_FRAMES - 1u);
    expect_open_malformed(&f);
    mem_free(&f);

    /* stsz 计数 +1：表越界（atom 长度不匹配） */
    mux_all(&f, &cfg);
    patch_u32(&f, "stsz", 12u, TEST_FRAMES + 1u);
    expect_open_malformed(&f);
    mem_free(&f);

    /* stsc samples_per_chunk=2：stco 条目耗尽前 sample 数不齐 */
    mux_all(&f, &cfg);
    patch_u32(&f, "stsc", 16u, 2u);
    expect_open_malformed(&f);
    mem_free(&f);

    /* stco 条目数 -1：展开中途 stco 不足 */
    mux_all(&f, &cfg);
    patch_u32(&f, "stco", 8u, TEST_FRAMES - 1u);
    expect_open_malformed(&f);
    mem_free(&f);

    /* stss 索引越界（> sample 数） */
    mux_all(&f, &cfg);
    patch_u32(&f, "stss", 12u, TEST_FRAMES + 1u);
    expect_open_malformed(&f);
    mem_free(&f);

    /* stts delta=0 */
    mux_all(&f, &cfg);
    patch_u32(&f, "stts", 16u, 0u);
    expect_open_malformed(&f);
    mem_free(&f);
}

/* R5-4：VFR 多 run 展开 + SAR/colr 权威源（tpcC）+ pasp 条件写 */
static void test_r5_vfr_sar_authority(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg.sar_num = 5u;
    cfg.sar_den = 4u;

    /* packet 帧头携带 sar，须与电影配置一致（tpcC 一致性规则）——
     * 本地编码一个 sar=5/4 的包，7 帧复用 */
    topos_frame_config fc;
    memset(&fc, 0, sizeof(fc));
    fc.struct_size = (uint32_t)sizeof(fc);
    fc.abi_version = TOPOS_CODEC_ABI_VERSION;
    fc.visible_width = 64u;
    fc.visible_height = 40u;
    fc.profile = 3u;
    fc.qmatrix_id = 1u;
    fc.qp_base = 28u;
    fc.color_range = 1u;
    fc.color_primaries = 1u;
    fc.color_transfer = 1u;
    fc.color_matrix = 1u;
    fc.sar_num = 5u;
    fc.sar_den = 4u;
    image_synth_cfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.width = 64u;
    sc.height = 40u;
    sc.kind = TC_SYNTH_MIXED;
    sc.seed = 0x5656524E31323334ull;
    uint16_t* pl[4] = { NULL, NULL, NULL, NULL };
    MT_CHECK_EQ_I64(image_synth_alloc(&sc, 0, &pl[0], &pl[1], &pl[2], &pl[3]), 0);
    topos_frame_input fin;
    memset(&fin, 0, sizeof(fin));
    fin.struct_size = (uint32_t)sizeof(fin);
    fin.abi_version = TOPOS_CODEC_ABI_VERSION;
    fin.planes[0] = pl[0];
    fin.planes[1] = pl[1];
    fin.planes[2] = pl[2];
    size_t bound = tc_frame_packet_bound(&fc);
    uint8_t* pkt = (uint8_t*)malloc(bound);
    topos_frame_stats st;
    MT_CHECK_EQ_I64(tc_frame_encode(&fc, &fin, pkt, bound, &st), TC_OK);

    mem_file f;
    memset(&f, 0, sizeof(f));
    topos_io sink;
    mem_io_sink(&f, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
    int32_t rc = TC_OK;
    uint64_t pts = 0u;
    for (uint32_t i = 0; i < TEST_FRAMES && rc == TC_OK; ++i) {
        uint32_t dur = (i % 3u) + 1u;
        rc = tc_mux_add_packet(m, pkt, st.packet_size, pts, dur);
        pts += dur;
    }
    MT_CHECK_EQ_I64(rc, TC_OK);
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    tc_mux_free(m);

    topos_movie_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    topos_movie* mv = NULL;
    topos_io io;
    mem_io_src(&f, &io);
    MT_CHECK_EQ_I64(tc_movie_open(&io, &mv), TC_OK);
    MT_CHECK_EQ_I64(tc_movie_info(mv, &info), TC_OK);
    MT_CHECK_EQ_U64(info.sar_num, 5u);
    MT_CHECK_EQ_U64(info.sar_den, 4u);
    /* VFR 逐帧展开（dur 1,2,3 循环；与 writer 端 RLE 合并方式无关） */
    uint64_t exp_pts = 0u;
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
        uint64_t p = 99u;
        uint32_t d = 0u;
        MT_CHECK_EQ_I64(tc_movie_packet_pts(mv, i, &p, &d), TC_OK);
        MT_CHECK_EQ_U64(p, exp_pts);
        MT_CHECK_EQ_U64(d, (uint64_t)((i % 3u) + 1u));
        exp_pts += (i % 3u) + 1u;
    }
    tc_movie_close(mv);

    /* 权威源：pasp 改 9:16、colr primaries 改 9 → 读侧仍取 tpcC（5:4 / 1） */
    MT_CHECK(find_bytes(&f, "pasp") != (size_t)-1);
    {
        size_t at = find_bytes(&f, "pasp");
        tc_store_be32(f.data + at + 4u, 9u);   /* hSpacing */
        tc_store_be32(f.data + at + 8u, 16u);  /* vSpacing */
    }
    {
        size_t at = find_bytes(&f, "colr");
        MT_CHECK(at != (size_t)-1);
        tc_store_be16(f.data + at + 8u, 9u);   /* primaries u16（nclc 后） */
    }
    mem_io_src(&f, &io);
    mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&io, &mv), TC_OK);
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_info(mv, &info), TC_OK);
    MT_CHECK_EQ_U64(info.sar_num, 5u);
    MT_CHECK_EQ_U64(info.sar_den, 4u);
    MT_CHECK_EQ_U64(info.color_primaries, 1u);
    tc_movie_close(mv);
    mem_free(&f);

    /* sar=1:1 不写 pasp（spec §4 条件写） */
    topos_movie_config cfg1;
    base_movie_cfg(&cfg1);
    mem_file base;
    mux_all(&base, &cfg1);
    MT_CHECK(find_bytes(&base, "pasp") == (size_t)-1);
    mem_free(&base);

    free(pkt);
    free(pl[0]);
    free(pl[1]);
    free(pl[2]);
}

/* R5-5：长时文件（2000 帧 VFR 多 run）——O(1) 索引与随机访问 */
#define LONG_FRAMES 2000u

static void test_r5_long_file(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file f;
    memset(&f, 0, sizeof(f));
    topos_io sink;
    mem_io_sink(&f, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
    int32_t rc = TC_OK;
    uint64_t pts = 0u;
    for (uint32_t i = 0; i < LONG_FRAMES && rc == TC_OK; ++i) {
        uint32_t dur = (i % 3u) + 1u;
        rc = tc_mux_add_packet(m, g_pkts[0], g_pkt_sizes[0], pts, dur);
        pts += dur;
    }
    MT_CHECK_EQ_I64(rc, TC_OK);
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    tc_mux_free(m);

    topos_movie* mv = NULL;
    topos_io io;
    mem_io_src(&f, &io);
    MT_CHECK_EQ_I64(tc_movie_open(&io, &mv), TC_OK);
    topos_movie_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_info(mv, &info), TC_OK);
    MT_CHECK_EQ_U64(info.sample_count, LONG_FRAMES);
    MT_CHECK(info.index_bytes <= LONG_FRAMES * 26u); /* 线性有界（≈25B/帧） */

    /* VFR pts 公式：每 3 帧一循环累计 6 tick */
    static const uint32_t kRem[3] = { 0u, 1u, 3u };
    static const uint32_t kSpot[] = { 0u, 1u, 2u, 999u, 1000u, LONG_FRAMES - 1u };
    for (size_t s = 0; s < sizeof(kSpot) / sizeof(kSpot[0]); ++s) {
        uint32_t i = kSpot[s];
        uint64_t p = 99u;
        uint32_t d = 0u;
        MT_CHECK_EQ_I64(tc_movie_packet_pts(mv, i, &p, &d), TC_OK);
        MT_CHECK_EQ_U64(p, (uint64_t)(i / 3u) * 6u + kRem[i % 3u]);
        MT_CHECK_EQ_U64(d, (uint64_t)((i % 3u) + 1u));
        size_t need = 0u;
        MT_CHECK_EQ_I64(tc_movie_packet(mv, i, NULL, 0, &need), TC_ERR_BUFFER_TOO_SMALL);
        MT_CHECK_EQ_U64(need, g_pkt_sizes[0]);
        uint8_t* buf = (uint8_t*)malloc(need);
        MT_CHECK(buf != NULL);
        if (buf != NULL) {
            MT_CHECK_EQ_I64(tc_movie_packet(mv, i, buf, need, NULL), TC_OK);
            MT_CHECK(memcmp(buf, g_pkts[0], need) == 0);
            free(buf);
        }
    }
    tc_movie_close(mv);
    mem_free(&f);
}

/* ===================== O3（复验 2026-08-31）：容器边界 ===================== */

/* P1-12（最小复现 6）：mdat FourCC 改 JUNK 的文件必须拒绝；stco 指到
 * mdat payload 之外（moov 区/文件尾）必须拒绝。 */
static void test_o3_mdat_required_and_containment(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file f;
    mux_all(&f, &cfg);

    /* ① mdat → JUNK：顶层 walk 只见 ftyp+JUNK+moov，缺 mdat */
    {
        mem_file g;
        g.cap = f.len; g.len = f.len;
        g.data = (uint8_t*)malloc(g.cap);
        memcpy(g.data, f.data, f.len);
        size_t at = find_bytes(&g, "mdat");
        MT_CHECK(at != (size_t)-1);
        memcpy(g.data + at, "JUNK", 4u);
        expect_open_malformed(&g);
        mem_free(&g);
    }

    /* ② stco 首条目 → 文件尾（moov 之后，仍在文件界内但不在 mdat payload） */
    {
        mem_file g;
        g.cap = f.len; g.len = f.len;
        g.data = (uint8_t*)malloc(g.cap);
        memcpy(g.data, f.data, f.len);
        size_t stco = find_bytes(&g, "stco");
        MT_CHECK(stco != (size_t)-1);
        tc_store_be32(g.data + stco + 12u, (uint32_t)g.len);
        expect_open_malformed(&g);
        mem_free(&g);
    }

    /* ③ stco 首条目 → ftyp 区（文件界内、mdat 外） */
    {
        mem_file g;
        g.cap = f.len; g.len = f.len;
        g.data = (uint8_t*)malloc(g.cap);
        memcpy(g.data, f.data, f.len);
        size_t stco = find_bytes(&g, "stco");
        tc_store_be32(g.data + stco + 12u, 4u);
        expect_open_malformed(&g);
        mem_free(&g);
    }

    /* ④ 基线未篡改文件仍可开（防过紧校验误伤） */
    open_and_verify_all(&f);
    mem_free(&f);
}

/* 真实 >4 GiB 稀疏文件 + 手工 co64（P1-13）：reader 在 64 位偏移上
 * 正确读回 packet。布局：ftyp(20) + mdat ext 头(16) + 稀疏洞 + payload
 * @4GiB+36 + moov（t_upgrade_co64 拓宽，条目再 +4GiB）。 */
#define O3_GI4 (0x100000000ull)

static int g_o3_fd = -1;

static int32_t o3_fd_read(void* ctx, uint64_t off, void* dst, size_t len)
{
    (void)ctx;
    uint8_t* d = (uint8_t*)dst;
    while (len != 0u) {
        size_t chunk = len > (1u << 30) ? (1u << 30) : len;
        /* 显式 64 位偏移：Windows off_t 为 32 位，o3 co64 偏移 >4 GiB 会被截断 */
        ssize_t n = pread(g_o3_fd, d, chunk, (long long)off);
        if (n <= 0) { return TC_ERR_IO; }
        d += n;
        off += (uint64_t)n;
        len -= (size_t)n;
    }
    return TC_OK;
}

static void test_o3_co64_over_4g_sparse(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file f;
    mux_all(&f, &cfg);

    /* ① 拓宽为 co64（祖先 size 已由 t_upgrade_co64 修正） */
    mem_file g;
    MT_CHECK(t_upgrade_co64(&f, 0, &g) == 0);
    MT_CHECK(find_bytes(&g, "co64") != (size_t)-1);

    /* ② 布局常量：payload = [36, moov_off)；拓宽后 moov = [moov_off+grow, g.len) */
    uint64_t moov_off = 0u;
    for (uint64_t o = 0u; o + 8ull <= (uint64_t)f.len;) {
        uint32_t sz = tc_load_be32(f.data + o);
        uint64_t size = (sz == 1u) ? tc_load_be64(f.data + o + 8ull) : sz;
        if (memcmp(f.data + o + 4u, "moov", 4u) == 0) { moov_off = o; break; }
        o += size;
    }
    MT_CHECK(moov_off > 36ull);
    uint64_t payload_off = 36ull;
    uint64_t payload_len = moov_off - payload_off;

    /* ③ co64 条目 += 4GiB（原条目指向 [36, 36+payload_len)；
     *    新 payload 绝对位置 = 4GiB + 36 → 新条目 = 原值 + 4GiB） */
    size_t co64_at = find_bytes(&g, "co64");
    uint32_t count = tc_load_be32(g.data + co64_at + 8u);
    for (uint32_t i = 0u; i < count; ++i) {
        uint64_t v = tc_load_be64(g.data + co64_at + 12u + (uint64_t)i * 8ull);
        tc_store_be64(g.data + co64_at + 12u + (uint64_t)i * 8ull, v + O3_GI4);
    }

    /* ④ 写稀疏文件 */
    char path_buf[128];
#if defined(_WIN32)
    /* MinGW 原生程序没有 MSYS 的 /tmp 映射，退到 %TEMP% */
    const char* o3_tmp = getenv("TEMP");
    snprintf(path_buf, sizeof(path_buf), "%s\\topos_o3_co64_%d.mov",
             (o3_tmp != NULL ? o3_tmp : "."), (int)getpid());
#else
    snprintf(path_buf, sizeof(path_buf), "/tmp/topos_o3_co64_%d.mov", (int)getpid());
#endif
    int fd = open(path_buf, O_CREAT | O_TRUNC | O_WRONLY | O_BINARY, 0600);
    MT_CHECK(fd >= 0);
    if (fd < 0) { mem_free(&g); mem_free(&f); return; }
    uint64_t payload_abs = O3_GI4 + 36ull;
    /* 拓宽只发生在 moov 内部：moov 起点不变（此前误 +grow 导致布局错位） */
    uint64_t moov_g_off = moov_off;
    uint64_t moov_g_len = (uint64_t)g.len - moov_g_off;
    uint64_t moov_abs = payload_abs + payload_len;
    /* 写 ftyp(20) + mdat ext 头(16) */
    uint8_t hdr[36];
    memcpy(hdr, f.data, 20u);
    tc_store_be32(hdr + 20, 1u);
    memcpy(hdr + 24, "mdat", 4u);
    tc_store_be64(hdr + 28, moov_abs - 20ull); /* mdat 总大小（含 16B 头） */
    MT_CHECK(port_fd_write(fd, hdr, 36) == 36);
    /* 稀疏洞后写 payload 与 moov（>4GiB 偏移必须 64 位 lseek，Windows off_t 截断） */
    MT_CHECK(port_lseek(fd, (long long)payload_abs, SEEK_SET)
             == (long long)payload_abs);
    MT_CHECK(port_fd_write(fd, f.data + payload_off, (size_t)payload_len)
             == (ssize_t)payload_len);
    MT_CHECK(port_lseek(fd, (long long)moov_abs, SEEK_SET)
             == (long long)moov_abs);
    MT_CHECK(port_fd_write(fd, g.data + moov_g_off, (size_t)moov_g_len)
             == (ssize_t)moov_g_len);
    port_fd_close(fd);

    /* ⑤ reader：open + 首包逐字节比对 */
    int rfd = open(path_buf, O_RDONLY | O_BINARY);
    MT_CHECK(rfd >= 0);
    if (rfd >= 0) {
        g_o3_fd = rfd;
        topos_io io;
        memset(&io, 0, sizeof(io));
        io.struct_size = (uint32_t)sizeof(topos_io);
        io.abi_version = TOPOS_CODEC_ABI_VERSION;
        io.ctx = NULL;
        io.read = o3_fd_read;
        io.length = moov_abs + moov_g_len;
        topos_movie* mv = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&io, &mv), TC_OK);
        if (mv != NULL) {
            size_t need = 0u;
            MT_CHECK_EQ_I64(
                tc_movie_packet(mv, 0u, NULL, 0u, &need), TC_ERR_BUFFER_TOO_SMALL);
            MT_CHECK_EQ_U64(need, g_pkt_sizes[0]);
            uint8_t* buf = (uint8_t*)malloc(need);
            MT_CHECK(buf != NULL);
            if (buf != NULL) {
                MT_CHECK_EQ_I64(tc_movie_packet(mv, 0u, buf, need, NULL), TC_OK);
                MT_CHECK(memcmp(buf, g_pkts[0], need) == 0);
                free(buf);
            }
            tc_movie_close(mv);
        }
        port_fd_close(rfd);
        g_o3_fd = -1;
    }
    remove(path_buf);
    mem_free(&g);
    mem_free(&f);
}

/* P1-13：mvhd/tkhd/mdhd v0 duration 饱和路径确定性验证
 *（4 × dur=0xFFFFFFFF，timescale=1000 → total≈2^34 超 u32 → 0xFFFFFFFF）。 */
static void test_o3_writer_duration_saturation(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    cfg.timescale = 1000u;
    mem_file f;
    memset(&f, 0, sizeof(f));
    topos_io sink;
    mem_io_sink(&f, &sink);
    topos_mux* m = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &m), TC_OK);
    int32_t rc = TC_OK;
    uint64_t pts = 0u;
    for (uint32_t i = 0u; i < 4u && rc == TC_OK; ++i) {
        rc = tc_mux_add_packet(m, g_pkts[i], g_pkt_sizes[i], pts, 0xFFFFFFFFu);
        pts += 0xFFFFFFFFull; /* pts 必须连续累计（无 edit list 布局） */
    }
    MT_CHECK_EQ_I64(rc, TC_OK);
    MT_CHECK_EQ_I64(tc_mux_finish(m), TC_OK);
    tc_mux_free(m);

    /* mvhd duration（type+20）＝ total*1000/1000 = ~2^34 → 饱和 0xFFFFFFFF */
    size_t mvhd = find_bytes(&f, "mvhd");
    MT_CHECK(mvhd != (size_t)-1);
    if (mvhd != (size_t)-1) {
        MT_CHECK(tc_load_be32(f.data + mvhd + 20u) == 0xFFFFFFFFu);
    }
    /* tkhd v0 duration（type+20）/ mdhd duration（type+16）同饱和 */
    /* tkhd v0：ver/flags+creation+modification+track_ID+reserved 后 duration
     * 在 type+24；mdhd：timescale 在 type+16，duration 在 type+20 */
    size_t tkhd = find_bytes(&f, "tkhd");
    MT_CHECK(tkhd != (size_t)-1);
    if (tkhd != (size_t)-1) {
        MT_CHECK(tc_load_be32(f.data + tkhd + 24u) == 0xFFFFFFFFu);
    }
    size_t mdhd = find_bytes(&f, "mdhd");
    MT_CHECK(mdhd != (size_t)-1);
    if (mdhd != (size_t)-1) {
        MT_CHECK(tc_load_be32(f.data + mdhd + 20u) == 0xFFFFFFFFu);
    }
    mem_free(&f);
}

/* ---------- 批量包读取（阶段4 兑现：读前聚读 arena） ---------- */

static int g_batch_io_reads; /* 计数 shim：断言连续合并/非连续逐包分支 */

static int32_t counting_mem_read(void* ctx, uint64_t off, void* buf, size_t len)
{
    g_batch_io_reads++;
    return mem_read(ctx, off, buf, len);
}

static void counting_io_src(mem_file* f, topos_io* io)
{
    mem_io_src(f, io);
    io->read = counting_mem_read;
}

static void test_movie_packet_batch(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file f;
    mux_all(&f, &cfg);
    topos_io io;
    counting_io_src(&f, &io);
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&io, &mv), TC_OK);

    size_t offs[TEST_FRAMES], sizes[TEST_FRAMES], need = 0;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) { sum += g_pkt_sizes[i]; }

    /* 探测：arena NULL / cap 不足 → BUFFER_TOO_SMALL + 精确总量 + 紧凑布局 */
    memset(offs, 0xAA, sizeof(offs));
    memset(sizes, 0xAA, sizeof(sizes));
    MT_CHECK_EQ_I64(tc_movie_packet_batch(mv, 0, TEST_FRAMES, NULL, 0,
                                          offs, sizes, &need),
                    TC_ERR_BUFFER_TOO_SMALL);
    MT_CHECK_EQ_U64(need, sum);
    size_t prefix = 0;
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
        MT_CHECK_EQ_U64(offs[i], prefix);
        MT_CHECK_EQ_U64(sizes[i], g_pkt_sizes[i]);
        prefix += g_pkt_sizes[i];
    }
    uint8_t* arena = (uint8_t*)malloc(need ? need : 1u);
    MT_CHECK(arena != NULL);
    MT_CHECK_EQ_I64(tc_movie_packet_batch(mv, 0, TEST_FRAMES, arena, need - 1u,
                                          NULL, NULL, NULL),
                    TC_ERR_BUFFER_TOO_SMALL);

    /* 全区间：本库布局恒连续 → 单次 io 读；逐包 memcmp 等价单帧读取 */
    g_batch_io_reads = 0;
    MT_CHECK_EQ_I64(tc_movie_packet_batch(mv, 0, TEST_FRAMES, arena, need,
                                          offs, sizes, NULL), TC_OK);
    MT_CHECK_EQ_I64(g_batch_io_reads, 1);
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
        MT_CHECK(memcmp(arena + offs[i], g_pkts[i], g_pkt_sizes[i]) == 0);
    }

    /* 子区间 [2,5) 同样合并为单读 */
    size_t need2 = 0;
    MT_CHECK_EQ_I64(tc_movie_packet_batch(mv, 2, 3, NULL, 0, NULL, NULL, &need2),
                    TC_ERR_BUFFER_TOO_SMALL);
    MT_CHECK_EQ_U64(need2, g_pkt_sizes[2] + g_pkt_sizes[3] + g_pkt_sizes[4]);
    g_batch_io_reads = 0;
    MT_CHECK_EQ_I64(tc_movie_packet_batch(mv, 2, 3, arena, need, NULL, NULL, NULL),
                    TC_OK);
    MT_CHECK_EQ_I64(g_batch_io_reads, 1);
    for (uint32_t i = 2; i < 5; ++i) {
        size_t o = 0;
        for (uint32_t k = 2; k < i; ++k) { o += g_pkt_sizes[k]; }
        MT_CHECK(memcmp(arena + o, g_pkts[i], g_pkt_sizes[i]) == 0);
    }

    /* count==0 → TC_OK（其余指针不触碰）；区间越界 → INVALID_ARGUMENT */
    MT_CHECK_EQ_I64(tc_movie_packet_batch(mv, 0, 0, NULL, 0, NULL, NULL, NULL),
                    TC_OK);
    MT_CHECK_EQ_I64(tc_movie_packet_batch(mv, TEST_FRAMES, 1, arena, need,
                                          NULL, NULL, NULL),
                    TC_ERR_INVALID_ARGUMENT);
    MT_CHECK_EQ_I64(tc_movie_packet_batch(mv, TEST_FRAMES - 1u, 2, arena, need,
                                          NULL, NULL, NULL),
                    TC_ERR_INVALID_ARGUMENT);
    tc_movie_close(mv);

    /* 非连续布局（外来交错模拟）：sample 0/1 之间插 13B 缝，stco 1+ 平移，
     * mdat u64 长度 +13 —— 全区间走逐包分支（读次数 = count）；缝后子区间
     * [1,7) 恢复连续 → 单读。单帧读取不受缝影响（补丁正确性自证） */
    {
        size_t stco_at = find_bytes(&f, "stco");
        MT_CHECK(stco_at != (size_t)-1);
        if (stco_at != (size_t)-1) {
            /* stco atom：size(4)+type(4)+version_flags(4)+entry_count(4)+entries；
             * find_bytes 命中 type 字段 → count @+8，条目 i @+12+4i */
            MT_CHECK_EQ_U64(tc_load_be32(f.data + stco_at + 8u), TEST_FRAMES);
            uint32_t entry1 = tc_load_be32(f.data + stco_at + 12u + 4u);
            const uint32_t gap = 13u;
            mem_file g;
            g.len = f.len + gap;
            g.cap = g.len;
            g.data = (uint8_t*)malloc(g.cap);
            MT_CHECK(g.data != NULL);
            if (g.data != NULL) {
                memcpy(g.data, f.data, entry1);
                memset(g.data + entry1, 0, gap);
                memcpy(g.data + entry1 + gap, f.data + entry1, f.len - entry1);
                /* 插缝点在 moov 之前 → g 内 stco 平移 gap；mdat ext 头在缝前不动 */
                for (uint32_t i = 1; i < TEST_FRAMES; ++i) {
                    uint32_t v = tc_load_be32(f.data + stco_at + 12u + 4u * i);
                    tc_store_be32(g.data + stco_at + gap + 12u + 4u * i, v + gap);
                }
                /* 标准布局 mdat ext 头 @20：[size=1][mdat][u64 长度] @28 */
                uint64_t mdat_sz = tc_load_be64(g.data + 28u);
                tc_store_be64(g.data + 28u, mdat_sz + gap);

                topos_io gio;
                counting_io_src(&g, &gio);
                topos_movie* mv2 = NULL;
                MT_CHECK_EQ_I64(tc_movie_open(&gio, &mv2), TC_OK);
                for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
                    uint8_t* buf = (uint8_t*)malloc(g_pkt_sizes[i]);
                    MT_CHECK_EQ_I64(tc_movie_packet(mv2, i, buf, g_pkt_sizes[i], NULL),
                                    TC_OK);
                    MT_CHECK(memcmp(buf, g_pkts[i], g_pkt_sizes[i]) == 0);
                    free(buf);
                }
                g_batch_io_reads = 0;
                MT_CHECK_EQ_I64(tc_movie_packet_batch(mv2, 0, TEST_FRAMES, arena,
                                                      need, offs, sizes, NULL),
                                TC_OK);
                MT_CHECK_EQ_I64(g_batch_io_reads, (int)TEST_FRAMES);
                for (uint32_t i = 0; i < TEST_FRAMES; ++i) {
                    MT_CHECK(memcmp(arena + offs[i], g_pkts[i], g_pkt_sizes[i]) == 0);
                }
                g_batch_io_reads = 0;
                MT_CHECK_EQ_I64(tc_movie_packet_batch(mv2, 1, TEST_FRAMES - 1u,
                                                      arena, need, NULL, NULL, NULL),
                                TC_OK);
                MT_CHECK_EQ_I64(g_batch_io_reads, 1);
                tc_movie_close(mv2);
            }
            mem_free(&g);
        }
    }
    free(arena);
    mem_free(&f);
}

/* RD7-04：tpcC 轨级版本必须覆盖全部保留代际（V 代际收纳 2026-09-13：
 * 原 V4/V5/V6 腿退役，改为 {V1, V2-VLC, V7-R2, V8}，cfg em → 位流 major
 * 映射见审计 §0）；sample 读取和批量读取都必须核对每个 TPIC header，
 * 不能用首帧猜版本来掩盖混合流。 */
static void test_track_and_sample_version_sync(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);

    static const uint32_t codings[4] = {0u, 1u, 8u, 9u};
    static const uint32_t majors[4] = {1u, 2u, 7u, 8u};
    for (unsigned ci = 0u; ci < 4u; ++ci) {
        size_t packet_size = 0u;
        uint8_t* packet = build_packet_with_coding(codings[ci], &packet_size);
        MT_CHECK(packet != NULL);
        if (packet == NULL) { continue; }

        mem_file f;
        memset(&f, 0, sizeof(f));
        topos_io sink;
        mem_io_sink(&f, &sink);
        topos_mux* mux = NULL;
        MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &mux), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_add_packet(mux, packet, packet_size, 0u, 1u), TC_OK);
        MT_CHECK_EQ_I64(tc_mux_finish(mux), TC_OK);
        tc_mux_free(mux);

        topos_io src;
        mem_io_src(&f, &src);
        topos_movie* movie = NULL;
        MT_CHECK_EQ_I64(tc_movie_open(&src, &movie), TC_OK);
        if (movie != NULL) {
            topos_movie_info info;
            memset(&info, 0, sizeof(info));
            info.struct_size = (uint32_t)sizeof(info);
            info.abi_version = TOPOS_CODEC_ABI_VERSION;
            MT_CHECK_EQ_I64(tc_movie_info(movie, &info), TC_OK);
            MT_CHECK_EQ_U64(info.reserved[0], majors[ci]);

            uint8_t* readback = (uint8_t*)malloc(packet_size);
            MT_CHECK(readback != NULL);
            if (readback != NULL) {
                MT_CHECK_EQ_I64(tc_movie_packet(movie, 0u, readback, packet_size, NULL), TC_OK);
                MT_CHECK(memcmp(readback, packet, packet_size) == 0);
                size_t offsets = 0u, sizes = 0u, need = 0u;
                MT_CHECK_EQ_I64(tc_movie_packet_batch(movie, 0u, 1u, readback,
                                                       packet_size, &offsets, &sizes, &need), TC_OK);
                MT_CHECK_EQ_U64(offsets, 0u);
                MT_CHECK_EQ_U64(sizes, packet_size);
                MT_CHECK_EQ_U64(need, packet_size);
                free(readback);
            }
            tc_movie_close(movie);
        }
        mem_free(&f);
        free(packet);
    }

    /* Track says V1, sample header says V2-VLC（保留代际间矛盾）: open can
     * remain moov-only, but both packet APIs must reject the contradictory
     * sample deterministically. */
    {
        size_t packet_size = 0u;
        uint8_t* packet = build_packet_with_coding(1u, &packet_size);
        MT_CHECK(packet != NULL);
        if (packet != NULL) {
            mem_file f;
            memset(&f, 0, sizeof(f));
            topos_io sink;
            mem_io_sink(&f, &sink);
            topos_mux* mux = NULL;
            MT_CHECK_EQ_I64(tc_mux_create(&cfg, &sink, &mux), TC_OK);
            MT_CHECK_EQ_I64(tc_mux_add_packet(mux, packet, packet_size, 0u, 1u), TC_OK);
            MT_CHECK_EQ_I64(tc_mux_finish(mux), TC_OK);
            tc_mux_free(mux);

            size_t tpcc = find_bytes(&f, "tpcC");
            MT_CHECK(tpcc != (size_t)-1);
            if (tpcc != (size_t)-1) {
                /* find_bytes points at atom type: version is payload +4
                 * (+8 from type), CRC is payload +24 (+28 from type). */
                tc_store_be16(f.data + tpcc + 8u, 1u);
                tc_store_be32(f.data + tpcc + 28u, tc_crc32(f.data + tpcc + 4u, 24u));
            }
            topos_io src;
            mem_io_src(&f, &src);
            topos_movie* movie = NULL;
            MT_CHECK_EQ_I64(tc_movie_open(&src, &movie), TC_OK);
            if (movie != NULL) {
                uint8_t* readback = (uint8_t*)malloc(packet_size);
                MT_CHECK(readback != NULL);
                if (readback != NULL) {
                    MT_CHECK_EQ_I64(tc_movie_packet(movie, 0u, readback, packet_size, NULL),
                                    TC_ERR_MALFORMED);
                    size_t need = 0u;
                    MT_CHECK_EQ_I64(tc_movie_packet_batch(movie, 0u, 1u, readback,
                                                           packet_size, NULL, NULL, &need),
                                    TC_ERR_MALFORMED);
                    MT_CHECK_EQ_U64(need, packet_size);
                    free(readback);
                }
                tc_movie_close(movie);
            }
            mem_free(&f);
            free(packet);
        }
    }
}

/* P1-11：fd 直读路径——mux 产物落盘 → tc_movie_open_fd 读回，与回调
 * 路径逐字节一致；fd<0 拒绝。 */
#if !defined(_MSC_VER)
#define port_fd_open open
#else
#define port_fd_open _open
#endif
static void test_movie_open_fd(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);
    mem_file mf;
    mux_all(&mf, &cfg);

    char path[256];
    const char* dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0') { dir = getenv("TEMP"); }
    if (dir == NULL || dir[0] == '\0') { dir = "."; }
    snprintf(path, sizeof(path), "%s/topos_fd_mov_%d.tmp", dir, (int)getpid());
    const int fd = port_fd_open(path, O_CREAT | O_TRUNC | O_RDWR | O_BINARY, 0600);
    MT_CHECK(fd >= 0);
    if (fd < 0) { mem_free(&mf); return; }
    MT_CHECK_EQ_I64(port_fd_write(fd, mf.data, (unsigned)mf.len),
                    (long long)mf.len);
    /* 写后文件偏移在末尾——fd 直读按绝对 offset pread，不受其影响 */
    topos_movie* mv = NULL;
    MT_CHECK_EQ_I64(tc_movie_open_fd(fd, (uint64_t)mf.len, &mv), TC_OK);
    verify_movie(mv);
    tc_movie_close(mv);
    MT_CHECK_EQ_I64(tc_movie_open_fd(-1, (uint64_t)mf.len, &mv),
                    TC_ERR_INVALID_ARGUMENT);
    port_fd_close(fd);
    remove(path);
    mem_free(&mf);
}

/* P1-18：singleton 表 atom 重复 / mdat 数量超限 / 截断 mdat 巨型声明
 * 全部稳定返回 TC_ERR_MALFORMED（不崩溃、不"最后覆盖"）。 */
static void mem_insert_at(mem_file* f, size_t at, const uint8_t* data, size_t len)
{
    if (f->len + len > f->cap) {
        size_t cap = f->cap ? f->cap : 256u;
        while (cap < f->len + len) { cap *= 2u; }
        f->data = (uint8_t*)realloc(f->data, cap);
        MT_CHECK(f->data != NULL);
        f->cap = cap;
    }
    memmove(f->data + at + len, f->data + at, f->len - at);
    memcpy(f->data + at, data, len);
    f->len += len;
}

static void rename_fourcc(mem_file* f, const char* tag, char a, char b)
{
    size_t at = find_bytes(f, tag);
    MT_CHECK(at != (size_t)-1);
    if (at != (size_t)-1) {
        f->data[at + 2] = (uint8_t)a;
        f->data[at + 3] = (uint8_t)b;
    }
}

static void test_malformed_singleton_and_mdat(void)
{
    topos_movie_config cfg;
    base_movie_cfg(&cfg);

    /* 1) 重复 stsz（stsc 改名 → 第二个 stsz；重复检查先于解析触发） */
    {
        mem_file f;
        mux_all(&f, &cfg);
        rename_fourcc(&f, "stsc", 's', 'z');
        expect_open_malformed(&f);
        mem_free(&f);
    }
    /* 2) 重复 stts（stco 改名） */
    {
        mem_file f;
        mux_all(&f, &cfg);
        rename_fourcc(&f, "stco", 't', 's');
        expect_open_malformed(&f);
        mem_free(&f);
    }
    /* 3) 重复 tpcC：真·复制 atom（36B）+ 逐级父 size 修补 */
    {
        mem_file f;
        mux_all(&f, &cfg);
        size_t at = find_bytes(&f, "tpcC");
        MT_CHECK(at != (size_t)-1);
        if (at != (size_t)-1) {
            static const char* chain[] = { "TPIC", "stsd", "stbl",
                                           "minf", "mdia", "trak", "moov" };
            for (int i = 0; i < 7; ++i) {
                size_t pat = find_bytes(&f, chain[i]);
                MT_CHECK(pat != (size_t)-1 && pat >= 4);
                uint32_t sz = tc_load_be32(f.data + pat - 4);
                tc_store_be32(f.data + pat - 4, sz + 36u);
            }
            mem_insert_at(&f, at + 36u, f.data + at, 36u);
            expect_open_malformed(&f);
        }
        mem_free(&f);
    }
    /* 4) mdat 数量超上限（1 + 8 个空 mdat = 9 → 明确拒绝） */
    {
        mem_file f;
        mux_all(&f, &cfg);
        uint8_t empty_mdat[8];
        tc_store_be32(empty_mdat, 8u);
        memcpy(empty_mdat + 4, "mdat", 4u);
        for (int i = 0; i < 8; ++i) {
            mem_insert_at(&f, f.len, empty_mdat, 8u);
        }
        expect_open_malformed(&f);
        mem_free(&f);
    }
    /* 5) 截断 mdat + 声明长度近 UINT64_MAX：checked add 不回绕，
     *    open 稳定拒绝（containment 失败） */
    {
        mem_file f;
        mux_all(&f, &cfg);
        MT_CHECK(f.len > 64);
        tc_store_be64(f.data + 28u, 0xFFFFFFFFFFFFFFFFull); /* mdat ext size */
        f.len = 40u; /* 截断 mdat payload */
        expect_open_malformed(&f);
        mem_free(&f);
    }
}

/* RD7-04：V7-B sample 必须能完整穿过 MOV mux/demux，而不是只在
 * elementary packet API 中可用。MOV 读侧只验证 base-only 结构，随后把
 * sample 交给统一 reduced decoder，确认容器不会绕过空间 base 路径。 */
static void test_v7b_mov_roundtrip(void)
{
    topos_frame_config frame_cfg;
    memset(&frame_cfg, 0, sizeof(frame_cfg));
    frame_cfg.struct_size = (uint32_t)sizeof(frame_cfg);
    frame_cfg.abi_version = TOPOS_CODEC_ABI_VERSION;
    frame_cfg.visible_width = 48u;
    frame_cfg.visible_height = 24u;
    frame_cfg.profile = 3u;
    frame_cfg.pixel_format = 0u;
    frame_cfg.bit_depth = 10u;
    frame_cfg.qmatrix_id = 1u;
    frame_cfg.qp_base = 28u;
    frame_cfg.color_range = 1u;
    frame_cfg.color_primaries = 1u;
    frame_cfg.color_transfer = 1u;
    frame_cfg.color_matrix = 1u;
    frame_cfg.sar_num = 1u;
    frame_cfg.sar_den = 1u;

    image_synth_cfg synth;
    memset(&synth, 0, sizeof(synth));
    synth.width = frame_cfg.visible_width;
    synth.height = frame_cfg.visible_height;
    synth.kind = TC_SYNTH_GRADIENT;
    synth.seed = 0xB70Bu;
    uint16_t* planes[TC_FRAME_MAX_PLANES] = { NULL, NULL, NULL, NULL };
    MT_CHECK_EQ_I64(image_synth_alloc(&synth, 0, &planes[0], &planes[1],
                                      &planes[2], &planes[3]), 0);

    topos_frame_input input;
    memset(&input, 0, sizeof(input));
    input.struct_size = (uint32_t)sizeof(input);
    input.abi_version = TOPOS_CODEC_ABI_VERSION;
    input.planes[0] = planes[0];
    input.planes[1] = planes[1];
    input.planes[2] = planes[2];

    const size_t packet_cap = 8u * 1024u * 1024u;
    uint8_t* packet = (uint8_t*)malloc(packet_cap);
    MT_CHECK(packet != NULL);
    if (packet == NULL) {
        for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) { free(planes[p]); }
        return;
    }
    topos_frame_stats encode_stats;
    memset(&encode_stats, 0, sizeof(encode_stats));
    MT_CHECK_EQ_I64(tc_v7b_frame_encode(&frame_cfg, &input, 16u, packet,
                                        packet_cap, &encode_stats), TC_OK);

    topos_movie_config movie_cfg;
    memset(&movie_cfg, 0, sizeof(movie_cfg));
    movie_cfg.struct_size = (uint32_t)sizeof(movie_cfg);
    movie_cfg.abi_version = TOPOS_CODEC_ABI_VERSION;
    movie_cfg.visible_width = frame_cfg.visible_width;
    movie_cfg.visible_height = frame_cfg.visible_height;
    movie_cfg.profile = frame_cfg.profile;
    movie_cfg.pixel_format = frame_cfg.pixel_format;
    movie_cfg.bit_depth = frame_cfg.bit_depth;
    movie_cfg.qmatrix_id = frame_cfg.qmatrix_id;
    movie_cfg.qp_base = frame_cfg.qp_base;
    movie_cfg.color_range = frame_cfg.color_range;
    movie_cfg.color_primaries = frame_cfg.color_primaries;
    movie_cfg.color_transfer = frame_cfg.color_transfer;
    movie_cfg.color_matrix = frame_cfg.color_matrix;
    movie_cfg.sar_num = frame_cfg.sar_num;
    movie_cfg.sar_den = frame_cfg.sar_den;
    movie_cfg.timescale = 24u;

    mem_file file;
    memset(&file, 0, sizeof(file));
    topos_io sink;
    mem_io_sink(&file, &sink);
    topos_mux* mux = NULL;
    MT_CHECK_EQ_I64(tc_mux_create(&movie_cfg, &sink, &mux), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_add_packet(mux, packet, encode_stats.packet_size,
                                      0u, 1u), TC_OK);
    MT_CHECK_EQ_I64(tc_mux_finish(mux), TC_OK);
    tc_mux_free(mux);

    topos_io source;
    /* The V7-B enhancement is opaque to base-range reads.  Locate its first
     * byte from the authenticated directory rather than assuming the old
     * per-plane RSD1 syntax. */
    const uint8_t* directory = packet + TC_FRAME_HEADER_SIZE;
    const uint16_t directory_slices = tc_load_be16(directory + 14u);
    const uint8_t* segments = directory + 36u + 2u * 24u +
        (size_t)directory_slices * 24u;
    const size_t residual_rel = (size_t)tc_load_be32(segments + 20u);
    MT_CHECK(residual_rel > TC_FRAME_HEADER_SIZE);
    MT_CHECK(residual_rel < (size_t)encode_stats.packet_size);
    trace_mem_file traced = {
        .file = &file,
        .calls = 0u,
        .bytes = 0u,
        .forbidden_lo = 36u + residual_rel,
        .forbidden_hi = 36u + encode_stats.packet_size,
        .touched_forbidden = 0,
    };
    mem_io_src(&file, &source);
    source.ctx = &traced;
    source.read = trace_mem_read;
    topos_movie* movie = NULL;
    MT_CHECK_EQ_I64(tc_movie_open(&source, &movie), TC_OK);
    topos_movie_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = TOPOS_CODEC_ABI_VERSION;
    MT_CHECK_EQ_I64(tc_movie_info(movie, &info), TC_OK);
    MT_CHECK_EQ_U64(info.reserved[0], 7u);
    MT_CHECK_EQ_U64(info.visible_width, frame_cfg.visible_width);
    MT_CHECK_EQ_U64(info.visible_height, frame_cfg.visible_height);

    /* RD4-04：probe + read the embedded base through the range reader.  The
     * trace is reset after moov indexing; reading a reduced sample must not
     * touch the residual payload or materialize the outer sample. */
    traced.calls = 0u;
    traced.bytes = 0u;
    traced.touched_forbidden = 0;
    size_t base_size = 0u;
    MT_CHECK_EQ_I64(tc_movie_packet_base(movie, 0u, NULL, 0u, &base_size),
                    TC_ERR_BUFFER_TOO_SMALL);
    MT_CHECK(base_size < (size_t)encode_stats.packet_size);
    MT_CHECK(traced.touched_forbidden == 0);
    uint8_t* base_packet = (uint8_t*)malloc(base_size);
    MT_CHECK(base_packet != NULL);
    if (base_packet != NULL) {
        traced.calls = 0u;
        traced.bytes = 0u;
        traced.touched_forbidden = 0;
        MT_CHECK_EQ_I64(tc_movie_packet_base(movie, 0u, base_packet, base_size, NULL),
                        TC_OK);
        MT_CHECK(traced.calls >= 2u); /* prefix/directory + base range */
        MT_CHECK(traced.touched_forbidden == 0);
        topos_frame_output base_info;
        MT_CHECK_EQ_I64(tc_frame_decode(base_packet, base_size, NULL, NULL,
                                        &base_info), TC_OK);
        MT_CHECK_EQ_U64(base_info.visible_width, 16u);
        MT_CHECK_EQ_U64(base_info.visible_height, 8u);

        size_t batch_need = 0u;
        MT_CHECK_EQ_I64(tc_movie_packet_base_batch(movie, 0u, 1u,
                                                   NULL, 0u, NULL, NULL,
                                                   NULL, &batch_need),
                        TC_ERR_BUFFER_TOO_SMALL);
        MT_CHECK_EQ_U64(batch_need, base_size);
        uint8_t* batch_packet = (uint8_t*)malloc(batch_need);
        size_t batch_offset = 0u;
        size_t batch_size = 0u;
        uint8_t batch_is_base = 0u;
        MT_CHECK(batch_packet != NULL);
        if (batch_packet != NULL) {
            traced.calls = 0u;
            traced.bytes = 0u;
            traced.touched_forbidden = 0;
            MT_CHECK_EQ_I64(tc_movie_packet_base_batch(
                                movie, 0u, 1u, batch_packet, batch_need,
                                &batch_offset, &batch_size, &batch_is_base,
                                NULL), TC_OK);
            MT_CHECK_EQ_U64(batch_offset, 0u);
            MT_CHECK_EQ_U64(batch_size, base_size);
            MT_CHECK_EQ_U64(batch_is_base, 1u);
            MT_CHECK(memcmp(batch_packet, base_packet, base_size) == 0);
            MT_CHECK(traced.touched_forbidden == 0);
            free(batch_packet);
        }
        free(base_packet);
    }

    uint8_t* roundtrip = (uint8_t*)malloc(encode_stats.packet_size);
    MT_CHECK(roundtrip != NULL);
    if (roundtrip != NULL) {
        MT_CHECK_EQ_I64(tc_movie_packet(movie, 0u, roundtrip,
                                        encode_stats.packet_size, NULL), TC_OK);
        MT_CHECK(memcmp(roundtrip, packet, encode_stats.packet_size) == 0);

        uint16_t reduced_y[16u * 8u];
        uint16_t reduced_u[8u * 8u];
        uint16_t reduced_v[8u * 8u];
        uint16_t* reduced[TC_FRAME_MAX_PLANES] = {
            reduced_y, reduced_u, reduced_v, NULL
        };
        size_t strides[TC_FRAME_MAX_PLANES] = { 16u, 8u, 8u, 0u };
        topos_frame_output reduced_info;
        MT_CHECK_EQ_I64(tc_frame_decode_reduced(roundtrip, encode_stats.packet_size,
                                                 TC_DECODE_SCALE_THIRD, reduced,
                                                 strides, &reduced_info), TC_OK);
        MT_CHECK_EQ_U64(reduced_info.visible_width, 16u);
        MT_CHECK_EQ_U64(reduced_info.visible_height, 8u);
        free(roundtrip);
    }
    tc_movie_close(movie);
    free(packet);
    for (uint32_t p = 0u; p < TC_FRAME_MAX_PLANES; ++p) { free(planes[p]); }
    mem_free(&file);
}

int main(void)
{
    build_test_packets();
    test_movie_open_fd();
    test_malformed_singleton_and_mdat();
    test_roundtrip_and_determinism();
    test_faststart();
    test_foreign_rejected();
    test_corruption_isolated();
    test_mux_rejections();
    test_open_rejections();
    test_write_failure_path();
    test_co64_and_layout_rules();
    test_alpha_budget_atom();
    test_movie_meta_atom();
    test_traw_tier_roundtrip();
    test_pf444_mux_roundtrip();
    test_gbr_mux_roundtrip();
    test_pro444_mux_roundtrip();
    test_r5_hand_co64();
    test_r5_unknown_atoms();
    test_r5_corrupt_sample_tables();
    test_r5_vfr_sar_authority();
    test_r5_long_file();
    test_o3_mdat_required_and_containment();
    test_o3_co64_over_4g_sparse();
    test_o3_writer_duration_saturation();
    test_movie_packet_batch();
    test_track_and_sample_version_sync();
    test_v7b_mov_roundtrip();
    for (uint32_t i = 0; i < TEST_FRAMES; ++i) { free(g_pkts[i]); }
    return MT_MAIN_RETURN();
}
