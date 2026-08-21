package a11

import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets
import java.util.Base64
import java.util.UUID

/** Owned binary metadata map used for headers and chunk attributes. */
typealias ByteMap = LinkedHashMap<String, ByteArray>

private val strictDecoder
    get() = StandardCharsets.UTF_8.newDecoder()
        .onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT)

/** Encode application/header text as UTF-8 bytes. */
fun utf8Encode(value: String): ByteArray = value.toByteArray(StandardCharsets.UTF_8)

/** Decode strict UTF-8, returning [invalidArgument] instead of replacement text. */
fun utf8Decode(value: ByteArray): StatusOr<String> = try {
    Ok(strictDecoder.decode(ByteBuffer.wrap(value)).toString())
} catch (error: Throwable) {
    invalidArgument("Byte data is not valid UTF-8.")
}

/** Join packet or serialized-record pieces into one owned byte array. */
fun concatBytes(parts: List<ByteArray>): ByteArray {
    val total = parts.sumOf { it.size }
    val result = ByteArray(total)
    var offset = 0
    for (part in parts) {
        part.copyInto(result, offset)
        offset += part.size
    }
    return result
}

/** Encode binary A11 values for JSON-only metadata channels. */
fun base64Encode(bytes: ByteArray): String = Base64.getEncoder().encodeToString(bytes)

/** Decode canonical base64 and reject malformed input. */
fun base64Decode(value: String): StatusOr<ByteArray> = try {
    Ok(Base64.getDecoder().decode(value))
} catch (error: IllegalArgumentException) {
    invalidArgument("Value is not valid base64.")
}

/** Deep-copy a byte map so caller mutation cannot change retained headers. */
fun copyByteMap(values: Map<String, ByteArray>): ByteMap {
    val result = ByteMap()
    for ((key, value) in values) result[key] = value.copyOf()
    return result
}

/**
 * Copy a map boundary into an owned byte map with optional per-key validation.
 * String values are UTF-8 encoded; ByteArray values are copied.
 */
fun normalizeByteMap(
    values: Map<String, Any>?,
    validateKey: ((String) -> Boolean)? = null,
): StatusOr<ByteMap> {
    val result = ByteMap()
    if (values == null) return Ok(result)
    for ((key, source) in values) {
        if (validateKey != null && !validateKey(key)) {
            return invalidArgument("Invalid byte-map key: $key.")
        }
        val bytes = when (source) {
            is ByteArray -> source.copyOf()
            is String -> utf8Encode(source)
            else -> return invalidArgument("Expected bytes or a string for key $key.")
        }
        result[key] = bytes
    }
    return Ok(result)
}

/** Hex digits in a generated id: 48 random bits, matching C++ `NewShortId`. */
private const val ID_HEX_DIGITS = 12

/**
 * Generate a validated-name-friendly id for actions, nodes, sessions, or streams.
 *
 * Twelve hex digits, as [a11.uuid.NewShortId] produces on the native side. Every
 * character is a hex digit, so a node id stays `<action id>#<port>` with exactly
 * one `#` for [Session] to split on.
 */
fun randomId(prefix: String): String {
    val uuid = UUID.randomUUID()
    val value = uuid.mostSignificantBits ushr (64 - 4 * ID_HEX_DIGITS)
    return prefix + String.format("%0${ID_HEX_DIGITS}x", value)
}
