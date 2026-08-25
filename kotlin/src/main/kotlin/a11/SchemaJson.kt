package a11

/**
 * An [ActionSchema] in JSON, which is how one travels.
 *
 * The Kotlin half of `a11.actions/v1`. The format's shape lives in
 * `cpp/a11/actions/describe.h`; this reads and writes the same thing, and
 * `testdata/actions/schema_document.json` pins the two against each other so
 * agreement is checked rather than assumed. That fixture exists because the
 * thing it replaces -- one handshake schema hand-copied into four languages --
 * is exactly what prose agreement produces.
 *
 * A document is `{format, actions}`; each entry is one schema plus `runnable`,
 * which says whether the answering side holds a handler and so is the
 * registry's annotation on a schema rather than part of one.
 *
 * `required` and `unary` are written only when true, and a port's `json_schema`
 * only when it says more than `{"type": "object"}` -- which is what an adapter
 * shows a model for a port carrying no schema at all. Every reader fills in
 * [ActionPortSchema]'s own defaults, so what is absent and what is spelled out
 * mean the same thing. Those defaults are this document's and not A11's
 * everywhere: `flow.catalogue/v1` defaults `unary` to true and omits it when
 * true, the opposite convention for a different format.
 */

/** The `format` field every schema document carries. */
const val SCHEMA_DOCUMENT_FORMAT = "a11.actions/v1"

/** Which ports a written schema includes. */
enum class PortView { CALLABLE, ALL }

/**
 * The JSON Schema that says nothing, which is not worth writing down.
 *
 * A port with no schema is shown to a model as `{"type": "object"}` anyway, so a
 * document spelling that out states exactly what leaving it out does.
 */
private fun saysNothing(schema: Map<String, Any?>): Boolean =
    schema.size == 1 && schema["type"] == "object"

private fun portToJson(port: ActionPortSchema, autofilled: Boolean): Map<String, Any?> {
    val entry = LinkedHashMap<String, Any?>()
    entry["name"] = port.name
    entry["type"] = port.type
    if (port.description.isNotEmpty()) entry["description"] = port.description
    if (port.required) entry["required"] = true
    if (port.unary) entry["unary"] = true
    if (autofilled) entry["autofilled"] = true
    port.jsonSchema?.let { if (!saysNothing(it)) entry["json_schema"] = it }
    return entry
}

@Suppress("UNCHECKED_CAST")
private fun portFromJson(entry: Map<*, *>): ActionPortSchema? {
    val name = entry["name"] as? String ?: return null
    if (name.isEmpty()) return null
    return ActionPortSchema(
        name = name,
        type = entry["type"] as? String ?: "application/json",
        description = entry["description"] as? String ?: "",
        required = entry["required"] == true,
        // False when absent, which is what the writer omits the field for and
        // what ActionPortSchema itself defaults to.
        unary = entry["unary"] == true,
        jsonSchema = entry["json_schema"] as? Map<String, Any?>,
    )
}

/** One schema as an `actions` entry. */
fun schemaToJson(
    schema: ActionSchema,
    runnable: Boolean = true,
    ports: PortView = PortView.CALLABLE,
): Map<String, Any?> {
    val entry = LinkedHashMap<String, Any?>()
    entry["name"] = schema.name
    if (schema.description.isNotEmpty()) entry["description"] = schema.description
    entry["runnable"] = runnable

    // Sorted, because declaration order is not preserved and a document that
    // reshuffles itself between calls is one nobody can diff.
    val inputs = schema.inputs.values.sortedBy { it.name }.mapNotNull { port ->
        val autofilled = port.autofills.isNotEmpty()
        // A caller cannot write an autofilled input: the runtime requires it
        // empty before applying the receiver's default.
        if (autofilled && ports == PortView.CALLABLE) null
        else portToJson(port, autofilled)
    }
    val outputs = schema.outputs.values.sortedBy { it.name }.map { portToJson(it, false) }
    if (inputs.isNotEmpty()) entry["inputs"] = inputs
    if (outputs.isNotEmpty()) entry["outputs"] = outputs

    val headers = schema.headers.values.sortedBy { it.name }.map { header ->
        val written = LinkedHashMap<String, Any?>()
        written["name"] = header.name
        if (header.description.isNotEmpty()) written["description"] = header.description
        if (header.defaultValue != null) {
            written["has_default"] = true
            // The value itself only where it is text: a header default can be
            // arbitrary bytes, and base64 in a document meant to be read by a
            // person is worse than saying only that a default exists.
            val text = utf8Decode(header.defaultValue!!)
            if (text is Ok) written["default"] = text.value
        }
        written
    }
    if (headers.isNotEmpty()) entry["headers"] = headers

    if (schema.outputToJsonField.isNotEmpty()) {
        entry["output_to_json_field"] = schema.outputToJsonField.toSortedMap()
    }
    return entry
}

/**
 * The schema an entry was written from.
 *
 * What cannot survive the trip comes back empty: an input's autofills are
 * receiver-owned defaults that deliberately never travel. A `user_facing` flag
 * from an older client is read and dropped -- narration travels on the reserved
 * log port, which no schema declares.
 */
fun schemaFromJson(entry: Map<*, *>): StatusOr<ActionSchema> {
    val name = entry["name"] as? String
    if (name.isNullOrEmpty()) {
        return invalidArgument("An action schema entry must have a name.")
    }
    val inputs = LinkedHashMap<String, ActionPortSchema>()
    val outputs = LinkedHashMap<String, ActionPortSchema>()
    for ((key, into) in listOf("inputs" to inputs, "outputs" to outputs)) {
        val written = entry[key] as? List<*> ?: continue
        for (one in written) {
            val port = (one as? Map<*, *>)?.let { portFromJson(it) } ?: continue
            into[port.name] = port
        }
    }
    val headers = LinkedHashMap<String, ActionHeaderSchema>()
    for (one in entry["headers"] as? List<*> ?: emptyList<Any?>()) {
        val written = one as? Map<*, *> ?: continue
        val headerName = written["name"] as? String ?: continue
        if (headerName.isEmpty()) continue
        headers[headerName] = ActionHeaderSchema(
            name = headerName,
            description = written["description"] as? String ?: "",
            defaultValue = (written["default"] as? String)?.let { utf8Encode(it) },
        )
    }
    val mapping = LinkedHashMap<String, String>()
    for ((output, field) in entry["output_to_json_field"] as? Map<*, *> ?: emptyMap<Any?, Any?>()) {
        val port = output as? String ?: continue
        val target = field as? String ?: continue
        // Only a mapping onto a port that came with it: an output named here
        // and absent above would fail validation about the wrong thing.
        if (!outputs.containsKey(port)) continue
        mapping[port] = target
    }
    val schema = ActionSchema(
        name = name,
        description = entry["description"] as? String ?: "",
        inputs = inputs,
        outputs = outputs,
        headers = headers,
        outputToJsonField = mapping,
    )
    schema.validate().let { if (!it.isOk) return it }
    return Ok(schema)
}

/**
 * The entries of a document, accepting a whole document or just its array.
 *
 * A caller handed one or the other should not have to care which.
 */
fun schemasInDocument(document: Any?): StatusOr<List<Map<*, *>>> {
    if (document is List<*>) {
        return Ok(document.filterIsInstance<Map<*, *>>())
    }
    val asMap = document as? Map<*, *>
        ?: return invalidArgument("An action schema document must be an object or an array.")
    val actions = asMap["actions"] as? List<*>
        ?: return invalidArgument("An action schema document is missing its 'actions' array.")
    return Ok(actions.filterIsInstance<Map<*, *>>())
}

/** A whole document from entries. */
fun schemaDocument(actions: List<Map<String, Any?>>): Map<String, Any?> =
    linkedMapOf("format" to SCHEMA_DOCUMENT_FORMAT, "actions" to actions)
