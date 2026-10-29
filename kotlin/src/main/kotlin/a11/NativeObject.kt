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

import a11.internal.NativeBridge
import a11.internal.NativeControl
import a11.internal.NativeReference

/** A JVM reference retaining shared C++ ownership for its reachable lifetime. */
abstract class NativeObject internal constructor(id: Long) {
    private val reference = NativeReference(id)
    @get:JvmName("getNativeId")
    internal val nativeId: Long
        get() = reference.id

    internal fun release() {
        reference.release()
    }
}

/** A cancellable native future; awaiting does not block a JVM coroutine worker. */
class NativeFuture<T> internal constructor(id: Long, private val decode: (Any?) -> T) :
    NativeObject(id) {
    suspend fun await(): StatusOr<T> = NativeBridge.await(this).map(decode)

    val isReady: StatusOr<Boolean>
        get() = NativeControl.futureIsReady(this)

    fun cancel(): Status =
        try {
            NativeControl.futureCancel(nativeId)
        } finally {
            java.lang.ref.Reference.reachabilityFence(this)
        }
}
