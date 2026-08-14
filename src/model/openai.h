/*
 * model/openai.h — OpenAI-compatible provider (Phase 1: synchronous).
 *
 * Covers OpenAI, OpenRouter, local OpenAI-compatible servers, vLLM, etc.
 * The Agent core never sees provider-specific JSON: everything is
 * normalized to ModelEvent (DESIGN.md §10).
 *
 * Ownership:
 *   - openai_model_new() returns a Model owned by the caller; destroy it
 *     with model->ops->destroy(model) (openai_destroy).
 *   - openai_build_request_body() appends the request JSON to *out
 *     (caller-owned String).
 */

#ifndef CAGENT_MODEL_OPENAI_H
#define CAGENT_MODEL_OPENAI_H

#include <stdbool.h>
#include <stdint.h>

#include "model/model.h"
#include "util/string.h"

/* Create a Model bound to this provider's /chat/completions endpoint. */
Model* openai_model_new(Provider* provider, const char* name, int64_t context_window,
                        int64_t max_output);

/* Pure function: serialize the chat-completions request body (streaming).
 * Exposed for tests; called internally by the request path. */
int openai_build_request_body(ModelRequest* request, String* out);

#endif /* CAGENT_MODEL_OPENAI_H */
