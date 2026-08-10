package a11

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.delay
import kotlinx.coroutines.withTimeout
import java.util.concurrent.Executors

/**
 * Coroutine equivalents of the primitives in `js/src/concurrency.ts`.
 *
 * The TypeScript runtime is single-threaded and drives its state machines with
 * microtasks; the Kotlin port runs the same pumps as coroutines. Blocking edges
 * (WebSocket I/O, IDE tool handlers) are pushed onto Java 21 virtual threads via
 * [A11Runtime.blockingScope] so the cooperative pumps never stall a carrier.
 */

/** A small externally-completable promise used by the stackless pumps. */
class Deferred<T> {
    private val delegate = CompletableDeferred<T>()

    val settled: Boolean get() = delegate.isCompleted

    /** Resolve once; later resolutions are ignored, matching the TS semantics. */
    fun resolve(value: T): Status {
        delegate.complete(value)
        return Status.ok()
    }

    suspend fun await(): T = delegate.await()
}

/** Yield to the scheduler for at least [ms] milliseconds. */
suspend fun sleep(ms: Long): Status {
    if (ms < 0) return invalidArgument("Sleep duration must be non-negative.")
    delay(ms)
    return Status.ok()
}

/** Await a value with an optional timeout, mapping expiry to DEADLINE_EXCEEDED. */
suspend fun <T> waitFor(
    timeoutMs: Long?,
    timeoutMessage: String = "Operation exceeded its deadline.",
    block: suspend () -> T,
): StatusOr<T> {
    if (timeoutMs != null && timeoutMs < 0) {
        return invalidArgument("timeoutMs must be a non-negative finite number.")
    }
    return try {
        if (timeoutMs == null) Ok(block()) else Ok(withTimeout(timeoutMs) { block() })
    } catch (error: TimeoutCancellationException) {
        deadlineExceeded(timeoutMessage)
    } catch (error: Throwable) {
        Status.fromException(error)
    }
}

/**
 * Process-wide scheduling contexts for the runtime.
 *
 * [scope] drives the cooperative message pumps; [blockingScope] uses a
 * virtual-thread-per-task executor so a blocking call parks a virtual thread
 * rather than a platform carrier.
 */
object A11Runtime {
    private val virtualThreadExecutor =
        Executors.newVirtualThreadPerTaskExecutor()

    val blockingDispatcher = virtualThreadExecutor.asCoroutineDispatcher()

    val scope: CoroutineScope = CoroutineScope(SupervisorJob() + blockingDispatcher)
}
