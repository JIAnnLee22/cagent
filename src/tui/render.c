/*
 * tui/render.c — pure screen model and layout.
 *
 * Layout: one header row, a scrollable message viewport, and two footer
 * rows (status + input). The backend decides how this text reaches a TTY;
 * the renderer tags every row with a TuiRowSection so the backend can style
 * header/footer/body differently.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "report/report.h"
#include "tui/render.h"

/* Braille/block spinner frames. Selected for wide terminal coverage and a
 * calm, modern feel; the pure renderer stays text-only, the backend only
 * styles the row. */
static const char* const k_spinner_frames[] = {
    "\xe2\x97\x8f",        /* ● */
    "\xe2\x97\x8f\xe2\x97\x8b", /* ●○ */
    "\xe2\x97\x8f\xe2\x97\x8b\xe2\x97\x8b", /* ●○○ */
    "\xe2\x97\x8f\xe2\x97\x8b\xe2\x97\x8b\xe2\x97\x8b", /* ●○○○ */
    "\xe2\x97\x8b\xe2\x97\x8f\xe2\x97\x8b\xe2\x97\x8b", /* ○●○○ */
    "\xe2\x97\x8b\xe2\x97\x8b\xe2\x97\x8f\xe2\x97\x8b", /* ○○●○ */
    "\xe2\x97\x8b\xe2\x97\x8b\xe2\x97\x8b\xe2\x97\x8f", /* ○○○● */
};
#define SPINNER_FRAME_COUNT (sizeof(k_spinner_frames) / sizeof(k_spinner_frames[0]))

size_t tui_spinner_frame_count(void) {
    return SPINNER_FRAME_COUNT;
}

const char* tui_spinner_frame(unsigned index) {
    return k_spinner_frames[index % SPINNER_FRAME_COUNT];
}

void tui_model_init(TuiModel* m) {
    memset(m, 0, sizeof(*m));
    m->lines = vector_new(sizeof(ScreenLine));
    m->choices = vector_new(sizeof(String));
    m->header = string_new();
    m->input = string_new();
    m->status = string_new();
    m->usage = string_new();
    string_append(&m->header, " cagent");
    m->rows = 24;
    m->cols = 80;
}

void tui_model_free(TuiModel* m) {
    for (size_t i = 0; i < vector_len(&m->lines); i++) {
        ScreenLine* l = vector_at(&m->lines, i);
        string_free(&l->text);
    }
    vector_free(&m->lines);
    for (size_t i = 0; i < vector_len(&m->choices); i++) {
        String* choice = vector_at(&m->choices, i);
        string_free(choice);
    }
    vector_free(&m->choices);
    string_free(&m->header);
    string_free(&m->input);
    string_free(&m->status);
    string_free(&m->usage);
}

static int append_display_text(String* out, const char* text, size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if ((ch < 0x20 && ch != '\n' && ch != '\t') || ch == 0x7f) {
            char escaped[4] = {'\\', 'x', hex[ch >> 4], hex[ch & 0x0f]};
            if (string_append_n(out, escaped, sizeof(escaped)) != AGENT_OK)
                return AGENT_ERR_OOM;
        } else if (string_append_char(out, (char)ch) != AGENT_OK) {
            return AGENT_ERR_OOM;
        }
    }
    return AGENT_OK;
}

void tui_model_append_n(TuiModel* m, LineKind kind, const char* text, size_t len) {
    if (m == NULL) {
        return;
    }
    m->assistant_streaming = false;
    ScreenLine line = {.kind = kind, .text = string_new()};
    if (text != NULL) {
        (void)append_display_text(&line.text, text, len);
    }
    if (vector_push(&m->lines, &line) == NULL) {
        string_free(&line.text);
    } else {
        m->history_scroll = 0; /* follow newly arrived messages */
    }
}

void tui_model_append(TuiModel* m, LineKind kind, const char* text) {
    tui_model_append_n(m, kind, text, text != NULL ? strlen(text) : 0);
}

void tui_model_append_stream_n(TuiModel* m, const char* text, size_t len) {
    if (m == NULL || text == NULL || len == 0) {
        return;
    }
    size_t count = vector_len(&m->lines);
    if (m->assistant_streaming && count > 0) {
        ScreenLine* last = vector_at(&m->lines, count - 1);
        if (last != NULL && last->kind == LINE_ASSISTANT) {
            (void)append_display_text(&last->text, text, len);
            m->history_scroll = 0;
            return;
        }
    }
    tui_model_append_n(m, LINE_ASSISTANT, text, len);
    m->assistant_streaming = true;
}

void tui_model_end_stream(TuiModel* m) {
    if (m != NULL) {
        m->assistant_streaming = false;
    }
}

void tui_model_scroll(TuiModel* m, int delta) {
    if (m == NULL || delta == 0) {
        return;
    }
    if (delta > 0) {
        size_t amount = (size_t)delta;
        if (SIZE_MAX - m->history_scroll < amount) {
            m->history_scroll = SIZE_MAX;
        } else {
            m->history_scroll += amount;
        }
    } else {
        size_t amount = (size_t)(-(int64_t)delta);
        m->history_scroll = m->history_scroll > amount ? m->history_scroll - amount : 0;
    }
}

void tui_model_scroll_to_bottom(TuiModel* m) {
    if (m != NULL) {
        m->history_scroll = 0;
    }
}

static bool choice_matches(const String* choice, const String* query) {
    if (choice == NULL || query == NULL) {
        return false;
    }
    size_t candidate = 0;
    for (size_t q = 0; q < query->len; q++) {
        unsigned char wanted = (unsigned char)query->data[q];
        bool found = false;
        while (candidate < choice->len) {
            unsigned char got = (unsigned char)choice->data[candidate++];
            if (tolower(wanted) == tolower(got)) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

size_t tui_choice_match_count(const TuiModel* m) {
    if (m == NULL || !m->choice_mode) {
        return 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < vector_len(&m->choices); i++) {
        const String* choice = vector_at(&m->choices, i);
        if (choice_matches(choice, &m->input)) {
            count++;
        }
    }
    return count;
}

size_t tui_choice_match_at(const TuiModel* m, size_t match_index) {
    if (m == NULL || !m->choice_mode) {
        return SIZE_MAX;
    }
    size_t matched = 0;
    for (size_t i = 0; i < vector_len(&m->choices); i++) {
        const String* choice = vector_at(&m->choices, i);
        if (!choice_matches(choice, &m->input)) {
            continue;
        }
        if (matched == match_index) {
            return i;
        }
        matched++;
    }
    return SIZE_MAX;
}

static size_t utf8_fit_width(const char* data, size_t len, int max_width);

static void report_append_bar(String* out, int current) {
    int filled = current * 20 / 100;
    string_append(out, "[");
    for (int i = 0; i < 20; i++) {
        string_append(out, i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91");
    }
    string_append(out, "]");
}

static void report_append_body(String* out) {
    const ReportDimension* dimensions = report_dimensions();
    const ReportGap* gaps = report_gaps();
    string_printf(out, "%s\n", report_title());
    string_printf(out, "%s\n", report_conclusion());
    string_printf(out, "总完成度: %d%%（按维度权重计算）\n", report_overall_percent());
    string_append(out, "\n能力维度\n");
    for (size_t i = 0; i < report_dimension_count(); i++) {
        string_printf(out, "%-18s ", dimensions[i].label);
        report_append_bar(out, dimensions[i].current);
        string_printf(out, " %d%%（权重 %d%%）\n", dimensions[i].current, dimensions[i].weight);
    }

    string_append(out, "\n分级缺口\n");
    for (size_t i = 0; i < report_gap_count(); i++) {
        const ReportGap* gap = &gaps[i];
        string_printf(out, "P%d  %s（当前 %d%%）\n", gap->priority, gap->title, gap->completeness);
        for (size_t j = 0; j < gap->items_len; j++) {
            string_printf(out, "  - %s\n", gap->items[j]);
        }
    }

    string_append(out, "\n已具备\n");
    const char* const* done = report_dones();
    for (size_t i = 0; i < report_done_count(); i++) {
        string_printf(out, "  + %s\n", done[i]);
    }

    string_append(out, "\n推进顺序\n");
    const char* const* roadmap = report_roadmap();
    for (size_t i = 0; i < report_roadmap_count(); i++) {
        string_printf(out, "  %zu. %s\n", i + 1, roadmap[i]);
    }
}

size_t tui_report_content_lines(void) {
    size_t lines = 4;                      /* title, conclusion, total, blank */
    lines += 1 + report_dimension_count(); /* section title + dimensions */
    lines += 1 + 1;                        /* blank + gap section title */
    for (size_t i = 0; i < report_gap_count(); i++) {
        const ReportGap* gap = &report_gaps()[i];
        lines += 1 + gap->items_len;
    }
    lines += 1 + 1 + report_done_count();    /* blank + section + done items */
    lines += 1 + 1 + report_roadmap_count(); /* blank + section + roadmap */
    return lines;
}

/* Wrap text to display cells, never splitting a UTF-8 codepoint. */
static void append_wrapped(String* out, const char* text, size_t len, int cols, bool prefix) {
    size_t off = 0;
    bool first = true;
    int width = cols > 0 ? cols : 1;
    int available = width - (prefix ? 2 : 0);
    if (available < 1) {
        available = 1;
    }
    while (off < len || first) {
        if (first && prefix) {
            string_append(out, "> ");
        }
        size_t take = utf8_fit_width(text + off, len - off, available);
        if (take == 0 && off < len) {
            /* Make progress for malformed/incomplete UTF-8. */
            take = 1;
        }
        if (take > 0) {
            string_append_n(out, text + off, take);
            off += take;
        }
        string_append_char(out, '\n');
        first = false;
    }
}

/* Visual badge prefix per line kind. These are pure text so the renderer
 * stays testable; the terminal backend may restyle them with color. */
static const char* line_badge(LineKind kind) {
    switch (kind) {
    case LINE_USER:
        return "\xe2\x9d\xaf "; /* ❯ */
    case LINE_ASSISTANT:
        return "\xe2\x97\x89 "; /* ◉ */
    case LINE_TOOL:
        return "\xe2\x9a\xa1 "; /* ⚡ */
    case LINE_TOOL_END:
        return "\xe2\x9c\x93 "; /* ✓ */
    case LINE_TOOL_ERROR:
        return "\xe2\x9c\x97 "; /* ✗ */
    case LINE_SYSTEM:
    default:
        return "! ";
    }
}

static void append_line(String* out, const ScreenLine* l, int cols) {
    bool prefix = l->kind == LINE_USER;
    if (prefix) {
        /* historical user lines keep the compact "> " marker */
        append_wrapped(out, l->text.data, l->text.len, cols, true);
    } else {
        const char* badge = line_badge(l->kind);
        string_append(out, badge);
        append_wrapped(out, l->text.data, l->text.len, cols - 2, false);
    }
}

static void append_clipped(String* out, const String* text, int cols) {
    if (text == NULL || cols <= 0) {
        return;
    }
    size_t n = utf8_fit_width(text->data, text->len, cols);
    string_append_n(out, text->data, n);
}

/* ---- statusline usage summary ---------------------------------------- */

/* 1234 -> "1.2k", 1100000 -> "1.1M", 2000000 -> "2M" */
static void append_scaled(String* s, int64_t n) {
    char buf[32];
    if (n >= 1000000) {
        snprintf(buf, sizeof(buf), "%.1fM", (double)n / 1000000.0);
        size_t bl = strlen(buf);
        if (bl > 3 && buf[bl - 3] == '.' && buf[bl - 2] == '0') {
            memmove(buf + bl - 3, buf + bl - 1, 2); /* "2.0M" -> "2M" */
        }
        string_append(s, buf);
    } else if (n >= 1000) {
        snprintf(buf, sizeof(buf), "%lldk", (long long)((n + 500) / 1000));
        string_append(s, buf);
    } else {
        snprintf(buf, sizeof(buf), "%lld", (long long)n);
        string_append(s, buf);
    }
}

int tui_format_usage(char* buf, size_t cap, int64_t tokens_in, int64_t tokens_out,
                     int64_t tokens_cached, double price_in, double price_out,
                     bool subscription, int64_t estimate, int64_t context_window,
                     bool window_verified) {
    if (buf == NULL || cap == 0) {
        return AGENT_ERR_IO;
    }
    String s = string_new();
    /* join segments with single spaces; (auto) always ends the line */
    bool any = false;
    if (tokens_in > 0) {
        if (any)
            string_append_char(&s, ' ');
        string_append(&s, "\xe2\x86\x91"); /* ↑ */
        append_scaled(&s, tokens_in);
        any = true;
    }
    if (tokens_out > 0) {
        if (any)
            string_append_char(&s, ' ');
        string_append(&s, "\xe2\x86\x93"); /* ↓ */
        append_scaled(&s, tokens_out);
        any = true;
    }
    if (tokens_cached > 0 && tokens_in > 0) {
        if (any)
            string_append_char(&s, ' ');
        double hit = (double)tokens_cached / (double)tokens_in * 100.0;
        if (hit > 999.9) {
            hit = 999.9;
        }
        char c[48];
        snprintf(c, sizeof(c), "(hit %.1f%%)", hit);
        string_append(&s, c);
        any = true;
    }
    if (subscription) {
        if (any)
            string_append_char(&s, ' ');
        string_append(&s, "(sub)");
        any = true;
    } else if (price_in > 0 || price_out > 0) {
        double cost = (double)tokens_in / 1e6 * price_in + (double)tokens_out / 1e6 * price_out;
        if (cost > 0) {
            if (any)
                string_append_char(&s, ' ');
            char c[48];
            snprintf(c, sizeof(c), cost < 0.01 ? "$%.4f" : "$%.3f", cost);
            string_append(&s, c);
            any = true;
        }
    }
    if (context_window > 0) {
        if (any)
            string_append_char(&s, ' ');
        double pct = (double)estimate / (double)context_window * 100.0;
        if (pct > 999.9) {
            pct = 999.9;
        }
        char c[48];
        snprintf(c, sizeof(c), "%.1f%%/", pct);
        string_append(&s, c);
        if (!window_verified) {
            string_append_char(&s, '~'); /* local default, not catalog-confirmed */
        }
        append_scaled(&s, context_window);
        any = true;
    }
    if (any)
        string_append_char(&s, ' ');
    string_append(&s, "(auto)");

    int rc = AGENT_OK;
    if (s.len + 1 > cap) {
        rc = AGENT_ERR_IO;
    } else {
        memcpy(buf, s.data, s.len + 1);
    }
    string_free(&s);
    return rc;
}

static int utf8_char_width(const char* data, size_t len, size_t off, size_t* next) {
    mbstate_t state = {0};
    wchar_t wc = 0;
    size_t n = mbrtowc(&wc, data + off, len - off, &state);
    if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
        *next = off + 1;
        return 1;
    }
    *next = off + n;
    int width = wcwidth(wc);
    return width > 0 ? width : 1;
}

static size_t utf8_fit_width(const char* data, size_t len, int max_width) {
    size_t off = 0;
    int width = 0;
    while (off < len && width < max_width) {
        size_t next = off;
        int char_width = utf8_char_width(data, len, off, &next);
        if (width + char_width > max_width) {
            break;
        }
        off = next;
        width += char_width;
    }
    return off;
}

static int utf8_width(const char* data, size_t len) {
    size_t off = 0;
    int width = 0;
    while (off < len) {
        size_t next = off;
        width += utf8_char_width(data, len, off, &next);
        off = next;
    }
    return width;
}

void tui_render_screen_sections(const TuiModel* m, String* out, int* cursor_row, int* cursor_col,
                                TuiRowSection* sections, size_t sections_cap) {
    *cursor_row = 0;
    *cursor_col = 0;
    string_clear(out);

    int rows = m->rows > 0 ? m->rows : 1;
    int cols = m->cols > 0 ? m->cols : 1;
    bool show_header = rows >= 3;
    bool show_status = rows >= 2;
    int middle_rows = rows - (show_header ? 1 : 0) - (show_status ? 1 : 0) - 1;

    /* Degrade tiny terminals to input-only or status+input layouts while
     * keeping the output and section arrays bounded by the actual row count. */
    if (sections != NULL && sections_cap > 0) {
        size_t n = (size_t)rows < sections_cap ? (size_t)rows : sections_cap;
        for (size_t i = 0; i < n; i++) {
            sections[i] = i == (size_t)rows - 1              ? TUI_ROW_INPUT
                          : show_status && i == (size_t)rows - 2 ? TUI_ROW_STATUS
                          : show_header && i == 0                ? TUI_ROW_HEADER
                                                                  : TUI_ROW_BODY;
        }
    }

    if (show_header) {
        append_clipped(out, &m->header, cols);
        string_append_char(out, '\n');
    }

    int emitted = 0;
    size_t scroll = 0;
    if (m->report_mode) {
        String all = string_new();
        report_append_body(&all);
        int total = 0;
        for (size_t i = 0; i < all.len; i++) {
            if (all.data[i] == '\n') {
                total++;
            }
        }
        int max_scroll = total > middle_rows ? total - middle_rows : 0;
        scroll = m->report_scroll > (size_t)max_scroll ? (size_t)max_scroll : m->report_scroll;
        int end_line = total - (int)scroll;
        int start_line = end_line - middle_rows;
        if (start_line < 0) {
            start_line = 0;
        }
        int line_no = 0;
        size_t start = 0;
        for (size_t i = 0; i <= all.len; i++) {
            if (i == all.len || all.data[i] == '\n') {
                if (line_no >= start_line && line_no < end_line) {
                    string_append_n(out, all.data + start, i - start);
                    string_append_char(out, '\n');
                    emitted++;
                }
                start = i + 1;
                line_no++;
            }
        }
        string_free(&all);
    } else if (m->choice_mode) {
        size_t matches = tui_choice_match_count(m);
        size_t selected = m->choice_selected;
        if (matches > 0 && selected >= matches) {
            selected = matches - 1;
        }
        size_t start = 0;
        if (matches > (size_t)middle_rows && selected >= (size_t)middle_rows) {
            start = selected - (size_t)middle_rows + 1;
        }
        size_t end = matches < start + (size_t)middle_rows ? matches : start + (size_t)middle_rows;
        for (size_t match = start; match < end; match++) {
            size_t choice_index = tui_choice_match_at(m, match);
            const String* choice = vector_at(&m->choices, choice_index);
            string_append(out, match == selected ? "> " : "  ");
            append_clipped(out, choice, cols - 2);
            string_append_char(out, '\n');
            emitted++;
        }
        if (matches == 0 && middle_rows > 0) {
            string_append(out, "  (no matching models)\n");
            emitted++;
        }
    } else {
        String all = string_new();
        for (size_t i = 0; i < vector_len(&m->lines); i++) {
            append_line(&all, vector_at(&m->lines, i), cols);
        }

        int total = 0;
        for (size_t i = 0; i < all.len; i++) {
            if (all.data[i] == '\n') {
                total++;
            }
        }
        int max_scroll = total > middle_rows ? total - middle_rows : 0;
        scroll = m->history_scroll > (size_t)max_scroll ? (size_t)max_scroll : m->history_scroll;
        int end_line = total - (int)scroll;
        int start_line = end_line - middle_rows;
        if (start_line < 0) {
            start_line = 0;
        }

        int line_no = 0;
        size_t start = 0;
        for (size_t i = 0; i <= all.len; i++) {
            if (i == all.len || all.data[i] == '\n') {
                if (line_no >= start_line && line_no < end_line) {
                    string_append_n(out, all.data + start, i - start);
                    string_append_char(out, '\n');
                    emitted++;
                }
                start = i + 1;
                line_no++;
            }
        }
        string_free(&all);
    }
    while (emitted < middle_rows) {
        string_append_char(out, '\n');
        emitted++;
    }

    if (show_status) {
        if (m->report_mode && scroll > 0) {
            string_append(out, "[report scrolling older content] ");
        } else if (!m->choice_mode && scroll > 0) {
            string_append(out, "[scrolling older messages] ");
        }
        size_t usage_width = utf8_width(m->usage.data, m->usage.len);
        if (usage_width > 0 && usage_width + 1 < (size_t)cols) {
            /* status left, usage right-aligned on the same row */
            append_clipped(out, &m->status, cols - (int)usage_width - 1);
            string_append_char(out, ' ');
            size_t take = utf8_fit_width(m->usage.data, m->usage.len, cols);
            string_append_n(out, m->usage.data, take);
        } else {
            append_clipped(out, &m->status, cols);
        }
        string_append_char(out, '\n');
    }

    string_append(out, "> ");
    int input_width = cols - 2;
    if (input_width < 0) {
        input_width = 0;
    }
    String display_input = string_new();
    if (!m->report_mode && m->input_secret) {
        for (size_t i = 0; i < m->input.len; i++) {
            unsigned char c = (unsigned char)m->input.data[i];
            if ((c & 0xc0U) != 0x80U) {
                string_append_char(&display_input, '*');
            }
        }
    } else if (!m->report_mode) {
        string_append_n(&display_input, m->input.data, m->input.len);
    }
    size_t shown = utf8_fit_width(display_input.data, display_input.len, input_width);
    string_append_n(out, display_input.data, shown);
    string_append_char(out, '\n');

    *cursor_row = rows;
    size_t cursor = m->report_mode ? 0 : (m->cursor < m->input.len ? m->cursor : m->input.len);
    if (!m->report_mode && m->input_secret) {
        size_t masked_cursor = 0;
        for (size_t i = 0; i < cursor; i++) {
            if ((((unsigned char)m->input.data[i]) & 0xc0U) != 0x80U) {
                masked_cursor++;
            }
        }
        cursor = masked_cursor;
    }
    if (cursor > shown) {
        cursor = shown;
    }
    *cursor_col = 3 + utf8_width(display_input.data, cursor);
    string_free(&display_input);
    if (*cursor_col > cols) {
        *cursor_col = cols;
    }
    if (*cursor_col < 1) {
        *cursor_col = 1;
    }
}

void tui_render_screen(const TuiModel* m, String* out, int* cursor_row, int* cursor_col) {
    tui_render_screen_sections(m, out, cursor_row, cursor_col, NULL, 0);
}
