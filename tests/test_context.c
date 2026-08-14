/*
 * tests/test_context.c — context estimation/compaction tests.
 */

#include <stdlib.h>
#include <string.h>

#include "agent/context.h"
#include "agent/message.h"
#include "test_common.h"
#include "util/error.h"

static Message* mk(MessageRole role, const char* content) {
    Message* m = message_new(role);
    message_set_content(m, content);
    return m;
}

static Message* mk_tool_call_assistant(void) {
    Message* m = message_new(MSG_ASSISTANT);
    const char* ids[] = {"call_1", "call_2"};
    for (size_t i = 0; i < 2; i++) {
        ToolCall* tc = calloc(1, sizeof(ToolCall));
        tc->id = strdup(ids[i]);
        tc->name = strdup("read");
        tc->arguments = strdup("{}");
        tool_call_list_append(&m->tool_calls, tc);
    }
    return m;
}

static Message* mk_tool_result(const char* id, const char* content) {
    Message* m = mk(MSG_TOOL, content);
    m->tool_call_id = strdup(id);
    return m;
}

static int test_estimate(void) {
    MessageList msgs = {0};
    message_list_append(&msgs, mk(MSG_USER, "0123456789abcdef")); /* 16 chars */
    CHECK(context_estimate_tokens(&msgs) == 4);                   /* 16/4 */

    message_list_append(&msgs, mk(MSG_ASSISTANT, "hello")); /* 5 chars */
    CHECK(context_estimate_tokens(&msgs) == 5);

    /* NULL content tolerated */
    message_list_append(&msgs, mk(MSG_TOOL, NULL));
    CHECK(context_estimate_tokens(&msgs) == 5);

    message_list_free(&msgs);
    return g_failures;
}

static int test_needs_compact(void) {
    Model model = {0};
    model.context_window = 100;

    MessageList small = {0};
    message_list_append(&small, mk(MSG_USER, "tiny"));
    CHECK(!context_needs_compact(&model, &small));

    /* 200 chars -> 50 tokens + 8192 reserve > 100 window */
    char big[201];
    memset(big, 'x', 200);
    big[200] = '\0';
    message_list_append(&small, mk(MSG_USER, big));
    CHECK(context_needs_compact(&model, &small));

    /* unknown window: extreme size triggers */
    Model unknown = {0};
    CHECK(!context_needs_compact(&unknown, &small));
    MessageList huge = {0};
    char* huge_buf = malloc(1024 * 1024 + 1);
    memset(huge_buf, 'y', 1024 * 1024);
    huge_buf[1024 * 1024] = '\0';
    message_list_append(&huge, mk(MSG_USER, huge_buf));
    CHECK(context_needs_compact(&unknown, &huge));
    free(huge_buf);

    message_list_free(&small);
    message_list_free(&huge);
    return g_failures;
}

static int test_compact_keeps_system_and_tail(void) {
    MessageList msgs = {0};
    message_list_append(&msgs, mk(MSG_SYSTEM, "you are helpful"));
    message_list_append(&msgs, mk(MSG_USER, "q1"));
    message_list_append(&msgs, mk(MSG_ASSISTANT, "a1"));
    message_list_append(&msgs, mk(MSG_USER, "q2"));
    message_list_append(&msgs, mk(MSG_TOOL, "tool result"));
    message_list_append(&msgs, mk(MSG_USER, "q3"));
    message_list_append(&msgs, mk(MSG_ASSISTANT, "final"));

    size_t before = msgs.len;
    CHECK(context_compact(&msgs, 4) == AGENT_OK);

    /* system kept, tail kept, middle replaced by one summary */
    CHECK(msgs.len < before);
    CHECK(msgs.items[0].role == MSG_SYSTEM);
    CHECK(strcmp(msgs.items[0].content, "you are helpful") == 0);

    /* the summary message exists somewhere */
    int found_summary = 0;
    for (size_t i = 0; i < msgs.len; i++) {
        if (msgs.items[i].content != NULL &&
            strstr(msgs.items[i].content, "context compaction") != NULL) {
            found_summary = 1;
        }
    }
    CHECK(found_summary);

    /* the tail (last messages) is intact */
    Message* last = message_list_last(&msgs);
    CHECK(last != NULL && strcmp(last->content, "final") == 0);

    message_list_free(&msgs);
    return g_failures;
}

static int test_compaction_prepare_and_apply(void) {
    MessageList msgs = {0};
    message_list_append(&msgs, mk(MSG_USER, "old goal"));
    message_list_append(&msgs, mk(MSG_ASSISTANT, "old answer"));
    message_list_append(&msgs, mk(MSG_TOOL, "old finding"));
    message_list_append(&msgs, mk(MSG_USER, "old extra"));
    message_list_append(&msgs, mk(MSG_USER, "keep this"));
    message_list_append(&msgs, mk(MSG_ASSISTANT, "keep that"));
    message_list_append(&msgs, mk(MSG_USER, "latest"));

    ContextCompactionRequest request = {0};
    CHECK(context_compaction_prepare(&msgs, 4, &request) == AGENT_OK);
    CHECK(request.start == 0 && request.count == 3);
    CHECK(request.request_messages.len == 1);
    CHECK(strstr(request.request_messages.items[0].content, "old finding") != NULL);
    CHECK(msgs.len == 7); /* prepare does not mutate the live history */
    CHECK(context_compaction_apply(&msgs, &request, "facts from the model") == AGENT_OK);
    CHECK(msgs.len == 5);
    CHECK(strcmp(msgs.items[0].content, "facts from the model") == 0);
    CHECK(strcmp(msgs.items[msgs.len - 1].content, "latest") == 0);

    context_compaction_request_free(&request);
    message_list_free(&msgs);
    return g_failures;
}

static int test_compaction_keeps_tool_exchange_atomic(void) {
    MessageList msgs = {0};
    message_list_append(&msgs, mk(MSG_USER, "old zero"));
    message_list_append(&msgs, mk(MSG_USER, "old one"));
    message_list_append(&msgs, mk_tool_call_assistant());
    message_list_append(&msgs, mk_tool_result("call_1", "result one"));
    message_list_append(&msgs, mk_tool_result("call_2", "result two"));
    message_list_append(&msgs, mk(MSG_USER, "keep one"));
    message_list_append(&msgs, mk(MSG_ASSISTANT, "keep two"));
    message_list_append(&msgs, mk(MSG_USER, "latest"));

    ContextCompactionRequest request = {0};
    CHECK(context_compaction_prepare(&msgs, 4, &request) == AGENT_OK);
    /* The nominal count is four, which lands on the second tool result.
     * Move the cut back to retain the assistant and both responses. */
    CHECK(request.start == 0 && request.count == 2);
    CHECK(strstr(request.request_messages.items[0].content, "old one") != NULL);
    CHECK(strstr(request.request_messages.items[0].content, "result one") == NULL);
    CHECK(context_compaction_apply(&msgs, &request, "earlier facts") == AGENT_OK);
    CHECK(msgs.len == 7);
    CHECK(msgs.items[1].role == MSG_ASSISTANT && msgs.items[1].tool_calls.len == 2);
    CHECK(msgs.items[2].role == MSG_TOOL && strcmp(msgs.items[2].tool_call_id, "call_1") == 0);
    CHECK(msgs.items[3].role == MSG_TOOL && strcmp(msgs.items[3].tool_call_id, "call_2") == 0);

    context_compaction_request_free(&request);
    message_list_free(&msgs);
    return g_failures;
}

static int test_compact_noop_on_small(void) {
    MessageList msgs = {0};
    message_list_append(&msgs, mk(MSG_USER, "only one"));
    size_t before = msgs.len;
    CHECK(context_compact(&msgs, 4) == AGENT_OK);
    CHECK(msgs.len == before);
    message_list_free(&msgs);
    return g_failures;
}

static int test_compact_keeps_summary_message_count(void) {
    /* 12 messages, keep 4 -> 1 system + summary + 4 tail = 6 */
    MessageList msgs = {0};
    message_list_append(&msgs, mk(MSG_SYSTEM, "sys"));
    for (int i = 0; i < 11; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "m%d", i);
        message_list_append(&msgs, mk(MSG_USER, buf));
    }
    CHECK(context_compact(&msgs, 4) == AGENT_OK);
    CHECK(msgs.len == 1 + 1 + 4);
    message_list_free(&msgs);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_estimate();
    g_failures += test_needs_compact();
    g_failures += test_compact_keeps_system_and_tail();
    g_failures += test_compaction_prepare_and_apply();
    g_failures += test_compaction_keeps_tool_exchange_atomic();
    g_failures += test_compact_noop_on_small();
    g_failures += test_compact_keeps_summary_message_count();

    if (g_failures == 0) {
        printf("test_context: all tests passed\n");
        return 0;
    }
    printf("test_context: %d test(s) failed\n", g_failures);
    return 1;
}
