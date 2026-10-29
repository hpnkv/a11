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

import a11.sdk.ensureInteractionCodec
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class ValidationTest {
    @Test
    fun binaryDecodersRejectInvalidRecordsAndIntegerNarrowing() {
        fun fields(vararg values: Any?) = MsgpackCodec.encodeFields(values.toList()).value()
        assertEquals(
            StatusCode.INVALID_ARGUMENT,
            Chunk.fromMsgpack(fields(null, "reference", byteArrayOf(1))).status.code,
        )
        assertEquals(
            StatusCode.INVALID_ARGUMENT,
            Port.fromMsgpack(fields("bad!", "node")).status.code,
        )
        assertEquals(
            StatusCode.INVALID_ARGUMENT,
            NodeRef.fromMsgpack(fields("node", 0xffff_ffffL, 2)).status.code,
        )
        assertEquals(
            StatusCode.INVALID_ARGUMENT,
            NodeFragment.fromMsgpack(
                    fields("bad!", 0, makeNullChunk().toMsgpack().value(), 0, false)
                )
                .status
                .code,
        )
        for (code in listOf(-1L, 17L, 0x1_0000_0000L, Long.MAX_VALUE)) {
            assertEquals(
                StatusCode.INVALID_ARGUMENT,
                decodeStatus(fields(code, "message", emptyList<Any?>())).status.code,
            )
        }
        assertEquals(
            StatusCode.INVALID_ARGUMENT,
            MsgpackCodec.decodeOne(byteArrayOf(1, 2)).status.code,
        )
        assertTrue(MsgpackCodec.decodeFields(byteArrayOf(1), 2, "pair") is Err)
    }

    @Test
    fun microsecondTimestampsSurviveBinaryJsonAndOwnedCopies() {
        val registry = SerializationRegistry(registerDefaults = true)
        for (micros in listOf(-1001L, -1L, 1L, 1234567890123456L)) {
            val metadata = ChunkMetadata("text/plain").also { it.timestampMicros = micros }
            val binary = metadata.toMsgpack().value()
            val decoded = ChunkMetadata.fromMsgpack(binary).value()
            assertEquals(micros, decoded.timestampMicros)
            assertContentEquals(binary, decoded.toMsgpack().value())
            val json = registry.toChunk(metadata).value()
            assertEquals(
                micros,
                (registry.fromChunk(json).value() as ChunkMetadata).timestampMicros,
            )
            val copied = NodeFragment(data = Chunk(metadata)).copy().getChunk().value()
            assertEquals(micros, copied.metadata!!.timestampMicros)
        }
    }

    @Test
    fun malformedTaggedModelsReturnErrorsInsteadOfDefaults() {
        ensureInteractionCodec()
        val registry = SerializationRegistry(registerDefaults = true)
        fun decode(tag: String, json: String) =
            registry.fromChunk(
                Chunk(ChunkMetadata("application/json;type=$tag"), data = utf8Encode(json))
            )
        for ((tag, json) in
            listOf(
                SerialTags.CHUNK to """{"data":"%%%"}""",
                SerialTags.CHUNK to """{"data":false}""",
                SerialTags.CHUNK_METADATA to """{"timestamp":"yesterday"}""",
                SerialTags.NODE_REF to """{"id":"node","offset":1.5}""",
                SerialTags.NODE_FRAGMENT to """{"data":false}""",
                SerialTags.PORT to """{"name":123}""",
                SerialTags.ACTION_MESSAGE to """{"inputs":false}""",
                SerialTags.WIRE_MESSAGE to """{"actions":false}""",
                SerialTags.STATUS to """{"code":4294967296,"message":"bad"}""",
                SerialTags.INTERACTION to """{"content":false}""",
                SerialTags.INTERACTION to """{"backend_specific_metadata":{"key":"%%%"}}""",
            )) assertTrue(decode(tag, json) is Err, "$tag: $json")
        assertEquals(
            Ok(emptyMap<String, Any?>()),
            registry.fromChunk(Chunk(ChunkMetadata(JSON_MIMETYPE), data = utf8Encode("{}"))),
        )
    }

    @Test
    fun literalTypeTagsAndTypedBytesRetainTheirRepresentation() {
        val registry = SerializationRegistry(registerDefaults = true)
        val literal = "example.Model+literal%20"
        val registration =
            registry.register(
                object : SerializationCodec {
                    override val tag = literal
                    override val mimetype = "application/x-example"

                    override fun test(value: Any?): Boolean = value is Port

                    override fun serialize(value: Any?): StatusOr<ByteArray> =
                        Ok(utf8Encode((value as Port).name))

                    override fun deserialize(data: ByteArray, chunk: Chunk): StatusOr<Any?> =
                        utf8Decode(data).map { Port(it) }
                }
            )
        assertTrue(registration.ok())
        val chunk = registry.toChunk(Port("port"), "application/x-example").value()
        assertEquals("application/x-example;type=$literal", chunk.mimetype)
        assertEquals(Ok(Port("port")), registry.fromChunk(chunk))
        val bytes = byteArrayOf(0, -1, 42)
        val encoded = registry.toChunk(bytes, JSON_MIMETYPE).value()
        assertContentEquals(bytes, registry.fromChunk(encoded).value() as ByteArray)
        for (mimetype in
            listOf("/json", "application/", "application /json", "application/json;a=1;a=2")) {
            assertTrue(
                registry.fromChunk(Chunk(ChunkMetadata(mimetype), data = utf8Encode("{}"))) is Err,
                mimetype,
            )
        }
        assertTrue(registry.fromChunk(Chunk(ChunkMetadata(JSON_MIMETYPE), ref = "node")) is Err)
    }

    @Test
    fun wireRecordsHaveContentEqualityAndIndependentCopies() {
        val original =
            WireMessage(
                nodeFragments =
                    mutableListOf(
                        NodeFragment(
                            "node",
                            Chunk(ChunkMetadata("text/plain"), data = byteArrayOf(1)),
                            0u,
                        )
                    ),
                actions =
                    mutableListOf(
                        ActionMessage(
                            "action",
                            "example",
                            inputs = mutableListOf(Port("input", "node")),
                        )
                    ),
                headers = linkedMapOf("header" to byteArrayOf(2)),
            )
        val copy = original.copy()
        assertEquals(original, copy)
        assertEquals(original.hashCode(), copy.hashCode())
        assertEquals(original, WireMessage.fromMsgpack(original.toMsgpack().value()).value())
        copy.nodeFragments[0].getChunk().value().data = byteArrayOf(3)
        assertTrue(original != copy)
        assertEquals(1.toByte(), original.nodeFragments[0].getChunk().value().data[0])
        assertEquals(NodeRef("node", 1u, 2uL), NodeRef("node", 1u, 2uL).copy())
    }

    @Test
    fun jsonRejectsMalformedGrammarAndRetainsLargeNumbers() {
        for (json in
            listOf(
                "01",
                "+1",
                "1.",
                "1e",
                "1e+",
                "--1",
                "1e999",
                "[1,]",
                "\u00a01",
                "\"raw\nline\"",
                "\"\\uD800\"",
                "\"\\uDC00\"",
            )) {
            assertTrue(A11Json.parse(json) is Err, json)
        }
        assertEquals(Ok("😀"), A11Json.parse("\"\\uD83D\\uDE00\""))
        assertTrue(A11Json.encodeToString("\uD800") is Err)
        assertTrue(A11Json.encodeToString(mapOf(1 to "invalid key")) is Err)
        assertEquals(Ok(1e20), A11Json.parse(A11Json.encodeToString(1e20).value()))
    }

    @Test
    fun unsignedKotlinIntegersRetainTheirValuesThroughNativeCodecs() {
        val registry = SerializationRegistry(registerDefaults = true)
        val maximum = java.math.BigInteger("18446744073709551615")
        for (mimetype in listOf(JSON_MIMETYPE, MSGPACK_MIMETYPE)) {
            assertEquals(
                4294967295L,
                registry.fromChunk(registry.toChunk(UInt.MAX_VALUE, mimetype).value()).value(),
            )
            assertEquals(
                maximum,
                registry.fromChunk(registry.toChunk(ULong.MAX_VALUE, mimetype).value()).value(),
            )
        }
        assertEquals(
            maximum,
            MsgpackCodec.decodeOne(MsgpackCodec.encodeOne(ULong.MAX_VALUE).value()).value(),
        )
        val status =
            Status(
                StatusCode.DATA_LOSS,
                "unsigned",
                listOf(mapOf("sequence" to UInt.MAX_VALUE, "count" to ULong.MAX_VALUE)),
            )
        assertEquals(status, decodeStatus(packStatus(status).value()).value().status)
    }
}
