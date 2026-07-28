"""Native nghttp2 WebSocket wire streams."""

from a11 import _native

from .http2 import Http2Options
from .wire_stream import WireStream, WireStreamOptions

ChannelFramingOptions = _native.ChannelFramingOptions
WebSocketClientOptions = _native.WebSocketClientOptions
WebSocketServerOptions = _native.WebSocketServerOptions
WebSocketWireStream = _native.WebSocketWireStream
WebSocketWireServer = _native.WebSocketWireServer


def _server_enter(server: WebSocketWireServer) -> WebSocketWireServer:
    return server


def _server_exit(server: WebSocketWireServer, exc_type, exc, traceback) -> None:
    del exc_type, exc, traceback
    server.stop()


WebSocketWireServer.__enter__ = _server_enter
WebSocketWireServer.__exit__ = _server_exit

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
