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

import a11.NativeObject
import a11.Status
import a11.StatusOr
import a11.internal.NativeBridge
import a11.map
import a11.orElse

/** A WebSocket transport implemented by A11's native networking stack. */
class WebSocketWireStream internal constructor(id: Long) : NativeWireStream(id) {
    companion object {
        /** Create a client; [start] performs the A11 handshake. */
        fun connect(
            url: String,
            headers: Map<String, String> = emptyMap(),
            splitSize: Int = 64 * 1024,
            http2Options: Http2Options = Http2Options(),
        ): StatusOr<WebSocketWireStream> =
            NativeBridge.call(
                    "websocket.connect",
                    args =
                        mapOf(
                            "url" to url,
                            "headers" to
                                headers.map { element -> listOf(element.key, element.value) },
                            "splitSize" to splitSize,
                            "http2Options" to http2Options.fields(),
                        ),
                )
                .map { value -> WebSocketWireStream(NativeBridge.handle(value)) }
    }
}

/** Native WebSocket listener. Stop it explicitly before releasing its reference. */
class WebSocketWireServer private constructor(id: Long) : NativeObject(id) {
    val port: StatusOr<Int>
        get() =
            NativeBridge.call("websocket.serverPort", this).map { value ->
                NativeBridge.number(value).toInt()
            }

    fun stop(): Status = NativeBridge.status("websocket.serverStop", this)

    companion object {
        /** Serve each connection concurrently with native transport backpressure. */
        fun create(
            path: String = "/a11",
            bindAddress: String = "127.0.0.1",
            port: Int = 0,
            http2Options: Http2Options = Http2Options(),
            onStream: suspend (WebSocketWireStream) -> Status,
        ): StatusOr<WebSocketWireServer> {
            val callback =
                NativeBridge.callback { raw ->
                        val stream = WebSocketWireStream(NativeBridge.handle(raw))
                        onStream(stream)
                    }
                    .orElse { status ->
                        return status
                    }
            try {
                return NativeBridge.call(
                        "websocket.serve",
                        args =
                            mapOf(
                                "path" to path,
                                "bindAddress" to bindAddress,
                                "http2Options" to http2Options.fields(),
                                "port" to port,
                                "callback" to mapOf("handle" to callback.id),
                            ),
                    )
                    .map { value -> WebSocketWireServer(NativeBridge.handle(value)) }
            } finally {
                callback.release()
            }
        }
    }
}
