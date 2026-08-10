package a11

import java.util.Base64
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * Proves the Kotlin msgpack framing is byte-for-byte compatible with the native
 * A11 encoder: each vector was produced by the installed Python `a11` package
 * (`to_msgpack()`), decoded here, and re-encoded — the bytes must match exactly.
 */
class GoldenVectorsTest {
    private val vectors: Map<String, ByteArray> by lazy {
        val text = javaClass.getResourceAsStream("/golden_vectors.txt")!!.bufferedReader().readText()
        text.lineSequence().filter { it.contains('=') }.associate {
            val (k, v) = it.split("=", limit = 2)
            k to Base64.getDecoder().decode(v)
        }
    }

    private fun <T> ok(value: StatusOr<T>): T = (value as Ok<T>).value

    @Test
    fun chunkJsonRoundTripsToIdenticalBytes() {
        val golden = vectors.getValue("chunk_json")
        val chunk = ok(Chunk.fromMsgpack(golden))
        assertEquals("application/json;type=object", chunk.mimetype)
        assertContentEquals("{\"a\":1}".toByteArray(), chunk.data)
        assertContentEquals(golden, ok(chunk.toMsgpack()))
    }

    @Test
    fun chunkTextRoundTrips() {
        val golden = vectors.getValue("chunk_text")
        val chunk = ok(Chunk.fromMsgpack(golden))
        assertEquals("text/plain", chunk.mimetype)
        assertContentEquals(golden, ok(chunk.toMsgpack()))
    }

    @Test
    fun nodeFragmentRoundTrips() {
        val golden = vectors.getValue("fragment")
        val fragment = ok(NodeFragment.fromMsgpack(golden))
        assertEquals("a1#text_output", fragment.id)
        assertEquals(0L, fragment.seq)
        assertEquals(false, fragment.continued)
        assertContentEquals(golden, ok(fragment.toMsgpack()))
    }

    @Test
    fun wireMessageRoundTrips() {
        val golden = vectors.getValue("wire_message")
        val message = ok(WireMessage.fromMsgpack(golden))
        assertEquals(1, message.actions.size)
        assertEquals(1, message.nodeFragments.size)
        val action = message.actions[0]
        assertEquals("a1", action.id)
        assertEquals("interact_with_llm", action.name)
        assertEquals("a1#interactions", action.inputs.single().id)
        assertEquals("a1#text_output", action.outputs.single().id)
        assertContentEquals("claude".toByteArray(), action.headers.getValue("x-a11-llm-provider"))
        assertContentEquals(golden, ok(message.toMsgpack()))
    }

    @Test
    fun allVectorsLoaded() {
        assertTrue(vectors.keys.containsAll(listOf("chunk_json", "chunk_text", "fragment", "wire_message")))
    }
}
