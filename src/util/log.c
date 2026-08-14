/*
 * util/log.c — minimal stderr logger.
 *
 * Phase 1 scope: single-threaded bootstrap logging to stderr.
 * Phase 2 (asynchronous runtime): add file sink, level filtering from
 * config, and thread-safe queueing so TUI output is never polluted.
 */

#include <stdarg.h>
#include <stdio.h>

#include "util/error.h"
#include "util/log.h"

static LogLevel g_level = LOG_INFO;
static FILE* g_file = NULL;
static bool g_stderr_enabled = true;

void log_set_stderr(bool enabled) {
    g_stderr_enabled = enabled;
}

int log_init(const char* path) {
    if (path == NULL) {
        return AGENT_ERR_IO;
    }
    FILE* f = fopen(path, "a");
    if (f == NULL) {
        return AGENT_ERR_IO; /* stderr-only logging continues */
    }
    if (g_file != NULL) {
        fclose(g_file);
    }
    g_file = f;
    return AGENT_OK;
}

void log_close(void) {
    if (g_file != NULL) {
        fclose(g_file);
        g_file = NULL;
    }
}

static const char* level_name(LogLevel level) {
    switch (level) {
    case LOG_TRACE:
        return "TRACE";
    case LOG_DEBUG:
        return "DEBUG";
    case LOG_INFO:
        return "INFO";
    case LOG_WARN:
        return "WARN";
    case LOG_ERROR:
        return "ERROR";
    default:
        return "?";
    }
}

void log_set_level(LogLevel level) {
    g_level = level;
}

void log_msg(LogLevel level, const char* fmt, ...) {
    if (level < g_level) {
        return;
    }

    /* Format once, then emit to every enabled sink. Each fprintf call is
     * line-atomic, so concurrent loggers never interleave mid-line. */
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (g_stderr_enabled) {
        fprintf(stderr, "[%s] %s\n", level_name(level), buf);
    }
    if (g_file != NULL) {
        fprintf(g_file, "[%s] %s\n", level_name(level), buf);
        fflush(g_file);
    }
}
