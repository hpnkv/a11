package a11.net

import a11.ByteMap
import a11.Status
import a11.WireMessage
import a11.WritableWireStream

/** Trailer/header carrying a peer's structured non-OK terminal status. */
const val ABORT_STATUS_HEADER = "x-a11-abort-status"

/** Hard ceiling for one reassembled A11 wire message (32 MiB). */
const val MAX_SINGLE_WIRE_MESSAGE_SIZE = 32 * 1024 * 1024

/** Consumes one inbound message; `null` announces the peer's half-close. */
typealias OnWireMessage = suspend (WireMessage?) -> Status

/** Runs once after both directions finish, or after an abort. */
typealias OnWireDone = suspend () -> Status

/**
 * Message-oriented, bidirectional transport shared by sessions and actions.
 *
 * Mirrors the `WireStream` contract in `js/src/wire_stream.ts`: [start]/[accept]
 * drive one endpoint; finish normally with [halfClose] + [drainOutgoingMessages],
 * or [abort] for a failed exchange. Ordered application data travels as sequenced
 * `NodeFragment`s above this layer, so the transport promises no global ordering.
 */
interface WireStream : WritableWireStream {
    override fun send(message: WireMessage): Status
    suspend fun start(onMessage: OnWireMessage? = null, onDone: OnWireDone? = null): Status
    suspend fun accept(onMessage: OnWireMessage? = null, onDone: OnWireDone? = null): Status
    fun halfClose(trailers: ByteMap = ByteMap()): Status
    suspend fun drainOutgoingMessages(): Status
    fun abort(status: Status): Status
    fun getStatus(): Status
    fun getTrailers(): ByteMap?
    fun getId(): String
    suspend fun wait(): Status
}
