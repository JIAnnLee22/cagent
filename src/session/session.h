/*
 * session/session.h — session persistence (DESIGN.md §26, Phase 3).
 *
 * A session is an append-only JSONL file under
 * ~/.local/state/cagent/sessions/<id>.jsonl:
 *
 *   {"type":"meta",   "id":..., "cwd":..., "model":..., "provider":...,
 *    "created_at":...}
 *   {"type":"message","role":..., "content":..., "tool_calls":[...],
 *    "tool_call_id":..., "reasoning":..., "is_error":..., "usage":{...}}
 *   {"type":"compaction", "start":..., "count":..., "summary":...}
 *   {"type":"stats",  "requests":..., "tool_calls":...,
 *    "model_time_ms":..., "tokens_in":..., "tokens_out":..., "cached":...}
 *
 * Each line is one JSON object (one message, compaction transaction or stats snapshot); the
 * file grows monotonically. No database in Phase 1.
 *
 * Ownership:
 *   - Session owns its id/path/cwd/model/provider strings.
 *   - session_append_message() copies the message into the file; the
 *     Message stays owned by the caller.
 *   - session_load_messages() appends freshly allocated Messages into
 *     *out (caller frees via message_list_free()).
 */

#ifndef CAGENT_SESSION_SESSION_H
#define CAGENT_SESSION_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "agent/message.h"
#include "model/model.h" /* Usage */
#include "util/error.h"

typedef struct Session {
    char* id;         /* owned */
    char* path;       /* owned; full path to the JSONL file */
    char* cwd;        /* owned */
    char* model_name; /* owned */
    char* provider;   /* owned; base url */
    int64_t created_at;
    int64_t updated_at;
    /* running totals (persisted in the stats line) */
    Usage usage;
    uint64_t request_count;
    uint64_t tool_call_count;
    int64_t model_time_ms;
    char* memory;        /* owned bounded structured memory records */
} Session;

/* Create a new session in dir (mkdir -p'ed). */
Session* session_create(const char* dir, const char* cwd, const char* model_name,
                        const char* provider);

/* Open an existing session by id; NULL when missing. Meta/stats lines are
 * read into the Session struct; messages are NOT loaded (see
 * session_load_messages). */
Session* session_open(const char* dir, const char* id);

void session_free(Session* s);

/* Append one message as a JSONL line. */
int session_append_message(Session* s, const Message* m);

/* Append an append-only compaction transaction.  On load, the summary is
 * inserted at start and count old messages are removed from the loaded view. */
int session_append_compaction(Session* s, size_t start, size_t count, const char* summary);

/* Append a stats snapshot line (totals so far). */
int session_append_stats(Session* s);

/* Append a bounded structured memory record for cross-session recovery. */
int session_append_memory(Session* s, const char* kind, const char* content);
const char* session_memory(const Session* s);

/* Load all persisted messages into out (appended). */
int session_load_messages(Session* s, MessageList* out);

/* Load the totals from the stats line (already done by session_open for
 * the totals fields). */

/* Default sessions directory under the XDG state root. Returns a static
 * buffer; NULL when $HOME is unset. */
const char* session_default_dir(void);

#endif /* CAGENT_SESSION_SESSION_H */
