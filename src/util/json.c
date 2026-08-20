/*
 * util/json.c — yyjson-backed JSON wrapper.
 *
 * JsonVal maps to yyjson_val, JsonMut to yyjson_mut_val, JsonBuilder
 * wraps a yyjson_mut_doc. All validation happens on the yyjson side;
 * this layer only adapts the API surface.
 */

#include <stdlib.h>

#include "util/json.h"
#include "yyjson.h"

/* ---- parsing / access ------------------------------------------------ */

struct JsonDoc {
    yyjson_doc* doc;
};

JsonDoc* json_parse(const char* data, size_t len) {
    if (data == NULL) {
        return NULL;
    }
    yyjson_doc* doc = yyjson_read(data, len, 0);
    if (doc == NULL) {
        return NULL;
    }
    JsonDoc* jd = malloc(sizeof(JsonDoc));
    if (jd == NULL) {
        yyjson_doc_free(doc);
        return NULL;
    }
    jd->doc = doc;
    return jd;
}

void json_doc_free(JsonDoc* jd) {
    if (jd == NULL) {
        return;
    }
    yyjson_doc_free(jd->doc);
    free(jd);
}

JsonVal* json_root(const JsonDoc* jd) {
    if (jd == NULL) {
        return NULL;
    }
    return (JsonVal*)yyjson_doc_get_root(jd->doc);
}

bool json_val_is_null(const JsonVal* v) {
    return v != NULL && yyjson_is_null((yyjson_val*)v);
}

bool json_val_is_bool(const JsonVal* v) {
    return v != NULL && yyjson_is_bool((yyjson_val*)v);
}

bool json_val_is_int(const JsonVal* v) {
    return v != NULL && yyjson_is_int((yyjson_val*)v);
}

bool json_val_is_real(const JsonVal* v) {
    return v != NULL && yyjson_is_real((yyjson_val*)v);
}

bool json_val_is_str(const JsonVal* v) {
    return v != NULL && yyjson_is_str((yyjson_val*)v);
}

bool json_val_is_obj(const JsonVal* v) {
    return v != NULL && yyjson_is_obj((yyjson_val*)v);
}

bool json_val_is_arr(const JsonVal* v) {
    return v != NULL && yyjson_is_arr((yyjson_val*)v);
}

const char* json_val_str(const JsonVal* v) {
    if (v == NULL || !yyjson_is_str((yyjson_val*)v)) {
        return NULL;
    }
    return yyjson_get_str((yyjson_val*)v);
}

int64_t json_val_int(const JsonVal* v) {
    if (v == NULL) {
        return 0;
    }
    const yyjson_val* val = (const yyjson_val*)v;
    if (yyjson_is_int((yyjson_val*)val)) {
        return yyjson_get_sint((yyjson_val*)val);
    }
    if (yyjson_is_real((yyjson_val*)val)) {
        return (int64_t)yyjson_get_real((yyjson_val*)val);
    }
    return 0;
}

double json_val_real(const JsonVal* v) {
    if (v == NULL) {
        return 0.0;
    }
    const yyjson_val* val = (const yyjson_val*)v;
    if (yyjson_is_real((yyjson_val*)val)) {
        return yyjson_get_real((yyjson_val*)val);
    }
    if (yyjson_is_int((yyjson_val*)val)) {
        return (double)yyjson_get_sint((yyjson_val*)val);
    }
    return 0.0;
}

bool json_val_bool(const JsonVal* v) {
    if (v == NULL || !yyjson_is_bool((yyjson_val*)v)) {
        return false;
    }
    return yyjson_get_bool((yyjson_val*)v);
}

JsonVal* json_val_obj_get(const JsonVal* obj, const char* key) {
    if (obj == NULL || key == NULL || !yyjson_is_obj((yyjson_val*)obj)) {
        return NULL;
    }
    return (JsonVal*)yyjson_obj_get((yyjson_val*)obj, key);
}

size_t json_val_arr_size(const JsonVal* arr) {
    if (arr == NULL || !yyjson_is_arr((yyjson_val*)arr)) {
        return 0;
    }
    return yyjson_arr_size((yyjson_val*)arr);
}

JsonVal* json_val_arr_get(const JsonVal* arr, size_t index) {
    if (arr == NULL || !yyjson_is_arr((yyjson_val*)arr)) {
        return NULL;
    }
    return (JsonVal*)yyjson_arr_get((yyjson_val*)arr, index);
}

const char* json_obj_get_str(const JsonVal* obj, const char* key) {
    JsonVal* v = json_val_obj_get(obj, key);
    return json_val_str(v);
}

int64_t json_obj_get_int(const JsonVal* obj, const char* key, int64_t dflt) {
    JsonVal* v = json_val_obj_get(obj, key);
    if (v == NULL) {
        return dflt;
    }
    if (yyjson_is_int((yyjson_val*)v)) {
        return yyjson_get_sint((yyjson_val*)v);
    }
    if (yyjson_is_real((yyjson_val*)v)) {
        return (int64_t)yyjson_get_real((yyjson_val*)v);
    }
    return dflt;
}

double json_obj_get_num(const JsonVal* obj, const char* key, double dflt) {
    JsonVal* v = json_val_obj_get(obj, key);
    if (v == NULL || !yyjson_is_num((yyjson_val*)v)) {
        return dflt;
    }
    return yyjson_get_num((yyjson_val*)v);
}

bool json_obj_get_bool(const JsonVal* obj, const char* key, bool dflt) {
    JsonVal* v = json_val_obj_get(obj, key);
    if (v == NULL || !yyjson_is_bool((yyjson_val*)v)) {
        return dflt;
    }
    return yyjson_get_bool((yyjson_val*)v);
}

int json_obj_foreach(const JsonVal* obj, JsonObjEachCb cb, void* userdata) {
    if (obj == NULL || cb == NULL || !yyjson_is_obj((yyjson_val*)obj)) {
        return AGENT_ERR_JSON;
    }
    yyjson_obj_iter iter = yyjson_obj_iter_with((yyjson_val*)obj);
    yyjson_val* key;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        yyjson_val* value = yyjson_obj_iter_get_val(key);
        int rc = cb(yyjson_get_str(key), (const JsonVal*)value, userdata);
        if (rc != AGENT_OK) {
            return rc;
        }
    }
    return AGENT_OK;
}

/* ---- building -------------------------------------------------------- */

struct JsonBuilder {
    yyjson_mut_doc* doc;
    yyjson_mut_val* root;
};

JsonBuilder* json_builder_new(void) {
    JsonBuilder* b = calloc(1, sizeof(JsonBuilder));
    if (b == NULL) {
        return NULL;
    }
    b->doc = yyjson_mut_doc_new(NULL);
    if (b->doc == NULL) {
        free(b);
        return NULL;
    }
    return b;
}

void json_builder_free(JsonBuilder* b) {
    if (b == NULL) {
        return;
    }
    yyjson_mut_doc_free(b->doc);
    free(b);
}

JsonMut* json_builder_root_obj(JsonBuilder* b) {
    if (b == NULL) {
        return NULL;
    }
    b->root = yyjson_mut_obj(b->doc);
    if (b->root != NULL) {
        yyjson_mut_doc_set_root(b->doc, b->root);
    }
    return (JsonMut*)b->root;
}

JsonMut* json_builder_root_arr(JsonBuilder* b) {
    if (b == NULL) {
        return NULL;
    }
    b->root = yyjson_mut_arr(b->doc);
    if (b->root != NULL) {
        yyjson_mut_doc_set_root(b->doc, b->root);
    }
    return (JsonMut*)b->root;
}

JsonMut* json_builder_obj_get(JsonBuilder* b, JsonMut* obj, const char* key) {
    (void)b;
    if (obj == NULL || key == NULL) {
        return NULL;
    }
    return (JsonMut*)yyjson_mut_obj_get((yyjson_mut_val*)obj, key);
}

const char* json_builder_obj_get_str(JsonBuilder* b, JsonMut* obj, const char* key) {
    JsonMut* v = json_builder_obj_get(b, obj, key);
    if (v == NULL) {
        return NULL;
    }
    return yyjson_mut_get_str((yyjson_mut_val*)v);
}

size_t json_builder_arr_size(JsonBuilder* b, JsonMut* arr) {
    (void)b;
    if (arr == NULL) {
        return 0;
    }
    return yyjson_mut_arr_size((yyjson_mut_val*)arr);
}

JsonMut* json_builder_arr_get(JsonBuilder* b, JsonMut* arr, size_t index) {
    (void)b;
    if (arr == NULL) {
        return NULL;
    }
    return (JsonMut*)yyjson_mut_arr_get((yyjson_mut_val*)arr, index);
}

JsonMut* json_builder_obj_add_obj(JsonBuilder* b, JsonMut* obj, const char* key) {
    if (b == NULL || obj == NULL || key == NULL) {
        return NULL;
    }
    return (JsonMut*)yyjson_mut_obj_add_obj(b->doc, (yyjson_mut_val*)obj, key);
}

JsonMut* json_builder_obj_add_arr(JsonBuilder* b, JsonMut* obj, const char* key) {
    if (b == NULL || obj == NULL || key == NULL) {
        return NULL;
    }
    return (JsonMut*)yyjson_mut_obj_add_arr(b->doc, (yyjson_mut_val*)obj, key);
}

/* NOTE: yyjson_mut_obj_add_str BORROWS key/value (they must outlive the
 * document). We always copy, so callers may free their strings right
 * after the call (DESIGN.md §35 ownership contract). */
int json_builder_obj_add_str(JsonBuilder* b, JsonMut* obj, const char* key, const char* value) {
    if (b == NULL || obj == NULL || key == NULL || value == NULL) {
        return AGENT_ERR_OOM;
    }
    /* the key is a caller string (our callers always pass literals);
     * the value is copied so callers may free it right after */
    yyjson_mut_val* v = yyjson_mut_strcpy(b->doc, value);
    if (v == NULL) {
        return AGENT_ERR_OOM;
    }
    if (yyjson_mut_obj_add_val(b->doc, (yyjson_mut_val*)obj, key, v)) {
        return AGENT_OK;
    }
    return AGENT_ERR_OOM;
}

int json_builder_obj_add_int(JsonBuilder* b, JsonMut* obj, const char* key, int64_t value) {
    if (b == NULL || obj == NULL || key == NULL) {
        return AGENT_ERR_OOM;
    }
    if (yyjson_mut_obj_add_int(b->doc, (yyjson_mut_val*)obj, key, value)) {
        return AGENT_OK;
    }
    return AGENT_ERR_OOM;
}

int json_builder_obj_add_real(JsonBuilder* b, JsonMut* obj, const char* key, double value) {
    if (b == NULL || obj == NULL || key == NULL) {
        return AGENT_ERR_OOM;
    }
    if (yyjson_mut_obj_add_real(b->doc, (yyjson_mut_val*)obj, key, value)) {
        return AGENT_OK;
    }
    return AGENT_ERR_OOM;
}

int json_builder_obj_add_bool(JsonBuilder* b, JsonMut* obj, const char* key, bool value) {
    if (b == NULL || obj == NULL || key == NULL) {
        return AGENT_ERR_OOM;
    }
    if (yyjson_mut_obj_add_bool(b->doc, (yyjson_mut_val*)obj, key, value)) {
        return AGENT_OK;
    }
    return AGENT_ERR_OOM;
}

int json_builder_obj_add_null(JsonBuilder* b, JsonMut* obj, const char* key) {
    if (b == NULL || obj == NULL || key == NULL) {
        return AGENT_ERR_OOM;
    }
    if (yyjson_mut_obj_add_null(b->doc, (yyjson_mut_val*)obj, key)) {
        return AGENT_OK;
    }
    return AGENT_ERR_OOM;
}

int json_builder_obj_add_val_copy(JsonBuilder* b, JsonMut* obj, const char* key,
                                  const JsonVal* src) {
    if (b == NULL || obj == NULL || key == NULL || src == NULL) {
        return AGENT_ERR_OOM;
    }
    yyjson_mut_val* copy = yyjson_val_mut_copy(b->doc, (yyjson_val*)src);
    if (copy == NULL) {
        return AGENT_ERR_OOM;
    }
    if (yyjson_mut_obj_add_val(b->doc, (yyjson_mut_val*)obj, key, copy)) {
        return AGENT_OK;
    }
    return AGENT_ERR_OOM;
}

JsonMut* json_builder_arr_add_obj(JsonBuilder* b, JsonMut* arr) {
    if (b == NULL || arr == NULL) {
        return NULL;
    }
    return (JsonMut*)yyjson_mut_arr_add_obj(b->doc, (yyjson_mut_val*)arr);
}

JsonMut* json_builder_arr_add_arr(JsonBuilder* b, JsonMut* arr) {
    if (b == NULL || arr == NULL) {
        return NULL;
    }
    return (JsonMut*)yyjson_mut_arr_add_arr(b->doc, (yyjson_mut_val*)arr);
}

int json_builder_arr_add_str(JsonBuilder* b, JsonMut* arr, const char* value) {
    if (b == NULL || arr == NULL || value == NULL) {
        return AGENT_ERR_OOM;
    }
    yyjson_mut_val* v = yyjson_mut_strcpy(b->doc, value);
    if (v == NULL) {
        return AGENT_ERR_OOM;
    }
    if (yyjson_mut_arr_add_val((yyjson_mut_val*)arr, v)) {
        return AGENT_OK;
    }
    return AGENT_ERR_OOM;
}

int json_builder_arr_add_int(JsonBuilder* b, JsonMut* arr, int64_t value) {
    if (b == NULL || arr == NULL) {
        return AGENT_ERR_OOM;
    }
    if (yyjson_mut_arr_add_int(b->doc, (yyjson_mut_val*)arr, value)) {
        return AGENT_OK;
    }
    return AGENT_ERR_OOM;
}

int json_builder_arr_add_bool(JsonBuilder* b, JsonMut* arr, bool value) {
    if (b == NULL || arr == NULL) {
        return AGENT_ERR_OOM;
    }
    if (yyjson_mut_arr_add_bool(b->doc, (yyjson_mut_val*)arr, value)) {
        return AGENT_OK;
    }
    return AGENT_ERR_OOM;
}

static int json_builder_stringify_flags(JsonBuilder* b, String* out, yyjson_write_flag flags) {
    if (b == NULL || out == NULL || b->root == NULL) {
        return AGENT_ERR_JSON;
    }

    size_t len = 0;
    const char* text = yyjson_mut_write(b->doc, flags, &len);
    if (text == NULL) {
        return AGENT_ERR_JSON;
    }

    int err = string_append_n(out, text, len);
    free((void*)text);
    return err;
}

int json_builder_stringify(JsonBuilder* b, String* out) {
    return json_builder_stringify_flags(b, out, 0);
}

int json_builder_stringify_pretty(JsonBuilder* b, String* out) {
    return json_builder_stringify_flags(b, out, YYJSON_WRITE_PRETTY);
}

void json_builder_reset(JsonBuilder* b) {
    if (b == NULL) {
        return;
    }
    yyjson_mut_doc_free(b->doc);
    b->doc = yyjson_mut_doc_new(NULL);
    b->root = NULL;
}
