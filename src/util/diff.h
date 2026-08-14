#ifndef CAGENT_UTIL_DIFF_H
#define CAGENT_UTIL_DIFF_H

#include <stddef.h>

#include "util/string.h"

/* Build a bounded, human-readable line preview for an approval prompt.
 * This is advisory only: tools must re-read and validate inputs at execution. */
int diff_preview_build(const char* path, const char* old_text, size_t old_len, const char* new_text,
                       size_t new_len, String* out);

#endif /* CAGENT_UTIL_DIFF_H */
