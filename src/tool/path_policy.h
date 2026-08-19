/* Shared workspace path policy for file-oriented tools. */
#ifndef CAGENT_TOOL_PATH_POLICY_H
#define CAGENT_TOOL_PATH_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#include "tool/tool.h"

/* Resolve a user-supplied path beneath ctx->cwd.
 *
 * Paths are workspace-relative only. Existing targets are canonicalized with
 * realpath() so symlinks cannot escape the workspace. When allow_missing_leaf
 * is true, only the final component may be absent (for write/create); its
 * parent must still resolve inside the workspace.
 */
int tool_path_resolve(const ToolContext* ctx, const char* path, bool allow_missing_leaf,
                      char* out, size_t out_size);

#endif /* CAGENT_TOOL_PATH_POLICY_H */
