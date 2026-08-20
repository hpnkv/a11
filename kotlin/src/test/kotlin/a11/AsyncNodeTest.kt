package a11

import kotlinx.coroutines.runBlocking
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * What a null chunk means to a reader.
 *
 * It marks the end of a node rather than being a value in it, so a reader must
 * neither hand it back nor refuse it: iteration skips it, and a node holding
 * nothing else reads as empty. That is what lets a caller close an optional
 * port — a unary `config` it wants the backend's defaults for — by writing a
 * bare null final.
 */
class AsyncNodeTest {
    private fun <T> ok(value: StatusOr<T>): T = when (value) {
        is Ok<T> -> value.value
        is Status -> throw AssertionError("expected ok, got $value")
    }

    @Test
    fun consumeAcceptsBothSpellingsOfASingleValue() = runBlocking {
        val asFinal = ok(AsyncNode.create("consume-final"))
        assertTrue(asFinal.finalize("value").isOk)
        assertEquals("value", ok(asFinal.consume()))

        val thenNull = ok(AsyncNode.create("consume-then-null"))
        ok(thenNull.put("value"))
        assertTrue(thenNull.finalize().isOk)
        assertEquals("value", ok(thenNull.consume()))
    }

    @Test
    fun consumeReadsANodeHoldingNoValueAsNone() = runBlocking {
        val closedEmpty = ok(AsyncNode.create("consume-closed-empty"))
        closedEmpty.close()
        assertNull(ok(closedEmpty.consume(allowNone = true)))

        val nullOnly = ok(AsyncNode.create("consume-null-only"))
        assertTrue(nullOnly.finalize().isOk)
        assertNull(ok(nullOnly.consume(allowNone = true)))
    }

    @Test
    fun consumeStillRefusesAnEmptyNodeWhenAValueIsRequired() = runBlocking {
        val nullOnly = ok(AsyncNode.create("consume-null-only-strict"))
        assertTrue(nullOnly.finalize().isOk)

        val refused = nullOnly.consume()

        assertTrue(refused is Status && refused.code == StatusCode.FAILED_PRECONDITION)
    }

    @Test
    fun iterationSkipsANullMarkerRatherThanFailing() = runBlocking {
        val node = ok(AsyncNode.create("iterate-null"))
        ok(node.put("first"))
        ok(node.put("second"))
        assertTrue(node.finalize().isOk)

        val values = mutableListOf<Any?>()
        while (true) {
            val value = ok(node.next()) ?: break
            values.add(value)
        }

        assertEquals(listOf<Any?>("first", "second"), values)
    }

    /**
     * Closing tells the peer, since nothing else can: a remote reader ends on a
     * not-continued fragment and a drain writes none.
     */
    @Test
    fun drainingTeesAClosureMarkerAfterTheData() = runBlocking {
        val sink = RecordingStream()
        val node = ok(AsyncNode.create("close-tee"))
        node.attachStream(sink)
        ok(node.put("only"))

        assertTrue(node.close().isOk)

        val fragments = sink.sent.flatMap { it.nodeFragments }
        assertEquals(2, fragments.size)
        assertTrue(!isCloseStatusChunk(ok(fragments[0].getChunk())))
        val marker = ok(fragments[1].getChunk())
        assertTrue(isCloseStatusChunk(marker))
        assertEquals("close-tee", fragments[1].id)
        assertTrue(!fragments[1].continued)
        assertTrue(statusFromChunk(marker).isOk)
        // Closing twice must not repeat the marker.
        assertTrue(node.close().isOk)
        assertEquals(2, sink.sent.flatMap { it.nodeFragments }.size)
    }

    private class RecordingStream : WritableWireStream {
        val sent = mutableListOf<WireMessage>()

        override fun send(message: WireMessage): Status {
            sent.add(message)
            return Status.ok()
        }
    }
}
