/*
 * tool/edit.c — edit tool: precise single-occurrence text replacement.
 *
 * Arguments (JSON): { "path": string, "old_text": string,
 *                     "new_text": string }
 *
 * Behavior (DESIGN.md §19):
 *   - old_text must match EXACTLY once; zero matches or multiple matches
 *     are errors (the model must disambiguate).
 *   - writes via temp file + rename so a failed write never corrupts the
 *     original.
 *   - files larger than 8 MiB are refused.
 *
 * Ownership: result.content is malloc'ed by this tool; the caller frees.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tool/tool.h"
#include "util/diff.h"
#include "util/json.h"
#include "util/string.h"

#define EDIT_MAX_FILE (8 * 1024 * 1024)

static int resolve_path(const char* cwd, const char* path, char* out, size_t out_size) {
    if (path[0] == '/') {
        snprintf(out, out_size, "%s", path);
    } else if (cwd != NULL) {
        snprintf(out, out_size, "%s/%s", cwd, path);
    } else {
        snprintf(out, out_size, "%s", path);
    }
    return out[0] == '\0' ? AGENT_ERR_TOOL : AGENT_OK;
}

static int edit_preview(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    JsonDoc* doc = json_parse(arguments, strlen(arguments));
    if (doc == NULL) {
        result->content = strdup("error: arguments are not valid JSON");
        result->is_error = true;
        return AGENT_OK;
    }
    JsonVal* root = json_root(doc);
    const char* path = json_obj_get_str(root, "path");
    const char* old_text = json_obj_get_str(root, "old_text");
    const char* new_text = json_obj_get_str(root, "new_text");
    if (path == NULL || old_text == NULL || new_text == NULL || old_text[0] == '\0') {
        result->content = strdup("error: path, non-empty old_text, and new_text are required");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    char full[PATH_MAX];
    if (resolve_path(ctx != NULL ? ctx->cwd : NULL, path, full, sizeof(full)) != AGENT_OK) {
        result->content = strdup("error: invalid path");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    struct stat st = {0};
    int stat_rc = stat(full, &st);
    if (stat_rc != 0 || st.st_size > EDIT_MAX_FILE) {
        String msg = string_new();
        if (stat_rc == 0 && st.st_size > EDIT_MAX_FILE) {
            string_printf(&msg, "error: %s exceeds the 8 MiB edit limit", full);
        } else {
            string_printf(&msg, "error: cannot preview %s: %s", full, strerror(errno));
        }
        result->content = string_take(&msg);
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    FILE* f = fopen(full, "rb");
    if (f == NULL) {
        String msg = string_new();
        string_printf(&msg, "error: cannot preview %s: %s", full, strerror(errno));
        result->content = string_take(&msg);
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    char* data = malloc((size_t)st.st_size + 1);
    if (data == NULL) {
        fclose(f);
        json_doc_free(doc);
        return AGENT_ERR_OOM;
    }
    size_t n = fread(data, 1, (size_t)st.st_size, f);
    fclose(f);
    data[n] = '\0';
    size_t old_len = strlen(old_text);
    size_t count = 0;
    for (const char* p = data; (p = strstr(p, old_text)) != NULL; p += old_len)
        count++;
    free(data);
    if (count != 1) {
        String msg = string_new();
        if (count == 0) {
            string_printf(&msg, "error: old_text not found in %s", full);
        } else {
            string_printf(&msg, "error: old_text matches %zu times in %s", count, full);
        }
        result->content = string_take(&msg);
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    String preview = string_new();
    int rc = diff_preview_build(full, old_text, old_len, new_text, strlen(new_text), &preview);
    json_doc_free(doc);
    if (rc != AGENT_OK) {
        string_free(&preview);
        return rc;
    }
    result->content = string_take(&preview);
    return AGENT_OK;
}

static int edit_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;

    JsonDoc* doc = json_parse(arguments, strlen(arguments));
    if (doc == NULL) {
        result->content = strdup("error: arguments are not valid JSON");
        result->is_error = true;
        return AGENT_OK;
    }
    JsonVal* root = json_root(doc);
    const char* path = json_obj_get_str(root, "path");
    const char* old_text = json_obj_get_str(root, "old_text");
    const char* new_text = json_obj_get_str(root, "new_text");
    if (path == NULL || old_text == NULL || new_text == NULL) {
        result->content = strdup("error: \"path\", \"old_text\" and "
                                 "\"new_text\" are all required");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    if (old_text[0] == '\0') {
        result->content = strdup("error: old_text must not be empty");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    /* path/old_text/new_text are borrowed from the document; keep it alive */

    char full[PATH_MAX];
    if (resolve_path(ctx->cwd, path, full, sizeof(full)) != AGENT_OK) {
        json_doc_free(doc);
        result->content = strdup("error: invalid path");
        result->is_error = true;
        return AGENT_OK;
    }

    /* size check */
    struct stat st;
    if (stat(full, &st) != 0) {
        json_doc_free(doc);
        String msg = string_new();
        string_printf(&msg, "error: cannot stat %s: %s", full, strerror(errno));
        result->content = string_take(&msg);
        result->is_error = true;
        return AGENT_OK;
    }
    if (st.st_size > EDIT_MAX_FILE) {
        json_doc_free(doc);
        String msg = string_new();
        string_printf(&msg, "error: %s is %.1f MiB (limit %d MiB); refusing", full,
                      st.st_size / 1048576.0, EDIT_MAX_FILE / 1048576);
        result->content = string_take(&msg);
        result->is_error = true;
        return AGENT_OK;
    }

    /* read the whole file */
    FILE* f = fopen(full, "rb");
    if (f == NULL) {
        json_doc_free(doc);
        String msg = string_new();
        string_printf(&msg, "error: cannot open %s: %s", full, strerror(errno));
        result->content = string_take(&msg);
        result->is_error = true;
        return AGENT_OK;
    }
    char* data = malloc((size_t)st.st_size + 1);
    if (data == NULL) {
        fclose(f);
        json_doc_free(doc);
        result->content = strdup("error: out of memory");
        result->is_error = true;
        return AGENT_OK;
    }
    size_t n = fread(data, 1, (size_t)st.st_size, f);
    fclose(f);
    data[n] = '\0';

    /* count occurrences of old_text */
    size_t old_len = strlen(old_text);
    size_t count = 0;
    const char* hit = NULL;
    for (const char* p = data; (p = strstr(p, old_text)) != NULL; p += old_len) {
        count++;
        hit = p;
    }

    if (count == 0) {
        json_doc_free(doc);
        String msg = string_new();
        string_printf(&msg, "error: old_text not found in %s", full);
        result->content = string_take(&msg);
        result->is_error = true;
        free(data);
        return AGENT_OK;
    }
    if (count > 1) {
        json_doc_free(doc);
        String msg = string_new();
        string_printf(&msg,
                      "error: old_text matches %zu times in %s; provide more "
                      "context to make the match unique",
                      count, full);
        result->content = string_take(&msg);
        result->is_error = true;
        free(data);
        return AGENT_OK;
    }

    /* single match: assemble before + new_text + after */
    size_t before_len = (size_t)(hit - data);
    const char* after = hit + old_len;
    size_t after_len = n - before_len - old_len;
    size_t new_len = strlen(new_text);

    String out = string_new();
    string_append_n(&out, data, before_len);
    string_append_n(&out, new_text, new_len);
    string_append_n(&out, after, after_len);

    /* atomic write: temp file + rename */
    char tmp_path[PATH_MAX + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp%ld", full, (long)getpid());
    FILE* tf = fopen(tmp_path, "wb");
    if (tf == NULL) {
        json_doc_free(doc);
        string_free(&out);
        String msg = string_new();
        string_printf(&msg, "error: cannot write %s: %s", tmp_path, strerror(errno));
        result->content = string_take(&msg);
        result->is_error = true;
        free(data);
        return AGENT_OK;
    }
    if (fchmod(fileno(tf), st.st_mode & 0777) != 0) {
        int saved_errno = errno;
        fclose(tf);
        unlink(tmp_path);
        json_doc_free(doc);
        string_free(&out);
        String msg = string_new();
        string_printf(&msg, "error: cannot preserve permissions for %s: %s", full,
                      strerror(saved_errno));
        result->content = string_take(&msg);
        result->is_error = true;
        free(data);
        return AGENT_OK;
    }
    size_t written = fwrite(out.data, 1, out.len, tf);
    if (fclose(tf) != 0 || written != out.len || rename(tmp_path, full) != 0) {
        json_doc_free(doc);
        string_free(&out);
        unlink(tmp_path);
        String msg = string_new();
        string_printf(&msg, "error: failed to commit edit to %s: %s", full, strerror(errno));
        result->content = string_take(&msg);
        result->is_error = true;
        free(data);
        return AGENT_OK;
    }

    free(data);
    json_doc_free(doc);
    string_free(&out);

    String msg = string_new();
    string_printf(&msg, "edited %s: replaced 1 occurrence (%zu bytes)", full,
                  new_len - old_len < 0 ? 0 : new_len - old_len);
    result->content = string_take(&msg);
    return AGENT_OK;
}

Tool edit_tool = {
    .name = "edit",
    .description = "Replace text in a file. old_text must match exactly "
                   "once; include enough surrounding context to make it "
                   "unique. Creates the file if it does not exist? No — "
                   "use write for new files.",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"path\":"
                    "{\"type\":\"string\"},\"old_text\":{\"type\":\"string\"},"
                    "\"new_text\":{\"type\":\"string\"}},\"required\":"
                    "[\"path\",\"old_text\",\"new_text\"]}",
    .flags = TOOL_FLAG_APPROVAL_REQUIRED,
    .preview = edit_preview,
    .execute = edit_execute,
};
