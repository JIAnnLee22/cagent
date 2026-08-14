#include <stdbool.h>
#include <stddef.h>

#include "util/diff.h"

#define DIFF_PREVIEW_MAX_BYTES (16 * 1024)
#define DIFF_PREVIEW_MAX_LINES 120

static size_t line_count(const char* text, size_t len) {
    if (text == NULL || len == 0)
        return 0;
    size_t lines = 1;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n' && i + 1 < len)
            lines++;
    }
    return lines;
}

static int append_lines(String* out, char prefix, const char* text, size_t len,
                        size_t* emitted_lines, bool* truncated) {
    if (text == NULL || len == 0)
        return AGENT_OK;
    size_t pos = 0;
    while (pos < len) {
        if (*emitted_lines >= DIFF_PREVIEW_MAX_LINES || out->len >= DIFF_PREVIEW_MAX_BYTES) {
            *truncated = true;
            return AGENT_OK;
        }
        size_t end = pos;
        while (end < len && text[end] != '\n')
            end++;
        size_t available = DIFF_PREVIEW_MAX_BYTES - out->len;
        if (available < 4) {
            *truncated = true;
            return AGENT_OK;
        }
        if (string_append_char(out, prefix) != AGENT_OK)
            return AGENT_ERR_OOM;
        size_t line_len = end - pos;
        if (line_len + 1 > available - 1) {
            line_len = available > 5 ? available - 5 : 0;
            *truncated = true;
        }
        if (line_len > 0 && string_append_n(out, text + pos, line_len) != AGENT_OK) {
            return AGENT_ERR_OOM;
        }
        if (*truncated && string_append(out, "...") != AGENT_OK)
            return AGENT_ERR_OOM;
        if (string_append_char(out, '\n') != AGENT_OK)
            return AGENT_ERR_OOM;
        (*emitted_lines)++;
        if (*truncated)
            return AGENT_OK;
        pos = end < len ? end + 1 : end;
    }
    return AGENT_OK;
}

int diff_preview_build(const char* path, const char* old_text, size_t old_len, const char* new_text,
                       size_t new_len, String* out) {
    if (out == NULL)
        return AGENT_ERR_TOOL;
    const char* label = path != NULL ? path : "file";
    if (string_printf(out, "--- %s (%zu bytes, %zu lines)\n", label, old_len,
                      line_count(old_text, old_len)) != AGENT_OK ||
        string_printf(out, "+++ %s (%zu bytes, %zu lines)\n", label, new_len,
                      line_count(new_text, new_len)) != AGENT_OK ||
        string_append(out, "@@ proposed change @@\n") != AGENT_OK) {
        return AGENT_ERR_OOM;
    }
    size_t emitted = 0;
    bool truncated = false;
    int rc = append_lines(out, '-', old_text, old_len, &emitted, &truncated);
    if (rc == AGENT_OK && !truncated) {
        rc = append_lines(out, '+', new_text, new_len, &emitted, &truncated);
    }
    if (rc != AGENT_OK)
        return rc;
    if (truncated && string_append(out, "...[preview truncated]\n") != AGENT_OK) {
        return AGENT_ERR_OOM;
    }
    return AGENT_OK;
}
