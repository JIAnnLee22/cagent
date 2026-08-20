/*
 * util/json.h — internal JSON wrapper (yyjson underneath).
 *
 * Design rule (DESIGN.md §36): modules must not use the yyjson API
 * directly; everything goes through this header so the implementation
 * can be swapped later.
 *
 * Two value namespaces:
 *   - JsonVal  — immutable values inside a parsed JsonDoc (read access).
 *   - JsonMut  — mutable values created by a JsonBuilder (write access).
 * They are distinct C types; do not mix them.
 *
 * Ownership:
 *   - JsonDoc is owned by the caller; json_doc_free() frees doc + values.
 *   - json_parse() copies nothing; the JsonVal pointers are valid until
 *     json_doc_free(). data may be freed right after parsing.
 *   - All JsonVal accessors return borrowed pointers.
 *   - JsonBuilder owns its document; json_builder_stringify() appends the
 *     compact JSON text to *out (caller-owned String), while
 *     json_builder_stringify_pretty() uses human-friendly indentation;
 *     json_builder_free()
 *     releases everything. json_builder_reset() clears the document for
 *     reuse (previously returned JsonMut pointers become invalid).
 *   - json_builder_*_add_str() copies the string into the document.
 *
 * Parsing failures return NULL; the JSON is not validated beyond syntax.
 */

#ifndef CAGENT_UTIL_JSON_H
#define CAGENT_UTIL_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/string.h"

typedef struct JsonDoc JsonDoc;
typedef struct JsonVal JsonVal;
typedef struct JsonMut JsonMut;
typedef struct JsonBuilder JsonBuilder;

/* ---- parsing / access ------------------------------------------------ */

JsonDoc* json_parse(const char* data, size_t len);
void json_doc_free(JsonDoc* doc);
JsonVal* json_root(const JsonDoc* doc);

bool json_val_is_null(const JsonVal* v);
bool json_val_is_bool(const JsonVal* v);
bool json_val_is_int(const JsonVal* v);
bool json_val_is_real(const JsonVal* v);
bool json_val_is_str(const JsonVal* v);
bool json_val_is_obj(const JsonVal* v);
bool json_val_is_arr(const JsonVal* v);

/* Borrowed string value, NULL unless the value is a string. */
const char* json_val_str(const JsonVal* v);
int64_t json_val_int(const JsonVal* v);
double json_val_real(const JsonVal* v);
bool json_val_bool(const JsonVal* v);

/* Object member lookup; NULL when missing or v is not an object. */
JsonVal* json_val_obj_get(const JsonVal* obj, const char* key);

/* Array access; NULL when out of range or v is not an array. */
size_t json_val_arr_size(const JsonVal* arr);
JsonVal* json_val_arr_get(const JsonVal* arr, size_t index);

/* Convenience: string/int member with a default when missing. */
const char* json_obj_get_str(const JsonVal* obj, const char* key);
int64_t json_obj_get_int(const JsonVal* obj, const char* key, int64_t dflt);
double json_obj_get_num(const JsonVal* obj, const char* key, double dflt);
bool json_obj_get_bool(const JsonVal* obj, const char* key, bool dflt);
typedef int (*JsonObjEachCb)(const char* key, const JsonVal* value, void* userdata);
int json_obj_foreach(const JsonVal* obj, JsonObjEachCb cb, void* userdata);

/* ---- building -------------------------------------------------------- */

JsonBuilder* json_builder_new(void);
void json_builder_free(JsonBuilder* b);

/* Root must be an object or an array; returns the root value. */
JsonMut* json_builder_root_obj(JsonBuilder* b);
JsonMut* json_builder_root_arr(JsonBuilder* b);

/* Attach members to an object / items to an array. Returns the new child
 * for *_add_obj/_add_arr (NULL on OOM). int return: AGENT_OK/OOM. */
/* Borrowed member of a mutable object (NULL when absent). */
JsonMut* json_builder_obj_get(JsonBuilder* b, JsonMut* obj, const char* key);

/* Borrowed string member of a mutable object (NULL when absent/not str). */
const char* json_builder_obj_get_str(JsonBuilder* b, JsonMut* obj, const char* key);

/* Mutable array helpers (read side of builder values). */
size_t json_builder_arr_size(JsonBuilder* b, JsonMut* arr);
JsonMut* json_builder_arr_get(JsonBuilder* b, JsonMut* arr, size_t index);

JsonMut* json_builder_obj_add_obj(JsonBuilder* b, JsonMut* obj, const char* key);
JsonMut* json_builder_obj_add_arr(JsonBuilder* b, JsonMut* obj, const char* key);
int json_builder_obj_add_str(JsonBuilder* b, JsonMut* obj, const char* key, const char* value);
int json_builder_obj_add_int(JsonBuilder* b, JsonMut* obj, const char* key, int64_t value);
int json_builder_obj_add_real(JsonBuilder* b, JsonMut* obj, const char* key, double value);
int json_builder_obj_add_bool(JsonBuilder* b, JsonMut* obj, const char* key, bool value);
int json_builder_obj_add_null(JsonBuilder* b, JsonMut* obj, const char* key);

/* Deep-copy an immutable JsonVal (e.g. a parsed schema object) into the
 * builder document under key. The source stays owned by its JsonDoc. */
int json_builder_obj_add_val_copy(JsonBuilder* b, JsonMut* obj, const char* key,
                                  const JsonVal* src);

JsonMut* json_builder_arr_add_obj(JsonBuilder* b, JsonMut* arr);
JsonMut* json_builder_arr_add_arr(JsonBuilder* b, JsonMut* arr);
int json_builder_arr_add_str(JsonBuilder* b, JsonMut* arr, const char* value);
int json_builder_arr_add_int(JsonBuilder* b, JsonMut* arr, int64_t value);
int json_builder_arr_add_bool(JsonBuilder* b, JsonMut* arr, bool value);

/* Append compact JSON text to out. Suitable for JSONL and request bodies. */
int json_builder_stringify(JsonBuilder* b, String* out);

/* Append indented JSON text to out. Suitable for standalone saved JSON files. */
int json_builder_stringify_pretty(JsonBuilder* b, String* out);

/* Clear the document; previously returned JsonMut pointers are invalid. */
void json_builder_reset(JsonBuilder* b);

#endif /* CAGENT_UTIL_JSON_H */
