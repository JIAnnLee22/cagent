/*
 * tests/test_buffer.c — Buffer unit tests (binary-safe byte buffer).
 */

#include <string.h>

#include "test_common.h"
#include "util/buffer.h"
#include "util/error.h"

static int test_binary_content(void) {
    Buffer b = buffer_new();
    CHECK(b.len == 0);

    /* content with embedded NUL bytes */
    const uint8_t bin[] = {0x00, 0x01, 0x02, 0xFF, 0x00, 0x7F};
    CHECK(buffer_append(&b, bin, sizeof(bin)) == AGENT_OK);
    CHECK(b.len == sizeof(bin));
    CHECK(memcmp(b.data, bin, sizeof(bin)) == 0);

    /* append more after the NUL bytes */
    const char* tail = "tail";
    CHECK(buffer_append(&b, tail, strlen(tail)) == AGENT_OK);
    CHECK(b.len == sizeof(bin) + strlen(tail));
    CHECK(memcmp(b.data + sizeof(bin), tail, strlen(tail)) == 0);

    buffer_free(&b);
    CHECK(b.data == NULL);
    CHECK(b.len == 0);
    CHECK(b.cap == 0);
    return g_failures;
}

static int test_growth_large(void) {
    Buffer b = buffer_new();
    size_t total = 0;
    uint8_t chunk[4096];
    memset(chunk, 0x5A, sizeof(chunk));

    for (int i = 0; i < 256; i++) { /* 1 MiB total */
        CHECK(buffer_append(&b, chunk, sizeof(chunk)) == AGENT_OK);
        total += sizeof(chunk);
    }
    CHECK(b.len == total);
    CHECK(b.cap >= b.len);
    CHECK(memcmp(b.data + total - sizeof(chunk), chunk, sizeof(chunk)) == 0);

    buffer_free(&b);
    return g_failures;
}

static int test_reserve_and_clear(void) {
    Buffer b = buffer_new();

    CHECK(buffer_reserve(&b, 8192) == AGENT_OK);
    CHECK(b.cap >= 8192);
    CHECK(b.len == 0);

    CHECK(buffer_append(&b, "abc", 3) == AGENT_OK);
    buffer_clear(&b);
    CHECK(b.len == 0);
    CHECK(b.cap >= 8192); /* capacity kept */

    /* still usable after clear */
    CHECK(buffer_append(&b, "xyz", 3) == AGENT_OK);
    CHECK(memcmp(b.data, "xyz", 3) == 0);

    buffer_free(&b);
    return g_failures;
}

static int test_empty_append(void) {
    Buffer b = buffer_new();

    CHECK(buffer_append(&b, NULL, 0) == AGENT_OK);
    CHECK(b.len == 0);

    buffer_free(&b);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_binary_content();
    g_failures += test_growth_large();
    g_failures += test_reserve_and_clear();
    g_failures += test_empty_append();

    if (g_failures == 0) {
        printf("test_buffer: all tests passed\n");
        return 0;
    }
    printf("test_buffer: %d test(s) failed\n", g_failures);
    return 1;
}
