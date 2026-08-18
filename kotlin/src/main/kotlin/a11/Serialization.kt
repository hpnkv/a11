package a11

/**
 * Bidirectional application-value codecs between Kotlin values and A11 [Chunk]s,
 * ported from `js/src/serialization.ts`.
 *
 * A chunk's metadata is the only thing that says how to read its bytes. The
 * media type gives the representation, and when the value is not one the format
 * already describes, a stable cross-language `tag` in the MIME `type` parameter
 * names it. Nothing inside the payload repeats that: a bare `application/json`
 * is a complete description and decodes to maps, lists and scalars.
 *
 * The default registry covers the JSON and MessagePack object graphs plus raw
 * bytes — the shapes the LLM SDK ports (`interactions`, `tools`, `event_stream`,
 * text) travel as.
 */

const val JSON_MIMETYPE = "application/json"
const val MSGPACK_MIMETYPE = "application/x-msgpack"
const val OCTET_STREAM_MIMETYPE = "application/octet-stream"
const val TEXT_MIMETYPE = "text/plain"

/**
 * Media types that describe their content completely on their own.
 *
 * A chunk using one carries no `type` parameter and no framing inside the
 * payload: `text/plain` is UTF-8 text and `application/octet-stream` is bytes,
 * and there is nothing a `;type=` could add. These are the default
 * representations for [String] and [ByteArray], which is what makes a string
 * chunk the string itself rather than a JSON-quoted copy, and a bytes chunk the
 * bytes rather than base64 inside a JSON string.
 */
private val SELF_DESCRIBING_MEDIA_TYPES = setOf(TEXT_MIMETYPE, OCTET_STREAM_MIMETYPE)

/**
 * Tags a JSON or MessagePack payload already spells out for itself.
 *
 * A chunk holding one of these carries no type parameter: writing `;type=object`
 * on an object says nothing a parser did not already know, and it stops a peer
 * that only has `application/json` from being understood.
 */
private val GENERIC_TAGS =
    setOf("object", "array", "string", "integer", "number", "boolean", "null")

private data class ParsedMimetype(val mediaType: String, val parameters: Map<String, String>)

private fun parseMimetype(value: String): StatusOr<ParsedMimetype> {
    if (value.isBlank()) return invalidArgument("Mimetype must be a non-empty string.")
    val parts = value.split(";")
    val mediaType = parts[0].trim().lowercase()
    if (mediaType.split("/").size != 2) return invalidArgument("Invalid mimetype: $value.")
    val params = LinkedHashMap<String, String>()
    for (raw in parts.drop(1)) {
        val sep = raw.indexOf('=')
        if (sep < 1) return invalidArgument("Invalid mimetype parameter in $value.")
        val name = raw.substring(0, sep).trim().lowercase()
        var param = raw.substring(sep + 1).trim()
        if (param.startsWith("\"") && param.endsWith("\"")) param = param.substring(1, param.length - 1)
        if (param.isEmpty()) return invalidArgument("Invalid mimetype parameter in $value.")
        params[name] = param
    }
    return Ok(ParsedMimetype(mediaType, params))
}

private fun wildcardMatches(value: String, pattern: String): Boolean {
    if (pattern == value) return true
    if (!pattern.contains('*') && !pattern.contains('?')) return false
    val escaped = buildString {
        for (c in pattern) when (c) {
            '*' -> append(".*")
            '?' -> append('.')
            in ".+^\${}()|\\[]" -> { append('\\'); append(c) }
            else -> append(c)
        }
    }
    return Regex("^$escaped$").matches(value)
}

private fun percentEncodeTag(tag: String): String = tag // A11 tags are token-safe.

/**
 * `data` as text, refusing anything that is not well-formed UTF-8.
 *
 * `String(bytes, UTF_8)` would substitute U+FFFD for malformed input and hand
 * back a plausible-looking string, which turns a peer's encoding bug into
 * silent corruption several steps away from its cause.
 */
private fun decodeUtf8Strictly(data: ByteArray): StatusOr<String> {
    val decoder = Charsets.UTF_8.newDecoder()
        .onMalformedInput(java.nio.charset.CodingErrorAction.REPORT)
        .onUnmappableCharacter(java.nio.charset.CodingErrorAction.REPORT)
    return try {
        Ok(decoder.decode(java.nio.ByteBuffer.wrap(data)).toString())
    } catch (error: java.nio.charset.CharacterCodingException) {
        invalidArgument("A $TEXT_MIMETYPE chunk is not valid UTF-8: ${error.message}.")
    }
}

private fun percentDecodeTag(tag: String): String =
    if (tag.contains('%')) java.net.URLDecoder.decode(tag, Charsets.UTF_8) else tag

private fun formatMimetype(parsed: ParsedMimetype, tag: String): String {
    val params = parsed.parameters.filterKeys { it != "type" }.toMutableMap()
    if (tag !in GENERIC_TAGS && parsed.mediaType !in SELF_DESCRIBING_MEDIA_TYPES) {
        params["type"] = percentEncodeTag(tag)
    }
    return parsed.mediaType + params.entries.joinToString("") { ";${it.key}=${it.value}" }
}

/** One application-value codec. */
interface SerializationCodec {
    val tag: String
    val mimetype: String
    fun test(value: Any?): Boolean
    fun serialize(value: Any?): StatusOr<ByteArray>
    fun deserialize(data: ByteArray, chunk: Chunk): StatusOr<Any?>
}

private class RegisteredCodec(val codec: SerializationCodec, val parsed: ParsedMimetype, val order: Int)

// --- wire transform ----------------------------------------------------------

/**
 * Encode [value] as a JSON- or MessagePack-ready tree.
 *
 * Nothing here is tagged. A [ByteArray] becomes base64 (or, in MessagePack, real
 * bytes), an [java.time.Instant] becomes an ISO string — exactly what the format
 * can say, and no more. Recovering the original type is the reader's job, and it
 * does that from the chunk's `;type=`.
 */
private fun toWire(value: Any?, binary: Boolean): StatusOr<Any?> {
    // A value that names its own type wins over the structural branches below.
    // An Interaction *is* a Map, and its codec is what turns it into the fields
    // the peer's strict `interactions` port expects.
    val codec = if (value != null) wireValueCodecFor(value) else null
    if (codec != null) {
        val dumped = codec.dump(value).orElse { return it }
        return toWire(dumped, binary)
    }
    return when (value) {
        null, is Boolean, is String, is Long, is Double -> Ok(value)
        is Int -> Ok(value.toLong())
        is Float -> Ok(value.toDouble())
        is ByteArray -> Ok(if (binary) value else base64Encode(value))
        is List<*> -> {
            val result = ArrayList<Any?>(value.size)
            for (item in value) result.add(toWire(item, binary).orElse { return it })
            Ok(result)
        }
        is Set<*> -> {
            val result = ArrayList<Any?>(value.size)
            for (item in value) result.add(toWire(item, binary).orElse { return it })
            Ok(result)
        }
        is Map<*, *> -> {
            val result = LinkedHashMap<String, Any?>()
            for ((k, v) in value) result[k.toString()] = toWire(v, binary).orElse { return it }
            Ok(result)
        }
        is java.time.Instant ->
            Ok(java.time.format.DateTimeFormatter.ISO_INSTANT.format(value))
        is A11Serializable -> invalidArgument(
            "No wire value codec is registered for ${value::class.java.name}.",
        )
        else -> invalidArgument(
            "Values of type ${value::class.java.name} cannot be serialized by the default codecs.",
        )
    }
}

private fun canonicalJsonTag(value: Any?): String? = when (value) {
    null -> "null"
    is Boolean -> "boolean"
    is String -> "string"
    is Int, is Long -> "integer"
    is Double -> if (value.isFinite() && value % 1.0 == 0.0) "integer" else "number"
    is Float -> canonicalJsonTag(value.toDouble())
    is List<*> -> "array"
    is Map<*, *> -> "object"
    else -> null
}

private fun jsonSerialize(value: Any?): StatusOr<ByteArray> {
    val wire = toWire(value, binary = false).orElse { return it }
    val text = A11Json.encodeToString(wire).orElse { return it }
    return Ok(utf8Encode(text))
}

private fun jsonDeserialize(data: ByteArray): StatusOr<Any?> {
    val text = utf8Decode(data).orElse { return it }
    return A11Json.parse(text)
}

private fun msgpackSerialize(value: Any?): StatusOr<ByteArray> {
    val wire = toWire(value, binary = true).orElse { return it }
    return noexcept("Failed to serialize MessagePack.") { Ok(MsgpackCodec.encodeOne(wire)) }
}

private fun msgpackDeserialize(data: ByteArray): StatusOr<Any?> = MsgpackCodec.decodeOne(data)

/** Ordered collection of codecs between application values and A11 chunks. */
class SerializationRegistry(registerDefaults: Boolean = false) {
    private val codecs = ArrayList<RegisteredCodec>()
    private var nextOrder = 0
    private var wireValueCache: List<RegisteredCodec>? = null
    private var wireValueGeneration = -1

    init {
        ensureCoreWireValues()
        if (registerDefaults) installDefaults()
    }

    /**
     * Codecs for the class-tagged types, derived from the wire-value registry.
     *
     * They are derived rather than registered because that registry grows as
     * types are first touched: an SDK type adds `a11.sdk.Interaction` whenever
     * the application first loads it, which can be after this registry was
     * built. They sort ahead of everything else, since a value that knows its
     * own class must not be claimed by the generic `object` codec.
     */
    private fun wireValueRegistrations(): List<RegisteredCodec> {
        val cached = wireValueCache
        if (cached != null && wireValueGeneration == wireValueCodecCount()) return cached
        val derived = ArrayList<RegisteredCodec>()
        for (wireValue in allWireValueCodecs()) {
            for (mimetype in listOf(JSON_MIMETYPE, MSGPACK_MIMETYPE)) {
                val parsed = parseMimetype(mimetype).orElse { continue }
                val json = mimetype == JSON_MIMETYPE
                val codec = functional(wireValue.tag, mimetype,
                    test = { wireValue.test(it) },
                    serialize = { if (json) jsonSerialize(it) else msgpackSerialize(it) },
                    deserialize = { data, _ ->
                        val decoded = (if (json) jsonDeserialize(data) else msgpackDeserialize(data))
                            .orElse { return@functional it }
                        if (decoded !is Map<*, *>) {
                            invalidArgument("A ${wireValue.tag} payload must be an object.")
                        } else {
                            wireValue.load(decoded.entries.associate { e -> e.key.toString() to e.value })
                        }
                    },
                )
                derived.add(RegisteredCodec(codec, parsed, -1))
            }
        }
        wireValueCache = derived
        wireValueGeneration = wireValueCodecCount()
        return derived
    }

    fun register(codec: SerializationCodec): Status {
        if (codec.tag.isEmpty()) return invalidArgument("Serialization codec tag must be non-empty.")
        val parsed = parseMimetype(codec.mimetype).orElse { return it }
        if (codecs.any { it.codec.tag == codec.tag && it.parsed.mediaType == parsed.mediaType }) {
            return alreadyExists("A codec for ${codec.tag} and ${parsed.mediaType} is already registered.")
        }
        codecs.add(RegisteredCodec(codec, parsed, nextOrder++))
        return Status.ok()
    }

    private fun installDefaults(): Status {
        // Text first: registration order picks the representation when a caller
        // names no mimetype, and a string travelling as itself beats a
        // JSON-quoted copy of itself. The JSON and MessagePack codecs below stay
        // registered, so asking for them still works and a peer that sends them
        // is still read.
        register(functional("string", TEXT_MIMETYPE,
            test = { it is String },
            serialize = { Ok((it as String).toByteArray(Charsets.UTF_8)) },
            deserialize = { data, _ -> decodeUtf8Strictly(data) },
        )).let { if (!it.isOk) return it }
        val jsonTags = listOf("null", "boolean", "integer", "number", "string", "array", "object")
        for (mimetype in listOf(JSON_MIMETYPE, MSGPACK_MIMETYPE)) {
            for (tag in jsonTags) {
                register(functional(tag, mimetype,
                    test = { canonicalJsonTag(it) == tag },
                    serialize = { if (mimetype == JSON_MIMETYPE) jsonSerialize(it) else msgpackSerialize(it) },
                    deserialize = { data, _ -> if (mimetype == JSON_MIMETYPE) jsonDeserialize(data) else msgpackDeserialize(data) },
                )).let { if (!it.isOk) return it }
            }
        }
        register(functional("bytes", OCTET_STREAM_MIMETYPE,
            test = { it is ByteArray },
            serialize = { Ok(it as ByteArray) },
            deserialize = { data, _ -> Ok(data.copyOf()) },
        )).let { if (!it.isOk) return it }
        for (mimetype in listOf(JSON_MIMETYPE, MSGPACK_MIMETYPE)) {
            register(functional("bytes", mimetype,
                test = { it is ByteArray },
                serialize = { if (mimetype == JSON_MIMETYPE) jsonSerialize(it) else msgpackSerialize(it) },
                deserialize = { data, _ ->
                    when (val decoded = if (mimetype == JSON_MIMETYPE) jsonDeserialize(data) else msgpackDeserialize(data)) {
                        is Ok -> (decoded.value as? ByteArray)?.let { Ok(it) }
                            ?: invalidArgument("Serialized bytes did not decode to byte data.")
                        is Status -> decoded
                    }
                },
            )).let { if (!it.isOk) return it }
        }
        return Status.ok()
    }

    /** Select a matching serializer and produce an owned [Chunk]. */
    fun toChunk(value: Any?, mimetype: String = ""): StatusOr<Chunk> {
        val selection = if (mimetype.isEmpty()) null else parseMimetype(mimetype).orElse { return it }
        // A selector picks a representation; the value's own type decides the
        // tag, so a type parameter in it is ignored.
        val candidates = (wireValueRegistrations() + codecs)
            .filter { it.codec.test(value) && (selection == null || registrationMatches(it.parsed, selection)) }
            .sortedBy { it.order }
        if (candidates.isEmpty()) {
            return notFound("No serializer is registered for the value${if (mimetype.isNotEmpty()) " and $mimetype" else ""}.")
        }
        val codec = candidates.first()
        val data = codec.codec.serialize(value).orElse { return it }
        val exactMimetype = formatMimetype(codec.parsed, codec.codec.tag)
        return Ok(Chunk(metadata = ChunkMetadata(mimetype = exactMimetype), data = data))
    }

    /**
     * Select a decoder from the chunk's metadata and return a typed value.
     *
     * [mimetypePatterns] constrains the *representation*. Which type comes back
     * is the chunk's `;type=`, or [expectedTag] when the caller names one. A
     * chunk with no type parameter is not underspecified — it holds exactly
     * what its format describes, and decodes to that.
     */
    fun fromChunk(chunk: Chunk, mimetypePatterns: List<String> = emptyList(), expectedTag: String? = null): StatusOr<Any?> {
        chunk.validate().let { if (!it.isOk) return it }
        if (chunk.mimetype.isEmpty()) return invalidArgument("The chunk has no mimetype.")
        val actual = parseMimetype(chunk.mimetype).orElse { return it }
        val encodedTag = actual.parameters["type"]?.let { percentDecodeTag(it) }
        // An untagged chunk contradicts nothing: it holds what its format
        // describes, and expectedTag is then a request to read it as that type.
        if (expectedTag != null && encodedTag != null && encodedTag != expectedTag) {
            return invalidArgument("The chunk contains $encodedTag, not $expectedTag.")
        }
        var selected = mimetypePatterns.isEmpty()
        for (pattern in mimetypePatterns) {
            val parsed = parseMimetype(pattern).orElse { return it }
            if (mimetypeMatches(actual, parsed)) selected = true
        }
        if (!selected) return notFound("The chunk mimetype ${chunk.mimetype} does not match the requested patterns.")

        val wanted = encodedTag ?: expectedTag
        val byMediaType = (wireValueRegistrations() + codecs)
            .filter { registrationMatches(it.parsed, actual) }
            .sortedBy { it.order }
        val generic = byMediaType.filter { it.codec.tag in GENERIC_TAGS }
        var candidates =
            if (wanted == null) generic else byMediaType.filter { it.codec.tag == wanted }
        if (candidates.isEmpty() && expectedTag == null) {
            // Either nothing named a type, or the tag names one this peer never
            // loaded. The bytes are still valid JSON or MessagePack, so hand
            // back what the format describes rather than refusing to look.
            candidates = generic.ifEmpty { byMediaType }
        }
        if (candidates.isEmpty()) return notFound("No deserializer is registered for ${chunk.mimetype}.")
        return candidates.first().codec.deserialize(chunk.data.copyOf(), chunk)
    }

    /**
     * Whether a chunk's own mimetype is one the caller asked for.
     *
     * The `type` parameter takes no part: a selector chooses a *representation*,
     * and which type comes back is settled separately by the tag.
     */
    private fun mimetypeMatches(actual: ParsedMimetype, selection: ParsedMimetype): Boolean {
        if (!wildcardMatches(actual.mediaType, selection.mediaType)) return false
        for ((name, expected) in selection.parameters) {
            if (name == "type") continue
            val value = actual.parameters[name] ?: return false
            if (!wildcardMatches(value, expected)) return false
        }
        return true
    }

    private fun registrationMatches(registered: ParsedMimetype, selection: ParsedMimetype): Boolean {
        if (!wildcardMatches(registered.mediaType, selection.mediaType)) return false
        for ((name, expected) in selection.parameters) {
            if (name == "type") continue
            val value = registered.parameters[name]
            if (value != null && !wildcardMatches(value, expected)) return false
        }
        return true
    }

    private inline fun functional(
        tag: String,
        mimetype: String,
        crossinline test: (Any?) -> Boolean,
        crossinline serialize: (Any?) -> StatusOr<ByteArray>,
        crossinline deserialize: (ByteArray, Chunk) -> StatusOr<Any?>,
    ): SerializationCodec = object : SerializationCodec {
        override val tag = tag
        override val mimetype = mimetype
        override fun test(value: Any?) = test(value)
        override fun serialize(value: Any?) = serialize(value)
        override fun deserialize(data: ByteArray, chunk: Chunk) = deserialize(data, chunk)
    }

    companion object {
        @Volatile
        private var global = SerializationRegistry(registerDefaults = true)

        fun getGlobal(): SerializationRegistry = global
        fun setGlobal(registry: SerializationRegistry) { global = registry }
    }
}
