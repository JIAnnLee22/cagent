/*
 * util/buffer.h — byte buffer for binary data.
 *
 * Used for HTTP response bodies, SSE fragment accumulation, and tool
 * output. Unlike String, data is NOT guaranteed NUL-terminated and may
 * contain embedded NUL bytes; use buffer.data + buffer.len.
 *
 * Ownership:
 *   - buffer.data is owned by Buffer; freed by buffer_free().
 *   - buffer_append() may realloc: do not keep borrowed pointers across
 *     mutating calls.
 *   - src is borrowed; never freed here.
 *
 * All mutators return AGENT_OK (0) or AGENT_ERR_OOM. On OOM the Buffer is
 * left unchanged (strong guarantee).
 */

#ifndef CAGENT_UTIL_BUFFER_H
#define CAGENT_UTIL_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#include "util/error.h"

typedef struct {
    uint8_t* data; /* owned; NULL when cap == 0 */
    size_t len;
    size_t cap;
} Buffer;

Buffer buffer_new(void);
void buffer_free(Buffer* b);

/* Ensure room for `extra` more bytes. May realloc. */
int buffer_reserve(Buffer* b, size_t extra);
int buffer_append(Buffer* b, const void* src, size_t n);

/* Drop contents; capacity is kept for reuse. */
void buffer_clear(Buffer* b);

#endif /* CAGENT_UTIL_BUFFER_H */
