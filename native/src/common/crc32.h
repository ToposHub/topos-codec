/* CRC-32 IEEE 802.3（spec §2.6/A.6，冻结）。
 * poly 0xEDB88320（反射）、init 0xFFFFFFFF、输出异或 0xFFFFFFFF，逐字节 LSB-first。
 * 自测向量："123456789" → 0xCBF43926（test_crc32.c 强制，位级参考实现交叉验证）。
 */
#ifndef TOPOS_INTERNAL_CRC32_H
#define TOPOS_INTERNAL_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* 逐字节表驱动；对 data[0..size) 返回 CRC-32 IEEE。size==0 → 0x00000000 */
uint32_t tc_crc32(const void* data, size_t size);

/* 增量式（容器阶段 5 分块校验使用）：crc 传入上一次返回值，首段传 0 */
uint32_t tc_crc32_update(uint32_t crc, const void* data, size_t size);

#endif /* TOPOS_INTERNAL_CRC32_H */
