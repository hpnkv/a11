package a11

/**
 * Portable outcome codes used across actions, stores, transports, and HTTP.
 *
 * A11 returns these structured outcomes from expected operational failures so an
 * agent can preserve the same meaning across process and language boundaries.
 * This is the Kotlin counterpart of `js/src/status.ts` and `a11/status.py`; the
 * integer [value] of each code is wire-compatible with both.
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

/**
 * Structured success or failure carried across A11 boundaries.
 *
 * Unlike [StatusOr], a bare [Status] is used where an operation has no return
 * value; [isOk] then reports whether it succeeded. Where an operation yields a
 * value, callers receive a [StatusOr] whose success case is [Ok].
 */
data class Status(
    val code: StatusCode,
    val message: String,
    val details: List<Map<String, Any?>> = emptyList(),
    val cause: Throwable? = null,
) : StatusOr<Nothing> {
    val isOk: Boolean get() = code == StatusCode.OK

    /** Throw a [StatusException] when this status is not OK. */
    fun raiseIfNotOk() {
        if (!isOk) throw StatusException(this)
    }

    override fun toString(): String = "[${code.name}] $message"

    companion object {
        /** Construct an OK status, optionally with a more specific message. */
        fun ok(message: String = "OK"): Status = Status(StatusCode.OK, message)

        /** Build a status from a caught exception, preserving an A11 status cause. */
        fun fromException(
            error: Throwable,
            message: String? = null,
            code: StatusCode = StatusCode.UNKNOWN,
        ): Status {
            if (error is StatusException) return error.status
            val effective = if (code == StatusCode.OK) StatusCode.UNKNOWN else code
            return Status(effective, message ?: (error.message ?: "Unknown error."), cause = error)
        }
    }
}

/**
 * A direct value on success ([Ok]), or a structured non-OK [Status] on failure.
 *
 * This mirrors the TypeScript `StatusOr<T>` union but wraps success in [Ok] so
 * the two cases are always statically distinguishable in Kotlin.
 */
sealed interface StatusOr<out T>

/** Successful [StatusOr] carrying a value. */
data class Ok<out T>(val value: T) : StatusOr<T>

/** Return whether a [StatusOr]/[Status] represents success. */
fun StatusOr<*>.isOk(): Boolean = when (this) {
    is Ok -> true
    is Status -> code == StatusCode.OK
}

/** Return the non-OK [Status], or null when this holds a value / is OK. */
fun StatusOr<*>.errorOrNull(): Status? = when (this) {
    is Ok -> null
    is Status -> if (isOk) null else this
}

/**
 * Unwrap a success value or run [onError] (which must not return normally).
 *
 * The idiomatic early-return pattern mirrors `if (!isOk(x)) return x` from the
 * TypeScript source: `val node = nodeMap.get(id).orElse { return it }`.
 */
inline fun <T> StatusOr<T>.orElse(onError: (Status) -> Nothing): T = when (this) {
    is Ok -> value
    is Status -> onError(this)
}

/** Unwrap a success value or null on failure. */
@Suppress("UNCHECKED_CAST")
fun <T> StatusOr<T>.valueOrNull(): T? = (this as? Ok<T>)?.value

/** Unwrap a success value or throw the code-specific [StatusException]. */
fun <T> StatusOr<T>.valueOrThrow(): T = when (this) {
    is Ok -> value
    is Status -> throw StatusException(this)
}

/** Kotlin exception carrying the original structured A11 [Status]. */
class StatusException(val status: Status) :
    RuntimeException(status.message, status.cause) {
    init {
        require(!status.isOk) { "StatusException cannot be created for an OK status." }
    }
}

/** Run a block, converting any thrown exception into a non-OK [Status]. */
inline fun <T> noexcept(message: String? = null, block: () -> StatusOr<T>): StatusOr<T> =
    try {
        block()
    } catch (error: Throwable) {
        Status.fromException(error, message)
    }

// --- Code-specific constructors (parity with js/src/status.ts) ---------------

fun cancelled(message: String = "Cancelled"): Status = Status(StatusCode.CANCELLED, message)
fun unknown(message: String = "Unknown"): Status = Status(StatusCode.UNKNOWN, message)
fun invalidArgument(message: String = "Invalid Argument"): Status =
    Status(StatusCode.INVALID_ARGUMENT, message)
fun deadlineExceeded(message: String = "Deadline Exceeded"): Status =
    Status(StatusCode.DEADLINE_EXCEEDED, message)
fun notFound(message: String = "Not Found"): Status = Status(StatusCode.NOT_FOUND, message)
fun alreadyExists(message: String = "Already Exists"): Status =
    Status(StatusCode.ALREADY_EXISTS, message)
fun permissionDenied(message: String = "Permission Denied"): Status =
    Status(StatusCode.PERMISSION_DENIED, message)
fun resourceExhausted(message: String = "Resource Exhausted"): Status =
    Status(StatusCode.RESOURCE_EXHAUSTED, message)
fun failedPrecondition(message: String = "Failed Precondition"): Status =
    Status(StatusCode.FAILED_PRECONDITION, message)
fun aborted(message: String = "Aborted"): Status = Status(StatusCode.ABORTED, message)
fun outOfRange(message: String = "Out of Range"): Status = Status(StatusCode.OUT_OF_RANGE, message)
fun unimplemented(message: String = "Unimplemented"): Status =
    Status(StatusCode.UNIMPLEMENTED, message)
fun internal(message: String = "Internal"): Status = Status(StatusCode.INTERNAL, message)
fun unavailable(message: String = "Unavailable"): Status = Status(StatusCode.UNAVAILABLE, message)
fun dataLoss(message: String = "Data Loss"): Status = Status(StatusCode.DATA_LOSS, message)
fun unauthenticated(message: String = "Unauthenticated"): Status =
    Status(StatusCode.UNAUTHENTICATED, message)

// --- HTTP / WebSocket mappings (used by the transport layer) -----------------

/** Map an HTTP response code to the nearest portable A11 outcome. */
fun statusCodeFromHttp(httpCode: Int): StatusCode = when {
    httpCode in 200..299 -> StatusCode.OK
    httpCode == 400 -> StatusCode.INVALID_ARGUMENT
    httpCode == 401 -> StatusCode.UNAUTHENTICATED
    httpCode == 403 -> StatusCode.PERMISSION_DENIED
    httpCode == 404 -> StatusCode.NOT_FOUND
    httpCode == 409 -> StatusCode.ABORTED
    httpCode == 429 -> StatusCode.RESOURCE_EXHAUSTED
    httpCode == 501 -> StatusCode.UNIMPLEMENTED
    httpCode == 503 -> StatusCode.UNAVAILABLE
    httpCode in 400..499 -> StatusCode.FAILED_PRECONDITION
    httpCode in 500..599 -> StatusCode.INTERNAL
    else -> StatusCode.UNKNOWN
}

/** Decode standard and A11-private WebSocket close codes. */
fun statusCodeFromWebSocket(closeCode: Int): StatusCode {
    val privateCode = closeCode - 3999
    if (closeCode == 1000) return StatusCode.OK
    if (privateCode in 1..15) return StatusCode.fromValue(privateCode) ?: StatusCode.UNKNOWN
    if (closeCode == 4007) return StatusCode.UNAUTHENTICATED
    return when (closeCode) {
        1001 -> StatusCode.ABORTED
        1002, 1003, 1007 -> StatusCode.INVALID_ARGUMENT
        1008 -> StatusCode.PERMISSION_DENIED
        1009 -> StatusCode.RESOURCE_EXHAUSTED
        1011 -> StatusCode.INTERNAL
        1012, 1013 -> StatusCode.UNAVAILABLE
        else -> StatusCode.UNKNOWN
    }
}

/** Encode a portable status as a standard or A11-private WebSocket close code. */
fun statusCodeToWebSocket(code: StatusCode): Int = when {
    code == StatusCode.OK -> 1000
    code.value in StatusCode.CANCELLED.value..StatusCode.DATA_LOSS.value -> 3999 + code.value
    code == StatusCode.UNAUTHENTICATED -> 4007
    else -> 4001
}
