/*
 * session/session.c — JSONL session persistence.
 *
 * One line per JSON object; lines are appended with fopen("a"). Messages
 * are serialized with the same field names the agent loop uses, so a
 * resumed session feeds back into the model verbatim.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "session/session.h"
#include "util/json.h"
#include "util/string.h"

/* ---- helpers ---------------------------------------------------------- */

#define SESSION_MEMORY_MAX (32 * 1024)

static int memory_add(Session* s, const char* kind, const char* content) {
    if (s == NULL || content == NULL ||
        (s->memory != NULL && strlen(s->memory) >= SESSION_MEMORY_MAX)) {
        return AGENT_OK;
    }
    const char* label = kind != NULL && kind[0] != '\0' ? kind : "note";
    size_t old_len = s->memory != NULL ? strlen(s->memory) : 0;
    size_t label_len = strlen(label);
    size_t content_len = strlen(content);
    size_t room = SESSION_MEMORY_MAX - old_len;
    size_t need = label_len + content_len + 6;
    if (need > room) content_len = room > label_len + 6 ? room - label_len - 6 : 0;
    if (content_len == 0 && need > room) return AGENT_OK;
    char* grown = realloc(s->memory, old_len + label_len + content_len + 7);
    if (grown == NULL) return AGENT_ERR_OOM;
    s->memory = grown;
    int written = snprintf(s->memory + old_len, label_len + content_len + 7,
                           "[%s] %.*s\n", label, (int)content_len, content);
    return written >= 0 ? AGENT_OK : AGENT_ERR_IO;
}

static int mkdir_p(const char* path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) {
        return -1;
    }
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char* p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

const char* session_default_dir(void) {
    static char dir[4096];
    const char* home = getenv("HOME");
    if (home == NULL) {
        return NULL;
    }
    snprintf(dir, sizeof(dir), "%s/.local/state/cagent/sessions", home);
    return dir;
}

static void gen_id(char* out, size_t out_size) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    unsigned r = (unsigned)rand() & 0xffffu;
    snprintf(out, out_size, "%04d%02d%02d-%02d%02d%02d-%04x", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, r);
}

/* ---- lifecycle --------------------------------------------------------- */

Session* session_create(const char* dir, const char* cwd, const char* model_name,
                        const char* provider) {
    if (dir == NULL) {
        return NULL;
    }
    if (mkdir_p(dir) != 0 && access(dir, W_OK) != 0) {
        return NULL;
    }

    Session* s = calloc(1, sizeof(Session));
    if (s == NULL) {
        return NULL;
    }
    char id[64];
    gen_id(id, sizeof(id));
    s->id = strdup(id);
    if (s->id == NULL) {
        session_free(s);
        return NULL;
    }
    size_t path_len = strlen(dir) + strlen(id) + 8;
    s->path = malloc(path_len);
    if (s->path == NULL) {
        session_free(s);
        return NULL;
    }
    snprintf(s->path, path_len, "%s/%s.jsonl", dir, id);
    s->cwd = cwd != NULL ? strdup(cwd) : NULL;
    s->model_name = model_name != NULL ? strdup(model_name) : NULL;
    s->provider = provider != NULL ? strdup(provider) : NULL;
    s->created_at = (int64_t)time(NULL);
    s->updated_at = s->created_at;

    /* write the meta line */
    String line = string_new();
    JsonBuilder* b = json_builder_new();
    if (b == NULL) {
        string_free(&line);
        session_free(s);
        return NULL;
    }
    JsonMut* root = json_builder_root_obj(b);
    json_builder_obj_add_str(b, root, "type", "meta");
    json_builder_obj_add_str(b, root, "id", s->id);
    if (s->cwd != NULL) {
        json_builder_obj_add_str(b, root, "cwd", s->cwd);
    }
    if (s->model_name != NULL) {
        json_builder_obj_add_str(b, root, "model", s->model_name);
    }
    if (s->provider != NULL) {
        json_builder_obj_add_str(b, root, "provider", s->provider);
    }
    json_builder_obj_add_int(b, root, "created_at", s->created_at);
    json_builder_stringify(b, &line);
    json_builder_free(b);
    string_append_char(&line, '\n');

    FILE* f = fopen(s->path, "w");
    if (f == NULL) {
        string_free(&line);
        session_free(s);
        return NULL;
    }
    fwrite(line.data, 1, line.len, f);
    fclose(f);
    string_free(&line);
    return s;
}

Session* session_open(const char* dir, const char* id) {
    if (dir == NULL || id == NULL) {
        return NULL;
    }
    Session* s = calloc(1, sizeof(Session));
    if (s == NULL) {
        return NULL;
    }
    s->id = strdup(id);
    size_t path_len = strlen(dir) + strlen(id) + 8;
    s->path = malloc(path_len);
    if (s->id == NULL || s->path == NULL) {
        session_free(s);
        return NULL;
    }
    snprintf(s->path, path_len, "%s/%s.jsonl", dir, id);

    struct stat st;
    if (stat(s->path, &st) != 0) {
        session_free(s);
        return NULL;
    }
    FILE* f = fopen(s->path, "rb");
    if (f == NULL) {
        session_free(s);
        return NULL;
    }
    long size = st.st_size;
    char* data = malloc((size_t)size + 1);
    if (data == NULL) {
        fclose(f);
        session_free(s);
        return NULL;
    }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[got] = '\0';

    /* scan lines for meta and stats */
    size_t off = 0;
    while (off < got) {
        char* nl = memchr(data + off, '\n', got - off);
        size_t line_len = nl != NULL ? (size_t)(nl - (data + off)) : got - off;
        JsonDoc* doc = json_parse(data + off, line_len);
        if (doc != NULL) {
            JsonVal* root = json_root(doc);
            if (root != NULL && json_val_is_obj(root)) {
                const char* type = json_obj_get_str(root, "type");
                if (type != NULL && strcmp(type, "meta") == 0) {
                    const char* v = json_obj_get_str(root, "cwd");
                    if (v != NULL) {
                        free(s->cwd);
                        s->cwd = strdup(v);
                    }
                    v = json_obj_get_str(root, "model");
                    if (v != NULL) {
                        free(s->model_name);
                        s->model_name = strdup(v);
                    }
                    v = json_obj_get_str(root, "provider");
                    if (v != NULL) {
                        free(s->provider);
                        s->provider = strdup(v);
                    }
                    s->created_at = json_obj_get_int(root, "created_at", 0);
                } else if (type != NULL && strcmp(type, "memory") == 0) {
                    (void)memory_add(s, json_obj_get_str(root, "kind"),
                                     json_obj_get_str(root, "content"));
                } else if (type != NULL && strcmp(type, "stats") == 0) {
                    s->request_count = (uint64_t)json_obj_get_int(root, "requests", 0);
                    s->tool_call_count = (uint64_t)json_obj_get_int(root, "tool_calls", 0);
                    s->model_time_ms = json_obj_get_int(root, "model_time_ms", 0);
                    s->usage.input_tokens = json_obj_get_int(root, "tokens_in", 0);
                    s->usage.output_tokens = json_obj_get_int(root, "tokens_out", 0);
                    s->usage.cached_tokens = json_obj_get_int(root, "cached", 0);
                    s->usage.total_tokens = json_obj_get_int(root, "total", 0);
                }
            }
            json_doc_free(doc);
        }
        off += line_len + 1;
    }
    free(data);
    s->updated_at = (int64_t)time(NULL);
    return s;
}
void session_free(Session* s) {
    if (s == NULL) {
        return;
    }
    free(s->id);
    free(s->path);
    free(s->cwd);
    free(s->model_name);
    free(s->provider);
    free(s->memory);
    free(s);
}

/* ---- message serialization --------------------------------------------- */

static void add_tool_calls(JsonBuilder* b, JsonMut* obj, const ToolCallList* calls) {
    JsonMut* arr = json_builder_obj_add_arr(b, obj, "tool_calls");
    if (arr == NULL) {
        return;
    }
    for (size_t i = 0; i < calls->len; i++) {
        const ToolCall* tc = &calls->items[i];
        JsonMut* c = json_builder_arr_add_obj(b, arr);
        if (c == NULL) {
            return;
        }
        if (tc->id != NULL) {
            json_builder_obj_add_str(b, c, "id", tc->id);
        }
        if (tc->name != NULL) {
            json_builder_obj_add_str(b, c, "name", tc->name);
        }
        if (tc->arguments != NULL) {
            json_builder_obj_add_str(b, c, "arguments", tc->arguments);
        }
    }
}

static int append_line(Session* s, const String* line) {
    FILE* f = fopen(s->path, "a");
    if (f == NULL) {
        return AGENT_ERR_IO;
    }
    fwrite(line->data, 1, line->len, f);
    fclose(f);
    s->updated_at = (int64_t)time(NULL);
    return AGENT_OK;
}

int session_append_message(Session* s, const Message* m) {
    if (s == NULL || m == NULL) {
        return AGENT_ERR_IO;
    }
    JsonBuilder* b = json_builder_new();
    if (b == NULL) {
        return AGENT_ERR_OOM;
    }
    JsonMut* root = json_builder_root_obj(b);
    json_builder_obj_add_str(b, root, "type", "message");
    json_builder_obj_add_str(b, root, "role", message_role_name(m->role));
    if (m->content != NULL) {
        json_builder_obj_add_str(b, root, "content", m->content);
    }
    if (m->reasoning != NULL) {
        json_builder_obj_add_str(b, root, "reasoning", m->reasoning);
    }
    if (m->tool_call_id != NULL) {
        json_builder_obj_add_str(b, root, "tool_call_id", m->tool_call_id);
    }
    if (m->is_error) {
        json_builder_obj_add_bool(b, root, "is_error", true);
    }
    if (m->tool_calls.len > 0) {
        add_tool_calls(b, root, &m->tool_calls);
    }
    if (m->usage.total_tokens > 0 || m->usage.input_tokens > 0) {
        JsonMut* u = json_builder_obj_add_obj(b, root, "usage");
        if (u != NULL) {
            json_builder_obj_add_int(b, u, "in", m->usage.input_tokens);
            json_builder_obj_add_int(b, u, "out", m->usage.output_tokens);
            json_builder_obj_add_int(b, u, "cached", m->usage.cached_tokens);
            json_builder_obj_add_int(b, u, "total", m->usage.total_tokens);
        }
    }

    String line = string_new();
    int err = json_builder_stringify(b, &line);
    json_builder_free(b);
    if (err != AGENT_OK) {
        string_free(&line);
        return err;
    }
    string_append_char(&line, '\n');
    err = append_line(s, &line);
    string_free(&line);
    return err;
}

int session_append_compaction(Session* s, size_t start, size_t count, const char* summary) {
    if (s == NULL || summary == NULL || count == 0) {
        return AGENT_ERR_IO;
    }
    JsonBuilder* b = json_builder_new();
    if (b == NULL) {
        return AGENT_ERR_OOM;
    }
    JsonMut* root = json_builder_root_obj(b);
    json_builder_obj_add_str(b, root, "type", "compaction");
    json_builder_obj_add_int(b, root, "start", (int64_t)start);
    json_builder_obj_add_int(b, root, "count", (int64_t)count);
    json_builder_obj_add_str(b, root, "summary", summary);

    String line = string_new();
    int err = json_builder_stringify(b, &line);
    json_builder_free(b);
    if (err != AGENT_OK) {
        string_free(&line);
        return err;
    }
    string_append_char(&line, '\n');
    err = append_line(s, &line);
    string_free(&line);
    return err;
}

int session_append_memory(Session* s, const char* kind, const char* content) {
    if (s == NULL || content == NULL || content[0] == '\0') return AGENT_ERR_IO;
    JsonBuilder* b = json_builder_new();
    if (b == NULL) return AGENT_ERR_OOM;
    JsonMut* root = json_builder_root_obj(b);
    json_builder_obj_add_str(b, root, "type", "memory");
    json_builder_obj_add_str(b, root, "kind", kind != NULL && kind[0] != '\0' ? kind : "note");
    json_builder_obj_add_str(b, root, "content", content);
    String line = string_new();
    int err = json_builder_stringify(b, &line);
    json_builder_free(b);
    if (err != AGENT_OK) { string_free(&line); return err; }
    string_append_char(&line, '\n');
    err = append_line(s, &line);
    string_free(&line);
    if (err == AGENT_OK) err = memory_add(s, kind, content);
    return err;
}

const char* session_memory(const Session* s) {
    return s != NULL ? s->memory : NULL;
}

int session_append_stats(Session* s) {
    if (s == NULL) {
        return AGENT_ERR_IO;
    }
    JsonBuilder* b = json_builder_new();
    if (b == NULL) {
        return AGENT_ERR_OOM;
    }
    JsonMut* root = json_builder_root_obj(b);
    json_builder_obj_add_str(b, root, "type", "stats");
    json_builder_obj_add_int(b, root, "requests", (int64_t)s->request_count);
    json_builder_obj_add_int(b, root, "tool_calls", (int64_t)s->tool_call_count);
    json_builder_obj_add_int(b, root, "model_time_ms", s->model_time_ms);
    json_builder_obj_add_int(b, root, "tokens_in", s->usage.input_tokens);
    json_builder_obj_add_int(b, root, "tokens_out", s->usage.output_tokens);
    json_builder_obj_add_int(b, root, "cached", s->usage.cached_tokens);
    json_builder_obj_add_int(b, root, "total", s->usage.total_tokens);

    String line = string_new();
    int err = json_builder_stringify(b, &line);
    json_builder_free(b);
    if (err != AGENT_OK) {
        string_free(&line);
        return err;
    }
    string_append_char(&line, '\n');
    err = append_line(s, &line);
    string_free(&line);
    return err;
}

/* ---- loading ------------------------------------------------------------ */

static int parse_tool_calls(JsonVal* root, ToolCallList* out) {
    JsonVal* calls = json_val_obj_get(root, "tool_calls");
    if (calls == NULL) {
        return AGENT_OK;
    }
    size_t n = json_val_arr_size(calls);
    for (size_t i = 0; i < n; i++) {
        JsonVal* c = json_val_arr_get(calls, i);
        if (c == NULL) {
            continue;
        }
        ToolCall* tc = calloc(1, sizeof(ToolCall));
        if (tc == NULL) {
            return AGENT_ERR_OOM;
        }
        const char* v = json_obj_get_str(c, "id");
        if (v != NULL) {
            tc->id = strdup(v);
        }
        v = json_obj_get_str(c, "name");
        if (v != NULL) {
            tc->name = strdup(v);
        }
        v = json_obj_get_str(c, "arguments");
        if (v != NULL) {
            tc->arguments = strdup(v);
        }
        if (tool_call_list_append(out, tc) != AGENT_OK) {
            free(tc);
            return AGENT_ERR_OOM;
        }
    }
    return AGENT_OK;
}

static int parse_message(JsonVal* root, Message* out) {
    const char* role = json_obj_get_str(root, "role");
    MessageRole r;
    if (role == NULL || strcmp(role, "system") == 0) {
        r = MSG_SYSTEM;
    } else if (strcmp(role, "user") == 0) {
        r = MSG_USER;
    } else if (strcmp(role, "assistant") == 0) {
        r = MSG_ASSISTANT;
    } else if (strcmp(role, "tool") == 0) {
        r = MSG_TOOL;
    } else {
        return AGENT_ERR_JSON;
    }
    out->role = r;

    const char* v = json_obj_get_str(root, "content");
    if (v != NULL) {
        out->content = strdup(v);
    }
    v = json_obj_get_str(root, "reasoning");
    if (v != NULL) {
        out->reasoning = strdup(v);
    }
    v = json_obj_get_str(root, "tool_call_id");
    if (v != NULL) {
        out->tool_call_id = strdup(v);
    }
    out->is_error = json_obj_get_bool(root, "is_error", false);
    if (parse_tool_calls(root, &out->tool_calls) != AGENT_OK) {
        return AGENT_ERR_OOM;
    }
    JsonVal* u = json_val_obj_get(root, "usage");
    if (u != NULL && json_val_is_obj(u)) {
        out->usage.input_tokens = json_obj_get_int(u, "in", 0);
        out->usage.output_tokens = json_obj_get_int(u, "out", 0);
        out->usage.cached_tokens = json_obj_get_int(u, "cached", 0);
        out->usage.total_tokens = json_obj_get_int(u, "total", 0);
    }
    return AGENT_OK;
}

int session_load_messages(Session* s, MessageList* out) {
    if (s == NULL || out == NULL) {
        return AGENT_ERR_IO;
    }
    struct stat st;
    if (stat(s->path, &st) != 0) {
        return AGENT_ERR_IO;
    }
    FILE* f = fopen(s->path, "rb");
    if (f == NULL) {
        return AGENT_ERR_IO;
    }
    long size = st.st_size;
    char* data = malloc((size_t)size + 1);
    if (data == NULL) {
        fclose(f);
        return AGENT_ERR_OOM;
    }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[got] = '\0';

    int err = AGENT_OK;
    size_t off = 0;
    while (err == AGENT_OK && off < got) {
        char* nl = memchr(data + off, '\n', got - off);
        size_t line_len = nl != NULL ? (size_t)(nl - (data + off)) : got - off;
        JsonDoc* doc = json_parse(data + off, line_len);
        if (doc != NULL) {
            JsonVal* root = json_root(doc);
            const char* type = root != NULL ? json_obj_get_str(root, "type") : NULL;
            if (type != NULL && strcmp(type, "message") == 0) {
                Message* m = message_new(MSG_USER); /* role fixed below */
                if (m == NULL) {
                    err = AGENT_ERR_OOM;
                } else if (parse_message(root, m) != AGENT_OK ||
                           message_list_append(out, m) != AGENT_OK) {
                    message_free(m);
                    err = AGENT_ERR_JSON;
                }
            } else if (type != NULL && strcmp(type, "compaction") == 0) {
                const char* summary = json_obj_get_str(root, "summary");
                int64_t start = json_obj_get_int(root, "start", -1);
                int64_t count = json_obj_get_int(root, "count", -1);
                if (summary == NULL || start < 0 || count <= 0 ||
                    (uint64_t)start > out->len ||
                    (uint64_t)count > out->len - (size_t)start) {
                    err = AGENT_ERR_JSON;
                } else {
                    size_t safe_count = message_list_tool_safe_prefix_count(
                        out, (size_t)start, (size_t)count);
                    if (safe_count > 0) {
                        Message* m = message_new(MSG_USER);
                        if (m == NULL || message_set_content(m, summary) != AGENT_OK ||
                            message_list_insert(out, (size_t)start, m) != AGENT_OK) {
                            message_free(m);
                            err = AGENT_ERR_OOM;
                        } else {
                            message_list_remove_range(out, (size_t)start + 1, safe_count);
                        }
                    }
                }
            }
            json_doc_free(doc);
        }
        off += line_len + 1;
    }
    free(data);
    return err;
}