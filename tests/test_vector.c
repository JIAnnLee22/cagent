/*
 * tests/test_vector.c — Vector unit tests.
 */

#include <stdlib.h>
#include <string.h>

#include "test_common.h"
#include "util/error.h"
#include "util/vector.h"

typedef struct {
    int a;
    double b;
    char tag[8];
} Item;

static int test_int_vector_growth(void) {
    Vector v = vector_new(sizeof(int));
    CHECK(v.elem_size == sizeof(int));
    CHECK(v.len == 0);

    for (int i = 0; i < 10000; i++) {
        int* slot = vector_push(&v, &i);
        CHECK(slot != NULL);
        CHECK(*slot == i);
    }
    CHECK(v.len == 10000);
    CHECK(v.cap >= 10000);

    /* verify contents after many reallocs */
    for (int i = 0; i < 10000; i++) {
        int* p = vector_at(&v, (size_t)i);
        CHECK(p != NULL && *p == i);
    }

    vector_free(&v);
    CHECK(v.items == NULL);
    CHECK(v.len == 0);
    return g_failures;
}

static int test_struct_vector(void) {
    Vector v = vector_new(sizeof(Item));
    CHECK(v.elem_size == sizeof(Item));

    for (int i = 0; i < 100; i++) {
        Item it = {i, i * 1.5, ""};
        snprintf(it.tag, sizeof(it.tag), "item-%d", i);
        Item* slot = vector_push(&v, &it);
        CHECK(slot != NULL);
    }

    CHECK(v.len == 100);
    Item* first = vector_at(&v, 0);
    CHECK(first != NULL && first->a == 0 && strcmp(first->tag, "item-0") == 0);
    Item* last = vector_at(&v, 99);
    CHECK(last != NULL && last->a == 99 && strcmp(last->tag, "item-99") == 0);

    /* out-of-range access */
    CHECK(vector_at(&v, 100) == NULL);
    CHECK(vector_at(&v, (size_t)-1) == NULL);

    /* data() points at the same array */
    CHECK(vector_data(&v) == v.items);

    vector_free(&v);
    return g_failures;
}

static int test_pop_and_clear(void) {
    Vector v = vector_new(sizeof(int));

    for (int i = 0; i < 10; i++) {
        CHECK(vector_push(&v, &i) != NULL);
    }
    vector_pop(&v);
    CHECK(v.len == 9);
    vector_pop(&v);
    vector_pop(&v);
    CHECK(v.len == 7);
    CHECK(*(int*)vector_at(&v, 6) == 6);

    vector_clear(&v);
    CHECK(v.len == 0);
    CHECK(v.cap >= 8); /* capacity kept */

    /* usable after clear */
    int x = 42;
    CHECK(vector_push(&v, &x) != NULL);
    CHECK(*(int*)vector_at(&v, 0) == 42);

    /* pop on empty is a no-op */
    vector_clear(&v);
    vector_pop(&v);
    CHECK(v.len == 0);

    vector_free(&v);
    return g_failures;
}

static int test_reserve(void) {
    Vector v = vector_new(sizeof(char));

    CHECK(vector_reserve(&v, 4096) == AGENT_OK);
    CHECK(v.cap >= 4096);
    CHECK(v.len == 0);

    /* no shrink on smaller reserve */
    size_t cap_before = v.cap;
    CHECK(vector_reserve(&v, 4) == AGENT_OK);
    CHECK(v.cap == cap_before);

    vector_free(&v);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_int_vector_growth();
    g_failures += test_struct_vector();
    g_failures += test_pop_and_clear();
    g_failures += test_reserve();

    if (g_failures == 0) {
        printf("test_vector: all tests passed\n");
        return 0;
    }
    printf("test_vector: %d test(s) failed\n", g_failures);
    return 1;
}
