/*
 * tests/test_hashmap.c — HashMap unit tests.
 */

#include <stdlib.h>
#include <string.h>

#include "test_common.h"
#include "util/hashmap.h"

static int test_put_get(void) {
    HashMap* m = hashmap_new();
    CHECK(m != NULL);
    CHECK(hashmap_len(m) == 0);

    int v1 = 1, v2 = 2, v3 = 3;

    CHECK(hashmap_put(m, "alpha", &v1, NULL) == AGENT_OK);
    CHECK(hashmap_put(m, "beta", &v2, NULL) == AGENT_OK);
    CHECK(hashmap_put(m, "gamma", &v3, NULL) == AGENT_OK);
    CHECK(hashmap_len(m) == 3);

    CHECK(hashmap_get(m, "alpha") == &v1);
    CHECK(hashmap_get(m, "beta") == &v2);
    CHECK(hashmap_get(m, "gamma") == &v3);
    CHECK(hashmap_get(m, "missing") == NULL);
    CHECK(hashmap_contains(m, "alpha"));
    CHECK(!hashmap_contains(m, "missing"));

    hashmap_free(m);
    return g_failures;
}

static int test_replace_and_remove(void) {
    HashMap* m = hashmap_new();
    CHECK(m != NULL);

    int old = 1, fresh = 2;
    CHECK(hashmap_put(m, "k", &old, NULL) == AGENT_OK);

    void* removed = NULL;
    CHECK(hashmap_put(m, "k", &fresh, &removed) == AGENT_OK);
    CHECK(removed == &old); /* replace reports the old value */
    CHECK(hashmap_get(m, "k") == &fresh);
    CHECK(hashmap_len(m) == 1);

    removed = NULL;
    CHECK(hashmap_remove(m, "k", &removed) == AGENT_OK);
    CHECK(removed == &fresh);
    CHECK(hashmap_len(m) == 0);
    CHECK(hashmap_get(m, "k") == NULL);

    /* removing an absent key fails */
    CHECK(hashmap_remove(m, "k", NULL) != AGENT_OK);

    hashmap_free(m);
    return g_failures;
}

static int test_key_is_copied(void) {
    HashMap* m = hashmap_new();
    CHECK(m != NULL);

    char key[16];
    int v = 7;
    strcpy(key, "original");
    CHECK(hashmap_put(m, key, &v, NULL) == AGENT_OK);

    /* mutating the caller's buffer must not affect the stored key */
    strcpy(key, "mutated!!!");
    CHECK(hashmap_get(m, "original") == &v);
    CHECK(hashmap_get(m, "mutated!!!") == NULL);

    hashmap_free(m);
    return g_failures;
}

static int test_growth_many_keys(void) {
    HashMap* m = hashmap_new();
    CHECK(m != NULL);

    char key[32];
    int values[5000];
    for (int i = 0; i < 5000; i++) {
        values[i] = i;
        snprintf(key, sizeof(key), "key-%05d", i);
        CHECK(hashmap_put(m, key, &values[i], NULL) == AGENT_OK);
    }
    CHECK(hashmap_len(m) == 5000);

    /* all keys retrievable after rehashes */
    for (int i = 0; i < 5000; i++) {
        snprintf(key, sizeof(key), "key-%05d", i);
        void* v = hashmap_get(m, key);
        CHECK(v != NULL && *(int*)v == i);
    }

    hashmap_free(m);
    return g_failures;
}

static int test_iteration(void) {
    HashMap* m = hashmap_new();
    CHECK(m != NULL);

    const char* names[] = {"one", "two", "three", "four"};
    int values[4] = {1, 2, 3, 4};
    for (int i = 0; i < 4; i++) {
        CHECK(hashmap_put(m, names[i], &values[i], NULL) == AGENT_OK);
    }

    int seen = 0;
    int sum = 0;
    HashMapIter it = hashmap_iter_first(m);
    while (it.key != NULL) {
        seen++;
        sum += *(int*)it.value;
        CHECK(strlen(it.key) > 0);
        it = hashmap_iter_next(&it);
    }
    CHECK(seen == 4);
    CHECK(sum == 10);

    /* iteration over an empty map yields nothing */
    HashMap* empty = hashmap_new();
    CHECK(empty != NULL);
    HashMapIter eit = hashmap_iter_first(empty);
    CHECK(eit.key == NULL);
    hashmap_free(empty);

    hashmap_free(m);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_put_get();
    g_failures += test_replace_and_remove();
    g_failures += test_key_is_copied();
    g_failures += test_growth_many_keys();
    g_failures += test_iteration();

    if (g_failures == 0) {
        printf("test_hashmap: all tests passed\n");
        return 0;
    }
    printf("test_hashmap: %d test(s) failed\n", g_failures);
    return 1;
}
