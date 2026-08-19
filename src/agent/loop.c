/*
 * agent/loop.c — the agent loop state machine (DESIGN.md §5).
 *
 *   user input -> model request -> stream events -> accumulate message
 *   -> tool calls? -> execute tools (parallel-safe batches or serial) -> append tool results
 *   -> next model request -> ... -> final answer.
 *
 * The loop is a suspend/resume state machine. Model requests and long-lived
 * tools park the Agent and return AGENT_STEP_BUSY; EventLoop callbacks and
 * later agent_resume() calls advance them without blocking other agents.
 *
 * Message serialization and tool execution are the only parts that touch
 * provider-adjacent formats; the model call goes through ModelOps.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/evp.h>

#include "agent/agent.h"
#include "agent/context.h"
#include "agent/message.h"
#include "model/model.h"
#include "runtime/runtime.h"
#include "session/session.h"
#include "tool/tool.h"
#include "util/json.h"
#include "util/log.h"
#include "util/string.h"

#define MAX_OUTPUT_LIMIT_CONTINUATIONS 3u

/* ---- per-request accumulation ---------------------------------------- */

typedef struct {
    Agent* agent;
    String text;             /* assistant content deltas */
    String reasoning;        /* reasoning deltas */
    ToolCallList tool_calls; /* per-index, filled by index */
    Usage usage;
    ModelStopReason stop_reason;
    bool error;
    int error_code;
    char error_msg[512]; /* owned copy; the event's message is borrowed */
} LoopCtx;

/* cross-resume state of one running turn (owned by Agent.loop_state) */
typedef struct {
    size_t tool_index;
    ToolTask* task;
    ToolResult result;
    bool done;
    bool cancel_requested;
} ParallelToolCall;

typedef struct {
    bool in_turn;
    bool request_done; /* model request finished (DONE/ERROR seen) */
    bool request_cancel_requested;
    uint64_t request_id;
    int64_t request_max_tokens;
    LoopCtx ctx;
    String messages_json;      /* serialized conversation during the request */
    size_t tool_message_index; /* assistant message that owns tool_calls */
    size_t tool_index;         /* serial tool execution cursor */
    ToolTask* tool_task;       /* owned pending async invocation */
    bool tool_cancel_requested;
    ParallelToolCall* parallel_tools; /* owned batch invocations */
    size_t parallel_tool_count;
    bool approval_pending;
    bool approval_decided;
    bool approval_granted;
    size_t approval_tool_index;
    unsigned char approval_digest[EVP_MAX_MD_SIZE];
    bool approval_digest_valid;
    size_t request_retries; /* retries for the current logical model request */
    bool retry_pending;     /* waiting for non-blocking retry backoff */
    struct timespec retry_at;
    size_t output_limit_continuations;
    bool output_continuation_active;
    uint64_t request_sequence; /* includes compaction requests */

    /* A compaction request is a real async model request, but its response
     * is kept separate from the normal assistant accumulation. */
    bool compaction_active;
    bool compaction_done;
    bool compaction_error;
    ContextCompactionRequest compaction;
    String compaction_text;
    bool skip_compaction_once;

    struct timespec t0; /* request start (stats) */

    /* Per-turn live memory injection (bounded); valid only while the
     * current model request is in flight. */
    String memory_injection;
} AgentLoopState;

static bool approval_digest(const ToolCall* tc, unsigned char out[EVP_MAX_MD_SIZE]) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == NULL) {
        return false;
    }
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
    if (ok && tc != NULL && tc->name != NULL) {
        ok = EVP_DigestUpdate(ctx, tc->name, strlen(tc->name) + 1) == 1;
    }
    if (ok && tc != NULL && tc->arguments != NULL) {
        ok = EVP_DigestUpdate(ctx, tc->arguments, strlen(tc->arguments) + 1) == 1;
    }
    unsigned int digest_len = 0;
    if (ok) {
        ok = EVP_DigestFinal_ex(ctx, out, &digest_len) == 1 && digest_len == 32;
    }
    EVP_MD_CTX_free(ctx);
    return ok;
}

static bool approval_matches(const AgentLoopState* st, const ToolCall* tc) {
    if (st == NULL || tc == NULL || !st->approval_digest_valid) {
        return false;
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    return approval_digest(tc, digest) &&
           memcmp(st->approval_digest, digest, 32) == 0;
}

static AgentLoopState* loop_state(Agent* a) {
    if (a->loop_state == NULL) {
        AgentLoopState* st = calloc(1, sizeof(AgentLoopState));
        if (st == NULL) {
            return NULL;
        }
        st->ctx.text = string_new();
        st->ctx.reasoning = string_new();
        st->compaction_text = string_new();
        st->memory_injection = string_new();
        a->loop_state = st;
    }
    return a->loop_state;
}

static void parallel_tools_free(AgentLoopState* st) {
    if (st == NULL)
        return;
    for (size_t i = 0; i < st->parallel_tool_count; i++) {
        ParallelToolCall* call = &st->parallel_tools[i];
        if (call->task != NULL) {
            if (call->task->cancel != NULL)
                call->task->cancel(call->task);
            if (call->task->destroy != NULL)
                call->task->destroy(call->task);
        }
        free(call->result.content);
    }
    free(st->parallel_tools);
    st->parallel_tools = NULL;
    st->parallel_tool_count = 0;
}

int agent_set_approval_result(Agent* a, bool approved) {
    if (a == NULL) {
        return AGENT_ERR_TOOL;
    }
    AgentLoopState* st = loop_state(a);
    if (st == NULL || a->state != AGENT_WAIT_USER || !st->approval_pending ||
        st->approval_decided) {
        return AGENT_ERR_TOOL;
    }
    st->approval_decided = true;
    st->approval_granted = approved;
    return AGENT_OK;
}

static void loop_state_free(AgentLoopState* st) {
    if (st == NULL) {
        return;
    }
    if (st->tool_task != NULL) {
        if (st->tool_task->cancel != NULL) {
            st->tool_task->cancel(st->tool_task);
        }
        st->tool_task->destroy(st->tool_task);
    }
    parallel_tools_free(st);
    string_free(&st->ctx.text);
    string_free(&st->ctx.reasoning);
    string_free(&st->compaction_text);
    string_free(&st->memory_injection);
    context_compaction_request_free(&st->compaction);
    tool_call_list_free(&st->ctx.tool_calls);
    free(st);
}

/* ---- message list -> JSON array -------------------------------------- */

static ToolCall* loop_ensure_tool_call(LoopCtx* ctx, size_t index) {
    /* grow the list to index+1, zero-filled slots; slots are heap
     * allocated because tool_call_list_append() takes ownership */
    while (ctx->tool_calls.len <= index) {
        ToolCall* zero = calloc(1, sizeof(ToolCall));
        if (zero == NULL) {
            return NULL;
        }
        if (tool_call_list_append(&ctx->tool_calls, zero) != AGENT_OK) {
            free(zero);
            return NULL;
        }
    }
    return &ctx->tool_calls.items[index];
}

static void on_model_event(void* userdata, const ModelEvent* ev) {
    AgentLoopState* st = userdata;
    if (st == NULL || ev == NULL || ev->request_id != st->request_id) {
        return;
    }
    LoopCtx* ctx = &st->ctx;

    switch (ev->type) {
    case MODEL_EVENT_TEXT_DELTA:
        string_append_n(&ctx->text, ev->u.text.data, ev->u.text.len);
        if (ev->u.text.len > 0 && ctx->agent != NULL && ctx->agent->event_cb != NULL) {
            AgentEvent text_ev = {.type = AGENT_EVT_TEXT,
                                  .text = ev->u.text.data,
                                  .text_len = ev->u.text.len};
            ctx->agent->event_cb(ctx->agent->event_ud, &text_ev);
        }
        break;
    case MODEL_EVENT_REASONING_DELTA:
        string_append_n(&ctx->reasoning, ev->u.text.data, ev->u.text.len);
        break;
    case MODEL_EVENT_TOOL_CALL_START: {
        ToolCall* tc = loop_ensure_tool_call(ctx, ev->u.tool_start.index);
        if (tc == NULL) {
            return;
        }
        if (ev->u.tool_start.id != NULL && tc->id == NULL) {
            tc->id = strdup(ev->u.tool_start.id);
        }
        if (ev->u.tool_start.name != NULL && tc->name == NULL) {
            tc->name = strdup(ev->u.tool_start.name);
        }
        break;
    }
    case MODEL_EVENT_TOOL_CALL_DELTA: {
        ToolCall* tc = loop_ensure_tool_call(ctx, ev->u.tool_delta.index);
        if (tc == NULL) {
            return;
        }
        if (tc->arguments == NULL) {
            tc->arguments = strndup(ev->u.tool_delta.delta, ev->u.tool_delta.len);
        } else {
            size_t old = strlen(tc->arguments);
            size_t n = ev->u.tool_delta.len;
            char* merged = realloc(tc->arguments, old + n + 1);
            if (merged != NULL) {
                memcpy(merged + old, ev->u.tool_delta.delta, n);
                merged[old + n] = '\0';
                tc->arguments = merged;
            }
        }
        break;
    }
    case MODEL_EVENT_TOOL_CALL_END:
        break; /* nothing to do; the list is already complete */
    case MODEL_EVENT_USAGE:
        ctx->usage = ev->u.usage.usage;
        break;
    case MODEL_EVENT_DONE: {
        /* the request finished: complete the stats for this request */
        st->request_done = true;
        ctx->stop_reason = ev->u.done.reason;
        Agent* a = ctx->agent;
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        a->request_count++;
        a->model_time_ms +=
            (int64_t)(t1.tv_sec - st->t0.tv_sec) * 1000 + (t1.tv_nsec - st->t0.tv_nsec) / 1000000;
        a->usage_total.input_tokens += ctx->usage.input_tokens;
        a->usage_total.output_tokens += ctx->usage.output_tokens;
        a->usage_total.cached_tokens += ctx->usage.cached_tokens;
        a->usage_total.total_tokens += ctx->usage.total_tokens;
        if (a->session != NULL) {
            a->session->request_count = a->request_count;
            a->session->model_time_ms = a->model_time_ms;
            a->session->usage = a->usage_total;
        }
        break;
    }
    case MODEL_EVENT_ERROR:
        st->request_done = true;
        ctx->error = true;
        ctx->error_code = ev->u.error.code;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s",
                 ev->u.error.message != NULL ? ev->u.error.message : "model error");
        break;
    }
}

static void on_compaction_event(void* userdata, const ModelEvent* ev) {
    AgentLoopState* st = userdata;
    if (st == NULL || ev == NULL || ev->request_id != st->request_id) {
        return;
    }
    switch (ev->type) {
    case MODEL_EVENT_TEXT_DELTA:
        (void)string_append_n(&st->compaction_text, ev->u.text.data, ev->u.text.len);
        break;
    case MODEL_EVENT_DONE:
        st->compaction_done = true;
        break;
    case MODEL_EVENT_ERROR:
        st->compaction_done = true;
        st->compaction_error = true;
        break;
    default:
        /* Summary requests do not expose tools or reasoning to the loop. */
        break;
    }
}

/* ---- tool execution --------------------------------------------------- */

static int append_tool_result(Agent* a, ToolCall* tc, ToolResult* result) {
    if (result->content == NULL) {
        result->content = strdup("error: tool returned no result");
        result->is_error = true;
    }
    if (result->content == NULL) {
        return AGENT_ERR_OOM;
    }

    /* Mirror before message_list_append(): appending may realloc the list
     * and invalidate the borrowed ToolCall pointer. */
    char* mirror = strdup(result->content);
    if (mirror == NULL) {
        free(result->content);
        result->content = NULL;
        return AGENT_ERR_OOM;
    }
    free(tc->result);
    tc->result = mirror;
    tc->is_error = result->is_error;
    if (a->session != NULL && result->is_error) {
        char memory[2304];
        snprintf(memory, sizeof(memory), "tool %s failed: %.*s",
                 tc->name != NULL ? tc->name : "unknown", 2048, result->content);
        (void)session_append_memory(a->session, "tool_error", memory);
    }

    Message* tm = message_new(MSG_TOOL);
    if (tm == NULL) {
        free(result->content);
        result->content = NULL;
        return AGENT_ERR_OOM;
    }
    /* A missing call id is invalid for every provider's tool-result
     * protocol. Keep it NULL so the provider serializer can fail closed,
     * rather than manufacturing call_id:"". */
    if (tc->id != NULL && tc->id[0] != '\0') {
        tm->tool_call_id = strdup(tc->id);
    }
    tm->content = result->content; /* ownership transferred */
    result->content = NULL;
    tm->is_error = result->is_error;
    if (tm->tool_call_id == NULL) {
        message_free(tm);
        return AGENT_ERR_OOM;
    }
    if (a->session != NULL) {
        (void)session_append_message(a->session, tm);
    }
    int rc = message_list_append(&a->messages, tm);
    if (rc != AGENT_OK) {
        message_free(tm);
    }
    return rc;
}

static int append_tool_rejection(Agent* a, ToolCall* tc, const char* reason) {
    ToolResult result = {
        .content = strdup(reason != NULL ? reason : "error: tool execution denied"),
        .is_error = true,
    };
    if (result.content == NULL) {
        return AGENT_ERR_OOM;
    }
    return append_tool_result(a, tc, &result);
}

static int normalize_tool_failure(int rc, ToolResult* result) {
    if (rc == AGENT_OK && result->content != NULL) {
        return AGENT_OK;
    }
    if (result->content == NULL) {
        result->content = strdup("error: tool invocation failed");
    }
    result->is_error = true;
    return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
}

static int start_tool_call(Agent* a, AgentLoopState* st, ToolCall* tc, bool* pending) {
    *pending = false;
    Tool* tool = tool_registry_find(a->tools, tc->name != NULL ? tc->name : "");
    ToolResult result = {0};

    if (tool == NULL) {
        result.is_error = true;
        result.content = strdup("error: unknown tool");
    } else {
        ToolContext tctx = {
            .agent = a,
            .runtime = a->runtime,
            .cwd = a->config.cwd,
            .cancel = &a->cancel,
        };
        int rc;
        if (tool->start != NULL) {
            ToolTask* task = NULL;
            rc = tool->start(&tctx, tc->arguments != NULL ? tc->arguments : "{}", &result, &task);
            if (rc == AGENT_OK && task != NULL) {
                if (task->poll == NULL || task->destroy == NULL) {
                    if (task->destroy != NULL) {
                        task->destroy(task);
                    }
                    free(result.content);
                    result.content = strdup("error: invalid asynchronous tool task");
                    result.is_error = true;
                } else {
                    free(result.content); /* pending tasks must not return an immediate result */
                    st->tool_task = task;
                    st->tool_cancel_requested = false;
                    *pending = true;
                    return AGENT_OK;
                }
            } else {
                if (task != NULL && task->destroy != NULL) {
                    task->destroy(task);
                }
                rc = normalize_tool_failure(rc, &result);
            }
        } else if (tool->execute != NULL) {
            rc = tool->execute(&tctx, tc->arguments != NULL ? tc->arguments : "{}", &result);
            rc = normalize_tool_failure(rc, &result);
        } else {
            rc = AGENT_ERR_TOOL;
            result.content = strdup("error: tool has no implementation");
            result.is_error = true;
        }
        if (rc != AGENT_OK && normalize_tool_failure(rc, &result) != AGENT_OK) {
            return AGENT_ERR_OOM;
        }
    }
    return append_tool_result(a, tc, &result);
}

static char* build_tool_preview(Agent* a, Tool* tool, ToolCall* tc) {
    if (a == NULL || tool == NULL || tool->preview == NULL || tc == NULL) {
        return NULL;
    }
    ToolContext tctx = {
        .agent = a,
        .runtime = a->runtime,
        .cwd = a->config.cwd,
        .cancel = &a->cancel,
    };
    ToolResult preview = {0};
    int rc = tool->preview(&tctx, tc->arguments != NULL ? tc->arguments : "{}", &preview);
    if (rc != AGENT_OK && preview.content == NULL) {
        preview.content = strdup("preview unavailable; review the raw arguments carefully");
    }
    return preview.content;
}

static int poll_tool_call(Agent* a, AgentLoopState* st, ToolCall* tc, bool* done) {
    ToolResult result = {0};
    bool task_done = false;
    int rc = st->tool_task->poll(st->tool_task, &result, &task_done);
    if (!task_done && rc == AGENT_OK) {
        *done = false;
        return AGENT_OK;
    }

    st->tool_task->destroy(st->tool_task);
    st->tool_task = NULL;
    st->tool_cancel_requested = false;
    if (normalize_tool_failure(rc, &result) != AGENT_OK) {
        return AGENT_ERR_OOM;
    }
    *done = true;
    return append_tool_result(a, tc, &result);
}

static bool parallel_batch_eligible(Agent* a, const Message* assistant) {
    if (assistant == NULL || assistant->tool_calls.len < 2)
        return false;
    for (size_t i = 0; i < assistant->tool_calls.len; i++) {
        const ToolCall* tc = &assistant->tool_calls.items[i];
        Tool* tool = tool_registry_find(a->tools, tc->name != NULL ? tc->name : "");
        if (tool == NULL || (tool->flags & TOOL_FLAG_PARALLEL_SAFE) == 0u)
            return false;
    }
    return true;
}

static int start_parallel_tools(Agent* a, AgentLoopState* st, const Message* assistant) {
    size_t count = assistant->tool_calls.len;
    st->parallel_tools = calloc(count, sizeof(*st->parallel_tools));
    if (st->parallel_tools == NULL)
        return AGENT_ERR_OOM;
    st->parallel_tool_count = count;
    for (size_t i = 0; i < count; i++) {
        ParallelToolCall* call = &st->parallel_tools[i];
        call->tool_index = i;
        ToolCall* tc = &assistant->tool_calls.items[i];
        Tool* tool = tool_registry_find(a->tools, tc->name != NULL ? tc->name : "");
        ToolContext tctx = {.agent = a,
                            .runtime = a->runtime,
                            .cwd = a->config.cwd,
                            .cancel = &a->cancel};
        if (a->event_cb != NULL) {
            AgentEvent ev = {.type = AGENT_EVT_TOOL_START, .name = tc->name, .text = tc->arguments};
            a->event_cb(a->event_ud, &ev);
        }
        int rc;
        if (tool->start != NULL) {
            rc = tool->start(&tctx, tc->arguments != NULL ? tc->arguments : "{}", &call->result,
                             &call->task);
            if (rc == AGENT_OK && call->task != NULL) {
                if (call->task->poll == NULL || call->task->destroy == NULL) {
                    if (call->task->destroy != NULL)
                        call->task->destroy(call->task);
                    call->task = NULL;
                    free(call->result.content);
                    call->result.content = strdup("error: invalid asynchronous tool task");
                    call->result.is_error = true;
                } else {
                    free(call->result.content);
                    call->result.content = NULL;
                    continue;
                }
            } else if (call->task != NULL) {
                if (call->task->destroy != NULL)
                    call->task->destroy(call->task);
                call->task = NULL;
            }
            if (call->task == NULL)
                rc = normalize_tool_failure(rc, &call->result);
        } else if (tool->execute != NULL) {
            rc = tool->execute(&tctx, tc->arguments != NULL ? tc->arguments : "{}", &call->result);
            rc = normalize_tool_failure(rc, &call->result);
        } else {
            rc = AGENT_ERR_TOOL;
            call->result.content = strdup("error: tool has no implementation");
            call->result.is_error = true;
        }
        if (rc != AGENT_OK && normalize_tool_failure(rc, &call->result) != AGENT_OK) {
            parallel_tools_free(st);
            return AGENT_ERR_OOM;
        }
        call->done = true;
    }
    return AGENT_OK;
}

static int poll_parallel_tools(Agent* a, AgentLoopState* st, bool* done) {
    *done = false;
    for (size_t i = 0; i < st->parallel_tool_count; i++) {
        ParallelToolCall* call = &st->parallel_tools[i];
        if (call->done || call->task == NULL)
            continue;
        ToolResult result = {0};
        bool task_done = false;
        int rc = call->task->poll(call->task, &result, &task_done);
        if (!task_done && rc == AGENT_OK)
            continue;
        call->task->destroy(call->task);
        call->task = NULL;
        if (normalize_tool_failure(rc, &result) != AGENT_OK) {
            free(result.content);
            return AGENT_ERR_OOM;
        }
        call->result = result;
        call->done = true;
    }
    for (size_t i = 0; i < st->parallel_tool_count; i++) {
        if (!st->parallel_tools[i].done)
            return AGENT_OK;
    }
    for (size_t i = 0; i < st->parallel_tool_count; i++) {
        Message* assistant = &a->messages.items[st->tool_message_index];
        ToolCall* tc = &assistant->tool_calls.items[st->parallel_tools[i].tool_index];
        int rc = append_tool_result(a, tc, &st->parallel_tools[i].result);
        if (rc != AGENT_OK)
            return rc;
        assistant = &a->messages.items[st->tool_message_index];
        tc = &assistant->tool_calls.items[st->parallel_tools[i].tool_index];
        if (a->event_cb != NULL) {
            AgentEvent ev = {.type = AGENT_EVT_TOOL_END,
                             .name = tc->name,
                             .is_error = tc->is_error};
            a->event_cb(a->event_ud, &ev);
        }
    }
    *done = true;
    return AGENT_OK;
}

/* ---- request initiation ------------------------------------------------ */

static uint64_t next_request_id(Agent* a, AgentLoopState* st) {
    st->request_sequence++;
    uint64_t low = st->request_sequence & UINT32_MAX;
    if (low == 0) {
        low = 1;
    }
    return (a->id << 32) | low;
}

static bool summary_has_text(const String* text) {
    if (text == NULL || text->data == NULL) {
        return false;
    }
    for (size_t i = 0; i < text->len; i++) {
        if (text->data[i] != ' ' && text->data[i] != '\t' && text->data[i] != '\n' &&
            text->data[i] != '\r') {
            return true;
        }
    }
    return false;
}

static void persist_compaction(Agent* a, const ContextCompactionRequest* request,
                               const char* summary) {
    if (a->session != NULL && request != NULL && summary != NULL) {
        (void)session_append_compaction(a->session, request->start, request->count, summary);
    }
}

static int start_compaction_request(Agent* a, AgentLoopState* st) {
    context_compaction_request_free(&st->compaction);
    int rc = context_compaction_prepare(&a->messages, 10, &st->compaction);
    if (rc != AGENT_OK || st->compaction.count == 0) {
        return rc;
    }

    string_clear(&st->compaction_text);
    st->compaction_done = false;
    st->compaction_error = false;
    st->compaction_active = true;

    ModelRequest req = {0};
    req.id = next_request_id(a, st);
    st->request_id = req.id;
    req.model = a->model;
    req.agent = a;
    req.tools = NULL; /* summary requests must never execute tools */
    req.system_prompt = a->config.system_prompt;
    req.messages = &st->compaction.request_messages;
    req.max_tokens = a->model->max_output > 0 ? a->model->max_output : 2048;
    if (req.max_tokens > 2048) {
        req.max_tokens = 2048;
    }
    req.temperature = 0;
    req.stream = true;
    req.is_compaction = true;
    req.event_cb = on_compaction_event;
    req.event_userdata = st;

    a->state = AGENT_WAIT_MODEL;
    rc = a->model->ops->request(a->model, &req);
    if (rc != AGENT_OK) {
        st->compaction_active = false;
        context_compaction_request_free(&st->compaction);
    }
    return rc;
}

static void finish_compaction(Agent* a, AgentLoopState* st) {
    bool applied = false;
    size_t before = a->messages.len;
    if (!st->compaction_error && summary_has_text(&st->compaction_text) &&
        context_compaction_apply(&a->messages, &st->compaction, st->compaction_text.data) ==
            AGENT_OK) {
        applied = true;
        persist_compaction(a, &st->compaction, st->compaction_text.data);
        log_info("LLM context compacted: %zu -> %zu messages", before, a->messages.len);
    } else if (context_compaction_apply_fallback(&a->messages, &st->compaction) == AGENT_OK &&
               a->messages.len < before) {
        applied = true;
        Message* summary = &a->messages.items[st->compaction.start];
        persist_compaction(a, &st->compaction, summary->content);
        log_warn("LLM context compaction failed; deterministic fallback applied");
    }
    (void)applied;
    context_compaction_request_free(&st->compaction);
    string_clear(&st->compaction_text);
    st->compaction_active = false;
    st->skip_compaction_once = true;
    a->state = AGENT_READY;
}

/* Bounded live memory injection: surface the session's structured memory
 * to the model on every normal turn (not only after resume), capped so the
 * per-turn overhead stays small. Returns the system prompt to use for the
 * current request (either the config prompt or the injected copy). */
static const char* build_turn_system_prompt(Agent* a, AgentLoopState* st) {
    const char* base = a->config.system_prompt != NULL ? a->config.system_prompt : "";
    const char* mem = a->session != NULL ? session_memory(a->session) : NULL;
    bool inject_memory = mem != NULL && mem[0] != '\0';
    if (!inject_memory && !st->output_continuation_active) {
        return base;
    }

    string_clear(&st->memory_injection);
    string_append(&st->memory_injection, base);
    if (inject_memory) {
        size_t mem_len = strlen(mem);
        bool truncated = mem_len > AGENT_MEMORY_INJECTION_MAX;
        if (truncated) {
            mem_len = AGENT_MEMORY_INJECTION_MAX;
        }
        string_append(&st->memory_injection,
                      "\n\n--- BEGIN UNTRUSTED SESSION MEMORY (Session memory; verify against files) ---\n");
        string_append_n(&st->memory_injection, mem, mem_len);
        string_append(&st->memory_injection, "\n--- END UNTRUSTED SESSION MEMORY ---\n");
        if (truncated) {
            string_append(&st->memory_injection, "\n[memory truncated]");
        }
    }
    if (st->output_continuation_active) {
        string_append(
            &st->memory_injection,
            "\n\nThe previous model response reached its output-token limit before "
            "completing the user's request. Continue the same task now. Do not stop at "
            "analysis or a progress update: use tools as needed, finish the requested work, "
            "verify it, and only then provide the final answer. Regenerate any interrupted "
            "tool call from scratch.");
    }
    return st->memory_injection.data;
}

static int start_model_request(Agent* a, AgentLoopState* st) {
    /* reset the per-request accumulation */
    st->ctx.agent = a;
    string_clear(&st->ctx.text);
    string_clear(&st->ctx.reasoning);
    tool_call_list_free(&st->ctx.tool_calls);
    memset(&st->ctx.tool_calls, 0, sizeof(st->ctx.tool_calls));
    st->ctx.usage = (Usage){0};
    st->ctx.stop_reason = MODEL_STOP_UNKNOWN;
    st->ctx.error = false;
    st->ctx.error_code = 0;
    st->ctx.error_msg[0] = '\0';
    st->request_done = false;
    st->request_cancel_requested = false;

    ModelRequest req = {0};
    req.id = next_request_id(a, st);
    st->request_id = req.id;
    req.model = a->model;
    req.agent = a;
    req.tools = a->tools;
    req.system_prompt = build_turn_system_prompt(a, st);
    req.messages = &a->messages; /* provider serializes its own format */
    req.max_tokens = a->runtime != NULL ? a->runtime->config.max_tokens : 4096;
    st->request_max_tokens = req.max_tokens;
    req.temperature = 0;
    req.stream = true;
    req.is_compaction = false;
    req.event_cb = on_model_event;
    req.event_userdata = st;

    a->state = AGENT_WAIT_MODEL;
    clock_gettime(CLOCK_MONOTONIC, &st->t0);
    return a->model->ops->request(a->model, &req);
}

/* Start (or restart) the model request for the current conversation. */
static int start_request(Agent* a, AgentLoopState* st) {
    st->request_retries = 0;
    st->retry_pending = false;
    const char* turn_prompt = build_turn_system_prompt(a, st);
    if (!st->skip_compaction_once &&
        context_needs_compact_request(a->model, turn_prompt, &a->messages, a->tools)) {
        int rc = start_compaction_request(a, st);
        if (rc == AGENT_OK && st->compaction_active) {
            return AGENT_OK; /* the summary request is now in flight */
        }
        /* Preparation/request failure retains the old best-effort fallback. */
        size_t before = a->messages.len;
        if (context_compact(&a->messages, 10) == AGENT_OK && a->messages.len < before) {
            log_warn("LLM compaction unavailable; deterministic fallback applied");
        }
        st->skip_compaction_once = true;
    }
    st->skip_compaction_once = false;
    return start_model_request(a, st);
}

static bool retryable_model_error(int code) {
    return code == AGENT_ERR_HTTP || code == 408 || code == 425 || code == 429 || code == 500 ||
           code == 502 || code == 503 || code == 504;
}

static bool retry_delay_elapsed(const struct timespec* deadline) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > deadline->tv_sec ||
           (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}

static void schedule_retry_delay(AgentLoopState* st) {
    long delay_ms = 250L << (st->request_retries > 4 ? 4 : st->request_retries - 1);
    if (delay_ms > 2000)
        delay_ms = 2000;
    clock_gettime(CLOCK_MONOTONIC, &st->retry_at);
    st->retry_at.tv_sec += delay_ms / 1000;
    st->retry_at.tv_nsec += (delay_ms % 1000) * 1000000L;
    if (st->retry_at.tv_nsec >= 1000000000L) {
        st->retry_at.tv_sec++;
        st->retry_at.tv_nsec -= 1000000000L;
    }
    st->retry_pending = true;
}

static bool request_hit_output_limit(const AgentLoopState* st) {
    if (st->ctx.stop_reason == MODEL_STOP_MAX_TOKENS) {
        return true;
    }
    return st->ctx.stop_reason == MODEL_STOP_UNKNOWN && st->request_max_tokens > 0 &&
           st->ctx.usage.output_tokens >= st->request_max_tokens;
}

static bool request_ended_incomplete(const AgentLoopState* st) {
    return st->ctx.stop_reason == MODEL_STOP_CONTENT_FILTER ||
           st->ctx.stop_reason == MODEL_STOP_INCOMPLETE;
}

/* ---- the state machine -------------------------------------------------- */

int agent_start(Agent* a, const char* user_input) {
    if (a == NULL || user_input == NULL || a->model == NULL) {
        return AGENT_ERR_MODEL;
    }
    AgentLoopState* st = loop_state(a);
    if (st == NULL) {
        return AGENT_ERR_OOM;
    }
    if (a->state == AGENT_CANCELLED) {
        /* Cancellation ends one turn, not the whole reusable session. */
        a->cancel.cancelled = false;
    }

    Message* um = message_new(MSG_USER);
    if (um == NULL) {
        return AGENT_ERR_OOM;
    }
    if (message_set_content(um, user_input) != AGENT_OK) {
        message_free(um);
        return AGENT_ERR_OOM;
    }
    if (a->session != NULL) {
        session_append_message(a->session, um); /* before append takes it */
    }
    message_list_append(&a->messages, um);

    st->in_turn = true;
    st->output_limit_continuations = 0;
    st->output_continuation_active = false;
    st->request_retries = 0;
    st->retry_pending = false;
    st->approval_pending = false;
    st->approval_decided = false;
    st->approval_granted = false;
    a->state = AGENT_READY;

    AgentStepResult r = agent_resume(a);
    switch (r) {
    case AGENT_STEP_BUSY:
    case AGENT_STEP_DONE:
        return AGENT_OK; /* started (or finished synchronously) */
    case AGENT_STEP_CANCELLED:
        return AGENT_ERR_CANCELLED;
    default:
        return AGENT_ERR_MODEL;
    }
}

AgentStepResult agent_resume(Agent* a) {
    if (a == NULL) {
        return AGENT_STEP_ERROR;
    }
    AgentLoopState* st = loop_state(a);
    if (st == NULL) {
        return AGENT_STEP_ERROR;
    }

    for (;;) {
        if (cancel_token_check(&a->cancel)) {
            /* Provider models are shared, so cancel exactly this Agent's
             * request before releasing its callback state. */
            bool model_pending =
                a->state == AGENT_WAIT_MODEL && ((st->compaction_active && !st->compaction_done) ||
                                                 (!st->compaction_active && !st->request_done));
            if (model_pending) {
                if (!st->request_cancel_requested && a->model != NULL &&
                    a->model->ops->cancel != NULL) {
                    st->request_cancel_requested = true;
                    (void)a->model->ops->cancel(a->model, st->request_id);
                }
                if ((st->compaction_active && !st->compaction_done) ||
                    (!st->compaction_active && !st->request_done)) {
                    return AGENT_STEP_BUSY;
                }
            }
            if (st->compaction_active) {
                context_compaction_request_free(&st->compaction);
                string_clear(&st->compaction_text);
                st->compaction_active = false;
            }

            /* A pending tool owns external resources. Ask it to cancel and
             * keep pumping until it reports completion, then make the Agent
             * terminal. This preserves the no-zombie/no-dangling-callback
             * ownership contract without blocking the EventLoop thread. */
            if (st->tool_task != NULL) {
                if (!st->tool_cancel_requested && st->tool_task->cancel != NULL) {
                    st->tool_task->cancel(st->tool_task);
                    st->tool_cancel_requested = true;
                }
                ToolResult discarded = {0};
                bool done = false;
                int rc = st->tool_task->poll(st->tool_task, &discarded, &done);
                free(discarded.content);
                if (!done && rc == AGENT_OK) {
                    return AGENT_STEP_BUSY;
                }
                st->tool_task->destroy(st->tool_task);
                st->tool_task = NULL;
                st->tool_cancel_requested = false;
            }
            if (st->parallel_tool_count > 0) {
                for (size_t i = 0; i < st->parallel_tool_count; i++) {
                    ParallelToolCall* call = &st->parallel_tools[i];
                    if (call->task != NULL && !call->cancel_requested &&
                        call->task->cancel != NULL) {
                        call->task->cancel(call->task);
                        call->cancel_requested = true;
                    }
                }
                for (size_t i = 0; i < st->parallel_tool_count; i++) {
                    ParallelToolCall* call = &st->parallel_tools[i];
                    if (call->task == NULL)
                        continue;
                    ToolResult discarded = {0};
                    bool done = false;
                    int rc = call->task->poll(call->task, &discarded, &done);
                    free(discarded.content);
                    if (!done && rc == AGENT_OK)
                        return AGENT_STEP_BUSY;
                    call->task->destroy(call->task);
                    call->task = NULL;
                }
                parallel_tools_free(st);
            }
            a->state = AGENT_CANCELLED;
            st->in_turn = false;
            return AGENT_STEP_CANCELLED;
        }

        switch (a->state) {
        case AGENT_READY:
            /* first request of the turn */
            {
                int rc = start_request(a, st);
                if (rc != AGENT_OK) {
                    a->state = AGENT_ERROR;
                    st->in_turn = false;
                    return AGENT_STEP_ERROR;
                }
            }
            return AGENT_STEP_BUSY; /* suspended on the first request */

        case AGENT_WAIT_MODEL:
            if (st->compaction_active) {
                if (!st->compaction_done) {
                    return AGENT_STEP_BUSY; /* summary request still in flight */
                }
                finish_compaction(a, st);
                continue; /* start the normal request after the summary */
            }
            if (!st->request_done) {
                return AGENT_STEP_BUSY; /* suspended on async I/O */
            }
            if (st->ctx.error) {
                int64_t max_retries = a->runtime != NULL ? a->runtime->config.max_retries : 0;
                bool no_partial_output = st->ctx.text.len == 0 && st->ctx.reasoning.len == 0 &&
                                         st->ctx.tool_calls.len == 0;
                if (st->retry_pending) {
                    if (!retry_delay_elapsed(&st->retry_at))
                        return AGENT_STEP_BUSY;
                    st->retry_pending = false;
                    int retry_rc = start_model_request(a, st);
                    if (retry_rc == AGENT_OK)
                        return AGENT_STEP_BUSY;
                    st->ctx.error = true;
                    st->ctx.error_code = retry_rc;
                    snprintf(st->ctx.error_msg, sizeof(st->ctx.error_msg),
                             "failed to restart model request");
                } else if (no_partial_output && retryable_model_error(st->ctx.error_code) &&
                           (int64_t)st->request_retries < max_retries) {
                    st->request_retries++;
                    schedule_retry_delay(st);
                    log_warn("model request failed (%d); retry %zu/%lld scheduled",
                             st->ctx.error_code, st->request_retries, (long long)max_retries);
                    if (a->event_cb != NULL) {
                        char status[96];
                        snprintf(status, sizeof(status),
                                 "model retry %zu/%lld after transient error", st->request_retries,
                                 (long long)max_retries);
                        AgentEvent ev = {.type = AGENT_EVT_STATUS, .text = status};
                        a->event_cb(a->event_ud, &ev);
                    }
                    return AGENT_STEP_BUSY;
                }
                AgentEvent ev = {.type = AGENT_EVT_ERROR, .text = st->ctx.error_msg};
                if (a->event_cb != NULL) {
                    a->event_cb(a->event_ud, &ev);
                }
                a->state = AGENT_ERROR;
                st->in_turn = false;
                return AGENT_STEP_ERROR;
            }

            st->output_continuation_active = false;
            bool output_limited = request_hit_output_limit(st);
            bool terminal_incomplete = request_ended_incomplete(st);
            if (!output_limited) {
                /* The cap applies to one consecutive truncation episode, not
                 * the whole user turn. A completed response (most notably a
                 * tool call) made progress and starts a fresh episode. */
                st->output_limit_continuations = 0;
            }

            /* assemble the assistant message from the accumulated stream */
            {
                Message* am = message_new(MSG_ASSISTANT);
                if (am == NULL) {
                    a->state = AGENT_ERROR;
                    st->in_turn = false;
                    return AGENT_STEP_ERROR;
                }
                am->content = string_take(&st->ctx.text); /* may be NULL */
                am->reasoning = string_take(&st->ctx.reasoning);
                am->usage = st->ctx.usage;
                am->tool_calls = st->ctx.tool_calls; /* ownership transferred */
                memset(&st->ctx.tool_calls, 0, sizeof(st->ctx.tool_calls));
                /* Never execute or replay a tool call whose JSON may have
                 * been cut off by a non-complete terminal condition. */
                if ((output_limited || terminal_incomplete) && am->tool_calls.len > 0) {
                    tool_call_list_free(&am->tool_calls);
                }
                /* Reasoning-only responses leave content NULL with no tool
                 * calls. Strict OpenAI-compatible endpoints require an empty
                 * string rather than content:null on the continuation. */
                if (am->content == NULL && am->tool_calls.len == 0) {
                    am->content = strdup("");
                }
                message_list_append(&a->messages, am);
            }
            {
                Message* last = message_list_last(&a->messages);
                if (a->session != NULL) {
                    session_append_message(a->session, last);
                }

                if (output_limited) {
                    if (st->output_limit_continuations >= MAX_OUTPUT_LIMIT_CONTINUATIONS) {
                        char message[256];
                        snprintf(message, sizeof(message),
                                 "model output reached max_tokens after %u consecutive automatic "
                                 "continuations; task remains incomplete (increase max_tokens or "
                                 "submit a narrower request)",
                                 MAX_OUTPUT_LIMIT_CONTINUATIONS);
                        AgentEvent ev = {.type = AGENT_EVT_ERROR, .text = message};
                        if (a->event_cb != NULL) {
                            a->event_cb(a->event_ud, &ev);
                        }
                        a->state = AGENT_ERROR;
                        st->in_turn = false;
                        return AGENT_STEP_ERROR;
                    }
                    st->output_limit_continuations++;
                    st->output_continuation_active = true;
                    if (a->event_cb != NULL) {
                        char status[128];
                        snprintf(status, sizeof(status),
                                 "model output limit reached; continuing automatically (%zu/%u)",
                                 st->output_limit_continuations, MAX_OUTPUT_LIMIT_CONTINUATIONS);
                        AgentEvent ev = {.type = AGENT_EVT_STATUS, .text = status};
                        a->event_cb(a->event_ud, &ev);
                    }
                    int rc = start_request(a, st);
                    if (rc != AGENT_OK) {
                        a->state = AGENT_ERROR;
                        st->in_turn = false;
                        return AGENT_STEP_ERROR;
                    }
                    return AGENT_STEP_BUSY;
                }

                if (terminal_incomplete) {
                    const char* reason = st->ctx.stop_reason == MODEL_STOP_CONTENT_FILTER
                                             ? "model response was stopped by a content filter"
                                             : "model response ended incomplete";
                    AgentEvent ev = {.type = AGENT_EVT_ERROR, .text = reason};
                    if (a->event_cb != NULL) {
                        a->event_cb(a->event_ud, &ev);
                    }
                    a->state = AGENT_ERROR;
                    st->in_turn = false;
                    return AGENT_STEP_ERROR;
                }

                /* final answer (no tool calls): done. Text deltas were
                 * already forwarded from on_model_event(). */
                if (last->tool_calls.len == 0) {
                    a->state = AGENT_DONE;
                    st->in_turn = false;
                    return AGENT_STEP_DONE;
                }

                /* start executing tools */
                a->state = AGENT_WAIT_TOOL;
                a->tool_call_count += last->tool_calls.len;
                if (a->session != NULL) {
                    a->session->tool_call_count = a->tool_call_count;
                }
                st->tool_message_index = a->messages.len - 1;
                st->tool_index = 0;
            }
            /* fallthrough */

        case AGENT_WAIT_USER: {
            if (!st->approval_pending || !st->approval_decided) {
                return AGENT_STEP_BUSY;
            }
            Message* assistant = &a->messages.items[st->tool_message_index];
            ToolCall* tc = &assistant->tool_calls.items[st->tool_index];
            bool approval_stale = st->approval_granted &&
                                  (st->tool_index != st->approval_tool_index ||
                                   !approval_matches(st, tc));
            if (!st->approval_granted || approval_stale) {
                int rc = append_tool_rejection(
                    a, tc, approval_stale ? "error: approval is stale; tool call changed"
                                          : "error: tool execution denied by user");
                if (rc != AGENT_OK) {
                    a->state = AGENT_ERROR;
                    st->in_turn = false;
                    return AGENT_STEP_ERROR;
                }
                assistant = &a->messages.items[st->tool_message_index];
                tc = &assistant->tool_calls.items[st->tool_index];
                if (a->event_cb != NULL) {
                    AgentEvent ev = {.type = AGENT_EVT_TOOL_END,
                                     .name = tc->name,
                                     .is_error = true};
                    a->event_cb(a->event_ud, &ev);
                }
                st->tool_index++;
                st->approval_pending = false;
                st->approval_decided = false;
                st->approval_granted = false;
                st->approval_digest_valid = false;
                a->state = AGENT_WAIT_TOOL;
                continue;
            }
            st->approval_pending = false;
            st->approval_decided = false;
            st->approval_digest_valid = false;
            a->state = AGENT_WAIT_TOOL;
            continue;
        }

        case AGENT_WAIT_TOOL: {
            Message* assistant = &a->messages.items[st->tool_message_index];
            if (!st->parallel_tool_count && st->tool_index == 0 &&
                parallel_batch_eligible(a, assistant)) {
                int parallel_rc = start_parallel_tools(a, st, assistant);
                if (parallel_rc != AGENT_OK) {
                    a->state = AGENT_ERROR;
                    st->in_turn = false;
                    return AGENT_STEP_ERROR;
                }
            }
            if (st->parallel_tool_count > 0) {
                bool parallel_done = false;
                int parallel_rc = poll_parallel_tools(a, st, &parallel_done);
                if (parallel_rc != AGENT_OK) {
                    parallel_tools_free(st);
                    a->state = AGENT_ERROR;
                    st->in_turn = false;
                    return AGENT_STEP_ERROR;
                }
                if (!parallel_done)
                    return AGENT_STEP_BUSY;
                st->tool_index = a->messages.items[st->tool_message_index].tool_calls.len;
                parallel_tools_free(st);
                continue;
            }
            if (st->tool_index < assistant->tool_calls.len) {
                ToolCall* tc = &assistant->tool_calls.items[st->tool_index];
                int rc;

                if (st->tool_task != NULL) {
                    bool done = false;
                    rc = poll_tool_call(a, st, tc, &done);
                    if (rc != AGENT_OK) {
                        a->state = AGENT_ERROR;
                        st->in_turn = false;
                        return AGENT_STEP_ERROR;
                    }
                    if (!done) {
                        return AGENT_STEP_BUSY;
                    }
                } else {
                    Tool* tool = tool_registry_find(a->tools, tc->name != NULL ? tc->name : "");
                    if (tool != NULL && (tool->flags & TOOL_FLAG_APPROVAL_REQUIRED) != 0u) {
                        if (a->parent != NULL || !a->approval_available || a->event_cb == NULL) {
                            rc = append_tool_rejection(
                                a, tc,
                                "error: side-effecting tool denied; no approval host is available");
                            if (rc != AGENT_OK) {
                                a->state = AGENT_ERROR;
                                st->in_turn = false;
                                return AGENT_STEP_ERROR;
                            }
                            assistant = &a->messages.items[st->tool_message_index];
                            tc = &assistant->tool_calls.items[st->tool_index];
                            if (a->event_cb != NULL) {
                                AgentEvent ev = {.type = AGENT_EVT_TOOL_END,
                                                 .name = tc->name,
                                                 .is_error = true};
                                a->event_cb(a->event_ud, &ev);
                            }
                            st->tool_index++;
                            continue;
                        }
                        if (!agent_session_trusted(a)) {
                            if (!st->approval_granted) {
                                st->approval_pending = true;
                                st->approval_decided = false;
                                st->approval_tool_index = st->tool_index;
                                st->approval_digest_valid =
                                    approval_digest(tc, st->approval_digest);
                                a->state = AGENT_WAIT_USER;
                                char* preview = build_tool_preview(a, tool, tc);
                                AgentEvent ev = {.type = AGENT_EVT_TOOL_APPROVAL,
                                                 .name = tc->name,
                                                 .text = tc->arguments,
                                                 .preview = preview};
                                a->event_cb(a->event_ud, &ev);
                                if (a->approval_cb != NULL && !st->approval_decided) {
                                    st->approval_decided = true;
                                    st->approval_granted = a->approval_cb(a->approval_ud, &ev);
                                }
                                free(preview);
                                if (st->approval_decided) {
                                    continue;
                                }
                                return AGENT_STEP_BUSY;
                            }
                        }
                        st->approval_granted = false;
                    }
                    if (a->event_cb != NULL) {
                        AgentEvent ev = {.type = AGENT_EVT_TOOL_START,
                                         .name = tc->name,
                                         .text = tc->arguments};
                        a->event_cb(a->event_ud, &ev);
                    }
                    bool pending = false;
                    rc = start_tool_call(a, st, tc, &pending);
                    if (rc != AGENT_OK) {
                        a->state = AGENT_ERROR;
                        st->in_turn = false;
                        return AGENT_STEP_ERROR;
                    }
                    if (pending) {
                        return AGENT_STEP_BUSY;
                    }
                }

                /* append_tool_result may have moved the message array. */
                assistant = &a->messages.items[st->tool_message_index];
                tc = &assistant->tool_calls.items[st->tool_index];
                if (a->event_cb != NULL) {
                    AgentEvent ev = {.type = AGENT_EVT_TOOL_END,
                                     .name = tc->name,
                                     .is_error = tc->is_error};
                    a->event_cb(a->event_ud, &ev);
                }
                st->tool_index++;
                continue; /* preserve serial tool-call ordering */
            }

            /* all tools done: next model request carries the results */
            int rc = start_request(a, st);
            if (rc != AGENT_OK) {
                a->state = AGENT_ERROR;
                st->in_turn = false;
                return AGENT_STEP_ERROR;
            }
            return AGENT_STEP_BUSY; /* suspended on the new request */
        }

        default:
            st->in_turn = false;
            return a->state == AGENT_DONE ? AGENT_STEP_DONE : AGENT_STEP_ERROR;
        }
    }
}

void agent_loop_state_cleanup(Agent* a) {
    if (a == NULL) {
        return;
    }
    AgentLoopState* st = a->loop_state;
    if (st != NULL && a->state == AGENT_WAIT_MODEL && !st->request_done && a->model != NULL &&
        a->model->ops->cancel != NULL) {
        st->request_cancel_requested = true;
        (void)a->model->ops->cancel(a->model, st->request_id);
    }
    loop_state_free(st);
    a->loop_state = NULL;
}

int agent_run(Agent* a, const char* user_input) {
    int rc = agent_start(a, user_input);
    if (rc != AGENT_OK) {
        return rc; /* CANCELLED or start failure */
    }
    AgentStepResult r;
    do {
        if (a->runtime != NULL) {
            runtime_pump(a->runtime, 50);
        }
        r = agent_resume(a);
    } while (r == AGENT_STEP_BUSY);
    return r == AGENT_STEP_DONE        ? AGENT_OK
           : r == AGENT_STEP_CANCELLED ? AGENT_ERR_CANCELLED
                                       : AGENT_ERR_MODEL;
}
