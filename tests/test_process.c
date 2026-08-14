/*
 * tests/test_process.c — process_run unit tests.
 */

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "runtime/process.h"
#include "test_common.h"
#include "util/error.h"

static int test_ok_with_output(void) {
    char* argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)"echo hello", NULL};
    ProcessResult r = {0};
    CHECK(process_run(NULL, argv, 5000, 0, &r) == AGENT_OK);
    CHECK(!r.timed_out);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out.data, "hello") != NULL);
    process_result_free(&r);
    return g_failures;
}

static int test_nonzero_exit(void) {
    char* argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)"exit 7", NULL};
    ProcessResult r = {0};
    CHECK(process_run(NULL, argv, 5000, 0, &r) == AGENT_OK);
    CHECK(r.exit_code == 7);
    process_result_free(&r);
    return g_failures;
}

static int test_signal_exit(void) {
    /* sh -c 'kill -TERM $$' -> the shell dies by signal (128+15) */
    char* argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)"kill -TERM $$", NULL};
    ProcessResult r = {0};
    CHECK(process_run(NULL, argv, 5000, 0, &r) == AGENT_OK);
    CHECK(r.exit_code == 128 + SIGTERM);
    process_result_free(&r);
    return g_failures;
}

static int test_stderr_captured(void) {
    char* argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)"echo err-out >&2; echo ok-out", NULL};
    ProcessResult r = {0};
    CHECK(process_run(NULL, argv, 5000, 0, &r) == AGENT_OK);
    CHECK(strstr(r.out.data, "err-out") != NULL);
    CHECK(strstr(r.out.data, "ok-out") != NULL);
    process_result_free(&r);
    return g_failures;
}

static int test_timeout_kills_tree(void) {
    /* the sleep is a child of sh in the same process group */
    char* argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)"sleep 30", NULL};
    ProcessResult r = {0};
    int64_t t0 = 0;
    (void)t0;
    CHECK(process_run(NULL, argv, 300, 0, &r) == AGENT_OK);
    CHECK(r.timed_out);
    CHECK(r.exit_code == 128 + SIGTERM || r.exit_code == 128 + SIGKILL);
    process_result_free(&r);

    /* no stray sleep process may remain (the whole group was killed) */
    ProcessResult pgrep = {0};
    char* p_argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)"pgrep -f '^sleep 30$' || true", NULL};
    CHECK(process_run(NULL, p_argv, 3000, 4096, &pgrep) == AGENT_OK);
    CHECK(pgrep.out.data == NULL || strstr(pgrep.out.data, "sleep 30") == NULL);
    process_result_free(&pgrep);
    return g_failures;
}

static int test_output_cap(void) {
    char* argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)"head -c 100000 /dev/zero | tr '\\0' 'x'",
                    NULL};
    ProcessResult r = {0};
    CHECK(process_run(NULL, argv, 5000, 4096, &r) == AGENT_OK);
    CHECK(r.output_capped);
    CHECK(r.out.len == 4096);
    process_result_free(&r);
    return g_failures;
}

static int test_cwd(void) {
    /* run in /tmp and check pwd output */
    char* argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)"pwd", NULL};
    ProcessResult r = {0};
    CHECK(process_run("/tmp", argv, 5000, 0, &r) == AGENT_OK);
    CHECK(strstr(r.out.data, "/tmp") != NULL);
    process_result_free(&r);
    return g_failures;
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int test_timeout_kills_background_pipe_holder(void) {
    /* The shell exits immediately, but its background child inherits the
     * captured pipes. Timeout must still kill the now-leaderless group. */
    char* argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)"sleep 30 & echo $!", NULL};
    ProcessResult r = {0};
    int64_t started = monotonic_ms();
    CHECK(process_run(NULL, argv, 300, 4096, &r) == AGENT_OK);
    CHECK(monotonic_ms() - started < 5000);
    CHECK(r.timed_out);

    pid_t background_pid = r.out.data != NULL ? (pid_t)strtol(r.out.data, NULL, 10) : -1;
    CHECK(background_pid > 1);
    if (background_pid > 1) {
        bool gone = false;
        for (int i = 0; i < 100; i++) {
            errno = 0;
            if (kill(background_pid, 0) != 0 && errno == ESRCH) {
                gone = true;
                break;
            }
            struct timespec pause = {.tv_nsec = 10 * 1000 * 1000};
            nanosleep(&pause, NULL);
        }
        CHECK(gone);
    }
    process_result_free(&r);
    return g_failures;
}

static int test_async_start_and_poll(void) {
    EventLoop* loop = event_loop_new();
    CHECK(loop != NULL);
    char* argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)"sleep 0.2; echo async", NULL};
    ProcessTask* task = NULL;
    int64_t started = monotonic_ms();
    CHECK(process_start(loop, NULL, argv, 5000, 4096, &task) == AGENT_OK);
    CHECK(monotonic_ms() - started < 500); /* start must not wait for sleep */

    ProcessResult r = {0};
    bool done = false;
    CHECK(process_poll(task, &r, &done) == AGENT_OK);
    CHECK(!done);
    for (int i = 0; i < 100 && !done; i++) {
        event_loop_wait(loop, 20);
        CHECK(process_poll(task, &r, &done) == AGENT_OK);
    }
    CHECK(done);
    CHECK(r.exit_code == 0);
    CHECK(r.out.data != NULL && strstr(r.out.data, "async") != NULL);
    process_result_free(&r);
    process_task_free(task);
    event_loop_free(loop);
    return g_failures;
}

static int test_async_cancel(void) {
    EventLoop* loop = event_loop_new();
    CHECK(loop != NULL);
    char* argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)"sleep 30", NULL};
    ProcessTask* task = NULL;
    CHECK(process_start(loop, NULL, argv, 0, 0, &task) == AGENT_OK);
    process_cancel(task);

    ProcessResult r = {0};
    bool done = false;
    for (int i = 0; i < 200 && !done; i++) {
        event_loop_wait(loop, 20);
        CHECK(process_poll(task, &r, &done) == AGENT_OK);
    }
    CHECK(done);
    CHECK(!r.timed_out); /* explicit cancellation is distinct from timeout */
    CHECK(r.exit_code == 128 + SIGTERM || r.exit_code == 128 + SIGKILL);
    process_result_free(&r);
    process_task_free(task);
    event_loop_free(loop);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_ok_with_output();
    g_failures += test_nonzero_exit();
    g_failures += test_signal_exit();
    g_failures += test_stderr_captured();
    g_failures += test_timeout_kills_tree();
    g_failures += test_timeout_kills_background_pipe_holder();
    g_failures += test_output_cap();
    g_failures += test_cwd();
    g_failures += test_async_start_and_poll();
    g_failures += test_async_cancel();

    if (g_failures == 0) {
        printf("test_process: all tests passed\n");
        return 0;
    }
    printf("test_process: %d test(s) failed\n", g_failures);
    return 1;
}
