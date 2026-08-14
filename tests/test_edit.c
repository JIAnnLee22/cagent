/*
 * tests/test_edit.c — edit tool tests (DESIGN.md §19).
 */

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_common.h"
#include "tool/tool.h"
#include "util/error.h"
#include "util/string.h"

extern Tool edit_tool;
extern Tool write_tool;

static char g_tmpdir[256];

static ToolContext make_ctx(void) {
    ToolContext ctx = {0};
    ctx.cwd = g_tmpdir;
    return ctx;
}

static void write_file(const char* name, const char* content) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", g_tmpdir, name);
    FILE* f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs(content, f);
        fclose(f);
    }
}

static char* read_file(const char* name) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", g_tmpdir, name);
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char* data = malloc((size_t)n + 1);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(data, 1, (size_t)n, f);
    fclose(f);
    data[got] = '\0';
    return data;
}

static void run_edit(const char* args, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    ToolContext ctx = make_ctx();
    CHECK(edit_tool.execute(&ctx, args, result) == AGENT_OK);
    CHECK(result->content != NULL);
}

static int test_edit_success(void) {
    write_file("a.c", "int main(void) {\n    return 0;\n}\n");
    char path[512];
    snprintf(path, sizeof(path), "%s/a.c", g_tmpdir);
    CHECK(chmod(path, 0600) == 0);
    ToolContext ctx = make_ctx();
    ToolResult preview = {0};
    CHECK(edit_tool.preview(&ctx,
                            "{\"path\":\"a.c\",\"old_text\":\"return 0;\","
                            "\"new_text\":\"return 42;\"}",
                            &preview) == AGENT_OK);
    CHECK(preview.content != NULL && strstr(preview.content, "-return 0;") != NULL);
    CHECK(preview.content != NULL && strstr(preview.content, "+return 42;") != NULL);
    free(preview.content);
    ToolResult r = {0};
    run_edit("{\"path\":\"a.c\",\"old_text\":\"return 0;\","
             "\"new_text\":\"return 42;\"}",
             &r);
    CHECK(!r.is_error);

    char* data = read_file("a.c");
    CHECK(data != NULL);
    CHECK(strstr(data, "return 42;") != NULL);
    CHECK(strstr(data, "return 0;") == NULL);
    struct stat st;
    CHECK(stat(path, &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    free(data);
    free(r.content);
    return g_failures;
}

static int test_edit_no_match(void) {
    write_file("b.c", "int x = 1;\n");
    ToolResult r = {0};
    run_edit("{\"path\":\"b.c\",\"old_text\":\"int y = 2;\","
             "\"new_text\":\"int z = 3;\"}",
             &r);
    CHECK(r.is_error);
    CHECK(strstr(r.content, "not found") != NULL);

    /* file unchanged */
    char* data = read_file("b.c");
    CHECK(data != NULL && strcmp(data, "int x = 1;\n") == 0);
    free(data);
    free(r.content);
    return g_failures;
}

static int test_edit_multiple_matches(void) {
    write_file("c.c", "f(1);\nf(2);\nf(3);\n");
    ToolResult r = {0};
    run_edit("{\"path\":\"c.c\",\"old_text\":\"f(\",\"new_text\":\"g(\"}", &r);
    CHECK(r.is_error);
    CHECK(strstr(r.content, "matches 3 times") != NULL);

    /* file unchanged */
    char* data = read_file("c.c");
    CHECK(data != NULL && strcmp(data, "f(1);\nf(2);\nf(3);\n") == 0);
    free(data);
    free(r.content);
    return g_failures;
}

static int test_edit_nonexistent_file(void) {
    ToolResult r = {0};
    run_edit("{\"path\":\"missing.c\",\"old_text\":\"x\",\"new_text\":\"y\"}", &r);
    CHECK(r.is_error);
    CHECK(strstr(r.content, "cannot stat") != NULL);
    free(r.content);
    return g_failures;
}

static int test_edit_empty_old_text(void) {
    ToolResult r = {0};
    run_edit("{\"path\":\"a.c\",\"old_text\":\"\",\"new_text\":\"y\"}", &r);
    CHECK(r.is_error);
    CHECK(strstr(r.content, "empty") != NULL);
    free(r.content);
    return g_failures;
}

static int test_edit_bad_json(void) {
    ToolResult r = {0};
    run_edit("not json", &r);
    CHECK(r.is_error);
    free(r.content);
    return g_failures;
}

int main(void) {
    g_failures = 0;

    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/cagent-edit-test-XXXXXX");
    char* dir = mkdtemp(g_tmpdir);
    CHECK(dir != NULL);

    g_failures += test_edit_success();
    g_failures += test_edit_no_match();
    g_failures += test_edit_multiple_matches();
    g_failures += test_edit_nonexistent_file();
    g_failures += test_edit_empty_old_text();
    g_failures += test_edit_bad_json();

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmpdir);
    int rc = system(cmd);
    (void)rc;

    if (g_failures == 0) {
        printf("test_edit: all tests passed\n");
        return 0;
    }
    printf("test_edit: %d test(s) failed\n", g_failures);
    return 1;
}
