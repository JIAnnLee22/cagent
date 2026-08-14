/*
 * runtime/scheduler.c — agent scheduler.
 */

#include <stdlib.h>
#include <string.h>

#include "runtime/scheduler.h"

static bool agent_terminal(const Agent* a) {
    return a->state == AGENT_DONE || a->state == AGENT_ERROR || a->state == AGENT_CANCELLED;
}

Scheduler* scheduler_new(size_t max_concurrent) {
    Scheduler* s = calloc(1, sizeof(Scheduler));
    if (s == NULL) {
        return NULL;
    }
    s->max_concurrent = max_concurrent > 0 ? max_concurrent : 1;
    return s;
}

void scheduler_free(Scheduler* s) {
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < s->len; i++) {
        free(s->inputs[i]);
    }
    free((void*)s->agents);
    free((void*)s->inputs);
    free(s);
}

int scheduler_add(Scheduler* s, Agent* a, const char* input) {
    if (s == NULL || a == NULL || input == NULL) {
        return AGENT_ERR_OOM;
    }
    if (s->len == s->cap) {
        size_t new_cap = s->cap == 0 ? 8 : s->cap * 2;
        /* one realloc at a time: a failed second must not leave the first
         * dangling (same pattern as tool_registry) */
        Agent** agents = (Agent**)realloc((void*)s->agents, new_cap * sizeof(Agent*));
        if (agents == NULL) {
            return AGENT_ERR_OOM;
        }
        s->agents = agents;
        char** inputs = (char**)realloc((void*)s->inputs, new_cap * sizeof(char*));
        if (inputs == NULL) {
            return AGENT_ERR_OOM; /* agents grown, inputs kept: consistent */
        }
        s->inputs = inputs;
        s->cap = new_cap;
    }
    s->agents[s->len] = a;
    s->inputs[s->len] = strdup(input);
    if (s->inputs[s->len] == NULL) {
        return AGENT_ERR_OOM;
    }
    s->len++;
    return AGENT_OK;
}

void scheduler_pump(Scheduler* s) {
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < s->len; i++) {
        Agent* a = s->agents[i];
        if (agent_terminal(a)) {
            continue;
        }
        if (a->state == AGENT_READY) {
            /* not started yet: admit up to the concurrency limit */
            if (s->running >= s->max_concurrent) {
                continue;
            }
            if (s->inputs[i] != NULL) {
                int rc = agent_start(a, s->inputs[i]);
                free(s->inputs[i]);
                s->inputs[i] = NULL;
                if (rc == AGENT_OK) {
                    s->running++;
                } else {
                    a->state = AGENT_ERROR; /* start failure is terminal */
                }
            }
        }
        /* advance one step */
        AgentStepResult r = agent_resume(a);
        if (r == AGENT_STEP_DONE || r == AGENT_STEP_ERROR || r == AGENT_STEP_CANCELLED) {
            if (s->running > 0) {
                s->running--;
            }
        }
    }
}

bool scheduler_all_done(const Scheduler* s) {
    if (s == NULL) {
        return true;
    }
    for (size_t i = 0; i < s->len; i++) {
        if (!agent_terminal(s->agents[i])) {
            return false;
        }
    }
    return true;
}
