package a11.sdk

import a11.ActionHeaderSchema
import a11.ActionPortSchema
import a11.ActionSchema

/**
 * The provider-agnostic `interact_with_llm` action contract, ported from
 * `js/src/sdk/interact_with_llm.ts`. Only the schema is provided here: the
 * concrete provider handlers (Claude/Gemini/Ollama) live in the Python backend
 * that a Kotlin client calls over a WebSocket. A Kotlin caller constructs an
 * [a11.Action] from this schema, sets the `x-a11-llm-*` headers, writes the
 * `interactions`/`tools`/`config` inputs, and reads the streamed outputs.
 */
val INTERACT_WITH_LLM_SCHEMA: ActionSchema = ActionSchema(
    name = "interact_with_llm",
    description = "Route an LLM interaction to a concrete backend chosen by the ${LlmHeaders.PROVIDER.header} header.",
    inputs = linkedMapOf(
        "interactions" to ActionPortSchema("interactions", "application/json", required = true),
        "tools" to ActionPortSchema("tools", "application/json", required = false),
        "config" to ActionPortSchema("config", "application/json", unary = true, required = false),
    ),
    outputs = linkedMapOf(
        "event_stream" to ActionPortSchema("event_stream", "application/json", required = false),
        "thoughts" to ActionPortSchema("thoughts", "text/plain", required = false),
        "text_output" to ActionPortSchema("text_output", "text/plain", required = false),
        "new_interactions" to ActionPortSchema("new_interactions", "application/json", required = true),
    ),
    headers = linkedMapOf(
        LlmHeaders.API_KEY.header to ActionHeaderSchema(LlmHeaders.API_KEY.header, "The backend API key."),
        LlmHeaders.PROVIDER.header to ActionHeaderSchema(LlmHeaders.PROVIDER.header, "Which backend to route to."),
        LlmHeaders.MODEL.header to ActionHeaderSchema(LlmHeaders.MODEL.header, "The downstream model."),
        LlmHeaders.BASE_URL.header to ActionHeaderSchema(LlmHeaders.BASE_URL.header, "The downstream base URL, where applicable."),
        LlmHeaders.ALLOWED_LLM_ACTIONS.header to ActionHeaderSchema(LlmHeaders.ALLOWED_LLM_ACTIONS.header, "Allowed tool action name patterns, comma-separated."),
    ),
)
