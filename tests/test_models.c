/*
 * tests/test_models.c — multi-model support tests (DESIGN.md §7).
 *
 * Covers: config "models" array parsing, runtime_model_by_name lookup
 * (name + label), default fallback, agent model selection, and the
 * subagent tool's per-task model passthrough.
 */

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agent/agent.h"
#include "mock_model.h"
#include "model/provider.h"
#include "runtime/runtime.h"
#include "test_common.h"
#include "test_server.h"
#include "util/error.h"
#include "util/project_context.h"

static char g_tmpdir[256];

static int test_config_models_parsing(void) {
    char path[600];
    snprintf(path, sizeof(path), "%s/config.json", g_tmpdir);
    FILE* f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("{\n"
              "  \"max_retries\": 3,\n"
              "  \"context_window\": 272000,\n"
              "  \"project_memory_max_bytes\": 2048,\n"
              "  \"price_in\": 0.2,\n"
              "  \"price_out\": 0.8,\n"
              "  \"models\": [\n"
              "    {\"name\": \"opencode-go/kimi-k3\", \"label\": \"fast\",\n"
              "     \"base_url\": \"https://opencode.ai/zen/go/v1\",\n"
              "     \"api_key_env\": \"OPENCODE_GO_API_KEY\",\n"
              "     \"context_window\": 200000, \"max_output\": 16384,\n"
              "     \"price_in\": 0.15, \"price_out\": 0.6, \"subscription\": false},\n"
              "    {\"name\": \"opencode-go/deepseek-v4-pro\",\n"
              "     \"context_window\": 128000, \"subscription\": true}\n"
              "  ]\n"
              "}\n",
              f);
        fclose(f);
    }

    Config c = config_default();
    CHECK(config_load_file(&c, path) == AGENT_OK);
    CHECK(c.n_models == 2);
    CHECK(c.max_retries == 3);
    CHECK(c.context_window == 272000);
    CHECK(c.context_window_set);
    CHECK(c.project_memory_max_bytes == 2048);
    CHECK(c.project_memory_max_bytes_set);
    CHECK(c.input_price == 0.2);
    CHECK(c.output_price == 0.8);
    CHECK(strcmp(c.models[0].name, "opencode-go/kimi-k3") == 0);
    CHECK(strcmp(c.models[0].label, "fast") == 0);
    CHECK(strcmp(c.models[0].base_url, "https://opencode.ai/zen/go/v1") == 0);
    CHECK(strcmp(c.models[0].api_key_env, "OPENCODE_GO_API_KEY") == 0);
    CHECK(c.models[0].context_window == 200000);
    CHECK(c.models[0].max_output == 16384);
    CHECK(c.models[0].input_price == 0.15);
    CHECK(c.models[0].output_price == 0.6);
    CHECK(!c.models[0].subscription);
    CHECK(c.models[1].context_window == 128000);
    CHECK(c.models[1].subscription);
    CHECK(c.models[1].base_url == NULL); /* inherits the global default */

    /* A pre-fix config can retain the OpenCode endpoint after selecting
     * ChatGPT. Missing per-model providers must not make those static models
     * change subscription on the next launch. */
    char legacy_path[600];
    snprintf(legacy_path, sizeof(legacy_path), "%s/legacy-selection.json", g_tmpdir);
    FILE* legacy = fopen(legacy_path, "w");
    CHECK(legacy != NULL);
    if (legacy != NULL) {
        fputs("{\"provider\":\"chatgpt\",\"base_url\":\"https://opencode.ai/zen/go/v1\","
              "\"models\":[{\"name\":\"kimi-k3\"}]}\n",
              legacy);
        fclose(legacy);
    }
    Config migrated = config_default();
    CHECK(config_load_file(&migrated, legacy_path) == AGENT_OK);
    CHECK(migrated.provider != NULL && strcmp(migrated.provider, "chatgpt") == 0);
    CHECK(migrated.n_models == 1 && migrated.models[0].provider != NULL &&
          strcmp(migrated.models[0].provider, "opencode-go") == 0);
    config_free(&migrated);
    unlink(legacy_path);

    CHECK(config_save_model(path, "opencode-go/deepseek-v4-pro") == AGENT_OK);
    Config saved = config_default();
    CHECK(config_load_file(&saved, path) == AGENT_OK);
    CHECK(saved.model_name != NULL && strcmp(saved.model_name, "opencode-go/deepseek-v4-pro") == 0);
    CHECK(saved.n_models == 2); /* model save preserves the other settings */
    CHECK(saved.project_memory_max_bytes == 2048);
    CHECK(saved.project_memory_max_bytes_set);
    config_free(&saved);

    CHECK(config_save_selection(path, "chatgpt", "gpt-5.6-sol") == AGENT_OK);
    Config selected = config_default();
    CHECK(config_load_file(&selected, path) == AGENT_OK);
    CHECK(selected.provider != NULL && strcmp(selected.provider, "chatgpt") == 0);
    CHECK(selected.model_name != NULL && strcmp(selected.model_name, "gpt-5.6-sol") == 0);
    config_free(&selected);
    config_free(&c);
    return g_failures;
}

static int test_runtime_model_lookup(void) {
    setenv("CAGENT_TEST_KEY", "test", 1);
    Config cfg = config_default();
    cfg.base_url = strdup("http://127.0.0.1:1/v1");
    cfg.api_key_env = strdup("CAGENT_TEST_KEY");
    cfg.model_name = strdup("opencode-go/glm-5.2");
    cfg.cwd = strdup(g_tmpdir);
    cfg.n_models = 2;
    cfg.models = calloc(2, sizeof(ModelConfig));
    cfg.models[0].name = strdup("opencode-go/kimi-k3");
    cfg.models[0].label = strdup("fast");
    cfg.models[1].name = strdup("gpt-4.1-mini");
    cfg.models[1].provider = strdup("openai");

    Runtime* rt = runtime_new(&cfg);
    config_free(&cfg);
    CHECK(rt != NULL);
    if (rt == NULL) {
        return g_failures;
    }

    /* default model by API name */
    Model* def = runtime_model_by_name(rt, "opencode-go/glm-5.2");
    CHECK(def == rt->model);

    /* table models by name and label */
    Model* kimi = runtime_model_by_name(rt, "opencode-go/kimi-k3");
    CHECK(kimi != NULL && kimi != rt->model);
    CHECK(runtime_model_by_name(rt, "fast") == kimi);
    CHECK(runtime_model_by_name(rt, "openai/gpt-4.1-mini") != NULL);
    char selector[128];
    CHECK(runtime_model_selector(rt, def, selector, sizeof(selector)) == AGENT_OK);
    CHECK(strcmp(selector, "opencode-go/glm-5.2") == 0);
    CHECK(runtime_model_selector(rt, runtime_model_by_name(rt, "openai/gpt-4.1-mini"), selector,
                                 sizeof(selector)) == AGENT_OK);
    CHECK(strcmp(selector, "openai/gpt-4.1-mini") == 0);

    /* unknown name */
    CHECK(runtime_model_by_name(rt, "no-such-model") == NULL);

    runtime_free(rt);
    return g_failures;
}

static int test_model_provider_isolation(void) {
    setenv("CAGENT_TEST_KEY", "test", 1);
    Config cfg = config_default();
    free(cfg.provider);
    cfg.provider = strdup("chatgpt");
    cfg.auth = strdup("chatgpt");
    cfg.model_name = strdup("chat-model");
    cfg.n_models = 2;
    cfg.models = calloc(2, sizeof(ModelConfig));
    cfg.models[0].name = strdup("openai-model");
    cfg.models[0].provider = strdup("openai");
    cfg.models[0].api_key_env = strdup("$CAGENT_TEST_KEY");
    cfg.models[1].name = strdup("claude-model");
    cfg.models[1].provider = strdup("anthropic");
    cfg.models[1].api_key_env = strdup("$CAGENT_TEST_KEY");

    Runtime* rt = runtime_new(&cfg);
    config_free(&cfg);
    CHECK(rt != NULL);
    if (rt == NULL) {
        unsetenv("CAGENT_TEST_KEY");
        return g_failures;
    }
    CHECK(rt->n_models == 2);
    CHECK(rt->models[0]->provider != NULL &&
          strcmp(rt->models[0]->provider->provider_name, "openai") == 0);
    CHECK(rt->models[1]->provider != NULL &&
          strcmp(rt->models[1]->provider->provider_name, "anthropic") == 0);
    char selector[128];
    CHECK(runtime_model_selector(rt, rt->models[0], selector, sizeof(selector)) == AGENT_OK);
    CHECK(strcmp(selector, "openai/openai-model") == 0);
    CHECK(runtime_model_selector(rt, rt->models[1], selector, sizeof(selector)) == AGENT_OK);
    CHECK(strcmp(selector, "anthropic/claude-model") == 0);

    runtime_free(rt);
    unsetenv("CAGENT_TEST_KEY");
    return g_failures;
}

static int test_agent_model_selection(void) {
    setenv("CAGENT_TEST_KEY", "test", 1);
    Config cfg = config_default();
    cfg.base_url = strdup("http://127.0.0.1:1/v1");
    cfg.api_key_env = strdup("CAGENT_TEST_KEY");
    cfg.model_name = strdup("opencode-go/glm-5.2");
    cfg.cwd = strdup(g_tmpdir);
    cfg.n_models = 1;
    cfg.models = calloc(1, sizeof(ModelConfig));
    cfg.models[0].name = strdup("opencode-go/kimi-k3");

    Runtime* rt = runtime_new(&cfg);
    config_free(&cfg);
    CHECK(rt != NULL);
    if (rt == NULL) {
        return g_failures;
    }

    /* named selection */
    AgentConfig ac = {0};
    ac.model_name = "opencode-go/kimi-k3";
    ac.cwd = g_tmpdir;
    Agent* a = agent_new(rt, &ac);
    CHECK(a != NULL);
    CHECK(a->model == runtime_model_by_name(rt, "opencode-go/kimi-k3"));

    /* unknown name falls back to the default */
    ac.model_name = "nope";
    Agent* b = agent_new(rt, &ac);
    CHECK(b != NULL);
    CHECK(b->model == rt->model);
    agent_destroy(a);
    agent_destroy(b);

    /* spawn with a named model wins; without it inherits the parent */
    Agent* parent = agent_new(rt, NULL);
    Agent* named_child = agent_spawn(parent, &ac); /* model_name = "nope" -> default */
    CHECK(named_child != NULL);
    CHECK(named_child->model == rt->model);
    Agent* inherit_child = agent_spawn(parent, NULL);
    CHECK(inherit_child != NULL);
    CHECK(inherit_child->model == parent->model);
    agent_destroy(inherit_child);
    agent_destroy(named_child);
    agent_destroy(parent);

    runtime_free(rt);
    return g_failures;
}

static int test_live_chatgpt_model_discovery(void) {
    char config_dir[600];
    char token_path[600];
    snprintf(config_dir, sizeof(config_dir), "%s/.config", g_tmpdir);
    snprintf(token_path, sizeof(token_path), "%s/cagent/auth.json", config_dir);
    CHECK(mkdir(config_dir, 0700) == 0);
    char cagent_dir[600];
    snprintf(cagent_dir, sizeof(cagent_dir), "%s/cagent", config_dir);
    CHECK(mkdir(cagent_dir, 0700) == 0);
    FILE* token = fopen(token_path, "w");
    CHECK(token != NULL);
    if (token != NULL) {
        fputs("{\"access_token\":\"test-access\",\"refresh_token\":\"test-refresh\","
              "\"account_id\":\"test-account\",\"expires_at\":4102444800}\n",
              token);
        fclose(token);
    }

    const char* old_home = getenv("HOME");
    char* saved_home = old_home != NULL ? strdup(old_home) : NULL;
    setenv("HOME", g_tmpdir, 1);
    int port = test_server_find_free_port();
    CHECK(port > 0);
    const char* body = "{\"models\":["
                       "{\"slug\":\"gpt-codex-one\",\"display_name\":\"Codex One\","
                       "\"context_window\":200000},"
                       "{\"id\":\"gpt-codex-two\",\"name\":\"Codex Two\","
                       "\"contextWindow\":128000},"
                       "{\"slug\":\"gpt-codex-one\",\"display_name\":\"Duplicate\"}]}";
    pid_t server = test_server_start(port, body, 200);
    CHECK(server > 0);
    CHECK(test_server_wait(port, 2000) == 0);

    char base[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    Config cfg = config_default();
    cfg.auth = strdup("chatgpt");
    cfg.base_url = strdup(base);
    cfg.model_name = strdup("not-in-catalog");
    CHECK(runtime_discover_models(&cfg, NULL, 0) == AGENT_OK);
    CHECK(cfg.n_models == 2);
    CHECK(strcmp(cfg.models[0].name, "gpt-codex-one") == 0);
    CHECK(strcmp(cfg.models[0].label, "Codex One") == 0);
    CHECK(cfg.models[0].context_window == 200000);
    CHECK(strcmp(cfg.models[1].name, "gpt-codex-two") == 0);
    CHECK(cfg.models[1].context_window == 128000);
    CHECK(strcmp(cfg.model_name, "gpt-codex-one") == 0);
    config_free(&cfg);
    test_server_stop(server);

    FILE* api_auth = fopen(token_path, "w");
    CHECK(api_auth != NULL);
    if (api_auth != NULL) {
        fputs("{\"type\":\"api_key\",\"provider\":\"opencode-go\","
              "\"api_key_env\":\"CAGENT_DISCOVERY_KEY\"}\n",
              api_auth);
        fclose(api_auth);
    }
    setenv("CAGENT_DISCOVERY_KEY", "test-api-key", 1);
    int api_port = test_server_find_free_port();
    CHECK(api_port > 0);
    const char* api_body = "{\"data\":["
                           "{\"id\":\"api-model-one\",\"name\":\"API One\","
                           "\"context_window\":64000},"
                           "{\"id\":\"api-model-two\",\"display_name\":\"API Two\","
                       "\"context_length\":131072,\"max_completion_tokens\":8192},"
                       "{\"id\":\"api-model-three\",\"limit\":{\"context\":65536,\"output\":4096}},"
                       "{\"id\":\"api-model-four\",\"top_provider\":{\"context_length\":32768,"
                       "\"max_completion_tokens\":2048}},"
                       "{\"id\":\"gpt-5.6-luna\"}]}";
    pid_t api_server = test_server_start(api_port, api_body, 200);
    CHECK(api_server > 0);
    CHECK(test_server_wait(api_port, 2000) == 0);
    char api_base[128];
    snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1", api_port);
    cfg = config_default();
    free(cfg.provider);
    cfg.provider = strdup("opencode-go");
    cfg.base_url = strdup(api_base);
    cfg.model_name = strdup("opencode-go/api-model-two");
    CHECK(runtime_discover_models(&cfg, NULL, 0) == AGENT_OK);
    CHECK(cfg.n_models == 5);
    CHECK(strcmp(cfg.models[0].name, "api-model-one") == 0);
    CHECK(strcmp(cfg.models[0].label, "API One") == 0);
    CHECK(cfg.models[0].context_window == 64000);
    CHECK(strcmp(cfg.models[1].name, "api-model-two") == 0);
    CHECK(cfg.models[1].context_window == 131072);
    CHECK(cfg.models[1].max_output == 8192);
    CHECK(strcmp(cfg.models[2].name, "api-model-three") == 0);
    CHECK(cfg.models[2].context_window == 65536);
    CHECK(cfg.models[2].max_output == 4096);
    CHECK(strcmp(cfg.models[3].name, "api-model-four") == 0);
    CHECK(cfg.models[3].context_window == 32768);
    CHECK(cfg.models[3].max_output == 2048);
    CHECK(strcmp(cfg.models[4].name, "gpt-5.6-luna") == 0);
    CHECK(cfg.models[4].context_window == 272000);
    CHECK(strcmp(cfg.model_name, "api-model-two") == 0); /* selector normalized to API id */
    config_free(&cfg);
    test_server_stop(api_server);
    unsetenv("CAGENT_DISCOVERY_KEY");
    unlink(token_path);

    if (saved_home != NULL) {
        setenv("HOME", saved_home, 1);
    } else {
        unsetenv("HOME");
    }
    free(saved_home);
    return g_failures;
}

static int test_multi_provider_discovery_and_selectors(void) {
    setenv("CAGENT_TEST_KEY", "test", 1);
    int default_port = test_server_find_free_port();
    int openai_port = test_server_find_free_port();
    CHECK(default_port > 0 && openai_port > 0);
    pid_t default_server =
        test_server_start(default_port, "{\"models\":[{\"id\":\"go-model\"}]}", 200);
    pid_t openai_server =
        test_server_start(openai_port, "{\"data\":[{\"id\":\"gpt-model\"}]}", 200);
    CHECK(default_server > 0 && openai_server > 0);
    CHECK(test_server_wait(default_port, 2000) == 0);
    CHECK(test_server_wait(openai_port, 2000) == 0);

    char default_base[128], openai_base[128];
    snprintf(default_base, sizeof(default_base), "http://127.0.0.1:%d/v1", default_port);
    snprintf(openai_base, sizeof(openai_base), "http://127.0.0.1:%d/v1", openai_port);
    Config cfg = config_default();
    free(cfg.base_url);
    cfg.base_url = strdup(default_base);
    cfg.api_key_env = strdup("$CAGENT_TEST_KEY");
    cfg.model_name = strdup("go-model");
    cfg.n_models = 1;
    cfg.models = calloc(1, sizeof(ModelConfig));
    cfg.models[0].name = strdup("gpt-model");
    cfg.models[0].provider = strdup("openai");
    cfg.models[0].base_url = strdup(openai_base);
    cfg.models[0].api_key_env = strdup("$CAGENT_TEST_KEY");

    CHECK(runtime_discover_all_models(&cfg, NULL, 0, NULL) == AGENT_OK);
    CHECK(cfg.n_models == 2);
    CHECK(strcmp(cfg.models[0].provider, "opencode-go") == 0);
    CHECK(strcmp(cfg.models[0].name, "go-model") == 0);
    CHECK(strcmp(cfg.models[1].provider, "openai") == 0);
    CHECK(strcmp(cfg.models[1].name, "gpt-model") == 0);
    config_free(&cfg);
    test_server_stop(default_server);
    test_server_stop(openai_server);
    unsetenv("CAGENT_TEST_KEY");
    return g_failures;
}

static int test_discovery_failure_details(void) {
    setenv("CAGENT_TEST_KEY", "test", 1);
    int port = test_server_find_free_port();
    CHECK(port > 0);
    pid_t server = test_server_start(port, "forbidden", 403);
    CHECK(server > 0);
    CHECK(test_server_wait(port, 2000) == 0);

    char base[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    Config cfg = config_default();
    cfg.base_url = strdup(base);
    cfg.api_key_env = strdup("$CAGENT_TEST_KEY");
    cfg.model_name = strdup("x");

    /* single-provider discovery reports the HTTP status */
    char detail[128] = "unset";
    CHECK(runtime_discover_models(&cfg, detail, sizeof(detail)) == AGENT_ERR_HTTP);
    CHECK(strstr(detail, "403") != NULL);
    CHECK(cfg.n_models == 0);

    /* all-provider discovery collects provider-qualified failures */
    char failures[512] = "unset";
    CHECK(runtime_discover_all_models(&cfg, failures, sizeof(failures), NULL) == AGENT_OK);
    CHECK(strstr(failures, "opencode-go") != NULL);
    CHECK(strstr(failures, "403") != NULL);
    config_free(&cfg);
    test_server_stop(server);

    /* missing credentials produce an auth detail instead of a bare code */
    Config cfg2 = config_default();
    cfg2.base_url = strdup(base);
    char detail2[128] = "unset";
    CHECK(runtime_discover_models(&cfg2, detail2, sizeof(detail2)) == AGENT_ERR_AUTH);
    CHECK(detail2[0] != '\0');
    CHECK(strcmp(detail2, "unset") != 0);
    config_free(&cfg2);
    unsetenv("CAGENT_TEST_KEY");
    return g_failures;
}

static int test_runtime_apply_catalog(void) {
    setenv("CAGENT_TEST_KEY", "test", 1);
    int port = test_server_find_free_port();
    CHECK(port > 0);
    char base[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);

    Config cfg = config_default();
    cfg.base_url = strdup(base);
    cfg.api_key_env = strdup("$CAGENT_TEST_KEY");
    cfg.model_name = strdup("static-one");
    cfg.n_models = 1;
    cfg.models = calloc(1, sizeof(ModelConfig));
    cfg.models[0].name = strdup("static-one");
    cfg.models[0].provider = strdup("opencode-go");
    cfg.models[0].base_url = strdup(base);
    cfg.models[0].api_key_env = strdup("$CAGENT_TEST_KEY");
    cfg.models[0].protocol = strdup("openai");
    cfg.models[0].context_window = 32000;

    Runtime* rt = runtime_new(&cfg);
    config_free(&cfg);
    CHECK(rt != NULL);
    if (rt == NULL) {
        unsetenv("CAGENT_TEST_KEY");
        return g_failures;
    }
    CHECK(rt->n_models == 1);
    CHECK(strcmp(rt->config.models[0].name, "static-one") == 0);

    /* a discovered catalog with two models replaces the table */
    Config catalog = config_default();
    catalog.n_models = 2;
    catalog.models = calloc(2, sizeof(ModelConfig));
    for (size_t i = 0; i < 2; i++) {
        char name[32];
        snprintf(name, sizeof(name), "live-%c", 'a' + (int)i);
        catalog.models[i].name = strdup(name);
        catalog.models[i].provider = strdup("opencode-go");
        catalog.models[i].base_url = strdup(base);
        catalog.models[i].api_key_env = strdup("$CAGENT_TEST_KEY");
        catalog.models[i].protocol = strdup("openai");
        catalog.models[i].context_window = 64000;
    }

    CHECK(runtime_apply_catalog(rt, &catalog) == AGENT_OK);
    CHECK(rt->config.n_models == 2);
    CHECK(rt->n_models == 2);
    CHECK(strcmp(rt->config.models[1].name, "live-b") == 0);
    /* the default model survives the swap */
    CHECK(rt->model != NULL);
    CHECK(strcmp(rt->model->name, "static-one") == 0);

    /* an empty catalog clears the extra table */
    Config empty = config_default();
    CHECK(runtime_apply_catalog(rt, &empty) == AGENT_OK);
    CHECK(rt->n_models == 0);
    CHECK(rt->config.n_models == 0);

    config_free(&catalog);
    config_free(&empty);
    runtime_free(rt);
    unsetenv("CAGENT_TEST_KEY");
    return g_failures;
}

static int test_runtime_overrides(void) {
    setenv("CAGENT_TEST_KEY", "test", 1);
    Config cfg = config_default();
    cfg.base_url = strdup("http://127.0.0.1:1/v1");
    cfg.api_key_env = strdup("$CAGENT_TEST_KEY");
    cfg.model_name = strdup("opencode-go/glm-5.2");
    cfg.cwd = strdup(g_tmpdir);

    Runtime* rt = runtime_new(&cfg);
    config_free(&cfg);
    CHECK(rt != NULL);
    if (rt == NULL) {
        return g_failures;
    }

    /* the $NAME form resolves the key from the environment */
    CHECK(rt->provider->api_key != NULL);
    CHECK(strcmp(rt->provider->api_key, "test") == 0);

    /* runtime overrides (TUI /base-url and /key) */
    CHECK(runtime_set_base_url(rt, "https://opencode.ai/zen/go/v1") == AGENT_OK);
    CHECK(strcmp(rt->provider->base_url, "https://opencode.ai/zen/go/v1") == 0);
    CHECK(strcmp(rt->config.base_url, "https://opencode.ai/zen/go/v1") == 0);

    CHECK(runtime_set_api_key(rt, "fresh-secret") == AGENT_OK);
    CHECK(strcmp(rt->provider->api_key, "fresh-secret") == 0);

    runtime_free(rt);
    return g_failures;
}

static int test_project_context_hierarchy(void) {
    char root[600], git[620], nested[620], path[700];
    snprintf(root, sizeof(root), "%s/context-root", g_tmpdir);
    snprintf(git, sizeof(git), "%s/.git", root);
    snprintf(nested, sizeof(nested), "%s/src", root);
    CHECK(mkdir(root, 0700) == 0);
    CHECK(mkdir(git, 0700) == 0);
    CHECK(mkdir(nested, 0700) == 0);
    snprintf(path, sizeof(path), "%s/AGENTS.md", root);
    FILE* f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("root-rule\n", f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/AGENTS.override.md", nested);
    CHECK(symlink("/etc/passwd", path) == 0);
    snprintf(path, sizeof(path), "%s/CLAUDE.md", nested);
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("nested-rule\n", f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/PROGRESS.md", root);
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("project-progress\n", f);
        fclose(f);
    }

    String prompt = string_new();
    CHECK(project_context_append(nested, &prompt) == AGENT_OK);
    CHECK(strstr(prompt.data, "root-rule") != NULL);
    CHECK(strstr(prompt.data, "nested-rule") != NULL);
    CHECK(strstr(prompt.data, "project-progress") != NULL);
    CHECK(strstr(prompt.data, "root:x:") == NULL); /* instruction symlinks are ignored */
    CHECK(strstr(prompt.data, "root-rule") < strstr(prompt.data, "nested-rule"));
    string_free(&prompt);

    /* Large project memory is an excerpt, not an unconditional full-file
     * injection. Both the beginning and the latest tail remain available. */
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("progress-head\n", f);
        for (int i = 0; i < 7000; i++) {
            fputc('x', f);
        }
        fputs("\nprogress-tail\n", f);
        fclose(f);
    }
    String capped = string_new();
    CHECK(project_context_append(nested, &capped) == AGENT_OK);
    CHECK(strstr(capped.data, "progress-head") != NULL);
    CHECK(strstr(capped.data, "progress-tail") != NULL);
    CHECK(strstr(capped.data, "[context truncated]") != NULL);
    CHECK(strlen(capped.data) < 5000);
    string_free(&capped);

    ProjectContextOptions no_memory = project_context_options_default();
    no_memory.include_progress = false;
    String without_progress = string_new();
    CHECK(project_context_append_with_options(nested, &without_progress, &no_memory) == AGENT_OK);
    CHECK(strstr(without_progress.data, "progress-head") == NULL);
    string_free(&without_progress);
    return g_failures;
}

static int test_catalog_refreshes_default_model(void) {
    setenv("CAGENT_TEST_KEY", "test", 1);
    Config cfg = config_default();
    cfg.base_url = strdup("http://127.0.0.1:1/v1");
    cfg.api_key_env = strdup("CAGENT_TEST_KEY");
    cfg.model_name = strdup("opencode-go/glm-5.2"); /* normalized to bare id */
    cfg.cwd = strdup(g_tmpdir);
    Runtime* rt = runtime_new(&cfg);
    config_free(&cfg);
    CHECK(rt != NULL);
    if (rt == NULL) {
        return g_failures;
    }
    CHECK(rt->model != NULL);
    int64_t before = rt->model->context_window; /* local default (128k) */
    CHECK(!rt->model->window_verified); /* no explicit config, no catalog yet */

    /* the live /models catalog carries the authoritative values */
    Config catalog = {0};
    catalog.n_models = 1;
    catalog.models = calloc(1, sizeof(ModelConfig));
    ModelConfig* e = &catalog.models[0];
    e->name = strdup("glm-5.2");
    e->provider = strdup("opencode-go");
    e->base_url = strdup("http://127.0.0.1:1/v1");
    e->protocol = strdup("openai");
    e->context_window = 272000;
    e->max_output = 32768;
    e->input_price = 3.0;
    e->output_price = 15.0;
    e->subscription = true;
    CHECK(runtime_apply_catalog(rt, &catalog) == AGENT_OK);
    config_free(&catalog);

    /* the default model must follow the discovered catalog (same object,
     * fields refreshed in place) */
    CHECK(rt->model->context_window == 272000);
    CHECK(rt->model->window_verified); /* discovered window is authoritative */
    CHECK(rt->model->context_window != before);
    CHECK(rt->model->max_output == 32768);
    CHECK(rt->model->input_price == 3.0);
    CHECK(rt->model->output_price == 15.0);
    CHECK(rt->model->subscription);

    /* a model absent from the catalog keeps its local fields */
    Config empty = {0};
    CHECK(runtime_apply_catalog(rt, &empty) == AGENT_OK);
    CHECK(rt->model->context_window == 272000);
    config_free(&empty);

    runtime_free(rt);
    return g_failures;
}

int main(void) {
    g_failures = 0;

    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/cagent-models-test-XXXXXX");
    char* dir = mkdtemp(g_tmpdir);
    CHECK(dir != NULL);
    const char* old_home = getenv("HOME");
    char* saved_home = old_home != NULL ? strdup(old_home) : NULL;
    if (dir != NULL) {
        setenv("HOME", g_tmpdir, 1);
    }

    g_failures += test_config_models_parsing();
    g_failures += test_runtime_model_lookup();
    g_failures += test_model_provider_isolation();
    g_failures += test_agent_model_selection();
    g_failures += test_catalog_refreshes_default_model();
    g_failures += test_live_chatgpt_model_discovery();
    g_failures += test_multi_provider_discovery_and_selectors();
    g_failures += test_discovery_failure_details();
    g_failures += test_runtime_apply_catalog();
    g_failures += test_runtime_overrides();
    g_failures += test_project_context_hierarchy();

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmpdir);
    int rc = system(cmd);
    (void)rc;
    if (saved_home != NULL) {
        setenv("HOME", saved_home, 1);
    } else {
        unsetenv("HOME");
    }
    free(saved_home);

    if (g_failures == 0) {
        printf("test_models: all tests passed\n");
        return 0;
    }
    printf("test_models: %d test(s) failed\n", g_failures);
    return 1;
}
