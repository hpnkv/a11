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
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlinx.coroutines.async
import kotlinx.coroutines.runBlocking

/**
 * What a null chunk means to a reader.
 *
 * It marks the end of a node and is not returned as a value. Iteration skips it, and a node
 * containing no other values reads as empty. A caller can therefore close an optional unary port
 * with a bare null final.
 */
class AsyncNodeTest {
    private fun <T> ok(value: StatusOr<T>): T =
        when (value) {
            is Ok<T> -> value.value
            is Err -> throw AssertionError("expected ok, got $value")
        }

    @Test
    fun consumeAcceptsBothSpellingsOfASingleValue() = runBlocking {
        val asFinal = ok(AsyncNode.create("consume-final"))
        assertTrue(asFinal.finalize("value").ok())
        assertEquals("value", ok(asFinal.consume()))

        val thenNull = ok(AsyncNode.create("consume-then-null"))
        ok(thenNull.put("value"))
        assertTrue(thenNull.finalize().ok())
        assertEquals("value", ok(thenNull.consume()))
    }

    @Test
    fun consumeReadsANodeHoldingNoValueAsNone() = runBlocking {
        val closedEmpty = ok(AsyncNode.create("consume-closed-empty"))
        closedEmpty.close()
        assertNull(ok(closedEmpty.consume(allowNone = true)))

        val nullOnly = ok(AsyncNode.create("consume-null-only"))
        assertTrue(nullOnly.finalize().ok())
        assertNull(ok(nullOnly.consume(allowNone = true)))
    }

    @Test
    fun consumeStillRefusesAnEmptyNodeWhenAValueIsRequired() = runBlocking {
        val nullOnly = ok(AsyncNode.create("consume-null-only-strict"))
        assertTrue(nullOnly.finalize().ok())

        val refused = nullOnly.consume()

        assertTrue(refused is Err && refused.status.code == StatusCode.FAILED_PRECONDITION)
    }

    @Test
    fun iterationSkipsANullMarkerRatherThanFailing() = runBlocking {
        val node = ok(AsyncNode.create("iterate-null"))
        ok(node.put("first"))
        ok(node.put("second"))
        assertTrue(node.finalize().ok())

        val values = mutableListOf<Any?>()
        while (true) {
            val value = ok(node.next()) ?: break
            values.add(value)
        }

        assertEquals(listOf<Any?>("first", "second"), values)
    }

    /**
     * Closing tells the peer, since nothing else can: a remote reader ends on a not-continued
     * fragment and a drain writes none.
     */
    @Test
    fun drainingTeesAClosureMarkerAfterTheData() = runBlocking {
        val sink = RecordingStream()
        val node = ok(AsyncNode.create("close-tee"))
        node.attachStream(sink)
        ok(node.put("only"))

        assertTrue(node.close().ok())

        val fragments = sink.sent.flatMap { it.nodeFragments }
        assertEquals(2, fragments.size)
        assertTrue(!isCloseStatusChunk(ok(fragments[0].getChunk())))
        val marker = ok(fragments[1].getChunk())
        assertTrue(isCloseStatusChunk(marker))
        assertEquals("close-tee", fragments[1].id)
        assertTrue(!fragments[1].continued)
        assertTrue(statusFromChunk(marker).ok())
        // Closing twice must not repeat the marker.
        assertTrue(node.close().ok())
        assertEquals(2, sink.sent.flatMap { it.nodeFragments }.size)
    }

    @Test
    fun cancellationWakesReadersAndStaysDistinctFromEndOfStream() = runBlocking {
        val reader = ok(AsyncNode.create("cancel-reader"))
        val pending =
            async(start = kotlinx.coroutines.CoroutineStart.UNDISPATCHED) { reader.nextFragment() }
        reader.cancelReader()
        assertEquals(StatusCode.ABORTED, pending.await().status.code)
        assertEquals(StatusCode.ABORTED, reader.nextFragment().status.code)
        val writer = ok(AsyncNode.create("cancel-writer"))
        writer.cancelWriter()
        assertEquals(StatusCode.ABORTED, writer.nextFragment().status.code)
        val ended = ok(AsyncNode.create("clean-end"))
        ended.close()
        assertEquals(Ok(null), ended.nextFragment())
        val failed = ok(AsyncNode.create("failed-end"))
        assertEquals(StatusCode.INVALID_ARGUMENT, failed.abortWithStatus(Status.ok()).code)
        val cause = dataLoss("disk failed")
        failed.abortWithStatus(cause)
        assertEquals(cause, failed.nextFragment().status)
        assertEquals(cause, failed.put("later").status)
    }

    @Test
    fun sequencesAreUniqueBoundedAndCannotCrossFinality() = runBlocking {
        val node = ok(AsyncNode.create("sequence"))
        assertEquals(StatusCode.INVALID_ARGUMENT, node.putChunk(makeNullChunk(), -1).status.code)
        assertEquals(
            StatusCode.INVALID_ARGUMENT,
            node.putChunk(makeNullChunk(), 0x1_0000_0000L).status.code,
        )
        ok(node.put("third", seq = 2, final = true))
        assertEquals(StatusCode.INVALID_ARGUMENT, node.put("too late", seq = 3).status.code)
        assertEquals(
            StatusCode.FAILED_PRECONDITION,
            node.put("other final", seq = 1, final = true).status.code,
        )
        ok(node.put("first", seq = 0))
        ok(node.put("second", seq = 1))
        assertEquals(StatusCode.ALREADY_EXISTS, node.put("replacement", seq = 1).status.code)
        assertEquals(3, ok(node.size()))
        assertEquals(listOf("first", "second", "third"), List(3) { ok(node.next()) })
        assertEquals(Ok(null), node.next())
        assertEquals(StatusCode.ALREADY_EXISTS, node.put("read duplicate", seq = 0).status.code)
    }

    @Test
    fun storedChunksOwnBytesAndMetadata() = runBlocking {
        val node = ok(AsyncNode.create("owned"))
        val bytes = utf8Encode("original")
        val metadata =
            ChunkMetadata("text/plain", attributes = linkedMapOf("key" to byteArrayOf(1)))
        val chunk = Chunk(metadata = metadata, data = bytes)
        ok(node.putChunk(chunk, final = true))
        bytes.fill(0)
        metadata.mimetype = "application/json"
        metadata.attributes.getValue("key")[0] = 2
        val read = ok(node.nextChunk())!!
        assertEquals("text/plain", read.mimetype)
        assertEquals("original", ok(utf8Decode(read.data)))
        assertEquals(1.toByte(), read.metadata!!.attributes.getValue("key")[0])
    }

    @Test
    fun timeoutAndCoroutineCancellationLeaveTheNodeReadable() = runBlocking {
        val node = ok(AsyncNode.create("timeout"))
        assertEquals(StatusCode.INVALID_ARGUMENT, node.next(-1).status.code)
        assertEquals(StatusCode.DEADLINE_EXCEEDED, node.next(1).status.code)
        val pending = async(start = kotlinx.coroutines.CoroutineStart.UNDISPATCHED) { node.next() }
        pending.cancel()
        pending.join()
        ok(node.put("after cancellation", final = true))
        assertEquals("after cancellation", ok(node.next()))
        val map = NodeMap()
        val canonical = ok(map.get("owned-node"))
        assertTrue(canonical === ok(map.get("owned-node")))
        assertNull(ok(map.discard("owned-node", node)))
        assertTrue(canonical === ok(map.discard("owned-node", canonical)))
        assertNull(ok(map.getIfExists("owned-node")))
    }

    @Test
    fun explicitReadsDistinguishJsonNullFromEndAndTerminalErrors() = runBlocking {
        val node = ok(AsyncNode.create("nullable"))
        ok(node.put(null))
        ok(node.put("value", final = true))
        assertEquals(Ok(NodeRead.Value(null)), node.read())
        assertEquals(Ok(NodeRead.Value("value")), node.read())
        assertEquals(Ok(NodeRead.End), node.read())
        val aborted = ok(AsyncNode.create("buffered-abort"))
        ok(aborted.put("buffered"))
        val error = dataLoss("read failure")
        aborted.abortWithStatus(error)
        assertEquals(Ok(NodeRead.Value("buffered")), aborted.read())
        assertEquals(error, aborted.read().status)
    }

    private class RecordingStream :
        a11.net.WireStream by a11.net.InProcessWireStream.createPair().first {
        val sent = mutableListOf<WireMessage>()

        override fun send(message: WireMessage): Status {
            sent.add(message)
            return Status.ok()
        }
    }
}
