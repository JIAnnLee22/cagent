#ifndef CAGENT_UTIL_PROJECT_CONTEXT_H
#define CAGENT_UTIL_PROJECT_CONTEXT_H

#include "util/string.h"

/* Append bounded repository instructions from the git/workspace root through
 * cwd. Per-directory priority: AGENTS.override.md, AGENTS.md/AGENTS.MD,
 * CLAUDE.md/CLAUDE.MD. Root PROGRESS.md is appended as project memory. */
int project_context_append(const char* cwd, String* prompt);

#endif /* CAGENT_UTIL_PROJECT_CONTEXT_H */
