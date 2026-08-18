/*
 * model/responses.c — OpenAI Responses API provider.
 *
 * The transport/lifetime code mirrors the OpenAI-compatible provider, while
 * request serialization and SSE event normalization target /responses.
 */

#include <stdlib.h>
#include <string.h>

#include "agent/message.h"
#include "model/model.h"
#include "model/provider.h"
#include "model/responses.h"
#include "model/stream.h"
#include "runtime/http.h"
#include "runtime/runtime.h"
#include "tool/registry.h"
#include "util/json.h"
#include "util/log.h"
#include "util/string.h"
#include "util/vector.h"

#define RAW_BODY_CAP 8192 /* error-message snippet cap */
#define MAX_TOOL_CALLS 64 /* per request */

/* ---- accumulator: SSE data events -> ModelEvent stream ---------------- */

typedef struct {
    size_t index;
    bool seen_start;
    char* call_id;            /* owned */
    char* name;               /* owned */
    String pending_arguments; /* deltas received before output_item.added */
    size_t argument_len;
} ToolCallState;

typedef struct {
    ModelEventCallback cb;
    void* userdata;
    uint64_t request_id;
    Vector calls; /* ToolCallState */
    bool error_emitted;
    bool done_emitted;
    bool usage_emitted;
} Accum;

static void emit(Accum* a, const ModelEvent* ev) {
    if (!a->error_emitted && !a->done_emitted && a->cb != NULL) {
        a->cb(a->userdata, ev);
    }
}

static void emit_text(Accum* a, const char* data, size_t len) {
    ModelEvent ev = {0};
    ev.type = MODEL_EVENT_TEXT_DELTA;
    ev.request_id = a->request_id;
    ev.u.text.data = data;
    ev.u.text.len = len;
    emit(a, &ev);
}

static void emit_reasoning(Accum* a, const char* data, size_t len) {
    ModelEvent ev = {0};
    ev.type = MODEL_EVENT_REASONING_DELTA;
    ev.request_id = a->request_id;
    ev.u.text.data = data;
    ev.u.text.len = len;
    emit(a, &ev);
}

static void emit_tool_start(Accum* a, size_t index, const char* id, const char* name) {
    ModelEvent ev = {0};
    ev.type = MODEL_EVENT_TOOL_CALL_START;
    ev.request_id = a->request_id;
    ev.u.tool_start.index = index;
    ev.u.tool_start.id = id;
    ev.u.tool_start.name = name;
    emit(a, &ev);
}

static void emit_tool_delta(Accum* a, size_t index, const char* delta, size_t len) {
    ModelEvent ev = {0};
    ev.type = MODEL_EVENT_TOOL_CALL_DELTA;
    ev.request_id = a->request_id;
    ev.u.tool_delta.index = index;
    ev.u.tool_delta.delta = delta;
    ev.u.tool_delta.len = len;
    emit(a, &ev);
}

static void emit_tool_end(Accum* a, size_t index) {
    ModelEvent ev = {0};
    ev.type = MODEL_EVENT_TOOL_CALL_END;
    ev.request_id = a->request_id;
    ev.u.tool_end.index = index;
    emit(a, &ev);
}

static void emit_usage(Accum* a, const Usage* usage) {
    if (a->usage_emitted || a->error_emitted || a->done_emitted) {
        return;
    }
    ModelEvent ev = {0};
    ev.type = MODEL_EVENT_USAGE;
    ev.request_id = a->request_id;
    ev.u.usage.usage = *usage;
    a->usage_emitted = true;
    emit(a, &ev);
}

static void emit_error(Accum* a, int code, const char* message) {
    if (a->error_emitted || a->done_emitted) {
        return;
    }
    ModelEvent ev = {0};
    ev.type = MODEL_EVENT_ERROR;
    ev.request_id = a->request_id;
    ev.u.error.code = code;
    ev.u.error.message = message;
    a->error_emitted = true;
    if (a->cb != NULL) {
        a->cb(a->userdata, &ev);
    }
}

static void emit_done(Accum* a, ModelStopReason reason) {
    if (a->done_emitted || a->error_emitted) {
        return;
    }
    ModelEvent ev = {0};
    ev.type = MODEL_EVENT_DONE;
    ev.request_id = a->request_id;
    ev.u.done.reason = reason;
    a->done_emitted = true;
    if (a->cb != NULL) {
        a->cb(a->userdata, &ev);
    }
}

static ToolCallState* accum_ensure_call(Accum* a, size_t index) {
    if (index >= MAX_TOOL_CALLS) {
        return NULL;
    }
    for (size_t i = 0; i < vector_len(&a->calls); i++) {
        ToolCallState* s = vector_at(&a->calls, i);
        if (s->index == index) {
            return s;
        }
    }
    ToolCallState s = {.index = index, .seen_start = false};
    s.pending_arguments = string_new();
    ToolCallState* result = vector_push(&a->calls, &s);
    if (result == NULL) {
        string_free(&s.pending_arguments);
    }
    return result;
}

static void call_state_set(char** dst, const char* value) {
    if (value == NULL || *dst != NULL) {
        return;
    }
    *dst = strdup(value);
}

static void call_states_free(Vector* calls) {
    for (size_t i = 0; i < vector_len(calls); i++) {
        ToolCallState* state = vector_at(calls, i);
        free(state->call_id);
        free(state->name);
        string_free(&state->pending_arguments);
    }
    vector_free(calls);
}

static void finish_stream(Accum* a, ModelStopReason reason) {
    if (a->done_emitted || a->error_emitted) {
        return;
    }
    for (size_t i = 0; i < vector_len(&a->calls); i++) {
        ToolCallState* state = vector_at(&a->calls, i);
        if (state->seen_start) {
            emit_tool_end(a, state->index);
        }
    }
    if (reason == MODEL_STOP_COMPLETE && vector_len(&a->calls) > 0) {
        reason = MODEL_STOP_TOOL_CALLS;
    }
    emit_done(a, reason);
}

static void emit_call_start_if_needed(Accum* a, ToolCallState* state) {
    if (state != NULL && !state->seen_start) {
        emit_tool_start(a, state->index, state->call_id, state->name);
        state->seen_start = true;
        if (state->pending_arguments.len > 0) {
            emit_tool_delta(a, state->index, state->pending_arguments.data,
                            state->pending_arguments.len);
            state->argument_len += state->pending_arguments.len;
            string_clear(&state->pending_arguments);
        }
    }
}

static void parse_usage(Accum* a, const JsonVal* usage_val) {
    if (usage_val == NULL || !json_val_is_obj(usage_val)) {
        return;
    }
    Usage u = {0};
    u.input_tokens = json_obj_get_int(usage_val, "input_tokens", 0);
    u.output_tokens = json_obj_get_int(usage_val, "output_tokens", 0);
    u.total_tokens = json_obj_get_int(usage_val, "total_tokens", 0);
    JsonVal* details = json_val_obj_get(usage_val, "input_tokens_details");
    if (details != NULL && json_val_is_obj(details)) {
        u.cached_tokens = json_obj_get_int(details, "cached_tokens", 0);
    }
    emit_usage(a, &u);
}

static ModelStopReason parse_incomplete_reason(const JsonVal* response) {
    JsonVal* details = response != NULL ? json_val_obj_get(response, "incomplete_details") : NULL;
    const char* reason =
        details != NULL && json_val_is_obj(details) ? json_obj_get_str(details, "reason") : NULL;
    if (reason == NULL)
        return MODEL_STOP_INCOMPLETE;
    if (strcmp(reason, "max_output_tokens") == 0 || strcmp(reason, "max_tokens") == 0)
        return MODEL_STOP_MAX_TOKENS;
    if (strcmp(reason, "content_filter") == 0)
        return MODEL_STOP_CONTENT_FILTER;
    return MODEL_STOP_INCOMPLETE;
}

/* Parse one Responses API SSE data payload. */
static int handle_event(Accum* a, const char* json, size_t len) {
    JsonDoc* doc = json_parse(json, len);
    if (doc == NULL) {
        return AGENT_ERR_JSON;
    }
    JsonVal* root = json_root(doc);
    const char* type = root != NULL ? json_obj_get_str(root, "type") : NULL;
    if (root == NULL || !json_val_is_obj(root) || type == NULL) {
        json_doc_free(doc);
        return AGENT_ERR_JSON;
    }

    if (strcmp(type, "response.output_text.delta") == 0) {
        const char* delta = json_obj_get_str(root, "delta");
        if (delta != NULL) {
            emit_text(a, delta, strlen(delta));
        }
    } else if (strcmp(type, "response.reasoning_summary_text.delta") == 0 ||
               strcmp(type, "response.reasoning_text.delta") == 0) {
        const char* delta = json_obj_get_str(root, "delta");
        if (delta != NULL) {
            emit_reasoning(a, delta, strlen(delta));
        }
    } else if (strcmp(type, "response.output_item.added") == 0) {
        int64_t index = json_obj_get_int(root, "output_index", -1);
        JsonVal* item = json_val_obj_get(root, "item");
        const char* item_type = item != NULL ? json_obj_get_str(item, "type") : NULL;
        if (index >= 0 && item != NULL &&
            strcmp(item_type != NULL ? item_type : "", "function_call") == 0) {
            ToolCallState* state = accum_ensure_call(a, (size_t)index);
            if (state == NULL) {
                json_doc_free(doc);
                return AGENT_ERR_OOM;
            }
            call_state_set(&state->call_id, json_obj_get_str(item, "call_id"));
            call_state_set(&state->name, json_obj_get_str(item, "name"));
            emit_call_start_if_needed(a, state);
            const char* args = json_obj_get_str(item, "arguments");
            if (args != NULL && args[0] != '\0') {
                emit_tool_delta(a, state->index, args, strlen(args));
                state->argument_len += strlen(args);
            }
        }
    } else if (strcmp(type, "response.function_call_arguments.delta") == 0) {
        int64_t index = json_obj_get_int(root, "output_index", -1);
        const char* delta = json_obj_get_str(root, "delta");
        if (index >= 0 && delta != NULL) {
            ToolCallState* state = accum_ensure_call(a, (size_t)index);
            if (state == NULL) {
                json_doc_free(doc);
                return AGENT_ERR_OOM;
            }
            if (state->seen_start) {
                emit_tool_delta(a, state->index, delta, strlen(delta));
                state->argument_len += strlen(delta);
            } else if (string_append(&state->pending_arguments, delta) != AGENT_OK) {
                json_doc_free(doc);
                return AGENT_ERR_OOM;
            }
        }
    } else if (strcmp(type, "response.function_call_arguments.done") == 0) {
        int64_t index = json_obj_get_int(root, "output_index", -1);
        const char* args = json_obj_get_str(root, "arguments");
        if (index >= 0 && args != NULL) {
            ToolCallState* state = accum_ensure_call(a, (size_t)index);
            if (state == NULL) {
                json_doc_free(doc);
                return AGENT_ERR_OOM;
            }
            if (!state->seen_start && args[0] != '\0') {
                if (string_append(&state->pending_arguments, args) != AGENT_OK) {
                    json_doc_free(doc);
                    return AGENT_ERR_OOM;
                }
            }
            emit_call_start_if_needed(a, state);
            if (state->argument_len == 0 && args[0] != '\0') {
                emit_tool_delta(a, state->index, args, strlen(args));
                state->argument_len = strlen(args);
            }
        }
    } else if (strcmp(type, "response.usage.updated") == 0) {
        parse_usage(a, json_val_obj_get(root, "usage"));
    } else if (strcmp(type, "response.completed") == 0) {
        JsonVal* response = json_val_obj_get(root, "response");
        if (response != NULL && json_val_is_obj(response)) {
            parse_usage(a, json_val_obj_get(response, "usage"));
        }
        finish_stream(a, MODEL_STOP_COMPLETE);
    } else if (strcmp(type, "response.incomplete") == 0) {
        JsonVal* response = json_val_obj_get(root, "response");
        if (response != NULL && json_val_is_obj(response)) {
            parse_usage(a, json_val_obj_get(response, "usage"));
        }
        finish_stream(a, parse_incomplete_reason(response));
    } else if (strcmp(type, "response.failed") == 0 || strcmp(type, "error") == 0) {
        JsonVal* error = json_val_obj_get(root, "error");
        const char* message = error != NULL ? json_obj_get_str(error, "message") : NULL;
        emit_error(a, AGENT_ERR_MODEL, message != NULL ? message : "Responses API error");
    }

    json_doc_free(doc);
    return AGENT_OK;
}

/* ---- SSE -> events glue ---------------------------------------------- */

typedef struct {
    Accum accum;
    SseParser* sse;
    String raw_body; /* response body snippet for error messages */
    HttpRequest* hr; /* owned by the HttpRuntime while in flight */
    Model* model;    /* borrowed; owns the active-transfer table */
    bool finished;
} Transfer;

typedef struct {
    Vector transfers; /* Transfer*; concurrent requests sharing this model */
} ResponsesPriv;

static void transfer_unregister(Transfer* transfer) {
    if (transfer == NULL || transfer->model == NULL || transfer->model->priv == NULL) {
        return;
    }
    ResponsesPriv* priv = transfer->model->priv;
    for (size_t i = 0; i < vector_len(&priv->transfers); i++) {
        Transfer** slot = (Transfer**)vector_at(&priv->transfers, i);
        if (*slot == transfer) {
            size_t last_index = vector_len(&priv->transfers) - 1;
            if (i != last_index) {
                Transfer** last = (Transfer**)vector_at(&priv->transfers, last_index);
                *slot = *last;
            }
            vector_pop(&priv->transfers);
            return;
        }
    }
}

static void transfer_free(Transfer* t) {
    if (t == NULL) {
        return;
    }
    if (t->sse != NULL) {
        sse_parser_free(t->sse);
    }
    string_free(&t->raw_body);
    call_states_free(&t->accum.calls);
    free(t);
}

static void sse_cb(void* userdata, const SseEvent* ev) {
    Transfer* t = userdata;
    if (ev->type == SSE_EVENT_DONE) {
        /* Some gateways still append [DONE]; a typed terminal event remains
         * authoritative, but this is a compatible fallback. */
        finish_stream(&t->accum, MODEL_STOP_UNKNOWN);
    } else if (ev->type == SSE_EVENT_DATA &&
               handle_event(&t->accum, ev->data, ev->len) != AGENT_OK) {
        emit_error(&t->accum, AGENT_ERR_JSON, "malformed stream JSON");
    }
}

static size_t write_cb(HttpRequest* req, const char* ptr, size_t n, void* userdata) {
    (void)req;
    Transfer* t = userdata;

    if (t->raw_body.len < RAW_BODY_CAP) {
        size_t room = RAW_BODY_CAP - t->raw_body.len;
        size_t take = n < room ? n : room;
        string_append_n(&t->raw_body, ptr, take);
    }

    if (sse_parser_feed(t->sse, ptr, n) != AGENT_OK) {
        return 0; /* abort the transfer */
    }
    return n;
}

/* ---- Responses input serialization ----------------------------------- */

static int append_instruction(String* instructions, const char* text) {
    if (text == NULL || text[0] == '\0') {
        return AGENT_OK;
    }
    if (instructions->len > 0 && string_append_char(instructions, '\n') != AGENT_OK) {
        return AGENT_ERR_OOM;
    }
    return string_append(instructions, text);
}

static int add_input_message(JsonBuilder* b, JsonMut* input, const char* role, const char* type,
                             const char* text) {
    JsonMut* item = json_builder_arr_add_obj(b, input);
    JsonMut* content = item != NULL ? json_builder_obj_add_arr(b, item, "content") : NULL;
    JsonMut* part = content != NULL ? json_builder_arr_add_obj(b, content) : NULL;
    if (item == NULL || content == NULL || part == NULL) {
        return AGENT_ERR_OOM;
    }
    json_builder_obj_add_str(b, item, "type", "message");
    json_builder_obj_add_str(b, item, "role", role);
    json_builder_obj_add_str(b, part, "type", type);
    json_builder_obj_add_str(b, part, "text", text != NULL ? text : "");
    return AGENT_OK;
}

static int add_function_call(JsonBuilder* b, JsonMut* input, const ToolCall* tc) {
    JsonMut* item = json_builder_arr_add_obj(b, input);
    if (item == NULL) {
        return AGENT_ERR_OOM;
    }
    json_builder_obj_add_str(b, item, "type", "function_call");
    json_builder_obj_add_str(b, item, "call_id", tc->id != NULL ? tc->id : "");
    json_builder_obj_add_str(b, item, "name", tc->name != NULL ? tc->name : "");
    json_builder_obj_add_str(b, item, "arguments", tc->arguments != NULL ? tc->arguments : "");
    return AGENT_OK;
}

static int add_function_output(JsonBuilder* b, JsonMut* input, const Message* m) {
    JsonMut* item = json_builder_arr_add_obj(b, input);
    if (item == NULL) {
        return AGENT_ERR_OOM;
    }
    json_builder_obj_add_str(b, item, "type", "function_call_output");
    json_builder_obj_add_str(b, item, "call_id", m->tool_call_id != NULL ? m->tool_call_id : "");
    json_builder_obj_add_str(b, item, "output", m->content != NULL ? m->content : "");
    return AGENT_OK;
}

static int add_responses_tools(JsonBuilder* b, JsonMut* root, const ToolRegistry* reg) {
    if (reg == NULL || tool_registry_count(reg) == 0) {
        return AGENT_OK;
    }
    JsonMut* tools = json_builder_obj_add_arr(b, root, "tools");
    if (tools == NULL) {
        return AGENT_ERR_OOM;
    }
    for (size_t i = 0; i < reg->len; i++) {
        if (!reg->enabled[i]) {
            continue;
        }
        Tool* tool = reg->tools[i];
        JsonMut* item = json_builder_arr_add_obj(b, tools);
        if (item == NULL) {
            return AGENT_ERR_OOM;
        }
        json_builder_obj_add_str(b, item, "type", "function");
        json_builder_obj_add_str(b, item, "name", tool->name);
        json_builder_obj_add_str(b, item, "description", tool->description);
        JsonDoc* schema_doc = json_parse(tool->input_schema, strlen(tool->input_schema));
        if (schema_doc == NULL) {
            return AGENT_ERR_JSON;
        }
        JsonVal* schema = json_root(schema_doc);
        int rc = schema != NULL && json_val_is_obj(schema)
                     ? json_builder_obj_add_val_copy(b, item, "parameters", schema)
                     : AGENT_ERR_JSON;
        json_doc_free(schema_doc);
        if (rc != AGENT_OK) {
            return rc;
        }
    }
    return AGENT_OK;
}

/* ---- request body ----------------------------------------------------- */

static const char* responses_api_model_name(const Model* model) {
    if (model == NULL || model->name == NULL || model->provider == NULL ||
        model->provider->provider_name == NULL) {
        return model != NULL ? model->name : NULL;
    }
    const char* provider = model->provider->provider_name;
    size_t provider_len = strlen(provider);
    if (strncmp(model->name, provider, provider_len) == 0 &&
        model->name[provider_len] == '/') {
        return model->name + provider_len + 1;
    }
    return model->name;
}

int responses_build_request_body(ModelRequest* req, String* out) {
    if (req == NULL || out == NULL || req->model == NULL) {
        return AGENT_ERR_MODEL;
    }
    JsonBuilder* b = json_builder_new();
    if (b == NULL) {
        return AGENT_ERR_OOM;
    }
    JsonMut* root = json_builder_root_obj(b);
    JsonMut* input = root != NULL ? json_builder_obj_add_arr(b, root, "input") : NULL;
    if (root == NULL || input == NULL) {
        json_builder_free(b);
        return AGENT_ERR_OOM;
    }

    json_builder_obj_add_str(b, root, "model", responses_api_model_name(req->model));
    bool chatgpt = provider_is_chatgpt(req->model->provider);
    /* ChatGPT Codex uses a restricted Responses request shape: it requires
     * store=false and rejects max_output_tokens/temperature. */
    if (chatgpt) {
        json_builder_obj_add_bool(b, root, "store", false);
    }
    json_builder_obj_add_bool(b, root, "stream", req->stream);
    if (!chatgpt && req->max_tokens > 0) {
        json_builder_obj_add_int(b, root, "max_output_tokens", req->max_tokens);
    }
    if (!chatgpt) {
        json_builder_obj_add_real(b, root, "temperature", req->temperature);
    }
    if (!chatgpt) {
        JsonMut* include = json_builder_obj_add_arr(b, root, "include");
        if (include == NULL ||
            json_builder_arr_add_str(b, include, "response.output_text.delta") != AGENT_OK ||
            json_builder_arr_add_str(b, include, "response.function_call_arguments.delta") !=
                AGENT_OK) {
            json_builder_free(b);
            return AGENT_ERR_OOM;
        }
    }

    String instructions = string_new();
    int rc = append_instruction(&instructions, req->system_prompt);
    const MessageList* msgs = req->messages;
    if (rc == AGENT_OK && msgs != NULL) {
        for (size_t i = 0; i < msgs->len; i++) {
            const Message* m = &msgs->items[i];
            if (m->role == MSG_SYSTEM) {
                rc = append_instruction(&instructions, m->content);
            } else if (m->role == MSG_USER) {
                rc = add_input_message(b, input, "user", "input_text", m->content);
            } else if (m->role == MSG_ASSISTANT) {
                if (m->content != NULL && m->content[0] != '\0') {
                    rc = add_input_message(b, input, "assistant", "output_text", m->content);
                } else if (rc == AGENT_OK && m->tool_calls.len == 0) {
                    /* guard: assistant items with an empty content array
                     * are rejected by strict endpoints */
                    rc = add_input_message(b, input, "assistant", "output_text", "");
                }
                for (size_t k = 0; rc == AGENT_OK && k < m->tool_calls.len; k++) {
                    rc = add_function_call(b, input, &m->tool_calls.items[k]);
                }
            } else if (m->role == MSG_TOOL) {
                rc = add_function_output(b, input, m);
            }
            if (rc != AGENT_OK) {
                break;
            }
        }
    }
    if (rc == AGENT_OK && instructions.len > 0) {
        rc = json_builder_obj_add_str(b, root, "instructions", instructions.data);
    }
    if (rc == AGENT_OK) {
        rc = add_responses_tools(b, root, req->tools);
    }
    string_free(&instructions);
    if (rc != AGENT_OK) {
        json_builder_free(b);
        return rc;
    }
    rc = json_builder_stringify(b, out);
    json_builder_free(b);
    return rc;
}

/* ---- lifecycle -------------------------------------------------------- */

static int responses_cancel(Model* model, uint64_t request_id) {
    if (model == NULL || model->priv == NULL || model->runtime == NULL) {
        return AGENT_ERR_MODEL;
    }
    ResponsesPriv* priv = model->priv;
    for (size_t i = 0; i < vector_len(&priv->transfers); i++) {
        Transfer** slot = (Transfer**)vector_at(&priv->transfers, i);
        Transfer* transfer = *slot;
        if (transfer->accum.request_id == request_id) {
            http_request_abort(model->runtime->http, transfer->hr);
            return AGENT_OK;
        }
    }
    return AGENT_OK; /* already completed/cancelled */
}

static void responses_destroy(Model* model) {
    if (model == NULL) {
        return;
    }
    ResponsesPriv* priv = model->priv;
    if (priv != NULL) {
        while (vector_len(&priv->transfers) > 0) {
            Transfer** slot =
                (Transfer**)vector_at(&priv->transfers, vector_len(&priv->transfers) - 1);
            Transfer* transfer = *slot;
            if (model->runtime != NULL && model->runtime->http != NULL && transfer->hr != NULL) {
                http_request_abort(model->runtime->http, transfer->hr);
            } else {
                transfer_unregister(transfer);
                transfer_free(transfer);
            }
        }
        vector_free(&priv->transfers);
        free(priv);
    }
    free(model->name);
    free(model);
}

/* http completion: flush SSE, report transport/status errors, release */
static void responses_done_cb(HttpRequest* req, const HttpDoneInfo* info, void* ud) {
    (void)req;
    Transfer* t = ud;
    if (t->finished) {
        return;
    }
    t->finished = true;

    sse_parser_finish(t->sse);
    if (info->rc == CURLE_OK && info->http_status >= 200 && info->http_status < 300 &&
        !t->accum.done_emitted && !t->accum.error_emitted) {
        if (t->accum.usage_emitted) {
            /* The gateway delivered chunks (usage or any data) but no
             * terminal response event: close whichever tools were opened
             * and stop normally. */
            finish_stream(&t->accum, MODEL_STOP_UNKNOWN);
        } else {
            emit_error(&t->accum, AGENT_ERR_HTTP,
                       "stream ended before a terminal response event");
        }
    }

    transfer_unregister(t);

    if (info->rc != CURLE_OK) {
        String msg = string_new();
        string_printf(&msg, "http transport error: %s", curl_easy_strerror(info->rc));
        emit_error(&t->accum, AGENT_ERR_HTTP, msg.data);
        string_free(&msg);
    } else if (info->http_status < 200 || info->http_status >= 300) {
        String msg = string_new();
        string_printf(&msg, "http status %d: %s", info->http_status,
                      t->raw_body.len > 0 ? t->raw_body.data : "(empty response body)");
        emit_error(&t->accum, info->http_status, msg.data);
        string_free(&msg);
    }

    transfer_free(t); /* the runtime freed the request already */
}

static int responses_request(Model* model, ModelRequest* req) {
    if (model == NULL || req == NULL) {
        return AGENT_ERR_MODEL;
    }

    Provider* p = model->provider;
    int auth_rc = provider_prepare_auth(p);
    if (auth_rc != AGENT_OK || p == NULL || p->api_key == NULL) {
        Accum accum = {0};
        accum.cb = req->event_cb;
        accum.userdata = req->event_userdata;
        accum.request_id = req->id;
        accum.calls = vector_new(sizeof(ToolCallState));
        emit_error(&accum, 401, provider_auth_error(p));
        call_states_free(&accum.calls);
        return AGENT_OK;
    }

    /* --- serialize the request body --- */
    String body = string_new();
    int err = responses_build_request_body(req, &body);
    if (err != AGENT_OK) {
        Accum accum = {0};
        accum.cb = req->event_cb;
        accum.userdata = req->event_userdata;
        accum.request_id = req->id;
        accum.calls = vector_new(sizeof(ToolCallState));
        emit_error(&accum, err, "failed to serialize request body");
        call_states_free(&accum.calls);
        string_free(&body);
        return AGENT_OK;
    }

    /* --- endpoint URL (trim a trailing '/') --- */
    size_t base_len = strlen(p->base_url);
    while (base_len > 0 && p->base_url[base_len - 1] == '/') {
        base_len--;
    }
    String url = string_new();
    string_append_n(&url, p->base_url, base_len);
    string_append(&url, "/responses");

    /* --- headers --- */
    String auth = string_new();
    string_append(&auth, "Authorization: Bearer ");
    string_append(&auth, p->api_key);
    const char* account_header = provider_account_id(p);
    String account = string_new();
    String originator = string_new();
    if (provider_is_chatgpt(p) && account_header != NULL) {
        string_printf(&account, "ChatGPT-Account-Id: %s", account_header);
        string_append(&originator, "originator: codex_cli_rs");
    }
    const char* headers[5] = {
        "Content-Type: application/json",
        "Accept: text/event-stream",
        auth.data,
        account.data != NULL && account.len > 0 ? account.data : NULL,
        originator.data != NULL && originator.len > 0 ? originator.data : NULL,
    };
    size_t n_headers = 3;
    if (headers[3] != NULL) {
        n_headers++;
    }
    if (headers[4] != NULL) {
        n_headers++;
    }

    Transfer* t = calloc(1, sizeof(Transfer));
    if (t == NULL) {
        string_free(&auth);
        string_free(&account);
        string_free(&originator);
        string_free(&url);
        string_free(&body);
        return AGENT_ERR_OOM;
    }
    t->accum.cb = req->event_cb;
    t->accum.userdata = req->event_userdata;
    t->accum.request_id = req->id;
    t->accum.calls = vector_new(sizeof(ToolCallState));
    t->model = model;
    t->sse = sse_parser_new(sse_cb, t);
    if (t->sse == NULL) {
        transfer_free(t);
        string_free(&auth);
        string_free(&account);
        string_free(&originator);
        string_free(&url);
        string_free(&body);
        return AGENT_ERR_OOM;
    }

    HttpRuntime* http = model->runtime != NULL ? model->runtime->http : NULL;
    if (http == NULL) {
        emit_error(&t->accum, AGENT_ERR_HTTP, "no HTTP runtime (async runtime not initialized)");
        transfer_free(t);
        string_free(&auth);
        string_free(&account);
        string_free(&originator);
        string_free(&url);
        string_free(&body);
        return AGENT_OK;
    }

    ResponsesPriv* priv = model->priv;
    if (priv == NULL || vector_push(&priv->transfers, (const void*)&t) == NULL) {
        transfer_free(t);
        string_free(&auth);
        string_free(&account);
        string_free(&originator);
        string_free(&url);
        string_free(&body);
        return AGENT_ERR_OOM;
    }
    err = http_request_start(http, url.data, body.data, body.len, headers, n_headers, RAW_BODY_CAP,
                             write_cb, t, responses_done_cb, t, &t->hr);
    string_free(&auth);
    string_free(&account);
    string_free(&originator);
    string_free(&url);
    string_free(&body);
    if (err != AGENT_OK) {
        transfer_unregister(t);
        emit_error(&t->accum, AGENT_ERR_HTTP, "failed to start HTTP request");
        transfer_free(t);
        return AGENT_OK;
    }
    return AGENT_OK; /* async: completion arrives via responses_done_cb */
}

static const ModelOps responses_ops = {
    .request = responses_request,
    .cancel = responses_cancel,
    .destroy = responses_destroy,
};

Model* responses_model_new(Provider* provider, const char* name, int64_t context_window,
                           int64_t max_output) {
    if (provider == NULL || name == NULL) {
        return NULL;
    }
    Model* m = calloc(1, sizeof(Model));
    if (m == NULL) {
        return NULL;
    }
    m->name = strdup(name);
    if (m->name == NULL) {
        free(m);
        return NULL;
    }
    m->ops = (ModelOps*)&responses_ops;
    m->provider = provider;
    m->context_window = context_window;
    m->max_output = max_output;
    ResponsesPriv* priv = calloc(1, sizeof(ResponsesPriv));
    if (priv == NULL) {
        free(m->name);
        free(m);
        return NULL;
    }
    priv->transfers = vector_new(sizeof(Transfer*));
    m->priv = priv;
    return m;
}
