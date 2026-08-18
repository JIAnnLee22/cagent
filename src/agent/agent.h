/*
 * agent/agent.h — the Agent (DESIGN.md §3.8).
 *
 * An Agent is a lightweight state machine, never a thread or a process.
 * The agent loop is driven by agent_run() (synchronous in Phase 1; the
 * Phase 4 async runtime keeps the same state machine and suspends instead
 * of blocking).
 *
 * Ownership:
 *   - runtime/model/tools/session/parent are BORROWED.
 *   - messages and config are OWNED by the Agent.
 *   - cancel is a value member; agent_cancel() propagates nothing in
 *     Phase 1 (no children yet).
 *
 * Core never prints: consumers observe the agent via the event callback
 * (the formal Event Bus arrives with the TUI in Phase 6; this is its
 * minimal seed). Event strings are borrowed and valid during the call.
 */

#ifndef CAGENT_AGENT_AGENT_H
#define CAGENT_AGENT_AGENT_H

#include <stdbool.h>
#include <stdint.h>

#include "agent/message.h"
#include "util/error.h"

typedef struct Runtime Runtime;
typedef struct Model Model;
typedef struct ToolRegistry ToolRegistry;
typedef struct Session Session;

typedef enum {
    AGENT_READY,
    AGENT_WAIT_MODEL, /* awaiting the model stream */
    AGENT_WAIT_TOOL,  /* executing tools */
    AGENT_WAIT_CHILD, /* awaiting a child agent (Phase 5) */
    AGENT_WAIT_USER,
    AGENT_DONE,
    AGENT_ERROR,
    AGENT_CANCELLED
} AgentState;

typedef struct CancelToken {
    bool cancelled;
    struct CancelToken* parent; /* borrowed; may be NULL */
    uint64_t generation;
} CancelToken;

bool cancel_token_check(const CancelToken* t);
void cancel_token_cancel(CancelToken* t);

/* Owned copies are made of every string in the config. */
typedef struct {
    char* system_prompt; /* owned; may be NULL */
    char* model_name;    /* owned; may be NULL (runtime default) */
    char* cwd;           /* owned; may be NULL (runtime default) */
} AgentConfig;

/* Maximum session-memory bytes appended to a normal request's system prompt.
 * Keep this shared by the loop and the UI estimator so their accounting does
 * not drift. */
#define AGENT_MEMORY_INJECTION_MAX 8192

/* ---- minimal event seed (Phase 6: formal Event Bus) ------------------ */

typedef enum {
    AGENT_EVT_TEXT,          /* assistant final-answer text delta */
    AGENT_EVT_TOOL_START,    /* tool invocation (name + raw arguments) */
    AGENT_EVT_TOOL_APPROVAL, /* side-effecting tool waits for host approval */
    AGENT_EVT_TOOL_END,      /* tool finished (name + is_error) */
    AGENT_EVT_STATUS,        /* transient host-visible progress status */
    AGENT_EVT_ERROR          /* agent-level error */
} AgentEventType;

typedef struct {
    AgentEventType type;
    const char* text;    /* borrowed; text delta, arguments, or error message */
    size_t text_len;     /* explicit for deltas; 0 means NUL-terminated text */
    const char* preview; /* borrowed approval preview; valid during callback */
    const char* name;    /* borrowed; tool name */
    bool is_error;
} AgentEvent;

typedef void (*AgentEventCb)(void* userdata, const AgentEvent* ev);
typedef bool (*AgentApprovalCb)(void* userdata, const AgentEvent* ev);

typedef struct Agent {
    uint64_t id;
    struct Runtime* runtime;    /* borrowed */
    struct Model* model;        /* borrowed */
    MessageList messages;       /* owned */
    struct ToolRegistry* tools; /* borrowed */
    struct Session* session;    /* borrowed; NULL in Phase 1 */
    CancelToken cancel;         /* owned */
    AgentState state;
    struct Agent* parent;        /* borrowed; NULL */
    AgentConfig config;          /* owned */
    AgentEventCb event_cb;       /* borrowed; may be NULL */
    void* event_ud;              /* borrowed */
    bool approval_available;     /* host can answer AGENT_EVT_TOOL_APPROVAL */
    bool session_trusted;        /* explicit process-local approval bypass */
    AgentApprovalCb approval_cb; /* optional synchronous approval host */
    void* approval_ud;           /* borrowed */
    /* usage statistics (DESIGN.md §72/§73) */
    Usage usage_total; /* accumulated across requests */
    uint64_t request_count;
    int64_t model_time_ms; /* wall time inside model requests */
    uint64_t tool_call_count;
    /* async loop state (Phase 4): opaque to everyone except loop.c */
    void* loop_state; /* owned by the loop; freed by agent_destroy */
} Agent;

/* ---- asynchronous driving (Phase 4) ----------------------------------- */

typedef enum {
    AGENT_STEP_BUSY,  /* suspended: needs more runtime pumping */
    AGENT_STEP_DONE,  /* turn finished (AGENT_DONE) */
    AGENT_STEP_ERROR, /* turn failed (AGENT_ERROR) */
    AGENT_STEP_CANCELLED
} AgentStepResult;

/* Start a turn. The agent suspends at the first model request; call
 * agent_resume() after pumping the runtime. */
int agent_start(Agent* a, const char* user_input);

/* Advance the state machine from its suspension point. Returns
 * AGENT_STEP_BUSY when waiting for async work (model request in flight,
 * HTTP in progress), otherwise the terminal step. */
AgentStepResult agent_resume(Agent* a);

/* Convenience: agent_start + pump + resume until finished (synchronous
 * semantics, used by tests and simple callers). */
int agent_run(Agent* a, const char* user_input);

/* Release the async loop state (called by agent_destroy). */
void agent_loop_state_cleanup(Agent* a);

Agent* agent_new(Runtime* rt, const AgentConfig* cfg);
void agent_destroy(Agent* a);
void agent_set_event_cb(Agent* a, AgentEventCb cb, void* userdata);
void agent_set_approval_available(Agent* a, bool available);
void agent_set_approval_cb(Agent* a, AgentApprovalCb cb, void* userdata);
int agent_set_approval_result(Agent* a, bool approved);
void agent_set_session_trusted(Agent* a, bool trusted);
bool agent_session_trusted(const Agent* a);

/* Estimate the effective normal-request context, including the configured
 * system prompt, bounded session memory, message history, and enabled tool
 * schemas. Used by diagnostics and the UI; this remains a heuristic. */
int64_t agent_context_estimate_tokens(const Agent* a);

/* Replace the agent's model (tests use a mock; the runtime keeps owning
 * the original). The new model is borrowed. */
void agent_set_model(Agent* a, struct Model* m);

/* Spawn a child agent (DESIGN.md §23-24): inherits runtime, model, tools
 * and cwd from the parent; its CancelToken is chained to the parent's so
 * cancelling the parent cancels the subtree. The child is an independent
 * lightweight state machine — never a thread or a process. The caller
 * owns the child and must agent_destroy() it. */
Agent* agent_spawn(Agent* parent, const AgentConfig* cfg);

/* Spawn a child agent (DESIGN.md §23-24): inherits runtime, model, tools
 * and cwd from the parent; its CancelToken is chained to the parent's so
 * cancelling the parent cancels the subtree. The child is an independent
 * lightweight state machine — never a thread or a process. The caller
 * owns the child and must agent_destroy() it. */
Agent* agent_spawn(Agent* parent, const AgentConfig* cfg);

#endif /* CAGENT_AGENT_AGENT_H */
