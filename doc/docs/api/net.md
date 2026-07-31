# Transports

A [`WireStream`][a11.net.wire_stream.WireStream] is A11's pluggable transport —
a bidirectional, ordered channel between two endpoints. Choosing an
implementation is how an agent goes from in-process to networked; see
[Principles](../principles.md#wirestream-how-bytes-move-between-peers).

## WireStream

::: a11.net.wire_stream.WireStream

::: a11.net.wire_stream.WireStreamOptions

## In-process

::: a11.net.in_process_wire_stream.InProcessWireStream

::: a11.net.in_process_wire_stream.create_in_process_wire_stream_pair

## WebSocket

::: a11.net.websocket_wire_stream.WebSocketWireStream

::: a11.net.websocket_wire_stream.WebSocketWireServer

## HTTP SSE

::: a11.net.http_sse_wire_stream.HttpSseWireStream

::: a11.net.http_sse_wire_stream.HttpSseServer

## WebRTC

::: a11.net.webrtc_wire_stream.WebRtcWireStream

::: a11.net.webrtc_wire_stream.WebRtcWireServer

## Signalling

Signalling is the out-of-band handshake WebRTC peers use to find each other and
exchange connection details.

::: a11.net.signalling.WebSocketSignallingServer

::: a11.net.signalling.WebSocketSignallingClient

::: a11.net.signalling.SignallingService

## HTTP/2 primitives

Low-level building blocks under the WebSocket and SSE transports; most agents
use them only indirectly.

::: a11.net.http2.Http2Client

::: a11.net.http2.Http2Server
