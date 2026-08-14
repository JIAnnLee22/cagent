/*
 * runtime/event_loop.c — epoll event loop.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "runtime/event_loop.h"

struct EventLoop {
    int epfd;
    int wake_fd;
};

static void on_wake(EventLoop* loop, int fd, uint32_t events, void* ud) {
    (void)loop;
    (void)events;
    (void)ud;
    /* drain the counter */
    uint64_t v;
    while (read(fd, &v, sizeof(v)) == sizeof(v)) {
        /* keep draining */
    }
}

EventLoop* event_loop_new(void) {
    EventLoop* l = calloc(1, sizeof(EventLoop));
    if (l == NULL) {
        return NULL;
    }
    l->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (l->epfd < 0) {
        free(l);
        return NULL;
    }
    l->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (l->wake_fd < 0) {
        close(l->epfd);
        free(l);
        return NULL;
    }

    static LoopWatcher wake_watcher; /* static: lives for the loop's life */
    wake_watcher.fd = l->wake_fd;
    wake_watcher.events = EPOLLIN;
    wake_watcher.cb = on_wake;
    wake_watcher.ud = NULL;
    if (event_loop_add(l, &wake_watcher) != AGENT_OK) {
        close(l->wake_fd);
        close(l->epfd);
        free(l);
        return NULL;
    }
    return l;
}

void event_loop_free(EventLoop* l) {
    if (l == NULL) {
        return;
    }
    close(l->epfd);
    close(l->wake_fd);
    free(l);
}

int event_loop_add(EventLoop* l, LoopWatcher* w) {
    if (l == NULL || w == NULL) {
        return AGENT_ERR_IO;
    }
    struct epoll_event ev = {0};
    ev.events = w->events;
    ev.data.ptr = w;
    if (epoll_ctl(l->epfd, EPOLL_CTL_ADD, w->fd, &ev) != 0) {
        return AGENT_ERR_IO;
    }
    return AGENT_OK;
}

int event_loop_modify(EventLoop* l, const LoopWatcher* w) {
    if (l == NULL || w == NULL) {
        return AGENT_ERR_IO;
    }
    struct epoll_event ev = {0};
    ev.events = w->events;
    ev.data.ptr = (void*)w;
    if (epoll_ctl(l->epfd, EPOLL_CTL_MOD, w->fd, &ev) != 0) {
        return AGENT_ERR_IO;
    }
    return AGENT_OK;
}

void event_loop_remove(EventLoop* l, int fd) {
    if (l == NULL) {
        return;
    }
    epoll_ctl(l->epfd, EPOLL_CTL_DEL, fd, NULL);
}

int event_loop_wait(EventLoop* l, int timeout_ms) {
    if (l == NULL) {
        return 0;
    }
    struct epoll_event events[32];
    int n = epoll_wait(l->epfd, events, 32, timeout_ms);
    if (n <= 0) {
        return 0; /* timeout or EINTR */
    }
    int dispatched = 0;
    for (int i = 0; i < n; i++) {
        LoopWatcher* w = events[i].data.ptr;
        if (w != NULL && w->cb != NULL) {
            w->cb(l, w->fd, events[i].events, w->ud);
            dispatched++;
        }
    }
    return dispatched;
}

int event_loop_wakeup(EventLoop* l) {
    if (l == NULL) {
        return AGENT_ERR_IO;
    }
    uint64_t one = 1;
    ssize_t n = write(l->wake_fd, &one, sizeof(one));
    return n == sizeof(one) ? AGENT_OK : AGENT_ERR_IO;
}
