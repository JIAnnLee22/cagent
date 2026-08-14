/*
 * util/string.h — dynamic NUL-terminated string.
 *
 * Ownership contract:
 *   - string.data is owned by String; freed by string_free()/string_take().
 *   - data is always NUL-terminated while the String is alive (may be NULL
 *     only when cap == 0, which behaves as the empty string).
 *   - All append/printf calls may realloc data: borrowed pointers into
 *     data must not be kept across calls that mutate the String.
 *   - string_take() transfers ownership of data to the caller, who must
 *     free() it; the String is reset to empty afterwards.
 *   - src parameters are borrowed; never freed here.
 *
 * All mutators return AGENT_OK (0) or AGENT_ERR_OOM. On OOM the String is
 * left unchanged (strong guarantee).
 */

#ifndef CAGENT_UTIL_STRING_H
#define CAGENT_UTIL_STRING_H

#include <stddef.h>

#include "util/error.h"

typedef struct {
    char* data; /* owned; NUL-terminated while alive, NULL when cap == 0 */
    size_t len; /* length excluding the NUL terminator */
    size_t cap; /* allocated capacity excluding the NUL terminator */
} String;

String string_new(void);
void string_free(String* s);

/* Ensure room for `extra` more bytes. May realloc. */
int string_reserve(String* s, size_t extra);
int string_append(String* s, const char* src);
int string_append_n(String* s, const char* src, size_t n);
int string_append_char(String* s, char c);
int string_printf(String* s, const char* fmt, ...);

/* Transfer ownership of data to the caller (caller must free()). */
char* string_take(String* s);

/* Drop contents; capacity is kept for reuse. */
void string_clear(String* s);

#endif /* CAGENT_UTIL_STRING_H */
