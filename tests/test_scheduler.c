/*
 * tests/test_scheduler.c — scheduler concurrency tests.
 *
 * Async mock models let the test control when requests complete, so the
 * scheduler's concurrency limit and suspension points are observable:
 * with max_concurrent=1 the second agent must NOT start until the first
 * one's parked request is completed.
 */

#include <stdlib.h>
#include <string.h>

#include "agent/agent.h"
#include "mock_model.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "test_common.h"
#include "util/error.h"

static char g_tmpdir[256];

static Runtime* make_runtime(size_t max_concurrent) {
    setenv("CAGENT_TEST_KEY", "test", 1);
    Config cfg = config_default();
    cfg.base_url = strdup("http://127.0.0.1:1/v1");
    cfg.api_key_env = strdup("CAGENT_TEST_KEY");
    cfg.model_name = strdup("mock");
    cfg.cwd = strdup(g_tmpdir);
    cfg.max_concurrent_agents = (int64_t)max_concurrent;
    Runtime* rt = runtime_new(&cfg);
    config_free(&cfg);
    return rt;
}

static void auto_approve_event(void* userdata, const AgentEvent* ev) {
    Agent* a = userdata;
    if (ev != NULL && ev->type == AGENT_EVT_TOOL_APPROVAL) {
        CHECK(agent_set_approval_result(a, true) == AGENT_OK);
    }
}

static Agent* make_agent(Runtime* rt, Model* mock) {
    Agent* a = agent_new(rt, NULL);
    CHECK(a != NULL);
    agent_set_model(a, mock);
    agent_set_approval_available(a, true);
    agent_set_event_cb(a, auto_approve_event, a);
    return a;
}

static int test_sync_agents_all_complete(void) {
    Runtime* rt = make_runtime(2);
    CHECK(rt != NULL);
    Scheduler* s = scheduler_new(2);
    CHECK(s != NULL);

    Model* mocks[5];
    Agent* agents[5];
    MockStep steps[] = {MOCK_TEXT_STEP("done")};
    for (int i = 0; i < 5; i++) {
        mocks[i] = mock_model_new("mock", steps, 1);
        agents[i] = make_agent(rt, mocks[i]);
        CHECK(scheduler_add(s, agents[i], "hello") == AGENT_OK);
    }

    for (int i = 0; i < 100 && !scheduler_all_done(s); i++) {
        scheduler_pump(s);
    }
    CHECK(scheduler_all_done(s));
    for (int i = 0; i < 5; i++) {
        CHECK(agents[i]->state == AGENT_DONE);
    }

    for (int i = 0; i < 5; i++) {
        agents[i]->model = mocks[i];
        agent_destroy(agents[i]);
        mocks[i]->ops->destroy(mocks[i]);
    }
    scheduler_free(s);
    runtime_free(rt);
    return g_failures;
}

static int test_async_concurrency_limit(void) {
    Runtime* rt = make_runtime(1);
    CHECK(rt != NULL);
    Scheduler* s = scheduler_new(1);
    CHECK(s != NULL);

    MockStep steps[] = {MOCK_TEXT_STEP("one"), MOCK_TEXT_STEP("two")};
    Model* m1 = mock_model_new_async("mock", steps, 1);
    Model* m2 = mock_model_new_async("mock", steps, 1);
    Agent* a1 = make_agent(rt, m1);
    Agent* a2 = make_agent(rt, m2);
    CHECK(scheduler_add(s, a1, "first") == AGENT_OK);
    CHECK(scheduler_add(s, a2, "second") == AGENT_OK);

    /* round 1: a1 starts and parks on its request; a2 must NOT start */
    scheduler_pump(s);
    CHECK(a1->state == AGENT_WAIT_MODEL);
    CHECK(a2->state == AGENT_READY); /* blocked by the concurrency limit */
    CHECK(s->running == 1);

    /* a1 completes its parked request and finishes; the freed slot lets
     * a2 start in the same pump round */
    mock_model_pump(m1);
    scheduler_pump(s);
    CHECK(a1->state == AGENT_DONE);
    CHECK(a2->state == AGENT_WAIT_MODEL);
    CHECK(s->running == 1);

    mock_model_pump(m2);
    scheduler_pump(s);
    CHECK(a2->state == AGENT_DONE);
    CHECK(scheduler_all_done(s));

    a1->model = m1;
    a2->model = m2;
    agent_destroy(a1);
    agent_destroy(a2);
    m1->ops->destroy(m1);
    m2->ops->destroy(m2);
    scheduler_free(s);
    runtime_free(rt);
    return g_failures;
}

static int test_async_tool_turn_two_rounds(void) {
    /* async agent doing a tool call: two parked requests */
    Runtime* rt = make_runtime(4);
    CHECK(rt != NULL);
    Scheduler* s = scheduler_new(4);
    CHECK(s != NULL);

    MockStep steps[] = {
        MOCK_TOOL_STEP("c1", "bash", "{\"command\":\"echo async-ok\"}"),
        MOCK_TEXT_STEP("final answer"),
    };
    Model* m = mock_model_new_async("mock", steps, 2);
    Agent* a = make_agent(rt, m);
    CHECK(scheduler_add(s, a, "do it") == AGENT_OK);

    scheduler_pump(s);
    CHECK(a->state == AGENT_WAIT_MODEL);
    mock_model_pump(m); /* round 1: tool call */
    scheduler_pump(s);
    /* bash now parks as an async tool instead of blocking this pump. */
    CHECK(a->state == AGENT_WAIT_TOOL);
    CHECK(a->messages.len == 2); /* user, assistant(tool_calls) */

    for (int i = 0; i < 100 &&
                    (a->state == AGENT_WAIT_TOOL || a->state == AGENT_WAIT_USER); i++) {
        runtime_pump(rt, 10); /* dispatch process pipe/timer events */
        scheduler_pump(s);    /* poll the agent in this test scheduler */
    }
    CHECK(a->state == AGENT_WAIT_MODEL);
    CHECK(a->messages.len == 3); /* tool result appended before round 2 */

    mock_model_pump(m); /* round 2: final text */
    scheduler_pump(s);
    CHECK(a->state == AGENT_DONE);
    CHECK(a->messages.len == 4); /* user, assistant(tool), tool, assistant */

    /* the tool really ran (cwd + echo) */
    Message* tool = &a->messages.items[2];
    CHECK(tool->role == MSG_TOOL);
    CHECK(!tool->is_error);
    CHECK(strstr(tool->content, "async-ok") != NULL);

    a->model = m;
    agent_destroy(a);
    m->ops->destroy(m);
    scheduler_free(s);
    runtime_free(rt);
    return g_failures;
}

static int test_slow_tool_does_not_block_other_agent(void) {
    Runtime* rt = make_runtime(4);
    CHECK(rt != NULL);
    Scheduler* s = scheduler_new(4);
    CHECK(s != NULL);

    MockStep slow_steps[] = {
        MOCK_TOOL_STEP("slow", "bash", "{\"command\":\"sleep 0.2; echo slow-done\"}"),
        MOCK_TEXT_STEP("slow final"),
    };
    MockStep fast_steps[] = {MOCK_TEXT_STEP("fast final")};
    Model* slow_model = mock_model_new_async("slow", slow_steps, 2);
    Model* fast_model = mock_model_new_async("fast", fast_steps, 1);
    Agent* slow = make_agent(rt, slow_model);
    Agent* fast = make_agent(rt, fast_model);
    CHECK(scheduler_add(s, slow, "slow") == AGENT_OK);
    CHECK(scheduler_add(s, fast, "fast") == AGENT_OK);

    scheduler_pump(s);
    mock_model_pump(slow_model);
    mock_model_pump(fast_model);
    scheduler_pump(s);
    CHECK(slow->state == AGENT_WAIT_TOOL || slow->state == AGENT_WAIT_USER);
    CHECK(fast->state == AGENT_DONE); /* completed while slow process is alive */

    for (int i = 0; i < 100 &&
                    (slow->state == AGENT_WAIT_TOOL || slow->state == AGENT_WAIT_USER); i++) {
        runtime_pump(rt, 10);
        scheduler_pump(s);
    }
    CHECK(slow->state == AGENT_WAIT_MODEL);
    mock_model_pump(slow_model);
    scheduler_pump(s);
    CHECK(slow->state == AGENT_DONE);

    slow->model = slow_model;
    fast->model = fast_model;
    agent_destroy(slow);
    agent_destroy(fast);
    slow_model->ops->destroy(slow_model);
    fast_model->ops->destroy(fast_model);
    scheduler_free(s);
    runtime_free(rt);
    return g_failures;
}

static int test_cancel_async_bash(void) {
    Runtime* rt = make_runtime(2);
    CHECK(rt != NULL);
    Scheduler* s = scheduler_new(2);
    CHECK(s != NULL);
    MockStep steps[] = {
        MOCK_TOOL_STEP("cancel", "bash", "{\"command\":\"sleep 30\",\"timeout\":300}"),
    };
    Model* model = mock_model_new_async("cancel", steps, 1);
    Agent* agent = make_agent(rt, model);
    CHECK(scheduler_add(s, agent, "cancel it") == AGENT_OK);

    scheduler_pump(s);
    mock_model_pump(model);
    scheduler_pump(s);
    CHECK(agent->state == AGENT_WAIT_TOOL || agent->state == AGENT_WAIT_USER);
    cancel_token_cancel(&agent->cancel);
    for (int i = 0; i < 300 && agent->state != AGENT_CANCELLED; i++) {
        runtime_pump(rt, 10);
        scheduler_pump(s);
    }
    CHECK(agent->state == AGENT_CANCELLED);
    CHECK(agent->messages.len == 2); /* cancelled tool result is not sent to the model */

    agent->model = model;
    agent_destroy(agent);
    model->ops->destroy(model);
    scheduler_free(s);
    runtime_free(rt);
    return g_failures;
}

int main(void) {
    g_failures = 0;

    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/cagent-sched-test-XXXXXX");
    char* dir = mkdtemp(g_tmpdir);
    CHECK(dir != NULL);

    g_failures += test_sync_agents_all_complete();
    g_failures += test_async_concurrency_limit();
    g_failures += test_async_tool_turn_two_rounds();
    g_failures += test_slow_tool_does_not_block_other_agent();
    g_failures += test_cancel_async_bash();

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmpdir);
    int rc = system(cmd);
    (void)rc;

    if (g_failures == 0) {
        printf("test_scheduler: all tests passed\n");
        return 0;
    }
    printf("test_scheduler: %d test(s) failed\n", g_failures);
    return 1;
}
