/*
 * tool/read.c — read tool: read a text file with line numbers.
 *
 * Arguments (JSON): { "path": string, "offset": int (>=1, default 1),
 *                     "limit": int (1..2000, default 200) }
 *
 * Behavior:
 *   - missing/empty path        -> error
 *   - nonexistent file          -> error with errno text
 *   - binary content (NUL byte  -> error "binary file"
 *     in the first 8 KiB)
 *   - output: "L<n>: <line>" per line; a trailing note when the file has
 *     more lines than requested.
 *
 * Ownership: result.content is malloc'ed by this tool; the caller frees.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool/tool.h"
#include "util/json.h"
#include "util/string.h"

#define READ_BINARY_SCAN 8192
#define READ_DEFAULT_LIMIT 200
#define READ_MAX_LIMIT 2000
#define READ_LINE_MAX 65536

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

static int check_binary(FILE* f) {
    char buf[READ_BINARY_SCAN];
    size_t n = fread(buf, 1, sizeof(buf), f);
    if (n > 0 && memchr(buf, '\0', n) != NULL) {
        return 1; /* binary */
    }
    fseek(f, 0, SEEK_SET);
    return 0;
}

static int read_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
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
    if (path == NULL || path[0] == '\0') {
        result->content = strdup("error: missing required argument \"path\"");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    int64_t offset = json_obj_get_int(root, "offset", 1);
    int64_t limit = json_obj_get_int(root, "limit", READ_DEFAULT_LIMIT);
    if (offset < 1) {
        offset = 1;
    }
    if (limit < 1) {
        limit = 1;
    }
    if (limit > READ_MAX_LIMIT) {
        limit = READ_MAX_LIMIT;
    }
    /* NOTE: `path` is borrowed from the yyjson document; the document must
     * stay alive until we are done with it (freed at the end of the
     * function). */

    char full[PATH_MAX];
    if (resolve_path(ctx->cwd, path, full, sizeof(full)) != AGENT_OK) {
        json_doc_free(doc);
        result->content = strdup("error: invalid path");
        result->is_error = true;
        return AGENT_OK;
    }

    FILE* f = fopen(full, "rb");
    if (f == NULL) {
        json_doc_free(doc);
        String msg = string_new();
        string_printf(&msg, "error: cannot open %s: %s", full, strerror(errno));
        result->content = string_take(&msg);
        result->is_error = true;
        return AGENT_OK;
    }

    if (check_binary(f)) {
        fclose(f);
        json_doc_free(doc);
        result->content = strdup("error: file looks binary (NUL byte found); "
                                 "refusing to dump it as text");
        result->is_error = true;
        return AGENT_OK;
    }

    String out = string_new();
    char line[READ_LINE_MAX];
    long lineno = 0;
    long emitted = 0;
    int truncated = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        if (lineno < offset) {
            continue;
        }
        if (emitted >= limit) {
            /* this line itself is the evidence of more content */
            truncated = 1;
            break;
        }
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        string_printf(&out, "L%ld: %s\n", lineno, line);
        emitted++;
    }
    fclose(f);

    if (truncated) {
        string_append(&out, "...[file has more lines; use offset/limit to "
                            "read further]\n");
    }
    if (emitted == 0 && lineno == 0) {
        string_append(&out, "(empty file)\n");
    }

    json_doc_free(doc);

    result->content = string_take(&out);
    return AGENT_OK;
}

Tool read_tool = {
    .name = "read",
    .description = "Read a text file with line numbers. Use offset (1-based) "
                   "and limit (max 2000) to read a slice of a large file.",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"path\":"
                    "{\"type\":\"string\",\"description\":\"File path, "
                    "relative to the working directory\"},\"offset\":"
                    "{\"type\":\"integer\",\"minimum\":1},"
                    "\"limit\":{\"type\":\"integer\",\"minimum\":1,"
                    "\"maximum\":2000}},\"required\":[\"path\"]}",
    .flags = TOOL_FLAG_PARALLEL_SAFE,
    .execute = read_execute,
};
