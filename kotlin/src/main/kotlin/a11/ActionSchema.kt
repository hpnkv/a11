package a11

/** MIME type used for structured action dispatch/completion status chunks. */
const val ACTION_STATUS_MIMETYPE = "application/x-a11-status"
/**
 * Chunk metadata attribute marking a status chunk as a closure marker: the
 * producer drained the node and closed its write half with that status. The
 * marker carries no application value and is never stored.
 */
const val CLOSE_STATUS_ATTRIBUTE = "a11-close"
/** Reserved output node carrying the action's eventual completion status. */
const val ACTION_STATUS_OUTPUT = "__status__"
/** Reserved output node acknowledging whether a remote call was dispatched. */
const val ACTION_DISPATCH_STATUS_OUTPUT = "__dispatch_status__"
/**
 * Reserved output node carrying the action's log.
 *
 * Every action has one, declared by nobody: it is not in the schema, so it never
 * appears in an [ActionMessage] or in a tool definition. Written through
 * [Action.log], closed with the action's other outputs. See `ActionLog.kt`.
 */
const val ACTION_LOG_OUTPUT = "__log__"
/** Reserved wire action name used to request remote cancellation. */
const val CANCEL_ACTION_NAME = "__cancel__"
/** Header naming the action id targeted by a cancellation message. */
const val CANCEL_ACTION_HEADER = "__action"
/** Prefix for framework headers normally forwarded to nested actions. */
const val ACTION_HEADER_PREFIX = "x-a11-"
/** Output mapping sentinel meaning the output is the complete JSON value. */
const val WHOLE_JSON_OUTPUT = "$"

/** Contract for one named action input or output node. */
class ActionPortSchema(
    val name: String,
    val type: String,
    val description: String = "",
    val required: Boolean = false,
    val unary: Boolean = false,
    val autofills: List<NodeFragment?> = emptyList(),
    /**
     * Optional JSON Schema for this port's value; the Kotlin stand-in for the
     * Python port schema's `typeinfo`. When set, [a11.sdk.ToolAdapter] describes
     * the port with it instead of deriving a bare shape from the MIME [type],
     * so a model sees the real fields of a JSON request/response object.
     */
    val jsonSchema: Map<String, Any?>? = null,
) {
    fun validate(): Status {
        validateName(name).let { if (!it.isOk) return it }
        if (type.isEmpty()) return invalidArgument("Action port type must not be empty.")
        for (fragment in autofills) fragment?.validate()?.let { if (!it.isOk) return it }
        return Status.ok()
    }
}

/** Describes one binary metadata value accepted by an action call. */
class ActionHeaderSchema(
    val name: String,
    val description: String = "",
    val defaultValue: ByteArray? = null,
) {
    fun validate(): Status = validateName(name)
}

/** Per-instance policy for binding and retaining an action's port nodes. */
data class ActionSettings(
    var bindStreamsOnInputsByDefault: Boolean? = null,
    var bindStreamsOnOutputsByDefault: Boolean? = null,
    var clearInputsAfterRun: Boolean? = null,
    var clearOutputsAfterRun: Boolean? = null,
)

/** Typed port and header contract for an [Action]. */
class ActionSchema(
    val name: String,
    val description: String = "",
    val inputs: LinkedHashMap<String, ActionPortSchema> = LinkedHashMap(),
    val outputs: LinkedHashMap<String, ActionPortSchema> = LinkedHashMap(),
    val headers: LinkedHashMap<String, ActionHeaderSchema> = LinkedHashMap(),
    val outputToJsonField: LinkedHashMap<String, String> = LinkedHashMap(),
) {
    fun validate(): Status {
        validateName(name).let { if (!it.isOk) return it }
        val reserved = setOf(ACTION_STATUS_OUTPUT, ACTION_DISPATCH_STATUS_OUTPUT, ACTION_LOG_OUTPUT)
        for (ports in listOf(inputs, outputs)) {
            for ((key, port) in ports) {
                validateName(key).let { if (!it.isOk) return it }
                port.validate().let { if (!it.isOk) return it }
                if (key != port.name) return invalidArgument("Action port key '$key' does not match port name '${port.name}'.")
                if (key in reserved) return invalidArgument("Action port name '$key' is reserved.")
            }
        }
        for ((key, header) in headers) {
            validateName(key).let { if (!it.isOk) return it }
            header.validate().let { if (!it.isOk) return it }
            if (key != header.name) return invalidArgument("Action header key '$key' does not match header name '${header.name}'.")
        }
        var wholeValues = 0
        for ((output, field) in outputToJsonField) {
            if (!outputs.containsKey(output)) return notFound("Output '$output' is not in the Action schema.")
            if (field == WHOLE_JSON_OUTPUT) wholeValues++ else validateName(field).let { if (!it.isOk) return it }
        }
        if (wholeValues > 1 || (wholeValues == 1 && outputToJsonField.size != 1)) {
            return failedPrecondition("Only one output can map to the complete JSON value.")
        }
        return Status.ok()
    }

    /** Map an output into a JSON field, or `$` as the whole result value. */
    fun mapOutputToJson(outputName: String, fieldName: String = ""): Status {
        validateName(outputName).let { if (!it.isOk) return it }
        if (!outputs.containsKey(outputName)) return notFound("Output '$outputName' is not in the Action schema.")
        val field = fieldName.ifEmpty { outputName }
        if (field != WHOLE_JSON_OUTPUT) validateName(field).let { if (!it.isOk) return it }
        outputToJsonField[outputName] = field
        return Status.ok()
    }

    /** Deep-copy this schema (optionally clearing autofills for trust boundaries). */
    fun copy(clearAutofills: Boolean = false): ActionSchema {
        fun copyPorts(src: Map<String, ActionPortSchema>): LinkedHashMap<String, ActionPortSchema> {
            val out = LinkedHashMap<String, ActionPortSchema>()
            for ((k, p) in src) out[k] = ActionPortSchema(
                p.name, p.type, p.description, p.required, p.unary,
                if (clearAutofills) emptyList() else p.autofills.map { it?.copy() },
                p.jsonSchema,
            )
            return out
        }
        val h = LinkedHashMap<String, ActionHeaderSchema>()
        for ((k, v) in headers) h[k] = ActionHeaderSchema(v.name, v.description, v.defaultValue?.copyOf())
        return ActionSchema(name, description, copyPorts(inputs), copyPorts(outputs), h, LinkedHashMap(outputToJsonField))
    }
}

/**
 * Encode a structured action status as a reserved-MIME chunk.
 *
 * With [closing] set the chunk is a node lifecycle marker instead of a value:
 * it says the producer drained the node and closed its write half with this
 * status. See [CLOSE_STATUS_ATTRIBUTE].
 */
fun statusToChunk(status: Status, closing: Boolean = false): StatusOr<Chunk> {
    val bytes = packStatus(status).orElse { return it }
    val metadata = ChunkMetadata(mimetype = ACTION_STATUS_MIMETYPE)
    if (closing) metadata.attributes[CLOSE_STATUS_ATTRIBUTE] = byteArrayOf('1'.code.toByte())
    return Ok(Chunk(metadata = metadata, data = bytes))
}

/** Whether a chunk carries the reserved action status MIME type. */
fun isStatusChunk(chunk: Chunk): Boolean = chunk.mimetype == ACTION_STATUS_MIMETYPE

/** Whether a chunk is a status chunk reporting that a node's writer closed. */
fun isCloseStatusChunk(chunk: Chunk): Boolean =
    isStatusChunk(chunk) && chunk.metadata?.attributes?.containsKey(CLOSE_STATUS_ATTRIBUTE) == true

/** Decode a status chunk while retaining outer parsing errors separately. */
fun decodeStatusChunk(chunk: Chunk): StatusOr<DecodedStatus> {
    chunk.validate().let { if (!it.isOk) return it }
    if (!isStatusChunk(chunk)) return invalidArgument("Chunk does not contain an Action status.")
    return decodeStatus(chunk.data)
}

/** Decode an Action status; malformed status chunks return their parsing status. */
fun statusFromChunk(chunk: Chunk): Status = when (val decoded = decodeStatusChunk(chunk)) {
    is Ok -> decoded.value.status
    is Status -> decoded
}
