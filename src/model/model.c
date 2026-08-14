/*
 * model/model.c — model helpers.
 */

#include "model/model.h"

const char* model_event_name(ModelEventType type) {
    switch (type) {
    case MODEL_EVENT_TEXT_DELTA:
        return "text_delta";
    case MODEL_EVENT_REASONING_DELTA:
        return "reasoning_delta";
    case MODEL_EVENT_TOOL_CALL_START:
        return "tool_call_start";
    case MODEL_EVENT_TOOL_CALL_DELTA:
        return "tool_call_delta";
    case MODEL_EVENT_TOOL_CALL_END:
        return "tool_call_end";
    case MODEL_EVENT_USAGE:
        return "usage";
    case MODEL_EVENT_DONE:
        return "done";
    case MODEL_EVENT_ERROR:
        return "error";
    }
    return "unknown";
}
