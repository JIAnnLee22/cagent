/*
 * model/responses.h — OpenAI Responses API provider.
 *
 * The provider maps the Responses streaming event protocol to the common
 * ModelEvent interface. It intentionally supports text output and function
 * calls only; the Agent core remains provider-agnostic.
 */

#ifndef CAGENT_MODEL_RESPONSES_H
#define CAGENT_MODEL_RESPONSES_H

#include <stdint.h>

#include "model/model.h"
#include "util/string.h"

/* Create a Model bound to this provider's /responses endpoint. */
Model* responses_model_new(Provider* provider, const char* name, int64_t context_window,
                           int64_t max_output);

/* Pure function: serialize a streaming Responses API request body. */
int responses_build_request_body(ModelRequest* request, String* out);

#endif /* CAGENT_MODEL_RESPONSES_H */
