/*
 * tests/test_openai.c — OpenAI provider tests.
 *
 * Part 1: request-body serialization (pure function).
 * Part 2: end-to-end against a throwaway local Python HTTP server that
 * streams an SSE response in small chunks (no external API dependency).
 * The Python server is for tests only; the program never needs Python.
 */

#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "agent/message.h"
#include "model/model.h"
#include "model/openai.h"
#include "model/provider.h"
#include "runtime/event_loop.h"
#include "runtime/http.h"
#include "runtime/runtime.h"
#include "test_common.h"
#include "tool/registry.h"
#include "tool/tool.h"
#include "util/error.h"
#include "util/json.h"
#include "util/string.h"
#include "util/vector.h"

static int openai_model_request_wrapper(ModelRequest* req);

/* ------------------------------------------------------------------ */
/* event collector                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    ModelEventType type;
    char* text; /* owned; text delta / tool delta / error message */
    size_t index;
    Usage usage;
} Rec;

typedef struct {
    Vector events; /* Rec */
    bool saw_done;
    ModelStopReason done_reason;
    int error_count;
} Collector;

static void collect_event(void* userdata, const ModelEvent* ev) {
    Collector* c = userdata;
    Rec rec = {0};
    rec.type = ev->type;
    switch (ev->type) {
    case MODEL_EVENT_TEXT_DELTA:
    case MODEL_EVENT_REASONING_DELTA:
        rec.text = strndup(ev->u.text.data, ev->u.text.len);
        break;
    case MODEL_EVENT_TOOL_CALL_START:
    case MODEL_EVENT_TOOL_CALL_END:
        rec.index =
            ev->type == MODEL_EVENT_TOOL_CALL_START ? ev->u.tool_start.index : ev->u.tool_end.index;
        break;
    case MODEL_EVENT_TOOL_CALL_DELTA:
        rec.index = ev->u.tool_delta.index;
        rec.text = strndup(ev->u.tool_delta.delta, ev->u.tool_delta.len);
        break;
    case MODEL_EVENT_USAGE:
        rec.usage = ev->u.usage.usage;
        break;
    case MODEL_EVENT_DONE:
        c->saw_done = true;
        c->done_reason = ev->u.done.reason;
        break;
    case MODEL_EVENT_ERROR:
        c->error_count++;
        rec.text = strdup(ev->u.error.message != NULL ? ev->u.error.message : "");
        break;
    }
    if (ev->type != MODEL_EVENT_DONE) {
        vector_push(&c->events, &rec);
    }
}

static void collector_init(Collector* c) {
    c->events = vector_new(sizeof(Rec));
    c->saw_done = false;
    c->done_reason = MODEL_STOP_UNKNOWN;
    c->error_count = 0;
}

static void collector_free(Collector* c) {
    for (size_t i = 0; i < vector_len(&c->events); i++) {
        Rec* r = vector_at(&c->events, i);
        free(r->text);
    }
    vector_free(&c->events);
}

/* ------------------------------------------------------------------ */
/* local HTTP server (python3, test-only)                              */
/* ------------------------------------------------------------------ */

static const char* PY_SERVER = "import http.server, socketserver, sys\n"
                               "PORT = int(sys.argv[1])\n"
                               "BODY = sys.argv[2]\n"
                               "STATUS = int(sys.argv[3])\n"
                               "import sys\n"
                               "class H(http.server.BaseHTTPRequestHandler):\n"
                               "    def do_POST(self):\n"
                               "        sys.stderr.write('SRV: got POST\\n')\n"
                               "        self.send_response(STATUS)\n"
                               "        self.send_header('Content-Type', 'text/event-stream')\n"
                               "        self.send_header('Content-Length', str(len(BODY)))\n"
                               "        self.end_headers()\n"
                               "        for i in range(0, len(BODY), 7):\n"
                               "            self.wfile.write(BODY[i:i+7].encode('utf-8'))\n"
                               "            self.wfile.flush()\n"
                               "    def log_message(self, *a): pass\n"
                               "socketserver.TCPServer(('127.0.0.1', PORT), H).serve_forever()\n";

static int find_free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &len) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return ntohs(addr.sin_port);
}

/* Spawn the python server; returns its pid or -1. */
static pid_t start_server(int port, const char* body, int status) {
    char port_s[16], status_s[8];
    snprintf(port_s, sizeof(port_s), "%d", port);
    snprintf(status_s, sizeof(status_s), "%d", status);

    pid_t pid = fork();
    if (pid == 0) {
        /* child: exec python3 */
        execlp("python3", "python3", "-c", PY_SERVER, port_s, body, status_s, (char*)NULL);
        _exit(127);
    }
    return pid;
}

static int wait_for_port(int port, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons((uint16_t)port);
            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                close(fd);
                return 0;
            }
            close(fd);
        }
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return -1;
}

static void stop_server(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

/* ------------------------------------------------------------------ */
/* tests                                                               */
/* ------------------------------------------------------------------ */

static Tool test_tool = {
    .name = "read",
    .description = "Read a file",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}",
    .flags = TOOL_FLAG_NONE,
    .execute = NULL, /* never invoked in these tests */
};

static int test_build_request_body(void) {
    Provider* p = provider_new("https://api.example.com/v1", "CAGENT_TEST_UNSET_KEY");
    CHECK(p != NULL);
    Model* m = openai_model_new(p, "test-model", 128000, 4096);
    CHECK(m != NULL);

    ToolRegistry* reg = tool_registry_new();
    CHECK(reg != NULL);
    CHECK(tool_registry_register(reg, &test_tool) == AGENT_OK);

    ModelRequest req = {0};
    req.id = 1;
    req.model = m;
    req.tools = reg;
    MessageList conv = {0};
    Message* historic_system = message_new(MSG_SYSTEM);
    message_set_content(historic_system, "stale rules");
    message_list_append(&conv, historic_system);
    Message* um = message_new(MSG_USER);
    message_set_content(um, "hi\n");
    message_list_append(&conv, um);
    req.messages = &conv;
    req.system_prompt = "project rules";
    req.max_tokens = 2048;
    req.temperature = 0.5;
    req.stream = true;

    String out = string_new();
    CHECK(openai_build_request_body(&req, &out) == AGENT_OK);

    JsonDoc* doc = json_parse(out.data, out.len);
    CHECK(doc != NULL);
    JsonVal* root = json_root(doc);
    CHECK(root != NULL);
    CHECK(strcmp(json_obj_get_str(root, "model"), "test-model") == 0);
    CHECK(json_obj_get_bool(root, "stream", false));
    CHECK(json_obj_get_int(root, "max_tokens", 0) == 2048);

    JsonVal* msgs = json_val_obj_get(root, "messages");
    CHECK(msgs != NULL && json_val_is_arr(msgs) && json_val_arr_size(msgs) == 2);
    JsonVal* m0 = json_val_arr_get(msgs, 0);
    CHECK(strcmp(json_obj_get_str(m0, "role"), "system") == 0);
    CHECK(strcmp(json_obj_get_str(m0, "content"), "project rules") == 0);
    JsonVal* m1 = json_val_arr_get(msgs, 1);
    /* embedded content with a newline must survive round-trip */
    CHECK(strcmp(json_obj_get_str(m1, "content"), "hi\n") == 0);

    JsonVal* tools = json_val_obj_get(root, "tools");
    CHECK(tools != NULL && json_val_arr_size(tools) == 1);
    JsonVal* fn = json_val_obj_get(json_val_arr_get(tools, 0), "function");
    CHECK(strcmp(json_obj_get_str(fn, "name"), "read") == 0);

    json_doc_free(doc);
    string_free(&out);
    tool_registry_free(reg);
    message_list_free(&conv);
    m->ops->destroy(m);
    provider_free(p);
    return g_failures;
}

/* The canonical streaming response used by the e2e tests. */
static const char* SSE_BODY =
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hello \"}}]}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"world\"}}]}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
    "\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"\"}}]}}]}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
    "\"function\":{\"arguments\":\"{\\\"path\\\":\"}}]}}]}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
    "\"function\":{\"arguments\":\"\\\"a.c\\\"}\"}}]}}]}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n"
    "\n"
    "data: {\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}\n"
    "\n"
    "data: [DONE]\n"
    "\n";

static int run_e2e(const char* body, int status, bool cancel_immediately, Collector* out) {
    int port = find_free_port();
    CHECK_MSG(port > 0, "no free port");
    if (port <= 0) {
        return -1;
    }

    pid_t server = start_server(port, body, status);
    CHECK_MSG(server > 0, "fork failed");
    if (server <= 0) {
        return -1;
    }
    if (wait_for_port(port, 3000) != 0) {
        stop_server(server);
        CHECK_MSG(0, "python server did not come up");
        return -1;
    }

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);

    Provider* p = provider_new(url, "CAGENT_TEST_UNSET_KEY");
    /* inject a fake key directly (env var may be unset in CI) */
    free(p->api_key);
    p->api_key = strdup("test-key");
    Model* m = openai_model_new(p, "test-model", 128000, 4096);
    CHECK(m != NULL);

    /* async runtime plumbing: the provider needs an HTTP channel */
    EventLoop* loop = event_loop_new();
    HttpRuntime* http = http_new(loop);
    Runtime fake_rt = {0};
    fake_rt.loop = loop;
    fake_rt.http = http;
    m->runtime = &fake_rt;

    ModelRequest req = {0};
    req.id = 42;
    req.model = m;
    MessageList conv = {0};
    Message* um = message_new(MSG_USER);
    message_set_content(um, "hi");
    message_list_append(&conv, um);
    req.messages = &conv;
    req.stream = true;
    req.event_cb = collect_event;
    req.event_userdata = out;

    collector_init(out);
    CHECK(openai_model_request_wrapper(&req) == AGENT_OK);
    if (cancel_immediately) {
        CHECK(m->ops->cancel(m, req.id) == AGENT_OK);
    }

    /* drive the loop until the transfer completes (async) */
    int loop_iter = 0;
    (void)loop_iter;
    for (; loop_iter < 200 && !out->saw_done && out->error_count == 0; loop_iter++) {
        long t = http_next_timeout_ms(http);
        int to = t >= 0 && t < 50 ? (int)t : 50;
        event_loop_wait(loop, to);
        http_pump(http);
    }
    http_pump(http); /* final drain */

    m->ops->destroy(m);
    provider_free(p);
    message_list_free(&conv);
    http_free(http);
    event_loop_free(loop);
    stop_server(server);
    return 0;
}

/* thin wrapper so the test links against the static ops table */
static int openai_model_request_wrapper(ModelRequest* req) {
    return req->model->ops->request(req->model, req);
}

static int test_e2e_streaming(void) {
    Collector c;
    if (run_e2e(SSE_BODY, 200, false, &c) != 0) {
        collector_free(&c);
        return g_failures;
    }

    CHECK(c.saw_done);
    CHECK(c.done_reason == MODEL_STOP_TOOL_CALLS);
    CHECK(c.error_count == 0);

    /* expected sequence (start/end/delta order):
     * text "Hello ", text "world",
     * tool_start(0), tool_delta(0, "{\"path\":"), tool_delta(0, "\"a.c\"}"),
     * usage, tool_end(0), done */
    size_t n = vector_len(&c.events);
    CHECK(n == 7);
    if (n == 7) {
        Rec* e0 = vector_at(&c.events, 0);
        CHECK(e0->type == MODEL_EVENT_TEXT_DELTA && strcmp(e0->text, "Hello ") == 0);
        Rec* e1 = vector_at(&c.events, 1);
        CHECK(e1->type == MODEL_EVENT_TEXT_DELTA && strcmp(e1->text, "world") == 0);
        Rec* e2 = vector_at(&c.events, 2);
        CHECK(e2->type == MODEL_EVENT_TOOL_CALL_START && e2->index == 0);
        Rec* e3 = vector_at(&c.events, 3);
        CHECK(e3->type == MODEL_EVENT_TOOL_CALL_DELTA && strcmp(e3->text, "{\"path\":") == 0);
        Rec* e4 = vector_at(&c.events, 4);
        CHECK(e4->type == MODEL_EVENT_TOOL_CALL_DELTA && strcmp(e4->text, "\"a.c\"}") == 0);
        Rec* e5 = vector_at(&c.events, 5);
        CHECK(e5->type == MODEL_EVENT_USAGE && e5->usage.input_tokens == 10 &&
              e5->usage.output_tokens == 5 && e5->usage.total_tokens == 15);
        Rec* e6 = vector_at(&c.events, 6);
        CHECK(e6->type == MODEL_EVENT_TOOL_CALL_END && e6->index == 0);
    }

    collector_free(&c);
    return g_failures;
}

static int test_output_limit_finish_reason(void) {
    static const char* body =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"still working\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
        "data: {\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":4096,"
        "\"total_tokens\":4116}}\n\n"
        "data: [DONE]\n\n";
    Collector c;
    if (run_e2e(body, 200, false, &c) != 0) {
        collector_free(&c);
        return g_failures;
    }
    CHECK(c.saw_done);
    CHECK(c.done_reason == MODEL_STOP_MAX_TOKENS);
    CHECK(c.error_count == 0);
    collector_free(&c);
    return g_failures;
}

static int test_e2e_http_error(void) {
    Collector c;
    if (run_e2e("{\"error\":{\"message\":\"rate limited\"}}", 429, false, &c) != 0) {
        collector_free(&c);
        return g_failures;
    }

    CHECK(!c.saw_done);
    CHECK(c.error_count == 1);
    if (vector_len(&c.events) == 1) {
        Rec* e0 = vector_at(&c.events, 0);
        CHECK(e0->type == MODEL_EVENT_ERROR);
        CHECK(e0->text != NULL && strstr(e0->text, "429") != NULL);
    }

    collector_free(&c);
    return g_failures;
}

static int test_clean_eof_without_done_errors(void) {
    Collector c;
    const char* incomplete = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n";
    if (run_e2e(incomplete, 200, false, &c) != 0) {
        collector_free(&c);
        return g_failures;
    }
    CHECK(!c.saw_done);
    CHECK(c.error_count == 1);
    CHECK(vector_len(&c.events) == 2); /* partial text followed by terminal error */
    collector_free(&c);
    return g_failures;
}


static int test_eof_with_finish_reason_no_done_is_ok(void) {
    /* Gateways that omit the [DONE] sentinel but deliver finish_reason
     * must stop normally instead of being reported as a dropped stream. */
    Collector c;
    const char* body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
    if (run_e2e(body, 200, false, &c) != 0) {
        collector_free(&c);
        return g_failures;
    }
    CHECK(c.saw_done);
    CHECK(c.error_count == 0);
    CHECK(c.done_reason == MODEL_STOP_COMPLETE);
    collector_free(&c);

    /* usage alone (no finish_reason, no [DONE]) also stops normally */
    Collector c3;
    const char* usage_only =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{}}],\"usage\":{\"prompt_tokens\":5,"
        "\"completion_tokens\":2,\"total_tokens\":7}}\n\n";
    if (run_e2e(usage_only, 200, false, &c3) != 0) {
        collector_free(&c3);
        return g_failures;
    }
    CHECK(c3.saw_done);
    CHECK(c3.error_count == 0);
    collector_free(&c3);

    /* a bare partial delta with no terminal evidence still errors */
    Collector c2;
    const char* partial = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n";
    if (run_e2e(partial, 200, false, &c2) != 0) {
        collector_free(&c2);
        return g_failures;
    }
    CHECK(!c2.saw_done);
    CHECK(c2.error_count == 1);
    collector_free(&c2);
    return g_failures;
}

static int test_request_scoped_cancel(void) {
    Collector c;
    if (run_e2e(SSE_BODY, 200, true, &c) != 0) {
        collector_free(&c);
        return g_failures;
    }
    CHECK(!c.saw_done);
    CHECK(c.error_count == 1);
    if (vector_len(&c.events) == 1) {
        Rec* ev = vector_at(&c.events, 0);
        CHECK(ev->type == MODEL_EVENT_ERROR);
        CHECK(ev->text != NULL && strstr(ev->text, "aborted") != NULL);
    }
    collector_free(&c);
    return g_failures;
}

/* Regression: an assistant message with NULL content and no tool calls
 * (reasoning-only response) must serialize to content:"" — never to
 * content:null — or strict endpoints (Google Console Go) reject the
 * request with "content or tool_calls must be set". */
static int test_empty_assistant_message(void) {
    Provider* p = provider_new("https://api.example.com/v1", "CAGENT_TEST_UNSET_KEY");
    CHECK(p != NULL);
    Model* m = openai_model_new(p, "test-model", 128000, 4096);
    CHECK(m != NULL);

    ModelRequest req = {0};
    req.id = 1;
    req.model = m;
    req.system_prompt = "rules";
    MessageList conv = {0};

    Message* am = message_new(MSG_ASSISTANT);
    am->content = NULL; /* reasoning-only reply */
    am->tool_calls.len = 0;
    message_list_append(&conv, am);
    Message* um = message_new(MSG_USER);
    message_set_content(um, "next");
    message_list_append(&conv, um);
    req.messages = &conv;

    String out = string_new();
    CHECK(openai_build_request_body(&req, &out) == AGENT_OK);

    JsonDoc* doc = json_parse(out.data, out.len);
    CHECK(doc != NULL);
    JsonVal* root = json_root(doc);
    CHECK(root != NULL);
    JsonVal* msgs = json_val_obj_get(root, "messages");
    CHECK(msgs != NULL && json_val_arr_size(msgs) == 3); /* system + assistant + user */
    JsonVal* m1 = json_val_arr_get(msgs, 1);
    CHECK(strcmp(json_obj_get_str(m1, "role"), "assistant") == 0);
    const char* c0 = json_obj_get_str(m1, "content");
    CHECK(c0 != NULL && c0[0] == '\0'); /* empty string, not null */
    JsonVal* tc = json_val_obj_get(m1, "tool_calls");
    CHECK(tc == NULL); /* no tool_calls field for an empty reply */

    json_doc_free(doc);
    string_free(&out);
    message_list_free(&conv);
    m->ops->destroy(m);
    provider_free(p);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_build_request_body();
    g_failures += test_empty_assistant_message();

    /* e2e requires python3; skip silently when unavailable */
    if (system("python3 --version >/dev/null 2>&1") == 0) {
        g_failures += test_e2e_streaming();
        g_failures += test_output_limit_finish_reason();
        g_failures += test_e2e_http_error();
        g_failures += test_clean_eof_without_done_errors();
        g_failures += test_eof_with_finish_reason_no_done_is_ok();
        g_failures += test_request_scoped_cancel();
    } else {
        printf("test_openai: python3 unavailable, skipping e2e tests\n");
    }

    if (g_failures == 0) {
        printf("test_openai: all tests passed\n");
        return 0;
    }
    printf("test_openai: %d test(s) failed\n", g_failures);
    return 1;
}
