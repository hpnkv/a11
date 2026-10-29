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

import a11.ActionMessage
import a11.ByteMap
import a11.Chunk
import a11.ChunkMetadata
import a11.NodeFragment
import a11.NodeRef
import a11.Port
import a11.WireMessage
import a11.base64Decode
import a11.base64Encode
import a11.fatal
import a11.map
import a11.value
import a11.wireValueCodecFor
import java.time.Instant

/** Convert JVM-native fields to the native wire JSON representation. */
internal fun nativeWireFields(value: Any?): Any? =
    when (value) {
        is ByteArray -> base64Encode(value)
        is Instant -> value.toString()
        is Map<*, *> ->
            value.entries.associate { (key, item) ->
                (key as? String ?: fatal("Wire field name must be text")) to nativeWireFields(item)
            }
        is List<*> -> value.map(::nativeWireFields)
        else ->
            wireValueCodecFor(value)?.let { element ->
                nativeWireFields(element.dump(value).value())
            } ?: value
    }

internal fun wireFields(value: Any): Map<String, Any?> {
    val codec = wireValueCodecFor(value) ?: fatal("Missing wire value adapter")
    return NativeBridge.fields(nativeWireFields(codec.dump(value).value()))
}

internal fun nativeChunk(value: Any?): Chunk = decodeChunk(NativeBridge.fields(value))

internal fun nativeFragment(value: Any?): NodeFragment =
    decodeNodeFragment(NativeBridge.fields(value))

internal fun nativeFragments(value: Any?): List<NodeFragment?> =
    (value as? List<*> ?: fatal("Expected native fragment list")).map { value ->
        if (value == null) null else nativeFragment(value)
    }

internal fun nativeBoolean(value: Any?): Boolean =
    value as? Boolean ?: fatal("Expected native boolean")

internal fun nativeText(value: Any?): String = value as? String ?: fatal("Expected native text")

internal fun nativeBytes(value: Any?): ByteArray =
    value as? ByteArray ?: fatal("Expected native bytes")

private fun fieldText(fields: Map<String, Any?>, name: String): String =
    fields[name]?.let(::nativeText) ?: ""

private fun fieldBytes(value: Any?): ByteArray =
    if (value == null) ByteArray(0) else base64Decode(nativeText(value)).value()

private fun fieldByteMap(value: Any?): ByteMap =
    linkedMapOf<String, ByteArray>().apply {
        if (value != null)
            for ((key, item) in NativeBridge.fields(value)) put(key, fieldBytes(item))
    }

internal fun decodeChunkMetadata(fields: Map<String, Any?>): ChunkMetadata {
    val metadata =
        ChunkMetadata(
            fieldText(fields, "mimetype"),
            attributes = fieldByteMap(fields["attributes"]),
        )
    fields["timestamp"]?.let { raw ->
        val instant = Instant.parse(nativeText(raw))
        metadata.timestampMicros =
            Math.addExact(Math.multiplyExact(instant.epochSecond, 1_000_000), instant.nano / 1000L)
    }
    return metadata
}

internal fun decodeChunk(fields: Map<String, Any?>): Chunk {
    if (fields.containsKey("handle")) return Chunk(NativeBridge.handle(fields))
    return Chunk(
        fields["metadata"]?.let { value -> decodeChunkMetadata(NativeBridge.fields(value)) },
        fieldText(fields, "ref"),
        fieldBytes(fields["data"]),
    )
}

internal fun decodeNodeRef(fields: Map<String, Any?>): NodeRef =
    NodeRef(
        fieldText(fields, "id"),
        fields["offset"]?.let { value -> NativeBridge.number(value).toUInt() } ?: 0u,
        fields["length"]?.let { value -> NativeBridge.number(value).toULong() },
    )

internal fun decodeNodeFragment(fields: Map<String, Any?>): NodeFragment {
    if (fields.containsKey("handle")) return NodeFragment(NativeBridge.handle(fields))
    val data = NativeBridge.fields(fields["data"])
    val payload =
        if (
            data.containsKey("id") &&
                !data.containsKey("data") &&
                !data.containsKey("ref") &&
                !data.containsKey("metadata")
        )
            decodeNodeRef(data)
        else decodeChunk(data)
    return NodeFragment(
        fieldText(fields, "id"),
        payload,
        fields["seq"]?.let { value -> NativeBridge.number(value).toUInt() },
        fields["continued"] == true,
    )
}

internal fun decodePort(fields: Map<String, Any?>): Port =
    Port(fieldText(fields, "name"), fieldText(fields, "id"))

internal fun decodeActionMessage(fields: Map<String, Any?>): ActionMessage =
    ActionMessage(
        fieldText(fields, "id"),
        fieldText(fields, "name"),
        (fields["inputs"] as? List<*>)
            ?.map { value -> decodePort(NativeBridge.fields(value)) }
            ?.toMutableList() ?: mutableListOf(),
        (fields["outputs"] as? List<*>)
            ?.map { value -> decodePort(NativeBridge.fields(value)) }
            ?.toMutableList() ?: mutableListOf(),
        fieldByteMap(fields["headers"]),
    )

internal fun decodeWireMessage(fields: Map<String, Any?>): WireMessage {
    if (fields.containsKey("handle")) return WireMessage(NativeBridge.handle(fields))
    return WireMessage(
        (fields["node_fragments"] as? List<*>)
            ?.map { value -> decodeNodeFragment(NativeBridge.fields(value)) }
            ?.toMutableList() ?: mutableListOf(),
        (fields["actions"] as? List<*>)
            ?.map { value -> decodeActionMessage(NativeBridge.fields(value)) }
            ?.toMutableList() ?: mutableListOf(),
        fieldByteMap(fields["headers"]),
    )
}
