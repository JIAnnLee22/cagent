/*
 * runtime/event_loop.h — epoll event loop (DESIGN.md §11).
 *
 * The event loop owns epoll and a self-pipe (eventfd) for cross-context
 * wakeups. Watchers are registered by the caller (the HTTP runtime
 * registers curl sockets; the async runtime will add stdin, pipes,
 * timerfds, signals). All callbacks run on the thread calling
 * event_loop_wait(); mutable state belongs to that thread.
 *
 * Ownership:
 *   - LoopWatcher objects are OWNED by the caller and must stay alive
 *     until event_loop_remove() (or event_loop_free()).
 *   - The event loop owns the epoll fd and the eventfd.
 */

#ifndef CAGENT_RUNTIME_EVENT_LOOP_H
#define CAGENT_RUNTIME_EVENT_LOOP_H

#include <stdint.h>

#include "util/error.h"

typedef struct EventLoop EventLoop;

typedef void (*LoopCb)(EventLoop* loop, int fd, uint32_t events, void* ud);

typedef struct {
    int fd;          /* watched fd */
    uint32_t events; /* EPOLLIN | EPOLLOUT | ... */
    LoopCb cb;       /* called with the actual event mask */
    void* ud;        /* caller context */
} LoopWatcher;

EventLoop* event_loop_new(void);
void event_loop_free(EventLoop* l);

/* Register/modify/remove a watcher. add returns AGENT_OK / OOM. */
int event_loop_add(EventLoop* l, LoopWatcher* w);
int event_loop_modify(EventLoop* l, const LoopWatcher* w);
void event_loop_remove(EventLoop* l, int fd);

/* Wait up to timeout_ms for events and dispatch callbacks. Returns the
 * number of callbacks invoked (0 on timeout). */
int event_loop_wait(EventLoop* l, int timeout_ms);

/* Wake a blocked event_loop_wait() from another thread/context. */
int event_loop_wakeup(EventLoop* l);

#endif /* CAGENT_RUNTIME_EVENT_LOOP_H */
