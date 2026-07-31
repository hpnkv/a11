"""Native nghttp2 WebSocket wire streams.

A `WebSocketWireStream` carries A11 [WireMessage][a11.data.types.WireMessage]
traffic over a WebSocket built on A11's nghttp2/HTTP2 stack -- the default
transport for connecting agents across a network.
`WebSocketWireServer` accepts inbound connections. See
[a11.net.wire_stream][a11.net.wire_stream] for the transport-agnostic interface
these implement.
"""

from a11 import _native
from a11._native_protocol import attach_protocol

from .http2 import Http2Options
from .wire_stream import WireStream, WireStreamOptions

from a11._native import ChannelFramingOptions
from a11._native import WebSocketClientOptions
from a11._native import WebSocketServerOptions
from a11._native import WebSocketWireStream
from a11._native import WebSocketWireServer


class _WebSocketWireServerProtocol:
    """Context-manager protocol for the server (``stop`` on exit)."""

    def __enter__(self) -> "WebSocketWireServer":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        del exc_type, exc, traceback
        self.stop()


attach_protocol(WebSocketWireServer, _WebSocketWireServerProtocol)

for _class in (
    ChannelFramingOptions,
    WebSocketClientOptions,
    WebSocketServerOptions,
    WebSocketWireStream,
    WebSocketWireServer,
):
    _class.__module__ = __name__


__all__ = [
    "ChannelFramingOptions",
    "Http2Options",
    "WebSocketClientOptions",
    "WebSocketServerOptions",
    "WebSocketWireServer",
    "WebSocketWireStream",
    "WireStream",
    "WireStreamOptions",
]
