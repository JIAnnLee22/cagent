/*
 * util/vector.h — generic dynamic array for plain-old-data elements.
 *
 * Elements are stored by value (memcpy semantics); the element type must
 * be trivially copyable. For elements owning heap memory (e.g. Message
 * with owned char* members), the OWNER of the Vector remains responsible
 * for freeing each element's internals before vector_free()/vector_pop().
 *
 * Ownership:
 *   - items is owned by Vector; freed by vector_free().
 *   - vector_push() copies *elem into the Vector (borrowed in).
 *   - vector_at() returns a borrowed pointer, invalidated by any
 *     subsequent push/clear (realloc may move the array).
 *
 * Mutators return AGENT_OK / AGENT_ERR_OOM; on OOM the Vector is
 * unchanged.
 */

#ifndef CAGENT_UTIL_VECTOR_H
#define CAGENT_UTIL_VECTOR_H

#include <stddef.h>

#include "util/error.h"

typedef struct {
    void* items; /* owned; NULL when cap == 0 */
    size_t len;
    size_t cap;
    size_t elem_size;
} Vector;

Vector vector_new(size_t elem_size);
void vector_free(Vector* v);

int vector_reserve(Vector* v, size_t extra);

/* Copy elem into the end. Returns pointer to the stored copy, or NULL on
 * OOM (the Vector is unchanged on failure). */
void* vector_push(Vector* v, const void* elem);

/* Borrowed pointer to element i; NULL when out of range. */
void* vector_at(Vector* v, size_t index);

void* vector_data(Vector* v);
size_t vector_len(const Vector* v);
size_t vector_elem_size(const Vector* v);
void vector_pop(Vector* v);   /* no-op when empty; caller owns the element */
void vector_clear(Vector* v); /* len = 0; capacity kept; caller owns elements */

#endif /* CAGENT_UTIL_VECTOR_H */
