/*
 * tool/write.c — write tool: create or fully overwrite a file.
 *
 * Arguments (JSON): { "path": string, "content": string }
 *
 * Behavior: creates the file (overwriting any existing content), reports
 * the byte count written. Path/permission errors are returned as tool
 * errors (errno text included).
 *
 * Ownership: result.content is malloc'ed by this tool; the caller frees.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tool/tool.h"
#include "util/diff.h"
#include "util/json.h"
#include "util/string.h"

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

static int write_preview(ToolContext* ctx, const char* arguments, ToolResult* result) {
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
    const char* content = json_obj_get_str(root, "content");
    if (path == NULL || path[0] == '\0' || content == NULL) {
        result->content = strdup("error: both \"path\" and \"content\" are required");
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

    char* old = NULL;
    size_t old_len = 0;
    struct stat st;
    if (stat(full, &st) == 0) {
        if (st.st_size > 1024 * 1024) {
            String summary = string_new();
            string_printf(&summary,
                          "overwrite %s: existing file is %lld bytes; new content is %zu bytes\n"
                          "[line preview omitted for files larger than 1 MiB]",
                          full, (long long)st.st_size, strlen(content));
            result->content = string_take(&summary);
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
        old = malloc((size_t)st.st_size + 1);
        if (old == NULL) {
            fclose(f);
            json_doc_free(doc);
            return AGENT_ERR_OOM;
        }
        old_len = fread(old, 1, (size_t)st.st_size, f);
        fclose(f);
        old[old_len] = '\0';
    } else if (errno != ENOENT) {
        String msg = string_new();
        string_printf(&msg, "error: cannot preview %s: %s", full, strerror(errno));
        result->content = string_take(&msg);
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }

    String preview = string_new();
    int rc = diff_preview_build(full, old, old_len, content, strlen(content), &preview);
    free(old);
    json_doc_free(doc);
    if (rc != AGENT_OK) {
        string_free(&preview);
        return rc;
    }
    result->content = string_take(&preview);
    return AGENT_OK;
}

static int write_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
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
    const char* content = json_obj_get_str(root, "content");
    if (path == NULL || path[0] == '\0' || content == NULL) {
        result->content = strdup("error: both \"path\" and \"content\" are required");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    /* path/content are borrowed from the document; keep it alive */

    char full[PATH_MAX];
    if (resolve_path(ctx->cwd, path, full, sizeof(full)) != AGENT_OK) {
        json_doc_free(doc);
        result->content = strdup("error: invalid path");
        result->is_error = true;
        return AGENT_OK;
    }

    FILE* f = fopen(full, "w");
    if (f == NULL) {
        json_doc_free(doc);
        String msg = string_new();
        string_printf(&msg, "error: cannot write %s: %s", full, strerror(errno));
        result->content = string_take(&msg);
        result->is_error = true;
        return AGENT_OK;
    }

    size_t n = strlen(content);
    size_t written = fwrite(content, 1, n, f);
    if (fclose(f) != 0 || written != n) {
        json_doc_free(doc);
        String msg = string_new();
        string_printf(&msg, "error: short write to %s: %s", full, strerror(errno));
        result->content = string_take(&msg);
        result->is_error = true;
        return AGENT_OK;
    }

    String out = string_new();
    string_printf(&out, "wrote %zu bytes to %s", written, full);
    json_doc_free(doc);

    result->content = string_take(&out);
    return AGENT_OK;
}

Tool write_tool = {
    .name = "write",
    .description = "Create a file or fully overwrite an existing one with "
                   "the given content.",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"path\":"
                    "{\"type\":\"string\",\"description\":\"File path, "
                    "relative to the working directory\"},\"content\":"
                    "{\"type\":\"string\"}},\"required\":[\"path\","
                    "\"content\"]}",
    .flags = TOOL_FLAG_APPROVAL_REQUIRED,
    .preview = write_preview,
    .execute = write_execute,
};
