/*
 * runtime/scheduler.h — agent scheduler (DESIGN.md §14).
 *
 * Runs at most max_concurrent agents at once. Agents waiting on async
 * work (model requests in flight) do NOT occupy a slot beyond their own
 * lightweight state; they are resumed by scheduler_pump() whenever the
 * runtime is driven.
 *
 * Ownership:
 *   - Agents are BORROWED; the scheduler never frees them.
 *   - input strings are copied by scheduler_add() and owned by the
 *     scheduler until the agent starts.
 */

#ifndef CAGENT_RUNTIME_SCHEDULER_H
#define CAGENT_RUNTIME_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>

#include "agent/agent.h"
#include "util/error.h"

typedef struct Scheduler {
    Agent** agents; /* owned array; elements borrowed */
    char** inputs;  /* owned; parallel pending inputs (NULL once started) */
    size_t len;
    size_t cap;
    size_t max_concurrent;
    size_t running;
} Scheduler;

Scheduler* scheduler_new(size_t max_concurrent);
void scheduler_free(Scheduler* s);

int scheduler_add(Scheduler* s, Agent* a, const char* input);

/* Start queued agents (up to the concurrency limit) and advance every
 * running agent one step. */
void scheduler_pump(Scheduler* s);

/* True when every agent reached a terminal state. */
bool scheduler_all_done(const Scheduler* s);

#endif /* CAGENT_RUNTIME_SCHEDULER_H */
