/*
 * tests/test_responses.c — OpenAI Responses provider tests.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent/message.h"
#include "model/provider.h"
#include "model/responses.h"
#include "runtime/event_loop.h"
#include "runtime/http.h"
#include "runtime/runtime.h"
#include "test_common.h"
#include "test_server.h"
#include "tool/registry.h"
#include "util/json.h"
#include "util/string.h"

static Tool test_tool = {
    .name = "read",
    .description = "Read a file",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}",
};

static int test_build_request_body(void) {
    Provider* provider = provider_new("http://127.0.0.1:1/v1", "test-key");
    Model* model = responses_model_new(provider, "test-model", 128000, 8192);
    ToolRegistry* tools = tool_registry_new();
    MessageList messages = {0};
    CHECK(provider != NULL && model != NULL && tools != NULL);
    if (provider == NULL || model == NULL || tools == NULL) {
        if (model != NULL) {
            model->ops->destroy(model);
        }
        provider_free(provider);
        tool_registry_free(tools);
        return g_failures;
    }
    CHECK(tool_registry_register(tools, &test_tool) == AGENT_OK);

    Message* system = message_new(MSG_SYSTEM);
    Message* user = message_new(MSG_USER);
    Message* assistant = message_new(MSG_ASSISTANT);
    Message* tool = message_new(MSG_TOOL);
    CHECK(system != NULL && user != NULL && assistant != NULL && tool != NULL);
    if (system == NULL || user == NULL || assistant == NULL || tool == NULL) {
        message_free(system);
        message_free(user);
        message_free(assistant);
        message_free(tool);
        message_list_free(&messages);
        tool_registry_free(tools);
        model->ops->destroy(model);
        provider_free(provider);
        return g_failures;
    }
    message_set_content(system, "system rule");
    message_set_content(user, "hello\nworld");
    ToolCall* call = calloc(1, sizeof(ToolCall));
    call->id = strdup("call_1");
    call->name = strdup("read");
    call->arguments = strdup("{\"path\":\"a.c\"}");
    tool_call_list_append(&assistant->tool_calls, call);
    tool->tool_call_id = strdup("call_1");
    tool->content = strdup("file contents");
    message_list_append(&messages, system);
    message_list_append(&messages, user);
    message_list_append(&messages, assistant);
    message_list_append(&messages, tool);

    ModelRequest req = {0};
    req.model = model;
    req.messages = &messages;
    req.tools = tools;
    req.system_prompt = "base instruction";
    req.max_tokens = 2048;
    req.temperature = 0.5;
    req.stream = true;
    String body = string_new();
    CHECK(responses_build_request_body(&req, &body) == AGENT_OK);
    JsonDoc* doc = json_parse(body.data, body.len);
    CHECK(doc != NULL);
    if (doc != NULL) {
        JsonVal* root = json_root(doc);
        JsonVal* input = json_val_obj_get(root, "input");
        JsonVal* instructions = json_val_obj_get(root, "instructions");
        JsonVal* tools_val = json_val_obj_get(root, "tools");
        CHECK(strcmp(json_obj_get_str(root, "model"), "test-model") == 0);
        CHECK(json_obj_get_int(root, "max_output_tokens", 0) == 2048);
        CHECK(json_val_is_str(instructions) &&
              strstr(json_val_str(instructions), "system rule") != NULL);
        CHECK(json_val_is_arr(input) && json_val_arr_size(input) == 3);
        CHECK(strcmp(json_obj_get_str(json_val_arr_get(input, 0), "role"), "user") == 0);
        CHECK(strcmp(json_obj_get_str(json_val_arr_get(input, 0), "type"), "message") == 0);
        CHECK(strcmp(json_obj_get_str(json_val_arr_get(input, 1), "type"), "function_call") == 0);
        CHECK(strcmp(json_obj_get_str(json_val_arr_get(input, 2), "type"),
                     "function_call_output") == 0);
        CHECK(json_val_is_arr(tools_val) && json_val_arr_size(tools_val) == 1);
        CHECK(strcmp(json_obj_get_str(json_val_arr_get(tools_val, 0), "name"), "read") == 0);
        json_doc_free(doc);
    }

    /* ChatGPT Codex requires Responses requests to opt out of server-side
     * storage, while the regular Responses provider keeps its default shape. */
    Provider* chat_provider =
        provider_new_chatgpt("https://chatgpt.com/backend-api/codex", NULL);
    Model* chat_model = chat_provider != NULL
                            ? responses_model_new(chat_provider, "chatgpt/gpt-5.4", 272000, 8192)
                            : NULL;
    CHECK(chat_provider != NULL && chat_model != NULL);
    if (chat_provider != NULL && chat_model != NULL) {
        ModelRequest chat_req = req;
        chat_req.model = chat_model;
        String chat_body = string_new();
        CHECK(responses_build_request_body(&chat_req, &chat_body) == AGENT_OK);
        JsonDoc* chat_doc = json_parse(chat_body.data, chat_body.len);
        CHECK(chat_doc != NULL);
        if (chat_doc != NULL) {
            JsonVal* chat_root = json_root(chat_doc);
            CHECK(strcmp(json_obj_get_str(chat_root, "model"), "gpt-5.4") == 0);
            JsonVal* store = json_val_obj_get(chat_root, "store");
            CHECK(json_val_is_bool(store) && !json_val_bool(store));
            CHECK(json_val_obj_get(chat_root, "max_output_tokens") == NULL);
            CHECK(json_val_obj_get(chat_root, "temperature") == NULL);
            CHECK(json_val_obj_get(chat_root, "include") == NULL);
            json_doc_free(chat_doc);
        }
        string_free(&chat_body);
        chat_model->ops->destroy(chat_model);
        provider_free(chat_provider);
    } else {
        if (chat_model != NULL) {
            chat_model->ops->destroy(chat_model);
        }
        provider_free(chat_provider);
    }

    string_free(&body);
    message_list_free(&messages);
    tool_registry_free(tools);
    model->ops->destroy(model);
    provider_free(provider);
    return g_failures;
}

typedef struct {
    String text;
    String arguments;
    String error;
    size_t starts;
    size_t ends;
    bool done;
    bool failed;
    ModelStopReason done_reason;
    Usage usage;
    char* call_id;
    char* tool_name;
} Collector;

static void collect_event(void* userdata, const ModelEvent* ev) {
    Collector* c = userdata;
    switch (ev->type) {
    case MODEL_EVENT_TEXT_DELTA:
        string_append_n(&c->text, ev->u.text.data, ev->u.text.len);
        break;
    case MODEL_EVENT_TOOL_CALL_START:
        c->starts++;
        free(c->call_id);
        free(c->tool_name);
        c->call_id = ev->u.tool_start.id != NULL ? strdup(ev->u.tool_start.id) : NULL;
        c->tool_name = ev->u.tool_start.name != NULL ? strdup(ev->u.tool_start.name) : NULL;
        break;
    case MODEL_EVENT_TOOL_CALL_DELTA:
        string_append_n(&c->arguments, ev->u.tool_delta.delta, ev->u.tool_delta.len);
        break;
    case MODEL_EVENT_TOOL_CALL_END:
        c->ends++;
        break;
    case MODEL_EVENT_USAGE:
        c->usage = ev->u.usage.usage;
        break;
    case MODEL_EVENT_DONE:
        c->done = true;
        c->done_reason = ev->u.done.reason;
        break;
    case MODEL_EVENT_ERROR:
        c->failed = true;
        string_append(&c->error, ev->u.error.message != NULL ? ev->u.error.message : "error");
        break;
    case MODEL_EVENT_REASONING_DELTA:
        break;
    }
}

static void collector_init(Collector* c) {
    memset(c, 0, sizeof(*c));
    c->text = string_new();
    c->arguments = string_new();
    c->error = string_new();
}

static void collector_free(Collector* c) {
    string_free(&c->text);
    string_free(&c->arguments);
    string_free(&c->error);
    free(c->call_id);
    free(c->tool_name);
}

static int test_streaming(void) {
    static const char* body =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hello \"}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"world\"}\n\n"
        "data: "
        "{\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":{\"type\":\"function_"
        "call\",\"call_id\":\"call_1\",\"name\":\"read\",\"arguments\":\"\"}}\n\n"
        "data: "
        "{\"type\":\"response.function_call_arguments.delta\",\"output_index\":1,\"delta\":\"{"
        "\\\"path\\\":\"}\n\n"
        "data: "
        "{\"type\":\"response.function_call_arguments.delta\",\"output_index\":1,\"delta\":\"\\\"a."
        "c\\\"}\"}\n\n"
        "data: "
        "{\"type\":\"response.completed\",\"response\":{\"usage\":{\"input_tokens\":10,\"output_"
        "tokens\":5,\"total_tokens\":15}}}\n\n";
    int port = test_server_find_free_port();
    CHECK(port > 0);
    if (port <= 0) {
        return g_failures;
    }
    pid_t server = test_server_start(port, body, 200);
    CHECK(server > 0 && test_server_wait(port, 3000) == 0);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    Provider* provider = provider_new(url, "test-key");
    Model* model = responses_model_new(provider, "test-model", 128000, 8192);
    EventLoop* loop = event_loop_new();
    HttpRuntime* http = http_new(loop);
    Runtime fake = {0};
    fake.loop = loop;
    fake.http = http;
    model->runtime = &fake;
    Collector c;
    collector_init(&c);
    ModelRequest req = {.id = 42,
                        .model = model,
                        .event_cb = collect_event,
                        .event_userdata = &c,
                        .stream = true,
                        .max_tokens = 128};
    CHECK(model->ops->request(model, &req) == AGENT_OK);
    for (int i = 0; i < 300 && !c.done && !c.failed; i++) {
        long timeout = http_next_timeout_ms(http);
        if (timeout < 0 || timeout > 20) {
            timeout = 20;
        }
        event_loop_wait(loop, (int)timeout);
        http_pump(http);
    }
    CHECK(c.done && !c.failed);
    CHECK(c.done_reason == MODEL_STOP_TOOL_CALLS);
    CHECK(strcmp(c.text.data, "Hello world") == 0);
    CHECK(c.starts == 1 && c.ends == 1);
    CHECK(strcmp(c.call_id, "call_1") == 0);
    CHECK(strcmp(c.tool_name, "read") == 0);
    CHECK(strcmp(c.arguments.data, "{\"path\":\"a.c\"}") == 0);
    CHECK(c.usage.input_tokens == 10 && c.usage.output_tokens == 5 && c.usage.total_tokens == 15);

    collector_free(&c);
    model->ops->destroy(model);
    provider_free(provider);
    http_free(http);
    event_loop_free(loop);
    test_server_stop(server);
    return g_failures;
}

static int test_incomplete_max_output_tokens(void) {
    static const char* body =
        "data: {\"type\":\"response.reasoning_text.delta\",\"delta\":\"still working\"}\n\n"
        "data: {\"type\":\"response.incomplete\",\"response\":{"
        "\"incomplete_details\":{\"reason\":\"max_output_tokens\"},"
        "\"usage\":{\"input_tokens\":10,\"output_tokens\":4096,\"total_tokens\":4106}}}\n\n";
    int port = test_server_find_free_port();
    CHECK(port > 0);
    if (port <= 0) {
        return g_failures;
    }
    pid_t server = test_server_start(port, body, 200);
    CHECK(server > 0 && test_server_wait(port, 3000) == 0);
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    Provider* provider = provider_new(url, "test-key");
    Model* model = responses_model_new(provider, "test-model", 128000, 8192);
    EventLoop* loop = event_loop_new();
    HttpRuntime* http = http_new(loop);
    Runtime fake = {.loop = loop, .http = http};
    model->runtime = &fake;
    Collector c;
    collector_init(&c);
    ModelRequest req = {.id = 44,
                        .model = model,
                        .event_cb = collect_event,
                        .event_userdata = &c,
                        .stream = true,
                        .max_tokens = 4096};
    CHECK(model->ops->request(model, &req) == AGENT_OK);
    for (int i = 0; i < 300 && !c.done && !c.failed; i++) {
        event_loop_wait(loop, 20);
        http_pump(http);
    }
    CHECK(c.done && !c.failed);
    CHECK(c.done_reason == MODEL_STOP_MAX_TOKENS);
    CHECK(c.usage.output_tokens == 4096);
    collector_free(&c);
    model->ops->destroy(model);
    provider_free(provider);
    http_free(http);
    event_loop_free(loop);
    test_server_stop(server);
    return g_failures;
}

static int test_http_error(void) {
    static const char* body = "{\"error\":{\"message\":\"rate limited\"}}";
    int port = test_server_find_free_port();
    CHECK(port > 0);
    if (port <= 0) {
        return g_failures;
    }
    pid_t server = test_server_start(port, body, 429);
    CHECK(server > 0 && test_server_wait(port, 3000) == 0);
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    Provider* provider = provider_new(url, "test-key");
    Model* model = responses_model_new(provider, "test-model", 128000, 8192);
    EventLoop* loop = event_loop_new();
    HttpRuntime* http = http_new(loop);
    Runtime fake = {0};
    fake.loop = loop;
    fake.http = http;
    model->runtime = &fake;
    Collector c;
    collector_init(&c);
    ModelRequest req = {.id = 43,
                        .model = model,
                        .event_cb = collect_event,
                        .event_userdata = &c,
                        .stream = true};
    CHECK(model->ops->request(model, &req) == AGENT_OK);
    for (int i = 0; i < 200 && !c.failed; i++) {
        event_loop_wait(loop, 20);
        http_pump(http);
    }
    CHECK(c.failed && strstr(c.error.data, "429") != NULL);
    collector_free(&c);
    model->ops->destroy(model);
    provider_free(provider);
    http_free(http);
    event_loop_free(loop);
    test_server_stop(server);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_build_request_body();
    g_failures += test_streaming();
    g_failures += test_incomplete_max_output_tokens();
    g_failures += test_http_error();
    if (g_failures == 0) {
        printf("test_responses: all tests passed\n");
        return 0;
    }
    printf("test_responses: %d test(s) failed\n", g_failures);
    return 1;
}
