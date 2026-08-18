/*
 * runtime/runtime.h — the Runtime (DESIGN.md §3.10, Phase 1 shape).
 *
 * The Runtime owns the provider, the model table, the tool registry and
 * the configuration. Agents borrow it. Phase 4 adds the event loop,
 * worker pool and scheduler here without changing the agent contract.
 *
 * Ownership:
 *   - Config strings are owned; config_free() releases them.
 *   - Runtime owns everything it points to; runtime_free() releases it.
 */

#ifndef CAGENT_RUNTIME_RUNTIME_H
#define CAGENT_RUNTIME_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "model/model.h"
#include "tool/registry.h"
#include "util/error.h"

/* One named model entry in the config (DESIGN.md §7). */
typedef struct {
    char* name;             /* owned; API model name, e.g. gpt-4.1-mini */
    char* label;            /* owned; selector alias; NULL = name */
    char* provider;         /* owned; builtin or custom provider name */
    char* base_url;         /* owned; NULL -> provider default endpoint */
    char* api_key_env;      /* owned; legacy override; auth.json is preferred */
    char* auth;             /* owned; legacy auth string */
    char* protocol;         /* owned; "openai" (default), "anthropic", or "responses" */
    char* models_path;      /* owned; custom provider model catalog path */
    int64_t context_window; /* 0 -> global default */
    int64_t max_output;     /* 0 -> global default */
    double input_price;     /* USD per 1M input tokens; 0 = unknown */
    double output_price;    /* USD per 1M output tokens; 0 = unknown */
    bool subscription;      /* flat-plan model: footer shows (sub) not $ */
} ModelConfig;

typedef struct {
    char* provider;    /* owned; builtin or custom provider name */
    char* base_url;    /* owned; NULL -> provider default */
    char* api_key_env; /* owned; legacy override; auth.json is preferred */
    char* auth;        /* owned; legacy auth string */
    char* protocol;    /* owned; "openai" (default), "anthropic", or "responses" */
    char* models_path; /* owned; custom provider model catalog path */
    char* model_name;  /* owned; NULL -> default model */
    char* cwd;         /* owned; NULL -> current directory */
    int64_t max_tokens;
    int64_t context_window;
    int64_t max_output;
    int64_t project_memory_max_bytes; /* 0 disables; default 4096 */
    bool project_memory_max_bytes_set; /* true when explicitly configured */
    bool context_window_set; /* true when the config file sets context_window */
    double input_price;  /* USD per 1M input tokens; 0 = unknown */
    double output_price; /* USD per 1M output tokens; 0 = unknown */
    bool subscription;   /* flat-plan model: footer shows (sub) not $ */
    int64_t max_concurrent_agents; /* scheduler limit; 0 -> default 16 */
    int64_t max_retries;           /* transient model request retries; default 2 */
    ModelConfig* models;           /* owned; extra named models; may be NULL */
    size_t n_models;
} Config;

void model_config_free(ModelConfig* m);

Config config_default(void); /* zeroed strings; use config_set_* to fill */
void config_free(Config* c);

/* Overlay fields present in a JSON config file. Missing file/keys keep
 * current values. Returns AGENT_OK (even when the file is absent). */
int config_load_file(Config* c, const char* path);
/* Atomically update the persisted default model while preserving all other
 * JSON settings. The path is normally ~/.config/cagent/config.json or the
 * value supplied by --config. */
int config_save_model(const char* path, const char* model_name);
/* Persist the active provider separately from the bare API model name. */
int config_save_selection(const char* path, const char* provider_name, const char* model_name);

typedef struct Runtime {
    Provider* provider; /* owned; default endpoint */
    Model* model;       /* owned; default model (config.model_name) */
    Model** models;     /* owned; extra named models (config.models) */
    size_t n_models;
    ToolRegistry* tools; /* owned */
    Config config;       /* owned */
    /* Phase 4 async runtime */
    struct EventLoop* loop;      /* owned */
    struct HttpRuntime* http;    /* owned */
    struct Scheduler* scheduler; /* owned */
} Runtime;

/* Format the unique selector used by the TUI: provider/model. */
int runtime_model_selector(const Runtime* rt, const Model* model, char* out, size_t cap);
/* Look up a model by provider/model selector, with bare-name compatibility. */
Model* runtime_model_by_name(Runtime* rt, const char* name);

/* Runtime provider overrides (in-memory; used by the TUI /settings
 * commands — DESIGN.md §40: the app opens without a key and the user
 * configures it interactively). Secrets are never logged. */
int runtime_set_base_url(Runtime* rt, const char* url);
int runtime_set_api_key(Runtime* rt, const char* key);

/* Drive the runtime: wait for events (up to timeout_ms), pump the shared
 * HTTP runtime, then advance all scheduled agents. */
void runtime_pump(Runtime* rt, int timeout_ms);

/* Refresh the selected provider's model catalog from its /models endpoint.
 * This is a live request: no catalog data is persisted or cached. Existing
 * config entries remain on failure. When `detail` is non-NULL it receives a
 * short human-readable failure reason ("HTTP 403", "network error (...)",
 * "authentication failed") and is set to an empty string on success. */
int runtime_discover_models(Config* cfg, char* detail, size_t detail_cap);
/* Discover the live catalogs for the default provider and every provider
 * declared by a configured model entry. Failed providers retain static
 * entries. When `failures` is non-NULL it receives "provider: reason; "
 * entries for every failed provider (empty when all succeed). When
 * `cancel` is non-NULL the loop checks it before each provider probe and
 * stops early once it is set (the in-flight probe still runs its bound). */
int runtime_discover_all_models(Config* cfg, char* failures, size_t failures_cap,
                                volatile bool* cancel);

/* Swap in a discovered catalog: the runtime deep-copies the table, rebuilds
 * the live model objects and frees the previous ones. The default model
 * (user's current selection) is preserved. On OOM the runtime is unchanged. */
int runtime_apply_catalog(Runtime* rt, const Config* catalog);

/* Builds the provider (env key), the default model (openai-compatible)
 * and the builtin tool registry. */
Runtime* runtime_new(const Config* cfg);
void runtime_free(Runtime* rt);

#endif /* CAGENT_RUNTIME_RUNTIME_H */
