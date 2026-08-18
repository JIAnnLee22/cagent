#ifndef CAGENT_UTIL_PROJECT_CONTEXT_H
#define CAGENT_UTIL_PROJECT_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>

#include "util/string.h"

/* Limits for automatically injected repository context.  Project memory is
 * intentionally much smaller than instruction files: it is a convenience
 * hint, not a replacement for reading the current files with a tool. */
typedef struct {
    size_t total_cap;     /* file-content bytes across all injected files */
    size_t file_cap;      /* maximum bytes from one instruction file */
    size_t progress_cap;  /* maximum bytes from root PROGRESS.md */
    bool include_progress;
} ProjectContextOptions;

ProjectContextOptions project_context_options_default(void);

/* Append bounded repository instructions from the git/workspace root through
 * cwd. Per-directory priority: AGENTS.override.md, AGENTS.md/AGENTS.MD,
 * CLAUDE.md/CLAUDE.MD. Root PROGRESS.md is appended as a bounded head/tail
 * project-memory excerpt. */
int project_context_append_with_options(const char* cwd, String* prompt,
                                        const ProjectContextOptions* options);

/* Compatibility wrapper using project_context_options_default(). */
int project_context_append(const char* cwd, String* prompt);

#endif /* CAGENT_UTIL_PROJECT_CONTEXT_H */
