package a11

/**
 * The tag → class table for values A11 serializes whole.
 *
 * A JSON or MessagePack payload is a tree of plain data, but some of what A11
 * puts on the wire is not plain: an interaction, a [Chunk], a [Status]. A chunk
 * holding one of those names it in its metadata
 * (`application/json;type=a11.Chunk`), and this file is what turns that name
 * into an object and an object back into that name.
 *
 * The tag comes from [SerialTags], so a value written by one language is read by
 * another. The runtime's own data types and declared models such as SDK configs
 * share one namespace: what a tag resolves to is what tells them apart.
 *
 * Nothing inside the payload is tagged. A model's own fields say what they hold,
 * and schemaless data is just data — a peer reading `application/json` with no
 * type parameter gets maps, lists and scalars, which is all the format ever
 * promised.
 *
 * A type joins the registry through [registerWireValueCodec]. Values with no
 * class of their own — an SDK model is a map here, not an instance — implement
 * [A11Serializable] to say what they are.
 */

/** Plain field map, the form a value takes between object and wire. */
typealias Fields = LinkedHashMap<String, Any?>

/**
 * How one class is written into, and read back out of, its wire fields.
 *
 * [dump] and [load] deal in *plain fields only* — the surrounding encoder walks
 * whatever they return, so a field may itself hold bytes or another registered
 * value without either side knowing.
 */
interface WireValueCodec {
    /** Canonical tag from [SerialTags], written as the chunk's `;type=`. */
    val tag: String
    fun test(value: Any?): Boolean
    fun dump(value: Any?): StatusOr<Fields>
    fun load(fields: Map<String, Any?>): StatusOr<Any?>
}

// Copy-on-write, because these are registered as types are first touched --
// which can happen on one thread while another is part-way through iterating
// them to decode a chunk. Registration is rare; iteration is on every decode.
private val wireValueCodecs = java.util.concurrent.CopyOnWriteArrayList<WireValueCodec>()
private val wireValueCodecsByTag =
    java.util.concurrent.ConcurrentHashMap<String, WireValueCodec>()

/**
 * Register a class so it survives being nested in a serialized value.
 *
 * Idempotent for the same codec: a module loaded twice must not fail, but two
 * different codecs claiming one tag is a bug worth reporting.
 */
fun registerWireValueCodec(codec: WireValueCodec): Status {
    if (codec.tag.isEmpty()) return invalidArgument("A wire value codec tag must be non-empty.")
    val existing = wireValueCodecsByTag[codec.tag]
    if (existing != null) {
        if (existing === codec) return Status.ok()
        return alreadyExists("A wire value codec for ${codec.tag} is already registered.")
    }
    wireValueCodecsByTag[codec.tag] = codec
    wireValueCodecs.add(codec)
    return Status.ok()
}

/** The codec that claims [value], or `null` when it is ordinary data. */
fun wireValueCodecFor(value: Any?): WireValueCodec? = wireValueCodecs.firstOrNull { it.test(value) }

/** The codec registered for [tag]. */
fun wireValueCodecByTag(tag: String): WireValueCodec? = wireValueCodecsByTag[tag]

/** Every registered codec, in registration order. */
fun allWireValueCodecs(): List<WireValueCodec> = wireValueCodecs

/**
 * How many codecs are registered.
 *
 * A registry deriving state from this one watches the count to know its
 * derivation is stale: SDK types register when their file is first touched,
 * which can happen long after a serialization registry was built.
 */
fun wireValueCodecCount(): Int = wireValueCodecs.size

/** Build a codec from lambdas, the common case. */
fun wireValueCodec(
    tag: String,
    test: (Any?) -> Boolean,
    dump: (Any?) -> StatusOr<Fields>,
    load: (Map<String, Any?>) -> StatusOr<Any?>,
): WireValueCodec = object : WireValueCodec {
    override val tag = tag
    override fun test(value: Any?) = test(value)
    override fun dump(value: Any?) = dump(value)
    override fun load(fields: Map<String, Any?>) = load(fields)
}

/** A [test] matching values that name themselves [tag] via [A11Serializable]. */
fun testSerializable(tag: String): (Any?) -> Boolean =
    { value -> value is A11Serializable && value.a11SerialTag == tag }

// --- Field helpers -----------------------------------------------------------
//
// The runtime's types omit fields holding their default, exactly as pydantic's
// model_dump() does on the Python side. Byte-for-byte agreement matters: a value
// decoded here and sent back must be the payload the peer sent, not a
// re-spelling of it.

private fun readString(fields: Map<String, Any?>, key: String): String = fields[key] as? String ?: ""

// An untagged byte field arrives as base64 from JSON and as real bytes from
// MessagePack; both are the same field, so both have to read.
private fun readBytes(value: Any?): ByteArray = when (value) {
    is ByteArray -> value
    is String -> base64Decode(value).valueOrNull() ?: ByteArray(0)
    else -> ByteArray(0)
}

private fun readLongOrNull(value: Any?): Long? = when (value) {
    null -> null
    is Long -> value
    is Int -> value.toLong()
    is Number -> value.toLong()
    else -> null
}

private fun readByteMap(value: Any?): ByteMap {
    val result = ByteMap()
    if (value is Map<*, *>) for ((k, v) in value) result[k.toString()] = readBytes(v)
    return result
}

private fun byteMapFields(map: ByteMap): Fields {
    val result = Fields()
    for ((k, v) in map) result[k] = v
    return result
}

// --- The runtime's own types -------------------------------------------------

private fun dumpChunkMetadata(value: ChunkMetadata): Fields {
    val fields = Fields()
    fields["mimetype"] = value.mimetype
    value.timestampMillis?.let { fields["timestamp"] = java.time.Instant.ofEpochMilli(it) }
    if (value.attributes.isNotEmpty()) fields["attributes"] = byteMapFields(value.attributes)
    return fields
}

private fun loadChunkMetadata(fields: Map<String, Any?>): ChunkMetadata = ChunkMetadata(
    mimetype = readString(fields, "mimetype"),
    // Untagged, a timestamp is the RFC 3339 string the field's type implies.
    timestampMillis = when (val raw = fields["timestamp"]) {
        is java.time.Instant -> raw.toEpochMilli()
        is String -> runCatching { java.time.Instant.parse(raw).toEpochMilli() }.getOrNull()
        else -> null
    },
    attributes = readByteMap(fields["attributes"]),
)

private fun dumpChunk(value: Chunk): Fields {
    val fields = Fields()
    fields["data"] = value.data
    value.metadata?.let { fields["metadata"] = dumpChunkMetadata(it) }
    if (value.ref.isNotEmpty()) fields["ref"] = value.ref
    return fields
}

private fun loadChunk(fields: Map<String, Any?>): Chunk {
    val metadata = fields["metadata"]
    return Chunk(
        metadata = when (metadata) {
            is ChunkMetadata -> metadata
            is Map<*, *> -> loadChunkMetadata(metadata.entries.associate { it.key.toString() to it.value })
            else -> null
        },
        ref = readString(fields, "ref"),
        data = readBytes(fields["data"]),
    )
}

private fun dumpNodeRef(value: NodeRef): Fields {
    val fields = Fields()
    fields["id"] = value.id
    if (value.offset != 0L) fields["offset"] = value.offset
    value.length?.let { fields["length"] = it }
    return fields
}

private fun loadNodeRef(fields: Map<String, Any?>): NodeRef = NodeRef(
    id = readString(fields, "id"),
    offset = readLongOrNull(fields["offset"]) ?: 0L,
    length = readLongOrNull(fields["length"]),
)

private fun asFields(value: Any?): Map<String, Any?> =
    (value as? Map<*, *>)?.entries?.associate { it.key.toString() to it.value } ?: emptyMap()

private fun dumpNodeFragment(value: NodeFragment): StatusOr<Fields> {
    val data = when (val payload = value.data) {
        is Chunk -> dumpChunk(payload)
        is NodeRef -> dumpNodeRef(payload)
        else -> return invalidArgument("A NodeFragment payload must be a Chunk or a NodeRef.")
    }
    val fields = Fields()
    fields["data"] = data
    if (value.id.isNotEmpty()) fields["id"] = value.id
    value.seq?.let { fields["seq"] = it }
    if (value.continued) fields["continued"] = true
    return Ok(fields)
}

private fun loadNodeFragment(fields: Map<String, Any?>): NodeFragment {
    val raw = fields["data"]
    val data: Any = when {
        raw is Chunk || raw is NodeRef -> raw
        raw is Map<*, *> -> {
            val inner = asFields(raw)
            // A NodeRef payload has no `data` key of its own; a Chunk always does.
            if (inner.containsKey("data") || inner.containsKey("metadata")) loadChunk(inner)
            else loadNodeRef(inner)
        }
        else -> Chunk()
    }
    return NodeFragment(
        id = readString(fields, "id"),
        data = data,
        seq = readLongOrNull(fields["seq"]),
        continued = fields["continued"] == true,
    )
}

private fun dumpPort(value: Port): Fields {
    val fields = Fields()
    if (value.name.isNotEmpty()) fields["name"] = value.name
    if (value.id.isNotEmpty()) fields["id"] = value.id
    return fields
}

private fun loadPort(fields: Map<String, Any?>): Port =
    Port(name = readString(fields, "name"), id = readString(fields, "id"))

private fun loadPorts(value: Any?): MutableList<Port> {
    val result = mutableListOf<Port>()
    if (value is List<*>) for (entry in value) {
        result.add(if (entry is Port) entry else loadPort(asFields(entry)))
    }
    return result
}

private fun dumpActionMessage(value: ActionMessage): Fields {
    val fields = Fields()
    fields["id"] = value.id
    fields["name"] = value.name
    if (value.inputs.isNotEmpty()) fields["inputs"] = value.inputs.map { dumpPort(it) }
    if (value.outputs.isNotEmpty()) fields["outputs"] = value.outputs.map { dumpPort(it) }
    if (value.headers.isNotEmpty()) fields["headers"] = byteMapFields(value.headers)
    return fields
}

private fun loadActionMessage(fields: Map<String, Any?>): ActionMessage = ActionMessage(
    id = readString(fields, "id"),
    name = readString(fields, "name"),
    inputs = loadPorts(fields["inputs"]),
    outputs = loadPorts(fields["outputs"]),
    headers = readByteMap(fields["headers"]),
)

private fun dumpWireMessage(value: WireMessage): StatusOr<Fields> {
    val fields = Fields()
    if (value.actions.isNotEmpty()) fields["actions"] = value.actions.map { dumpActionMessage(it) }
    if (value.nodeFragments.isNotEmpty()) {
        fields["node_fragments"] = value.nodeFragments.map { dumpNodeFragment(it).orElse { s -> return s } }
    }
    if (value.headers.isNotEmpty()) fields["headers"] = byteMapFields(value.headers)
    return Ok(fields)
}

private fun loadWireMessage(fields: Map<String, Any?>): WireMessage {
    val actions = mutableListOf<ActionMessage>()
    (fields["actions"] as? List<*>)?.forEach {
        actions.add(if (it is ActionMessage) it else loadActionMessage(asFields(it)))
    }
    val fragments = mutableListOf<NodeFragment>()
    (fields["node_fragments"] as? List<*>)?.forEach {
        fragments.add(if (it is NodeFragment) it else loadNodeFragment(asFields(it)))
    }
    return WireMessage(nodeFragments = fragments, actions = actions, headers = readByteMap(fields["headers"]))
}

private fun dumpStatus(value: Status): Fields {
    val fields = Fields()
    fields["code"] = value.code.value.toLong()
    fields["message"] = value.message
    if (value.details.isNotEmpty()) fields["details"] = value.details
    return fields
}

private fun loadStatus(fields: Map<String, Any?>): StatusOr<Status> {
    val code = readLongOrNull(fields["code"])?.toInt() ?: 0
    val statusCode = StatusCode.entries.firstOrNull { it.value == code }
        ?: return invalidArgument("Unknown A11 status code $code.")
    @Suppress("UNCHECKED_CAST")
    val details = (fields["details"] as? List<Map<String, Any?>>) ?: emptyList()
    return Ok(Status(statusCode, readString(fields, "message"), details))
}

private fun installCoreWireValues() {
    registerWireValueCodec(wireValueCodec(SerialTags.CHUNK_METADATA,
        test = { it is ChunkMetadata },
        dump = { Ok(dumpChunkMetadata(it as ChunkMetadata)) },
        load = { Ok(loadChunkMetadata(it)) }))
    registerWireValueCodec(wireValueCodec(SerialTags.CHUNK,
        test = { it is Chunk },
        dump = { Ok(dumpChunk(it as Chunk)) },
        load = { Ok(loadChunk(it)) }))
    registerWireValueCodec(wireValueCodec(SerialTags.NODE_REF,
        test = { it is NodeRef },
        dump = { Ok(dumpNodeRef(it as NodeRef)) },
        load = { Ok(loadNodeRef(it)) }))
    registerWireValueCodec(wireValueCodec(SerialTags.NODE_FRAGMENT,
        test = { it is NodeFragment },
        dump = { dumpNodeFragment(it as NodeFragment) },
        load = { Ok(loadNodeFragment(it)) }))
    registerWireValueCodec(wireValueCodec(SerialTags.PORT,
        test = { it is Port },
        dump = { Ok(dumpPort(it as Port)) },
        load = { Ok(loadPort(it)) }))
    registerWireValueCodec(wireValueCodec(SerialTags.ACTION_MESSAGE,
        test = { it is ActionMessage },
        dump = { Ok(dumpActionMessage(it as ActionMessage)) },
        load = { Ok(loadActionMessage(it)) }))
    registerWireValueCodec(wireValueCodec(SerialTags.WIRE_MESSAGE,
        test = { it is WireMessage },
        dump = { dumpWireMessage(it as WireMessage) },
        load = { Ok(loadWireMessage(it)) }))
    // A Status carried as data -- an interaction's outcome, say. Recognised by
    // type, so an ordinary map a caller is sending stays ordinary data.
    registerWireValueCodec(wireValueCodec(SerialTags.STATUS,
        test = { it is Status },
        dump = { Ok(dumpStatus(it as Status)) },
        load = { loadStatus(it) }))
}

private val coreWireValuesInstalled: Unit = installCoreWireValues()

/** Force the core codecs to be installed; safe to call repeatedly. */
fun ensureCoreWireValues() {
    coreWireValuesInstalled
}
