/*
 * util/error.h — unified error codes.
 *
 * Design rule (DESIGN.md §3.9): low-level code must never call exit(1).
 * Errors bubble up as AgentError; the top layer decides retry / report /
 * cancel tool / cancel agent / exit program.
 *
 * Ownership: error_name() returns a static string, borrowed forever.
 */

#ifndef CAGENT_UTIL_ERROR_H
#define CAGENT_UTIL_ERROR_H

typedef enum {
    AGENT_OK = 0,
    AGENT_ERR_OOM,      /* allocation failure */
    AGENT_ERR_IO,       /* file / pipe / socket error */
    AGENT_ERR_JSON,     /* parse or serialize failure */
    AGENT_ERR_HTTP,     /* transport level failure */
    AGENT_ERR_MODEL,    /* model/provider protocol failure */
    AGENT_ERR_TOOL,     /* tool execution failure */
    AGENT_ERR_PROCESS,  /* process spawn / wait failure */
    AGENT_ERR_CANCELLED, /* operation was cancelled */
    AGENT_ERR_AUTH      /* authentication or credential failure */
} AgentError;

/* Human-readable name for logging/tests. Static storage, never freed. */
const char* error_name(AgentError err);

#endif /* CAGENT_UTIL_ERROR_H */
