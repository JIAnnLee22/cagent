/*
 * tool/test.c — structured CTest result tool for agent self-verification.
 *
 * Only invokes ctest with argv (no shell) and returns a compact summary plus
 * failed test lines instead of forcing the model to parse the full log.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runtime/process.h"
#include "tool/tool.h"
#include "util/json.h"
#include "util/string.h"

#define TEST_TIMEOUT_MS 120000
#define TEST_OUTPUT_CAP (256 * 1024)

static bool safe_build_dir(const char* path) {
    if (path == NULL || path[0] == '\0' || path[0] == '/') return false;
    const char* p = path;
    while (*p != '\0') {
        while (*p == '/') p++;
        const char* start = p;
        while (*p != '\0' && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len == 2 && start[0] == '.' && start[1] == '.') return false;
    }
    return true;
}

static const char* find_in_range(const char* start, const char* end, const char* needle) {
    size_t needle_len = strlen(needle);
    for (const char* p = start; p != NULL && p + needle_len <= end; p++) {
        if (memcmp(p, needle, needle_len) == 0) return p;
    }
    return NULL;
}

static int xml_attr_int(const char* tag, const char* end, const char* name) {
    char key[64];
    int n = snprintf(key, sizeof(key), "%s=\"", name);
    if (n <= 0 || (size_t)n >= sizeof(key)) return 0;
    const char* p = find_in_range(tag, end, key);
    if (p == NULL) return 0;
    p += n;
    return atoi(p);
}

static void append_junit_failures(String* out, const char* xml, const char* end) {
    const char* cursor = xml;
    while ((cursor = find_in_range(cursor, end, "<testcase")) != NULL) {
        const char* close = find_in_range(cursor, end, "</testcase>");
        if (close == NULL) break;
        if (find_in_range(cursor, close, "<failure") != NULL ||
            find_in_range(cursor, close, "<error") != NULL) {
            const char* name = find_in_range(cursor, close, "name=\"");
            string_append(out, "failure: ");
            if (name != NULL) {
                name += 6;
                const char* name_end = memchr(name, '\"', (size_t)(close - name));
                if (name_end != NULL) string_append_n(out, name, (size_t)(name_end - name));
            } else {
                string_append(out, "unnamed test");
            }
            string_append_char(out, '\n');
        }
        cursor = close + 11;
    }
}

static char* read_file_bounded(const char* path, size_t cap) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0 || (size_t)size > cap || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); return NULL;
    }
    char* data = malloc((size_t)size + 1);
    if (data == NULL) { fclose(f); return NULL; }
    size_t n = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(data); return NULL; }
    data[n] = '\0';
    return data;
}

static int test_preview(ToolContext* ctx, const char* arguments, ToolResult* result) {
    (void)ctx;
    result->content = NULL;
    result->is_error = false;
    JsonDoc* doc = json_parse(arguments != NULL ? arguments : "{}",
                               arguments != NULL ? strlen(arguments) : 2);
    JsonVal* root = doc != NULL ? json_root(doc) : NULL;
    const char* build_dir = root != NULL ? json_obj_get_str(root, "build_dir") : NULL;
    char text[256];
    if (build_dir != NULL && !safe_build_dir(build_dir)) {
        result->content = strdup("error: build_dir must stay inside the project");
        result->is_error = true;
    } else {
        snprintf(text, sizeof(text),
                 "Run CTest in build_dir '%s' and report structured pass/fail counts. "
                 "On failure the agent is expected to fix the code or restore the checkpoint.",
                 build_dir != NULL ? build_dir : "build");
        result->content = strdup(text);
    }
    json_doc_free(doc); /* build_dir is not used after this point */
    return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
}

static int test_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    JsonDoc* doc = json_parse(arguments != NULL ? arguments : "{}",
                               arguments != NULL ? strlen(arguments) : 2);
    JsonVal* root = doc != NULL ? json_root(doc) : NULL;
    const char* action = root != NULL ? json_obj_get_str(root, "action") : NULL;
    const char* build_dir = root != NULL ? json_obj_get_str(root, "build_dir") : "build";
    if (action == NULL || strcmp(action, "ctest") != 0 || !safe_build_dir(build_dir)) {
        result->content = strdup("error: action must be ctest and build_dir must stay inside the project");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }

    char junit_path[] = "/tmp/cagent-ctest-XXXXXX.xml";
    int junit_fd = mkstemps(junit_path, 4);
    if (junit_fd < 0) {
        result->content = strdup("error: cannot create CTest result file");
        result->is_error = true;
        json_doc_free(doc);
        return AGENT_OK;
    }
    close(junit_fd);
    unlink(junit_path);
    char* argv[] = {(char*)"/usr/bin/ctest", (char*)"--test-dir", (char*)build_dir,
                    (char*)"--output-junit", junit_path, (char*)"--output-on-failure", NULL};
    ProcessResult process = {0};
    int rc = process_run(ctx != NULL ? ctx->cwd : NULL, argv, TEST_TIMEOUT_MS,
                         TEST_OUTPUT_CAP, &process);
    if (rc != AGENT_OK) {
        result->content = strdup("error: failed to start ctest");
        result->is_error = true;
        json_doc_free(doc);
        process_result_free(&process);
        return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }

    char* junit = read_file_bounded(junit_path, TEST_OUTPUT_CAP);
    unlink(junit_path);
    int total = 0, failed = 0, errors = 0;
    if (junit != NULL) {
        const char* suites = strstr(junit, "<testsuites");
        const char* suites_end = suites != NULL ? strchr(suites, '>') : NULL;
        if (suites != NULL && suites_end != NULL) {
            total = xml_attr_int(suites, suites_end, "tests");
            failed = xml_attr_int(suites, suites_end, "failures");
            errors = xml_attr_int(suites, suites_end, "errors");
        }
    }
    String out = string_new();
    string_printf(&out, "ctest summary: total=%d passed=%d failed=%d errors=%d exit=%d\n",
                  total, total - failed - errors, failed, errors, process.exit_code);
    if (junit != NULL) {
        append_junit_failures(&out, junit, junit + strlen(junit));
        free(junit);
    } else if (process.exit_code != 0) {
        string_append(&out, "failure: no JUnit result was produced\n");
    }
    if (process.timed_out) string_append(&out, "failure: ctest timed out after 120s\n");
    if (process.output_capped) string_append(&out, "note: diagnostic output truncated at 256 KiB\n");
    if (process.timed_out || process.exit_code != 0 || failed > 0 || errors > 0) {
        if (ctx != NULL && git_checkpoint_available(ctx->cwd)) {
            string_append(&out, "recovery: a clean checkpoint exists for this workspace; "
                                "run git_restore_checkpoint (approval required) to discard "
                                "this turn's tracked changes\n");
        } else {
            string_append(&out, "recovery: no checkpoint yet; run git_checkpoint on a clean "
                                "tree before the next risky turn, then git_restore_checkpoint "
                                "on failure\n");
        }
    }
    result->content = string_take(&out);
    result->is_error = process.timed_out || process.exit_code != 0 || failed > 0 || errors > 0;
    process_result_free(&process);
    json_doc_free(doc);
    return AGENT_OK;
}

Tool test_tool = { /* NOLINT(misc-use-internal-linkage) */
    .name = "test",
    .description = "Run CTest and return structured pass/fail counts and failures.",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"ctest\"]},\"build_dir\":{\"type\":\"string\"}},\"required\":[\"action\"]}",
    .flags = TOOL_FLAG_APPROVAL_REQUIRED,
    .preview = test_preview,
    .execute = test_execute,
};
