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
import kotlin.test.assertTrue

/** Pure-Kotlin round trips for the Status codec and the serialization registry. */
class RoundTripTest {
    private fun <T> ok(value: StatusOr<T>): T = (value as Ok<T>).value

    @Test
    fun statusCodecRoundTrips() {
        val original = notFound("missing thing")
        val bytes = ok(packStatus(original))
        val decoded = ok(decodeStatus(bytes)).status
        assertEquals(StatusCode.NOT_FOUND, decoded.code)
        assertEquals("missing thing", decoded.message)
    }

    @Test
    fun jsonSerializationRoundTripsObject() {
        val registry = SerializationRegistry(registerDefaults = true)
        val value = linkedMapOf<String, Any?>("a" to 1L, "b" to listOf("x", "y"), "c" to true)
        val chunk = ok(registry.toChunk(value, "application/json"))
        assertTrue(chunk.mimetype.startsWith("application/json"))
        @Suppress("UNCHECKED_CAST")
        val back = ok(registry.fromChunk(chunk)) as Map<String, Any?>
        assertEquals(1L, back["a"])
        assertEquals(listOf("x", "y"), back["b"])
        assertEquals(true, back["c"])
    }

    @Test
    fun jsonSerializationRoundTripsString() {
        val registry = SerializationRegistry(registerDefaults = true)
        val chunk = ok(registry.toChunk("hello", "application/json"))
        assertEquals("hello", ok(registry.fromChunk(chunk)))
    }
}
