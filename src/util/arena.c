/*
 * util/arena.c — arena allocator.
 *
 * Layout: singly linked list of blocks. head points at the most recently
 * allocated block (the one being filled). Each block carries its own
 * used/cap so reset() can walk the list and free everything.
 *
 * Alignment: block headers are sized to a multiple of max_align_t via a
 * union, so user data starts at a max_align_t-aligned offset; malloc
 * guarantees the block base is aligned as well.
 */

#include <stdalign.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "util/arena.h"

#define ARENA_DEFAULT_BLOCK 8192

typedef struct ArenaBlock ArenaBlock;

/*
 * Block header. The max_align_t member guarantees that sizeof(ArenaBlock)
 * is a multiple of max_align_t (and the struct's own alignment), so user
 * data starting right after the header is max_align_t-aligned when the
 * block base is (malloc guarantees that).
 */
struct ArenaBlock {
    ArenaBlock* next; /* previous block, or NULL for the first */
    size_t used;
    size_t cap;
    max_align_t align_pad;
};

struct Arena {
    ArenaBlock* head; /* current fill block */
    size_t block_size;
};

static size_t align_up(size_t n) {
    const size_t a = _Alignof(max_align_t);
    size_t r = (n + (a - 1)) & ~(a - 1);
    return r < n ? 0 : r; /* 0 signals overflow */
}

Arena* arena_new(size_t block_size) {
    Arena* a = malloc(sizeof(Arena));
    if (a == NULL) {
        return NULL;
    }
    a->head = NULL;
    a->block_size = block_size == 0 ? ARENA_DEFAULT_BLOCK : block_size;
    return a;
}

void arena_destroy(Arena* a) {
    if (a == NULL) {
        return;
    }
    arena_reset(a);
    free(a);
}

void* arena_alloc(Arena* a, size_t size) {
    if (a == NULL || size == 0) {
        return NULL;
    }

    size_t aligned = align_up(size);
    if (aligned == 0) {
        return NULL; /* size overflow */
    }

    ArenaBlock* b = a->head;
    if (b == NULL || aligned > b->cap - b->used) {
        size_t cap = aligned > a->block_size ? aligned : a->block_size;
        ArenaBlock* nb = malloc(sizeof(ArenaBlock) + cap);
        if (nb == NULL) {
            return NULL;
        }
        nb->next = a->head;
        nb->used = 0;
        nb->cap = cap;
        a->head = nb;
        b = nb;
    }

    void* p = (char*)b + sizeof(ArenaBlock) + b->used;
    b->used += aligned;
    return p;
}

void* arena_alloc_zero(Arena* a, size_t size) {
    void* p = arena_alloc(a, size);
    if (p != NULL) {
        memset(p, 0, size);
    }
    return p;
}

char* arena_strdup(Arena* a, const char* s) {
    if (s == NULL) {
        return NULL;
    }
    size_t len = strlen(s);
    char* p = arena_alloc(a, len + 1);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, s, len + 1);
    return p;
}

void arena_reset(Arena* a) {
    if (a == NULL) {
        return;
    }
    ArenaBlock* b = a->head;
    while (b != NULL) {
        ArenaBlock* next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
}
