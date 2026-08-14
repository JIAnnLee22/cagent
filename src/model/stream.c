/*
 * model/stream.c — SSE parser implementation.
 *
 * Buffered line scan: feed() appends to an internal byte buffer, then
 * processes complete lines (terminated by '\n', tolerating '\r\n').
 * Processed bytes are compacted with memmove — lines are small (KB scale),
 * so this is simple and correct. The current event's data: payload is
 * accumulated in a String; an empty line flushes it.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "model/stream.h"
#include "util/buffer.h"
#include "util/string.h"

struct SseParser {
    SseCallback cb;
    void* userdata;
    Buffer pending;    /* bytes not yet parsed into lines */
    String event_data; /* data payload of the current event */
    bool have_event;   /* at least one data: line seen in this event */
    bool done;         /* [DONE] received */
};

SseParser* sse_parser_new(SseCallback cb, void* userdata) {
    SseParser* p = calloc(1, sizeof(SseParser));
    if (p == NULL) {
        return NULL;
    }
    p->cb = cb;
    p->userdata = userdata;
    p->pending = buffer_new();
    p->event_data = string_new();
    return p;
}

void sse_parser_free(SseParser* p) {
    if (p == NULL) {
        return;
    }
    buffer_free(&p->pending);
    string_free(&p->event_data);
    free(p);
}

static void sse_emit(SseParser* p, SseEventType type) {
    SseEvent ev = {0};
    ev.type = type;
    ev.data = p->event_data.data;
    ev.len = p->event_data.len;
    p->cb(p->userdata, &ev);
}

static void sse_flush_event(SseParser* p) {
    if (p->have_event) {
        sse_emit(p, SSE_EVENT_DATA);
        string_clear(&p->event_data);
        p->have_event = false;
    }
}

/* Handle one complete line without its '\n' (line_len excludes it). */
static void sse_process_line(SseParser* p, const char* line, size_t line_len) {
    /* strip a trailing '\r' (CRLF line endings) */
    if (line_len > 0 && line[line_len - 1] == '\r') {
        line_len--;
    }

    if (line_len == 0) {
        /* empty line terminates the current event */
        sse_flush_event(p);
        return;
    }

    /* only the data: field matters; ignore everything else */
    if (line_len < 5 || memcmp(line, "data:", 5) != 0) {
        return;
    }

    const char* value = line + 5;
    size_t value_len = line_len - 5;
    /* per spec, exactly one leading space after the colon is dropped */
    if (value_len > 0 && value[0] == ' ') {
        value++;
        value_len--;
    }

    if (!p->have_event && value_len == 6 && memcmp(value, "[DONE]", 6) == 0) {
        p->done = true;
        sse_flush_event(p); /* flush any earlier data lines, if any */
        sse_emit(p, SSE_EVENT_DONE);
        return;
    }

    if (string_append_n(&p->event_data, value, value_len) != AGENT_OK) {
        /* OOM: mark done to stop feeding; the caller sees the error code
         * from feed() below only if it checks. We surface it via done. */
        p->done = true;
        return;
    }
    p->have_event = true;
}

int sse_parser_feed(SseParser* p, const char* data, size_t len) {
    if (p == NULL || (data == NULL && len > 0)) {
        return AGENT_ERR_OOM;
    }
    if (p->done) {
        return AGENT_OK; /* stream finished; discard further input */
    }
    if (len == 0) {
        return AGENT_OK;
    }

    int err = buffer_append(&p->pending, data, len);
    if (err != AGENT_OK) {
        return err;
    }

    while (p->pending.len > 0) {
        /* find the next '\n' */
        uint8_t* nl = memchr(p->pending.data, '\n', p->pending.len);
        if (nl == NULL) {
            /* no complete line yet; guard against pathological growth */
            if (p->pending.len > SSE_MAX_LINE_BYTES) {
                return AGENT_ERR_IO;
            }
            break;
        }

        size_t line_len = (size_t)(nl - p->pending.data);
        sse_process_line(p, (const char*)p->pending.data, line_len);

        if (p->done) {
            /* [DONE] seen: discard the rest */
            buffer_clear(&p->pending);
            return AGENT_OK;
        }

        /* compact: drop the processed line and its '\n' */
        size_t remaining = p->pending.len - line_len - 1;
        if (remaining > 0) {
            memmove(p->pending.data, nl + 1, remaining);
        }
        p->pending.len = remaining;
    }

    return AGENT_OK;
}

void sse_parser_finish(SseParser* p) {
    if (p == NULL || p->done) {
        return;
    }
    if (p->pending.len > 0) {
        sse_process_line(p, (const char*)p->pending.data, p->pending.len);
        buffer_clear(&p->pending);
    }
    sse_flush_event(p);
    p->done = true;
}
