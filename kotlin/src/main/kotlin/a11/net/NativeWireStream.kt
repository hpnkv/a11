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
import a11.NativeObject
import a11.Status
import a11.WireMessage
import a11.internal.NativeBridge
import a11.internal.NativeData
import a11.internal.decodeWireMessage
import a11.internal.nativeText
import a11.map
import a11.orElse
import a11.statusFields
import a11.value

/** A native transport. Releasing this reference does not close the stream. */
open class NativeWireStream internal constructor(id: Long) : NativeObject(id), WireStream {
    override fun send(message: WireMessage): Status = NativeData.streamSend(this, message)

    /** Send native contents by move; [message] retains its C++ moved-from state. */
    fun sendMoved(message: WireMessage): Status = NativeData.streamSendMoved(this, message)

    override suspend fun start(onMessage: OnWireMessage?, onDone: OnWireDone?): Status =
        drive(false, onMessage, onDone)

    override suspend fun accept(onMessage: OnWireMessage?, onDone: OnWireDone?): Status =
        drive(true, onMessage, onDone)

    private suspend fun drive(
        accept: Boolean,
        onMessage: OnWireMessage?,
        onDone: OnWireDone?,
    ): Status {
        val message =
            onMessage?.let { handler ->
                NativeBridge.callback { raw ->
                        handler(raw?.let { value -> decodeWireMessage(NativeBridge.fields(value)) })
                    }
                    .orElse { status ->
                        return status
                    }
            }
        val done =
            onDone?.let { handler ->
                NativeBridge.callback { handler() }
                    .orElse { status ->
                        message?.release()
                        return status
                    }
            }
        try {
            return NativeBridge.async(
                    "stream.drive",
                    this,
                    mapOf(
                        "accept" to accept,
                        "onMessage" to message?.let { value -> mapOf("handle" to value.id) },
                        "onDone" to done?.let { value -> mapOf("handle" to value.id) },
                    ),
                )
                .status
        } finally {
            message?.release()
            done?.release()
        }
    }

    override fun halfClose(trailers: ByteMap): Status =
        NativeBridge.status("stream.halfClose", this, mapOf("trailers" to trailers))

    override suspend fun drainOutgoingMessages(): Status =
        NativeBridge.async("stream.drain", this).status

    override fun abort(status: Status): Status =
        NativeBridge.status("stream.abort", this, mapOf("status" to statusFields(status)))

    override fun getStatus(): Status =
        NativeBridge.call("stream.status", this).map(NativeBridge::statusValue).orElse { status ->
            return status
        }

    override fun getTrailers(): ByteMap? =
        NativeBridge.call("stream.trailers", this)
            .map { raw ->
                raw?.let { value ->
                    NativeBridge.fields(value).mapValuesTo(ByteMap()) { (_, bytes) ->
                        bytes as ByteArray
                    }
                }
            }
            .value()

    override fun getId(): String = NativeBridge.call("stream.id", this).map(::nativeText).value()

    override suspend fun wait(): Status {
        val completion = NativeBridge.async("stream.done", this).status
        completion.orElse { status ->
            return status
        }
        return getStatus()
    }
}
