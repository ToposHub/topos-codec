/* endian：已知向量 + 非对齐访问 + 随机往返 */
#include "common/endian.h"
#include "mini_test.h"

#include <string.h>

int main(void)
{
    /* 已知向量（ASCII "12345678" = 0x31..0x38） */
    static const uint8_t digits[8] = { '1','2','3','4','5','6','7','8' };
    MT_CHECK_EQ_U64(tc_load_be16(digits), 0x3132u);
    MT_CHECK_EQ_U64(tc_load_be32(digits), 0x31323334u);
    MT_CHECK_EQ_U64(tc_load_be64(digits), 0x3132333435363738ull);

    /* store 已知向量 */
    uint8_t buf[9];
    memset(buf, 0, sizeof buf);
    tc_store_be16(buf, 0xABCDu);
    MT_CHECK(buf[0] == 0xAB && buf[1] == 0xCD);
    tc_store_be32(buf, 0x01020304u);
    MT_CHECK(buf[0] == 1 && buf[1] == 2 && buf[2] == 3 && buf[3] == 4);
    tc_store_be64(buf, 0x0102030405060708ull);
    MT_CHECK(memcmp(buf, digits + 7 - 7, 8) != 0); /* 0x01.. != '1'.. */
    MT_CHECK(buf[0] == 1 && buf[7] == 8);

    /* 非对齐地址（buf+1，捕捉对齐假设；UBSan 下此测试具门禁效力） */
    uint8_t unaligned[17];
    for (int i = 0; i < 17; ++i) { unaligned[i] = (uint8_t)(i * 7 + 1); }
    MT_CHECK_EQ_U64(tc_load_be16(unaligned + 1), ((uint16_t)unaligned[1] << 8) | unaligned[2]);
    MT_CHECK_EQ_U64(tc_load_be32(unaligned + 3), ((uint32_t)unaligned[3] << 24) |
                                                ((uint32_t)unaligned[4] << 16) |
                                                ((uint32_t)unaligned[5] << 8) |
                                                (uint32_t)unaligned[6]);
    uint64_t v64 = tc_load_be64(unaligned + 9);
    tc_store_be64(unaligned + 1, v64);
    MT_CHECK_EQ_U64(tc_load_be64(unaligned + 1), v64);

    /* 随机 store→load 往返（1 万组，覆盖位拼接正确性） */
    for (int i = 0; i < 10000; ++i) {
        uint64_t x = mt_rand_u64();
        uint16_t x16 = (uint16_t)x;
        uint32_t x32 = (uint32_t)x;
        uint8_t t[8];
        tc_store_be16(t, x16);
        MT_CHECK_EQ_U64(tc_load_be16(t), x16);
        tc_store_be32(t, x32);
        MT_CHECK_EQ_U64(tc_load_be32(t), x32);
        tc_store_be64(t, x);
        MT_CHECK_EQ_U64(tc_load_be64(t), x);
    }

    return MT_MAIN_RETURN();
}
