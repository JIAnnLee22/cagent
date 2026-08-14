/*
 * tests/test_tui.c — TUI logic tests (screen rendering + input parsing).
 * No terminal required: tui_new() works on a pipe (raw mode just stays
 * inactive) and the renderer is pure text.
 */

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include "report/report.h"
#include "test_common.h"
#include "tui/tui.h"

static int test_model_and_render(void) {
    TuiModel m;
    tui_model_init(&m);
    m.rows = 8;
    m.cols = 40;

    tui_model_append(&m, LINE_USER, "hello");
    tui_model_append(&m, LINE_ASSISTANT, "hi there");
    tui_model_append(&m, LINE_TOOL, "read file.c");
    string_append(&m.input, "typed input");
    m.cursor = 5;
    string_append(&m.status, "ready");

    String screen = string_new();
    int crow = 0, ccol = 0;
    tui_render_screen(&m, &screen, &crow, &ccol);

    /* header + history viewport (8 - header - two footer rows = 5) */
    CHECK(strstr(screen.data, "cagent") != NULL);
    CHECK(strstr(screen.data, "hello") != NULL);
    CHECK(strstr(screen.data, "hi there") != NULL);
    CHECK(strstr(screen.data, "read file.c") != NULL);
    CHECK(strstr(screen.data, "ready") != NULL);
    CHECK(strstr(screen.data, "> typed input") != NULL);

    /* cursor: input line is the last row, cursor after "> " + 5 chars */
    CHECK(crow == 8);
    CHECK(ccol == 3 + 5);

    string_free(&screen);
    tui_model_free(&m);
    return g_failures;
}

static int test_row_sections(void) {
    TuiModel m;
    tui_model_init(&m);
    m.rows = 8;
    m.cols = 40;

    String screen = string_new();
    int row = 0, col = 0;
    TuiRowSection sections[9];
    for (size_t i = 0; i < 9; i++) {
        sections[i] = TUI_ROW_HEADER;
    }
    tui_render_screen_sections(&m, &screen, &row, &col, sections, 8);
    CHECK(sections[0] == TUI_ROW_HEADER);
    for (size_t i = 1; i <= 5; i++) {
        CHECK(sections[i] == TUI_ROW_BODY);
    }
    CHECK(sections[6] == TUI_ROW_STATUS);
    CHECK(sections[7] == TUI_ROW_INPUT);
    CHECK(sections[8] == TUI_ROW_HEADER); /* capacity guard remains untouched */

    for (int rows = 1; rows <= 3; rows++) {
        m.rows = rows;
        string_clear(&screen);
        for (size_t i = 0; i < 3; i++) {
            sections[i] = TUI_ROW_BODY;
        }
        tui_render_screen_sections(&m, &screen, &row, &col, sections, (size_t)rows);
        size_t newlines = 0;
        for (size_t i = 0; i < screen.len; i++) {
            if (screen.data[i] == '\n') {
                newlines++;
            }
        }
        CHECK(newlines == (size_t)rows);
        CHECK(row == rows);
        CHECK(sections[rows - 1] == TUI_ROW_INPUT);
        if (rows >= 2) {
            CHECK(sections[rows - 2] == TUI_ROW_STATUS);
        }
        if (rows >= 3) {
            CHECK(sections[0] == TUI_ROW_HEADER);
        }
    }

    string_free(&screen);
    tui_model_free(&m);
    return g_failures;
}

static int test_streaming_and_history_replay(void) {
    TuiModel model;
    tui_model_init(&model);
    tui_model_append_stream_n(&model, "hello", 5);
    tui_model_append_stream_n(&model, " world", 6);
    CHECK(model.lines.len == 1);
    ScreenLine* streamed = vector_at(&model.lines, 0);
    CHECK(streamed != NULL && streamed->kind == LINE_ASSISTANT);
    CHECK(streamed != NULL && strcmp(streamed->text.data, "hello world") == 0);
    tui_model_append(&model, LINE_TOOL, "read {}");
    tui_model_append_stream_n(&model, "next", 4);
    tui_model_append(&model, LINE_SYSTEM, "\x1b]52;clipboard\x07");
    CHECK(model.lines.len == 4);
    ScreenLine* sanitized = vector_at(&model.lines, 3);
    CHECK(sanitized != NULL && strstr(sanitized->text.data, "\\x1B") != NULL);
    CHECK(sanitized != NULL && strchr(sanitized->text.data, '\x1b') == NULL);
    tui_model_free(&model);

    MessageList history = {0};
    Message* user = message_new(MSG_USER);
    CHECK(user != NULL);
    CHECK(message_set_content(user, "inspect") == AGENT_OK);
    CHECK(message_list_append(&history, user) == AGENT_OK);
    Message* assistant = message_new(MSG_ASSISTANT);
    CHECK(assistant != NULL);
    CHECK(message_set_content(assistant, "working") == AGENT_OK);
    ToolCall* call = calloc(1, sizeof(ToolCall));
    CHECK(call != NULL);
    if (call != NULL) {
        call->id = strdup("call-1");
        call->name = strdup("read");
        call->arguments = strdup("{\"path\":\"a.c\"}");
        CHECK(tool_call_list_append(&assistant->tool_calls, call) == AGENT_OK);
    }
    CHECK(message_list_append(&history, assistant) == AGENT_OK);
    Message* result = message_new(MSG_TOOL);
    CHECK(result != NULL);
    result->tool_call_id = strdup("call-1");
    CHECK(message_set_content(result, "ok") == AGENT_OK);
    CHECK(message_list_append(&history, result) == AGENT_OK);

    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    Tui* tui = tui_new(pipefd[0]);
    CHECK(tui != NULL);
    tui_replay_history(tui, &history);
    TuiModel* replayed = tui_model(tui);
    CHECK(replayed->lines.len == 4);
    ScreenLine* first = vector_at(&replayed->lines, 0);
    ScreenLine* second = vector_at(&replayed->lines, 1);
    ScreenLine* third = vector_at(&replayed->lines, 2);
    ScreenLine* fourth = vector_at(&replayed->lines, 3);
    CHECK(first != NULL && first->kind == LINE_USER && strcmp(first->text.data, "inspect") == 0);
    CHECK(second != NULL && second->kind == LINE_ASSISTANT &&
          strcmp(second->text.data, "working") == 0);
    CHECK(third != NULL && third->kind == LINE_TOOL && strstr(third->text.data, "read") != NULL);
    CHECK(fourth != NULL && fourth->kind == LINE_TOOL_END &&
          strstr(fourth->text.data, "read") != NULL);

    tui_free(tui);
    close(pipefd[0]);
    close(pipefd[1]);
    message_list_free(&history);
    return g_failures;
}

static int test_scrolling_keeps_tail(void) {
    TuiModel m;
    tui_model_init(&m);
    m.rows = 4; /* one history row plus header and two footer rows */
    m.cols = 40;

    for (int i = 0; i < 10; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "line-%d", i);
        tui_model_append(&m, LINE_ASSISTANT, buf);
    }

    String screen = string_new();
    int crow = 0, ccol = 0;
    tui_render_screen(&m, &screen, &crow, &ccol);

    /* follow mode shows the latest message */
    CHECK(strstr(screen.data, "line-0") == NULL);
    CHECK(strstr(screen.data, "line-8") == NULL);
    CHECK(strstr(screen.data, "line-9") != NULL);

    /* PageUp/scroll moves the middle viewport toward older messages. */
    tui_model_scroll(&m, 1);
    tui_render_screen(&m, &screen, &crow, &ccol);
    CHECK(strstr(screen.data, "line-8") != NULL);
    CHECK(strstr(screen.data, "line-9") == NULL);

    string_free(&screen);
    tui_model_free(&m);
    return g_failures;
}

static char g_submitted[64];
static int g_submit_count;

static void collect_submit(void* ud, const char* line) {
    snprintf(g_submitted, sizeof(g_submitted), "%s", line);
    g_submit_count++;
    if (ud != NULL && tui_choice_active((Tui*)ud)) {
        tui_choice_stop((Tui*)ud);
    }
}

static int test_model_choice(void) {
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    Tui* t = tui_new(pipefd[0]);
    CHECK(t != NULL);
    g_submit_count = 0;
    tui_set_callbacks(t, collect_submit, NULL, t);
    const char* labels[] = {"openai/gpt-4o", "opencode/deepseek-v4", "anthropic/claude"};
    tui_choice_start(t, labels, 3, 1);
    CHECK(tui_choice_active(t));
    CHECK(tui_choice_selected_index(t) == 1);

    /* Up/Down navigate choices instead of scrolling history. */
    tui_feed_bytes(t, "\x1b[A", 3);
    CHECK(tui_choice_selected_index(t) == 0);
    tui_feed_bytes(t, "\x1b[B", 3);
    CHECK(tui_choice_selected_index(t) == 1);
    CHECK(tui_model(t)->input.len == 0);

    /* Application-cursor mode uses SS3 (ESC O) instead of CSI (ESC [). */
    tui_feed_bytes(t, "\x1bOA", 3);
    CHECK(tui_choice_selected_index(t) == 0);
    tui_feed_bytes(t, "\x1bOB", 3);
    CHECK(tui_choice_selected_index(t) == 1);
    CHECK(tui_model(t)->input.len == 0);

    /* Typing filters with a case-insensitive subsequence query and resets
     * the selection to the first match. */
    tui_feed_bytes(t, "DEEP", 4);
    CHECK(tui_choice_match_count(tui_model(t)) == 1);
    CHECK(tui_choice_selected_index(t) == 1);
    String screen = string_new();
    int row = 0, col = 0;
    tui_model(t)->rows = 8;
    tui_model(t)->cols = 60;
    tui_render_screen(tui_model(t), &screen, &row, &col);
    CHECK(strstr(screen.data, "opencode/deepseek-v4") != NULL);
    CHECK(strstr(screen.data, "openai/gpt-4o") == NULL);

    /* Enter accepts even with a non-empty query; the selected model index is
     * available before the callback closes the choice mode. */
    tui_feed_bytes(t, "\r", 1);
    CHECK(g_submit_count == 1);
    CHECK(strcmp(g_submitted, "DEEP") == 0);
    CHECK(!tui_choice_active(t)); /* callback below stops it */

    string_free(&screen);
    close(pipefd[0]);
    close(pipefd[1]);
    tui_free(t);
    return g_failures;
}

static int test_report_render(void) {
    TuiModel m;
    tui_model_init(&m);
    m.rows = 80;
    m.cols = 120;
    m.report_mode = true;

    String screen = string_new();
    int row = 0, col = 0;
    tui_render_screen(&m, &screen, &row, &col);
    CHECK(report_overall_percent() == 86);
    CHECK(strstr(screen.data, "cagent 自举开发能力评估报告") != NULL);
    CHECK(strstr(screen.data, "总完成度: 86%") != NULL);
    CHECK(strstr(screen.data, "读-改-建-测闭环") != NULL);
    CHECK(strstr(screen.data, "安全/审批/回滚") != NULL);
    CHECK(strstr(screen.data, "规划与执行跟踪") != NULL);
    CHECK(strstr(screen.data, "记忆与状态恢复") != NULL);
    CHECK(strstr(screen.data, "工具完备性") != NULL);
    CHECK(strstr(screen.data, "自诊断") != NULL);
    CHECK(strstr(screen.data, "█") != NULL);
    CHECK(strstr(screen.data, "P0  审批与安全边界") != NULL);
    CHECK(strstr(screen.data, "推进顺序") != NULL);
    string_free(&screen);
    tui_model_free(&m);
    return g_failures;
}

static int test_report_scroll(void) {
    TuiModel m;
    tui_model_init(&m);
    m.rows = 12;
    m.cols = 100;
    m.report_mode = true;
    String first = string_new();
    String second = string_new();
    int row = 0, col = 0;
    tui_render_screen(&m, &first, &row, &col);
    m.report_scroll = 10;
    tui_render_screen(&m, &second, &row, &col);
    CHECK(strcmp(first.data, second.data) != 0);
    CHECK(strstr(second.data, "[report scrolling older content]") != NULL);
    CHECK(strstr(first.data, "推进顺序") != NULL);
    CHECK(strstr(second.data, "推进顺序") == NULL);
    string_free(&first);
    string_free(&second);
    tui_model_free(&m);
    return g_failures;
}

static int test_report_keys(void) {
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    Tui* t = tui_new(pipefd[0]);
    CHECK(t != NULL);
    g_submit_count = 0;
    tui_set_callbacks(t, collect_submit, NULL, NULL);
    tui_report_start(t);
    CHECK(tui_report_active(t));
    size_t top_scroll = tui_model(t)->report_scroll;
    tui_feed_bytes(t, "\x1b[B", 3);
    CHECK(tui_model(t)->report_scroll < top_scroll);
    tui_feed_bytes(t, "\x1b[A", 3);
    CHECK(tui_model(t)->report_scroll == top_scroll);
    tui_feed_bytes(t, "\x1b[6~", 4);
    CHECK(tui_model(t)->report_scroll < top_scroll);
    tui_feed_bytes(t, "\r", 1);
    CHECK(!tui_report_active(t));
    CHECK(g_submit_count == 0);

    tui_report_start(t);
    tui_feed_bytes(t, "\x1b", 1);
    CHECK(!tui_report_active(t));
    close(pipefd[0]);
    close(pipefd[1]);
    tui_free(t);
    return g_failures;
}

static int test_report_exit_restores(void) {
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    Tui* t = tui_new(pipefd[0]);
    CHECK(t != NULL);
    tui_model(t)->rows = 12;
    tui_model(t)->cols = 80;
    tui_model_append(tui_model(t), LINE_ASSISTANT, "dialogue survives report mode");
    tui_report_start(t);
    CHECK(tui_report_active(t));
    tui_report_stop(t);
    CHECK(!tui_report_active(t));
    CHECK(tui_model(t)->report_scroll == 0);
    String screen = string_new();
    int row = 0, col = 0;
    tui_render_screen(tui_model(t), &screen, &row, &col);
    CHECK(strstr(screen.data, "dialogue survives report mode") != NULL);
    CHECK(strstr(screen.data, "自举开发能力评估报告") == NULL);
    string_free(&screen);
    close(pipefd[0]);
    close(pipefd[1]);
    tui_free(t);
    return g_failures;
}

static int test_input_parsing(void) {
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    Tui* t = tui_new(pipefd[0]);
    CHECK(t != NULL);
    g_submit_count = 0;
    tui_set_callbacks(t, collect_submit, NULL, NULL);
    for (int i = 0; i < 12; i++) {
        char line[16];
        snprintf(line, sizeof(line), "message-%d", i);
        tui_model_append(tui_model(t), LINE_ASSISTANT, line);
    }
    tui_feed_bytes(t, "\x1b[5~", 4); /* PageUp */
    CHECK(tui_model(t)->history_scroll > 0);
    tui_feed_bytes(t, "\x1b[6~", 4); /* PageDown */
    CHECK(tui_model(t)->history_scroll == 0);

    /* UTF-8 IME input is preserved and cursor editing stays codepoint-safe. */
    tui_feed_bytes(t, "\xe4\xbd\xa0\xe5\xa5\xbd", 6); /* 你好 */
    CHECK(strcmp(tui_model(t)->input.data, "你好") == 0);
    CHECK(tui_model(t)->cursor == 6);
    tui_feed_bytes(t, "\x1b[D", 3); /* left over 好 */
    CHECK(tui_model(t)->cursor == 3);
    tui_feed_bytes(t, "\x7f", 1);
    CHECK(strcmp(tui_model(t)->input.data, "好") == 0);
    CHECK(tui_model(t)->cursor == 0);
    string_clear(&tui_model(t)->input);
    tui_model(t)->cursor = 0;

    /* plain typing */
    tui_feed_bytes(t, "abc", 3);
    CHECK(strcmp(tui_model(t)->input.data, "abc") == 0);
    CHECK(tui_model(t)->cursor == 3);

    /* backspace */
    tui_feed_bytes(t, "\x7f", 1);
    CHECK(strcmp(tui_model(t)->input.data, "ab") == 0);

    /* arrow keys: ESC [ D (left), ESC [ C (right) */
    tui_feed_bytes(t, "\x1b[D", 3);
    CHECK(tui_model(t)->cursor == 1);
    tui_feed_bytes(t, "\x1b[C", 3);
    CHECK(tui_model(t)->cursor == 2);

    /* insert in the middle */
    tui_feed_bytes(t, "\x1b[D", 3);
    tui_feed_bytes(t, "X", 1);
    CHECK(strcmp(tui_model(t)->input.data, "aXb") == 0);

    /* Enter submits the line and clears the input */
    tui_feed_bytes(t, "\r", 1);
    CHECK(g_submit_count == 1);
    CHECK(strcmp(g_submitted, "aXb") == 0);
    CHECK(tui_model(t)->input.len == 0);

    tui_set_input_secret(t, true);
    tui_feed_bytes(t, "secret-key", 10);
    String secret_screen = string_new();
    int secret_row = 0, secret_col = 0;
    tui_render_screen(tui_model(t), &secret_screen, &secret_row, &secret_col);
    CHECK(strstr(secret_screen.data, "secret-key") == NULL);
    CHECK(strstr(secret_screen.data, "**********") != NULL);
    tui_feed_bytes(t, "\r", 1);
    CHECK(strcmp(g_submitted, "secret-key") == 0);
    CHECK(tui_model(t)->input.len == 0);
    CHECK(tui_model(t)->lines.len == 13);
    string_free(&secret_screen);

    close(pipefd[0]);
    close(pipefd[1]);
    tui_free(t);
    return g_failures;
}

static int test_input_cursor_placement(void) {
    TuiModel m;
    tui_model_init(&m);
    m.rows = 4;
    m.cols = 20;

    String screen = string_new();
    int crow = 0, ccol = 0;

    /* empty input: cursor sits right after the "> " prompt */
    tui_render_screen(&m, &screen, &crow, &ccol);
    CHECK(crow == m.rows);
    CHECK(ccol == 3);

    /* mid-line ASCII cursor */
    string_append(&m.input, "abc");
    m.cursor = 2;
    tui_render_screen(&m, &screen, &crow, &ccol);
    CHECK(ccol == 3 + 2);

    /* multi-byte input: column tracks display width, not byte count. The
     * pure renderer uses wcwidth (like the TUI's runtime locale), with a
     * width-1 fallback for CJK in the C locale, so derive the expectation
     * from wcwidth itself. */
    setlocale(LC_ALL, "");
    string_clear(&m.input);
    string_append(&m.input, "\xe4\xbd\xa0\xe5\xa5\xbd"); /* 你好 */
    m.cursor = m.input.len;
    int cjk_width = wcwidth(L'\u4f60'); /* 你 */
    if (cjk_width < 1) {
        cjk_width = 1;
    }
    tui_render_screen(&m, &screen, &crow, &ccol);
    CHECK(ccol == 3 + 2 * cjk_width);

    /* cursor never exceeds the terminal width even for long input */
    string_clear(&m.input);
    for (int i = 0; i < 60; i++) {
        string_append(&m.input, "x");
    }
    m.cursor = m.input.len;
    tui_render_screen(&m, &screen, &crow, &ccol);
    CHECK(ccol >= 1 && ccol <= m.cols);

    /* secret input masks the cursor position by codepoint count: the four
     * bullets of "ab你好" land the cursor at 3 + 4 regardless of locale. */
    string_clear(&m.input);
    m.input_secret = true;
    string_append(&m.input, "ab\xe4\xbd\xa0\xe5\xa5\xbd"); /* ab你好 */
    m.cursor = m.input.len;
    tui_render_screen(&m, &screen, &crow, &ccol);
    CHECK(ccol == 3 + 4);

    string_free(&screen);
    tui_model_free(&m);
    return g_failures;
}

static int test_usage_format(void) {
    char buf[192];

    /* full form: tokens + cache-hit % + cost + context + auto (verified window) */
    CHECK(tui_format_usage(buf, sizeof(buf), 1100000, 98000, 700000, 0.15, 0.60, false, 155648,
                           272000, true) == AGENT_OK);
    CHECK(strcmp(buf, "↑1.1M ↓98k (hit 63.6%) $0.224 57.2%/272k (auto)") == 0);

    /* unverified window carries a "~" estimator marker */
    CHECK(tui_format_usage(buf, sizeof(buf), 1100000, 98000, 700000, 0.15, 0.60, false, 155648,
                           128000, false) == AGENT_OK);
    CHECK(strcmp(buf, "↑1.1M ↓98k (hit 63.6%) $0.224 121.6%/~128k (auto)") == 0);

    /* exact millions collapse: "2.0M" -> "2M" */
    CHECK(tui_format_usage(buf, sizeof(buf), 2000000, 0, 0, 0, 0, false, 0, 0, false) ==
          AGENT_OK);
    CHECK(strcmp(buf, "↑2M (auto)") == 0);

    /* small counts stay raw */
    CHECK(tui_format_usage(buf, sizeof(buf), 512, 128, 0, 0, 0, false, 0, 0, false) ==
          AGENT_OK);
    CHECK(strcmp(buf, "↑512 ↓128 (auto)") == 0);

    /* cache hits are hidden at zero and shown as a % of input between
     * ↓ and $/(sub) */
    CHECK(tui_format_usage(buf, sizeof(buf), 12000, 3000, 5000, 3.0, 15.0, true, 0, 128000,
                           false) == AGENT_OK);
    CHECK(strcmp(buf, "↑12k ↓3k (hit 41.7%) (sub) 0.0%/~128k (auto)") == 0);
    CHECK(tui_format_usage(buf, sizeof(buf), 12000, 3000, 0, 0.05, 0.1, false, 0, 0, false) ==
          AGENT_OK);
    CHECK(strcmp(buf, "↑12k ↓3k $0.0009 (auto)") == 0);

    /* sub-dollar cost keeps four decimals */
    CHECK(tui_format_usage(buf, sizeof(buf), 1000, 500, 0, 1.0, 2.0, false, 0, 0, false) ==
          AGENT_OK);
    CHECK(strcmp(buf, "↑1k ↓500 $0.0020 (auto)") == 0);

    /* zero counters degrade to the context/auto tail; unverified keeps ~ */
    CHECK(tui_format_usage(buf, sizeof(buf), 0, 0, 0, 0, 0, false, 0, 128000, false) ==
          AGENT_OK);
    CHECK(strcmp(buf, "0.0%/~128k (auto)") == 0);
    CHECK(tui_format_usage(buf, sizeof(buf), 0, 0, 0, 0, 0, false, 0, 128000, true) ==
          AGENT_OK);
    CHECK(strcmp(buf, "0.0%/128k (auto)") == 0);

    /* truncation is reported, not silent */
    CHECK(tui_format_usage(buf, 8, 2000000, 0, 0, 0, 0, false, 0, 0, false) == AGENT_ERR_IO);
    return g_failures;
}

static int test_usage_right_aligned(void) {
    TuiModel m;
    tui_model_init(&m);
    m.rows = 6;
    m.cols = 44;

    string_append(&m.status, "ready");
    string_append(&m.usage, "↑1.1M ↓98k $0.224 57.2%/272k (auto)");

    String screen = string_new();
    int crow = 0, ccol = 0;
    tui_render_screen(&m, &screen, &crow, &ccol);

    /* status row (second-to-last line; last is the input row) */
    {
        size_t end = screen.len;
        while (end > 0 && screen.data[end - 1] == '\n') {
            end--;
        }
        while (end > 0 && screen.data[end - 1] != '\n') {
            end--;
        } /* end = start of the input row */
        size_t start = end > 0 ? end - 1 : 0; /* the '\n' closing the status row */
        while (start > 0 && screen.data[start - 1] != '\n') {
            start--;
        }
        char* row = strndup(screen.data + start, end > 0 ? end - 1 - start : 0);
        CHECK(row != NULL);
        if (row != NULL) {
            CHECK(strncmp(row, "ready", 5) == 0);
            CHECK(strstr(row, "↑1.1M ↓98k $0.224 57.2%/272k (auto)") != NULL);
            /* 44 display cols; "(auto)" ends flush with the right edge:
             * byte offset 39 (0-based) = 44 - 6 + 1 */
            CHECK(strstr(row, "(auto)") == row + 39);
            free(row);
        }
    }

    string_free(&screen);
    tui_model_free(&m);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_model_and_render();
    g_failures += test_row_sections();
    g_failures += test_streaming_and_history_replay();
    g_failures += test_scrolling_keeps_tail();
    g_failures += test_model_choice();
    g_failures += test_report_render();
    g_failures += test_report_scroll();
    g_failures += test_report_keys();
    g_failures += test_report_exit_restores();
    g_failures += test_input_parsing();
    g_failures += test_input_cursor_placement();
    g_failures += test_usage_format();
    g_failures += test_usage_right_aligned();

    if (g_failures == 0) {
        printf("test_tui: all tests passed\n");
        return 0;
    }
    printf("test_tui: %d test(s) failed\n", g_failures);
    return 1;
}
