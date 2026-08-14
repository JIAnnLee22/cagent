/*
 * tool/read.h — builtin tools registration.
 */

#ifndef CAGENT_TOOL_BUILTIN_H
#define CAGENT_TOOL_BUILTIN_H

#include "tool/registry.h"

/* Registers the Phase 1 builtin tools: read, write, bash. */
void register_builtin_tools(ToolRegistry* reg);

#endif /* CAGENT_TOOL_BUILTIN_H */
