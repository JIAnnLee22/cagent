/*
 * agent/context.c — context management.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/context.h"
#include "agent/message.h"
#include "tool/registry.h"
#include "util/string.h"

#define OUTPUT_RESERVE 8192
#define COMPACT_MIN_KEEP 4
#define SUMMARY_INPUT_MAX_CHARS (256 * 1024)

static void count_text_units(const char* text, int64_t* ascii_bytes, int64_t* codepoints) {
    if (text == NULL || ascii_bytes == NULL || codepoints == NULL) {
        return;
    }
    const unsigned char* bytes = (const unsigned char*)text;
    size_t len = strlen(text);
    size_t i = 0;
    while (i < len) {
        unsigned char c = bytes[i];
        if (c < 0x80) {
            (*ascii_bytes)++;
            i++;
            continue;
        }
        /* Count one token-like unit per UTF-8 leading byte.  Malformed input
         * is conservatively counted as one unit per byte. */
        (*codepoints)++;
        size_t width = 1;
        if ((c & 0xe0) == 0xc0 && i + 1 < len && (bytes[i + 1] & 0xc0) == 0x80) {
            width = 2;
        } else if ((c & 0xf0) == 0xe0 && i + 2 < len &&
                   (bytes[i + 1] & 0xc0) == 0x80 && (bytes[i + 2] & 0xc0) == 0x80) {
            width = 3;
        } else if ((c & 0xf8) == 0xf0 && i + 3 < len &&
                   (bytes[i + 1] & 0xc0) == 0x80 && (bytes[i + 2] & 0xc0) == 0x80 &&
                   (bytes[i + 3] & 0xc0) == 0x80) {
            width = 4;
        }
        i += width;
    }
}

static int64_t units_to_tokens(int64_t ascii_bytes, int64_t codepoints) {
    return ascii_bytes / 4 + codepoints;
}

int64_t context_estimate_tokens(const MessageList* msgs) {
    int64_t ascii_bytes = 0;
    int64_t codepoints = 0;
    if (msgs == NULL) {
        return 0;
    }
    for (size_t i = 0; i < msgs->len; i++) {
        const Message* m = &msgs->items[i];
        count_text_units(m->content, &ascii_bytes, &codepoints);
        count_text_units(m->reasoning, &ascii_bytes, &codepoints);
        for (size_t k = 0; k < m->tool_calls.len; k++) {
            const ToolCall* tc = &m->tool_calls.items[k];
            count_text_units(tc->name, &ascii_bytes, &codepoints);
            count_text_units(tc->arguments, &ascii_bytes, &codepoints);
            count_text_units(tc->result, &ascii_bytes, &codepoints);
        }
    }
    return units_to_tokens(ascii_bytes, codepoints);
}

static int64_t text_estimate_tokens(const char* text) {
    int64_t ascii_bytes = 0;
    int64_t codepoints = 0;
    count_text_units(text, &ascii_bytes, &codepoints);
    return units_to_tokens(ascii_bytes, codepoints);
}

int64_t context_estimate_request_tokens(const char* system_prompt, const MessageList* msgs,
                                        const ToolRegistry* tools) {
    int64_t estimate = context_estimate_tokens(msgs);
    estimate += text_estimate_tokens(system_prompt);
    if (tools != NULL) {
        size_t schema_bytes = 0;
        if (tool_registry_schema_bytes(tools, &schema_bytes) == AGENT_OK) {
            /* Tool schemas are predominantly ASCII JSON, so use the same
             * conservative four-bytes/token heuristic as message text. */
            estimate += (int64_t)(schema_bytes / 4);
        }
    }
    return estimate;
}

bool context_needs_compact(const Model* model, const MessageList* msgs) {
    return context_needs_compact_request(model, NULL, msgs, NULL);
}

bool context_needs_compact_request(const Model* model, const char* system_prompt,
                                   const MessageList* msgs, const ToolRegistry* tools) {
    int64_t estimate = context_estimate_request_tokens(system_prompt, msgs, tools);
    int64_t window = model != NULL ? model->context_window : 0;

    if (window <= 0) {
        /* unknown window: compact only for extreme sizes */
        return estimate >= 256 * 1024;
    }
    int64_t reserve = OUTPUT_RESERVE;
    if (window / 2 < reserve) {
        reserve = window / 2; /* tiny windows: never reserve more than half */
    }
    return estimate + reserve > window;
}

static int compaction_bounds(const MessageList* msgs, size_t keep_recent, size_t* start,
                             size_t* count) {
    if (msgs == NULL || start == NULL || count == NULL) {
        return AGENT_ERR_MODEL;
    }
    *start = 0;
    *count = 0;
    if (msgs->len <= COMPACT_MIN_KEEP + 2) {
        return AGENT_OK;
    }
    if (keep_recent < COMPACT_MIN_KEEP) {
        keep_recent = COMPACT_MIN_KEEP;
    }
    if (msgs->len <= keep_recent + 1) {
        return AGENT_OK;
    }

    *start = (msgs->items[0].role == MSG_SYSTEM) ? 1 : 0;
    *count = msgs->len - *start - keep_recent;
    *count = message_list_tool_safe_prefix_count(msgs, *start, *count);
    return AGENT_OK;
}

static int append_prompt_text(String* out, const char* text) {
    if (text == NULL || out->len >= SUMMARY_INPUT_MAX_CHARS) {
        return AGENT_OK;
    }
    size_t n = strlen(text);
    size_t room = SUMMARY_INPUT_MAX_CHARS - out->len;
    if (n > room) {
        n = room;
    }
    return string_append_n(out, text, n);
}

static int append_message_transcript(const Message* m, String* out) {
    if (string_printf(out, "\n--- %s ---\n", message_role_name(m->role)) != AGENT_OK) {
        return AGENT_ERR_OOM;
    }
    if (append_prompt_text(out, m->content) != AGENT_OK) {
        return AGENT_ERR_OOM;
    }
    if (out->len >= SUMMARY_INPUT_MAX_CHARS) {
        return AGENT_OK;
    }
    if (m->reasoning != NULL) {
        if (string_printf(out, "\n[reasoning]\n") != AGENT_OK ||
            append_prompt_text(out, m->reasoning) != AGENT_OK) {
            return AGENT_ERR_OOM;
        }
        if (out->len >= SUMMARY_INPUT_MAX_CHARS) {
            return AGENT_OK;
        }
    }
    for (size_t i = 0; i < m->tool_calls.len; i++) {
        const ToolCall* tc = &m->tool_calls.items[i];
        if (string_printf(out, "\n[tool call %s] %s\narguments: ",
                          tc->id != NULL ? tc->id : "", tc->name != NULL ? tc->name : "") !=
                AGENT_OK ||
            append_prompt_text(out, tc->arguments) != AGENT_OK ||
            string_printf(out, "\nresult: ") != AGENT_OK || append_prompt_text(out, tc->result) != AGENT_OK) {
            return AGENT_ERR_OOM;
        }
        if (out->len >= SUMMARY_INPUT_MAX_CHARS) {
            return AGENT_OK;
        }
    }
    return AGENT_OK;
}

static int build_summary_prompt(const MessageList* msgs, size_t start, size_t count,
                                String* out) {
    if (string_append(out,
                      "Summarize the earlier conversation below for a coding agent. "
                      "Preserve concrete facts, decisions, files, tool findings, "
                      "unfinished work, errors, and constraints needed to continue. "
                      "Be concise and do not invent facts. Return only the summary.\n") !=
        AGENT_OK) {
        return AGENT_ERR_OOM;
    }
    for (size_t i = 0; i < count; i++) {
        if (append_message_transcript(&msgs->items[start + i], out) != AGENT_OK) {
            return AGENT_ERR_OOM;
        }
    }
    if (out->len >= SUMMARY_INPUT_MAX_CHARS) {
        /* Keep the prompt NUL-terminated while making truncation explicit. */
        static const char marker[] = "\n[transcript truncated]\n";
        size_t marker_len = sizeof(marker) - 1;
        if (out->len >= marker_len) {
            memcpy(out->data + out->len - marker_len, marker, marker_len);
        }
    }
    return AGENT_OK;
}

int context_compaction_prepare(const MessageList* msgs, size_t keep_recent,
                               ContextCompactionRequest* out) {
    if (out == NULL) {
        return AGENT_ERR_MODEL;
    }
    memset(out, 0, sizeof(*out));
    int rc = compaction_bounds(msgs, keep_recent, &out->start, &out->count);
    if (rc != AGENT_OK || out->count == 0) {
        return rc;
    }

    String prompt = string_new();
    rc = build_summary_prompt(msgs, out->start, out->count, &prompt);
    if (rc != AGENT_OK) {
        string_free(&prompt);
        return rc;
    }
    Message* request_message = message_new(MSG_USER);
    if (request_message == NULL) {
        string_free(&prompt);
        return AGENT_ERR_OOM;
    }
    request_message->content = string_take(&prompt);
    rc = message_list_append(&out->request_messages, request_message);
    if (rc != AGENT_OK) {
        message_free(request_message);
        context_compaction_request_free(out);
        return rc;
    }
    return AGENT_OK;
}

void context_compaction_request_free(ContextCompactionRequest* request) {
    if (request == NULL) {
        return;
    }
    message_list_free(&request->request_messages);
    request->start = 0;
    request->count = 0;
}

int context_compaction_apply(MessageList* msgs, const ContextCompactionRequest* request,
                             const char* summary) {
    if (msgs == NULL || request == NULL || summary == NULL || request->count == 0 ||
        request->start >= msgs->len || request->count > msgs->len - request->start) {
        return AGENT_ERR_MODEL;
    }

    size_t safe_count =
        message_list_tool_safe_prefix_count(msgs, request->start, request->count);
    if (safe_count == 0) {
        return AGENT_ERR_MODEL;
    }

    Message* sm = message_new(MSG_USER);
    if (sm == NULL || message_set_content(sm, summary) != AGENT_OK) {
        message_free(sm);
        return AGENT_ERR_OOM;
    }

    /* Insert first. message_list_insert leaves sm owned by the caller on
     * failure, so the original range remains untouched. */
    if (message_list_insert(msgs, request->start, sm) != AGENT_OK) {
        message_free(sm);
        return AGENT_ERR_OOM;
    }
    message_list_remove_range(msgs, request->start + 1, safe_count);
    return AGENT_OK;
}

static int fallback_summary(const MessageList* msgs, size_t start, size_t count, String* out) {
    int roles[4] = {0};
    for (size_t i = 0; i < count; i++) {
        const Message* m = &msgs->items[start + i];
        if (m->role >= MSG_SYSTEM && m->role <= MSG_TOOL) {
            roles[m->role]++;
        }
    }
    return string_printf(out,
                         "[context compaction: %zu earlier message(s) omitted to "
                         "stay within the context window. Omitted: %d user, %d "
                         "assistant, %d tool, %d system. The most recent messages, "
                         "the current goal and the latest tool results are preserved below.]",
                         count, roles[MSG_USER], roles[MSG_ASSISTANT], roles[MSG_TOOL],
                         roles[MSG_SYSTEM]);
}

int context_compaction_apply_fallback(MessageList* msgs,
                                      const ContextCompactionRequest* request) {
    if (msgs == NULL || request == NULL || request->count == 0 || request->start >= msgs->len ||
        request->count > msgs->len - request->start) {
        return AGENT_OK;
    }
    String summary = string_new();
    int rc = fallback_summary(msgs, request->start, request->count, &summary);
    if (rc == AGENT_OK) {
        rc = context_compaction_apply(msgs, request, summary.data);
    }
    string_free(&summary);
    return rc;
}

int context_compact(MessageList* msgs, size_t keep_recent) {
    ContextCompactionRequest request = {0};
    int rc = context_compaction_prepare(msgs, keep_recent, &request);
    if (rc == AGENT_OK && request.count > 0) {
        rc = context_compaction_apply_fallback(msgs, &request);
    }
    context_compaction_request_free(&request);
    return rc;
}
