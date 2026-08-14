/*
 * tool/plan.c — persistent, structured task tracking for self-development.
 *
 * The plan is project-local metadata at .cagent/plan.json. It is deliberately
 * separate from the conversation so compaction/resume cannot lose step state.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tool/tool.h"
#include "util/json.h"
#include "util/string.h"

typedef struct {
    char* id;
    char* title;
    char* acceptance;
    char* status;
    char* result;
    int64_t attempts;
} PlanStep;

typedef struct {
    PlanStep* items;
    size_t len;
} PlanList;

static void plan_step_free(PlanStep* step) {
    if (step == NULL) return;
    free(step->id);
    free(step->title);
    free(step->acceptance);
    free(step->status);
    free(step->result);
    memset(step, 0, sizeof(*step));
}

static void plan_free(PlanList* plan) {
    if (plan == NULL) return;
    for (size_t i = 0; i < plan->len; i++) plan_step_free(&plan->items[i]);
    free(plan->items);
    memset(plan, 0, sizeof(*plan));
}

static int plan_path(const ToolContext* ctx, char* out, size_t cap) {
    const char* cwd = ctx != NULL && ctx->cwd != NULL ? ctx->cwd : ".";
    if (snprintf(out, cap, "%s/.cagent/plan.json", cwd) >= (int)cap) return AGENT_ERR_IO;
    return AGENT_OK;
}

static int plan_load(const char* path, PlanList* out) {
    FILE* f = fopen(path, "rb");
    if (f == NULL && errno == ENOENT) return AGENT_OK;
    if (f == NULL) return AGENT_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return AGENT_ERR_IO; }
    long size = ftell(f);
    if (size < 0 || size > 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return AGENT_ERR_IO;
    }
    char* data = malloc((size_t)size + 1);
    if (data == NULL) { fclose(f); return AGENT_ERR_OOM; }
    size_t n = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[n] = '\0';
    JsonDoc* doc = json_parse(data, n);
    free(data);
    if (doc == NULL) return AGENT_ERR_JSON;
    JsonVal* root = json_root(doc);
    JsonVal* steps = root != NULL ? json_val_obj_get(root, "steps") : NULL;
    if (steps == NULL || !json_val_is_arr(steps)) {
        json_doc_free(doc);
        return AGENT_ERR_JSON;
    }
    size_t count = json_val_arr_size(steps);
    PlanStep* items = count > 0 ? calloc(count, sizeof(*items)) : NULL;
    if (count > 0 && items == NULL) { json_doc_free(doc); return AGENT_ERR_OOM; }
    size_t filled = 0;
    for (size_t i = 0; i < count; i++) {
        JsonVal* item = json_val_arr_get(steps, i);
        const char* id = json_obj_get_str(item, "id");
        const char* title = json_obj_get_str(item, "title");
        if (id == NULL || title == NULL || id[0] == '\0' || title[0] == '\0') continue;
        PlanStep* step = &items[filled];
        step->id = strdup(id);
        step->title = strdup(title);
        const char* value = json_obj_get_str(item, "acceptance");
        step->acceptance = value != NULL ? strdup(value) : strdup("");
        value = json_obj_get_str(item, "status");
        step->status = value != NULL ? strdup(value) : strdup("pending");
        value = json_obj_get_str(item, "result");
        step->result = value != NULL ? strdup(value) : NULL;
        step->attempts = json_obj_get_int(item, "attempts", 0);
        if (step->id == NULL || step->title == NULL || step->acceptance == NULL ||
            step->status == NULL) {
            plan_free(&(PlanList){.items = items, .len = filled + 1});
            json_doc_free(doc);
            return AGENT_ERR_OOM;
        }
        filled++;
    }
    out->items = items;
    out->len = filled;
    json_doc_free(doc);
    return AGENT_OK;
}

static int plan_save(const char* path, const PlanList* plan) {
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir)) return AGENT_ERR_IO;
    char* slash = strrchr(dir, '/');
    if (slash == NULL) return AGENT_ERR_IO;
    *slash = '\0';
    /* path is .../.cagent/plan.json; create the .cagent parent. */
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return AGENT_ERR_IO;

    JsonBuilder* builder = json_builder_new();
    JsonMut* root = builder != NULL ? json_builder_root_obj(builder) : NULL;
    JsonMut* steps = root != NULL ? json_builder_obj_add_arr(builder, root, "steps") : NULL;
    if (builder == NULL || root == NULL || steps == NULL) {
        json_builder_free(builder);
        return AGENT_ERR_OOM;
    }
    if (json_builder_obj_add_int(builder, root, "version", 1) != AGENT_OK) {
        json_builder_free(builder);
        return AGENT_ERR_OOM;
    }
    for (size_t i = 0; i < plan->len; i++) {
        const PlanStep* step = &plan->items[i];
        JsonMut* item = json_builder_arr_add_obj(builder, steps);
        if (item == NULL || json_builder_obj_add_str(builder, item, "id", step->id) != AGENT_OK ||
            json_builder_obj_add_str(builder, item, "title", step->title) != AGENT_OK ||
            json_builder_obj_add_str(builder, item, "acceptance", step->acceptance) != AGENT_OK ||
            json_builder_obj_add_str(builder, item, "status", step->status) != AGENT_OK ||
            json_builder_obj_add_int(builder, item, "attempts", step->attempts) != AGENT_OK) {
            json_builder_free(builder);
            return AGENT_ERR_OOM;
        }
        if (step->result != NULL &&
            json_builder_obj_add_str(builder, item, "result", step->result) != AGENT_OK) {
            json_builder_free(builder);
            return AGENT_ERR_OOM;
        }
    }
    String data = string_new();
    int rc = json_builder_stringify(builder, &data);
    json_builder_free(builder);
    if (rc != AGENT_OK) { string_free(&data); return rc; }

    char temp[PATH_MAX];
    if (snprintf(temp, sizeof(temp), "%s.tmp.XXXXXX", path) >= (int)sizeof(temp)) {
        string_free(&data);
        return AGENT_ERR_IO;
    }
    int fd = mkstemp(temp);
    if (fd < 0) { string_free(&data); return AGENT_ERR_IO; }
    (void)fchmod(fd, 0600);
    FILE* f = fdopen(fd, "wb");
    if (f == NULL) {
        close(fd); unlink(temp); string_free(&data); return AGENT_ERR_IO;
    }
    bool ok = fwrite(data.data, 1, data.len, f) == data.len;
    if (ok) { fputc('\n', f); ok = fflush(f) == 0 && !ferror(f); }
    int close_rc = fclose(f);
    if (!ok || close_rc != 0 || rename(temp, path) != 0) {
        unlink(temp); string_free(&data); return AGENT_ERR_IO;
    }
    string_free(&data);
    return AGENT_OK;
}

static PlanStep* find_step(PlanList* plan, const char* id) {
    for (size_t i = 0; i < plan->len; i++) {
        if (strcmp(plan->items[i].id, id) == 0) return &plan->items[i];
    }
    return NULL;
}

static int set_status(PlanStep* step, const char* status, const char* result) {
    char* copy = strdup(status);
    if (copy == NULL) return AGENT_ERR_OOM;
    char* result_copy = result != NULL ? strdup(result) : NULL;
    if (result != NULL && result_copy == NULL) { free(copy); return AGENT_ERR_OOM; }
    free(step->status);
    free(step->result);
    step->status = copy;
    step->result = result_copy;
    return AGENT_OK;
}

static int plan_execute(ToolContext* ctx, const char* arguments, ToolResult* result) {
    result->content = NULL;
    result->is_error = false;
    JsonDoc* args = json_parse(arguments != NULL ? arguments : "{}",
                               arguments != NULL ? strlen(arguments) : 2);
    JsonVal* root = args != NULL ? json_root(args) : NULL;
    const char* action = root != NULL ? json_obj_get_str(root, "action") : NULL;
    if (action == NULL) {
        result->content = strdup("error: missing plan action (list/add/start/complete/fail/reset)");
        result->is_error = true;
        json_doc_free(args);
        return AGENT_OK;
    }
    char path[PATH_MAX];
    PlanList plan = {0};
    int rc = plan_path(ctx, path, sizeof(path));
    if (rc == AGENT_OK && strcmp(action, "list") != 0) rc = plan_load(path, &plan);
    if (rc != AGENT_OK) {
        result->content = strdup("error: cannot load project plan");
        result->is_error = true;
        json_doc_free(args); plan_free(&plan); return AGENT_OK;
    }
    if (strcmp(action, "list") == 0) {
        rc = plan_load(path, &plan);
        if (rc == AGENT_OK) {
            String out = string_new();
            if (plan.len == 0) string_append(&out, "(no plan steps)\n");
            for (size_t i = 0; i < plan.len; i++) {
                PlanStep* step = &plan.items[i];
                string_printf(&out, "%s [%s] %s (attempts=%lld)\n  acceptance: %s\n",
                              step->id, step->status, step->title,
                              (long long)step->attempts, step->acceptance);
                if (step->result != NULL) string_printf(&out, "  result: %s\n", step->result);
            }
            result->content = string_take(&out);
        }
    } else if (strcmp(action, "add") == 0) {
        const char* id = json_obj_get_str(root, "id");
        const char* title = json_obj_get_str(root, "title");
        const char* acceptance = json_obj_get_str(root, "acceptance");
        if (id == NULL || title == NULL || find_step(&plan, id) != NULL) {
            result->content = strdup("error: add requires a unique id and title");
            result->is_error = true;
        } else {
            PlanStep* grown = realloc(plan.items, (plan.len + 1) * sizeof(*grown));
            if (grown == NULL) {
                result->content = strdup("error: out of memory"); result->is_error = true;
            } else {
                plan.items = grown;
                PlanStep* step = &plan.items[plan.len++];
                memset(step, 0, sizeof(*step));
                step->id = strdup(id); step->title = strdup(title);
                step->acceptance = strdup(acceptance != NULL ? acceptance : "");
                step->status = strdup("pending");
                if (step->id == NULL || step->title == NULL || step->acceptance == NULL ||
                    step->status == NULL || plan_save(path, &plan) != AGENT_OK) {
                    result->content = strdup("error: cannot save project plan"); result->is_error = true;
                } else {
                    result->content = strdup("plan step added");
                }
            }
        }
    } else {
        const char* id = json_obj_get_str(root, "id");
        PlanStep* step = id != NULL ? find_step(&plan, id) : NULL;
        if (step == NULL) {
            result->content = strdup("error: unknown plan step id"); result->is_error = true;
        } else if (strcmp(action, "start") == 0) {
            rc = set_status(step, "in_progress", NULL);
            if (rc == AGENT_OK) rc = plan_save(path, &plan);
            result->content = strdup(rc == AGENT_OK ? "plan step started" : "error: cannot save project plan");
            result->is_error = rc != AGENT_OK;
        } else if (strcmp(action, "complete") == 0 || strcmp(action, "fail") == 0 ||
                   strcmp(action, "reset") == 0) {
            const char* value = json_obj_get_str(root, strcmp(action, "fail") == 0 ? "error" : "result");
            const char* status = strcmp(action, "complete") == 0 ? "completed"
                                : strcmp(action, "fail") == 0 ? "failed" : "pending";
            if (strcmp(action, "fail") == 0) step->attempts++;
            rc = set_status(step, status, value);
            if (rc == AGENT_OK) rc = plan_save(path, &plan);
            result->content = strdup(rc == AGENT_OK ? "plan step updated" : "error: cannot save project plan");
            result->is_error = rc != AGENT_OK;
        } else {
            result->content = strdup("error: unknown plan action"); result->is_error = true;
        }
    }
    if (result->content == NULL) {
        result->content = strdup("error: plan operation failed"); result->is_error = true;
    }
    plan_free(&plan);
    json_doc_free(args);
    return result->content != NULL ? AGENT_OK : AGENT_ERR_OOM;
}

int plan_summary(const char* cwd, String* out) {
    if (out == NULL) {
        return AGENT_ERR_IO;
    }
    string_clear(out);
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/.cagent/plan.json",
                 cwd != NULL ? cwd : ".") >= (int)sizeof(path)) {
        return AGENT_ERR_IO;
    }
    PlanList plan = {0};
    int rc = plan_load(path, &plan);
    if (rc != AGENT_OK) {
        plan_free(&plan);
        return rc;
    }
    for (size_t i = 0; i < plan.len; i++) {
        const PlanStep* step = &plan.items[i];
        string_printf(out, "%s [%s] %s (attempts=%lld)\n  acceptance: %s\n", step->id,
                      step->status, step->title, (long long)step->attempts, step->acceptance);
        if (step->result != NULL) {
            string_printf(out, "  result: %s\n", step->result);
        }
    }
    plan_free(&plan);
    return AGENT_OK;
}

Tool plan_tool = { /* NOLINT(misc-use-internal-linkage) */
    .name = "plan",
    .description = "Persist and track project plan steps with acceptance criteria and retry counts.",
    .input_schema = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"add\",\"start\",\"complete\",\"fail\",\"reset\"]},\"id\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"},\"acceptance\":{\"type\":\"string\"},\"result\":{\"type\":\"string\"},\"error\":{\"type\":\"string\"}},\"required\":[\"action\"]}",
    .flags = TOOL_FLAG_NONE,
    .execute = plan_execute,
};
