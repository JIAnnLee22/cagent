/*
 * agent/context.h — context management (DESIGN.md §27/§28).
 *
 * Compaction is prepared without mutating the live conversation.  The
 * caller may send request_messages to a model and apply the returned text
 * only after that request finishes.  This is important for the async agent
 * loop: a failed/cancelled summary must not destroy the original history.
 */

#ifndef CAGENT_AGENT_CONTEXT_H
#define CAGENT_AGENT_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "agent/message.h"
#include "model/model.h"
#include "util/error.h"

/* Rough local estimate of message history tokens. ASCII is treated as four
 * bytes/token and each non-ASCII UTF-8 code point as one token. This is a
 * display/compaction heuristic, not a provider tokenizer. */
int64_t context_estimate_tokens(const MessageList* msgs);

/* Estimate the content sent in a normal request: system prompt, message
 * history, and the enabled tool schema. */
int64_t context_estimate_request_tokens(const char* system_prompt,
                                        const MessageList* msgs,
                                        const ToolRegistry* tools);

/* True when message history alone needs compaction (legacy API). */
bool context_needs_compact(const Model* model, const MessageList* msgs);

/* Request-aware variant used by the agent loop.  Including the fixed system
 * prompt and tool schema prevents a large hidden prompt from bypassing the
 * compaction threshold. */
bool context_needs_compact_request(const Model* model, const char* system_prompt,
                                   const MessageList* msgs, const ToolRegistry* tools);

/* An immutable description of one compaction operation.  request_messages
 * is owned by this object and remains valid until it is freed. */
typedef struct {
    size_t start;
    size_t count;
    MessageList request_messages;
} ContextCompactionRequest;

/* Build a bounded transcript prompt for the omitted middle.  The source is
 * not modified.  count == 0 means there is nothing meaningful to compact. */
int context_compaction_prepare(const MessageList* msgs, size_t keep_recent,
                               ContextCompactionRequest* out);
void context_compaction_request_free(ContextCompactionRequest* request);

/* Apply an LLM-produced summary atomically: insertion happens before the
 * old range is removed, so an allocation failure leaves msgs unchanged. */
int context_compaction_apply(MessageList* msgs, const ContextCompactionRequest* request,
                             const char* summary);

/* Apply the deterministic fallback summary for a prepared request. */
int context_compaction_apply_fallback(MessageList* msgs,
                                      const ContextCompactionRequest* request);

/* One-shot deterministic compaction retained as the fallback path. */
int context_compact(MessageList* msgs, size_t keep_recent);

#endif /* CAGENT_AGENT_CONTEXT_H */
