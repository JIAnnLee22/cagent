/*
 * tests/test_string.c — String unit tests.
 */

#include <stdlib.h>
#include <string.h>

#include "test_common.h"
#include "util/error.h"
#include "util/string.h"

static int test_basic_append(void) {
    String s = string_new();
    CHECK(s.len == 0);
    CHECK(s.data == NULL || s.data[0] == '\0');

    CHECK(string_append(&s, "hello") == AGENT_OK);
    CHECK(s.len == 5);
    CHECK(strcmp(s.data, "hello") == 0);
    CHECK(s.data[5] == '\0'); /* NUL terminated */

    string_free(&s);
    CHECK(s.data == NULL);
    CHECK(s.len == 0);
    CHECK(s.cap == 0);
    return g_failures;
}

static int test_append_n_and_char(void) {
    String s = string_new();

    CHECK(string_append_n(&s, "prefix-suffix", 6) == AGENT_OK);
    CHECK(strcmp(s.data, "prefix") == 0);
    CHECK(string_append_char(&s, '!') == AGENT_OK);
    CHECK(strcmp(s.data, "prefix!") == 0);

    /* n == 0 and empty src are no-ops */
    CHECK(string_append_n(&s, "ignored", 0) == AGENT_OK);
    CHECK(s.len == 7);
    CHECK(string_append(&s, "") == AGENT_OK);
    CHECK(s.len == 7);

    string_free(&s);
    return g_failures;
}

static int test_printf(void) {
    String s = string_new();

    CHECK(string_printf(&s, "%s=%d", "count", 42) == AGENT_OK);
    CHECK(strcmp(s.data, "count=42") == 0);

    /* appending to non-empty string */
    CHECK(string_printf(&s, " %s", "more") == AGENT_OK);
    CHECK(strcmp(s.data, "count=42 more") == 0);

    string_free(&s);
    return g_failures;
}

static int test_growth_and_large_content(void) {
    String s = string_new();
    size_t total = 0;
    const char* piece = "0123456789abcdef";

    /* 10k appends of 16 bytes = 160 KiB, forces many reallocs */
    for (int i = 0; i < 10000; i++) {
        CHECK(string_append(&s, piece) == AGENT_OK);
        total += strlen(piece);
    }
    CHECK(s.len == total);
    CHECK(s.cap >= s.len);
    CHECK(strncmp(s.data, piece, 16) == 0);
    CHECK(s.data[s.len] == '\0');
    /* last 16 bytes must be the piece again */
    CHECK(memcmp(s.data + s.len - 16, piece, 16) == 0);

    string_free(&s);
    return g_failures;
}

static int test_reserve(void) {
    String s = string_new();

    CHECK(string_reserve(&s, 4096) == AGENT_OK);
    CHECK(s.cap >= 4096);
    CHECK(s.len == 0);

    /* reserve small amounts after big one must not shrink */
    size_t cap_before = s.cap;
    CHECK(string_reserve(&s, 8) == AGENT_OK);
    CHECK(s.cap == cap_before);

    string_free(&s);
    return g_failures;
}

static int test_clear_reuse(void) {
    String s = string_new();

    CHECK(string_append(&s, "to be cleared") == AGENT_OK);
    string_clear(&s);
    CHECK(s.len == 0);
    CHECK(s.data != NULL && s.data[0] == '\0');

    /* string stays usable after clear */
    CHECK(string_append(&s, "reused") == AGENT_OK);
    CHECK(strcmp(s.data, "reused") == 0);

    string_free(&s);
    return g_failures;
}

static int test_take(void) {
    String s = string_new();
    CHECK(string_append(&s, "owned by caller") == AGENT_OK);

    char* p = string_take(&s);
    CHECK(p != NULL);
    CHECK(strcmp(p, "owned by caller") == 0);
    CHECK(s.data == NULL);
    CHECK(s.len == 0);
    CHECK(s.cap == 0);

    free(p);

    /* take on empty string yields NULL, which is safe to free() */
    char* q = string_take(&s);
    CHECK(q == NULL);
    free(q);

    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_basic_append();
    g_failures += test_append_n_and_char();
    g_failures += test_printf();
    g_failures += test_growth_and_large_content();
    g_failures += test_reserve();
    g_failures += test_clear_reuse();
    g_failures += test_take();

    if (g_failures == 0) {
        printf("test_string: all tests passed\n");
        return 0;
    }
    printf("test_string: %d test(s) failed\n", g_failures);
    return 1;
}
