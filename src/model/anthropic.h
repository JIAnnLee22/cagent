/*
 * model/anthropic.h — Anthropic Messages API provider (DESIGN.md §7).
 *
 * Wire format: POST {base}/v1/messages with x-api-key +
 * anthropic-version headers; SSE stream of message/content_block_*
 * events. Tool calls arrive as tool_use content blocks with
 * input_json_delta fragments; tool results are user messages with
 * tool_result blocks. Covers OpenCode Go's Anthropic-protocol models
 * (MiniMax/Qwen entries).
 */

#ifndef CAGENT_MODEL_ANTHROPIC_H
#define CAGENT_MODEL_ANTHROPIC_H

#include <stdint.h>

#include "model/model.h"
#include "util/string.h"

Model* anthropic_model_new(Provider* provider, const char* name, int64_t context_window,
                           int64_t max_output);

/* Pure function: serialize the conversation into the Anthropic Messages
 * request body (streaming). Exposed for tests. */
int anthropic_build_request_body(ModelRequest* request, String* out);

#endif /* CAGENT_MODEL_ANTHROPIC_H */
