/*
 * util/buffer.c — byte buffer. Same growth policy as String (exponential
 * doubling, overflow-checked), but binary-safe.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util/buffer.h"

#define BUFFER_MIN_CAP 32

Buffer buffer_new(void) {
    Buffer b = {0};
    return b;
}

void buffer_free(Buffer* b) {
    if (b == NULL) {
        return;
    }
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int buffer_grow(Buffer* b, size_t need) {
    if (b->cap >= need) {
        return AGENT_OK;
    }

    size_t new_cap = b->cap > 0 ? b->cap : BUFFER_MIN_CAP;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2) {
            return AGENT_ERR_OOM;
        }
        new_cap *= 2;
    }

    uint8_t* p = realloc(b->data, new_cap);
    if (p == NULL) {
        return AGENT_ERR_OOM;
    }
    b->data = p;
    b->cap = new_cap;
    return AGENT_OK;
}

int buffer_reserve(Buffer* b, size_t extra) {
    if (extra > SIZE_MAX - b->len) {
        return AGENT_ERR_OOM;
    }
    return buffer_grow(b, b->len + extra);
}

int buffer_append(Buffer* b, const void* src, size_t n) {
    if (n == 0) {
        return AGENT_OK;
    }
    if (src == NULL) {
        return AGENT_ERR_OOM;
    }
    if (n > SIZE_MAX - b->len) {
        return AGENT_ERR_OOM;
    }

    int err = buffer_grow(b, b->len + n);
    if (err != AGENT_OK) {
        return err;
    }

    memcpy(b->data + b->len, src, n);
    b->len += n;
    return AGENT_OK;
}

void buffer_clear(Buffer* b) {
    b->len = 0;
}
