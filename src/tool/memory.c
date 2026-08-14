/* tool/memory.c — append a structured cross-session memory record. */

#include <stdlib.h>
#include <string.h>

#include "agent/agent.h"
#include "session/session.h"
#include "tool/tool.h"
#include "util/json.h"

static int memory_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    if (ctx == NULL || ctx->agent == NULL || ctx->agent->session == NULL) {
        result->content = strdup("error: structured memory requires an active session");
        result->is_error = true;
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    const char* raw = arguments != NULL ? arguments : "{}";
    JsonDoc* doc = json_parse(raw, strlen(raw));
    JsonVal* root = doc != NULL ? json_root(doc) : NULL;
    const char* content = root != NULL ? json_obj_get_str(root, "content") : NULL;
    const char* kind = root != NULL ? json_obj_get_str(root, "kind") : "note";
    if (content == NULL || content[0] == '\0') {
        result->content = strdup("error: memory content is required");
        result->is_error = true;
        json_doc_free(doc);
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    int rc = session_append_memory(ctx->agent->session, kind, content);
    result->content = strdup(rc == AGENT_OK ? "memory recorded" : "error: cannot persist memory");
    result->is_error = rc != AGENT_OK;
    json_doc_free(doc);
    return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
}

Tool memory_tool = { /* NOLINT(misc-use-internal-linkage) */
    .name = "memory",
    .description = "Persist a concise decision, constraint, lesson, or unfinished item for session resume.",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"kind\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"content\"]}",
    .flags = TOOL_FLAG_NONE,
    .execute = memory_execute,
};
