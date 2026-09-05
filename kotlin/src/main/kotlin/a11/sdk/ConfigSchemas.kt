/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
