/*
 * tests/test_tools.c — read / write / bash tool tests (DESIGN.md §64).
 *
 * Covers: read nonexistent, read line numbers, read offset/limit, read
 * binary, write create/overwrite, bash exit 0 / nonzero / stderr /
 * timeout.
 */

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agent/agent.h"
#include "session/session.h"
#include "test_common.h"
#include "tool/tool.h"
#include "util/error.h"
#include "util/string.h"

extern Tool read_tool;
extern Tool write_tool;
extern Tool bash_tool;
extern Tool bench_tool;
extern Tool git_checkpoint_tool;
extern Tool git_restore_checkpoint_tool;
extern Tool git_diff_tool;
extern Tool git_commit_tool;
extern Tool git_revert_tool;
extern Tool diagnose_tool;
extern Tool memory_tool;
extern Tool plan_tool;
extern Tool test_tool;
extern Tool list_tool;
extern Tool find_tool;

static char g_tmpdir[256];

static ToolContext make_ctx(void) {
    ToolContext ctx = {0};
    ctx.cwd = g_tmpdir;
    return ctx;
}

static void run_tool(Tool* tool, const char* args, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    ToolContext ctx = make_ctx();
    CHECK(tool->execute(&ctx, args, result) == AGENT_OK);
    CHECK(result->content != NULL);
}

static void run_preview(Tool* tool, const char* args, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    ToolContext ctx = make_ctx();
    CHECK(tool->preview != NULL);
    CHECK(tool->preview(&ctx, args, result) == AGENT_OK);
    CHECK(result->content != NULL);
}

static int test_read_nonexistent(void) {
    ToolResult r = {0};
    run_tool(&read_tool, "{\"path\":\"nope.txt\"}", &r);
    CHECK(r.is_error);
    CHECK(strstr(r.content, "cannot open") != NULL);
    free(r.content);
    return g_failures;
}

static int test_read_line_numbers_and_offset(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/sample.txt", g_tmpdir);
    FILE* f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        for (int i = 1; i <= 10; i++) {
            fprintf(f, "line %d\n", i);
        }
        fclose(f);
    }

    ToolResult r = {0};
    char args[600];
    snprintf(args, sizeof(args), "{\"path\":\"sample.txt\"}");
    run_tool(&read_tool, args, &r);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "L1: line 1") != NULL);
    CHECK(strstr(r.content, "L10: line 10") != NULL);
    free(r.content);

    /* offset + limit slice */
    r.content = NULL;
    snprintf(args, sizeof(args), "{\"path\":\"sample.txt\",\"offset\":8,\"limit\":2}");
    run_tool(&read_tool, args, &r);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "L8: line 8") != NULL);
    CHECK(strstr(r.content, "L9: line 9") != NULL);
    CHECK(strstr(r.content, "L10:") == NULL);
    CHECK(strstr(r.content, "more lines") != NULL); /* truncated note */
    free(r.content);

    return g_failures;
}

static int test_read_binary(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/bin.dat", g_tmpdir);
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        fwrite("abc\0def", 1, 7, f); /* fputs would stop at the NUL */
        fclose(f);
    }

    ToolResult r = {0};
    char args[600];
    snprintf(args, sizeof(args), "{\"path\":\"bin.dat\"}");
    run_tool(&read_tool, args, &r);
    CHECK(r.is_error);
    CHECK(strstr(r.content, "binary") != NULL);
    free(r.content);
    return g_failures;
}

static int test_write_create_and_overwrite(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/out.txt", g_tmpdir);

    ToolResult r = {0};
    char args[1024];
    snprintf(args, sizeof(args), "{\"path\":\"out.txt\",\"content\":\"first\\n\"}");
    run_tool(&write_tool, args, &r);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "wrote 6 bytes") != NULL);
    free(r.content);

    /* verify content */
    FILE* f = fopen(path, "rb");
    CHECK(f != NULL);
    char buf[64] = {0};
    if (f != NULL) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        CHECK(n == 6 && strcmp(buf, "first\n") == 0);
        fclose(f);
    }

    /* overwrite */
    r.content = NULL;
    snprintf(args, sizeof(args), "{\"path\":\"out.txt\",\"content\":\"second\"}");
    run_tool(&write_tool, args, &r);
    CHECK(!r.is_error);
    free(r.content);

    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f != NULL) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        CHECK(n == 6 && strcmp(buf, "second") == 0);
        fclose(f);
    }

    /* missing content arg */
    r.content = NULL;
    run_tool(&write_tool, "{\"path\":\"out.txt\"}", &r);
    CHECK(r.is_error);
    free(r.content);

    return g_failures;
}

static int test_approval_previews(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/preview.txt", g_tmpdir);
    FILE* f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("before\n", f);
        fclose(f);
    }
    ToolResult r = {0};
    run_preview(&write_tool, "{\"path\":\"preview.txt\",\"content\":\"after\\n\"}", &r);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "---") != NULL);
    CHECK(strstr(r.content, "-before") != NULL);
    CHECK(strstr(r.content, "+after") != NULL);
    free(r.content);

    r = (ToolResult){0};
    run_preview(&bash_tool, "{\"command\":\"rm -rf build\",\"timeout\":5}", &r);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "timeout: 5s") != NULL);
    CHECK(strstr(r.content, "risky patterns") != NULL);
    free(r.content);
    return g_failures;
}

static int test_navigation_tools(void) {
    char dir[512];
    char sub[512];
    snprintf(dir, sizeof(dir), "%s/nav", g_tmpdir);
    snprintf(sub, sizeof(sub), "%s/nav/sub", g_tmpdir);
    CHECK(mkdir(dir, 0700) == 0);
    CHECK(mkdir(sub, 0700) == 0);
    char file[512];
    snprintf(file, sizeof(file), "%s/nav/main.c", g_tmpdir);
    FILE* f = fopen(file, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("int main(void) {}\n", f);
        fclose(f);
    }
    snprintf(file, sizeof(file), "%s/nav/sub/helper.h", g_tmpdir);
    f = fopen(file, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("#pragma once\n", f);
        fclose(f);
    }

    ToolResult r = {0};
    run_tool(&list_tool, "{\"path\":\"nav\",\"depth\":2}", &r);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "nav/main.c") != NULL);
    CHECK(strstr(r.content, "nav/sub/helper.h") != NULL);
    free(r.content);

    r = (ToolResult){0};
    run_tool(&find_tool, "{\"pattern\":\"*.h\",\"path\":\"nav\"}", &r);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "helper.h") != NULL);
    CHECK(strstr(r.content, "main.c") == NULL);
    free(r.content);

    r = (ToolResult){0};
    run_tool(&find_tool, "{\"pattern\":\"passwd\",\"path\":\"../\"}", &r);
    CHECK(r.is_error);
    CHECK(strstr(r.content, "workspace") != NULL);
    free(r.content);
    return g_failures;
}

static int test_bash_ok(void) {
    ToolResult r = {0};
    run_tool(&bash_tool, "{\"command\":\"echo hello\"}", &r);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "hello") != NULL);
    CHECK(strstr(r.content, "exit code: 0") != NULL);
    free(r.content);
    return g_failures;
}

static int test_bash_nonzero_exit(void) {
    ToolResult r = {0};
    run_tool(&bash_tool, "{\"command\":\"exit 3\"}", &r);
    CHECK(!r.is_error); /* the tool itself succeeds; the exit code is data */
    CHECK(strstr(r.content, "exit code: 3") != NULL);
    free(r.content);
    return g_failures;
}

static int test_bash_stderr(void) {
    ToolResult r = {0};
    run_tool(&bash_tool, "{\"command\":\"echo to-stderr >&2\"}", &r);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "to-stderr") != NULL);
    free(r.content);
    return g_failures;
}

static int test_bash_timeout(void) {
    ToolResult r = {0};
    run_tool(&bash_tool, "{\"command\":\"sleep 30\",\"timeout\":1}", &r);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "timed out") != NULL);
    free(r.content);
    return g_failures;
}

static int test_bash_output_cap(void) {
    ToolResult r = {0};
    /* 1 MiB of output > 64 KiB cap */
    run_tool(&bash_tool, "{\"command\":\"head -c 1048576 /dev/zero | tr '\\\\0' 'x'\"}", &r);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "truncated at 64 KiB") != NULL);
    CHECK(strlen(r.content) < 80 * 1024);
    free(r.content);
    return g_failures;
}

static int test_structured_test_tool(void) {
    /* executing arbitrary project code requires explicit approval */
    CHECK(test_tool.flags & TOOL_FLAG_APPROVAL_REQUIRED);
    CHECK(test_tool.preview != NULL);

    ToolResult r = {0};
    run_preview(&test_tool, "{\"action\":\"ctest\",\"build_dir\":\"build\"}", &r);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "CTest") != NULL);
    free(r.content);
    r.content = NULL;
    run_tool(&test_tool, "{\"action\":\"invalid\"}", &r);
    CHECK(r.is_error);
    free(r.content);
    r.content = NULL;
    run_tool(&test_tool, "{\"action\":\"ctest\",\"build_dir\":\"missing\"}", &r);
    CHECK(r.is_error);
    CHECK(strstr(r.content, "ctest summary:") != NULL ||
          strstr(r.content, "failed to start") != NULL);
    /* failures always carry a recovery directive */
    CHECK(strstr(r.content, "recovery:") != NULL);
    free(r.content);
    return g_failures;
}

static int test_bench_tool(void) {
    ToolResult r = {0};
    run_tool(&bench_tool, "{\"build_dir\":\"../outside\"}", &r);
    CHECK(r.is_error);
    free(r.content);
    r.content = NULL;
    run_tool(&bench_tool, "{\"build_dir\":\"missing-bench\"}", &r);
    CHECK(r.is_error);
    CHECK(strstr(r.content, "benchmark") != NULL || strstr(r.content, "cagent_bench") != NULL);
    free(r.content);
    return g_failures;
}

static int test_diagnose_tool(void) {
    Agent agent = {0};
    agent.state = AGENT_WAIT_TOOL;
    agent.config.cwd = g_tmpdir;
    ToolContext ctx = {.agent = &agent};
    ToolResult r = {0};
    CHECK(diagnose_tool.execute(&ctx, "{}", &r) == AGENT_OK);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "agent.state=wait_tool") != NULL);
    CHECK(strstr(r.content, "agent.messages=0") != NULL);
    free(r.content);
    return g_failures;
}

static int test_memory_tool(void) {
    char sdir[512];
    snprintf(sdir, sizeof(sdir), "%s/memory-sessions", g_tmpdir);
    Session* session = session_create(sdir, g_tmpdir, "mock", "provider");
    CHECK(session != NULL);
    if (session == NULL)
        return g_failures;
    Agent agent = {0};
    agent.session = session;
    ToolContext ctx = {.agent = &agent};
    ToolResult r = {0};
    CHECK(memory_tool.execute(&ctx, "{\"kind\":\"decision\",\"content\":\"keep argv-only tools\"}",
                              &r) == AGENT_OK);
    CHECK(!r.is_error);
    free(r.content);
    char sid[128];
    snprintf(sid, sizeof(sid), "%s", session->id);
    session_free(session);
    Session* loaded = session_open(sdir, sid);
    CHECK(loaded != NULL);
    if (loaded != NULL) {
        CHECK(strstr(session_memory(loaded), "keep argv-only tools") != NULL);
        session_free(loaded);
    }
    return g_failures;
}

static int test_plan_tool(void) {
    ToolResult r = {0};
    run_tool(&plan_tool, "{\"action\":\"list\"}", &r);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "no plan steps") != NULL);
    free(r.content);

    r.content = NULL;
    run_tool(&plan_tool,
             "{\"action\":\"add\",\"id\":\"build\",\"title\":\"Build project\","
             "\"acceptance\":\"ctest passes\"}",
             &r);
    CHECK(!r.is_error);
    free(r.content);

    r.content = NULL;
    run_tool(&plan_tool, "{\"action\":\"start\",\"id\":\"build\"}", &r);
    CHECK(!r.is_error);
    free(r.content);
    r.content = NULL;
    run_tool(&plan_tool, "{\"action\":\"fail\",\"id\":\"build\",\"error\":\"compile error\"}", &r);
    CHECK(!r.is_error);
    free(r.content);

    r.content = NULL;
    run_tool(&plan_tool, "{\"action\":\"list\"}", &r);
    CHECK(strstr(r.content, "[failed]") != NULL);
    CHECK(strstr(r.content, "attempts=1") != NULL);
    free(r.content);

    r.content = NULL;
    run_tool(&plan_tool, "{\"action\":\"complete\",\"id\":\"build\",\"result\":\"green\"}", &r);
    CHECK(!r.is_error);
    free(r.content);
    r.content = NULL;
    run_tool(&plan_tool, "{\"action\":\"list\"}", &r);
    CHECK(strstr(r.content, "[completed]") != NULL);
    CHECK(strstr(r.content, "result: green") != NULL);
    free(r.content);
    return g_failures;
}

static int test_git_checkpoint_tools(void) {
    char original[sizeof(g_tmpdir)];
    snprintf(original, sizeof(original), "%s", g_tmpdir);
    char repo[sizeof(g_tmpdir)];
    CHECK(strlen(original) + strlen("/checkpoint-repo") < sizeof(repo));
    memcpy(repo, original, strlen(original));
    memcpy(repo + strlen(original), "/checkpoint-repo", strlen("/checkpoint-repo") + 1);
    CHECK(mkdir(repo, 0700) == 0);
    memcpy(g_tmpdir, repo, strlen(repo) + 1);
    const char* old_home = getenv("HOME");
    char saved_home[512];
    snprintf(saved_home, sizeof(saved_home), "%s", old_home != NULL ? old_home : "");
    setenv("HOME", repo, 1);

    char command[2200];
    snprintf(command, sizeof(command),
             "git -C %s init -q && git -C %s config user.email test@example.com && "
             "git -C %s config user.name test && printf base > %s/tracked.txt && "
             "git -C %s add tracked.txt && git -C %s commit -qm base",
             repo, repo, repo, repo, repo, repo);
    CHECK(system(command) == 0);

    ToolResult r = {0};
    CHECK(!git_checkpoint_available(repo));
    run_tool(&git_checkpoint_tool, "{}", &r);
    CHECK(!r.is_error);
    CHECK(git_checkpoint_available(repo));
    free(r.content);

    FILE* f;
    char tracked[sizeof(repo) + 32];
    snprintf(tracked, sizeof(tracked), "%s/tracked.txt", repo);
    f = fopen(tracked, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("broken", f);
        fclose(f);
    }
    r.content = NULL;
    run_tool(&git_restore_checkpoint_tool, "{}", &r);
    CHECK(!r.is_error);
    free(r.content);
    f = fopen(tracked, "r");
    char content[32] = {0};
    if (f != NULL) {
        (void)fread(content, 1, sizeof(content) - 1, f);
        fclose(f);
    }
    CHECK(strcmp(content, "base") == 0);

    char untracked[sizeof(repo) + 32];
    snprintf(untracked, sizeof(untracked), "%s/untracked.txt", repo);
    f = fopen(untracked, "w");
    if (f != NULL) {
        fputs("keep", f);
        fclose(f);
    }
    r.content = NULL;
    run_tool(&git_restore_checkpoint_tool, "{}", &r);
    CHECK(!r.is_error);
    free(r.content);
    CHECK(access(untracked, F_OK) == 0);

    if (saved_home[0] != '\0')
        setenv("HOME", saved_home, 1);
    else
        unsetenv("HOME");
    snprintf(g_tmpdir, sizeof(g_tmpdir), "%s", original);
    return g_failures;
}

static int test_plan_summary(void) {
    char dir[512];
    char sub[600];
    snprintf(dir, sizeof(dir), "%s/plan-summary", g_tmpdir);
    snprintf(sub, sizeof(sub), "%s/.cagent", dir);
    CHECK(mkdir(dir, 0700) == 0);
    CHECK(mkdir(sub, 0700) == 0);
    char path[640];
    snprintf(path, sizeof(path), "%s/plan.json", sub);
    FILE* f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("{\"version\":1,\"steps\":["
              "{\"id\":\"step-1\",\"title\":\"Build the thing\","
              "\"acceptance\":\"tests green\",\"status\":\"in_progress\","
              "\"attempts\":2,\"result\":\"halfway\"}]}\n",
              f);
        fclose(f);
    }
    String out = string_new();
    CHECK(plan_summary(dir, &out) == AGENT_OK);
    CHECK(strstr(out.data, "step-1") != NULL);
    CHECK(strstr(out.data, "Build the thing") != NULL);
    CHECK(strstr(out.data, "tests green") != NULL);
    CHECK(strstr(out.data, "attempts=2") != NULL);
              "\"attempts\":2,\"result\":\"halfway\"}]}\n",
    string_free(&out);

    /* no plan file -> empty output, not an error */
    char empty_dir[512];
    snprintf(empty_dir, sizeof(empty_dir), "%s/plan-empty", g_tmpdir);
    CHECK(mkdir(empty_dir, 0700) == 0);
    out = string_new();
    CHECK(plan_summary(empty_dir, &out) == AGENT_OK);
    CHECK(out.len == 0);
    string_free(&out);
    return g_failures;
}

static int test_git_tools(void) {
    char command[1600];
    snprintf(command, sizeof(command),
             "git -C %s init -q && git -C %s config user.email test@example.com && "
             "git -C %s config user.name test && printf one > %s/tracked.txt && "
             "git -C %s add tracked.txt",
             g_tmpdir, g_tmpdir, g_tmpdir, g_tmpdir, g_tmpdir);
    CHECK(system(command) == 0);
    snprintf(command, sizeof(command), "printf two > %s/tracked.txt", g_tmpdir);
    CHECK(system(command) == 0);

    ToolResult r = {0};
    run_tool(&git_diff_tool, "{}", &r);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "tracked.txt") != NULL);
    free(r.content);

    r.content = NULL;
    run_tool(&git_diff_tool, "{\"path\":\"../outside\"}", &r);
    CHECK(r.is_error);
    CHECK(strstr(r.content, "inside the repository") != NULL);
    free(r.content);

    r.content = NULL;
    run_tool(&git_revert_tool, "{\"target\":\"HEAD;rm -rf /\"}", &r);
    CHECK(r.is_error);
    CHECK(strstr(r.content, "target must be") != NULL);
    free(r.content);

    CHECK(git_commit_tool.flags & TOOL_FLAG_APPROVAL_REQUIRED);
    CHECK(git_revert_tool.flags & TOOL_FLAG_APPROVAL_REQUIRED);
    return g_failures;
}

static int test_bash_bad_json(void) {
    ToolResult r = {0};
    run_tool(&bash_tool, "not json", &r);
    CHECK(r.is_error);
    free(r.content);
    return g_failures;
}

int main(void) {
    g_failures = 0;

    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/cagent-tools-test-XXXXXX");
    char* dir = mkdtemp(g_tmpdir);
    CHECK(dir != NULL);

    g_failures += test_read_nonexistent();
    g_failures += test_read_line_numbers_and_offset();
    g_failures += test_read_binary();
    g_failures += test_write_create_and_overwrite();
    g_failures += test_approval_previews();
    g_failures += test_navigation_tools();
    g_failures += test_bash_ok();
    g_failures += test_bash_nonzero_exit();
    g_failures += test_bash_stderr();
    g_failures += test_bash_timeout();
    g_failures += test_bash_output_cap();
    g_failures += test_bash_bad_json();
    g_failures += test_bench_tool();
    g_failures += test_diagnose_tool();
    g_failures += test_memory_tool();
    g_failures += test_plan_tool();
    g_failures += test_structured_test_tool();
    g_failures += test_git_checkpoint_tools();
    g_failures += test_plan_summary();
    g_failures += test_git_tools();

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmpdir);
    int rc = system(cmd);
    (void)rc;

    if (g_failures == 0) {
        printf("test_tools: all tests passed\n");
        return 0;
    }
    printf("test_tools: %d test(s) failed\n", g_failures);
    return 1;
}
