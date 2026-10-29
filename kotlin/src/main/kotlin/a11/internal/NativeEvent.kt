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

import a11.StatusOr

internal sealed interface NativeEvent {
    data class Completion(val token: Long, val result: StatusOr<Any?>, val owned: LongArray) :
        NativeEvent

    data class Invocation(
        val token: Long,
        val promise: Long,
        val arguments: Any?,
        val owned: LongArray,
    ) : NativeEvent

    data class Cancellation(val promise: Long) : NativeEvent

    data class Release(val token: Long) : NativeEvent
}
