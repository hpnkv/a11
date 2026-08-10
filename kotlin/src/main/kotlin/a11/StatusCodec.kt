package a11

/** Box used where a decoded (possibly non-OK) Status is itself the value. */
data class DecodedStatus(val status: Status)

/** Encode the native A11 concatenated-MessagePack Status layout (code, message, details). */
fun packStatus(status: Status): StatusOr<ByteArray> {
    val details: List<Any?> = status.details
    return MsgpackCodec.encodeFields(listOf(status.code.value, status.message, details))
}

/** Decode a Status while preserving an inner non-OK value as data. */
fun decodeStatus(bytes: ByteArray): StatusOr<DecodedStatus> {
    val fields = MsgpackCodec.decodeFields(bytes, 3, "Status").orElse { return it }
    val rawCode = fields[0]
    val message = fields[1]
    val rawDetails = fields[2]
    val codeInt = when (rawCode) {
        is Long -> rawCode.toInt()
        is Int -> rawCode
        else -> return invalidArgument("MessagePack does not contain a valid Status.")
    }
    val code = StatusCode.fromValue(codeInt)
        ?: return invalidArgument("MessagePack does not contain a valid Status.")
    if (message !is String) return invalidArgument("MessagePack does not contain a valid Status.")
    if (rawDetails != null && rawDetails !is List<*>) {
        return invalidArgument("MessagePack does not contain a valid Status.")
    }
    @Suppress("UNCHECKED_CAST")
    val details: List<Map<String, Any?>> = (rawDetails as? List<*>)?.map {
        (it as? Map<Any?, Any?>)?.entries?.associate { (k, v) -> k.toString() to v }
            ?: return invalidArgument("Every Status detail must be an object.")
    } ?: emptyList()
    return Ok(DecodedStatus(Status(code, message, details)))
}

/** Convenience decoder; malformed bytes are themselves returned as a Status. */
fun unpackStatus(bytes: ByteArray): Status = when (val decoded = decodeStatus(bytes)) {
    is Ok -> decoded.value.status
    is Status -> decoded
}
