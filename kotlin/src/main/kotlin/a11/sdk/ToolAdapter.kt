package a11.sdk

import a11.ActionPortSchema
import a11.ActionSchema
import a11.Ok
import a11.StatusOr
import a11.WHOLE_JSON_OUTPUT

/**
 * Translate an [ActionSchema] into the JSON-Schema tool contract an LLM
 * consumes, ported from `js/src/sdk/tool_adapter.ts`. Non-unary ports become
 * arrays; autofilled inputs are hidden from the model.
 *
 * A port's value shape comes from its own [ActionPortSchema.jsonSchema] when the
 * schema declares one (the analog of the Python port's `typeinfo`), and is
 * otherwise derived from the port's MIME type.
 */
class ToolAdapter(private val schema: ActionSchema) {

    /** JSON Schema for the tool's callable inputs (autofilled inputs excluded). */
    fun getInputSchema(): StatusOr<Map<String, Any?>> {
        val properties = LinkedHashMap<String, Any?>()
        val required = ArrayList<String>()
        for (port in schema.inputs.values) {
            if (port.autofills.isNotEmpty()) continue
            if (port.required) required.add(port.name)
            properties[port.name] = wrapCollection(nodeSchema(port), port, port.required)
        }
        return Ok(organiseAndDeduplicateJsonschema(linkedMapOf(
            "type" to "object", "properties" to properties, "required" to required,
        )))
    }

    /** JSON Schema for the tool's result, honoring `output_to_json_field`. */
    fun getOutputSchema(): StatusOr<Map<String, Any?>> {
        val properties = LinkedHashMap<String, Any?>()
        val required = ArrayList<String>()
        for (port in schema.outputs.values) {
            if (port.required) required.add(port.name)
            properties[port.name] = wrapCollection(nodeSchema(port), port, true)
        }
        val substitutions = schema.outputToJsonField
        val schemaOut: Map<String, Any?> = when {
            substitutions.isEmpty() -> linkedMapOf("type" to "object", "properties" to properties, "required" to required)
            substitutions.size == 1 && substitutions.values.first() == WHOLE_JSON_OUTPUT ->
                @Suppress("UNCHECKED_CAST")
                (properties[substitutions.keys.first()] as Map<String, Any?>)
            else -> {
                for ((name, sub) in substitutions) if (properties.containsKey(name)) {
                    properties[sub] = properties.remove(name)
                }
                linkedMapOf("type" to "object", "properties" to properties, "required" to required)
            }
        }
        return Ok(organiseAndDeduplicateJsonschema(schemaOut))
    }

    /** The port's declared JSON Schema, or one derived from its MIME type. */
    private fun nodeSchema(port: ActionPortSchema): Map<String, Any?> =
        port.jsonSchema ?: mimeToJsonSchema(port.type)

    private fun wrapCollection(nodeSchema: Map<String, Any?>, port: ActionPortSchema, minOne: Boolean): Map<String, Any?> {
        if (port.unary) return nodeSchema
        val wrapped = linkedMapOf<String, Any?>("type" to "array", "items" to nodeSchema)
        if (minOne) wrapped["minItems"] = 1
        return wrapped
    }
}
