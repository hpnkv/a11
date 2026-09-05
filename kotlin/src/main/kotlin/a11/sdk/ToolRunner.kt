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

import a11.ActionRegistryLike
import a11.Ok
import a11.StatusCode
import a11.StatusOr
import a11.orElse

/** One tool definition surfaced to a model: `{name, description, input_schema}`. */
typealias ToolDefinition = LinkedHashMap<String, Any?>

/**
 * Build model tool definitions for the allow-listed registry actions, ported
 * from `js/src/sdk/tool_runner.ts` (`getToolDefinitions`). A missing action is
 * skipped; any other lookup failure is propagated.
 *
 * The reverse direction — executing the tools a model asked for — is handled by
 * the [a11.Session] dispatch path in this Kotlin client: an incoming tool
 * `ActionMessage` is resolved against the client registry and run locally, and
 * its outputs are teed back to the caller. That is how the plugin exposes IDE
 * tools to a backend-hosted `interact_with_llm`.
 */
fun getToolDefinitions(
    registry: ActionRegistryLike?,
    allowedActions: List<String> = emptyList(),
): StatusOr<List<ToolDefinition>> {
    if (registry == null) return Ok(emptyList())
    val definitions = ArrayList<ToolDefinition>()
    for (name in allowedActions) {
        val schema = when (val s = registry.getSchema(name)) {
            is Ok -> s.value
            else -> {
                val status = s as a11.Status
                if (status.code == StatusCode.NOT_FOUND) continue else return status
            }
        }
        val inputSchema = ToolAdapter(schema).getInputSchema().orElse { return it }
        definitions.add(ToolDefinition().apply {
            put("name", schema.name)
            put("description", schema.description)
            put("input_schema", inputSchema)
        })
    }
    return Ok(definitions)
}
