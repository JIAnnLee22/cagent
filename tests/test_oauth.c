/* OAuth storage/config/provider tests; no real OAuth network is used. */

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "auth/oauth.h"
#include "model/provider.h"
#include "runtime/runtime.h"
#include "test_common.h"

static char g_tmpdir[PATH_MAX];

static int write_token_file(const char* path, int64_t expires_at) {
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL);
    fprintf(f,
            "{\"access_token\":\"access-token\",\"refresh_token\":\"refresh-token\","
            "\"account_id\":\"account-id\",\"expires_at\":%lld}\n",
            (long long)expires_at);
    fclose(f);
    CHECK(chmod(path, 0600) == 0);
    return g_failures;
}

static int test_oauth_path_and_storage(void) {
    char path[PATH_MAX];
    CHECK(oauth_default_path(path, sizeof(path)) == AGENT_OK);
    CHECK(strstr(path, "/.config/cagent/auth.json") != NULL);

    char token_path[PATH_MAX];
    snprintf(token_path, sizeof(token_path), "%s/token.json", g_tmpdir);
    write_token_file(token_path, (int64_t)time(NULL) + 3600);

    OAuthToken token;
    oauth_token_init(&token);
    CHECK(oauth_load(token_path, &token) == AGENT_OK);
    CHECK(strcmp(token.access_token, "access-token") == 0);
    CHECK(strcmp(token.refresh_token, "refresh-token") == 0);
    CHECK(strcmp(token.account_id, "account-id") == 0);
    CHECK(!oauth_token_expiring(&token, 60));
    CHECK(oauth_token_expiring(&token, 7200));
    oauth_token_free(&token);

    CHECK(oauth_remove(token_path) == AGENT_OK);
    CHECK(access(token_path, F_OK) != 0);
    return g_failures;
}

static int test_codex_millisecond_expiry(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/codex-auth.json", g_tmpdir);
    int64_t expires_at = (int64_t)time(NULL) + 3600;
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        fprintf(f,
                "{\"openai-codex\":{\"type\":\"oauth\",\"access\":\"access-token\","
                "\"refresh\":\"refresh-token\",\"accountId\":\"account-id\","
                "\"expires\":%lld}}\n",
                (long long)(expires_at * 1000));
        fclose(f);
    }

    CHECK(oauth_provider_configured(path, "chatgpt"));
    CHECK(!oauth_provider_configured(path, "missing"));
    OAuthToken token;
    oauth_token_init(&token);
    CHECK(oauth_load_provider(path, "chatgpt", &token) == AGENT_OK);
    CHECK(token.expires_at == expires_at);
    CHECK(!oauth_token_expiring(&token, 60));
    CHECK(oauth_token_expiring(&token, 7200));
    oauth_token_free(&token);
    unlink(path);
    return g_failures;
}

static int test_api_key_auth_merge(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/auth.json", g_tmpdir);
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("{\"deepseek\":{\"type\":\"api_key\",\"key\":\"old-key\"}}", f);
        fclose(f);
    }
    CHECK(auth_save_api_key(path, "xiaomi", "new-key") == AGENT_OK);
    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f != NULL) {
        char data[4096] = {0};
        size_t n = fread(data, 1, sizeof(data) - 1, f);
        data[n] = '\0';
        fclose(f);
        CHECK(strstr(data, "deepseek") != NULL);
        CHECK(strstr(data, "old-key") != NULL);
        CHECK(strstr(data, "xiaomi") != NULL);
        CHECK(strstr(data, "new-key") != NULL);
    }
    unlink(path);
    return g_failures;
}

static int test_chatgpt_provider_loads_token(void) {
    char token_path[PATH_MAX];
    snprintf(token_path, sizeof(token_path), "%s/provider.json", g_tmpdir);
    write_token_file(token_path, (int64_t)time(NULL) + 3600);

    Provider* p = provider_new_chatgpt("https://chatgpt.com/backend-api/codex", token_path);
    CHECK(p != NULL);
    CHECK(provider_is_chatgpt(p));
    CHECK(p->api_key != NULL && strcmp(p->api_key, "access-token") == 0);
    CHECK(strcmp(provider_account_id(p), "account-id") == 0);
    CHECK(provider_prepare_auth(p) == AGENT_OK);
    provider_free(p);
    unlink(token_path);
    return g_failures;
}

static int test_config_auth_field(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/config.json", g_tmpdir);
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL);
    fputs("{\"auth\":\"chatgpt\",\"model\":\"gpt-5\"}", f);
    fclose(f);

    Config cfg = config_default();
    CHECK(config_load_file(&cfg, path) == AGENT_OK);
    CHECK(cfg.auth != NULL && strcmp(cfg.auth, "chatgpt") == 0);
    CHECK(cfg.model_name != NULL && strcmp(cfg.model_name, "gpt-5") == 0);
    config_free(&cfg);
    unlink(path);
    return g_failures;
}

static int test_custom_provider_and_named_auth(void) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.config/cagent", g_tmpdir);
    /* The test HOME is the temporary directory; create its config path. */
    char config_dir[PATH_MAX];
    snprintf(config_dir, sizeof(config_dir), "%s/.config", g_tmpdir);
    mkdir(config_dir, 0700);
    mkdir(dir, 0700);

    char custom_path[PATH_MAX];
    char auth_path[PATH_MAX];
    snprintf(custom_path, sizeof(custom_path), "%s/custom_provider.json", dir);
    snprintf(auth_path, sizeof(auth_path), "%s/auth.json", dir);
    FILE* f = fopen(custom_path, "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("{\"name\":\"my-gateway\",\"baseUrl\":\"https://example.test/v1\","
              "\"protocol\":\"openai\",\"models\":[\"model-a\","
              "{\"name\":\"model-b\",\"contextWindow\":8192}]}",
              f);
        fclose(f);
    }
    f = fopen(auth_path, "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        fputs("{\"my-gateway\":{\"type\":\"api_key\",\"key\":\"named-key\"}}", f);
        fclose(f);
    }

    Config cfg = config_default();
    free(cfg.provider);
    cfg.provider = strdup("my-gateway");
    CHECK(config_load_custom_providers(&cfg) == AGENT_OK);
    CHECK(cfg.base_url != NULL && strcmp(cfg.base_url, "https://example.test/v1") == 0);
    CHECK(cfg.protocol != NULL && strcmp(cfg.protocol, "openai") == 0);
    CHECK(cfg.model_name != NULL && strcmp(cfg.model_name, "model-a") == 0);
    CHECK(cfg.n_models == 2);
    CHECK(cfg.models[1].context_window == 8192);

    Provider* p = provider_new_auth(cfg.base_url, cfg.provider, auth_path, NULL);
    CHECK(p != NULL);
    CHECK(p != NULL && p->api_key != NULL && strcmp(p->api_key, "named-key") == 0);
    provider_free(p);
    config_free(&cfg);
    unlink(custom_path);
    unlink(auth_path);
    rmdir(dir);
    rmdir(config_dir);
    return g_failures;
}

int main(void) {
    char template[] = "/tmp/cagent-oauth-test-XXXXXX";
    char* dir = mkdtemp(template);
    CHECK(dir != NULL);
    if (dir != NULL) {
        snprintf(g_tmpdir, sizeof(g_tmpdir), "%s", dir);
        setenv("HOME", dir, 1);
    }
    g_failures = 0;
    test_oauth_path_and_storage();
    test_codex_millisecond_expiry();
    test_api_key_auth_merge();
    test_chatgpt_provider_loads_token();
    test_config_auth_field();
    test_custom_provider_and_named_auth();
    if (dir != NULL) {
        rmdir(dir);
    }
    if (g_failures == 0) {
        printf("test_oauth: all tests passed\n");
        return 0;
    }
    printf("test_oauth: %d test(s) failed\n", g_failures);
    return 1;
}
