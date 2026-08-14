/*
 * tests/test_http.c — concurrent HTTP runtime tests (libcurl multi).
 *
 * Launches one local python3 server and fires several requests at it
 * concurrently; verifies all complete with the expected body/status.
 */

#include <stdlib.h>
#include <string.h>

#include "runtime/event_loop.h"
#include "runtime/http.h"
#include "test_common.h"
#include "test_server.h"
#include "util/error.h"

#define NREQ 4

typedef struct {
    int done;
    int status;
    CURLcode rc;
    char body[256];
    size_t body_len;
} DoneInfo;

static size_t collect_write(HttpRequest* req, const char* data, size_t len, void* ud) {
    (void)req;
    DoneInfo* d = ud;
    if (d->body_len + len < sizeof(d->body)) {
        memcpy(d->body + d->body_len, data, len);
        d->body_len += len;
    }
    return len;
}

static void collect_done(HttpRequest* req, const HttpDoneInfo* info, void* ud) {
    (void)req;
    DoneInfo* d = ud;
    d->done = 1;
    d->status = info->http_status;
    d->rc = info->rc;
    if (info->body != NULL) {
        size_t n = info->body_len < sizeof(d->body) - 1 ? info->body_len : sizeof(d->body) - 1;
        memcpy(d->body, info->body, n);
        d->body_len = n;
    }
}

static int test_concurrent_requests(void) {
    int port = test_server_find_free_port();
    CHECK(port > 0);
    const char* body = "hello concurrent world";
    pid_t server = test_server_start(port, body, 200);
    CHECK(server > 0);
    CHECK(test_server_wait(port, 3000) == 0);

    EventLoop* loop = event_loop_new();
    HttpRuntime* http = http_new(loop);
    CHECK(loop != NULL && http != NULL);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/x", port);

    DoneInfo infos[NREQ] = {0};
    for (int i = 0; i < NREQ; i++) {
        HttpRequest* req = NULL;
        CHECK(http_request_start(http, url, "{}", 2, NULL, 0, 4096, collect_write, &infos[i],
                                 collect_done, &infos[i], &req) == AGENT_OK);
    }

    /* drive until all four complete */
    int all_done = 0;
    for (int iter = 0; iter < 400 && !all_done; iter++) {
        long t = http_next_timeout_ms(http);
        int to = t >= 0 && t < 20 ? (int)t : 20;
        event_loop_wait(loop, to);
        http_pump(http);
        all_done = 1;
        for (int i = 0; i < NREQ; i++) {
            if (!infos[i].done) {
                all_done = 0;
                break;
            }
        }
    }
    CHECK(all_done);

    for (int i = 0; i < NREQ; i++) {
        CHECK(infos[i].done);
        CHECK(infos[i].rc == CURLE_OK);
        CHECK(infos[i].status == 200);
        CHECK(infos[i].body_len == strlen(body));
        CHECK(memcmp(infos[i].body, body, strlen(body)) == 0);
    }

    http_free(http);
    event_loop_free(loop);
    test_server_stop(server);
    return g_failures;
}

static int test_http_error_status(void) {
    int port = test_server_find_free_port();
    CHECK(port > 0);
    pid_t server = test_server_start(port, "nope", 404);
    CHECK(server > 0);
    CHECK(test_server_wait(port, 3000) == 0);

    EventLoop* loop = event_loop_new();
    HttpRuntime* http = http_new(loop);
    CHECK(loop != NULL && http != NULL);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/x", port);
    DoneInfo d = {0};
    HttpRequest* req = NULL;
    CHECK(http_request_start(http, url, "{}", 2, NULL, 0, 4096, collect_write, &d, collect_done, &d,
                             &req) == AGENT_OK);

    for (int iter = 0; iter < 200 && !d.done; iter++) {
        long t = http_next_timeout_ms(http);
        int to = t >= 0 && t < 20 ? (int)t : 20;
        event_loop_wait(loop, to);
        http_pump(http);
    }
    CHECK(d.done);
    CHECK(d.rc == CURLE_OK);
    CHECK(d.status == 404);

    http_free(http);
    event_loop_free(loop);
    test_server_stop(server);
    return g_failures;
}

int main(void) {
    g_failures = 0;

    if (system("python3 --version >/dev/null 2>&1") == 0) {
        g_failures += test_concurrent_requests();
        g_failures += test_http_error_status();
    } else {
        printf("test_http: python3 unavailable, skipping e2e tests\n");
    }

    if (g_failures == 0) {
        printf("test_http: all tests passed\n");
        return 0;
    }
    printf("test_http: %d test(s) failed\n", g_failures);
    return 1;
}
