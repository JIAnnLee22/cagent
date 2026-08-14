/* Bounded workspace file-name search without invoking a shell. */
#include <dirent.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tool/tool.h"
#include "util/json.h"
#include "util/string.h"

#define FIND_OUTPUT_CAP (64 * 1024)
#define FIND_DEFAULT_MAX 100
#define FIND_MAX_RESULTS 500
#define FIND_MAX_DEPTH 12

typedef struct {
    String out;
    const char* pattern;
    size_t count;
    size_t max_results;
    int max_depth;
    bool include_hidden;
    bool glob;
    bool truncated;
} FindState;

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

static bool matches(const FindState* state, const char* relative, const char* name) {
    if (state->glob) {
        return fnmatch(state->pattern, relative, FNM_PATHNAME) == 0 ||
               fnmatch(state->pattern, name, 0) == 0;
    }
    return strcasestr(relative, state->pattern) != NULL;
}

static int find_walk(const char* full, const char* display, int depth, FindState* state) {
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
        char child[PATH_MAX];
        char shown[PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", full, name) >= (int)sizeof(child) ||
            snprintf(shown, sizeof(shown), "%s/%s", display, name) >= (int)sizeof(shown)) {
            state->truncated = true;
            free(entries[i]);
            continue;
        }
        struct stat st;
        if (lstat(child, &st) != 0) {
            free(entries[i]);
            continue;
        }
        if (matches(state, shown, name)) {
            if (state->count >= state->max_results || state->out.len >= FIND_OUTPUT_CAP) {
                state->truncated = true;
            } else {
                const char* suffix = S_ISDIR(st.st_mode) ? "/" : (S_ISLNK(st.st_mode) ? "@" : "");
                if (string_printf(&state->out, "%s%s\n", shown, suffix) != AGENT_OK) {
                    free(entries[i]);
                    for (int j = i + 1; j < n; j++)
                        free(entries[j]);
                    free(entries);
                    return AGENT_ERR_OOM;
                }
                state->count++;
            }
        }
        if (S_ISDIR(st.st_mode) && depth < state->max_depth && !skip_recurse(name) &&
            state->count < state->max_results && state->out.len < FIND_OUTPUT_CAP) {
            int rc = find_walk(child, shown, depth + 1, state);
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

static int find_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    JsonDoc* doc =
        json_parse(arguments != NULL ? arguments : "{}", arguments != NULL ? strlen(arguments) : 2);
    if (doc == NULL) {
        result->content = strdup("error: arguments are not valid JSON");
        result->is_error = true;
        return AGENT_OK;
    }
    JsonVal* obj = json_root(doc);
    const char* pattern = json_obj_get_str(obj, "pattern");
    const char* path = json_obj_get_str(obj, "path");
    if (path == NULL || path[0] == '\0')
        path = ".";
    if (pattern == NULL || pattern[0] == '\0') {
        result->content = strdup("error: missing required argument \"pattern\"");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    if (!safe_relative(path)) {
        result->content = strdup("error: path must stay inside the workspace");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    int64_t depth = json_obj_get_int(obj, "max_depth", 8);
    int64_t max_results = json_obj_get_int(obj, "max_results", FIND_DEFAULT_MAX);
    if (depth < 1)
        depth = 1;
    if (depth > FIND_MAX_DEPTH)
        depth = FIND_MAX_DEPTH;
    if (max_results < 1)
        max_results = 1;
    if (max_results > FIND_MAX_RESULTS)
        max_results = FIND_MAX_RESULTS;

    const char* cwd = ctx != NULL && ctx->cwd != NULL ? ctx->cwd : ".";
    char workspace[PATH_MAX], input[PATH_MAX], target[PATH_MAX];
    if (realpath(cwd, workspace) == NULL ||
        snprintf(input, sizeof(input), "%s/%s", workspace, path) >= (int)sizeof(input) ||
        realpath(input, target) == NULL || !inside_root(workspace, target)) {
        result->content = strdup("error: cannot search path or path leaves the workspace");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    struct stat st;
    if (stat(target, &st) != 0 || !S_ISDIR(st.st_mode)) {
        result->content = strdup("error: find path is not a directory");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }

    FindState state = {.out = string_new(),
                       .pattern = pattern,
                       .max_results = (size_t)max_results,
                       .max_depth = (int)depth,
                       .include_hidden = json_obj_get_bool(obj, "include_hidden", false),
                       .glob = strpbrk(pattern, "*?[") != NULL};
    int rc = find_walk(target, path, 1, &state);
    if (rc == AGENT_ERR_IO && state.count == 0) {
        string_free(&state.out);
        result->content = strdup("error: cannot search directory");
        result->is_error = true;
    } else if (rc != AGENT_OK && rc != AGENT_ERR_IO) {
        string_free(&state.out);
        json_doc_free(doc);
        return rc;
    } else {
        if (state.count == 0)
            string_append(&state.out, "(no matching paths)\n");
        if (state.truncated)
            string_append(&state.out, "...[results truncated]\n");
        result->content = string_take(&state.out);
    }
    json_doc_free(doc);
    return AGENT_OK;
}

Tool find_tool = {
    .name = "find",
    .description = "Find workspace paths by case-insensitive substring or shell-style glob, with "
                   "bounded traversal.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":"
        "\"string\"},\"max_depth\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":12},\"max_"
        "results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500},\"include_hidden\":{"
        "\"type\":\"boolean\"}},\"required\":[\"pattern\"]}",
    .flags = TOOL_FLAG_PARALLEL_SAFE,
    .execute = find_execute,
};
