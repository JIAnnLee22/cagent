/*
 * tests/test_sse.c — SSE parser unit tests.
 *
 * The fragmentation cases are the point of this parser (DESIGN.md §10):
 * `data:` lines must survive arbitrary network chunking, down to one byte
 * per feed call.
 */

#include <stdlib.h>
#include <string.h>

#include "model/stream.h"
#include "test_common.h"
#include "util/error.h"
#include "util/vector.h"

typedef struct {
    SseEventType type;
    char* data; /* owned copy */
} RecordedEvent;

typedef struct {
    Vector events; /* RecordedEvent */
    int done_count;
} Collector;

static void collect(void* userdata, const SseEvent* ev) {
    Collector* c = userdata;
    if (ev->type == SSE_EVENT_DONE) {
        c->done_count++;
        return;
    }
    RecordedEvent rec = {.type = ev->type, .data = NULL};
    if (ev->len > 0) {
        rec.data = malloc(ev->len + 1);
        memcpy(rec.data, ev->data, ev->len);
        rec.data[ev->len] = '\0';
    } else {
        rec.data = strdup("");
    }
    vector_push(&c->events, &rec);
}

static void collector_init(Collector* c) {
    c->events = vector_new(sizeof(RecordedEvent));
    c->done_count = 0;
}

static void collector_free(Collector* c) {
    for (size_t i = 0; i < vector_len(&c->events); i++) {
        RecordedEvent* e = vector_at(&c->events, i);
        free(e->data);
    }
    vector_free(&c->events);
}

/* feed a string in chunks of `chunk_size` bytes (0 = all at once) */
static int feed_chunked(SseParser* p, const char* data, size_t chunk_size) {
    size_t len = strlen(data);
    size_t off = 0;
    while (off < len) {
        size_t n = chunk_size == 0 ? len - off : (len - off < chunk_size ? len - off : chunk_size);
        int err = sse_parser_feed(p, data + off, n);
        if (err != AGENT_OK) {
            return err;
        }
        off += n;
    }
    return AGENT_OK;
}

static const char* STREAM = "data: {\"a\":1}\n"
                            "\n"
                            "data: {\"b\":2}\n"
                            "\n"
                            "data: [DONE]\n"
                            "\n";

static int test_whole_stream(void) {
    Collector c;
    collector_init(&c);
    SseParser* p = sse_parser_new(collect, &c);

    CHECK(feed_chunked(p, STREAM, 0) == AGENT_OK);
    sse_parser_finish(p);

    CHECK(vector_len(&c.events) == 2);
    if (vector_len(&c.events) == 2) {
        RecordedEvent* e0 = vector_at(&c.events, 0);
        CHECK(e0->type == SSE_EVENT_DATA);
        CHECK(strcmp(e0->data, "{\"a\":1}") == 0);
        RecordedEvent* e1 = vector_at(&c.events, 1);
        CHECK(strcmp(e1->data, "{\"b\":2}") == 0);
    }
    CHECK(c.done_count == 1);

    sse_parser_free(p);
    collector_free(&c);
    return g_failures;
}

static int test_byte_by_byte(void) {
    Collector c;
    collector_init(&c);
    SseParser* p = sse_parser_new(collect, &c);

    /* every single byte as its own feed — the hard case */
    CHECK(feed_chunked(p, STREAM, 1) == AGENT_OK);
    sse_parser_finish(p);

    CHECK(vector_len(&c.events) == 2);
    if (vector_len(&c.events) == 2) {
        RecordedEvent* e0 = vector_at(&c.events, 0);
        CHECK(strcmp(e0->data, "{\"a\":1}") == 0);
        RecordedEvent* e1 = vector_at(&c.events, 1);
        CHECK(strcmp(e1->data, "{\"b\":2}") == 0);
    }
    CHECK(c.done_count == 1);

    sse_parser_free(p);
    collector_free(&c);
    return g_failures;
}

static int test_arbitrary_chunks(void) {
    Collector c;
    collector_init(&c);
    SseParser* p = sse_parser_new(collect, &c);

    /* 3-byte chunks split inside JSON, whitespace and [DONE] alike */
    CHECK(feed_chunked(p, STREAM, 3) == AGENT_OK);
    sse_parser_finish(p);

    CHECK(vector_len(&c.events) == 2);
    CHECK(c.done_count == 1);

    sse_parser_free(p);
    collector_free(&c);
    return g_failures;
}

static int test_crlf_and_other_fields(void) {
    const char* stream = "event: ping\n" /* ignored */
                         "id: 42\n"      /* ignored */
                         ": comment\n"   /* ignored */
                         "data: {\"x\":1}\r\n"
                         "\r\n"
                         "data: {\"y\":2}\r\n"
                         "\r\n";

    Collector c;
    collector_init(&c);
    SseParser* p = sse_parser_new(collect, &c);

    CHECK(feed_chunked(p, stream, 0) == AGENT_OK);
    sse_parser_finish(p);

    CHECK(vector_len(&c.events) == 2);
    if (vector_len(&c.events) == 2) {
        RecordedEvent* e0 = vector_at(&c.events, 0);
        CHECK(strcmp(e0->data, "{\"x\":1}") == 0);
        RecordedEvent* e1 = vector_at(&c.events, 1);
        CHECK(strcmp(e1->data, "{\"y\":2}") == 0);
    }
    CHECK(c.done_count == 0);

    sse_parser_free(p);
    collector_free(&c);
    return g_failures;
}

static int test_multi_line_data_joins(void) {
    /* per SSE spec, consecutive data: lines join into one event */
    const char* stream = "data: part1\n"
                         "data:part2\n" /* no space after colon: still valid */
                         "\n"
                         "data: [DONE]\n"
                         "\n";

    Collector c;
    collector_init(&c);
    SseParser* p = sse_parser_new(collect, &c);

    CHECK(feed_chunked(p, stream, 2) == AGENT_OK);
    sse_parser_finish(p);

    CHECK(vector_len(&c.events) == 1);
    if (vector_len(&c.events) == 1) {
        RecordedEvent* e0 = vector_at(&c.events, 0);
        CHECK(strcmp(e0->data, "part1part2") == 0);
    }
    CHECK(c.done_count == 1);

    sse_parser_free(p);
    collector_free(&c);
    return g_failures;
}

static int test_trailing_line_without_newline(void) {
    const char* stream = "data: {\"first\":1}\n"
                         "\n"
                         "data: {\"last\":1}"; /* no trailing \n */

    Collector c;
    collector_init(&c);
    SseParser* p = sse_parser_new(collect, &c);

    CHECK(feed_chunked(p, stream, 4) == AGENT_OK);
    sse_parser_finish(p); /* must flush the unterminated line */

    CHECK(vector_len(&c.events) == 2);
    if (vector_len(&c.events) == 2) {
        RecordedEvent* e1 = vector_at(&c.events, 1);
        CHECK(strcmp(e1->data, "{\"last\":1}") == 0);
    }

    sse_parser_free(p);
    collector_free(&c);
    return g_failures;
}

static int test_large_event(void) {
    /* a 256 KiB data line split into 7-byte chunks */
    size_t payload = 256 * 1024;
    char* line = malloc(payload + 32);
    CHECK(line != NULL);
    memcpy(line, "data: ", 6);
    for (size_t i = 6; i < payload + 6; i++) {
        line[i] = 'a' + (char)(i % 26);
    }
    size_t len = payload + 6;
    line[len] = '\n';
    line[len + 1] = '\n';
    line[len + 2] = '\0';

    Collector c;
    collector_init(&c);
    SseParser* p = sse_parser_new(collect, &c);

    CHECK(feed_chunked(p, line, 7) == AGENT_OK);
    sse_parser_finish(p);

    CHECK(vector_len(&c.events) == 1);
    if (vector_len(&c.events) == 1) {
        RecordedEvent* e0 = vector_at(&c.events, 0);
        CHECK(e0->data != NULL && strlen(e0->data) == payload);
        CHECK(e0->data[0] == 'a' + (char)(6 % 26));
        CHECK(e0->data[payload - 1] == 'a' + (char)((6 + payload - 1) % 26));
    }

    sse_parser_free(p);
    collector_free(&c);
    free(line);
    return g_failures;
}

static int test_oversized_line_rejected(void) {
    Collector c;
    collector_init(&c);
    SseParser* p = sse_parser_new(collect, &c);

    /* feed a line exceeding the 1 MiB cap without a newline */
    size_t big = SSE_MAX_LINE_BYTES + 64;
    char* line = malloc(big);
    CHECK(line != NULL);
    memset(line, 'x', big);

    /* feed in 64 KiB chunks; must eventually return AGENT_ERR_IO */
    int err = AGENT_OK;
    size_t off = 0;
    while (off < big && err == AGENT_OK) {
        size_t n = big - off < 65536 ? big - off : 65536;
        err = sse_parser_feed(p, line + off, n);
        off += n;
    }
    CHECK(err == AGENT_ERR_IO);

    free(line);
    sse_parser_free(p);
    collector_free(&c);
    return g_failures;
}

static int test_feed_after_done_discarded(void) {
    Collector c;
    collector_init(&c);
    SseParser* p = sse_parser_new(collect, &c);

    CHECK(feed_chunked(p, "data: [DONE]\n\n", 0) == AGENT_OK);
    CHECK(c.done_count == 1);
    /* trailing bytes after DONE are discarded, not errors */
    CHECK(sse_parser_feed(p, "data: extra\n", 12) == AGENT_OK);
    CHECK(vector_len(&c.events) == 0);
    CHECK(c.done_count == 1);

    sse_parser_free(p);
    collector_free(&c);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_whole_stream();
    g_failures += test_byte_by_byte();
    g_failures += test_arbitrary_chunks();
    g_failures += test_crlf_and_other_fields();
    g_failures += test_multi_line_data_joins();
    g_failures += test_trailing_line_without_newline();
    g_failures += test_large_event();
    g_failures += test_oversized_line_rejected();
    g_failures += test_feed_after_done_discarded();

    if (g_failures == 0) {
        printf("test_sse: all tests passed\n");
        return 0;
    }
    printf("test_sse: %d test(s) failed\n", g_failures);
    return 1;
}
