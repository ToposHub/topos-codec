/* mini_test —— 零依赖单测框架（阶段 1 决策：不引入第三方测试库）。
 * 每个测试文件是独立可执行（ctest 注册），返回 0 = 全部通过。
 * mt_rand_u64 为确定性 xorshift64*：测试必须可复现（spec §2.4 精神）。
 */
#ifndef MINI_TEST_H
#define MINI_TEST_H

#include <stdint.h>
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define MT_ATTR_UNUSED __attribute__((unused))
#else
#define MT_ATTR_UNUSED
#endif

static int mt_failures = 0;

static MT_ATTR_UNUSED void mt_report(const char* file, int line, const char* expr)
{
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    mt_failures++;
}

#define MT_CHECK(cond) \
    do { if (!(cond)) { mt_report(__FILE__, __LINE__, #cond); } } while (0)

#define MT_CHECK_EQ_U64(a, b)                                                    \
    do {                                                                        \
        uint64_t _x = (uint64_t)(a), _y = (uint64_t)(b);                        \
        if (_x != _y) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s (=%llu) != %s (=%llu)\n",           \
                    __FILE__, __LINE__, #a, (unsigned long long)_x,             \
                    #b, (unsigned long long)_y);                                \
            mt_failures++;                                                      \
        }                                                                       \
    } while (0)

#define MT_CHECK_EQ_I64(a, b) MT_CHECK_EQ_U64((int64_t)(a), (int64_t)(b))

#define MT_MAIN_RETURN() (mt_failures == 0 ? 0 : 1)

static MT_ATTR_UNUSED uint64_t mt_rand_u64(void)
{
    static uint64_t state = 0x9E3779B97F4A7C15ull;
    uint64_t x = state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

#endif /* MINI_TEST_H */
