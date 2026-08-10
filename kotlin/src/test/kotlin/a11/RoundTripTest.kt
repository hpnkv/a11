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
