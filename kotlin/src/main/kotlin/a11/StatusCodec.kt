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
import a11.internal.nativeBytes

/** A status carried as a successful data value. */
data class DecodedStatus(val status: Status)

fun packStatus(status: Status): StatusOr<ByteArray> =
    NativeBridge.call("status.encode", args = mapOf("value" to statusFields(status)))
        .map(::nativeBytes)

fun decodeStatus(bytes: ByteArray): StatusOr<DecodedStatus> = NativeControl.decodeStatus(bytes)

fun unpackStatus(bytes: ByteArray): Status =
    when (val decoded = decodeStatus(bytes)) {
        is Ok -> decoded.value.status
        is Err -> decoded.status
    }
