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
import a11.invalidArgument
import a11.orElse
import a11.registerWireValueCodec
import a11.testSerializable
import a11.utf8Decode
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

/** Conversation roles. `model` is the assistant role, matching the Python SDK. */
enum class Role(val value: String) {
    SYSTEM("system"),
    ASSISTANT("model"),
    USER("user"),
}

/**
 * Portable, backend-independent record of one turn; JSON object on the wire.
 *
 * The map retains the `a11.sdk.Interaction` type tag through serialization, so it can be nested in
 * another value and accepted by strict interaction ports.
 */
class Interaction : LinkedHashMap<String, Any?>, A11Serializable {
    constructor() : super()

    constructor(source: Map<String, Any?>) : super(source)

    override val a11SerialTag: String
        get() = SerialTags.INTERACTION
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
    result["backend_specific_metadata"] =
        partial["backend_specific_metadata"] ?: emptyMap<String, Any?>()
    for ((k, v) in partial) if (!result.containsKey(k)) result[k] = v
    return result
}

/**
 * Build an interaction carrying a single text message.
 *
 * The content is the backend-neutral `{role, content: [text part]}` envelope, so a plain text turn
 * stays portable across a mid-conversation model switch.
 *
 * `content` and `system_instructions` contain [Chunk] values, as required by the
 * backend-independent interaction schema.
 */
fun makeTextMessageInteraction(
    text: String,
    systemPrompt: String = "",
    role: Role = Role.USER,
    registry: SerializationRegistry = SerializationRegistry.getGlobal(),
): StatusOr<Interaction> {
    if (role == Role.SYSTEM)
        return invalidArgument("A text message interaction cannot use the system role as content.")

    val roleStr = if (role == Role.ASSISTANT) "model" else "user"
    val content =
        registry
            .toChunk(
                linkedMapOf<String, Any?>(
                    "role" to roleStr,
                    "content" to listOf(linkedMapOf<String, Any?>("type" to "text", "text" to text)),
                )
            )
            .orElse { status ->
                return status
            }
    val instructions =
        if (systemPrompt.isEmpty()) emptyList()
        else {
            listOf(
                registry.toChunk(systemPrompt).orElse { status ->
                    return status
                }
            )
        }
    return Ok(
        makeInteraction(
            mapOf(
                "role" to role.value,
                "content" to listOf(content),
                "system_instructions" to instructions,
            )
        )
    )
}

/**
 * A model an interaction nests, held as a map that knows its own name.
 *
 * Kotlin has no field-by-field port of these — an interaction is a map here, and so is everything
 * inside it — but they still have to arrive at a Python peer as their own types rather than as
 * anonymous objects. Naming them is enough for that; the fields ride along untouched.
 */
class TaggedModel(override val a11SerialTag: String, source: Map<String, Any?> = emptyMap()) :
    LinkedHashMap<String, Any?>(source), A11Serializable

/** An `a11.sdk.Peer`: who should run an action. */
fun makePeer(fields: Map<String, Any?> = emptyMap()): TaggedModel =
    TaggedModel(SerialTags.PEER, fields)

/** An `a11.sdk.ActionConfig`: how one action of an interaction is run. */
fun makeActionConfig(fields: Map<String, Any?> = emptyMap()): TaggedModel =
    TaggedModel(SerialTags.ACTION_CONFIG, fields)

/** An `a11.sdk.UsageMetadata`: an interaction's token usage. */
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
                Ok(Fields().also { element -> element.putAll(value as Map<String, Any?>) })
            },
            load = { fields -> Ok(build(fields)) },
        )
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
    val codec =
        wireValueCodecByTag(tag)
            ?: return invalidArgument("No wire value codec is registered for $tag.")
    // An instance a caller built, or one a peer already sent us, is left alone.
    if (codec.test(value)) return Ok(value)
    if (value !is Map<*, *>) return invalidArgument("Expected the fields of a $tag.")
    return codec.load(value.entries.associate { item -> item.key.toString() to item.value })
}

private fun loadTaggedList(tag: String, value: Any?): StatusOr<List<Any?>> {
    val items = value as? List<*> ?: return invalidArgument("Expected an array.")
    val result = ArrayList<Any?>(items.size)
    for (item in items) result.add(
        loadTagged(tag, item).orElse { status ->
            return status
        }
    )
    return Ok(result)
}

private fun loadTaggedListMap(tag: String, value: Any?): StatusOr<Map<String, Any?>> {
    val entries = value as? Map<*, *> ?: return invalidArgument("Expected an object.")
    val result = LinkedHashMap<String, Any?>()
    for ((key, item) in entries) {
        result[key.toString()] =
            loadTaggedList(tag, item).orElse { status ->
                return status
            }
    }
    return Ok(result)
}

private fun loadTaggedMap(tag: String, value: Any?): StatusOr<Map<String, Any?>> {
    val entries = value as? Map<*, *> ?: return invalidArgument("Expected an object.")
    val result = LinkedHashMap<String, Any?>()
    for ((key, item) in entries) {
        result[key.toString()] =
            loadTagged(tag, item).orElse { status ->
                return status
            }
    }
    return Ok(result)
}

/** A `dict[str, bytes]` field, whose values the wire spells as base64. */
private fun loadByteMap(value: Any?): StatusOr<Map<String, ByteArray>> {
    val entries = value as? Map<*, *> ?: return invalidArgument("Expected a byte map.")
    val result = LinkedHashMap<String, ByteArray>()
    for ((key, item) in entries) {
        if (key !is String) return invalidArgument("Byte map keys must be strings.")
        result[key] =
            when (item) {
                is ByteArray -> item.copyOf()
                is String ->
                    base64Decode(item).orElse { status ->
                        return status
                    }
                else -> return invalidArgument("Byte map values must be bytes or base64 text.")
            }
    }
    return Ok(result)
}

/**
 * Rebuild an interaction from the bare tree a peer sent.
 *
 * Only fields that are actually present are rewritten, so an interaction that is decoded and sent
 * straight back is byte-identical to the one that arrived.
 */
private fun loadInteraction(fields: Map<String, Any?>): StatusOr<Any?> {
    val result = Interaction(fields)
    fun <T> put(key: String, load: (Any?) -> StatusOr<T>): Status {
        if (!fields.containsKey(key)) return Status.ok()
        result[key] =
            load(fields[key]).orElse { status ->
                return status
            }
        return Status.ok()
    }
    put("status") { value -> loadTagged(SerialTags.STATUS, value) }
        .orElse { status ->
            return status
        }
    put("content") { value -> loadTaggedList(SerialTags.CHUNK, value) }
        .orElse { status ->
            return status
        }
    put("system_instructions") { value -> loadTaggedList(SerialTags.CHUNK, value) }
        .orElse { status ->
            return status
        }
    put("action_calls") { value -> loadTaggedList(SerialTags.ACTION_MESSAGE, value) }
        .orElse { status ->
            return status
        }
    put("action_inputs") { value -> loadTaggedListMap(SerialTags.NODE_FRAGMENT, value) }
        .orElse { status ->
            return status
        }
    put("action_outputs") { value -> loadTaggedListMap(SerialTags.NODE_FRAGMENT, value) }
        .orElse { status ->
            return status
        }
    put("action_configs") { value -> loadTaggedMap(SerialTags.ACTION_CONFIG, value) }
        .orElse { status ->
            return status
        }
    put("usage_metadata") { value -> loadTagged(SerialTags.USAGE_METADATA, value) }
        .orElse { status ->
            return status
        }
    if (fields.containsKey("backend_specific_metadata")) {
        result["backend_specific_metadata"] =
            loadByteMap(fields["backend_specific_metadata"]).orElse { status ->
                return status
            }
    }
    return Ok(result)
}

/** An action config nests a peer, and carries a `dict[str, bytes]`. */
private fun loadActionConfig(fields: Map<String, Any?>): StatusOr<Any?> {
    val result = TaggedModel(SerialTags.ACTION_CONFIG, fields)
    if (fields.containsKey("peer") && fields["peer"] !is String) {
        result["peer"] =
            loadTagged(SerialTags.PEER, fields["peer"]).orElse { status ->
                return status
            }
    }
    if (fields.containsKey("header_autofills")) {
        result["header_autofills"] =
            loadByteMap(fields["header_autofills"]).orElse { status ->
                return status
            }
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
                Ok(Fields().also { element -> element.putAll(value as Map<String, Any?>) })
            },
            load = ::loadInteraction,
        )
    )
    registerWireValueCodec(
        wireValueCodec(
            SerialTags.ACTION_CONFIG,
            test = testSerializable(SerialTags.ACTION_CONFIG),
            dump = { value ->
                @Suppress("UNCHECKED_CAST")
                Ok(Fields().also { element -> element.putAll(value as Map<String, Any?>) })
            },
            load = ::loadActionConfig,
        )
    )
    registerMapModel(SerialTags.PEER) { fields -> makePeer(fields) }
    registerMapModel(SerialTags.USAGE_METADATA) { fields -> makeUsageMetadata(fields) }
}

/** Force the SDK codecs to be installed; safe to call repeatedly. */
fun ensureInteractionCodec() {
    sdkCodecsInstalled
}

/** Regex patterns for the actions the LLM may invoke as tools. */
fun getAllowedLlmActionPatterns(action: Action): StatusOr<List<String>> {
    val header =
        action.getHeader(LlmHeaders.ALLOWED_LLM_ACTIONS.header).orElse { status ->
            return status
        } ?: return Ok(emptyList())
    val decoded =
        utf8Decode(header).orElse { status ->
            return status
        }
    return Ok(decoded.split(",").map { value -> value.trim() }.filter { item -> item.isNotEmpty() })
}

/** Whether an action name is fully matched by any allowed regex pattern. */
fun actionNameMatchesAllowed(name: String, patterns: List<String>): StatusOr<Boolean> {
    for (pattern in patterns) {
        val regex =
            try {
                Regex("^(?:$pattern)$")
            } catch (error: IllegalArgumentException) {
                return invalidArgument(
                    "Allowed LLM action pattern \"$pattern\" is not a valid regular expression."
                )
            }
        if (regex.matches(name)) return Ok(true)
    }
    return Ok(false)
}
