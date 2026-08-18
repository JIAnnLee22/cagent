#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "util/project_context.h"

#define PROJECT_CONTEXT_FILE_CAP (64 * 1024)
#define PROJECT_CONTEXT_TOTAL_CAP (128 * 1024)
#define PROJECT_PROGRESS_CAP (4 * 1024)
#define PROJECT_CONTEXT_MAX_DEPTH 64

ProjectContextOptions project_context_options_default(void) {
    ProjectContextOptions options = {
        .total_cap = PROJECT_CONTEXT_TOTAL_CAP,
        .file_cap = PROJECT_CONTEXT_FILE_CAP,
        .progress_cap = PROJECT_PROGRESS_CAP,
        .include_progress = true,
    };
    return options;
}

static bool path_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool regular_file(const char* path, struct stat* st) {
    /* Instruction symlinks are ignored: following one could disclose an
     * arbitrary file outside the workspace to the model provider. */
    return lstat(path, st) == 0 && S_ISREG(st->st_mode);
}

static bool utf8_continuation(unsigned char c) {
    return (c & 0xc0) == 0x80;
}

static size_t utf8_complete_prefix(const unsigned char* data, size_t len) {
    if (data == NULL || len == 0) {
        return 0;
    }
    size_t lead = len - 1;
    while (lead > 0 && utf8_continuation(data[lead])) {
        lead--;
    }
    unsigned char c = data[lead];
    size_t width = 1;
    if ((c & 0xe0) == 0xc0) {
        width = 2;
    } else if ((c & 0xf0) == 0xe0) {
        width = 3;
    } else if ((c & 0xf8) == 0xf0) {
        width = 4;
    } else if (utf8_continuation(c)) {
        return lead;
    }
    return len - lead >= width ? len : lead;
}

static int append_file(const char* path, const char* label, String* prompt, size_t* added,
                       size_t total_cap, size_t file_cap, bool preserve_tail) {
    struct stat st;
    if (!regular_file(path, &st) || st.st_size <= 0 || *added >= total_cap || file_cap == 0) {
        return AGENT_OK;
    }

    size_t file_size = (size_t)st.st_size;
    size_t wanted = file_size;
    bool truncated = false;
    if (wanted > file_cap) {
        wanted = file_cap;
        truncated = true;
    }
    size_t remaining = total_cap - *added;
    if (wanted > remaining) {
        wanted = remaining;
        truncated = true;
    }
    if (wanted == 0) {
        return AGENT_OK;
    }

    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return AGENT_OK;
    char* data = malloc(wanted + 1);
    if (data == NULL) {
        fclose(f);
        return AGENT_ERR_OOM;
    }

    size_t n = 0;
    bool marker_inserted = false;
    if (preserve_tail && truncated) {
        /* Progress files conventionally put the newest information at the
         * end. Keep both ends so a bounded excerpt remains useful after a
         * long-running project has accumulated history. */
        static const char marker[] = "\n[context truncated]\n";
        const size_t marker_len = sizeof(marker) - 1;
        if (wanted > marker_len + 2 && fseeko(f, 0, SEEK_SET) == 0) {
            size_t available = wanted - marker_len;
            size_t head = available / 2;
            size_t tail = available - head;
            size_t head_read = fread(data, 1, head, f);
            n = utf8_complete_prefix((const unsigned char*)data, head_read);
            if (head_read == head) {
                memcpy(data + n, marker, marker_len);
                n += marker_len;
                marker_inserted = true;
                if (fseeko(f, (off_t)(file_size - tail), SEEK_SET) == 0) {
                    size_t tail_start = n;
                    size_t got_tail = fread(data + tail_start, 1, tail, f);
                    size_t skip = 0;
                    while (skip < got_tail &&
                           utf8_continuation((unsigned char)data[tail_start + skip])) {
                        skip++;
                    }
                    size_t complete =
                        utf8_complete_prefix((const unsigned char*)data + tail_start + skip,
                                              got_tail - skip);
                    if (skip > 0 || complete < got_tail - skip) {
                        memmove(data + tail_start, data + tail_start + skip, complete);
                    }
                    n += complete;
                }
            }
        }
    }
    if (n == 0) {
        (void)fseeko(f, 0, SEEK_SET);
        n = fread(data, 1, wanted, f);
    }
    fclose(f);
    data[n] = '\0';

    int rc = string_printf(prompt, "\n%s (%s):\n", label, path);
    if (rc == AGENT_OK)
        rc = string_append_n(prompt, data, n);
    if (rc == AGENT_OK && (n == 0 || data[n - 1] != '\n'))
        rc = string_append_char(prompt, '\n');
    /* Keep the historical marker for head-only excerpts.  The progress
     * head/tail path already inserted it inside the excerpt. */
    if (rc == AGENT_OK && truncated && !marker_inserted)
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

int project_context_append_with_options(const char* cwd, String* prompt,
                                        const ProjectContextOptions* options) {
    if (cwd == NULL || prompt == NULL)
        return AGENT_OK;

    ProjectContextOptions defaults = project_context_options_default();
    const ProjectContextOptions* opts = options != NULL ? options : &defaults;
    if (opts->total_cap == 0 || opts->file_cap == 0)
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
                int rc = append_file(path, "Repository instructions", prompt, &added,
                                      opts->total_cap, opts->file_cap, false);
                if (rc != AGENT_OK)
                    return rc;
                break; /* one instruction file per directory, in priority order */
            }
        }
    }

    /* PROGRESS.md is project memory rather than an instruction override. */
    char progress[PATH_MAX];
    const char* root = dirs[count - 1];
    if (opts->include_progress && opts->progress_cap > 0 &&
        snprintf(progress, sizeof(progress), "%s/PROGRESS.md", root) < (int)sizeof(progress)) {
        size_t progress_cap =
            opts->progress_cap < opts->file_cap ? opts->progress_cap : opts->file_cap;
        int rc = append_file(progress, "Project memory; verify against current files", prompt,
                             &added, opts->total_cap, progress_cap, true);
        if (rc != AGENT_OK)
            return rc;
    }
    return AGENT_OK;
}

int project_context_append(const char* cwd, String* prompt) {
    ProjectContextOptions options = project_context_options_default();
    return project_context_append_with_options(cwd, prompt, &options);
}
