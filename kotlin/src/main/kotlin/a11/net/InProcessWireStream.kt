package a11.net

import a11.ByteMap
import a11.Deferred
import a11.Ok
import a11.Status
import a11.WireMessage
import a11.A11Runtime
import a11.copyByteMap
import a11.failedPrecondition
import a11.randomId
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch

/**
 * Two directly-linked in-process [WireStream] endpoints that pass `WireMessage`s
 * without byte framing. Useful for driving the full Session/Action/SDK stack in
 * a single process (tests, and an offline plugin fallback). The observable
 * lifecycle — half-close barrier, abort, trailers, and completion — matches the
 * channel-backed transports.
 */
class InProcessWireStream private constructor(private val id: String) : WireStream {

    private sealed interface Item
    private class Data(val message: WireMessage) : Item
    private class HalfClose(val trailers: ByteMap) : Item
    private class Abort(val status: Status) : Item

    private lateinit var peer: InProcessWireStream
    private val inbox = Channel<Item>(Channel.UNLIMITED)
    private var onMessage: OnWireMessage? = null
    private var onDone: OnWireDone? = null

    private var status: Status = Status.ok()
    private var trailers: ByteMap? = null
    private var localEnded = false
    private var remoteEnded = false
    private var finished = false
    private val finishedDone = Deferred<Status>()

    override fun getId(): String = id
    override fun getStatus(): Status = status
    override fun getTrailers(): ByteMap? = trailers?.let { copyByteMap(it) }

    override fun send(message: WireMessage): Status {
        if (localEnded || finished) return failedPrecondition("This endpoint has already terminated.")
        peer.inbox.trySend(Data(message))
        return Status.ok()
    }

    override suspend fun start(onMessage: OnWireMessage?, onDone: OnWireDone?): Status = begin(onMessage, onDone)
    override suspend fun accept(onMessage: OnWireMessage?, onDone: OnWireDone?): Status = begin(onMessage, onDone)

    private fun begin(onMessage: OnWireMessage?, onDone: OnWireDone?): Status {
        this.onMessage = onMessage
        this.onDone = onDone
        A11Runtime.scope.launch { receiveLoop() }
        return Status.ok()
    }

    private suspend fun receiveLoop() {
        for (item in inbox) {
            when (item) {
                is Data -> onMessage?.invoke(item.message)
                is HalfClose -> {
                    remoteEnded = true
                    trailers = item.trailers
                    onMessage?.invoke(null)
                    maybeFinish()
                    if (finished) break
                }
                is Abort -> {
                    remoteEnded = true
                    if (status.isOk) status = item.status
                    onMessage?.invoke(null)
                    finishNow()
                    break
                }
            }
        }
    }

    override fun halfClose(trailers: ByteMap): Status {
        if (localEnded || finished) return Status.ok()
        localEnded = true
        peer.inbox.trySend(HalfClose(copyByteMap(trailers)))
        maybeFinish()
        return Status.ok()
    }

    override suspend fun drainOutgoingMessages(): Status = Status.ok()

    override fun abort(status: Status): Status {
        if (localEnded || finished) return Status.ok()
        localEnded = true
        if (this.status.isOk) this.status = status
        peer.inbox.trySend(Abort(status))
        finishNow()
        return status
    }

    override suspend fun wait(): Status = finishedDone.await()

    private fun maybeFinish() {
        if (localEnded && remoteEnded) finishNow()
    }

    private fun finishNow() {
        if (finished) return
        finished = true
        inbox.close()
        A11Runtime.scope.launch { onDone?.invoke() }
        finishedDone.resolve(status)
    }

    companion object {
        /** Create a linked client/server endpoint pair. */
        fun createPair(): Pair<InProcessWireStream, InProcessWireStream> {
            val a = InProcessWireStream(randomId("ip-"))
            val b = InProcessWireStream(randomId("ip-"))
            a.peer = b
            b.peer = a
            return a to b
        }
    }
}
