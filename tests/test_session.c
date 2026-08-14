/*
 * tests/test_session.c — JSONL session persistence tests.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "session/session.h"
#include "test_common.h"
#include "util/error.h"

static char g_tmpdir[512];

static int test_create_append_reload(void) {
    /* session dir inside the tmpdir */
    char sdir[600];
    snprintf(sdir, sizeof(sdir), "%s/sessions", g_tmpdir);

    Session* s = session_create(sdir, "/work", "test-model", "http://x/v1");
    CHECK(s != NULL);
    if (s == NULL) {
        return g_failures;
    }
    CHECK(s->id != NULL && strlen(s->id) > 0);
    CHECK(strcmp(s->model_name, "test-model") == 0);
    CHECK(strcmp(s->cwd, "/work") == 0);

    /* append messages */
    Message* u = message_new(MSG_USER);
    message_set_content(u, "hello session");
    CHECK(session_append_message(s, u) == AGENT_OK);
    message_free(u);

    Message* a = message_new(MSG_ASSISTANT);
    message_set_content(a, "hi there");
    ToolCall* tc = calloc(1, sizeof(ToolCall));
    CHECK(tc != NULL);
    tc->id = strdup("call_9");
    tc->name = strdup("read");
    tc->arguments = strdup("{\"path\":\"x\"}");
    tool_call_list_append(&a->tool_calls, tc);
    a->usage.input_tokens = 10;
    a->usage.output_tokens = 5;
    a->usage.total_tokens = 15;
    CHECK(session_append_message(s, a) == AGENT_OK);
    message_free(a);

    Message* t = message_new(MSG_TOOL);
    t->tool_call_id = strdup("call_9");
    t->content = strdup("result data");
    t->is_error = true;
    CHECK(session_append_message(s, t) == AGENT_OK);
    message_free(t);

    /* stats */
    s->request_count = 3;
    s->tool_call_count = 1;
    s->model_time_ms = 1234;
    s->usage = (Usage){10, 5, 0, 15};
    CHECK(session_append_stats(s) == AGENT_OK);
    CHECK(session_append_memory(s, "decision", "keep structured tool results") == AGENT_OK);

    char sid[128];
    snprintf(sid, sizeof(sid), "%s", s->id);
    session_free(s);

    /* reopen and verify everything round-trips */
    Session* s2 = session_open(sdir, sid);
    CHECK(s2 != NULL);
    if (s2 == NULL) {
        return g_failures;
    }
    CHECK(strcmp(s2->id, sid) == 0);
    CHECK(strcmp(s2->model_name, "test-model") == 0);
    CHECK(strcmp(s2->cwd, "/work") == 0);
    CHECK(strcmp(s2->provider, "http://x/v1") == 0);
    CHECK(s2->request_count == 3);
    CHECK(s2->tool_call_count == 1);
    CHECK(s2->model_time_ms == 1234);
    CHECK(s2->usage.input_tokens == 10);
    CHECK(s2->usage.output_tokens == 5);
    CHECK(strstr(session_memory(s2), "[decision] keep structured tool results") != NULL);

    MessageList msgs = {0};
    CHECK(session_load_messages(s2, &msgs) == AGENT_OK);
    CHECK(msgs.len == 3);

    Message* m0 = &msgs.items[0];
    CHECK(m0->role == MSG_USER && strcmp(m0->content, "hello session") == 0);

    Message* m1 = &msgs.items[1];
    CHECK(m1->role == MSG_ASSISTANT);
    CHECK(strcmp(m1->content, "hi there") == 0);
    CHECK(m1->tool_calls.len == 1);
    CHECK(strcmp(m1->tool_calls.items[0].id, "call_9") == 0);
    CHECK(strcmp(m1->tool_calls.items[0].name, "read") == 0);
    CHECK(strcmp(m1->tool_calls.items[0].arguments, "{\"path\":\"x\"}") == 0);
    CHECK(m1->usage.input_tokens == 10 && m1->usage.output_tokens == 5);

    Message* m2 = &msgs.items[2];
    CHECK(m2->role == MSG_TOOL);
    CHECK(strcmp(m2->tool_call_id, "call_9") == 0);
    CHECK(strcmp(m2->content, "result data") == 0);
    CHECK(m2->is_error);

    message_list_free(&msgs);
    session_free(s2);
    return g_failures;
}

static int test_compaction_round_trip(void) {
    char sdir[600];
    snprintf(sdir, sizeof(sdir), "%s/compaction", g_tmpdir);
    Session* s = session_create(sdir, NULL, "m", "p");
    CHECK(s != NULL);
    if (s == NULL) {
        return g_failures;
    }
    const char* contents[] = {"goal", "old one", "old two", "latest"};
    for (size_t i = 0; i < 4; i++) {
        Message* m = message_new(MSG_USER);
        CHECK(m != NULL);
        if (m != NULL) {
            message_set_content(m, contents[i]);
            CHECK(session_append_message(s, m) == AGENT_OK);
            message_free(m);
        }
    }
    CHECK(session_append_compaction(s, 1, 2, "facts retained") == AGENT_OK);
    char sid[128];
    snprintf(sid, sizeof(sid), "%s", s->id);
    session_free(s);

    Session* loaded = session_open(sdir, sid);
    CHECK(loaded != NULL);
    if (loaded != NULL) {
        MessageList msgs = {0};
        CHECK(session_load_messages(loaded, &msgs) == AGENT_OK);
        CHECK(msgs.len == 3);
        CHECK(strcmp(msgs.items[0].content, "goal") == 0);
        CHECK(strcmp(msgs.items[1].content, "facts retained") == 0);
        CHECK(strcmp(msgs.items[2].content, "latest") == 0);
        message_list_free(&msgs);
        session_free(loaded);
    }
    return g_failures;
}

static void append_text_message(Session* s, MessageRole role, const char* content) {
    Message* m = message_new(role);
    CHECK(m != NULL);
    if (m != NULL) {
        CHECK(message_set_content(m, content) == AGENT_OK);
        CHECK(session_append_message(s, m) == AGENT_OK);
        message_free(m);
    }
}

static int test_legacy_compaction_repairs_split_tool_exchange(void) {
    char sdir[600];
    snprintf(sdir, sizeof(sdir), "%s/tool-compaction", g_tmpdir);
    Session* s = session_create(sdir, NULL, "m", "p");
    CHECK(s != NULL);
    if (s == NULL) {
        return g_failures;
    }

    append_text_message(s, MSG_USER, "old zero");
    append_text_message(s, MSG_USER, "old one");

    Message* assistant = message_new(MSG_ASSISTANT);
    CHECK(assistant != NULL);
    if (assistant != NULL) {
        const char* ids[] = {"call_1", "call_2"};
        for (size_t i = 0; i < 2; i++) {
            ToolCall* tc = calloc(1, sizeof(ToolCall));
            CHECK(tc != NULL);
            if (tc != NULL) {
                tc->id = strdup(ids[i]);
                tc->name = strdup("read");
                tc->arguments = strdup("{}");
                CHECK(tool_call_list_append(&assistant->tool_calls, tc) == AGENT_OK);
            }
        }
        CHECK(session_append_message(s, assistant) == AGENT_OK);
        message_free(assistant);
    }

    Message* tool = message_new(MSG_TOOL);
    tool->tool_call_id = strdup("call_1");
    tool->content = strdup("result one");
    CHECK(session_append_message(s, tool) == AGENT_OK);
    message_free(tool);
    tool = message_new(MSG_TOOL);
    tool->tool_call_id = strdup("call_2");
    tool->content = strdup("result two");
    CHECK(session_append_message(s, tool) == AGENT_OK);
    message_free(tool);

    append_text_message(s, MSG_USER, "keep one");
    append_text_message(s, MSG_ASSISTANT, "keep two");
    append_text_message(s, MSG_USER, "latest");

    /* Legacy compaction count 4 retains call_2's tool response without its
     * assistant owner. Replay must reduce the count to 2 and keep the whole
     * exchange instead. */
    CHECK(session_append_compaction(s, 0, 4, "earlier facts") == AGENT_OK);
    char sid[128];
    snprintf(sid, sizeof(sid), "%s", s->id);
    session_free(s);

    Session* loaded = session_open(sdir, sid);
    CHECK(loaded != NULL);
    if (loaded != NULL) {
        MessageList msgs = {0};
        CHECK(session_load_messages(loaded, &msgs) == AGENT_OK);
        CHECK(msgs.len == 7);
        CHECK(msgs.items[0].role == MSG_USER &&
              strcmp(msgs.items[0].content, "earlier facts") == 0);
        CHECK(msgs.items[1].role == MSG_ASSISTANT && msgs.items[1].tool_calls.len == 2);
        CHECK(msgs.items[2].role == MSG_TOOL && strcmp(msgs.items[2].tool_call_id, "call_1") == 0);
        CHECK(msgs.items[3].role == MSG_TOOL && strcmp(msgs.items[3].tool_call_id, "call_2") == 0);
        message_list_free(&msgs);
        session_free(loaded);
    }
    return g_failures;
}

static int test_open_missing(void) {
    CHECK(session_open(g_tmpdir, "no-such-session") == NULL);
    return g_failures;
}

static int test_load_empty_session(void) {
    char sdir[600];
    snprintf(sdir, sizeof(sdir), "%s/sessions2", g_tmpdir);
    Session* s = session_create(sdir, NULL, "m", "p");
    CHECK(s != NULL);
    if (s == NULL) {
        return g_failures;
    }
    char sid[128];
    snprintf(sid, sizeof(sid), "%s", s->id);
    session_free(s);

    Session* s2 = session_open(sdir, sid);
    CHECK(s2 != NULL);
    if (s2 == NULL) {
        return g_failures;
    }
    MessageList msgs = {0};
    CHECK(session_load_messages(s2, &msgs) == AGENT_OK);
    CHECK(msgs.len == 0);
    message_list_free(&msgs);
    session_free(s2);
    return g_failures;
}

int main(void) {
    g_failures = 0;

    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/cagent-session-test-XXXXXX");
    char* dir = mkdtemp(g_tmpdir);
    CHECK(dir != NULL);

    g_failures += test_create_append_reload();
    g_failures += test_compaction_round_trip();
    g_failures += test_legacy_compaction_repairs_split_tool_exchange();
    g_failures += test_open_missing();
    g_failures += test_load_empty_session();

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmpdir);
    int rc = system(cmd);
    (void)rc;

    if (g_failures == 0) {
        printf("test_session: all tests passed\n");
        return 0;
    }
    printf("test_session: %d test(s) failed\n", g_failures);
    return 1;
}
