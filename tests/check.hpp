#pragma once
#include <cstdio>

// 极简断言宏，供各测试独立使用
static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define TEST_SUMMARY(name)                                                 \
    do {                                                                   \
        if (g_failures == 0)                                               \
            std::printf("%s: ALL PASS\n", name);                           \
        else                                                               \
            std::printf("%s: %d FAILED\n", name, g_failures);              \
        return g_failures ? 1 : 0;                                         \
    } while (0)