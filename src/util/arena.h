/*
 * util/arena.h — arena allocator for short-lived data.
 *
 * Lifetime contract (DESIGN.md §3.3):
 *   - One LLM request's temporary data (JSON, SSE parse buffers, tool-call
 *     arguments) lives in a per-request Arena; arena_reset() frees it all.
 *   - Long-lived objects (Agent, Session, Message history, ToolRegistry)
 *     must NEVER be allocated from a request arena.
 *   - Arena does not support freeing individual allocations; either the
 *     whole arena is reset/destroyed or nothing is.
 *
 * Ownership:
 *   - arena_alloc/alloc_zero/strdup return memory owned by the Arena.
 *     Pointers remain valid until arena_reset()/arena_destroy().
 *   - arena_destroy() frees everything; the Arena pointer itself is freed.
 *   - All returned pointers are aligned to max_align_t.
 *
 * Thread-safety: NOT thread-safe. One Arena per thread or per request.
 */

#ifndef CAGENT_UTIL_ARENA_H
#define CAGENT_UTIL_ARENA_H

#include <stddef.h>

typedef struct Arena Arena;

/* block_size == 0 selects an internal default (8 KiB). */
Arena* arena_new(size_t block_size);

/* Free all blocks and the Arena itself. */
void arena_destroy(Arena* a);

/* NULL on allocation failure. */
void* arena_alloc(Arena* a, size_t size);
void* arena_alloc_zero(Arena* a, size_t size);
char* arena_strdup(Arena* a, const char* s);

/* Free all blocks; the Arena stays usable. */
void arena_reset(Arena* a);

#endif /* CAGENT_UTIL_ARENA_H */
