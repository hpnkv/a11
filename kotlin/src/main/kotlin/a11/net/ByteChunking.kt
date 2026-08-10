package a11.net

import a11.Ok
import a11.Status
import a11.StatusOr
import a11.alreadyExists
import a11.concatBytes
import a11.invalidArgument
import a11.orElse
import a11.outOfRange
import a11.resourceExhausted
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Action Engine-compatible byte packet framing, byte-exact with
 * `js/src/byte_chunking.ts`. Metadata suffixes are fixed little-endian: a
 * complete packet is `[payload][transientId u64][type u8]`; a first chunk adds a
 * `packetCount u32`; later chunks add a `sequence u32`.
 */

private const val COMPLETE_METADATA_SIZE = 9
private const val CHUNK_METADATA_SIZE = 13
private const val FIRST_CHUNK_METADATA_SIZE = 17
const val MINIMUM_BYTE_PACKET_SIZE = FIRST_CHUNK_METADATA_SIZE + 1
private const val UINT32_MAX = 0xffff_ffffL

enum class BytePacketType(val value: Int) {
    COMPLETE_BYTES(0x00), BYTE_CHUNK(0x01), LENGTH_SUFFIXED_BYTE_CHUNK(0x02)
}

class BytePacket(
    val type: BytePacketType,
    val payload: ByteArray,
    val transientId: Long,
    val sequence: Int,
    val packetCount: Int,
)

private fun le(vararg parts: ByteArray): ByteArray = concatBytes(parts.toList())

private fun putU32(value: Int): ByteArray =
    ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(value).array()

private fun putU64(value: Long): ByteArray =
    ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(value).array()

private fun completePacket(payload: ByteArray, transientId: Long): ByteArray =
    le(payload, putU64(transientId), byteArrayOf(BytePacketType.COMPLETE_BYTES.value.toByte()))

private fun chunkPacket(payload: ByteArray, transientId: Long, sequence: Int, packetCount: Int?): ByteArray {
    val suffix = if (packetCount == null) {
        le(putU32(sequence), putU64(transientId), byteArrayOf(BytePacketType.BYTE_CHUNK.value.toByte()))
    } else {
        le(putU32(packetCount), putU32(sequence), putU64(transientId),
            byteArrayOf(BytePacketType.LENGTH_SUFFIXED_BYTE_CHUNK.value.toByte()))
    }
    return le(payload, suffix)
}

/** Split bytes using the fixed little-endian packet suffixes. */
fun splitBytesIntoPackets(bytes: ByteArray, transientId: Long, packetSize: Int = 64 * 1024): StatusOr<List<ByteArray>> {
    if (packetSize < MINIMUM_BYTE_PACKET_SIZE) {
        return invalidArgument("packetSize must be an integer of at least $MINIMUM_BYTE_PACKET_SIZE.")
    }
    if (bytes.size <= packetSize - COMPLETE_METADATA_SIZE) {
        return Ok(listOf(completePacket(bytes, transientId)))
    }
    val firstPayloadSize = packetSize - FIRST_CHUNK_METADATA_SIZE
    val laterPayloadSize = packetSize - CHUNK_METADATA_SIZE
    val laterCount = Math.ceil((bytes.size - firstPayloadSize).toDouble() / laterPayloadSize).toInt()
    if (laterCount >= UINT32_MAX) return outOfRange("Byte message requires too many packets.")
    val packetCount = laterCount + 1
    val packets = ArrayList<ByteArray>(packetCount)
    packets.add(chunkPacket(bytes.copyOfRange(0, firstPayloadSize), transientId, 0, packetCount))
    var offset = firstPayloadSize
    var sequence = 1
    while (offset < bytes.size) {
        val end = minOf(bytes.size, offset + laterPayloadSize)
        packets.add(chunkPacket(bytes.copyOfRange(offset, end), transientId, sequence, null))
        offset = end
        sequence++
    }
    return Ok(packets)
}

/** Parse one byte packet without retaining its input buffer. */
fun parseBytePacket(packet: ByteArray): StatusOr<BytePacket> {
    if (packet.size < COMPLETE_METADATA_SIZE) return invalidArgument("Byte packet is shorter than complete-packet metadata.")
    val rawType = packet[packet.size - 1].toInt() and 0xff
    val type = BytePacketType.entries.firstOrNull { it.value == rawType }
        ?: return invalidArgument("Byte packet has an unknown type.")
    val metadataSize = when (type) {
        BytePacketType.COMPLETE_BYTES -> COMPLETE_METADATA_SIZE
        BytePacketType.BYTE_CHUNK -> CHUNK_METADATA_SIZE
        BytePacketType.LENGTH_SUFFIXED_BYTE_CHUNK -> FIRST_CHUNK_METADATA_SIZE
    }
    if (packet.size < metadataSize) return invalidArgument("Byte packet is shorter than its declared metadata.")
    val view = ByteBuffer.wrap(packet).order(ByteOrder.LITTLE_ENDIAN)
    val transientOffset = packet.size - COMPLETE_METADATA_SIZE
    var sequence = 0
    var packetCount = 0
    if (type != BytePacketType.COMPLETE_BYTES) {
        val sequenceOffset = transientOffset - 4
        sequence = view.getInt(sequenceOffset)
        if (type == BytePacketType.LENGTH_SUFFIXED_BYTE_CHUNK) {
            packetCount = view.getInt(sequenceOffset - 4)
            if (sequence != 0 || packetCount == 0) {
                return invalidArgument("First byte chunk must have sequence zero and a positive count.")
            }
        }
    }
    val transientId = view.getLong(transientOffset)
    return Ok(BytePacket(type, packet.copyOfRange(0, packet.size - metadataSize), transientId, sequence, packetCount))
}

/** Bounded out-of-order and interleaved byte-message reassembly. */
class ByteReassembler(
    private val packetSize: Int = 64 * 1024,
    private val maxMessageSize: Int = 32 * 1024 * 1024,
    private val maxPendingMessages: Int = 64,
    private val maxPendingBytes: Int = 64 * 1024 * 1024,
) {
    private class Pending {
        var packetCount: Int? = null
        val chunks = HashMap<Int, ByteArray>()
        var byteCount = 0
    }

    private val pending = HashMap<Long, Pending>()
    private var pendingBytes = 0

    fun clear() { pending.clear(); pendingBytes = 0 }

    /** Admit one packet and return a full message when this completes one. */
    fun feed(serialized: ByteArray): StatusOr<ByteArray?> {
        if (serialized.size > packetSize) return outOfRange("Incoming byte packet exceeds packetSize.")
        val packet = parseBytePacket(serialized).orElse { return it }
        if (packet.payload.size > maxMessageSize) return outOfRange("Incoming byte message exceeds its limit.")
        if (packet.type == BytePacketType.COMPLETE_BYTES) {
            if (pending.containsKey(packet.transientId)) return alreadyExists("Complete byte packet collides with pending chunks.")
            return Ok(packet.payload)
        }
        if (pendingBytes + packet.payload.size > maxPendingBytes) return resourceExhausted("Pending byte chunks exceed maxPendingBytes.")
        val entry = pending.getOrPut(packet.transientId) {
            if (pending.size >= maxPendingMessages) return resourceExhausted("Too many byte messages are pending reassembly.")
            Pending()
        }
        if (packet.type == BytePacketType.LENGTH_SUFFIXED_BYTE_CHUNK) {
            if (entry.packetCount != null && entry.packetCount != packet.packetCount) {
                return invalidArgument("Byte message has conflicting packet counts.")
            }
            entry.packetCount = packet.packetCount
        }
        val count = entry.packetCount
        if (count != null && packet.sequence >= count) return outOfRange("Byte chunk sequence exceeds the declared packet count.")
        if (entry.chunks.containsKey(packet.sequence)) return alreadyExists("Duplicate byte chunk sequence.")
        if (entry.byteCount + packet.payload.size > maxMessageSize) return outOfRange("Reassembled byte message exceeds maxMessageSize.")
        entry.chunks[packet.sequence] = packet.payload
        entry.byteCount += packet.payload.size
        pendingBytes += packet.payload.size
        val total = entry.packetCount ?: return Ok(null)
        if (entry.chunks.size != total) return Ok(null)
        val parts = ArrayList<ByteArray>(total)
        for (seq in 0 until total) parts.add(entry.chunks[seq] ?: return Ok(null))
        pendingBytes -= entry.byteCount
        pending.remove(packet.transientId)
        return Ok(concatBytes(parts))
    }
}
