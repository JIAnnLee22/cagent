/*
 * tool/grep.c — grep tool: code search via ripgrep (fallback: grep).
 *
 * Arguments (JSON): { "pattern": string, "path": string (optional,
 *                     default "."), "max_results": int (1..500,
 *                     default 50) }
 *
 * Behavior (DESIGN.md §22): ripgrep is preferred; when unavailable the
 * tool falls back to plain grep -rn. Neither available -> a clear tool
 * error. The pattern is passed as an argv element (never through a
 * shell), so there is no injection risk.
 *
 * Ownership: result.content is malloc'ed by this tool; the caller frees.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runtime/process.h"
#include "runtime/runtime.h"
#include "tool/path_policy.h"
#include "tool/tool.h"
#include "util/json.h"
#include "util/string.h"

#define GREP_OUTPUT_CAP (64 * 1024)
#define GREP_DEFAULT_MAX 50
#define GREP_MAX_RESULTS 500

/* locate an executable in PATH; returns a malloc'ed path or NULL */
static char* find_in_path(const char* name) {
    const char* path_env = getenv("PATH");
    if (path_env == NULL) {
        return NULL;
    }
    size_t name_len = strlen(name);

    const char* p = path_env;
    for (;;) {
        const char* colon = strchr(p, ':');
        size_t dir_len = colon != NULL ? (size_t)(colon - p) : strlen(p);
        if (dir_len > 0) {
            char* candidate = malloc(dir_len + 1 + name_len + 1);
            if (candidate == NULL) {
                return NULL;
            }
            memcpy(candidate, p, dir_len);
            candidate[dir_len] = '/';
            memcpy(candidate + dir_len + 1, name, name_len + 1);
            if (access(candidate, X_OK) == 0) {
                return candidate;
            }
            free(candidate);
        }
        if (colon == NULL) {
            break;
        }
        p = colon + 1;
    }
    return NULL;
}

static void build_result(ProcessResult* pr, const char* pattern, ToolResult* result) {
    (void)pr;
    String out = string_new();
    if (pr->out.len == 0) {
        string_printf(&out, "(no matches for %s in scope)", pattern);
    } else {
        string_append_n(&out, pr->out.data, pr->out.len);
    }
    if (pr->timed_out) {
        string_append(&out, "\n[search timed out]");
    }
    if (pr->output_capped) {
        string_append(&out, "\n...[results truncated]");
    }
    string_append(&out, "\n");
    result->content = string_take(&out);
}

static int grep_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;

    JsonDoc* doc = json_parse(arguments, strlen(arguments));
    if (doc == NULL) {
        result->content = strdup("error: arguments are not valid JSON");
        result->is_error = true;
        return AGENT_OK;
    }
    JsonVal* root = json_root(doc);
    const char* pattern = json_obj_get_str(root, "pattern");
    if (pattern == NULL || pattern[0] == '\0') {
        result->content = strdup("error: missing required argument \"pattern\"");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    const char* path = json_obj_get_str(root, "path");
    if (path == NULL || path[0] == '\0') {
        path = ".";
    }
    char safe_path[PATH_MAX];
    if (tool_path_resolve(ctx, path, false, safe_path, sizeof(safe_path)) != AGENT_OK) {
        result->content = strdup("error: path must stay inside the workspace");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    int64_t max_results = json_obj_get_int(root, "max_results", GREP_DEFAULT_MAX);
    if (max_results < 1) {
        max_results = 1;
    }
    if (max_results > GREP_MAX_RESULTS) {
        max_results = GREP_MAX_RESULTS;
    }
    /* pattern/path are borrowed from the document; keep it alive */

    char max_str[32];
    snprintf(max_str, sizeof(max_str), "%lld", (long long)max_results);

    char* rg = find_in_path("rg");
    ProcessResult pr = {0};
    int rc;

    if (rg != NULL) {
        char* argv[] = {rg, "--no-heading", "-n", "-m", max_str, (char*)pattern, safe_path, NULL};
        rc = process_run(ctx->cwd, argv, 30000, GREP_OUTPUT_CAP, &pr);
        if (rc == AGENT_OK && !pr.timed_out && (pr.exit_code == 0 || pr.exit_code == 1)) {
            /* rg exit 1 = no matches, still a valid result */
            build_result(&pr, pattern, result);
            process_result_free(&pr);
            free(rg);
            json_doc_free(doc);
            return AGENT_OK;
        }
        process_result_free(&pr);
        free(rg);
        /* fall through to grep */
    }

    char* grep_path = find_in_path("grep");
    if (grep_path == NULL) {
        json_doc_free(doc);
        result->content = strdup("error: neither ripgrep (rg) nor grep is "
                                 "available on PATH");
        result->is_error = true;
        return AGENT_OK;
    }
    /* grep -rn <pattern> <path>; -m limits matches per file */
    char* argv[] = {grep_path, "-rn", "-m", max_str, (char*)pattern, safe_path, NULL};
    rc = process_run(ctx->cwd, argv, 30000, GREP_OUTPUT_CAP, &pr);
    free(grep_path);
    if (rc != AGENT_OK) {
        json_doc_free(doc);
        process_result_free(&pr);
        result->content = strdup("error: failed to run grep");
        result->is_error = true;
        return AGENT_OK;
    }
    build_result(&pr, pattern, result);
    process_result_free(&pr);

    json_doc_free(doc);
    return AGENT_OK;
}

typedef struct {
    ToolTask base;
    EventLoop* loop; /* borrowed */
    ProcessTask* process;
    char* cwd;
    char* pattern;
    char* path;
    char max_results[32];
    bool running_rg;
    bool cancelled;
} GrepTask;

static int grep_task_start_process(GrepTask* task, bool use_rg) {
    char* executable = find_in_path(use_rg ? "rg" : "grep");
    if (executable == NULL) {
        return AGENT_ERR_PROCESS;
    }

    int rc;
    if (use_rg) {
        char* argv[] = {executable, "--no-heading", "-n", "-m", task->max_results,
                        task->pattern, task->path, NULL};
        rc = process_start(task->loop, task->cwd, argv, 30000, GREP_OUTPUT_CAP, &task->process);
    } else {
        char* argv[] = {executable, "-rn", "-m", task->max_results, task->pattern, task->path,
                        NULL};
        rc = process_start(task->loop, task->cwd, argv, 30000, GREP_OUTPUT_CAP, &task->process);
    }
    free(executable);
    task->running_rg = use_rg;
    return rc;
}

static int grep_task_poll(ToolTask* base, ToolResult* result, bool* done) {
    GrepTask* task = (GrepTask*)base;
    ProcessResult pr = {0};
    bool process_done = false;
    int rc = process_poll(task->process, &pr, &process_done);
    if (!process_done && rc == AGENT_OK) {
        *done = false;
        return AGENT_OK;
    }

    bool successful = process_done && rc == AGENT_OK &&
                      (!task->running_rg ||
                       (!pr.timed_out && (pr.exit_code == 0 || pr.exit_code == 1)));
    bool was_rg = task->running_rg;
    if (task->cancelled) {
        if (process_done) {
            process_result_free(&pr);
        }
        process_task_free(task->process);
        task->process = NULL;
        result->content = strdup("search cancelled");
        result->is_error = true;
        *done = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }

    if (successful) {
        build_result(&pr, task->pattern, result);
        process_result_free(&pr);
        process_task_free(task->process);
        task->process = NULL;
        *done = true;
        return AGENT_OK;
    }

    if (process_done) {
        process_result_free(&pr);
    }
    process_task_free(task->process);
    task->process = NULL;

    if (was_rg) {
        rc = grep_task_start_process(task, false);
        if (rc == AGENT_OK) {
            *done = false;
            return AGENT_OK;
        }
        result->content = strdup("error: neither ripgrep (rg) nor grep is available on PATH");
        result->is_error = true;
        *done = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }

    result->content = strdup("error: failed to run grep");
    result->is_error = true;
    *done = true;
    return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
}

static void grep_task_cancel(ToolTask* base) {
    GrepTask* task = (GrepTask*)base;
    task->cancelled = true;
    process_cancel(task->process);
}

static void grep_task_destroy(ToolTask* base) {
    GrepTask* task = (GrepTask*)base;
    process_task_free(task->process);
    free(task->cwd);
    free(task->pattern);
    free(task->path);
    free(task);
}

static int grep_start(ToolContext* ctx, const char* arguments, ToolResult* result,
                      ToolTask** task_out) {
    *task_out = NULL;
    result->content = NULL;
    result->is_error = false;
    if (ctx == NULL || ctx->runtime == NULL || ctx->runtime->loop == NULL) {
        return grep_execute(ctx, arguments, result);
    }

    JsonDoc* doc = json_parse(arguments, strlen(arguments));
    if (doc == NULL) {
        result->content = strdup("error: arguments are not valid JSON");
        result->is_error = true;
        return AGENT_OK;
    }
    JsonVal* root = json_root(doc);
    const char* pattern = json_obj_get_str(root, "pattern");
    if (pattern == NULL || pattern[0] == '\0') {
        result->content = strdup("error: missing required argument \"pattern\"");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    const char* path = json_obj_get_str(root, "path");
    if (path == NULL || path[0] == '\0') {
        path = ".";
    }
    char safe_path[PATH_MAX];
    if (tool_path_resolve(ctx, path, false, safe_path, sizeof(safe_path)) != AGENT_OK) {
        result->content = strdup("error: path must stay inside the workspace");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    int64_t max_results = json_obj_get_int(root, "max_results", GREP_DEFAULT_MAX);
    if (max_results < 1) {
        max_results = 1;
    }
    if (max_results > GREP_MAX_RESULTS) {
        max_results = GREP_MAX_RESULTS;
    }

    GrepTask* task = calloc(1, sizeof(GrepTask));
    if (task == NULL) {
        json_doc_free(doc);
        return AGENT_ERR_OOM;
    }
    task->base.poll = grep_task_poll;
    task->base.cancel = grep_task_cancel;
    task->base.destroy = grep_task_destroy;
    task->loop = ctx->runtime->loop;
    task->cwd = ctx->cwd != NULL ? strdup(ctx->cwd) : NULL;
    task->pattern = strdup(pattern);
    task->path = strdup(safe_path);
    snprintf(task->max_results, sizeof(task->max_results), "%lld", (long long)max_results);
    json_doc_free(doc);
    if (task->pattern == NULL || task->path == NULL || (ctx->cwd != NULL && task->cwd == NULL)) {
        grep_task_destroy(&task->base);
        return AGENT_ERR_OOM;
    }

    char* rg_probe = find_in_path("rg");
    bool have_rg = rg_probe != NULL;
    free(rg_probe);
    int rc = grep_task_start_process(task, have_rg);
    if (rc != AGENT_OK && have_rg) {
        rc = grep_task_start_process(task, false);
    }
    if (rc != AGENT_OK) {
        grep_task_destroy(&task->base);
        result->content = strdup("error: neither ripgrep (rg) nor grep is available on PATH");
        result->is_error = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    *task_out = &task->base;
    return AGENT_OK;
}

Tool grep_tool = {
    .name = "grep",
    .description = "Search files for a pattern (ripgrep preferred, grep "
                   "fallback). Returns path:line: content lines.",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"pattern\":"
                    "{\"type\":\"string\",\"description\":\"Regex or "
                    "literal pattern\"},\"path\":{\"type\":\"string\","
                    "\"description\":\"File or directory to search, "
                    "relative to the working directory\"},\"max_results\":"
                    "{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}},"
                    "\"required\":[\"pattern\"]}",
    .flags = TOOL_FLAG_PARALLEL_SAFE,
    .execute = grep_execute,
    .start = grep_start,
};
