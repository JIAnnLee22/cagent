/*
 * agent/message.h — conversation messages (DESIGN.md §3.4).
 *
 * Ownership:
 *   - All char* fields are OWNED by the Message/ToolCall; freed by
 *     message_free()/tool_call_list_free().
 *   - message_new() allocates a standalone Message; message_free() frees
 *     its internals AND the Message itself.
 *   - message_list_append() takes ownership of the Message and copies it
 *     INTO the list array: the list owns a shallow copy, so the caller
 *     must NOT message_free() the source afterwards (it would double
 *     free). List elements are released by message_list_free() only.
 *   - tool_call_list_append() takes ownership of the ToolCall.
 *   - message_list_last() returns a borrowed pointer.
 */

#ifndef CAGENT_AGENT_MESSAGE_H
#define CAGENT_AGENT_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "model/model.h" /* Usage */
#include "util/error.h"

typedef enum { MSG_SYSTEM, MSG_USER, MSG_ASSISTANT, MSG_TOOL } MessageRole;

typedef struct {
    char* id;        /* owned; tool_call_id from the model */
    char* name;      /* owned; tool name */
    char* arguments; /* owned; raw JSON arguments string */
    char* result;    /* owned; filled by the agent after execution */
    bool is_error;   /* tool execution failed */
} ToolCall;

typedef struct {
    ToolCall* items; /* owned */
    size_t len;
    size_t cap;
} ToolCallList;

typedef struct {
    MessageRole role;
    char* content;           /* owned; NULL allowed (tool-call assistant msg) */
    char* reasoning;         /* owned; assistant only; may be NULL */
    char* tool_call_id;      /* owned; MSG_TOOL only */
    ToolCallList tool_calls; /* owned; MSG_ASSISTANT only */
    Usage usage;             /* MSG_ASSISTANT only; zeroed when absent */
    bool is_error;           /* MSG_TOOL only */
} Message;

typedef struct MessageList {
    Message* items; /* owned */
    size_t len;
    size_t cap;
} MessageList;

Message* message_new(MessageRole role);
void message_free(Message* m);

/* Copy text into m->content (frees a previous value). */
int message_set_content(Message* m, const char* text);

void tool_call_list_free(ToolCallList* list);
/* Takes ownership of tc (including on failure). */
int tool_call_list_append(ToolCallList* list, ToolCall* tc);

/* Shallow-copy the Message into the list WITHOUT taking ownership; the
 * caller keeps the source (used by move_range, where the source is a
 * list element). */
int message_list_append_copy(MessageList* list, const Message* m);

int message_list_append(MessageList* list, Message* m); /* takes ownership */
void message_list_free(MessageList* list);
Message* message_list_last(const MessageList* list); /* borrowed; may be NULL */

/* Insert m (ownership taken) at index; elements shift right. */
int message_list_insert(MessageList* list, size_t index, Message* m);

/* Reduce a prefix removal count when its boundary would split an assistant
 * tool_calls message from the contiguous MSG_TOOL responses that follow it.
 * The return value is <= count. */
size_t message_list_tool_safe_prefix_count(const MessageList* list, size_t start, size_t count);

/* Deinit and drop items[start..start+count); the tail is compacted. */
void message_list_remove_range(MessageList* list, size_t start, size_t count);

/* Move items[start..start+count) from src into dest. The moved slots in
 * src are zeroed (ownership transferred; src stays usable for free). */
int message_list_move_range(MessageList* dest, MessageList* src, size_t start, size_t count);

/* Static strings, never freed. */
const char* message_role_name(MessageRole role);

#endif /* CAGENT_AGENT_MESSAGE_H */
