/* topos_image.h —— Topos Image（.toos / TPIM envelope）公共 C ABI（阶段 1 起）。
 *
 * 规范：docs/image/topos_image_file_spec_v0.md（阶段 0 冻结）；
 * 决策：docs/image/ADR-I001-stage0-decisions.md。
 *
 * 分层契约（冻结）：
 *  - 本 ABI 只做文件封装（preamble/directory/chunk/IDSC）与 PIXL 搬运；
 *    一切像素编解码复用 topos_codec.h 的同一套核心（禁止第二实现）；
 *  - I/O 走 topos_codec.h 的 topos_io 回调（read / write / seek_write），
 *    核心不做文件系统操作；原子临时文件 + rename 策略属 CLI/适配器层；
 *  - 所有公开结构遵循 struct_size / abi_version 首两字段约定，全 0 初始化
 *    （除两字段）即合法最小配置；
 *  - 单调用无状态、可多线程并发（错误详情线程局部，同 codec 契约）。
 */
#ifndef TOPOS_IMAGE_H
#define TOPOS_IMAGE_H

#include "topos_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* image 层 ABI 版本：结构布局或函数语义不兼容变更 +1 */
#define TOPOS_IMAGE_ABI_VERSION 1

/* —— image 层错误码（spec §11；复用 codec TC_ERR_* 语义，-100 段为本层新增） —— */
#define TC_IMG_ERR_BAD_MAGIC            (-100) /* preamble magic 非 TPIM（含裸 TPIC packet 误当 .toos） */
#define TC_IMG_ERR_BAD_PREAMBLE         (-101) /* preamble 字段/版本/尺寸/CRC 非法 */
#define TC_IMG_ERR_BAD_DIRECTORY        (-102) /* 目录越界/重叠/乱序/重复 critical/缺失必需 chunk */
#define TC_IMG_ERR_UNKNOWN_CRITICAL     (-103) /* 未知 critical chunk */
#define TC_IMG_ERR_CHUNK_CONFLICT       (-104) /* IDSC ↔ PIXL 交叉校验失败（spec §6.1） */
#define TC_IMG_ERR_METADATA_CONFLICT    (-105) /* ICC/CICP/OCIO 矛盾（阶段 3 起使用） */
#define TC_IMG_ERR_LIMIT                (-106) /* image 层硬上限（chunk 长度/count 等） */
#define TC_IMG_ERR_IO_WRITE_FAILED      (-107) /* 原子写失败（临时文件/rename/flush，CLI 层映射） */

/* —— chunk FourCC（big-endian u32 视角，即文件字节序 'I','D','S','C'） —— */
#define TC_IMG_CHUNK_IDSC  0x49445343u /* 'IDSC' critical 图片描述 */
#define TC_IMG_CHUNK_PIXL  0x5049584Cu /* 'PIXL' critical TPIC elementary packet */
#define TC_IMG_CHUNK_ICCP  0x49434350u /* 'ICCP' optional ICC profile */
#define TC_IMG_CHUNK_OCIO  0x4F43494Fu /* 'OCIO' optional OCIO colorspace 名称 */
#define TC_IMG_CHUNK_XMP   0x584D5020u /* "XMP " optional（第 4 字节为空格） */
#define TC_IMG_CHUNK_EXIF  0x45584946u /* 'EXIF' optional */
#define TC_IMG_CHUNK_THMB  0x54484D42u /* 'THMB' optional 可丢弃缩略图 */
#define TC_IMG_CHUNK_HASH  0x48415348u /* 'HASH' optional SHA-256 摘要（36B） */
/* v1.7：容器元数据（档位/厂商；载荷与 MOV tpcD 同构，见
 * src/shared/codec/topos_meta.py 与 container_spec v1.7 §4） */
#define TC_IMG_CHUNK_TMET  0x544D4554u /* 'TMET' optional 档位/厂商元数据 */
/* RC1（M6）：TRAW 开发元数据（as-shot WB/EI/black level/cfa_layout/
 * sensor matrix 引用）——载荷结构归宿主 Python 侧（topos_meta 同模式：
 * native 视角为带限额的不透明 optional chunk，OPTIONAL|PRESERVE） */
#define TC_IMG_CHUNK_TRWM  0x5452574Du /* 'TRWM' optional TRAW 元数据 */
/* 预留：v1 读写端按 unknown 处理，writer 拒绝产出 */
#define TC_IMG_CHUNK_TILE  0x54494C45u
#define TC_IMG_CHUNK_TDIR  0x54444952u
#define TC_IMG_CHUNK_MIPM  0x4D49504Du
#define TC_IMG_CHUNK_AUXC  0x41555843u

/* chunk_flags（spec §4） */
#define TC_IMG_CHUNK_FLAG_CRITICAL  1u
#define TC_IMG_CHUNK_FLAG_OPTIONAL  2u
#define TC_IMG_CHUNK_FLAG_PRESERVE  4u  /* 可编辑 round-trip 字节级保留 */
#define TC_IMG_CHUNK_FLAG_DROPPABLE 8u  /* 如 THMB；允许写入端丢弃 */

/* —— 固定布局尺寸（spec §3/§4/§6；逐 offset 冻结） —— */
#define TC_IMG_PREAMBLE_SIZE    64u
#define TC_IMG_DIR_ENTRY_SIZE   32u
#define TC_IMG_IDSC_SIZE        128u

/* —— 硬上限（spec §10；超限 → TC_ERR_LIMIT_EXCEEDED / TC_IMG_ERR_LIMIT） —— */
#define TC_IMG_MAX_CHUNKS       64u
#define TC_IMG_MAX_CHUNK_PIXL   268435456ull /* 256 MiB == TC_MAX_PACKET_SIZE */
#define TC_IMG_MAX_CHUNK_ICCP   8388608ull   /* 8 MiB */
#define TC_IMG_MAX_CHUNK_OCIO   4096ull
#define TC_IMG_MAX_CHUNK_XMP    16777216ull  /* 16 MiB */
#define TC_IMG_MAX_CHUNK_EXIF   16777216ull  /* 16 MiB */
#define TC_IMG_MAX_CHUNK_THMB   4194304ull   /* 4 MiB */
#define TC_IMG_MAX_CHUNK_HASH   36ull        /* 1B algo + 3B reserved + 32B 摘要 */
#define TC_IMG_MAX_CHUNK_TRWM   256ull       /* TRWM v1 固定 40B；余量给扩展代 */

/* —— IDSC 常量（spec §6；0 保留值语义见各字段规则） —— */
#define TC_IMG_CHANNEL_MODEL_RGB   0u
#define TC_IMG_CHANNEL_MODEL_YUV   1u
#define TC_IMG_CHANNEL_MODEL_GRAY  2u
#define TC_IMG_CHANNEL_MODEL_CFA   3u /* TRAW（批 1）：Bayer 相位平面 R/Gr/Gb/B */
#define TC_IMG_SAMPLE_KIND_UINT    0u
/* HALF（2026-09-19 激活，spec §15）：IEEE 754 binary16 样本经冻结单调映射
 * （tc_image_half_to_codes）转 u16 码值后进 bd=16 整数管线——压缩方式与
 * UINT 完全一致，样本域语义由 sample_kind 声明。仅 image_profile 4 可用。 */
#define TC_IMG_SAMPLE_KIND_HALF    1u
#define TC_IMG_LAYOUT_PLANAR       1u
#define TC_IMG_ALPHA_ABSENT        0u
#define TC_IMG_ALPHA_STRAIGHT      1u
#define TC_IMG_ALPHA_PREMULT       2u
#define TC_IMG_CODEC_ID_TPIC       0u
#define TC_IMG_PROFILE_PREVIEW     0u /* YUV 4:2:2 10/12-bit（codec profile 3） */
#define TC_IMG_PROFILE_HQ          1u /* GBR 4:4:4 10-bit（codec profile 5） */
#define TC_IMG_PROFILE_XQ          2u /* GBR 4:4:4 12-bit（codec profile 5/6） */
/* TRAW 拍板（2026-09-13）：原 reserved 值 3（Image Lossless，ADR-I001 D9）
 * 改派 Image RAW——Lossless 档移除，无损语义由 RAW Linear 模式（批 4）承载 */
#define TC_IMG_PROFILE_RAW         3u /* Bayer CFA 12-bit（codec profile 7，TRAW） */
/* HALF（2026-09-19）：GBR 4:4:4 float16 —— 内层 pf=2/bd=16/codec profile 5，
 * sample_kind=HALF；HDR 线性合成中间件 / 精确缓存（qp=0 无损） */
#define TC_IMG_PROFILE_HALF_FLOAT  4u

#define TC_IMG_IDSC_FLAG_ORIENTATION_NORMALIZED 0x1u

/* ================ 解析结构（host 字节序；pack/unpack 显式偏移） ================ */

typedef struct topos_image_preamble {
    uint32_t struct_size;
    uint32_t abi_version;
    uint16_t file_version_major;   /* v1 = 1 */
    uint16_t file_version_minor;   /* v1 = 0 */
    uint32_t flags;                /* 未定义位必须 0 */
    uint64_t file_size;            /* == 实际文件长度 */
    uint64_t directory_offset;
    uint32_t directory_entry_size; /* v1 = 32 */
    uint32_t directory_count;      /* [2, TC_IMG_MAX_CHUNKS] */
    uint32_t primary_image_index;  /* v1 = 0 */
    uint32_t compatibility_flags;  /* 提示位（非权威） */
    uint32_t header_crc32;         /* 对 64B、本字段置 0 */
    uint32_t directory_crc32;
    uint32_t reserved[4];
} topos_image_preamble;

typedef struct topos_image_dir_entry {
    uint32_t chunk_type;    /* FourCC */
    uint32_t chunk_flags;   /* TC_IMG_CHUNK_FLAG_* */
    uint64_t chunk_offset;  /* 绝对偏移 */
    uint64_t chunk_size;
    uint32_t chunk_crc32;
    uint32_t reserved;      /* 必须 0 */
} topos_image_dir_entry;

/* IDSC —— spec §6 逐字段；含义与规则见规范，此处仅承载 */
typedef struct topos_image_idsc {
    uint32_t struct_size;
    uint32_t abi_version;
    uint16_t idsc_version_major;   /* = 1 */
    uint16_t idsc_version_minor;   /* = 0 */
    uint32_t flags;                /* bit0 = orientation_normalized */
    int32_t display_x_min, display_y_min, display_x_max, display_y_max;
    int32_t data_x_min, data_y_min, data_x_max, data_y_max;
    uint32_t orientation;          /* 1..8（EXIF 语义）；v1 writer 只写 1 */
    uint32_t pixel_aspect_num;     /* ≥ 1 */
    uint32_t pixel_aspect_den;     /* ≥ 1 */
    uint8_t channel_model;         /* TC_IMG_CHANNEL_MODEL_* */
    uint8_t channel_count;         /* 3/4 */
    uint8_t sample_kind;           /* TC_IMG_SAMPLE_KIND_UINT（v1） */
    uint8_t valid_bit_depth;       /* 8/10/12 */
    uint8_t container_bit_depth;   /* 10/12/16 */
    uint8_t storage_layout;        /* TC_IMG_LAYOUT_PLANAR（v1） */
    uint8_t subsampling;           /* 0=4:4:4 1=4:2:2 2=4:2:0 */
    uint8_t chroma_siting;         /* codec 域；4:4:4 → 0 */
    uint8_t alpha_presence;        /* TC_IMG_ALPHA_* */
    uint8_t alpha_mode;            /* codec 0/1/2 */
    uint8_t alpha_bit_depth;       /* mode1=16；mode2∈{8,10,12}；absent=0 */
    uint8_t alpha_max_abs_err;     /* mode2 上界；否则 0 */
    uint8_t codec_id;              /* TC_IMG_CODEC_ID_TPIC（v1） */
    uint8_t payload_major;         /* == 内层 packet version_major（V1..V6） */
    uint8_t payload_minor;         /* == 内层 packet version_minor */
    uint8_t image_profile;         /* TC_IMG_PROFILE_*（3 = RAW/TRAW） */
    uint8_t codec_profile;         /* 内层 profile（3/5/6/7） */
    uint8_t pixel_format;          /* 内层 0/1/2/3（3 = CFA） */
    uint8_t color_primaries;       /* CICP，== 内层 */
    uint8_t color_transfer;
    uint8_t color_matrix;          /* GBR identity → 0 */
    uint8_t color_range;           /* 0 limited / 1 full */
    uint8_t iccp_ref;              /* 0/1：ICCP chunk 存在且为色彩表征权威 */
    uint8_t ocio_ref;              /* 0/1 */
    uint32_t payload_header_crc32; /* 绑定摘要：内层 frame header CRC */
} topos_image_idsc;

/* probe/validate 结果（preamble + directory 关键项 + IDSC + 便捷镜像） */
typedef struct topos_image_info {
    uint32_t struct_size;
    uint32_t abi_version;
    topos_image_preamble preamble;
    topos_image_idsc idsc;
    uint64_t pixl_offset;
    uint64_t pixl_size;
    /* 内层 packet 便捷镜像（probe 时已按 spec §6.1 与 IDSC 交叉校验） */
    uint16_t visible_width;
    uint16_t visible_height;
    uint8_t plane_count;           /* 平面总数（含 alpha plane：无 alpha=3，有=4；镜像自内层 frame header） */
    uint8_t has_alpha;             /* 0/1 */
    uint8_t bit_depth;
    uint8_t profile;
    uint8_t pixel_format;
    uint8_t alpha_mode;
    uint8_t alpha_bit_depth;
    uint8_t alpha_premultiplied;
    uint32_t reserved[8];
} topos_image_info;

/* ================ 阶段 1：probe / validate / decode ================ */

/* 任意 status（含未知值）→ 非空静态串；未知码返回 "unknown status" */
const char* tc_image_status_message(int32_t status);

/* 只解析 preamble、directory 与 IDSC + 内层 packet header（53B）。
 * 不分配像素平面、不读 PIXL 全量（目录 ≤ 2 KiB 级临时分配）。
 * 校验顺序 = spec §2.11；失败时 *out 不完整（前缀已填部分仅供诊断）。 */
int32_t tc_image_probe(const topos_io* io, topos_image_info* out);

/* validate：probe 全部检查 + 逐 chunk 流式 CRC 校验（64 KiB 块，不整块分配）。
 * flags：TC_IMG_VALIDATE_DEEP = 追加整读 PIXL 并对内层 packet 做结构探测。 */
#define TC_IMG_VALIDATE_DEEP 1u
int32_t tc_image_validate(const topos_io* io, uint32_t flags, topos_image_info* out);

/* 解码到 caller-provided planes。planes[i] 需填 pixels/stride（struct_size/
 * abi_version 须合法）；几何由 tc_image_query_decode_buffer 或 info 镜像决定。
 * 全程语义 == 对 PIXL 直接调用 tc_frame_decode（无层间分叉）；
 * 返回码、conceal 语义与 codec 一致（TC_WARN_CONCEALED = 帧仍交付）。 */
int32_t tc_image_decode(const topos_io* io,
                        const topos_plane_view planes[TC_FRAME_MAX_PLANES],
                        topos_frame_output* out_info);

/* 解码到 caller-provided 的目标尺寸平面（仅允许缩小，不放大）。目标几何
 * 按 visible_width/height 计算，4:2:2 的 U/V 宽度仍为 ceil(width/2)。
 * 目标严格匹配 1/2、1/3、1/4、1/8 时使用 codec reduced request，否则使用
 * 精确 scaled request；两者都消除 Python/numpy 中间缩放副本。现有 V1/V2
 * 码流仍须顺序消费全部变长熵码字，真正跳过高频字节段属于 V7-A/V7-B。
 * out_info 的 visible/coded 尺寸为目标尺寸，slice 状态沿用源帧解码结果。
 * 这是增量 API，不改变 tc_image_decode 的既有语义。 */
int32_t tc_image_decode_preview(const topos_io* io,
                                uint32_t target_width,
                                uint32_t target_height,
                                const topos_plane_view planes[TC_FRAME_MAX_PLANES],
                                topos_frame_output* out_info);

/* plane 的 visible 尺寸（uint16 元素计；含 alpha 平面 plane=3）。
 * info 须来自成功的 tc_image_probe/validate。 */
int32_t tc_image_query_decode_buffer(const topos_image_info* info, uint32_t plane,
                                     uint32_t* width, uint32_t* height);

/* ================ 阶段 3：可选 metadata chunk 受限读取 ================ */

/* 读取指定 optional chunk 的原始字节（ICCP/OCIO/XMP/EXIF/THMB/HASH 及未知
 * optional 均可；IDSC/PIXL/critical → INVALID_ARGUMENT）。
 * 读取时执行受限校验（spec §7，ADR-I003）：
 *   - 长度硬上限（§10）超限 → TC_IMG_ERR_LIMIT；
 *   - OCIO：必须为可打印 UTF-8 名称，禁止路径分隔符/驱动器/脚本与动态库
 *     后缀 → TC_IMG_ERR_METADATA_CONFLICT；
 *   - ICCP：≥ 132B 且 offset 36 为 'acsp'，colorSpace 必须与 IDSC 通道模型
 *     相符（RGB ⇔ 'RGB '）→ 否则 TC_IMG_ERR_METADATA_CONFLICT。
 * 调用约定：buffer==NULL 或 cap==0 → 只查询 *out_size（TC_OK）；
 * cap 不足 → TC_ERR_BUFFER_TOO_SMALL（不写 buffer）；
 * 内容校验失败 → 返回相应错误码且 *out_size 复位为 0（调用方仍应
 * 以返回码为准）。
 * info 须来自成功的 tc_image_probe/validate（同一 io）。 */
int32_t tc_image_read_metadata(const topos_io* io, const topos_image_info* info,
                               uint32_t chunk_type, void* buffer, size_t cap,
                               size_t* out_size);

/* ================ 阶段 4：能力查询（单一真相源见 docs/image/capability_manifest.json） ================ */

/* image 层能力描述（读写端以实测登记为准；reserved 组合不在此出现） */
typedef struct topos_image_capabilities {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t file_version_major;   /* 本库可读写的 envelope major（=1） */
    uint32_t file_version_minor;
    uint32_t max_coded_dim;        /* 16384（上游 TC_MAX_CODED_DIM） */
    uint64_t max_pixl_bytes;       /* 256 MiB */
    uint32_t max_chunks;
    uint32_t profiles_mask;        /* bit i = image_profile i 可读写（bit3 = RAW/TRAW） */
    uint32_t metadata_mask;        /* bit0 ICCP bit1 OCIO bit2 XMP bit3 EXIF bit4 THMB bit5 HASH */
    uint32_t tile_roi : 1;         /* 恒 0（v1 非目标） */
    uint32_t half_float : 1;       /* 1：sample_kind=HALF 可读写（2026-09-19 激活） */
    uint32_t multi_image : 1;      /* 恒 0 */
    uint32_t reserved_bits : 29;
    uint32_t reserved[6];
} topos_image_capabilities;

/* 查询本库能力。out==NULL 或 struct_size 不符 → INVALID_ARGUMENT。
 * 纯函数，无失败路径以外的状态；组合是否真的可编码还须经
 * topos_codec.h tc_query_support() 按具体 profile/pf/bd/alpha 协商。 */
int32_t tc_image_query_capabilities(topos_image_capabilities* out);

/* ================ 阶段 5：HALF 样本域映射（host 侧便利 API；非码流语义） ================
 *
 * spec §15 冻结映射的批量执行（编码前 half→码值 / 解码后码值→half）：
 *   code = (h & 0x8000) ? 0x8000 - (h & 0x7FFF) : 0x8000 + (h & 0x7FFF)
 * 有限值严格单调（对称对数域），±0 → 0x8000；全部有限 half 与码值双射，
 * qp=0 时整条整数管线逐位可逆。src==dst 允许（逐元素独立，可原地映射）。
 * n==0 为合法空操作；任一指针 NULL → TC_ERR_INVALID_ARGUMENT。
 * 与 Python/numpy 参考实现（topos_image_binding）逐位一致，由双方测试钉死
 * （同 P1-12 tc_convert_packed_rgb 契约模式）。 */
int32_t tc_image_half_to_codes(const uint16_t* half, size_t n, uint16_t* codes);
int32_t tc_image_codes_to_half(const uint16_t* codes, size_t n, uint16_t* half);

/* 从一个合法 TPIC elementary packet 派生与之十项交叉一致的 IDSC（spec §6.1）。
 * image_profile：TC_IMG_PROFILE_*，须与 packet 内容匹配（GBR10→HQ、GBR12→XQ、
 * YUV422→Preview、CFA 12-bit→RAW），否则 tc_image_write 的交叉校验会拒绝。
 * 编码器/适配器用本函数生成 IDSC，禁止各自手拼字段。 */
int32_t tc_image_derive_idsc(const void* packet, size_t size, uint8_t image_profile,
                             topos_image_idsc* out);

/* ================ 阶段 1：写入（envelope 序列化） ================ */

typedef struct topos_image_chunk_in {
    uint32_t chunk_type;   /* FourCC；不得为 IDSC/PIXL/预留值 */
    uint32_t chunk_flags;  /* 必须含 OPTIONAL 位；known optional 长度受限 */
    const void* data;      /* 调用方所有，仅调用期间引用 */
    size_t size;
} topos_image_chunk_in;

typedef struct topos_image_write_params {
    uint32_t struct_size;
    uint32_t abi_version;
    topos_image_idsc idsc;      /* 写前做完整字段校验 + 与 PIXL 交叉校验 */
    const void* pixl_data;      /* 完整 TPIC elementary packet（含 53B header） */
    size_t pixl_size;
    const topos_image_chunk_in* extra_chunks; /* 可选 chunk（按给定顺序写盘） */
    uint32_t extra_count;       /* ≤ TC_IMG_MAX_CHUNKS-2 */
    uint32_t compatibility_flags; /* preamble 提示位 */
    uint32_t reserved[8];       /* 全 0 */
} topos_image_write_params;

/* 序列化 TPIM envelope 到 sink（write 追加 + seek_write 回填，同 MOV mux 契约）。
 * 成功时 *out_size = 写入总长（== envelope file_size）。确定性：相同输入 →
 * 逐字节相同输出（golden 依赖）。
 * 失败语义：IDSC 字段/交叉校验失败 → CHUNK_CONFLICT / INVALID_ARGUMENT；
 * 预留 FourCC、critical 位或超限 chunk → INVALID_ARGUMENT / LIMIT；
 * sink 失败 → TC_ERR_IO（此时 sink 内容不完整，由调用层负责丢弃临时文件）。 */
int32_t tc_image_write(const topos_image_write_params* params, const topos_io* sink,
                       uint64_t* out_size);

#ifdef __cplusplus
}
#endif

#endif /* TOPOS_IMAGE_H */
