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

import a11.net.ByteReassembler
import a11.net.parseBytePacket
import a11.net.splitBytesIntoPackets
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class ByteChunkingTest {
    @Test
    fun outOfOrderAndInterleavedMessagesReleasePendingCapacity() {
        val a = ByteArray(200) { it.toByte() }
        val b = ByteArray(100) { (-it).toByte() }
        val pa = splitBytesIntoPackets(a, 1uL, 32).value()
        val pb = splitBytesIntoPackets(b, 2uL, 32).value()
        val assembler = ByteReassembler(packetSize = 32, maxPendingMessages = 2)
        val found = mutableListOf<ByteArray>()
        for (index in maxOf(pa.size, pb.size) - 1 downTo 0) {
            if (index < pa.size) assembler.feed(pa[index]).value()?.let { found.add(it) }
            if (index < pb.size) assembler.feed(pb[index]).value()?.let { found.add(it) }
        }
        assertEquals(2, found.size)
        assertTrue(found.any { it.contentEquals(a) })
        assertTrue(found.any { it.contentEquals(b) })
        assertNull(assembler.feed(pa.first()).value())
        assertEquals(StatusCode.ALREADY_EXISTS, assembler.feed(pa.first()).status.code)
        assembler.clear()
        var complete: ByteArray? = null
        for (packet in pa) assembler.feed(packet).value()?.let { complete = it }
        assertContentEquals(a, complete)
    }

    @Test
    fun unsignedSuffixesAndLateCountsCannotEscapeBounds() {
        val packets = splitBytesIntoPackets(ByteArray(50), 1uL, 32).value()
        val unsigned = packets[1].copyOf()
        ByteBuffer.wrap(unsigned).order(ByteOrder.LITTLE_ENDIAN).putInt(unsigned.size - 13, -1)
        assertEquals(UInt.MAX_VALUE, parseBytePacket(unsigned).value().sequence)
        assertEquals(
            StatusCode.OUT_OF_RANGE,
            ByteReassembler(packetSize = 32).feed(unsigned).status.code,
        )
        val assembler = ByteReassembler(packetSize = 32, maxPendingMessages = 1)
        val late = packets[1].copyOf()
        ByteBuffer.wrap(late).order(ByteOrder.LITTLE_ENDIAN).putInt(late.size - 13, 20)
        assertNull(assembler.feed(late).value())
        assertEquals(StatusCode.OUT_OF_RANGE, assembler.feed(packets[0]).status.code)
        assertNull(assembler.feed(splitBytesIntoPackets(ByteArray(50), 2uL, 32).value()[0]).value())
        val limited = ByteReassembler(packetSize = 32, maxPendingBytes = 1)
        assertEquals(StatusCode.RESOURCE_EXHAUSTED, limited.feed(packets[0]).status.code)
    }

    @Test
    fun nativeLimitsCountersAndUnsignedIdentifiersRemainInspectable() {
        assertEquals(18, a11.net.MINIMUM_BYTE_PACKET_SIZE)
        assertEquals(
            StatusCode.INVALID_ARGUMENT,
            ByteReassembler.create(a11.net.ByteChunkingOptions(packetSize = 1uL)).status.code,
        )
        assertEquals(
            StatusCode.INVALID_ARGUMENT,
            a11.net.ByteChunkingOptions(maxPendingMessages = 0uL).validate().code,
        )
        val bytes = ByteArray(100) { value -> value.toByte() }
        val packets = splitBytesIntoPackets(bytes, ULong.MAX_VALUE, 32).value()
        val packet = parseBytePacket(packets.first()).value()
        assertEquals(ULong.MAX_VALUE, packet.transientId)
        val copy = packet.copy()
        packet.transientId = 42uL
        packet.sequence = UInt.MAX_VALUE
        packet.packetCount = UInt.MAX_VALUE
        packet.payload = byteArrayOf(3)
        assertEquals(ULong.MAX_VALUE, copy.transientId)
        assertEquals(42uL, packet.transientId)
        assertEquals(UInt.MAX_VALUE, packet.sequence)
        assertEquals(UInt.MAX_VALUE, packet.packetCount)
        assertContentEquals(byteArrayOf(3), packet.payload)
        val assembler =
            ByteReassembler.create(a11.net.ByteChunkingOptions(packetSize = 32uL)).value()
        assertEquals(0uL, assembler.pendingMessageCount)
        assertNull(assembler.feed(packets.first()).value())
        assertEquals(1uL, assembler.pendingMessageCount)
        assertEquals(copy.payload.size.toULong(), assembler.pendingByteCount)
        assembler.clear()
        assertEquals(0uL, assembler.pendingMessageCount)
        assertEquals(0uL, assembler.pendingByteCount)
    }
}
