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
import a11.internal.nativeBytes

/** A11 MessagePack operations delegated to the native codec. */
object MsgpackCodec {
    fun encodeFields(values: List<Any?>): StatusOr<ByteArray> =
        NativeBridge.call("msgpack.encodeFields", args = mapOf("values" to values))
            .map(::nativeBytes)

    fun decodeFields(bytes: ByteArray, count: Int, context: String): StatusOr<List<Any?>> =
        NativeBridge.call(
                "msgpack.decodeFields",
                args = mapOf("bytes" to bytes, "count" to count, "context" to context),
            )
            .map { value ->
                value as? List<*> ?: fatal("Native MessagePack fields are not an array")
            }

    fun encodeOne(value: Any?): StatusOr<ByteArray> =
        NativeBridge.call("msgpack.encode", args = mapOf("value" to value)).map(::nativeBytes)

    fun decodeOne(bytes: ByteArray): StatusOr<Any?> =
        NativeBridge.call("msgpack.decode", args = mapOf("bytes" to bytes))
}
