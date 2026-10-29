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

import a11.flow.Flow
import a11.internal.NativeBridge
import a11.net.WebSocketWireServer
import a11.net.WebSocketWireStream
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.yield

class NativeBindingTest {

    @Test
    fun movedStoreWritesKeepTheirNativePayloadAndAllowSourceReuse() = runBlocking {
        withTimeout(5000) {
            val store = LocalChunkStore.create("moved-store").value()
            val fragment =
                NodeFragment("moved-store", Chunk(data = utf8Encode("moved")), 0u, continued = true)
            val expected = fragment.copy()
            assertEquals(0L, store.putMoved(fragment).value())
            assertEquals(expected, store.get(0).value())
            fragment.copyFrom(NodeFragment("moved-store", Chunk(data = utf8Encode("reused")), 1u))
            assertEquals(1L, store.put(fragment).value())
            assertEquals(fragment, store.get(1).value())
        }
    }

    @Test
    fun aReturnedServerCallbackRetainsAnEscapedStreamWrapper() = runBlocking {
        withTimeout(5000) {
            val captured = CompletableDeferred<WebSocketWireStream>()
            val server =
                WebSocketWireServer.create { stream ->
                        captured.complete(stream)
                        stream.accept().orElse { status ->
                            return@create status
                        }
                        dataLoss("handler returned")
                    }
                    .value()
            try {
                val client =
                    WebSocketWireStream.connect("ws://127.0.0.1:${server.port.value()}/a11").value()
                client.start()
                val retained = captured.await()
                assertFalse(client.wait().ok())
                assertTrue(
                    retained.nativeId > 0,
                    "Returning from a callback must retain escaped wrappers",
                )
                assertTrue(retained.getId().isNotEmpty())
            } finally {
                server.stop()
            }
        }
    }

    @Test
    fun nativeLibraryAndLanguageServiceAreAvailable() {
        assertEquals(
            System.getProperty("a11.test.version"),
            NativeBridge.call("bridge.version").value(),
        )
        assertTrue(NativeBridge.call("bridge.operations").value() is List<*>)
        assertEquals(true, Flow.check("flow example {}").value()["ok"])
        Flow.compile("flow example {}").value().withReleasedNative { assertTrue(it.plan().ok()) }
    }

    @Test
    fun nativeStoreAndReaderDeliverWithoutJvmBlocking() = runBlocking {
        withTimeout(5000) {
            LocalChunkStore.create("jni-store").value().withReleasedNative { store ->
                val original =
                    NodeFragment(
                        "jni-store",
                        Chunk(ChunkMetadata("text/plain"), data = utf8Encode("native")),
                        0u,
                        false,
                    )
                assertEquals(0L, store.put(original).value())
                assertEquals(original, store.get(0).value())
                ChunkStoreReader.create(store).value().withReleasedNative { reader ->
                    assertEquals(original, reader.next().value())
                    assertEquals(null, reader.next().value())
                }
            }
        }
    }

    @Test
    fun nativeSessionEchoPreservesMessagesAndCompletesOnHalfClose() = runBlocking {
        withTimeout(5000) {
            val serverFinished = CompletableDeferred<Status>()
            WebSocketWireServer.create(path = "/ws") { stream ->
                    SessionWithRecv.create().value().withReleasedNative { session ->
                        var status = session.addStream(stream, StreamMode.ACCEPT)
                        if (!status.ok()) return@create status
                        while (true) {
                            val received =
                                session.receive().orElse { status ->
                                    return@create status
                                } ?: break
                            status = session.send(received)
                            if (!status.ok()) return@create status
                        }
                        status = session.halfClose()
                        if (status.ok()) status = session.awaitCompletion()
                        serverFinished.complete(status)
                        status
                    }
                }
                .value()
                .withReleasedNative { server ->
                    try {
                        SessionWithRecv.create().value().withReleasedNative { session ->
                            WebSocketWireStream.connect(
                                    "ws://127.0.0.1:${server.port.value()}/ws",
                                    mapOf("hello" to "world"),
                                )
                                .value()
                                .withReleasedNative { stream ->
                                    assertTrue(session.addStream(stream).ok())
                                    val message =
                                        WireMessage(
                                            nodeFragments =
                                                mutableListOf(
                                                    NodeFragment(
                                                        "test",
                                                        Chunk(
                                                            ChunkMetadata("text/plain"),
                                                            data = utf8Encode("native\u0000echo"),
                                                        ),
                                                    )
                                                )
                                        )
                                    assertTrue(session.send(message).ok())
                                    assertEquals(message, session.receive().value())
                                    assertTrue(session.halfClose().ok())
                                    assertEquals(null, session.receive().value())
                                    assertTrue(session.awaitCompletion().ok())
                                    assertTrue(serverFinished.await().ok())
                                }
                        }
                    } finally {
                        assertTrue(server.stop().ok())
                    }
                }
        }
    }

    @Test
    fun nativeFutureSupportsSeveralWaitersAndRepeatedObservation() = runBlocking {
        withTimeout(5000) {
            LocalChunkStore.create("shared-result").value().withReleasedNative { store ->
                val id =
                    NativeBridge.handle(
                        NativeBridge.call("store.get", store.nativeId, mapOf("seq" to 0)).value()
                    )
                NativeFuture(id, { a11.internal.nativeFragment(it) }).withReleasedNative { future ->
                    assertFalse(future.isReady.value())
                    val waiters =
                        List(16) { async(start = CoroutineStart.UNDISPATCHED) { future.await() } }
                    val fragment =
                        NodeFragment(
                            "shared-result",
                            Chunk(data = utf8Encode("payload")),
                            0u,
                            false,
                        )
                    assertEquals(0L, store.put(fragment).value())
                    for (result in waiters.awaitAll()) assertEquals(fragment, result.value())
                    assertTrue(future.isReady.value())
                    assertEquals(fragment, future.await().value())
                }
            }
        }
    }

    @Test
    fun cancellingJvmWaitDoesNotInvalidateStoreOrItsOtherObservers() = runBlocking {
        withTimeout(5000) {
            LocalChunkStore.create("cancellation").value().withReleasedNative { store ->
                val waiting = launch(start = CoroutineStart.UNDISPATCHED) { store.get(0) }
                yield()
                waiting.cancelAndJoin()
                assertTrue(waiting.isCancelled)
                val fragment =
                    NodeFragment(
                        "cancellation",
                        Chunk(data = utf8Encode("after cancel")),
                        0u,
                        false,
                    )
                assertTrue(store.put(fragment).ok())
                assertEquals(fragment, store.get(0).value())
            }
        }
    }

    @Test
    fun releasingNativeReferenceDoesNotCancelSharedOwner() = runBlocking {
        withTimeout(5000) {
            val stores = a11.internal.NativeControl.storeHandleCount()
            val readers = a11.internal.NativeControl.readerHandleCount()
            val store = LocalChunkStore.create("owned-store").value()
            val reader = ChunkStoreReader.create(store).value()
            val fragment = NodeFragment("owned-store", Chunk(data = utf8Encode("owned")), 0u, false)
            assertTrue(store.put(fragment).ok())
            store.release()
            store.release()
            assertEquals(StatusCode.FAILED_PRECONDITION, store.getId().status.code)
            reader.withReleasedNative { assertEquals(fragment, it.next().value()) }
            assertEquals(stores, a11.internal.NativeControl.storeHandleCount())
            assertEquals(readers, a11.internal.NativeControl.readerHandleCount())
        }
    }

    @Test
    fun statusJsonRetainsShapesAndWireDecoderEnforcesNativeObjectDetails() {
        ensureCoreWireValues()
        val details = listOf(false, null, "text", listOf(1L), mapOf("reason" to "denied"))
        val original = Status(StatusCode.PERMISSION_DENIED, "native details", details)
        assertEquals(
            StatusCode.INVALID_ARGUMENT,
            decodeStatus(packStatus(original).value()).status.code,
        )
        val objectDetails =
            Status(
                StatusCode.PERMISSION_DENIED,
                "native details",
                listOf(mapOf("reason" to "denied")),
            )
        assertEquals(objectDetails, decodeStatus(packStatus(objectDetails).value()).value().status)
        val registry = SerializationRegistry(registerDefaults = true)
        assertEquals(original, registry.fromChunk(registry.toChunk(original).value()).value())
    }

    @Test
    fun managedJvmLifetimeReleasesReferencesWithoutOwnershipSyntax() = runBlocking {
        withTimeout(5000) {
            lateinit var weakStore: java.lang.ref.WeakReference<LocalChunkStore>
            val fragment =
                NodeFragment("managed-lifetime", Chunk(data = utf8Encode("retained")), 0u, false)
            suspend fun makeReader(): ChunkStoreReader {
                val store = LocalChunkStore.create("managed-lifetime").value()
                weakStore = java.lang.ref.WeakReference(store)
                store.put(fragment).orElse { status -> error("test fixture failed: $status") }
                return ChunkStoreReader.create(store).value()
            }
            val reader = makeReader()
            try {
                while (weakStore.get() != null) {
                    System.gc()
                    kotlinx.coroutines.delay(10)
                }
                assertEquals(fragment, reader.next().value())
            } finally {
                reader.release()
            }
        }
    }
}

private inline fun <T : NativeObject, R> T.withReleasedNative(block: (T) -> R): R =
    try {
        block(this)
    } finally {
        release()
    }
