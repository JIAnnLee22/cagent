/*
 * util/log.h — unified logging.
 *
 * Levels: TRACE < DEBUG < INFO < WARN < ERROR.
 *
 * Ownership contract:
 *   - log_* functions never take ownership of the message string;
 *     the caller keeps ownership of all arguments.
 *   - All functions are safe to call from any thread; output is
 *     line-atomic per call (single fprintf to the sink).
 *
 * TODO(phase 2): file sink under ~/.local/state/cagent/, level filter,
 *                redaction of secrets in headers.
 */

#ifndef CAGENT_UTIL_LOG_H
#define CAGENT_UTIL_LOG_H

#include <stdbool.h>

typedef enum { LOG_TRACE = 0, LOG_DEBUG = 1, LOG_INFO = 2, LOG_WARN = 3, LOG_ERROR = 4 } LogLevel;

/* Set minimum level that gets emitted. Default: LOG_INFO. */
void log_set_level(LogLevel level);

/* Optional file sink (DESIGN.md §39: ~/.local/state/cagent/). The file
 * is appended to; failure to open keeps stderr-only logging. Callers
 * must not log secrets (api keys never appear in log calls). */
int log_init(const char* path);
void log_close(void);

/* Suppress the stderr sink (the TUI owns the terminal); the file sink
 * keeps working. */
void log_set_stderr(bool enabled);

/* Emit a formatted message at the given level. printf-style. */
void log_msg(LogLevel level, const char* fmt, ...);

#define log_trace(...) log_msg(LOG_TRACE, __VA_ARGS__)
#define log_debug(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define log_info(...) log_msg(LOG_INFO, __VA_ARGS__)
#define log_warn(...) log_msg(LOG_WARN, __VA_ARGS__)
#define log_error(...) log_msg(LOG_ERROR, __VA_ARGS__)

#endif /* CAGENT_UTIL_LOG_H */
