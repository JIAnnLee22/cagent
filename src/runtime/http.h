/*
 * runtime/http.h — shared HTTP runtime on libcurl multi + epoll.
 *
 * All agents share ONE HttpRuntime (DESIGN.md §9): each request is a
 * CURL easy handle added to the multi; curl sockets are registered with
 * the event loop and driven by curl_multi_socket_action. Requests are
 * fully async: http_request_start() returns immediately, the done
 * callback fires when the transfer completes (or fails).
 *
 * Ownership:
 *   - HttpRequest is OWNED by the HttpRuntime after start; freed by the
 *     runtime after the done callback returns. Do NOT touch it after
 *     starting, except through the done callback.
 *   - url/body are copied; headers are copied into the request.
 *   - write_cb (optional) receives response body bytes as they arrive;
 *     returning 0 aborts the transfer.
 *   - done_cb: status = HTTP status code; rc = curl result. The response
 *     buffer is valid during the callback only.
 */

#ifndef CAGENT_RUNTIME_HTTP_H
#define CAGENT_RUNTIME_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include <curl/curl.h>

#include "runtime/event_loop.h"
#include "util/error.h"

typedef struct HttpRuntime HttpRuntime;
typedef struct HttpRequest HttpRequest;

/* Return 0 to abort the transfer. */
typedef size_t (*HttpWriteCb)(HttpRequest* req, const char* data, size_t len, void* ud);

typedef struct {
    int http_status;  /* 0 when the transfer failed */
    CURLcode rc;      /* CURLE_OK on success */
    const char* body; /* borrowed; valid during the callback */
    size_t body_len;
} HttpDoneInfo;

typedef void (*HttpDoneCb)(HttpRequest* req, const HttpDoneInfo* info, void* ud);

HttpRuntime* http_new(EventLoop* loop);
void http_free(HttpRuntime* h);

/* Start an async HTTP request. Returns AGENT_OK or an error; on success
 * the runtime owns `req_out`. */
int http_request_start(HttpRuntime* h, const char* url, const char* body, size_t body_len,
                       const char* const headers[], size_t n_headers, size_t response_cap,
                       HttpWriteCb write_cb, void* write_ud, HttpDoneCb done_cb, void* done_ud,
                       HttpRequest** req_out);

/* Start an async GET request. The response and callback ownership semantics
 * are identical to http_request_start(); no request body is sent. */
int http_request_get(HttpRuntime* h, const char* url, const char* const headers[],
                     size_t n_headers, size_t response_cap, HttpWriteCb write_cb, void* write_ud,
                     HttpDoneCb done_cb, void* done_ud, HttpRequest** req_out);

/* Abort a request: removes it from the multi and invokes the done
 * callback with rc = CURLE_ABORTED_BY_CALLBACK, status = 0. Safe to call
 * from the done callback itself (no-op then). */
void http_request_abort(HttpRuntime* h, HttpRequest* req);

/* Drive the multi: called after epoll events or a timer tick. */
void http_pump(HttpRuntime* h);

/* Remaining time (ms) until the multi's next timer, or -1 when none.
 * The event loop uses this as its epoll_wait timeout. */
long http_next_timeout_ms(HttpRuntime* h);

#endif /* CAGENT_RUNTIME_HTTP_H */
