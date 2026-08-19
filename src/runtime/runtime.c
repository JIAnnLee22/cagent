/*
 * runtime/runtime.c — runtime assembly.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "auth/oauth.h"
#include "model/anthropic.h"
#include "model/openai.h"
#include "model/provider.h"
#include "model/responses.h"
#include "runtime/event_loop.h"
#include "runtime/http.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "tool/builtin.h"
#include "util/json.h"
#include "util/log.h"
#include "util/string.h"

/* OpenCode Go subscription is the default provider (DESIGN.md §7):
 * https://opencode.ai/zen/go/v1/chat/completions, Bearer key, model ids
 * like glm-5.2. Provider-qualified selectors are normalized after discovery. */
#define DEFAULT_BASE_URL "https://opencode.ai/zen/go/v1"
#define CHATGPT_CODEX_BASE_URL "https://chatgpt.com/backend-api/codex"
/* The Codex /models endpoint returns an empty catalog for obsolete client
 * versions. Keep this in sync with the current Codex wire protocol. */
#define CHATGPT_CLIENT_VERSION "1.0.0"
#define MODEL_DISCOVERY_TIMEOUT_SECONDS 30
#define DEFAULT_API_KEY_ENV "$OPENCODE_GO_API_KEY"
#define DEFAULT_MODEL "glm-5.2"
#define DEFAULT_MAX_TOKENS 4096
#define DEFAULT_CONTEXT_WINDOW 128000
#define DEFAULT_MAX_OUTPUT 8192
/* Transient network failures are common on mobile/VPN/TUN links. Keep the
 * retry budget large enough to bridge a short outage without making
 * permanent failures feel hung. */
#define DEFAULT_MAX_RETRIES 5
#define DEFAULT_PROJECT_MEMORY_MAX_BYTES (4 * 1024)

static bool legacy_auth_is_chatgpt(const char* auth) {
    return auth != NULL && strcmp(auth, "chatgpt") == 0;
}

static bool provider_name_is_chatgpt(const char* provider) {
    return provider != NULL &&
           (strcmp(provider, "chatgpt") == 0 || strcmp(provider, "openai-codex") == 0);
}

static bool provider_names_equal(const char* left, const char* right) {
    if (left == NULL || right == NULL) {
        return left == right;
    }
    return strcmp(left, right) == 0 ||
           (provider_name_is_chatgpt(left) && provider_name_is_chatgpt(right));
}

static const char* builtin_provider_for_base_url(const char* base_url) {
    static const char* const names[] = {"opencode-go", "openai", "anthropic", "chatgpt"};
    if (base_url == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char* builtin = provider_builtin_base_url(names[i]);
        if (builtin != NULL && strcmp(base_url, builtin) == 0) {
            return names[i];
        }
    }
    return NULL;
}

static Provider* provider_from_config(const char* base_url, const char* provider_name,
                                      const char* api_key_env, const char* auth) {
    char path[PATH_MAX];
    if (oauth_default_path(path, sizeof(path)) != AGENT_OK) {
        path[0] = '\0';
    }
    /* The legacy auth flag belongs only to the ChatGPT provider.  Do not let
     * a global `auth: chatgpt` turn explicitly configured OpenAI/Anthropic
     * model entries into ChatGPT models (and therefore give them all the
     * `chatgpt/` selector prefix). */
    if (legacy_auth_is_chatgpt(auth) &&
        (provider_name_is_chatgpt(provider_name) ||
         (provider_name != NULL && strcmp(provider_name, "opencode-go") == 0))) {
        Provider* p = provider_new_chatgpt(base_url, path[0] != '\0' ? path : NULL);
        if (p != NULL && provider_name != NULL && strcmp(provider_name, p->provider_name) != 0) {
            char* name = strdup(provider_name);
            if (name == NULL) {
                provider_free(p);
                return NULL;
            }
            free(p->provider_name);
            p->provider_name = name;
        }
        return p;
    }
    return provider_new_auth(base_url, provider_name, path[0] != '\0' ? path : NULL, api_key_env);
}

void model_config_free(ModelConfig* m) {
    if (m == NULL) {
        return;
    }
    free(m->name);
    free(m->label);
    free(m->provider);
    free(m->base_url);
    free(m->api_key_env);
    free(m->auth);
    free(m->protocol);
    free(m->models_path);
    memset(m, 0, sizeof(*m));
}

Config config_default(void) {
    Config c = {0};
    c.provider = strdup("opencode-go");
    c.max_tokens = DEFAULT_MAX_TOKENS;
    c.context_window = DEFAULT_CONTEXT_WINDOW;
    c.max_output = DEFAULT_MAX_OUTPUT;
    c.max_concurrent_agents = 16;
    c.max_retries = DEFAULT_MAX_RETRIES;
    /* A separate set bit lets config.json explicitly use zero to disable
     * the automatic PROGRESS.md excerpt. */
    c.project_memory_max_bytes = DEFAULT_PROJECT_MEMORY_MAX_BYTES;
    c.project_memory_max_bytes_set = false;
    return c;
}

void config_free(Config* c) {
    if (c == NULL) {
        return;
    }
    free(c->provider);
    free(c->base_url);
    free(c->api_key_env);
    free(c->auth);
    free(c->protocol);
    free(c->models_path);
    free(c->model_name);
    free(c->cwd);
    for (size_t i = 0; i < c->n_models; i++) {
        model_config_free(&c->models[i]);
    }
    free(c->models);
    memset(c, 0, sizeof(*c));
}

/* Phase 1: the config file is optional and best-effort. Fields present in
 * the file override current values; unknown keys are ignored. */
int config_load_file(Config* c, const char* path) {
    if (c == NULL || path == NULL) {
        return AGENT_OK;
    }

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return AGENT_OK; /* absent config is fine */
    }
    /* read the whole file (small) in one go */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return AGENT_ERR_IO;
    }
    long size = ftell(f);
    if (size < 0 || size > 1024 * 1024) {
        fclose(f);
        return AGENT_ERR_IO; /* too large or unknown size */
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return AGENT_ERR_IO;
    }
    char* data = malloc((size_t)size + 1);
    if (data == NULL) {
        fclose(f);
        return AGENT_ERR_OOM;
    }
    size_t n = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[n] = '\0';

    JsonDoc* doc = json_parse(data, n);
    free(data);
    if (doc == NULL) {
        return AGENT_ERR_JSON;
    }
    JsonVal* root = json_root(doc);
    if (root == NULL || !json_val_is_obj(root)) {
        json_doc_free(doc);
        return AGENT_ERR_JSON;
    }
    bool provider_present = json_obj_get_str(root, "provider") != NULL;

    const char* v = json_obj_get_str(root, "provider");
    if (v != NULL) {
        free(c->provider);
        c->provider = strdup(v);
    }
    v = json_obj_get_str(root, "base_url");
    if (v != NULL) {
        free(c->base_url);
        c->base_url = strdup(v);
    }
    v = json_obj_get_str(root, "api_key_env");
    if (v != NULL) {
        free(c->api_key_env);
        c->api_key_env = strdup(v);
    }
    v = json_obj_get_str(root, "auth");
    if (v != NULL) {
        free(c->auth);
        c->auth = strdup(v);
        if (strcmp(v, "chatgpt") == 0 && c->provider != NULL &&
            strcmp(c->provider, "opencode-go") == 0) {
            free(c->provider);
            c->provider = strdup("chatgpt");
        }
    }
    v = json_obj_get_str(root, "protocol");
    if (v != NULL) {
        free(c->protocol);
        c->protocol = strdup(v);
    }
    v = json_obj_get_str(root, "models_path");
    if (v != NULL) {
        free(c->models_path);
        c->models_path = strdup(v);
    }
    v = json_obj_get_str(root, "model");
    if (v != NULL) {
        free(c->model_name);
        c->model_name = strdup(v);
    }
    /* Older configs may have persisted provider/model as one selector while
     * omitting the provider field. Recover the provider before runtime
     * construction so the bare model id is sent to the right endpoint. */
    if (!provider_present && c->model_name != NULL) {
        const char* slash = strchr(c->model_name, '/');
        if (slash != NULL && slash != c->model_name) {
            size_t provider_len = (size_t)(slash - c->model_name);
            char* inferred = strndup(c->model_name, provider_len);
            if (inferred == NULL) {
                json_doc_free(doc);
                return AGENT_ERR_OOM;
            }
            free(c->provider);
            c->provider = inferred;
        }
    }
    int64_t iv;
    if (json_val_is_int(json_val_obj_get(root, "max_tokens"))) {
        iv = json_obj_get_int(root, "max_tokens", 0);
        if (iv > 0)
            c->max_tokens = iv;
    }
    if (json_val_is_int(json_val_obj_get(root, "context_window"))) {
        iv = json_obj_get_int(root, "context_window", 0);
        if (iv > 0) {
            c->context_window = iv;
            c->context_window_set = true;
        }
    }
    if (json_val_is_int(json_val_obj_get(root, "max_output"))) {
        iv = json_obj_get_int(root, "max_output", 0);
        if (iv > 0)
            c->max_output = iv;
    }
    /* optional per-model billing: USD per 1M tokens; subscription flat plans */
    c->input_price = json_obj_get_num(root, "price_in", 0.0);
    c->output_price = json_obj_get_num(root, "price_out", 0.0);
    c->subscription = json_obj_get_bool(root, "subscription", false);
    if (json_val_is_int(json_val_obj_get(root, "max_concurrent_agents"))) {
        iv = json_obj_get_int(root, "max_concurrent_agents", 0);
        if (iv > 0)
            c->max_concurrent_agents = iv;
    }
    if (json_val_is_int(json_val_obj_get(root, "max_retries"))) {
        iv = json_obj_get_int(root, "max_retries", DEFAULT_MAX_RETRIES);
        if (iv >= 0 && iv <= 10)
            c->max_retries = iv;
    }
    if (json_val_is_int(json_val_obj_get(root, "project_memory_max_bytes"))) {
        iv = json_obj_get_int(root, "project_memory_max_bytes", 0);
        if (iv >= 0 && iv <= 128 * 1024) {
            c->project_memory_max_bytes = iv;
            c->project_memory_max_bytes_set = true;
        }
    }
    /* models array: [{name, label?, provider?, base_url?, api_key_env?,
     *                context_window?, max_output?, price_in?, price_out?,
     *                subscription?}] */
    JsonVal* models = json_val_obj_get(root, "models");
    if (models != NULL && json_val_is_arr(models)) {
        size_t n = json_val_arr_size(models);
        if (n > 0) {
            ModelConfig* mc = calloc(n, sizeof(ModelConfig));
            if (mc == NULL) {
                json_doc_free(doc);
                return AGENT_ERR_OOM;
            }
            size_t filled = 0;
            for (size_t i = 0; i < n; i++) {
                JsonVal* item = json_val_arr_get(models, i);
                if (item == NULL || !json_val_is_obj(item)) {
                    continue;
                }
                const char* name = json_obj_get_str(item, "name");
                if (name == NULL || name[0] == '\0') {
                    continue;
                }
                ModelConfig* m = &mc[filled];
                m->name = strdup(name);
                const char* v = json_obj_get_str(item, "label");
                m->label = v != NULL ? strdup(v) : NULL;
                v = json_obj_get_str(item, "provider");
                m->provider = v != NULL ? strdup(v) : NULL;
                v = json_obj_get_str(item, "base_url");
                m->base_url = v != NULL ? strdup(v) : NULL;
                v = json_obj_get_str(item, "api_key_env");
                m->api_key_env = v != NULL ? strdup(v) : NULL;
                v = json_obj_get_str(item, "auth");
                m->auth = v != NULL ? strdup(v) : NULL;
                v = json_obj_get_str(item, "protocol");
                m->protocol = v != NULL ? strdup(v) : NULL;
                v = json_obj_get_str(item, "models_path");
                m->models_path = v != NULL ? strdup(v) : NULL;
                m->context_window = json_obj_get_int(item, "context_window", 0);
                m->max_output = json_obj_get_int(item, "max_output", 0);
                m->input_price = json_obj_get_num(item, "price_in", 0.0);
                m->output_price = json_obj_get_num(item, "price_out", 0.0);
                m->subscription = json_obj_get_bool(item, "subscription", false);
                filled++;
            }
            if (filled > 0) {
                for (size_t i = 0; i < c->n_models; i++) {
                    model_config_free(&c->models[i]);
                }
                free(c->models);
                c->models = mc;
                c->n_models = filled;
            } else {
                free(mc);
            }
        }
    }

    /* A selection saved by an older version may have changed the top-level
     * provider while leaving the old builtin endpoint in place. Preserve
     * provider-less static entries on that old provider instead of silently
     * reinterpreting every OpenCode model as a ChatGPT model. Live discovery
     * will replace these entries when it succeeds. */
    const char* legacy_provider = builtin_provider_for_base_url(c->base_url);
    if (legacy_provider != NULL && !provider_names_equal(legacy_provider, c->provider)) {
        for (size_t i = 0; i < c->n_models; i++) {
            if (c->models[i].provider == NULL) {
                c->models[i].provider = strdup(legacy_provider);
                if (c->models[i].provider == NULL) {
                    json_doc_free(doc);
                    return AGENT_ERR_OOM;
                }
            }
        }
    }

    json_doc_free(doc);
    return AGENT_OK;
}

typedef struct {
    JsonBuilder* builder;
    JsonMut* root;
    const char* provider_name;
    const char* model_name;
    bool provider_written;
    bool model_written;
} ConfigSaveContext;

static int config_save_member(const char* key, const JsonVal* value, void* userdata) {
    ConfigSaveContext* ctx = userdata;
    if (strcmp(key, "provider") == 0 && ctx->provider_name != NULL) {
        if (ctx->provider_written) {
            return AGENT_OK;
        }
        ctx->provider_written = true;
        return json_builder_obj_add_str(ctx->builder, ctx->root, "provider", ctx->provider_name);
    }
    if (strcmp(key, "model") == 0) {
        if (ctx->model_written) {
            return AGENT_OK;
        }
        ctx->model_written = true;
        return json_builder_obj_add_str(ctx->builder, ctx->root, "model", ctx->model_name);
    }
    return json_builder_obj_add_val_copy(ctx->builder, ctx->root, key, value);
}

static int config_ensure_parent_dirs(const char* path) {
    char parent[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(parent)) {
        return AGENT_ERR_IO;
    }
    memcpy(parent, path, len + 1);
    char* slash = strrchr(parent, '/');
    if (slash == NULL) {
        return AGENT_OK;
    }
    if (slash == parent) {
        return AGENT_OK;
    }
    *slash = '\0';
    for (char* p = parent + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(parent, 0700) != 0 && errno != EEXIST) {
            return AGENT_ERR_IO;
        }
        *p = '/';
    }
    if (mkdir(parent, 0700) != 0 && errno != EEXIST) {
        return AGENT_ERR_IO;
    }
    return AGENT_OK;
}

static int config_save_fields(const char* path, const char* provider_name,
                              const char* model_name) {
    if (path == NULL || path[0] == '\0' || model_name == NULL || model_name[0] == '\0' ||
        (provider_name != NULL && provider_name[0] == '\0')) {
        return AGENT_ERR_IO;
    }

    char* data = NULL;
    size_t data_len = 0;
    FILE* input = fopen(path, "rb");
    if (input != NULL) {
        if (fseek(input, 0, SEEK_END) != 0) {
            fclose(input);
            return AGENT_ERR_IO;
        }
        long size = ftell(input);
        if (size < 0 || size > 1024 * 1024 || fseek(input, 0, SEEK_SET) != 0) {
            fclose(input);
            return AGENT_ERR_IO;
        }
        data = malloc((size_t)size + 1);
        if (data == NULL) {
            fclose(input);
            return AGENT_ERR_OOM;
        }
        data_len = fread(data, 1, (size_t)size, input);
        fclose(input);
        data[data_len] = '\0';
    } else if (errno != ENOENT) {
        return AGENT_ERR_IO;
    }

    JsonDoc* doc = NULL;
    JsonVal* root = NULL;
    if (data != NULL) {
        doc = json_parse(data, data_len);
        root = doc != NULL ? json_root(doc) : NULL;
        if (root == NULL || !json_val_is_obj(root)) {
            free(data);
            json_doc_free(doc);
            return AGENT_ERR_JSON;
        }
    }

    JsonBuilder* builder = json_builder_new();
    JsonMut* out_root = builder != NULL ? json_builder_root_obj(builder) : NULL;
    if (builder == NULL || out_root == NULL) {
        free(data);
        json_doc_free(doc);
        json_builder_free(builder);
        return AGENT_ERR_OOM;
    }
    ConfigSaveContext ctx = {
        .builder = builder,
        .root = out_root,
        .provider_name = provider_name,
        .model_name = model_name,
        .provider_written = false,
        .model_written = false,
    };
    int rc = root != NULL ? json_obj_foreach(root, config_save_member, &ctx) : AGENT_OK;
    if (rc == AGENT_OK && provider_name != NULL && !ctx.provider_written) {
        rc = json_builder_obj_add_str(builder, out_root, "provider", provider_name);
    }
    if (rc == AGENT_OK && !ctx.model_written) {
        rc = json_builder_obj_add_str(builder, out_root, "model", model_name);
    }
    String serialized = string_new();
    if (rc == AGENT_OK) {
        rc = json_builder_stringify(builder, &serialized);
    }
    json_builder_free(builder);
    json_doc_free(doc);
    free(data);
    if (rc != AGENT_OK) {
        string_free(&serialized);
        return rc;
    }

    rc = config_ensure_parent_dirs(path);
    if (rc != AGENT_OK) {
        string_free(&serialized);
        return rc;
    }
    char temp[PATH_MAX];
    if (snprintf(temp, sizeof(temp), "%s.tmp.XXXXXX", path) >= (int)sizeof(temp)) {
        string_free(&serialized);
        return AGENT_ERR_IO;
    }
    int fd = mkstemp(temp);
    if (fd < 0) {
        string_free(&serialized);
        return AGENT_ERR_IO;
    }
    (void)fchmod(fd, 0600);
    FILE* output = fdopen(fd, "wb");
    if (output == NULL) {
        close(fd);
        unlink(temp);
        string_free(&serialized);
        return AGENT_ERR_IO;
    }
    bool ok = fwrite(serialized.data, 1, serialized.len, output) == serialized.len;
    if (ok) {
        fputc('\n', output);
        ok = !ferror(output) && fflush(output) == 0;
    }
    int close_rc = fclose(output);
    if (!ok || close_rc != 0 || rename(temp, path) != 0) {
        unlink(temp);
        string_free(&serialized);
        return AGENT_ERR_IO;
    }
    string_free(&serialized);
    return AGENT_OK;
}

int config_save_model(const char* path, const char* model_name) {
    return config_save_fields(path, NULL, model_name);
}

int config_save_selection(const char* path, const char* provider_name, const char* model_name) {
    return config_save_fields(path, provider_name, model_name);
}

typedef struct {
    Config* config;
    bool done;
    int rc;
    long http_status; /* HTTP status when rc == AGENT_ERR_HTTP; 0 = transport */
    CURLcode curl_rc; /* transport error code when rc == AGENT_ERR_HTTP */
} ModelDiscovery;

static void model_discovery_done(HttpRequest* req, const HttpDoneInfo* info, void* userdata) {
    (void)req;
    ModelDiscovery* discovery = userdata;
    if (discovery == NULL || info == NULL) {
        return;
    }
    discovery->done = true;
    if (info->rc != CURLE_OK || info->http_status < 200 || info->http_status >= 300) {
        discovery->rc = AGENT_ERR_HTTP;
        discovery->http_status = info->http_status;
        discovery->curl_rc = info->rc;
        return;
    }
    JsonDoc* doc = info->body != NULL ? json_parse(info->body, info->body_len) : NULL;
    JsonVal* root = doc != NULL ? json_root(doc) : NULL;
    JsonVal* models = root != NULL ? json_val_obj_get(root, "models") : NULL;
    if (models == NULL) {
        models = root != NULL ? json_val_obj_get(root, "data") : NULL;
    }
    if (models == NULL || !json_val_is_arr(models)) {
        json_doc_free(doc);
        discovery->rc = AGENT_ERR_JSON;
        return;
    }

    size_t capacity = json_val_arr_size(models);
    ModelConfig* entries = capacity > 0 ? calloc(capacity, sizeof(ModelConfig)) : NULL;
    size_t count = 0;
    int rc = capacity > 0 && entries == NULL ? AGENT_ERR_OOM : AGENT_OK;
    for (size_t i = 0; rc == AGENT_OK && i < capacity; i++) {
        JsonVal* item = json_val_arr_get(models, i);
        if (item == NULL || !json_val_is_obj(item)) {
            continue;
        }
        const char* visibility = json_obj_get_str(item, "visibility");
        if ((visibility != NULL && strcmp(visibility, "hide") == 0) ||
            !json_obj_get_bool(item, "supported_in_api", true)) {
            continue;
        }
        const char* name = json_obj_get_str(item, "slug");
        if (name == NULL || name[0] == '\0') {
            name = json_obj_get_str(item, "id");
        }
        if (name == NULL || name[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(entries[j].name, name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        ModelConfig* entry = &entries[count];
        entry->provider =
            discovery->config->provider != NULL ? strdup(discovery->config->provider) : NULL;
        entry->base_url =
            discovery->config->base_url != NULL ? strdup(discovery->config->base_url) : NULL;
        entry->api_key_env =
            discovery->config->api_key_env != NULL ? strdup(discovery->config->api_key_env) : NULL;
        entry->protocol =
            discovery->config->protocol != NULL ? strdup(discovery->config->protocol) : NULL;
        entry->models_path =
            discovery->config->models_path != NULL ? strdup(discovery->config->models_path) : NULL;
        entry->name = strdup(name);
        const char* display = json_obj_get_str(item, "display_name");
        if (display == NULL || display[0] == '\0') {
            display = json_obj_get_str(item, "name");
        }
        entry->label = display != NULL && display[0] != '\0' ? strdup(display) : NULL;
        /* Model catalogs are not consistent about these names.  OpenAI's
         * /models normally uses context_length/max_completion_tokens, while
         * some gateways use context_window/max_output_tokens (or put the
         * limits under `limit`).  Prefer the explicit cagent names, then
         * accept the standard aliases instead of silently falling back to
         * the local 128k guess. */
        entry->context_window = json_obj_get_int(item, "context_window", 0);
        if (entry->context_window <= 0)
            entry->context_window = json_obj_get_int(item, "contextWindow", 0);
        if (entry->context_window <= 0)
            entry->context_window = json_obj_get_int(item, "context_length", 0);
        if (entry->context_window <= 0)
            entry->context_window = json_obj_get_int(item, "contextLength", 0);
        if (entry->context_window <= 0)
            entry->context_window = json_obj_get_int(item, "max_input_tokens", 0);
        if (entry->context_window <= 0)
            entry->context_window = json_obj_get_int(item, "input_token_limit", 0);
        /* OpenCode's catalog is intentionally minimal (id/object/created),
         * so the authoritative limit is currently documented rather than
         * returned by /models. Keep this provider-specific fallback here,
         * rather than poisoning the global default for unrelated providers. */
        if (entry->context_window <= 0 && discovery->config->provider != NULL &&
            strcmp(discovery->config->provider, "opencode-go") == 0 &&
            strcmp(name, "gpt-5.6-luna") == 0)
            entry->context_window = 272000;
        entry->max_output = json_obj_get_int(item, "max_output_tokens", 0);
        if (entry->max_output <= 0)
            entry->max_output = json_obj_get_int(item, "max_output", 0);
        if (entry->max_output <= 0)
            entry->max_output = json_obj_get_int(item, "max_completion_tokens", 0);
        JsonVal* limits = json_val_obj_get(item, "limit");
        if (limits == NULL)
            limits = json_val_obj_get(item, "limits");
        if (limits != NULL && json_val_is_obj(limits)) {
            if (entry->context_window <= 0)
                entry->context_window = json_obj_get_int(limits, "context", 0);
            if (entry->context_window <= 0)
                entry->context_window = json_obj_get_int(limits, "context_length", 0);
            if (entry->max_output <= 0)
                entry->max_output = json_obj_get_int(limits, "output", 0);
            if (entry->max_output <= 0)
                entry->max_output = json_obj_get_int(limits, "max_output_tokens", 0);
        }
        /* OpenRouter and several OpenAI-compatible catalogs expose limits in
         * `top_provider`, not on the model object itself. */
        JsonVal* top = json_val_obj_get(item, "top_provider");
        if (top != NULL && json_val_is_obj(top)) {
            if (entry->context_window <= 0)
                entry->context_window = json_obj_get_int(top, "context_length", 0);
            if (entry->max_output <= 0)
                entry->max_output = json_obj_get_int(top, "max_completion_tokens", 0);
        }
        entry->input_price = json_obj_get_num(item, "price_in", 0.0);
        entry->output_price = json_obj_get_num(item, "price_out", 0.0);
        entry->subscription = json_obj_get_bool(item, "subscription", false);
        if (entry->name == NULL || entry->provider == NULL || entry->base_url == NULL ||
            entry->protocol == NULL ||
            (discovery->config->api_key_env != NULL && entry->api_key_env == NULL) ||
            (discovery->config->models_path != NULL && entry->models_path == NULL) ||
            (display != NULL && display[0] != '\0' && entry->label == NULL)) {
            model_config_free(entry);
            rc = AGENT_ERR_OOM;
            break;
        }
        count++;
    }
    json_doc_free(doc);

    if (rc != AGENT_OK || count == 0) {
        for (size_t i = 0; i < count; i++) {
            model_config_free(&entries[i]);
        }
        free(entries);
        discovery->rc = rc != AGENT_OK ? rc : AGENT_ERR_MODEL;
        return;
    }

    bool selected_available = false;
    char* normalized_selection = NULL;
    if (discovery->config->model_name != NULL) {
        const char* selected = discovery->config->model_name;
        for (size_t i = 0; i < count; i++) {
            const char* slash = strchr(selected, '/');
            bool selector_match =
                slash != NULL && entries[i].provider != NULL &&
                (size_t)(slash - selected) == strlen(entries[i].provider) &&
                strncmp(selected, entries[i].provider, (size_t)(slash - selected)) == 0 &&
                strcmp(slash + 1, entries[i].name) == 0;
            bool label_match = entries[i].label != NULL && strcmp(selected, entries[i].label) == 0;
            if (strcmp(selected, entries[i].name) == 0 || selector_match || label_match) {
                selected_available = true;
                if (strcmp(selected, entries[i].name) != 0) {
                    normalized_selection = strdup(entries[i].name);
                    if (normalized_selection == NULL) {
                        for (size_t j = 0; j < count; j++)
                            model_config_free(&entries[j]);
                        free(entries);
                        discovery->rc = AGENT_ERR_OOM;
                        return;
                    }
                }
                break;
            }
        }
    }
    if (normalized_selection != NULL) {
        free(discovery->config->model_name);
        discovery->config->model_name = normalized_selection;
    } else if (!selected_available) {
        if (discovery->config->model_name != NULL) {
            log_warn("configured model '%s' is not in the live catalog; falling back to '%s'",
                     discovery->config->model_name, entries[0].name);
        }
        char* selected = strdup(entries[0].name);
        if (selected == NULL) {
            for (size_t i = 0; i < count; i++) {
                model_config_free(&entries[i]);
            }
            free(entries);
            discovery->rc = AGENT_ERR_OOM;
            return;
        }
        free(discovery->config->model_name);
        discovery->config->model_name = selected;
    }

    for (size_t i = 0; i < discovery->config->n_models; i++) {
        model_config_free(&discovery->config->models[i]);
    }
    free(discovery->config->models);
    discovery->config->models = entries;
    discovery->config->n_models = count;
    discovery->rc = AGENT_OK;
}

static int config_resolve_provider(Config* c);

static int model_config_copy(ModelConfig* dst, const ModelConfig* src,
                             const char* fallback_provider) {
    memset(dst, 0, sizeof(*dst));
    dst->name = src->name != NULL ? strdup(src->name) : NULL;
    dst->label = src->label != NULL ? strdup(src->label) : NULL;
    dst->provider = src->provider != NULL
                        ? strdup(src->provider)
                        : (fallback_provider != NULL ? strdup(fallback_provider) : NULL);
    dst->base_url = src->base_url != NULL ? strdup(src->base_url) : NULL;
    dst->api_key_env = src->api_key_env != NULL ? strdup(src->api_key_env) : NULL;
    dst->auth = src->auth != NULL ? strdup(src->auth) : NULL;
    dst->protocol = src->protocol != NULL ? strdup(src->protocol) : NULL;
    dst->models_path = src->models_path != NULL ? strdup(src->models_path) : NULL;
    dst->context_window = src->context_window;
    dst->max_output = src->max_output;
    dst->input_price = src->input_price;
    dst->output_price = src->output_price;
    dst->subscription = src->subscription;
    if (dst->name == NULL || dst->provider == NULL || (src->label != NULL && dst->label == NULL) ||
        (src->base_url != NULL && dst->base_url == NULL) ||
        (src->api_key_env != NULL && dst->api_key_env == NULL) ||
        (src->auth != NULL && dst->auth == NULL) ||
        (src->protocol != NULL && dst->protocol == NULL) ||
        (src->models_path != NULL && dst->models_path == NULL)) {
        model_config_free(dst);
        return AGENT_ERR_OOM;
    }
    return AGENT_OK;
}

static int model_config_append(Config* cfg, ModelConfig* entry) {
    ModelConfig* grown = realloc(cfg->models, (cfg->n_models + 1) * sizeof(*grown));
    if (grown == NULL) {
        return AGENT_ERR_OOM;
    }
    cfg->models = grown;
    cfg->models[cfg->n_models++] = *entry;
    memset(entry, 0, sizeof(*entry));
    return AGENT_OK;
}

static bool provider_name_seen(char* const* names, size_t count, const char* name) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static bool chatgpt_provider_seen(char* const* names, size_t count) {
    return provider_name_seen(names, count, "chatgpt") ||
           provider_name_seen(names, count, "openai-codex");
}

static const char* configured_chatgpt_provider(void) {
    static char path[PATH_MAX];
    if (oauth_default_path(path, sizeof(path)) != AGENT_OK) {
        return NULL;
    }
    /* Prefer chatgpt when both entries exist: auth.json may contain a stale
     * openai-codex entry alongside a still-valid legacy chatgpt token. */
    bool chatgpt = oauth_provider_configured(path, "chatgpt");
    bool codex = oauth_provider_configured(path, "openai-codex");
    if (chatgpt) {
        return "chatgpt";
    }
    if (codex) {
        return "openai-codex";
    }
    return NULL;
}

static bool config_has_chatgpt_model(const Config* cfg, const char* model_name) {
    if (cfg == NULL || model_name == NULL) {
        return false;
    }
    for (size_t i = 0; i < cfg->n_models; i++) {
        const char* provider = cfg->models[i].provider != NULL
                                   ? cfg->models[i].provider
                                   : cfg->provider;
        if (provider_name_is_chatgpt(provider) && cfg->models[i].name != NULL &&
            strcmp(cfg->models[i].name, model_name) == 0) {
            return true;
        }
    }
    return false;
}

static int config_add_chatgpt_fallbacks(Config* cfg) {
    if (cfg == NULL || configured_chatgpt_provider() == NULL) {
        return AGENT_OK;
    }

    /* Keep the current picker useful while the asynchronous /models request
     * is still running (or when it is temporarily unavailable). The live
     * catalog remains authoritative and replaces these entries afterwards. */
    static const char* const names[] = {"gpt-5.6-sol", "gpt-5.6-terra"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (config_has_chatgpt_model(cfg, names[i])) {
            continue;
        }
        ModelConfig fallback = {0};
        fallback.name = strdup(names[i]);
        fallback.provider = strdup("chatgpt");
        fallback.base_url = strdup(CHATGPT_CODEX_BASE_URL);
        fallback.protocol = strdup("responses");
        fallback.context_window = 272000;
        fallback.max_output = DEFAULT_MAX_OUTPUT;
        fallback.subscription = true;
        if (fallback.name == NULL || fallback.provider == NULL || fallback.base_url == NULL ||
            fallback.protocol == NULL || model_config_append(cfg, &fallback) != AGENT_OK) {
            model_config_free(&fallback);
            return AGENT_ERR_OOM;
        }
    }
    return AGENT_OK;
}

static int discover_provider_probe(const Config* source, const char* provider,
                                   const ModelConfig* hint, Config* probe) {
    *probe = config_default();
    free(probe->provider);
    probe->provider = strdup(provider);
    if (probe->provider == NULL) {
        config_free(probe);
        return AGENT_ERR_OOM;
    }
    const bool is_default = source->provider != NULL && strcmp(source->provider, provider) == 0;
    const char* base = hint != NULL && hint->base_url != NULL
                           ? hint->base_url
                           : (is_default ? source->base_url : NULL);
    const char* keyenv = hint != NULL && hint->api_key_env != NULL
                             ? hint->api_key_env
                             : (is_default ? source->api_key_env : NULL);
    const char* auth =
        hint != NULL && hint->auth != NULL ? hint->auth : (is_default ? source->auth : NULL);
    const char* protocol = hint != NULL && hint->protocol != NULL
                               ? hint->protocol
                               : (is_default ? source->protocol : NULL);
    const char* models_path = hint != NULL && hint->models_path != NULL
                                  ? hint->models_path
                                  : (is_default ? source->models_path : NULL);
    const char* selected = is_default ? source->model_name : (hint != NULL ? hint->name : NULL);
    probe->base_url = base != NULL ? strdup(base) : NULL;
    probe->api_key_env = keyenv != NULL ? strdup(keyenv) : NULL;
    probe->auth = auth != NULL ? strdup(auth) : NULL;
    probe->protocol = protocol != NULL ? strdup(protocol) : NULL;
    probe->models_path = models_path != NULL ? strdup(models_path) : NULL;
    probe->model_name = selected != NULL ? strdup(selected) : NULL;
    if ((base != NULL && probe->base_url == NULL) ||
        (keyenv != NULL && probe->api_key_env == NULL) || (auth != NULL && probe->auth == NULL) ||
        (protocol != NULL && probe->protocol == NULL) ||
        (models_path != NULL && probe->models_path == NULL) ||
        (selected != NULL && probe->model_name == NULL)) {
        config_free(probe);
        return AGENT_ERR_OOM;
    }
    return AGENT_OK;
}

/* ---- discovery failure reporting ---- */

static void detail_setf(char* detail, size_t cap, const char* fmt, ...) {
    if (detail == NULL || cap == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, cap, fmt, ap);
    va_end(ap);
}

static void append_failure(char* out, size_t cap, const char* provider, const char* reason) {
    if (out == NULL || cap == 0 || provider == NULL || reason == NULL) {
        return;
    }
    size_t len = strlen(out);
    if (len + 1 >= cap) {
        return;
    }
    snprintf(out + len, cap - len, "%s: %s; ", provider, reason);
}

int runtime_discover_models(Config* cfg, char* detail, size_t detail_cap) {
    if (detail != NULL && detail_cap > 0) {
        detail[0] = '\0';
    }
    if (cfg == NULL) {
        return AGENT_ERR_IO;
    }
    int rc = config_resolve_provider(cfg);
    if (rc != AGENT_OK) {
        detail_setf(detail, detail_cap, "invalid provider configuration (%s)", error_name(rc));
        return rc;
    }

    Provider* provider =
        provider_from_config(cfg->base_url, cfg->provider, cfg->api_key_env, cfg->auth);
    if (provider == NULL || provider->api_key == NULL) {
        detail_setf(detail, detail_cap, "%s", provider_auth_error(provider));
        provider_free(provider);
        return AGENT_ERR_AUTH;
    }

    EventLoop* loop = event_loop_new();
    HttpRuntime* http = loop != NULL ? http_new(loop) : NULL;
    if (http == NULL) {
        event_loop_free(loop);
        provider_free(provider);
        return AGENT_ERR_OOM;
    }

    String url = string_new();
    size_t base_len = strlen(cfg->base_url);
    while (base_len > 0 && cfg->base_url[base_len - 1] == '/') {
        base_len--;
    }
    const char* models_path = cfg->models_path != NULL ? cfg->models_path : "/models";
    if (string_append_n(&url, cfg->base_url, base_len) != AGENT_OK ||
        (models_path[0] == '/' ? string_append(&url, models_path)
                               : (string_append_char(&url, '/') == AGENT_OK &&
                                  string_append(&url, models_path) == AGENT_OK)) != AGENT_OK ||
        (provider_is_chatgpt(provider) &&
         string_append(&url, "?client_version=" CHATGPT_CLIENT_VERSION) != AGENT_OK)) {
        string_free(&url);
        http_free(http);
        event_loop_free(loop);
        provider_free(provider);
        return AGENT_ERR_OOM;
    }

    String auth = string_new();
    String account = string_new();
    const char* headers[6] = {"Accept: application/json", NULL, NULL, NULL, NULL, NULL};
    size_t n_headers = 1;
    if (cfg->protocol != NULL && strcmp(cfg->protocol, "anthropic") == 0) {
        if (string_printf(&auth, "x-api-key: %s", provider->api_key) != AGENT_OK) {
            rc = AGENT_ERR_OOM;
        }
        headers[n_headers++] = auth.data;
        headers[n_headers++] = "anthropic-version: 2023-06-01";
    } else {
        if (string_printf(&auth, "Authorization: Bearer %s", provider->api_key) != AGENT_OK) {
            rc = AGENT_ERR_OOM;
        }
        headers[n_headers++] = auth.data;
    }
    if (rc == AGENT_ERR_OOM) {
        string_free(&auth);
        string_free(&url);
        http_free(http);
        event_loop_free(loop);
        provider_free(provider);
        return rc;
    }
    if (provider_is_chatgpt(provider) && provider->account_id != NULL &&
        provider->account_id[0] != '\0') {
        if (string_printf(&account, "ChatGPT-Account-Id: %s", provider->account_id) != AGENT_OK) {
            string_free(&auth);
            string_free(&url);
            http_free(http);
            event_loop_free(loop);
            provider_free(provider);
            return AGENT_ERR_OOM;
        }
        headers[n_headers++] = account.data;
        headers[n_headers++] = "originator: codex_cli_rs";
    }

    ModelDiscovery discovery = {.config = cfg, .done = false, .rc = AGENT_ERR_HTTP};
    HttpRequest* request = NULL;
    rc = http_request_get(http, url.data, headers, n_headers, 1024 * 1024, NULL, NULL,
                          model_discovery_done, &discovery, &request);
    if (rc == AGENT_OK) {
        http_pump(http);
        time_t deadline = time(NULL) + MODEL_DISCOVERY_TIMEOUT_SECONDS;
        while (!discovery.done && time(NULL) < deadline) {
            long timeout = http_next_timeout_ms(http);
            if (timeout < 0 || timeout > 100) {
                timeout = 100;
            }
            (void)event_loop_wait(loop, (int)timeout);
            http_pump(http);
        }
        if (!discovery.done && request != NULL) {
            http_request_abort(http, request);
        }
        rc = discovery.rc;
    }

    if (rc != AGENT_OK) {
        if (discovery.http_status > 0) {
            detail_setf(detail, detail_cap, "HTTP %ld", discovery.http_status);
        } else if (discovery.curl_rc != CURLE_OK) {
            detail_setf(detail, detail_cap, "network error (%s)",
                        curl_easy_strerror(discovery.curl_rc));
        } else if (rc == AGENT_ERR_JSON) {
            detail_setf(detail, detail_cap, "invalid model list response");
        } else if (rc == AGENT_ERR_MODEL) {
            detail_setf(detail, detail_cap, "empty model list");
        } else {
            detail_setf(detail, detail_cap, "%s", error_name(rc));
        }
    }

    string_free(&account);
    string_free(&auth);
    string_free(&url);
    http_free(http);
    event_loop_free(loop);
    provider_free(provider);
    return rc;
}

int runtime_discover_all_models(Config* cfg, char* failures, size_t failures_cap,
                                volatile bool* cancel) {
    if (failures != NULL && failures_cap > 0) {
        failures[0] = '\0';
    }
    if (cfg == NULL) {
        return AGENT_ERR_IO;
    }
    int rc = config_resolve_provider(cfg);
    if (rc != AGENT_OK) {
        return rc;
    }

    size_t static_count = cfg->n_models;
    ModelConfig* static_models =
        static_count > 0 ? calloc(static_count, sizeof(*static_models)) : NULL;
    if (static_count > 0 && static_models == NULL) {
        return AGENT_ERR_OOM;
    }
    for (size_t i = 0; i < static_count; i++) {
        rc = model_config_copy(&static_models[i], &cfg->models[i], cfg->provider);
        if (rc != AGENT_OK) {
            for (size_t j = 0; j < i; j++)
                model_config_free(&static_models[j]);
            free(static_models);
            return rc;
        }
    }

    /* Discover the selected provider, providers explicitly named by model
     * entries, and ChatGPT when a complete OAuth login is present. This keeps
     * unrelated builtins lazy while making `cagent --login` visible in /model. */
    size_t name_cap = static_count + 3;
    char** providers = calloc(name_cap, sizeof(*providers));
    if (providers == NULL) {
        for (size_t i = 0; i < static_count; i++)
            model_config_free(&static_models[i]);
        free(static_models);
        return AGENT_ERR_OOM;
    }
    size_t provider_count = 0;
    providers[provider_count++] = strdup(cfg->provider);
    if (providers[0] == NULL) {
        free(providers);
        for (size_t i = 0; i < static_count; i++)
            model_config_free(&static_models[i]);
        free(static_models);
        return AGENT_ERR_OOM;
    }
    for (size_t i = 0; i < static_count; i++) {
        const char* provider =
            static_models[i].provider != NULL ? static_models[i].provider : cfg->provider;
        if (!provider_name_seen(providers, provider_count, provider)) {
            providers[provider_count] = strdup(provider);
            if (providers[provider_count] == NULL) {
                rc = AGENT_ERR_OOM;
                break;
            }
            provider_count++;
        }
    }
    const char* oauth_provider = configured_chatgpt_provider();
    if (rc == AGENT_OK && !chatgpt_provider_seen(providers, provider_count) &&
        oauth_provider != NULL) {
        providers[provider_count] = strdup(oauth_provider);
        if (providers[provider_count] == NULL) {
            rc = AGENT_ERR_OOM;
        } else {
            provider_count++;
        }
    }

    /* OpenCode Go is the default API-key subscription. Keep it discoverable
     * even after the user selects ChatGPT as the current provider, provided
     * credentials for it are actually available. This is what makes both
     * subscriptions appear in one model picker without probing unrelated
     * providers. */
    if (rc == AGENT_OK && !provider_name_seen(providers, provider_count, "opencode-go")) {
        char auth_path[PATH_MAX];
        if (oauth_default_path(auth_path, sizeof(auth_path)) != AGENT_OK) {
            auth_path[0] = '\0';
        }
        Provider* opencode = provider_new_auth(
            provider_builtin_base_url("opencode-go"), "opencode-go",
            auth_path[0] != '\0' ? auth_path : NULL, provider_builtin_key_env("opencode-go"));
        bool available = opencode != NULL && opencode->api_key != NULL &&
                         opencode->api_key[0] != '\0';
        provider_free(opencode);
        if (available) {
            providers[provider_count] = strdup("opencode-go");
            if (providers[provider_count] == NULL) {
                rc = AGENT_ERR_OOM;
            } else {
                provider_count++;
            }
        }
    }

    for (size_t i = 0; i < cfg->n_models; i++)
        model_config_free(&cfg->models[i]);
    free(cfg->models);
    cfg->models = NULL;
    cfg->n_models = 0;

    if (rc == AGENT_OK) {
        for (size_t p = 0; p < provider_count; p++) {
            if (cancel != NULL && *cancel) {
                break;
            }
            const char* provider = providers[p];
            const ModelConfig* hint = NULL;
            for (size_t i = 0; i < static_count; i++) {
                if (static_models[i].provider != NULL &&
                    strcmp(static_models[i].provider, provider) == 0) {
                    hint = &static_models[i];
                    break;
                }
            }
            Config probe;
            if (discover_provider_probe(cfg, provider, hint, &probe) != AGENT_OK) {
                rc = AGENT_ERR_OOM;
                break;
            }
            char detail[128];
            int discover_rc = runtime_discover_models(&probe, detail, sizeof(detail));
            if (discover_rc == AGENT_OK) {
                if (strcmp(provider, cfg->provider) == 0 && probe.model_name != NULL) {
                    free(cfg->model_name);
                    cfg->model_name = strdup(probe.model_name);
                    if (cfg->model_name == NULL) {
                        config_free(&probe);
                        rc = AGENT_ERR_OOM;
                        break;
                    }
                }
                for (size_t i = 0; i < probe.n_models; i++) {
                    if (model_config_append(cfg, &probe.models[i]) != AGENT_OK) {
                        rc = AGENT_ERR_OOM;
                        break;
                    }
                }
            } else {
                const char* reason = detail[0] != '\0' ? detail : error_name(discover_rc);
                log_warn("model discovery failed for provider %s: %s", provider, reason);
                append_failure(failures, failures_cap, provider, reason);
                for (size_t i = 0; i < static_count; i++) {
                    const char* static_provider = static_models[i].provider != NULL
                                                      ? static_models[i].provider
                                                      : cfg->provider;
                    if (strcmp(static_provider, provider) != 0)
                        continue;
                    ModelConfig copy;
                    if (model_config_copy(&copy, &static_models[i], cfg->provider) != AGENT_OK ||
                        model_config_append(cfg, &copy) != AGENT_OK) {
                        model_config_free(&copy);
                        rc = AGENT_ERR_OOM;
                        break;
                    }
                }
            }
            config_free(&probe);
            if (rc != AGENT_OK)
                break;
        }
    }

    for (size_t i = 0; i < static_count; i++)
        model_config_free(&static_models[i]);
    free(static_models);
    for (size_t i = 0; i < provider_count; i++)
        free(providers[i]);
    free(providers);
    return rc;
}

static int config_resolve_provider(Config* c) {
    if (c == NULL) {
        return AGENT_ERR_IO;
    }
    if (c->provider == NULL && legacy_auth_is_chatgpt(c->auth)) {
        c->provider = strdup("chatgpt");
    }
    if (c->provider == NULL) {
        char auth_path[PATH_MAX];
        const char* home = getenv("HOME");
        if (home != NULL && oauth_default_path(auth_path, sizeof(auth_path)) == AGENT_OK) {
            FILE* auth_file = fopen(auth_path, "rb");
            if (auth_file != NULL) {
                if (fseek(auth_file, 0, SEEK_END) == 0) {
                    long size = ftell(auth_file);
                    if (size > 0 && size <= 1024 * 1024 && fseek(auth_file, 0, SEEK_SET) == 0) {
                        char* data = malloc((size_t)size + 1);
                        if (data != NULL) {
                            size_t n = fread(data, 1, (size_t)size, auth_file);
                            data[n] = '\0';
                            JsonDoc* doc = json_parse(data, n);
                            JsonVal* root = doc != NULL ? json_root(doc) : NULL;
                            const char* auth_provider =
                                root != NULL ? json_obj_get_str(root, "provider") : NULL;
                            if (auth_provider != NULL) {
                                c->provider = strdup(auth_provider);
                            }
                            json_doc_free(doc);
                            free(data);
                        }
                    }
                }
                fclose(auth_file);
            }
        }
        if (c->provider == NULL) {
            c->provider = strdup("opencode-go");
        }
        if (c->provider == NULL) {
            return AGENT_ERR_OOM;
        }
    }

    const char* builtin_base = provider_builtin_base_url(c->provider);
    const char* builtin_protocol = provider_builtin_protocol(c->provider);
    const char* builtin_key = provider_builtin_key_env(c->provider);
    if (builtin_base != NULL) {
        /* `config_save_selection()` historically persisted only provider and
         * model. If the user switched subscriptions, base_url/protocol/key
         * can therefore belong to the previous builtin provider. Do not send
         * a ChatGPT OAuth token to the OpenCode endpoint (or vice versa). */
        const char* base_provider = builtin_provider_for_base_url(c->base_url);
        if (base_provider != NULL && !provider_names_equal(base_provider, c->provider)) {
            free(c->base_url);
            c->base_url = NULL;
            free(c->protocol);
            c->protocol = NULL;
            free(c->api_key_env);
            c->api_key_env = NULL;
            free(c->models_path);
            c->models_path = NULL;
        }
        if (c->base_url == NULL) {
            c->base_url = strdup(builtin_base);
        }
        if (c->protocol == NULL && builtin_protocol != NULL) {
            c->protocol = strdup(builtin_protocol);
        }
        if (c->api_key_env == NULL && builtin_key != NULL) {
            c->api_key_env = strdup(builtin_key);
        }
        return c->base_url != NULL && c->protocol != NULL ? AGENT_OK : AGENT_ERR_OOM;
    }

    const char* home = getenv("HOME");
    if (home == NULL) {
        return AGENT_ERR_IO;
    }
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/.config/cagent/providers.json", home) >=
        (int)sizeof(path)) {
        return AGENT_ERR_IO;
    }
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return AGENT_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return AGENT_ERR_IO;
    }
    long size = ftell(f);
    if (size < 0 || size > 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return AGENT_ERR_IO;
    }
    char* data = malloc((size_t)size + 1);
    if (data == NULL) {
        fclose(f);
        return AGENT_ERR_OOM;
    }
    size_t n = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[n] = '\0';
    JsonDoc* doc = json_parse(data, n);
    free(data);
    JsonVal* root = doc != NULL ? json_root(doc) : NULL;
    JsonVal* providers = root != NULL ? json_val_obj_get(root, "providers") : NULL;
    JsonVal* spec = providers != NULL ? json_val_obj_get(providers, c->provider) : NULL;
    if (spec == NULL && root != NULL) {
        spec = json_val_obj_get(root, c->provider);
    }
    if (spec == NULL || !json_val_is_obj(spec)) {
        json_doc_free(doc);
        return AGENT_ERR_IO;
    }
    const char* v = json_obj_get_str(spec, "base_url");
    if (c->base_url == NULL && v != NULL) {
        c->base_url = strdup(v);
    }
    v = json_obj_get_str(spec, "protocol");
    if (c->protocol == NULL && v != NULL) {
        c->protocol = strdup(v);
    }
    v = json_obj_get_str(spec, "models_path");
    if (c->models_path == NULL && v != NULL) {
        c->models_path = strdup(v);
    }
    v = json_obj_get_str(spec, "api_key_env");
    if (c->api_key_env == NULL && v != NULL) {
        c->api_key_env = strdup(v);
    }
    json_doc_free(doc);
    return c->base_url != NULL && c->protocol != NULL ? AGENT_OK : AGENT_ERR_JSON;
}

/* Build the extra-model table (rt->models) from rt->config.models. Each
 * table model owns its provider (unlike the default model, whose provider
 * is rt->provider). Entries that cannot build a provider or model are
 * skipped. Returns AGENT_OK or AGENT_ERR_OOM. */
static int runtime_build_models(Runtime* rt) {
    if (rt->config.n_models > 0) {
        rt->models = (Model**)calloc(rt->config.n_models, sizeof(Model*));
        if (rt->models == NULL) {
            return AGENT_ERR_OOM;
        }
        for (size_t i = 0; i < rt->config.n_models; i++) {
            ModelConfig* mc = &rt->config.models[i];
            const char* mprovider = mc->provider != NULL ? mc->provider : rt->config.provider;
            bool same_provider = provider_names_equal(mprovider, rt->config.provider);
            /* Credentials are scoped to a provider.  In particular, a
             * legacy global ChatGPT auth setting must not be inherited by a
             * model explicitly assigned to another provider. */
            const char* mauth = mc->auth != NULL ? mc->auth : (same_provider ? rt->config.auth : NULL);
            const char* base = mc->base_url != NULL ? mc->base_url : rt->config.base_url;
            if (mc->base_url == NULL && mprovider != NULL &&
                (rt->config.provider == NULL || strcmp(mprovider, rt->config.provider) != 0)) {
                const char* builtin = provider_builtin_base_url(mprovider);
                if (builtin != NULL)
                    base = builtin;
            }
            const char* keyenv = mc->api_key_env != NULL
                                     ? mc->api_key_env
                                     : (same_provider ? rt->config.api_key_env
                                                       : provider_builtin_key_env(mprovider));
            Provider* p = provider_from_config(base, mprovider, keyenv, mauth);
            if (p == NULL) {
                continue;
            }
            int64_t window =
                mc->context_window > 0 ? mc->context_window : rt->config.context_window;
            int64_t out = mc->max_output > 0 ? mc->max_output : rt->config.max_output;
            const char* mproto =
                mc->protocol != NULL
                    ? mc->protocol
                    : (provider_is_oauth(p) ? "responses"
                                            : (provider_builtin_protocol(mprovider) != NULL
                                                   ? provider_builtin_protocol(mprovider)
                                                   : "openai"));
            Model* m;
            if (strcmp(mproto, "anthropic") == 0) {
                m = anthropic_model_new(p, mc->name, window, out);
            } else if (strcmp(mproto, "responses") == 0) {
                m = responses_model_new(p, mc->name, window, out);
            } else {
                m = openai_model_new(p, mc->name, window, out);
            }
            if (m == NULL) {
                provider_free(p);
                continue;
            }
            m->runtime = rt;
            m->input_price = mc->input_price;
            m->output_price = mc->output_price;
            m->subscription = mc->subscription;
            m->window_verified = mc->context_window > 0 || rt->config.context_window_set;
            rt->models[rt->n_models++] = m;
        }
    }
    return AGENT_OK;
}

/* Replace the runtime's model catalog with a freshly discovered one
 * (typically produced by runtime_discover_all_models). The runtime deep-
 * copies the new table, rebuilds the live model objects and frees the old
 * ones. The default model (rt->model) is left untouched so the user's
 * current selection survives the swap. On OOM the runtime state is
 * unchanged. */
int runtime_apply_catalog(Runtime* rt, const Config* catalog) {
    if (rt == NULL || catalog == NULL) {
        return AGENT_ERR_IO;
    }

    ModelConfig* models = NULL;
    if (catalog->n_models > 0) {
        models = calloc(catalog->n_models, sizeof(ModelConfig));
        if (models == NULL) {
            return AGENT_ERR_OOM;
        }
        for (size_t i = 0; i < catalog->n_models; i++) {
            ModelConfig* dst = &models[i];
            const ModelConfig* src = &catalog->models[i];
            dst->name = strdup(src->name);
            dst->label = src->label != NULL ? strdup(src->label) : NULL;
            dst->provider = src->provider != NULL ? strdup(src->provider) : NULL;
            dst->base_url = src->base_url != NULL ? strdup(src->base_url) : NULL;
            dst->api_key_env = src->api_key_env != NULL ? strdup(src->api_key_env) : NULL;
            dst->auth = src->auth != NULL ? strdup(src->auth) : NULL;
            dst->protocol = src->protocol != NULL ? strdup(src->protocol) : NULL;
            dst->models_path = src->models_path != NULL ? strdup(src->models_path) : NULL;
            dst->context_window = src->context_window;
            dst->max_output = src->max_output;
            dst->input_price = src->input_price;
            dst->output_price = src->output_price;
            dst->subscription = src->subscription;
            if (dst->name == NULL || dst->provider == NULL || dst->base_url == NULL ||
                dst->protocol == NULL) {
                for (size_t j = 0; j <= i; j++) {
                    model_config_free(&models[j]);
                }
                free(models);
                return AGENT_ERR_OOM;
            }
        }
    }

    /* swap the catalog, rebuild the live table, roll back on OOM */
    ModelConfig* old_cfg_models = rt->config.models;
    size_t old_cfg_n = rt->config.n_models;
    Model** old_models = rt->models;
    size_t old_n = rt->n_models;

    rt->config.models = models;
    rt->config.n_models = catalog->n_models;
    rt->models = NULL;
    rt->n_models = 0;
    if (runtime_build_models(rt) != AGENT_OK) {
        rt->config.models = old_cfg_models;
        rt->config.n_models = old_cfg_n;
        for (size_t i = 0; i < catalog->n_models; i++) {
            model_config_free(&models[i]);
        }
        free(models);
        rt->models = old_models;
        rt->n_models = old_n;
        return AGENT_ERR_OOM;
    }

    for (size_t i = 0; i < old_n; i++) {
        Model* m = old_models[i];
        if (m != NULL) {
            Provider* p = m->provider;
            m->ops->destroy(m);
            provider_free(p);
        }
    }
    free(old_models);
    for (size_t i = 0; i < old_cfg_n; i++) {
        model_config_free(&old_cfg_models[i]);
    }
    free(old_cfg_models);

    /* The default model (rt->model) is deliberately kept alive across the
     * swap so in-flight agents never dangle, but its per-model fields
     * must follow the freshly discovered catalog: the live /models
     * response carries the authoritative context_window (and optionally
     * pricing), while rt->model was built from local config/defaults.
     * Providers store these fields only on the Model struct, so updating
     * them in place is safe. */
    if (rt->model != NULL && rt->model->name != NULL) {
        const char* bare = rt->model->name;
        const char* prefixed = NULL;
        char full[512];
        if (rt->config.provider != NULL) {
            snprintf(full, sizeof(full), "%s/%s", rt->config.provider, bare);
            prefixed = full;
        }
        for (size_t i = 0; i < rt->config.n_models; i++) {
            ModelConfig* mc = &rt->config.models[i];
            bool match = mc->name != NULL &&
                         (strcmp(mc->name, bare) == 0 ||
                          (prefixed != NULL && strcmp(mc->name, prefixed) == 0));
            if (!match) {
                continue;
            }
            if (mc->context_window > 0) {
                rt->model->context_window = mc->context_window;
                rt->model->window_verified = true;
            }
            if (mc->max_output > 0) {
                rt->model->max_output = mc->max_output;
            }
            if (mc->input_price != 0 || mc->output_price != 0 || mc->subscription) {
                rt->model->input_price = mc->input_price;
                rt->model->output_price = mc->output_price;
                rt->model->subscription = mc->subscription;
            }
            break;
        }
    }
    return AGENT_OK;
}

Runtime* runtime_new(const Config* cfg) {
    Runtime* rt = calloc(1, sizeof(Runtime));
    if (rt == NULL) {
        return NULL;
    }

    /* config copy */
    rt->config = config_default();
    if (cfg != NULL) {
        free(rt->config.provider);
        free(rt->config.base_url);
        free(rt->config.api_key_env);
        free(rt->config.model_name);
        free(rt->config.auth);
        free(rt->config.models_path);
        free(rt->config.cwd);
        rt->config.provider = cfg->provider != NULL ? strdup(cfg->provider) : NULL;
        rt->config.base_url = cfg->base_url != NULL ? strdup(cfg->base_url) : NULL;
        rt->config.api_key_env = cfg->api_key_env != NULL ? strdup(cfg->api_key_env) : NULL;
        rt->config.auth = cfg->auth != NULL ? strdup(cfg->auth) : NULL;
        rt->config.protocol = cfg->protocol != NULL ? strdup(cfg->protocol) : NULL;
        rt->config.models_path = cfg->models_path != NULL ? strdup(cfg->models_path) : NULL;
        rt->config.model_name = cfg->model_name != NULL ? strdup(cfg->model_name) : NULL;
        rt->config.cwd = cfg->cwd != NULL ? strdup(cfg->cwd) : NULL;
        if (cfg->max_tokens > 0) {
            rt->config.max_tokens = cfg->max_tokens;
        }
        if (cfg->context_window > 0) {
            rt->config.context_window = cfg->context_window;
        }
        if (cfg->context_window_set) {
            rt->config.context_window_set = true;
        }
        if (cfg->max_output > 0) {
            rt->config.max_output = cfg->max_output;
        }
        rt->config.input_price = cfg->input_price;
        rt->config.output_price = cfg->output_price;
        rt->config.subscription = cfg->subscription;
        if (cfg->max_concurrent_agents > 0) {
            rt->config.max_concurrent_agents = cfg->max_concurrent_agents;
        }
        if (cfg->max_retries >= 0) {
            rt->config.max_retries = cfg->max_retries;
        }
        if (cfg->project_memory_max_bytes_set || cfg->project_memory_max_bytes > 0) {
            rt->config.project_memory_max_bytes = cfg->project_memory_max_bytes;
            rt->config.project_memory_max_bytes_set =
                cfg->project_memory_max_bytes_set || cfg->project_memory_max_bytes > 0;
        }
        if (rt->config.base_url == NULL && cfg->base_url != NULL) {
            runtime_free(rt);
            return NULL;
        }
        /* deep-copy the model table */
        if (cfg->n_models > 0) {
            rt->config.models = calloc(cfg->n_models, sizeof(ModelConfig));
            if (rt->config.models == NULL) {
                runtime_free(rt);
                return NULL;
            }
            for (size_t i = 0; i < cfg->n_models; i++) {
                ModelConfig* dst = &rt->config.models[i];
                const ModelConfig* src = &cfg->models[i];
                dst->name = strdup(src->name);
                dst->label = src->label != NULL ? strdup(src->label) : NULL;
                dst->provider = src->provider != NULL ? strdup(src->provider) : NULL;
                dst->base_url = src->base_url != NULL ? strdup(src->base_url) : NULL;
                dst->api_key_env = src->api_key_env != NULL ? strdup(src->api_key_env) : NULL;
                dst->auth = src->auth != NULL ? strdup(src->auth) : NULL;
                dst->protocol = src->protocol != NULL ? strdup(src->protocol) : NULL;
                dst->models_path = src->models_path != NULL ? strdup(src->models_path) : NULL;
                dst->context_window = src->context_window;
                dst->max_output = src->max_output;
                dst->input_price = src->input_price;
                dst->output_price = src->output_price;
                dst->subscription = src->subscription;
            }
            rt->config.n_models = cfg->n_models;
        }
    }
    if (config_resolve_provider(&rt->config) != AGENT_OK) {
        runtime_free(rt);
        return NULL;
    }
    if (config_add_chatgpt_fallbacks(&rt->config) != AGENT_OK) {
        runtime_free(rt);
        return NULL;
    }
    if (rt->config.model_name == NULL) {
        rt->config.model_name = strdup(DEFAULT_MODEL);
    }
    if (rt->config.cwd == NULL) {
        char buf[4096];
        if (getcwd(buf, sizeof(buf)) != NULL) {
            rt->config.cwd = strdup(buf);
        }
    }
    if (rt->config.base_url == NULL || rt->config.model_name == NULL ||
        rt->config.provider == NULL) {
        runtime_free(rt);
        return NULL;
    }

    /* provider + model + tools */
    rt->provider = provider_from_config(rt->config.base_url, rt->config.provider,
                                        rt->config.api_key_env, rt->config.auth);
    if (rt->provider == NULL) {
        runtime_free(rt);
        return NULL;
    }
    /* Normalize persisted provider/name selectors to the bare API model id,
     * including offline startup when live discovery is unavailable. */
    if (rt->config.model_name != NULL && rt->config.provider != NULL) {
        size_t provider_len = strlen(rt->config.provider);
        if (strncmp(rt->config.model_name, rt->config.provider, provider_len) == 0 &&
            rt->config.model_name[provider_len] == '/') {
            char* bare = strdup(rt->config.model_name + provider_len + 1);
            if (bare == NULL) {
                runtime_free(rt);
                return NULL;
            }
            free(rt->config.model_name);
            rt->config.model_name = bare;
        }
    }

    /* resolve a default model LABEL to the actual API name (e.g.
     * --model fast -> kimi-k3) */
    if (rt->config.model_name != NULL) {
        for (size_t i = 0; i < rt->config.n_models; i++) {
            if (rt->config.models[i].label != NULL &&
                strcmp(rt->config.models[i].label, rt->config.model_name) == 0) {
                free(rt->config.model_name);
                rt->config.model_name = strdup(rt->config.models[i].name);
                break;
            }
        }
    }

    /* carry per-model billing onto the runtime config for the default
     * model; a named entry overrides the top-level price/subscription */
    if (rt->config.model_name != NULL) {
        for (size_t i = 0; i < rt->config.n_models; i++) {
            if (rt->config.models[i].name != NULL &&
                strcmp(rt->config.models[i].name, rt->config.model_name) == 0) {
                rt->config.input_price = rt->config.models[i].input_price;
                rt->config.output_price = rt->config.models[i].output_price;
                rt->config.subscription = rt->config.models[i].subscription;
                break;
            }
        }
    }

    const char* proto = rt->config.protocol != NULL
                            ? rt->config.protocol
                            : (provider_is_oauth(rt->provider) ? "responses" : "openai");
    if (strcmp(proto, "anthropic") == 0) {
        rt->model = anthropic_model_new(rt->provider, rt->config.model_name,
                                        rt->config.context_window, rt->config.max_output);
    } else if (strcmp(proto, "responses") == 0) {
        rt->model = responses_model_new(rt->provider, rt->config.model_name,
                                        rt->config.context_window, rt->config.max_output);
    } else {
        rt->model = openai_model_new(rt->provider, rt->config.model_name, rt->config.context_window,
                                     rt->config.max_output);
    }
    if (rt->model == NULL) {
        runtime_free(rt);
        return NULL;
    }
    rt->model->input_price = rt->config.input_price;
    rt->model->output_price = rt->config.output_price;
    rt->model->subscription = rt->config.subscription;
    rt->model->window_verified = rt->config.context_window_set;

    /* extra named models: each gets its own provider (endpoint + key env) */
    if (runtime_build_models(rt) != AGENT_OK) {
        runtime_free(rt);
        return NULL;
    }

    rt->tools = tool_registry_new();
    if (rt->tools == NULL) {
        runtime_free(rt);
        return NULL;
    }
    register_builtin_tools(rt->tools);

    /* Phase 4 async runtime */
    rt->loop = event_loop_new();
    if (rt->loop == NULL) {
        runtime_free(rt);
        return NULL;
    }
    rt->http = http_new(rt->loop);
    if (rt->http == NULL) {
        runtime_free(rt);
        return NULL;
    }
    rt->scheduler = scheduler_new((size_t)rt->config.max_concurrent_agents);
    if (rt->scheduler == NULL) {
        runtime_free(rt);
        return NULL;
    }
    rt->model->runtime = rt; /* the provider reaches the HTTP channel */

    return rt;
}

int runtime_model_selector(const Runtime* rt, const Model* model, char* out, size_t cap) {
    if (rt == NULL || model == NULL || model->name == NULL || out == NULL || cap == 0) {
        return AGENT_ERR_IO;
    }
    const char* provider = model->provider != NULL ? model->provider->provider_name : NULL;
    if (provider == NULL || provider[0] == '\0') {
        provider = rt->config.provider != NULL ? rt->config.provider : "unknown";
    }
    size_t provider_len = strlen(provider);
    if (strncmp(model->name, provider, provider_len) == 0 && model->name[provider_len] == '/') {
        if (snprintf(out, cap, "%s", model->name) >= (int)cap)
            return AGENT_ERR_IO;
    } else if (snprintf(out, cap, "%s/%s", provider, model->name) >= (int)cap) {
        return AGENT_ERR_IO;
    }
    return AGENT_OK;
}

static bool runtime_model_matches(const Runtime* rt, const Model* model, const char* selector) {
    char formatted[PATH_MAX];
    if (runtime_model_selector(rt, model, formatted, sizeof(formatted)) == AGENT_OK &&
        strcmp(formatted, selector) == 0) {
        return true;
    }
    return model->name != NULL && strcmp(model->name, selector) == 0;
}

Model* runtime_model_by_name(Runtime* rt, const char* name) {
    if (rt == NULL || name == NULL) {
        return NULL;
    }
    if (rt->model != NULL && runtime_model_matches(rt, rt->model, name)) {
        return rt->model;
    }
    for (size_t i = 0; i < rt->n_models; i++) {
        Model* m = rt->models[i];
        if (m == NULL) {
            continue;
        }
        if (runtime_model_matches(rt, m, name)) {
            return m;
        }
        /* label aliases remain accepted for compatibility. */
        if (i < rt->config.n_models && rt->config.models[i].label != NULL &&
            strcmp(rt->config.models[i].label, name) == 0) {
            return m;
        }
    }
    return NULL;
}

int runtime_set_base_url(Runtime* rt, const char* url) {
    if (rt == NULL || url == NULL || rt->provider == NULL) {
        return AGENT_ERR_IO;
    }
    char* copy = strdup(url);
    if (copy == NULL) {
        return AGENT_ERR_OOM;
    }
    free(rt->provider->base_url);
    rt->provider->base_url = copy;
    free(rt->config.base_url);
    rt->config.base_url = strdup(url); /* best effort */
    return AGENT_OK;
}

int runtime_set_api_key(Runtime* rt, const char* key) {
    if (rt == NULL || key == NULL || rt->provider == NULL) {
        return AGENT_ERR_IO;
    }
    char* copy = strdup(key);
    if (copy == NULL) {
        return AGENT_ERR_OOM;
    }
    /* replace the stored secret: never log the old or new value */
    free(rt->provider->api_key);
    rt->provider->api_key = copy;
    return AGENT_OK;
}

void runtime_pump(Runtime* rt, int timeout_ms) {
    if (rt == NULL) {
        return;
    }
    /* the event loop waits at most until the HTTP timer fires */
    long http_timer = http_next_timeout_ms(rt->http);
    int to = timeout_ms;
    if (http_timer >= 0 && http_timer < to) {
        to = (int)http_timer;
    }
    event_loop_wait(rt->loop, to);
    http_pump(rt->http);
    scheduler_pump(rt->scheduler);
}

void runtime_free(Runtime* rt) {
    if (rt == NULL) {
        return;
    }
    if (rt->model != NULL) {
        rt->model->ops->destroy(rt->model);
    }
    for (size_t i = 0; i < rt->n_models; i++) {
        Model* m = rt->models[i];
        if (m != NULL) {
            /* table models own their provider (unlike the default model,
             * whose provider is rt->provider) */
            Provider* p = m->provider;
            m->ops->destroy(m);
            provider_free(p);
        }
    }
    free((void*)rt->models);
    provider_free(rt->provider);
    tool_registry_free(rt->tools);
    scheduler_free(rt->scheduler);
    http_free(rt->http);
    event_loop_free(rt->loop);
    config_free(&rt->config);
    free(rt);
}
