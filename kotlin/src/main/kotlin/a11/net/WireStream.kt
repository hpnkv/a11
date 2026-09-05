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
