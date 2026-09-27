#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>

inline int g_fails = 0;
inline int g_pass = 0;

inline void check(bool cond, const char* expr, const char* file, int line) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fails;
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    }
}

#define CHECK(expr) check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        auto _va = (a);                                                                \
        auto _vb = (b);                                                                \
        if (_va == _vb) {                                                              \
            ++g_pass;                                                                  \
        } else {                                                                       \
            ++g_fails;                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s  (%s vs %s)\n", __FILE__,       \
                         __LINE__, #a, #b, std::to_string(_va).c_str(),                \
                         std::to_string(_vb).c_str());                                 \
        }                                                                              \
    } while (0)

#define CHECK_STREQ(a, b)                                                              \
    do {                                                                               \
        std::string _va = (a);                                                         \
        std::string _vb = (b);                                                         \
        if (_va == _vb) {                                                              \
            ++g_pass;                                                                  \
        } else {                                                                       \
            ++g_fails;                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n  left : [%s]\n  right: [%s]\n", \
                         __FILE__, __LINE__, #a, #b, _va.c_str(), _vb.c_str());        \
        }                                                                              \
    } while (0)

inline int test_report(const char* suite) {
    std::printf("%s: %d passed, %d failed\n", suite, g_pass, g_fails);
    return g_fails ? 1 : 0;
}
