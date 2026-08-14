/*
 * model/stream.h — SSE (Server-Sent Events) parser for model streams.
 *
 * Handles arbitrary network fragmentation: feed() may receive any byte
 * slice; lines and events may be split across calls in any way. Only the
 * `data:` field is collected (per SSE spec, multiple data: lines inside
 * one event are joined with '\n'); other fields (event:, id:, retry:,
 * comments) are ignored — the OpenAI-compatible stream only uses data.
 *
 * A `data: [DONE]` line produces SSE_EVENT_DONE. An empty line flushes the
 * pending event as SSE_EVENT_DATA.
 *
 * Ownership:
 *   - SseEvent.data is BORROWED: it points into parser-internal storage
 *     and is only valid during the callback invocation.
 *   - The parser owns all internal buffers; freed by sse_parser_free().
 *   - The parser does not own userdata.
 *
 * Error policy: a single line longer than SSE_MAX_LINE_BYTES makes feed()
 * return AGENT_ERR_IO (protocol anomaly; do not continue feeding).
 * After DONE, further input is discarded (feed returns AGENT_OK) — the
 * trailing blank line after [DONE] is a normal part of the stream.
 */

#ifndef CAGENT_MODEL_STREAM_H
#define CAGENT_MODEL_STREAM_H

#include <stddef.h>

#include "util/error.h"

#define SSE_MAX_LINE_BYTES (1024 * 1024)

typedef enum {
    SSE_EVENT_DATA, /* one complete data payload */
    SSE_EVENT_DONE  /* data: [DONE] */
} SseEventType;

typedef struct {
    SseEventType type;
    const char* data; /* borrowed; valid only during the callback */
    size_t len;
} SseEvent;

typedef struct SseParser SseParser;

/* The callback must not feed the parser again (re-entrancy forbidden). */
typedef void (*SseCallback)(void* userdata, const SseEvent* event);

SseParser* sse_parser_new(SseCallback cb, void* userdata);
void sse_parser_free(SseParser* p);

/* Feed an arbitrary chunk of the byte stream. */
int sse_parser_feed(SseParser* p, const char* data, size_t len);

/* Flush a trailing line without '\n' and any pending event. Call once at
 * end of stream. Safe to call multiple times. */
void sse_parser_finish(SseParser* p);

#endif /* CAGENT_MODEL_STREAM_H */
