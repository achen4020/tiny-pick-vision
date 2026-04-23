#ifndef TPV_TESTLIB_H
#define TPV_TESTLIB_H
#include <stdio.h>
#include <stdlib.h>

extern int tpv_test_pass;
extern int tpv_test_fail;

#define TEST(name) static void name(void)
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        tpv_test_fail++; return; \
    } \
} while (0)
#define CHECK_EQ_I(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "FAIL %s:%d  %s == %s (%lld vs %lld)\n", \
                __FILE__, __LINE__, #a, #b, _a, _b); \
        tpv_test_fail++; return; \
    } \
} while (0)
#define RUN(name) do { \
    int before = tpv_test_fail; \
    name(); \
    if (tpv_test_fail == before) { tpv_test_pass++; printf("  ok  %s\n", #name); } \
} while (0)
#define FINISH() do { \
    printf("\n%d passed, %d failed\n", tpv_test_pass, tpv_test_fail); \
    return tpv_test_fail ? 1 : 0; \
} while (0)

#endif
