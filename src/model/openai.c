/*
 * model/openai.c — OpenAI-compatible provider.
 *
 * Request path: build body JSON -> POST {base}/chat/completions via
 * libcurl easy (Phase 1; the async runtime in Phase 4 moves this onto
 * curl multi + epoll) -> write callback feeds the SSE parser -> each
 * complete data event is parsed and normalized into ModelEvent values
 * delivered to the agent callback.
 *
 * Error mapping: transport failures -> MODEL_EVENT_ERROR(code<0);
 * non-2xx HTTP -> MODEL_EVENT_ERROR(code=status, message=body snippet);
 * malformed SSE JSON -> MODEL_EVENT_ERROR(code=AGENT_ERR_JSON).
 */

#include <stdlib.h>
#include <string.h>

#include "agent/message.h"
#include "model/model.h"
#include "model/openai.h"
#include "model/provider.h"
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
} ToolCallState;

typedef struct {
    ModelEventCallback cb;
    void* userdata;
    uint64_t request_id;
    Vector calls; /* ToolCallState */
    bool error_emitted;
    bool done_emitted;
    bool usage_emitted; /* a usage object arrived before the stream closed */
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
    ModelEvent ev = {0};
    ev.type = MODEL_EVENT_USAGE;
    ev.request_id = a->request_id;
    ev.u.usage.usage = *usage;
    a->usage_emitted = true;
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

static ModelStopReason parse_finish_reason(const char* reason) {
    if (reason == NULL)
        return MODEL_STOP_UNKNOWN;
    if (strcmp(reason, "stop") == 0)
        return MODEL_STOP_COMPLETE;
    if (strcmp(reason, "tool_calls") == 0 || strcmp(reason, "function_call") == 0)
        return MODEL_STOP_TOOL_CALLS;
    if (strcmp(reason, "length") == 0)
        return MODEL_STOP_MAX_TOKENS;
    if (strcmp(reason, "content_filter") == 0)
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

/* Parse one SSE data payload (one chat.completion.chunk) and emit events.
 * Returns AGENT_OK or AGENT_ERR_JSON on malformed payload. */
static int handle_chunk(Accum* a, const char* json, size_t len) {
    JsonDoc* doc = json_parse(json, len);
    if (doc == NULL) {
        return AGENT_ERR_JSON;
    }
    JsonVal* root = json_root(doc);
    if (root == NULL || !json_val_is_obj(root)) {
        json_doc_free(doc);
        return AGENT_ERR_JSON;
    }

    /* usage-only chunks (choices == [] with stream_options.include_usage) */
    JsonVal* usage_val = json_val_obj_get(root, "usage");
    if (usage_val != NULL && json_val_is_obj(usage_val)) {
        Usage u = {0};
        u.input_tokens = json_obj_get_int(usage_val, "prompt_tokens", 0);
        u.output_tokens = json_obj_get_int(usage_val, "completion_tokens", 0);
        u.cached_tokens =
            json_obj_get_int(usage_val, "prompt_tokens_details", 0) == 0
                ? json_obj_get_int(json_val_obj_get(usage_val, "prompt_tokens_details"),
                                   "cached_tokens", 0)
                : 0;
        u.total_tokens = json_obj_get_int(usage_val, "total_tokens", 0);
        emit_usage(a, &u);
    }

    JsonVal* choices = json_val_obj_get(root, "choices");
    if (choices == NULL || json_val_arr_size(choices) == 0) {
        json_doc_free(doc);
        return AGENT_OK;
    }
    JsonVal* choice = json_val_arr_get(choices, 0);
    const char* finish_reason = json_obj_get_str(choice, "finish_reason");
    if (finish_reason != NULL) {
        a->stop_reason = parse_finish_reason(finish_reason);
    }
    JsonVal* delta = json_val_obj_get(choice, "delta");
    if (delta == NULL || !json_val_is_obj(delta)) {
        json_doc_free(doc);
        return AGENT_OK;
    }

    JsonVal* content = json_val_obj_get(delta, "content");
    if (content != NULL && json_val_is_str(content)) {
        const char* s = json_val_str(content);
        emit_text(a, s, strlen(s));
    }

    JsonVal* reasoning = json_val_obj_get(delta, "reasoning_content");
    if (reasoning != NULL && json_val_is_str(reasoning)) {
        const char* s = json_val_str(reasoning);
        emit_reasoning(a, s, strlen(s));
    }

    JsonVal* calls = json_val_obj_get(delta, "tool_calls");
    if (calls != NULL && json_val_is_arr(calls)) {
        size_t n = json_val_arr_size(calls);
        for (size_t i = 0; i < n; i++) {
            JsonVal* item = json_val_arr_get(calls, i);
            if (item == NULL || !json_val_is_obj(item)) {
                continue;
            }
            int64_t idx = json_obj_get_int(item, "index", -1);
            if (idx < 0) {
                continue;
            }
            ToolCallState* st = accum_ensure_call(a, (size_t)idx);
            if (st == NULL) {
                continue;
            }

            const char* id = json_obj_get_str(item, "id");
            JsonVal* fn = json_val_obj_get(item, "function");
            const char* name = NULL;
            const char* args = NULL;
            if (fn != NULL && json_val_is_obj(fn)) {
                name = json_obj_get_str(fn, "name");
                args = json_obj_get_str(fn, "arguments");
            }

            if (!st->seen_start) {
                emit_tool_start(a, (size_t)idx, id, name);
                st->seen_start = true;
                /* a first fragment may already carry a non-empty arguments
                 * prefix; emit it so nothing is lost */
                if (args != NULL && args[0] != '\0') {
                    emit_tool_delta(a, (size_t)idx, args, strlen(args));
                }
            } else if (args != NULL && args[0] != '\0') {
                emit_tool_delta(a, (size_t)idx, args, strlen(args));
            }
        }
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
} OpenAIPriv;

static void transfer_unregister(Transfer* transfer) {
    if (transfer == NULL || transfer->model == NULL || transfer->model->priv == NULL) {
        return;
    }
    OpenAIPriv* priv = transfer->model->priv;
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
    if (ev->type == SSE_EVENT_DONE) {
        /* close out every seen tool call, then the stream */
        for (size_t i = 0; i < vector_len(&t->accum.calls); i++) {
            ToolCallState* s = vector_at(&t->accum.calls, i);
            emit_tool_end(&t->accum, s->index);
        }
        emit_done(&t->accum);
        return;
    }
    if (ev->type == SSE_EVENT_DATA) {
        if (handle_chunk(&t->accum, ev->data, ev->len) != AGENT_OK) {
            emit_error(&t->accum, AGENT_ERR_JSON, "malformed stream JSON");
        }
        return;
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

/* ---- message serialization (OpenAI wire format) ---------------------- */

static int serialize_messages(const MessageList* msgs, const char* system_prompt, String* out) {
    JsonBuilder* b = json_builder_new();
    if (b == NULL) {
        return AGENT_ERR_OOM;
    }
    JsonMut* arr = json_builder_root_arr(b);
    if (arr == NULL) {
        json_builder_free(b);
        return AGENT_ERR_OOM;
    }

    if (system_prompt != NULL && system_prompt[0] != '\0') {
        JsonMut* system = json_builder_arr_add_obj(b, arr);
        if (system == NULL) {
            json_builder_free(b);
            return AGENT_ERR_OOM;
        }
        json_builder_obj_add_str(b, system, "role", "system");
        json_builder_obj_add_str(b, system, "content", system_prompt);
    }

    for (size_t i = 0; i < msgs->len; i++) {
        const Message* m = &msgs->items[i];
        if (m->role == MSG_SYSTEM && system_prompt != NULL && system_prompt[0] != '\0') {
            continue; /* system_prompt is the authoritative merged instruction */
        }
        JsonMut* jo = json_builder_arr_add_obj(b, arr);
        if (jo == NULL) {
            json_builder_free(b);
            return AGENT_ERR_OOM;
        }
        json_builder_obj_add_str(b, jo, "role", message_role_name(m->role));

        switch (m->role) {
        case MSG_SYSTEM:
        case MSG_USER:
            if (m->content != NULL) {
                json_builder_obj_add_str(b, jo, "content", m->content);
            } else {
                json_builder_obj_add_str(b, jo, "content", "");
            }
            break;

        case MSG_ASSISTANT:
            /* An assistant message must carry content or tool_calls;
             * strict providers (Google Console Go) reject "content":
             * null with no tool_calls. An empty string is accepted
             * everywhere and is what OpenAI itself sends for
             * tool-call-only messages. */
            json_builder_obj_add_str(b, jo, "content", m->content != NULL ? m->content : "");
            if (m->tool_calls.len > 0) {
                JsonMut* calls = json_builder_obj_add_arr(b, jo, "tool_calls");
                if (calls == NULL) {
                    json_builder_free(b);
                    return AGENT_ERR_OOM;
                }
                for (size_t k = 0; k < m->tool_calls.len; k++) {
                    const ToolCall* tc = &m->tool_calls.items[k];
                    JsonMut* call = json_builder_arr_add_obj(b, calls);
                    JsonMut* fn = json_builder_obj_add_obj(b, call, "function");
                    if (call == NULL || fn == NULL) {
                        json_builder_free(b);
                        return AGENT_ERR_OOM;
                    }
                    json_builder_obj_add_str(b, call, "type", "function");
                    if (tc->id != NULL) {
                        json_builder_obj_add_str(b, call, "id", tc->id);
                    }
                    if (tc->name != NULL) {
                        json_builder_obj_add_str(b, fn, "name", tc->name);
                    }
                    json_builder_obj_add_str(b, fn, "arguments",
                                             tc->arguments != NULL ? tc->arguments : "");
                }
            }
            break;

        case MSG_TOOL:
            if (m->tool_call_id != NULL) {
                json_builder_obj_add_str(b, jo, "tool_call_id", m->tool_call_id);
            }
            json_builder_obj_add_str(b, jo, "content", m->content != NULL ? m->content : "");
            break;
        }
    }

    int err = json_builder_stringify(b, out);
    json_builder_free(b);
    return err;
}

/* ---- request body ----------------------------------------------------- */

int openai_build_request_body(ModelRequest* req, String* out) {
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
    json_builder_obj_add_bool(b, root, "stream", req->stream);
    if (req->max_tokens > 0) {
        json_builder_obj_add_int(b, root, "max_tokens", req->max_tokens);
    }
    json_builder_obj_add_real(b, root, "temperature", req->temperature);

    /* messages: serialize the conversation in the OpenAI wire format */
    if (req->messages != NULL && req->messages->len > 0) {
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
        if (msgs_root == NULL || !json_val_is_arr(msgs_root)) {
            json_doc_free(msgs_doc);
            json_builder_free(b);
            return AGENT_ERR_JSON;
        }
        if (json_builder_obj_add_val_copy(b, root, "messages", msgs_root) != AGENT_OK) {
            json_doc_free(msgs_doc);
            json_builder_free(b);
            return AGENT_ERR_OOM;
        }
        json_doc_free(msgs_doc);
    } else if (req->system_prompt != NULL && req->system_prompt[0] != '\0') {
        MessageList empty_messages = {0};
        String msgs = string_new();
        int err = serialize_messages(&empty_messages, req->system_prompt, &msgs);
        if (err != AGENT_OK) {
            string_free(&msgs);
            json_builder_free(b);
            return err;
        }
        JsonDoc* msgs_doc = json_parse(msgs.data, msgs.len);
        string_free(&msgs);
        if (msgs_doc == NULL ||
            json_builder_obj_add_val_copy(b, root, "messages", json_root(msgs_doc)) != AGENT_OK) {
            json_doc_free(msgs_doc);
            json_builder_free(b);
            return AGENT_ERR_JSON;
        }
        json_doc_free(msgs_doc);
    } else {
        JsonMut* empty = json_builder_obj_add_arr(b, root, "messages");
        if (empty == NULL) {
            json_builder_free(b);
            return AGENT_ERR_OOM;
        }
    }

    /* tools: registry schema */
    if (req->tools != NULL && tool_registry_count(req->tools) > 0) {
        String schema = string_new();
        int err = tool_registry_schema_json(req->tools, &schema);
        if (err == AGENT_OK) {
            JsonDoc* tools_doc = json_parse(schema.data, schema.len);
            if (tools_doc == NULL) {
                err = AGENT_ERR_JSON;
            } else {
                JsonVal* tools_root = json_root(tools_doc);
                if (tools_root != NULL && json_val_is_arr(tools_root)) {
                    if (json_builder_obj_add_val_copy(b, root, "tools", tools_root) != AGENT_OK) {
                        err = AGENT_ERR_OOM;
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

    int err = json_builder_stringify(b, out);
    json_builder_free(b);
    return err;
}

/* ---- lifecycle -------------------------------------------------------- */

static int openai_cancel(Model* model, uint64_t request_id) {
    if (model == NULL || model->priv == NULL || model->runtime == NULL) {
        return AGENT_ERR_MODEL;
    }
    OpenAIPriv* priv = model->priv;
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

static void openai_destroy(Model* model) {
    if (model == NULL) {
        return;
    }
    OpenAIPriv* priv = model->priv;
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

/* http completion: flush SSE, report transport/status errors, release */
static void openai_done_cb(HttpRequest* req, const HttpDoneInfo* info, void* ud) {
    (void)req;
    Transfer* t = ud;
    if (t->finished) {
        return;
    }
    t->finished = true;

    sse_parser_finish(t->sse);
    if (info->rc == CURLE_OK && info->http_status >= 200 && info->http_status < 300 &&
        !t->accum.done_emitted && !t->accum.error_emitted) {
        if (t->accum.stop_reason != MODEL_STOP_UNKNOWN || t->accum.usage_emitted) {
            /* The gateway delivered a complete stream (finish_reason or
             * at least one chunk) but omitted the [DONE] sentinel:
             * close outstanding tool calls and stop normally. */
            for (size_t i = 0; i < vector_len(&t->accum.calls); i++) {
                ToolCallState* s = vector_at(&t->accum.calls, i);
                emit_tool_end(&t->accum, s->index);
            }
            emit_done(&t->accum);
        } else {
            emit_error(&t->accum, AGENT_ERR_HTTP, "stream ended before [DONE]");
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

static int openai_request(Model* model, ModelRequest* req) {
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

    /* --- serialize the request body --- */
    String body = string_new();
    int err = openai_build_request_body(req, &body);
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

    /* --- endpoint URL (trim a trailing '/') --- */
    size_t base_len = strlen(p->base_url);
    while (base_len > 0 && p->base_url[base_len - 1] == '/') {
        base_len--;
    }
    String url = string_new();
    string_append_n(&url, p->base_url, base_len);
    string_append(&url, "/chat/completions");

    /* --- headers --- */
    String auth = string_new();
    string_append(&auth, "Authorization: Bearer ");
    string_append(&auth, p->api_key);
    const char* headers[] = {
        "Content-Type: application/json",
        "Accept: text/event-stream",
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

    OpenAIPriv* priv = model->priv;
    if (priv == NULL || vector_push(&priv->transfers, (const void*)&t) == NULL) {
        transfer_free(t);
        string_free(&auth);
        string_free(&url);
        string_free(&body);
        return AGENT_ERR_OOM;
    }
    err = http_request_start(http, url.data, body.data, body.len, headers, 3, RAW_BODY_CAP,
                             write_cb, t, openai_done_cb, t, &t->hr);
    string_free(&auth);
    string_free(&url);
    string_free(&body);
    if (err != AGENT_OK) {
        transfer_unregister(t);
        emit_error(&t->accum, AGENT_ERR_HTTP, "failed to start HTTP request");
        transfer_free(t);
        return AGENT_OK;
    }
    return AGENT_OK; /* async: completion arrives via openai_done_cb */
}

static const ModelOps openai_ops = {
    .request = openai_request,
    .cancel = openai_cancel,
    .destroy = openai_destroy,
};

Model* openai_model_new(Provider* provider, const char* name, int64_t context_window,
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
    m->ops = (ModelOps*)&openai_ops;
    m->provider = provider;
    m->context_window = context_window;
    m->max_output = max_output;
    OpenAIPriv* priv = calloc(1, sizeof(OpenAIPriv));
    if (priv == NULL) {
        free(m->name);
        free(m);
        return NULL;
    }
    priv->transfers = vector_new(sizeof(Transfer*));
    m->priv = priv;
    return m;
}
