#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>

inline int g_fails = 0;
inline int g_pass = 0;

inline void check(bool cond, const char* expr, const char* file, int line) {
    if (cond) ++g_pass;
    else { ++g_fails; std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr); }
}
#define CHECK(expr) check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
inline int test_report(const char* suite) {
    std::printf("%s: %d passed, %d failed\n", suite, g_pass, g_fails);
    return g_fails ? 1 : 0;
}
