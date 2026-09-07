/* Copyright 2026 The A11 Authors */

package a11

import java.util.Base64

const val AUTHORIZATION_HEADER = "x-a11-auth"
const val AUTHORIZE_ACTION = "__authorize__"
const val AUTHORIZATION_VERSION = 1
const val MAX_AUTHORIZATION_BYTES = 16 * 1024
const val MAX_AUTHORIZATION_HOPS = 8

data class AuthorizationEnvelope(
    val chain: List<String>,
    val version: Int = AUTHORIZATION_VERSION,
)

private fun validateAuthorization(value: AuthorizationEnvelope): Status {
    if (value.version != AUTHORIZATION_VERSION) {
        return invalidArgument("Unsupported A11 authorization version.")
    }
    if (value.chain.isEmpty() || value.chain.size > MAX_AUTHORIZATION_HOPS) {
        return invalidArgument("Invalid A11 authorization chain length.")
    }
    if (value.chain.any { statement ->
            statement.isEmpty() || statement.count { it == '.' } != 2
        }
    ) {
        return invalidArgument(
            "Every authorization statement must be compact JWS."
        )
    }
    return Status.ok()
}

fun encodeAuthorization(value: AuthorizationEnvelope): StatusOr<ByteArray> {
    validateAuthorization(value).let { if (!it.isOk) return it }
    val encoded = MsgpackCodec.encodeOne(listOf(value.version, value.chain))
    return if (encoded.size <= MAX_AUTHORIZATION_BYTES) Ok(encoded)
    else invalidArgument("The authorization value is too large.")
}

fun decodeAuthorization(value: ByteArray): StatusOr<AuthorizationEnvelope> {
    if (value.size > MAX_AUTHORIZATION_BYTES) {
        return invalidArgument("The authorization value is too large.")
    }
    val decoded = MsgpackCodec.decodeOne(value).orElse { return it }
    val fields = decoded as? List<*>
        ?: return invalidArgument(
            "The authorization envelope must be [1, chain]."
        )
    if (
        fields.size != 2 ||
        (fields[0] as? Number)?.toInt() != AUTHORIZATION_VERSION
    ) {
        return invalidArgument(
            "The authorization envelope must be [1, chain]."
        )
    }
    val chain = (fields[1] as? List<*>)?.map {
        it as? String
            ?: return invalidArgument(
                "The authorization chain must contain JWS strings."
            )
    } ?: return invalidArgument("The authorization chain must be a list.")
    val envelope = AuthorizationEnvelope(chain)
    validateAuthorization(envelope).let { if (!it.isOk) return it }
    val canonical = encodeAuthorization(envelope).orElse { return it }
    if (!canonical.contentEquals(value)) {
        return invalidArgument("The authorization envelope is not canonical.")
    }
    return Ok(envelope)
}

fun authorizationToText(
    value: AuthorizationEnvelope,
): StatusOr<String> {
    val encoded = encodeAuthorization(value).orElse { return it }
    val body = Base64.getUrlEncoder().withoutPadding().encodeToString(encoded)
    return Ok("a11-auth/1.$body")
}

fun authorizationFromText(value: String): StatusOr<AuthorizationEnvelope> {
    val prefix = "a11-auth/1."
    if (!value.startsWith(prefix)) return invalidArgument("Expected $prefix.")
    return try {
        val encoded = Base64.getUrlDecoder().decode(value.removePrefix(prefix))
        decodeAuthorization(encoded)
    } catch (_: IllegalArgumentException) {
        invalidArgument("The authorization value is not base64url.")
    }
}

fun getAuthorization(action: Action): StatusOr<AuthorizationEnvelope?> {
    val full = action.getHeader(AUTHORIZATION_HEADER).orElse { return it }
        ?: return Ok(null)
    val reference = action.getHeader(AUTHORIZATION_REFERENCE_HEADER)
        .orElse { return it }
    if (reference != null) {
        return invalidArgument(
            "An action cannot carry both authorization forms."
        )
    }
    return decodeAuthorization(full)
}

fun setAuthorizationHeader(
    action: Action,
    value: AuthorizationEnvelope?,
): Status {
    if (value == null) return action.removeHeader(AUTHORIZATION_HEADER)
    val encoded = encodeAuthorization(value).orElse { return it }
    action.setHeader(AUTHORIZATION_HEADER, encoded).let {
        if (!it.isOk) return it
    }
    return action.removeHeader(AUTHORIZATION_REFERENCE_HEADER)
}

fun setAuthorizationReference(action: Action, contextId: String?): Status {
    if (contextId == null) {
        return action.removeHeader(AUTHORIZATION_REFERENCE_HEADER)
    }
    val decoded = try {
        Base64.getUrlDecoder().decode(contextId)
    } catch (_: IllegalArgumentException) {
        return invalidArgument(
            "An authorization context ID must contain 128 bits."
        )
    }
    if (decoded.size != 16) {
        return invalidArgument(
            "An authorization context ID must contain 128 bits."
        )
    }
    action.setHeader(AUTHORIZATION_REFERENCE_HEADER, decoded).let {
        if (!it.isOk) return it
    }
    return action.removeHeader(AUTHORIZATION_HEADER)
}
