/*
 * model/provider.c — provider endpoint and authentication state.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/oauth.h"
#include "model/provider.h"
#include "util/json.h"

static Provider* provider_alloc(const char* base_url) {
    if (base_url == NULL) {
        return NULL;
    }
    Provider* p = calloc(1, sizeof(Provider));
    if (p == NULL) {
        return NULL;
    }
    p->base_url = strdup(base_url);
    if (p->base_url == NULL) {
        free(p);
        return NULL;
    }
    p->auth_kind = PROVIDER_AUTH_API_KEY;
    return p;
}

Provider* provider_new(const char* base_url, const char* api_key_env) {
    Provider* p = provider_alloc(base_url);
    if (p == NULL) {
        return NULL;
    }
    p->api_key_env = api_key_env != NULL ? strdup(api_key_env) : NULL;
    if (api_key_env != NULL && p->api_key_env == NULL) {
        provider_free(p);
        return NULL;
    }

    if (api_key_env != NULL) {
        if (api_key_env[0] == '$') {
            /* "$NAME": read the key from the environment. */
            const char* key = getenv(api_key_env + 1);
            if (key != NULL && key[0] != '\0') {
                p->api_key = strdup(key);
                if (p->api_key == NULL) {
                    provider_free(p);
                    return NULL;
                }
            }
        } else {
            /* Otherwise the value IS the API key (config-file key). */
            p->api_key = strdup(api_key_env);
            if (p->api_key == NULL) {
                provider_free(p);
                return NULL;
            }
        }
    }
    return p;
}

typedef struct {
    char* type;
    char* provider;
    char* api_key_env;
    char* api_key;
} AuthFile;

static void auth_file_free(AuthFile* auth) {
    if (auth == NULL) {
        return;
    }
    free(auth->type);
    free(auth->provider);
    free(auth->api_key_env);
    free(auth->api_key);
    memset(auth, 0, sizeof(*auth));
}

static int auth_file_load(const char* path, const char* provider_name, AuthFile* out) {
    if (path == NULL || out == NULL) {
        return AGENT_ERR_IO;
    }
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        return errno == ENOENT ? AGENT_OK : AGENT_ERR_IO;
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
    int close_rc = fclose(f);
    data[n] = '\0';
    if (close_rc != 0 || n != (size_t)size) {
        free(data);
        return AGENT_ERR_IO;
    }
    JsonDoc* doc = json_parse(data, n);
    free(data);
    JsonVal* root = doc != NULL ? json_root(doc) : NULL;
    if (root == NULL || !json_val_is_obj(root)) {
        json_doc_free(doc);
        return AGENT_ERR_JSON;
    }
    JsonVal* entry = root;
    if (json_val_obj_get(root, "type") == NULL) {
        const char* selected = provider_name != NULL ? provider_name : "opencode-go";
        entry = json_val_obj_get(root, selected);
        if (entry == NULL && selected != NULL && strcmp(selected, "chatgpt") == 0) {
            entry = json_val_obj_get(root, "openai-codex");
        }
    }
    if (entry == NULL || !json_val_is_obj(entry)) {
        json_doc_free(doc);
        return AGENT_ERR_JSON;
    }
    const char* type = json_obj_get_str(entry, "type");
    const char* provider = json_obj_get_str(entry, "provider");
    const char* key_env = json_obj_get_str(entry, "api_key_env");
    const char* key = json_obj_get_str(entry, "api_key");
    if (key == NULL) {
        key = json_obj_get_str(entry, "key");
    }
    int rc = type != NULL ? AGENT_OK : AGENT_ERR_JSON;
    if (rc == AGENT_OK) {
        out->type = strdup(type);
        out->provider = provider != NULL ? strdup(provider) : NULL;
        out->api_key_env = key_env != NULL ? strdup(key_env) : NULL;
        out->api_key = key != NULL ? strdup(key) : NULL;
        if (out->type == NULL || (provider != NULL && out->provider == NULL) ||
            (key_env != NULL && out->api_key_env == NULL) ||
            (key != NULL && out->api_key == NULL)) {
            rc = AGENT_ERR_OOM;
        }
    }
    json_doc_free(doc);
    if (rc != AGENT_OK) {
        auth_file_free(out);
    }
    return rc;
}

const char* provider_builtin_base_url(const char* name) {
    if (name == NULL || strcmp(name, "opencode-go") == 0) {
        return "https://opencode.ai/zen/go/v1";
    }
    if (strcmp(name, "openai") == 0) {
        return "https://api.openai.com/v1";
    }
    if (strcmp(name, "anthropic") == 0) {
        return "https://api.anthropic.com/v1";
    }
    if (name != NULL && (strcmp(name, "xiaomi") == 0 || strcmp(name, "xiaomi-mimo") == 0 ||
                         strcmp(name, "mimo") == 0)) {
        return "https://api.xiaomimimo.com/v1";
    }
    if (strcmp(name, "chatgpt") == 0 || strcmp(name, "openai-codex") == 0) {
        return "https://chatgpt.com/backend-api/codex";
    }
    return NULL;
}

const char* provider_builtin_protocol(const char* name) {
    if (name != NULL && strcmp(name, "anthropic") == 0) {
        return "anthropic";
    }
    if (name != NULL &&
        (strcmp(name, "chatgpt") == 0 || strcmp(name, "openai-codex") == 0)) {
        return "responses";
    }
    if (provider_builtin_base_url(name) != NULL) {
        return "openai";
    }
    return NULL;
}

const char* provider_builtin_key_env(const char* name) {
    if (name != NULL && strcmp(name, "openai") == 0) {
        return "$OPENAI_API_KEY";
    }
    if (name != NULL && strcmp(name, "anthropic") == 0) {
        return "$ANTHROPIC_API_KEY";
    }
    if (name != NULL && (strcmp(name, "xiaomi") == 0 || strcmp(name, "xiaomi-mimo") == 0 ||
                         strcmp(name, "mimo") == 0)) {
        return "$MIMO_API_KEY";
    }
    if (name == NULL || strcmp(name, "opencode-go") == 0) {
        return "$OPENCODE_GO_API_KEY";
    }
    return NULL;
}

Provider* provider_new_auth(const char* base_url, const char* provider_name,
                            const char* auth_path, const char* legacy_key_env) {
    AuthFile auth = {0};
    int auth_rc = auth_file_load(auth_path, provider_name, &auth);
    if (auth_rc != AGENT_OK && auth_rc != AGENT_ERR_IO) {
        return NULL;
    }
    const char* type = auth.type;
    if (auth.provider != NULL && provider_name != NULL &&
        strcmp(auth.provider, provider_name) != 0) {
        auth_file_free(&auth);
        return NULL;
    }
    if ((type != NULL && strcmp(type, "oauth") == 0) ||
        (type == NULL && provider_name != NULL && strcmp(provider_name, "chatgpt") == 0)) {
        const char* oauth_provider = auth.provider != NULL ? auth.provider : provider_name;
        if (oauth_provider == NULL || strcmp(oauth_provider, "chatgpt") == 0 ||
            strcmp(oauth_provider, "openai-codex") == 0) {
            Provider* p = provider_new_chatgpt(base_url, auth_path);
            if (p != NULL && provider_name != NULL) {
                char* name = strdup(provider_name);
                if (name == NULL) {
                    provider_free(p);
                    p = NULL;
                } else {
                    free(p->provider_name);
                    p->provider_name = name;
                }
            }
            auth_file_free(&auth);
            return p;
        }
        auth_file_free(&auth);
        return NULL;
    }
    const char* key_source = legacy_key_env;
    char* env_source = NULL;
    if (type != NULL && strcmp(type, "api_key") == 0) {
        if (auth.api_key_env != NULL) {
            env_source = auth.api_key_env[0] == '$' ? strdup(auth.api_key_env) : NULL;
            if (env_source == NULL && auth.api_key_env[0] != '$') {
                size_t len = strlen(auth.api_key_env);
                env_source = malloc(len + 2);
                if (env_source != NULL) {
                    env_source[0] = '$';
                    memcpy(env_source + 1, auth.api_key_env, len + 1);
                }
            }
            key_source = env_source;
        } else if (auth.api_key != NULL) {
            key_source = auth.api_key;
        } else {
            key_source = NULL;
        }
    }
    Provider* p = provider_new(base_url, key_source);
    if (p != NULL && provider_name != NULL) {
        p->provider_name = strdup(provider_name);
        if (p->provider_name == NULL) {
            provider_free(p);
            p = NULL;
        }
    }
    free(env_source);
    auth_file_free(&auth);
    return p;
}

Provider* provider_new_chatgpt(const char* base_url, const char* oauth_path) {
    Provider* p = provider_alloc(base_url);
    if (p == NULL) {
        return NULL;
    }
    p->auth_kind = PROVIDER_AUTH_OAUTH;
    p->provider_name = strdup("chatgpt");
    p->api_key_env = strdup("oauth");
    p->oauth_path = oauth_path != NULL ? strdup(oauth_path) : NULL;
    if (p->provider_name == NULL || p->api_key_env == NULL ||
        (oauth_path != NULL && p->oauth_path == NULL)) {
        provider_free(p);
        return NULL;
    }
    (void)provider_prepare_auth(p);
    return p;
}

static int provider_set_auth_error(Provider* p, const char* message) {
    if (p == NULL) {
        return AGENT_ERR_AUTH;
    }
    char* copy = strdup(message != NULL ? message : "authentication failed");
    if (copy == NULL) {
        return AGENT_ERR_OOM;
    }
    free(p->auth_error);
    p->auth_error = copy;
    return AGENT_ERR_AUTH;
}

static int provider_set_token(Provider* p, OAuthToken* token) {
    char* access = token->access_token != NULL ? strdup(token->access_token) : NULL;
    char* refresh = token->refresh_token != NULL ? strdup(token->refresh_token) : NULL;
    char* account = token->account_id != NULL ? strdup(token->account_id) : NULL;
    if (access == NULL || refresh == NULL || account == NULL) {
        free(access);
        free(refresh);
        free(account);
        return AGENT_ERR_OOM;
    }
    free(p->api_key);
    free(p->refresh_token);
    free(p->account_id);
    p->api_key = access;
    p->refresh_token = refresh;
    p->account_id = account;
    p->expires_at = token->expires_at;
    free(p->auth_error);
    p->auth_error = NULL;
    return AGENT_OK;
}

int provider_prepare_auth(Provider* p) {
    if (p == NULL) {
        return AGENT_ERR_AUTH;
    }
    if (p->auth_kind != PROVIDER_AUTH_OAUTH) {
        return p->api_key != NULL ? AGENT_OK : AGENT_ERR_AUTH;
    }
    if (p->oauth_path == NULL) {
        return provider_set_auth_error(p, "ChatGPT OAuth path is not configured");
    }

    OAuthToken token;
    oauth_token_init(&token);
    int rc = oauth_load_provider(p->oauth_path, p->provider_name, &token);
    if (rc != AGENT_OK) {
        oauth_token_free(&token);
        free(p->api_key);
        p->api_key = NULL;
        return provider_set_auth_error(p, "ChatGPT OAuth is not configured; run cagent --login");
    }
    if (oauth_token_expiring(&token, 300)) {
        rc = oauth_refresh_provider(p->oauth_path, p->provider_name, &token);
        if (rc != AGENT_OK) {
            oauth_token_free(&token);
            free(p->api_key);
            p->api_key = NULL;
            return provider_set_auth_error(p, "ChatGPT OAuth refresh failed; run cagent --login again");
        }
    }
    rc = provider_set_token(p, &token);
    oauth_token_free(&token);
    return rc;
}

bool provider_is_chatgpt(const Provider* p) {
    return p != NULL && p->auth_kind == PROVIDER_AUTH_OAUTH && p->provider_name != NULL &&
           (strcmp(p->provider_name, "chatgpt") == 0 ||
            strcmp(p->provider_name, "openai-codex") == 0);
}

bool provider_is_oauth(const Provider* p) {
    return p != NULL && p->auth_kind == PROVIDER_AUTH_OAUTH;
}

const char* provider_account_id(const Provider* p) {
    return p != NULL ? p->account_id : NULL;
}

const char* provider_auth_error(const Provider* p) {
    return p != NULL && p->auth_error != NULL ? p->auth_error : "missing API key";
}

void provider_free(Provider* p) {
    if (p == NULL) {
        return;
    }
    free(p->base_url);
    free(p->provider_name);
    free(p->api_key);
    free(p->api_key_env);
    free(p->oauth_path);
    free(p->refresh_token);
    free(p->account_id);
    free(p->auth_error);
    free(p);
}
