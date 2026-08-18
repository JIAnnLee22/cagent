/*
 * tui/tui.h — ncursesw terminal UI facade.
 *
 * The TUI owns terminal/backend state and a pure TuiModel. It reports
 * complete input lines and cancellation through callbacks; it never calls
 * the Agent directly.
 */

#ifndef CAGENT_TUI_TUI_H
#define CAGENT_TUI_TUI_H

#include <stdbool.h>
#include <termios.h>

#include "agent/agent.h"
#include "tui/render.h"

typedef struct Tui Tui;

/* line is always a NUL-terminated string; an empty submission is "". */
typedef void (*TuiSubmitCb)(void* ud, const char* line);
typedef void (*TuiCancelCb)(void* ud);

Tui* tui_new(int fd);
void tui_free(Tui*);

void tui_set_callbacks(Tui* t, TuiSubmitCb submit, TuiCancelCb cancel, void* ud);
/* Called for a standalone Esc in ordinary input mode. Choice/report modes
 * use their dedicated cancellation callbacks instead. */
void tui_set_escape_callback(Tui* t, TuiCancelCb escape);
void tui_set_header(Tui* t, const char* header);

/* Agent event sink (registered as the agent's event_cb). */
void tui_on_agent_event(void* ud, const AgentEvent* ev);

/* Populate a newly-created TUI from restored conversation messages.
 * This is display-only: it neither emits events nor persists messages. */
void tui_replay_history(Tui* t, const MessageList* messages);

/* Feed raw bytes from stdin (escape-sequence aware). */
void tui_feed_bytes(Tui* t, const char* data, size_t len);

void tui_set_busy(Tui* t, bool busy);
void tui_set_status(Tui* t, const char* status);
void tui_set_usage(Tui* t, const char* usage);
void tui_set_input_secret(Tui* t, bool secret);

/* Temporarily replace the history viewport with a keyboard-navigable,
 * fuzzy-filtered choice list. Labels are copied by the TUI. */
void tui_set_choice_cancel_callback(Tui* t, TuiCancelCb cancel);
void tui_choice_start(Tui* t, const char* const* labels, size_t count, size_t selected);
void tui_choice_stop(Tui* t);
bool tui_choice_active(const Tui* t);
size_t tui_choice_selected_index(const Tui* t);

/* Full-screen static report mode. It never submits input to the app. */
void tui_report_start(Tui* t);
void tui_report_stop(Tui* t);
bool tui_report_active(const Tui* t);
void tui_report_scroll(Tui* t, int delta);
void tui_set_report_cancel_callback(Tui* t, TuiCancelCb cancel);

bool tui_check_resize(Tui* t);
void tui_render(Tui* t);

TuiModel* tui_model(Tui* t);

#endif /* CAGENT_TUI_TUI_H */
