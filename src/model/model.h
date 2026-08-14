/*
 * model/model.h — model abstraction (DESIGN.md §3.7).
 *
 * The Agent core only sees normalized ModelEvent values; provider-specific
 * SSE JSON is parsed inside the provider implementation (openai.c etc.).
 *
 * Ownership:
 *   - Model.name / Model.priv are owned by Model; freed by ops->destroy.
 *   - Model.provider / Model.ops are borrowed (runtime-owned).
 *   - ModelRequest holds borrowed pointers only; valid for the duration of
 *     the request call.
 *   - Event payload strings inside ModelEvent are BORROWED and only valid
 *     during the callback invocation; copy if you need them later.
 *
 * Thread-safety: a Model is used by one Agent request at a time in
 * Phase 1 (synchronous). Phase 4 (async runtime) will serialize per
 * provider through the shared curl multi handle.
 */

#ifndef CAGENT_MODEL_MODEL_H
#define CAGENT_MODEL_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/error.h"

typedef struct Model Model;
typedef struct Provider Provider;
typedef struct ModelRequest ModelRequest;
typedef struct ToolRegistry ToolRegistry; /* tool/registry.h */
typedef struct Agent Agent;               /* agent/agent.h */
typedef struct MessageList MessageList;   /* agent/message.h */

typedef enum {
    MODEL_EVENT_TEXT_DELTA,      /* assistant text increment */
    MODEL_EVENT_REASONING_DELTA, /* reasoning increment */
    MODEL_EVENT_TOOL_CALL_START, /* first fragment of a tool call */
    MODEL_EVENT_TOOL_CALL_DELTA, /* arguments increment for a tool call */
    MODEL_EVENT_TOOL_CALL_END,   /* the tool call's fragments are complete */
    MODEL_EVENT_USAGE,           /* final usage, if the provider reports it */
    MODEL_EVENT_DONE,            /* stream reached a provider terminal event */
    MODEL_EVENT_ERROR            /* stream failed; code + message */
} ModelEventType;

/* Provider-specific terminal reasons normalized for the Agent loop. Unknown
 * preserves compatibility with gateways that emit only a generic [DONE]. */
typedef enum {
    MODEL_STOP_UNKNOWN = 0,
    MODEL_STOP_COMPLETE,
    MODEL_STOP_TOOL_CALLS,
    MODEL_STOP_MAX_TOKENS,
    MODEL_STOP_CONTENT_FILTER,
    MODEL_STOP_INCOMPLETE
} ModelStopReason;

/* Normalized token usage (provider-specific formats converted here). */
typedef struct {
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t cached_tokens; /* 0 when the provider does not report it */
    int64_t total_tokens;
} Usage;

typedef struct {
    ModelEventType type;
    uint64_t request_id; /* echoes ModelRequest.id */
    union {
        struct {
            const char* data; /* borrowed */
            size_t len;
        } text;
        struct {
            size_t index;     /* tool call index within this request */
            const char* id;   /* borrowed; may be NULL on a fragment */
            const char* name; /* borrowed; may be NULL on a fragment */
        } tool_start;
        struct {
            size_t index;
            const char* delta; /* borrowed; arguments increment */
            size_t len;
        } tool_delta;
        struct {
            size_t index;
        } tool_end;
        struct {
            Usage usage;
        } usage;
        struct {
            ModelStopReason reason;
        } done;
        struct {
            int code;            /* HTTP status or negative AgentError */
            const char* message; /* borrowed */
        } error;
    } u;
} ModelEvent;

/* Event receiver: called by the provider for every normalized event. */
typedef void (*ModelEventCallback)(void* userdata, const ModelEvent* event);

typedef struct ModelOps {
    /* Start an asynchronous request. Events are delivered through the
     * callback in ModelRequest as the shared HTTP runtime is pumped. */
    int (*request)(Model* model, ModelRequest* request);

    /* Cancel one request by its stable ModelRequest.id. Models are shared
     * by concurrent Agents, so cancellation must never abort sibling
     * requests using the same Model instance. */
    int (*cancel)(Model* model, uint64_t request_id);

    /* Release Model.priv and the Model itself. */
    void (*destroy)(Model* model);
} ModelOps;

struct Model {
    ModelOps* ops;           /* borrowed; static vtable */
    Provider* provider;      /* borrowed */
    struct Runtime* runtime; /* borrowed; HTTP channel (async runtime) */
    char* name;              /* owned */
    int64_t context_window;
    int64_t max_output;
    /* False when context_window is only the local DEFAULT (no catalog /
     * config entry confirmed it); the footer then shows the window with a
     * "~" estimate marker instead of presenting a guess as fact. */
    bool window_verified;
    /* Optional per-model billing metadata (0/false = unknown).
     * input_price/output_price are USD per 1M tokens and feed the
     * footer cost display; subscription models bill a flat plan, so the
     * footer shows (sub) instead of a $ amount. */
    double input_price;
    double output_price;
    bool subscription;
    void* priv; /* owned; provider-specific */
};

struct ModelRequest {
    uint64_t id;
    Model* model;                 /* borrowed */
    struct Agent* agent;          /* borrowed; context/cancel target, may be NULL */
    ModelEventCallback event_cb;  /* borrowed; event receiver (may be NULL) */
    void* event_userdata;         /* borrowed; passed to event_cb */
    struct ToolRegistry* tools;   /* borrowed; may be NULL */
    const char* system_prompt;    /* borrowed; may be NULL */
    struct MessageList* messages; /* borrowed; the provider serializes in
                                     its own wire format (DESIGN.md §3.7) */
    bool is_compaction;           /* summary request; provider may ignore */
    int64_t max_tokens;
    double temperature;
    bool stream;
};

/* Human-readable event name for logging/tests. Static, never freed. */
const char* model_event_name(ModelEventType type);

#endif /* CAGENT_MODEL_MODEL_H */
