/*
 * tests/test_anthropic.c — Anthropic Messages provider tests.
 *
 * Part 1: request-body serialization (system extraction, tool_use input
 * objects, tool_result blocks, same-role merging, tools conversion).
 * Part 2: end-to-end against a local python3 server streaming an
 * Anthropic SSE conversation (text + tool_use + usage).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent/message.h"
#include "model/anthropic.h"
#include "model/model.h"
#include "model/provider.h"
#include "runtime/event_loop.h"
#include "runtime/http.h"
#include "runtime/runtime.h"
#include "test_common.h"
#include "test_server.h"
#include "tool/registry.h"
#include "tool/tool.h"
#include "util/error.h"
#include "util/json.h"
#include "util/string.h"
#include "util/vector.h"

static Tool test_tool = {
    .name = "read",
    .description = "Read a file",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}",
    .flags = TOOL_FLAG_NONE,
    .execute = NULL,
};

/* ------------------------------------------------------------------ */
/* serialization                                                        */
/* ------------------------------------------------------------------ */

static int test_build_request_body(void) {
    Provider* p = provider_new("https://api.anthropic.com", "$CAGENT_TEST_KEY");
    CHECK(p != NULL);
    Model* m = anthropic_model_new(p, "claude-test", 200000, 8192);
    CHECK(m != NULL);

    ToolRegistry* reg = tool_registry_new();
    CHECK(reg != NULL);
    tool_registry_register(reg, &test_tool);

    /* conversation: system, user, assistant(tool_use), tool_result, user */
    MessageList conv = {0};
    Message* sys = message_new(MSG_SYSTEM);
    message_set_content(sys, "you are helpful");
    message_list_append(&conv, sys);
    Message* u1 = message_new(MSG_USER);
    message_set_content(u1, "read the file");
    message_list_append(&conv, u1);
    Message* a1 = message_new(MSG_ASSISTANT);
    message_set_content(a1, "let me check");
    ToolCall* tc = calloc(1, sizeof(ToolCall));
    tc->id = strdup("toolu_1");
    tc->name = strdup("read");
    tc->arguments = strdup("{\"path\":\"a.c\"}");
    tool_call_list_append(&a1->tool_calls, tc);
    message_list_append(&conv, a1);
    Message* tr = message_new(MSG_TOOL);
    tr->tool_call_id = strdup("toolu_1");
    tr->content = strdup("file contents");
    message_list_append(&conv, tr);
    Message* u2 = message_new(MSG_USER);
    message_set_content(u2, "thanks");
    message_list_append(&conv, u2);

    ModelRequest req = {0};
    req.id = 7;
    req.model = m;
    req.tools = reg;
    req.messages = &conv;
    req.max_tokens = 2048;

    String out = string_new();
    CHECK(anthropic_build_request_body(&req, &out) == AGENT_OK);

    JsonDoc* doc = json_parse(out.data, out.len);
    CHECK(doc != NULL);
    JsonVal* root = json_root(doc);
    CHECK(root != NULL);
    CHECK(strcmp(json_obj_get_str(root, "model"), "claude-test") == 0);
    CHECK(json_obj_get_int(root, "max_tokens", 0) == 2048);

    /* system extracted to the top level */
    CHECK(strcmp(json_obj_get_str(root, "system"), "you are helpful") == 0);

    /* messages: user / assistant(tool_use) / user(tool_result+thanks merged) */
    JsonVal* msgs = json_val_obj_get(root, "messages");
    CHECK(msgs != NULL && json_val_arr_size(msgs) == 3);

    JsonVal* m0 = json_val_arr_get(msgs, 0);
    CHECK(strcmp(json_obj_get_str(m0, "role"), "user") == 0);

    JsonVal* m1 = json_val_arr_get(msgs, 1);
    CHECK(strcmp(json_obj_get_str(m1, "role"), "assistant") == 0);
    JsonVal* c1 = json_val_obj_get(m1, "content");
    CHECK(c1 != NULL && json_val_arr_size(c1) == 2);
    JsonVal* b0 = json_val_arr_get(c1, 0);
    CHECK(strcmp(json_obj_get_str(b0, "type"), "text") == 0);
    CHECK(strcmp(json_obj_get_str(b0, "text"), "let me check") == 0);
    JsonVal* b1 = json_val_arr_get(c1, 1);
    CHECK(strcmp(json_obj_get_str(b1, "type"), "tool_use") == 0);
    CHECK(strcmp(json_obj_get_str(b1, "id"), "toolu_1") == 0);
    CHECK(strcmp(json_obj_get_str(b1, "name"), "read") == 0);
    JsonVal* input = json_val_obj_get(b1, "input");
    CHECK(input != NULL && json_val_is_obj(input));
    CHECK(strcmp(json_obj_get_str(input, "path"), "a.c") == 0);

    /* tool_result and the following user message merged into one user msg */
    JsonVal* m2 = json_val_arr_get(msgs, 2);
    CHECK(strcmp(json_obj_get_str(m2, "role"), "user") == 0);
    JsonVal* c2 = json_val_obj_get(m2, "content");
    CHECK(c2 != NULL && json_val_arr_size(c2) == 2);
    JsonVal* tr_block = json_val_arr_get(c2, 0);
    CHECK(strcmp(json_obj_get_str(tr_block, "type"), "tool_result") == 0);
    CHECK(strcmp(json_obj_get_str(tr_block, "tool_use_id"), "toolu_1") == 0);
    CHECK(strcmp(json_obj_get_str(tr_block, "content"), "file contents") == 0);
    JsonVal* thanks_block = json_val_arr_get(c2, 1);
    CHECK(strcmp(json_obj_get_str(thanks_block, "type"), "text") == 0);
    CHECK(strcmp(json_obj_get_str(thanks_block, "text"), "thanks") == 0);

    /* tools converted to the Anthropic shape */
    JsonVal* tools = json_val_obj_get(root, "tools");
    CHECK(tools != NULL && json_val_arr_size(tools) == 1);
    JsonVal* t0 = json_val_arr_get(tools, 0);
    CHECK(strcmp(json_obj_get_str(t0, "name"), "read") == 0);
    JsonVal* schema = json_val_obj_get(t0, "input_schema");
    CHECK(schema != NULL && json_val_is_obj(schema));

    json_doc_free(doc);
    string_free(&out);
    tool_registry_free(reg);
    m->ops->destroy(m);
    provider_free(p);
    message_list_free(&conv);
    return g_failures;
}

/* ------------------------------------------------------------------ */
/* e2e: local server streaming Anthropic SSE                           */
/* ------------------------------------------------------------------ */

typedef struct {
    Vector events; /* type + text copies */
    bool saw_done;
    ModelStopReason done_reason;
    int error_count;
    int64_t in_tokens, out_tokens, cached_tokens;
} Collector;

typedef struct {
    ModelEventType type;
    char* text; /* owned */
    size_t index;
} Rec;

static void collect_event(void* userdata, const ModelEvent* ev) {
    Collector* c = userdata;
    if (ev->type == MODEL_EVENT_DONE) {
        c->saw_done = true;
        c->done_reason = ev->u.done.reason;
        return;
    }
    if (ev->type == MODEL_EVENT_ERROR) {
        c->error_count++;
        return;
    }
    Rec r = {0};
    r.type = ev->type;
    switch (ev->type) {
    case MODEL_EVENT_TEXT_DELTA:
        r.text = strndup(ev->u.text.data, ev->u.text.len);
        break;
    case MODEL_EVENT_TOOL_CALL_START:
    case MODEL_EVENT_TOOL_CALL_END:
        r.index =
            ev->type == MODEL_EVENT_TOOL_CALL_START ? ev->u.tool_start.index : ev->u.tool_end.index;
        break;
    case MODEL_EVENT_TOOL_CALL_DELTA:
        r.index = ev->u.tool_delta.index;
        r.text = strndup(ev->u.tool_delta.delta, ev->u.tool_delta.len);
        break;
    case MODEL_EVENT_USAGE:
        c->in_tokens = ev->u.usage.usage.input_tokens;
        c->out_tokens = ev->u.usage.usage.output_tokens;
        c->cached_tokens = ev->u.usage.usage.cached_tokens;
        break;
    default:
        break;
    }
    if (ev->type != MODEL_EVENT_USAGE) {
        vector_push(&c->events, &r);
    }
}

static void collector_free(Collector* c) {
    for (size_t i = 0; i < vector_len(&c->events); i++) {
        Rec* r = vector_at(&c->events, i);
        free(r->text);
    }
    vector_free(&c->events);
}

static const char* SSE_BODY =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"m1\",\"role\":\"assistant\","
    "\"usage\":{\"input_tokens\":25,\"output_tokens\":0,\"cache_read_input_tokens\":8}}}\n"
    "\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":"
    "{\"type\":\"text\",\"text\":\"\"}}\n"
    "\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
    "{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n"
    "\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
    "{\"type\":\"text_delta\",\"text\":\" world\"}}\n"
    "\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n"
    "\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":"
    "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"read\",\"input\":{}}}\n"
    "\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":"
    "{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\"}}\n"
    "\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":"
    "{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"a.c\\\"}\"}}\n"
    "\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":1}\n"
    "\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},"
    "\"usage\":{\"output_tokens\":12}}\n"
    "\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n"
    "\n";

static int test_e2e_streaming(void) {
    int port = test_server_find_free_port();
    CHECK(port > 0);
    pid_t server = test_server_start(port, SSE_BODY, 200);
    CHECK(server > 0);
    CHECK(test_server_wait(port, 3000) == 0);

    EventLoop* loop = event_loop_new();
    HttpRuntime* http = http_new(loop);
    Runtime fake_rt = {0};
    fake_rt.loop = loop;
    fake_rt.http = http;

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    Provider* p = provider_new(url, "CAGENT_TEST_UNSET_KEY");
    free(p->api_key);
    p->api_key = strdup("test-key");
    Model* m = anthropic_model_new(p, "claude-test", 200000, 8192);
    m->runtime = &fake_rt;

    MessageList conv = {0};
    Message* um = message_new(MSG_USER);
    message_set_content(um, "read a file");
    message_list_append(&conv, um);

    Collector c = {0};
    c.events = vector_new(sizeof(Rec));
    ModelRequest req = {0};
    req.id = 42;
    req.model = m;
    req.messages = &conv;
    req.event_cb = collect_event;
    req.event_userdata = &c;

    CHECK(req.model->ops->request(req.model, &req) == AGENT_OK);

    for (int i = 0; i < 300 && !c.saw_done && c.error_count == 0; i++) {
        long t = http_next_timeout_ms(http);
        int to = t >= 0 && t < 20 ? (int)t : 20;
        event_loop_wait(loop, to);
        http_pump(http);
    }

    CHECK(c.saw_done);
    CHECK(c.done_reason == MODEL_STOP_TOOL_CALLS);
    CHECK(c.error_count == 0);

    /* expected: text "Hello", text " world", tool_start(0),
     * tool_delta x2, tool_end(0), usage, done */
    size_t n = vector_len(&c.events);
    CHECK(n == 6);
    if (n == 6) {
        Rec* e0 = vector_at(&c.events, 0);
        CHECK(e0->type == MODEL_EVENT_TEXT_DELTA && strcmp(e0->text, "Hello") == 0);
        Rec* e1 = vector_at(&c.events, 1);
        CHECK(e1->type == MODEL_EVENT_TEXT_DELTA && strcmp(e1->text, " world") == 0);
        Rec* e2 = vector_at(&c.events, 2);
        CHECK(e2->type == MODEL_EVENT_TOOL_CALL_START && e2->index == 0);
        Rec* e3 = vector_at(&c.events, 3);
        CHECK(e3->type == MODEL_EVENT_TOOL_CALL_DELTA && strcmp(e3->text, "{\"path\":") == 0);
        Rec* e4 = vector_at(&c.events, 4);
        CHECK(e4->type == MODEL_EVENT_TOOL_CALL_DELTA && strcmp(e4->text, "\"a.c\"}") == 0);
        Rec* e5 = vector_at(&c.events, 5);
        CHECK(e5->type == MODEL_EVENT_TOOL_CALL_END && e5->index == 0);
    }
    CHECK(c.in_tokens == 25);
    CHECK(c.out_tokens == 12);
    CHECK(c.cached_tokens == 8); /* cache_read_input_tokens */

    collector_free(&c);
    m->ops->destroy(m);
    provider_free(p);
    message_list_free(&conv);
    http_free(http);
    event_loop_free(loop);
    test_server_stop(server);
    return g_failures;
}

static int test_max_tokens_stop_reason(void) {
    const char* body =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":9}}}\n\n"
        "event: message_delta\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"max_tokens\"},"
        "\"usage\":{\"output_tokens\":4096}}\n\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    int port = test_server_find_free_port();
    CHECK(port > 0);
    pid_t server = test_server_start(port, body, 200);
    CHECK(server > 0);
    CHECK(test_server_wait(port, 3000) == 0);
    EventLoop* loop = event_loop_new();
    HttpRuntime* http = http_new(loop);
    Runtime fake_rt = {.loop = loop, .http = http};
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    Provider* p = provider_new(url, "CAGENT_TEST_UNSET_KEY");
    free(p->api_key);
    p->api_key = strdup("test-key");
    Model* m = anthropic_model_new(p, "claude-test", 200000, 8192);
    m->runtime = &fake_rt;
    MessageList conv = {0};
    Message* user = message_new(MSG_USER);
    message_set_content(user, "continue");
    message_list_append(&conv, user);
    Collector c = {.events = vector_new(sizeof(Rec))};
    ModelRequest req = {.id = 76,
                        .model = m,
                        .messages = &conv,
                        .event_cb = collect_event,
                        .event_userdata = &c};
    CHECK(m->ops->request(m, &req) == AGENT_OK);
    for (int i = 0; i < 300 && !c.saw_done && c.error_count == 0; i++) {
        event_loop_wait(loop, 20);
        http_pump(http);
    }
    CHECK(c.saw_done);
    CHECK(c.done_reason == MODEL_STOP_MAX_TOKENS);
    CHECK(c.error_count == 0);
    CHECK(c.out_tokens == 4096);
    collector_free(&c);
    m->ops->destroy(m);
    provider_free(p);
    message_list_free(&conv);
    http_free(http);
    event_loop_free(loop);
    test_server_stop(server);
    return g_failures;
}

static int test_done_sentinel_compatibility(void) {
    const char* body =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":1}}}\n\n"
        "event: content_block_delta\n"
        "data: "
        "{\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"ok\"}}\n\n"
        "data: [DONE]\n\n";
    int port = test_server_find_free_port();
    CHECK(port > 0);
    pid_t server = test_server_start(port, body, 200);
    CHECK(server > 0);
    CHECK(test_server_wait(port, 3000) == 0);
    EventLoop* loop = event_loop_new();
    HttpRuntime* http = http_new(loop);
    Runtime fake_rt = {.loop = loop, .http = http};
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    Provider* p = provider_new(url, "CAGENT_TEST_UNSET_KEY");
    free(p->api_key);
    p->api_key = strdup("test-key");
    Model* m = anthropic_model_new(p, "claude-test", 200000, 8192);
    m->runtime = &fake_rt;
    MessageList conv = {0};
    Message* user = message_new(MSG_USER);
    message_set_content(user, "hi");
    message_list_append(&conv, user);
    Collector c = {.events = vector_new(sizeof(Rec))};
    ModelRequest req = {.id = 77,
                        .model = m,
                        .messages = &conv,
                        .event_cb = collect_event,
                        .event_userdata = &c};
    CHECK(m->ops->request(m, &req) == AGENT_OK);
    for (int i = 0; i < 300 && !c.saw_done && c.error_count == 0; i++) {
        event_loop_wait(loop, 20);
        http_pump(http);
    }
    CHECK(c.saw_done);
    CHECK(c.error_count == 0);
    collector_free(&c);
    m->ops->destroy(m);
    provider_free(p);
    message_list_free(&conv);
    http_free(http);
    event_loop_free(loop);
    test_server_stop(server);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    setenv("CAGENT_TEST_KEY", "test", 1);
    g_failures += test_build_request_body();

    if (system("python3 --version >/dev/null 2>&1") == 0) {
        g_failures += test_e2e_streaming();
        g_failures += test_max_tokens_stop_reason();
        g_failures += test_done_sentinel_compatibility();
    } else {
        printf("test_anthropic: python3 unavailable, skipping e2e\n");
    }

    if (g_failures == 0) {
        printf("test_anthropic: all tests passed\n");
        return 0;
    }
    printf("test_anthropic: %d test(s) failed\n", g_failures);
    return 1;
}
