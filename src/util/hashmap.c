/*
 * util/hashmap.c — string-keyed hash map.
 *
 * Chained buckets, FNV-1a 64-bit hashing, growth at load factor 0.75.
 * Keys are strdup'ed; values are caller-owned.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util/hashmap.h"

#define HASHMAP_MIN_BUCKETS 16
#define HASHMAP_MAX_LOAD 75 /* percent */

typedef struct HashMapEntry {
    char* key;   /* owned */
    void* value; /* borrowed from caller */
    struct HashMapEntry* next;
} HashMapEntry;

struct HashMap {
    HashMapEntry** buckets; /* owned */
    size_t n_buckets;
    size_t n_entries;
};

static uint64_t hash_fnv1a(const char* s) {
    uint64_t h = UINT64_C(14695981039346656037);
    while (*s != '\0') {
        h ^= (uint8_t)*s++;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

HashMap* hashmap_new(void) {
    HashMap* m = calloc(1, sizeof(HashMap));
    if (m == NULL) {
        return NULL;
    }
    m->buckets = (HashMapEntry**)calloc(HASHMAP_MIN_BUCKETS, sizeof(HashMapEntry*));
    if (m->buckets == NULL) {
        free(m);
        return NULL;
    }
    m->n_buckets = HASHMAP_MIN_BUCKETS;
    return m;
}

void hashmap_free(HashMap* m) {
    if (m == NULL) {
        return;
    }
    for (size_t i = 0; i < m->n_buckets; i++) {
        HashMapEntry* e = m->buckets[i];
        while (e != NULL) {
            HashMapEntry* next = e->next;
            free(e->key);
            free(e);
            e = next;
        }
    }
    free((void*)m->buckets);
    free(m);
}

static int hashmap_rehash(HashMap* m) {
    size_t new_n = m->n_buckets * 2;
    HashMapEntry** new_buckets = (HashMapEntry**)calloc(new_n, sizeof(HashMapEntry*));
    if (new_buckets == NULL) {
        return AGENT_ERR_OOM;
    }

    for (size_t i = 0; i < m->n_buckets; i++) {
        HashMapEntry* e = m->buckets[i];
        while (e != NULL) {
            HashMapEntry* next = e->next;
            size_t idx = (size_t)(hash_fnv1a(e->key) % new_n);
            e->next = new_buckets[idx];
            new_buckets[idx] = e;
            e = next;
        }
    }

    free((void*)m->buckets);
    m->buckets = new_buckets;
    m->n_buckets = new_n;
    return AGENT_OK;
}

int hashmap_put(HashMap* m, const char* key, void* value, void** old_value_out) {
    if (m == NULL || key == NULL) {
        return AGENT_ERR_OOM;
    }
    if (old_value_out != NULL) {
        *old_value_out = NULL;
    }

    size_t idx = (size_t)(hash_fnv1a(key) % m->n_buckets);
    for (HashMapEntry* e = m->buckets[idx]; e != NULL; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            if (old_value_out != NULL) {
                *old_value_out = e->value;
            }
            e->value = value;
            return AGENT_OK;
        }
    }

    /* grow before inserting to keep the load factor bounded */
    if ((m->n_entries + 1) * 100 / m->n_buckets > HASHMAP_MAX_LOAD) {
        int err = hashmap_rehash(m);
        if (err != AGENT_OK) {
            return err;
        }
        idx = (size_t)(hash_fnv1a(key) % m->n_buckets);
    }

    HashMapEntry* e = malloc(sizeof(HashMapEntry));
    if (e == NULL) {
        return AGENT_ERR_OOM;
    }
    e->key = strdup(key);
    if (e->key == NULL) {
        free(e);
        return AGENT_ERR_OOM;
    }
    e->value = value;
    e->next = m->buckets[idx];
    m->buckets[idx] = e;
    m->n_entries++;
    return AGENT_OK;
}

void* hashmap_get(const HashMap* m, const char* key) {
    if (m == NULL || key == NULL) {
        return NULL;
    }
    size_t idx = (size_t)(hash_fnv1a(key) % m->n_buckets);
    for (HashMapEntry* e = m->buckets[idx]; e != NULL; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            return e->value;
        }
    }
    return NULL;
}

bool hashmap_contains(const HashMap* m, const char* key) {
    return hashmap_get(m, key) != NULL;
}

int hashmap_remove(HashMap* m, const char* key, void** removed_out) {
    if (m == NULL || key == NULL) {
        return AGENT_ERR_IO;
    }
    if (removed_out != NULL) {
        *removed_out = NULL;
    }

    size_t idx = (size_t)(hash_fnv1a(key) % m->n_buckets);
    HashMapEntry** link = &m->buckets[idx];
    while (*link != NULL) {
        HashMapEntry* e = *link;
        if (strcmp(e->key, key) == 0) {
            *link = e->next;
            if (removed_out != NULL) {
                *removed_out = e->value;
            }
            free(e->key);
            free(e);
            m->n_entries--;
            return AGENT_OK;
        }
        link = &e->next;
    }
    return AGENT_ERR_IO;
}

size_t hashmap_len(const HashMap* m) {
    return m == NULL ? 0 : m->n_entries;
}

HashMapIter hashmap_iter_first(const HashMap* m) {
    HashMapIter it = {0};
    it.map = m;
    if (m == NULL) {
        return it;
    }
    for (size_t i = 0; i < m->n_buckets; i++) {
        if (m->buckets[i] != NULL) {
            it.bucket_index = i;
            it.entry = m->buckets[i];
            it.key = it.entry->key;
            it.value = it.entry->value;
            return it;
        }
    }
    return it;
}

HashMapIter hashmap_iter_next(const HashMapIter* it) {
    HashMapIter next = {0};
    if (it == NULL || it->map == NULL) {
        return next;
    }
    next.map = it->map;
    next.entry = it->entry == NULL ? NULL : it->entry->next;
    if (next.entry != NULL) {
        next.bucket_index = it->bucket_index;
        next.key = next.entry->key;
        next.value = next.entry->value;
        return next;
    }
    /* advance to the next non-empty bucket */
    for (size_t i = it->bucket_index + 1; i < it->map->n_buckets; i++) {
        if (it->map->buckets[i] != NULL) {
            next.entry = it->map->buckets[i];
            next.bucket_index = i;
            next.key = next.entry->key;
            next.value = next.entry->value;
            return next;
        }
    }
    return next; /* exhausted: key stays NULL */
}
