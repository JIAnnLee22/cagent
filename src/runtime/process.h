/*
 * runtime/process.h — child process execution with capture (DESIGN.md §21).
 *
 * process_start() integrates stdout/stderr pipes and timeout handling with
 * EventLoop, so a child never blocks the agent/runtime thread. process_run()
 * is the synchronous compatibility wrapper used by direct tool tests.
 * Both paths kill the whole process group (SIGTERM, then SIGKILL after a
 * grace period), cap captured output, and always reap the child.
 *
 * Ownership:
 *   - argv is borrowed; argv[0] must be a path to an executable (the
 *     process layer does no PATH lookup — resolve first). Child processes
 *     receive a restricted environment without credential-like variables.
 *   - result->out is owned by ProcessResult; process_result_free() frees it.
 */

#ifndef CAGENT_RUNTIME_PROCESS_H
#define CAGENT_RUNTIME_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/event_loop.h"
#include "util/error.h"
#include "util/string.h"

typedef struct {
    int exit_code; /* WEXITSTATUS; 128+sig when signaled; -2 unknown */
    bool timed_out;
    bool output_capped;
    String out; /* owned; merged stdout+stderr */
} ProcessResult;

typedef struct ProcessTask ProcessTask;

/* Start a non-blocking process task. argv is needed only during this call;
 * the child has already forked before process_start() returns. */
int process_start(EventLoop* loop, const char* cwd, char* const argv[], int64_t timeout_ms,
                  size_t output_cap, ProcessTask** task_out);

/* Poll child/reaping state. When *done is true, result receives ownership of
 * the captured output and may be released with process_result_free(). */
int process_poll(ProcessTask* task, ProcessResult* result, bool* done);

/* Begin non-blocking termination. Safe and idempotent. */
void process_cancel(ProcessTask* task);

/* Remove watchers, kill/reap an unfinished child, and free the task. */
void process_task_free(ProcessTask* task);

/* Synchronous compatibility wrapper. cwd may be NULL; timeout_ms <= 0 means
 * no timeout; output_cap 0 means unlimited. */
int process_run(const char* cwd, char* const argv[], int64_t timeout_ms, size_t output_cap,
                ProcessResult* result);

void process_result_free(ProcessResult* r);

#endif /* CAGENT_RUNTIME_PROCESS_H */
