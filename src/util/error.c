/*
 * util/error.c — error code names.
 */

#include "util/error.h"

const char* error_name(AgentError err) {
    switch (err) {
    case AGENT_OK:
        return "ok";
    case AGENT_ERR_OOM:
        return "out of memory";
    case AGENT_ERR_IO:
        return "io error";
    case AGENT_ERR_JSON:
        return "json error";
    case AGENT_ERR_HTTP:
        return "http error";
    case AGENT_ERR_MODEL:
        return "model error";
    case AGENT_ERR_TOOL:
        return "tool error";
    case AGENT_ERR_PROCESS:
        return "process error";
    case AGENT_ERR_CANCELLED:
        return "cancelled";
    case AGENT_ERR_AUTH:
        return "authentication error";
    }
    return "unknown error";
}
