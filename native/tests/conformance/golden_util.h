/* golden 向量共享枚举（gen/check 双模式共用，保证字节一致性）。
 *
 * 记录布局（小端，直接写 struct 字段数组，无 padding —— 手工序列化）：
 *   u8 depth | u8 qp | u16 px[64] | i32 F[64] | i32 q[64] | i32 Fp[64] | u16 rec[64]
 * 记录数确定性枚举：depths{10,12} × qps{0,4,20,40,63} ×
 *   图案 {4×常量, 64×单脉冲, 3×渐变, 3×棋盘, 64×随机}。
 */
#ifndef GOLDEN_UTIL_H
#define GOLDEN_UTIL_H

#include <stdint.h>
#include <stddef.h>

#define GOLDEN_MAGIC "TPG1"
#define GOLDEN_VERSION 1u
#define GOLDEN_RECORD_SIZE (2 + 64 * 2 + 64 * 4 + 64 * 4 + 64 * 4 + 64 * 2)

typedef struct golden_record {
    uint8_t depth;
    uint8_t qp;
    uint16_t px[64];
    int32_t F[64];
    int32_t q[64];
    int32_t Fp[64];
    uint16_t rec[64];
} golden_record;

typedef void (*golden_emit_fn)(const golden_record* rec, void* ud);

/* 枚举全部记录（确定性）；返回记录数 */
size_t golden_enumerate(golden_emit_fn emit, void* ud);

/* 序列化（小端字节流，无对齐假设） */
void golden_serialize(const golden_record* rec, uint8_t out[GOLDEN_RECORD_SIZE]);

/* 文件尾校验：全部记录字节的 64 位折叠异或（旋转 13） */
uint64_t golden_checksum(const uint8_t* records_blob, size_t total_bytes);

#endif /* GOLDEN_UTIL_H */
