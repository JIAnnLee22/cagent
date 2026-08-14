/*
 * report/report.c — static self-development assessment data.
 *
 * Source snapshot: current implementation and PROGRESS.md. Keep this
 * module synchronized when progress is reassessed. The overall value is
 * calculated from the dimension weights instead of being hard-coded.
 */

#include "report/report.h"

static const ReportDimension g_dimensions[] = {
    {"读-改-建-测闭环", 30, 92}, {"安全/审批/回滚", 25, 75}, {"规划与执行跟踪", 15, 90},
    {"记忆与状态恢复", 15, 88},  {"工具完备性", 10, 88},     {"自诊断", 5, 75},
};

static const char* g_gap_approval[] = {
    "增加容器/seccomp 等强沙箱",
    "限制既有 read/write/edit 的 workspace 越界路径",
    "擦洗子进程继承的敏感环境变量",
    "持久化审批审计记录",
};
static const char* g_gap_git[] = {
    "审批前提供完整 staged patch 浏览",
    "提供结构化 git add/path staging",
    "测试失败后建议而非自动执行回滚",
};
static const char* g_gap_plan[] = {
    "将计划批准绑定到能力和路径范围",
    "提供失败步骤的交互式重规划",
    "为长任务增加 token/费用预算",
};
static const char* g_gap_memory[] = {
    "提供 session 列表与可视化选择器",
    "显示 compaction 与持久化失败",
    "防止不可信项目内容污染长期 memory",
};
static const char* g_gap_tests[] = {
    "增加真实 PTY 的审批/resize 回归",
    "增加可选真实 provider 联调套件",
    "覆盖磁盘满和 session 写入失败",
};
static const char* g_gap_parallel[] = {
    "增加并行写冲突检测",
    "提供 batch edit 原子语义",
    "限制大型同步目录遍历的事件循环占用",
};
static const char* g_gap_diagnostics[] = {
    "将 retry/compaction 状态显示在 TUI",
    "保存历史 bench 基线并比较回归",
    "增加费用与每轮 token 预算指标",
};

static const ReportGap g_gaps[] = {
    {0, "审批与安全边界", 70, g_gap_approval, sizeof(g_gap_approval) / sizeof(g_gap_approval[0])},
    {0, "git 集成与回滚", 70, g_gap_git, sizeof(g_gap_git) / sizeof(g_gap_git[0])},
    {1, "任务规划与执行跟踪", 85, g_gap_plan, sizeof(g_gap_plan) / sizeof(g_gap_plan[0])},
    {1, "跨会话项目记忆", 80, g_gap_memory, sizeof(g_gap_memory) / sizeof(g_gap_memory[0])},
    {2, "测试结果结构化理解", 80, g_gap_tests, sizeof(g_gap_tests) / sizeof(g_gap_tests[0])},
    {2, "并行 tool-call", 70, g_gap_parallel, sizeof(g_gap_parallel) / sizeof(g_gap_parallel[0])},
    {3, "自我诊断与 profile", 75, g_gap_diagnostics,
     sizeof(g_gap_diagnostics) / sizeof(g_gap_diagnostics[0])},
};

static const char* g_done[] = {
    "代码理解：read/list/find/grep 与项目指令继承",
    "代码修改：write/edit 精确修改",
    "构建与测试：cmake、ninja、ctest 闭环",
    "多步执行：模型→工具→结果状态机",
    "上下文管理：compaction 与摘要 fallback",
    "会话持久化：JSONL 与 resume",
    "并发子任务：subagent 并行与取消传播",
    "连续工作：实时流式、历史回显、有限重试与轮次上限",
    "受监督变更：逐工具审批与 write/edit/bash/git 预览",
    "测试执行受审批门控：test 带预览，失败输出恢复指令（含 checkpoint 状态）",
    "任务上下文持久化：plan 摘要 resume 注入，会话记忆每轮有界注入",
};

static const char* g_roadmap[] = {
    "P0 安全边界：workspace 路径隔离、环境擦洗与强沙箱",
    "P1 可观测性：retry/compaction/预算状态与 session 选择器",
    "P1 git 体验：完整 patch 浏览、结构化 staging 与恢复建议",
    "P2 并行写冲突检测、batch edit 与异步目录遍历",
};

int report_overall_percent(void) {
    int weighted = 0;
    int weight_total = 0;
    for (size_t i = 0; i < sizeof(g_dimensions) / sizeof(g_dimensions[0]); i++) {
        weighted += g_dimensions[i].weight * g_dimensions[i].current;
        weight_total += g_dimensions[i].weight;
    }
    if (weight_total <= 0) {
        return 0;
    }
    return (weighted + weight_total / 2) / weight_total;
}

size_t report_dimension_count(void) {
    return sizeof(g_dimensions) / sizeof(g_dimensions[0]);
}

const ReportDimension* report_dimensions(void) {
    return g_dimensions;
}

size_t report_gap_count(void) {
    return sizeof(g_gaps) / sizeof(g_gaps[0]);
}

const ReportGap* report_gaps(void) {
    return g_gaps;
}

size_t report_done_count(void) {
    return sizeof(g_done) / sizeof(g_done[0]);
}

const char* const* report_dones(void) {
    return g_done;
}

size_t report_roadmap_count(void) {
    return sizeof(g_roadmap) / sizeof(g_roadmap[0]);
}

const char* const* report_roadmap(void) {
    return g_roadmap;
}

const char* report_title(void) {
    return "cagent 自举开发能力评估报告";
}

const char* report_conclusion(void) {
    return "基础读改建测闭环已具备，但审批安全与回滚能力仍是开启自主迭代的前置条件。";
}
