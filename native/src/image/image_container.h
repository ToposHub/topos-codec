/* image_container —— TPIM 固定布局显式 pack/unpack + IDSC 规则（lib 内部）。
 *
 * 规范：docs/image/topos_image_file_spec_v0.md §2/§3/§4/§6（阶段 0 冻结）。
 * 所有来自文件的 offset/size/count 先做 checked arithmetic 与硬上限校验；
 * big-endian 显式偏移读写，禁止序列化 C struct。
 */
#ifndef TOPOS_INTERNAL_IMAGE_CONTAINER_H
#define TOPOS_INTERNAL_IMAGE_CONTAINER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "topos_codec.h"
#include "topos_image.h"

/* 内层 packet header 交叉校验需要（src/ 为 lib PRIVATE include 根） */
#include "bitstream/frame_header.h"

/* magic / FourCC 字节 */
#define TCI_MAGIC_TPIM 0x5450494Du /* 'T','P','I','M' */

/* 已知 optional chunk 的长度上限查询；known → true 且 *limit 填充 */
bool tci_chunk_limit(uint32_t type, uint64_t* limit);

/* type 是否为 v1 writer 拒绝产出的预留 FourCC */
bool tci_chunk_reserved(uint32_t type);

/* —— preamble（64B） —— */
int32_t tci_pack_preamble(const topos_image_preamble* p, uint8_t out[TC_IMG_PREAMBLE_SIZE]);
/* 解包 + 全字段规则（magic/file_size/preamble_size/entry_size/count/flags/
 * reserved/两 CRC）；crc_ok 传入 directory 原始字节是否已验证前可 NULL ——
 * 注意：directory CRC 在 reader 拿到目录字节后单独校验，本函数只查规则字段。 */
int32_t tci_unpack_preamble(const uint8_t in[TC_IMG_PREAMBLE_SIZE],
                            topos_image_preamble* out);

/* —— directory entry（32B） —— */
int32_t tci_pack_dir_entry(const topos_image_dir_entry* e, uint8_t out[TC_IMG_DIR_ENTRY_SIZE]);
int32_t tci_unpack_dir_entry(const uint8_t in[TC_IMG_DIR_ENTRY_SIZE],
                             topos_image_dir_entry* out);

/* —— IDSC（128B） —— */
int32_t tci_pack_idsc(const topos_image_idsc* s, uint8_t out[TC_IMG_IDSC_SIZE]);
int32_t tci_unpack_idsc(const uint8_t* in, size_t size, topos_image_idsc* out);

/* IDSC 字段规则（不含与 PIXL 的交叉校验） */
int32_t tci_idsc_validate(const topos_image_idsc* s);

/* IDSC ↔ 内层 packet header 交叉校验（spec §6.1 十项）。
 * raw 为序列化 53B header（绑定摘要 payload_header_crc32 = 其前 49B 的 CRC-32）。
 * 失败统一 TC_IMG_ERR_CHUNK_CONFLICT 并写 last_error 定位项。 */
int32_t tci_idsc_crosscheck(const topos_image_idsc* s, const topos_frame_header* fh,
                            const uint8_t raw[TC_FRAME_HEADER_SIZE]);

/* —— 阶段 3：metadata 受限校验（读写端共用，spec §7/§10、ADR-I003） —— */

/* OCIO：可打印 UTF-8 名称；禁止控制字符、路径分隔符（/ \\ :)与动态库后缀 */
int32_t tci_validate_ocio(const uint8_t* data, size_t size);

/* ICCP：≥132B 且 offset 36 为 'acsp'；channel_model=RGB 时 colorSpace 必须
 * 为 'RGB '（offset 16..19）。深度 primaries↔CICP 比对属 v1.1 扩展。 */
int32_t tci_validate_iccp(const uint8_t* data, size_t size, uint8_t channel_model);

#endif /* TOPOS_INTERNAL_IMAGE_CONTAINER_H */
