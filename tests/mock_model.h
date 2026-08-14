/*
 * tests/mock_model.h — scripted model for agent-loop tests (DESIGN.md §63).
 * No network involved: each request consumes the next step of a script.
 */

#ifndef CAGENT_TESTS_MOCK_MODEL_H
#define CAGENT_TESTS_MOCK_MODEL_H

#include <stddef.h>

#include "model/model.h"

typedef enum {
    MOCK_TEXT,        /* emit a text delta + done */
    MOCK_TEXT_CHUNKS, /* emit multiple text deltas + done */
    MOCK_TOOL_CALL,   /* emit one tool_call start/delta/end + done */
    MOCK_TOOL_CALLS,  /* emit multiple tool calls in one assistant turn */
    MOCK_ERROR        /* emit an error event */
} MockStepType;

typedef struct {
    const char* id;
    const char* name;
    const char* args;
} MockToolCall;

typedef struct {
    MockStepType type;
    const char* text;               /* MOCK_TEXT */
    const char* const* text_chunks; /* MOCK_TEXT_CHUNKS; borrowed */
    size_t n_text_chunks;
    const char* tool_id;            /* MOCK_TOOL_CALL */
    const char* tool_name;          /* MOCK_TOOL_CALL */
    const char* tool_args;          /* MOCK_TOOL_CALL */
    const MockToolCall* tool_calls; /* MOCK_TOOL_CALLS; borrowed */
    size_t n_tool_calls;
    ModelStopReason stop_reason; /* terminal reason for non-error steps */
    int error_code;              /* MOCK_ERROR */
    const char* error_msg;       /* MOCK_ERROR */
} MockStep;

#define MOCK_TEXT_STEP(t) {.type = MOCK_TEXT, .text = (t), .stop_reason = MODEL_STOP_COMPLETE}
#define MOCK_LIMIT_STEP(t) {.type = MOCK_TEXT, .text = (t), .stop_reason = MODEL_STOP_MAX_TOKENS}
#define MOCK_TEXT_CHUNKS_STEP(chunks, count)                                                       \
    {.type = MOCK_TEXT_CHUNKS,                                                                     \
     .text_chunks = (chunks),                                                                      \
     .n_text_chunks = (count),                                                                     \
     .stop_reason = MODEL_STOP_COMPLETE}
#define MOCK_TOOL_STEP(id, name, args)                                                             \
    {.type = MOCK_TOOL_CALL,                                                                       \
     .tool_id = (id),                                                                              \
     .tool_name = (name),                                                                          \
     .tool_args = (args),                                                                          \
     .stop_reason = MODEL_STOP_TOOL_CALLS}
#define MOCK_TOOL_LIMIT_STEP(id, name, args)                                                       \
    {.type = MOCK_TOOL_CALL,                                                                       \
     .tool_id = (id),                                                                              \
     .tool_name = (name),                                                                          \
     .tool_args = (args),                                                                          \
     .stop_reason = MODEL_STOP_MAX_TOKENS}
#define MOCK_TOOLS_STEP(calls, count)                                                              \
    {.type = MOCK_TOOL_CALLS,                                                                      \
     .tool_calls = (calls),                                                                        \
     .n_tool_calls = (count),                                                                      \
     .stop_reason = MODEL_STOP_TOOL_CALLS}
#define MOCK_ERROR_STEP(code, msg) {.type = MOCK_ERROR, .error_code = (code), .error_msg = (msg)}

/* provider may be NULL. The returned Model is owned by the caller and
 * destroyed via model->ops->destroy(). */
Model* mock_model_new(const char* name, const MockStep* steps, size_t n_steps);

/* Async variant: request() records the pending request and returns
 * WITHOUT emitting events; mock_model_pump() later completes it (emits
 * the next step's events). Lets tests control when a "request finishes"
 * so the scheduler's suspension points can be verified. */
Model* mock_model_new_async(const char* name, const MockStep* steps, size_t n_steps);
void mock_model_pump(Model* m);

/* Request counters used to assert that compaction is a separate model turn. */
size_t mock_model_compaction_requests(Model* m);
size_t mock_model_regular_requests(Model* m);

/* System prompt of the most recent regular (non-compaction) request. */
const char* mock_model_last_system_prompt(Model* m);

#endif /* CAGENT_TESTS_MOCK_MODEL_H */
