#ifndef BDTRACE_TEST_HELPERS_H
#define BDTRACE_TEST_HELPERS_H

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_test_failures = 0;
static int g_test_count = 0;

#define RUN_TEST(fn) do { \
    ++g_test_count; \
    int _before = g_test_failures; \
    std::printf("  TEST %s ... ", #fn); \
    fn(); \
    if (g_test_failures == _before) std::printf("OK\n"); \
} while(0)

#define TEST_REPORT() do { \
    std::printf("\n%d/%d tests passed\n", g_test_count - g_test_failures, g_test_count); \
    return g_test_failures > 0 ? 1 : 0; \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        std::printf("FAIL\n    %s:%d: ASSERT_TRUE(%s)\n", __FILE__, __LINE__, #expr); \
        ++g_test_failures; return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::printf("FAIL\n    %s:%d: ASSERT_EQ\n", __FILE__, __LINE__); \
        ++g_test_failures; return; \
    } \
} while(0)

#define ASSERT_GT(a, b) do { \
    if (!((a) > (b))) { \
        std::printf("FAIL\n    %s:%d: ASSERT_GT\n", __FILE__, __LINE__); \
        ++g_test_failures; return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    if (std::string(a) != std::string(b)) { \
        std::printf("FAIL\n    %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        ++g_test_failures; return; \
    } \
} while(0)

#endif // BDTRACE_TEST_HELPERS_H
