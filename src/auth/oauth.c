/*
 * auth/oauth.c — official Codex ChatGPT OAuth authorization-code flow.
 *
 * The login flow is intentionally separate from the Agent EventLoop: it is a
 * short-lived interactive operation started by --login. Model requests only
 * use the resulting bearer token and refresh it before expiry.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <openssl/sha.h>

#include "auth/oauth.h"
#include "util/json.h"
#include "util/string.h"

#define OAUTH_ISSUER "https://auth.openai.com"
#define OAUTH_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define OAUTH_CALLBACK_PATH "/auth/callback"
#define OAUTH_DEFAULT_PORT 1455
#define OAUTH_FALLBACK_PORT 1457
#define OAUTH_TIMEOUT_SECONDS (10 * 60)
#define OAUTH_MAX_RESPONSE (1024 * 1024)

static char g_oauth_error[256];

static int oauth_fail(const char* message) {
    snprintf(g_oauth_error, sizeof(g_oauth_error), "%s", message != NULL ? message : "OAuth error");
    return AGENT_ERR_AUTH;
}

static int oauth_failf(const char* fmt, const char* detail) {
    snprintf(g_oauth_error, sizeof(g_oauth_error), fmt, detail != NULL ? detail : "unknown");
    return AGENT_ERR_AUTH;
}

const char* oauth_last_error(void) {
    return g_oauth_error[0] != '\0' ? g_oauth_error : "OAuth operation failed";
}

void oauth_token_init(OAuthToken* token) {
    if (token != NULL) {
        memset(token, 0, sizeof(*token));
    }
}

void oauth_token_free(OAuthToken* token) {
    if (token == NULL) {
        return;
    }
    free(token->access_token);
    free(token->refresh_token);
    free(token->account_id);
    memset(token, 0, sizeof(*token));
}

static int replace_string(char** dst, const char* src) {
    char* copy = src != NULL ? strdup(src) : NULL;
    if (src != NULL && copy == NULL) {
        return AGENT_ERR_OOM;
    }
    free(*dst);
    *dst = copy;
    return AGENT_OK;
}

static int random_bytes(unsigned char* out, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return oauth_fail("cannot open system random source");
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, out + off, len - off);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            close(fd);
            return oauth_fail("cannot read system random source");
        }
        off += (size_t)n;
    }
    close(fd);
    return AGENT_OK;
}

static char base64url_char(unsigned int value) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    return alphabet[value & 63U];
}

static size_t base64url_encode(const unsigned char* data, size_t len, char* out, size_t cap) {
    size_t needed = (len * 8 + 5) / 6;
    if (out == NULL || cap <= needed) {
        return 0;
    }
    size_t out_len = 0;
    unsigned int accumulator = 0;
    unsigned int bits = 0;
    for (size_t i = 0; i < len; i++) {
        accumulator = (accumulator << 8) | data[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out[out_len++] = base64url_char(accumulator >> bits);
        }
    }
    if (bits > 0) {
        out[out_len++] = base64url_char(accumulator << (6 - bits));
    }
    out[out_len] = '\0';
    return out_len;
}

static int base64url_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '-') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return -1;
}

static size_t base64url_decode(const char* text, unsigned char* out, size_t cap) {
    size_t len = strlen(text);
    size_t out_len = 0;
    unsigned int accumulator = 0;
    unsigned int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int value = base64url_value((unsigned char)text[i]);
        if (value < 0) {
            return 0;
        }
        accumulator = (accumulator << 6) | (unsigned int)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_len >= cap) {
                return 0;
            }
            out[out_len++] = (unsigned char)(accumulator >> bits);
        }
    }
    return out_len;
}

static int make_pkce(char* verifier, size_t verifier_cap, char* challenge, size_t challenge_cap,
                     char* state, size_t state_cap) {
    unsigned char random_data[32];
    if (random_bytes(random_data, sizeof(random_data)) != AGENT_OK) {
        return AGENT_ERR_AUTH;
    }
    if (base64url_encode(random_data, sizeof(random_data), verifier, verifier_cap) == 0 ||
        base64url_encode(random_data, sizeof(random_data), state, state_cap) == 0) {
        return oauth_fail("failed to create OAuth state");
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)verifier, strlen(verifier), digest);
    if (base64url_encode(digest, sizeof(digest), challenge, challenge_cap) == 0) {
        return oauth_fail("failed to create PKCE challenge");
    }
    return AGENT_OK;
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int url_decode(const char* src, char* dst, size_t cap) {
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%' && src[i + 1] != '\0' && src[i + 2] != '\0') {
            int hi = hex_value((unsigned char)src[++i]);
            int lo = hex_value((unsigned char)src[++i]);
            if (hi < 0 || lo < 0) {
                return AGENT_ERR_AUTH;
            }
            c = (unsigned char)((hi << 4) | lo);
        } else if (c == '+') {
            c = ' ';
        }
        if (out + 1 >= cap) {
            return AGENT_ERR_OOM;
        }
        dst[out++] = (char)c;
    }
    dst[out] = '\0';
    return AGENT_OK;
}

static int query_value(const char* query, const char* wanted, char* out, size_t cap) {
    const char* p = query;
    while (*p != '\0') {
        const char* amp = strchr(p, '&');
        size_t part_len = amp != NULL ? (size_t)(amp - p) : strlen(p);
        const char* eq = memchr(p, '=', part_len);
        if (eq != NULL) {
            size_t key_len = (size_t)(eq - p);
            if (strlen(wanted) == key_len && strncmp(p, wanted, key_len) == 0) {
                size_t value_len = part_len - key_len - 1;
                char* encoded = malloc(value_len + 1);
                if (encoded == NULL) {
                    return AGENT_ERR_OOM;
                }
                memcpy(encoded, eq + 1, value_len);
                encoded[value_len] = '\0';
                int rc = url_decode(encoded, out, cap);
                free(encoded);
                return rc;
            }
        }
        if (amp == NULL) {
            break;
        }
        p = amp + 1;
    }
    if (cap > 0) {
        out[0] = '\0';
    }
    return AGENT_ERR_IO;
}

static int append_query(String* url, const char* key, const char* value, bool* first) {
    char* escaped = curl_easy_escape(NULL, value != NULL ? value : "", 0);
    if (escaped == NULL) {
        return AGENT_ERR_OOM;
    }
    int rc = string_printf(url, "%s%s=%s", *first ? "?" : "&", key, escaped);
    curl_free(escaped);
    if (rc == AGENT_OK) {
        *first = false;
    }
    return rc;
}

static int build_authorize_url(const char* redirect_uri, const char* verifier,
                               const char* challenge, const char* state, String* out) {
    (void)verifier;
    *out = string_new();
    if (string_append(out, OAUTH_ISSUER "/oauth/authorize") != AGENT_OK) {
        return AGENT_ERR_OOM;
    }
    bool first = true;
    const struct {
        const char* key;
        const char* value;
    } params[] = {
        {"response_type", "code"},
        {"client_id", OAUTH_CLIENT_ID},
        {"scope", "openid profile email offline_access api.connectors.read api.connectors.invoke"},
        {"code_challenge_method", "S256"},
        {"id_token_add_organizations", "true"},
        {"codex_cli_simplified_flow", "true"},
        {"originator", "codex_cli_rs"},
    };
    for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
        if (append_query(out, params[i].key, params[i].value, &first) != AGENT_OK) {
            string_free(out);
            return AGENT_ERR_OOM;
        }
    }
    if (append_query(out, "redirect_uri", redirect_uri, &first) != AGENT_OK ||
        append_query(out, "code_challenge", challenge, &first) != AGENT_OK ||
        append_query(out, "state", state, &first) != AGENT_OK) {
        string_free(out);
        return AGENT_ERR_OOM;
    }
    return AGENT_OK;
}

static void open_browser(const char* url) {
    const char* commands[] = {"xdg-open", "gio", "open"};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        pid_t pid = fork();
        if (pid == 0) {
            if (strcmp(commands[i], "gio") == 0) {
                execlp(commands[i], commands[i], "open", url, (char*)NULL);
            } else {
                execlp(commands[i], commands[i], url, (char*)NULL);
            }
            _exit(127);
        }
        if (pid > 0) {
            int status = 0;
            (void)waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                return;
            }
        }
    }
    printf("Open this URL in a browser to continue login:\n%s\n", url);
    fflush(stdout);
}

static int bind_listener(int* port_out) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return oauth_fail("cannot create OAuth callback socket");
    }
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int ports[] = {OAUTH_DEFAULT_PORT, OAUTH_FALLBACK_PORT};
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((uint16_t)ports[i]);
        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            if (listen(fd, 1) == 0) {
                *port_out = ports[i];
                return fd;
            }
            break;
        }
    }
    close(fd);
    return oauth_fail("OAuth callback ports 1455 and 1457 are unavailable");
}

static int read_http_request(int fd, char* out, size_t cap) {
    size_t used = 0;
    while (used + 1 < cap) {
        ssize_t n = recv(fd, out + used, cap - used - 1, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        used += (size_t)n;
        out[used] = '\0';
        if (strstr(out, "\r\n\r\n") != NULL) {
            return AGENT_OK;
        }
    }
    return used > 0 ? AGENT_OK : AGENT_ERR_IO;
}

static void send_callback_page(int fd, bool success) {
    const char* body = success ?
        "<!doctype html><title>cagent login</title><p>cagent login complete. You may close this window.</p>" :
        "<!doctype html><title>cagent login</title><p>cagent login failed. You may close this window.</p>";
    char response[512];
    int n = snprintf(response, sizeof(response),
                     "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                     strlen(body), body);
    if (n > 0) {
        (void)send(fd, response, (size_t)n, MSG_NOSIGNAL);
    }
}

static int wait_for_callback(int listener, const char* expected_state, char* code, size_t code_cap,
                             char* verifier, size_t verifier_cap, bool* device_done) {
    struct pollfd pfd = {.fd = listener, .events = POLLIN};
    int timeout = OAUTH_TIMEOUT_SECONDS * 1000;
    int prc;
    do {
        prc = poll(&pfd, 1, timeout);
    } while (prc < 0 && errno == EINTR);
    if (prc <= 0) {
        return oauth_fail(prc == 0 ? "OAuth login timed out" : "OAuth callback wait failed");
    }
    int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0) {
        return oauth_fail("cannot accept OAuth callback");
    }
    char request[16384] = {0};
    int rc = read_http_request(client, request, sizeof(request));
    if (rc != AGENT_OK) {
        send_callback_page(client, false);
        close(client);
        return oauth_fail("invalid OAuth callback request");
    }
    char* get = strstr(request, "GET ");
    char* path = get != NULL ? get + 4 : NULL;
    char* end = path != NULL ? strchr(path, ' ') : NULL;
    if (path == NULL || end == NULL || strncmp(path, OAUTH_CALLBACK_PATH "?", 15) != 0) {
        send_callback_page(client, false);
        close(client);
        return oauth_fail("unexpected OAuth callback path");
    }
    *end = '\0';
    char state[256] = {0};
    char error[256] = {0};
    (void)query_value(path + 15, "state", state, sizeof(state));
    if (state[0] == '\0' || strcmp(state, expected_state) != 0) {
        send_callback_page(client, false);
        close(client);
        return oauth_fail("OAuth state validation failed");
    }
    (void)query_value(path + 15, "error", error, sizeof(error));
    if (error[0] != '\0') {
        send_callback_page(client, false);
        close(client);
        return oauth_fail("OAuth provider returned an authorization error");
    }
    rc = query_value(path + 15, "code", code, code_cap);
    if (rc != AGENT_OK || code[0] == '\0') {
        send_callback_page(client, false);
        close(client);
        return oauth_fail("OAuth callback did not contain an authorization code");
    }
    if (verifier != NULL && verifier_cap > 0) {
        verifier[0] = '\0';
    }
    if (device_done != NULL) {
        *device_done = true;
    }
    send_callback_page(client, true);
    close(client);
    return AGENT_OK;
}

typedef struct {
    String body;
} ResponseBody;

static size_t response_write(char* data, size_t size, size_t count, void* userdata) {
    ResponseBody* response = userdata;
    size_t n = size * count;
    if (response->body.len + n > OAUTH_MAX_RESPONSE) {
        return 0;
    }
    return string_append_n(&response->body, data, n) == AGENT_OK ? n : 0;
}

static int oauth_post(const char* url, const char* body, const char* content_type,
                      long* status_out, String* response_out) {
    CURL* easy = curl_easy_init();
    if (easy == NULL) {
        return oauth_fail("cannot initialize OAuth HTTP client");
    }
    ResponseBody response = {.body = string_new()};
    struct curl_slist* headers = NULL;
    char content_header[128];
    snprintf(content_header, sizeof(content_header), "Content-Type: %s", content_type);
    headers = curl_slist_append(headers, content_header);
    headers = curl_slist_append(headers, "Accept: application/json");
    if (headers == NULL) {
        curl_easy_cleanup(easy);
        string_free(&response.body);
        return oauth_fail("cannot allocate OAuth HTTP headers");
    }
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, response_write);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "cagent/0.1");
    CURLcode crc = curl_easy_perform(easy);
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(easy);
    if (crc != CURLE_OK) {
        string_free(&response.body);
        return oauth_failf("OAuth HTTP request failed: %s", curl_easy_strerror(crc));
    }
    if (status_out != NULL) {
        *status_out = status;
    }
    if (response_out != NULL) {
        *response_out = response.body;
    } else {
        string_free(&response.body);
    }
    return AGENT_OK;
}

static int parse_jwt_claims(const char* jwt, char** account_id, int64_t* expires_at) {
    const char* first = strchr(jwt, '.');
    const char* second = first != NULL ? strchr(first + 1, '.') : NULL;
    if (first == NULL || second == NULL || second <= first + 1) {
        return AGENT_ERR_JSON;
    }
    size_t encoded_len = (size_t)(second - first - 1);
    char* encoded = malloc(encoded_len + 1);
    unsigned char* decoded = malloc(encoded_len + 1);
    if (encoded == NULL || decoded == NULL) {
        free(encoded);
        free(decoded);
        return AGENT_ERR_OOM;
    }
    memcpy(encoded, first + 1, encoded_len);
    encoded[encoded_len] = '\0';
    size_t decoded_len = base64url_decode(encoded, decoded, encoded_len + 1);
    free(encoded);
    if (decoded_len == 0) {
        free(decoded);
        return AGENT_ERR_JSON;
    }
    JsonDoc* doc = json_parse((const char*)decoded, decoded_len);
    free(decoded);
    if (doc == NULL) {
        return AGENT_ERR_JSON;
    }
    JsonVal* root = json_root(doc);
    JsonVal* auth = root != NULL ? json_val_obj_get(root, "https://api.openai.com/auth") : NULL;
    const char* account = auth != NULL ? json_obj_get_str(auth, "chatgpt_account_id") : NULL;
    if (account == NULL && root != NULL) {
        account = json_obj_get_str(root, "chatgpt_account_id");
    }
    if (account != NULL && account_id != NULL) {
        int rc = replace_string(account_id, account);
        if (rc != AGENT_OK) {
            json_doc_free(doc);
            return rc;
        }
    }
    if (expires_at != NULL && root != NULL) {
        int64_t exp = json_obj_get_int(root, "exp", 0);
        if (exp > 0) {
            *expires_at = exp;
        }
    }
    json_doc_free(doc);
    return AGENT_OK;
}

static int parse_token_response(const String* body, OAuthToken* old, OAuthToken* out) {
    JsonDoc* doc = json_parse(body->data, body->len);
    if (doc == NULL) {
        return oauth_fail("OAuth token response was not valid JSON");
    }
    JsonVal* root = json_root(doc);
    const char* access = root != NULL ? json_obj_get_str(root, "access_token") : NULL;
    const char* refresh = root != NULL ? json_obj_get_str(root, "refresh_token") : NULL;
    const char* id_token = root != NULL ? json_obj_get_str(root, "id_token") : NULL;
    int64_t expires_in = root != NULL ? json_obj_get_int(root, "expires_in", 0) : 0;
    if (access == NULL || access[0] == '\0') {
        json_doc_free(doc);
        return oauth_fail("OAuth token response did not contain an access token");
    }
    oauth_token_init(out);
    if (replace_string(&out->access_token, access) != AGENT_OK ||
        replace_string(&out->refresh_token, refresh != NULL ? refresh :
                       (old != NULL ? old->refresh_token : NULL)) != AGENT_OK ||
        (old != NULL && replace_string(&out->account_id, old->account_id) != AGENT_OK)) {
        json_doc_free(doc);
        oauth_token_free(out);
        return AGENT_ERR_OOM;
    }
    out->expires_at = expires_in > 0 ? (int64_t)time(NULL) + expires_in : 0;
    char* claims_account = NULL;
    int64_t claims_exp = 0;
    if (id_token != NULL) {
        (void)parse_jwt_claims(id_token, &claims_account, &claims_exp);
    }
    if (claims_account == NULL) {
        (void)parse_jwt_claims(out->access_token, &claims_account, &claims_exp);
    }
    if (claims_account != NULL) {
        free(out->account_id);
        out->account_id = claims_account;
        if (claims_exp > 0) {
            out->expires_at = claims_exp;
        }
    }
    if (out->refresh_token == NULL || out->refresh_token[0] == '\0' ||
        out->account_id == NULL || out->account_id[0] == '\0') {
        json_doc_free(doc);
        oauth_token_free(out);
        return oauth_fail("OAuth token response did not contain a ChatGPT account id or refresh token");
    }
    json_doc_free(doc);
    return AGENT_OK;
}

static int read_file(const char* path, String* out) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return errno == ENOENT ? AGENT_ERR_IO : oauth_fail("cannot open OAuth token file");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return oauth_fail("cannot seek OAuth token file");
    }
    long size = ftell(f);
    if (size < 0 || size > OAUTH_MAX_RESPONSE) {
        fclose(f);
        return oauth_fail("OAuth token file is invalid or too large");
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return oauth_fail("cannot seek OAuth token file");
    }
    *out = string_new();
    int rc = string_reserve(out, (size_t)size);
    if (rc == AGENT_OK && size > 0 && fread(out->data, 1, (size_t)size, f) != (size_t)size) {
        rc = AGENT_ERR_IO;
    }
    if (rc == AGENT_OK) {
        out->len = (size_t)size;
        out->data[out->len] = '\0';
    }
    fclose(f);
    if (rc != AGENT_OK) {
        string_free(out);
        return oauth_fail("cannot read OAuth token file");
    }
    return AGENT_OK;
}

static int parse_token_object(const JsonVal* root, OAuthToken* out) {
    const char* access = root != NULL ? json_obj_get_str(root, "access_token") : NULL;
    if (access == NULL) {
        access = root != NULL ? json_obj_get_str(root, "access") : NULL;
    }
    const char* refresh = root != NULL ? json_obj_get_str(root, "refresh_token") : NULL;
    if (refresh == NULL) {
        refresh = root != NULL ? json_obj_get_str(root, "refresh") : NULL;
    }
    const char* account = root != NULL ? json_obj_get_str(root, "account_id") : NULL;
    if (account == NULL) {
        account = root != NULL ? json_obj_get_str(root, "accountId") : NULL;
    }
    int64_t expires = root != NULL ? json_obj_get_int(root, "expires_at", 0) : 0;
    if (expires == 0 && root != NULL) {
        expires = json_obj_get_int(root, "expires", 0);
    }
    /* Codex auth.json stores `expires` in milliseconds, while legacy files
     * and OAuthToken use Unix seconds. Normalize both accepted fields before
     * comparing against time(NULL), otherwise saved tokens never refresh. */
    if (expires >= 100000000000LL) {
        expires /= 1000;
    }
    oauth_token_init(out);
    int rc = AGENT_OK;
    if (access == NULL || refresh == NULL || account == NULL || access[0] == '\0' ||
        refresh[0] == '\0' || account[0] == '\0') {
        rc = oauth_fail("OAuth token file is incomplete");
    } else if (replace_string(&out->access_token, access) != AGENT_OK ||
               replace_string(&out->refresh_token, refresh) != AGENT_OK ||
               replace_string(&out->account_id, account) != AGENT_OK) {
        rc = AGENT_ERR_OOM;
    } else {
        out->expires_at = expires;
    }
    if (rc != AGENT_OK) {
        oauth_token_free(out);
    }
    return rc;
}

static int ensure_parent(const char* path) {
    char copy[PATH_MAX];
    if (snprintf(copy, sizeof(copy), "%s", path) >= (int)sizeof(copy)) {
        return oauth_fail("OAuth token path is too long");
    }
    char* slash = strrchr(copy, '/');
    if (slash == NULL) {
        return AGENT_OK;
    }
    *slash = '\0';
    char* p = copy;
    if (*p == '/') {
        p++;
    }
    for (; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(copy, 0700);
            *p = '/';
        }
    }
    if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
        return oauth_fail("cannot create OAuth token directory");
    }
    return AGENT_OK;
}

typedef struct {
    JsonBuilder* builder;
    JsonMut* root;
    const char* skip_key;
} AuthCopyContext;

static int copy_auth_member(const char* key, const JsonVal* value, void* userdata) {
    AuthCopyContext* ctx = userdata;
    if (ctx == NULL || key == NULL || value == NULL || !json_val_is_obj(value)) {
        return AGENT_OK;
    }
    if (ctx->skip_key != NULL && strcmp(key, ctx->skip_key) == 0) {
        return AGENT_OK;
    }
    const char* type = json_obj_get_str(value, "type");
    if (type != NULL && strcmp(type, "api_key") == 0) {
        const char* api_key = json_obj_get_str(value, "key");
        if (api_key == NULL) {
            api_key = json_obj_get_str(value, "api_key");
        }
        if (api_key == NULL || api_key[0] == '\0') {
            return AGENT_OK;
        }
        JsonMut* entry = json_builder_obj_add_obj(ctx->builder, ctx->root, key);
        return entry == NULL ||
                       json_builder_obj_add_str(ctx->builder, entry, "type", "api_key") != AGENT_OK ||
                       json_builder_obj_add_str(ctx->builder, entry, "key", api_key) != AGENT_OK
                   ? AGENT_ERR_OOM
                   : AGENT_OK;
    }
    if (type != NULL && strcmp(type, "oauth") == 0) {
        return json_builder_obj_add_val_copy(ctx->builder, ctx->root, key, value);
    }
    return AGENT_OK;
}

static int write_auth_json(const char* path, const String* json) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(tmp)) {
        return oauth_fail("auth file path is too long");
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return oauth_fail("cannot create auth file");
    }
    (void)fchmod(fd, 0600);
    size_t written = 0;
    while (written < json->len) {
        ssize_t n = write(fd, json->data + written, json->len - written);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        written += (size_t)n;
    }
    bool newline_written = false;
    while (written == json->len) {
        ssize_t n = write(fd, "\n", 1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        newline_written = n == 1;
        break;
    }
    int sync_rc = fsync(fd);
    int close_rc = close(fd);
    if (written != json->len || !newline_written || sync_rc != 0 || close_rc != 0 ||
        rename(tmp, path) != 0) {
        unlink(tmp);
        return oauth_fail("cannot atomically save auth file");
    }
    (void)chmod(path, 0600);
    return AGENT_OK;
}

static int save_token(const char* path, const char* provider_name, const OAuthToken* token) {
    if (path == NULL || token == NULL || token->access_token == NULL ||
        token->refresh_token == NULL || token->account_id == NULL) {
        return oauth_fail("cannot save incomplete OAuth token");
    }
    if (ensure_parent(path) != AGENT_OK) {
        return AGENT_ERR_IO;
    }

    JsonBuilder* builder = json_builder_new();
    JsonMut* root = builder != NULL ? json_builder_root_obj(builder) : NULL;
    String existing = string_new();
    JsonDoc* old_doc = NULL;
    if (root != NULL && read_file(path, &existing) == AGENT_OK) {
        old_doc = json_parse(existing.data, existing.len);
        JsonVal* old_root = old_doc != NULL ? json_root(old_doc) : NULL;
        if (old_root != NULL && json_val_is_obj(old_root) &&
            json_val_obj_get(old_root, "type") == NULL &&
            json_val_obj_get(old_root, "access_token") == NULL &&
            json_val_obj_get(old_root, "access") == NULL) {
            AuthCopyContext copy = {builder, root, provider_name != NULL ? provider_name : "openai-codex"};
            if (json_obj_foreach(old_root, copy_auth_member, &copy) != AGENT_OK) {
                json_doc_free(old_doc);
                string_free(&existing);
                json_builder_free(builder);
                return AGENT_ERR_JSON;
            }
        }
    }
    const char* provider = provider_name != NULL ? provider_name : "openai-codex";
    JsonMut* entry = root != NULL ? json_builder_obj_add_obj(builder, root, provider) : NULL;
    int64_t expires_ms = token->expires_at > INT64_MAX / 1000
                             ? INT64_MAX
                             : token->expires_at * 1000;
    String json = string_new();
    int rc = (entry == NULL ||
              json_builder_obj_add_str(builder, entry, "type", "oauth") != AGENT_OK ||
              json_builder_obj_add_str(builder, entry, "access", token->access_token) != AGENT_OK ||
              json_builder_obj_add_str(builder, entry, "refresh", token->refresh_token) != AGENT_OK ||
              json_builder_obj_add_int(builder, entry, "expires", expires_ms) != AGENT_OK ||
              json_builder_obj_add_str(builder, entry, "accountId", token->account_id) != AGENT_OK)
                 ? AGENT_ERR_OOM
                 : json_builder_stringify_pretty(builder, &json);
    json_builder_free(builder);
    json_doc_free(old_doc);
    string_free(&existing);
    if (rc != AGENT_OK) {
        string_free(&json);
        return rc;
    }

    int write_rc = write_auth_json(path, &json);
    string_free(&json);
    return write_rc;
}

int auth_save_api_key(const char* path, const char* provider, const char* key) {
    if (path == NULL || provider == NULL || provider[0] == '\0' || key == NULL || key[0] == '\0') {
        return oauth_fail("invalid API key auth entry");
    }
    if (ensure_parent(path) != AGENT_OK) {
        return AGENT_ERR_IO;
    }
    JsonBuilder* builder = json_builder_new();
    JsonMut* root = builder != NULL ? json_builder_root_obj(builder) : NULL;
    String existing = string_new();
    JsonDoc* old_doc = NULL;
    if (root != NULL && read_file(path, &existing) == AGENT_OK) {
        old_doc = json_parse(existing.data, existing.len);
        JsonVal* old_root = old_doc != NULL ? json_root(old_doc) : NULL;
        if (old_root != NULL && json_val_is_obj(old_root) &&
            json_val_obj_get(old_root, "type") == NULL &&
            json_val_obj_get(old_root, "access_token") == NULL &&
            json_val_obj_get(old_root, "access") == NULL) {
            AuthCopyContext copy = {builder, root, provider};
            if (json_obj_foreach(old_root, copy_auth_member, &copy) != AGENT_OK) {
                json_doc_free(old_doc);
                string_free(&existing);
                json_builder_free(builder);
                return AGENT_ERR_JSON;
            }
        }
    }
    JsonMut* entry = root != NULL ? json_builder_obj_add_obj(builder, root, provider) : NULL;
    String json = string_new();
    int rc = (entry == NULL ||
              json_builder_obj_add_str(builder, entry, "type", "api_key") != AGENT_OK ||
              json_builder_obj_add_str(builder, entry, "key", key) != AGENT_OK)
                 ? AGENT_ERR_OOM
                 : json_builder_stringify_pretty(builder, &json);
    json_builder_free(builder);
    json_doc_free(old_doc);
    string_free(&existing);
    if (rc != AGENT_OK) {
        string_free(&json);
        return rc;
    }
    rc = write_auth_json(path, &json);
    string_free(&json);
    return rc;
}

int oauth_default_path(char* out, size_t cap) {
    const char* home = getenv("HOME");
    if (home == NULL || home[0] == '\0' || out == NULL || cap == 0) {
        return oauth_fail("HOME is not set; cannot locate OAuth token file");
    }
    if (snprintf(out, cap, "%s/.config/cagent/auth.json", home) >= (int)cap) {
        return oauth_fail("OAuth token path is too long");
    }
    return AGENT_OK;
}

static int read_auth_data(const char* path, String* data) {
    int rc = read_file(path, data);
    if (rc != AGENT_OK && path != NULL && strstr(path, "/auth.json") != NULL) {
        char legacy[PATH_MAX];
        if (snprintf(legacy, sizeof(legacy), "%s", path) < (int)sizeof(legacy)) {
            char* suffix = strstr(legacy, "/auth.json");
            if (suffix != NULL) {
                memcpy(suffix, "/oauth.json", sizeof("/oauth.json"));
                rc = read_file(legacy, data);
            }
        }
    }
    return rc;
}

int oauth_load_provider(const char* path, const char* provider, OAuthToken* out) {
    if (path == NULL || out == NULL) {
        return oauth_fail("invalid auth file path");
    }
    String data = string_new();
    int rc = read_auth_data(path, &data);
    if (rc == AGENT_OK) {
        JsonDoc* doc = json_parse(data.data, data.len);
        JsonVal* root = doc != NULL ? json_root(doc) : NULL;
        JsonVal* entry = root;
        if (root != NULL && json_val_is_obj(root) &&
            json_val_obj_get(root, "access_token") == NULL &&
            json_val_obj_get(root, "access") == NULL) {
            const char* name = provider != NULL ? provider : "openai-codex";
            entry = json_val_obj_get(root, name);
            if (entry == NULL && strcmp(name, "chatgpt") == 0) {
                entry = json_val_obj_get(root, "openai-codex");
            }
        }
        rc = entry != NULL ? parse_token_object(entry, out) : AGENT_ERR_AUTH;
        json_doc_free(doc);
        string_free(&data);
    }
    return rc;
}

int oauth_load(const char* path, OAuthToken* out) {
    return oauth_load_provider(path, NULL, out);
}

bool oauth_provider_configured(const char* path, const char* provider) {
    OAuthToken token;
    oauth_token_init(&token);
    int rc = oauth_load_provider(path, provider, &token);
    oauth_token_free(&token);
    return rc == AGENT_OK;
}

bool oauth_token_expiring(const OAuthToken* token, int64_t within_seconds) {
    return token == NULL || token->expires_at <= 0 ||
           token->expires_at <= (int64_t)time(NULL) + within_seconds;
}

int oauth_refresh_provider(const char* path, const char* provider, OAuthToken* token) {
    if (path == NULL || token == NULL || token->refresh_token == NULL ||
        token->refresh_token[0] == '\0') {
        return oauth_fail("OAuth refresh token is unavailable");
    }
    String body = string_new();
    if (string_printf(&body, "{\"client_id\":\"%s\",\"grant_type\":\"refresh_token\","
                      "\"refresh_token\":\"%s\"}", OAUTH_CLIENT_ID, token->refresh_token) != AGENT_OK) {
        string_free(&body);
        return AGENT_ERR_OOM;
    }
    String response = string_new();
    long status = 0;
    int rc = oauth_post(OAUTH_ISSUER "/oauth/token", body.data, "application/json", &status,
                        &response);
    string_free(&body);
    if (rc != AGENT_OK) {
        string_free(&response);
        return rc;
    }
    if (status < 200 || status >= 300) {
        string_free(&response);
        return oauth_fail("OAuth refresh was rejected; run cagent --login again");
    }
    OAuthToken next;
    rc = parse_token_response(&response, token, &next);
    string_free(&response);
    if (rc != AGENT_OK) {
        return rc;
    }
    oauth_token_free(token);
    *token = next;
    rc = save_token(path, provider, token);
    if (rc != AGENT_OK) {
        return rc;
    }
    return AGENT_OK;
}

int oauth_refresh(const char* path, OAuthToken* token) {
    return oauth_refresh_provider(path, NULL, token);
}

int oauth_remove_provider(const char* path, const char* provider) {
    if (path == NULL || provider == NULL || provider[0] == '\0') {
        return oauth_fail("invalid auth file path");
    }
    String data = string_new();
    int rc = read_auth_data(path, &data);
    if (rc != AGENT_OK) {
        string_free(&data);
        return rc == AGENT_ERR_IO ? AGENT_OK : rc;
    }
    JsonDoc* doc = json_parse(data.data, data.len);
    JsonVal* old_root = doc != NULL ? json_root(doc) : NULL;
    if (old_root == NULL || !json_val_is_obj(old_root) ||
        json_val_obj_get(old_root, "type") != NULL ||
        json_val_obj_get(old_root, "access_token") != NULL ||
        json_val_obj_get(old_root, "access") != NULL) {
        json_doc_free(doc);
        string_free(&data);
        return unlink(path) == 0 || errno == ENOENT ? AGENT_OK : AGENT_ERR_IO;
    }
    JsonBuilder* builder = json_builder_new();
    JsonMut* root = builder != NULL ? json_builder_root_obj(builder) : NULL;
    AuthCopyContext copy = {builder, root, provider};
    if (root == NULL || json_obj_foreach(old_root, copy_auth_member, &copy) != AGENT_OK) {
        json_builder_free(builder);
        json_doc_free(doc);
        string_free(&data);
        return AGENT_ERR_OOM;
    }
    String output = string_new();
    rc = json_builder_stringify_pretty(builder, &output);
    json_builder_free(builder);
    json_doc_free(doc);
    string_free(&data);
    if (rc == AGENT_OK) {
        rc = write_auth_json(path, &output);
    }
    string_free(&output);
    return rc;
}

int oauth_remove(const char* path) {
    return oauth_remove_provider(path, "openai-codex");
}

static int exchange_code(const char* code, const char* redirect_uri, const char* verifier,
                         OAuthToken* out) {
    char* ecode = curl_easy_escape(NULL, code, 0);
    char* eredirect = curl_easy_escape(NULL, redirect_uri, 0);
    char* everifier = curl_easy_escape(NULL, verifier, 0);
    if (ecode == NULL || eredirect == NULL || everifier == NULL) {
        curl_free(ecode);
        curl_free(eredirect);
        curl_free(everifier);
        return AGENT_ERR_OOM;
    }
    String body = string_new();
    int rc = string_printf(&body, "grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&code_verifier=%s",
                           ecode, eredirect, OAUTH_CLIENT_ID, everifier);
    curl_free(ecode);
    curl_free(eredirect);
    curl_free(everifier);
    if (rc != AGENT_OK) {
        string_free(&body);
        return rc;
    }
    String response = string_new();
    long status = 0;
    rc = oauth_post(OAUTH_ISSUER "/oauth/token", body.data,
                    "application/x-www-form-urlencoded", &status, &response);
    string_free(&body);
    if (rc == AGENT_OK && (status < 200 || status >= 300)) {
        string_free(&response);
        return oauth_fail("OAuth authorization code was rejected");
    }
    if (rc == AGENT_OK) {
        rc = parse_token_response(&response, NULL, out);
    }
    string_free(&response);
    return rc;
}

static int browser_login(const char* path, OAuthToken* out) {
    char verifier[128], challenge[128], state[128];
    if (make_pkce(verifier, sizeof(verifier), challenge, sizeof(challenge), state, sizeof(state)) != AGENT_OK) {
        return AGENT_ERR_AUTH;
    }
    int port = 0;
    int listener = bind_listener(&port);
    if (listener < 0) {
        return AGENT_ERR_AUTH;
    }
    char redirect_uri[128];
    snprintf(redirect_uri, sizeof(redirect_uri), "http://localhost:%d%s", port, OAUTH_CALLBACK_PATH);
    String url = string_new();
    int rc = build_authorize_url(redirect_uri, verifier, challenge, state, &url);
    if (rc != AGENT_OK) {
        close(listener);
        return rc;
    }
    printf("Starting ChatGPT OAuth login...\n");
    open_browser(url.data);
    string_free(&url);
    char code[8192];
    bool done = false;
    rc = wait_for_callback(listener, state, code, sizeof(code), NULL, 0, &done);
    close(listener);
    if (rc != AGENT_OK) {
        return rc;
    }
    rc = exchange_code(code, redirect_uri, verifier, out);
    if (rc != AGENT_OK) {
        return rc;
    }
    rc = save_token(path, "openai-codex", out);
    if (rc == AGENT_OK) {
        printf("ChatGPT login completed.\n");
    }
    return rc;
}

static int parse_device_response(const String* response, const char* key, char* out, size_t cap) {
    JsonDoc* doc = json_parse(response->data, response->len);
    if (doc == NULL) {
        return AGENT_ERR_JSON;
    }
    JsonVal* root = json_root(doc);
    const char* value = root != NULL ? json_obj_get_str(root, key) : NULL;
    int rc = value != NULL ? (snprintf(out, cap, "%s", value) < (int)cap ? AGENT_OK : AGENT_ERR_OOM)
                           : AGENT_ERR_IO;
    json_doc_free(doc);
    return rc;
}

static int device_login(const char* path, OAuthToken* out) {
    String request = string_new();
    if (string_printf(&request, "{\"client_id\":\"%s\"}", OAUTH_CLIENT_ID) != AGENT_OK) {
        string_free(&request);
        return AGENT_ERR_OOM;
    }
    String response = string_new();
    long status = 0;
    int rc = oauth_post(OAUTH_ISSUER "/api/accounts/deviceauth/usercode", request.data,
                        "application/json", &status, &response);
    string_free(&request);
    if (rc != AGENT_OK || status < 200 || status >= 300) {
        string_free(&response);
        return oauth_fail("ChatGPT device-code login is unavailable");
    }
    char device_id[512], user_code[512], interval_buf[32];
    rc = parse_device_response(&response, "device_auth_id", device_id, sizeof(device_id));
    if (rc == AGENT_OK) {
        rc = parse_device_response(&response, "user_code", user_code, sizeof(user_code));
    }
    int64_t interval = 5;
    if (parse_device_response(&response, "interval", interval_buf, sizeof(interval_buf)) == AGENT_OK) {
        interval = strtoll(interval_buf, NULL, 10);
        if (interval < 1 || interval > 60) {
            interval = 5;
        }
    }
    string_free(&response);
    if (rc != AGENT_OK) {
        return oauth_fail("invalid ChatGPT device-code response");
    }
    printf("Open https://auth.openai.com/codex/device and enter code %s\n", user_code);
    fflush(stdout);
    time_t deadline = time(NULL) + OAUTH_TIMEOUT_SECONDS;
    char authorization_code[8192] = {0};
    char verifier[256] = {0};
    while (time(NULL) < deadline) {
        String poll_body = string_new();
        if (string_printf(&poll_body, "{\"device_auth_id\":\"%s\",\"user_code\":\"%s\"}",
                          device_id, user_code) != AGENT_OK) {
            string_free(&poll_body);
            return AGENT_ERR_OOM;
        }
        String poll_response = string_new();
        status = 0;
        rc = oauth_post(OAUTH_ISSUER "/api/accounts/deviceauth/token", poll_body.data,
                        "application/json", &status, &poll_response);
        string_free(&poll_body);
        if (rc != AGENT_OK) {
            string_free(&poll_response);
            return rc;
        }
        if (status >= 200 && status < 300) {
            rc = parse_device_response(&poll_response, "authorization_code", authorization_code,
                                       sizeof(authorization_code));
            if (rc == AGENT_OK) {
                rc = parse_device_response(&poll_response, "code_verifier", verifier, sizeof(verifier));
            }
            string_free(&poll_response);
            if (rc == AGENT_OK) {
                break;
            }
            return oauth_fail("invalid ChatGPT device authorization response");
        }
        string_free(&poll_response);
        if (status != 403 && status != 404) {
            return oauth_fail("ChatGPT device authorization was rejected");
        }
        sleep((unsigned int)interval);
    }
    if (authorization_code[0] == '\0' || verifier[0] == '\0') {
        return oauth_fail("ChatGPT device-code login timed out");
    }
    rc = exchange_code(authorization_code, OAUTH_ISSUER "/deviceauth/callback", verifier, out);
    if (rc != AGENT_OK) {
        return rc;
    }
    rc = save_token(path, "openai-codex", out);
    if (rc == AGENT_OK) {
        printf("ChatGPT login completed.\n");
    }
    return rc;
}

int oauth_login(const char* path, bool device_code) {
    if (path == NULL) {
        return oauth_fail("invalid OAuth token path");
    }
    g_oauth_error[0] = '\0';
    OAuthToken token;
    oauth_token_init(&token);
    int rc = device_code ? device_login(path, &token) : browser_login(path, &token);
    oauth_token_free(&token);
    return rc;
}
