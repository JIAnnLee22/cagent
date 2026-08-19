/* Shared workspace path policy for file-oriented tools. */
#include "tool/path_policy.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>

static bool safe_relative(const char* path) {
    if (path == NULL || path[0] == '\0' || path[0] == '/') {
        return false;
    }
    for (const char* p = path; *p != '\0';) {
        while (*p == '/') {
            p++;
        }
        const char* start = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        if ((size_t)(p - start) == 2 && start[0] == '.' && start[1] == '.') {
            return false;
        }
    }
    return true;
}

static bool inside_root(const char* root, const char* path) {
    size_t root_len = strlen(root);
    return strcmp(root, "/") == 0 ||
           (strncmp(root, path, root_len) == 0 &&
            (path[root_len] == '\0' || path[root_len] == '/'));
}

static int copy_path(const char* path, char* out, size_t out_size) {
    int n = snprintf(out, out_size, "%s", path);
    return n >= 0 && (size_t)n < out_size ? AGENT_OK : AGENT_ERR_TOOL;
}

int tool_path_resolve(const ToolContext* ctx, const char* path, bool allow_missing_leaf,
                      char* out, size_t out_size) {
    if (out == NULL || out_size == 0 || ctx == NULL || ctx->cwd == NULL ||
        !safe_relative(path)) {
        return AGENT_ERR_TOOL;
    }

    char workspace[PATH_MAX];
    if (realpath(ctx->cwd, workspace) == NULL) {
        return AGENT_ERR_TOOL;
    }

    char candidate[PATH_MAX];
    int n = snprintf(candidate, sizeof(candidate), "%s/%s", workspace, path);
    if (n < 0 || (size_t)n >= sizeof(candidate)) {
        return AGENT_ERR_TOOL;
    }

    char resolved[PATH_MAX];
    if (realpath(candidate, resolved) != NULL) {
        return inside_root(workspace, resolved) ? copy_path(resolved, out, out_size)
                                                : AGENT_ERR_TOOL;
    }
    int realpath_errno = errno;
    if (!allow_missing_leaf || realpath_errno != ENOENT) {
        return AGENT_ERR_TOOL;
    }

    /* A dangling symlink is an existing leaf for policy purposes. Do not
     * allow write() to follow it after realpath() failed. */
    struct stat st;
    if (lstat(candidate, &st) == 0 || errno != ENOENT) {
        return AGENT_ERR_TOOL;
    }

    char parent[PATH_MAX];
    if (copy_path(candidate, parent, sizeof(parent)) != AGENT_OK) {
        return AGENT_ERR_TOOL;
    }
    char* slash = strrchr(parent, '/');
    if (slash == NULL || slash[1] == '\0') {
        return AGENT_ERR_TOOL;
    }
    const char* leaf = slash + 1;
    *slash = '\0';

    char resolved_parent[PATH_MAX];
    if (realpath(parent, resolved_parent) == NULL ||
        !inside_root(workspace, resolved_parent)) {
        return AGENT_ERR_TOOL;
    }
    n = snprintf(resolved, sizeof(resolved), "%s/%s", resolved_parent, leaf);
    if (n < 0 || (size_t)n >= sizeof(resolved)) {
        return AGENT_ERR_TOOL;
    }
    return copy_path(resolved, out, out_size);
}
