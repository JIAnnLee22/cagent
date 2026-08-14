/*
 * tool/bash.c — bash tool: run a shell command, capture stdout/stderr.
 *
 * Arguments (JSON): { "command": string, "timeout": int (1..300, default 30) }
 *
 * Implementation (no system()): process_run() -> posix_spawn("/bin/sh",
 * -c) in the tool's working directory, running in its own process group
 * so the timeout can kill the whole tree (SIGTERM -> 2s -> SIGKILL).
 * Output capped at 64 KiB.
 *
 * Ownership: result.content is malloc'ed by this tool; the caller frees.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/process.h"
#include "runtime/runtime.h"
#include "tool/tool.h"
#include "util/json.h"
#include "util/string.h"

#define BASH_OUTPUT_CAP (64 * 1024)
#define BASH_DEFAULT_TIMEOUT 30
#define BASH_MAX_TIMEOUT 300

typedef struct {
    ToolTask base;
    ProcessTask* process;
    int64_t timeout_s;
} BashTask;

static int bash_preview(ToolContext* ctx, const char* arguments, ToolResult* result) {
    (void)ctx;
    result->content = NULL;
    result->is_error = false;
    JsonDoc* doc = json_parse(arguments, strlen(arguments));
    if (doc == NULL) {
        result->content = strdup("error: arguments are not valid JSON");
        result->is_error = true;
        return AGENT_OK;
    }
    JsonVal* root = json_root(doc);
    const char* command = json_obj_get_str(root, "command");
    if (command == NULL || command[0] == '\0') {
        result->content = strdup("error: missing required argument \"command\"");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    int64_t timeout = json_obj_get_int(root, "timeout", BASH_DEFAULT_TIMEOUT);
    if (timeout < 1)
        timeout = 1;
    if (timeout > BASH_MAX_TIMEOUT)
        timeout = BASH_MAX_TIMEOUT;
    String out = string_new();
    string_printf(&out, "shell command (cwd: %s, timeout: %llds):\n$ ",
                  ctx != NULL && ctx->cwd != NULL ? ctx->cwd : ".", (long long)timeout);
    size_t command_len = strlen(command);
    size_t shown = command_len > 4096 ? 4096 : command_len;
    string_append_n(&out, command, shown);
    if (shown < command_len)
        string_append(&out, "\n...[command preview truncated]");
    const char* risky[] = {"rm -rf",           "sudo ",  "curl |",
                           "curl -",           "wget ",  "git push --force",
                           "git reset --hard", "dd if=", "> /dev/"};
    bool warned = false;
    for (size_t i = 0; i < sizeof(risky) / sizeof(risky[0]); i++) {
        if (strcasestr(command, risky[i]) != NULL) {
            if (!warned)
                string_append(&out, "\n\nwarning: command matches risky patterns:");
            string_printf(&out, "\n- %s", risky[i]);
            warned = true;
        }
    }
    if (warned)
        string_append(&out, "\nHeuristic warning only; review the complete command.");
    json_doc_free(doc);
    result->content = string_take(&out);
    return AGENT_OK;
}

static void build_bash_result(ProcessResult* pr, int64_t timeout, ToolResult* result) {
    String out = string_new();
    string_append_n(&out, pr->out.data, pr->out.len);
    if (pr->timed_out) {
        string_append(&out, "\n[command timed out after ");
        string_printf(&out, "%lld", (long long)timeout);
        string_append(&out, "s and was killed]\n");
    } else {
        string_append(&out, "\n");
        if (pr->exit_code < 0) {
            string_append(&out, "exit code: unknown");
        } else {
            string_printf(&out, "exit code: %d", pr->exit_code);
        }
    }
    if (pr->output_capped) {
        string_append(&out, "\n...[output truncated at 64 KiB]");
    }
    result->content = string_take(&out);
}

static int bash_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;

    JsonDoc* doc = json_parse(arguments, strlen(arguments));
    if (doc == NULL) {
        result->content = strdup("error: arguments are not valid JSON");
        result->is_error = true;
        return AGENT_OK;
    }
    JsonVal* root = json_root(doc);
    const char* command = json_obj_get_str(root, "command");
    if (command == NULL || command[0] == '\0') {
        result->content = strdup("error: missing required argument \"command\"");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    int64_t timeout = json_obj_get_int(root, "timeout", BASH_DEFAULT_TIMEOUT);
    if (timeout < 1) {
        timeout = 1;
    }
    if (timeout > BASH_MAX_TIMEOUT) {
        timeout = BASH_MAX_TIMEOUT;
    }
    /* command is borrowed from the document; keep it alive */

    char* argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)command, NULL};
    ProcessResult pr = {0};
    int rc = process_run(ctx->cwd, argv, timeout * 1000, BASH_OUTPUT_CAP, &pr);
    if (rc != AGENT_OK) {
        json_doc_free(doc);
        process_result_free(&pr);
        result->content = strdup("error: failed to run command");
        result->is_error = true;
        return AGENT_OK;
    }

    build_bash_result(&pr, timeout, result);
    process_result_free(&pr);
    json_doc_free(doc);
    return AGENT_OK;
}

static int bash_task_poll(ToolTask* base, ToolResult* result, bool* done) {
    BashTask* task = (BashTask*)base;
    ProcessResult pr = {0};
    int rc = process_poll(task->process, &pr, done);
    if (!*done) {
        return rc;
    }
    if (rc != AGENT_OK) {
        process_result_free(&pr);
        result->content = strdup("error: failed while running command");
        result->is_error = true;
        return AGENT_OK;
    }
    build_bash_result(&pr, task->timeout_s, result);
    process_result_free(&pr);
    return AGENT_OK;
}

static void bash_task_cancel(ToolTask* base) {
    BashTask* task = (BashTask*)base;
    process_cancel(task->process);
}

static void bash_task_destroy(ToolTask* base) {
    BashTask* task = (BashTask*)base;
    process_task_free(task->process);
    free(task);
}

static int bash_start(ToolContext* ctx, const char* arguments, ToolResult* result,
                      ToolTask** task_out) {
    *task_out = NULL;
    result->content = NULL;
    result->is_error = false;

    /* Direct/unit-test contexts do not own an EventLoop; preserve the
     * synchronous public entry point for those callers. */
    if (ctx == NULL || ctx->runtime == NULL || ctx->runtime->loop == NULL) {
        return bash_execute(ctx, arguments, result);
    }

    JsonDoc* doc = json_parse(arguments, strlen(arguments));
    if (doc == NULL) {
        result->content = strdup("error: arguments are not valid JSON");
        result->is_error = true;
        return AGENT_OK;
    }
    JsonVal* root = json_root(doc);
    const char* command = json_obj_get_str(root, "command");
    if (command == NULL || command[0] == '\0') {
        result->content = strdup("error: missing required argument \"command\"");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    int64_t timeout = json_obj_get_int(root, "timeout", BASH_DEFAULT_TIMEOUT);
    if (timeout < 1) {
        timeout = 1;
    }
    if (timeout > BASH_MAX_TIMEOUT) {
        timeout = BASH_MAX_TIMEOUT;
    }

    BashTask* task = calloc(1, sizeof(BashTask));
    if (task == NULL) {
        json_doc_free(doc);
        return AGENT_ERR_OOM;
    }
    task->base.poll = bash_task_poll;
    task->base.cancel = bash_task_cancel;
    task->base.destroy = bash_task_destroy;
    task->timeout_s = timeout;

    char* argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)command, NULL};
    int rc = process_start(ctx->runtime->loop, ctx->cwd, argv, timeout * 1000, BASH_OUTPUT_CAP,
                           &task->process);
    json_doc_free(doc);
    if (rc != AGENT_OK) {
        free(task);
        result->content = strdup("error: failed to run command");
        result->is_error = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }

    *task_out = &task->base;
    return AGENT_OK;
}

Tool bash_tool = {
    .name = "bash",
    .description = "Execute a shell command (sh -c) and capture stdout, "
                   "stderr and the exit code. Long-running commands hit "
                   "the timeout and are killed.",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"command\":"
                    "{\"type\":\"string\",\"description\":\"Shell command "
                    "to execute\"},\"timeout\":{\"type\":\"integer\","
                    "\"minimum\":1,\"maximum\":300,\"description\":\"Timeout "
                    "in seconds\"}},\"required\":[\"command\"]}",
    .flags = TOOL_FLAG_APPROVAL_REQUIRED,
    .preview = bash_preview,
    .execute = bash_execute,
    .start = bash_start,
};
