/*
 * runtime/http.c — libcurl multi over epoll.
 *
 * Integration points:
 *   - CURLMOPT_SOCKETFUNCTION: register/unregister curl sockets with the
 *     event loop; socket events call curl_multi_socket_action.
 *   - CURLMOPT_TIMERFUNCTION: records the multi's timer; the event loop
 *     uses http_next_timeout_ms() as its wait timeout.
 *   - After each wait, http_pump() runs curl_multi_socket_action(0) to
 *     service timers, then drains CURLMSG_DONE messages.
 */

#include <arpa/inet.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "runtime/http.h"
#include "util/buffer.h"
#include "util/string.h"
#include "util/vector.h"

struct HttpRequest {
    HttpRuntime* h;
    CURL* easy;
    String body;                /* owned; kept alive for the transfer */
    struct curl_slist* headers; /* owned */
    struct curl_slist* resolve; /* owned libc-resolved host pinning */
    Buffer response;            /* owned; capped */
    bool response_capped;
    size_t response_cap;
    HttpWriteCb write_cb;
    void* write_ud;
    HttpDoneCb done_cb;
    void* done_ud;
    int http_status;
    CURLcode rc;
    bool finished;
};

typedef struct SocketWatcher SocketWatcher;

/* one epoll watcher per curl socket */
struct SocketWatcher {
    LoopWatcher w;  /* embedded, not a pointer: owned here */
    HttpRuntime* h; /* the runtime, NOT the request: finish_request() frees
                       the request while this watcher may still be
                       registered (curl sends CURL_POLL_REMOVE after) */
    curl_socket_t sock;
    bool removed; /* released from epoll; safe to free */
};

struct HttpRuntime {
    EventLoop* loop;
    CURLM* multi;
    long timer_ms;   /* -1 = no timer */
    size_t active;   /* in-flight requests */
    Vector requests; /* HttpRequest*; owned, for http_free() cleanup */
    Vector watchers; /* SocketWatcher*; SOCKET-level, shared by requests on
                        the same connection; freed on REMOVE or http_free */
};

static HttpRequest* find_request(HttpRuntime* h, CURL* easy) {
    (void)h;
    CURL* p = NULL;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &p);
    return (HttpRequest*)p;
}

static bool proxy_environment_present(void) {
    static const char* names[] = {
        "http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char* value = getenv(names[i]);
        if (value != NULL && value[0] != '\0') {
            return true;
        }
    }
    return false;
}

/* Resolve through libc/NSS once and hand all addresses to libcurl. This
 * avoids a multi+resolver completion path that can stall on fake-IP TUN
 * setups, while preserving explicit proxy environments unchanged. */
static struct curl_slist* resolve_url_host(const char* url) {
    if (url == NULL || proxy_environment_present()) {
        return NULL;
    }
    CURLU* parts = curl_url();
    if (parts == NULL || curl_url_set(parts, CURLUPART_URL, url, 0) != CURLUE_OK) {
        curl_url_cleanup(parts);
        return NULL;
    }
    char* host = NULL;
    char* scheme = NULL;
    char* explicit_port = NULL;
    if (curl_url_get(parts, CURLUPART_HOST, &host, 0) != CURLUE_OK ||
        curl_url_get(parts, CURLUPART_SCHEME, &scheme, 0) != CURLUE_OK) {
        curl_free(host);
        curl_free(scheme);
        curl_url_cleanup(parts);
        return NULL;
    }
    (void)curl_url_get(parts, CURLUPART_PORT, &explicit_port, 0);
    char default_port[6];
    const char* port = explicit_port;
    if (port == NULL) {
        snprintf(default_port, sizeof(default_port), "%s", strcmp(scheme, "https") == 0 ? "443" : "80");
        port = default_port;
    }

    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    /* Do not pin an address family that is not actually configured locally.
     * Broken IPv6 routes are common on VPN/TUN networks and can otherwise
     * make the first request spend the whole connect timeout on a dead path. */
    hints.ai_flags = AI_ADDRCONFIG;
    struct addrinfo* addresses = NULL;
    if (getaddrinfo(host, port, &hints, &addresses) != 0) {
        curl_free(host);
        curl_free(scheme);
        curl_free(explicit_port);
        curl_url_cleanup(parts);
        return NULL;
    }

    struct curl_slist* resolved = NULL;
    for (struct addrinfo* ai = addresses; ai != NULL; ai = ai->ai_next) {
        char address[INET6_ADDRSTRLEN] = {0};
        if (getnameinfo(ai->ai_addr, ai->ai_addrlen, address, sizeof(address), NULL, 0,
                        NI_NUMERICHOST) != 0) {
            continue;
        }
        char entry[512];
        if (ai->ai_family == AF_INET6) {
            snprintf(entry, sizeof(entry), "%s:%s:[%s]", host, port, address);
        } else {
            snprintf(entry, sizeof(entry), "%s:%s:%s", host, port, address);
        }
        struct curl_slist* next = curl_slist_append(resolved, entry);
        if (next == NULL) {
            curl_slist_free_all(resolved);
            resolved = NULL;
            break;
        }
        resolved = next;
    }
    freeaddrinfo(addresses);
    curl_free(host);
    curl_free(scheme);
    curl_free(explicit_port);
    curl_url_cleanup(parts);
    return resolved;
}

static void socket_watcher_cb(EventLoop* loop, int fd, uint32_t events, void* ud) {
    (void)loop;
    (void)fd;
    SocketWatcher* sw = ud;
    int action = 0;
    if (events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
        action |= CURL_CSELECT_IN;
    }
    if (events & EPOLLOUT) {
        action |= CURL_CSELECT_OUT;
    }
    if (action != 0) {
        /* NOTE: no http_pump() here — curl_multi_socket_action is NOT
         * reentrant; completions are drained by the loop's http_pump(). */
        int running = 0;
        curl_multi_socket_action(sw->h->multi, sw->sock, action, &running);
    }
}

static int socket_cb(CURL* easy, curl_socket_t s, int what, void* userp, void* socketp) {
    HttpRuntime* h = userp;
    HttpRequest* req = find_request(h, easy);
    if (req == NULL) {
        return 0;
    }
    SocketWatcher* sw = socketp;
    (void)req;

    switch (what) {
    case CURL_POLL_REMOVE:
    case CURL_POLL_NONE:
        if (sw != NULL && !sw->removed) {
            sw->removed = true;
            event_loop_remove(h->loop, sw->w.fd);
            /* drop from the runtime's socket watcher list (swap-remove) */
            for (size_t i = 0; i < vector_len(&h->watchers); i++) {
                SocketWatcher* w2 = *(SocketWatcher**)vector_at(&h->watchers, i);
                if (w2 == sw) {
                    size_t last = vector_len(&h->watchers) - 1;
                    *(SocketWatcher**)vector_at(&h->watchers, i) =
                        *(SocketWatcher**)vector_at(&h->watchers, last);
                    vector_pop(&h->watchers);
                    break;
                }
            }
            free(sw);
            curl_multi_assign(h->multi, s, NULL);
        }
        break;
    default: {
        if (sw == NULL) {
            sw = calloc(1, sizeof(SocketWatcher));
            if (sw == NULL) {
                return -1;
            }
            sw->w.fd = (int)s;
            sw->w.cb = socket_watcher_cb;
            sw->w.ud = sw;
            sw->h = h;
            sw->sock = s;
            sw->removed = false;
            curl_multi_assign(h->multi, s, sw);
            if (vector_push(&h->watchers, (const void*)&sw) == NULL) {
                free(sw);
                curl_multi_assign(h->multi, s, NULL);
                return -1;
            }
            int add_rc = event_loop_add(h->loop, &sw->w);
            if (add_rc != AGENT_OK) {
                free(sw);
                curl_multi_assign(h->multi, s, NULL);
                return -1;
            }
        }
        uint32_t ev = 0;
        if (what == CURL_POLL_IN || what == CURL_POLL_INOUT) {
            ev |= EPOLLIN;
        }
        if (what == CURL_POLL_OUT || what == CURL_POLL_INOUT) {
            ev |= EPOLLOUT;
        }
        sw->w.events = ev;
        event_loop_modify(h->loop, &sw->w);
        break;
    }
    }
    return 0;
}

static int timer_cb(CURLM* multi, long timeout_ms, void* userp) {
    (void)multi;
    HttpRuntime* h = userp;
    h->timer_ms = timeout_ms;
    /* NO wakeup here: the event loop already uses http_next_timeout_ms()
     * as its epoll_wait timeout, so an expired timer returns from the
     * wait and http_pump() services it. Waking on EVERY timer change
     * flooded the loop with wake events and starved socket data. */
    return 0;
}

HttpRuntime* http_new(EventLoop* loop) {
    HttpRuntime* h = calloc(1, sizeof(HttpRuntime));
    if (h == NULL) {
        return NULL;
    }
    h->loop = loop;
    h->timer_ms = -1;
    h->requests = vector_new(sizeof(HttpRequest*));
    h->watchers = vector_new(sizeof(SocketWatcher*));
    h->multi = curl_multi_init();
    if (h->multi == NULL) {
        free(h);
        return NULL;
    }
    curl_multi_setopt(h->multi, CURLMOPT_SOCKETFUNCTION, socket_cb);
    curl_multi_setopt(h->multi, CURLMOPT_SOCKETDATA, h);
    curl_multi_setopt(h->multi, CURLMOPT_TIMERFUNCTION, timer_cb);
    curl_multi_setopt(h->multi, CURLMOPT_TIMERDATA, h);
    curl_multi_setopt(h->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, 64L);
    return h;
}

void http_free(HttpRuntime* h) {
    if (h == NULL) {
        return;
    }
    /* abort any in-flight request (done callbacks fire) */
    /* abort any in-flight request (done callbacks fire) */
    for (size_t i = vector_len(&h->requests); i > 0; i--) {
        HttpRequest* req = *(HttpRequest**)vector_at(&h->requests, i - 1);
        http_request_abort(h, req);
    }
    vector_free(&h->requests);
    /* release any socket watchers curl never sent REMOVE for */
    for (size_t i = 0; i < vector_len(&h->watchers); i++) {
        SocketWatcher* sw = *(SocketWatcher**)vector_at(&h->watchers, i);
        if (sw != NULL && !sw->removed) {
            event_loop_remove(h->loop, sw->w.fd);
        }
        free(sw);
    }
    vector_free(&h->watchers);
    curl_multi_cleanup(h->multi);
    free(h);
}

static size_t http_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    HttpRequest* req = userdata;
    size_t n = size * nmemb;

    /* cap the raw response buffer */
    if (req->response_cap > 0 && req->response.len + n > req->response_cap) {
        size_t room = req->response_cap - req->response.len;
        if (room > 0) {
            buffer_append(&req->response, ptr, room);
        }
        req->response_capped = true;
    } else if (n > 0) {
        buffer_append(&req->response, ptr, n);
    }

    /* stream the FULL chunk to the caller (SSE parser) */
    if (req->write_cb != NULL) {
        if (req->write_cb(req, ptr, n, req->write_ud) == 0) {
            return 0; /* abort the transfer */
        }
    }
    return n;
}

static void finish_request(HttpRuntime* h, HttpRequest* req) {
    if (req->finished) {
        return;
    }
    req->finished = true;

    HttpDoneInfo info = {0};
    info.rc = req->rc;
    info.http_status = req->http_status;
    info.body = (const char*)req->response.data;
    info.body_len = req->response.len;

    HttpDoneCb done = req->done_cb;
    void* ud = req->done_ud;
    if (done != NULL) {
        done(req, &info, ud);
    }

    /* NOTE: socket watchers are NOT touched here — they are socket-level
     * and may be shared by other requests on the same connection. */

    /* drop the request from the tracking list (swap-remove) */
    for (size_t i = 0; i < vector_len(&h->requests); i++) {
        HttpRequest* r = *(HttpRequest**)vector_at(&h->requests, i);
        if (r == req) {
            size_t last = vector_len(&h->requests) - 1;
            *(HttpRequest**)vector_at(&h->requests, i) =
                *(HttpRequest**)vector_at(&h->requests, last);
            vector_pop(&h->requests);
            break;
        }
    }

    /* the runtime owns the request; free it after the callback */
    curl_easy_cleanup(req->easy);
    curl_slist_free_all(req->headers);
    curl_slist_free_all(req->resolve);
    string_free(&req->body);
    buffer_free(&req->response);
    free(req);
    h->active--;
}

static void process_multi_info(HttpRuntime* h) {
    CURLMsg* msg;
    int left = 0;
    while ((msg = curl_multi_info_read(h->multi, &left)) != NULL) {
        if (msg->msg != CURLMSG_DONE) {
            continue;
        }
        CURL* easy = msg->easy_handle;
        HttpRequest* req = find_request(h, easy);
        if (req == NULL) {
            curl_multi_remove_handle(h->multi, easy);
            continue;
        }
        req->rc = msg->data.result;
        long status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        req->http_status = (int)status;
        curl_multi_remove_handle(h->multi, easy);
        finish_request(h, req);
    }
}

void http_pump(HttpRuntime* h) {
    if (h == NULL) {
        return;
    }
    /* service expired timers, then drain completions */
    int running = 0;
    curl_multi_socket_action(h->multi, CURL_SOCKET_TIMEOUT, 0, &running);
    process_multi_info(h);
}

long http_next_timeout_ms(HttpRuntime* h) {
    if (h == NULL) {
        return -1;
    }
    long t = -1;
    curl_multi_timeout(h->multi, &t);
    if (t < 0) {
        t = -1;
    }
    return t;
}

static int http_request_start_method(HttpRuntime* h, const char* url, const char* body,
                                     size_t body_len, bool is_get,
                                     const char* const headers[], size_t n_headers,
                                     size_t response_cap, HttpWriteCb write_cb, void* write_ud,
                                     HttpDoneCb done_cb, void* done_ud, HttpRequest** req_out) {
    if (h == NULL || url == NULL || req_out == NULL) {
        return AGENT_ERR_HTTP;
    }

    HttpRequest* req = calloc(1, sizeof(HttpRequest));
    if (req == NULL) {
        return AGENT_ERR_OOM;
    }
    req->h = h;
    req->write_cb = write_cb;
    req->write_ud = write_ud;
    req->done_cb = done_cb;
    req->done_ud = done_ud;
    req->response_cap = response_cap;
    req->body = string_new();
    req->response = buffer_new();

    if (!is_get && string_append_n(&req->body, body, body_len) != AGENT_OK) {
        goto fail;
    }
    for (size_t i = 0; i < n_headers; i++) {
        req->headers = curl_slist_append(req->headers, headers[i]);
        if (req->headers == NULL) {
            goto fail;
        }
    }

    req->easy = curl_easy_init();
    if (req->easy == NULL) {
        goto fail;
    }
    curl_easy_setopt(req->easy, CURLOPT_PRIVATE, req);
    curl_easy_setopt(req->easy, CURLOPT_URL, url);
    curl_easy_setopt(req->easy, CURLOPT_HTTPHEADER, req->headers);
    req->resolve = resolve_url_host(url);
    if (req->resolve != NULL) {
        curl_easy_setopt(req->easy, CURLOPT_RESOLVE, req->resolve);
    }
    if (is_get) {
        curl_easy_setopt(req->easy, CURLOPT_HTTPGET, 1L);
    } else {
        curl_easy_setopt(req->easy, CURLOPT_POST, 1L);
        curl_easy_setopt(req->easy, CURLOPT_POSTFIELDS, req->body.data);
        curl_easy_setopt(req->easy, CURLOPT_POSTFIELDSIZE, (long)req->body.len);
    }
    curl_easy_setopt(req->easy, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(req->easy, CURLOPT_WRITEDATA, req);
    curl_easy_setopt(req->easy, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(req->easy, CURLOPT_TIMEOUT, 600L);
    /* Treat a dead/stalled connection as a transient transport failure so
     * the agent retry policy can recover instead of waiting indefinitely.
     * SSE keep-alive comments count as received bytes and therefore do not
     * trip this guard while the provider is maintaining a live stream. */
    curl_easy_setopt(req->easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(req->easy, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(req->easy, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(req->easy, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(req->easy, CURLOPT_TCP_KEEPINTVL, 15L);
#if LIBCURL_VERSION_NUM >= 0x073E00 /* CURLOPT_TCP_KEEPCNT since 7.62.0 */
    curl_easy_setopt(req->easy, CURLOPT_TCP_KEEPCNT, 3L);
#endif
    curl_easy_setopt(req->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(req->easy, CURLOPT_USERAGENT, "cagent/0.1");

    if (curl_multi_add_handle(h->multi, req->easy) != CURLM_OK) {
        goto fail;
    }
    if (vector_push(&h->requests, (const void*)&req) == NULL) {
        curl_multi_remove_handle(h->multi, req->easy);
        goto fail;
    }
    h->active++;
    *req_out = req;

    /* kick the multi and wake the loop */
    int running = 0;
    curl_multi_socket_action(h->multi, CURL_SOCKET_TIMEOUT, 0, &running);
    event_loop_wakeup(h->loop);
    return AGENT_OK;

fail:
    if (req->easy != NULL) {
        curl_multi_remove_handle(h->multi, req->easy);
        curl_easy_cleanup(req->easy);
    }
    curl_slist_free_all(req->headers);
    curl_slist_free_all(req->resolve);
    string_free(&req->body);
    buffer_free(&req->response);
    free(req);
    return AGENT_ERR_OOM;
}

int http_request_start(HttpRuntime* h, const char* url, const char* body, size_t body_len,
                       const char* const headers[], size_t n_headers, size_t response_cap,
                       HttpWriteCb write_cb, void* write_ud, HttpDoneCb done_cb, void* done_ud,
                       HttpRequest** req_out) {
    return http_request_start_method(h, url, body, body_len, false, headers, n_headers,
                                     response_cap, write_cb, write_ud, done_cb, done_ud, req_out);
}

int http_request_get(HttpRuntime* h, const char* url, const char* const headers[],
                     size_t n_headers, size_t response_cap, HttpWriteCb write_cb, void* write_ud,
                     HttpDoneCb done_cb, void* done_ud, HttpRequest** req_out) {
    return http_request_start_method(h, url, NULL, 0, true, headers, n_headers, response_cap,
                                     write_cb, write_ud, done_cb, done_ud, req_out);
}

void http_request_abort(HttpRuntime* h, HttpRequest* req) {
    if (h == NULL || req == NULL || req->finished) {
        return;
    }
    curl_multi_remove_handle(h->multi, req->easy);
    req->rc = CURLE_ABORTED_BY_CALLBACK;
    req->http_status = 0;
    finish_request(h, req);
    event_loop_wakeup(h->loop);
}
