/*
 * tool/registry.h — tool registry (DESIGN.md §3.6).
 *
 * All tools are registered at compile time (no dynamic plugins in
 * Phase 1); adding a capability = adding a .c/.h + one register call.
 *
 * Ownership:
 *   - The registry owns its array of Tool* and the parallel enabled[];
 *     the Tool objects themselves are borrowed (static).
 *   - tool_registry_schema_json() appends an OpenAI "tools" array JSON to
 *     *out (caller-owned String).
 */

#ifndef CAGENT_TOOL_REGISTRY_H
#define CAGENT_TOOL_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

#include "tool/tool.h"
#include "util/error.h"
#include "util/string.h"

typedef struct ToolRegistry {
    Tool** tools;  /* owned array; elements borrowed */
    bool* enabled; /* owned; parallel to tools */
    size_t len;
    size_t cap;
} ToolRegistry;

ToolRegistry* tool_registry_new(void);
void tool_registry_free(ToolRegistry* reg);

/* Borrowed Tool*, or NULL when absent/disabled. */
Tool* tool_registry_find(const ToolRegistry* reg, const char* name);

int tool_registry_register(ToolRegistry* reg, Tool* tool);
void tool_registry_set_enabled(ToolRegistry* reg, const char* name, bool on);
bool tool_registry_is_enabled(const ToolRegistry* reg, const char* name);
size_t tool_registry_count(const ToolRegistry* reg);

/* Append the OpenAI tools array JSON (only enabled tools). */
int tool_registry_schema_json(const ToolRegistry* reg, String* out);

/* Return the compact JSON byte size of the enabled tools schema.  This is
 * used for the local context-pressure estimate; it does not expose or retain
 * the temporary JSON buffer. */
int tool_registry_schema_bytes(const ToolRegistry* reg, size_t* out_bytes);

/* The stock registry with the Phase 1 builtin tools. */
ToolRegistry* tool_registry_default(void);

#endif /* CAGENT_TOOL_REGISTRY_H */
