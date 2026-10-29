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

import a11.net.ABORT_STATUS_HEADER
import a11.net.BinaryChannel
import a11.net.ChannelEndpointRole
import a11.net.ChannelWireStream
import a11.net.InProcessWireStream
import a11.net.splitBytesIntoPackets
import java.util.concurrent.atomic.AtomicInteger
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout

class LifecycleTest {
    @Test
    fun inProcessDeliveryOwnsTheMessageAndWaitIncludesTheDoneCallback() = runBlocking {
        withNativeTimeout(5000) {
            val (left, right) = InProcessWireStream.createPair()
            val seen = CompletableDeferred<WireMessage>()
            val releaseDone = CompletableDeferred<Unit>()
            val enteredDone = CompletableDeferred<Unit>()
            val completed = AtomicInteger()
            val original =
                WireMessage(
                    nodeFragments =
                        mutableListOf(
                            NodeFragment("node", Chunk(data = utf8Encode("owned")), 0u, false)
                        )
                )
            assertTrue(left.send(original).ok())
            original.nodeFragments[0].getChunk().value().data.fill(0)
            original.nodeFragments.clear()
            assertTrue(
                left
                    .start(
                        onDone = {
                            completed.incrementAndGet()
                            Status.ok()
                        }
                    )
                    .ok()
            )
            assertTrue(
                right
                    .accept(
                        onMessage = {
                            if (it != null) seen.complete(it)
                            Status.ok()
                        },
                        onDone = {
                            enteredDone.complete(Unit)
                            releaseDone.await()
                            completed.incrementAndGet()
                            Status.ok()
                        },
                    )
                    .ok()
            )
            assertEquals(
                "owned",
                utf8Decode(seen.await().nodeFragments.single().getChunk().value().data).value(),
            )
            assertEquals(StatusCode.FAILED_PRECONDITION, right.accept().code)
            left.halfClose()
            right.halfClose()
            enteredDone.await()
            val waiting = async(start = CoroutineStart.UNDISPATCHED) { right.wait() }
            assertFalse(waiting.isCompleted)
            releaseDone.complete(Unit)
            assertTrue(left.wait().ok())
            assertTrue(right.wait().ok())
            assertEquals(2, completed.get())
            assertEquals(StatusCode.FAILED_PRECONDITION, left.send(original).code)
        }
    }

    @Test
    fun callbackFailuresAbortThePeerAndDoneFailuresAreReported() = runBlocking {
        withNativeTimeout(5000) {
            val (left, right) = InProcessWireStream.createPair()
            val failure = permissionDenied("callback refused")
            left.start()
            right.accept(onMessage = { if (it == null) Status.ok() else failure })
            left.send(WireMessage(nodeFragments = mutableListOf(NodeFragment("node"))))
            assertEquals(failure, left.wait())
            assertEquals(failure, right.wait())
            val (a, b) = InProcessWireStream.createPair()
            val doneFailure = dataLoss("done failed")
            a.start(onDone = { doneFailure })
            b.accept()
            a.halfClose()
            b.halfClose()
            assertEquals(doneFailure, a.wait())
            assertTrue(b.wait().ok())
        }
    }

    @Test
    fun sessionAbortPreservesLocalStatusAndReportsNativePeerAbort() = runBlocking {
        withNativeTimeout(5000) {
            val (left, right) = InProcessWireStream.createPair()
            val local = Session(id = "local")
            val remote = Session(id = "remote")
            local.addStream(left)
            remote.addStream(right, StreamMode.ACCEPT)
            val failed = resourceExhausted("session quota")
            assertTrue(local.abort(failed).ok())
            assertEquals(failed, local.getStatus())
            val peerStatus = aborted("Session has aborted its streams")
            assertEquals(peerStatus, remote.done().await())
            assertEquals(peerStatus, remote.getStatus())
        }
    }

    @Test
    fun channelCallbackAndLocalAbortKeepTheOriginalStatus() = runBlocking {
        withNativeTimeout(5000) {
            val channel = TestChannel()
            val stream = ChannelWireStream(channel, "channel", ChannelEndpointRole.CLIENT)
            assertEquals(StatusCode.FAILED_PRECONDITION, stream.accept().code)
            val failure = dataLoss("consumer failed")
            val done = AtomicInteger()
            assertTrue(
                stream
                    .start(
                        onMessage = { failure },
                        onDone = {
                            done.incrementAndGet()
                            Status.ok()
                        },
                    )
                    .ok()
            )
            val message = WireMessage(nodeFragments = mutableListOf(NodeFragment("node")))
            for (packet in splitBytesIntoPackets(message.toMsgpack().value(), 1uL).value()) channel
                .deliver(packet)
            assertEquals(failure, stream.wait())
            assertFalse(channel.isOpen())
            assertEquals(1, done.get())
            val decoded = a11.net.ByteReassembler()
            var abort: WireMessage? = null
            for (packet in channel.sent) decoded.feed(packet).value()?.let {
                abort = WireMessage.fromMsgpack(it).value()
            }
            assertEquals(
                failure,
                decodeStatus(abort!!.headers.getValue(ABORT_STATUS_HEADER)).value().status,
            )
        }
    }

    @Test
    fun cancellingAnActionCancelsItsHandlerAndFinalizesItsOutputs() = runBlocking {
        withNativeTimeout(5000) {
            val entered = CompletableDeferred<Unit>()
            val released = CompletableDeferred<Unit>()
            val schema =
                ActionSchema(
                    "cancelled_handler",
                    outputs = linkedMapOf("out" to ActionPortSchema("out", TEXT_MIMETYPE)),
                )
            val source = schema.copy()
            val nodeMap = NodeMap()
            val action =
                Action.create(
                        schema,
                        ActionCreateOptions(
                            nodeMap = nodeMap,
                            settings =
                                ActionSettings(
                                    clearInputsAfterRun = true,
                                    clearOutputsAfterRun = true,
                                ),
                            handler = {
                                try {
                                    entered.complete(Unit)
                                    awaitCancellation()
                                } finally {
                                    released.complete(Unit)
                                }
                            },
                        ),
                    )
                    .value()
            schema.outputs.clear()
            assertEquals(source.outputs.keys, action.schema.outputs.keys)
            val output = action.getOutput("out").value()
            val callbacks = AtomicInteger()
            action.setOnCancelled {
                callbacks.incrementAndGet()
                Status.ok()
            }
            action.run().value()
            entered.await()
            action.cancel()
            action.cancel()
            assertEquals(StatusCode.CANCELLED, action.wait().status.code)
            released.await()
            assertTrue(action.isDone)
            assertTrue(action.isCancelled)
            assertEquals(1, callbacks.get())
            assertEquals(StatusCode.CANCELLED, output.next().status.code)
            assertEquals(0, nodeMap.size)
        }
    }

    @Test
    fun cancelledDoneCallbacksStillReleaseWaiters() = runBlocking {
        withNativeTimeout(5000) {
            val (left, right) = InProcessWireStream.createPair()
            left.start(
                onDone = { throw kotlinx.coroutines.CancellationException("cancelled callback") }
            )
            right.accept()
            left.halfClose()
            right.halfClose()
            assertEquals(StatusCode.CANCELLED, left.wait().code)
            assertTrue(right.wait().ok())
            val channel = TestChannel()
            val stream = ChannelWireStream(channel, "cancelled-done")
            stream.start(
                onDone = { throw kotlinx.coroutines.CancellationException("cancelled callback") }
            )
            val failure = dataLoss("prior failure")
            stream.abort(failure)
            assertEquals(failure, stream.wait())
        }
    }

    @Test
    fun cancellationCallbackFailuresDoNotPreventCancellation() = runBlocking {
        val action = Action.create(ActionSchema("cancel_callback")).value()
        val failure = permissionDenied("callback failure")
        action.setOnCancelled { failure }
        assertEquals(failure, action.cancel())
        assertEquals(StatusCode.CANCELLED, action.wait().status.code)
        assertTrue(action.isDone)
    }

    @Test
    fun competingCancellationInvokesCallbacksOnce() = runBlocking {
        val action = Action.create(ActionSchema("competing_cancel")).value()
        val calls = AtomicInteger()
        val start = CompletableDeferred<Unit>()
        action.setOnCancelled {
            calls.incrementAndGet()
            Status.ok()
        }
        val jobs =
            List(64) {
                async(kotlinx.coroutines.Dispatchers.Default) {
                    start.await()
                    action.cancel()
                }
            }
        start.complete(Unit)
        for (job in jobs) assertTrue(job.await().ok())
        assertEquals(1, calls.get())
        assertEquals(StatusCode.CANCELLED, action.wait().status.code)
    }

    private class TestChannel : BinaryChannel {
        private var opened = false
        private var onMessage: (ByteArray) -> Unit = {}
        val sent = java.util.concurrent.CopyOnWriteArrayList<ByteArray>()

        override suspend fun open(): Status {
            opened = true
            return Status.ok()
        }

        override fun isOpen(): Boolean = opened

        override suspend fun send(packet: ByteArray): Status {
            sent.add(packet)
            return Status.ok()
        }

        override fun close(): Status {
            opened = false
            return Status.ok()
        }

        override fun setCallbacks(
            onMessage: (ByteArray) -> Unit,
            onClosed: (Int, String) -> Unit,
            onError: (Status) -> Unit,
        ) {
            this.onMessage = onMessage
        }

        fun deliver(packet: ByteArray) {
            onMessage(packet)
        }
    }
}

private suspend fun <T> withNativeTimeout(
    timeoutMs: Long,
    block: suspend kotlinx.coroutines.CoroutineScope.() -> T,
): T =
    try {
        withTimeout(timeoutMs, block)
    } catch (timeout: kotlinx.coroutines.TimeoutCancellationException) {
        System.err.println(Debug.fiberReport())
        throw timeout
    }
