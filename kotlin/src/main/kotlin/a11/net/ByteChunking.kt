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

package a11.net

import a11.NativeObject
import a11.Status
import a11.StatusOr
import a11.internal.NativeFraming
import a11.invalidArgument
import a11.map
import a11.value

private val framingDefaults by lazy { NativeFraming.defaults() }

/** Smallest packet containing framing metadata and one payload byte. */
val MINIMUM_BYTE_PACKET_SIZE: Int
    get() = NativeFraming.minimumPacketSize()

/** Native packet shape identifiers, unchanged on the wire. */
enum class BytePacketType(val value: Int) {
    COMPLETE_BYTES(0x00),
    BYTE_CHUNK(0x01),
    LENGTH_SUFFIXED_BYTE_CHUNK(0x02),
}

/** Native framing limits; validate runtime configuration before constructing a reassembler. */
data class ByteChunkingOptions(
    val packetSize: ULong = framingDefaults[0].toULong(),
    val maxMessageSize: ULong = framingDefaults[1].toULong(),
    val maxPendingMessages: ULong = framingDefaults[2].toULong(),
    val maxPendingBytes: ULong = framingDefaults[3].toULong(),
) {
    fun validate(): Status = NativeFraming.validate(this)
}

/** A parsed native packet. Payload access returns owned JVM bytes. */
class BytePacket internal constructor(id: Long) : NativeObject(id) {
    constructor(
        type: BytePacketType,
        payload: ByteArray,
        transientId: ULong = 0uL,
        sequence: UInt = 0u,
        packetCount: UInt = 0u,
    ) : this(
        NativeFraming.packetCreate(
            type.value,
            payload,
            transientId.toLong(),
            sequence.toLong(),
            packetCount.toLong(),
        )
    )

    var type: BytePacketType
        get() = BytePacketType.entries[NativeFraming.packetType(this)]
        set(value) = NativeFraming.packetTypeSet(this, value.value)

    var payload: ByteArray
        get() = NativeFraming.packetPayload(this)
        set(value) = NativeFraming.packetPayloadSet(this, value)

    var transientId: ULong
        get() = NativeFraming.packetTransientId(this).toULong()
        set(value) = NativeFraming.packetTransientIdSet(this, value.toLong())

    var sequence: UInt
        get() = NativeFraming.packetSequence(this).toUInt()
        set(value) = NativeFraming.packetSequenceSet(this, value.toLong())

    var packetCount: UInt
        get() = NativeFraming.packetCount(this).toUInt()
        set(value) = NativeFraming.packetCountSet(this, value.toLong())

    fun copy(): BytePacket = BytePacket(NativeFraming.packetCopy(this))
}

/** Split bytes using C++'s fixed little-endian packet suffixes. */
fun splitBytesIntoPackets(
    bytes: ByteArray,
    transientId: ULong,
    packetSize: Int = framingDefaults[0].toInt(),
): StatusOr<List<ByteArray>> = NativeFraming.split(bytes, transientId.toLong(), packetSize)

/** Parse one packet using the native validator. */
fun parseBytePacket(packet: ByteArray): StatusOr<BytePacket> = NativeFraming.parse(packet)

/** Native bounded, thread-safe reassembly of out-of-order and interleaved messages. */
class ByteReassembler private constructor(id: Long) : NativeObject(id) {
    /** Limits must be valid, as in C++; use [create] for runtime configuration. */
    constructor(
        packetSize: Int = framingDefaults[0].toInt(),
        maxMessageSize: Int = framingDefaults[1].toInt(),
        maxPendingMessages: Int = framingDefaults[2].toInt(),
        maxPendingBytes: Int = framingDefaults[3].toInt(),
    ) : this(createLegacy(packetSize, maxMessageSize, maxPendingMessages, maxPendingBytes).value())

    fun clear() = NativeFraming.clear(this)

    fun feed(serialized: ByteArray): StatusOr<ByteArray?> = NativeFraming.feed(this, serialized)

    val pendingMessageCount: ULong
        get() = NativeFraming.pendingMessages(this).toULong()

    val pendingByteCount: ULong
        get() = NativeFraming.pendingBytes(this).toULong()

    companion object {
        fun create(
            options: ByteChunkingOptions = ByteChunkingOptions()
        ): StatusOr<ByteReassembler> = NativeFraming.create(options).map(::ByteReassembler)

        private fun createLegacy(
            packetSize: Int,
            maxMessageSize: Int,
            maxPendingMessages: Int,
            maxPendingBytes: Int,
        ): StatusOr<Long> {
            if (
                listOf(packetSize, maxMessageSize, maxPendingMessages, maxPendingBytes).any { limit
                    ->
                    limit < 0
                }
            )
                return invalidArgument("Byte chunking limits must not be negative")
            return NativeFraming.create(
                ByteChunkingOptions(
                    packetSize.toULong(),
                    maxMessageSize.toULong(),
                    maxPendingMessages.toULong(),
                    maxPendingBytes.toULong(),
                )
            )
        }
    }
}
