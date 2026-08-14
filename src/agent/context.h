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

/* Rough estimate: total message characters / 4. */
int64_t context_estimate_tokens(const MessageList* msgs);

/* True when compaction is needed: estimate + output reserve exceed the
 * model's context window (or the window is unknown and the estimate is
 * very large). */
bool context_needs_compact(const Model* model, const MessageList* msgs);

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
