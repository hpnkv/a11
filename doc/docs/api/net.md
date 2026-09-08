# Transports

A [`WireStream`][a11.net.wire_stream.WireStream] is A11's transport abstraction:
a bidirectional, message channel connecting two peers. Concrete implementations
support in-process channels, WebSocket, HTTP SSE, and WebRTC data channels.

## Transport contract

Each endpoint starts once in one role: `start()` for the initiator or
`accept()` for the responder. The message callback receives `None` when the
peer half-closes its write direction. The local endpoint can continue sending
until it also half-closes.

`send()` validates and admits a `WireMessage` to bounded transport state. A
successful return does not mean that the peer has received it. Half-close
after the final send, then drain the outgoing direction when application
shutdown depends on delivery through the local transport buffers. Full
completion occurs after both directions finish or an error aborts the stream.

WireStream does not provide global message ordering. Ordered action data uses
sequenced `NodeFragment` records, which the receiving node reconstructs. A
session can also attach several streams whose callbacks advance independently.
See the [WireStream lifecycle](../lifecycles/wire-stream.md) for the complete
shutdown and failure contract.

## Transport selection

| Implementation | Boundary |
| --- | --- |
| In-process | Two endpoints in one process, including tests and internal bridges |
| WebSocket | Long-lived client/server connections over HTTP/1.1 or HTTP/2 |
| HTTP SSE | Browser-compatible HTTP request and event-stream routes |
| WebRTC | Browser or native peers connected through signalling and ICE |

All implementations carry the same `WireMessage` format. Sessions and actions
therefore retain their protocol when deployment changes transport.

## WireStream example

WireStreams connect endpoints, deliver serialized frames, and handle independent
bidirectional shutdowns:

```python
import a11

client_stream, server_stream = a11.create_in_process_wire_stream_pair()

async def on_message(msg):
    if msg is None:
        print("Peer closed write direction")
        return
    print("Received:", msg)

async def on_done(status):
    print("Stream finished:", status)

await server_stream.accept(on_message, on_done)
await client_stream.start(on_message, on_done)
```

::: a11.net.wire_stream.WireStream

::: a11.net.wire_stream.WireStreamOptions

## In-process

::: a11.net.in_process_wire_stream.InProcessWireStream

::: a11.net.in_process_wire_stream.create_in_process_wire_stream_pair

## WebSocket

```python
stream = a11.WebSocketWireStream.connect("ws://127.0.0.1:8080/ws")

server = a11.WebSocketWireServer.create(accept_callback, port=8080)
```

::: a11.net.websocket_wire_stream.WebSocketWireStream

::: a11.net.websocket_wire_stream.WebSocketWireServer

## HTTP SSE

::: a11.net.http_sse_wire_stream.HttpSseWireStream

::: a11.net.http_sse_wire_stream.HttpSseServer

## WebRTC

::: a11.net.webrtc_wire_stream.WebRtcWireStream

::: a11.net.webrtc_wire_stream.WebRtcWireServer

Configured telemetry records one `a11.wire_stream` lifecycle span per endpoint
with its stream ID, terminal status, and send-size events. See
[observability](observability.md#emitted-spans).

## Signalling

Signalling provides out-of-band coordination for WebRTC peer connections:

::: a11.net.signalling.WebSocketSignallingServer

::: a11.net.signalling.WebSocketSignallingClient

::: a11.net.signalling.SignallingService

## HTTP/2 Primitives

::: a11.net.http2.Http2Client

::: a11.net.http2.Http2Server
