/*
 * tests/test_event_loop.c — event loop tests.
 */

#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "runtime/event_loop.h"
#include "test_common.h"
#include "util/error.h"

typedef struct {
    int calls;
    uint32_t last_events;
} Counter;

static void count_cb(EventLoop* loop, int fd, uint32_t events, void* ud) {
    (void)loop;
    (void)fd;
    Counter* c = ud;
    c->calls++;
    c->last_events = events;
}

static int test_timerfd_wakes_loop(void) {
    EventLoop* l = event_loop_new();
    CHECK(l != NULL);

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    CHECK(tfd >= 0);
    struct itimerspec its = {0};
    its.it_value.tv_nsec = 20 * 1000 * 1000; /* 20 ms */
    CHECK(timerfd_settime(tfd, 0, &its, NULL) == 0);

    Counter c = {0};
    LoopWatcher w = {0};
    w.fd = tfd;
    w.events = EPOLLIN;
    w.cb = count_cb;
    w.ud = &c;
    CHECK(event_loop_add(l, &w) == AGENT_OK);

    /* the wait must return before the 5s timeout */
    int n = event_loop_wait(l, 5000);
    CHECK(n >= 1);
    CHECK(c.calls == 1);
    CHECK(c.last_events & EPOLLIN);

    event_loop_remove(l, tfd);
    close(tfd);
    event_loop_free(l);
    return g_failures;
}

static int test_wakeup_returns(void) {
    EventLoop* l = event_loop_new();
    CHECK(l != NULL);

    /* wake from the same thread: wait must return quickly */
    CHECK(event_loop_wakeup(l) == AGENT_OK);
    int n = event_loop_wait(l, 5000);
    CHECK(n >= 1);

    /* second wake: the counter was drained, so a fresh wake fires again */
    CHECK(event_loop_wakeup(l) == AGENT_OK);
    n = event_loop_wait(l, 5000);
    CHECK(n >= 1);

    event_loop_free(l);
    return g_failures;
}

static int test_timeout_returns_zero(void) {
    EventLoop* l = event_loop_new();
    CHECK(l != NULL);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int n = event_loop_wait(l, 30);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK(n == 0);
    int64_t elapsed = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(elapsed < 500); /* returned on timeout, not stuck */

    event_loop_free(l);
    return g_failures;
}

static int test_multiple_watchers(void) {
    EventLoop* l = event_loop_new();
    CHECK(l != NULL);

    int tfd1 = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    int tfd2 = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    CHECK(tfd1 >= 0 && tfd2 >= 0);

    struct itimerspec its = {0};
    its.it_value.tv_nsec = 10 * 1000 * 1000;
    timerfd_settime(tfd1, 0, &its, NULL);
    timerfd_settime(tfd2, 0, &its, NULL);

    Counter c1 = {0}, c2 = {0};
    LoopWatcher w1 = {.fd = tfd1, .events = EPOLLIN, .cb = count_cb, .ud = &c1};
    LoopWatcher w2 = {.fd = tfd2, .events = EPOLLIN, .cb = count_cb, .ud = &c2};
    CHECK(event_loop_add(l, &w1) == AGENT_OK);
    CHECK(event_loop_add(l, &w2) == AGENT_OK);

    int n = event_loop_wait(l, 5000);
    CHECK(n >= 2); /* both timers fired in one wait */
    CHECK(c1.calls == 1 && c2.calls == 1);

    event_loop_remove(l, tfd1);
    event_loop_remove(l, tfd2);
    close(tfd1);
    close(tfd2);
    event_loop_free(l);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_timerfd_wakes_loop();
    g_failures += test_wakeup_returns();
    g_failures += test_timeout_returns_zero();
    g_failures += test_multiple_watchers();

    if (g_failures == 0) {
        printf("test_event_loop: all tests passed\n");
        return 0;
    }
    printf("test_event_loop: %d test(s) failed\n", g_failures);
    return 1;
}
