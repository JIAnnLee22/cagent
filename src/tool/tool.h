/*
 * tool/tool.h — tool abstraction (DESIGN.md §3.5).
 *
 * A Tool is a statically-defined function descriptor. The Agent core only
 * knows ToolRegistry; concrete tools (read/write/bash/...) register
 * themselves at startup.
 *
 * Ownership:
 *   - name / description / input_schema are STATIC strings (borrowed
 *     forever); the registry never frees them.
 *   - ToolResult.content is allocated by the tool with malloc(); the CALLER
 *     owns it after execute() returns or an async task completes.
 *   - ToolContext fields are borrowed; valid only during execute()/start().
 *   - ToolTask owns every value needed after start() returns.
 *
 * Contract: a tool failure is NOT an agent failure — errors are reported
 * through ToolResult (is_error=true) and returned to the model, which
 * decides the next step. execute()/start()/poll() return AGENT_OK on a
 * successful invocation (even when is_error), or AgentError on an internal
 * failure (e.g. OOM).
 */

#ifndef CAGENT_TOOL_TOOL_H
#define CAGENT_TOOL_TOOL_H

#include <stdbool.h>
#include <stdint.h>

#include "util/error.h"
#include "util/string.h"

typedef struct ToolContext ToolContext;
typedef struct ToolTask ToolTask;
typedef struct Agent Agent;
typedef struct Runtime Runtime;
typedef struct CancelToken CancelToken;

typedef struct ToolResult {
    char* content; /* owned by tool; caller frees */
    bool is_error;
} ToolResult;

struct ToolContext {
    struct Agent* agent;        /* borrowed; may be NULL (unit tests) */
    struct Runtime* runtime;    /* borrowed; may be NULL (unit tests) */
    const char* cwd;            /* borrowed; working directory base */
    struct CancelToken* cancel; /* borrowed; may be NULL */
};

/* A pending tool invocation. poll() must return quickly and set done=false
 * while waiting. cancel() begins best-effort non-blocking cancellation;
 * destroy() releases the concrete task and is mandatory. */
struct ToolTask {
    int (*poll)(ToolTask* task, ToolResult* result, bool* done);
    void (*cancel)(ToolTask* task);
    void (*destroy)(ToolTask* task);
};

typedef struct Tool {
    const char* name;         /* static; registry key */
    const char* description;  /* static */
    const char* input_schema; /* static; JSON Schema string */
    uint32_t flags;           /* approval and concurrency capabilities */

    /* Optional, read-only approval preview. The caller owns result.content.
     * A preview is advisory and must never replace execution-time validation. */
    int (*preview)(ToolContext* ctx, const char* arguments, ToolResult* result);

    /* Synchronous implementation (optional when start is provided). */
    int (*execute)(ToolContext* ctx, const char* arguments, ToolResult* result);

    /* Optional async entry point. Immediate completion sets *task=NULL and
     * fills result; pending completion returns an owned task. */
    int (*start)(ToolContext* ctx, const char* arguments, ToolResult* result, ToolTask** task);
} Tool;

/* Tools with external side effects require an explicit approval result from
 * the host before execute()/start() is entered. */
#define TOOL_FLAG_NONE 0u
#define TOOL_FLAG_APPROVAL_REQUIRED (1u << 0)
/* The invocation is read-only and may run concurrently with another
 * parallel-safe invocation from the same assistant message. */
#define TOOL_FLAG_PARALLEL_SAFE (1u << 1)

/* Format the project plan at <cwd>/.cagent/plan.json into `out` as a
 * compact text block (one line per step plus acceptance/result). Returns
 * AGENT_OK with an empty `out` when no plan file exists; AGENT_ERR_* on
 * load/parse failures (caller should treat those as "no plan"). */
int plan_summary(const char* cwd, String* out);

/* True when a clean-worktree checkpoint exists for the given cwd (the file
 * git_checkpoint writes under ~/.local/state/cagent/checkpoints/). */
bool git_checkpoint_available(const char* cwd);

#endif /* CAGENT_TOOL_TOOL_H */
