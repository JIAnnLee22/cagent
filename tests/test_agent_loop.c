/*
 * tests/test_agent_loop.c — agent loop tests with the scripted mock model.
 *
 * Covers (DESIGN.md §65 Phase 1): text turn, tool-call turn, unknown
 * tool, model error, bash through the loop, cancellation.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent/agent.h"
#include "mock_model.h"
#include "runtime/runtime.h"
#include "session/session.h"
#include "test_common.h"
#include "util/error.h"
#include "util/string.h"

static char g_tmpdir[256];

/* build a minimal runtime (openai provider with a fake key) and an agent
 * whose model is replaced by the mock */
static Agent* make_agent(const MockStep* steps, size_t n) {
    setenv("CAGENT_TEST_KEY", "test", 1);

    Config cfg = config_default();
    cfg.base_url = strdup("http://127.0.0.1:1/v1");
    cfg.api_key_env = strdup("CAGENT_TEST_KEY");
    cfg.model_name = strdup("mock");
    cfg.cwd = strdup(g_tmpdir);
    Runtime* rt = runtime_new(&cfg);
    config_free(&cfg);
    CHECK(rt != NULL);
    if (rt == NULL) {
        return NULL;
    }

    Agent* a = agent_new(rt, NULL);
    CHECK(a != NULL);
    if (a == NULL) {
        runtime_free(rt);
        return NULL;
    }

    Model* mock = mock_model_new("mock", steps, n);
    CHECK(mock != NULL);
    agent_set_model(a, mock);

    /* stash both so the test can free them later; the agent borrows */
    a->session = NULL;
    return a;
}

static void auto_approve_event(void* userdata, const AgentEvent* ev) {
    Agent* a = userdata;
    if (ev != NULL && ev->type == AGENT_EVT_TOOL_APPROVAL) {
        CHECK(agent_set_approval_result(a, true) == AGENT_OK);
    }
}

static void enable_auto_approval(Agent* a) {
    agent_set_approval_available(a, true);
    agent_set_event_cb(a, auto_approve_event, a);
}

typedef struct {
    Agent* agent;
    size_t approvals;
    bool approve_requests;
    char error[256];
} ApprovalEvents;

static void collect_approval_events(void* userdata, const AgentEvent* ev) {
    ApprovalEvents* events = userdata;
    if (events == NULL || ev == NULL) {
        return;
    }
    if (ev->type == AGENT_EVT_TOOL_APPROVAL) {
        events->approvals++;
        if (events->approve_requests) {
            CHECK(agent_set_approval_result(events->agent, true) == AGENT_OK);
        }
    } else if (ev->type == AGENT_EVT_ERROR && ev->text != NULL) {
        snprintf(events->error, sizeof(events->error), "%s", ev->text);
    }
}

typedef struct {
    bool event_seen;
    bool callback_seen;
    char preview[2048];
} SyncApproval;

static void collect_approval_event(void* userdata, const AgentEvent* ev) {
    SyncApproval* approval = userdata;
    if (ev != NULL && ev->type == AGENT_EVT_TOOL_APPROVAL) {
        approval->event_seen = true;
        snprintf(approval->preview, sizeof(approval->preview), "%s",
                 ev->preview != NULL ? ev->preview : "");
    }
}

static bool approve_synchronously(void* userdata, const AgentEvent* ev) {
    SyncApproval* approval = userdata;
    approval->callback_seen =
        ev != NULL && ev->type == AGENT_EVT_TOOL_APPROVAL && ev->preview != NULL;
    return true;
}

static bool reject_synchronously(void* userdata, const AgentEvent* ev) {
    SyncApproval* approval = userdata;
    approval->callback_seen = ev != NULL && ev->type == AGENT_EVT_TOOL_APPROVAL;
    return false;
}

static void teardown_agent(Agent* a) {
    Runtime* rt = a->runtime;
    Model* mock = a->model;
    agent_destroy(a);
    mock->ops->destroy(mock);
    runtime_free(rt);
}

typedef struct {
    String text;
    size_t deltas;
    size_t statuses;
    char error[256];
} TextEventCollector;

static void collect_text_events(void* userdata, const AgentEvent* ev) {
    TextEventCollector* collector = userdata;
    if (collector == NULL || ev == NULL)
        return;
    if (ev->type == AGENT_EVT_STATUS) {
        collector->statuses++;
        return;
    }
    if (ev->type == AGENT_EVT_ERROR) {
        snprintf(collector->error, sizeof(collector->error), "%s",
                 ev->text != NULL ? ev->text : "");
        return;
    }
    if (ev->type != AGENT_EVT_TEXT || ev->text == NULL)
        return;
    size_t len = ev->text_len != 0 ? ev->text_len : strlen(ev->text);
    string_append_n(&collector->text, ev->text, len);
    collector->deltas++;
}

static int test_streamed_text_events(void) {
    const char* chunks[] = {"hello", " ", "world"};
    MockStep steps[] = {MOCK_TEXT_CHUNKS_STEP(chunks, 3)};
    Agent* a = make_agent(steps, 1);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }
    TextEventCollector collector = {.text = string_new()};
    agent_set_event_cb(a, collect_text_events, &collector);

    CHECK(agent_run(a, "hi") == AGENT_OK);
    CHECK(collector.deltas == 3);
    CHECK(strcmp(collector.text.data, "hello world") == 0);
    CHECK(strcmp(a->messages.items[1].content, "hello world") == 0);

    string_free(&collector.text);
    teardown_agent(a);
    return g_failures;
}

static int test_text_turn(void) {
    MockStep steps[] = {MOCK_TEXT_STEP("hello there")};
    Agent* a = make_agent(steps, 1);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }

    int rc = agent_run(a, "hi");
    CHECK(rc == AGENT_OK);
    CHECK(a->state == AGENT_DONE);
    CHECK(a->messages.len == 2);

    Message* user = &a->messages.items[0];
    CHECK(user->role == MSG_USER && strcmp(user->content, "hi") == 0);
    Message* asst = &a->messages.items[1];
    CHECK(asst->role == MSG_ASSISTANT);
    CHECK(strcmp(asst->content, "hello there") == 0);
    CHECK(asst->tool_calls.len == 0);

    teardown_agent(a);
    return g_failures;
}

static int test_tool_call_turn(void) {
    /* round 1: read tool; round 2: final answer */
    MockStep steps[] = {
        MOCK_TOOL_STEP("call_1", "read", "{\"path\":\"file.txt\",\"offset\":1,\"limit\":10}"),
        MOCK_TEXT_STEP("done reading"),
    };
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }

    /* seed a file for the read tool */
    char path[512];
    snprintf(path, sizeof(path), "%s/file.txt", g_tmpdir);
    FILE* f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("line one\nline two\n", f);
        fclose(f);
    }

    int rc = agent_run(a, "read file.txt");
    CHECK(rc == AGENT_OK);
    CHECK(a->state == AGENT_DONE);
    CHECK(a->messages.len == 4);

    /* [0] user, [1] assistant(tool_calls), [2] tool, [3] assistant(text) */
    Message* asst = &a->messages.items[1];
    CHECK(asst->role == MSG_ASSISTANT);
    CHECK(asst->tool_calls.len == 1);
    CHECK(strcmp(asst->tool_calls.items[0].name, "read") == 0);
    CHECK(strcmp(asst->tool_calls.items[0].id, "call_1") == 0);
    CHECK(strcmp(asst->tool_calls.items[0].arguments,
                 "{\"path\":\"file.txt\",\"offset\":1,\"limit\":10}") == 0);

    Message* tool = &a->messages.items[2];
    CHECK(tool->role == MSG_TOOL);
    CHECK(!tool->is_error);
    CHECK(strcmp(tool->tool_call_id, "call_1") == 0);
    CHECK(strstr(tool->content, "L1: line one") != NULL);
    CHECK(strstr(tool->content, "L2: line two") != NULL);

    Message* final = &a->messages.items[3];
    CHECK(final->role == MSG_ASSISTANT);
    CHECK(strcmp(final->content, "done reading") == 0);

    teardown_agent(a);
    return g_failures;
}

static int test_unknown_tool(void) {
    MockStep steps[] = {
        MOCK_TOOL_STEP("call_1", "no_such_tool", "{}"),
        MOCK_TEXT_STEP("ok"),
    };
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }

    int rc = agent_run(a, "use the tool");
    CHECK(rc == AGENT_OK);
    CHECK(a->messages.len == 4);

    Message* tool = &a->messages.items[2];
    CHECK(tool->role == MSG_TOOL);
    CHECK(tool->is_error);
    CHECK(strstr(tool->content, "unknown tool") != NULL);

    teardown_agent(a);
    return g_failures;
}

static int test_model_error(void) {
    MockStep steps[] = {MOCK_ERROR_STEP(500, "mock boom")};
    Agent* a = make_agent(steps, 1);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }
    /* This test checks the terminal error path, not the transient retry
     * budget. Disable retries so the failure remains fast and deterministic. */
    a->runtime->config.max_retries = 0;

    int rc = agent_run(a, "hi");
    CHECK(rc != AGENT_OK);
    CHECK(a->state == AGENT_ERROR);

    teardown_agent(a);
    return g_failures;
}

static int test_transient_model_error_retries(void) {
    MockStep steps[] = {MOCK_ERROR_STEP(503, "temporarily unavailable"),
                        MOCK_TEXT_STEP("recovered")};
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL)
        return g_failures;
    TextEventCollector collector = {.text = string_new()};
    agent_set_event_cb(a, collect_text_events, &collector);
    CHECK(agent_run(a, "retry") == AGENT_OK);
    CHECK(a->state == AGENT_DONE);
    CHECK(mock_model_regular_requests(a->model) == 2);
    CHECK(strcmp(a->messages.items[1].content, "recovered") == 0);
    CHECK(collector.statuses == 1);
    CHECK(strcmp(collector.text.data, "recovered") == 0);
    string_free(&collector.text);
    teardown_agent(a);
    return g_failures;
}

static int test_output_limit_auto_continues(void) {
    MockStep steps[] = {
        MOCK_LIMIT_STEP(""),
        MOCK_TEXT_STEP("finished after continuation"),
    };
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL)
        return g_failures;
    TextEventCollector collector = {.text = string_new()};
    agent_set_event_cb(a, collect_text_events, &collector);

    CHECK(agent_run(a, "finish the task") == AGENT_OK);
    CHECK(a->state == AGENT_DONE);
    CHECK(mock_model_regular_requests(a->model) == 2);
    CHECK(collector.statuses == 1);
    CHECK(strcmp(collector.text.data, "finished after continuation") == 0);
    CHECK(a->messages.len == 3);
    CHECK(a->messages.items[1].role == MSG_ASSISTANT);
    CHECK(a->messages.items[1].content != NULL && a->messages.items[1].content[0] == '\0');
    CHECK(strstr(mock_model_last_system_prompt(a->model),
                 "previous model response reached its output-token limit") != NULL);

    string_free(&collector.text);
    teardown_agent(a);
    return g_failures;
}

static int test_output_limit_continuation_cap(void) {
    MockStep steps[] = {
        MOCK_LIMIT_STEP(""),
        MOCK_LIMIT_STEP(""),
        MOCK_LIMIT_STEP(""),
        MOCK_LIMIT_STEP(""),
    };
    Agent* a = make_agent(steps, 4);
    CHECK(a != NULL);
    if (a == NULL)
        return g_failures;
    TextEventCollector collector = {.text = string_new()};
    agent_set_event_cb(a, collect_text_events, &collector);

    CHECK(agent_run(a, "finish the task") != AGENT_OK);
    CHECK(a->state == AGENT_ERROR);
    CHECK(mock_model_regular_requests(a->model) == 4);
    CHECK(collector.statuses == 3);
    CHECK(strstr(collector.error, "after 3 consecutive automatic continuations") != NULL);
    CHECK(strstr(collector.error, "task remains incomplete") != NULL);

    string_free(&collector.text);
    teardown_agent(a);
    return g_failures;
}

static int test_output_limit_cap_resets_after_tool_progress(void) {
    MockStep steps[] = {
        MOCK_LIMIT_STEP("first partial response"),
        MOCK_LIMIT_STEP("second partial response"),
        MOCK_TOOL_STEP("call_1", "no_such_tool", "{}"),
        MOCK_LIMIT_STEP("third partial response"),
        MOCK_LIMIT_STEP("fourth partial response"),
        MOCK_TEXT_STEP("finished after separate truncation episodes"),
    };
    Agent* a = make_agent(steps, 6);
    CHECK(a != NULL);
    if (a == NULL)
        return g_failures;
    TextEventCollector collector = {.text = string_new()};
    agent_set_event_cb(a, collect_text_events, &collector);

    CHECK(agent_run(a, "finish a long tool task") == AGENT_OK);
    CHECK(a->state == AGENT_DONE);
    CHECK(mock_model_regular_requests(a->model) == 6);
    CHECK(collector.statuses == 4);
    CHECK(strcmp(message_list_last(&a->messages)->content,
                 "finished after separate truncation episodes") == 0);

    string_free(&collector.text);
    teardown_agent(a);
    return g_failures;
}

static int test_truncated_tool_call_is_not_executed(void) {
    MockStep steps[] = {
        MOCK_TOOL_LIMIT_STEP("call_1", "write",
                             "{\"path\":\"must-not-exist.txt\",\"content\":\"bad\""),
        MOCK_TEXT_STEP("recovered safely"),
    };
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL)
        return g_failures;
    TextEventCollector collector = {.text = string_new()};
    agent_set_event_cb(a, collect_text_events, &collector);

    CHECK(agent_run(a, "write only when the tool call is complete") == AGENT_OK);
    CHECK(a->state == AGENT_DONE);
    CHECK(mock_model_regular_requests(a->model) == 2);
    CHECK(a->messages.len == 3);
    CHECK(a->messages.items[1].tool_calls.len == 0);
    char path[512];
    snprintf(path, sizeof(path), "%s/must-not-exist.txt", g_tmpdir);
    CHECK(access(path, F_OK) != 0);

    string_free(&collector.text);
    teardown_agent(a);
    return g_failures;
}

static int test_long_tool_loop_has_no_fixed_limit(void) {
    enum { TOOL_ROUNDS = 65 };
    MockStep steps[TOOL_ROUNDS + 1];
    for (size_t i = 0; i < TOOL_ROUNDS; i++) {
        steps[i] = (MockStep){.type = MOCK_TOOL_CALL,
                              .tool_id = "call",
                              .tool_name = "no_such_tool",
                              .tool_args = "{}",
                              .stop_reason = MODEL_STOP_TOOL_CALLS};
    }
    steps[TOOL_ROUNDS] = (MockStep)MOCK_TEXT_STEP("finished long task");

    Agent* a = make_agent(steps, TOOL_ROUNDS + 1);
    CHECK(a != NULL);
    if (a == NULL)
        return g_failures;
    CHECK(agent_run(a, "keep working until complete") == AGENT_OK);
    CHECK(a->state == AGENT_DONE);
    CHECK(mock_model_regular_requests(a->model) == TOOL_ROUNDS + 1);
    CHECK(strcmp(message_list_last(&a->messages)->content, "finished long task") == 0);
    teardown_agent(a);
    return g_failures;
}

static int test_side_effect_without_approval_is_denied(void) {
    MockStep steps[] = {
        MOCK_TOOL_STEP("call_1", "bash", "{\"command\":\"echo must-not-run\"}"),
        MOCK_TEXT_STEP("handled denial"),
    };
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }

    agent_set_session_trusted(a, true);
    CHECK(agent_run(a, "run bash") == AGENT_OK);
    CHECK(a->messages.len == 4);
    Message* tool = &a->messages.items[2];
    CHECK(tool->is_error);
    CHECK(strstr(tool->content, "no approval host") != NULL);
    CHECK(strstr(tool->content, "must-not-run") == NULL);
    teardown_agent(a);
    return g_failures;
}

static int test_synchronous_approval_with_preview(void) {
    MockStep steps[] = {
        MOCK_TOOL_STEP("call_1", "write", "{\"path\":\"approved.txt\",\"content\":\"ok\\n\"}"),
        MOCK_TEXT_STEP("written"),
    };
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL)
        return g_failures;
    SyncApproval approval = {0};
    agent_set_event_cb(a, collect_approval_event, &approval);
    agent_set_approval_cb(a, approve_synchronously, &approval);
    CHECK(agent_run(a, "write the file") == AGENT_OK);
    CHECK(approval.event_seen);
    CHECK(approval.callback_seen);
    CHECK(strstr(approval.preview, "+ok") != NULL);
    char path[512];
    snprintf(path, sizeof(path), "%s/approved.txt", g_tmpdir);
    CHECK(access(path, F_OK) == 0);
    teardown_agent(a);
    return g_failures;
}

static int test_synchronous_approval_rejection(void) {
    MockStep steps[] = {
        MOCK_TOOL_STEP("call_1", "write", "{\"path\":\"rejected.txt\",\"content\":\"no\"}"),
        MOCK_TEXT_STEP("denial handled"),
    };
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL)
        return g_failures;
    SyncApproval approval = {0};
    agent_set_event_cb(a, collect_approval_event, &approval);
    agent_set_approval_cb(a, reject_synchronously, &approval);
    CHECK(agent_run(a, "do not write") == AGENT_OK);
    CHECK(approval.event_seen && approval.callback_seen);
    CHECK(a->messages.items[2].is_error);
    CHECK(strstr(a->messages.items[2].content, "denied by user") != NULL);
    char path[512];
    snprintf(path, sizeof(path), "%s/rejected.txt", g_tmpdir);
    CHECK(access(path, F_OK) != 0);
    teardown_agent(a);
    return g_failures;
}

static int test_session_trusted_mode(void) {
    MockStep steps[] = {
        MOCK_TOOL_STEP("call_1", "write", "{\"path\":\"trusted-a.txt\",\"content\":\"alpha\\n\"}"),
        MOCK_TOOL_STEP("call_2", "write", "{\"path\":\"trusted-b.txt\",\"content\":\"beta\\n\"}"),
        MOCK_TEXT_STEP("written"),
    };
    Agent* a = make_agent(steps, 3);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }

    ApprovalEvents events = {.agent = a};
    agent_set_approval_available(a, true);
    agent_set_event_cb(a, collect_approval_events, &events);
    agent_set_session_trusted(a, true);
    CHECK(agent_session_trusted(a));

    Agent* child = agent_spawn(a, NULL);
    CHECK(child != NULL);
    if (child != NULL) {
        CHECK(!agent_session_trusted(child));
        agent_set_session_trusted(child, true);
        CHECK(!agent_session_trusted(child));
        agent_destroy(child);
    }

    CHECK(agent_run(a, "write both files") == AGENT_OK);
    CHECK(events.approvals == 0);
    char first[512], second[512];
    snprintf(first, sizeof(first), "%s/trusted-a.txt", g_tmpdir);
    snprintf(second, sizeof(second), "%s/trusted-b.txt", g_tmpdir);
    CHECK(access(first, F_OK) == 0);
    CHECK(access(second, F_OK) == 0);

    teardown_agent(a);
    return g_failures;
}

static int test_session_trusted_off_restores_approval(void) {
    MockStep steps[] = {
        MOCK_TOOL_STEP("call_1", "write",
                       "{\"path\":\"manual-after-trust.txt\",\"content\":\"ok\\n\"}"),
        MOCK_TEXT_STEP("written"),
    };
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }

    ApprovalEvents events = {.agent = a, .approve_requests = true};
    agent_set_approval_available(a, true);
    agent_set_event_cb(a, collect_approval_events, &events);
    agent_set_session_trusted(a, true);
    agent_set_session_trusted(a, false);
    CHECK(!agent_session_trusted(a));
    CHECK(agent_run(a, "write with approval") == AGENT_OK);
    CHECK(events.approvals == 1);

    teardown_agent(a);
    return g_failures;
}

static int test_bash_through_loop(void) {
    MockStep steps[] = {
        MOCK_TOOL_STEP("call_1", "bash", "{\"command\":\"echo hi from bash\"}"),
        MOCK_TEXT_STEP("ok"),
    };
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }
    enable_auto_approval(a);

    int rc = agent_run(a, "run bash");
    CHECK(rc == AGENT_OK);
    CHECK(a->messages.len == 4);

    Message* tool = &a->messages.items[2];
    CHECK(tool->role == MSG_TOOL);
    CHECK(!tool->is_error);
    CHECK(strstr(tool->content, "hi from bash") != NULL);
    CHECK(strstr(tool->content, "exit code: 0") != NULL);

    teardown_agent(a);
    return g_failures;
}

static int test_parallel_safe_tool_calls(void) {
    char first_path[512];
    char second_path[512];
    snprintf(first_path, sizeof(first_path), "%s/parallel-a.txt", g_tmpdir);
    snprintf(second_path, sizeof(second_path), "%s/parallel-b.txt", g_tmpdir);
    FILE* first = fopen(first_path, "w");
    FILE* second = fopen(second_path, "w");
    CHECK(first != NULL && second != NULL);
    if (first != NULL) {
        fputs("alpha\n", first);
        fclose(first);
    }
    if (second != NULL) {
        fputs("beta\n", second);
        fclose(second);
    }

    MockToolCall calls[] = {
        {.id = "call_1", .name = "read", .args = "{\"path\":\"parallel-a.txt\"}"},
        {.id = "call_2", .name = "read", .args = "{\"path\":\"parallel-b.txt\"}"},
    };
    MockStep steps[] = {MOCK_TOOLS_STEP(calls, 2), MOCK_TEXT_STEP("parallel done")};
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL)
        return g_failures;
    CHECK(agent_run(a, "read both") == AGENT_OK);
    CHECK(a->messages.len == 5);
    CHECK(strstr(a->messages.items[2].content, "alpha") != NULL);
    CHECK(strstr(a->messages.items[3].content, "beta") != NULL);
    CHECK(strcmp(a->messages.items[4].content, "parallel done") == 0);
    teardown_agent(a);
    return g_failures;
}

static int test_multiple_async_tool_calls_are_serial(void) {
    MockToolCall calls[] = {
        {.id = "call_1", .name = "bash", .args = "{\"command\":\"printf first\"}"},
        {.id = "call_2", .name = "bash", .args = "{\"command\":\"printf second\"}"},
    };
    MockStep steps[] = {
        MOCK_TOOLS_STEP(calls, 2),
        MOCK_TEXT_STEP("both done"),
    };
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }
    enable_auto_approval(a);

    CHECK(agent_run(a, "run both") == AGENT_OK);
    CHECK(a->messages.len == 5);
    CHECK(a->messages.items[2].role == MSG_TOOL);
    CHECK(strcmp(a->messages.items[2].tool_call_id, "call_1") == 0);
    CHECK(strstr(a->messages.items[2].content, "first") != NULL);
    CHECK(a->messages.items[3].role == MSG_TOOL);
    CHECK(strcmp(a->messages.items[3].tool_call_id, "call_2") == 0);
    CHECK(strstr(a->messages.items[3].content, "second") != NULL);
    CHECK(strcmp(a->messages.items[4].content, "both done") == 0);

    teardown_agent(a);
    return g_failures;
}

static void seed_large_history(Agent* a) {
    for (int i = 0; i < 14; i++) {
        char text[256];
        memset(text, (int)('a' + (i % 26)), sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
        Message* m = message_new(i % 2 == 0 ? MSG_USER : MSG_ASSISTANT);
        CHECK(m != NULL);
        if (m != NULL) {
            CHECK(message_set_content(m, text) == AGENT_OK);
            CHECK(message_list_append(&a->messages, m) == AGENT_OK);
        }
    }
}

static int test_async_llm_compaction(void) {
    MockStep steps[] = {
        MOCK_TEXT_STEP("facts: keep the API decision"),
        MOCK_TEXT_STEP("final after compaction"),
    };
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }
    a->model->context_window = 100;
    seed_large_history(a);

    CHECK(agent_start(a, "continue the work") == AGENT_OK);
    CHECK(a->state == AGENT_WAIT_MODEL);
    CHECK(mock_model_compaction_requests(a->model) == 1);
    CHECK(mock_model_regular_requests(a->model) == 0);

    mock_model_pump(a->model); /* summary completes; normal request must not start in callback */
    CHECK(agent_resume(a) == AGENT_STEP_BUSY);
    CHECK(a->state == AGENT_WAIT_MODEL);
    CHECK(mock_model_compaction_requests(a->model) == 1);
    CHECK(mock_model_regular_requests(a->model) == 1);
    CHECK(a->messages.len < 15);

    mock_model_pump(a->model);
    CHECK(agent_resume(a) == AGENT_STEP_DONE);
    CHECK(a->state == AGENT_DONE);
    int found = 0;
    for (size_t i = 0; i < a->messages.len; i++) {
        if (a->messages.items[i].content != NULL &&
            strstr(a->messages.items[i].content, "facts: keep") != NULL) {
            found = 1;
        }
    }
    CHECK(found);
    teardown_agent(a);
    return g_failures;
}

static int test_llm_compaction_fallback(void) {
    MockStep steps[] = {
        MOCK_ERROR_STEP(500, "summary unavailable"),
        MOCK_TEXT_STEP("final after fallback"),
    };
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }
    a->model->context_window = 100;
    seed_large_history(a);
    CHECK(agent_start(a, "continue") == AGENT_OK);
    mock_model_pump(a->model);
    CHECK(agent_resume(a) == AGENT_STEP_BUSY);
    CHECK(mock_model_regular_requests(a->model) == 1);
    int found = 0;
    for (size_t i = 0; i < a->messages.len; i++) {
        if (a->messages.items[i].content != NULL &&
            strstr(a->messages.items[i].content, "context compaction") != NULL) {
            found = 1;
        }
    }
    CHECK(found);
    mock_model_pump(a->model);
    CHECK(agent_resume(a) == AGENT_STEP_DONE);
    teardown_agent(a);
    return g_failures;
}

static int test_llm_compaction_cancelled(void) {
    MockStep steps[] = {MOCK_TEXT_STEP("summary")};
    Agent* a = make_agent(steps, 1);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }
    a->model->context_window = 100;
    seed_large_history(a);
    CHECK(agent_start(a, "cancel") == AGENT_OK);
    cancel_token_cancel(&a->cancel);
    CHECK(agent_resume(a) == AGENT_STEP_CANCELLED);
    CHECK(a->state == AGENT_CANCELLED);
    CHECK(mock_model_compaction_requests(a->model) == 1);
    teardown_agent(a);
    return g_failures;
}

static int test_live_memory_injection(void) {
    MockStep steps[] = {MOCK_TEXT_STEP("ok"), MOCK_TEXT_STEP("ok")};
    Agent* a = make_agent(steps, 2);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }

    /* a real session with a bounded memory record */
    char sdir[600];
    snprintf(sdir, sizeof(sdir), "%s/mem-sessions", g_tmpdir);
    Session* s = session_create(sdir, g_tmpdir, "mock", "http://127.0.0.1:1/v1");
    CHECK(s != NULL);
    if (s == NULL) {
        teardown_agent(a);
        return g_failures;
    }
    CHECK(session_append_memory(s, "note", "remember-this-decision") == AGENT_OK);
    a->session = s;

    CHECK(agent_run(a, "turn one") == AGENT_OK);
    const char* sys = mock_model_last_system_prompt(a->model);
    CHECK(sys != NULL);
    CHECK(strstr(sys, "Session memory") != NULL);
    CHECK(strstr(sys, "remember-this-decision") != NULL);

    /* without a session no injection is appended */
    a->session = NULL;
    CHECK(agent_run(a, "turn two") == AGENT_OK);
    sys = mock_model_last_system_prompt(a->model);
    CHECK(sys != NULL);
    CHECK(strstr(sys, "Session memory") == NULL);

    session_free(s);
    teardown_agent(a);
    return g_failures;
}

static int test_cancelled(void) {
    MockStep steps[] = {MOCK_TEXT_STEP("never delivered")};
    Agent* a = make_agent(steps, 1);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }

    cancel_token_cancel(&a->cancel);
    int rc = agent_run(a, "hi");
    CHECK(rc == AGENT_ERR_CANCELLED);
    CHECK(a->state == AGENT_CANCELLED);

    teardown_agent(a);
    return g_failures;
}

static int test_cancel_inflight_model_request(void) {
    MockStep steps[] = {MOCK_TEXT_STEP("never delivered")};
    Agent* a = make_agent(steps, 1);
    CHECK(a != NULL);
    if (a == NULL) {
        return g_failures;
    }

    Model* sync = a->model;
    Model* async = mock_model_new_async("async-cancel", steps, 1);
    CHECK(async != NULL);
    agent_set_model(a, async);
    sync->ops->destroy(sync);

    CHECK(agent_start(a, "hi") == AGENT_OK);
    CHECK(a->state == AGENT_WAIT_MODEL);
    cancel_token_cancel(&a->cancel);
    CHECK(agent_resume(a) == AGENT_STEP_CANCELLED);
    CHECK(a->state == AGENT_CANCELLED);

    Model* recovery = mock_model_new("recovery", steps, 1);
    CHECK(recovery != NULL);
    agent_set_model(a, recovery);
    async->ops->destroy(async);
    CHECK(agent_run(a, "try again") == AGENT_OK);
    CHECK(a->state == AGENT_DONE);
    CHECK(strcmp(message_list_last(&a->messages)->content, "never delivered") == 0);

    teardown_agent(a);
    return g_failures;
}

int main(void) {
    g_failures = 0;

    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/cagent-agent-test-XXXXXX");
    char* dir = mkdtemp(g_tmpdir);
    CHECK(dir != NULL);

    g_failures += test_text_turn();
    g_failures += test_streamed_text_events();
    g_failures += test_tool_call_turn();
    g_failures += test_unknown_tool();
    g_failures += test_model_error();
    g_failures += test_transient_model_error_retries();
    g_failures += test_output_limit_auto_continues();
    g_failures += test_output_limit_continuation_cap();
    g_failures += test_output_limit_cap_resets_after_tool_progress();
    g_failures += test_truncated_tool_call_is_not_executed();
    g_failures += test_long_tool_loop_has_no_fixed_limit();
    g_failures += test_side_effect_without_approval_is_denied();
    g_failures += test_synchronous_approval_with_preview();
    g_failures += test_synchronous_approval_rejection();
    g_failures += test_session_trusted_mode();
    g_failures += test_session_trusted_off_restores_approval();
    g_failures += test_bash_through_loop();
    g_failures += test_parallel_safe_tool_calls();
    g_failures += test_multiple_async_tool_calls_are_serial();
    g_failures += test_async_llm_compaction();
    g_failures += test_llm_compaction_fallback();
    g_failures += test_llm_compaction_cancelled();
    g_failures += test_cancelled();
    g_failures += test_cancel_inflight_model_request();
    g_failures += test_live_memory_injection();

    /* cleanup */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmpdir);
    int rc = system(cmd);
    (void)rc;

    if (g_failures == 0) {
        printf("test_agent_loop: all tests passed\n");
        return 0;
    }
    printf("test_agent_loop: %d test(s) failed\n", g_failures);
    return 1;
}
