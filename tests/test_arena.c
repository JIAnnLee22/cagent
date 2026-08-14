/*
 * tests/test_arena.c — Arena unit tests.
 *
 * Runs under ASan/UBSan in CI to catch use-after-free, double free and
 * alignment issues.
 */

#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"
#include "util/arena.h"

#define NPTR 128

static int test_alloc_alignment_and_disjoint(void) {
    Arena* a = arena_new(0);
    CHECK(a != NULL);

    void* ptrs[NPTR];
    for (int i = 0; i < NPTR; i++) {
        ptrs[i] = arena_alloc(a, (size_t)i + 1); /* odd sizes */
        CHECK(ptrs[i] != NULL);
        CHECK((uintptr_t)ptrs[i] % _Alignof(max_align_t) == 0);
    }

    /* all pointers must be pairwise disjoint */
    for (int i = 0; i < NPTR; i++) {
        for (int j = i + 1; j < NPTR; j++) {
            CHECK(ptrs[i] != ptrs[j]);
        }
    }

    arena_destroy(a);
    return g_failures;
}

static int test_alloc_zero_and_strdup(void) {
    Arena* a = arena_new(0);
    CHECK(a != NULL);

    uint8_t* z = arena_alloc_zero(a, 64);
    CHECK(z != NULL);
    for (int i = 0; i < 64; i++) {
        CHECK(z[i] == 0);
    }

    char* dup = arena_strdup(a, "arena string");
    CHECK(dup != NULL);
    CHECK(strcmp(dup, "arena string") == 0);

    char* dup2 = arena_strdup(a, "another");
    CHECK(dup2 != NULL);
    CHECK(strcmp(dup2, "another") == 0);
    /* strdup results must not overlap the first string */
    CHECK(dup != dup2);

    arena_destroy(a);
    return g_failures;
}

static int test_large_allocation(void) {
    Arena* a = arena_new(0);
    CHECK(a != NULL);

    /* 1 MiB, far beyond the default block size */
    size_t big = 1024 * 1024;
    char* p = arena_alloc(a, big);
    CHECK(p != NULL);
    memset(p, 0xAB, big);

    /* arena still usable after the big block */
    char* small = arena_alloc(a, 16);
    CHECK(small != NULL);
    CHECK((uintptr_t)small % _Alignof(max_align_t) == 0);

    arena_destroy(a);
    return g_failures;
}

static int test_tiny_blocks_force_multiple_blocks(void) {
    Arena* a = arena_new(16); /* tiny blocks */
    CHECK(a != NULL);

    void* ptrs[64];
    for (int i = 0; i < 64; i++) {
        ptrs[i] = arena_alloc(a, 8);
        CHECK(ptrs[i] != NULL);
        /* no overlap: each block of 16 bytes fits at most one 8-byte item
         * after the 32-byte header, so consecutive allocations must differ */
        if (i > 0) {
            CHECK(ptrs[i] != ptrs[i - 1]);
        }
    }

    arena_destroy(a);
    return g_failures;
}

static int test_reset_reuse(void) {
    Arena* a = arena_new(0);
    CHECK(a != NULL);

    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 500; i++) {
            char* p = arena_alloc(a, 32);
            CHECK(p != NULL);
            memset(p, (int)round, 32);
        }
        arena_reset(a);
    }

    /* after reset, arena must be usable again */
    char* q = arena_strdup(a, "post-reset");
    CHECK(q != NULL);
    CHECK(strcmp(q, "post-reset") == 0);

    arena_destroy(a);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_alloc_alignment_and_disjoint();
    g_failures += test_alloc_zero_and_strdup();
    g_failures += test_large_allocation();
    g_failures += test_tiny_blocks_force_multiple_blocks();
    g_failures += test_reset_reuse();

    if (g_failures == 0) {
        printf("test_arena: all tests passed\n");
        return 0;
    }
    printf("test_arena: %d test(s) failed\n", g_failures);
    return 1;
}
