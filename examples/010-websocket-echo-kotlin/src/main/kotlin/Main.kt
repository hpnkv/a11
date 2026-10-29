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

import a11.Chunk
import a11.ChunkMetadata
import a11.NodeFragment
import a11.SessionWithRecv
import a11.Status
import a11.StreamMode
import a11.WireMessage
import a11.cancelled
import a11.internal
import a11.net.WebSocketWireServer
import a11.net.WebSocketWireStream
import a11.orElse
import a11.utf8Decode
import a11.utf8Encode
import java.io.IOException
import kotlin.system.exitProcess
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.isActive
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.runInterruptible

private suspend fun echo(stream: WebSocketWireStream): Status {
    val session =
        SessionWithRecv.create().orElse { status ->
            return status
        }
    session.addStream(stream, StreamMode.ACCEPT).orElse { status ->
        return status
    }
    try {
        while (true) {
            val message =
                session.receive().orElse { status ->
                    session.abort(status)
                    return status
                } ?: break
            session.sendMoved(message).orElse { status ->
                session.abort(status)
                return status
            }
        }
        session.halfClose().orElse { status ->
            return status
        }
        return session.awaitCompletion()
    } finally {
        if (!currentCoroutineContext().isActive) session.abort(cancelled("Echo handler cancelled"))
    }
}

private fun textMessage(text: String): WireMessage {
    val chunk = Chunk(ChunkMetadata("text/plain"), data = utf8Encode(text))
    return WireMessage(nodeFragments = mutableListOf(NodeFragment("test", chunk)))
}

private suspend fun exchange(session: SessionWithRecv): Status {
    while (true) {
        print("> ")
        System.out.flush()
        val text =
            try {
                runInterruptible(Dispatchers.IO) { readlnOrNull() }
            } catch (error: IOException) {
                return internal("Cannot read stdin: ${error.message}")
            } ?: break
        if (text.equals("exit", ignoreCase = true)) break
        session.send(textMessage(text)).orElse { status ->
            return status
        }
        val message =
            session.receive().orElse { status ->
                return status
            } ?: break
        for (fragment in message.nodeFragments) {
            val chunk = fragment.data as? Chunk ?: continue
            val echoed =
                utf8Decode(chunk.data).orElse { status ->
                    return status
                }
            println("Echo: $echoed")
        }
    }
    session.halfClose().orElse { status ->
        return status
    }
    return session.awaitCompletion()
}

private suspend fun runClient(port: Int): Status {
    val session =
        SessionWithRecv.create().orElse { status ->
            return status
        }
    val connection =
        WebSocketWireStream.connect("ws://127.0.0.1:$port/ws", mapOf("hello" to "world"))
    val stream =
        connection.orElse { status ->
            return status
        }
    session.addStream(stream).orElse { status ->
        return status
    }
    try {
        val result = exchange(session)
        result.orElse { status ->
            session.abort(status)
            return status
        }
        return result
    } finally {
        if (!currentCoroutineContext().isActive) session.abort(cancelled("Echo client cancelled"))
    }
}

private suspend fun runEcho(): Status {
    val listener = WebSocketWireServer.create(path = "/ws", onStream = ::echo)
    val server =
        listener.orElse { status ->
            return status
        }
    var result = Status.ok()
    try {
        val port =
            server.port.orElse { status ->
                return status
            }
        result = runClient(port)
    } finally {
        val stopped = server.stop()
        result = result.updated(stopped)
    }
    return result
}

fun main() {
    runBlocking { runEcho() }
        .orElse { status ->
            System.err.println(status)
            exitProcess(1)
        }
}
