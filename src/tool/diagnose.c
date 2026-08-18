/* tool/diagnose.c — bounded runtime self-diagnostics for the agent. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/agent.h"
#include "agent/context.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "session/session.h"
#include "tool/registry.h"
#include "tool/tool.h"
#include "util/string.h"

static const char* state_name(AgentState state) {
    switch (state) {
    case AGENT_READY: return "ready";
    case AGENT_WAIT_MODEL: return "wait_model";
    case AGENT_WAIT_TOOL: return "wait_tool";
    case AGENT_WAIT_CHILD: return "wait_child";
    case AGENT_WAIT_USER: return "wait_user";
    case AGENT_DONE: return "done";
    case AGENT_ERROR: return "error";
    case AGENT_CANCELLED: return "cancelled";
    default: return "unknown";
    }
}

static int diagnose_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    (void)arguments;
    result->content = NULL;
    result->is_error = false;
    if (ctx == NULL || ctx->agent == NULL) {
        result->content = strdup("error: diagnostics require an active agent");
        result->is_error = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    Agent* agent = ctx->agent;
    String out = string_new();
    string_printf(&out, "agent.state=%s\nagent.messages=%zu\nagent.tool_calls=%zu\n"
                        "agent.context_tokens=%lld\nagent.cwd=%s\n",
                  state_name(agent->state), agent->messages.len, agent->tool_call_count,
                  (long long)agent_context_estimate_tokens(agent),
                  agent->config.cwd != NULL ? agent->config.cwd : "");
    if (agent->tools != NULL) {
        string_printf(&out, "tools.registered=%zu\n", tool_registry_count(agent->tools));
    }
    if (agent->runtime != NULL && agent->runtime->scheduler != NULL) {
        Scheduler* scheduler = agent->runtime->scheduler;
        string_printf(&out, "scheduler.agents=%zu\nscheduler.running=%zu\nscheduler.limit=%zu\n",
                      scheduler->len, scheduler->running, scheduler->max_concurrent);
    }
    if (agent->session != NULL) {
        const Session* session = agent->session;
        string_printf(&out, "session.requests=%llu\nsession.tool_calls=%llu\n"
                            "session.tokens_in=%lld\nsession.tokens_out=%lld\n"
                            "session.memory_bytes=%zu\n",
                      (unsigned long long)session->request_count,
                      (unsigned long long)session->tool_call_count,
                      (long long)session->usage.input_tokens,
                      (long long)session->usage.output_tokens,
                      session_memory(session) != NULL ? strlen(session_memory(session)) : 0);
    }
    result->content = string_take(&out);
    return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
}

Tool diagnose_tool = { /* NOLINT(misc-use-internal-linkage) */
    .name = "diagnose",
    .description = "Report bounded agent, scheduler, context, session, and tool metrics.",
    .input_schema = "{\"type\":\"object\"}",
    .flags = TOOL_FLAG_PARALLEL_SAFE,
    .execute = diagnose_execute,
};
