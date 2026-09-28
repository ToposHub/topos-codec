/* 线程局部错误详情：覆盖写/截断/线程隔离 */
#include "common/error.h"
#include "topos_codec.h"
#include "mini_test.h"

#include <string.h>

#if !defined(_WIN32)
#include <pthread.h>

static void* mt_error_thread_fn(void* arg)
{
    (void)arg;
    tc_set_error(TC_ERR_IO, "child-payload");
    return NULL;
}
#endif

int main(void)
{
    /* 初始态为空串 */
    MT_CHECK(strcmp(tc_last_error(), "") == 0);

    /* 格式化 + 覆盖写 */
    tc_set_error(TC_ERR_MALFORMED, "slice %u plane %d", 7u, 2);
    MT_CHECK(strstr(tc_last_error(), "slice 7 plane 2") != NULL);
    MT_CHECK(strstr(tc_last_error(), "[-9]") != NULL);
    tc_set_error(TC_ERR_TRUNCATED, "overwritten");
    MT_CHECK(strcmp(tc_last_error(), "[-10] overwritten") == 0);

    /* C14：安全包装——内层详情完整保留（历史 tc_set_error(..., "%s",
     * tc_last_error()) 是同缓冲重叠读写，内层丢失） */
    tc_set_error(TC_ERR_MALFORMED, "inner-diagnostic xyz");
    tc_wrap_error(TC_ERR_IO, "wrap: ");
    MT_CHECK(strcmp(tc_last_error(), "[-14] wrap: [-9] inner-diagnostic xyz") == 0);

    /* 超长截断：NUL 结尾、长度受 TC_ERROR_DETAIL_MAX 约束 */
    char big[800];
    memset(big, 'A', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    tc_set_error(TC_ERR_IO, "%s", big);
    MT_CHECK(strlen(tc_last_error()) < TC_ERROR_DETAIL_MAX);
    MT_CHECK(strstr(tc_last_error(), "[-14]") != NULL);

    /* 线程隔离：子线程写不影响主线程 */
#if !defined(_WIN32)
    tc_set_error(TC_ERR_CANCELLED, "main-marker");
    pthread_t th;
    MT_CHECK(pthread_create(&th, NULL, mt_error_thread_fn, NULL) == 0);
    MT_CHECK(pthread_join(th, NULL) == 0);
    MT_CHECK(strstr(tc_last_error(), "main-marker") != NULL);
    MT_CHECK(strstr(tc_last_error(), "child-payload") == NULL);
#endif

    return MT_MAIN_RETURN();
}
