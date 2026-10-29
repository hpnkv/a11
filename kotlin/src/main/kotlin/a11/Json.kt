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

import a11.internal.JvmCodec
import a11.internal.NativeBridge
import a11.internal.nativeBytes
import a11.internal.nativeText

/** JSON parsing and encoding through the shared C++ codec. */
object A11Json {
    fun encodeToString(value: Any?): StatusOr<String> {
        val bytes =
            JvmCodec.encodeOne(value).orElse { status ->
                return status
            }
        return NativeBridge.call("json.encodeOrdered", args = mapOf("bytes" to bytes))
            .map(::nativeText)
    }

    fun parse(text: String): StatusOr<Any?> =
        NativeBridge.call("json.parseOrdered", args = mapOf("value" to text)).flatMap { value ->
            JvmCodec.decodeOne(nativeBytes(value))
        }
}
