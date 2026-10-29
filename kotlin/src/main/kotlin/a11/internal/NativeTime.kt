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

package a11.internal

import a11.Duration
import a11.StatusOr
import a11.Time

internal data class TimeComponents(val seconds: Long, val nanoseconds: Int, val infinity: Int = 0)

/** Typed JNI calls to Abseil's time values and arithmetic. */
internal object NativeTime {
    init {
        NativeBridge.ensureLoaded()
    }

    external fun durationParse(text: String): StatusOr<Duration>

    external fun durationString(self: Duration): String

    external fun durationHash(self: Duration): Int

    external fun durationCompare(self: Duration, other: Duration): Int

    external fun durationNanosecondCount(self: Duration): StatusOr<Long>

    external fun durationParts(self: Duration): StatusOr<TimeComponents>

    external fun durationAdd(self: Duration, other: Duration): Long

    external fun durationSubtract(self: Duration, other: Duration): Long

    external fun timeParse(text: String): StatusOr<Time>

    external fun timeString(self: Time): String

    external fun timeHash(self: Time): Int

    external fun timeCompare(self: Time, other: Time): Int

    external fun timeNanoseconds(self: Time): StatusOr<Long>

    external fun timeParts(self: Time): StatusOr<TimeComponents>

    external fun timeAdd(self: Time, other: Duration): Long

    external fun timeSubtract(self: Time, other: Duration): Long

    external fun durationValue(seconds: Long, nanoseconds: Int, infinity: Int): StatusOr<Duration>

    external fun timeValue(seconds: Long, nanoseconds: Int, infinity: Int): StatusOr<Time>

    external fun durationNanoseconds(count: Long): Long

    external fun durationMicroseconds(count: Long): Long

    external fun durationMilliseconds(count: Long): Long

    external fun durationSeconds(count: Long): Long

    external fun durationMinutes(count: Long): Long

    external fun durationHours(count: Long): Long

    external fun durationNegate(self: Duration): Long

    external fun durationMultiply(self: Duration, factor: Long): Long

    external fun durationDivide(self: Duration, factor: Long): Long

    external fun timeDifference(self: Time, other: Time): Long

    external fun timeNow(): Long
}
