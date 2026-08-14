/*
 * tests/test_message.c — Message / MessageList / ToolCallList tests.
 */

#include <stdlib.h>
#include <string.h>

#include "agent/message.h"
#include "test_common.h"
#include "util/error.h"

static int test_message_basics(void) {
    Message* m = message_new(MSG_USER);
    CHECK(m != NULL);
    CHECK(m->role == MSG_USER);
    CHECK(m->content == NULL);

    CHECK(message_set_content(m, "hello") == AGENT_OK);
    CHECK(strcmp(m->content, "hello") == 0);

    /* set_content replaces */
    CHECK(message_set_content(m, "world") == AGENT_OK);
    CHECK(strcmp(m->content, "world") == 0);

    message_free(m);
    return g_failures;
}

static int test_tool_call_list(void) {
    ToolCallList list = {0};
    CHECK(list.len == 0);

    for (int i = 0; i < 20; i++) {
        ToolCall* tc = calloc(1, sizeof(ToolCall));
        CHECK(tc != NULL);
        char buf[16];
        snprintf(buf, sizeof(buf), "id-%d", i);
        tc->id = strdup(buf);
        tc->name = strdup("read");
        tc->arguments = strdup("{}");
        CHECK(tool_call_list_append(&list, tc) == AGENT_OK);
    }
    CHECK(list.len == 20);
    CHECK(strcmp(list.items[19].id, "id-19") == 0);
    CHECK(list.items[5].name != NULL && strcmp(list.items[5].name, "read") == 0);

    tool_call_list_free(&list);
    CHECK(list.len == 0);
    CHECK(list.items == NULL);
    return g_failures;
}

static int test_message_list(void) {
    MessageList list = {0};

    Message* m1 = message_new(MSG_USER);
    message_set_content(m1, "first");
    Message* m2 = message_new(MSG_ASSISTANT);
    message_set_content(m2, "second");
    Message* m3 = message_new(MSG_TOOL);
    m3->tool_call_id = strdup("call_1");
    m3->content = strdup("result");
    m3->is_error = true;

    CHECK(message_list_append(&list, m1) == AGENT_OK);
    CHECK(message_list_append(&list, m2) == AGENT_OK);
    CHECK(message_list_append(&list, m3) == AGENT_OK);
    CHECK(list.len == 3);

    Message* last = message_list_last(&list);
    CHECK(last == &list.items[2]);
    CHECK(last->role == MSG_TOOL);
    CHECK(strcmp(last->content, "result") == 0);
    CHECK(last->is_error);

    message_list_free(&list);
    CHECK(list.len == 0);
    return g_failures;
}

static int test_role_names(void) {
    CHECK(strcmp(message_role_name(MSG_SYSTEM), "system") == 0);
    CHECK(strcmp(message_role_name(MSG_USER), "user") == 0);
    CHECK(strcmp(message_role_name(MSG_ASSISTANT), "assistant") == 0);
    CHECK(strcmp(message_role_name(MSG_TOOL), "tool") == 0);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_message_basics();
    g_failures += test_tool_call_list();
    g_failures += test_message_list();
    g_failures += test_role_names();

    if (g_failures == 0) {
        printf("test_message: all tests passed\n");
        return 0;
    }
    printf("test_message: %d test(s) failed\n", g_failures);
    return 1;
}
