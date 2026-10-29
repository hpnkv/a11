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

import a11.internal.NativeBridge
import a11.internal.NativeData
import a11.internal.decodeWireMessage
import a11.net.NativeWireStream

/** A native session with pull-based reception and bounded native buffering. */
class SessionWithRecv private constructor(id: Long) : NativeObject(id) {
    suspend fun addStream(stream: NativeWireStream, mode: StreamMode = StreamMode.START): Status =
        NativeBridge.async(
                "sessionWithRecv.addStream",
                this,
                mapOf("stream" to stream, "accept" to (mode == StreamMode.ACCEPT)),
            )
            .status

    fun send(message: WireMessage): Status = NativeData.sessionSend(this, message)

    /** Send native contents by move; [message] retains its C++ moved-from state. */
    fun sendMoved(message: WireMessage): Status = NativeData.sessionSendMoved(this, message)

    /** Return null only for end of stream; failures remain an error result. */
    suspend fun receive(
        timeout: kotlin.time.Duration = kotlin.time.Duration.INFINITE
    ): StatusOr<WireMessage?> = receiveWith(mapOf("timeout" to timeout))

    suspend fun receive(timeout: Duration): StatusOr<WireMessage?> =
        receiveWith(mapOf("timeout" to timeout))

    suspend fun receive(deadline: Time): StatusOr<WireMessage?> =
        receiveWith(mapOf("deadline" to deadline))

    private suspend fun receiveWith(arguments: Map<String, Any?>): StatusOr<WireMessage?> =
        NativeBridge.async("sessionWithRecv.receive", this, arguments).map { value ->
            value?.let { fields -> decodeWireMessage(NativeBridge.fields(fields)) }
        }

    suspend fun receive(timeoutMs: Long?): StatusOr<WireMessage?> =
        NativeBridge.async("sessionWithRecv.receive", this, mapOf("timeoutMs" to timeoutMs)).map {
            raw ->
            raw?.let { value -> decodeWireMessage(NativeBridge.fields(value)) }
        }

    fun halfClose(): Status = NativeBridge.status("sessionWithRecv.halfClose", this)

    suspend fun awaitCompletion(): Status {
        val status = NativeBridge.async("sessionWithRecv.done", this).status
        status.orElse { failure ->
            return failure
        }
        return this.status
    }

    fun abort(status: Status): Status =
        NativeBridge.status("sessionWithRecv.abort", this, mapOf("status" to statusFields(status)))

    val status: Status
        get() =
            NativeBridge.call("sessionWithRecv.status", this)
                .map(NativeBridge::statusValue)
                .orElse { status ->
                    return status
                }

    companion object {
        fun create(id: String = "", headers: ByteMap = ByteMap()): StatusOr<SessionWithRecv> =
            NativeBridge.call(
                    "sessionWithRecv.create",
                    args = mapOf("id" to id, "headers" to headers),
                )
                .map { value -> SessionWithRecv(NativeBridge.handle(value)) }
    }
}
