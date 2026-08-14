/*
 * bench/bench.c — micro-benchmarks for the hot paths (Phase 7).
 *
 * Measures: message-list serialization (the per-turn conversation
 * re-serialization cost) and SSE chunk parsing throughput.
 *
 * Build: nix develop -c clang -O2 -D_GNU_SOURCE -I src bench/bench.c \
 *        <sources> -lyyjson -o bench/bench
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "agent/message.h"
#include "model/stream.h"
#include "util/json.h"
#include "util/string.h"

static int64_t
now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* build a conversation of n messages with realistic sizes */
static MessageList
make_conversation(int n)
{
    MessageList msgs = {0};
    char buf[512];
    for (int i = 0; i < n; i++) {
        Message* m = message_new(i % 2 == 0 ? MSG_USER : MSG_ASSISTANT);
        snprintf(buf, sizeof(buf),
                 "message number %d with some realistic content length "
                 "padding 0123456789abcdefghijklmnopqrstuvwxyz", i);
        message_set_content(m, buf);
        message_list_append(&msgs, m);
    }
    return msgs;
}

/* forward decl: loop.c's serializer is static; replicate the hot loop
 * shape with the public json wrapper to measure the wrapper cost */
static int
serialize_loop(const MessageList* msgs, String* out)
{
    JsonBuilder* b = json_builder_new();
    if (b == NULL) {
        return -1;
    }
    JsonMut* arr = json_builder_root_arr(b);
    if (arr == NULL) {
        json_builder_free(b);
        return -1;
    }
    for (size_t i = 0; i < msgs->len; i++) {
        const Message* m = &msgs->items[i];
        JsonMut* jo = json_builder_arr_add_obj(b, arr);
        json_builder_obj_add_str(b, jo, "role", message_role_name(m->role));
        if (m->content != NULL) {
            json_builder_obj_add_str(b, jo, "content", m->content);
        }
    }
    int err = json_builder_stringify(b, out);
    json_builder_free(b);
    return err;
}

static void
bench_serialize(int turns, int msgs_per_turn)
{
    MessageList msgs = make_conversation(msgs_per_turn);
    String out = string_new();

    int64_t t0 = now_us();
    for (int i = 0; i < turns; i++) {
        string_clear(&out);
        serialize_loop(&msgs, &out);
    }
    int64_t t1 = now_us();

    printf("serialize: %d turns x %d msgs (%.1f kB each): %.1f us/turn "
           "(%.2f MB/s)\n",
           turns, msgs_per_turn, (double)out.len / 1024.0,
           (double)(t1 - t0) / turns,
           (double)out.len * turns / 1024.0 / 1024.0 * 1e6 / (double)(t1 - t0));

    string_free(&out);
    message_list_free(&msgs);
}

static void
sse_noop(void* ud, const SseEvent* ev)
{
    (void)ud;
    (void)ev;
}

static void
bench_sse(int chunks)
{
    /* a realistic streaming chunk */
    const char* chunk =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hello world \"}}]}\n\n";
    size_t chunk_len = strlen(chunk);

    int64_t t0 = now_us();
    for (int i = 0; i < chunks; i++) {
        SseParser* p = sse_parser_new(sse_noop, NULL);
        sse_parser_feed(p, chunk, chunk_len);
        sse_parser_free(p);
    }
    int64_t t1 = now_us();

    printf("sse: %d chunks: %.1f ns/chunk (parser create+feed+free)\n", chunks,
           (double)(t1 - t0) * 1000.0 / chunks);
}

int
main(void)
{
    bench_serialize(2000, 20);   /* medium conversation, many turns */
    bench_serialize(500, 200);   /* long conversation */
    bench_sse(100000);
    return 0;
}
