/*
 * tool/subagent.c — subagent tool (DESIGN.md §23-25).
 *
 * Arguments (JSON):
 *   { "task": string }                                  — one child agent
 *   { "tasks": [ {"task": string, "role": string?}, ... ] } — parallel children
 *   optional per-task: "system_prompt", "model" (named model from the
 *   runtime's table), "timeout" (seconds, default 120)
 *
 * Children are lightweight in-process agents sharing the parent's runtime
 * (model, tools, HTTP engine, scheduler) — never threads or processes.
 * Parallel tasks are started together and driven by runtime_pump() so
 * their model requests overlap on the shared libcurl multi handle.
 *
 * Result: one block per child: "[<role>] <final answer>". Failures are
 * reported per child (is_error text) and the tool itself returns OK.
 *
 * Ownership: result.content is malloc'ed by this tool; the caller frees.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "agent/agent.h"
#include "runtime/runtime.h"
#include "tool/tool.h"
#include "util/json.h"
#include "util/string.h"
#include "util/vector.h"

#define SUBAGENT_DEFAULT_TIMEOUT_S 120

typedef struct {
    char* task;          /* owned */
    char* role;          /* owned; may be NULL */
    char* system_prompt; /* owned; may be NULL */
    char* model;         /* owned; may be NULL (inherit parent) */
    int64_t timeout_s;
} SubTask;

static void subtask_free(SubTask* t) {
    free(t->task);
    free(t->role);
    free(t->system_prompt);
    free(t->model);
}

/* Parse {task} or {tasks:[...]} into a list. */
static int parse_tasks(const char* arguments, Vector* out) {
    JsonDoc* doc = json_parse(arguments, strlen(arguments));
    if (doc == NULL) {
        return AGENT_ERR_JSON;
    }
    JsonVal* root = json_root(doc);
    if (root == NULL || !json_val_is_obj(root)) {
        json_doc_free(doc);
        return AGENT_ERR_JSON;
    }

    /* single task */
    JsonVal* single = json_val_obj_get(root, "task");
    JsonVal* tasks = json_val_obj_get(root, "tasks");

    if (tasks != NULL && json_val_is_arr(tasks)) {
        size_t n = json_val_arr_size(tasks);
        for (size_t i = 0; i < n; i++) {
            JsonVal* item = json_val_arr_get(tasks, i);
            if (item == NULL || !json_val_is_obj(item)) {
                continue;
            }
            const char* task = json_obj_get_str(item, "task");
            if (task == NULL) {
                continue;
            }
            SubTask t = {0};
            t.task = strdup(task);
            const char* v = json_obj_get_str(item, "role");
            t.role = v != NULL ? strdup(v) : NULL;
            v = json_obj_get_str(item, "system_prompt");
            t.system_prompt = v != NULL ? strdup(v) : NULL;
            v = json_obj_get_str(item, "model");
            t.model = v != NULL ? strdup(v) : NULL;
            t.timeout_s = json_obj_get_int(item, "timeout", SUBAGENT_DEFAULT_TIMEOUT_S);
            if (t.timeout_s < 1) {
                t.timeout_s = 1;
            }
            if (vector_push(out, &t) == NULL) {
                subtask_free(&t);
                json_doc_free(doc);
                return AGENT_ERR_OOM;
            }
        }
    } else if (single != NULL && json_val_is_str(single)) {
        SubTask t = {0};
        t.task = strdup(json_val_str(single));
        t.timeout_s = SUBAGENT_DEFAULT_TIMEOUT_S;
        if (vector_push(out, &t) == NULL) {
            subtask_free(&t);
            json_doc_free(doc);
            return AGENT_ERR_OOM;
        }
    }

    json_doc_free(doc);
    return vector_len(out) > 0 ? AGENT_OK : AGENT_ERR_JSON;
}

static bool subtask_finished(const Agent* a) {
    return a->state == AGENT_DONE || a->state == AGENT_ERROR || a->state == AGENT_CANCELLED;
}

/* Append the child's final answer (last assistant content) to out. */
static void append_child_result(String* out, const char* role, const Agent* child) {
    string_append(out, "[");
    string_append(out, role != NULL ? role : "subagent");
    string_append(out, "] ");
    if (child->state == AGENT_ERROR || child->state == AGENT_CANCELLED) {
        string_append(out, "(failed or cancelled)\n");
        return;
    }
    const char* content = NULL;
    for (size_t i = child->messages.len; i > 0; i--) {
        const Message* m = &child->messages.items[i - 1];
        if (m->role == MSG_ASSISTANT && m->content != NULL) {
            content = m->content;
            break;
        }
    }
    if (content != NULL) {
        string_append(out, content);
    } else {
        string_append(out, "(no answer)");
    }
    string_append(out, "\n");
}

typedef struct {
    ToolTask base;
    Vector tasks;    /* SubTask */
    Agent** children;
    size_t n;
    int64_t deadline_ms;
    bool cancelled;
} SubagentTask;

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void subagent_task_cancel(ToolTask* base) {
    SubagentTask* task = (SubagentTask*)base;
    task->cancelled = true;
    for (size_t i = 0; i < task->n; i++) {
        if (task->children[i] != NULL && !subtask_finished(task->children[i])) {
            cancel_token_cancel(&task->children[i]->cancel);
        }
    }
}

static void subagent_task_destroy(ToolTask* base) {
    SubagentTask* task = (SubagentTask*)base;
    if (task == NULL) {
        return;
    }
    subagent_task_cancel(base);
    for (size_t i = 0; i < task->n; i++) {
        agent_destroy(task->children[i]);
    }
    free((void*)task->children);
    for (size_t i = 0; i < vector_len(&task->tasks); i++) {
        subtask_free(vector_at(&task->tasks, i));
    }
    vector_free(&task->tasks);
    free(task);
}

static int subagent_task_poll(ToolTask* base, ToolResult* result, bool* done) {
    SubagentTask* task = (SubagentTask*)base;
    if (!task->cancelled && monotonic_ms() >= task->deadline_ms) {
        subagent_task_cancel(base);
    }

    bool all_done = true;
    for (size_t i = 0; i < task->n; i++) {
        Agent* child = task->children[i];
        if (child != NULL && !subtask_finished(child)) {
            (void)agent_resume(child);
            if (!subtask_finished(child)) {
                all_done = false;
            }
        }
    }
    if (!all_done) {
        *done = false;
        return AGENT_OK;
    }

    String out = string_new();
    for (size_t i = 0; i < task->n; i++) {
        if (task->children[i] == NULL) {
            continue;
        }
        SubTask* subtask = vector_at(&task->tasks, i);
        append_child_result(&out, subtask->role, task->children[i]);
    }
    result->content = string_take(&out);
    result->is_error = false;
    *done = true;
    return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
}

static int subagent_start(ToolContext* ctx, const char* arguments, ToolResult* result,
                          ToolTask** task_out) {
    *task_out = NULL;
    result->content = NULL;
    result->is_error = false;
    if (ctx == NULL || ctx->agent == NULL || ctx->runtime == NULL) {
        result->content = strdup("error: subagent tool requires an agent context");
        result->is_error = true;
        return AGENT_OK;
    }

    SubagentTask* task = calloc(1, sizeof(SubagentTask));
    if (task == NULL) {
        return AGENT_ERR_OOM;
    }
    task->base.poll = subagent_task_poll;
    task->base.cancel = subagent_task_cancel;
    task->base.destroy = subagent_task_destroy;
    task->tasks = vector_new(sizeof(SubTask));

    int rc = parse_tasks(arguments, &task->tasks);
    if (rc != AGENT_OK || vector_len(&task->tasks) == 0) {
        subagent_task_destroy(&task->base);
        result->content = strdup("error: need \"task\" (string) or \"tasks\" "
                                 "(array of {task, role?})");
        result->is_error = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }

    task->n = vector_len(&task->tasks);
    task->children = (Agent**)calloc(task->n, sizeof(Agent*));
    if (task->children == NULL) {
        subagent_task_destroy(&task->base);
        return AGENT_ERR_OOM;
    }

    int64_t max_timeout_ms = 1000;
    for (size_t i = 0; i < task->n; i++) {
        SubTask* subtask = vector_at(&task->tasks, i);
        AgentConfig cfg = {
            .system_prompt = subtask->system_prompt,
            .model_name = subtask->model,
            .cwd = ctx->agent->config.cwd,
        };
        task->children[i] = agent_spawn(ctx->agent, &cfg);
        if (task->children[i] != NULL) {
            (void)agent_start(task->children[i], subtask->task);
        }
        int64_t timeout_ms = subtask->timeout_s * 1000;
        if (timeout_ms > max_timeout_ms) {
            max_timeout_ms = timeout_ms;
        }
    }
    task->deadline_ms = monotonic_ms() + max_timeout_ms;
    *task_out = &task->base;
    return AGENT_OK;
}

Tool subagent_tool = {
    .name = "subagent",
    .description = "Run one or more child agents (in parallel when given "
                   "an array). Children share the parent's model, tools and "
                   "working directory and return their final answer.",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"task\":"
                    "{\"type\":\"string\",\"description\":\"Instruction for "
                    "the child agent\"},\"tasks\":{\"type\":\"array\","
                    "\"items\":{\"type\":\"object\",\"properties\":{\"task\":"
                    "{\"type\":\"string\"},\"role\":{\"type\":\"string\"},"
                    "\"system_prompt\":{\"type\":\"string\"},\"timeout\":"
                    "{\"type\":\"integer\"}}}}},\"oneOf\":[{\"required\":"
                    "[\"task\"]},{\"required\":[\"tasks\"]}]}",
    .flags = TOOL_FLAG_NONE,
    .execute = NULL,
    .start = subagent_start,
};
