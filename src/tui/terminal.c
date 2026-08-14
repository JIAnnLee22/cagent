/*
 * tui/terminal.c — ncursesw terminal backend.
 *
 * The TUI keeps its screen model and layout renderer independent from this
 * backend. ncurses owns terminal capabilities, resize dimensions, cursor,
 * and repainting; the Agent core never sees any of it.
 */

#include <limits.h>
#include <locale.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <ncurses.h>

#include "tui/terminal.h"
#include "util/error.h"

static bool g_curses_active = false;
static bool g_cursor_visible = false;

int terminal_raw_mode(int fd, struct termios* saved) {
    if (!isatty(fd) || saved == NULL) {
        return AGENT_ERR_IO;
    }
    if (tcgetattr(fd, saved) != 0) {
        return AGENT_ERR_IO;
    }
    /* ncursesw decodes multibyte input/output according to the process
     * locale. Without this, UTF-8 IME commits are treated as raw bytes. */
    (void)setlocale(LC_ALL, "");
    if (initscr() == NULL) {
        return AGENT_ERR_IO;
    }
    g_curses_active = true;
    raw();
    noecho();
    keypad(stdscr, TRUE);
    meta(stdscr, TRUE);
    use_default_colors();
    start_color();
    return AGENT_OK;
}

void terminal_restore(int fd, const struct termios* saved) {
    if (g_curses_active) {
        curs_set(1);
        g_cursor_visible = true;
        endwin();
        g_curses_active = false;
    }
    if (saved != NULL) {
        tcsetattr(fd, TCSAFLUSH, saved);
    }
}

void terminal_get_size(int fd, int* rows, int* cols) {
    if (rows == NULL || cols == NULL) {
        return;
    }
    if (g_curses_active) {
        *rows = LINES > 0 ? LINES : 24;
        *cols = COLS > 0 ? COLS : 80;
        return;
    }
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = (int)ws.ws_row;
        *cols = (int)ws.ws_col;
        return;
    }
    *rows = 24;
    *cols = 80;
}

void terminal_clear_screen(int fd) {
    (void)fd;
    if (g_curses_active) {
        erase();
    }
}

void terminal_move(int fd, int row, int col) {
    (void)fd;
    if (g_curses_active && row > 0 && col > 0) {
        move(row - 1, col - 1);
    }
}

void terminal_hide_cursor(int fd) {
    (void)fd;
    if (g_curses_active) {
        curs_set(0);
        g_cursor_visible = false;
    }
}

void terminal_show_cursor(int fd) {
    (void)fd;
    if (g_curses_active) {
        curs_set(1);
        g_cursor_visible = true;
    }
}

static chtype section_attributes(TuiRowSection section) {
    switch (section) {
    case TUI_ROW_HEADER:
    case TUI_ROW_STATUS:
        return A_REVERSE;
    case TUI_ROW_INPUT:
        return A_BOLD;
    case TUI_ROW_BODY:
    default:
        return A_NORMAL;
    }
}

void terminal_draw_screen(int fd, const char* text, size_t len, int cursor_row, int cursor_col,
                          const TuiRowSection* sections, size_t sections_len) {
    (void)fd;
    if (!g_curses_active || text == NULL) {
        return;
    }
    attrset(A_NORMAL);
    erase();
    size_t pos = 0;
    int row = 0;
    while (pos < len && row < LINES) {
        size_t start = pos;
        while (pos < len && text[pos] != '\n') {
            pos++;
        }
        size_t count = pos - start;
        if (count > (size_t)INT_MAX) {
            count = (size_t)INT_MAX;
        }
        TuiRowSection section =
            sections != NULL && (size_t)row < sections_len ? sections[row] : TUI_ROW_BODY;
        attrset(section_attributes(section));
        move(row, 0);
        if (section == TUI_ROW_HEADER || section == TUI_ROW_STATUS) {
            clrtoeol();
        }
        if (count > 0) {
            mvaddnstr(row, 0, text + start, (int)count);
        }
        if (pos < len && text[pos] == '\n') {
            pos++;
        }
        row++;
    }
    attrset(A_NORMAL);
    if (cursor_row > 0 && cursor_row <= LINES && cursor_col > 0 && cursor_col <= COLS) {
        /* The renderer always targets the input row; make the editing cursor
         * visible there (it stays hidden while ncurses owns the screen). */
        move(cursor_row - 1, cursor_col - 1);
        if (!g_cursor_visible) {
            curs_set(1);
            g_cursor_visible = true;
        }
    } else if (g_cursor_visible) {
        curs_set(0);
        g_cursor_visible = false;
    }
    refresh();
}
