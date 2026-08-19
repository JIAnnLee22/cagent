/*
 * tests/test_grep.c — grep tool tests (ripgrep preferred).
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_common.h"
#include "tool/tool.h"
#include "util/error.h"

extern Tool grep_tool;

static char g_tmpdir[256];

static int test_grep_finds_matches(void) {
    /* seed files */
    char path[600];
    snprintf(path, sizeof(path), "%s/a.txt", g_tmpdir);
    FILE* f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("hello world\n", f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/b.txt", g_tmpdir);
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("nothing here\n", f);
        fclose(f);
    }

    ToolResult r = {0};
    r.content = NULL;
    ToolContext ctx = {0};
    ctx.cwd = g_tmpdir;
    CHECK(grep_tool.execute(&ctx, "{\"pattern\":\"hello\"}", &r) == AGENT_OK);
    CHECK(r.content != NULL);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "a.txt") != NULL);
    CHECK(strstr(r.content, "hello world") != NULL);
    CHECK(strstr(r.content, "b.txt") == NULL);
    free(r.content);
    return g_failures;
}

static int test_grep_no_matches(void) {
    ToolResult r = {0};
    r.content = NULL;
    ToolContext ctx = {0};
    ctx.cwd = g_tmpdir;
    CHECK(grep_tool.execute(&ctx, "{\"pattern\":\"zzz_no_such_zzz\"}", &r) == AGENT_OK);
    CHECK(!r.is_error);
    CHECK(strstr(r.content, "no matches") != NULL);
    free(r.content);
    return g_failures;
}

static int test_grep_missing_pattern(void) {
    ToolResult r = {0};
    r.content = NULL;
    ToolContext ctx = {0};
    ctx.cwd = g_tmpdir;
    CHECK(grep_tool.execute(&ctx, "{\"path\":\".\"}", &r) == AGENT_OK);
    CHECK(r.is_error);
    free(r.content);
    return g_failures;
}

static int test_grep_workspace_path_policy(void) {
    char outside[512];
    char link_path[512];
    snprintf(outside, sizeof(outside), "/tmp/cagent-grep-outside-%ld.txt", (long)getpid());
    snprintf(link_path, sizeof(link_path), "%s/escape.txt", g_tmpdir);

    FILE* f = fopen(outside, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("outside secret\n", f);
        fclose(f);
    }
    CHECK(symlink(outside, link_path) == 0);

    ToolContext ctx = {0};
    ctx.cwd = g_tmpdir;
    ToolResult r = {0};
    CHECK(grep_tool.execute(&ctx, "{\"pattern\":\"outside\",\"path\":\"/tmp\"}", &r) ==
          AGENT_OK);
    CHECK(r.is_error);
    CHECK(strstr(r.content, "workspace") != NULL);
    free(r.content);

    r = (ToolResult){0};
    CHECK(grep_tool.execute(&ctx,
                            "{\"pattern\":\"outside\",\"path\":\"../cagent-grep-outside-placeholder.txt\"}",
                            &r) == AGENT_OK);
    CHECK(r.is_error);
    CHECK(strstr(r.content, "workspace") != NULL);
    free(r.content);

    r = (ToolResult){0};
    CHECK(grep_tool.execute(&ctx, "{\"pattern\":\"outside\",\"path\":\"escape.txt\"}", &r) ==
          AGENT_OK);
    CHECK(r.is_error);
    CHECK(strstr(r.content, "workspace") != NULL);
    free(r.content);

    unlink(link_path);
    unlink(outside);
    return g_failures;
}

static int test_grep_max_results(void) {
    /* many matching lines, max_results small */
    char path[600];
    snprintf(path, sizeof(path), "%s/many.txt", g_tmpdir);
    FILE* f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        for (int i = 0; i < 200; i++) {
            fprintf(f, "match line %d\n", i);
        }
        fclose(f);
    }

    ToolResult r = {0};
    r.content = NULL;
    ToolContext ctx = {0};
    ctx.cwd = g_tmpdir;
    CHECK(grep_tool.execute(&ctx, "{\"pattern\":\"match line\",\"max_results\":5}", &r) ==
          AGENT_OK);
    CHECK(!r.is_error);
    /* rg -m 5 caps per file, so at most 5 lines appear */
    int count = 0;
    const char* p = r.content;
    while ((p = strstr(p, "match line")) != NULL) {
        count++;
        p += 10;
    }
    CHECK(count <= 5);
    free(r.content);
    return g_failures;
}

int main(void) {
    g_failures = 0;

    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/cagent-grep-test-XXXXXX");
    char* dir = mkdtemp(g_tmpdir);
    CHECK(dir != NULL);

    g_failures += test_grep_finds_matches();
    g_failures += test_grep_no_matches();
    g_failures += test_grep_missing_pattern();
    g_failures += test_grep_workspace_path_policy();
    g_failures += test_grep_max_results();

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmpdir);
    int rc = system(cmd);
    (void)rc;

    if (g_failures == 0) {
        printf("test_grep: all tests passed\n");
        return 0;
    }
    printf("test_grep: %d test(s) failed\n", g_failures);
    return 1;
}
