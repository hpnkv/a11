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

import java.util.concurrent.Executors
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.delay
import kotlinx.coroutines.withTimeoutOrNull

/** A small externally-completable promise used by the stackless pumps. */
class Deferred<T> {
    private val delegate = CompletableDeferred<T>()

    val settled: Boolean
        get() = delegate.isCompleted

    /** Complete once; repeated completion returns ALREADY_EXISTS. */
    fun resolve(value: T): Status =
        if (delegate.complete(value)) Status.ok()
        else alreadyExists("Promise has already been completed")

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
        if (timeoutMs == null) Ok(block())
        else withTimeoutOrNull(timeoutMs) { Ok(block()) } ?: deadlineExceeded(timeoutMessage)
    } catch (error: CancellationException) {
        throw error
    } catch (error: Exception) {
        Status.fromException(error)
    }
}

/**
 * Process-wide scheduling contexts for the runtime.
 *
 * [scope] drives the cooperative message pumps; [blockingDispatcher] uses a virtual-thread-per-task
 * executor so a blocking call parks a virtual thread rather than a platform carrier.
 */
object A11Runtime {
    private val virtualThreadExecutor = Executors.newVirtualThreadPerTaskExecutor()

    val blockingDispatcher = virtualThreadExecutor.asCoroutineDispatcher()

    val scope: CoroutineScope = CoroutineScope(SupervisorJob() + blockingDispatcher)
}
