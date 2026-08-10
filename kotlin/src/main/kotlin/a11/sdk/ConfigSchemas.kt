package a11.sdk

/**
 * Default model ids and config-port key names for the server backends, mirroring
 * `js/src/sdk/config_schemas.ts`. The `config` port carries a JSON object of
 * provider-specific options; a Kotlin caller builds it as a plain map.
 */
const val GEMINI_DEFAULT_MODEL = "gemini-3.5-flash"
const val OLLAMA_DEFAULT_MODEL = "llama3.2"
const val CLAUDE_DEFAULT_MODEL = "claude-sonnet-4-6"

/** Convenience builder for the Claude `messages.create` config object. */
fun claudeConfig(
    maxTokens: Int = 10240,
    thinking: Boolean = false,
    thinkingSummaries: Boolean = false,
    effort: String? = null,
    webSearch: Boolean = false,
): Map<String, Any?> = buildMap {
    put("max_tokens", maxTokens)
    put("thinking", thinking)
    put("thinking_summaries", thinkingSummaries)
    if (effort != null) put("effort", effort)
    put("web_search", webSearch)
}
