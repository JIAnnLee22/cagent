/*
 * tests/test_json.c — JSON wrapper unit tests (parse + build round-trip).
 */

#include <string.h>

#include "test_common.h"
#include "util/json.h"
#include "util/string.h"

static const char* SAMPLE = "{"
                            "  \"model\": \"gpt-test\","
                            "  \"stream\": true,"
                            "  \"max_tokens\": 4096,"
                            "  \"messages\": ["
                            "    {\"role\": \"user\", \"content\": \"hello\"},"
                            "    {\"role\": \"assistant\", \"content\": null,"
                            "     \"tool_calls\": [{\"id\": \"call_1\", \"function\": {\"name\": "
                            "\"read\", \"arguments\": \"{\\\"path\\\": \\\"a.c\\\"}\"}}]}"
                            "  ],"
                            "  \"n\": null,"
                            "  \"pi\": 3.25,"
                            "  \"ok\": false"
                            "}";

static int test_parse_access(void) {
    JsonDoc* doc = json_parse(SAMPLE, strlen(SAMPLE));
    CHECK(doc != NULL);

    JsonVal* root = json_root(doc);
    CHECK(root != NULL);
    CHECK(json_val_is_obj(root));

    CHECK(strcmp(json_obj_get_str(root, "model"), "gpt-test") == 0);
    CHECK(json_obj_get_str(root, "missing") == NULL);
    CHECK(json_obj_get_bool(root, "stream", false));
    CHECK(json_obj_get_int(root, "max_tokens", 0) == 4096);
    CHECK(json_obj_get_int(root, "missing", -1) == -1);

    /* null value is present but not a string */
    JsonVal* n = json_val_obj_get(root, "n");
    CHECK(n != NULL);
    CHECK(json_val_is_null(n));
    CHECK(json_val_str(n) == NULL);

    CHECK(json_val_real(json_val_obj_get(root, "pi")) == 3.25);
    CHECK(!json_obj_get_bool(root, "ok", true));

    /* messages array */
    JsonVal* msgs = json_val_obj_get(root, "messages");
    CHECK(msgs != NULL);
    CHECK(json_val_is_arr(msgs));
    CHECK(json_val_arr_size(msgs) == 2);

    JsonVal* m0 = json_val_arr_get(msgs, 0);
    CHECK(m0 != NULL);
    CHECK(strcmp(json_obj_get_str(m0, "role"), "user") == 0);
    CHECK(strcmp(json_obj_get_str(m0, "content"), "hello") == 0);

    JsonVal* m1 = json_val_arr_get(msgs, 1);
    CHECK(m1 != NULL);
    CHECK(json_val_is_null(json_val_obj_get(m1, "content")));

    /* nested tool_calls -> function -> arguments */
    JsonVal* calls = json_val_obj_get(m1, "tool_calls");
    CHECK(calls != NULL);
    CHECK(json_val_arr_size(calls) == 1);
    JsonVal* call0 = json_val_arr_get(calls, 0);
    CHECK(call0 != NULL);
    CHECK(strcmp(json_obj_get_str(call0, "id"), "call_1") == 0);
    JsonVal* fn = json_val_obj_get(call0, "function");
    CHECK(fn != NULL);
    CHECK(strcmp(json_obj_get_str(fn, "name"), "read") == 0);
    CHECK(strcmp(json_obj_get_str(fn, "arguments"), "{\"path\": \"a.c\"}") == 0);

    /* out-of-range array access */
    CHECK(json_val_arr_get(msgs, 2) == NULL);

    json_doc_free(doc);
    return g_failures;
}

static int test_parse_invalid(void) {
    CHECK(json_parse("{ not json", 10) == NULL);
    CHECK(json_parse("", 0) == NULL);
    CHECK(json_parse(NULL, 0) == NULL);
    return g_failures;
}

static int test_builder_roundtrip(void) {
    JsonBuilder* b = json_builder_new();
    CHECK(b != NULL);

    JsonMut* root = json_builder_root_obj(b);
    CHECK(root != NULL);
    CHECK(json_builder_obj_add_str(b, root, "model", "gpt-test") == AGENT_OK);
    CHECK(json_builder_obj_add_int(b, root, "max_tokens", 4096) == AGENT_OK);
    CHECK(json_builder_obj_add_bool(b, root, "stream", true) == AGENT_OK);
    CHECK(json_builder_obj_add_null(b, root, "extra") == AGENT_OK);

    JsonMut* msgs = json_builder_obj_add_arr(b, root, "messages");
    CHECK(msgs != NULL);
    JsonMut* u = json_builder_arr_add_obj(b, msgs);
    CHECK(u != NULL);
    CHECK(json_builder_obj_add_str(b, u, "role", "user") == AGENT_OK);
    CHECK(json_builder_obj_add_str(b, u, "content", "hello world") == AGENT_OK);
    JsonMut* a = json_builder_arr_add_obj(b, msgs);
    CHECK(a != NULL);
    CHECK(json_builder_obj_add_str(b, a, "role", "assistant") == AGENT_OK);

    String out = string_new();
    CHECK(json_builder_stringify(b, &out) == AGENT_OK);
    CHECK(out.len > 0);

    /* parse the produced JSON back and verify */
    JsonDoc* doc = json_parse(out.data, out.len);
    CHECK(doc != NULL);
    JsonVal* root2 = json_root(doc);
    CHECK(root2 != NULL);
    CHECK(strcmp(json_obj_get_str(root2, "model"), "gpt-test") == 0);
    CHECK(json_obj_get_int(root2, "max_tokens", 0) == 4096);
    CHECK(json_obj_get_bool(root2, "stream", false));
    CHECK(json_val_is_null(json_val_obj_get(root2, "extra")));
    JsonVal* msgs2 = json_val_obj_get(root2, "messages");
    CHECK(msgs2 != NULL && json_val_arr_size(msgs2) == 2);
    JsonVal* u2 = json_val_arr_get(msgs2, 0);
    CHECK(strcmp(json_obj_get_str(u2, "content"), "hello world") == 0);

    string_free(&out);
    json_doc_free(doc);
    json_builder_free(b);
    return g_failures;
}

static int test_builder_pretty_stringify(void) {
    JsonBuilder* b = json_builder_new();
    CHECK(b != NULL);
    JsonMut* root = json_builder_root_obj(b);
    CHECK(root != NULL);
    CHECK(json_builder_obj_add_str(b, root, "model", "gpt-test") == AGENT_OK);
    JsonMut* nested = json_builder_obj_add_obj(b, root, "settings");
    CHECK(nested != NULL);
    CHECK(json_builder_obj_add_bool(b, nested, "stream", true) == AGENT_OK);

    String out = string_new();
    CHECK(json_builder_stringify_pretty(b, &out) == AGENT_OK);
    CHECK(strchr(out.data, '\n') != NULL);
    CHECK(strstr(out.data, "\"settings\"") != NULL);

    JsonDoc* doc = json_parse(out.data, out.len);
    CHECK(doc != NULL);
    CHECK(json_val_is_obj(json_root(doc)));
    CHECK(strcmp(json_obj_get_str(json_root(doc), "model"), "gpt-test") == 0);

    json_doc_free(doc);
    string_free(&out);
    json_builder_free(b);
    return g_failures;
}

static int test_builder_reset_and_nested_arrays(void) {
    JsonBuilder* b = json_builder_new();
    CHECK(b != NULL);

    /* round 1: root array with nested objects */
    JsonMut* arr = json_builder_root_arr(b);
    CHECK(arr != NULL);
    JsonMut* item = json_builder_arr_add_obj(b, arr);
    CHECK(item != NULL);
    CHECK(json_builder_obj_add_str(b, item, "k", "v1") == AGENT_OK);
    String s1 = string_new();
    CHECK(json_builder_stringify(b, &s1) == AGENT_OK);
    CHECK(strcmp(s1.data, "[{\"k\":\"v1\"}]") == 0);

    /* reset and reuse */
    json_builder_reset(b);
    JsonMut* root = json_builder_root_obj(b);
    CHECK(root != NULL);
    CHECK(json_builder_obj_add_str(b, root, "k", "v2") == AGENT_OK);
    String s2 = string_new();
    CHECK(json_builder_stringify(b, &s2) == AGENT_OK);
    CHECK(strcmp(s2.data, "{\"k\":\"v2\"}") == 0);

    /* nested array in object */
    json_builder_reset(b);
    JsonMut* r2 = json_builder_root_obj(b);
    CHECK(r2 != NULL);
    JsonMut* nums = json_builder_obj_add_arr(b, r2, "nums");
    CHECK(nums != NULL);
    CHECK(json_builder_arr_add_int(b, nums, 1) == AGENT_OK);
    CHECK(json_builder_arr_add_int(b, nums, 2) == AGENT_OK);
    CHECK(json_builder_arr_add_bool(b, nums, true) == AGENT_OK);
    String s3 = string_new();
    CHECK(json_builder_stringify(b, &s3) == AGENT_OK);
    CHECK(strcmp(s3.data, "{\"nums\":[1,2,true]}") == 0);

    string_free(&s1);
    string_free(&s2);
    string_free(&s3);
    json_builder_free(b);
    return g_failures;
}

int main(void) {
    g_failures = 0;
    g_failures += test_parse_access();
    g_failures += test_parse_invalid();
    g_failures += test_builder_roundtrip();
    g_failures += test_builder_pretty_stringify();
    g_failures += test_builder_reset_and_nested_arrays();

    if (g_failures == 0) {
        printf("test_json: all tests passed\n");
        return 0;
    }
    printf("test_json: %d test(s) failed\n", g_failures);
    return 1;
}
