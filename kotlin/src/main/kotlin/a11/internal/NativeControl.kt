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

import a11.DecodedStatus
import a11.NativeFuture
import a11.Status
import a11.StatusOr

/** Direct JNI control operations without serialized request envelopes. */
internal object NativeControl {
    init {
        NativeBridge.ensureLoaded()
    }

    external fun decodeStatus(bytes: ByteArray): StatusOr<DecodedStatus>

    external fun validateName(value: String): Status

    external fun statusCodeFromHttp(value: Int): Int

    external fun statusCodeToHttp(value: Int): Int

    external fun statusCodeFromWebSocket(value: Int): Int

    external fun statusCodeToWebSocket(value: Int): Int

    external fun release(id: Long)

    external fun futureIsReady(value: NativeFuture<*>): StatusOr<Boolean>

    external fun futureCancel(id: Long): Status

    external fun storeHandleCount(): Long

    external fun readerHandleCount(): Long
}
