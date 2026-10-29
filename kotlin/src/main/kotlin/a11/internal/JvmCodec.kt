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

package a11.internal

import a11.NativeObject
import a11.Ok
import a11.Status
import a11.StatusOr
import a11.durationFields
import a11.invalidArgument
import a11.orElse
import a11.outOfRange
import a11.value
import java.math.BigInteger
import org.msgpack.core.MessagePack
import org.msgpack.core.MessageUnpacker
import org.msgpack.value.ValueType

/** Standard MessagePack transport for JNI requests and responses. */
internal object JvmCodec {
    /** Encode a JNI envelope. */
    fun encodeOne(value: Any?): StatusOr<ByteArray> {
        MessagePack.newDefaultBufferPacker().use { packer ->
            val status = packValue(packer, value)
            status.orElse { failure ->
                return failure
            }
            return Ok(packer.toByteArray())
        }
    }

    /** Decode a single value from a standalone MessagePack buffer. */
    fun decodeOne(bytes: ByteArray): StatusOr<Any?> =
        try {
            MessagePack.newDefaultUnpacker(bytes).use { unpacker ->
                val value =
                    unpackValue(unpacker).orElse { status ->
                        return status
                    }
                if (unpacker.hasNext()) invalidArgument("Trailing MessagePack data.") else Ok(value)
            }
        } catch (error: Exception) {
            if (error is java.util.concurrent.CancellationException) throw error
            invalidArgument("Invalid MessagePack data.")
        }

    private fun packValue(packer: org.msgpack.core.MessageBufferPacker, value: Any?): Status {
        when (value) {
            is kotlin.time.Duration -> return packValue(packer, durationFields(value))
            is NativeObject -> return packValue(packer, mapOf("handle" to value.nativeId))
            null -> packer.packNil()
            is Boolean -> packer.packBoolean(value)
            is Byte -> packer.packLong(value.toLong())
            is Short -> packer.packLong(value.toLong())
            is Int -> packer.packLong(value.toLong())
            is Long -> packer.packLong(value)
            is UByte -> packer.packLong(value.toLong())
            is UShort -> packer.packLong(value.toLong())
            is UInt -> packer.packLong(value.toLong())
            is ULong ->
                if (value <= Long.MAX_VALUE.toULong()) packer.packLong(value.toLong())
                else packer.packBigInteger(BigInteger(value.toString()))
            is Float -> packer.packDouble(value.toDouble())
            is Double -> packer.packDouble(value)
            is String -> {
                var offset = 0
                while (offset < value.length) {
                    val char = value[offset++]
                    if (char.isHighSurrogate()) {
                        if (offset == value.length || !value[offset++].isLowSurrogate()) {
                            return invalidArgument("Unpaired UTF-16 surrogate in JVM string")
                        }
                    } else if (char.isLowSurrogate()) {
                        return invalidArgument("Unpaired UTF-16 surrogate in JVM string")
                    }
                }
                packer.packString(value)
            }
            is BigInteger -> {
                if (value.bitLength() > (if (value.signum() < 0) 63 else 64)) {
                    return outOfRange("Integer exceeds the native 64-bit representation")
                }
                packer.packBigInteger(value)
            }
            is ByteArray -> {
                packer.packBinaryHeader(value.size)
                packer.writePayload(value)
            }
            is List<*> -> {
                packer.packArrayHeader(value.size)
                for (item in value) {
                    val status = packValue(packer, item)
                    status.orElse { status ->
                        return status
                    }
                }
            }
            is Map<*, *> -> {
                packer.packMapHeader(value.size)
                for ((k, v) in value) {
                    if (k !is String) return invalidArgument("Object keys must be strings")
                    val keyStatus = packValue(packer, k)
                    keyStatus.orElse { status ->
                        return status
                    }
                    val valueStatus = packValue(packer, v)
                    valueStatus.orElse { status ->
                        return status
                    }
                }
            }
            else -> return invalidArgument("Unsupported JVM value type: ${value::class.java.name}")
        }
        return Status.ok()
    }

    private fun unpackValue(unpacker: MessageUnpacker): StatusOr<Any?> {
        return when (unpacker.nextFormat.valueType!!) {
            ValueType.NIL -> {
                unpacker.unpackNil()
                Ok(null)
            }
            ValueType.BOOLEAN -> Ok(unpacker.unpackBoolean())
            ValueType.INTEGER ->
                Ok(
                    unpacker.unpackBigInteger().let { value ->
                        if (value.bitLength() <= 63) value.toLong() else value
                    }
                )
            ValueType.FLOAT -> Ok(unpacker.unpackDouble())
            ValueType.STRING -> Ok(unpacker.unpackString())
            ValueType.BINARY -> Ok(unpacker.readPayload(unpacker.unpackBinaryHeader()))
            ValueType.EXTENSION ->
                invalidArgument("Unsupported MessagePack extension in JNI envelope")
            ValueType.ARRAY -> {
                val size = unpacker.unpackArrayHeader()
                val values = ArrayList<Any?>(size)
                repeat(size) {
                    values.add(
                        unpackValue(unpacker).orElse { status ->
                            return status
                        }
                    )
                }
                Ok(values)
            }
            ValueType.MAP -> {
                val size = unpacker.unpackMapHeader()
                val fields = LinkedHashMap<Any?, Any?>(size)
                repeat(size) {
                    val key =
                        unpackValue(unpacker).orElse { status ->
                            return status
                        }
                    fields[key] =
                        unpackValue(unpacker).orElse { status ->
                            return status
                        }
                }
                Ok(fields)
            }
        }
    }
}
