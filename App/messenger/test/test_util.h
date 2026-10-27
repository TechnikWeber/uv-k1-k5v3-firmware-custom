/* Minimal test helpers for the host tests. */
#ifndef MESSENGER_TEST_UTIL_H
#define MESSENGER_TEST_UTIL_H

#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_STR(a, b) do { \
    g_checks++; \
    if (strcmp((a), (b)) != 0) { g_failed++; \
        printf("  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } \
} while (0)

#define CHECK_INT(a, b) do { \
    g_checks++; \
    long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { g_failed++; \
        printf("  FAIL %s:%d: %s = %ld, expected %ld\n", __FILE__, __LINE__, #a, _a, _b); } \
} while (0)

#define TEST_REPORT(name) \
    (printf("%-14s %d checks, %d failed\n", name, g_checks, g_failed), g_failed ? 1 : 0)

#endif
