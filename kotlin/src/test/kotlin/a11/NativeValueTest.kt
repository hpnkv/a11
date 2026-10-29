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
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

class NativeValueTest {
    @Test
    fun nativeMethodsAndMemberViewsMutateTheSameRecord() {
        val fragment = NodeFragment("node", Chunk(ChunkMetadata("text/plain")))
        val chunk = fragment.getChunk().value()
        val metadata = chunk.metadata!!
        val attributes = metadata.attributes
        metadata.setAttribute("native", byteArrayOf(0, -1)).orElse { status ->
            error(status.toString())
        }
        assertContentEquals(byteArrayOf(0, -1), attributes["native"])
        attributes["kotlin"] = byteArrayOf(4)
        assertContentEquals(
            byteArrayOf(4),
            fragment.getChunk().value().metadata!!.getAttribute("kotlin").value(),
        )
        chunk.data = utf8Encode("shared")
        assertContentEquals(utf8Encode("shared"), fragment.getChunk().value().data)
        assertEquals(StatusCode.NOT_FOUND, metadata.getAttribute("missing").status.code)
        assertEquals(StatusCode.FAILED_PRECONDITION, fragment.getNodeRef().status.code)
    }

    @Test
    fun constructorsAndCopiesFollowNativeValueOwnership() {
        val source = Chunk(ChunkMetadata("text/plain"), data = byteArrayOf(1))
        val fragment = NodeFragment("node", source)
        source.data = byteArrayOf(2)
        assertContentEquals(byteArrayOf(1), fragment.getChunk().value().data)
        val copy = fragment.copy()
        fragment.getChunk().value().data = byteArrayOf(3)
        assertContentEquals(byteArrayOf(1), copy.getChunk().value().data)
        val bytes = copy.getChunk().value().data
        bytes[0] = 9
        assertContentEquals(byteArrayOf(1), copy.getChunk().value().data)
    }

    @Test
    fun nativeCollectionsMutateAndRoundTripWithoutShadowState() {
        val message = WireMessage()
        message.actions.add(ActionMessage("call", "action"))
        val action = message.actions.single()
        action.inputs.add(Port("input", "node"))
        val port = action.inputs.single()
        port.id = "updated"
        action.headers["header"] = byteArrayOf(1)
        assertEquals("updated", message.actions.single().inputs.single().id)
        assertContentEquals(byteArrayOf(1), message.actions.single().headers["header"])
        val decoded = WireMessage.fromMsgpack(message.toMsgpack().value()).value()
        assertEquals(message, decoded)
        assertEquals(Port("input", "updated"), action.inputs.removeAt(0))
        assertTrue(message.actions.single().inputs.isEmpty())
        action.headers.entries.single().setValue(byteArrayOf(2))
        assertContentEquals(byteArrayOf(2), message.actions.single().headers["header"])
        action.headers.clear()
        assertTrue(message.actions.single().headers.isEmpty())
    }

    @Test
    fun aChildRetainsItsParentAndNativeAbsenceStaysDistinct() {
        fun child(): ChunkMetadata =
            NodeFragment("node", Chunk(ChunkMetadata("text/plain"))).getChunk().value().metadata!!
        val metadata = child()
        System.gc()
        metadata.mimetype = "application/octet-stream"
        assertEquals("application/octet-stream", metadata.mimetype)
        val chunk = Chunk()
        assertNull(chunk.metadata)
        assertFalse(chunk.isNull)
        assertTrue(makeNullChunk().isNull)
        assertFalse(chunk.hasObject)
        assertTrue(chunk.materialize().ok())
    }

    @Test
    fun unicodeAndEmbeddedNulUseUtf8RatherThanModifiedJniUtf8() {
        val text = "text/\u0000\uD83D\uDE00é"
        val metadata = ChunkMetadata(text)
        assertEquals(text, metadata.mimetype)
        metadata.attributes["unicode"] = utf8Encode(text)
        assertContentEquals(utf8Encode(text), metadata.getAttribute("unicode").value())
        assertEquals(metadata, ChunkMetadata.fromMsgpack(metadata.toMsgpack().value()).value())
    }

    @Test
    fun nativeUnsignedFieldsRetainTheirRangesWithoutNarrowing() {
        val reference = NodeRef("node", UInt.MAX_VALUE, 1uL)
        assertTrue(reference.validate().ok())
        assertEquals(reference, NodeRef.fromMsgpack(reference.toMsgpack().value()).value())
        reference.length = ULong.MAX_VALUE
        assertEquals(ULong.MAX_VALUE, reference.length)
        assertEquals(StatusCode.INVALID_ARGUMENT, reference.validate().code)
        val fragment = NodeFragment("node", makeNullChunk(), UInt.MAX_VALUE)
        assertEquals(
            UInt.MAX_VALUE,
            NodeFragment.fromMsgpack(fragment.toMsgpack().value()).value().seq,
        )
    }

    @Test
    fun malformedHostStringsRemainInspectableAndValidationIsRecoverable() {
        for (text in listOf("\uD800", "\uDC80", "\uDC00")) {
            val metadata = ChunkMetadata(text)
            assertEquals(text, metadata.mimetype)
            assertEquals(StatusCode.INVALID_ARGUMENT, validateName(text).code)
            assertEquals(StatusCode.INVALID_ARGUMENT, Duration.parse(text).status.code)
        }
    }

    @Test
    fun explicitNativeCopyMoveAndSwapRetainValueOwnership() {
        val source = Chunk(ChunkMetadata("text/plain"), data = utf8Encode("source"))
        val copied = Chunk()
        copied.copyFrom(source)
        source.data = utf8Encode("changed")
        assertContentEquals(utf8Encode("source"), copied.data)
        val moved = Chunk()
        moved.moveFrom(source)
        assertContentEquals(utf8Encode("changed"), moved.data)
        source.data = utf8Encode("reused")
        moved.swap(copied)
        assertContentEquals(utf8Encode("source"), moved.data)
        assertContentEquals(utf8Encode("changed"), copied.data)
        assertContentEquals(utf8Encode("reused"), source.data)
        moved.moveFrom(moved)
        assertContentEquals(utf8Encode("source"), moved.data)
        val left = ActionMessage("left", "example", headers = linkedMapOf("left" to byteArrayOf(1)))
        val right =
            ActionMessage("right", "example", headers = linkedMapOf("right" to byteArrayOf(2)))
        val headers = left.headers
        left.swap(right)
        assertEquals("right", left.id)
        assertContentEquals(byteArrayOf(2), headers["right"])
        assertTrue(!headers.containsKey("left"))
    }

    @Test
    fun bulkReplacementAcceptsNativeViewsAndSelfAssignment() {
        val message = WireMessage(actions = mutableListOf(ActionMessage("call", "example")))
        message.actions = message.actions
        assertEquals("call", message.actions.single().id)
        val action = message.actions.single()
        action.inputs = mutableListOf(Port("first", "one"), Port("second", "two"))
        action.inputs = action.inputs
        assertEquals(listOf("one", "two"), action.inputs.map { port -> port.id })
        action.inputs.clear()
        assertTrue(action.inputs.isEmpty())
        val first = ChunkMetadata("text/plain", attributes = linkedMapOf("one" to byteArrayOf(1)))
        val second = ChunkMetadata("text/plain")
        second.attributes = first.attributes
        first.attributes["one"] = byteArrayOf(2)
        assertContentEquals(byteArrayOf(1), second.attributes["one"])
        second.attributes = second.attributes
        second.attributes.putAll(linkedMapOf("two" to byteArrayOf(3)))
        first.attributes.putAll(second.attributes)
        assertEquals(2, first.attributes.size)
        assertContentEquals(byteArrayOf(3), first.attributes["two"])
        first.attributes.clear()
        assertTrue(first.attributes.isEmpty())
    }

    @Test
    fun millisecondAdaptersRetainAbseilRangeWithoutIntegerMultiplication() {
        for (millis in listOf(Long.MIN_VALUE, -1L, 0L, Long.MAX_VALUE)) {
            val metadata = ChunkMetadata("text/plain", timestampMillis = millis)
            assertEquals(millis, metadata.timestampMillis)
            assertEquals(millis, metadata.copy().timestampMillis)
        }
        val metadata = ChunkMetadata()
        assertNull(metadata.timestamp)
        metadata.timestamp = Time.infiniteFuture()
        assertEquals(Time.infiniteFuture(), metadata.timestamp)
        metadata.timestampMillis = null
        assertNull(metadata.timestamp)
    }

    @Test
    fun everyWireRecordSupportsNativeAssignmentOperations() {
        assignments(
            ChunkMetadata("first"),
            ChunkMetadata("second"),
            { ChunkMetadata() },
            { value -> value.copy() },
            ChunkMetadata::copyFrom,
            ChunkMetadata::moveFrom,
            ChunkMetadata::swap,
        )
        assignments(
            Chunk(data = byteArrayOf(1)),
            Chunk(data = byteArrayOf(2)),
            { Chunk() },
            { value -> value.copy() },
            Chunk::copyFrom,
            Chunk::moveFrom,
            Chunk::swap,
        )
        assignments(
            NodeRef("first"),
            NodeRef("second"),
            { NodeRef("") },
            { value -> value.copy() },
            NodeRef::copyFrom,
            NodeRef::moveFrom,
            NodeRef::swap,
        )
        assignments(
            NodeFragment("first"),
            NodeFragment("second"),
            { NodeFragment() },
            { value -> value.copy() },
            NodeFragment::copyFrom,
            NodeFragment::moveFrom,
            NodeFragment::swap,
        )
        assignments(
            Port("first", "one"),
            Port("second", "two"),
            { Port() },
            { value -> value.copy() },
            Port::copyFrom,
            Port::moveFrom,
            Port::swap,
        )
        assignments(
            ActionMessage("first", "example"),
            ActionMessage("second", "example"),
            { ActionMessage() },
            { value -> value.copy() },
            ActionMessage::copyFrom,
            ActionMessage::moveFrom,
            ActionMessage::swap,
        )
        assignments(
            WireMessage(headers = mapOf("first" to byteArrayOf(1))),
            WireMessage(headers = mapOf("second" to byteArrayOf(2))),
            { WireMessage() },
            { value -> value.copy() },
            WireMessage::copyFrom,
            WireMessage::moveFrom,
            WireMessage::swap,
        )
    }

    private fun <T> assignments(
        first: T,
        second: T,
        empty: () -> T,
        copy: (T) -> T,
        copyFrom: (T, T) -> Unit,
        moveFrom: (T, T) -> Unit,
        swap: (T, T) -> Unit,
    ) {
        val expectedFirst = copy(first)
        val expectedSecond = copy(second)
        val target = empty()
        copyFrom(target, first)
        assertEquals(expectedFirst, target)
        moveFrom(target, second)
        assertEquals(expectedSecond, target)
        swap(target, first)
        assertEquals(expectedFirst, target)
        assertEquals(expectedSecond, first)
        copyFrom(second, expectedSecond)
        assertEquals(expectedSecond, second)
    }

    @Test
    fun nativeChildViewsRetainRecordsAfterParentHandlesAreReleased() {
        val fragment = NodeFragment("node", Chunk(ChunkMetadata("text/plain")))
        val chunk = fragment.getChunk().value()
        val metadata = chunk.metadata!!
        fragment.release()
        chunk.release()
        metadata.setAttribute("retained", byteArrayOf(1)).orElse { status ->
            error(status.toString())
        }
        assertContentEquals(byteArrayOf(1), metadata.getAttribute("retained").value())
    }

    @Test
    fun nativeDiagnosticsAndSizeEstimatesCoverEveryWireRecord() {
        val metadata = ChunkMetadata("text/plain")
        val chunk = Chunk(metadata, data = byteArrayOf(1, 2, 3))
        val reference = NodeRef("node")
        val fragment = NodeFragment("node", chunk)
        val port = Port("input", "node")
        val action = ActionMessage("call", "example")
        val message = WireMessage(nodeFragments = listOf(fragment))
        for ((record, name) in
            listOf(
                metadata to "ChunkMetadata",
                chunk to "Chunk",
                reference to "NodeRef",
                fragment to "NodeFragment",
                port to "Port",
                action to "ActionMessage",
                message to "WireMessage",
            )) assertTrue(record.toString().startsWith("$name("))
        assertTrue(metadata.approxBytes > 0uL)
        assertTrue(chunk.approxBytes > metadata.approxBytes)
        assertTrue(reference.approxBytes > 0uL)
        assertTrue(fragment.approxBytes > chunk.approxBytes)
        assertTrue(port.approxBytes > 0uL)
        assertTrue(action.approxBytes > 0uL)
        assertTrue(message.approxBytes > fragment.approxBytes)
    }
}
