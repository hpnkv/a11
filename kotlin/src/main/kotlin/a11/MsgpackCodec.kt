package a11

import org.msgpack.core.MessagePack
import org.msgpack.core.MessageUnpacker
import org.msgpack.value.ValueType

/**
 * A11's concatenated-MessagePack field representation.
 *
 * Every wire record (Status, Chunk, NodeFragment, WireMessage, …) is a fixed
 * number of MessagePack values written back-to-back. This mirrors
 * `js/src/msgpack_codec.ts` (`concatBytes(values.map(encode))` /
 * `decodeMulti`) and the Python `a11` implementation, so the bytes are
 * interchangeable across all three languages.
 */
object MsgpackCodec {

    /** Encode each value independently and concatenate, matching A11's layout. */
    fun encodeFields(values: List<Any?>): StatusOr<ByteArray> = noexcept("Failed to encode MessagePack.") {
        val parts = ArrayList<ByteArray>(values.size)
        for (value in values) parts.add(encodeOne(value))
        Ok(concatBytes(parts))
    }

    /** Decode exactly [count] concatenated MessagePack values. */
    fun decodeFields(bytes: ByteArray, count: Int, context: String): StatusOr<List<Any?>> {
        return try {
            MessagePack.newDefaultUnpacker(bytes).use { unpacker ->
                val values = ArrayList<Any?>(count)
                while (unpacker.hasNext()) values.add(unpackValue(unpacker))
                if (values.size != count) {
                    invalidArgument("$context contains ${values.size} MessagePack fields; expected $count.")
                } else {
                    Ok(values)
                }
            }
        } catch (error: Throwable) {
            invalidArgument("Failed to decode $context MessagePack.")
        }
    }

    /** Encode a single value (used by the MessagePack serialization codec). */
    fun encodeOne(value: Any?): ByteArray {
        MessagePack.newDefaultBufferPacker().use { packer ->
            packValue(packer, value)
            return packer.toByteArray()
        }
    }

    /** Decode a single value from a standalone MessagePack buffer. */
    fun decodeOne(bytes: ByteArray): StatusOr<Any?> = try {
        MessagePack.newDefaultUnpacker(bytes).use { Ok(unpackValue(it)) }
    } catch (error: Throwable) {
        invalidArgument("Invalid MessagePack data.")
    }

    private fun packValue(packer: org.msgpack.core.MessageBufferPacker, value: Any?) {
        when (value) {
            null -> packer.packNil()
            is Boolean -> packer.packBoolean(value)
            is Byte -> packer.packLong(value.toLong())
            is Short -> packer.packLong(value.toLong())
            is Int -> packer.packLong(value.toLong())
            is Long -> packer.packLong(value)
            is Float -> packer.packDouble(value.toDouble())
            is Double -> packer.packDouble(value)
            is String -> packer.packString(value)
            is ByteArray -> {
                packer.packBinaryHeader(value.size)
                packer.writePayload(value)
            }
            is List<*> -> {
                packer.packArrayHeader(value.size)
                for (item in value) packValue(packer, item)
            }
            is Map<*, *> -> {
                packer.packMapHeader(value.size)
                for ((k, v) in value) {
                    packValue(packer, k)
                    packValue(packer, v)
                }
            }
            else -> throw IllegalArgumentException(
                "Cannot MessagePack-encode a value of type ${value::class.java.name}.",
            )
        }
    }

    private fun unpackValue(unpacker: MessageUnpacker): Any? {
        return when (unpacker.nextFormat.valueType!!) {
            ValueType.NIL -> {
                unpacker.unpackNil(); null
            }
            ValueType.BOOLEAN -> unpacker.unpackBoolean()
            ValueType.INTEGER -> unpacker.unpackLong()
            ValueType.FLOAT -> unpacker.unpackDouble()
            ValueType.STRING -> unpacker.unpackString()
            ValueType.BINARY -> {
                val length = unpacker.unpackBinaryHeader()
                unpacker.readPayload(length)
            }
            ValueType.EXTENSION -> {
                // A11 records never carry extension types on the wire.
                val header = unpacker.unpackExtensionTypeHeader()
                unpacker.readPayload(header.length)
            }
            ValueType.ARRAY -> {
                val length = unpacker.unpackArrayHeader()
                (0 until length).map { unpackValue(unpacker) }
            }
            ValueType.MAP -> {
                val length = unpacker.unpackMapHeader()
                val result = LinkedHashMap<Any?, Any?>(length)
                repeat(length) {
                    val key = unpackValue(unpacker)
                    result[key] = unpackValue(unpacker)
                }
                result
            }
        }
    }
}
