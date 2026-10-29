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

import a11.A11Runtime
import a11.ByteMap
import a11.Deferred
import a11.Err
import a11.Ok
import a11.Status
import a11.WireMessage
import a11.aborted
import a11.cancelled
import a11.copyByteMap
import a11.decodeStatus
import a11.failedPrecondition
import a11.makeHalfCloseMessage
import a11.orElse
import a11.packStatus
import a11.unavailable
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch

/** Which side of a binary channel a [ChannelWireStream] may drive. */
enum class ChannelEndpointRole {
    CLIENT,
    SERVER,
    EITHER,
}

/**
 * Transport-adapter seam shared by WebSocket and other packet transports. [ChannelWireStream]
 * supplies byte packetization, reassembly, and the A11 half-close/abort lifecycle; the adapter owns
 * opening, packet I/O, and closure.
 */
interface BinaryChannel {
    suspend fun open(): Status

    fun isOpen(): Boolean

    suspend fun send(packet: ByteArray): Status

    fun close(): Status

    fun setCallbacks(
        onMessage: (ByteArray) -> Unit,
        onClosed: (Int, String) -> Unit,
        onError: (Status) -> Unit,
    )
}

/**
 * Adds the A11 WireStream lifecycle to a packet-oriented [BinaryChannel].
 *
 * Ported in spirit from `js/src/channel_wire_stream.ts`: each [WireMessage] is split into bounded
 * packets via [splitBytesIntoPackets] and reassembled with a [ByteReassembler]; half-close and
 * abort travel as empty header-only messages, the latter carrying [ABORT_STATUS_HEADER]. The
 * reference's microtask pumps become coroutine loops over kotlinx channels.
 */
class ChannelWireStream(
    private val channel: BinaryChannel,
    private val id: String,
    private val role: ChannelEndpointRole = ChannelEndpointRole.EITHER,
    private val splitSize: Int = 64 * 1024,
) : WireStream {

    private enum class End {
        NONE,
        HALF_CLOSE,
        ABORT,
    }

    private class Outbound(val bytes: ByteArray, val end: End)

    private val reassembler = ByteReassembler(packetSize = splitSize)
    private val outbound = Channel<Outbound>(Channel.UNLIMITED)
    private val inboundPackets = Channel<ByteArray>(Channel.UNLIMITED)

    private var onMessage: OnWireMessage? = null
    private var onDone: OnWireDone? = null
    @Volatile private var status: Status = Status.ok()
    @Volatile private var trailers: ByteMap? = null
    @Volatile private var localEnd = End.NONE
    @Volatile private var localEndSent = End.NONE
    @Volatile private var remoteHalfClosed = false
    @Volatile private var remoteAborted = false
    @Volatile private var finished = false
    private var messageId = 0L
    private val drainDone = Deferred<Status>()
    private val finishedDone = Deferred<Status>()
    private var started = false

    override fun getId(): String = id

    override fun getStatus(): Status = status

    override fun getTrailers(): ByteMap? = trailers?.let { value -> copyByteMap(value) }

    override fun send(message: WireMessage): Status {
        message.validate().orElse { status ->
            return status
        }
        var end = End.NONE
        var out = message
        if (message.isHalfClose) {
            val headers = lowercaseHeaders(message.headers)
            out = WireMessage(headers = headers)
            end = if (headers.containsKey(ABORT_STATUS_HEADER)) End.ABORT else End.HALF_CLOSE
        }
        val bytes =
            out.toMsgpack().orElse { status ->
                return status
            }
        if (remoteAborted) return failedPrecondition("The peer aborted the stream.")
        if (localEnd != End.NONE || finished)
            return failedPrecondition("This endpoint has already terminated.")
        localEnd = end
        if (end == End.ABORT) {
            val decoded =
                decodeStatus(out.headers.getValue(ABORT_STATUS_HEADER))
                    .orElse { status ->
                        return status
                    }
                    .status
            status =
                if (decoded.ok()) aborted("The stream was aborted by this endpoint.") else decoded
        }
        if (!outbound.trySend(Outbound(bytes, end)).isSuccess)
            return failedPrecondition("The outgoing queue is closed.")
        return Status.ok()
    }

    override suspend fun start(onMessage: OnWireMessage?, onDone: OnWireDone?): Status =
        begin(onMessage, onDone, accept = false)

    override suspend fun accept(onMessage: OnWireMessage?, onDone: OnWireDone?): Status =
        begin(onMessage, onDone, accept = true)

    private suspend fun begin(
        onMessage: OnWireMessage?,
        onDone: OnWireDone?,
        accept: Boolean,
    ): Status {
        if (started) return failedPrecondition("WireStream has already started.")
        if (
            (accept && role == ChannelEndpointRole.CLIENT) ||
                (!accept && role == ChannelEndpointRole.SERVER)
        ) {
            return failedPrecondition("WireStream endpoint role does not match start/accept.")
        }
        started = true
        this.onMessage = onMessage
        this.onDone = onDone
        channel.setCallbacks(
            onMessage = { packet -> inboundPackets.trySend(packet) },
            onClosed = { _, _ -> handleChannelClosed() },
            onError = { s -> finish(s) },
        )
        val opened = channel.open()
        opened.orElse { status ->
            finish(status)
            return status
        }
        A11Runtime.scope.launch { senderLoop() }
        A11Runtime.scope.launch { receiverLoop() }
        return Status.ok()
    }

    override fun halfClose(trailers: ByteMap): Status {
        if (localEnd != End.NONE || finished) return Status.ok()
        return send(makeHalfCloseMessage(lowercaseHeaders(trailers)))
    }

    override suspend fun drainOutgoingMessages(): Status {
        if (localEnd != End.HALF_CLOSE)
            return failedPrecondition("drainOutgoingMessages() requires halfClose() first.")
        if (localEndSent == End.HALF_CLOSE) return Status.ok()
        return drainDone.await()
    }

    override fun abort(status: Status): Status {
        if (status.ok()) return failedPrecondition("Abort status must be non-OK.")
        if (localEnd != End.NONE || finished) return Status.ok()
        val packed =
            packStatus(status).orElse { status ->
                return status
            }
        return send(makeHalfCloseMessage(ByteMap().apply { put(ABORT_STATUS_HEADER, packed) }))
    }

    override suspend fun wait(): Status = finishedDone.await()

    private suspend fun senderLoop() {
        for (out in outbound) {
            if (finished) break
            val packets =
                splitBytesIntoPackets(out.bytes, (messageId++).toULong(), splitSize).orElse { status
                    ->
                    finish(status)
                    return
                }
            for (packet in packets) {
                val sent = channel.send(packet)
                sent.orElse { status ->
                    finish(status)
                    return
                }
            }
            if (out.end != End.NONE) {
                localEndSent = out.end
                if (out.end == End.HALF_CLOSE) drainDone.resolve(Status.ok())
                maybeFinish()
                return
            }
        }
    }

    private suspend fun receiverLoop() {
        for (packet in inboundPackets) {
            if (finished) break
            val complete =
                reassembler.feed(packet).orElse { status ->
                    finish(status)
                    return
                } ?: continue
            val message =
                WireMessage.fromMsgpack(complete).orElse { status ->
                    finish(status)
                    return
                }
            if (!message.isHalfClose) {
                if (remoteHalfClosed || remoteAborted) {
                    finish(failedPrecondition("Peer sent data after a terminal message."))
                    return
                }
                val delivered = invokeMessage(message)
                delivered.orElse { status ->
                    abort(status)
                    return
                }
                continue
            }
            val headers = lowercaseHeaders(message.headers)
            val abortBytes = headers[ABORT_STATUS_HEADER]
            if (abortBytes != null) {
                val decoded = decodeStatus(abortBytes)
                remoteAborted = true
                trailers = null
                if (status.ok())
                    status =
                        when (decoded) {
                            is Ok ->
                                if (decoded.value.status.ok()) aborted(decoded.value.status.message)
                                else decoded.value.status
                            is Err -> decoded.status
                        }
                finish(null)
                return
            }
            remoteHalfClosed = true
            trailers = headers
            val delivered = invokeMessage(null)
            delivered.orElse { status ->
                abort(status)
                return
            }
            maybeFinish()
            if (finished) return
        }
    }

    private suspend fun invokeMessage(message: WireMessage?): Status =
        try {
            onMessage?.invoke(message) ?: Status.ok()
        } catch (error: Exception) {
            Status.fromException(error)
        }

    private fun handleChannelClosed() {
        if (finished) return
        val expected =
            remoteAborted ||
                localEndSent == End.ABORT ||
                (localEndSent == End.HALF_CLOSE && remoteHalfClosed)
        if (expected) maybeFinish()
        else finish(unavailable("Channel closed before A11 termination."))
    }

    private fun maybeFinish() {
        if (
            remoteAborted ||
                localEndSent == End.ABORT ||
                (localEndSent == End.HALF_CLOSE && remoteHalfClosed)
        )
            finish(null)
    }

    @Synchronized
    private fun finish(terminal: Status?) {
        if (finished) return
        finished = true
        if (terminal != null && status.ok()) status = terminal
        if (!drainDone.settled)
            drainDone.resolve(
                if (status.ok()) failedPrecondition("Finished before drain.") else status
            )
        outbound.close()
        inboundPackets.close()
        channel.close()
        A11Runtime.scope.launch {
            try {
                val completed = onDone?.invoke() ?: Status.ok()
                status = status.updated(completed)
            } catch (error: CancellationException) {
                status = status.updated(cancelled("WireStream completion callback was cancelled."))
                throw error
            } catch (error: Exception) {
                status = status.updated(Status.fromException(error))
            } finally {
                finishedDone.resolve(status)
            }
        }
    }

    private fun lowercaseHeaders(headers: ByteMap): ByteMap {
        val result = ByteMap()
        for ((k, v) in headers) result[k.lowercase()] = v.copyOf()
        return result
    }
}
