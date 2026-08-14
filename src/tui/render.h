/*
 * tui/render.h — screen model and rendering (pure logic, testable).
 *
 * The renderer turns the TUI model (history lines + input + status) into
 * a fixed-size screen buffer. It never touches the terminal itself —
 * tui.c passes the buffer and row metadata to the ncursesw backend.
 */

#ifndef CAGENT_TUI_RENDER_H
#define CAGENT_TUI_RENDER_H

#include <stdbool.h>
#include <stddef.h>

#include "util/string.h"
#include "util/vector.h"

typedef enum {
    LINE_USER,       /* user input echo */
    LINE_ASSISTANT,  /* assistant text */
    LINE_TOOL,       /* tool invocation */
    LINE_TOOL_END,   /* successful tool result marker */
    LINE_TOOL_ERROR, /* failed tool result marker */
    LINE_SYSTEM,     /* notices/errors */
} LineKind;

/* Visual section of a rendered screen row. The pure renderer tags every
 * output row so the terminal backend can style header/footer differently
 * from the scrollable message area. */
typedef enum {
    TUI_ROW_HEADER, /* fixed top bar */
    TUI_ROW_BODY,   /* scrollable message/report/choice viewport */
    TUI_ROW_STATUS, /* fixed status footer row */
    TUI_ROW_INPUT,  /* fixed input footer row */
} TuiRowSection;

typedef struct {
    LineKind kind;
    String text; /* owned */
} ScreenLine;

typedef struct {
    Vector lines;             /* ScreenLine */
    String header;            /* fixed top bar */
    String input;             /* input line text */
    bool input_secret;        /* render input as bullets and never echo on submit */
    size_t cursor;            /* input cursor offset */
    String status;            /* status footer */
    String usage;             /* context/token usage shown right-aligned on the status row */
    bool busy;                /* agent running */
    unsigned busy_frame;      /* spinner animation frame (advanced by tui_tick) */
    bool assistant_streaming; /* append text deltas to the current assistant line */
    /* Temporary interactive choice list (used by /model). */
    Vector choices; /* String entries, owned */
    bool choice_mode;
    size_t choice_selected; /* selected match ordinal after filtering */
    bool report_mode;
    size_t report_scroll;  /* visual report rows above the newest report row */
    size_t history_scroll; /* visual rows above the newest message */
    int rows, cols;
    bool quit_pending;
} TuiModel;

void tui_model_init(TuiModel* m);
void tui_model_free(TuiModel* m);

void tui_model_append(TuiModel* m, LineKind kind, const char* text);
void tui_model_append_n(TuiModel* m, LineKind kind, const char* text, size_t len);
void tui_model_append_stream_n(TuiModel* m, const char* text, size_t len);
void tui_model_end_stream(TuiModel* m);

/* Positive delta scrolls toward older messages; negative delta returns
 * toward the newest messages. Appending a message follows the tail again. */
void tui_model_scroll(TuiModel* m, int delta);
void tui_model_scroll_to_bottom(TuiModel* m);

/* Fuzzy-match the editable query against choice entries as a
 * case-insensitive UTF-8 byte subsequence. */
size_t tui_choice_match_count(const TuiModel* m);
size_t tui_choice_match_at(const TuiModel* m, size_t match_index);

/* Number of logical rows emitted by the static report body. */
size_t tui_report_content_lines(void);

/* Render a header, scrollable middle message area, and two-line footer into
 * a plain text screen (rows x cols) with an explicit cursor position. */
void tui_render_screen(const TuiModel* m, String* out, int* cursor_row, int* cursor_col);

/* Extended renderer used by the terminal frontend. When `sections` is
 * non-NULL it receives the TuiRowSection for each screen row (up to
 * `sections_cap` entries; at most `rows` are written). When `row_kinds` is
 * non-NULL it receives the LineKind of each body row so the backend can
 * style user/assistant/tool/error rows with their own colors. */
void tui_render_screen_sections(const TuiModel* m, String* out, int* cursor_row, int* cursor_col,
                                TuiRowSection* sections, size_t sections_cap);

void tui_render_screen_sections_kinds(const TuiModel* m, String* out, int* cursor_row,
                                      int* cursor_col, TuiRowSection* sections, size_t sections_cap,
                                      LineKind* row_kinds, size_t row_kinds_cap);

/* Spinner frames used by the busy status; exposed for tests. */
size_t tui_spinner_frame_count(void);
const char* tui_spinner_frame(unsigned index);

/* Format the Claude Code-style usage summary, e.g.
 *   "↑1.1M ↓98k (hit 63.6%) $20.158 57.2%/272k (auto)"
 *   "↑12.3k (sub) 45.0%/128k (auto)"
 * Segments are skipped while their data is unknown/zero (tokens, cache
 * hits, cost), so the line fills up as the session progresses. `estimate`
 * is the current context token estimate, `context_window` the model
 * window; prices are USD per 1M tokens. `window_verified` marks whether
 * context_window came from a live catalog / explicit config entry; an
 * unverified window is shown with a "~" estimator prefix instead of
 * presenting the local default as fact. Returns AGENT_OK on success,
 * AGENT_ERR_IO when the result would not fit in cap. */
int tui_format_usage(char* buf, size_t cap, int64_t tokens_in, int64_t tokens_out,
                     int64_t tokens_cached, double price_in, double price_out,
                     bool subscription, int64_t estimate, int64_t context_window,
                     bool window_verified);

#endif /* CAGENT_TUI_RENDER_H */
