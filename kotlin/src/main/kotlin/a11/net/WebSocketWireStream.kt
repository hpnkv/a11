package a11.net

import a11.Ok
import a11.Status
import a11.StatusOr
import a11.failedPrecondition
import a11.invalidArgument
import a11.randomId
import a11.unavailable
import java.net.URI
import java.net.http.HttpClient
import java.net.http.WebSocket
import java.nio.ByteBuffer
import java.util.concurrent.CompletionStage
import java.util.concurrent.TimeUnit

/**
 * Client-side A11 [BinaryChannel] over the JDK [WebSocket] (`java.net.http`).
 *
 * Each A11 packet is one binary WebSocket message; partial frames are
 * reassembled by `last`. Blocking futures are awaited on the virtual-thread
 * dispatcher, so a send parks a virtual thread rather than a carrier. Packet
 * framing, reassembly, and lifecycle are delegated to [ChannelWireStream].
 */
class WebSocketBinaryChannel(
    private val url: String,
    private val headers: Map<String, String> = emptyMap(),
    private val connectTimeoutMs: Long = 30_000,
) : BinaryChannel {

    @Volatile private var socket: WebSocket? = null
    @Volatile private var closed = false
    private var onMessage: (ByteArray) -> Unit = {}
    private var onClosed: (Int, String) -> Unit = { _, _ -> }
    private var onError: (Status) -> Unit = {}
    private val partial = ArrayList<ByteArray>()

    override fun setCallbacks(
        onMessage: (ByteArray) -> Unit,
        onClosed: (Int, String) -> Unit,
        onError: (Status) -> Unit,
    ) {
        this.onMessage = onMessage
        this.onClosed = onClosed
        this.onError = onError
    }

    override suspend fun open(): Status {
        return try {
            val builder = HttpClient.newHttpClient().newWebSocketBuilder()
            for ((name, value) in headers) builder.header(name, value)
            val listener = object : WebSocket.Listener {
                override fun onOpen(webSocket: WebSocket) {
                    webSocket.request(1)
                }

                override fun onBinary(webSocket: WebSocket, data: ByteBuffer, last: Boolean): CompletionStage<*>? {
                    val chunk = ByteArray(data.remaining())
                    data.get(chunk)
                    partial.add(chunk)
                    if (last) {
                        val message = if (partial.size == 1) partial[0] else a11.concatBytes(partial.toList())
                        partial.clear()
                        try { onMessage(message) } catch (error: Throwable) { onError(unavailable("WebSocket message handler failed.")) }
                    }
                    webSocket.request(1)
                    return null
                }

                override fun onClose(webSocket: WebSocket, statusCode: Int, reason: String): CompletionStage<*>? {
                    closed = true
                    onClosed(statusCode, reason)
                    return null
                }

                override fun onError(webSocket: WebSocket, error: Throwable) {
                    closed = true
                    onError(unavailable(error.message ?: "WebSocket transport error."))
                }
            }
            socket = builder.buildAsync(URI.create(url), listener).get(connectTimeoutMs, TimeUnit.MILLISECONDS)
            Status.ok()
        } catch (error: Throwable) {
            unavailable("Could not open WebSocket to $url: ${error.message}")
        }
    }

    override fun isOpen(): Boolean = socket != null && !closed

    override suspend fun send(packet: ByteArray): Status {
        val ws = socket ?: return failedPrecondition("WebSocket is not open.")
        return try {
            ws.sendBinary(ByteBuffer.wrap(packet), true).get(connectTimeoutMs, TimeUnit.MILLISECONDS)
            Status.ok()
        } catch (error: Throwable) {
            unavailable("WebSocket send failed: ${error.message}")
        }
    }

    override fun close(): Status {
        if (closed) return Status.ok()
        closed = true
        try { socket?.sendClose(WebSocket.NORMAL_CLOSURE, "A11 stream complete") } catch (_: Throwable) {}
        return Status.ok()
    }
}

/**
 * Client-side A11 WireStream over the JDK RFC 6455 WebSocket (HTTP/1.1).
 *
 * As of the native runtime's HTTP/1.1 support (server option `enable_http1`, on
 * by default), this client connects **directly** to the native
 * `WebSocketWireServer`: the server sniffs the cleartext `Upgrade: websocket`
 * request (or negotiates `http/1.1` over TLS) and accepts it over an HTTP/1.1
 * connection. This is the standard JVM transport for reaching an A11 backend.
 */
object WebSocketWireStream {
    /** Create (but do not yet open) a client endpoint for a `ws://`/`wss://` URL. */
    fun connect(
        url: String,
        headers: Map<String, String> = emptyMap(),
        splitSize: Int = 64 * 1024,
    ): StatusOr<WireStream> {
        val uri = try { URI.create(url) } catch (error: Throwable) { return invalidArgument("WebSocket URL is invalid.") }
        if (uri.scheme != "ws" && uri.scheme != "wss") return invalidArgument("WebSocket URL must start with ws:// or wss://.")
        val channel = WebSocketBinaryChannel(url, headers)
        return Ok(ChannelWireStream(channel, randomId("ws-"), ChannelEndpointRole.CLIENT, splitSize))
    }
}
