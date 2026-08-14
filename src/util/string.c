/*
 * util/string.c — dynamic NUL-terminated string.
 *
 * Growth policy: exponential doubling from a small floor, so appends are
 * amortized O(1). All growth is overflow-checked.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/error.h"
#include "util/string.h"

#define STRING_MIN_CAP 32

String string_new(void) {
    String s = {0};
    return s;
}

void string_free(String* s) {
    if (s == NULL) {
        return;
    }
    free(s->data);
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

static int string_grow(String* s, size_t need) {
    if (s->cap >= need) {
        return AGENT_OK;
    }

    size_t new_cap = s->cap > 0 ? s->cap : STRING_MIN_CAP;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2) {
            return AGENT_ERR_OOM; /* would overflow */
        }
        new_cap *= 2;
    }

    char* p = realloc(s->data, new_cap + 1);
    if (p == NULL) {
        return AGENT_ERR_OOM;
    }
    s->data = p;
    s->cap = new_cap;
    return AGENT_OK;
}

int string_reserve(String* s, size_t extra) {
    if (extra > SIZE_MAX - s->len) {
        return AGENT_ERR_OOM; /* len + extra would overflow */
    }
    return string_grow(s, s->len + extra);
}

int string_append_n(String* s, const char* src, size_t n) {
    if (n == 0) {
        return AGENT_OK;
    }
    if (src == NULL) {
        return AGENT_ERR_OOM; /* caller contract violation; treat as fatal */
    }
    if (n > SIZE_MAX - s->len - 1) {
        return AGENT_ERR_OOM;
    }

    int err = string_grow(s, s->len + n);
    if (err != AGENT_OK) {
        return err;
    }

    memcpy(s->data + s->len, src, n);
    s->len += n;
    s->data[s->len] = '\0';
    return AGENT_OK;
}

int string_append(String* s, const char* src) {
    return string_append_n(s, src, strlen(src));
}

int string_append_char(String* s, char c) {
    return string_append_n(s, &c, 1);
}

int string_printf(String* s, const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (needed < 0) {
        va_end(ap2);
        return AGENT_ERR_IO;
    }

    int err = string_reserve(s, (size_t)needed);
    if (err != AGENT_OK) {
        va_end(ap2);
        return err;
    }

    vsnprintf(s->data + s->len, s->cap - s->len + 1, fmt, ap2);
    va_end(ap2);

    s->len += (size_t)needed;
    return AGENT_OK;
}

char* string_take(String* s) {
    char* p = s->data;
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
    return p;
}

void string_clear(String* s) {
    s->len = 0;
    if (s->data != NULL) {
        s->data[0] = '\0';
    }
}
