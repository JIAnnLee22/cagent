/*
 * tui/format.c — render tool calls as human-readable one-liners.
 *
 * Design (DESIGN.md §36): the formatter is the only place that knows how
 * each tool's arguments map to a friendly summary. It pulls a small,
 * tool-specific field set out of the arguments JSON and falls back to a
 * sanitized plain-text excerpt when parsing fails or the tool is unknown.
 *
 * The raw JSON is never shown to the user (it lives in the Session and is
 * shipped back to the model verbatim). Untrusted input (file paths, shell
 * commands, model-generated strings) is sanitized so embedded control
 * characters or ANSI sequences cannot blow up the terminal.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tui/format.h"
#include "util/error.h"
#include "util/json.h"
#include "util/string.h"

#define SUMMARY_MAX_BYTES 512

/* ---- helpers ---------------------------------------------------------- */

/* Append `s` to `out`, replacing any non-printable / control bytes with
 * '?' so untrusted content cannot inject escape sequences into the TUI.
 * Newlines and tabs collapse to a single space. */
static void append_sanitized(String* out, const char* s, size_t n) {
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            (void)string_append_char(out, ' ');
        } else if (c < 0x20 || c == 0x7f) {
            (void)string_append_char(out, '?');
        } else {
            (void)string_append_char(out, (char)c);
        }
    }
}

/* Append a sanitized preview of a string field. Newlines and tabs are
 * flattened to spaces so multi-line content stays on one row. The caller
 * caps the total length; we do not truncate here. */
static void append_field_preview(String* out, const char* value) {
    if (value == NULL) {
        return;
    }
    append_sanitized(out, value, strlen(value));
}

/* Append a JSON scalar field if present, in the form "<key>=<value>".
 * Handles string, integer and real fields. Returns true when appended.
 * Non-scalar values (null, arrays, nested objects) are skipped — they
 * are not useful in a one-line summary. */
static bool append_kv(String* out, JsonVal* obj, const char* key) {
    if (obj == NULL) {
        return false;
    }
    JsonVal* v = json_val_obj_get(obj, key);
    if (v == NULL) {
        return false;
    }
    if (!json_val_is_str(v) && !json_val_is_int(v) &&
        !json_val_is_real(v) && !json_val_is_bool(v)) {
        return false;
    }
    string_append_char(out, ' ');
    string_append(out, key);
    string_append_char(out, '=');
    if (json_val_is_str(v)) {
        append_field_preview(out, json_val_str(v));
    } else if (json_val_is_int(v)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)json_val_int(v));
        string_append(out, buf);
    } else if (json_val_is_real(v)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", json_val_real(v));
        string_append(out, buf);
    } else {
        string_append(out, json_val_bool(v) ? "true" : "false");
    }
    return true;
}

/* Truncate to at most `max_cells` display cells by walking UTF-8 code
 * points. Counts each byte as 1 cell except for continuation bytes (those
 * cannot legally start a codepoint). This is intentionally a rough visual
 * width; CJK characters contribute >1, control bytes already replaced. */
static void truncate_to_cells(String* out, size_t max_cells) {
    if (out->len == 0) {
        return;
    }
    size_t cells = 0;
    size_t i = 0;
    while (i < out->len) {
        unsigned char c = (unsigned char)out->data[i];
        size_t step = 1;
        if ((c & 0x80) == 0) {
            step = 1;
        } else if ((c & 0xE0) == 0xC0) {
            step = 2;
        } else if ((c & 0xF0) == 0xE0) {
            step = 3;
        } else if ((c & 0xF8) == 0xF0) {
            step = 4;
        }
        if (i + step > out->len) {
            break;
        }
        cells += 1;
        if (cells > max_cells) {
            out->len = i;
            out->data[out->len] = '\0';
            string_append(out, "…");
            return;
        }
        i += step;
    }
}

/* Append a single space if `out` already has content and does not end in
 * whitespace. */
static void append_separator(String* out) {
    if (out->len > 0 && out->data[out->len - 1] != ' ') {
        string_append_char(out, ' ');
    }
}

/* Sanitize a possibly-NULL raw arguments string and truncate it. The
 * result is appended to `out`. Used as a fallback when the JSON does not
 * parse or the tool is unknown. */
static void append_raw_fallback(String* out, const char* args) {
    if (args == NULL || args[0] == '\0') {
        return;
    }
    /* Strip leading whitespace + leading '{' so the fallback does not look
     * like raw JSON. */
    const char* p = args;
    while (*p != '\0' && (unsigned char)*p <= ' ') {
        p++;
    }
    if (*p == '{') {
        p++;
    }
    while (*p != '\0' && (unsigned char)*p <= ' ') {
        p++;
    }
    append_sanitized(out, p, strlen(p));
}

/* ---- per-tool summaries ---------------------------------------------- */

static void summary_bash(String* out, JsonVal* args) {
    JsonVal* cmd = json_val_obj_get(args, "command");
    if (cmd != NULL && json_val_is_str(cmd)) {
        const char* s = json_val_str(cmd);
        if (s != NULL) {
            append_field_preview(out, s);
        }
    }
    JsonVal* to = json_val_obj_get(args, "timeout");
    if (to != NULL && (json_val_is_int(to) || json_val_is_real(to))) {
        char buf[32];
        snprintf(buf, sizeof(buf), " (timeout=%llds)",
                 (long long)(json_val_is_int(to) ? json_val_int(to)
                                                  : (int64_t)json_val_real(to)));
        string_append(out, buf);
    }
}

static void summary_read(String* out, JsonVal* args) {
    if (append_kv(out, args, "path")) {
        append_kv(out, args, "offset");
        append_kv(out, args, "limit");
    }
}

static void summary_write(String* out, JsonVal* args) {
    JsonVal* p = json_val_obj_get(args, "path");
    if (p != NULL && json_val_is_str(p)) {
        append_field_preview(out, json_val_str(p));
    }
    JsonVal* c = json_val_obj_get(args, "content");
    if (c != NULL && json_val_is_str(c)) {
        const char* s = json_val_str(c);
        if (s != NULL) {
            size_t n = strlen(s);
            char buf[32];
            snprintf(buf, sizeof(buf), " (%zu bytes)", n);
            string_append(out, buf);
        }
    }
}

static void summary_edit(String* out, JsonVal* args) {
    if (!append_kv(out, args, "path")) {
        return;
    }
    JsonVal* old = json_val_obj_get(args, "old_text");
    JsonVal* new = json_val_obj_get(args, "new_text");
    if (old != NULL && json_val_is_str(old) && new != NULL && json_val_is_str(new)) {
        size_t o = strlen(json_val_str(old));
        size_t n = strlen(json_val_str(new));
        char buf[48];
        snprintf(buf, sizeof(buf), " (-%zu/+%zu)", o, n);
        string_append(out, buf);
    }
}

static void summary_list(String* out, JsonVal* args) {
    JsonVal* p = json_val_obj_get(args, "path");
    if (p != NULL && json_val_is_str(p)) {
        const char* s = json_val_str(p);
        if (s != NULL && s[0] != '\0') {
            append_field_preview(out, s);
        }
    }
    append_kv(out, args, "depth");
    append_kv(out, args, "max_results");
    if (out->len == 0) {
        string_append(out, ".");
    }
}

static void summary_find(String* out, JsonVal* args) {
    if (append_kv(out, args, "pattern")) {
        append_kv(out, args, "path");
        append_kv(out, args, "max_depth");
    }
}

static void summary_grep(String* out, JsonVal* args) {
    if (append_kv(out, args, "pattern")) {
        append_kv(out, args, "path");
        JsonVal* m = json_val_obj_get(args, "max_results");
        if (m != NULL && (json_val_is_int(m) || json_val_is_real(m))) {
            char buf[32];
            snprintf(buf, sizeof(buf), " (-n %lld)",
                     (long long)(json_val_is_int(m) ? json_val_int(m)
                                                    : (int64_t)json_val_real(m)));
            string_append(out, buf);
        }
    }
}

static void summary_subagent(String* out, JsonVal* args) {
    JsonVal* tasks = json_val_obj_get(args, "tasks");
    if (tasks != NULL && json_val_is_arr(tasks)) {
        size_t n = json_val_arr_size(tasks);
        char buf[32];
        snprintf(buf, sizeof(buf), "%zu subagent%s", n, n == 1 ? "" : "s");
        string_append(out, buf);
        JsonVal* first = json_val_arr_get(tasks, 0);
        if (first != NULL && json_val_is_obj(first)) {
            string_append(out, ": ");
            append_field_preview(out, json_val_str(json_val_obj_get(first, "task")));
        }
        return;
    }
    JsonVal* t = json_val_obj_get(args, "task");
    if (t != NULL && json_val_is_str(t)) {
        append_field_preview(out, json_val_str(t));
    }
}

static void summary_memory(String* out, JsonVal* args) {
    JsonVal* k = json_val_obj_get(args, "kind");
    if (k != NULL && json_val_is_str(k)) {
        string_append(out, json_val_str(k));
        string_append(out, ": ");
    }
    append_field_preview(out, json_val_str(json_val_obj_get(args, "content")));
}

static void summary_plan(String* out, JsonVal* args) {
    JsonVal* a = json_val_obj_get(args, "action");
    if (a != NULL && json_val_is_str(a)) {
        string_append(out, json_val_str(a));
    }
    JsonVal* id = json_val_obj_get(args, "id");
    if (id != NULL && json_val_is_str(id)) {
        const char* s = json_val_str(id);
        if (s != NULL && s[0] != '\0') {
            string_append(out, " ");
            string_append(out, s);
        }
        JsonVal* t = json_val_obj_get(args, "title");
        if (t != NULL && json_val_is_str(t)) {
            string_append(out, " — ");
            append_field_preview(out, json_val_str(t));
        }
    }
}

static void summary_git_commit(String* out, JsonVal* args) {
    append_field_preview(out, json_val_str(json_val_obj_get(args, "message")));
}

static void summary_git_revert(String* out, JsonVal* args) {
    append_field_preview(out, json_val_str(json_val_obj_get(args, "target")));
}

static void summary_git_diff(String* out, JsonVal* args) {
    JsonVal* p = json_val_obj_get(args, "path");
    if (p != NULL && json_val_is_str(p)) {
        const char* s = json_val_str(p);
        if (s != NULL && s[0] != '\0') {
            append_field_preview(out, s);
        }
    }
    if (out->len == 0) {
        string_append(out, "working tree");
    }
}

static void summary_test(String* out, JsonVal* args) {
    append_kv(out, args, "action");
    JsonVal* b = json_val_obj_get(args, "build_dir");
    if (b != NULL && json_val_is_str(b)) {
        const char* s = json_val_str(b);
        if (s != NULL && s[0] != '\0') {
            string_append(out, " (");
            string_append(out, s);
            string_append(out, ")");
        }
    }
}

static void summary_bench(String* out, JsonVal* args) {
    JsonVal* b = json_val_obj_get(args, "build_dir");
    if (b != NULL && json_val_is_str(b)) {
        const char* s = json_val_str(b);
        if (s != NULL && s[0] != '\0') {
            string_append(out, s);
            return;
        }
    }
    string_append(out, "default build");
}

static void summary_kv_only(String* out, JsonVal* args, const char* key) {
    JsonVal* v = json_val_obj_get(args, key);
    if (v != NULL && json_val_is_str(v)) {
        const char* s = json_val_str(v);
        if (s != NULL && s[0] != '\0') {
            append_field_preview(out, s);
        }
    }
}

/* ---- public entry point ---------------------------------------------- */

typedef void (*SummaryFn)(String* out, JsonVal* args);

typedef struct {
    const char* name;
    SummaryFn fn;
} SummaryDispatch;

static const SummaryDispatch kDispatch[] = {
    {"bash", summary_bash},
    {"read", summary_read},
    {"write", summary_write},
    {"edit", summary_edit},
    {"list", summary_list},
    {"find", summary_find},
    {"grep", summary_grep},
    {"subagent", summary_subagent},
    {"memory", summary_memory},
    {"plan", summary_plan},
    {"git_commit", summary_git_commit},
    {"git_revert", summary_git_revert},
    {"git_diff", summary_git_diff},
    {"test", summary_test},
    {"bench", summary_bench},
    {"git_checkpoint", NULL},
    {"git_restore_checkpoint", NULL},
    {"git_status", NULL},
    {"diagnose", NULL},
};

String tui_format_tool_call_summary(const char* name, const char* arguments) {
    String out = string_new();
    string_append(&out, name != NULL ? name : "?");

    if (arguments != NULL && arguments[0] != '\0') {
        JsonDoc* doc = json_parse(arguments, strlen(arguments));
        if (doc != NULL) {
            JsonVal* root = json_root(doc);
            JsonVal* obj = (root != NULL && json_val_is_obj(root)) ? root : NULL;
            SummaryFn fn = NULL;
            for (size_t i = 0; i < sizeof(kDispatch) / sizeof(kDispatch[0]); i++) {
                if (name != NULL && strcmp(name, kDispatch[i].name) == 0) {
                    fn = kDispatch[i].fn;
                    break;
                }
            }
            if (obj != NULL && fn != NULL) {
                append_separator(&out);
                String body = string_new();
                fn(&body, obj);
                if (body.len > 0) {
                    string_append(&out, body.data);
                }
                string_free(&body);
            } else if (obj != NULL) {
                /* Known tool with no dedicated formatter (e.g. git_status). */
                append_separator(&out);
                summary_kv_only(&out, obj, "path");
                if (out.len == strlen(name) + 1) {
                    /* Nothing to show; drop the trailing separator. */
                    out.len = strlen(name);
                    out.data[out.len] = '\0';
                }
            } else {
                /* args is JSON but not an object — fall back below. */
                append_separator(&out);
                append_raw_fallback(&out, arguments);
            }
            json_doc_free(doc);
        } else {
            append_separator(&out);
            append_raw_fallback(&out, arguments);
        }
    }

    if (out.len > SUMMARY_MAX_BYTES) {
        out.len = SUMMARY_MAX_BYTES;
        out.data[out.len] = '\0';
        string_append(&out, "…");
    }
    truncate_to_cells(&out, TUI_TOOL_SUMMARY_MAX_CELLS);
    return out;
}
