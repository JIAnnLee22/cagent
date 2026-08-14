/*
 * model/anthropic.c — Anthropic Messages API provider.
 *
 * Request body (streaming):
 *   { model, max_tokens, system?, messages: [{role, content: blocks}],
 *     tools?: [{name, description, input_schema}], stream: true }
 *
 * Blocks: text / tool_use (assistant), tool_result (user). Adjacent
 * messages of the same role are merged (the API rejects consecutive
 * same-role messages).
 *
 * SSE events: message_start / content_block_start / content_block_delta
 * / content_block_stop / message_delta / message_stop / error / ping.
 * Normalized to ModelEvent like the OpenAI provider.
 */

#include <stdlib.h>
#include <string.h>

#include "agent/message.h"
#include "model/anthropic.h"
#include "model/provider.h"
#include "model/stream.h"
#include "runtime/http.h"
#include "runtime/runtime.h"
#include "tool/registry.h"
#include "util/json.h"
#include "util/log.h"
#include "util/string.h"
#include "util/vector.h"

#define RAW_BODY_CAP 8192
#define MAX_TOOL_CALLS 64

/* ---- accumulator: Anthropic SSE events -> ModelEvent stream ----------- */

typedef struct {
    size_t index;
    bool seen_start;
} ToolCallState;

typedef struct {
    ModelEventCallback cb;
    void* userdata;
    uint64_t request_id;
    Vector calls; /* ToolCallState */
    bool error_emitted;
    bool done_emitted;
    /* stream state */
    bool current_is_tool;
    size_t current_tool_idx;
    size_t tool_counter;
    Usage usage;
    ModelStopReason stop_reason;
} Accum;

static void emit(Accum* a, const ModelEvent* ev) {
    if (!a->error_emitted && !a->done_emitted) {
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
    ModelEvent ev = {0};
    ev.type = MODEL_EVENT_USAGE;
    ev.request_id = a->request_id;
    ev.u.usage.usage = *usage;
    emit(a, &ev);
}

static void emit_error(Accum* a, int code, const char* message) {
    if (a->error_emitted || a->done_emitted)
        return;
    ModelEvent ev = {0};
    ev.type = MODEL_EVENT_ERROR;
    ev.request_id = a->request_id;
    ev.u.error.code = code;
    ev.u.error.message = message;
    emit(a, &ev);
    a->error_emitted = true;
}

static ModelStopReason parse_stop_reason(const char* reason) {
    if (reason == NULL)
        return MODEL_STOP_UNKNOWN;
    if (strcmp(reason, "end_turn") == 0 || strcmp(reason, "stop_sequence") == 0)
        return MODEL_STOP_COMPLETE;
    if (strcmp(reason, "tool_use") == 0)
        return MODEL_STOP_TOOL_CALLS;
    if (strcmp(reason, "max_tokens") == 0)
        return MODEL_STOP_MAX_TOKENS;
    if (strcmp(reason, "refusal") == 0)
        return MODEL_STOP_CONTENT_FILTER;
    return MODEL_STOP_INCOMPLETE;
}

static void emit_done(Accum* a) {
    if (a->error_emitted || a->done_emitted)
        return;
    ModelEvent ev = {0};
    ev.type = MODEL_EVENT_DONE;
    ev.request_id = a->request_id;
    ev.u.done.reason = a->stop_reason;
    a->cb(a->userdata, &ev);
    a->done_emitted = true;
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
    return vector_push(&a->calls, &s);
}

/* Parse one SSE data payload and emit normalized events. */
static void handle_event(Accum* a, const char* json, size_t len) {
    JsonDoc* doc = json_parse(json, len);
    if (doc == NULL) {
        emit_error(a, AGENT_ERR_JSON, "malformed stream JSON");
        return;
    }
    JsonVal* root = json_root(doc);
    if (root == NULL || !json_val_is_obj(root)) {
        json_doc_free(doc);
        emit_error(a, AGENT_ERR_JSON, "malformed stream JSON");
        return;
    }

    const char* type = json_obj_get_str(root, "type");
    if (type == NULL) {
        json_doc_free(doc);
        return;
    }

    if (strcmp(type, "message_start") == 0) {
        JsonVal* msg = json_val_obj_get(root, "message");
        if (msg != NULL && json_val_is_obj(msg)) {
            JsonVal* usage = json_val_obj_get(msg, "usage");
            if (usage != NULL && json_val_is_obj(usage)) {
                a->usage.input_tokens = json_obj_get_int(usage, "input_tokens", 0);
                a->usage.cached_tokens =
                    json_obj_get_int(usage, "cache_read_input_tokens", 0) > 0
                        ? json_obj_get_int(usage, "cache_read_input_tokens", 0)
                        : json_obj_get_int(usage, "cached_tokens", 0);
            }
        }
    } else if (strcmp(type, "content_block_start") == 0) {
        JsonVal* cb = json_val_obj_get(root, "content_block");
        if (cb != NULL && json_val_is_obj(cb)) {
            const char* cb_type = json_obj_get_str(cb, "type");
            if (cb_type != NULL && strcmp(cb_type, "tool_use") == 0) {
                a->current_is_tool = true;
                a->current_tool_idx = a->tool_counter++;
                ToolCallState* st = accum_ensure_call(a, a->current_tool_idx);
                if (st != NULL) {
                    st->seen_start = true;
                }
                emit_tool_start(a, a->current_tool_idx, json_obj_get_str(cb, "id"),
                                json_obj_get_str(cb, "name"));
            } else {
                a->current_is_tool = false;
            }
        }
    } else if (strcmp(type, "content_block_delta") == 0) {
        JsonVal* delta = json_val_obj_get(root, "delta");
        if (delta != NULL && json_val_is_obj(delta)) {
            const char* d_type = json_obj_get_str(delta, "type");
            if (d_type == NULL) {
                /* nothing */
            } else if (strcmp(d_type, "text_delta") == 0) {
                const char* text = json_obj_get_str(delta, "text");
                if (text != NULL) {
                    emit_text(a, text, strlen(text));
                }
            } else if (strcmp(d_type, "input_json_delta") == 0) {
                const char* partial = json_obj_get_str(delta, "partial_json");
                if (partial != NULL && a->current_is_tool) {
                    emit_tool_delta(a, a->current_tool_idx, partial, strlen(partial));
                }
            }
        }
    } else if (strcmp(type, "content_block_stop") == 0) {
        if (a->current_is_tool) {
            emit_tool_end(a, a->current_tool_idx);
            a->current_is_tool = false;
        }
    } else if (strcmp(type, "message_delta") == 0) {
        JsonVal* delta = json_val_obj_get(root, "delta");
        if (delta != NULL && json_val_is_obj(delta)) {
            const char* stop_reason = json_obj_get_str(delta, "stop_reason");
            if (stop_reason != NULL) {
                a->stop_reason = parse_stop_reason(stop_reason);
            }
        }
        JsonVal* usage = json_val_obj_get(root, "usage");
        if (usage != NULL && json_val_is_obj(usage)) {
            a->usage.output_tokens = json_obj_get_int(usage, "output_tokens", 0);
        }
    } else if (strcmp(type, "message_stop") == 0) {
        a->usage.total_tokens = a->usage.input_tokens + a->usage.output_tokens;
        emit_usage(a, &a->usage);
        emit_done(a);
    } else if (strcmp(type, "error") == 0) {
        JsonVal* err = json_val_obj_get(root, "error");
        const char* msg = err != NULL ? json_obj_get_str(err, "message") : NULL;
        emit_error(a, AGENT_ERR_MODEL, msg != NULL ? msg : "anthropic stream error");
    }
    /* "ping" and unknown types: ignore */

    json_doc_free(doc);
}

/* ---- SSE -> events glue ----------------------------------------------- */

typedef struct {
    Accum accum;
    SseParser* sse;
    String raw_body; /* response body snippet for error messages */
    HttpRequest* hr; /* owned by HttpRuntime while in flight */
    Model* model;    /* borrowed; owns the active-transfer table */
    bool finished;
} Transfer;

typedef struct {
    Vector transfers; /* Transfer* */
} AnthropicPriv;

static void transfer_unregister(Transfer* transfer) {
    if (transfer == NULL || transfer->model == NULL || transfer->model->priv == NULL) {
        return;
    }
    AnthropicPriv* priv = transfer->model->priv;
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
    vector_free(&t->accum.calls);
    free(t);
}

static void sse_cb(void* userdata, const SseEvent* ev) {
    Transfer* t = userdata;
    if (ev->type == SSE_EVENT_DATA) {
        handle_event(&t->accum, ev->data, ev->len);
    } else if (ev->type == SSE_EVENT_DONE) {
        /* Some compatible gateways terminate with data: [DONE] instead of
         * Anthropic's message_stop event. */
        if (t->accum.current_is_tool) {
            emit_tool_end(&t->accum, t->accum.current_tool_idx);
            t->accum.current_is_tool = false;
        }
        t->accum.usage.total_tokens = t->accum.usage.input_tokens + t->accum.usage.output_tokens;
        emit_usage(&t->accum, &t->accum.usage);
        emit_done(&t->accum);
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

/* ---- request body serialization ---------------------------------------- */

typedef struct {
    JsonBuilder* b;
    JsonMut* msgs; /* the messages array */
} MsgCtx;

/* Start (or merge into) a message with the given role. The previous
 * message is fetched from the builder on every call — no cached pointers
 * (builder reallocations can move values). */
static JsonMut* msg_start(MsgCtx* c, const char* role) {
    size_t n = json_builder_arr_size(c->b, c->msgs);
    if (n > 0) {
        JsonMut* last = json_builder_arr_get(c->b, c->msgs, n - 1);
        const char* last_role = json_builder_obj_get_str(c->b, last, "role");
        if (last_role != NULL && strcmp(last_role, role) == 0) {
            return last; /* merge into the previous message */
        }
    }
    JsonMut* m = json_builder_arr_add_obj(c->b, c->msgs);
    if (m == NULL) {
        return NULL;
    }
    json_builder_obj_add_str(c->b, m, "role", role);
    json_builder_obj_add_arr(c->b, m, "content");
    return m;
}

static int msg_add_text(MsgCtx* c, const char* role, const char* text) {
    JsonMut* m = msg_start(c, role);
    if (m == NULL) {
        return AGENT_ERR_OOM;
    }
    JsonMut* content = json_builder_obj_get(c->b, m, "content");
    JsonMut* block = json_builder_arr_add_obj(c->b, (JsonMut*)content);
    if (block == NULL) {
        return AGENT_ERR_OOM;
    }
    json_builder_obj_add_str(c->b, block, "type", "text");
    json_builder_obj_add_str(c->b, block, "text", text);
    return AGENT_OK;
}

static int msg_add_tool_use(MsgCtx* c, const ToolCall* tc) {
    JsonMut* m = msg_start(c, "assistant");
    if (m == NULL) {
        return AGENT_ERR_OOM;
    }
    JsonMut* content = json_builder_obj_get(c->b, m, "content");
    JsonMut* block = json_builder_arr_add_obj(c->b, (JsonMut*)content);
    if (block == NULL) {
        return AGENT_ERR_OOM;
    }
    json_builder_obj_add_str(c->b, block, "type", "tool_use");
    if (tc->id != NULL) {
        json_builder_obj_add_str(c->b, block, "id", tc->id);
    }
    if (tc->name != NULL) {
        json_builder_obj_add_str(c->b, block, "name", tc->name);
    }
    /* input: parse the JSON arguments string into an object */
    if (tc->arguments != NULL && tc->arguments[0] != '\0') {
        JsonDoc* args = json_parse(tc->arguments, strlen(tc->arguments));
        if (args != NULL) {
            JsonVal* root = json_root(args);
            if (root != NULL && json_val_is_obj(root)) {
                json_builder_obj_add_val_copy(c->b, block, "input", root);
            }
            json_doc_free(args);
        }
    }
    if (json_builder_obj_get(c->b, block, "input") == NULL) {
        json_builder_obj_add_obj(c->b, block, "input");
    }
    return AGENT_OK;
}

static int msg_add_tool_result(MsgCtx* c, const Message* m) {
    JsonMut* msg = msg_start(c, "user");
    if (msg == NULL) {
        return AGENT_ERR_OOM;
    }
    JsonMut* content = json_builder_obj_get(c->b, msg, "content");
    JsonMut* block = json_builder_arr_add_obj(c->b, (JsonMut*)content);
    if (block == NULL) {
        return AGENT_ERR_OOM;
    }
    json_builder_obj_add_str(c->b, block, "type", "tool_result");
    if (m->tool_call_id != NULL) {
        json_builder_obj_add_str(c->b, block, "tool_use_id", m->tool_call_id);
    }
    json_builder_obj_add_str(c->b, block, "content", m->content != NULL ? m->content : "");
    return AGENT_OK;
}

static int serialize_messages(const MessageList* msgs, const char* system_prompt, String* out) {
    JsonBuilder* b = json_builder_new();
    if (b == NULL) {
        return AGENT_ERR_OOM;
    }
    JsonMut* root = json_builder_root_obj(b);
    if (root == NULL) {
        json_builder_free(b);
        return AGENT_ERR_OOM;
    }

    MsgCtx c = {0};
    c.b = b;
    c.msgs = json_builder_obj_add_arr(b, root, "messages");
    if (c.msgs == NULL) {
        json_builder_free(b);
        return AGENT_ERR_OOM;
    }

    /* system prompt(s): merged into the top-level "system" field */
    String sys = string_new();
    if (system_prompt != NULL) {
        string_append(&sys, system_prompt);
    }
    for (size_t i = 0; i < msgs->len; i++) {
        const Message* m = &msgs->items[i];
        if (m->role == MSG_SYSTEM && m->content != NULL) {
            if (sys.len > 0) {
                string_append_char(&sys, '\n');
            }
            string_append(&sys, m->content);
        }
    }
    if (sys.len > 0) {
        json_builder_obj_add_str(b, root, "system", sys.data);
    }
    string_free(&sys);

    for (size_t i = 0; i < msgs->len; i++) {
        const Message* m = &msgs->items[i];
        switch (m->role) {
        case MSG_SYSTEM:
            break; /* already in the top-level system */
        case MSG_USER:
            if (m->content != NULL) {
                msg_add_text(&c, "user", m->content);
            }
            break;
        case MSG_ASSISTANT:
            if (m->content != NULL && m->content[0] != '\0') {
                msg_add_text(&c, "assistant", m->content);
            } else if (m->tool_calls.len == 0) {
                /* Anthropic rejects assistant messages without content or
                 * tool_use blocks; emit an empty text block as a guard. */
                msg_add_text(&c, "assistant", "");
            }
            for (size_t k = 0; k < m->tool_calls.len; k++) {
                msg_add_tool_use(&c, &m->tool_calls.items[k]);
            }
            break;
        case MSG_TOOL:
            msg_add_tool_result(&c, m);
            break;
        }
    }

    int err = json_builder_stringify(b, out);
    json_builder_free(b);
    return err;
}

int anthropic_build_request_body(ModelRequest* req, String* out) {
    if (req == NULL || out == NULL || req->model == NULL) {
        return AGENT_ERR_MODEL;
    }

    JsonBuilder* b = json_builder_new();
    if (b == NULL) {
        return AGENT_ERR_OOM;
    }
    JsonMut* root = json_builder_root_obj(b);
    if (root == NULL) {
        json_builder_free(b);
        return AGENT_ERR_OOM;
    }
    json_builder_obj_add_str(b, root, "model", req->model->name);
    json_builder_obj_add_int(b, root, "max_tokens", req->max_tokens > 0 ? req->max_tokens : 4096);
    json_builder_obj_add_bool(b, root, "stream", true);

    /* messages (with system extraction) */
    String msgs = string_new();
    int err = serialize_messages(req->messages, req->system_prompt, &msgs);
    if (err != AGENT_OK) {
        string_free(&msgs);
        json_builder_free(b);
        return err;
    }
    JsonDoc* msgs_doc = json_parse(msgs.data, msgs.len);
    string_free(&msgs);
    if (msgs_doc == NULL) {
        json_builder_free(b);
        return AGENT_ERR_JSON;
    }
    JsonVal* msgs_root = json_root(msgs_doc);
    JsonVal* sys = json_val_obj_get(msgs_root, "system");
    if (sys != NULL) {
        json_builder_obj_add_val_copy(b, root, "system", sys);
    }
    JsonVal* arr = json_val_obj_get(msgs_root, "messages");
    if (arr == NULL || !json_val_is_arr(arr)) {
        JsonMut* empty = json_builder_obj_add_arr(b, root, "messages");
        if (empty == NULL) {
            json_doc_free(msgs_doc);
            json_builder_free(b);
            return AGENT_ERR_OOM;
        }
    } else {
        if (json_builder_obj_add_val_copy(b, root, "messages", arr) != AGENT_OK) {
            json_doc_free(msgs_doc);
            json_builder_free(b);
            return AGENT_ERR_OOM;
        }
    }
    json_doc_free(msgs_doc);

    /* tools (Anthropic shape: name/description/input_schema) */
    if (req->tools != NULL && tool_registry_count(req->tools) > 0) {
        String schema = string_new();
        err = tool_registry_schema_json(req->tools, &schema);
        if (err == AGENT_OK) {
            JsonDoc* tools_doc = json_parse(schema.data, schema.len);
            if (tools_doc == NULL) {
                err = AGENT_ERR_JSON;
            } else {
                JsonVal* tools_root = json_root(tools_doc);
                if (tools_root != NULL && json_val_is_arr(tools_root)) {
                    JsonMut* tools = json_builder_obj_add_arr(b, root, "tools");
                    if (tools != NULL) {
                        size_t n = json_val_arr_size(tools_root);
                        for (size_t i = 0; i < n; i++) {
                            JsonVal* t0 = json_val_arr_get(tools_root, i);
                            JsonVal* fn = json_val_obj_get(t0, "function");
                            JsonMut* t1 = json_builder_arr_add_obj(b, tools);
                            if (fn == NULL || t1 == NULL) {
                                err = AGENT_ERR_JSON;
                                break;
                            }
                            const char* name = json_obj_get_str(fn, "name");
                            if (name != NULL) {
                                json_builder_obj_add_str(b, t1, "name", name);
                            }
                            const char* desc = json_obj_get_str(fn, "description");
                            if (desc != NULL) {
                                json_builder_obj_add_str(b, t1, "description", desc);
                            }
                            JsonVal* params = json_val_obj_get(fn, "parameters");
                            if (params != NULL && json_val_is_obj(params)) {
                                json_builder_obj_add_val_copy(b, t1, "input_schema", params);
                            }
                        }
                    }
                } else {
                    err = AGENT_ERR_JSON;
                }
                json_doc_free(tools_doc);
            }
        }
        string_free(&schema);
        if (err != AGENT_OK) {
            json_builder_free(b);
            return err;
        }
    }

    err = json_builder_stringify(b, out);
    json_builder_free(b);
    return err;
}

/* ---- http glue (same pattern as the OpenAI provider) -------------------- */

static void done_cb(HttpRequest* req, const HttpDoneInfo* info, void* ud) {
    (void)req;
    Transfer* t = ud;
    if (t->finished) {
        return;
    }
    t->finished = true;

    sse_parser_finish(t->sse);
    if (info->rc == CURLE_OK && info->http_status >= 200 && info->http_status < 300 &&
        !t->accum.done_emitted && !t->accum.error_emitted) {
        if (t->accum.stop_reason != MODEL_STOP_UNKNOWN || t->accum.usage.input_tokens > 0) {
            /* The gateway delivered content (delta + stop_reason or any
             * chunk) but omitted message_stop: close the stream normally. */
            if (t->accum.current_is_tool) {
                emit_tool_end(&t->accum, t->accum.current_tool_idx);
                t->accum.current_is_tool = false;
            }
            t->accum.usage.total_tokens =
                t->accum.usage.input_tokens + t->accum.usage.output_tokens;
            emit_usage(&t->accum, &t->accum.usage);
            emit_done(&t->accum);
        } else {
            emit_error(&t->accum, AGENT_ERR_HTTP, "stream ended before message_stop");
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

    transfer_free(t);
}

static int anthropic_request(Model* model, ModelRequest* req) {
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
        vector_free(&accum.calls);
        return AGENT_OK;
    }

    String body = string_new();
    int err = anthropic_build_request_body(req, &body);
    if (err != AGENT_OK) {
        Accum accum = {0};
        accum.cb = req->event_cb;
        accum.userdata = req->event_userdata;
        accum.request_id = req->id;
        accum.calls = vector_new(sizeof(ToolCallState));
        emit_error(&accum, err, "failed to serialize request body");
        vector_free(&accum.calls);
        string_free(&body);
        return AGENT_OK;
    }

    /* endpoint: {base}/v1/messages — but tolerate a base_url that already
     * ends in /v1 (e.g. OpenCode Go's .../zen/go/v1) */
    size_t base_len = strlen(p->base_url);
    while (base_len > 0 && p->base_url[base_len - 1] == '/') {
        base_len--;
    }
    String url = string_new();
    string_append_n(&url, p->base_url, base_len);
    if (base_len >= 3 && strncmp(p->base_url + base_len - 3, "/v1", 3) == 0) {
        string_append(&url, "/messages");
    } else {
        string_append(&url, "/v1/messages");
    }

    String auth = string_new();
    string_append(&auth, "x-api-key: ");
    string_append(&auth, p->api_key);
    const char* headers[] = {
        "Content-Type: application/json",
        "anthropic-version: 2023-06-01",
        auth.data,
    };

    Transfer* t = calloc(1, sizeof(Transfer));
    if (t == NULL) {
        string_free(&auth);
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
        string_free(&url);
        string_free(&body);
        return AGENT_ERR_OOM;
    }

    HttpRuntime* http = model->runtime != NULL ? model->runtime->http : NULL;
    if (http == NULL) {
        emit_error(&t->accum, AGENT_ERR_HTTP, "no HTTP runtime (async runtime not initialized)");
        transfer_free(t);
        string_free(&auth);
        string_free(&url);
        string_free(&body);
        return AGENT_OK;
    }

    AnthropicPriv* priv = model->priv;
    if (priv == NULL || vector_push(&priv->transfers, (const void*)&t) == NULL) {
        transfer_free(t);
        string_free(&auth);
        string_free(&url);
        string_free(&body);
        return AGENT_ERR_OOM;
    }
    err = http_request_start(http, url.data, body.data, body.len, headers, 3, RAW_BODY_CAP,
                             write_cb, t, done_cb, t, &t->hr);
    string_free(&auth);
    string_free(&url);
    string_free(&body);
    if (err != AGENT_OK) {
        transfer_unregister(t);
        emit_error(&t->accum, AGENT_ERR_HTTP, "failed to start HTTP request");
        transfer_free(t);
        return AGENT_OK;
    }
    return AGENT_OK;
}

static int anthropic_cancel(Model* model, uint64_t request_id) {
    if (model == NULL || model->priv == NULL || model->runtime == NULL) {
        return AGENT_ERR_MODEL;
    }
    AnthropicPriv* priv = model->priv;
    for (size_t i = 0; i < vector_len(&priv->transfers); i++) {
        Transfer** slot = (Transfer**)vector_at(&priv->transfers, i);
        Transfer* transfer = *slot;
        if (transfer->accum.request_id == request_id) {
            http_request_abort(model->runtime->http, transfer->hr);
            return AGENT_OK;
        }
    }
    return AGENT_OK;
}

static void anthropic_destroy(Model* model) {
    if (model == NULL) {
        return;
    }
    AnthropicPriv* priv = model->priv;
    if (priv != NULL) {
        while (vector_len(&priv->transfers) > 0) {
            Transfer** slot =
                (Transfer**)vector_at(&priv->transfers, vector_len(&priv->transfers) - 1);
            Transfer* transfer = *slot;
            if (model->runtime != NULL && transfer->hr != NULL) {
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

static const ModelOps anthropic_ops = {
    .request = anthropic_request,
    .cancel = anthropic_cancel,
    .destroy = anthropic_destroy,
};

Model* anthropic_model_new(Provider* provider, const char* name, int64_t context_window,
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
    m->ops = (ModelOps*)&anthropic_ops;
    m->provider = provider;
    m->context_window = context_window;
    m->max_output = max_output;
    AnthropicPriv* priv = calloc(1, sizeof(AnthropicPriv));
    if (priv == NULL) {
        free(m->name);
        free(m);
        return NULL;
    }
    priv->transfers = vector_new(sizeof(Transfer*));
    m->priv = priv;
    return m;
}
