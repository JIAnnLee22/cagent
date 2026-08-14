/* Bounded, deterministic workspace directory listing. */
#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tool/tool.h"
#include "util/json.h"
#include "util/string.h"

#define LIST_OUTPUT_CAP (64 * 1024)
#define LIST_DEFAULT_MAX 200
#define LIST_MAX_RESULTS 500
#define LIST_MAX_DEPTH 4

typedef struct {
    String out;
    size_t count;
    size_t max_results;
    int max_depth;
    bool include_hidden;
    bool truncated;
} ListState;

static bool safe_relative(const char* path) {
    if (path == NULL || path[0] == '\0' || path[0] == '/')
        return false;
    for (const char* p = path; *p != '\0';) {
        while (*p == '/')
            p++;
        const char* start = p;
        while (*p != '\0' && *p != '/')
            p++;
        if ((size_t)(p - start) == 2 && start[0] == '.' && start[1] == '.')
            return false;
    }
    return true;
}

static bool inside_root(const char* root, const char* path) {
    size_t n = strlen(root);
    return strcmp(root, "/") == 0 ||
           (strncmp(root, path, n) == 0 && (path[n] == '\0' || path[n] == '/'));
}

static bool skip_recurse(const char* name) {
    static const char* skipped[] = {".git", "node_modules", "build", "build-asan", "build-bench",
                                    "dist", "target",       ".venv", "__pycache__"};
    for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++) {
        if (strcmp(name, skipped[i]) == 0)
            return true;
    }
    return false;
}

static int list_walk(const char* full, const char* display, int depth, ListState* state) {
    struct dirent** entries = NULL;
    int n = scandir(full, &entries, NULL, alphasort);
    if (n < 0)
        return AGENT_ERR_IO;
    for (int i = 0; i < n; i++) {
        const char* name = entries[i]->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
            (!state->include_hidden && name[0] == '.')) {
            free(entries[i]);
            continue;
        }
        if (state->count >= state->max_results || state->out.len >= LIST_OUTPUT_CAP) {
            state->truncated = true;
            free(entries[i]);
            continue;
        }
        char child[PATH_MAX];
        char shown[PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", full, name) >= (int)sizeof(child) ||
            snprintf(shown, sizeof(shown), "%s%s%s", display, strcmp(display, ".") == 0 ? "/" : "/",
                     name) >= (int)sizeof(shown)) {
            state->truncated = true;
            free(entries[i]);
            continue;
        }
        struct stat st;
        if (lstat(child, &st) != 0) {
            free(entries[i]);
            continue;
        }
        const char* suffix = S_ISDIR(st.st_mode) ? "/" : (S_ISLNK(st.st_mode) ? "@" : "");
        if (string_printf(&state->out, "%s%s\n", shown, suffix) != AGENT_OK) {
            free(entries[i]);
            for (int j = i + 1; j < n; j++)
                free(entries[j]);
            free(entries);
            return AGENT_ERR_OOM;
        }
        state->count++;
        if (S_ISDIR(st.st_mode) && depth < state->max_depth && !skip_recurse(name)) {
            int rc = list_walk(child, shown, depth + 1, state);
            if (rc == AGENT_ERR_OOM) {
                free(entries[i]);
                for (int j = i + 1; j < n; j++)
                    free(entries[j]);
                free(entries);
                return rc;
            }
            if (rc == AGENT_ERR_IO)
                state->truncated = true;
        }
        free(entries[i]);
    }
    free(entries);
    return AGENT_OK;
}

static int list_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    JsonDoc* doc =
        json_parse(arguments != NULL ? arguments : "{}", arguments != NULL ? strlen(arguments) : 2);
    if (doc == NULL) {
        result->content = strdup("error: arguments are not valid JSON");
        result->is_error = true;
        return AGENT_OK;
    }
    JsonVal* root_obj = json_root(doc);
    const char* path = json_obj_get_str(root_obj, "path");
    if (path == NULL || path[0] == '\0')
        path = ".";
    if (!safe_relative(path)) {
        result->content = strdup("error: path must stay inside the workspace");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    int64_t depth = json_obj_get_int(root_obj, "depth", 1);
    int64_t max_results = json_obj_get_int(root_obj, "max_results", LIST_DEFAULT_MAX);
    if (depth < 1)
        depth = 1;
    if (depth > LIST_MAX_DEPTH)
        depth = LIST_MAX_DEPTH;
    if (max_results < 1)
        max_results = 1;
    if (max_results > LIST_MAX_RESULTS)
        max_results = LIST_MAX_RESULTS;

    char workspace[PATH_MAX];
    char target_input[PATH_MAX];
    char target[PATH_MAX];
    const char* cwd = ctx != NULL && ctx->cwd != NULL ? ctx->cwd : ".";
    if (realpath(cwd, workspace) == NULL ||
        snprintf(target_input, sizeof(target_input), "%s/%s", workspace, path) >=
            (int)sizeof(target_input) ||
        realpath(target_input, target) == NULL || !inside_root(workspace, target)) {
        result->content = strdup("error: cannot list path or path leaves the workspace");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    struct stat st;
    if (stat(target, &st) != 0 || !S_ISDIR(st.st_mode)) {
        result->content = strdup("error: list path is not a directory");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }

    ListState state = {.out = string_new(),
                       .max_results = (size_t)max_results,
                       .max_depth = (int)depth,
                       .include_hidden = json_obj_get_bool(root_obj, "include_hidden", false)};
    int rc = list_walk(target, path, 1, &state);
    if (rc == AGENT_ERR_IO && state.count == 0) {
        string_free(&state.out);
        result->content = strdup("error: cannot read directory");
        result->is_error = true;
    } else if (rc != AGENT_OK && rc != AGENT_ERR_IO) {
        string_free(&state.out);
        json_doc_free(doc);
        return rc;
    } else {
        if (state.count == 0)
            string_append(&state.out, "(empty directory)\n");
        if (state.truncated)
            string_append(&state.out, "...[listing truncated]\n");
        result->content = string_take(&state.out);
    }
    json_doc_free(doc);
    return AGENT_OK;
}

Tool list_tool = {
    .name = "list",
    .description =
        "List workspace files and directories with deterministic ordering and bounded depth.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"depth\":{\"type\":"
        "\"integer\",\"minimum\":1,\"maximum\":4},\"max_results\":{\"type\":\"integer\","
        "\"minimum\":1,\"maximum\":500},\"include_hidden\":{\"type\":\"boolean\"}}}",
    .flags = TOOL_FLAG_PARALLEL_SAFE,
    .execute = list_execute,
};
