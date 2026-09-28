#ifndef TOPOS_INTERNAL_CPUDETECT_H
#define TOPOS_INTERNAL_CPUDETECT_H

#include <stdint.h>

/* 运行时 CPU 能力探测（阶段 1 骨架：只探测、不 dispatch —— 计划阶段 1 任务，
 * SIMD 实现属阶段 9）。返回 TOPOS_CPU_* 位或。 */
uint32_t tc_internal_cpu_flags(void);

#endif /* TOPOS_INTERNAL_CPUDETECT_H */
