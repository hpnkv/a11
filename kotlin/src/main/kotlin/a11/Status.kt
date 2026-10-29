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

/**
 * Portable outcome codes used across actions, stores, transports, and HTTP.
 *
 * A11 returns these structured outcomes from expected operational failures so an agent can preserve
 * the same meaning across process and language boundaries. This is the Kotlin counterpart of
 * `js/src/status.ts` and `a11/status.py`; the integer [value] of each code is wire-compatible with
 * both.
 */
enum class StatusCode(val value: Int) {
    /** The operation completed successfully. */
    OK(0),
    /** The caller or owning task cancelled the operation. */
    CANCELLED(1),
    /** No more specific failure code is known. */
    UNKNOWN(2),
    /** Input is invalid regardless of current runtime state. */
    INVALID_ARGUMENT(3),
    /** The operation did not finish before its deadline. */
    DEADLINE_EXCEEDED(4),
    /** The requested entity does not exist. */
    NOT_FOUND(5),
    /** The entity being created already exists. */
    ALREADY_EXISTS(6),
    /** The authenticated caller lacks permission. */
    PERMISSION_DENIED(7),
    /** A bounded queue, quota, or other resource is exhausted. */
    RESOURCE_EXHAUSTED(8),
    /** Runtime state must change before this operation is valid. */
    FAILED_PRECONDITION(9),
    /** The operation was aborted, commonly due to a conflict. */
    ABORTED(10),
    /** A requested offset or value lies outside its valid range. */
    OUT_OF_RANGE(11),
    /** This endpoint does not implement the requested operation. */
    UNIMPLEMENTED(12),
    /** An A11 invariant or implementation failed. */
    INTERNAL(13),
    /** A transient service or transport is unavailable. */
    UNAVAILABLE(14),
    /** Data was corrupted or irrecoverably lost. */
    DATA_LOSS(15),
    /** The caller's identity could not be established. */
    UNAUTHENTICATED(16);

    companion object {
        private val byValue = entries.associateBy { it.value }

        /** Return the code for a wire integer, or null when out of range. */
        fun fromValue(value: Int): StatusCode? = byValue[value]
    }
}

/** Canonical outcome with optional structured A11 error details. */
sealed class Status
protected constructor(
    val code: StatusCode = StatusCode.OK,
    message: String = "",
    details: List<Any?> = emptyList(),
    cause: Throwable? = null,
) {
    fun ok(): Boolean = code == StatusCode.OK

    val cause: Throwable? = if (ok()) null else cause
    val message: String = if (ok()) "" else message
    val details: List<Any?> =
        if (ok()) emptyList()
        else java.util.Collections.unmodifiableList(details.map { value -> freezeDetail(value) })

    /** Continue after success, or explicitly propagate a statically non-OK status. */
    inline fun orElse(onError: (status: Err) -> Nothing): Status {
        if (this is Err) onError(this)
        return this
    }

    /** Retain the first error, following Abseil Status.Update with an immutable value. */
    fun updated(next: Status): Status {
        orElse { status ->
            return status
        }
        return next
    }

    /** Explicit exception adapter for application boundaries. */
    fun raiseIfNotOk() {
        orElse { status -> throw StatusException(status) }
    }

    /** Copy the value, applying canonical OK construction to updated fields. */
    fun copy(
        code: StatusCode = this.code,
        message: String = this.message,
        details: List<Any?> = this.details,
        cause: Throwable? = this.cause,
    ): Status = Status.invoke(code, message, details, cause)

    override fun equals(other: Any?): Boolean =
        other is Status &&
            code == other.code &&
            message == other.message &&
            details == other.details

    override fun hashCode(): Int =
        31 * (31 * code.hashCode() + message.hashCode()) + details.hashCode()

    override fun toString(): String =
        if (ok()) "OK"
        else
            buildString {
                append("${code.name}: $message")
                if (details.isNotEmpty()) append(" [details=$details]")
            }

    companion object {
        /** Canonical construction: only error statuses can be error results. */
        operator fun invoke(
            code: StatusCode = StatusCode.OK,
            message: String = "",
            details: List<Any?> = emptyList(),
            cause: Throwable? = null,
        ): Status =
            if (code == StatusCode.OK) SuccessfulStatus else Err(code, message, details, cause)

        /** OK statuses discard messages, details, and causes, as Abseil does. */
        fun ok(): Status = SuccessfulStatus

        /** Convert an application exception, preserving cancellation and JVM errors. */
        fun fromException(
            error: Throwable,
            message: String? = null,
            code: StatusCode = StatusCode.UNKNOWN,
        ): Err {
            if (error is java.util.concurrent.CancellationException) throw error
            if (error is Error) throw error
            if (error is StatusException) return error.status
            val effective = if (code == StatusCode.OK) StatusCode.UNKNOWN else code
            return Err(effective, message ?: (error.message ?: "Unknown error."), cause = error)
        }
    }
}

private fun freezeDetail(value: Any?): Any? =
    when (value) {
        is Map<*, *> ->
            java.util.Collections.unmodifiableMap(
                value.mapValues { entry -> freezeDetail(entry.value) }
            )
        is List<*> ->
            java.util.Collections.unmodifiableList(value.map { value -> freezeDetail(value) })
        is Byte -> value.toLong()
        is Short -> value.toLong()
        is Int -> value.toLong()
        is UByte -> value.toLong()
        is UShort -> value.toLong()
        is UInt -> value.toLong()
        is ULong ->
            if (value <= Long.MAX_VALUE.toULong()) value.toLong()
            else java.math.BigInteger(value.toString())
        is Float -> value.toDouble()
        else -> value
    }

/** A value (including null) or a non-OK status; these cases never overlap. */
sealed interface StatusOr<out T> {
    val status: Status

    fun ok(): Boolean
}

/** Successful result. JVM references retain their ordinary identity and lifetime. */
data class Ok<out T>(val value: T) : StatusOr<T> {
    override fun ok(): Boolean = true

    override val status: Status
        get() = Status.ok()
}

private object SuccessfulStatus : Status()

/** A non-OK status is also an error result; no propagation wrapper is required. */
class Err
internal constructor(
    code: StatusCode,
    message: String = "",
    details: List<Any?> = emptyList(),
    cause: Throwable? = null,
) : Status(code, message, details, cause), StatusOr<Nothing> {
    override val status: Err
        get() = this

    init {
        if (code == StatusCode.OK)
            fatal("An OK status is not a valid constructor argument to StatusOr<T>")
    }

    companion object {
        /** Explicitly require an error when a general status must become a result. */
        operator fun invoke(status: Status): Err =
            when (status) {
                is Err -> status
                else -> fatal("An OK status is not a valid constructor argument to StatusOr<T>")
            }
    }
}

/** JVM termination for violated A11 invariants. */
internal fun fatal(message: String): Nothing {
    try {
        System.err.println("FATAL: $message")
        Throwable("A11 fatal check").printStackTrace(System.err)
        System.err.flush()
    } finally {
        Runtime.getRuntime().halt(1)
    }
    throw AssertionError("Runtime.halt returned")
}

/** Unwrap a value, or explicitly handle/propagate its operational error. */
inline fun <T> StatusOr<T>.orElse(onError: (status: Err) -> Nothing): T =
    when (this) {
        is Ok -> value
        is Err -> onError(status)
    }

/** Read the value; invalid access terminates the JVM, including with exceptions enabled. */
fun <T> StatusOr<T>.value(): T =
    when (this) {
        is Ok -> value
        is Err ->
            fatal("StatusOr.value(): Attempting to fetch value instead of handling error $status")
    }

/** Transform a value, retaining an error unchanged. */
inline fun <T, R> StatusOr<T>.map(transform: (T) -> R): StatusOr<R> =
    when (this) {
        is Ok -> Ok(transform(value))
        is Err -> this
    }

/** Chain a fallible operation, retaining an error unchanged. */
inline fun <T, R> StatusOr<T>.flatMap(transform: (T) -> StatusOr<R>): StatusOr<R> =
    when (this) {
        is Ok -> transform(value)
        is Err -> this
    }

/** Kotlin exception carrying the original structured A11 status. */
class StatusException(status: Status) : RuntimeException(status.message, status.cause) {
    val status: Err = Err(status)
}

/** Adapt application exceptions, preserving coroutine cancellation and JVM errors. */
inline fun <T> noexcept(message: String? = null, block: () -> StatusOr<T>): StatusOr<T> =
    try {
        block()
    } catch (error: Exception) {
        Status.fromException(error, message)
    }

// --- Code-specific constructors (parity with js/src/status.ts) ---------------

fun cancelled(message: String = "Cancelled"): Err = Err(StatusCode.CANCELLED, message)

fun unknown(message: String = "Unknown"): Err = Err(StatusCode.UNKNOWN, message)

fun invalidArgument(message: String = "Invalid Argument"): Err =
    Err(StatusCode.INVALID_ARGUMENT, message)

fun deadlineExceeded(message: String = "Deadline Exceeded"): Err =
    Err(StatusCode.DEADLINE_EXCEEDED, message)

fun notFound(message: String = "Not Found"): Err = Err(StatusCode.NOT_FOUND, message)

fun alreadyExists(message: String = "Already Exists"): Err = Err(StatusCode.ALREADY_EXISTS, message)

fun permissionDenied(message: String = "Permission Denied"): Err =
    Err(StatusCode.PERMISSION_DENIED, message)

fun resourceExhausted(message: String = "Resource Exhausted"): Err =
    Err(StatusCode.RESOURCE_EXHAUSTED, message)

fun failedPrecondition(message: String = "Failed Precondition"): Err =
    Err(StatusCode.FAILED_PRECONDITION, message)

fun aborted(message: String = "Aborted"): Err = Err(StatusCode.ABORTED, message)

fun outOfRange(message: String = "Out of Range"): Err = Err(StatusCode.OUT_OF_RANGE, message)

fun unimplemented(message: String = "Unimplemented"): Err = Err(StatusCode.UNIMPLEMENTED, message)

fun internal(message: String = "Internal"): Err = Err(StatusCode.INTERNAL, message)

fun unavailable(message: String = "Unavailable"): Err = Err(StatusCode.UNAVAILABLE, message)

fun dataLoss(message: String = "Data Loss"): Err = Err(StatusCode.DATA_LOSS, message)

fun unauthenticated(message: String = "Unauthenticated"): Err =
    Err(StatusCode.UNAUTHENTICATED, message)

// --- HTTP / WebSocket mappings (used by the transport layer) -----------------

fun statusCodeFromHttp(httpCode: Int): StatusCode =
    StatusCode.fromValue(a11.internal.NativeControl.statusCodeFromHttp(httpCode))
        ?: fatal("Native HTTP status mapping returned an invalid code")

fun statusCodeFromWebSocket(closeCode: Int): StatusCode =
    StatusCode.fromValue(a11.internal.NativeControl.statusCodeFromWebSocket(closeCode))
        ?: fatal("Native WebSocket status mapping returned an invalid code")

fun statusCodeToHttp(code: StatusCode): Int =
    a11.internal.NativeControl.statusCodeToHttp(code.value)

fun statusCodeToWebSocket(code: StatusCode): Int =
    a11.internal.NativeControl.statusCodeToWebSocket(code.value)
