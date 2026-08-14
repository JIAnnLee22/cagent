#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util/project_context.h"

#define PROJECT_CONTEXT_FILE_CAP (64 * 1024)
#define PROJECT_CONTEXT_TOTAL_CAP (128 * 1024)
#define PROJECT_CONTEXT_MAX_DEPTH 64

static bool path_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool regular_file(const char* path, struct stat* st) {
    /* Instruction symlinks are ignored: following one could disclose an
     * arbitrary file outside the workspace to the model provider. */
    return lstat(path, st) == 0 && S_ISREG(st->st_mode);
}

static int append_file(const char* path, const char* label, String* prompt, size_t* added) {
    struct stat st;
    if (!regular_file(path, &st) || st.st_size <= 0 || *added >= PROJECT_CONTEXT_TOTAL_CAP) {
        return AGENT_OK;
    }
    size_t wanted = (size_t)st.st_size;
    bool truncated = false;
    if (wanted > PROJECT_CONTEXT_FILE_CAP) {
        wanted = PROJECT_CONTEXT_FILE_CAP;
        truncated = true;
    }
    size_t remaining = PROJECT_CONTEXT_TOTAL_CAP - *added;
    if (wanted > remaining) {
        wanted = remaining;
        truncated = true;
    }
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return AGENT_OK;
    char* data = malloc(wanted + 1);
    if (data == NULL) {
        fclose(f);
        return AGENT_ERR_OOM;
    }
    size_t n = fread(data, 1, wanted, f);
    fclose(f);
    data[n] = '\0';
    int rc = string_printf(prompt, "\n%s (%s):\n", label, path);
    if (rc == AGENT_OK)
        rc = string_append_n(prompt, data, n);
    if (rc == AGENT_OK && (n == 0 || data[n - 1] != '\n'))
        rc = string_append_char(prompt, '\n');
    if (rc == AGENT_OK && truncated)
        rc = string_append(prompt, "[context truncated]\n");
    free(data);
    if (rc == AGENT_OK)
        *added += n;
    return rc;
}

static bool parent_dir(char* path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';
    char* slash = strrchr(path, '/');
    if (slash == NULL)
        return false;
    if (slash == path) {
        if (path[1] == '\0')
            return false;
        path[1] = '\0';
    } else {
        *slash = '\0';
    }
    return true;
}

int project_context_append(const char* cwd, String* prompt) {
    if (cwd == NULL || prompt == NULL)
        return AGENT_OK;
    char resolved[PATH_MAX];
    if (realpath(cwd, resolved) == NULL)
        return AGENT_OK;

    char dirs[PROJECT_CONTEXT_MAX_DEPTH][PATH_MAX];
    size_t count = 0;
    char current[PATH_MAX];
    snprintf(current, sizeof(current), "%s", resolved);
    bool found_git_root = false;
    while (count < PROJECT_CONTEXT_MAX_DEPTH) {
        snprintf(dirs[count++], PATH_MAX, "%s", current);
        char git_marker[PATH_MAX];
        if (snprintf(git_marker, sizeof(git_marker), "%s/.git", current) <
                (int)sizeof(git_marker) &&
            path_exists(git_marker)) {
            found_git_root = true;
            break;
        }
        if (!parent_dir(current))
            break;
    }
    if (!found_git_root)
        count = 1; /* a non-git workspace trusts cwd only */

    static const char* candidates[] = {
        "AGENTS.override.md", "AGENTS.md", "AGENTS.MD", "CLAUDE.md", "CLAUDE.MD",
    };
    size_t added = 0;
    for (size_t rev = count; rev > 0; rev--) {
        const char* dir = dirs[rev - 1];
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            char path[PATH_MAX];
            if (snprintf(path, sizeof(path), "%s/%s", dir, candidates[i]) >= (int)sizeof(path)) {
                continue;
            }
            struct stat st;
            if (regular_file(path, &st)) {
                int rc = append_file(path, "Repository instructions", prompt, &added);
                if (rc != AGENT_OK)
                    return rc;
                break; /* one instruction file per directory, in priority order */
            }
        }
    }

    /* PROGRESS.md is project memory rather than an instruction override. */
    char progress[PATH_MAX];
    const char* root = dirs[count - 1];
    if (snprintf(progress, sizeof(progress), "%s/PROGRESS.md", root) < (int)sizeof(progress)) {
        int rc =
            append_file(progress, "Project memory; verify against current files", prompt, &added);
        if (rc != AGENT_OK)
            return rc;
    }
    return AGENT_OK;
}
