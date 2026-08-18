/*
 * tests/test_tool_registry.c — ToolRegistry unit tests.
 */

#include <string.h>

#include "test_common.h"
#include "tool/registry.h"
#include "tool/tool.h"
#include "util/error.h"
#include "util/json.h"

static int dummy_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    (void)ctx;
    (void)arguments;
    (void)result;
    return AGENT_OK;
}

static Tool read_tool = {
    .name = "read",
    .description = "Read a file",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}",
    .flags = TOOL_FLAG_NONE,
    .execute = dummy_execute,
};

static Tool write_tool = {
    .name = "write",
    .description = "Write a file",
    .input_schema = "{\"type\":\"object\"}",
    .flags = TOOL_FLAG_NONE,
    .execute = dummy_execute,
};

static int test_register_find(void) {
    ToolRegistry* reg = tool_registry_new();
    CHECK(reg != NULL);

    CHECK(tool_registry_register(reg, &read_tool) == AGENT_OK);
    CHECK(tool_registry_register(reg, &write_tool) == AGENT_OK);
    CHECK(tool_registry_count(reg) == 2);

    Tool* t = tool_registry_find(reg, "read");
    CHECK(t == &read_tool);
    CHECK(strcmp(t->name, "read") == 0);
    CHECK(tool_registry_find(reg, "write") == &write_tool);
    CHECK(tool_registry_find(reg, "nope") == NULL);

    /* duplicate registration is rejected */
    CHECK(tool_registry_register(reg, &read_tool) != AGENT_OK);
    CHECK(tool_registry_count(reg) == 2);

    tool_registry_free(reg);
    return g_failures;
}

static int test_enable_disable(void) {
    ToolRegistry* reg = tool_registry_new();
    CHECK(reg != NULL);
    CHECK(tool_registry_register(reg, &read_tool) == AGENT_OK);

    CHECK(tool_registry_is_enabled(reg, "read"));
    tool_registry_set_enabled(reg, "read", false);
    CHECK(!tool_registry_is_enabled(reg, "read"));
    CHECK(tool_registry_find(reg, "read") == NULL); /* disabled = hidden */
    CHECK(tool_registry_count(reg) == 1);

    tool_registry_set_enabled(reg, "read", true);
    CHECK(tool_registry_find(reg, "read") == &read_tool);

    /* unknown name: no-op */
    tool_registry_set_enabled(reg, "missing", false);
    CHECK(tool_registry_is_enabled(reg, "read"));

    tool_registry_free(reg);
    return g_failures;
}

static int test_schema_json(void) {
    ToolRegistry* reg = tool_registry_new();
    CHECK(reg != NULL);
    CHECK(tool_registry_register(reg, &read_tool) == AGENT_OK);
    CHECK(tool_registry_register(reg, &write_tool) == AGENT_OK);

    String out = string_new();
    CHECK(tool_registry_schema_json(reg, &out) == AGENT_OK);
    size_t schema_bytes = 0;
    CHECK(tool_registry_schema_bytes(reg, &schema_bytes) == AGENT_OK);
    CHECK(schema_bytes == out.len);

    /* parse and verify the structure */
    JsonDoc* doc = json_parse(out.data, out.len);
    CHECK(doc != NULL);
    JsonVal* arr = json_root(doc);
    CHECK(arr != NULL && json_val_is_arr(arr));
    CHECK(json_val_arr_size(arr) == 2);

    JsonVal* t0 = json_val_arr_get(arr, 0);
    CHECK(strcmp(json_obj_get_str(t0, "type"), "function") == 0);
    JsonVal* fn0 = json_val_obj_get(t0, "function");
    CHECK(fn0 != NULL);
    CHECK(strcmp(json_obj_get_str(fn0, "name"), "read") == 0);
    CHECK(strcmp(json_obj_get_str(fn0, "description"), "Read a file") == 0);
    JsonVal* params = json_val_obj_get(fn0, "parameters");
    CHECK(params != NULL && json_val_is_obj(params));
    CHECK(strcmp(json_obj_get_str(params, "type"), "object") == 0);

    JsonVal* t1 = json_val_arr_get(arr, 1);
    CHECK(strcmp(json_obj_get_str(json_val_obj_get(t1, "function"), "name"), "write") == 0);

    /* disabled tools are excluded from the schema */
    tool_registry_set_enabled(reg, "read", false);
    String out2 = string_new();
    CHECK(tool_registry_schema_json(reg, &out2) == AGENT_OK);
    JsonDoc* doc2 = json_parse(out2.data, out2.len);
    JsonVal* arr2 = json_root(doc2);
    CHECK(json_val_arr_size(arr2) == 1);
    size_t reduced_bytes = 0;
    CHECK(tool_registry_schema_bytes(reg, &reduced_bytes) == AGENT_OK);
    CHECK(reduced_bytes < schema_bytes);
    JsonVal* only = json_val_arr_get(arr2, 0);
    CHECK(strcmp(json_obj_get_str(json_val_obj_get(only, "function"), "name"), "write") == 0);

    json_doc_free(doc2);
    json_doc_free(doc);
    string_free(&out2);
    string_free(&out);
    tool_registry_free(reg);
    return g_failures;
}

static int test_registry_growth(void) {
    /* force several capacity doublings (min cap is 8) */
    ToolRegistry* reg = tool_registry_new();
    CHECK(reg != NULL);

    Tool tools[40];
    char name[16];
    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof(name), "tool-%02d", i);
        tools[i] = (Tool){.name = name,
                          .description = "growth test",
                          .input_schema = "{\"type\":\"object\"}",
                          .flags = TOOL_FLAG_NONE,
                          .execute = dummy_execute};
        CHECK(tool_registry_register(reg, &tools[i]) == AGENT_OK);
    }
    CHECK(tool_registry_count(reg) == 40);
    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof(name), "tool-%02d", i);
        CHECK(tool_registry_find(reg, name) == &tools[i]);
    }

    tool_registry_free(reg);
    return g_failures;
}

static int test_empty_registry_schema(void) {
    ToolRegistry* reg = tool_registry_new();
    CHECK(reg != NULL);

    String out = string_new();
    CHECK(tool_registry_schema_json(reg, &out) == AGENT_OK);
    CHECK(strcmp(out.data, "[]") == 0);

    string_free(&out);
    tool_registry_free(reg);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_register_find();
    g_failures += test_enable_disable();
    g_failures += test_schema_json();
    g_failures += test_empty_registry_schema();

    if (g_failures == 0) {
        printf("test_tool_registry: all tests passed\n");
        return 0;
    }
    printf("test_tool_registry: %d test(s) failed\n", g_failures);
    return 1;
}
