/*
 * agent/message.c — conversation messages.
 */

#include <stdlib.h>
#include <string.h>

#include "agent/message.h"

#define LIST_MIN_CAP 4

Message* message_new(MessageRole role) {
    Message* m = calloc(1, sizeof(Message));
    if (m == NULL) {
        return NULL;
    }
    m->role = role;
    return m;
}

/* Release the Message's owned internals WITHOUT freeing m itself
 * (list elements live inside the list's array). */
static void message_deinit(Message* m) {
    free(m->content);
    free(m->reasoning);
    free(m->tool_call_id);
    tool_call_list_free(&m->tool_calls);
}

void message_free(Message* m) {
    if (m == NULL) {
        return;
    }
    message_deinit(m);
    free(m); /* m was allocated by message_new() */
}

int message_set_content(Message* m, const char* text) {
    if (m == NULL || text == NULL) {
        return AGENT_ERR_OOM;
    }
    char* copy = strdup(text);
    if (copy == NULL) {
        return AGENT_ERR_OOM;
    }
    free(m->content);
    m->content = copy;
    return AGENT_OK;
}

void tool_call_list_free(ToolCallList* list) {
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < list->len; i++) {
        ToolCall* tc = &list->items[i];
        free(tc->id);
        free(tc->name);
        free(tc->arguments);
        free(tc->result);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

int tool_call_list_append(ToolCallList* list, ToolCall* tc) {
    if (list == NULL || tc == NULL) {
        return AGENT_ERR_OOM;
    }
    if (list->len == list->cap) {
        size_t new_cap = list->cap == 0 ? LIST_MIN_CAP : list->cap * 2;
        if (new_cap > SIZE_MAX / sizeof(ToolCall)) {
            return AGENT_ERR_OOM;
        }
        ToolCall* items = realloc(list->items, new_cap * sizeof(ToolCall));
        if (items == NULL) {
            return AGENT_ERR_OOM;
        }
        list->items = items;
        list->cap = new_cap;
    }
    list->items[list->len++] = *tc;
    /* take ownership of the shell; the list now owns the internals.
     * tc must have been allocated with malloc(). On failure above the
     * caller keeps ownership. */
    free(tc);
    return AGENT_OK;
}

int message_list_append_copy(MessageList* list, const Message* m) {
    if (list == NULL || m == NULL) {
        return AGENT_ERR_OOM;
    }
    /* copy first: realloc may move the array and invalidate m when the
     * caller passes an element of this very list */
    Message copy = *m;
    if (list->len == list->cap) {
        size_t new_cap = list->cap == 0 ? LIST_MIN_CAP : list->cap * 2;
        if (new_cap > SIZE_MAX / sizeof(Message)) {
            return AGENT_ERR_OOM;
        }
        Message* items = realloc(list->items, new_cap * sizeof(Message));
        if (items == NULL) {
            return AGENT_ERR_OOM;
        }
        list->items = items;
        list->cap = new_cap;
    }
    list->items[list->len++] = copy;
    return AGENT_OK;
}

int message_list_append(MessageList* list, Message* m) {
    int err = message_list_append_copy(list, m);
    if (err != AGENT_OK) {
        return err;
    }
    /* take ownership of the shell (same contract as
     * tool_call_list_append): m must have been allocated by
     * message_new(); the list now owns the internals. */
    free(m);
    return AGENT_OK;
}

void message_list_free(MessageList* list) {
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < list->len; i++) {
        message_deinit(&list->items[i]); /* elements are NOT free()'d */
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

Message* message_list_last(const MessageList* list) {
    if (list == NULL || list->len == 0) {
        return NULL;
    }
    return &list->items[list->len - 1];
}

int message_list_insert(MessageList* list, size_t index, Message* m) {
    if (list == NULL || m == NULL) {
        return AGENT_ERR_OOM;
    }
    if (index > list->len) {
        index = list->len;
    }
    Message copy = *m; /* copy before realloc (see append) */
    if (list->len == list->cap) {
        size_t new_cap = list->cap == 0 ? LIST_MIN_CAP : list->cap * 2;
        if (new_cap > SIZE_MAX / sizeof(Message)) {
            return AGENT_ERR_OOM;
        }
        Message* items = realloc(list->items, new_cap * sizeof(Message));
        if (items == NULL) {
            return AGENT_ERR_OOM;
        }
        list->items = items;
        list->cap = new_cap;
    }
    size_t tail = list->len - index;
    if (tail > 0) {
        memmove(&list->items[index + 1], &list->items[index], tail * sizeof(Message));
    }
    list->items[index] = copy;
    list->len++;
    free(m); /* same take-ownership contract as append */
    return AGENT_OK;
}

size_t message_list_tool_safe_prefix_count(const MessageList* list, size_t start, size_t count) {
    if (list == NULL || start >= list->len || count == 0) {
        return 0;
    }
    if (count > list->len - start) {
        count = list->len - start;
    }

    size_t end = start + count;
    if (end >= list->len || list->items[end].role != MSG_TOOL) {
        return count;
    }

    /* A strict tool protocol treats an assistant tool_calls message and all
     * immediately following tool responses as one atomic history unit. If
     * the requested boundary lands inside those responses, retain the whole
     * unit rather than leaving an orphan MSG_TOOL at the start of the tail. */
    while (end > start && list->items[end].role == MSG_TOOL) {
        end--;
    }
    if (list->items[end].role == MSG_ASSISTANT && list->items[end].tool_calls.len > 0) {
        return end - start;
    }
    return count; /* malformed pre-existing history; do not discard extra data */
}

void message_list_remove_range(MessageList* list, size_t start, size_t count) {
    if (list == NULL || start >= list->len || count == 0) {
        return;
    }
    if (count > list->len - start) {
        count = list->len - start;
    }
    for (size_t i = 0; i < count; i++) {
        message_deinit(&list->items[start + i]);
    }
    size_t tail = list->len - (start + count);
    if (tail > 0) {
        memmove(&list->items[start], &list->items[start + count], tail * sizeof(Message));
    }
    list->len -= count;
}

int message_list_move_range(MessageList* dest, MessageList* src, size_t start, size_t count) {
    if (dest == NULL || src == NULL || start >= src->len || count == 0) {
        return AGENT_OK;
    }
    if (dest == src) {
        return AGENT_ERR_TOOL; /* in-place move would dangle on realloc */
    }
    if (count > src->len - start) {
        count = src->len - start;
    }
    for (size_t i = 0; i < count; i++) {
        Message* m = &src->items[start + i];
        if (message_list_append_copy(dest, m) != AGENT_OK) {
            return AGENT_ERR_OOM; /* caller keeps src intact on failure */
        }
        memset(m, 0, sizeof(*m)); /* ownership transferred */
    }
    return AGENT_OK;
}

const char* message_role_name(MessageRole role) {
    switch (role) {
    case MSG_SYSTEM:
        return "system";
    case MSG_USER:
        return "user";
    case MSG_ASSISTANT:
        return "assistant";
    case MSG_TOOL:
        return "tool";
    }
    return "unknown";
}
