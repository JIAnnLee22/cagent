/*
 * tests/test_tui_format.c — tool-call summary formatter tests.
 *
 * Pure unit tests: no terminal, no TUI, just the formatter. We exercise
 * every tool the registry ships, plus the malformed-JSON and unknown-tool
 * fallbacks, to guarantee that no raw JSON arguments leak into the UI.
 */

#include <stdlib.h>
#include <string.h>

#include "test_common.h"
#include "tui/format.h"

static int test_summary_returns_non_empty(void) {
    String s = tui_format_tool_call_summary("bash", "{\"command\":\"ls -la\"}");
    CHECK(s.data != NULL);
    CHECK(s.len > 0);
    CHECK(strstr(s.data, "bash") != NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_no_raw_json(void) {
    const char* args = "{\"command\":\"ls -la\",\"timeout\":5}";
    String s = tui_format_tool_call_summary("bash", args);
    /* Must never surface JSON braces, colons between fields, or quoted keys. */
    CHECK(strchr(s.data, '{') == NULL);
    CHECK(strchr(s.data, '}') == NULL);
    CHECK(strstr(s.data, "\"command\"") == NULL);
    CHECK(strstr(s.data, "\"timeout\"") == NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_bash(void) {
    String s = tui_format_tool_call_summary("bash",
                                            "{\"command\":\"echo hi\",\"timeout\":7}");
    CHECK(strstr(s.data, "echo hi") != NULL);
    CHECK(strstr(s.data, "timeout=7") != NULL || strstr(s.data, "7s") != NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_read(void) {
    String s = tui_format_tool_call_summary("read",
                                            "{\"path\":\"src/main.c\",\"offset\":10,\"limit\":50}");
    CHECK(strstr(s.data, "src/main.c") != NULL);
    CHECK(strstr(s.data, "offset=10") != NULL);
    CHECK(strstr(s.data, "limit=50") != NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_write(void) {
    String s = tui_format_tool_call_summary("write",
                                            "{\"path\":\"a.c\",\"content\":\"abc\\ndef\\n\"}");
    CHECK(strstr(s.data, "a.c") != NULL);
    CHECK(strstr(s.data, "4 bytes") != NULL || strstr(s.data, "bytes") != NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_edit(void) {
    String s = tui_format_tool_call_summary(
        "edit",
        "{\"path\":\"x.c\",\"old_text\":\"foo\",\"new_text\":\"foobar\"}");
    CHECK(strstr(s.data, "x.c") != NULL);
    CHECK(strstr(s.data, "-3/+6") != NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_list(void) {
    String s = tui_format_tool_call_summary("list",
                                            "{\"path\":\"src\",\"depth\":2,\"max_results\":50}");
    CHECK(strstr(s.data, "src") != NULL);
    CHECK(strstr(s.data, "depth=2") != NULL);
    string_free(&s);
    /* No path arg should still produce a usable summary. */
    String s2 = tui_format_tool_call_summary("list", "{}");
    CHECK(strstr(s2.data, "list") != NULL);
    string_free(&s2);
    return g_failures;
}

static int test_summary_find(void) {
    String s = tui_format_tool_call_summary("find",
                                            "{\"pattern\":\"*.c\",\"path\":\"src\",\"max_depth\":4}");
    CHECK(strstr(s.data, "pattern=*.c") != NULL);
    CHECK(strstr(s.data, "path=src") != NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_grep(void) {
    String s = tui_format_tool_call_summary("grep",
                                            "{\"pattern\":\"TODO\",\"path\":\"src\",\"max_results\":100}");
    CHECK(strstr(s.data, "pattern=TODO") != NULL);
    CHECK(strstr(s.data, "path=src") != NULL);
    CHECK(strstr(s.data, "-n 100") != NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_subagent_single(void) {
    String s = tui_format_tool_call_summary("subagent", "{\"task\":\"summarize this file\"}");
    CHECK(strstr(s.data, "summarize this file") != NULL);
    CHECK(strchr(s.data, '{') == NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_subagent_parallel(void) {
    const char* args = "{\"tasks\":["
                       "{\"task\":\"first\"},"
                       "{\"task\":\"second\"}"
                       "]}";
    String s = tui_format_tool_call_summary("subagent", args);
    CHECK(strstr(s.data, "2 subagents") != NULL);
    CHECK(strstr(s.data, "first") != NULL);
    CHECK(strchr(s.data, '{') == NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_memory(void) {
    String s = tui_format_tool_call_summary("memory",
                                            "{\"kind\":\"decision\",\"content\":\"use libcurl\"}");
    CHECK(strstr(s.data, "decision") != NULL);
    CHECK(strstr(s.data, "use libcurl") != NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_plan(void) {
    String s = tui_format_tool_call_summary(
        "plan", "{\"action\":\"add\",\"id\":\"step-1\",\"title\":\"Write tests\"}");
    CHECK(strstr(s.data, "add") != NULL);
    CHECK(strstr(s.data, "step-1") != NULL);
    CHECK(strstr(s.data, "Write tests") != NULL);
    string_free(&s);

    String s2 = tui_format_tool_call_summary("plan", "{\"action\":\"list\"}");
    CHECK(strstr(s2.data, "list") != NULL);
    CHECK(strchr(s2.data, '{') == NULL);
    string_free(&s2);
    return g_failures;
}

static int test_summary_git_tools(void) {
    String s = tui_format_tool_call_summary("git_commit", "{\"message\":\"Initial commit\"}");
    CHECK(strstr(s.data, "Initial commit") != NULL);
    CHECK(strchr(s.data, '{') == NULL);
    string_free(&s);

    String s2 = tui_format_tool_call_summary("git_revert", "{\"target\":\"HEAD~1\"}");
    CHECK(strstr(s2.data, "HEAD~1") != NULL);
    string_free(&s2);

    String s3 = tui_format_tool_call_summary("git_diff", "{\"path\":\"src\"}");
    CHECK(strstr(s3.data, "src") != NULL);
    string_free(&s3);

    String s4 = tui_format_tool_call_summary("git_diff", "{}");
    CHECK(strstr(s4.data, "working tree") != NULL);
    string_free(&s4);

    String s5 = tui_format_tool_call_summary("git_status", "{}");
    CHECK(strstr(s5.data, "git_status") != NULL);
    string_free(&s5);

    String s6 = tui_format_tool_call_summary("git_checkpoint", "{}");
    CHECK(strstr(s6.data, "git_checkpoint") != NULL);
    string_free(&s6);
    return g_failures;
}

static int test_summary_test_bench(void) {
    String s = tui_format_tool_call_summary("test",
                                            "{\"action\":\"ctest\",\"build_dir\":\"build\"}");
    CHECK(strstr(s.data, "action=ctest") != NULL);
    CHECK(strstr(s.data, "build") != NULL);
    string_free(&s);

    String s2 = tui_format_tool_call_summary("test", "{\"action\":\"ctest\"}");
    CHECK(strstr(s2.data, "action=ctest") != NULL);
    string_free(&s2);

    String s3 = tui_format_tool_call_summary("bench", "{\"build_dir\":\"build\"}");
    CHECK(strstr(s3.data, "build") != NULL);
    string_free(&s3);

    String s4 = tui_format_tool_call_summary("bench", "{}");
    CHECK(strstr(s4.data, "default") != NULL);
    string_free(&s4);
    return g_failures;
}

static int test_summary_diagnose(void) {
    String s = tui_format_tool_call_summary("diagnose", "{}");
    CHECK(strstr(s.data, "diagnose") != NULL);
    CHECK(strchr(s.data, '{') == NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_unknown_tool(void) {
    String s = tui_format_tool_call_summary("mystery", "{\"foo\":\"bar\"}");
    CHECK(strstr(s.data, "mystery") != NULL);
    /* Unknown tool with valid object: shows nothing extra (no fallback), and
     * definitely no raw JSON braces. */
    CHECK(strchr(s.data, '{') == NULL);
    CHECK(strchr(s.data, '}') == NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_malformed_json(void) {
    /* Not even close to JSON — must not blow up. */
    String s = tui_format_tool_call_summary("bash", "{not valid");
    CHECK(s.data != NULL);
    CHECK(strstr(s.data, "bash") != NULL);
    CHECK(strchr(s.data, '{') == NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_null_inputs(void) {
    String s = tui_format_tool_call_summary(NULL, NULL);
    CHECK(s.data != NULL);
    CHECK(strstr(s.data, "?") != NULL);
    string_free(&s);

    String s2 = tui_format_tool_call_summary("bash", NULL);
    CHECK(strstr(s2.data, "bash") != NULL);
    CHECK(s2.len >= strlen("bash"));
    string_free(&s2);
    return g_failures;
}

static int test_summary_oversized(void) {
    char huge[8192];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    char args[8200];
    snprintf(args, sizeof(args), "{\"command\":\"%s\"}", huge);
    String s = tui_format_tool_call_summary("bash", args);
    /* Capped well below the raw input. */
    CHECK(s.len < 400);
    /* Truncation marker present. */
    const char* ellipsis = strstr(s.data, "\xe2\x80\xa6");
    CHECK(ellipsis != NULL || strstr(s.data, "...") != NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_sanitizes_control_chars(void) {
    /* Embedded ANSI escape and newline must be neutralized so the terminal
     * cannot be tricked into clearing the screen, while the actual text is
     * still surfaced on the same line. */
    String s = tui_format_tool_call_summary("bash",
                                            "{\"command\":\"echo \\u001b[2J\\n rm -rf /\"}");
    CHECK(strchr(s.data, '\x1b') == NULL);
    CHECK(strchr(s.data, '\n') == NULL);
    CHECK(strstr(s.data, "echo") != NULL);
    CHECK(strstr(s.data, "rm -rf /") != NULL);
    string_free(&s);
    return g_failures;
}

static int test_summary_tool_call_does_not_show_arguments_field(void) {
    /* Some providers wrap tool arguments under an `arguments` key. We must
     * never display that field label in the TUI, even when the inner shape
     * is unexpected. */
    String s = tui_format_tool_call_summary("bash",
                                            "{\"arguments\":{\"command\":\"ls\"}}");
    CHECK(strstr(s.data, "\"arguments\"") == NULL);
    CHECK(strstr(s.data, "arguments=") == NULL);
    string_free(&s);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_summary_returns_non_empty();
    g_failures += test_summary_no_raw_json();
    g_failures += test_summary_bash();
    g_failures += test_summary_read();
    g_failures += test_summary_write();
    g_failures += test_summary_edit();
    g_failures += test_summary_list();
    g_failures += test_summary_find();
    g_failures += test_summary_grep();
    g_failures += test_summary_subagent_single();
    g_failures += test_summary_subagent_parallel();
    g_failures += test_summary_memory();
    g_failures += test_summary_plan();
    g_failures += test_summary_git_tools();
    g_failures += test_summary_test_bench();
    g_failures += test_summary_diagnose();
    g_failures += test_summary_unknown_tool();
    g_failures += test_summary_malformed_json();
    g_failures += test_summary_null_inputs();
    g_failures += test_summary_oversized();
    g_failures += test_summary_sanitizes_control_chars();
    g_failures += test_summary_tool_call_does_not_show_arguments_field();

    if (g_failures == 0) {
        printf("test_tui_format: all tests passed\n");
        return 0;
    }
    printf("test_tui_format: %d test(s) failed\n", g_failures);
    return 1;
}
