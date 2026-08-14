/*
 * tests/mock_model.c — scripted mock model.
 */

#include <stdlib.h>
#include <string.h>

#include "mock_model.h"

typedef struct {
    const MockStep* steps;
    size_t n_steps;
    size_t round;
    bool async;           /* request() parks until mock_model_pump() */
    ModelRequest pending; /* parked request (borrowed pointers) */
    bool has_pending;
    size_t compaction_requests;
    size_t regular_requests;
    char* last_system_prompt; /* strdup of the most recent regular request */
} MockPriv;

static void emit(MockPriv* priv, ModelEventCallback cb, void* ud, uint64_t req_id,
                 const ModelEvent* ev) {
    (void)priv;
    ModelEvent e = *ev;
    e.request_id = req_id;
    cb(ud, &e);
}

static void emit_tool_calls(MockPriv* priv, ModelEventCallback cb, void* ud, uint64_t req_id,
                            const MockToolCall* calls, size_t n_calls,
                            ModelStopReason stop_reason) {
    ModelEvent ev = {0};
    for (size_t i = 0; i < n_calls; i++) {
        ev.type = MODEL_EVENT_TOOL_CALL_START;
        ev.u.tool_start.index = i;
        ev.u.tool_start.id = calls[i].id;
        ev.u.tool_start.name = calls[i].name;
        emit(priv, cb, ud, req_id, &ev);
        if (calls[i].args != NULL && calls[i].args[0] != '\0') {
            ev.type = MODEL_EVENT_TOOL_CALL_DELTA;
            ev.u.tool_delta.index = i;
            ev.u.tool_delta.delta = calls[i].args;
            ev.u.tool_delta.len = strlen(calls[i].args);
            emit(priv, cb, ud, req_id, &ev);
        }
        ev.type = MODEL_EVENT_TOOL_CALL_END;
        ev.u.tool_end.index = i;
        emit(priv, cb, ud, req_id, &ev);
    }
    ev.type = MODEL_EVENT_DONE;
    ev.u.done.reason = stop_reason;
    emit(priv, cb, ud, req_id, &ev);
}

static int mock_request(Model* model, ModelRequest* req) {
    MockPriv* priv = model->priv;
    if (req->is_compaction) {
        priv->compaction_requests++;
    } else {
        priv->regular_requests++;
        free(priv->last_system_prompt);
        priv->last_system_prompt = req->system_prompt != NULL ? strdup(req->system_prompt) : NULL;
    }
    if (priv->async) {
        priv->pending = *req; /* parked; completed by mock_model_pump() */
        priv->has_pending = true;
        return AGENT_OK;
    }
    if (priv->round >= priv->n_steps) {
        ModelEvent ev = {0};
        ev.type = MODEL_EVENT_ERROR;
        ev.u.error.code = 500;
        ev.u.error.message = "mock script exhausted";
        emit(priv, req->event_cb, req->event_userdata, req->id, &ev);
        return AGENT_OK;
    }

    const MockStep* s = &priv->steps[priv->round++];
    ModelEvent ev = {0};

    switch (s->type) {
    case MOCK_TEXT:
        ev.type = MODEL_EVENT_TEXT_DELTA;
        ev.u.text.data = s->text != NULL ? s->text : "";
        ev.u.text.len = strlen(ev.u.text.data);
        emit(priv, req->event_cb, req->event_userdata, req->id, &ev);
        ev.type = MODEL_EVENT_DONE;
        ev.u.done.reason = s->stop_reason;
        emit(priv, req->event_cb, req->event_userdata, req->id, &ev);
        break;

    case MOCK_TEXT_CHUNKS:
        for (size_t i = 0; i < s->n_text_chunks; i++) {
            ev.type = MODEL_EVENT_TEXT_DELTA;
            ev.u.text.data = s->text_chunks[i] != NULL ? s->text_chunks[i] : "";
            ev.u.text.len = strlen(ev.u.text.data);
            emit(priv, req->event_cb, req->event_userdata, req->id, &ev);
        }
        ev.type = MODEL_EVENT_DONE;
        ev.u.done.reason = s->stop_reason;
        emit(priv, req->event_cb, req->event_userdata, req->id, &ev);
        break;

    case MOCK_TOOL_CALL: {
        MockToolCall call = {.id = s->tool_id, .name = s->tool_name, .args = s->tool_args};
        emit_tool_calls(priv, req->event_cb, req->event_userdata, req->id, &call, 1,
                        s->stop_reason);
        break;
    }
    case MOCK_TOOL_CALLS:
        emit_tool_calls(priv, req->event_cb, req->event_userdata, req->id, s->tool_calls,
                        s->n_tool_calls, s->stop_reason);
        break;

    case MOCK_ERROR:
        ev.type = MODEL_EVENT_ERROR;
        ev.u.error.code = s->error_code;
        ev.u.error.message = s->error_msg != NULL ? s->error_msg : "mock error";
        emit(priv, req->event_cb, req->event_userdata, req->id, &ev);
        break;
    }
    return AGENT_OK;
}

static int mock_cancel(Model* model, uint64_t request_id) {
    MockPriv* priv = model != NULL ? model->priv : NULL;
    if (priv != NULL && priv->has_pending && priv->pending.id == request_id) {
        ModelRequest pending = priv->pending;
        priv->has_pending = false;
        memset(&priv->pending, 0, sizeof(priv->pending));
        ModelEvent ev = {0};
        ev.type = MODEL_EVENT_ERROR;
        ev.u.error.code = AGENT_ERR_CANCELLED;
        ev.u.error.message = "mock request cancelled";
        emit(priv, pending.event_cb, pending.event_userdata, pending.id, &ev);
    }
    return AGENT_OK;
}

static void mock_destroy(Model* model) {
    if (model == NULL) {
        return;
    }
    free(((MockPriv*)model->priv)->last_system_prompt);
    free(model->priv);
    free(model->name);
    free(model);
}

static const ModelOps mock_ops = {
    .request = mock_request,
    .cancel = mock_cancel,
    .destroy = mock_destroy,
};

static Model* mock_model_alloc(const char* name, const MockStep* steps, size_t n_steps,
                               bool async) {
    Model* m = calloc(1, sizeof(Model));
    if (m == NULL) {
        return NULL;
    }
    MockPriv* priv = calloc(1, sizeof(MockPriv));
    if (priv == NULL) {
        free(m);
        return NULL;
    }
    m->name = strdup(name != NULL ? name : "mock");
    if (m->name == NULL) {
        free(priv);
        free(m);
        return NULL;
    }
    priv->steps = steps;
    priv->n_steps = n_steps;
    priv->async = async;
    m->ops = (ModelOps*)&mock_ops;
    m->priv = priv;
    m->context_window = 128000;
    m->max_output = 8192;
    return m;
}

Model* mock_model_new(const char* name, const MockStep* steps, size_t n_steps) {
    return mock_model_alloc(name, steps, n_steps, false);
}

Model* mock_model_new_async(const char* name, const MockStep* steps, size_t n_steps) {
    return mock_model_alloc(name, steps, n_steps, true);
}

/* Complete the parked request: emit the next step's events to the parked
 * callback (mirrors the synchronous request path). */
void mock_model_pump(Model* m) {
    MockPriv* priv = m->priv;
    if (priv == NULL || !priv->has_pending) {
        return;
    }
    priv->has_pending = false;
    ModelRequest req = priv->pending;

    if (priv->round >= priv->n_steps) {
        ModelEvent ev = {0};
        ev.type = MODEL_EVENT_ERROR;
        ev.u.error.code = 500;
        ev.u.error.message = "mock script exhausted";
        emit(priv, req.event_cb, req.event_userdata, req.id, &ev);
        return;
    }

    const MockStep* st = &priv->steps[priv->round++];
    ModelEvent ev = {0};
    switch (st->type) {
    case MOCK_TEXT:
        ev.type = MODEL_EVENT_TEXT_DELTA;
        ev.u.text.data = st->text != NULL ? st->text : "";
        ev.u.text.len = strlen(ev.u.text.data);
        emit(priv, req.event_cb, req.event_userdata, req.id, &ev);
        ev.type = MODEL_EVENT_DONE;
        ev.u.done.reason = st->stop_reason;
        emit(priv, req.event_cb, req.event_userdata, req.id, &ev);
        break;
    case MOCK_TEXT_CHUNKS:
        for (size_t i = 0; i < st->n_text_chunks; i++) {
            ev.type = MODEL_EVENT_TEXT_DELTA;
            ev.u.text.data = st->text_chunks[i] != NULL ? st->text_chunks[i] : "";
            ev.u.text.len = strlen(ev.u.text.data);
            emit(priv, req.event_cb, req.event_userdata, req.id, &ev);
        }
        ev.type = MODEL_EVENT_DONE;
        ev.u.done.reason = st->stop_reason;
        emit(priv, req.event_cb, req.event_userdata, req.id, &ev);
        break;
    case MOCK_TOOL_CALL: {
        MockToolCall call = {.id = st->tool_id, .name = st->tool_name, .args = st->tool_args};
        emit_tool_calls(priv, req.event_cb, req.event_userdata, req.id, &call, 1, st->stop_reason);
        break;
    }
    case MOCK_TOOL_CALLS:
        emit_tool_calls(priv, req.event_cb, req.event_userdata, req.id, st->tool_calls,
                        st->n_tool_calls, st->stop_reason);
        break;
    case MOCK_ERROR:
        ev.type = MODEL_EVENT_ERROR;
        ev.u.error.code = st->error_code;
        ev.u.error.message = st->error_msg != NULL ? st->error_msg : "mock error";
        emit(priv, req.event_cb, req.event_userdata, req.id, &ev);
        break;
    }
}

size_t mock_model_compaction_requests(Model* m) {
    MockPriv* priv = m != NULL ? m->priv : NULL;
    return priv != NULL ? priv->compaction_requests : 0;
}

size_t mock_model_regular_requests(Model* m) {
    MockPriv* priv = m != NULL ? m->priv : NULL;
    return priv != NULL ? priv->regular_requests : 0;
}

/* System prompt of the most recent regular (non-compaction) request. */
const char* mock_model_last_system_prompt(Model* m) {
    MockPriv* priv = m != NULL ? m->priv : NULL;
    return priv != NULL ? priv->last_system_prompt : NULL;
}
