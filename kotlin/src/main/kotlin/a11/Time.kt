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

import a11.internal.NativeBridge
import a11.internal.NativeTime
import a11.internal.nativeText
import kotlin.time.Duration.Companion.nanoseconds
import kotlin.time.Duration.Companion.seconds

/** Preserve all representable Kotlin duration components at the native boundary. */
internal fun durationFields(value: kotlin.time.Duration): Map<String, Any?> =
    if (value.isInfinite()) mapOf("infinity" to if (value.isNegative()) -1 else 1)
    else
        value.toComponents { seconds, nanoseconds ->
            mapOf("seconds" to seconds, "nanoseconds" to nanoseconds)
        }

/** Abseil duration, retaining its full range, infinities, and quarter-nanosecond ticks. */
class Duration private constructor(id: Long) : NativeObject(id), Comparable<Duration> {
    fun nanoseconds(): StatusOr<Long> = NativeTime.durationNanosecondCount(this)

    /** Convert exactly; report precision or range loss instead of rounding. */
    fun toKotlinDuration(): StatusOr<kotlin.time.Duration> {
        val parts =
            NativeTime.durationParts(this).orElse { status ->
                return status
            }
        if (parts.infinity != 0)
            return Ok(
                if (parts.infinity < 0) -kotlin.time.Duration.INFINITE
                else kotlin.time.Duration.INFINITE
            )
        val converted = parts.seconds.seconds + parts.nanoseconds.nanoseconds
        if (converted.isInfinite()) return outOfRange("Duration exceeds Kotlin's finite range")
        val exact =
            converted.toComponents { seconds, nanoseconds ->
                seconds == parts.seconds && nanoseconds == parts.nanoseconds
            }
        if (!exact) return outOfRange("Duration loses precision in Kotlin's representation")
        return Ok(converted)
    }

    operator fun plus(other: Duration): Duration = Duration(NativeTime.durationAdd(this, other))

    operator fun plus(other: kotlin.time.Duration): Duration = this + of(other)

    operator fun minus(other: Duration): Duration =
        Duration(NativeTime.durationSubtract(this, other))

    operator fun minus(other: kotlin.time.Duration): Duration = this - of(other)

    operator fun unaryMinus(): Duration = Duration(NativeTime.durationNegate(this))

    operator fun times(factor: Long): Duration = Duration(NativeTime.durationMultiply(this, factor))

    operator fun div(divisor: Long): Duration = Duration(NativeTime.durationDivide(this, divisor))

    override fun compareTo(other: Duration): Int = NativeTime.durationCompare(this, other)

    override fun equals(other: Any?): Boolean = other is Duration && compareTo(other) == 0

    override fun hashCode(): Int = NativeTime.durationHash(this)

    override fun toString(): String = NativeTime.durationString(this)

    companion object {
        internal fun fromNative(id: Long): Duration = Duration(id)

        fun parse(value: String): StatusOr<Duration> = NativeTime.durationParse(value)

        fun of(value: kotlin.time.Duration): Duration =
            if (value.isInfinite())
                NativeTime.durationValue(0, 0, if (value.isNegative()) -1 else 1).value()
            else
                value.toComponents { seconds, nanoseconds ->
                    NativeTime.durationValue(seconds, nanoseconds, 0).value()
                }

        fun nanoseconds(value: Long): Duration = Duration(NativeTime.durationNanoseconds(value))

        fun microseconds(value: Long): Duration = Duration(NativeTime.durationMicroseconds(value))

        fun milliseconds(value: Long): Duration = Duration(NativeTime.durationMilliseconds(value))

        fun seconds(value: Long): Duration = Duration(NativeTime.durationSeconds(value))

        fun minutes(value: Long): Duration = Duration(NativeTime.durationMinutes(value))

        fun hours(value: Long): Duration = Duration(NativeTime.durationHours(value))

        fun zero(): Duration = of(kotlin.time.Duration.ZERO)

        fun infinite(): Duration = of(kotlin.time.Duration.INFINITE)
    }
}

/** An Abseil instant with native arithmetic, ordering, and infinite bounds. */
class Time private constructor(id: Long) : NativeObject(id), Comparable<Time> {
    fun nanosecondsSinceEpoch(): StatusOr<Long> = NativeTime.timeNanoseconds(this)

    /** Convert exactly; Kotlin Instant is experimental in Kotlin 2.2. */
    @kotlin.time.ExperimentalTime
    fun toKotlinInstant(): StatusOr<kotlin.time.Instant> {
        val parts =
            NativeTime.timeParts(this).orElse { status ->
                return status
            }
        val converted = kotlin.time.Instant.fromEpochSeconds(parts.seconds, parts.nanoseconds)
        if (
            converted.epochSeconds != parts.seconds ||
                converted.nanosecondsOfSecond != parts.nanoseconds
        )
            return outOfRange("Instant exceeds Kotlin's representable range")
        return Ok(converted)
    }

    operator fun plus(duration: Duration): Time = Time(NativeTime.timeAdd(this, duration))

    operator fun plus(duration: kotlin.time.Duration): Time = this + Duration.of(duration)

    operator fun minus(duration: Duration): Time = Time(NativeTime.timeSubtract(this, duration))

    operator fun minus(duration: kotlin.time.Duration): Time = this - Duration.of(duration)

    operator fun minus(other: Time): Duration =
        Duration.fromNative(NativeTime.timeDifference(this, other))

    override fun compareTo(other: Time): Int = NativeTime.timeCompare(this, other)

    override fun equals(other: Any?): Boolean = other is Time && compareTo(other) == 0

    override fun hashCode(): Int = NativeTime.timeHash(this)

    override fun toString(): String = NativeTime.timeString(this)

    companion object {
        fun now(): Time = Time(NativeTime.timeNow())

        fun parse(value: String): StatusOr<Time> = NativeTime.timeParse(value)

        @kotlin.time.ExperimentalTime
        fun fromKotlinInstant(value: kotlin.time.Instant): Time =
            NativeTime.timeValue(value.epochSeconds, value.nanosecondsOfSecond, 0).value()

        fun unixEpoch(): Time = NativeTime.timeValue(0, 0, 0).value()

        fun infiniteFuture(): Time = NativeTime.timeValue(0, 0, 1).value()

        fun infinitePast(): Time = NativeTime.timeValue(0, 0, -1).value()
    }
}

/** Native cooperative-fiber diagnostics. */
object Debug {
    fun fiberReport(): String = NativeBridge.call("debug.fiberReport").map(::nativeText).value()

    fun currentFiberId(): Long = NativeBridge.currentFiberId()

    fun totalCompletedWaits(): Long =
        NativeBridge.call("debug.completedWaits").map(NativeBridge::number).value()
}
