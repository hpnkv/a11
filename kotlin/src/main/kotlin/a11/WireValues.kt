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

package a11

import a11.internal.NativeBridge
import a11.internal.decodeActionMessage
import a11.internal.decodeChunk
import a11.internal.decodeChunkMetadata
import a11.internal.decodeNodeFragment
import a11.internal.decodeNodeRef
import a11.internal.decodePort
import a11.internal.decodeWireMessage
import a11.internal.nativeWireFields

/**
 * The tag → class table for values A11 serializes whole.
 *
 * A JSON or MessagePack payload is a tree of plain data, but some of what A11 puts on the wire is
 * not plain: an interaction, a [Chunk], a [Status]. A chunk holding one of those names it in its
 * metadata (`application/json;type=a11.Chunk`), and this file is what turns that name into an
 * object and an object back into that name.
 *
 * The tag comes from [SerialTags], so a value written by one language is read by another. The
 * runtime's own data types and declared models such as SDK configs share one namespace: what a tag
 * resolves to is what tells them apart.
 *
 * Nothing inside the payload is tagged. A model's own fields say what they hold, and schemaless
 * data is just data — a peer reading `application/json` with no type parameter gets maps, lists and
 * scalars, which is all the format ever promised.
 *
 * A type joins the registry through [registerWireValueCodec]. Values with no class of their own —
 * an SDK model is a map here, not an instance — implement [A11Serializable] to say what they are.
 */

/** Plain field map, the form a value takes between object and wire. */
typealias Fields = LinkedHashMap<String, Any?>

/**
 * How one class is written into, and read back out of, its wire fields.
 *
 * [dump] and [load] deal in *plain fields only* — the surrounding encoder walks whatever they
 * return, so a field may itself hold bytes or another registered value without either side knowing.
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
private val wireValueCodecsByTag = java.util.concurrent.ConcurrentHashMap<String, WireValueCodec>()

/**
 * Register a class so it survives being nested in a serialized value.
 *
 * Idempotent for the same codec: a module loaded twice must not fail, but two different codecs
 * claiming one tag is a bug worth reporting.
 */
@Synchronized
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
fun wireValueCodecFor(value: Any?): WireValueCodec? =
    wireValueCodecs.firstOrNull { item -> item.test(value) }

/** The codec registered for [tag]. */
fun wireValueCodecByTag(tag: String): WireValueCodec? = wireValueCodecsByTag[tag]

/** Every registered codec, in registration order. */
fun allWireValueCodecs(): List<WireValueCodec> = wireValueCodecs.toList()

/**
 * How many codecs are registered.
 *
 * A registry deriving state from this one watches the count to know its derivation is stale: SDK
 * types register when their file is first touched, which can happen long after a serialization
 * registry was built.
 */
fun wireValueCodecCount(): Int = wireValueCodecs.size

/** Build a codec from lambdas, the common case. */
fun wireValueCodec(
    tag: String,
    test: (Any?) -> Boolean,
    dump: (Any?) -> StatusOr<Fields>,
    load: (Map<String, Any?>) -> StatusOr<Any?>,
): WireValueCodec =
    object : WireValueCodec {
        override val tag = tag

        override fun test(value: Any?) = test(value)

        override fun dump(value: Any?) = dump(value)

        override fun load(fields: Map<String, Any?>) = load(fields)
    }

/** A [test] matching values that name themselves [tag] via [A11Serializable]. */
fun testSerializable(tag: String): (Any?) -> Boolean = { value ->
    value is A11Serializable && value.a11SerialTag == tag
}

// --- Field helpers -----------------------------------------------------------
//
// The runtime's types omit fields holding their default, exactly as pydantic's
// model_dump() does on the Python side. Byte-for-byte agreement matters: a
// value
// decoded here and sent back must be the payload the peer sent, not a
// re-spelling of it.

private fun normalizeNative(type: String, fields: Map<String, Any?>): StatusOr<Map<String, Any?>> =
    NativeBridge.call(
            "data.$type",
            args = mapOf("action" to "normalize", "fields" to nativeWireFields(fields)),
        )
        .map(NativeBridge::fields)

private fun byteMapFields(map: ByteMap): Fields {
    val result = Fields()
    for ((k, v) in map) result[k] = v
    return result
}

// --- The runtime's own types -------------------------------------------------

private fun dumpChunkMetadata(value: ChunkMetadata): Fields {
    val fields = Fields()
    fields["mimetype"] = value.mimetype
    value.timestampMicros?.let { value ->
        fields["timestamp"] =
            java.time.Instant.ofEpochSecond(
                Math.floorDiv(value, 1_000_000),
                Math.floorMod(value, 1_000_000L) * 1000,
            )
    }
    if (value.attributes.isNotEmpty()) fields["attributes"] = byteMapFields(value.attributes)
    return fields
}

private fun loadChunkMetadata(fields: Map<String, Any?>): StatusOr<ChunkMetadata> =
    normalizeNative("ChunkMetadata", fields).map(::decodeChunkMetadata)

private fun dumpChunk(value: Chunk): Fields {
    val fields = Fields()
    fields["data"] = value.data
    value.metadata?.let { value -> fields["metadata"] = dumpChunkMetadata(value) }
    if (value.ref.isNotEmpty()) fields["ref"] = value.ref
    return fields
}

private fun loadChunk(fields: Map<String, Any?>): StatusOr<Chunk> =
    normalizeNative("Chunk", fields).map(::decodeChunk)

private fun dumpNodeRef(value: NodeRef): Fields {
    val fields = Fields()
    fields["id"] = value.id
    if (value.offset != 0u) fields["offset"] = value.offset.toLong()
    value.length?.let { value -> fields["length"] = value.toLong() }
    return fields
}

private fun loadNodeRef(fields: Map<String, Any?>): StatusOr<NodeRef> =
    normalizeNative("NodeRef", fields).map(::decodeNodeRef)

private fun dumpNodeFragment(value: NodeFragment): StatusOr<Fields> {
    val data =
        when (val payload = value.data) {
            is Chunk -> dumpChunk(payload)
            is NodeRef -> dumpNodeRef(payload)
        }
    val fields = Fields()
    fields["data"] = data
    if (value.id.isNotEmpty()) fields["id"] = value.id
    value.seq?.let { value -> fields["seq"] = value.toLong() }
    if (value.continued) fields["continued"] = true
    return Ok(fields)
}

private fun loadNodeFragment(fields: Map<String, Any?>): StatusOr<NodeFragment> =
    normalizeNative("NodeFragment", fields).map(::decodeNodeFragment)

private fun dumpPort(value: Port): Fields {
    val fields = Fields()
    if (value.name.isNotEmpty()) fields["name"] = value.name
    if (value.id.isNotEmpty()) fields["id"] = value.id
    return fields
}

private fun loadPort(fields: Map<String, Any?>): StatusOr<Port> =
    normalizeNative("Port", fields).map(::decodePort)

private fun dumpActionMessage(value: ActionMessage): Fields {
    val fields = Fields()
    fields["id"] = value.id
    fields["name"] = value.name
    if (value.inputs.isNotEmpty()) fields["inputs"] = value.inputs.map { value -> dumpPort(value) }
    if (value.outputs.isNotEmpty())
        fields["outputs"] = value.outputs.map { value -> dumpPort(value) }
    if (value.headers.isNotEmpty()) fields["headers"] = byteMapFields(value.headers)
    return fields
}

private fun loadActionMessage(fields: Map<String, Any?>): StatusOr<ActionMessage> =
    normalizeNative("ActionMessage", fields).map(::decodeActionMessage)

private fun dumpWireMessage(value: WireMessage): StatusOr<Fields> {
    val fields = Fields()
    if (value.actions.isNotEmpty())
        fields["actions"] = value.actions.map { value -> dumpActionMessage(value) }
    if (value.nodeFragments.isNotEmpty()) {
        fields["node_fragments"] =
            value.nodeFragments.map { value ->
                dumpNodeFragment(value).orElse { s ->
                    return s
                }
            }
    }
    if (value.headers.isNotEmpty()) fields["headers"] = byteMapFields(value.headers)
    return Ok(fields)
}

private fun loadWireMessage(fields: Map<String, Any?>): StatusOr<WireMessage> =
    normalizeNative("WireMessage", fields).map(::decodeWireMessage)

private fun dumpStatus(value: Status): Fields {
    val fields = Fields()
    fields["code"] = value.code.value.toLong()
    fields["message"] = value.message
    if (value.details.isNotEmpty()) fields["details"] = value.details
    return fields
}

private fun loadStatus(fields: Map<String, Any?>): StatusOr<Status> =
    NativeBridge.call("status.normalize", args = mapOf("value" to fields))
        .map(NativeBridge::statusValue)

private fun installCoreWireValues() {
    registerWireValueCodec(
        wireValueCodec(
            SerialTags.CHUNK_METADATA,
            test = { it is ChunkMetadata },
            dump = { Ok(dumpChunkMetadata(it as ChunkMetadata)) },
            load = { loadChunkMetadata(it) },
        )
    )
    registerWireValueCodec(
        wireValueCodec(
            SerialTags.CHUNK,
            test = { it is Chunk },
            dump = { Ok(dumpChunk(it as Chunk)) },
            load = { loadChunk(it) },
        )
    )
    registerWireValueCodec(
        wireValueCodec(
            SerialTags.NODE_REF,
            test = { it is NodeRef },
            dump = { Ok(dumpNodeRef(it as NodeRef)) },
            load = { loadNodeRef(it) },
        )
    )
    registerWireValueCodec(
        wireValueCodec(
            SerialTags.NODE_FRAGMENT,
            test = { it is NodeFragment },
            dump = { dumpNodeFragment(it as NodeFragment) },
            load = { loadNodeFragment(it) },
        )
    )
    registerWireValueCodec(
        wireValueCodec(
            SerialTags.PORT,
            test = { it is Port },
            dump = { Ok(dumpPort(it as Port)) },
            load = { loadPort(it) },
        )
    )
    registerWireValueCodec(
        wireValueCodec(
            SerialTags.ACTION_MESSAGE,
            test = { it is ActionMessage },
            dump = { Ok(dumpActionMessage(it as ActionMessage)) },
            load = { loadActionMessage(it) },
        )
    )
    registerWireValueCodec(
        wireValueCodec(
            SerialTags.WIRE_MESSAGE,
            test = { it is WireMessage },
            dump = { dumpWireMessage(it as WireMessage) },
            load = { loadWireMessage(it) },
        )
    )
    // A Status carried as data -- an interaction's outcome, say. Recognised by
    // type, so an ordinary map a caller is sending stays ordinary data.
    registerWireValueCodec(
        wireValueCodec(
            SerialTags.STATUS,
            test = { it is Status },
            dump = { Ok(dumpStatus(it as Status)) },
            load = { loadStatus(it) },
        )
    )
}

private val coreWireValuesInstalled: Unit = installCoreWireValues()

/** Force the core codecs to be installed; safe to call repeatedly. */
fun ensureCoreWireValues() {
    coreWireValuesInstalled
}
