# Transports

A [`WireStream`][a11.net.wire_stream.WireStream] is A11's transport abstraction:
a bidirectional, message channel connecting two peers. Concrete implementations
support in-process channels, WebSocket, HTTP SSE, and WebRTC data channels.

## Using WireStreams

WireStreams connect endpoints, deliver serialized frames, and handle independent
bidirectional shutdowns:

```python
import a11

# Create an in-process connected pair for testing or local routing
client_stream, server_stream = a11.create_in_process_wire_stream_pair()

# Start or accept streams with message and completion handlers
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
# Connect to a WebSocket endpoint
stream = a11.WebSocketWireStream.connect("ws://127.0.0.1:8080/ws")

# Or run a WebSocket listener
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

## Signalling

Signalling provides out-of-band coordination for WebRTC peer connections:

::: a11.net.signalling.WebSocketSignallingServer

::: a11.net.signalling.WebSocketSignallingClient

::: a11.net.signalling.SignallingService

## HTTP/2 Primitives

::: a11.net.http2.Http2Client

::: a11.net.http2.Http2Server
