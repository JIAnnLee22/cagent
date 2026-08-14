/*
 * tests/test_common.h — minimal test harness shared by all test files.
 *
 * Each test file defines test_* functions returning 0 on success, and a
 * main() that sums failures and exits non-zero if any failed.
 */

#ifndef CAGENT_TEST_COMMON_H
#define CAGENT_TEST_COMMON_H

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                        \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fputc('\n', stderr);                                                                   \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

#endif /* CAGENT_TEST_COMMON_H */
