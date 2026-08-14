/*
 * model/provider.h — provider endpoint configuration (DESIGN.md §3.7).
 *
 * One Provider is shared by all Models that talk to the same endpoint.
 * Authentication is normally loaded from ~/.config/cagent/auth.json:
 *   {"openai":{"type":"api_key", "key":"..."}}
 *   {"openai-codex":{"type":"oauth", "access":"...", "refresh":"..."}}
 * The legacy api_key_env argument remains supported for migration. Keys and
 * tokens must never be logged, shown in the TUI, or included in crash dumps.
 *
 * Ownership:
 *   - base_url / api_key / api_key_env are owned by Provider; freed by
 *     provider_free().
 */

#ifndef CAGENT_MODEL_PROVIDER_H
#define CAGENT_MODEL_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/error.h"

typedef enum {
    PROVIDER_AUTH_API_KEY = 0,
    PROVIDER_AUTH_OAUTH,
} ProviderAuthKind;

typedef struct Provider {
    char* base_url;       /* owned; e.g. https://api.openai.com/v1 */
    char* provider_name;  /* owned; builtin/custom vendor name */
    char* api_key;        /* owned; API key or current OAuth access token */
    char* api_key_env;    /* owned; env var name or config marker */
    ProviderAuthKind auth_kind;
    char* oauth_path;     /* owned; OAuth token file */
    char* refresh_token;  /* owned; OAuth refresh token */
    char* account_id;     /* owned; ChatGPT-Account-Id */
    int64_t expires_at;   /* Unix time; OAuth access token expiry */
    char* auth_error;     /* owned; non-secret diagnostic */
} Provider;

Provider* provider_new(const char* base_url, const char* api_key_env);
Provider* provider_new_auth(const char* base_url, const char* provider_name,
                            const char* auth_path, const char* legacy_key_env);
Provider* provider_new_chatgpt(const char* base_url, const char* oauth_path);

/* Built-in vendor metadata. NULL means the name must be defined in
 * ~/.config/cagent/providers.json. */
const char* provider_builtin_base_url(const char* name);
const char* provider_builtin_protocol(const char* name);
const char* provider_builtin_key_env(const char* name);
void provider_free(Provider* p);

/* Refresh/load ChatGPT OAuth credentials before a model request. */
int provider_prepare_auth(Provider* p);
bool provider_is_chatgpt(const Provider* p);
bool provider_is_oauth(const Provider* p);
const char* provider_account_id(const Provider* p);
const char* provider_auth_error(const Provider* p);

#endif /* CAGENT_MODEL_PROVIDER_H */
