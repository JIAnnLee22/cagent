/*
 * agent/agent.c — agent lifecycle and cancellation.
 */

#include <stdlib.h>
#include <string.h>

#include "agent/agent.h"
#include "runtime/runtime.h"
#include "util/log.h"

static uint64_t g_next_agent_id = 1;

bool cancel_token_check(const CancelToken* t) {
    for (const CancelToken* cur = t; cur != NULL; cur = cur->parent) {
        if (cur->cancelled) {
            return true;
        }
    }
    return false;
}

void cancel_token_cancel(CancelToken* t) {
    if (t != NULL) {
        t->cancelled = true;
        t->generation++;
    }
}

Agent* agent_new(Runtime* rt, const AgentConfig* cfg) {
    Agent* a = calloc(1, sizeof(Agent));
    if (a == NULL) {
        return NULL;
    }
    a->id = g_next_agent_id++;
    a->runtime = rt;
    a->model = rt != NULL ? rt->model : NULL;
    a->tools = rt != NULL ? rt->tools : NULL;
    a->state = AGENT_READY;

    /* named model selection (DESIGN.md §7): unknown names fall back to
     * the default with a warning */
    if (cfg != NULL && cfg->model_name != NULL && rt != NULL) {
        Model* m = runtime_model_by_name(rt, cfg->model_name);
        if (m != NULL) {
            a->model = m;
        } else {
            log_warn("model '%s' not found; using the default", cfg->model_name);
        }
    }

    if (cfg != NULL) {
        if (cfg->system_prompt != NULL) {
            a->config.system_prompt = strdup(cfg->system_prompt);
            if (a->config.system_prompt == NULL) {
                agent_destroy(a);
                return NULL;
            }
        }
        if (cfg->model_name != NULL) {
            a->config.model_name = strdup(cfg->model_name);
            if (a->config.model_name == NULL) {
                agent_destroy(a);
                return NULL;
            }
        }
    }

    /* working directory: explicit config > runtime default */
    const char* cwd =
        (cfg != NULL && cfg->cwd != NULL) ? cfg->cwd : (rt != NULL ? rt->config.cwd : NULL);
    if (cwd != NULL) {
        a->config.cwd = strdup(cwd);
        if (a->config.cwd == NULL) {
            agent_destroy(a);
            return NULL;
        }
    }
    return a;
}

void agent_destroy(Agent* a) {
    if (a == NULL) {
        return;
    }
    agent_loop_state_cleanup(a);
    message_list_free(&a->messages);
    free(a->config.system_prompt);
    free(a->config.model_name);
    free(a->config.cwd);
    free(a);
}

void agent_set_event_cb(Agent* a, AgentEventCb cb, void* userdata) {
    if (a != NULL) {
        a->event_cb = cb;
        a->event_ud = userdata;
    }
}

void agent_set_approval_available(Agent* a, bool available) {
    if (a != NULL) {
        a->approval_available = available;
    }
}

void agent_set_approval_cb(Agent* a, AgentApprovalCb cb, void* userdata) {
    if (a != NULL) {
        a->approval_cb = cb;
        a->approval_ud = userdata;
        if (cb != NULL) {
            a->approval_available = true;
        }
    }
}

void agent_set_session_trusted(Agent* a, bool trusted) {
    if (a != NULL && a->parent == NULL) {
        a->session_trusted = trusted;
    }
}

bool agent_session_trusted(const Agent* a) {
    return a != NULL && a->parent == NULL && a->session_trusted;
}

Agent* agent_spawn(Agent* parent, const AgentConfig* cfg) {
    if (parent == NULL) {
        return NULL;
    }
    AgentConfig inherited = {0};
    if (cfg != NULL)
        inherited = *cfg;
    if (inherited.system_prompt == NULL)
        inherited.system_prompt = parent->config.system_prompt;
    if (inherited.cwd == NULL)
        inherited.cwd = parent->config.cwd;
    Agent* child = agent_new(parent->runtime, &inherited);
    if (child == NULL) {
        return NULL;
    }
    child->parent = parent;
    /* a named model wins; otherwise inherit the parent's */
    if (cfg == NULL || cfg->model_name == NULL) {
        child->model = parent->model;
    }
    child->cancel.parent = &parent->cancel;
    return child;
}

void agent_set_model(Agent* a, Model* m) {
    if (a != NULL) {
        a->model = m;
    }
}
