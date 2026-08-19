/*
 * tool/git.c — structured, argv-only git helpers.
 *
 * These tools intentionally do not invoke a shell. Mutating operations carry
 * TOOL_FLAG_APPROVAL_REQUIRED and are therefore gated by the agent loop.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runtime/process.h"
#include "tool/tool.h"
#include "util/json.h"
#include "util/string.h"

#define GIT_TIMEOUT_MS 30000
#define GIT_PREVIEW_TIMEOUT_MS 3000
#define GIT_OUTPUT_CAP (128 * 1024)
#define GIT_PREVIEW_OUTPUT_CAP (16 * 1024)

static bool safe_pathspec(const char* path) {
    if (path == NULL || path[0] == '\0' || path[0] == '/') {
        return false;
    }
    const char* p = path;
    while (*p != '\0') {
        while (*p == '/')
            p++;
        const char* start = p;
        while (*p != '\0' && *p != '/')
            p++;
        size_t len = (size_t)(p - start);
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            return false;
        }
    }
    return true;
}

static int git_result(const char* cwd, char* const argv[], ToolResult* result) {
    ProcessResult process = {0};
    int rc = process_run(cwd, argv, GIT_TIMEOUT_MS, GIT_OUTPUT_CAP, &process);
    if (rc != AGENT_OK) {
        process_result_free(&process);
        result->content = strdup("error: failed to start git");
        result->is_error = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }

    String out = string_new();
    string_append_n(&out, process.out.data, process.out.len);
    if (process.timed_out) {
        string_append(&out, "\ngit timed out after 30s");
    } else {
        string_printf(&out, "\nexit code: %d", process.exit_code);
    }
    if (process.output_capped) {
        string_append(&out, "\n...[output truncated]");
    }
    result->content = string_take(&out);
    result->is_error = process.timed_out || process.exit_code != 0;
    process_result_free(&process);
    return AGENT_OK;
}

static int git_preview_result(const char* cwd, char* const argv[], ToolResult* result) {
    ProcessResult process = {0};
    int rc = process_run(cwd, argv, GIT_PREVIEW_TIMEOUT_MS, GIT_PREVIEW_OUTPUT_CAP, &process);
    if (rc != AGENT_OK) {
        process_result_free(&process);
        result->content = strdup("preview unavailable: failed to start git");
        result->is_error = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    String out = string_new();
    string_append_n(&out, process.out.data, process.out.len);
    if (process.timed_out)
        string_append(&out, "\n[git preview timed out after 3s]");
    if (process.output_capped)
        string_append(&out, "\n...[preview truncated]");
    result->content = string_take(&out);
    result->is_error = process.timed_out || process.exit_code != 0;
    process_result_free(&process);
    return AGENT_OK;
}

static int parse_args(const char* arguments, JsonDoc** doc_out, JsonVal** root_out,
                      ToolResult* result) {
    *doc_out =
        json_parse(arguments != NULL ? arguments : "{}", arguments != NULL ? strlen(arguments) : 2);
    if (*doc_out == NULL) {
        result->content = strdup("error: arguments are not valid JSON");
        result->is_error = true;
        return AGENT_ERR_TOOL;
    }
    *root_out = json_root(*doc_out);
    if (*root_out == NULL || !json_val_is_obj(*root_out)) {
        json_doc_free(*doc_out);
        *doc_out = NULL;
        result->content = strdup("error: arguments must be a JSON object");
        result->is_error = true;
        return AGENT_ERR_TOOL;
    }
    return AGENT_OK;
}

static uint64_t checkpoint_hash(const char* cwd) {
    uint64_t hash = 1469598103934665603ULL;
    const unsigned char* p = (const unsigned char*)(cwd != NULL ? cwd : ".");
    while (*p != '\0') {
        hash ^= *p++;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static int checkpoint_path(const ToolContext* ctx, char* out, size_t cap) {
    const char* home = getenv("HOME");
    if (home == NULL || home[0] == '\0')
        home = "/tmp";
    const char* cwd = ctx != NULL && ctx->cwd != NULL ? ctx->cwd : ".";
    if (snprintf(out, cap, "%s/.local/state/cagent/checkpoints/%016llx.json", home,
                 (unsigned long long)checkpoint_hash(cwd)) >= (int)cap)
        return AGENT_ERR_IO;
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s/.local", home) >= (int)sizeof(dir) ||
        (mkdir(dir, 0700) != 0 && errno != EEXIST))
        return AGENT_ERR_IO;
    if (snprintf(dir, sizeof(dir), "%s/.local/state", home) >= (int)sizeof(dir) ||
        (mkdir(dir, 0700) != 0 && errno != EEXIST))
        return AGENT_ERR_IO;
    if (snprintf(dir, sizeof(dir), "%s/.local/state/cagent", home) >= (int)sizeof(dir) ||
        (mkdir(dir, 0700) != 0 && errno != EEXIST))
        return AGENT_ERR_IO;
    if (snprintf(dir, sizeof(dir), "%s/.local/state/cagent/checkpoints", home) >=
            (int)sizeof(dir) ||
        (mkdir(dir, 0700) != 0 && errno != EEXIST))
        return AGENT_ERR_IO;
    return AGENT_OK;
}

bool git_checkpoint_available(const char* cwd) {
    const char* home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        home = "/tmp";
    }
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/.local/state/cagent/checkpoints/%016llx.json", home,
                 (unsigned long long)checkpoint_hash(cwd != NULL ? cwd : ".")) >= (int)sizeof(path)) {
        return false;
    }
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

static bool valid_commit_hash(const char* hash) {
    if (hash == NULL || strlen(hash) != 40)
        return false;
    for (size_t i = 0; i < 40; i++) {
        if (!isxdigit((unsigned char)hash[i]))
            return false;
    }
    return true;
}

static int git_checkpoint_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    (void)arguments;
    result->content = NULL;
    result->is_error = false;
    char* status_argv[] = {(char*)"/usr/bin/git", (char*)"status", (char*)"--porcelain", NULL};
    ProcessResult status = {0};
    int rc = process_run(ctx != NULL ? ctx->cwd : NULL, status_argv, GIT_TIMEOUT_MS, GIT_OUTPUT_CAP,
                         &status);
    if (rc != AGENT_OK || status.exit_code != 0) {
        process_result_free(&status);
        result->content = strdup("error: cannot inspect git working tree");
        result->is_error = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    if (status.out.len != 0) {
        process_result_free(&status);
        result->content = strdup("error: checkpoint requires a clean working tree");
        result->is_error = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    process_result_free(&status);
    char* head_argv[] = {(char*)"/usr/bin/git", (char*)"rev-parse", (char*)"HEAD", NULL};
    ProcessResult head = {0};
    rc = process_run(ctx != NULL ? ctx->cwd : NULL, head_argv, GIT_TIMEOUT_MS, GIT_OUTPUT_CAP,
                     &head);
    while (rc == AGENT_OK && head.out.len > 0 &&
           (head.out.data[head.out.len - 1] == '\n' || head.out.data[head.out.len - 1] == '\r')) {
        head.out.data[--head.out.len] = '\0';
    }
    if (rc != AGENT_OK || head.exit_code != 0 || !valid_commit_hash(head.out.data)) {
        process_result_free(&head);
        result->content = strdup("error: cannot resolve git HEAD");
        result->is_error = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    char path[PATH_MAX];
    rc = checkpoint_path(ctx, path, sizeof(path));
    if (rc == AGENT_OK) {
        char temp[PATH_MAX];
        if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(temp)) {
            rc = AGENT_ERR_IO;
        } else {
            int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0)
                rc = AGENT_ERR_IO;
            else {
                FILE* f = fdopen(fd, "w");
                if (f == NULL) {
                    close(fd);
                    rc = AGENT_ERR_IO;
                } else {
                    bool wrote = fprintf(f, "%s\n", head.out.data) > 0;
                    bool closed = fclose(f) == 0;
                    if (wrote && closed && rename(temp, path) == 0)
                        rc = AGENT_OK;
                    else {
                        unlink(temp);
                        rc = AGENT_ERR_IO;
                    }
                }
            }
        }
    }
    result->content = strdup(rc == AGENT_OK ? "git checkpoint saved (clean HEAD)"
                                            : "error: cannot save git checkpoint");
    result->is_error = rc != AGENT_OK;
    process_result_free(&head);
    return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
}

static int git_restore_checkpoint_execute(ToolContext* ctx, const char* arguments,
                                          ToolResult* result) {
    (void)arguments;
    result->content = NULL;
    result->is_error = false;
    char path[PATH_MAX];
    int rc = checkpoint_path(ctx, path, sizeof(path));
    char head[64] = {0};
    if (rc == AGENT_OK) {
        FILE* f = fopen(path, "r");
        if (f == NULL || fgets(head, sizeof(head), f) == NULL)
            rc = AGENT_ERR_IO;
        if (f != NULL)
            fclose(f);
    }
    size_t len = strlen(head);
    while (len > 0 && (head[len - 1] == '\n' || head[len - 1] == '\r'))
        head[--len] = '\0';
    if (rc != AGENT_OK || !valid_commit_hash(head)) {
        result->content = strdup("error: no valid clean-worktree checkpoint exists");
        result->is_error = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    char* current_argv[] = {(char*)"/usr/bin/git", (char*)"rev-parse", (char*)"HEAD", NULL};
    ProcessResult current = {0};
    rc = process_run(ctx != NULL ? ctx->cwd : NULL, current_argv, GIT_TIMEOUT_MS,
                     GIT_OUTPUT_CAP, &current);
    while (rc == AGENT_OK && current.out.len > 0 &&
           (current.out.data[current.out.len - 1] == '\n' ||
            current.out.data[current.out.len - 1] == '\r')) {
        current.out.data[--current.out.len] = '\0';
    }
    bool same_head = rc == AGENT_OK && current.exit_code == 0 &&
                     strcmp(current.out.data != NULL ? current.out.data : "", head) == 0;
    process_result_free(&current);
    if (!same_head) {
        result->content = strdup("error: checkpoint HEAD differs from current HEAD; refusing reset");
        result->is_error = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    char* argv[] = {(char*)"/usr/bin/git", (char*)"reset", (char*)"--hard", head, NULL};
    rc = git_result(ctx != NULL ? ctx->cwd : NULL, argv, result);
    if (rc == AGENT_OK && !result->is_error) {
        String out = string_new();
        string_append_n(&out, result->content, strlen(result->content));
        string_append(&out, "\ntracked files restored; untracked files were left untouched");
        free(result->content);
        result->content = string_take(&out);
    }
    return rc;
}

static int git_status_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    (void)arguments;
    result->content = NULL;
    result->is_error = false;
    char* argv[] = {(char*)"/usr/bin/git", (char*)"status", (char*)"--short", (char*)"--branch",
                    NULL};
    ProcessResult process = {0};
    int rc =
        process_run(ctx != NULL ? ctx->cwd : NULL, argv, GIT_TIMEOUT_MS, GIT_OUTPUT_CAP, &process);
    if (rc != AGENT_OK) {
        result->content = strdup("error: failed to start git status");
        result->is_error = true;
        process_result_free(&process);
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    int changed = 0;
    const char* p = process.out.data;
    while (p != NULL && *p != '\0') {
        const char* end = strchr(p, '\n');
        if (p[0] != '\0' && p[0] != '#')
            changed++;
        p = end != NULL ? end + 1 : NULL;
    }
    String out = string_new();
    string_printf(&out, "git status: %s changed=%d exit=%d\n", changed == 0 ? "clean" : "dirty",
                  changed, process.exit_code);
    string_append_n(&out, process.out.data, process.out.len);
    result->content = string_take(&out);
    result->is_error = process.timed_out || process.exit_code != 0;
    process_result_free(&process);
    return AGENT_OK;
}

static int git_diff_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    JsonDoc* doc = NULL;
    JsonVal* root = NULL;
    if (parse_args(arguments, &doc, &root, result) != AGENT_OK) {
        return AGENT_OK;
    }
    const char* path = json_obj_get_str(root, "path");
    if (path != NULL && !safe_pathspec(path)) {
        json_doc_free(doc);
        result->content = strdup("error: path must stay inside the repository");
        result->is_error = true;
        return AGENT_OK;
    }
    char* argv[] = {(char*)"/usr/bin/git",
                    (char*)"diff",
                    (char*)"--no-ext-diff",
                    (char*)"--stat",
                    (char*)"--name-status",
                    (char*)"--",
                    (char*)path,
                    NULL};
    if (path == NULL) {
        argv[6] = NULL;
    }
    int rc = git_result(ctx != NULL ? ctx->cwd : NULL, argv, result);
    json_doc_free(doc);
    return rc;
}

static int git_commit_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    JsonDoc* doc = NULL;
    JsonVal* root = NULL;
    if (parse_args(arguments, &doc, &root, result) != AGENT_OK) {
        return AGENT_OK;
    }
    const char* message = json_obj_get_str(root, "message");
    if (message == NULL || message[0] == '\0') {
        json_doc_free(doc);
        result->content = strdup("error: missing commit message");
        result->is_error = true;
        return AGENT_OK;
    }
    char* argv[] = {(char*)"/usr/bin/git", (char*)"commit", (char*)"-m", (char*)message, NULL};
    int rc = git_result(ctx != NULL ? ctx->cwd : NULL, argv, result);
    json_doc_free(doc);
    return rc;
}

static bool valid_target(const char* target) {
    if (target == NULL || target[0] == '\0') {
        return false;
    }
    if (strcmp(target, "HEAD") == 0) {
        return true;
    }
    if (strncmp(target, "HEAD~", 5) == 0 && target[5] != '\0') {
        for (const char* p = target + 5; *p != '\0'; p++) {
            if (!isdigit((unsigned char)*p))
                return false;
        }
        return true;
    }
    size_t len = strlen(target);
    if (len < 7 || len > 40) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)target[i]))
            return false;
    }
    return true;
}

static int git_restore_checkpoint_preview(ToolContext* ctx, const char* arguments,
                                          ToolResult* result) {
    (void)arguments;
    result->content = NULL;
    result->is_error = false;
    char path[PATH_MAX];
    char head[64] = {0};
    int rc = checkpoint_path(ctx, path, sizeof(path));
    if (rc == AGENT_OK) {
        FILE* f = fopen(path, "r");
        if (f == NULL || fgets(head, sizeof(head), f) == NULL) {
            rc = AGENT_ERR_IO;
        }
        if (f != NULL)
            fclose(f);
    }
    size_t len = strlen(head);
    while (len > 0 && (head[len - 1] == '\n' || head[len - 1] == '\r'))
        head[--len] = '\0';
    if (rc != AGENT_OK || !valid_commit_hash(head)) {
        result->content = strdup("error: no valid clean-worktree checkpoint exists");
        result->is_error = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }

    char* argv[] = {(char*)"/usr/bin/git", (char*)"status", (char*)"--short", NULL};
    ProcessResult status = {0};
    rc = process_run(ctx != NULL ? ctx->cwd : NULL, argv, GIT_PREVIEW_TIMEOUT_MS,
                     GIT_PREVIEW_OUTPUT_CAP, &status);
    if (rc != AGENT_OK || status.exit_code != 0) {
        process_result_free(&status);
        result->content = strdup("error: cannot inspect working tree before reset");
        result->is_error = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    String out = string_new();
    string_printf(&out, "git reset --hard %s\nCurrent working tree (will be discarded):\n",
                  head);
    if (status.out.len == 0)
        string_append(&out, "(clean)\n");
    else
        string_append_n(&out, status.out.data, status.out.len);
    string_append(&out, "Tracked changes will be discarded; untracked files remain.\n");
    result->content = string_take(&out);
    process_result_free(&status);
    return AGENT_OK;
}

static int git_commit_preview(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    JsonDoc* doc = NULL;
    JsonVal* root = NULL;
    if (parse_args(arguments, &doc, &root, result) != AGENT_OK)
        return AGENT_OK;
    const char* message = json_obj_get_str(root, "message");
    if (message == NULL || message[0] == '\0') {
        json_doc_free(doc);
        result->content = strdup("error: missing commit message");
        result->is_error = true;
        return AGENT_OK;
    }
    char* argv[] = {(char*)"/usr/bin/git", (char*)"diff",          (char*)"--cached",
                    (char*)"--stat",       (char*)"--name-status", NULL};
    ToolResult diff = {0};
    int rc = git_preview_result(ctx != NULL ? ctx->cwd : NULL, argv, &diff);
    String out = string_new();
    string_printf(&out, "git commit message: %s\n\nstaged changes:\n", message);
    if (diff.content != NULL)
        string_append(&out, diff.content);
    free(diff.content);
    json_doc_free(doc);
    if (rc != AGENT_OK) {
        string_free(&out);
        return rc;
    }
    result->content = string_take(&out);
    result->is_error = diff.is_error;
    return AGENT_OK;
}

static int git_revert_preview(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    JsonDoc* doc = NULL;
    JsonVal* root = NULL;
    if (parse_args(arguments, &doc, &root, result) != AGENT_OK)
        return AGENT_OK;
    const char* target = json_obj_get_str(root, "target");
    if (!valid_target(target)) {
        json_doc_free(doc);
        result->content = strdup("error: invalid revert target");
        result->is_error = true;
        return AGENT_OK;
    }
    char* argv[] = {(char*)"/usr/bin/git", (char*)"show", (char*)"-s",
                    (char*)"--oneline",    (char*)target, NULL};
    int rc = git_preview_result(ctx != NULL ? ctx->cwd : NULL, argv, result);
    json_doc_free(doc);
    return rc;
}

static int git_revert_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    JsonDoc* doc = NULL;
    JsonVal* root = NULL;
    if (parse_args(arguments, &doc, &root, result) != AGENT_OK) {
        return AGENT_OK;
    }
    const char* target = json_obj_get_str(root, "target");
    if (!valid_target(target)) {
        json_doc_free(doc);
        result->content = strdup("error: target must be HEAD, HEAD~N, or a 7-40 digit commit id");
        result->is_error = true;
        return AGENT_OK;
    }
    char* argv[] = {(char*)"/usr/bin/git", (char*)"revert", (char*)"--no-edit", (char*)target,
                    NULL};
    int rc = git_result(ctx != NULL ? ctx->cwd : NULL, argv, result);
    json_doc_free(doc);
    return rc;
}

Tool git_checkpoint_tool = {
    /* NOLINT(misc-use-internal-linkage) */
    .name = "git_checkpoint",
    .description = "Save a clean HEAD checkpoint outside the repository for protected recovery.",
    .input_schema = "{\"type\":\"object\"}",
    .flags = TOOL_FLAG_PARALLEL_SAFE,
    .execute = git_checkpoint_execute,
};

Tool git_restore_checkpoint_tool = {
    /* NOLINT(misc-use-internal-linkage) */
    .name = "git_restore_checkpoint",
    .description =
        "Restore tracked files to the saved clean checkpoint; untracked files remain untouched.",
    .input_schema = "{\"type\":\"object\"}",
    .flags = TOOL_FLAG_APPROVAL_REQUIRED,
    .preview = git_restore_checkpoint_preview,
    .execute = git_restore_checkpoint_execute,
};

Tool git_status_tool = {
    /* NOLINT(misc-use-internal-linkage) */
    .name = "git_status",
    .description = "Report branch and clean/dirty working-tree state for baseline checks.",
    .input_schema = "{\"type\":\"object\"}",
    .flags = TOOL_FLAG_PARALLEL_SAFE,
    .execute = git_status_execute,
};

Tool git_diff_tool = {
    /* NOLINT(misc-use-internal-linkage) */
    .name = "git_diff",
    .description = "Show a structured summary of tracked working-tree changes.",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}",
    .flags = TOOL_FLAG_PARALLEL_SAFE,
    .execute = git_diff_execute,
};

Tool git_commit_tool = {
    /* NOLINT(misc-use-internal-linkage) */
    .name = "git_commit",
    .description = "Create a commit from already staged changes.",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\"}},"
                    "\"required\":[\"message\"]}",
    .flags = TOOL_FLAG_APPROVAL_REQUIRED,
    .preview = git_commit_preview,
    .execute = git_commit_execute,
};

Tool git_revert_tool = {
    /* NOLINT(misc-use-internal-linkage) */
    .name = "git_revert",
    .description = "Revert one validated commit target with --no-edit.",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\"}},"
                    "\"required\":[\"target\"]}",
    .flags = TOOL_FLAG_APPROVAL_REQUIRED,
    .preview = git_revert_preview,
    .execute = git_revert_execute,
};
