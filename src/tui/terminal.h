/*
 * tui/terminal.h — ncursesw terminal backend.
 *
 * The TUI is the only component that touches terminal state. The screen
 * model and layout renderer remain independent and testable without a TTY.
 */

#ifndef CAGENT_TUI_TERMINAL_H
#define CAGENT_TUI_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <termios.h>

#include "tui/render.h"

/* Switch fd into ncursesw/raw mode; *saved receives the previous state.
 * Returns AGENT_OK or AGENT_ERR_IO when fd is not a terminal. */
int terminal_raw_mode(int fd, struct termios* saved);

/* Restore ncurses and the saved terminal state. */
void terminal_restore(int fd, const struct termios* saved);

/* Query the terminal size via ncurses, or ioctl before initialization. */
void terminal_get_size(int fd, int* rows, int* cols);

void terminal_clear_screen(int fd);
void terminal_move(int fd, int row, int col); /* 1-based */
void terminal_hide_cursor(int fd);
void terminal_show_cursor(int fd);

/* Draw the pure renderer's text buffer through ncursesw. Each row is styled
 * according to its TuiRowSection tag (header/status rendered as reverse
 * bars, input bold, message viewport normal). `sections` may be NULL when
 * no styling information is available. */
void terminal_draw_screen(int fd, const char* text, size_t len, int cursor_row, int cursor_col,
                          const TuiRowSection* sections, size_t sections_len);

#endif /* CAGENT_TUI_TERMINAL_H */
