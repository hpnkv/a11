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
@file:OptIn(kotlin.time.ExperimentalTime::class)

package a11

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue
import kotlin.test.fail
import kotlin.time.Duration.Companion.minutes
import kotlin.time.Duration.Companion.nanoseconds
import kotlin.time.Duration.Companion.seconds
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout

class TimeTest {
    @Test
    fun kotlinDurationsRoundTripWithoutMillisecondConversion() {
        for (duration in
            listOf(
                kotlin.time.Duration.ZERO,
                1.nanoseconds,
                (-1).nanoseconds,
                2.minutes + 123.nanoseconds,
                -2.minutes - 123.nanoseconds,
                kotlin.time.Duration.INFINITE,
                -kotlin.time.Duration.INFINITE,
            )) {
            assertEquals(duration, verified(Duration.of(duration).toKotlinDuration()))
        }
        assertEquals(123L, verified(Duration.nanoseconds(123).nanoseconds()))
    }

    @Test
    fun abseilPrecisionAndRangeAreRetainedUntilAnExplicitCheckedConversion() {
        val fraction = verified(Duration.parse("1.25ns"))
        assertEquals("1.25ns", fraction.toString())
        assertEquals(StatusCode.OUT_OF_RANGE, fraction.nanoseconds().status.code)
        assertEquals(StatusCode.OUT_OF_RANGE, fraction.toKotlinDuration().status.code)
        val largestNanos = Duration.nanoseconds(Long.MAX_VALUE)
        assertEquals(Long.MAX_VALUE, verified(largestNanos.nanoseconds()))
        assertEquals(StatusCode.OUT_OF_RANGE, largestNanos.toKotlinDuration().status.code)
        val largestSeconds = Duration.seconds(Long.MAX_VALUE)
        assertEquals(StatusCode.OUT_OF_RANGE, largestSeconds.toKotlinDuration().status.code)
        assertEquals(Duration.infinite(), largestSeconds + Duration.seconds(1))
        assertEquals(StatusCode.INVALID_ARGUMENT, Duration.parse("not a duration").status.code)
    }

    @Test
    fun nativeArithmeticHasValueEqualityHashingAndInfiniteBounds() {
        val minute = verified(Duration.parse("1m"))
        val sixty = Duration.seconds(60)
        assertEquals(minute, sixty)
        assertEquals(minute.hashCode(), sixty.hashCode())
        assertEquals(Duration.seconds(120), minute * 2)
        assertEquals(Duration.seconds(30), minute / 2)
        assertEquals(Duration.seconds(-60), -minute)
        val epoch = Time.unixEpoch()
        val later = epoch + 2.seconds + Duration.nanoseconds(1)
        assertEquals(Duration.seconds(2) + Duration.nanoseconds(1), later - epoch)
        assertEquals(epoch, later - Duration.seconds(2) - 1.nanoseconds)
        assertTrue(Time.infinitePast() < epoch)
        assertTrue(epoch < Time.infiniteFuture())
        assertEquals(
            kotlin.time.Duration.INFINITE,
            verified((Time.infiniteFuture() - Time.infinitePast()).toKotlinDuration()),
        )
    }

    @Test
    fun instantsConvertExactlyAcrossTheEpochAndBeyondInt64Nanoseconds() {
        for (text in
            listOf(
                "1969-12-31T23:59:59.999999999Z",
                "1970-01-01T00:00:00Z",
                "2500-01-01T00:00:00.123456789Z",
            )) {
            val instant = kotlin.time.Instant.parse(text)
            val native = Time.fromKotlinInstant(instant)
            assertEquals(instant, verified(native.toKotlinInstant()))
            val parsed = verified(Time.parse(text))
            assertEquals(native, parsed)
            assertEquals(native.hashCode(), parsed.hashCode())
        }
        assertEquals(
            StatusCode.OUT_OF_RANGE,
            verified(Time.parse("2500-01-01T00:00:00Z")).nanosecondsSinceEpoch().status.code,
        )
        assertEquals(StatusCode.OUT_OF_RANGE, Time.infiniteFuture().toKotlinInstant().status.code)
        assertEquals(StatusCode.OUT_OF_RANGE, Time.infinitePast().toKotlinInstant().status.code)
        val quarter = Time.unixEpoch() + verified(Duration.parse("0.25ns"))
        assertEquals(StatusCode.OUT_OF_RANGE, quarter.toKotlinInstant().status.code)
        assertEquals(StatusCode.INVALID_ARGUMENT, Time.parse("not an instant").status.code)
        assertEquals(Time.unixEpoch(), verified(Time.parse("1970-01-01T01:00:00+01:00")))
    }

    @Test
    fun typedTimeoutsReachNativeOperationsAtNanosecondPrecision() = runBlocking {
        withTimeout(5000) {
            val store = verified(LocalChunkStore.create("typed-timeout"))
            try {
                assertEquals(
                    StatusCode.DEADLINE_EXCEEDED,
                    store.get(0, timeout = 1.nanoseconds).status.code,
                )
                val native = verified(Duration.parse("0.25ns"))
                assertEquals(
                    StatusCode.DEADLINE_EXCEEDED,
                    store.get(0, timeout = native).status.code,
                )
                val fragment =
                    NodeFragment("typed-timeout", Chunk(data = utf8Encode("ready")), 0u, false)
                verified(store.put(fragment))
                assertEquals(
                    fragment,
                    verified(store.get(0, timeout = kotlin.time.Duration.INFINITE)),
                )
            } finally {
                store.release()
            }
        }
    }

    @Test
    fun malformedNativeTimeFieldsFailAsStatuses() {
        val result = a11.internal.NativeTime.timeValue(0, -1, 0)
        assertEquals(StatusCode.INVALID_ARGUMENT, result.status.code)
    }
}

private fun <T> verified(result: StatusOr<T>): T =
    result.orElse { status -> fail(status.toString()) }
