package a11

/**
 * Wire data model, byte-compatible with `js/src/data.ts` and `a11/data`.
 *
 * Each type serializes to the concatenated-MessagePack field layout via
 * [MsgpackCodec]. Fields are mutable to match the reference implementation,
 * where the action/session layers rewrite fragment ids and continuation flags
 * in place.
 */

private const val UINT32_MAX = 0xffff_ffffL
private const val UINT32_RANGE = 0x1_0000_0000L
private val NAME_PATTERN = Regex("^[A-Za-z0-9_](?:[A-Za-z0-9_#-]{0,253}[A-Za-z0-9_])?$")

/** Validate an action, port, node, session, stream, or metadata identifier. */
fun validateName(name: String): Status {
    if (name.length !in 1..255) {
        return invalidArgument("name must contain between 1 and 255 characters")
    }
    if (!NAME_PATTERN.matches(name)) {
        return invalidArgument(
            "name must start and end with an ASCII letter, digit, or underscore and " +
                "contain only [a-zA-Z0-9-_#]",
        )
    }
    return Status.ok()
}

private fun validateOptionalName(value: String): Status =
    if (value.isEmpty()) Status.ok() else validateName(value)

private fun asString(value: Any?, field: String): StatusOr<String> =
    if (value is String) Ok(value) else invalidArgument("$field must be a string.")

private fun asBinary(value: Any?, field: String): StatusOr<ByteArray> =
    if (value is ByteArray) Ok(value.copyOf()) else invalidArgument("$field must be MessagePack binary data.")

private fun asUnsigned(value: Any?, field: String, maximum: Long): StatusOr<Long> {
    val n = when (value) {
        is Long -> value
        is Int -> value.toLong()
        else -> return invalidArgument("$field must be a non-negative integer.")
    }
    return if (n in 0..maximum) Ok(n) else outOfRange("$field exceeds its supported range.")
}

private fun decodeByteMap(value: Any?, field: String): StatusOr<ByteMap> {
    if (value !is Map<*, *>) return invalidArgument("$field must be an object.")
    val result = ByteMap()
    for ((rawKey, rawValue) in value) {
        val key = rawKey as? String ?: return invalidArgument("Invalid key in $field.")
        validateName(key).let { if (!it.isOk) return invalidArgument("Invalid key in $field: ${it.message}") }
        val bytes = rawValue as? ByteArray ?: return invalidArgument("$field.$key must be binary.")
        result[key] = bytes.copyOf()
    }
    return Ok(result)
}

/** MIME type, timestamp (epoch millis), and binary attributes describing a [Chunk]. */
class ChunkMetadata(
    var mimetype: String = "",
    var timestampMillis: Long? = null,
    var attributes: ByteMap = ByteMap(),
) {
    fun validate(): Status {
        for ((key, _) in attributes) {
            validateName(key).let { if (!it.isOk) return it }
        }
        return Status.ok()
    }

    fun toMsgpack(): StatusOr<ByteArray> {
        validate().let { if (!it.isOk) return it }
        val micros = timestampMillis?.let { it * 1000 }
        return MsgpackCodec.encodeFields(listOf(mimetype, micros, LinkedHashMap(attributes)))
    }

    companion object {
        fun fromMsgpack(bytes: ByteArray): StatusOr<ChunkMetadata> {
            val fields = MsgpackCodec.decodeFields(bytes, 3, "ChunkMetadata").orElse { return it }
            val mimetype = asString(fields[0], "ChunkMetadata.mimetype").orElse { return it }
            val timestamp = when (val raw = fields[1]) {
                null -> null
                is Long -> raw / 1000
                is Int -> raw.toLong() / 1000
                else -> return invalidArgument("ChunkMetadata.timestamp must be integer microseconds.")
            }
            val attributes = decodeByteMap(fields[2], "ChunkMetadata.attributes").orElse { return it }
            return Ok(ChunkMetadata(mimetype, timestamp, attributes))
        }
    }
}

/** Smallest typed payload carried through an A11 node. */
class Chunk(
    var metadata: ChunkMetadata? = null,
    var ref: String = "",
    var data: ByteArray = ByteArray(0),
) {
    val mimetype: String get() = metadata?.mimetype ?: ""
    val isEmpty: Boolean get() = ref.isEmpty() && data.isEmpty()

    /** Whether this is the explicit null chunk used as a final terminator. */
    val isNull: Boolean get() = isEmpty && mimetype == "application/octet-stream"

    fun validate(): Status {
        if (ref.isNotEmpty() && data.isNotEmpty()) {
            return invalidArgument("Only one of ref or data may be set")
        }
        return metadata?.validate() ?: Status.ok()
    }

    fun toMsgpack(): StatusOr<ByteArray> {
        validate().let { if (!it.isOk) return it }
        val metadataBytes = metadata?.toMsgpack()?.orElse { return it }
        return MsgpackCodec.encodeFields(listOf(metadataBytes, ref, data))
    }

    companion object {
        fun fromMsgpack(bytes: ByteArray): StatusOr<Chunk> {
            val fields = MsgpackCodec.decodeFields(bytes, 3, "Chunk").orElse { return it }
            val metadata = if (fields[0] == null) null else {
                val encoded = asBinary(fields[0], "Chunk.metadata").orElse { return it }
                ChunkMetadata.fromMsgpack(encoded).orElse { return it }
            }
            val ref = asString(fields[1], "Chunk.ref").orElse { return it }
            val data = asBinary(fields[2], "Chunk.data").orElse { return it }
            return Ok(Chunk(metadata, ref, data))
        }
    }
}

/** Reference to all or part of another logical node instead of inline bytes. */
class NodeRef(var id: String, var offset: Long = 0, var length: Long? = null) {
    fun validate(): Status {
        validateName(id).let { if (!it.isOk) return it }
        if (offset < 0 || offset > UINT32_MAX) return invalidArgument("NodeRef.offset must be a uint32 integer.")
        val len = length
        if (len != null && (len < 0 || len > UINT32_RANGE || len + offset > UINT32_RANGE)) {
            return invalidArgument("Offset + length must not exceed 2^32")
        }
        return Status.ok()
    }

    fun toMsgpack(): StatusOr<ByteArray> {
        validate().let { if (!it.isOk) return it }
        return MsgpackCodec.encodeFields(listOf(id, offset, length))
    }

    companion object {
        fun fromMsgpack(bytes: ByteArray): StatusOr<NodeRef> {
            val fields = MsgpackCodec.decodeFields(bytes, 3, "NodeRef").orElse { return it }
            val id = asString(fields[0], "NodeRef.id").orElse { return it }
            val offset = asUnsigned(fields[1], "NodeRef.offset", UINT32_MAX).orElse { return it }
            val length = if (fields[2] == null) null else
                asUnsigned(fields[2], "NodeRef.length", UINT32_RANGE).orElse { return it }
            return Ok(NodeRef(id, offset, length))
        }
    }
}

/** One sequenced piece of an [AsyncNode]'s ordered stream. */
class NodeFragment(
    var id: String = "",
    var data: Any = Chunk(),
    var seq: Long? = null,
    var continued: Boolean = false,
) {
    init {
        require(data is Chunk || data is NodeRef) { "NodeFragment.data must be a Chunk or NodeRef." }
    }

    fun getChunk(): StatusOr<Chunk> =
        (data as? Chunk)?.let { Ok(it) } ?: failedPrecondition("Data is not a Chunk")

    fun getNodeRef(): StatusOr<NodeRef> =
        (data as? NodeRef)?.let { Ok(it) } ?: failedPrecondition("Data is not a NodeRef")

    fun validate(): Status {
        validateOptionalName(id).let { if (!it.isOk) return it }
        val s = seq
        if (s != null && (s < 0 || s > UINT32_MAX)) {
            return invalidArgument("NodeFragment.seq must be a uint32 integer or null.")
        }
        return when (val d = data) {
            is Chunk -> d.validate()
            is NodeRef -> d.validate()
            else -> invalidArgument("NodeFragment.data must be a Chunk or NodeRef.")
        }
    }

    fun toMsgpack(): StatusOr<ByteArray> {
        validate().let { if (!it.isOk) return it }
        val d = data
        val encoded = when (d) {
            is Chunk -> d.toMsgpack()
            is NodeRef -> d.toMsgpack()
            else -> return invalidArgument("NodeFragment.data must be a Chunk or NodeRef.")
        }.orElse { return it }
        val variant = if (d is Chunk) 0 else 1
        return MsgpackCodec.encodeFields(listOf(id, variant, encoded, seq, continued))
    }

    fun copy(): NodeFragment {
        val clonedData: Any = when (val d = data) {
            is Chunk -> Chunk(d.metadata?.let { ChunkMetadata(it.mimetype, it.timestampMillis, copyByteMap(it.attributes)) }, d.ref, d.data.copyOf())
            is NodeRef -> NodeRef(d.id, d.offset, d.length)
            else -> d
        }
        return NodeFragment(id, clonedData, seq, continued)
    }

    companion object {
        fun fromMsgpack(bytes: ByteArray): StatusOr<NodeFragment> {
            val fields = MsgpackCodec.decodeFields(bytes, 5, "NodeFragment").orElse { return it }
            val id = asString(fields[0], "NodeFragment.id").orElse { return it }
            val variant = asUnsigned(fields[1], "NodeFragment.data index", 1).orElse { return it }
            val encoded = asBinary(fields[2], "NodeFragment.data").orElse { return it }
            val data: Any = if (variant == 0L) Chunk.fromMsgpack(encoded).orElse { return it }
            else NodeRef.fromMsgpack(encoded).orElse { return it }
            val seq = if (fields[3] == null) null else
                asUnsigned(fields[3], "NodeFragment.seq", UINT32_MAX).orElse { return it }
            val continued = fields[4] as? Boolean
                ?: return invalidArgument("NodeFragment.continued must be bool.")
            return Ok(NodeFragment(id, data, seq, continued))
        }
    }
}

/** Maps one schema port name onto the concrete node id for an action instance. */
class Port(var name: String = "", var id: String = "") {
    fun validate(): Status {
        validateOptionalName(name).let { if (!it.isOk) return it }
        return validateOptionalName(id)
    }

    fun toMsgpack(): StatusOr<ByteArray> {
        validate().let { if (!it.isOk) return it }
        return MsgpackCodec.encodeFields(listOf(name, id))
    }

    companion object {
        fun fromMsgpack(bytes: ByteArray): StatusOr<Port> {
            val fields = MsgpackCodec.decodeFields(bytes, 2, "Port").orElse { return it }
            val name = asString(fields[0], "Port.name").orElse { return it }
            val id = asString(fields[1], "Port.id").orElse { return it }
            return Ok(Port(name, id))
        }
    }
}

/** Wire description of one action invocation. */
class ActionMessage(
    var id: String = "",
    var name: String = "",
    var inputs: MutableList<Port> = mutableListOf(),
    var outputs: MutableList<Port> = mutableListOf(),
    var headers: ByteMap = ByteMap(),
) {
    fun validate(): Status {
        validateOptionalName(id).let { if (!it.isOk) return it }
        validateOptionalName(name).let { if (!it.isOk) return it }
        for (port in inputs + outputs) port.validate().let { if (!it.isOk) return it }
        for ((key, _) in headers) validateName(key).let { if (!it.isOk) return it }
        return Status.ok()
    }

    fun toMsgpack(): StatusOr<ByteArray> {
        validate().let { if (!it.isOk) return it }
        val inputBytes = inputs.map { it.toMsgpack().orElse { e -> return e } }
        val outputBytes = outputs.map { it.toMsgpack().orElse { e -> return e } }
        return MsgpackCodec.encodeFields(listOf(id, name, inputBytes, outputBytes, LinkedHashMap(headers)))
    }

    companion object {
        fun fromMsgpack(bytes: ByteArray): StatusOr<ActionMessage> {
            val fields = MsgpackCodec.decodeFields(bytes, 5, "ActionMessage").orElse { return it }
            val id = asString(fields[0], "ActionMessage.id").orElse { return it }
            val name = asString(fields[1], "ActionMessage.name").orElse { return it }
            val inputs = decodePortList(fields[2], "inputs").orElse { return it }
            val outputs = decodePortList(fields[3], "outputs").orElse { return it }
            val headers = decodeByteMap(fields[4], "ActionMessage.headers").orElse { return it }
            return Ok(ActionMessage(id, name, inputs, outputs, headers))
        }

        private fun decodePortList(value: Any?, field: String): StatusOr<MutableList<Port>> {
            if (value !is List<*>) return invalidArgument("ActionMessage.$field must be a list.")
            val ports = mutableListOf<Port>()
            for (item in value) {
                val encoded = asBinary(item, "ActionMessage.$field").orElse { return it }
                ports.add(Port.fromMsgpack(encoded).orElse { return it })
            }
            return Ok(ports)
        }
    }
}

/** Atomic application message exchanged by every A11 transport. */
class WireMessage(
    var nodeFragments: MutableList<NodeFragment> = mutableListOf(),
    var actions: MutableList<ActionMessage> = mutableListOf(),
    var headers: ByteMap = ByteMap(),
) {
    /** Whether this carries only headers and can serve as a lifecycle marker. */
    val isHalfClose: Boolean get() = actions.isEmpty() && nodeFragments.isEmpty()

    fun validate(): Status {
        for (fragment in nodeFragments) fragment.validate().let { if (!it.isOk) return it }
        for (action in actions) action.validate().let { if (!it.isOk) return it }
        for ((key, _) in headers) validateName(key).let { if (!it.isOk) return it }
        return Status.ok()
    }

    fun toMsgpack(): StatusOr<ByteArray> {
        validate().let { if (!it.isOk) return it }
        val fragmentBytes = nodeFragments.map { it.toMsgpack().orElse { e -> return e } }
        val actionBytes = actions.map { it.toMsgpack().orElse { e -> return e } }
        return MsgpackCodec.encodeFields(listOf(VERSION, fragmentBytes, actionBytes, LinkedHashMap(headers)))
    }

    companion object {
        /** Current MessagePack/JSON wire schema version. */
        const val VERSION = 1

        fun fromMsgpack(bytes: ByteArray): StatusOr<WireMessage> {
            val fields = MsgpackCodec.decodeFields(bytes, 4, "WireMessage").orElse { return it }
            val version = asUnsigned(fields[0], "WireMessage.version", UINT32_MAX).orElse { return it }
            if (version.toInt() != VERSION) {
                return invalidArgument("Invalid serialized WireMessage version: $version")
            }
            val fragments = mutableListOf<NodeFragment>()
            (fields[1] as? List<*> ?: return invalidArgument("WireMessage.node_fragments must be a list.")).forEach {
                val encoded = asBinary(it, "WireMessage.node_fragments").orElse { e -> return e }
                fragments.add(NodeFragment.fromMsgpack(encoded).orElse { e -> return e })
            }
            val actions = mutableListOf<ActionMessage>()
            (fields[2] as? List<*> ?: return invalidArgument("WireMessage.actions must be a list.")).forEach {
                val encoded = asBinary(it, "WireMessage.actions").orElse { e -> return e }
                actions.add(ActionMessage.fromMsgpack(encoded).orElse { e -> return e })
            }
            val headers = decodeByteMap(fields[3], "WireMessage.headers").orElse { return it }
            return Ok(WireMessage(fragments, actions, headers))
        }
    }
}

/** Build an empty lifecycle message carrying optional terminal headers. */
fun makeHalfCloseMessage(headers: ByteMap = ByteMap()): WireMessage =
    WireMessage(headers = copyByteMap(headers))

/** Build the explicit octet-stream null chunk used to terminate unary data. */
fun makeNullChunk(): Chunk = Chunk(metadata = ChunkMetadata(mimetype = "application/octet-stream"))
