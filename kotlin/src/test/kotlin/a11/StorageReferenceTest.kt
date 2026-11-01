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

import java.nio.file.Files
import java.nio.file.Path
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertSame
import kotlin.test.assertTrue

class StorageReferenceTest {
    @Test
    fun sharedWireFixtures() {
        var directory: Path? = Path.of("").toAbsolutePath()
        while (
            directory != null && !Files.exists(directory.resolve("testdata/node_refs.json"))
        ) directory = directory.parent
        val cases =
            A11Json.parse(Files.readString(directory!!.resolve("testdata/node_refs.json"))).value()
                as List<*>
        for (entry in cases) {
            val record = entry as Map<*, *>
            @Suppress("UNCHECKED_CAST")
            val reference =
                wireValueCodecByTag(SerialTags.NODE_REF)!!
                    .load(record["json"] as Map<String, Any?>)
                    .value() as NodeRef
            val bytes = reference.toMsgpack().value()
            assertEquals(
                record["msgpack_hex"],
                bytes.joinToString("") { "%02x".format(it.toInt() and 255) },
            )
            assertEquals(reference, NodeRef.fromMsgpack(bytes).value())
            assertEquals(
                record["json"],
                wireValueCodecByTag(SerialTags.NODE_REF)!!.dump(reference).value(),
            )
        }
    }

    @Test
    fun hostStorageResolver() {
        val first = NodeMap(StorageIdentity("first"))
        val second = NodeMap(StorageIdentity("second"))
        val host = NodeMap()
        assertTrue(host.registerStorage("first", first).ok())
        assertTrue(host.registerStorage("second", second).ok())
        assertSame(
            first.get("same").value(),
            host.resolve(NodeRef("same", storage = StorageIdentity("first"))).value(),
        )
        assertSame(
            second.get("same").value(),
            host.resolve(NodeRef("same", storage = StorageIdentity("second"))).value(),
        )
        val credentials = StorageCredentials("bearer", mapOf("token" to "test-token"))
        val identity =
            StorageIdentity(
                "dynamic",
                listOf(StorageService("stream", "https://example", credentials)),
            )
        assertTrue(
            host
                .setStorageResolver(
                    { input ->
                        assertEquals(identity, input)
                        Ok(first)
                    },
                    listOf("dynamic"),
                )
                .ok()
        )
        assertSame(
            first.get("node").value(),
            host.resolve(NodeRef("node", storage = identity)).value(),
        )
        assertFalse(identity.toString().contains("test-token"))
        assertFalse(NodeRef("node", storage = identity).toString().contains("test-token"))
    }
}
