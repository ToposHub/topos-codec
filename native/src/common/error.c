#include "common/error.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>

#include "topos_codec.h"

static _Thread_local char tc_tl_detail[TC_ERROR_DETAIL_MAX] = { 0 };

void tc_set_error(int32_t status, const char* fmt, ...)
{
    va_list ap;
    int written = snprintf(tc_tl_detail, TC_ERROR_DETAIL_MAX,
                           "[%" PRId32 "] ", status);
    if (written < 0) {
        tc_tl_detail[0] = '\0';
        return;
    }
    if ((size_t)written >= TC_ERROR_DETAIL_MAX) {
        written = TC_ERROR_DETAIL_MAX - 1;
    }
    va_start(ap, fmt);
    (void)vsnprintf(tc_tl_detail + (size_t)written,
                    TC_ERROR_DETAIL_MAX - (size_t)written, fmt, ap);
    va_end(ap);
    /* vsnprintf 负责截断 + NUL；无需额外处理 */
}

const char* tc_last_error(void)
{
    return tc_tl_detail;
}

void tc_wrap_error(int32_t status, const char* prefix)
{
    /* C14：先快照（副本）再重写——tc_set_error 直接以 tc_last_error() 作
     * "%s" 参数会重写同一线程局部缓冲后从中读取（重叠读写），内层详情
     * 丢失且属未定义行为。 */
    char snap[TC_ERROR_DETAIL_MAX];
    size_t n = 0u;
    while (n + 1u < sizeof(snap) && tc_tl_detail[n] != '\0') {
        snap[n] = tc_tl_detail[n];
        ++n;
    }
    snap[n] = '\0';
    tc_set_error(status, "%s%s", prefix, snap);
}
