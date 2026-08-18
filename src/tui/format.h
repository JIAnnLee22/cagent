/*
 * tui/format.h — pure text formatters used by the TUI and the plain REPL.
 *
 * Goal: surface only the information a human reader needs (tool name plus a
 * short, human-readable summary of its arguments) without ever rendering
 * the raw arguments JSON. The underlying JSON stays in the Session and
 * Provider requests so model execution is unaffected; this is display only.
 *
 * The functions are pure and free of terminal / ncurses state, so they are
 * testable without a TTY.
 */

#ifndef CAGENT_TUI_FORMAT_H
#define CAGENT_TUI_FORMAT_H

#include "util/string.h"

/* Soft cap (display cells, not bytes) for one tool-call line. The renderer
 * still applies its own wrap; this is a sanity limit so a giant `command`
 * does not produce a screen-wide log line. */
#define TUI_TOOL_SUMMARY_MAX_CELLS 96

/* Build a one-line summary for a tool invocation, e.g.
 *
 *     tui_format_tool_call_summary("bash", "{\"command\":\"ls -la\"}")
 *         → "bash ls -la"
 *     tui_format_tool_call_summary("read", "{\"path\":\"a.c\",\"offset\":5}")
 *         → "read a.c:5"
 *
 * Always begins with the tool name (or "?" when NULL). For known tools the
 * relevant arguments are pulled out of the JSON; for unknown tools or
 * malformed arguments it falls back to a safe, sanitized excerpt of the
 * raw input — never the raw JSON braces or quoted keys.
 *
 * Returns a heap-owned String the caller must release with string_free().
 * On OOM the returned String is empty. */
String tui_format_tool_call_summary(const char* name, const char* arguments);

#endif /* CAGENT_TUI_FORMAT_H */
