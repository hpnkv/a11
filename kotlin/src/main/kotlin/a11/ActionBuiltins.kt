package a11

/**
 * The actions every A11 peer answers, whatever it was built to do.
 *
 * The Kotlin half of `cpp/a11/actions/builtins.h`, and the reason the IDE plugin
 * no longer announces anything: a gateway asks what this side serves, and this
 * is what answers.
 *
 * These are not registrations. A peer that cannot be asked has to be told, and
 * telling is what four hand-copied handshakes were for -- so discovery cannot be
 * something an application remembers to install. It has to hold for a registry
 * nobody configured and for one an application has called `unregister` all over,
 * which rules out being an entry in the map. What holds instead is this table,
 * consulted by [ActionRegistry] on a miss.
 *
 * The handlers reach their registry through the action they were given rather
 * than capturing one, which is what keeps this an object and not a cycle.
 */

/** Lists the actions a peer serves, with their schemas. */
const val LIST_ACTIONS_NAME = "__list_actions__"

/** Returns one action's schema, or NotFound. */
const val GET_SCHEMA_NAME = "__get_schema__"

/** Echoes a value, so a caller can tell A11 from anything holding a port. */
const val PING_NAME = "__ping"

/** A schema and handler pair the registry resolves without holding. */
class BuiltinAction(val schema: ActionSchema, val handler: ActionHandler)

/** Whether a name is one of A11's own rather than an application's. */
fun isReservedActionName(name: String): Boolean =
    name.length > 4 && name.startsWith("__")

private fun port(
    name: String,
    type: String,
    description: String,
    required: Boolean,
    unary: Boolean,
): ActionPortSchema = ActionPortSchema(
    name = name,
    type = type,
    description = description,
    required = required,
    unary = unary,
)

private fun textChunk(text: String, mimetype: String): Chunk =
    Chunk(data = utf8Encode(text), metadata = ChunkMetadata(mimetype = mimetype))

/**
 * The text on a unary input, or "" where it said nothing.
 *
 * Reads bytes rather than deserialising: the two things a builtin accepts are a
 * JSON request document and an action name, which are text either way.
 */
private suspend fun readUnaryText(action: Action, portName: String): String {
    val node = action.getInput(portName)
    if (node !is Ok) return ""
    val chunk = node.value.nextChunk()
    if (chunk !is Ok) return ""
    val value = chunk.value ?: return ""
    if (value.isNull || value.isEmpty) return ""
    val text = utf8Decode(value.data)
    return if (text is Ok) text.value else ""
}

private suspend fun writeUnary(
    action: Action,
    portName: String,
    text: String,
    mimetype: String,
): Status {
    val node = action.getOutput(portName)
    if (node !is Ok) return node as Status
    val written = node.value.finalize(textChunk(text, mimetype))
    return if (written.isOk) Status.ok() else written
}

/** Which schemas a request asked for, and how much of each. */
private class SchemaQuery(
    val names: List<String> = emptyList(),
    val exact: List<String> = emptyList(),
    val ports: PortView = PortView.CALLABLE,
    val includeReserved: Boolean = false,
    val runnableOnly: Boolean = false,
)

private fun parseQuery(encoded: String): SchemaQuery {
    val trimmed = encoded.trim()
    // No request is the default request: asking a peer what it serves, with
    // nothing further to say, is the common case and must not need a document.
    if (trimmed.isEmpty() || trimmed == "null") return SchemaQuery()
    val parsed = A11Json.parse(trimmed)
    if (parsed !is Ok) return SchemaQuery()
    val value = parsed.value
    if (value is List<*>) {
        // A bare array is read as patterns, because that is what a caller who
        // wrote one meant.
        return SchemaQuery(names = value.filterIsInstance<String>())
    }
    val asked = value as? Map<*, *> ?: return SchemaQuery()
    return SchemaQuery(
        names = (asked["names"] as? List<*>)?.filterIsInstance<String>() ?: emptyList(),
        exact = (asked["exact"] as? List<*>)?.filterIsInstance<String>() ?: emptyList(),
        ports = if (asked["ports"] == "all") PortView.ALL else PortView.CALLABLE,
        includeReserved = asked["include_reserved"] == true,
        runnableOnly = asked["runnable_only"] == true,
    )
}

private fun accepts(query: SchemaQuery, name: String): Boolean {
    val named = query.exact.contains(name)
    if (!query.includeReserved && isReservedActionName(name) && !named) return false
    if (query.names.isEmpty() && query.exact.isEmpty()) return true
    if (named) return true
    for (pattern in query.names) {
        try {
            // Full match, the same rule `x-a11-allowed-llm-actions` uses, so a
            // pattern means one thing across A11 rather than two.
            if (Regex("^(?:$pattern)$").matches(name)) return true
        } catch (_: Exception) {
            // A pattern that will not compile matches nothing.
        }
    }
    return false
}

private suspend fun runListActions(action: Action): Status {
    val registry = action.getRegistry() as? ActionRegistry
        ?: return failedPrecondition(
            "This action was not dispatched through a registry, so there is nothing to describe.",
        )
    val query = parseQuery(readUnaryText(action, "request"))
    val entries = mutableListOf<Map<String, Any?>>()
    for (name in registry.listRegisteredActions().sorted()) {
        if (!accepts(query, name)) continue
        val schema = registry.getSchema(name)
        if (schema !is Ok) continue
        val runnable = registry.getHandler(name) is Ok
        if (query.runnableOnly && !runnable) continue
        entries.add(schemaToJson(schema.value, runnable, query.ports))
    }
    val encoded = A11Json.encodeToString(schemaDocument(entries))
    if (encoded !is Ok) return encoded as Status
    return writeUnary(action, "actions", encoded.value, JSON_MIMETYPE)
}

private suspend fun runGetSchema(action: Action): Status {
    val registry = action.getRegistry() as? ActionRegistry
        ?: return failedPrecondition(
            "This action was not dispatched through a registry, so there is nothing to describe.",
        )
    val name = readUnaryText(action, "action")
    if (name.isEmpty()) {
        return invalidArgument(
            "__get_schema__ needs the name of an action on its 'action' input.",
        )
    }
    // NotFound rather than an empty document, and distinct from the
    // InvalidArgument an unnameable id gets.
    val schema = registry.getSchema(name)
    if (schema !is Ok) return schema as Status
    val runnable = registry.getHandler(name) is Ok
    val encoded = A11Json.encodeToString(
        schemaDocument(listOf(schemaToJson(schema.value, runnable, PortView.ALL))),
    )
    if (encoded !is Ok) return encoded as Status
    return writeUnary(action, "schema", encoded.value, JSON_MIMETYPE)
}

private suspend fun runPing(action: Action): Status =
    writeUnary(action, "output", readUnaryText(action, "input"), TEXT_MIMETYPE)

private val builtinTable: Map<String, BuiltinAction> by lazy {
    linkedMapOf(
        LIST_ACTIONS_NAME to BuiltinAction(
            ActionSchema(
                name = LIST_ACTIONS_NAME,
                description =
                    "List the actions this peer serves, with their schemas, as one" +
                        " a11.actions/v1 document. Takes an optional request object on" +
                        " 'request': 'names' (full-match patterns), 'exact' (names)," +
                        " 'ports' (\"callable\" or \"all\"), 'include_reserved', and" +
                        " 'runnable_only'.",
                inputs = linkedMapOf(
                    "request" to port(
                        "request", JSON_MIMETYPE,
                        "Which actions to describe. Absent means all of them.",
                        false, true,
                    ),
                ),
                outputs = linkedMapOf(
                    "actions" to port(
                        "actions", JSON_MIMETYPE,
                        "The a11.actions/v1 document, whole.", true, true,
                    ),
                ),
            ),
            ::runListActions,
        ),
        GET_SCHEMA_NAME to BuiltinAction(
            ActionSchema(
                name = GET_SCHEMA_NAME,
                description =
                    "Describe one action this peer serves, as an a11.actions/v1" +
                        " document. Fails NOT_FOUND when the name is not registered here.",
                inputs = linkedMapOf(
                    "action" to port(
                        "action", TEXT_MIMETYPE,
                        "Name of the action to describe.", true, true,
                    ),
                ),
                outputs = linkedMapOf(
                    "schema" to port(
                        "schema", JSON_MIMETYPE,
                        "The a11.actions/v1 document for that one action.", true, true,
                    ),
                ),
            ),
            ::runGetSchema,
        ),
        // Wording and shape kept exactly: four languages' clients probe with
        // this, and a probe that started describing itself differently would be
        // the first thing anybody diffing two peers noticed.
        PING_NAME to BuiltinAction(
            ActionSchema(
                name = PING_NAME,
                description =
                    "Ping the server to check if it is alive. Requires a single value" +
                        " on the port `input`, which it returns as a single value on the" +
                        " port `output`.",
                inputs = linkedMapOf(
                    "input" to port("input", TEXT_MIMETYPE, "Ping input value", false, false),
                ),
                outputs = linkedMapOf(
                    "output" to port("output", TEXT_MIMETYPE, "Pong response value", false, false),
                ),
            ),
            ::runPing,
        ),
    )
}

/** Whether [name] is a builtin every registry answers for. */
fun isBuiltinAction(name: String): Boolean = builtinTable.containsKey(name)

/** The builtin named [name], or null. */
fun getBuiltinAction(name: String): BuiltinAction? = builtinTable[name]

/** Every builtin's name, sorted. */
fun builtinActionNames(): List<String> = builtinTable.keys.sorted()
