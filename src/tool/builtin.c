/*
 * tool/builtin.c — register the Phase 1 builtin tools.
 */

#include "tool/builtin.h"

extern Tool read_tool;
extern Tool write_tool;
extern Tool bash_tool;
extern Tool bench_tool;
extern Tool edit_tool;
extern Tool diagnose_tool;
extern Tool git_diff_tool;
extern Tool git_checkpoint_tool;
extern Tool git_restore_checkpoint_tool;
extern Tool git_status_tool;
extern Tool git_commit_tool;
extern Tool git_revert_tool;
extern Tool grep_tool;
extern Tool list_tool;
extern Tool find_tool;
extern Tool memory_tool;
extern Tool plan_tool;
extern Tool subagent_tool;
extern Tool test_tool;

void register_builtin_tools(ToolRegistry* reg) {
    tool_registry_register(reg, &read_tool);
    tool_registry_register(reg, &write_tool);
    tool_registry_register(reg, &edit_tool);
    tool_registry_register(reg, &diagnose_tool);
    tool_registry_register(reg, &git_diff_tool);
    tool_registry_register(reg, &git_checkpoint_tool);
    tool_registry_register(reg, &git_restore_checkpoint_tool);
    tool_registry_register(reg, &git_status_tool);
    tool_registry_register(reg, &git_commit_tool);
    tool_registry_register(reg, &git_revert_tool);
    tool_registry_register(reg, &bash_tool);
    tool_registry_register(reg, &bench_tool);
    tool_registry_register(reg, &grep_tool);
    tool_registry_register(reg, &list_tool);
    tool_registry_register(reg, &find_tool);
    tool_registry_register(reg, &memory_tool);
    tool_registry_register(reg, &plan_tool);
    tool_registry_register(reg, &subagent_tool);
    tool_registry_register(reg, &test_tool);
}
