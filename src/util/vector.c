/*
 * util/vector.c — generic dynamic array.
 *
 * Growth: exponential doubling from a floor, overflow-checked.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util/vector.h"

#define VECTOR_MIN_CAP 8

Vector vector_new(size_t elem_size) {
    Vector v = {0};
    v.elem_size = elem_size;
    return v;
}

void vector_free(Vector* v) {
    if (v == NULL) {
        return;
    }
    free(v->items);
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

static int vector_grow(Vector* v, size_t need) {
    if (v->cap >= need) {
        return AGENT_OK;
    }

    size_t new_cap = v->cap > 0 ? v->cap : VECTOR_MIN_CAP;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2) {
            return AGENT_ERR_OOM;
        }
        new_cap *= 2;
    }

    if (new_cap > SIZE_MAX / v->elem_size) {
        return AGENT_ERR_OOM; /* byte count would overflow */
    }

    void* p = realloc(v->items, new_cap * v->elem_size);
    if (p == NULL) {
        return AGENT_ERR_OOM;
    }
    v->items = p;
    v->cap = new_cap;
    return AGENT_OK;
}

int vector_reserve(Vector* v, size_t extra) {
    if (extra > SIZE_MAX - v->len) {
        return AGENT_ERR_OOM;
    }
    return vector_grow(v, v->len + extra);
}

void* vector_push(Vector* v, const void* elem) {
    if (elem == NULL) {
        return NULL;
    }

    int err = vector_grow(v, v->len + 1);
    if (err != AGENT_OK) {
        return NULL;
    }

    void* slot = (char*)v->items + v->len * v->elem_size;
    memcpy(slot, elem, v->elem_size);
    v->len++;
    return slot;
}

void* vector_at(Vector* v, size_t index) {
    if (v == NULL || index >= v->len) {
        return NULL;
    }
    return (char*)v->items + index * v->elem_size;
}

void* vector_data(Vector* v) {
    return v == NULL ? NULL : v->items;
}

size_t vector_len(const Vector* v) {
    return v == NULL ? 0 : v->len;
}

size_t vector_elem_size(const Vector* v) {
    return v == NULL ? 0 : v->elem_size;
}

void vector_pop(Vector* v) {
    if (v != NULL && v->len > 0) {
        v->len--;
    }
}

void vector_clear(Vector* v) {
    if (v != NULL) {
        v->len = 0;
    }
}
