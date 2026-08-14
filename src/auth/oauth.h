/*
 * auth/oauth.h — official Codex ChatGPT OAuth authentication.
 *
 * Tokens are account credentials, not API keys. Callers must never log or
 * expose OAuthToken contents. The implementation stores them atomically with
 * mode 0600 and refreshes rotated refresh tokens in place.
 */
#ifndef CAGENT_AUTH_OAUTH_H
#define CAGENT_AUTH_OAUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/error.h"

typedef struct {
    char* access_token;
    char* refresh_token;
    char* account_id;
    int64_t expires_at;
} OAuthToken;

void oauth_token_init(OAuthToken* token);
void oauth_token_free(OAuthToken* token);

/* Resolve ~/.config/cagent/auth.json into out. */
int oauth_default_path(char* out, size_t cap);

/* Load/save/refresh credentials. oauth_refresh updates the file atomically. */
int oauth_load(const char* path, OAuthToken* out);
int oauth_load_provider(const char* path, const char* provider, OAuthToken* out);
/* Safe presence check: parses and immediately clears the selected credentials. */
bool oauth_provider_configured(const char* path, const char* provider);
int oauth_refresh(const char* path, OAuthToken* token);
int oauth_refresh_provider(const char* path, const char* provider, OAuthToken* token);
int oauth_remove(const char* path);
int oauth_remove_provider(const char* path, const char* provider);
/* Save one provider's API key while preserving other auth.json entries. */
int auth_save_api_key(const char* path, const char* provider, const char* key);

/* Run the official browser or device-code PKCE login flow. */
int oauth_login(const char* path, bool device_code);

/* Safe, non-secret diagnostic for the last OAuth operation. */
const char* oauth_last_error(void);

/* True when the access token should be refreshed before a request. */
bool oauth_token_expiring(const OAuthToken* token, int64_t within_seconds);

#endif /* CAGENT_AUTH_OAUTH_H */
