package a11.sdk

import a11.A11Serializable
import a11.Action
import a11.Chunk
import a11.Fields
import a11.Ok
import a11.SerialTags
import a11.SerializationRegistry
import a11.Status
import a11.StatusOr
import a11.base64Decode
import a11.errorOrNull
import a11.invalidArgument
import a11.orElse
import a11.registerWireValueCodec
import a11.testSerializable
import a11.utf8Decode
import a11.valueOrNull
import a11.wireValueCodec
import a11.wireValueCodecByTag
import java.util.UUID

/** Framework headers that select and configure an LLM backend. */
enum class LlmHeaders(val header: String) {
    API_KEY("x-a11-llm-api-key"),
    MODEL("x-a11-llm-model"),
    PROVIDER("x-a11-llm-provider"),
    BASE_URL("x-a11-llm-base-url"),
    ALLOWED_LLM_ACTIONS("x-a11-allowed-llm-actions"),
}

/**
 * Conversation roles. `model` is the assistant role, matching the Python SDK.
 */
enum class Role(val value: String) {
    SYSTEM("system"), ASSISTANT("model"), USER("user")
}

/**
 * Portable, backend-independent record of one turn; JSON object on the wire.
 *
 * The map retains the `a11.sdk.Interaction` type tag through serialization, so
 * it can be nested in another value and accepted by strict interaction ports.
 */
class Interaction : LinkedHashMap<String, Any?>, A11Serializable {
    constructor() : super()
    constructor(source: Map<String, Any?>) : super(source)

    override val a11SerialTag: String get() = SerialTags.INTERACTION
}

/** Build a fully-defaulted interaction from a partial one. */
fun makeInteraction(partial: Map<String, Any?> = emptyMap()): Interaction {
    val result = Interaction()
    result["id"] = partial["id"] ?: UUID.randomUUID().toString()
    result["role"] = partial["role"] ?: Role.USER.value
    result["previous_interaction_id"] = partial["previous_interaction_id"] ?: ""
    result["model"] = partial["model"] ?: ""
    result["system_instructions"] = partial["system_instructions"] ?: emptyList<Any?>()
    result["content"] = partial["content"] ?: emptyList<Any?>()
    result["action_calls"] = partial["action_calls"] ?: emptyList<Any?>()
    result["action_inputs"] = partial["action_inputs"] ?: emptyMap<String, Any?>()
    result["action_outputs"] = partial["action_outputs"] ?: emptyMap<String, Any?>()
    result["backend_specific_metadata"] = partial["backend_specific_metadata"] ?: emptyMap<String, Any?>()
    for ((k, v) in partial) if (!result.containsKey(k)) result[k] = v
    return result
}

/**
 * Build an interaction carrying a single text message.
 *
 * The content is the backend-neutral `{role, content: [text part]}` envelope,
 * so a plain text turn stays portable across a mid-conversation model switch.
 *
 * `content` and `system_instructions`
 * contain [Chunk] values, as required by the
 * backend-independent interaction schema.
 */
fun makeTextMessageInteraction(
    text: String,
    systemPrompt: String = "",
    role: Role = Role.USER,
    registry: SerializationRegistry = SerializationRegistry.getGlobal(),
): StatusOr<Interaction> {
    if (role == Role.SYSTEM) return invalidArgument("A text message interaction cannot use the system role as content.")
    val roleStr = if (role == Role.ASSISTANT) "model" else "user"
    val content = registry.toChunk(linkedMapOf<String, Any?>(
        "role" to roleStr,
        "content" to listOf(linkedMapOf<String, Any?>("type" to "text", "text" to text)),
    )).orElse { return it }
    val instructions = if (systemPrompt.isEmpty()) emptyList() else {
        listOf(registry.toChunk(systemPrompt).orElse { return it })
    }
    return Ok(makeInteraction(mapOf(
        "role" to role.value,
        "content" to listOf(content),
        "system_instructions" to instructions,
    )))
}

/**
 * A model an interaction nests, held as a map that knows its own name.
 *
 * Kotlin has no field-by-field port of these — an interaction is a map here,
 * and so is everything inside it — but they still have to arrive at a Python
 * peer as their own types rather than as anonymous objects. Naming them is
 * enough for that; the fields ride along untouched.
 */
class TaggedModel(override val a11SerialTag: String, source: Map<String, Any?> = emptyMap()) :
    LinkedHashMap<String, Any?>(source), A11Serializable

/** An `a11.sdk.Peer`: who should run an action. */
fun makePeer(fields: Map<String, Any?> = emptyMap()): TaggedModel =
    TaggedModel(SerialTags.PEER, fields)

/** An `a11.sdk.ActionConfig`: how one action of an interaction is run. */
fun makeActionConfig(fields: Map<String, Any?> = emptyMap()): TaggedModel =
    TaggedModel(SerialTags.ACTION_CONFIG, fields)

/** An `a11.sdk.UsageMetadata`: an interaction's token accounting. */
fun makeUsageMetadata(fields: Map<String, Any?> = emptyMap()): TaggedModel =
    TaggedModel(SerialTags.USAGE_METADATA, fields)

// --- Serialization -----------------------------------------------------------
//
// These are maps at runtime, so each is recognised by the tag it names rather
// than by shape: a caller's own map must stay ordinary data. `dump` copies the
// entries, since handing the encoder the very map it is walking would alias
// mutable state for no reason.

private fun registerMapModel(tag: String, build: (Map<String, Any?>) -> Any): Status =
    registerWireValueCodec(
        wireValueCodec(
            tag,
            test = testSerializable(tag),
            dump = { value ->
                @Suppress("UNCHECKED_CAST")
                Ok(Fields().also { it.putAll(value as Map<String, Any?>) })
            },
            load = { fields -> Ok(build(fields)) },
        ),
    )

// --- Reading an interaction's fields -----------------------------------------
//
// Nothing on the wire says that `content` holds Chunks or `status` a Status --
// the model's shape does, and with no field-by-field class to hold it, this is
// where that shape is written down. Kotlin's counterpart of the Python model's
// annotations and the TypeScript zod schema; the three must agree.

/** Rebuild one value that the field's declared type identifies as [tag]. */
private fun loadTagged(tag: String, value: Any?): StatusOr<Any?> {
    if (value == null) return Ok(null)
    val codec = wireValueCodecByTag(tag)
        ?: return invalidArgument("No wire value codec is registered for $tag.")
    // An instance a caller built, or one a peer already sent us, is left alone.
    if (codec.test(value)) return Ok(value)
    if (value !is Map<*, *>) return invalidArgument("Expected the fields of a $tag.")
    return codec.load(value.entries.associate { it.key.toString() to it.value })
}

private fun loadTaggedList(tag: String, value: Any?): StatusOr<List<Any?>> {
    val items = value as? List<*> ?: return Ok(emptyList())
    val result = ArrayList<Any?>(items.size)
    for (item in items) result.add(loadTagged(tag, item).orElse { return it })
    return Ok(result)
}

private fun loadTaggedListMap(tag: String, value: Any?): StatusOr<Map<String, Any?>> {
    val entries = value as? Map<*, *> ?: return Ok(emptyMap())
    val result = LinkedHashMap<String, Any?>()
    for ((key, item) in entries) {
        result[key.toString()] = loadTaggedList(tag, item).orElse { return it }
    }
    return Ok(result)
}

private fun loadTaggedMap(tag: String, value: Any?): StatusOr<Map<String, Any?>> {
    val entries = value as? Map<*, *> ?: return Ok(emptyMap())
    val result = LinkedHashMap<String, Any?>()
    for ((key, item) in entries) {
        result[key.toString()] = loadTagged(tag, item).orElse { return it }
    }
    return Ok(result)
}

/** A `dict[str, bytes]` field, whose values the wire spells as base64. */
private fun loadByteMap(value: Any?): Map<String, Any?> {
    val entries = value as? Map<*, *> ?: return emptyMap()
    val result = LinkedHashMap<String, Any?>()
    for ((key, item) in entries) {
        result[key.toString()] = when (item) {
            is ByteArray -> item
            is String -> base64Decode(item).valueOrNull() ?: ByteArray(0)
            else -> item
        }
    }
    return result
}

/**
 * Rebuild an interaction from the bare tree a peer sent.
 *
 * Only fields that are actually present are rewritten, so an interaction that
 * is decoded and sent straight back is byte-identical to the one that arrived.
 */
private fun loadInteraction(fields: Map<String, Any?>): StatusOr<Any?> {
    val result = Interaction(fields)
    fun <T> put(key: String, load: (Any?) -> StatusOr<T>): Status? {
        if (!fields.containsKey(key)) return null
        val loaded = load(fields[key])
        val failure = loaded.errorOrNull()
        if (failure != null) return failure
        result[key] = loaded.valueOrNull()
        return null
    }
    put("status") { loadTagged(SerialTags.STATUS, it) }?.let { return it }
    put("content") { loadTaggedList(SerialTags.CHUNK, it) }?.let { return it }
    put("system_instructions") { loadTaggedList(SerialTags.CHUNK, it) }?.let { return it }
    put("action_calls") { loadTaggedList(SerialTags.ACTION_MESSAGE, it) }?.let { return it }
    put("action_inputs") { loadTaggedListMap(SerialTags.NODE_FRAGMENT, it) }?.let { return it }
    put("action_outputs") { loadTaggedListMap(SerialTags.NODE_FRAGMENT, it) }?.let { return it }
    put("action_configs") { loadTaggedMap(SerialTags.ACTION_CONFIG, it) }?.let { return it }
    put("usage_metadata") { loadTagged(SerialTags.USAGE_METADATA, it) }?.let { return it }
    if (fields.containsKey("backend_specific_metadata")) {
        result["backend_specific_metadata"] =
            loadByteMap(fields["backend_specific_metadata"])
    }
    return Ok(result)
}

/** An action config nests a peer, and carries a `dict[str, bytes]`. */
private fun loadActionConfig(fields: Map<String, Any?>): StatusOr<Any?> {
    val result = TaggedModel(SerialTags.ACTION_CONFIG, fields)
    if (fields.containsKey("peer") && fields["peer"] !is String) {
        result["peer"] = loadTagged(SerialTags.PEER, fields["peer"]).orElse { return it }
    }
    if (fields.containsKey("header_autofills")) {
        result["header_autofills"] = loadByteMap(fields["header_autofills"])
    }
    return Ok(result)
}

private val sdkCodecsInstalled: Unit = run {
    registerWireValueCodec(
        wireValueCodec(
            SerialTags.INTERACTION,
            test = testSerializable(SerialTags.INTERACTION),
            dump = { value ->
                @Suppress("UNCHECKED_CAST")
                Ok(Fields().also { it.putAll(value as Map<String, Any?>) })
            },
            load = ::loadInteraction,
        ),
    )
    registerWireValueCodec(
        wireValueCodec(
            SerialTags.ACTION_CONFIG,
            test = testSerializable(SerialTags.ACTION_CONFIG),
            dump = { value ->
                @Suppress("UNCHECKED_CAST")
                Ok(Fields().also { it.putAll(value as Map<String, Any?>) })
            },
            load = ::loadActionConfig,
        ),
    )
    registerMapModel(SerialTags.PEER) { makePeer(it) }
    registerMapModel(SerialTags.USAGE_METADATA) { makeUsageMetadata(it) }
}

/** Force the SDK codecs to be installed; safe to call repeatedly. */
fun ensureInteractionCodec() {
    sdkCodecsInstalled
}

/** Regex patterns for the actions the LLM may invoke as tools. */
fun getAllowedLlmActionPatterns(action: Action): StatusOr<List<String>> {
    val header = action.getHeader(LlmHeaders.ALLOWED_LLM_ACTIONS.header).orElse { return it } ?: return Ok(emptyList())
    val decoded = utf8Decode(header).orElse { return it }
    return Ok(decoded.split(",").map { it.trim() }.filter { it.isNotEmpty() })
}

/** Whether an action name is fully matched by any allowed regex pattern. */
fun actionNameMatchesAllowed(name: String, patterns: List<String>): StatusOr<Boolean> {
    for (pattern in patterns) {
        val regex = try { Regex("^(?:$pattern)$") } catch (error: Throwable) {
            return invalidArgument("Allowed LLM action pattern \"$pattern\" is not a valid regular expression.")
        }
        if (regex.matches(name)) return Ok(true)
    }
    return Ok(false)
}
