/*
 * tool/registry.c — tool registry.
 */

#include <stdlib.h>
#include <string.h>

#include "tool/registry.h"
#include "util/json.h"

#define REGISTRY_MIN_CAP 8

ToolRegistry* tool_registry_new(void) {
    ToolRegistry* reg = calloc(1, sizeof(ToolRegistry));
    if (reg == NULL) {
        return NULL;
    }
    reg->cap = REGISTRY_MIN_CAP;
    reg->tools = (Tool**)malloc(reg->cap * sizeof(Tool*));
    reg->enabled = (bool*)malloc(reg->cap * sizeof(bool));
    if (reg->tools == NULL || reg->enabled == NULL) {
        tool_registry_free(reg);
        return NULL;
    }
    return reg;
}

void tool_registry_free(ToolRegistry* reg) {
    if (reg == NULL) {
        return;
    }
    free((void*)reg->tools);
    free((void*)reg->enabled);
    free(reg);
}

int tool_registry_register(ToolRegistry* reg, Tool* tool) {
    if (reg == NULL || tool == NULL || tool->name == NULL) {
        return AGENT_ERR_TOOL;
    }
    if (tool_registry_find(reg, tool->name) != NULL) {
        return AGENT_ERR_TOOL; /* duplicate name */
    }
    if (reg->len == reg->cap) {
        if (reg->cap > SIZE_MAX / 2) {
            return AGENT_ERR_OOM; /* capacity would overflow */
        }
        size_t new_cap = reg->cap * 2;
        if (new_cap > SIZE_MAX / sizeof(Tool*)) {
            return AGENT_ERR_OOM; /* byte count would overflow */
        }
        /* realloc one array at a time: a failed second realloc must not
         * leave the first one dangling */
        Tool** t = (Tool**)realloc((void*)reg->tools, new_cap * sizeof(Tool*));
        if (t == NULL) {
            return AGENT_ERR_OOM;
        }
        reg->tools = t;
        bool* e = (bool*)realloc(reg->enabled, new_cap * sizeof(bool));
        if (e == NULL) {
            return AGENT_ERR_OOM; /* tools grown, enabled kept: consistent */
        }
        reg->enabled = e;
        reg->cap = new_cap;
    }
    /* NOLINTNEXTLINE(clang-analyzer-unix.Malloc): false positive, cap >= 8. */
    reg->tools[reg->len] = tool;
    reg->enabled[reg->len] = true;
    reg->len++;
    return AGENT_OK;
}

Tool* tool_registry_find(const ToolRegistry* reg, const char* name) {
    if (reg == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < reg->len; i++) {
        if (reg->enabled[i] && strcmp(reg->tools[i]->name, name) == 0) {
            return reg->tools[i];
        }
    }
    return NULL;
}

void tool_registry_set_enabled(ToolRegistry* reg, const char* name, bool on) {
    if (reg == NULL || name == NULL) {
        return;
    }
    for (size_t i = 0; i < reg->len; i++) {
        if (strcmp(reg->tools[i]->name, name) == 0) {
            reg->enabled[i] = on;
            return;
        }
    }
}

bool tool_registry_is_enabled(const ToolRegistry* reg, const char* name) {
    if (reg == NULL || name == NULL) {
        return false;
    }
    for (size_t i = 0; i < reg->len; i++) {
        if (strcmp(reg->tools[i]->name, name) == 0) {
            return reg->enabled[i];
        }
    }
    return false;
}

size_t tool_registry_count(const ToolRegistry* reg) {
    return reg == NULL ? 0 : reg->len;
}

int tool_registry_schema_json(const ToolRegistry* reg, String* out) {
    if (reg == NULL || out == NULL) {
        return AGENT_ERR_TOOL;
    }

    JsonBuilder* b = json_builder_new();
    if (b == NULL) {
        return AGENT_ERR_OOM;
    }
    JsonMut* arr = json_builder_root_arr(b);
    if (arr == NULL) {
        json_builder_free(b);
        return AGENT_ERR_OOM;
    }

    for (size_t i = 0; i < reg->len; i++) {
        if (!reg->enabled[i]) {
            continue;
        }
        JsonMut* t = json_builder_arr_add_obj(b, arr);
        JsonMut* fn = json_builder_obj_add_obj(b, t, "function");
        if (t == NULL || fn == NULL) {
            json_builder_free(b);
            return AGENT_ERR_OOM;
        }
        json_builder_obj_add_str(b, t, "type", "function");
        json_builder_obj_add_str(b, fn, "name", reg->tools[i]->name);
        json_builder_obj_add_str(b, fn, "description", reg->tools[i]->description);

        /* input_schema is a raw JSON object; embed it as a value */
        JsonDoc* schema_doc =
            json_parse(reg->tools[i]->input_schema, strlen(reg->tools[i]->input_schema));
        if (schema_doc == NULL) {
            json_builder_free(b);
            return AGENT_ERR_JSON;
        }
        JsonVal* schema_root = json_root(schema_doc);
        if (schema_root == NULL || !json_val_is_obj(schema_root)) {
            json_doc_free(schema_doc);
            json_builder_free(b);
            return AGENT_ERR_JSON;
        }
        /* deep-copy the parsed schema into the builder document */
        if (json_builder_obj_add_val_copy(b, fn, "parameters", schema_root) != AGENT_OK) {
            json_doc_free(schema_doc);
            json_builder_free(b);
            return AGENT_ERR_OOM;
        }
        json_doc_free(schema_doc);
    }

    int err = json_builder_stringify(b, out);
    json_builder_free(b);
    return err;
}
