/*
 * tui/tui.c — terminal UI implementation.
 *
 * Input is UTF-8 byte-stream driven with a tiny escape-sequence state
 * machine (arrow keys, Home/End). Cursor offsets remain byte offsets so
 * submitted strings are preserved exactly; editing moves across codepoints.
 * Full lines are handed to the submit callback;
 * Ctrl+C goes to the cancel callback (the main loop decides whether that
 * cancels the agent or quits).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tui/terminal.h"
#include "tui/tui.h"
#include "util/log.h"

#define TUI_INPUT_MAX 8192

struct Tui {
    int fd;
    struct termios saved;
    bool raw_active;
    TuiModel model;
    TuiSubmitCb submit;
    TuiCancelCb cancel;
    TuiCancelCb choice_cancel;
    TuiCancelCb report_cancel;
    void* ud;
    /* escape sequence parsing */
    int esc_state; /* 0 none, 1 saw ESC, 2 saw CSI/SS3 prefix, 3 saw ESC[number */
    int esc_code;
};

Tui* tui_new(int fd) {
    Tui* t = calloc(1, sizeof(Tui));
    if (t == NULL) {
        return NULL;
    }
    t->fd = fd;
    tui_model_init(&t->model);
    if (terminal_raw_mode(fd, &t->saved) == AGENT_OK) {
        t->raw_active = true;
        terminal_hide_cursor(fd);
    }
    terminal_get_size(fd, &t->model.rows, &t->model.cols);
    return t;
}

void tui_free(Tui* t) {
    if (t == NULL) {
        return;
    }
    if (t->raw_active) {
        terminal_show_cursor(t->fd);
        terminal_restore(t->fd, &t->saved);
    }
    tui_model_free(&t->model);
    free(t);
}

void tui_set_callbacks(Tui* t, TuiSubmitCb submit, TuiCancelCb cancel, void* ud) {
    t->submit = submit;
    t->cancel = cancel;
    t->ud = ud;
}

void tui_set_header(Tui* t, const char* header) {
    if (t == NULL) {
        return;
    }
    string_clear(&t->model.header);
    if (header != NULL) {
        string_append(&t->model.header, header);
    }
    tui_render(t);
}

TuiModel* tui_model(Tui* t) {
    return t != NULL ? &t->model : NULL;
}

static const char* history_tool_name(const MessageList* messages, size_t before,
                                     const char* tool_call_id) {
    if (messages == NULL || tool_call_id == NULL) {
        return "tool";
    }
    for (size_t i = before; i > 0; i--) {
        const Message* msg = &messages->items[i - 1];
        if (msg->role != MSG_ASSISTANT) {
            continue;
        }
        for (size_t j = 0; j < msg->tool_calls.len; j++) {
            const ToolCall* tc = &msg->tool_calls.items[j];
            if (tc->id != NULL && strcmp(tc->id, tool_call_id) == 0) {
                return tc->name != NULL ? tc->name : "tool";
            }
        }
    }
    return "tool";
}

void tui_replay_history(Tui* t, const MessageList* messages) {
    if (t == NULL || messages == NULL) {
        return;
    }
    for (size_t i = 0; i < messages->len; i++) {
        const Message* msg = &messages->items[i];
        switch (msg->role) {
        case MSG_USER:
            if (msg->content != NULL) {
                tui_model_append(&t->model, LINE_USER, msg->content);
            }
            break;
        case MSG_ASSISTANT:
            if (msg->content != NULL && msg->content[0] != '\0') {
                tui_model_append(&t->model, LINE_ASSISTANT, msg->content);
            }
            for (size_t j = 0; j < msg->tool_calls.len; j++) {
                const ToolCall* tc = &msg->tool_calls.items[j];
                String line = string_new();
                string_printf(&line, "%s %s", tc->name != NULL ? tc->name : "tool",
                              tc->arguments != NULL ? tc->arguments : "{}");
                tui_model_append_n(&t->model, LINE_TOOL, line.data, line.len);
                string_free(&line);
            }
            break;
        case MSG_TOOL: {
            String line = string_new();
            string_printf(&line, "%s %s", msg->is_error ? "\xe2\x9c\x97" : "\xe2\x9c\x93",
                          history_tool_name(messages, i, msg->tool_call_id));
            tui_model_append_n(&t->model, LINE_TOOL_END, line.data, line.len);
            string_free(&line);
            break;
        }
        case MSG_SYSTEM:
            break; /* project/system instructions are intentionally hidden */
        }
    }
    tui_model_end_stream(&t->model);
    tui_render(t);
}

/* ---- agent events ------------------------------------------------------ */

void tui_on_agent_event(void* ud, const AgentEvent* ev) {
    Tui* t = ud;
    TuiModel* m = &t->model;

    switch (ev->type) {
    case AGENT_EVT_TEXT:
        if (ev->text != NULL) {
            size_t len = ev->text_len != 0 ? ev->text_len : strlen(ev->text);
            tui_model_append_stream_n(m, ev->text, len);
        }
        break;
    case AGENT_EVT_TOOL_START: {
        String line = string_new();
        string_printf(&line, "%s %s", ev->name != NULL ? ev->name : "?",
                      ev->text != NULL ? ev->text : "");
        tui_model_append_n(m, LINE_TOOL, line.data, line.len);
        string_free(&line);
        break;
    }
    case AGENT_EVT_TOOL_APPROVAL: {
        String line = string_new();
        string_printf(&line,
                      "审批请求：%s\n变更预览：\n%s\n输入 /approve 执行，/reject 拒绝，"
                      "或 /trust on 开启本进程自动批准",
                      ev->name != NULL ? ev->name : "?",
                      ev->preview != NULL ? ev->preview : (ev->text != NULL ? ev->text : "{}"));
        tui_model_append_n(m, LINE_SYSTEM, line.data, line.len);
        string_free(&line);
        break;
    }
    case AGENT_EVT_TOOL_END: {
        String line = string_new();
        string_printf(&line, "%s %s", ev->is_error ? "\xe2\x9c\x97" : "\xe2\x9c\x93",
                      ev->name != NULL ? ev->name : "?");
        tui_model_append_n(m, LINE_TOOL_END, line.data, line.len);
        string_free(&line);
        break;
    }
    case AGENT_EVT_STATUS:
        break; /* the application maps this event to the status bar */
    case AGENT_EVT_ERROR:
        if (ev->text != NULL) {
            tui_model_append(m, LINE_SYSTEM, ev->text);
        }
        break;
    }
    tui_render(t);
}

/* ---- input ------------------------------------------------------------- */

static void tui_insert_char(Tui* t, char c) {
    TuiModel* m = &t->model;
    if (m->input.len >= TUI_INPUT_MAX) {
        return;
    }
    if (m->cursor > m->input.len) {
        m->cursor = m->input.len;
    }
    /* insert at cursor; append one byte to grow the buffer, but keep the
     * logical length separate from the temporary terminator. */
    size_t old_len = m->input.len;
    if (string_append_char(&m->input, '\0') != AGENT_OK) {
        return;
    }
    memmove(m->input.data + m->cursor + 1, m->input.data + m->cursor,
            old_len - m->cursor + 1); /* include the NUL terminator */
    m->input.data[m->cursor] = c;
    m->input.len = old_len + 1;
    m->input.data[m->input.len] = '\0';
    m->cursor++;
    if (m->choice_mode) {
        m->choice_selected = 0;
    }
}

static bool utf8_continuation(unsigned char c) {
    return (c & 0xc0U) == 0x80U;
}

/* Cursor positions are byte offsets, but editing must not split UTF-8. */
static size_t utf8_prev(const char* data, size_t pos) {
    if (data == NULL || pos == 0) {
        return 0;
    }
    size_t start = pos - 1;
    while (start > 0 && utf8_continuation((unsigned char)data[start])) {
        start--;
    }
    return start;
}

static size_t utf8_next(const char* data, size_t len, size_t pos) {
    if (data == NULL || pos >= len) {
        return len;
    }
    size_t next = pos + 1;
    while (next < len && utf8_continuation((unsigned char)data[next])) {
        next++;
    }
    return next;
}

static void tui_backspace(Tui* t) {
    TuiModel* m = &t->model;
    if (m->cursor == 0 || m->input.len == 0) {
        return;
    }
    if (m->cursor > m->input.len) {
        m->cursor = m->input.len;
    }
    size_t start = utf8_prev(m->input.data, m->cursor);
    memmove(m->input.data + start, m->input.data + m->cursor, m->input.len - m->cursor);
    m->input.len -= m->cursor - start;
    m->input.data[m->input.len] = '\0';
    m->cursor = start;
    if (m->choice_mode) {
        m->choice_selected = 0;
    }
}

static void tui_submit_line(Tui* t) {
    TuiModel* m = &t->model;
    if (m->input.len == 0 && !m->choice_mode) {
        return;
    }
    String submitted = string_new();
    if (string_append_n(&submitted, m->input.data, m->input.len) != AGENT_OK) {
        return;
    }
    /* Choice callbacks need the current filtered query to resolve the
     * selected entry. The query is not secret and the callback normally
     * closes the choice mode, which clears it. */
    bool choice = m->choice_mode;
    if (!m->input_secret) {
        tui_model_append(m, LINE_USER, submitted.data);
    }
    if (choice && t->submit != NULL) {
        t->submit(t->ud, submitted.data);
    } else {
        /* Clear the editable buffer before ordinary callbacks so a secret can
         * never be rendered in plaintext when the callback changes mode. */
        string_clear(&m->input);
        m->cursor = 0;
        if (t->submit != NULL) {
            t->submit(t->ud, submitted.data);
        }
    }
    string_free(&submitted);
}

static void tui_report_request_close(Tui* t) {
    if (t->report_cancel != NULL) {
        t->report_cancel(t->ud);
    } else {
        tui_report_stop(t);
    }
}

void tui_feed_bytes(Tui* t, const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (t->esc_state) {
        case 0:
            if (c == 0x1b) {
                /* Delay ESC handling until we know whether this is an arrow
                 * sequence. A standalone ESC is finalized after this input
                 * chunk; this keeps choice/report arrows usable. */
                t->esc_state = 1;
            } else if (c == '\r' || c == '\n') {
                if (t->model.report_mode) {
                    tui_report_request_close(t);
                } else {
                    tui_submit_line(t);
                }
            } else if (c == 0x7f || c == '\b') {
                if (!t->model.report_mode) {
                    tui_backspace(t);
                }
            } else if (c == 3) { /* Ctrl+C */
                if (t->model.report_mode) {
                    tui_report_request_close(t);
                } else if (t->model.choice_mode && t->choice_cancel != NULL) {
                    t->choice_cancel(t->ud);
                } else if (t->cancel != NULL) {
                    t->cancel(t->ud);
                }
            } else if (c == 4) { /* Ctrl+D */
                if (t->model.report_mode) {
                    tui_report_request_close(t);
                } else if (t->model.choice_mode && t->choice_cancel != NULL) {
                    t->choice_cancel(t->ud);
                } else if (t->submit != NULL) {
                    t->submit(t->ud, "/quit");
                }
            } else if (c >= 32 && c != 0x7f && !t->model.report_mode) {
                /* Keep UTF-8 bytes verbatim; terminal IMEs deliver committed
                 * Chinese text as UTF-8 after raw mode is enabled. */
                tui_insert_char(t, (char)c);
            }
            break;
        case 1:
            if (c == '[' || c == 'O') {
                /* Accept both CSI (ESC [) and SS3 (ESC O). Terminals in
                 * application-cursor mode commonly emit SS3 arrows. */
                t->esc_state = 2;
                t->esc_code = 0;
            } else {
                t->esc_state = 0;
                if (t->model.report_mode) {
                    tui_report_request_close(t);
                } else if (t->model.choice_mode && t->choice_cancel != NULL) {
                    t->choice_cancel(t->ud);
                }
            }
            break;
        case 2:
        case 3:
            if (c >= '0' && c <= '9') {
                t->esc_code = t->esc_code * 10 + (int)(c - '0');
                t->esc_state = 3;
            } else if (c == 'A') { /* up / scroll older */
                if (t->model.report_mode) {
                    tui_report_scroll(t, 1);
                } else if (t->model.choice_mode) {
                    size_t matches = tui_choice_match_count(&t->model);
                    if (matches > 0 && t->model.choice_selected > 0) {
                        t->model.choice_selected--;
                    }
                } else {
                    tui_model_scroll(&t->model, 1);
                }
                t->esc_state = 0;
            } else if (c == 'B') { /* down / scroll newer */
                if (t->model.report_mode) {
                    tui_report_scroll(t, -1);
                } else if (t->model.choice_mode) {
                    size_t matches = tui_choice_match_count(&t->model);
                    if (matches > 0 && t->model.choice_selected + 1 < matches) {
                        t->model.choice_selected++;
                    }
                } else {
                    tui_model_scroll(&t->model, -1);
                }
                t->esc_state = 0;
            } else if (c == 'D') { /* left */
                if (!t->model.report_mode) {
                    t->model.cursor = utf8_prev(t->model.input.data, t->model.cursor);
                }
                t->esc_state = 0;
            } else if (c == 'C') { /* right */
                if (!t->model.report_mode) {
                    t->model.cursor =
                        utf8_next(t->model.input.data, t->model.input.len, t->model.cursor);
                }
                t->esc_state = 0;
            } else if (c == 'H') { /* home */
                if (!t->model.report_mode) {
                    t->model.cursor = 0;
                }
                t->esc_state = 0;
            } else if (c == 'F') { /* end */
                if (!t->model.report_mode) {
                    t->model.cursor = t->model.input.len;
                }
                t->esc_state = 0;
            } else if (c == '~') {
                if (t->model.report_mode) {
                    if (t->esc_code == 5) {
                        tui_report_scroll(t, t->model.rows > 4 ? t->model.rows / 2 : 1);
                    } else if (t->esc_code == 6) {
                        tui_report_scroll(t, -(t->model.rows > 4 ? t->model.rows / 2 : 1));
                    }
                } else if (t->esc_code == 1) {
                    t->model.cursor = 0; /* Home */
                } else if (t->esc_code == 4) {
                    t->model.cursor = t->model.input.len; /* End */
                } else if (t->esc_code == 5) {
                    tui_model_scroll(&t->model, t->model.rows > 4 ? t->model.rows / 2 : 1);
                } else if (t->esc_code == 6) {
                    tui_model_scroll(&t->model, -(t->model.rows > 4 ? t->model.rows / 2 : 1));
                }
                t->esc_state = 0;
                t->esc_code = 0;
            } else {
                t->esc_state = 0;
                t->esc_code = 0;
            }
            break;
        default:
            t->esc_state = 0;
            break;
        }
    }
    if (t->esc_state == 1) {
        /* A complete read containing only ESC is a cancel key. If this was
         * the prefix of an escape sequence, state 2/3 would have won above. */
        t->esc_state = 0;
        if (t->model.report_mode) {
            tui_report_request_close(t);
        } else if (t->model.choice_mode && t->choice_cancel != NULL) {
            t->choice_cancel(t->ud);
        }
    }
    tui_render(t);
}

/* ---- status / resize / render ------------------------------------------ */

void tui_set_busy(Tui* t, bool busy) {
    t->model.busy = busy;
    string_clear(&t->model.status);
    string_append(&t->model.status, busy ? "\xe2\x97\x8f running... (Ctrl+C to cancel)"
                                         : "ready. type a message, Ctrl+D or /exit to quit");
    tui_render(t);
}

void tui_set_status(Tui* t, const char* status) {
    string_clear(&t->model.status);
    if (status != NULL) {
        string_append(&t->model.status, status);
    }
    tui_render(t);
}

void tui_set_usage(Tui* t, const char* usage) {
    string_clear(&t->model.usage);
    if (usage != NULL) {
        string_append(&t->model.usage, usage);
    }
    tui_render(t);
}

void tui_set_input_secret(Tui* t, bool secret) {
    if (t == NULL) {
        return;
    }
    t->model.input_secret = secret;
    tui_render(t);
}

void tui_set_choice_cancel_callback(Tui* t, TuiCancelCb cancel) {
    if (t != NULL) {
        t->choice_cancel = cancel;
    }
}

void tui_choice_start(Tui* t, const char* const* labels, size_t count, size_t selected) {
    if (t == NULL) {
        return;
    }
    TuiModel* m = &t->model;
    for (size_t i = 0; i < vector_len(&m->choices); i++) {
        String* choice = vector_at(&m->choices, i);
        string_free(choice);
    }
    vector_clear(&m->choices);
    for (size_t i = 0; i < count; i++) {
        String choice = string_new();
        if (labels != NULL && labels[i] != NULL) {
            string_append(&choice, labels[i]);
        }
        if (vector_push(&m->choices, &choice) == NULL) {
            string_free(&choice);
            break;
        }
    }
    m->report_mode = false;
    m->report_scroll = 0;
    m->choice_mode = true;
    m->choice_selected = selected < vector_len(&m->choices) ? selected : 0;
    string_clear(&m->input);
    m->cursor = 0;
    tui_render(t);
}

void tui_choice_stop(Tui* t) {
    if (t == NULL) {
        return;
    }
    TuiModel* m = &t->model;
    m->choice_mode = false;
    m->choice_selected = 0;
    for (size_t i = 0; i < vector_len(&m->choices); i++) {
        String* choice = vector_at(&m->choices, i);
        string_free(choice);
    }
    vector_clear(&m->choices);
    string_clear(&m->input);
    m->cursor = 0;
    tui_render(t);
}

bool tui_choice_active(const Tui* t) {
    return t != NULL && t->model.choice_mode;
}

size_t tui_choice_selected_index(const Tui* t) {
    if (t == NULL || !t->model.choice_mode) {
        return SIZE_MAX;
    }
    return tui_choice_match_at(&t->model, t->model.choice_selected);
}

void tui_set_report_cancel_callback(Tui* t, TuiCancelCb cancel) {
    if (t != NULL) {
        t->report_cancel = cancel;
    }
}

void tui_report_start(Tui* t) {
    if (t == NULL) {
        return;
    }
    if (t->model.choice_mode) {
        tui_choice_stop(t);
    }
    t->model.report_mode = true;
    int rows = t->model.rows > 0 ? t->model.rows : 1;
    int middle_rows = rows - 3;
    if (middle_rows < 1) {
        middle_rows = 1;
    }
    size_t total = tui_report_content_lines();
    t->model.report_scroll = total > (size_t)middle_rows ? total - (size_t)middle_rows : 0;
    string_clear(&t->model.input);
    t->model.cursor = 0;
    t->model.input_secret = false;
    tui_render(t);
}

void tui_report_stop(Tui* t) {
    if (t == NULL) {
        return;
    }
    t->model.report_mode = false;
    t->model.report_scroll = 0;
    string_clear(&t->model.input);
    t->model.cursor = 0;
    tui_render(t);
}

bool tui_report_active(const Tui* t) {
    return t != NULL && t->model.report_mode;
}

void tui_report_scroll(Tui* t, int delta) {
    if (t == NULL || !t->model.report_mode || delta == 0) {
        return;
    }
    int rows = t->model.rows > 0 ? t->model.rows : 1;
    int middle_rows = rows - 3;
    if (middle_rows < 1) {
        middle_rows = 1;
    }
    size_t total = tui_report_content_lines();
    size_t max_scroll = total > (size_t)middle_rows ? total - (size_t)middle_rows : 0;
    if (t->model.report_scroll > max_scroll) {
        t->model.report_scroll = max_scroll;
    }
    if (delta > 0) {
        size_t amount = (size_t)delta;
        if (amount > max_scroll - t->model.report_scroll) {
            t->model.report_scroll = max_scroll;
        } else {
            t->model.report_scroll += amount;
        }
    } else {
        size_t amount = (size_t)(-(int64_t)delta);
        t->model.report_scroll =
            t->model.report_scroll > amount ? t->model.report_scroll - amount : 0;
    }
    tui_render(t);
}

bool tui_check_resize(Tui* t) {
    int rows = 0, cols = 0;
    terminal_get_size(t->fd, &rows, &cols);
    if (rows != t->model.rows || cols != t->model.cols) {
        t->model.rows = rows;
        t->model.cols = cols;
        tui_render(t);
        return true;
    }
    return false;
}

void tui_render(Tui* t) {
    if (!t->raw_active) {
        return; /* not a terminal: nothing to draw */
    }
    String screen = string_new();
    int crow = 0, ccol = 0;
    size_t section_count = t->model.rows > 0 ? (size_t)t->model.rows : 1;
    TuiRowSection* sections = calloc(section_count, sizeof(*sections));
    tui_render_screen_sections(&t->model, &screen, &crow, &ccol, sections,
                               sections != NULL ? section_count : 0);

    terminal_draw_screen(t->fd, screen.data, screen.len, crow, ccol, sections,
                         sections != NULL ? section_count : 0);
    free(sections);
    string_free(&screen);
}
