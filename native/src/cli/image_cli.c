/* image_cli —— toos：Topos Image 命令行工具（阶段 4 交付物，spec/ADR-I001）。
 *
 * 用法：
 *   toos probe <file.toos>                        解析并打印图片描述
 *   toos verify <file.toos> [--deep]              结构+CRC 校验（--deep 含 PIXL 探测）
 *   toos decode <file.toos> -o <out.raw>          解码为 planar uint16（LE）+ .json sidecar
 *   toos encode <in.raw> -o <out.toos> [opts]     planar uint16 → .toos（原子写）
 *       --width W --height H --pixel-format 0|1|2 --bit-depth 10|12
 *       [--profile 3|5|6] [--alpha-mode 0|1|2] [--alpha-bit-depth 16|8|10|12]
 *       [--qp 0..95] [--chroma-qp-offset -31..31] [--qmatrix 0|1|2|3]
 *       [--slice-rows N] [--image-profile 0|1|2]
 *       [--color-range 0|1] [--overwrite]
 *   toos benchmark <file.toos> [--iters N]        probe/validate/decode 分项计时
 *
 * 原子写（spec §12）：同目录临时文件 → 写满 → flush/fsync → 重新 probe/validate
 * → rename。失败/取消删除临时文件，绝不留下半文件冒充成功。
 * 退出码：0 成功 / 1 失败 / 2 用法错误。
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* —— 平台可移植层（Windows/POSIX；阶段 12 跨平台构建前置） ——
 * MSVC 无 <unistd.h>；Windows rename() 不覆盖已存在目标（POSIX 语义为
 * 原子替换），--overwrite 必须走 MoveFileEx；64-bit 文件偏移与单调时钟
 * 亦分别用 _fseeki64/_ftelli64 与 QPC。 */
#ifdef _WIN32
  #include <windows.h>
  #include <process.h>
  #define tp_seek(f, off, whence) _fseeki64((f), (int64_t)(off), (whence))
  static int tp_fsync_file(FILE* f) { return _commit(_fileno(f)); }
  static long tp_pid(void) { return (long)_getpid(); }
  static int tp_unlink(const char* p) { return remove(p); }
  static int tp_rename_replace(const char* from, const char* to)
  {
      return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) == 0 ? -1 : 0;
  }
#else
  #include <unistd.h>
  /* POSIX 侧 long 为 64-bit（LP64）；v1 文件 ≤ ~300 MiB，(long) 截断无实害 */
  #define tp_seek(f, off, whence) fseek((f), (long)(off), (whence))
  static int tp_fsync_file(FILE* f) { return fsync(fileno(f)); }
  static long tp_pid(void) { return (long)getpid(); }
  static int tp_unlink(const char* p) { return unlink(p); }
  #define tp_rename_replace rename
#endif

#include "topos_codec.h"
#include "topos_image.h"
#include "../common/crc32.h"
#include "image/half_map.h"
#include "../bitstream/frame_header.h" /* TC_TRANSFER_TRAW_LOG0 */

/* —— stdio 文件 io（read） —— */
static int32_t file_read(void* ctx, uint64_t off, void* buf, size_t n)
{
    FILE* f = (FILE*)ctx;
    if (tp_seek(f, off, SEEK_SET) != 0) { return TC_ERR_IO; }
    if (fread(buf, 1, n, f) != n) { return TC_ERR_IO; }
    return TC_OK;
}

typedef struct {
    FILE* f;
    int io_failed;
} file_sink;

static int32_t file_write(void* ctx, const void* d, size_t n)
{
    file_sink* s = (file_sink*)ctx;
    if (fwrite(d, 1, n, s->f) != n) { s->io_failed = 1; return TC_ERR_IO; }
    return TC_OK;
}

static int32_t file_seek_write(void* ctx, uint64_t off, const void* d, size_t n)
{
    file_sink* s = (file_sink*)ctx;
    if (tp_seek(s->f, off, SEEK_SET) != 0 ||
        fwrite(d, 1, n, s->f) != n) { s->io_failed = 1; return TC_ERR_IO; }
    return TC_OK;
}

static int32_t file_sink_init(file_sink* s, FILE* f, topos_io* io)
{
    s->f = f;
    s->io_failed = 0;
    memset(io, 0, sizeof(*io));
    io->struct_size = (uint32_t)sizeof(*io);
    io->abi_version = TOPOS_CODEC_ABI_VERSION;
    io->ctx = s;
    io->write = file_write;
    io->seek_write = file_seek_write;
    return TC_OK;
}

static int die(int32_t rc, const char* what)
{
    fprintf(stderr, "toos: %s 失败: %s (%s)\n", what, tc_image_status_message(rc),
            tc_last_error());
    return 1;
}

/* 已打开句柄的文件长度（fstat；避免二次 fopen 与"打开失败=0 与空文件=0"
 * 的歧义——调用方 fopen 成功后此处失败即真实 IO 错误，返回 0 由 probe
 * 以 TRUNCATED 报告） */
static uint64_t file_length(FILE* f)
{
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(_fileno(f), &st) != 0) { return 0; }
#else
    struct stat st;
    if (fstat(fileno(f), &st) != 0) { return 0; }
#endif
    return st.st_size > 0 ? (uint64_t)st.st_size : 0;
}

/* —— probe —— */
static int cmd_probe(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) { fprintf(stderr, "toos: cannot open %s\n", path); return 1; }
    topos_io io;
    memset(&io, 0, sizeof(io));
    io.struct_size = (uint32_t)sizeof(io);
    io.abi_version = TOPOS_CODEC_ABI_VERSION;
    io.ctx = f;
    io.read = file_read;
    io.length = file_length(f);

    topos_image_info info;
    const int32_t rc = tc_image_probe(&io, &info);
    if (rc != TC_OK) { fclose(f); return die(rc, "probe"); }
    const char* channels =
        (info.pixel_format == 2u) ? "G,B,R"
                                  : (info.pixel_format == 3u) ? "R,Gr,Gb,B" : "Y,U,V";
    printf("file:            %s\n", path);
    printf("file_size:       %llu\n", (unsigned long long)info.preamble.file_size);
    printf("file_version:    %u.%u\n", (unsigned)info.preamble.file_version_major,
           (unsigned)info.preamble.file_version_minor);
    printf("image_profile:   %u (%s)\n", (unsigned)info.idsc.image_profile,
           info.idsc.image_profile == TC_IMG_PROFILE_PREVIEW ? "Image Preview"
           : info.idsc.image_profile == TC_IMG_PROFILE_HQ ? "Image HQ"
           : info.idsc.image_profile == TC_IMG_PROFILE_XQ ? "Image XQ"
           : info.idsc.image_profile == TC_IMG_PROFILE_RAW ? "Image RAW"
           : info.idsc.image_profile == TC_IMG_PROFILE_HALF_FLOAT ? "Image Float (float16)"
           : "reserved");
    printf("size:            %u x %u\n", (unsigned)info.visible_width,
           (unsigned)info.visible_height);
    printf("planes:          %u (%s%s)\n", (unsigned)info.plane_count, channels,
           info.has_alpha ? ",A" : "");
    printf("bit_depth:       %u\n", (unsigned)info.bit_depth);
    printf("codec:           profile=%u pixel_format=%u\n", (unsigned)info.profile,
           (unsigned)info.pixel_format);
    printf("alpha:           mode=%u depth=%u %s\n", (unsigned)info.alpha_mode,
           (unsigned)info.alpha_bit_depth,
           info.alpha_premultiplied ? "premultiplied" : "straight");
    printf("color:           range=%u primaries=%u transfer=%u matrix=%u\n",
           (unsigned)info.idsc.color_range, (unsigned)info.idsc.color_primaries,
           (unsigned)info.idsc.color_transfer, (unsigned)info.idsc.color_matrix);
    printf("pixl:            offset=%llu size=%llu\n",
           (unsigned long long)info.pixl_offset, (unsigned long long)info.pixl_size);
    fclose(f);
    return 0;
}

/* —— verify —— */
static int cmd_verify(const char* path, int deep)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) { fprintf(stderr, "toos: cannot open %s\n", path); return 1; }
    topos_io io;
    memset(&io, 0, sizeof(io));
    io.struct_size = (uint32_t)sizeof(io);
    io.abi_version = TOPOS_CODEC_ABI_VERSION;
    io.ctx = f;
    io.read = file_read;
    io.length = file_length(f);

    topos_image_info info;
    const int32_t rc = tc_image_validate(&io, deep ? TC_IMG_VALIDATE_DEEP : 0u, &info);
    fclose(f);
    if (rc != TC_OK) { return die(rc, "verify"); }
    printf("%s: OK%s\n", path, deep ? " (deep)" : "");
    return 0;
}

/* —— decode —— */
static int cmd_decode(const char* path, const char* out_path)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) { fprintf(stderr, "toos: cannot open %s\n", path); return 1; }
    topos_io io;
    memset(&io, 0, sizeof(io));
    io.struct_size = (uint32_t)sizeof(io);
    io.abi_version = TOPOS_CODEC_ABI_VERSION;
    io.ctx = f;
    io.read = file_read;
    io.length = file_length(f);

    topos_image_info info;
    int32_t rc = tc_image_probe(&io, &info);
    if (rc != TC_OK) { fclose(f); return die(rc, "probe"); }

    uint32_t pw[TC_FRAME_MAX_PLANES] = {0}, ph[TC_FRAME_MAX_PLANES] = {0};
    size_t total = 0;
    for (uint32_t p = 0; p < info.plane_count; ++p) {
        rc = tc_image_query_decode_buffer(&info, p, &pw[p], &ph[p]);
        if (rc != TC_OK) { fclose(f); return die(rc, "plane geometry"); }
        total += (size_t)pw[p] * ph[p] * 2u;
    }
    uint16_t* mem = (uint16_t*)malloc(total);
    if (mem == NULL) { fclose(f); fprintf(stderr, "toos: OOM\n"); return 1; }
    topos_plane_view views[TC_FRAME_MAX_PLANES];
    memset(views, 0, sizeof(views)); /* 未用 plane 槽必须为 0（契约） */
    uint16_t* ptrs[TC_FRAME_MAX_PLANES] = {NULL, NULL, NULL, NULL};
    size_t off = 0;
    for (uint32_t p = 0; p < info.plane_count; ++p) {
        ptrs[p] = mem + off / 2u;
        off += (size_t)pw[p] * ph[p] * 2u;
        memset(&views[p], 0, sizeof(views[p]));
        views[p].struct_size = (uint32_t)sizeof(views[p]);
        views[p].abi_version = TOPOS_CODEC_ABI_VERSION; /* plane_view 为 codec 域结构 */
        views[p].pixels = ptrs[p];
        views[p].stride = 0;
    }
    topos_frame_output out;
    memset(&out, 0, sizeof(out));
    out.struct_size = (uint32_t)sizeof(out);
    out.abi_version = TOPOS_CODEC_ABI_VERSION;
    const int32_t drc = tc_image_decode(&io, views, &out);
    fclose(f);
    if (drc < 0) { free(mem); return die(drc, "decode"); }

    const int is_half = (info.idsc.sample_kind == TC_IMG_SAMPLE_KIND_HALF);
    if (is_half) {
        /* HALF 样本域：码值 → half 位模式（原地逆映射，spec §15）——
         * 输出字节即 float16 位平面（LE） */
        for (uint32_t p = 0; p < info.plane_count; ++p) {
            tci_code_unmap_plane(ptrs[p], (size_t)pw[p] * ph[p], ptrs[p]);
        }
    }

    FILE* o = fopen(out_path, "wb");
    if (o == NULL) { free(mem); fprintf(stderr, "toos: cannot write %s\n", out_path); return 1; }
    if (fwrite(mem, 1, total, o) != total) { fclose(o); free(mem); return 1; }
    fclose(o);

    /* sidecar：几何 + 色彩标签（显式精度/通道语义，spec §13 无静默转换） */
    char side[1024];
    snprintf(side, sizeof(side), "%s.json", out_path);
    FILE* sj = fopen(side, "w");
    if (sj != NULL) {
        const char* ch = (info.pixel_format == 2u) ? "G,B,R"
                         : (info.pixel_format == 3u) ? "R,Gr,Gb,B" : "Y,U,V";
        fprintf(sj,
                "{\n  \"width\": %u,\n  \"height\": %u,\n"
                "  \"bit_depth\": %u,\n  \"pixel_format\": %u,\n"
                "  \"channels\": \"%s%s\",\n  \"alpha_mode\": %u,\n"
                "  \"alpha_bit_depth\": %u,\n"
                "  \"color_range\": %u,\n  \"color_primaries\": %u,\n"
                "  \"color_transfer\": %u,\n  \"color_matrix\": %u,\n"
                "  \"sample\": \"%s\",\n"
                "  \"concealed_slices\": %u\n}\n",
                (unsigned)info.visible_width, (unsigned)info.visible_height,
                (unsigned)info.bit_depth, (unsigned)info.pixel_format, ch,
                info.has_alpha ? ",A" : "", (unsigned)info.alpha_mode,
                (unsigned)info.alpha_bit_depth,
                (unsigned)info.idsc.color_range, (unsigned)info.idsc.color_primaries,
                (unsigned)info.idsc.color_transfer, (unsigned)info.idsc.color_matrix,
                is_half ? "float16 bits LE planar (half, spec §15)"
                        : "uint16 LE planar",
                (unsigned)out.concealed_slices);
        fclose(sj);
    }
    printf("%s: decoded %u planes -> %s (%llu bytes)%s\n", path,
           (unsigned)info.plane_count, out_path, (unsigned long long)total,
           drc == TC_WARN_CONCEALED ? " [concealed]" : "");
    free(mem);
    return 0;
}

/* —— encode（原子写） —— */
static void print_usage_encode(void)
{
    fprintf(stderr,
            "usage: toos encode <in.raw> -o <out.toos> --width W --height H\n"
            "       --pixel-format 0|1|2 --bit-depth 10|12|16\n"
            "       [--profile 3|5|6] [--alpha-mode 0|1|2] [--alpha-bit-depth N]\n"
            "       [--qp N] [--chroma-qp-offset N] [--entropy vlc|v1|rans2] [--rdo on|off] [--slice-rows N] [--image-profile 0|1|2|3|4]\n"
            "       [--color-range 0|1] [--overwrite]\n"
            "\n"
            "  RAW（pf=3，--pixel-format 3）：输入 = 4 相位平面 R/Gr/Gb/B 依次拼接，\n"
            "  每平面 (W/2)*(H/2) 个 uint16 LE（RGGB 相位下采样）。\n"
            "\n"
            "  FLOAT（--half / --tier float-*，spec §15；v1.8 产品命名\n"
            "  ADR-C057）：输入 = G,B,R[,A] 依次拼接的 float16 位平面\n"
            "  （uint16 LE 容器），经冻结映射转码值进 16-bit 管线——压缩与\n"
            "  整数档完全一致。组合冻结 GBR/16-bit/profile5/linear/qm0，\n"
            "  image_profile 4。四档质量阶梯（不追无损；2K 真实视频帧实测）：\n"
            "    float-ultra   qp44（视觉透明 ±2 half-ULP，~2.0-2.3:1，\n"
            "              小于 EXR zip16）\n"
            "    float-high    qp58（~2.6-3.2:1，PSNR ≈71dB）\n"
            "    float-medium  qp72 / float-low qp82（快速代理）\n"
            "  float-* 有损档写盘前拒绝 Inf/NaN（防止量化跨类别）；\n"
            "  有限负值与 >1 值允许，特殊值请走显式 HALF/QP 并验往返。\n"
            "  严格无损不入阶梯：--half --qp 20（实测逐位无损；无损窗口\n"
            "  与内容域约束见 spec §15.3）。解码侧输出逆映射后的 float16 位平面。\n"
            "\n"
            "  entropy 默认 rans2（V7-R2 上下文 rANS——与视频链路同默认\n"
            "  ADR-C038；2026-09-12 起图片路径跟随视频优化集，同画质码率较\n"
            "  vlc −7~12%%、解码 −11~27%%，编码慢 20~70%%——实测\n"
            "  tools/toos_entropy_2026-09-12.json）。AQ（色度逐带 qp 偏移）\n"
            "  保持开启；旧版 topos_image 读端以 UNSUPPORTED_VERSION 明确\n"
            "  拒绝（内层版本随 codec 演进，spec §14）。\n"
            "  vlc = 旧图片默认（spec v2 canonical VLC + AQ/RDO；--rdo off\n"
            "  关闭 V2 逐系数 RDO，AQ 保持开启）；v1 = 旧 Rice 位流（与旧版\n"
            "  逐字节兼容）。\n"
            "  intra/intra-range/rans = 已退役（V 代际收纳 2026-09-13，\n"
            "  ADR-C0xx）——CLI 拒绝；旧文件仍可解码（UNSUPPORTED_VERSION\n"
            "  语义见读端）。\n"
            "\n"
            "  档位 --tier（ADR-I011 修订：质量预设，tier 命名与 image profile\n"
            "  命名空间 Preview/HQ/XQ 分离；锚点 = 1080p 实拍素材实测）:\n"
            "    422-low     Topos 422 Low     低   4:2:2 10-bit  qm3 qp63  ~1.0 MB/帧\n"
            "    422-medium  Topos 422 Medium  中   4:2:2 10-bit  qm1 qp61  ~1.3 MB/帧\n"
            "    422-high    Topos 422 High    高   4:2:2 10-bit  qm1 qp58  ~1.7 MB/帧（≈PNG 8-bit）\n"
            "    422-ultra   Topos 422 Ultra   超高 4:2:2 10-bit  qm0 qp58  ~2.9 MB/帧\n"
            "      （Ultra：视觉无损、flat 保真矩阵；= 无 --tier 默认）\n"
            "    444-high    Topos 444 High    高   GBR 4:4:4 10-bit  qm1 qp63  ~0.8~1.8 MB/帧\n"
            "    444-ultra   Topos 444 Ultra   超高 GBR 4:4:4 10-bit  qm0 qp58  ~2.7~4.1 MB/帧\n"
            "      （Ultra：视觉无损级 + 全色度分辨率，PSNR 高于 422-ultra 约 +4dB；\n"
            "       修订六 2026-09-12：444 档 12-bit→10-bit（ProRes 4444 同位深\n"
            "       口径），12-bit 高保真走 --bit-depth 12 显式档，非预设）\n"
            "    raw12-2     Topos RAW 12-bit 2:1    CFA qm0 qp45（近无损，~25dB 裕量）\n"
            "    raw12-4     Topos RAW 12-bit 4:1    CFA qm0 qp59（默认 RAW 档，透明线）\n"
            "    raw12-6     Topos RAW 12-bit 6:1    CFA qm0 qp64\n"
            "    raw12-8     Topos RAW 12-bit 8:1    CFA qm0 qp66\n"
            "    raw12-12    Topos RAW 12-bit 12:1   CFA qm0 qp68\n"
            "    raw12-16    Topos RAW 12-bit 16:1   CFA qm0 qp70（审片代理）\n"
    "    raw16-2     Topos RAW 16-bit 2:1    CFA qm0 qp51（近无损域锚 qe63，实测 1.5~2.0:1）\n"
    "    raw16-4     Topos RAW 16-bit 4:1    CFA qm0 qp72（16-bit 归档默认，实测 3.0~6.2:1）\n"
    "    raw16-6     Topos RAW 16-bit 6:1    CFA qm0 qp76（实测 3.8~10.8:1）\n"
    "    raw16-8     Topos RAW 16-bit 8:1    CFA qm0 qp78（实测 4.4~14.7:1）\n"
    "    raw16-12    Topos RAW 16-bit 12:1   CFA qm0 qp80（实测 5.2~21.1:1）\n"
    "    raw16-16    Topos RAW 16-bit 16:1   CFA qm0 qp82（实测 6.5~31.9:1，随内容浮动）\n"
            "      （TRAW 档位 = CQ 质量锚定：比率为定位标签，体积随内容浮动；\n"
            "       名义体积 = W×H×2/比率，干净场景自动显著小于名义）\n"
            "  qm = 量化矩阵（0 = flat；1 = Standard；2 = 444 Compact；\n"
            "       3 = 422 Low Compact；2 仅 GBR 4:4:4 12-bit，3 仅 YUV 4:2:2 10-bit）。\n"
            "  无 --tier 时默认 = 422-ultra 档（ADR-I010 视觉无损，qm0 qp58）。\n"
            "  显式 --pixel-format/\n"
            "  --bit-depth/--qp/--chroma-qp-offset/--qmatrix/--image-profile 覆盖档位预设。真无损\n"
            "  归档不入档位菜单：--qp 28（10-bit 数学无损）。\n");
}

/* —— 编码档位表（ADR-I011 修订四，2026-09-05）。与
 * docs/image/capability_manifest.json 的 tiers 一一对应（manifest 为文档
 * 真相源，此表为 CLI 执行面；pytest 交叉校验两侧一致）。锚点素材修正：
 * 修订一~三的标定输入误用 4:4:4 布局源（色度取了 U 平面上下两半），真
 * 4:2:2（色度 2:1 降采样）下各档实测比旧锚点低 10~16%（色度更平滑更可
 * 压），PSNR 不变——本表锚点已按真 4:2:2 修正。产品语义（修订三定案）：
 * 很好画质 + 相对小体积，档位不设真无损（真无损走 --qp 28 或无损容器）；
 * 代理场景走半分辨率工作流（960×540 + 422-low ≈0.4 MB/帧）。体积档
 * low = 422 Low Compact qm3 qp63；medium/high = Standard 矩阵单轴 qp 61/58；
 * ultra = qm0 qp58（视觉无损级，= 无 --tier 默认）。 —— */
typedef struct {
    const char* id;
    const char* label;
    uint8_t pixel_format;
    uint8_t bit_depth;
    uint8_t image_profile;
    uint8_t qmatrix;
    uint8_t qp;
} toos_tier_preset;

static const toos_tier_preset kTierPresets[] = {
    { "422-low",    "Topos 422 Low",    0u, 10u, 0u, 3u, 63u },
    { "422-medium", "Topos 422 Medium", 0u, 10u, 0u, 1u, 61u },
    { "422-high",   "Topos 422 High",   0u, 10u, 0u, 1u, 58u },
    { "422-ultra",  "Topos 422 Ultra",  0u, 10u, 0u, 0u, 58u },
    /* 444 家族（修订六 2026-09-12）：GBR 10-bit（Image HQ）——与
     * ProRes 4444 同位深口径；12-bit 高保真仍可 --bit-depth 12 显式档
     * （Image XQ，非预设）。444-high = Standard 矩阵收缩档；
     * 444-ultra = flat 视觉无损级（全色度分辨率 + qm0，PSNR 高于
     * 422-ultra 约 +4dB——4:4:4 去掉色度二次采样后的实测）。 */
    { "444-high",   "Topos 444 High",   2u, 10u, 1u, 1u, 63u },
    { "444-ultra",  "Topos 444 Ultra",  2u, 10u, 1u, 0u, 58u },
    /* FLOAT 家族（v1.8 产品命名，ADR-C057）：GBR444 float16——pf2/bd16/
     * profile5 + qm0 + transfer linear + image_profile 4。产品语义（用户
     * 拍板，2026-09-21）：**不追求无损**——发挥中间片编码优势，较小体积
     * + 非常好画质 + 编解码实时；四档质量阶梯与 422 家族同一套
     * Low/Medium/High/Ultra 词表，TMET label = 'toos <Quality> 16bit
     * float'（tier id 1..4 与整数阶梯共用；浮点域由 IDSC sample_kind=
     * HALF 自描述，不入 tier id——未来 FLOAT32 同词表换位深即可）。
     * 锚点 = 2K 真实视频帧实测（spec §15.3）：qp44 视觉透明（±2
     * half-ULP，2.0~2.3:1，小于 EXR half zip16 同帧 6568 KiB）→ Ultra；
     * qp58 ≈71dB → High；qp72 ≈44dB → Medium；qp82 → Low。真无损不再
     * 入预设阶梯（qp20/32 逐位无损仍可 --half --qp 20 显式达成）。
     * 比率档名为定位标签，体积随内容浮动（同 raw12/16 家族口径）。 */
    { "float-low",    "toos Low 16bit float",    2u, 16u, 4u, 0u, 82u },
    { "float-medium", "toos Medium 16bit float", 2u, 16u, 4u, 0u, 72u },
    { "float-high",   "toos High 16bit float",   2u, 16u, 4u, 0u, 58u },
    { "float-ultra",  "toos Ultra 16bit float",  2u, 16u, 4u, 0u, 44u },
    /* TRAW（Topos RAW）12-bit 家族（topos_traw_format_plan §1.1.1，2026-09-13
     * 拍板）：pf=3 CFA 4 相位平面 + profile 7 + qm0 冻结 + transfer=LOG0。
     * CQ 质量锚定预设：比率档名仅为定位标签，体积随内容浮动（无码控机）；
     * 锚点 = 试点 UHD 实测效率点（qp44≈2:1 / qp58≈3.7:1→4:1 档 qp59 /
     * qp64=6.07 / qp66=7.7 / qp68=10.9 / qp70=17.6）。 */
    { "raw12-2",    "Topos RAW 12-bit 2:1",    3u, 12u, 3u, 0u, 45u },
    { "raw12-4",    "Topos RAW 12-bit 4:1",    3u, 12u, 3u, 0u, 59u },
    { "raw12-6",    "Topos RAW 12-bit 6:1",    3u, 12u, 3u, 0u, 64u },
    { "raw12-8",    "Topos RAW 12-bit 8:1",    3u, 12u, 3u, 0u, 66u },
    { "raw12-12",   "Topos RAW 12-bit 12:1",   3u, 12u, 3u, 0u, 68u },
    { "raw12-16",   "Topos RAW 12-bit 16:1",   3u, 12u, 3u, 0u, 70u },
    /* TRAW 16-bit linear 归档家族（批 4）：pf=3 CFA + profile 7 + qm0 +
     * transfer=linear。锚点 = 批 4 真实 DNG 标定（4 传感器 UHD 中心裁剪：
     * DJI 4P 20MP 16-bit / Leica CL 24MP / Pentax K-1 36MP / K-3 III 26MP
     * 14-bit；tools/traw_batch4_2026-09-13.json）。qe = qp_base+12 后按
     * qp_base 域封顶（≤63 顶格 63，≥64 走 v1.5 域放开 95）→ 可分 qe =
     * [0..63]∪[76..95]，64..75 为死区；raw16-2 存 qp51（+12 恰好触顶，
     * 51..63 字节同径）。四传感器无损边界一致 = qp36（1.11~1.35:1）。
     * 码率随内容浮动（CQ 语义）：纹理重的航拍 7:1 封顶、干净画面 >30:1。 */
    { "raw16-2",    "Topos RAW 16-bit 2:1",    3u, 16u, 3u, 0u, 51u },
    { "raw16-4",    "Topos RAW 16-bit 4:1",    3u, 16u, 3u, 0u, 72u },
    { "raw16-6",    "Topos RAW 16-bit 6:1",    3u, 16u, 3u, 0u, 76u },
    { "raw16-8",    "Topos RAW 16-bit 8:1",    3u, 16u, 3u, 0u, 78u },
    { "raw16-12",   "Topos RAW 16-bit 12:1",   3u, 16u, 3u, 0u, 80u },
    { "raw16-16",   "Topos RAW 16-bit 16:1",   3u, 16u, 3u, 0u, 82u },
};

#define K_TIER_N (sizeof(kTierPresets) / sizeof(kTierPresets[0]))

static const toos_tier_preset* find_tier(const char* id)
{
    for (size_t i = 0; i < K_TIER_N; ++i) {
        if (strcmp(kTierPresets[i].id, id) == 0) { return &kTierPresets[i]; }
    }
    return NULL;
}

/* —— v1.7 容器元数据（TMET chunk；载荷与 MOV tpcD 同构，container_spec
 * v1.7 §4）。422 质量阶梯 tier id 与 src/shared/codec/topos_meta.py 的
 * TOPOS_IMAGE_TIER_IDS 同值；444/RAW/HF 预设不入表（tier_id=0，读端展示
 * 回退 image_profile 类名）。 —— */

/* 按**最终生效参数**回配质量阶梯（而非按是否传了 --tier）——"无参数默认
 * ≡ 显式 --tier 422-ultra" 的字节等价契约因此保持；位深与样本域不入
 * tier id（12-bit 高保真显式档同阶梯同 id，位深进 label 与 IDSC；浮点域
 * 由 IDSC sample_kind=HALF 自描述——v1.8 FLOAT 阶梯与整数阶梯共用同一套
 * Low..Ultra tier id，读端按样本域取展示名，ADR-C057）。 */
static uint8_t tier_meta_id_by_params(uint8_t pixel_format, uint8_t qmatrix,
                                      uint8_t qp, int half)
{
    if (pixel_format == 0u && !half) {
        if (qmatrix == 3u && qp == 63u) { return 1u; } /* 422-low    */
        if (qmatrix == 1u && qp == 61u) { return 2u; } /* 422-medium */
        if (qmatrix == 1u && qp == 58u) { return 3u; } /* 422-high   */
        if (qmatrix == 0u && qp == 58u) { return 4u; } /* 422-ultra（默认） */
    }
    if (half && pixel_format == 2u && qmatrix == 0u) {
        if (qp == 82u) { return 1u; } /* float-low    */
        if (qp == 72u) { return 2u; } /* float-medium */
        if (qp == 58u) { return 3u; } /* float-high   */
        if (qp == 44u) { return 4u; } /* float-ultra  */
    }
    return 0u;
}

static void tmet_be16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void tmet_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* TMET 载荷：'TPCD' | ver=1(u16) | tier(u8) | rsv=0(u8) | vlen(u16) |
 * llen(u16) | "TOPOS" | label | CRC32(前缀)。超长返回 0（调用方跳过）。 */
static size_t build_tmet(uint8_t tier_id, const char* label,
                         uint8_t* out, size_t cap)
{
    static const char vendor[] = "TOPOS";
    const size_t vlen = sizeof(vendor) - 1u;
    const size_t llen = strlen(label);
    const size_t payload = 12u + vlen + llen;
    if (vlen > 15u || llen > 63u || cap < payload + 4u) { return 0u; }
    memset(out, 0, payload + 4u);
    memcpy(out, "TPCD", 4u);
    tmet_be16(out + 4, 1u);
    out[6] = tier_id;
    out[7] = 0u;
    tmet_be16(out + 8, (uint16_t)vlen);
    tmet_be16(out + 10, (uint16_t)llen);
    memcpy(out + 12, vendor, vlen);
    if (llen != 0u) { memcpy(out + 12 + vlen, label, llen); }
    tmet_be32(out + payload, tc_crc32(out, payload));
    return payload + 4u;
}

static int cmd_encode(int argc, char** argv)
{
    const char* in_path = NULL;
    const char* out_path = NULL;
    topos_frame_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.abi_version = TOPOS_CODEC_ABI_VERSION;
    /* 默认 = 422-ultra 档（qm0 qp58，ADR-I010 视觉无损）+ V7-R2 rANS 熵：
     * 2026-09-12 起图片链路与视频（ADR-C038）同默认——同画质码率较 V2 VLC
     * −7~12%、解码 −11~27%（tools/toos_entropy_2026-09-12.json）。
     * --entropy vlc 取回旧图片默认（V2 canonical VLC）；v1 = 旧 Rice 位流
     * （与旧版逐字节一致）。 */
    cfg.qp_base = 58u;
    cfg.slice_rows = 0u;
    uint32_t width = 0, height = 0, bit_depth = 10, image_profile = 0xFFFFFFFFu;
    int overwrite = 0;
    int entropy_vlc = 8; /* 0=v1 1=vlc 8=rans2（V 代际收纳 2026-09-13：em 3/6/7 退役） */
    int rdo_on = 1;
    int rdo_explicit = 0; /* 用户显式给过 --rdo（默认值随熵档解释） */
    const char* tier_id = NULL;
    int qmatrix = -1; /* -1 = 未指定（由档位或默认 flat 决定） */
    int chroma_qp_offset = 0;
    int half_mode = 0; /* --half：HALF 样本域（spec §15，2026-09-19） */
    int pf_explicit = 0, bd_explicit = 0, profile_explicit = 0, qp_explicit = 0;

    /* 预扫 --tier：预设先行填充 pf/bd/qp/qmatrix/profile，显式同名参数仍可覆盖 */
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--tier") == 0 && i + 1 < argc) { tier_id = argv[i + 1]; }
    }
    /* --half 预扫（float-* 档位蕴含 half；其余整数域档位与 --half 互斥） */
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--half") == 0) { half_mode = 1; }
    }
    if (tier_id != NULL && strncmp(tier_id, "float-", 6) == 0) { half_mode = 1; }
    if (half_mode && tier_id != NULL && strncmp(tier_id, "float-", 6) != 0) {
        fprintf(stderr, "toos: --half 与整数域档位互斥（float 请用 --tier float-* "
                        "或 --qp 显式调节）\n");
        return 2;
    }
    const toos_tier_preset* tier = NULL;
    if (tier_id != NULL) {
        tier = find_tier(tier_id);
        if (tier == NULL) {
            fprintf(stderr, "toos: 未知档位 --tier %s（可用:", tier_id);
            for (size_t i = 0; i < K_TIER_N; ++i) {
                fprintf(stderr, " %s", kTierPresets[i].id);
            }
            fprintf(stderr, "）\n");
            return 2;
        }
        cfg.pixel_format = tier->pixel_format;
        bit_depth = tier->bit_depth;
        cfg.qp_base = tier->qp;
        cfg.qmatrix_id = tier->qmatrix;
        image_profile = tier->image_profile;
    }

    for (int i = 0; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "-o") == 0 && i + 1 < argc) { out_path = argv[++i]; }
        else if (strcmp(a, "--width") == 0 && i + 1 < argc) { width = (uint32_t)atoi(argv[++i]); }
        else if (strcmp(a, "--height") == 0 && i + 1 < argc) { height = (uint32_t)atoi(argv[++i]); }
        else if (strcmp(a, "--tier") == 0 && i + 1 < argc) { ++i; } /* 预扫已消费 */
        else if (strcmp(a, "--half") == 0) { /* 预扫已消费（无值开关） */ }
        else if (strcmp(a, "--qmatrix") == 0 && i + 1 < argc) {
            qmatrix = atoi(argv[++i]);
            if (qmatrix < 0 || qmatrix > 3) {
                fprintf(stderr, "toos: --qmatrix 需 0|1|2|3（得到 %d）\n", qmatrix);
                return 2;
            }
        }
        else if (strcmp(a, "--pixel-format") == 0 && i + 1 < argc) { cfg.pixel_format = (uint8_t)atoi(argv[++i]); pf_explicit = 1; }
        else if (strcmp(a, "--bit-depth") == 0 && i + 1 < argc) { bit_depth = (uint32_t)atoi(argv[++i]); bd_explicit = 1; }
        else if (strcmp(a, "--profile") == 0 && i + 1 < argc) { cfg.profile = (uint8_t)atoi(argv[++i]); profile_explicit = 1; }
        else if (strcmp(a, "--alpha-mode") == 0 && i + 1 < argc) { cfg.alpha_mode = (uint8_t)atoi(argv[++i]); }
        else if (strcmp(a, "--alpha-bit-depth") == 0 && i + 1 < argc) { cfg.alpha_bit_depth = (uint8_t)atoi(argv[++i]); }
        else if (strcmp(a, "--qp") == 0 && i + 1 < argc) { cfg.qp_base = (uint8_t)atoi(argv[++i]); qp_explicit = 1; }
        else if (strcmp(a, "--chroma-qp-offset") == 0 && i + 1 < argc) {
            chroma_qp_offset = atoi(argv[++i]);
            if (chroma_qp_offset < -31 || chroma_qp_offset > 31) {
                fprintf(stderr, "toos: --chroma-qp-offset 需 -31..31（得到 %d）\n",
                        chroma_qp_offset);
                return 2;
            }
        }
        else if (strcmp(a, "--entropy") == 0 && i + 1 < argc) {
            const char* e = argv[++i];
            /* V 代际收纳（2026-09-13）：写面收缩 vlc|v1|rans2；em 3/6/7
             * 已退役（CLI 层拒绝，退役代际仅保留解码，em 编号封存）。 */
            if (strcmp(e, "vlc") == 0) { entropy_vlc = 1; }
            else if (strcmp(e, "v1") == 0) { entropy_vlc = 0; }
            else if (strcmp(e, "rans2") == 0) { entropy_vlc = 8; }
            else if (strcmp(e, "intra") == 0 || strcmp(e, "intra-range") == 0 ||
                     strcmp(e, "rans") == 0) {
                fprintf(stderr, "toos: --entropy %s: generation retired "
                                "(V consolidation 2026-09-13, see ADR-C0xx); "
                                "use vlc|v1|rans2\n", e);
                return 2;
            }
            else { fprintf(stderr, "toos: --entropy 需 vlc|v1|rans2（得到 %s）\n", e); return 2; }
        }
        else if (strcmp(a, "--rdo") == 0 && i + 1 < argc) {
            const char* r = argv[++i];
            rdo_explicit = 1;
            if (strcmp(r, "on") == 0) { rdo_on = 1; }
            else if (strcmp(r, "off") == 0) { rdo_on = 0; }
            else { fprintf(stderr, "toos: --rdo 需 on|off（得到 %s）\n", r); return 2; }
        }
        else if (strcmp(a, "--slice-rows") == 0 && i + 1 < argc) { cfg.slice_rows = (uint8_t)atoi(argv[++i]); }
        else if (strcmp(a, "--image-profile") == 0 && i + 1 < argc) { image_profile = (uint32_t)atoi(argv[++i]); }
        else if (strcmp(a, "--color-range") == 0 && i + 1 < argc) { cfg.color_range = (uint8_t)atoi(argv[++i]); }
        else if (strcmp(a, "--overwrite") == 0) { overwrite = 1; }
        else if (in_path == NULL) { in_path = a; }
        else { print_usage_encode(); return 2; }
    }
    if (entropy_vlc == 8) {
        /* V7-R2 上下文 rANS（视频现行默认，ADR-C038）。AQ（色度逐带 qp
         * 偏移）与熵无关、图片档策略保持开启（与 vlc 路径同口径）；
         * RDO 为 V2 VLC 专属（码率模型基于 VLC 符号位），不适用。 */
        cfg.reserved[0] = 8u;
        cfg.reserved[1] = 1u;
    } else if (entropy_vlc == 1) {
        /* V2 + canonical VLC；AQ/RDO 为编码器内部策略（reserved[1]/[2]），
         * m7 路径带确定性降级（fcache 上限/OOM 回退 plain），位流恒为合法
         * V2-VLC——解码侧无新要求。 */
        cfg.reserved[0] = 1u;
        cfg.reserved[1] = 1u;
        cfg.reserved[2] = (uint32_t)rdo_on;
    }
    if (qmatrix >= 0) { cfg.qmatrix_id = (uint8_t)qmatrix; } /* 显式覆盖档位 */
    if (rdo_explicit && rdo_on && entropy_vlc != 1) {
        /* RDO 码率模型基于 V2 VLC 符号位（与视频编码器约束同源）——
         * 显式 --rdo on 且非 vlc 时报错，不静默忽略。 */
        fprintf(stderr, "toos: --rdo on 须 --entropy vlc（RDO 码率模型基于 V2 VLC 符号位）\n");
        return 2;
    }
    if (in_path == NULL || out_path == NULL || width == 0u || height == 0u) {
        print_usage_encode();
        return 2;
    }
    if (chroma_qp_offset != 0 && cfg.pixel_format == 3u) {
        fprintf(stderr, "toos: --chroma-qp-offset 对 RAW（pf=3）无色度语义（须 0）\n");
        return 2;
    }
    cfg.visible_width = (uint16_t)width;
    cfg.visible_height = (uint16_t)height;
    cfg.bit_depth = (uint8_t)bit_depth;
    cfg.qp_delta_chroma = (int8_t)chroma_qp_offset;
    cfg.color_range = 1u;
    cfg.color_primaries = 1u;
    /* H1（2026-09-21）：整数档写侧默认 sRGB EOTF（CICP 13）——图片
     * 默认 sRGB（写侧显式标注，非读侧猜测）；浮点 linear(8) / RAW
     * LOG0·linear 冻结对不变。旧文件（transfer=1）不回填不重解释。 */
    cfg.color_transfer = (cfg.pixel_format == 3u)
        ? (bit_depth == 16u ? 8u : TC_TRANSFER_TRAW_LOG0) : 13u;
    cfg.color_matrix = (cfg.pixel_format == 2u || cfg.pixel_format == 3u) ? 0u : 1u;
    cfg.chroma_siting = 0u;
    /* alpha 位深缺省：mode1 → 16（无损）；mode2 → 12（codec 仅允许 8/10/12，
     * 旧行为统一默认 16 会让 `--alpha-mode 2` 必然 config validate 失败）。
     * 无 alpha（mode 0）保持 0——codec 要求 mode0 时 alpha_bit_depth 必须为 0 */
    if (cfg.alpha_mode != 0u && cfg.alpha_bit_depth == 0u) {
        cfg.alpha_bit_depth = (cfg.alpha_mode == 2u) ? 12u : 16u;
    }
    if (half_mode) {
        /* HALF 样本域（spec §15）：冻结组合 pf2/bd16/profile5 + linear +
         * qm0；默认 qp=20（Q=8 全内容可编地板；无损归档显式 --qp 0-10，
         * 内容域约束见 spec §15.3）。组合字段与用户显式值冲突 → 显式报错
         * （不静默覆盖）；qp/qmatrix 显式值保留。须在 profile/image_profile
         * 默认填充之前执行（默认值不构成「显式冲突」）。 */
        if ((pf_explicit && cfg.pixel_format != 2u) ||
            (bd_explicit && bit_depth != 16u) ||
            (profile_explicit && cfg.profile != 5u) ||
            (image_profile != 0xFFFFFFFFu && image_profile != TC_IMG_PROFILE_HALF_FLOAT)) {
            fprintf(stderr, "toos: --half 冻结组合为 GBR(pf=2) + 16-bit + "
                            "profile 5 + image_profile 4——与显式参数冲突（spec §15）\n");
            return 2;
        }
        cfg.pixel_format = 2u;
        bit_depth = 16u;
        cfg.bit_depth = 16u;
        cfg.profile = 5u;
        cfg.color_transfer = 8u;
        cfg.color_matrix = 0u;
        if (qmatrix < 0) { cfg.qmatrix_id = 0u; }
        if (!qp_explicit && tier_id == NULL) { cfg.qp_base = 20u; }
        image_profile = TC_IMG_PROFILE_HALF_FLOAT;
        if (cfg.alpha_mode == 2u) {
            fprintf(stderr, "toos: --half alpha 仅支持 mode 1（无损；近似 "
                            "mode2 的码值域语义对 half 域无定义）\n");
            return 2;
        }
    }
    if (cfg.profile == 0u) {
        cfg.profile = (cfg.pixel_format == 0u) ? 3u
                      : (cfg.pixel_format == 3u) ? 7u : 5u;
    }
    if (image_profile == 0xFFFFFFFFu) {
        image_profile = (cfg.pixel_format == 2u)
                            ? (bit_depth == 12u ? TC_IMG_PROFILE_XQ : TC_IMG_PROFILE_HQ)
                            : (cfg.pixel_format == 3u) ? TC_IMG_PROFILE_RAW
                                                       : TC_IMG_PROFILE_PREVIEW;
    }

    int32_t rc = tc_frame_config_validate(&cfg);
    if (rc != TC_OK) {
        fprintf(stderr, "toos: invalid config: %s\n", tc_status_message(rc));
        return 1;
    }

    /* 覆盖策略：默认拒绝（spec §12 明确 overwrite policy） */
    if (!overwrite) {
        FILE* t = fopen(out_path, "rb");
        if (t != NULL) {
            fclose(t);
            fprintf(stderr, "toos: %s exists (use --overwrite)\n", out_path);
            return 1;
        }
    }

    /* 读 raw planar 输入（uint16 LE；平面序 = codec 契约：YUV→Y,U,V；GBR→G,B,R；A=3） */
    topos_frame_input input;
    memset(&input, 0, sizeof(input));
    input.struct_size = (uint32_t)sizeof(input);
    input.abi_version = TOPOS_CODEC_ABI_VERSION;
    topos_frame_output geo;
    memset(&geo, 0, sizeof(geo));
    geo.struct_size = (uint32_t)sizeof(geo);
    geo.abi_version = TOPOS_CODEC_ABI_VERSION;
    geo.visible_width = cfg.visible_width;
    geo.visible_height = cfg.visible_height;
    geo.plane_count = (uint8_t)(cfg.pixel_format == 3u
                                    ? 4u
                                    : 3u + (cfg.alpha_mode != 0u ? 1u : 0u));
    geo.pixel_format = cfg.pixel_format;
    uint32_t plane_sizes[4];
    if (cfg.pixel_format == 3u) {
        /* TRAW CFA：输入 = R/Gr/Gb/B 4 相位平面，各 (W/2)*(H/2) uint16 LE */
        const uint32_t pw = (width + 1u) / 2u;
        const uint32_t ph = (height + 1u) / 2u;
        for (int i = 0; i < 4; ++i) { plane_sizes[i] = pw * ph; }
    } else {
        const int chroma_full = (cfg.pixel_format != 0u);
        const uint32_t cw = chroma_full ? width : (width + 1u) / 2u;
        plane_sizes[0] = (uint32_t)width * height;
        plane_sizes[1] = cw * height;
        plane_sizes[2] = cw * height;
        plane_sizes[3] = cfg.alpha_mode != 0u ? (uint32_t)width * height : 0u;
    }
    size_t raw_total = 0;
    for (int i = 0; i < 4; ++i) { raw_total += (size_t)plane_sizes[i] * 2u; }
    uint16_t* mem = (uint16_t*)malloc(raw_total);
    if (mem == NULL) { fprintf(stderr, "toos: OOM\n"); return 1; }
    {
        FILE* rf = fopen(in_path, "rb");
        if (rf == NULL) { free(mem); fprintf(stderr, "toos: cannot open %s\n", in_path); return 1; }
        if (fread(mem, 1, raw_total, rf) != raw_total) {
            fclose(rf); free(mem);
            fprintf(stderr, "toos: raw too short (need %llu bytes)\n",
                    (unsigned long long)raw_total);
            return 1;
        }
        fclose(rf);
    }
    {
        size_t o = 0;
        /* ABI 契约（topos_codec.h）：NULL 平面的下标必须 ≥ plane_count——
         * 无 alpha 的 3 平面格式不得给 planes[3] 赋指针（越界读会在
         * V7-R2 编码路径产生损坏码流，2026-09-19 实测） */
        const uint32_t nplanes = (cfg.pixel_format == 3u)
                                     ? 4u
                                     : 3u + (cfg.alpha_mode != 0u ? 1u : 0u);
        for (uint32_t i = 0; i < nplanes; ++i) {
            input.planes[i] = mem + o / 2u;
            o += (size_t)plane_sizes[i] * 2u;
        }
    }
    if (half_mode) {
        /* float-* 为有损产品预设：Inf/NaN 的 half 指数全 1，量化后
         * 可能跨到有限值（实测 float-low 可把 qNaN 变普通数）。在写盘
         * 前拒绝；显式 --half --qp 路径保留其既有专业控制语义。 */
        if (tier_id != NULL && strncmp(tier_id, "float-", 6) == 0) {
            for (int i = 0; i < 4; ++i) {
                for (uint32_t j = 0; j < plane_sizes[i]; ++j) {
                    if ((input.planes[i][j] & 0x7C00u) == 0x7C00u) {
                        fprintf(stderr,
                                "toos: float-* lossy tier rejects Inf/NaN"
                                " (plane %d sample %u); sanitize input or use"
                                " explicit --half --qp and verify roundtrip\n",
                                i, (unsigned)j);
                        free(mem);
                        return 1;
                    }
                }
            }
        }
        /* 输入按 half 位模式（uint16 LE 容器）读取 → 冻结映射转码值（原地）；
         * 输出侧 toos decode 对 HALF 文件执行逆映射（spec §15） */
        for (int i = 0; i < 4; ++i) {
            if (plane_sizes[i] > 0u) {
                /* planes 指向本函数持有的可写 mem；ABI 输入字段声明为 const。 */
                uint16_t* writable_plane = (uint16_t*)(void*)input.planes[i];
                tci_half_map_plane(input.planes[i], plane_sizes[i], writable_plane);
            }
        }
        if (getenv("TOOS_HALF_DEBUG") != NULL) {
            /* 诊断：导出映射后平面 + cfg/input 结构体（路径=环境变量值；
             * HALF 链路排查用，见设计记录 §5.3） */
            FILE* dbg = fopen(getenv("TOOS_HALF_DEBUG"), "wb");
            if (dbg != NULL) {
                for (int i = 0; i < 4; ++i) {
                    fwrite(input.planes[i], 2u, plane_sizes[i], dbg);
                }
                fwrite(&cfg, 1u, sizeof(cfg), dbg);
                fwrite(&input, 1u, sizeof(input), dbg);
                fclose(dbg);
            }
        }
    }

    /* 1) 编码 packet */
    const size_t cap = tc_frame_packet_bound(&cfg);
    uint8_t* packet = (uint8_t*)malloc(cap);
    if (packet == NULL) { free(mem); fprintf(stderr, "toos: OOM\n"); return 1; }
    topos_frame_stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = (uint32_t)sizeof(stats);
    stats.abi_version = TOPOS_CODEC_ABI_VERSION;
    rc = tc_frame_encode(&cfg, &input, packet, cap, &stats);
    free(mem);
    if (rc != TC_OK) { free(packet); return die(rc, "frame encode"); }

    /* 2) 派生 IDSC（公共函数，禁手拼） */
    topos_image_idsc idsc;
    rc = tc_image_derive_idsc(packet, stats.packet_size, (uint8_t)image_profile, &idsc);
    if (rc != TC_OK) { free(packet); return die(rc, "derive idsc"); }
    /* spec §8：mode2 必须记录实测最大绝对误差（derive 置 0 由写入方覆盖；
     * codec 在 topos_frame_stats.alpha_max_abs_error 输出真实上界） */
    if (cfg.alpha_mode == 2u) {
        idsc.alpha_max_abs_err = (uint8_t)stats.alpha_max_abs_error;
    }

    /* 3) 原子写（spec §12）：同目录临时文件 → 回填 → 自检 → rename */
    char tmp[1024];
    /* 路径超长时 snprintf 静默截断会让临时文件落在意外名字上——显式拒绝 */
    if (snprintf(tmp, sizeof(tmp), "%s.toos.tmp-%ld-%d", out_path, tp_pid(),
                 rand()) >= (int)sizeof(tmp)) {
        free(packet);
        fprintf(stderr, "toos: output path too long (temp name > %zu)\n",
                sizeof(tmp));
        return 1;
    }
    FILE* tf = fopen(tmp, "wb");
    if (tf == NULL) { free(packet); fprintf(stderr, "toos: cannot create temp %s\n", tmp); return 1; }
    file_sink sink;
    topos_io sio;
    file_sink_init(&sink, tf, &sio);
    topos_image_write_params wp;
    memset(&wp, 0, sizeof(wp));
    wp.struct_size = (uint32_t)sizeof(wp);
    wp.abi_version = TOPOS_IMAGE_ABI_VERSION;
    wp.idsc = idsc;
    wp.pixl_data = packet;
    wp.pixl_size = stats.packet_size;
    /* v1.7 TMET：档位 + 厂商标识（422 阶梯 = tier id + 'toos <Quality>
     * <bd>bit' 展示名；v1.8 FLOAT 阶梯同 tier id，label 带 ' float' 后缀
     * （ADR-C057）；其余预设/非预设参数点仅落 vendor——读端展示回退
     * 类名） */
    uint8_t tmet[96];
    topos_image_chunk_in tmet_chunk;
    {
        const uint8_t mtier =
            tier_meta_id_by_params(cfg.pixel_format, cfg.qmatrix_id,
                                   cfg.qp_base, half_mode);
        char label[64];
        label[0] = '\0';
        if (mtier != 0u) {
            static const char* const kQual[5] = {
                "", "Low", "Medium", "High", "Ultra"
            };
            snprintf(label, sizeof(label),
                     half_mode ? "toos %s %ubit float" : "toos %s %ubit",
                     kQual[mtier], (unsigned)bit_depth);
        } else if (tier_id != NULL && strncmp(tier_id, "raw", 3) == 0) {
            /* M3-R1：TRAW 预设文件内展示名（N-6：toos RAW <bd>bit <N>:1，
             * 与应用导出侧同文——档位产物逐位一致验收含 TMET）。 */
            if (strncmp(tier_id, "raw12-", 6) == 0
                    || strncmp(tier_id, "raw16-", 6) == 0) {
                const char* ratio = strrchr(tier_id, '-');
                snprintf(label, sizeof(label), "toos RAW %ubit %s:1",
                         (unsigned)bit_depth, ratio + 1);
            }
        }
        const size_t tmet_len = build_tmet(mtier, label, tmet, sizeof(tmet));
        if (tmet_len != 0u) {
            memset(&tmet_chunk, 0, sizeof(tmet_chunk));
            tmet_chunk.chunk_type = TC_IMG_CHUNK_TMET;
            tmet_chunk.chunk_flags = TC_IMG_CHUNK_FLAG_OPTIONAL
                                     | TC_IMG_CHUNK_FLAG_PRESERVE;
            tmet_chunk.data = tmet;
            tmet_chunk.size = tmet_len;
            wp.extra_chunks = &tmet_chunk;
            wp.extra_count = 1u;
        }
    }
    uint64_t written = 0;
    rc = tc_image_write(&wp, &sio, &written);
    if (rc == TC_OK) {
        if (fflush(tf) != 0 || tp_fsync_file(tf) != 0) { rc = TC_ERR_IO; }
    }
    fclose(tf);
    if (rc != TC_OK || sink.io_failed) {
        tp_unlink(tmp);
        free(packet);
        return die(rc != TC_OK ? rc : TC_IMG_ERR_IO_WRITE_FAILED, "atomic write");
    }
    /* 写后自检：重新 probe+validate 临时文件 */
    {
        FILE* vf = fopen(tmp, "rb");
        topos_io vio;
        memset(&vio, 0, sizeof(vio));
        vio.struct_size = (uint32_t)sizeof(vio);
        vio.abi_version = TOPOS_CODEC_ABI_VERSION;
        vio.ctx = vf;
        vio.read = file_read;
        vio.length = file_length(vf);
        topos_image_info vinfo;
        const int32_t vrc = tc_image_validate(&vio, 0, &vinfo);
        fclose(vf);
        if (vrc != TC_OK) {
            tp_unlink(tmp);
            free(packet);
            return die(vrc, "write-back self check");
        }
    }
    if (tp_rename_replace(tmp, out_path) != 0) {
        const int saved = errno;
        tp_unlink(tmp);
        free(packet);
        fprintf(stderr, "toos: rename failed (%s)\n", strerror(saved));
        return 1;
    }
    printf("%s: wrote %llu bytes (packet %u, qp %u, qm %u, entropy %s, tier %s, alpha %u)\n",
           out_path,
           (unsigned long long)written, (unsigned)stats.packet_size,
           (unsigned)stats.qp_base, (unsigned)cfg.qmatrix_id,
           entropy_vlc == 8 ? "rans2" : (entropy_vlc ? "vlc" : "v1"),
           tier_id != NULL ? tier_id : "-",
           (unsigned)cfg.alpha_mode);
    free(packet);
    return 0;
}

/* —— benchmark（benchmark_protocol：分项计时 + 分位数） —— */
static int cmp_u64(const void* a, const void* b)
{
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static double ns_to_ms(uint64_t ns) { return (double)ns / 1.0e6; }

static uint64_t now_ns(void)
{
#ifdef _WIN32
    /* QPC 单调时钟（MSVC CRT 无 clock_gettime/CLOCK_MONOTONIC） */
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); }
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000000ull) / (uint64_t)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void report_percentiles(const char* label, uint64_t* samples, int n)
{
    qsort(samples, (size_t)n, sizeof(uint64_t), cmp_u64);
    const uint64_t p50 = samples[(int)(0.50 * (n - 1))];
    const uint64_t p95 = samples[(int)(0.95 * (n - 1))];
    const uint64_t p99 = samples[(int)(0.99 * (n - 1))];
    const uint64_t mx = samples[n - 1];
    printf("%-10s p50=%8.3f p95=%8.3f p99=%8.3f max=%8.3f (ms)\n", label,
           ns_to_ms(p50), ns_to_ms(p95), ns_to_ms(p99), ns_to_ms(mx));
}

static int cmd_benchmark(const char* path, int iters)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) { fprintf(stderr, "toos: cannot open %s\n", path); return 1; }
    topos_io io;
    memset(&io, 0, sizeof(io));
    io.struct_size = (uint32_t)sizeof(io);
    io.abi_version = TOPOS_CODEC_ABI_VERSION;
    io.ctx = f;
    io.read = file_read;
    io.length = file_length(f);

    topos_image_info info;
    int32_t rc = tc_image_probe(&io, &info);
    if (rc != TC_OK) { fclose(f); return die(rc, "probe"); }

    uint32_t pw[TC_FRAME_MAX_PLANES] = {0}, ph[TC_FRAME_MAX_PLANES] = {0};
    size_t total = 0;
    for (uint32_t p = 0; p < info.plane_count; ++p) {
        tc_image_query_decode_buffer(&info, p, &pw[p], &ph[p]);
        total += (size_t)pw[p] * ph[p] * 2u;
    }
    uint16_t* mem = (uint16_t*)malloc(total);
    if (mem == NULL) {
        fclose(f);
        fprintf(stderr, "toos: OOM (%llu bytes)\n", (unsigned long long)total);
        return 1;
    }
    uint64_t* t_probe = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)iters);
    uint64_t* t_valid = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)iters);
    uint64_t* t_dec = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)iters);
    if (t_probe == NULL || t_valid == NULL || t_dec == NULL) {
        free(t_probe); free(t_valid); free(t_dec); free(mem); fclose(f);
        fprintf(stderr, "toos: OOM (samples)\n");
        return 1;
    }
    topos_plane_view views[TC_FRAME_MAX_PLANES];
    memset(views, 0, sizeof(views)); /* 未用 plane 槽必须为 0（契约） */
    size_t off = 0;
    for (uint32_t p = 0; p < info.plane_count; ++p) {
        memset(&views[p], 0, sizeof(views[p]));
        views[p].struct_size = (uint32_t)sizeof(views[p]);
        views[p].abi_version = TOPOS_CODEC_ABI_VERSION; /* plane_view 为 codec 域结构 */
        views[p].pixels = mem + off / 2u;
        views[p].stride = 0;
        off += (size_t)pw[p] * ph[p] * 2u;
    }

    topos_frame_output out;
    printf("benchmark: %s (%ux%u planes=%u bd=%u, %d iters, hot cache)\n", path,
           (unsigned)info.visible_width, (unsigned)info.visible_height,
           (unsigned)info.plane_count, (unsigned)info.bit_depth, iters);
    for (int i = 0; i < iters; ++i) {
        uint64_t t0 = now_ns();
        rc = tc_image_probe(&io, &info);
        uint64_t t1 = now_ns();
        if (rc != TC_OK) {
            free(t_probe); free(t_valid); free(t_dec); free(mem); fclose(f);
            return die(rc, "bench probe");
        }
        rc = tc_image_validate(&io, 0, &info);
        uint64_t t2 = now_ns();
        if (rc != TC_OK) {
            free(t_probe); free(t_valid); free(t_dec); free(mem); fclose(f);
            return die(rc, "bench validate");
        }
        memset(&out, 0, sizeof(out));
        out.struct_size = (uint32_t)sizeof(out);
        out.abi_version = TOPOS_CODEC_ABI_VERSION;
        rc = tc_image_decode(&io, views, &out);
        uint64_t t3 = now_ns();
        if (rc < 0) {
            free(t_probe); free(t_valid); free(t_dec); free(mem); fclose(f);
            return die(rc, "bench decode");
        }
        t_probe[i] = t1 - t0;
        t_valid[i] = t2 - t1;
        t_dec[i] = t3 - t2;
    }
    report_percentiles("probe", t_probe, iters);
    report_percentiles("validate", t_valid, iters);
    report_percentiles("decode", t_dec, iters);
    free(t_probe); free(t_valid); free(t_dec);
    free(mem);
    fclose(f);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: toos probe|verify|decode|encode|benchmark <args>\n"
                "  toos probe <file.toos>\n"
                "  toos verify <file.toos> [--deep]\n"
                "  toos decode <file.toos> -o <out.raw>\n"
                "  toos encode <in.raw> -o <out.toos> [options]\n"
                "  toos benchmark <file.toos> [--iters N]\n");
        return 2;
    }
    srand((unsigned)(now_ns() & 0xFFFFFFFFu));
    const char* cmd = argv[1];
    if (strcmp(cmd, "probe") == 0) { return cmd_probe(argv[2]); }
    if (strcmp(cmd, "verify") == 0) {
        int deep = 0;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--deep") == 0) { deep = 1; }
        }
        return cmd_verify(argv[2], deep);
    }
    if (strcmp(cmd, "decode") == 0) {
        const char* out = NULL;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { out = argv[++i]; }
        }
        if (out == NULL) {
            fprintf(stderr, "usage: toos decode <file.toos> -o <out.raw>\n");
            return 2;
        }
        return cmd_decode(argv[2], out);
    }
    if (strcmp(cmd, "encode") == 0) { return cmd_encode(argc - 2, argv + 2); }
    if (strcmp(cmd, "benchmark") == 0) {
        int iters = 20;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) { iters = atoi(argv[++i]); }
        }
        return cmd_benchmark(argv[2], iters);
    }
    fprintf(stderr, "toos: unknown command '%s'\n", cmd);
    return 2;
}
