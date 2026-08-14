/*
 * util/hashmap.h — string-keyed map (char* key -> void* value).
 *
 * Ownership:
 *   - Keys are copied on hashmap_put(); the caller keeps ownership of the
 *     key argument.
 *   - Values are owned by the caller, never freed by the HashMap.
 *     hashmap_put() on an existing key replaces the value and reports the
 *     old one via *old_value_out (may be NULL to ignore).
 *   - hashmap_free() frees keys and buckets only.
 *   - Iteration pointers (key/value) are borrowed and only valid until the
 *     next mutation of the map.
 *
 * Thread-safety: NOT thread-safe.
 */

#ifndef CAGENT_UTIL_HASHMAP_H
#define CAGENT_UTIL_HASHMAP_H

#include <stdbool.h>
#include <stddef.h>

#include "util/error.h"

typedef struct HashMap HashMap;

HashMap* hashmap_new(void);
void hashmap_free(HashMap* m);

/* Insert or replace. On replace, *old_value_out receives the previous
 * value (NULL when the key was absent or old_value_out is NULL).
 * Returns AGENT_OK / AGENT_ERR_OOM. */
int hashmap_put(HashMap* m, const char* key, void* value, void** old_value_out);

/* Borrowed value for key, or NULL when absent. */
void* hashmap_get(const HashMap* m, const char* key);
bool hashmap_contains(const HashMap* m, const char* key);

/* Remove key; *removed_out receives the removed value (may be NULL).
 * Returns AGENT_OK when removed, AGENT_ERR_IO when the key was absent. */
int hashmap_remove(HashMap* m, const char* key, void** removed_out);

size_t hashmap_len(const HashMap* m);

/* Iteration. Typical use:
 *   HashMapIter it = hashmap_iter_first(m);
 *   while (it.key != NULL) { ...; it = hashmap_iter_next(&it); }
 */
typedef struct {
    const HashMap* map; /* borrowed */
    const char* key;    /* borrowed; NULL when exhausted */
    void* value;        /* borrowed */
    /* internal */
    size_t bucket_index;
    const struct HashMapEntry* entry;
} HashMapIter;

HashMapIter hashmap_iter_first(const HashMap* m);
HashMapIter hashmap_iter_next(const HashMapIter* it);

#endif /* CAGENT_UTIL_HASHMAP_H */
