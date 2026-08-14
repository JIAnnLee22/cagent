/*
 * test_smoke.c — minimal smoke test to verify the CTest pipeline.
 * Every module gets its own test file under tests/ as the project grows.
 */

#include <stdio.h>
#include <string.h>

#include "yyjson.h"

static int test_yyjson_links(void) {
    yyjson_doc* doc = yyjson_read("{ \"ok\": true }", 14, 0);
    if (doc == NULL) {
        fprintf(stderr, "FAIL: yyjson could not parse a trivial document\n");
        return 1;
    }

    yyjson_val* root = yyjson_doc_get_root(doc);
    yyjson_val* ok = yyjson_obj_get(root, "ok");
    int pass = (ok != NULL && yyjson_get_bool(ok));

    yyjson_doc_free(doc);
    return pass ? 0 : 1;
}

int main(void) {
    int failures = 0;

    failures += test_yyjson_links();

    if (failures == 0) {
        printf("smoke: all tests passed\n");
        return 0;
    }
    printf("smoke: %d test(s) failed\n", failures);
    return 1;
}
