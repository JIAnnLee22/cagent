/* tool/bench.c — bounded invocation of the optional self benchmark suite. */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/process.h"
#include "tool/tool.h"
#include "util/json.h"
#include "util/string.h"

#define BENCH_TIMEOUT_MS 120000
#define BENCH_OUTPUT_CAP (256 * 1024)

static bool safe_component(const char* value) {
    if (value == NULL || value[0] == '\0' || value[0] == '/') return false;
    const char* p = value;
    while (*p != '\0') {
        while (*p == '/') p++;
        const char* start = p;
        while (*p != '\0' && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len == 2 && start[0] == '.' && start[1] == '.') return false;
    }
    return true;
}

static int bench_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    JsonDoc* doc = json_parse(arguments != NULL ? arguments : "{}",
                               arguments != NULL ? strlen(arguments) : 2);
    JsonVal* root = doc != NULL ? json_root(doc) : NULL;
    const char* build_dir = root != NULL ? json_obj_get_str(root, "build_dir") : "build";
    if (!safe_component(build_dir)) {
        result->content = strdup("error: build_dir must stay inside the project");
        result->is_error = true;
        json_doc_free(doc);
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    const char* cwd = ctx != NULL && ctx->cwd != NULL ? ctx->cwd : ".";
    char executable[PATH_MAX];
    if (snprintf(executable, sizeof(executable), "%s/%s/cagent_bench", cwd, build_dir) >=
        (int)sizeof(executable)) {
        result->content = strdup("error: benchmark path is too long");
        result->is_error = true;
        json_doc_free(doc);
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    char* argv[] = {executable, NULL};
    ProcessResult process = {0};
    int rc = process_run(cwd, argv, BENCH_TIMEOUT_MS, BENCH_OUTPUT_CAP, &process);
    if (rc != AGENT_OK) {
        result->content = strdup("error: failed to start cagent_bench; configure with "
                                 "-DCAGENT_BUILD_BENCH=ON");
        result->is_error = true;
        process_result_free(&process);
        json_doc_free(doc);
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }
    String out = string_new();
    string_printf(&out, "benchmark exit=%d timeout=%s\n", process.exit_code,
                  process.timed_out ? "true" : "false");
    string_append_n(&out, process.out.data, process.out.len);
    if (process.output_capped) string_append(&out, "\n...[output truncated]\n");
    result->content = string_take(&out);
    result->is_error = process.timed_out || process.exit_code != 0;
    process_result_free(&process);
    json_doc_free(doc);
    return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
}

Tool bench_tool = { /* NOLINT(misc-use-internal-linkage) */
    .name = "bench",
    .description = "Run the optional cagent_bench suite and return bounded profile output.",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"build_dir\":{\"type\":\"string\"}}}",
    .flags = TOOL_FLAG_NONE,
    .execute = bench_execute,
};
