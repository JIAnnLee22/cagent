/*
 * tests/test_subagent.c — subagent tests (DESIGN.md §65, §88).
 *
 * Covers: single child, parallel children (shared mock script), cancel
 * propagation through the parent chain, 100 concurrent agents via the
 * scheduler, and a resource report (RSS / threads / fds).
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent/agent.h"
#include "mock_model.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "test_common.h"
#include "tool/tool.h"
#include "util/error.h"

extern Tool subagent_tool;

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

/* parent agent whose model is replaced by the mock */
static Agent* make_parent(Runtime* rt, Model* mock) {
    Agent* a = agent_new(rt, NULL);
    CHECK(a != NULL);
    agent_set_model(a, mock);
    return a;
}

static void free_agent_pair(Agent* a, Model* m) {
    a->model = m;
    agent_destroy(a);
    m->ops->destroy(m);
}

/* ------------------------------------------------------------------ */
/* single subagent                                                      */
/* ------------------------------------------------------------------ */

static int test_single_subagent(void) {
    Runtime* rt = make_runtime(4);
    CHECK(rt != NULL);

    /* script: parent round 1 -> subagent tool; child -> "child reply";
     * parent round 2 -> final */
    MockStep steps[] = {
        MOCK_TOOL_STEP("s1", "subagent", "{\"task\":\"explore the code\"}"),
        MOCK_TEXT_STEP("child reply"),
        MOCK_TEXT_STEP("parent final"),
    };
    Model* mock = mock_model_new("mock", steps, 3);
    Agent* parent = make_parent(rt, mock);

    int rc = agent_run(parent, "use a subagent");
    CHECK(rc == AGENT_OK);
    CHECK(parent->state == AGENT_DONE);

    /* the tool result message carries the child's answer */
    Message* tool = NULL;
    for (size_t i = 0; i < parent->messages.len; i++) {
        if (parent->messages.items[i].role == MSG_TOOL) {
            tool = &parent->messages.items[i];
        }
    }
    CHECK(tool != NULL);
    CHECK(!tool->is_error);
    CHECK(tool->content != NULL && strstr(tool->content, "child reply") != NULL);
    CHECK(strstr(tool->content, "[subagent]") != NULL);

    Message* final = message_list_last(&parent->messages);
    CHECK(final->role == MSG_ASSISTANT);
    CHECK(strcmp(final->content, "parent final") == 0);

    free_agent_pair(parent, mock);
    runtime_free(rt);
    return g_failures;
}

/* ------------------------------------------------------------------ */
/* parallel subagents                                                   */
/* ------------------------------------------------------------------ */

static int test_parallel_subagents(void) {
    Runtime* rt = make_runtime(8);
    CHECK(rt != NULL);

    /* two children run in parallel; the shared mock script is consumed in
     * spawn order: child-a, child-b, then the parent's final round */
    MockStep steps[] = {
        MOCK_TOOL_STEP("s1", "subagent",
                       "{\"tasks\":[{\"task\":\"task a\",\"role\":\"explore\"},"
                       "{\"task\":\"task b\",\"role\":\"review\"}]}"),
        MOCK_TEXT_STEP("result from a"),
        MOCK_TEXT_STEP("result from b"),
        MOCK_TEXT_STEP("parent done"),
    };
    Model* mock = mock_model_new("mock", steps, 4);
    Agent* parent = make_parent(rt, mock);

    int rc = agent_run(parent, "run two subagents");
    CHECK(rc == AGENT_OK);

    Message* tool = NULL;
    for (size_t i = 0; i < parent->messages.len; i++) {
        if (parent->messages.items[i].role == MSG_TOOL) {
            tool = &parent->messages.items[i];
        }
    }
    CHECK(tool != NULL);
    CHECK(!tool->is_error);
    CHECK(tool->content != NULL);
    CHECK(strstr(tool->content, "[explore]") != NULL);
    CHECK(strstr(tool->content, "result from a") != NULL);
    CHECK(strstr(tool->content, "[review]") != NULL);
    CHECK(strstr(tool->content, "result from b") != NULL);

    Message* final = message_list_last(&parent->messages);
    CHECK(strcmp(final->content, "parent done") == 0);

    free_agent_pair(parent, mock);
    runtime_free(rt);
    return g_failures;
}

/* ------------------------------------------------------------------ */
/* cancel propagation                                                   */
/* ------------------------------------------------------------------ */

static int test_cancel_propagates_to_children(void) {
    Runtime* rt = make_runtime(4);
    CHECK(rt != NULL);
    MockStep steps[] = {MOCK_TEXT_STEP("never")};
    Model* mock = mock_model_new("mock", steps, 1);
    Agent* parent = make_parent(rt, mock);

    Agent* child = agent_spawn(parent, NULL);
    CHECK(child != NULL);
    Agent* grandchild = agent_spawn(child, NULL);
    CHECK(grandchild != NULL);

    /* cancelling the parent must be visible down the whole chain */
    CHECK(!cancel_token_check(&grandchild->cancel));
    cancel_token_cancel(&parent->cancel);
    CHECK(cancel_token_check(&child->cancel));
    CHECK(cancel_token_check(&grandchild->cancel));

    /* a cancelled parent also refuses to run */
    int rc = agent_run(parent, "hi");
    CHECK(rc == AGENT_ERR_CANCELLED);

    agent_destroy(grandchild);
    agent_destroy(child);
    free_agent_pair(parent, mock);
    runtime_free(rt);
    return g_failures;
}

/* ------------------------------------------------------------------ */
/* 100 concurrent agents + resource report                              */
/* ------------------------------------------------------------------ */

static long read_status_field(const char* field) {
    FILE* f = fopen("/proc/self/status", "r");
    if (f == NULL) {
        return -1;
    }
    char line[256];
    long value = -1;
    size_t flen = strlen(field);
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, field, flen) == 0) {
            value = atol(line + flen);
            break;
        }
    }
    fclose(f);
    return value;
}

static int count_fds(void) {
    DIR* d = opendir("/proc/self/fd");
    if (d == NULL) {
        return -1;
    }
    int n = 0;
    while (readdir(d) != NULL) {
        n++;
    }
    closedir(d);
    return n;
}

static int test_many_agents_and_resources(void) {
    Runtime* rt = make_runtime(16);
    CHECK(rt != NULL);
    Scheduler* s = scheduler_new(16);
    CHECK(s != NULL);

    enum { N = 100 };
    Model* mocks[N];
    Agent* agents[N];
    MockStep steps[] = {MOCK_TEXT_STEP("ok")};

    for (int i = 0; i < N; i++) {
        mocks[i] = mock_model_new("mock", steps, 1);
        agents[i] = agent_new(rt, NULL);
        agent_set_model(agents[i], mocks[i]);
        CHECK(scheduler_add(s, agents[i], "task") == AGENT_OK);
    }

    for (int i = 0; i < 200 && !scheduler_all_done(s); i++) {
        runtime_pump(rt, 5); /* drives the HTTP engine */
        scheduler_pump(s);   /* advances the test's scheduler */
    }
    CHECK(scheduler_all_done(s));

    int done = 0;
    for (int i = 0; i < N; i++) {
        if (agents[i]->state == AGENT_DONE) {
            done++;
        }
    }
    CHECK(done == N);

    /* resource report (informational; DESIGN.md §88) */
    long rss_kb = read_status_field("VmRSS:");
    long threads = read_status_field("Threads:");
    int fds = count_fds();
    printf("resources after %d agents: RSS=%ld kB, threads=%ld, fds=%d\n", N, rss_kb, threads, fds);
    CHECK(rss_kb > 0);
    CHECK(threads > 0 && threads <= 8); /* fixed pool, no per-agent threads */
    CHECK(fds > 0 && fds < 200);        /* no fd explosion */

    for (int i = 0; i < N; i++) {
        agents[i]->model = mocks[i];
        agent_destroy(agents[i]);
        mocks[i]->ops->destroy(mocks[i]);
    }
    scheduler_free(s);
    runtime_free(rt);
    return g_failures;
}

int main(void) {
    g_failures = 0;

    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/cagent-subagent-test-XXXXXX");
    char* dir = mkdtemp(g_tmpdir);
    CHECK(dir != NULL);

    g_failures += test_single_subagent();
    g_failures += test_parallel_subagents();
    g_failures += test_cancel_propagates_to_children();
    g_failures += test_many_agents_and_resources();

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmpdir);
    int rc = system(cmd);
    (void)rc;

    if (g_failures == 0) {
        printf("test_subagent: all tests passed\n");
        return 0;
    }
    printf("test_subagent: %d test(s) failed\n", g_failures);
    return 1;
}
