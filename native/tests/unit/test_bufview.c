/* bufview：空输入/超大尺寸/NULL 语义（阶段 1 完成门槛项） */
#include "common/bufview.h"
#include "mini_test.h"

#include <stddef.h>
#include <string.h>

int main(void)
{
    uint8_t backing[16];
    for (int i = 0; i < 16; ++i) { backing[i] = (uint8_t)i; }

    tc_bufview v = tc_bufview_make(backing, sizeof backing);
    const void* p = NULL;

    /* 正常读取 */
    MT_CHECK(tc_bufview_read(&v, 0, 16, &p) && p == backing);
    MT_CHECK(tc_bufview_read(&v, 8, 8, &p) && p == backing + 8);
    MT_CHECK(tc_bufview_read(&v, 16, 0, &p));              /* 尾端零长读合法 */
    /* 越界（逐边界） */
    MT_CHECK(!tc_bufview_read(&v, 16, 1, &p));
    MT_CHECK(!tc_bufview_read(&v, 17, 0, &p));
    MT_CHECK(!tc_bufview_read(&v, SIZE_MAX, 0, &p));       /* 超大尺寸 */
    MT_CHECK(!tc_bufview_read(&v, 0, SIZE_MAX, &p));
    MT_CHECK(!tc_bufview_read(&v, (size_t)-8, 8, &p));

    /* slice / remaining */
    tc_bufview sub;
    MT_CHECK(tc_bufview_slice(&v, 4, 8, &sub) && sub.size == 8 && sub.data == backing + 4);
    MT_CHECK(!tc_bufview_slice(&v, 4, 13, &sub));
    MT_CHECK(!tc_bufview_slice(&v, SIZE_MAX, 1, &sub));
    tc_bufview rest;
    MT_CHECK(tc_bufview_remaining(&v, 8, &rest) && rest.size == 8 && rest.data == backing + 8);
    MT_CHECK(tc_bufview_remaining(&v, 16, &rest) && rest.size == 0);
    MT_CHECK(!tc_bufview_remaining(&v, 17, &rest));
    MT_CHECK(!tc_bufview_remaining(&v, SIZE_MAX, &rest));

    /* 空视图（size == 0 且 data 有效）：零长读 OK */
    tc_bufview empty = tc_bufview_make(backing, 0);
    MT_CHECK(tc_bufview_read(&empty, 0, 0, &p));
    MT_CHECK(!tc_bufview_read(&empty, 0, 1, &p));
    MT_CHECK(!tc_bufview_remaining(&empty, 1, &rest));

    /* NULL data：一律失败（含 size==0 / len==0，杜绝 NULL+0 歧义） */
    tc_bufview nullv = tc_bufview_make(NULL, 16);
    MT_CHECK(!tc_bufview_read(&nullv, 0, 0, &p));
    MT_CHECK(!tc_bufview_read(&nullv, 0, 1, &p));
    MT_CHECK(!tc_bufview_slice(&nullv, 0, 0, &sub));
    tc_bufview nullz = tc_bufview_make(NULL, 0);
    MT_CHECK(!tc_bufview_read(&nullz, 0, 0, &p));

    /* v == NULL 防御 */
    MT_CHECK(!tc_bufview_read(NULL, 0, 0, &p));

    /* 失败不写 out */
    tc_bufview poison = { NULL, 0 };
    MT_CHECK(!tc_bufview_slice(&v, 0, 100, &poison) && poison.data == NULL);

    return MT_MAIN_RETURN();
}
