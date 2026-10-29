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

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertSame
import kotlin.test.assertTrue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.withTimeout

class ConcurrencyTest {
    @Test
    fun deadlinesAndNullableSuccessHaveDifferentOutcomes() = runTest {
        assertEquals(Ok(null), waitFor(10) { null })
        assertEquals(StatusCode.DEADLINE_EXCEEDED, waitFor(10) { delay(20) }.status.code)
        assertEquals(StatusCode.INVALID_ARGUMENT, waitFor(-1) { error("must not run") }.status.code)
        assertEquals(StatusCode.DEADLINE_EXCEEDED, waitFor(0) { error("must not run") }.status.code)
        assertFailsWith<CancellationException> { waitFor(100) { withTimeout(5) { delay(10) } } }
    }

    @Test
    fun parentCancellationEscapesAndReleasesTheWait() = runTest {
        var released = false
        val waiting =
            async(start = CoroutineStart.UNDISPATCHED) {
                waitFor(null) {
                    try {
                        awaitCancellation()
                    } finally {
                        released = true
                    }
                }
            }
        waiting.cancelAndJoin()
        assertTrue(waiting.isCancelled)
        assertTrue(released)
        val cancellation = CancellationException("parent")
        assertSame(
            cancellation,
            assertFailsWith<CancellationException> { noexcept<Int> { throw cancellation } },
        )
        assertFailsWith<AssertionError> { noexcept<Int> { throw AssertionError("invariant") } }
    }

    @Test
    fun jsonEncodingPreservesCancellationFromApplicationCollections() {
        val cancellation = CancellationException("collection cancelled")
        val map =
            object : AbstractMap<String, Any?>() {
                override val entries: Set<Map.Entry<String, Any?>>
                    get() = throw cancellation
            }
        assertSame(
            cancellation,
            assertFailsWith<CancellationException> { A11Json.encodeToString(map) },
        )
    }

    @Test
    fun exceptionAdaptersKeepAnExistingOperationalError() = runTest {
        val original = dataLoss("disk failed")
        assertSame(original, waitFor(null) { throw StatusException(original) }.status)
        assertSame(original, noexcept<Int> { throw StatusException(original) }.status)
        val deferred = Deferred<Int>()
        deferred.resolve(1)
        assertEquals(StatusCode.ALREADY_EXISTS, deferred.resolve(2).code)
        assertTrue(deferred.settled)
        assertEquals(1, deferred.await())
    }
}
