/* 错误详情 —— 内部：线程局部最近错误（公共查询入口 tc_last_error，spec §9）。
 *
 * 语义：tc_set_error 只在 lib 内部调用；每次失败的 API 返回前恰好写一次；
 * 成功的调用不清除（调用方以返回码为准，详情仅供诊断）。
 */
#ifndef TOPOS_INTERNAL_ERROR_H
#define TOPOS_INTERNAL_ERROR_H

#include <stdint.h>

#define TC_ERROR_DETAIL_MAX 256

/* 记录本线程最近错误："[status] 格式化详情"（超长截断，保证 NUL 结尾） */
void tc_set_error(int32_t status, const char* fmt, ...);

/* C14（2026-09-27 检查计划）：安全错误包装——快照当前线程局部详情后
 * 重写为 "[status] prefix<快照>"。tc_set_error(..., "%s", tc_last_error())
 * 是同一线程局部缓冲上的重叠读写（先重写再读），原始诊断丢失；包装
 * 调用一律走本入口。 */
void tc_wrap_error(int32_t status, const char* prefix);

#endif /* TOPOS_INTERNAL_ERROR_H */
