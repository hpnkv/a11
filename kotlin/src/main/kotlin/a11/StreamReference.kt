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

/** Locate an auxiliary producer independently of its stream payload. */
data class StreamReference(
    val producerId: String,
    val parentProducerId: String,
    val node: NodeRef,
    val purpose: String,
    val source: Map<String, Any?>,
) {
    internal fun toChunk(): StatusOr<Chunk> {
        if (producerId.isEmpty() || purpose.isEmpty())
            return invalidArgument("Stream producer and purpose are required.")
        node.validate().orElse {
            return it
        }
        val nodeFields =
            wireValueCodecByTag(SerialTags.NODE_REF)!!.dump(node).orElse {
                return it
            }
        val encoded =
            A11Json.encodeToString(
                    mapOf(
                        "producer_id" to producerId,
                        "parent_producer_id" to parentProducerId,
                        "node" to nodeFields,
                        "purpose" to purpose,
                        "source" to source,
                    )
                )
                .orElse {
                    return it
                }
        val bytes = utf8Encode(encoded)
        if (bytes.size > 16384) return resourceExhausted("Stream reference exceeds 16384 bytes.")
        return Ok(
            Chunk(
                metadata =
                    ChunkMetadata(
                        mimetype = "application/json",
                        attributes =
                            ByteMap().apply { put("a11-stream-reference", utf8Encode("1")) },
                    ),
                data = bytes,
            )
        )
    }

    companion object {
        internal fun fromValue(value: Any?): StatusOr<StreamReference> {
            val fields = value as? Map<*, *> ?: return invalidArgument("Invalid stream reference.")
            val producer =
                fields["producer_id"] as? String ?: return invalidArgument("Invalid producer id.")
            val purpose =
                fields["purpose"] as? String ?: return invalidArgument("Invalid stream purpose.")
            val parent = fields["parent_producer_id"] ?: ""
            if (parent !is String) return invalidArgument("Invalid parent producer id.")
            val sourceFields =
                fields["source"] as? Map<*, *> ?: return invalidArgument("Invalid stream source.")
            val source = LinkedHashMap<String, Any?>()
            for ((key, item) in sourceFields) {
                source[key as? String ?: return invalidArgument("Invalid source field.")] = item
            }
            val rawNode =
                fields["node"] as? Map<*, *> ?: return invalidArgument("Invalid node reference.")
            val nodeFields = LinkedHashMap<String, Any?>()
            for ((key, item) in rawNode) {
                nodeFields[
                    key as? String ?: return invalidArgument("Invalid node reference field.")] =
                    item
            }
            val node =
                wireValueCodecByTag(SerialTags.NODE_REF)!!.load(nodeFields).orElse {
                    return it
                }
            if (node !is NodeRef) return invalidArgument("Invalid node reference.")
            node.validate().orElse {
                return it
            }
            return Ok(StreamReference(producer, parent, node, purpose, source))
        }
    }
}
